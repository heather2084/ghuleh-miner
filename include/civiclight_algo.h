/*
 * CIVIC (CivicNet) "civiclight" algorithm — clean-room implementation.
 * Copyright 2026 primo-arm-miner contributors (Ghuleh Miner additions).
 *
 * civiclight is a two-version PoW: both versions start with SHA256d(header).
 * Version 1 (pre-activation) XORs the result with a constant byte and hashes
 * once more with SHA256. Version 2 (activation timestamp onward) instead
 * runs the SHA256d result through yespower (N=2048, r=8), XORs that output
 * against the SHA256d result, and finishes with one more SHA256. Which
 * version applies is decided by the block's nTime field.
 *
 * This header/implementation was written from a plain-language description
 * of the algorithm's steps, not copied from any CivicLight source (their
 * repos carry no license file, so their code isn't cleared for reuse here).
 * The proof-of-work-heavy part, yespower itself, comes instead from its
 * original authors' own BSD-2 licensed reference implementation, vendored
 * unmodified at third_party/yespower/ — see third_party/yespower/LICENSE
 * and NOTICE.
 */
#ifndef CIVICLIGHT_ALGO_H
#define CIVICLIGHT_ALGO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct work;

/* yespower parameters civiclight v2 uses, fixed by the coin's consensus
 * rules -- not configurable. */
#define CIVICLIGHT_YESPOWER_N 2048
#define CIVICLIGHT_YESPOWER_R 8

/* Block time (Unix seconds, from the header's nTime field) at and after
 * which civiclight v2 (the yespower-hardened version) activates. Below
 * this, v1 (SHA256d + XOR + SHA256) applies. */
#define CIVICLIGHT_V2_ACTIVATION_TIME 1784797200u /* 2026-05-19T00:00:00Z, per CivicNet */

/* Run the startup self-test (known-answer vectors for both v1 and v2, plus
 * a yespower-primitive check) and prepare any per-thread state. Returns
 * false if the self-test fails -- never mine on a failed self-test. */
int civiclight_init_runtime(int n_threads);
void civiclight_cleanup_runtime(void);

/* Compute civiclight(header[80], nTime) -> out[32]. Exposed directly for
 * the self-test; scanhash_civic() is what the mining loop calls. */
void civiclight_hash(const uint8_t header[80], uint32_t nTime, uint8_t out[32]);

int scanhash_civic(int thr_id, struct work *work, uint32_t max_hashes,
                    unsigned long *hashes_done);

#ifdef __cplusplus
}
#endif

#endif /* CIVICLIGHT_ALGO_H */
