# Provenance

This repository contains substantial original work by primo-arm-miner
contributors. It also has known development ancestry from earlier
GPL-licensed mining software used as references and integration harnesses
during the project bootstrap phase, and translation ancestry from the
permissively-licensed VerusHash reference code.

This document is a release-compliance record, last verified against the
ancestor source trees in a file-by-file audit on 2026-07-06. It is
intentionally conservative: listing a file or subsystem here does not assert
that every current line remains derivative, only that the subsystem has
known ancestry that should be disclosed when distributing releases.

## Release posture

- Distribute this repository under `GPL-3.0-or-later`.
- Preserve third-party copyright and license notices that appear in the
  tree (see "Third-party notices" below).
- Do not market the repository as wholly independent of prior GPL-licensed
  mining software unless you have a separate clean-room record or legal
  review supporting that claim.

## Known source ancestry

- `ccminer` (via the oink70 ARM fork and the `ccminer_arm_cc1` development
  harness; GPL — fork LICENSE is GPLv3, source headers GPL-2.0-or-later)
  - The coordinator/config/stratum scaffolding began as modifications of
    ccminer and was substantially rewritten during ownership and compliance
    passes. Files with ccminer ancestry (recognizable fragments — struct
    layouts, option-table rows, error/log strings, individual helper
    functions — survive in some of them):
    - `include/miner.h` (struct work layout, CL_* color macros, option
      globals vocabulary)
    - `src/miner.cpp`
    - `src/config.cpp` (ccminer-compatible option table and help format)
    - `src/main.cpp`
    - `src/utils/log.cpp` (modeled on ccminer's applog)
    - `src/api.cpp` (ccminer API wire-format strings retained for
      protocol compatibility)
    - the stratum layer: `src/stratum.cpp`, `src/stratum_handshake.cpp`,
      `src/stratum_rpc.cpp`, `src/stratum_standard.cpp`,
      `src/stratum_transport.cpp`, `src/stratum_session.cpp`,
      `src/stratum_job.cpp`, `src/stratum_state.cpp`,
      `src/stratum_verus.cpp` (target/submit helpers from
      `equi/equi-stratum.cpp`)
    - `src/algorithm/verus.cpp` (scan/keying helper flow ported from the
      fork's `verusscan.cpp`, since renamed and extended)

- `ccminer_oink70_source` (oink70 ARM ccminer fork)
  - The actual upstream of the Verus implementation: primo's VerusHash
    began as a translation of its sse2neon-based Verus code into native
    ARM NEON intrinsics (no sse2neon code remains). Also kept as a
    historical reference for behavior comparison.

- `cpuminer-opt-26.1` (GPL-2.0-or-later headers)
  - Used as a reading reference for SHA256d and Scrypt integration
    patterns and standard-stratum behavior. The audit found no copied
    code; `src/algorithm/sha256_neon.c` and `src/algorithm/scrypt_neon.c`
    are independent implementations sharing only the family's scanhash
    interface conventions.

## Subsystems verified original (2026-07-06 audit)

No identifiable ancestry beyond shared project vocabulary:
`src/stratum_xmr.cpp`, `src/dev_fee.cpp`, `src/algorithm/randomx_algo.cpp`,
`src/algorithm/sha256_ce_asm.S`, `src/algorithm/scrypt_blockmix_asm.S`,
the Verus x2/fused/runtime-asm dispatch layers, the scrypt ROMix kernels,
`src/algorithm/cpu_features.c` (grown from a 54-line probe written for this
project), the TLS transport and WebSocket handshake code, `android/`,
`tests/`, and the build system.

## Third-party notices retained in-tree

- VerusHash core (translation ancestry, notices carried in the files).
  These files are new ARM implementations authored for this project (no
  ancestor ships them), but their hashing cores are statement-level
  translations of the upstream implementations, so upstream notices apply:
  - `src/algorithm/clhash_native.c` / `clhash_native_noasm.c` /
    `include/clhash_native.h`: ARM NEON translation of the Verus CLHash
    variant, Copyright (c) 2018 Michael Toutonghi, based on CLHash,
    Copyright (c) 2017, 2018 Daniel Lemire and Owen Kaser — Apache-2.0.
    The Apache-2.0 license text is retained at `LICENSES/Apache-2.0.txt`.
  - `src/algorithm/haraka_native.c` / `include/haraka_native.h`: ARM AES
    translation of Haraka v2, Copyright (c) 2016 kste (MIT). VerusHash
    construction Copyright (c) 2018 The Verus Developers (MIT).

- `src/algorithm/scrypt.h`
  - Retains Colin Percival copyright and redistribution terms and must be
    preserved in redistribution.

- `src/algorithm/sha256_neon.c`
  - Based on public-domain implementations by Jeffrey Walton
    (noloader/SHA-Intrinsics) and Colin Percival; credits retained.

- `third_party/RandomX/`
  - Vendored copy of the RandomX reference implementation
    (https://github.com/tevador/RandomX) at upstream commit `1e9d4b2`
    (2026-05-24), used unmodified as the RandomX hashing library
    (verified: single import commit, no modifications since).
  - BSD 3-Clause license (tevador and The Monero Project); the license text
    is retained at `third_party/RandomX/LICENSE` and must be preserved in
    redistribution. BSD-3 is compatible with this project's GPL-3.0-or-later
    distribution.

- `third_party/yespower/`
  - Vendored copy of the yespower reference implementation
    (https://github.com/openwall/yespower) at upstream commit
    `1977c283bc43eed5a2c2579e02d6d996e49866b0` (2026-06-15): `yespower.h`,
    `yespower-ref.c`, `sha256.c`/`sha256.h`, `sysendian.h`,
    `insecure_memzero.h`, used unmodified.
  - BSD 2-Clause license (Copyright 2009 Colin Percival, Copyright
    2013-2018 Alexander Peslyak); the license text is retained at
    `third_party/yespower/LICENSE` and `LICENSES/BSD-2-Clause-yespower.txt`
    and must be preserved in redistribution. BSD-2 is compatible with this
    project's GPL-3.0-or-later distribution.
  - `src/algorithm/civiclight.cpp` (the CIVIC/CivicNet "civiclight"
    algorithm's combination logic -- SHA256d, then either XOR+SHA256 or
    yespower+XOR+SHA256 depending on block time) is this project's own
    clean-room implementation, written from a plain-language description
    of the algorithm rather than from CivicLight's own source. CivicLight's
    repos (github.com/CivicLight/civicnet-easyminer-android and
    civiclight-miner-windows) carry no LICENSE file as of 2026-08, so their
    code is not cleared for reuse here regardless of claims made elsewhere
    on their project site.

## Compliance note

The ccminer-ancestry files listed above were substantially rewritten, but
the repository was not developed under a documented clean-room process. For
release purposes, the safest posture is to treat the combined work as
GPL-covered and preserve this provenance record alongside the project
license. All identified third-party ancestries (Apache-2.0, MIT, BSD) are
compatible with GPL-3.0-or-later distribution.
