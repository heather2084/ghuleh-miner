/*
 * CIVIC (CivicNet) "civiclight" algorithm.
 * Copyright 2026 primo-arm-miner contributors (Ghuleh Miner additions).
 *
 * Initially written from a plain-language description of the algorithm's
 * steps. That first pass had a real bug (it skipped an intermediate SHA256
 * round before the XOR/yespower stage) which made every submitted share
 * come back "Low difficulty share". The combination logic below was
 * corrected against CivicLight's own published open-source CPU miner --
 * github.com/CivicLight/civiclight-miner-windows,
 * cpuminer-opt-source/algo/civiclight/civiclight.c -- which the project
 * explicitly states is open source (see civiclight.xyz/faq.html: "Everything
 * is open source at github.com/CivicLight, including the node/wallet, the
 * CPU miner, and this website"). See include/civiclight_algo.h for the
 * algorithm description. yespower itself -- the proof-of-work-heavy part of
 * civiclight v2 -- comes from its original authors' BSD-2 licensed reference
 * implementation, vendored unmodified at third_party/yespower/.
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

// Shared implementation: both civiclight_hash() (self-test / cold-path
// callers) and civiclight_hash_with_local() (the hot mining loop, see
// scanhash_civic below) funnel through here. `local` is only touched by the
// v2/yespower branch -- the v1 branch returns before it would be used, so
// callers on the v1-only path may pass a not-yet-initialized local safely.
static void civiclight_hash_impl(const uint8_t header[80], uint32_t nTime, uint8_t out[32],
                                  yespower_local_t *local)
{
    uint8_t intermediate[32];
    sha256d(header, 80, intermediate);

    // Both v1 and v2 run the sha256d(header) result through one *more*
    // plain SHA256 before the XOR/yespower step -- this "hash1" stage was
    // missing here previously (we were XORing/feeding yespower with
    // `intermediate` directly), which is why every share came back "Low
    // difficulty share": our PoW hash never matched the network's for the
    // same header. Corrected against CivicLight's own published open-source
    // miner (github.com/CivicLight/civiclight-miner-windows,
    // cpuminer-opt-source/algo/civiclight/civiclight.c -- explicitly stated
    // open source at civiclight.xyz/faq.html) and verified offline against
    // a real solved CivicNet block (35998): this fixed pipeline produces a
    // hash under that block's actual PoW target; the previous one did not.
    uint8_t hash1[32];
    SHA256_Buf(intermediate, 32, hash1);

    if (nTime < CIVICLIGHT_V2_ACTIVATION_TIME) {
        // v1: XOR with the constant byte, then one more SHA256.
        uint8_t xored[32];
        for (int i = 0; i < 32; i++)
            xored[i] = hash1[i] ^ 0x5A;
        SHA256_Buf(xored, 32, out);
        return;
    }

    // v2: yespower(hash1, N=2048, r=8), XOR against hash1, then one more
    // SHA256. `local` is the caller's scratch buffer -- see the two wrapper
    // functions below for who owns/allocates it and why that split exists.
    yespower_params_t params;
    params.version = YESPOWER_1_0;
    params.N = CIVICLIGHT_YESPOWER_N;
    params.r = CIVICLIGHT_YESPOWER_R;
    params.pers = nullptr;
    params.perslen = 0;

    yespower_binary_t yhash;
    yespower(local, hash1, 32, &params, &yhash);

    uint8_t xored[32];
    for (int i = 0; i < 32; i++)
        xored[i] = yhash.uc[i] ^ hash1[i];
    SHA256_Buf(xored, 32, out);
}

void civiclight_hash(const uint8_t header[80], uint32_t nTime, uint8_t out[32])
{
    // Cold-path wrapper: the startup self-test (civiclight_init_runtime)
    // calls this a total of twice, so allocating and freeing yespower's
    // scratch buffer here is fine. The per-nonce mining loop must NOT go
    // through this function -- see civiclight_hash_with_local() below.
    yespower_local_t local;
    yespower_init_local(&local);
    civiclight_hash_impl(header, nTime, out, &local);
    yespower_free_local(&local);
}

// Hot-path variant for scanhash_civic(): takes an already-initialized
// yespower local buffer instead of allocating one internally.
//
// yespower_init_local()/yespower_free_local() allocate and release
// yespower's scratch buffer, which for this coin's fixed parameters
// (N=2048, r=8) is ~128*N*r = 2 MiB (see RFC 7914 / the yespower reference
// implementation for that sizing formula). civiclight_hash() used to be the
// ONLY hash entry point, called once per nonce from the scanhash_civic loop
// below -- meaning every single hash attempt did a fresh 2 MiB malloc and
// free before/after the actual (comparatively cheap) yespower computation.
// That made heap allocation overhead the dominant per-hash cost instead of
// the PoW work itself, throttling real throughput far below what the
// hardware -- and the hashrate this app displays, which is measured off the
// same loop -- would suggest. yespower's own reference implementation
// allocates the local buffer once per thread and reuses it across many
// hashes; scanhash_civic now does the same, allocating once per batch
// (still far more often than the ideal "once per mining thread," but a
// batch is typically millions of nonces, so this removes effectively all of
// the waste) and passing it in here instead.
static void civiclight_hash_with_local(const uint8_t header[80], uint32_t nTime, uint8_t out[32],
                                       yespower_local_t *local)
{
    civiclight_hash_impl(header, nTime, out, local);
}

// ---------------------------------------------------------------------
// Self-test
//
// These are not CivicLight's own test vectors -- they're generated by
// running the corrected civiclight_hash() above (see the provenance note
// at the top of this file) against a deterministic local test header, using
// this build's own SHA256 and the vendored (BSD-2) yespower reference
// implementation. If a future NEON/asm path is added for either sha256 or
// yespower here, these vectors are what must keep matching.
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
    if (!hex_eq(out, "8a086ae422e9a23a5c6c917887350108c712bd7e295a331bd5caf25a1a535f89")) {
        applog(LOG_ERR, "civiclight self-test FAILED (v1 vector mismatch) -- refusing to mine");
        pthread_mutex_unlock(&g_civic_lock);
        return 0;
    }

    // v2 (nTime at/after activation)
    civiclight_hash(header, CIVICLIGHT_V2_ACTIVATION_TIME, out);
    if (!hex_eq(out, "792bbe18ad67c99128c6a18f00e20ef2b3557fd1f7dee26f9382f5d852520680")) {
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

    // REVERTED (see git history): an earlier version of this function used
    // le32enc uniformly for every word, on the theory that build_standard_work()
    // populates pdata[0..18] via le32dec() of the raw job bytes, so le32enc
    // undoes that exactly. That reasoning was checked against a real solved
    // CivicNet block's header fields taken from a block explorer -- but a
    // block explorer's raw header bytes aren't stratum wire data, and that
    // "verification" never actually exercised real pool-sent job bytes.
    // Deployed and tested live against us.nitropool.net, the le32enc-uniform
    // version rejected 100% of shares as "Low difficulty share", and direct
    // inspection of real live nTime/nBits wire values confirmed why: e.g. a
    // live nTime of 0x6a923457 only decodes to a plausible current Unix
    // timestamp when read big-endian (le32dec gives a nonsense 2016 date).
    // The pool's stratum server sends version/prevhash/nTime/nBits/nonce
    // byte-swapped per word relative to real header bytes -- exactly the
    // same convention scanhash_sha256d() already handles correctly for its
    // algorithm against this same pool family (see its comment). The merkle
    // root (words 9-16) is the one exception: it's computed locally by
    // build_merkle_root(), not pool wire data, so it's already in correct
    // header byte order and needs no swap.
    for (int i = 0; i < 9; i++)
        be32enc(header + i * 4, pdata[i]);
    for (int i = 9; i < 17; i++)
        le32enc(header + i * 4, pdata[i]);
    for (int i = 17; i < 20; i++)
        be32enc(header + i * 4, pdata[i]);

    // nTime for the v1/v2 activation-time check must match the same wire
    // interpretation as the header bytes we just built, not the raw
    // pdata[17] (= le32dec(wire_bytes), the wrong way round -- decoded a
    // live wire value of 0x6a923457 to an implausible 2016 timestamp).
    // header+68 now holds be32enc(pdata[17]), i.e. wire_bytes reversed, so
    // le32dec(header+68) recovers be32dec(wire_bytes) -- the correct,
    // plausible-current-timestamp interpretation.
    nTime = le32dec(header + 68);

    // Allocate yespower's scratch buffer ONCE for this whole batch instead
    // of once per nonce -- see the comment on civiclight_hash_with_local()
    // above for why the old per-nonce allocation (via civiclight_hash())
    // was silently throttling real throughput on every device mining v2
    // (i.e. all of them, since v2 activated back in May 2026).
    yespower_local_t local;
    yespower_init_local(&local);

    uint32_t remaining = max_hashes;
    while (remaining > 0 &&
           !miner_work_restart_requested(work->restart_generation) &&
           !miner_should_abort()) {
        be32enc(header + 76, n);

        uint8_t hash[32];
        civiclight_hash_with_local(header, nTime, hash, &local);

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

    yespower_free_local(&local);

    *hashes_done = n - first_nonce;
    pdata[19] = n;
    return work->valid_nonces;
}
