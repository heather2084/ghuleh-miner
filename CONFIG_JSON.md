# Config JSON Reference

This document describes the supported `config.json` format for `ghuleh-miner`.

For shorter copy-paste examples, see [`CONFIG_EXAMPLES.md`](CONFIG_EXAMPLES.md).

## Loading Rules

- If you run `./ghuleh-miner` and `./config.json` exists, it is loaded automatically.
- You can load a different file with `./ghuleh-miner -c /path/to/file.json`.
- Explicit CLI flags override values from the config file.
- Top-level settings act as defaults. Pool-local settings override those defaults where supported.

## Top-Level Keys

| Key | Type | Meaning |
| --- | --- | --- |
| `algo` | string | Mining algorithm: `verus`, `sha256d`, `scrypt`, or `randomx` (Monero and other rx/0 chains — extended-header forks like Zephyr work with the coin's own pool + wallet; aliases `rx`, `xmr`, `monero`). RandomX fast mode needs ~2.5 GiB free RAM; falls back to a ~5x-slower light mode when it doesn't fit. |
| `api-bind` | string or integer | API bind address. Accepts `IP:PORT`, `IP`, or just a port number. |
| `benchmark` | boolean | Run offline benchmark mode instead of pool mining. |
| `cpu-affinity` | string or integer | CPU affinity mask. Accepts `-1`/`all`, decimal masks, or hex masks such as `0xf0`. |
| `cpu-priority` | integer | Process priority level from `0` to `5`. |
| `debug` | boolean | Enable debug logging. |
| `nicehash` | boolean | RandomX only: the pool owns the nonce's top byte, so the miner scans just the low 24 bits. Auto-enabled when the pool advertises the `nicehash` extension at login (NiceHash, xmrig-proxy, mining proxies generally), so set this only if your pool assigns nonce slices without advertising them. |
| `pass` | string | Worker password. Also used as the default pool password when `pools[]` entries omit `pass`. |
| `protocol-dump` | boolean | Log raw stratum protocol messages. |
| `quiet` | boolean | Reduce normal log output. |
| `retries` | integer | Retry limit. `-1` means retry forever. |
| `retry-pause` | integer | Seconds to wait before reconnect attempts. |
| `statsavg` | integer | Stats averaging window in seconds. |
| `threads` | integer | Number of mining threads. |
| `timeout` | integer | Global network timeout in seconds. Also used as the default timeout for pools that do not set their own `timeout`. |
| `url` | string | Single-pool stratum URL. |
| `user` | string | Wallet or username. Also used as the default pool user when `pools[]` entries omit `user`. |
| `userpass` | string | Alternative combined `user:pass` form. |
| `pools` | array | Multi-pool configuration array. |

## Pool Keys

Each entry in `pools` is an object. Supported keys:

| Key | Type | Meaning |
| --- | --- | --- |
| `name` | string | Friendly pool label used by the API and logs. |
| `url` | string | Pool stratum URL. Required for the pool entry to be usable. `stratum+tcp://` is plain TCP; `stratum+ssl://` (or `stratum+tcps://`) enables TLS. |
| `user` | string | Pool-specific username or wallet. Overrides top-level `user`. |
| `pass` | string | Pool-specific password. Overrides top-level `pass`. |
| `timeout` | integer | Pool-specific network timeout in seconds. Overrides top-level `timeout`; `0` (or omitting the key) means "use the global timeout". |
| `disabled` | boolean or integer | If truthy, the pool is skipped for startup and failover. |

## Behavior Notes

- If both `url` and `pools` are present, the `pools` array is used as the configured pool set.
- Startup selects the first configured pool that is not disabled.
- Failover skips disabled pools.
- If a pool entry omits `user` or `pass`, the top-level `user` and `pass` are inherited.
- If a pool entry sets `user` or `pass` to `""`, that explicit empty string suppresses top-level inheritance for that field.
- Pool entries without a valid `url` are ignored.
- `-u`, `-p`, `-O`, and `-T` remain valid CLI-wide overrides when `pools[]` is used.
- `-o` / `--url` overrides a configured `pools[]` array by switching that invocation into single-pool mode.
- On this project’s common 8-core big.LITTLE layout, CPUs `0-3` are typically LITTLE and `4-7` are big, so:
  - `cpu-affinity: -1` or `all` means all CPUs
  - `cpu-affinity: 15` or `0x0f` means CPUs `0-3`
  - `cpu-affinity: 240` or `0xf0` means CPUs `4-7`
- If you restrict affinity to 4 CPUs, also reduce `threads` to `4` to avoid oversubscribing those cores.

## Single-Pool Example

```json
{
  "algo": "verus",
  "url": "stratum+tcp://pool.verus.io:9998",
  "user": "RWallet123.worker",
  "pass": "x",
  "threads": 8,
  "api-bind": "127.0.0.1:4068",
  "retry-pause": 10
}
```

## Multi-Pool Example

```json
{
  "algo": "verus",
  "user": "RWallet123.worker",
  "pass": "x",
  "threads": 8,
  "api-bind": "127.0.0.1:4068",
  "timeout": 180,
  "pools": [
    {
      "name": "Primary",
      "url": "stratum+tcp://pool.verus.io:9998",
      "disabled": 0
    },
    {
      "name": "Backup",
      "url": "stratum+tcp://usse.vipor.net:5040",
      "timeout": 120,
      "disabled": 0
    }
  ]
}
```

## CLI-Only Options

These are meaningful on the command line but should not be placed in `config.json`:

- `config`
- `help`
- `version`
