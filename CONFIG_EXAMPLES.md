# Config Examples

This file is the short version of the config docs. For the full option reference, see [`CONFIG_JSON.md`](CONFIG_JSON.md).

## 1. Minimal Single-Pool Config

Use this when you want `./ghuleh-miner` to start from the local `config.json` with no extra flags.

```json
{
  "algo": "verus",
  "url": "stratum+tcp://pool.verus.io:9998",
  "user": "YOUR_WALLET.worker",
  "pass": "x",
  "threads": 8
}
```

Run it with:

```bash
./ghuleh-miner
```

## 2. Single-Pool Config With API Bind

Use this if you want the read-only API on a specific address and port.

```json
{
  "algo": "verus",
  "url": "stratum+tcp://pool.verus.io:9998",
  "user": "YOUR_WALLET.worker",
  "pass": "x",
  "threads": 8,
  "api-bind": "127.0.0.1:4068",
  "retry-pause": 10,
  "cpu-priority": 1
}
```

## 3. Multi-Pool Failover Config

Use this when you want one primary pool and one or more backups.

```json
{
  "algo": "verus",
  "user": "YOUR_WALLET.worker",
  "pass": "x",
  "threads": 8,
  "timeout": 180,
  "api-bind": "127.0.0.1:4068",
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

Notes:

- Top-level `user` and `pass` are inherited by pool entries that do not set their own credentials.
- `disabled: 1` removes a pool from startup and failover selection.
- A pool-local `timeout` overrides the top-level `timeout` for that pool only.

## 4. Override A Config Value From CLI

The config file provides defaults, and explicit CLI flags win.

Example:

```bash
./ghuleh-miner -t 4
```

That keeps the rest of `config.json` but overrides `threads` to `4` for that launch.

## 5. Affinity Examples For 8-Core big.LITTLE Boards

If your board exposes CPUs `0-3` as LITTLE cores and `4-7` as big cores:

```json
{
  "threads": 8,
  "cpu-affinity": -1
}
```

Use all CPUs.

```json
{
  "threads": 4,
  "cpu-affinity": 240
}
```

Use only CPUs `4-7` (`240` decimal = `0xf0` hex).

```json
{
  "threads": 4,
  "cpu-affinity": 15
}
```

Use only CPUs `0-3` (`15` decimal = `0x0f` hex).

If you keep `threads: 8` but restrict the mask to only 4 CPUs, the miner will place multiple threads on the same cores and performance usually drops.
