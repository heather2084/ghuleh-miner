# Ghuleh Miner — Remote Control & Extensibility Design

Design for a forked/rebranded version of Primo ARM Miner that: (1) ships with
no dev fee, (2) lets you add a fee later — and adjust or kill it — across
every already-installed copy with no app update, always with advance notice,
and (3) lets you add new coins without an app update in the common case.

The whole design rests on one idea: **separate what's *data* from what's
*code***. Pool addresses, wallets, fee percentages, and coin lists are data —
they can be pushed from a server you control and picked up by the app on its
own. Hashing algorithms are code — they have to ship inside the APK. Keeping
that line clean is what makes almost everything below possible.

---

## 1. One signed config file drives fee + coins + updates

Host a single JSON file anywhere (a GitHub Gist raw URL, S3, your own
domain — no real backend needed to start). The app fetches it on launch and
every ~6–12 hours while `MinerService` is running, verifies it, caches the
last good copy, and applies it.

```json
{
  "config_version": 7,
  "issued_at": "2026-08-25T00:00:00Z",

  "fee": {
    "enabled": true,
    "percent": 1.5,
    "cycle_minutes": 60,
    "notice_message": "Starting Sept 1, 2026, a 1.5% fee supports app development.",
    "effective_at": "2026-09-01T00:00:00Z",
    "targets": {
      "verus":   { "url": "stratum+tcp://fee.ghuleh.dev:9101", "wallet": "R..." },
      "randomx": { "url": "stratum+tcp://fee.ghuleh.dev:9102", "wallet": "4..." }
    }
  },

  "coins": [
    {
      "id": "vrsc",
      "name": "Verus",
      "algo": "verus",
      "default_pools": [
        { "url": "stratum+tcp://pool1.example:port", "priority": 1 },
        { "url": "stratum+tcp://pool2.example:port", "priority": 2 }
      ],
      "address_regex": "^R[a-zA-Z0-9]{33}$"
    }
  ],

  "latest_apk": {
    "version": 12,
    "url": "https://ghuleh.dev/releases/cellhasher-12.apk",
    "sha256": "..."
  },

  "signature": "base64-ed25519-signature-over-everything-above"
}
```

**Why signing matters more than usual here:** this file controls where
100+ phones' hashrate goes. Sign it with Ed25519, bake the public key into
the APK at build time, keep the private key offline on your own machine —
never on the config host. If the host ever gets compromised, an attacker can
edit the JSON but can't produce a valid signature, so the app rejects it and
falls back to the last verified config. This is the one piece of the whole
system worth not skipping, even for v1.

**Fail-closed behavior:**
- No successful fetch ever → fee disabled, no coins beyond what ships in the
  APK, no update prompt. Safe by default.
- Fetch fails (offline phone, host down) → keep using last verified config.
- Signature doesn't verify → discard, treat as a failed fetch. Never apply
  an unsigned or badly-signed payload.

---

## 2. The fee: on/off, adjustable, always with notice

Two gates control whether the fee actually runs, and both have to pass:

- `fee.enabled` — is a fee configured at all.
- `fee.effective_at` — has the notice period elapsed.

The moment the app sees a config where `effective_at` is in the future, it
shows a persistent (dismissible-but-reappearing) in-app banner with
`notice_message` and a countdown. The fee scheduler itself stays completely
inert until the clock passes `effective_at` — not "enabled," `effective_at`.
That's what makes "I won't do it without notice" a property of the code,
not just a promise: there's no code path that skims runtime before the
notice window ends, no matter what `enabled` says.

Once active, the mechanic is exactly Primo's: a timer periodically swaps the
live stratum connection to `fee.targets[<current algo>]` for
`(percent/100) * cycle_minutes` minutes per cycle, then swaps back. Since
you're already building multi-pool/failover support (`ProfileStore` in
Primo already does this), the fee target is just one more pool entry that
gets scheduled by a clock instead of by connection failure — no new
subsystem, just one more consumer of code you're building anyway.

**Changing your mind later:** push `percent: 0` or `enabled: false` and every
phone that's polled in the last ~12 hours drops the fee on its next check.
No update, no store review, no waiting on users.

---

## 3. Coins vs. algorithms — the distinction that makes "add coins easily" possible

- **A coin is data:** a name, which algorithm it uses, a default pool list,
  a wallet-address validation pattern. Nothing here requires native code.
- **An algorithm is code:** the actual hash function (RandomX, VerusHash,
  scrypt, sha256d), compiled into `libprimo.so` (or whatever you rename it).

**Adding a new coin on an algorithm you already support** (e.g. another
RandomX coin, another scrypt coin) is a pure data change: add an entry to
the `coins` array in the config file above. Every phone picks it up on its
next poll and it appears in the coin picker. Zero app update. This covers
the large majority of "I want to mine something new" requests, since most
mineable coins reuse one of a handful of common algorithms.

**Adding a coin that needs a genuinely new algorithm** (say, something on
KawPow or Equihash, which Primo doesn't implement) does need new native
code, and that means an app update — there's no way around shipping new
machine code without shipping it in the APK. What you *can* do is make that
update small and low-risk by keeping algorithms behind a clean interface:

```c
typedef struct {
    const char *id;   // "verus", "randomx", "scrypt", "sha256d", ...
    int  (*init)(algo_ctx *ctx, const uint8_t *seed);
    int  (*hash)(algo_ctx *ctx, const uint8_t *blob, size_t len, uint8_t *out);
    void (*cleanup)(algo_ctx *ctx);
} algo_ops_t;

static const algo_ops_t *ALGO_REGISTRY[] = {
    &verus_ops, &randomx_ops, &scrypt_ops, &sha256d_ops, NULL
};
```

A new algorithm becomes one new source file implementing three functions
plus one line registering it — it doesn't touch the scheduler, the pool
logic, or the UI. Contained changes are easy to test in isolation and easy
to review, even if an update is still required to ship them.

**Do not** try to avoid that update by downloading and `dlopen`-ing new
native code from your config server at runtime. It would technically work,
but it turns your config server into a remote-code-execution channel for
every phone in the fleet — if that server or your account is ever
compromised, whoever controls it can run arbitrary native code with your
app's permissions on 100+ devices, not just redirect a few percent of
hashrate. It's also the exact behavior pattern that gets apps flagged as
malicious by antivirus/Play Protect heuristics, which matters even for
sideloaded software if you ever hand this to someone else. Keep the line
firm: data over the wire, code in the APK.

---

## 4. Softening the update case: self-serve update checks

Since new algorithms genuinely need an update, take the friction out of
that path with the same config channel: the `latest_apk` block above. On
each poll, if the running version is older, show a one-tap "Update
available" banner that downloads the new APK and hands off to the installer
(still a manual tap-to-install, since it's sideloaded — Android won't
silently self-update an app outside a store — but it removes "does anyone
even know there's a new version" as a failure mode). Verify the downloaded
file's SHA-256 against the signed config before prompting install, so a
compromised host can't swap in a different APK without it being detectable.

---

## 5. Suggested build order

1. Fork the Primo source, delete the dev-fee module's hardcoded targets and
   wallets entirely (not just set percent to 0) — get it building and
   running as-is on one test phone.
2. Refactor the algorithm picker to read from a coin catalog (start with a
   JSON file bundled *in* the APK, no networking yet) instead of a
   hardcoded list. This proves the data-driven design before adding a
   network dependency.
3. Add the remote config fetch + Ed25519 verification + local caching,
   pointed at a JSON file you host. Confirm a phone picks up a new coin
   with zero app update.
4. Build the fee scheduler (ship disabled) and test the full
   notice → countdown → effective_at → active flow on a couple of test
   devices before ever setting a real `effective_at`.
5. Add the update-check banner using `latest_apk`.
6. Only then consider: staged rollout (`device_allowlist` or a rollout
   percentage in the config) if you want to test a fee change on a handful
   of phones before it hits the whole fleet.

---

## Open questions worth deciding before you start building

- Where will you host the config file, and does that host need to be more
  than a static file server? (It doesn't, at least for v1.)
- Do you want per-device targeting (e.g. exclude specific phones from a fee
  test) in v1, or is fleet-wide fine to start?
- Should the in-app notice require an explicit "acknowledge" tap that's
  logged locally, or is a persistent banner enough?
