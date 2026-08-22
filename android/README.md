# Ghuleh Miner — Android APK (Milestone 1)

(Fork of primo-arm-miner. Historical entries below predate the fork and
describe upstream's own build/release process — kept for context, not
all still literally true here. Command output paths have been corrected
to match this fork's actual `build_apk_termux.sh`.)

A thin Android wrapper around the existing native miner. **No miner rewrite** —
the same C++ binary runs as a foreground-service subprocess, and the UI just
talks to its read-only API on `127.0.0.1:4068`.

## Architecture

```
┌─────────────────────────────────────────────┐
│  APK                                          │
│  ┌─────────────┐   ┌──────────────────────┐  │
│  │ MiningActivity│  │ ConfigActivity        │ │
│  │ start/stop +  │  │ algo/url/user/pass/   │ │
│  │ live stats    │  │ threads -> config.json│ │
│  └──────┬──────┘   └──────────┬───────────┘  │
│         │ poll 4068           │ write          │
│         ▼                     ▼               │
│  ┌──────────────────────────────────────┐    │
│  │ MinerService (foreground + wakelock)  │    │
│  │   exec nativeLibDir/libprimo.so       │    │
│  │       -c config.json --api-bind ...   │    │
│  └──────────────────────────────────────┘    │
└─────────────────────────────────────────────┘
```

- The miner ships **inside the APK as `lib/arm64-v8a/libprimo.so`**. Naming it
  `lib*.so` + `extractNativeLibs="true"` lands it in `nativeLibraryDir`, the one
  app directory Android mounts executable — so we can `exec()` it directly.
  This sidesteps W^X / exec-from-data restrictions on modern targetSdk.
- UI is **plain Android framework Views (no Jetpack Compose / no AndroidX)** so
  the whole thing builds on-device in Termux without Gradle/AGP.

## Build (on an Android phone, over SSH, in Termux)

This is the intended build host — Termux ships native-aarch64 versions of the
whole APK toolchain, whereas the standard NDK/aapt2/d8 are x86_64-only.

```bash
# 1. one-time toolchain
pkg install openjdk-17 kotlin aapt aapt2 d8 apksigner android-tools zip

# 2. build a SELF-CONTAINED native miner (static curl+jansson) and stage it +
#    libc++ into jniLibs. The stock build_termux.sh binary is NOT usable in the
#    APK — it dynamically links Termux's libcurl/jansson/libc++ (see notes below).
bash android/build_native_termux.sh

# 3. build + sign the APK
cd android && bash build_apk_termux.sh
# -> android/build/ghuleh-miner.apk  (debug-signed)
```

### Alternative: the same build in docker (arm64 Linux host)

The identical scripts also run inside
[termux-docker](https://github.com/termux/termux-docker) on any arm64 Linux
box (`termux/termux-docker:aarch64`) — validated against on-device builds
(byte-count-identical output, A/B'd hashrates on a real phone). The
on-device path above remains the reference method; a phone is still the
only place to *validate* mining performance. Container-specific gotchas:

- termux-exec's shebang rewriting isn't active under `docker exec`/`RUN`:
  create `/bin/sh` and `/usr/bin/env` symlinks to their `$PREFIX/bin`
  equivalents (as root) or scripts won't exec.
- `docker cp` writes files as root; `chown 1000:1000` them for the
  `system` build user.
- Run commands through `/entrypoint.sh` (e.g.
  `docker exec <c> /entrypoint.sh sh -c 'pkg ...'`) so the Termux
  environment is set up.
- Same package list as above, plus `wget` (static-deps downloads). `aapt`
  matters: it's what provides `zipalign`.

### Why the native build is special
- The APK binary must depend only on **bionic system libs** (+ a bundled
  `libc++_shared.so`). `build_native_termux.sh` builds **static** libcurl
  (TLS via static **mbedTLS**, so `stratum+ssl://` works from the APK too) +
  static libjansson via `build_static_deps_termux.sh`, then relinks the miner
  against them. RandomX made two things matter that never did before: the standalone
  clang-16 ships **no libc++ headers** (the clang++ wrapper adds Termux's
  `include/c++/v1` — the miner's own C++ never needed them), and the relink
  must link Termux's `libc++_shared.so` explicitly (`-nostdlib++`) because the
  driver's implicit `-lc++` resolves to the symbol-poor `/system/lib64`
  libc++.
- **DNS:** the app sandbox blocks `getaddrinfo` from a raw native subprocess
  ("Could not resolve host"), even though TCP works. `MinerService` resolves the
  pool host on the JVM side and launches the miner with the IP
  (`config.runtime.json`); `stratum+tcp` needs no hostname. Hostnames the JVM
  can't pre-resolve (the compiled-in dev-fee pools, pool-directed
  `client.reconnect` targets) are covered by the miner's own **DNS fallback**
  (`src/utils/dns_fallback.c`): on a resolver failure it queries
  1.1.1.1/8.8.8.8 directly over plain sockets — which DO work in the
  sandbox — and retries via `CURLOPT_RESOLVE`.

Install: `adb install -r android/build/ghuleh-miner.apk`, or copy to the
phone and tap it (enable "install unknown apps").

## Pool configuration notes

### RandomX (Monero)

Select `randomx` as the algorithm and point it at a Monero pool
(`stratum+tcp://host:port`, user = your XMR address, optionally `.worker` or
`+<difficulty>` per the pool). Failover pools work the same as the other algos.

RandomX is **memory-hard**: fast mode allocates a ~2.1 GiB dataset. The app
picks fast vs **light mode** (256 MiB, ~5x slower) automatically from device
RAM — light on phones under ~3 GiB total, or when memory is already tight — and
the native miner also falls back to light if the big allocation fails. Expect
heavy thermal throttling: RandomX is far hotter than the other algorithms, so
sustained phone hashrate is cooling-limited.

### Merged mining (Litecoin + Dogecoin, scrypt)

LTC+DOGE merged mining is handled **entirely by the pool** (AuxPoW) — the scrypt
miner needs no special mode and there is no separate "merge mining" field in the
app. You enable it purely through **the pool you point at and your login / worker
name**:

1. Choose a pool that does LTC+DOGE merged mining (e.g. litecoinpool.org,
   prohashing, aikapool, or any "scrypt merge" pool).
2. In **Config**, set algorithm = `scrypt` and the pool's `stratum+tcp://…` URL.
3. Put your credentials in **WALLET / USER.WORKER** exactly how that pool wants
   them — this is where merged mining is actually configured:
   - **Account-based pools** (litecoinpool.org, prohashing): the user field is
     your *site username*, e.g. `myaccount.worker1`. Your LTC **and** DOGE payout
     addresses are set on the pool's website; both coins are credited
     automatically from the same scrypt shares.
   - **Address-based pools**: the user field is your LTC address plus a worker,
     e.g. `Lxxxxxxxx.rig1`. If the pool also wants a DOGE address it's typically
     given on their site or appended to the worker/password per their docs.

The miner just submits scrypt shares; the pool splits the reward across LTC and
DOGE. So "set it up correctly on the pool side, mine normally" is the whole flow.

### Failover pools

Each algorithm can have a **primary pool plus up to 3 failovers** (Config →
POOLS → "+ ADD FAILOVER POOL"). They are tried in card order: the miner starts
on the first reachable pool and fails over down the list when a pool is
unreachable or drops. Failover cards may leave **wallet/password blank to reuse
the primary's** (set them only if the backup pool needs different credentials).
The dashboard's pool line shows the pool you are *currently* mining on.

### Network monitoring (API on the LAN)

The dashboard always reads the miner's read-only status API on `127.0.0.1:4068`.
The **Allow network monitoring** checkbox in Config controls the *bind address*:

- **off** (default): API binds `127.0.0.1` — reachable only from this phone.
- **on**: API binds `0.0.0.0` — other devices on the same wifi can query it,
  e.g. a desktop ccminer-compatible monitor at `http://<phone-ip>:4068`. It stays
  **read-only** (summary / threads / pool / hwinfo, etc. — no control commands).

Takes effect the next time mining starts.

## Status / TODO

**Core path validated end-to-end on a rooted Galaxy S10+ (Exynos 9820, 2026-06-25)
— mines in-app at ~30-39 MH/s sha256d with the API live.**
- [x] Scaffold: manifest, 2 activities, foreground service, API client, build script
- [x] On-device build pipeline (aapt2 → javac → kotlinc → d8 → zipalign →
      apksigner). Needed: JDK-11 bytecode (`--release 11` / `-jvm-target 11`, since
      Termux defaults to JDK 21 and d8/build-tools 33 only supports Java 11) + `zip`.
- [x] APK installs; foreground service execs the binary from `nativeLibraryDir`.
- [x] **Self-contained native binary** via static curl+jansson + bundled libc++
      (`build_native_termux.sh`). NEEDED = libm/libc++/libdl/libc only.
- [x] **DNS** resolved JVM-side, miner launched with pool IP (every `pools[]`
      entry is resolved, not just the primary, so failover works in the sandbox).
- [x] Config UX: sectioned form (MINER / POOLS / MONITORING); algorithm is a
      **dropdown**; each algo keeps its own pools/threads (`profiles.json`) so
      switching algos repopulates that algo's data. Existing config.json (and v1
      single-pool profiles) auto-migrated. Miner unchanged — SAVE writes the
      ccminer-compatible `pools[]` config.json from the active algo. All app
      config writes (`profiles.json`, `config.json`, `config.runtime.json`) are
      atomic (`writeTextAtomic`: tmp + fsync + rename) so a kill/power loss
      mid-write can't corrupt the user's wallets/pools.
- [x] **Pool failover**: primary + up to 3 failover pools per algo as removable
      cards, priority order = card order, riding the miner's existing failover.
      Blank failover user/pass inherit the primary's (top-level inheritance).
      Each pool's `name` is set to its hostname so the API/dashboard show a
      readable label even after the DNS→IP rewrite; the dashboard's pool line is
      the LIVE pool from the API (`pool` command), so a mid-run failover is
      visible. Validated on this box: dead primary → failover → 17/17 accepted.
- [x] **Miner crash detection**: `MinerService` watches the subprocess exit and
      stops the service with a note (`exitNote`); the dashboard shows
      "STOPPED: MINER EXITED (CODE N)" instead of an eternal "connecting…".
      The pill/status are driven by real service state (`MinerService.running`),
      which also fixed the brief post-Stop pill flicker.
- [x] Save validation: blank primary pool URL is rejected with a toast instead
      of silently writing a config the miner can't use.
- [x] **Per-algorithm coin-color accents** (`Palette.kt`): verus = Verus blue,
      sha256d = Bitcoin orange, scrypt = Litecoin silver-blue, randomx = Monero
      orange (teal = fallback). Hero card (runtime gradient + algo badge),
      hashrate, MINING status, thread chips, START pill and the config page's
      headers/SAVE all re-tint live when the algorithm changes.
- [x] **Dark dashboard home page** (custom theme, coin accent): big hashrate,
      algo/pool, live per-thread chips (from `threads`), stat grid (accepted/
      rejected/difficulty/uptime/max-hash/temp/battery/charge), Start/Stop pill.
      Config moved to a **cog in the action bar** (no more button). Battery/charge
      from Android `BatteryManager`; everything else from the 4068 API.
- [x] Mining confirmed: stratum connect/authorize/work + 4068 API summary.
- [x] minSdk 24 (Android 7+), arm64-v8a only — installs on essentially any ARMv8 phone.
- [x] **Full-speed-while-foreground (no root):** the mining screen sets
      `FLAG_KEEP_SCREEN_ON`, so a focused on-screen app stays `top-app` = all cores
      + uclamp boost. This is the no-root performance ceiling.
- [x] Battery-optimization exemption request (so OEMs don't reap the service).
- [x] **Root booster (STUB):** `RootBooster` moves the miner into the `top-app`
      cpuset on rooted devices = full cores even backgrounded/screen-off. No-op
      without root. Confirmed working on the magisk S10+. TODO: periodic re-assert
      + a UI toggle.
- [x] **App icon** — the "P_" brand mark in the site palette (replaced the
      original CPU-chip vector): adaptive icon (`mipmap-anydpi-v26`) +
      layer-list fallback (`mipmap-anydpi`) so it works API 24+, foreground
      PNGs per density. App accent aligned to the site brand teal. Verified
      rendering on-device.
- [x] **Service lifecycle serialized** (2026-07-10): `stateLock` guards
      process/wakelock state; the async launch re-checks `stopping` after the
      DNS/config prep so a quick Start→Stop can no longer orphan a miner
      (which also squatted the 4068 API port); wakelock acquisition is
      idempotent across repeated onStartCommand deliveries.
- [x] **RootBooster hardened** (2026-07-10): `su` calls get a 5 s watchdog
      (minSdk 24 has no `waitFor(timeout)`); root = exit 0 AND a line exactly
      `0` (a `contains("0")` check misclassified uid 1000); timeouts aren't
      cached so a later root grant still works.
- [x] **License texts bundled** (2026-07-10): GPL-3.0 / NOTICE / RandomX BSD-3 /
      Apache-2.0 ship in the APK under `assets/licenses/`; the first-launch
      disclaimer states the license and source URL (GPL binary-distribution
      compliance).
- [x] **In-app help + site linkbacks** (2026-07-12): every config section header
      (MINER / POOLS / MONITORING) carries a ⓘ button that opens a popup
      explaining that section's inputs, with a LEARN MORE button to the matching
      primolab.dev page — POOLS links to the page for the coin being configured
      (mine-verus / mine-monero / …), MINER to the FAQ, MONITORING to the docs.
      The dashboard's overflow menu gains **About** (app version + a
      primolab.dev button). All site URLs live in one place (`Links.kt`).
- [x] **Log page upgraded** (2026-07-12): lines are re-colored with the app
      palette using the site-terminal convention — dim timestamps, green
      reserved for the word "Accepted", red for Rejected/errors, amber for
      failover/reconnect/timeout events (keyword-based: the subprocess writes
      to a pipe, so the native logger emits no ANSI — the strip regex stays as
      defense). Action-bar **share** button sends the log tail + app version +
      device model (the tester "send me your log" flow, two taps). A
      "↓ LATEST" pill appears when scrolled up so returning to the tail is one
      tap; auto-follow still pauses while reading scrollback.
- [x] **POST_NOTIFICATIONS runtime request** (2026-07-12): Start now prompts on
      Android 13+ when not granted (manifest entry alone never prompts — the
      "Mining…" notification was silently invisible). The request happens
      BEFORE the service launch (it posts the notification once, at
      startForeground, so a later grant wouldn't show it) and mining starts
      from the callback whether granted or not — the notification is
      status-only, never a gate.
- [ ] targetSdk 34 needs a real `foregroundServiceType` justification (currently
      `dataSync` at targetSdk 33). DEFERRED until a bump is forced: sideloaded
      APKs have no Play deadline and Android 15/16 install targetSdk 33 fine;
      at 34 `dataSync` gets a 6-hour cap, so the plan is `specialUse` + the
      PROPERTY_SPECIAL_USE_FGS_SUBTYPE justification.
- [x] **First-launch disclaimer dialog** (`MiningActivity.showDisclaimer`): states
      it's a cryptocurrency CPU miner (heat/power/battery/wear), discloses the
      **dev fee** (2% verus / 1% sha256d+scrypt+randomx, time-sliced — see
      `src/dev_fee.cpp`), and an own-risk / no-responsibility / no-warranty clause.
      Non-cancelable; I UNDERSTAND accepts, EXIT closes the app. **"Don't show
      again"** (checked by default) persists as `disclaimerOk` in `profiles.json`
      (same mechanism as `lanApi`). Note: `disclaimer_text` needs
      `formatted="false"` — the `%` fee figures trip aapt2's format-arg check.
- [x] Test on a NON-rooted device — DONE in the field: Note 20 Ultra working,
      plus non-rooted testers (AGTERM, CupofX) run it with exactly the expected
      background throttling ("N of M CPUs" + uclamp); full speed while
      on-screen, as designed.
- [x] Per-thread / temp view (poll `threads` + `hwinfo` API commands)
- [x] **Thread chips wrap to centered rows** for >8-thread phones (10/12-core),
      with status as a colored pill (mining/connecting) and threshold-colored temp.
- [x] **Optional LAN API** — a Config checkbox binds the status API to `0.0.0.0`
      for remote ccminer-compatible monitoring (default stays `127.0.0.1`).
- [x] **Settings cog** replaces the framework wrench in the action bar (vector).
- [x] **In-app log viewer** (`LogActivity`): read-only tail of the miner's
      `miner.log` (drained native stdout), ANSI-stripped, 2s auto-refresh, opened
      from a log icon beside the cog. Useful for diagnosing on non-rooted devices
      without `adb logcat`.
- [x] **IPv6 pool fix**: pool hosts that resolve to IPv6 were producing an
      unparseable `stratum+tcp://<v6>:port` URL (silent connect-retry loop, UI
      stuck "connecting"). `MinerService` now prefers an IPv4 address, bracketing
      IPv6 (`[addr]:port`) only when that's all a host offers.
- [x] Release signing keystore — DONE 2026-07-06: `build_apk_termux.sh`
      release-signs when `PRIMO_KEYSTORE` (path to the release `.jks`) +
      `PRIMO_KS_PASS` (+ optional `PRIMO_KS_ALIAS`, default `ghuleh`) are
      set → `build/ghuleh-miner-release.apk`, printing the cert SHA-256
      fingerprint (publish it with releases). Unset = debug keystore,
      unchanged. The keystore lives OUTSIDE all repos (backed up privately);
      the SAME key must sign every release forever — a changed key forces
      users to uninstall (losing in-app config) to update. Signing is
      machine-independent: an unsigned/debug build can be re-signed
      anywhere with apksigner; the signature, not the build box, is the
      app identity. (Native lib builds stay on-device per the pinned
      clang-16 recipe.) The password reaches apksigner via
      `--ks-pass env:KS_PASS` (never `pass:` on argv, which is readable
      in `/proc/*/cmdline`).
- [ ] Optional later: Gradle/AGP project for x86 CI builds; JNI in-process variant
      if any device rejects subprocess exec

## Distribution

Historical entry, predates the fork — upstream's own plan for
primolab.dev/dl/, GitLab/GitHub mirrors, and an untracked path on
upstream's own machine. None of that is this fork's distribution path.
This fork is sideloaded from a locally-built, locally-signed APK — see the
root README and docs/DISTRIBUTION.md. APK signing: release keystore is
stable across versions — see the checklist item above.
```
