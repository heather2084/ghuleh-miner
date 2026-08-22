# Ghuleh Miner

A minimal, pure ARM-native multi-algorithm cryptocurrency miner — no x86 compatibility layer — with a ccminer-compatible CLI and configuration surface.

*A fork of [PrimoLab's primo-arm-miner](https://gitlab.com/PrimoLab/primo-arm-miner) (GPL-3.0-or-later) — see [NOTICE](NOTICE) and [PROVENANCE.md](PROVENANCE.md) for full ancestry.*

## Quick Install (Linux arm64 / Termux)

This is a source-only fork — there's no separate hosted installer. Build
directly from the repo:

```bash
git clone https://github.com/heather2084/ghuleh-miner.git
cd ghuleh-miner
make -j"$(nproc)"
```

See [Building](#building) below for CMake, Termux-specific build scripts,
and toolchain overrides. ARMv8 crypto extensions (AES/PMULL/SHA2) are
required — standard on every 64-bit ARM SoC of the last decade.

### Runtime packages

The binary links two shared libraries: **libcurl** and **libjansson**. If
they're not already on your system, install them before running the miner
(it exits with a missing-library error otherwise):

```bash
# Debian / Ubuntu / Armbian / Raspberry Pi OS (arm64)
sudo apt-get install -y libcurl4 libjansson4

# Termux (Android)
pkg install libcurl libjansson

# Fedora / RHEL
sudo dnf install libcurl jansson

# Alpine
sudo apk add curl jansson
```

> **Termux note:** if launching fails with `CANNOT LINK EXECUTABLE ...
> libcurl.so`, your Termux packages are out of sync (libcurl newer than its
> ngtcp2 dependency). Run `pkg update && pkg upgrade -y` and retry.

> **DNS fallback:** if the system resolver can't resolve a pool hostname
> (some public resolvers block mining pools; Android app sandboxes block
> DNS for native subprocesses), the miner retries with a built-in resolver
> that queries Cloudflare (1.1.1.1) / Google (8.8.8.8) directly. It tries
> every address the pool's DNS publishes (IPv6 only when there is no
> IPv4), only runs after the system resolver fails, and the whole attempt
> is capped at ~10 seconds; set `PRIMO_DNS_FALLBACK=0` to disable it.

## Features

- **Pure ARM-native, no x86 compatibility layer** — Verus, SHA256d, and scrypt paths written directly in ARM intrinsics and AArch64 assembly, not translated from SSE
- **Hardware crypto extensions** — ARMv8 PMULL, AES, and SHA2 instructions on the hot paths
- **Per-core runtime optimization** — big.LITTLE topology detected at startup; interleaved CLHash, fused-dispatch CLHash, and SoA scrypt kernels enabled per thread where they win
- **Hotplug-resilient core pinning** — pins are chosen from the platform-allowed cpuset and reconciled continuously; threads adopt cores that Android parks/wakes at runtime instead of losing their pins
- **RandomX (Monero)** — vendored reference library (tevador/RandomX, BSD-3) with the aarch64 JIT; fast mode (~2.1 GiB dataset) with automatic light-mode fallback (256 MiB) on low-RAM devices. Other rx/0 chains work too, including extended-header forks — Zephyr (ZEPH) is live-validated (`-a randomx` pointed at a ZEPH pool with a ZEPH wallet)
- **ccminer-compatible control surface** — same CLI flags, JSON config format, and monitoring API
- **Full stratum support** — standard (SHA256d/scrypt), Verus/equihash, and Monero (RandomX) dialects, multi-pool failover, TLS (`stratum+ssl://`) via the system libcurl with no extra TLS library linked
- **Self-verifying** — SHA256d, scrypt, and RandomX cross-check their optimized kernels (including the hand-written assembly) against reference implementations at startup and refuse to mine on mismatch; the Verus interleaved/fused/asm paths are cross-checked under load by `make test` and at runtime with `VERUS_X2_SELFTEST=1`
- **Tiny footprint** — a single ~557 KB binary (~317 KB built with `PRIMO_RANDOMX=0`), two runtime libraries (libcurl, libjansson)

## Performance

Measured on RK3588 (4×Cortex-A55 @ 1.8 GHz + 4×Cortex-A76 @ 2.25–2.35 GHz):

| Algorithm | A76 single core | 8 threads (4×A55 + 4×A76) |
| --- | --- | --- |
| Verus (VerusHash v2.2) | ~1.44 MH/s | ~7.5 MH/s |
| SHA256d | ~16.3 MH/s | ~90 MH/s |
| Scrypt (N=1024) | ~4.8 kH/s | ~25.9 kH/s |
| RandomX (Monero rx/0) | ~160 H/s | ~690 H/s (~760 with huge pages) |

RandomX numbers are fast mode (needs ~2.5 GiB free RAM; the miner falls back
to light mode, ~5x slower, when the dataset doesn't fit — or force it with
`PRIMO_RANDOMX_LIGHT=1`). RandomX is memory-latency bound, so LITTLE cores
contribute far less than on the other algorithms (~7% on RK3588) and it runs
the chassis notably hotter. On Linux SBCs, 2 MiB huge pages are ~11% faster:
`sysctl vm.nr_hugepages=1200` (the miner logs a hint when they're missing).

Galaxy S10+ (Exynos 9820, Termux): ~5.5-5.7 MH/s Verus across 8 threads.
Per-core optimizations (two-nonce interleaved CLHash on big cores, fused
case dispatch on ARM A75+-generation big cores, 4-lane SoA scrypt) are
selected automatically per thread at runtime.

Numbers vary with thermal headroom, governor, and per-SoC core mix.

> **Android: getting full speed from the big cores.** Android throttles apps
> that are not the *focused* foreground app, in two distinct ways:
>
> 1. **Withheld cores (cpuset).** Backgrounded or with the screen off, the OS
>    removes one or more cores from the app's `top-app` cpuset; the miner logs
>    `Platform allows this process only N of M CPUs`. With more threads than
>    allowed cores the extras share a core (`only N core(s) available; sharing
>    CPU X with thread Y`) and show up at half rate.
> 2. **Capped frequency (uclamp).** Even when every core is granted, a
>    non-foreground process can be held at a low CPU frequency, so a big core
>    delivers little-core hashrate. The miner detects this and logs `CPU n
>    (Cortex-Xn) only A/B MHz under sustained load — big cores appear
>    frequency-capped`.
>
> Both are OS policy the miner cannot override, and both have the same fix: keep
> the Termux app open on screen, or launch the miner from an **SSH or `adb shell`**
> session — shell sessions run in the all-core, unthrottled `top-app` group even
> with the screen off. A withheld core is re-adopted automatically (~20 s) once
> it returns.
>
> **MediaTek SoCs** additionally hotplug the big-core cluster *offline* under
> sustained thermal load (the kernel uses CPU hotplug as a cooling device), so
> the online-core set flaps in the log. This is firmware thermal management and
> needs root to disable — improve cooling or accept the reduced sustained rate.

## Building

```bash
# Makefile builds now track header dependencies automatically.
make

# or build with CMake into ./build and refresh the repo-root binary
./build.sh

# run the test harness: per-algo self-tests + end-to-end share round-trips
# against a local mock stratum pool (plain TCP and TLS), ~45 s
make test
```

If you are switching branches, changing toolchains, or recovering from an older mixed-object worktree, use:

```bash
make clean && make -j"$(nproc)"
```

**Requirements:**
- 64-bit ARM CPU (aarch64) with crypto extensions (ARMv8+)
- 32-bit ARM (ARMv7) is not supported and won't be: the hot paths are
  hand-written AArch64 assembly, and ARMv7 lacks the PMULL/AES/SHA2
  instructions these algorithms are built on. 32-bit-only phones are also
  simply too old to earn anything mining — that hardware is below even the
  "old phone" bar this project aims for.
- `make` build defaults to `clang-16` / `clang++-16` with `lld`
- Alternate compatible Clang driver names can be selected with `CC=...`, `CXX=...`, and `PRIMO_LINKER=...`
- libcurl, libjansson

Examples:

```bash
# Default validated toolchain
make -j"$(nproc)"

# Override compiler drivers and use the toolchain's default linker
make clean
CC=clang CXX=clang++ PRIMO_LINKER= make -j"$(nproc)"
```

`./build.sh` uses the same overrides for the CMake path via `CC`, `CXX`, and `PRIMO_LINKER`. Leaving `PRIMO_LINKER` empty drops the `lld`-specific linker selection.

### Device build profiles

The Verus CLHash hand-scheduled assembly is **selected at runtime per core**
(it is compiled into every binary and enabled on out-of-order cores), so a
single binary is optimal on every device — there is no longer a separate
"fast" build to pick. `make` defaults to `PROFILE=rk3588`, which adds only the
Cortex-A53 erratum workaround (a harmless NOP on non-A53 cores). For phones and
SBCs without a real Cortex-A53 core you can drop it:

```bash
make PROFILE=generic
```

Both profiles produce the same runtime-dispatched binary; `generic` only omits
the A53 erratum NOP. Both keep the `-mtune=cortex-a53` codegen tuning, which
benchmarks fastest across heterogeneous big.LITTLE SoCs. All per-core kernel
selection (the CLHash assembly, interleaved/fused CLHash, SoA scrypt) happens at
runtime. (To force the portable C CLHash path for debugging: `VERUS_ASM=0`.)

> **Do not raise `-march` to `armv8.2-a`.** It implies the LSE atomics extension,
> which the compiler then emits inline; on an ARMv8.0 core (common in budget and
> older SoCs) those instructions fault with `SIGILL` the moment mining starts.
> The build stays on `-march=armv8-a+crypto` so one binary runs on every ARMv8 core.

## License And Provenance

- Distributed under `GPL-3.0-or-later`. See [`LICENSE`](LICENSE).
- Project-level release notice: [`NOTICE`](NOTICE).
- Known file and subsystem ancestry: [`PROVENANCE.md`](PROVENANCE.md).
- Third-party notices that remain in-tree, such as `src/algorithm/scrypt.h`, must be preserved in redistribution.

## Usage

### Command Line (ccminer-compatible)

```bash
# Basic mining
./primo-arm-miner -a verus -o stratum+tcp://pool.verus.io:9998 -u WALLET.worker

# With all options
./primo-arm-miner \
  -a verus \
  -o stratum+tcp://pool.verus.io:9998 \
  -u RWallet123.worker \
  -p x \
  -t 8

# Benchmark mode
./primo-arm-miner --benchmark -t 4

# Benchmark scrypt
./primo-arm-miner -a scrypt --benchmark -t 4

# Benchmark sha256d
./primo-arm-miner -a sha256d --benchmark -t 4

# Mine Monero (RandomX; aliases: randomx / rx / xmr / monero)
./primo-arm-miner -a randomx -o stratum+tcp://pool.supportxmr.com:3333 -u XMR_WALLET -p x

# TLS pools: use stratum+ssl:// (stratum+tcps:// also accepted). Requires a
# TLS-enabled libcurl (any stock distro libcurl qualifies); certificates are
# not verified, matching common miner behavior — pool certs are self-signed.
./primo-arm-miner -a randomx -o stratum+ssl://pool.supportxmr.com:443 -u XMR_WALLET -p x
```

### JSON Config File (ccminer-compatible)

```bash
# Auto-load local ./config.json when present
./primo-arm-miner

# Or load an explicit config path
./primo-arm-miner -c config.json
```

**config.json:**
```json
{
  "algo": "verus",
  "url": "stratum+tcp://pool.verus.io:9998",
  "user": "RWallet123.worker",
  "pass": "x",
  "threads": 8,
  "api-bind": "127.0.0.1:4068",
  "quiet": false,
  "debug": false
}
```

Explicit CLI flags override values from the config file. That lets `config.json` act as a default profile while still allowing one-off overrides such as `-t 2` or `-a sha256d`. When `pools[]` is present, `-o/--url` switches the run into single-pool mode and ignores the configured pool array for that invocation.

See [`CONFIG_EXAMPLES.md`](CONFIG_EXAMPLES.md) for quick copy-paste examples and [`CONFIG_JSON.md`](CONFIG_JSON.md) for the full `config.json` reference, including `api-bind`, multi-pool keys, and precedence rules.

### Multi-Pool Config (ccminer pools.json compatible)

```json
{
  "user": "wallet.worker",
  "pass": "x",
  "pools": [
    {
      "name": "Primary",
      "url": "stratum+tcp://pool1.verus.io:9998",
      "timeout": 180,
      "disabled": 0
    },
    {
      "name": "Backup",
      "url": "stratum+tcp://pool2.verus.io:9998",
      "timeout": 180,
      "disabled": 0
    }
  ],
  "threads": 8
}
```

Top-level `user` and `pass` act as defaults for every pool entry. The miner skips `disabled` pools for startup and failover, and a pool-local `timeout` overrides the global `timeout` only for that pool.
CLI `-u`, `-p`, `-O`, and `-T` still apply as global overrides in multi-pool mode. CLI `-o` also remains valid: it switches that invocation into single-pool mode and ignores the configured `pools[]` array.

## Configuration Compatibility

This miner is intended to accept the same common config files and command-line arguments used by ccminer:

```bash
# Use your existing ccminer config
./primo-arm-miner -c /path/to/ccminer.conf

# Or just replace the binary and rely on local ./config.json
cp primo-arm-miner ccminer
```

### Supported Options

**Mining:**
- `-a, --algo` - Algorithm (`verus`, `sha256d`, `scrypt`, `randomx`)
- `-o, --url` - Pool URL
- `-u, --user` - Wallet + worker
- `-p, --pass` - Password
- `-O, --userpass` - User:pass format
- `-t, --threads` - Thread count
- `-c, --config` - Config file

**Network:**
- `-r, --retries` - Retry count
- `-R, --retry-pause` - Retry delay
- `-T, --timeout` - Timeout

Startup uses the same retry policy as steady-state reconnects. With `-r -1`, the miner keeps retrying until interrupted.

**Display:**
- `-D, --debug` - Debug output
- `-P, --protocol-dump` - Protocol dump without requiring `-D`
- `-q, --quiet` - Quiet mode
- `-N, --statsavg` - Stats window

**Misc:**
- `--nicehash` - RandomX: the pool owns the nonce's top byte, so the miner
  scans only the low 24 bits. Auto-enabled when the pool advertises the
  `nicehash` extension at login (NiceHash, xmrig-proxy, and mining-proxy
  setups generally) — pass it only if your pool assigns nonce slices without
  advertising them. Harmless elsewhere, but it does shrink the scan space.
- `--benchmark` - Offline synthetic benchmark mode for all supported algorithms
- `-b, --api-bind` - API endpoint
- `-V, --version` - Version
- `-h, --help` - Help

## Dev Fee

**Dev fee is disabled in this fork.** The fee targets in `src/dev_fee.cpp`
are compiled out entirely (empty per-algorithm target list), so the fee
scheduler never arms and no time slice is ever taken on any algorithm.

Upstream PrimoLab's `primo-arm-miner` includes a small default dev fee —
see their project (linked above) for details if you're curious how it
works there. This is a GPL fork, and disabling it was a deliberate choice
for this build.

## Validation

- Startup self-tests verify the scrypt SoA path against a reference
  implementation on every launch; `VERUS_X2_SELFTEST=1` cross-checks the
  interleaved Verus path hash-for-hash.
- Live share acceptance verified on pool.verus.io (Verus, 100% over 350+
  shares), public-pool.io (SHA256d), and litecoinpool.org (scrypt).

## Credits

**Project Contributors:**
- Verus implementation by Monkins1010
- ARM optimization by Mixed-Nuts

**Release Notes:**
- Project release ancestry is documented in [`PROVENANCE.md`](PROVENANCE.md)
- Project redistribution notice is in [`NOTICE`](NOTICE)
- Built with substantial contributor assistance during the rewrite/compliance passes

## Disclaimer

This software is provided **as-is, without warranty of any kind**, and you
install and run it **entirely at your own risk**. The authors and
contributors accept **no responsibility for any damage** resulting from its
use.

Cryptocurrency mining is one of the most demanding workloads a device can
run. Be aware that sustained mining will:

- run your CPU at or near 100% load for extended periods, generating
  **significant heat** — especially on passively cooled phones and SBCs;
- accelerate **battery wear** on mobile devices (mine plugged in, ideally
  with the battery between charge limits, and never under a pillow or in
  direct sun);
- increase **power consumption** and may shorten the lifespan of hardware
  that is run hot for long periods.

Monitor your device temperatures, ensure adequate cooling, and stop mining
if a device gets too hot to touch comfortably. You are also responsible for
ensuring that mining complies with local regulations, your electricity
arrangements, and the terms of any pool you connect to. Double-check wallet
addresses — shares mined to a mistyped address are unrecoverable.

Use responsibly.

## Internals

Architecture, locking rules, per-algorithm invariants, the self-test matrix,
and the Android pinning story are documented for contributors in
[`docs/INTERNALS.md`](docs/INTERNALS.md).

## License

This repository is released under `GPL-3.0-or-later`.

See:
- [`LICENSE`](LICENSE) for the full license text
- [`NOTICE`](NOTICE) for project-level release notice
- [`PROVENANCE.md`](PROVENANCE.md) for known ancestry and compliance notes
