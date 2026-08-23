/*
 * CIVIC (CivicNet) "civiclight" algorithm -- clean-room implementation.
 * Copyright 2026 primo-arm-miner contributors (Ghuleh Miner additions).
 *
 * See include/civiclight_algo.h for the algorithm description and a note
 * on why this was written from a plain-language spec rather than ported
 * from CivicLight's own (unlicensed) source. yespower itself -- the
 * proof-of-work-heavy part of civiclight v2 -- comes from its original
 * authors' BSD-2 licensed reference implementation, vendored unmodified
 * at third_party/yespower/.
 *
 * This is the portable reference path (third_party/yespower/yespower-ref.c).
 * Like the project's other algorithms, an ARM NEON/asm-optimized path can
 * follow once this is validated correct and self-tests pass; nothing here
 * is a bottleneck-critical hot loop yet, so correctness came first.
 */
#include "civiclight_algo.h"
#include "miner.h"
#include "byteorder.h"

extern "C" {
#include "yespower.h"
#include "sha256.h"
}

#include <cstdio>
#include <cstring>
#include <pthread.h>

static pthread_mutex_t g_civic_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_civic_ready = false;

// ---------------------------------------------------------------------
// Core hash
// ---------------------------------------------------------------------

static void sha256d(const uint8_t *in, size_t len, uint8_t out[32])
{
    uint8_t round1[32];
    SHA256_Buf(in, len, round1);
    SHA256_Buf(round1, 32, out);
}

void civiclight_hash(const uint8_t header[80], uint32_t nTime, uint8_t out[32])
{
    uint8_t intermediate[32];
    sha256d(header, 80, intermediate);

    if (nTime < CIVICLIGHT_V2_ACTIVATION_TIME) {
        // v1: XOR with the constant byte, then one more SHA256.
        uint8_t xored[32];
        for (int i = 0; i < 32; i++)
            xored[i] = intermediate[i] ^ 0x5A;
        SHA256_Buf(xored, 32, out);
        return;
    }

    // v2: yespower(intermediate, N=2048, r=8), XOR against intermediate,
    // then one more SHA256.
    yespower_local_t local;
    yespower_init_local(&local);

    yespower_params_t params;
    params.version = YESPOWER_1_0;
    params.N = CIVICLIGHT_YESPOWER_N;
    params.r = CIVICLIGHT_YESPOWER_R;
    params.pers = nullptr;
    params.perslen = 0;

    yespower_binary_t yhash;
    yespower(&local, intermediate, 32, &params, &yhash);
    yespower_free_local(&local);

    uint8_t xored[32];
    for (int i = 0; i < 32; i++)
        xored[i] = yhash.uc[i] ^ intermediate[i];
    SHA256_Buf(xored, 32, out);
}

// ---------------------------------------------------------------------
// Self-test
//
// CivicLight's own repos carry no license, so these are NOT their test
// vectors -- they're independently generated: the v1 vector by hashing
// the deterministic test header with a from-scratch SHA256d+XOR+SHA256
// implementation, the v2 vector the same way but through the vendored
// (BSD-2) yespower reference implementation. If a future NEON/asm path
// is added for either sha256 or yespower here, these vectors are what
// must keep matching.
// ---------------------------------------------------------------------

static bool hex_eq(const uint8_t *bin, const char *hex)
{
    char buf[65];
    for (int i = 0; i < 32; i++)
        snprintf(buf + i * 2, 3, "%02x", bin[i]);
    buf[64] = '\0';
    return strcmp(buf, hex) == 0;
}

int civiclight_init_runtime(int n_threads)
{
    (void)n_threads;

    pthread_mutex_lock(&g_civic_lock);

    // Deterministic 80-byte test header: bytes 0..79.
    uint8_t header[80];
    for (int i = 0; i < 80; i++)
        header[i] = (uint8_t)i;

    uint8_t out[32];

    // v1 (nTime below activation)
    civiclight_hash(header, CIVICLIGHT_V2_ACTIVATION_TIME - 1, out);
    if (!hex_eq(out, "6ae97142dabd660d21c54a03d76eee6def2b4f0be0764420d4071b3d18bf947f")) {
        applog(LOG_ERR, "civiclight self-test FAILED (v1 vector mismatch) -- refusing to mine");
        pthread_mutex_unlock(&g_civic_lock);
        return 0;
    }

    // v2 (nTime at/after activation)
    civiclight_hash(header, CIVICLIGHT_V2_ACTIVATION_TIME, out);
    if (!hex_eq(out, "3c7a20baeefd2700d2542db4b54a52f751947f7e68525ed82709fd566c8940de")) {
        applog(LOG_ERR, "civiclight self-test FAILED (v2/yespower vector mismatch) -- refusing to mine");
        pthread_mutex_unlock(&g_civic_lock);
        return 0;
    }

    g_civic_ready = true;
    applog(LOG_INFO, "civiclight self-test passed");
    pthread_mutex_unlock(&g_civic_lock);
    return 1;
}

void civiclight_cleanup_runtime(void)
{
    g_civic_ready = false;
}

// ---------------------------------------------------------------------
// Mining loop
// ---------------------------------------------------------------------

int scanhash_civic(int thr_id, struct work *work, uint32_t max_hashes,
                    unsigned long *hashes_done)
{
    (void)thr_id;

    uint32_t *pdata = work->data;
    uint32_t *ptarget = work->target;
    uint8_t header[80];
    uint32_t n = pdata[19];   // nonce, word 19 (byte 76)
    uint32_t first_nonce = n;
    uint32_t nTime;

    work->valid_nonces = 0;

    // Same stratum-word-order convention as scanhash_sha256d: version +
    // prevhash + nTime/nBits/nonce are byte-swapped back to real header
    // bytes; the merkle root (words 9-16) is already correctly ordered.
    for (int i = 0; i < 9; i++)
        be32enc(header + i * 4, pdata[i]);
    for (int i = 9; i < 17; i++)
        le32enc(header + i * 4, pdata[i]);
    for (int i = 17; i < 20; i++)
        be32enc(header + i * 4, pdata[i]);

    nTime = pdata[17];

    uint32_t remaining = max_hashes;
    while (remaining > 0 &&
           !miner_work_restart_requested(work->restart_generation) &&
           !miner_should_abort()) {
        be32enc(header + 76, n);

        uint8_t hash[32];
        civiclight_hash(header, nTime, hash);

        // hash[] is a standard big-endian SHA256 digest (word i's bytes at
        // hash[i*4..i*4+3], big-endian). hash_le_target wants each word
        // byte-swapped in place (same convention as scanhash_sha256d's
        // hash_le), not word-order reversed -- le32dec on the same offset
        // does exactly that swap.
        uint32_t hash_le[8];
        for (int i = 0; i < 8; i++)
            hash_le[i] = le32dec(hash + i * 4);

        if (hash_le_target(hash_le, ptarget)) {
            pdata[19] = n;
            work->nonces[work->valid_nonces] = n;
            bn_store_share_difficulty(hash_le, ptarget, work, work->valid_nonces);
            applog(LOG_INFO, "civiclight: Found nonce %08x!", n);
            work->valid_nonces++;
            if (work->valid_nonces >= MAX_NONCES)
                break;
        }

        n++;
        remaining--;
    }

    *hashes_done = n - first_nonce;
    pdata[19] = n;
    return work->valid_nonces;
}
