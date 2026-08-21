# Security Policy

## Reporting a vulnerability

TODO: replace with a contact you check regularly (not your personal
inbox, ideally — a dedicated address or a private GitHub security
advisory). Until this is filled in, treat the project as having no
disclosure process yet.

In scope: the miner binary and its stratum/API surfaces, the Android app,
and the release/distribution pipeline (artifact or signature tampering).
The read-only monitoring API (default `127.0.0.1:4068` in the app;
configurable for the CLI) is designed to be safe to expose on a trusted
LAN — reports that assume a hostile LAN are still welcome.

## Verifying what you run

TODO once there's a real release process: publish a SHA-256 of the
release signing certificate here (`apksigner verify --print-certs`
against a release-signed APK — not the Termux debug build), and put
`.sha256` files beside any APK download link, the way upstream
(primo-arm-miner) does. Until then, this fork's only "verified" artifact
is the git history in this repo itself.

## Known, deliberate trade-offs

These are inherited from upstream and still true of this fork's code —
not fixed, not planned to be fixed, documented so nobody assumes
otherwise:

- **`stratum+ssl://` does not verify pool certificates.** Mining pools
  overwhelmingly use self-signed TLS certificates, and the Android app
  connects by resolved IP (its sandbox blocks native DNS), so peer and
  hostname verification are disabled — the norm across mining software.
  Treat stratum TLS as transport privacy (defeats passive eavesdropping
  on the network you're on), not pool authentication (it does not prove
  you're actually talking to the pool you think you are). Prefer
  `stratum+ssl://` over plain `stratum+tcp://` anyway — better than
  nothing — but don't oversell it as more than it is. Stratum
  credentials are a wallet address, not a secret, which is why this is
  an acceptable trade-off rather than a real vulnerability.
- **DNS fallback queries public resolvers directly.** When (and only
  when) the system resolver fails, pool hostnames are resolved by
  querying Cloudflare (1.1.1.1) then Google (8.8.8.8) over plain port 53
  — this bypasses the system resolver and reveals the pool hostname to
  those services. Set `PRIMO_DNS_FALLBACK=0` to disable.

## No dev fee, no telemetry, no forced root — as of this fork

Whoever installs this APK, whether that's your own fleet or someone
else entirely, should be able to trust a few concrete things about it:

- **No dev fee is compiled in.** `src/dev_fee.cpp`'s target table is
  empty for every algorithm (see `FORK_PLAN.md` §1) — nothing mines to
  anyone else's wallet. If a fee is ever added, `ARCHITECTURE.md`
  describes the commitment this project is holding itself to: it ships
  disabled by default, it is driven by a config signed with a key that
  never leaves the maintainer's control, and the code path that would
  activate it is gated on a notice period elapsing first — there is no
  code path that can skim runtime silently, not a policy promise but a
  property of how the scheduler is built.
- **Root is optional, never required.** `RootBooster` is a pure
  performance option gated behind `isRootAvailable()` — the app is
  built and tested to work fully on stock, unrooted phones. Nobody
  installing this should feel pressured to root their device to use it.
- **Minimal permissions.** `WAKE_LOCK`, `FOREGROUND_SERVICE`,
  `POST_NOTIFICATIONS`, `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`,
  `INTERNET` — nothing that touches contacts, location, storage, or
  camera. Any future change that adds a permission should be able to
  explain why in one sentence here.
- **Updates will be integrity-checked.** The planned remote-config
  update-check (`ARCHITECTURE.md` §4) verifies a downloaded APK's
  SHA-256 against the same signed config as the fee/coin data before
  ever prompting install, so a compromised config host can't silently
  swap in a different binary.

## Operator notes (for whoever builds/deploys this — not an end-user concern)

- **Release signing key:** the Termux build in this repo currently
  produces a **debug-signed** APK — fine for testing, not for anything
  you'd hand to another person or rely on to update in place. Generate
  a real release keystore before distributing beyond your own test
  devices, and treat that keystore file + password as the single most
  sensitive artifact in this project. Losing it means every future
  update requires a fresh uninstall/reinstall on every phone that has
  the old version.
- **The remote-config signing key** (once built): this is the trust
  anchor for the fee/coin/update system across every installed copy.
  Keep the private key offline and off the machine that hosts the
  config file — sign locally, upload only the signed result.
- **Dependency versions are pinned and won't auto-update.** curl
  8.11.1, mbedTLS 3.6.2, jansson 2.14, LLVM/clang 16.0.6 are all built
  from source at fixed versions. Forking means you no longer
  automatically inherit upstream's security patches for these — worth
  periodically checking curl and mbedTLS in particular (they handle all
  network/TLS traffic) for anything serious and bumping versions rather
  than letting them sit for years.
- **Whatever remote-access tooling you use to manage deployed devices**
  (Tailscale, ADB, scrcpy, or anything else) is worth securing in
  proportion to how many devices it can reach — 2FA on the relevant
  accounts, and keeping the admin machine itself clean, protects
  everything reachable through it at once.
- **GitHub token hygiene:** use fine-grained, repo-scoped tokens (as
  opposed to classic all-repo tokens), let them expire rather than
  living forever, and revoke anything you're not actively using.

## Supported versions

Only the latest release is supported; there are no security backports.
