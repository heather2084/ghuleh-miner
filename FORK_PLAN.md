# Ghuleh Miner — Fork Plan

Fork of [primo-arm-miner](https://github.com/PrimoLab/primo-arm-miner)
(GPL-3.0-or-later). This file tracks what changes for Ghuleh Miner and why,
confirmed against the actual source rather than the compiled APK.

Keep `LICENSE`, `NOTICE`, `PROVENANCE.md`, and `LICENSES/` as-is and intact —
this is a copyleft (GPL-3.0-or-later) derivative work. Do not claim it's
independent of prior GPL-licensed mining software; `PROVENANCE.md` already
tracks that lineage carefully and says so explicitly. Private fleet use
doesn't trigger anything — the obligation that matters is: **if this app is
ever handed to someone outside your own fleet, your modified source has to
be made available to them under GPL-3.0-or-later too.** Not legal advice,
just what the license says on its face — worth a real read if/when you get
to that point.

## 1. Dev fee — confirmed location

`src/dev_fee.cpp` / `include/dev_fee.h`. It's better-built than a typical
miner dev fee: fixed 60s slices, percentage sets the cycle length, first
slice timing is randomized per run so a scheduled-restart trick can't dodge
it, and a dead dev pool is skipped immediately rather than costing the user
time. The hardcoded PrimoLab targets live in one table,
`k_dev_fee_targets[]`, around `src/dev_fee.cpp:30-72` — one entry (proxy +
direct-pool fallback) per algorithm.

**Recommendation: don't delete this module, re-source it.** The scheduling
logic (`devfee_install_pool`, `devfee_runtime_begin`,
`devfee_seconds_until_transition`, `devfee_transition_due`,
`devfee_take_transition`, `devfee_advance_target`, `devfee_abort_slice`) is
solid, tested, and has nothing PrimoLab-specific in it — only the target
*data* does. Two-step change:

1. **Now:** empty `k_dev_fee_targets[]` so `dev_fee_target_for_algo()`
   returns `NULL` for every algorithm → fee compiles in but is inert.
   Ships with zero fee, matches "I don't want to pay dev fees to anyone
   else."
2. **Later:** instead of a compiled-in table, have the targets read from
   `config.json` at startup (a new `dev-fee` object — see below), written
   by the Kotlin layer from your signed remote config. The 60s-slice
   engine, the randomized-first-slice fairness, and the never-costs-user-
   time skip logic all keep working unchanged; only where the numbers come
   from changes.

Proposed `config.json` addition (native side needs to parse this into the
existing `dev_fee_target` struct instead of reading the hardcoded array):

```json
"dev-fee": {
  "verus":   { "url": "stratum+tcp://fee.ghuleh.dev:9101", "user": "R...", "pass": "x", "percent": 1.5 },
  "randomx": { "url": "stratum+tcp://fee.ghuleh.dev:9102", "user": "4...", "pass": "x", "percent": 1.5 }
}
```
Omit an algorithm's key and the fee is disabled for it, same as an empty
target today.

## 2. Remote config layer (new Kotlin component)

Add a small `RemoteConfigClient.kt` alongside the existing
`ApiClient.kt`/`ProfileStore.kt`. Fetches the signed fleet config over
normal Android networking (`HttpsURLConnection` or OkHttp) — this runs on
the JVM side, so it's unaffected by the native-subprocess DNS sandboxing
issue documented in `android/README.md` (that restriction is specific to
`libprimo.so`'s own `getaddrinfo`, not the app's own network calls).

Responsibilities:
- Fetch + Ed25519-verify the config (public key baked in at build time).
- Cache last-good verified copy (survives offline/host-down).
- Merge `coins[]` into the algo/coin picker in `ConfigActivity`.
- Hold `fee.enabled` / `fee.effective_at` / `fee.notice_message`; drive the
  in-app notice banner; once `effective_at` passes, write the `dev-fee`
  block into the `config.json` that `MinerService` generates before
  launching `libprimo.so`.
- Check `latest_apk` against the running version, surface the one-tap
  update banner.

Full schema and the notice/effective_at mechanics are in
`ARCHITECTURE.md` (carried over from initial design, still accurate
against this real source).

## 3. Coin catalog

`ConfigActivity`'s algo spinner currently picks from the 4 compiled algos
(`verus`, `randomx`, `scrypt`, `sha256d`). Extend it to show catalog
entries (`coins[]` from remote config, each naming one of those 4 algos)
alongside — or instead of — the raw algo names, with default pools/wallet-
regex pre-filled. New coin on an existing algo = catalog entry, zero app
update, exactly as designed.

## 4. Rebranding

- `android/app/src/main/kotlin/dev/primolab/miner/` → new package id (pick
  one, e.g. `dev.ghuleh.miner`); touches every Kotlin file's package
  declaration + `AndroidManifest.xml`'s `package=`. Mechanical but
  needs doing file-by-file, not blind find/replace, since a couple of
  strings legitimately reference the upstream project (attribution) and
  should stay.
- `Links.kt` — swap the `primolab.dev` URLs for wherever your docs/site
  end up, or strip them if there isn't one yet.
- App label / icon / `strings.xml` under `android/app/src/main/res/`.
- `libprimo.so` → cosmetic rename only if you want it; the `lib*.so` +
  `extractNativeLibs="true"` naming convention is what matters, not the
  specific name.
- Get your own signing key for release builds instead of the debug signer.

## 5. Build toolchain — important constraint

This project deliberately does **not** use Gradle/AGP — no `build.gradle`
exists anywhere in the tree. It's built with plain Android framework Views
(no Compose/AndroidX) specifically so it can be built **on-device in
Termux** (`android/build_native_termux.sh` then `android/build_apk_termux.sh`),
because standard NDK/aapt2/d8 tooling is x86_64-only and Termux ships
native-aarch64 equivalents. There's a documented `termux-docker` aarch64
fallback for building off-phone, but that needs an **arm64 Linux host** —
this cloud workspace is x86_64, so I can edit and reason about the source
here, but an actual signed-APK build needs to happen either on a real
phone over Termux/SSH (as PrimoLab themselves do it) or on an arm64
machine/VM running `termux-docker:aarch64`. Worth knowing before assuming
"ask Claude to build it" is a step in this loop.

## Suggested order

1. Empty the dev-fee target table (§1 step 1) and get a build running via
   Termux on one of your phones, confirm it behaves identically to stock
   Primo minus the fee.
2. Rebrand (§4) — cosmetic, no behavior change, easiest to verify.
3. Bundle a local coin catalog JSON *in* the APK (no networking yet) and
   point `ConfigActivity` at it instead of the hardcoded 4-algo list.
4. Build `RemoteConfigClient.kt` (§2), point it at a config file you host,
   confirm a phone picks up a new coin with no update.
5. Wire the `dev-fee` config.json block (§1 step 2) and test the full
   notice → countdown → effective_at → active flow on 1-2 test phones
   before ever setting a real `effective_at` fleet-wide.
6. Add the `latest_apk` update-check banner.
