# Distribution & Release Pipeline

**This document describes upstream PrimoLab's own release infrastructure for
primo-arm-miner — their domain, their forge mirrors, their signing key, their
maintainer build box. None of it is part of this fork. It's kept below,
clearly marked, purely as historical/reference context for how the upstream
project this was forked from does releases. It does not describe
Ghuleh Miner and none of these URLs, keys, or org links belong to this
project.**

## How Ghuleh Miner is actually distributed

This fork is not published as a public release feed. It lives in a private
GitHub repository (`github.com/heather2084/ghuleh-miner`), built and signed
locally, and installed by sideloading the built APK (or CLI binary) directly
onto each device. There is no public download site, no forge mirror, and no
`install.sh` — that script has been removed from this fork because it pointed
at PrimoLab's own download server, not anything this project controls. See
the root [README.md](../README.md) for the actual build/install steps.

---

## Upstream's pipeline (historical reference only — not this fork)

How primo-arm-miner gets from a `master` commit to a one-line install on any
ARM64 device, per upstream's own docs. First written 2026-06-09 for a
GitHub-Actions pipeline; rewritten 2026-07-06 when distribution moved to its
current, forge-independent shape.

### Canonical downloads: primolab.dev/dl/ (upstream's site, not this fork's)

Upstream releases are served from **<https://primolab.dev/dl/>** — PrimoLab's
own site — and code forges (GitLab, later GitHub) act as mirrors. This is
deliberate: the project's GitHub org was auto-flagged by abuse detection in
2026-06 (new org + crypto-miner topic), which taught upstream not to make any
single forge the load-bearing download host.

Per release, `/dl/` holds versioned artifacts plus stable "latest" aliases,
each with a `.sha256` next to it and a combined `SHA256SUMS`:

| Asset | Target |
|---|---|
| `primo-arm-miner-<ver>-linux-arm64.tar.gz` (alias `primo-arm-miner-linux-arm64.tar.gz`) | arm64 SBCs / servers (glibc) |
| `primo-arm-miner-<ver>-android-arm64.tar.gz` (alias `primo-arm-miner-android-arm64.tar.gz`) | Termux on Android (bionic) |
| `primo-arm-miner-<ver>.apk` (alias `primo-arm-miner.apk`) | Android APK (release-signed) |

Each tarball contains the binary plus `LICENSE`, `NOTICE`, `README.md`,
`CONFIG_EXAMPLES.md`, and `PROVENANCE.md` — binaries never travel without the
GPL license text.

**Upstream's release APK signing certificate (SHA-256 digest, their key, not
this fork's):** `c28b614c2f6c67673ae1b1c98db914bbfb11853c0a3d49a6190ba233981cd8c3`.
This fork is signed with a separate, independently generated key — see the
main project README for how to verify a Ghuleh Miner build.

### Forge mirrors (upstream's)

- **GitLab (live since v1.0.7)**: <https://gitlab.com/PrimoLab/primo-arm-miner>
  — full source history, tags, and per-release pages. Release assets are
  *copies* uploaded to the project's generic package registry (not links back
  to primolab.dev), so the mirror stays downloadable even if the primary host
  is down. `/-/releases/permalink/latest` always redirects to the newest
  release — the site's mirror buttons use it so they never go stale.
  Per release: push `master` + the tag, upload the three artifacts +
  `SHA256SUMS` to `packages/generic/primo-arm-miner/<ver>/`, create the
  release with those asset links, and verify one asset hash anonymously.
- **GitHub (live since 2026-07-13, org flag cleared after 33 days)**:
  <https://github.com/PrimoLab/primo-arm-miner> — same procedure as GitLab.
  v1.0.7 mirrored: master + tag pushed, release created with asset *copies*
  (uploaded via `gh release create`, verified hash-identical to `/dl/`);
  the broken v1.0.5 release + remote tag were deleted (glibc-2.38 build).
  `releases/latest` is the evergreen link the site buttons use.
  **Actions is disabled repo-wide** (API setting, not a commit) so pushes
  can't trigger miner builds on shared runners — the CI burst that caused
  the original flag. Re-enable deliberately or never; `/dl/` is canonical.

Artifact SHA-256s must be byte-identical between `/dl/` and every mirror —
a divergence means a stale or tampered mirror and is a release-blocker
(for upstream's own release process).

### How upstream builds releases (their maintainer side)

One command on the project's own arm64 box, using two frozen container
images (an aarch64 host is non-negotiable — see "Why not cross-compile"):

- **Linux artifact**: Debian + clang-16 container; `make` + the full
  `make test` harness gate the release (self-tests plus live share
  round-trips against a mock pool, TCP and TLS).
- **Termux binary, static `libprimo.so`, and APK**: a `termux-docker`
  container carrying the *pinned clang-16 toolchain* (the same one validated
  on-device — Termux's stock newer clang measured ~5% slower on Verus).
- Artifacts are `--version`-verified, SHA-256'd, and staged to `/dl/`.

The orchestration script and its runbook live in upstream's (separate,
private) site repository. The container images were validated by A/B against
phone-built golden binaries before being trusted (CLI hashrate mid-bracket,
APK live-mined all four algorithms with zero rejects).

**Why not cross-compile:** no official aarch64-host Android NDK exists, and
toolchain identity is a *measured* performance variable. Consensus-critical
hash code gets built by the exact validated toolchain or not at all.

**glibc floor (upstream's builder, checked every time their linux builder
image changes):** the linux binary's floor is whatever glibc the builder
container has. History: v1.0.5's first build (Ubuntu 24.04 runner) required
GLIBC_2.38 and would not start on Debian 12 or Raspberry Pi OS — caught only
by a real install test. Current builder (Debian 12 "bookworm") yields a
**GLIBC_2.34 floor**: covers Debian 12+, Ubuntu 22.04+, current Raspberry Pi
OS and Armbian; drops Debian 11 (glibc 2.31, LTS ends 2026-08). Verify after
any image rebuild: `objdump -T primo-arm-miner | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1`.

**Runtime dependencies: libcurl + libjansson only.** OpenSSL was removed
2026-06-10 — it was two calls (SHA-1 + base64 for the WebSocket handshake),
now local in `src/api.cpp`. This deliberately avoids the libssl 1.1-vs-3
SONAME split that would otherwise reintroduce the same portability problem an
old-glibc build was meant to solve. The binary's `NEEDED` list is
libcurl.so.4, libjansson.so.4, libm, libstdc++, libc — all decade-stable.
(TLS stratum rides the *system* libcurl; no TLS library is linked.)

This applies equally to builds of this fork, since the source it's built
from is unchanged in this respect.

### GitHub Actions workflow in this repo

`.github/workflows/release.yml` in this fork is a mechanically-renamed copy
of upstream's build/smoke-test workflow (artifact names updated to
`ghuleh-miner-*`). It builds and smoke-tests the tarballs on GitHub's free
arm64 runners; it does not publish anywhere on its own, and this fork does
not run a public release pipeline. If it is ever used, its three historical
CI fixes still apply: `chmod -R a+w` the checkout before the termux
container (uid mismatch), build + smoke-test in ONE container session, and
build the linux artifact inside an old-glibc container (see floor above).

### Portability of the optimized paths (applies to this fork too)

Everything performance-critical adapts at **runtime**, which is what makes
single-binary distribution safe — this is unchanged source behavior shared
with upstream:

- ARMv8 crypto extensions (AES/PMULL/SHA2) are detected at startup; the
  miner refuses to run without them (they are baseline on every ARMv8
  phone/SBC SoC of the last decade).
- big/LITTLE topology detection (sysfs MIDR + `/proc/cpuinfo` fallback for
  Android, frequency-median fallback for unknown parts) drives thread
  pinning and the per-core algorithm policies.
- **CLHash hand-asm is runtime-dispatched** (`_asm`/`_noasm` symbol variants,
  per-thread function pointers), so the historical rk3588-vs-generic build
  split no longer exists — one universal binary is optimal everywhere.
- **Verus x2 interleave**: bit-identical output on any ARMv8 CPU (same
  NEON/PMULL instructions as x1 — only the scheduling differs), so it is
  *correct* everywhere unconditionally. The speedup requires an out-of-order
  core (+24% on A76); in-order LITTLE cores auto-fall back to x1. The fused
  dispatch variant is gated by a MIDR allowlist (ARM-designed A75+; −24% on
  Samsung Mongoose M4 taught us why).
- **Scrypt SoA-4**: same runtime policy (big cores only); unknown topology
  defaults to the safe dual-scalar path.
- **SHA256d**: the dual-nonce asm helps in-order and OoO cores alike; NEON
  fallback exists for (hypothetical) cores without the SHA2 extension.

### install.sh (removed from this fork)

Upstream ships a POSIX `install.sh` that downloads pre-built binaries from
`primolab.dev`. This fork removed that script because it silently fetched
someone else's binaries under this project's name — this fork is
build-from-source only (see the root README).

For reference, what upstream's script did: detects Termux vs Linux and
`aarch64`, rejects 32-bit ARM with an explanation (ARMv8 crypto required),
warns if `/proc/cpuinfo` lacks `aes`, downloads the right asset from
upstream's `/dl/`, verifies its SHA-256 against the published `.sha256`
(hard-fails on mismatch; https enforced for the default URL), installs
runtime libs best-effort, installs to `$PREFIX/bin` (Termux) or
`/usr/local/bin` (falling back to `~/.local/bin` without sudo), verifies the
binary runs, prints a mining quickstart.
