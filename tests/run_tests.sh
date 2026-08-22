#!/usr/bin/env bash
# Repo test harness for ghuleh-miner: `make test` runs this.
#
# T1  sha256d init self-tests gate mining ("abc" vectors + dual-asm cross-check)
# T2  scrypt init self-test (single vs dual vs SoA-4 cross-validation)
# T3  verus x2/fused/asm runtime cross-check (VERUS_X2_SELFTEST) under load
# T4  end-to-end share round-trip against a local mock stratum pool (sha256d):
#     subscribe -> authorize -> notify -> real mining -> submit validated by
#     the mock for shape -> "Accepted" seen in the miner log
# T5  same round-trip over stratum+ssl (TLS via libcurl); SKIPPED when the
#     system libcurl has no TLS or `openssl` is unavailable for the test cert
# T6  RandomX nicehash nonce contract: the mock XMR pool reserves the nonce
#     MSB and advertises the "nicehash" extension; every submitted nonce must
#     still carry the reserved byte (light mode, so no 2 GiB dataset)
# T7  same pool WITHOUT the extension: the miner must scan the full 32-bit
#     space (no mode line) and still land an accepted share
# T6/T7 are SKIPPED on PRIMO_RANDOMX=0 builds.
#
# Everything runs in a scratch dir so the repo's config.json is never picked
# up. Total runtime ~45 s on an RK3588. Exit 0 = all pass (skips allowed).
set -u

BIN=$(readlink -f "${1:-./ghuleh-miner}")
REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
PASS=0; FAIL=0; SKIP=0
MINER_PID=""; MOCK_PID=""

cleanup() {
    [ -n "$MINER_PID" ] && kill -9 "$MINER_PID" 2>/dev/null
    [ -n "$MOCK_PID" ] && kill -9 "$MOCK_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT
cd "$WORK"

result() { # result <ok|fail|skip> <name> [detail]
    case "$1" in
        ok)   PASS=$((PASS+1)); echo "PASS  $2";;
        skip) SKIP=$((SKIP+1)); echo "SKIP  $2 (${3:-})";;
        *)    FAIL=$((FAIL+1)); echo "FAIL  $2 (${3:-})";;
    esac
}

run_bench() { # run_bench <log> <seconds> <args...>
    local log=$1 secs=$2; shift 2
    "$BIN" "$@" > "$log" 2>&1 &
    MINER_PID=$!
    sleep "$secs"
    kill -TERM "$MINER_PID" 2>/dev/null
    wait "$MINER_PID" 2>/dev/null
    MINER_PID=""
}

# ---- T1: sha256d self-tests ------------------------------------------------
run_bench t1.log 6 --benchmark -a sha256d -t 2 -N 5
if grep -q "SHA256d self-test passed" t1.log; then
    result ok "sha256d init self-tests"
else
    result fail "sha256d init self-tests" "no 'self-test passed' in log"
fi

# ---- T2: scrypt self-test ---------------------------------------------------
run_bench t2.log 6 --benchmark -a scrypt -t 2 -N 5
if grep -q "Scrypt self-test passed" t2.log; then
    result ok "scrypt init self-test"
else
    result fail "scrypt init self-test" "no 'self-test passed' in log"
fi

# ---- T3: verus x2/fused/asm cross-check under load ---------------------------
VERUS_X2_SELFTEST=1 run_bench t3.log 8 --benchmark -a verus -t 2 -N 5
if grep -q "FAILED" t3.log; then
    result fail "verus x2 selftest" "variant mismatch (see t3.log)"
elif grep -qE "Hashrate: [1-9]" t3.log; then
    result ok "verus x2/fused/asm cross-check"
else
    result fail "verus x2 selftest" "no hashrate produced"
fi

# ---- mock-pool round trip helper ---------------------------------------------
# run_pool_test <name> <mock-extra-args> <miner-url-scheme>
run_pool_test() {
    local name=$1 tlspem=$2 scheme=$3
    local mock_log=$name.mock.log miner_log=$name.miner.log
    local mock_args=(--once)
    [ -n "$tlspem" ] && mock_args+=(--tls "$tlspem")

    python3 "$REPO/tests/mock_pool.py" "${mock_args[@]}" > "$mock_log" 2>&1 &
    MOCK_PID=$!
    local port="" i
    for i in $(seq 1 20); do
        port=$(sed -n 's/^MOCK_READY //p' "$mock_log")
        [ -n "$port" ] && break
        sleep 0.2
    done
    if [ -z "$port" ]; then
        result fail "$name" "mock pool did not start"
        kill -9 "$MOCK_PID" 2>/dev/null; MOCK_PID=""
        return
    fi

    "$BIN" -a sha256d -o "$scheme://127.0.0.1:$port" -u test.worker -p x -t 2 \
        > "$miner_log" 2>&1 &
    MINER_PID=$!

    local ok=""
    for i in $(seq 1 60); do
        grep -q "^SUBMIT_OK" "$mock_log" && grep -q "Accepted" "$miner_log" \
            && { ok=1; break; }
        grep -q "^SUBMIT_BAD" "$mock_log" && break
        kill -0 "$MINER_PID" 2>/dev/null || break
        sleep 0.5
    done
    kill -TERM "$MINER_PID" 2>/dev/null; wait "$MINER_PID" 2>/dev/null; MINER_PID=""
    kill -9 "$MOCK_PID" 2>/dev/null; wait "$MOCK_PID" 2>/dev/null; MOCK_PID=""

    if [ -n "$ok" ]; then
        result ok "$name"
    elif grep -q "^SUBMIT_BAD" "$mock_log"; then
        result fail "$name" "$(sed -n 's/^SUBMIT_BAD //p' "$mock_log" | head -1)"
    else
        result fail "$name" "no accepted share (see $WORK/$miner_log)"
    fi
}

# ---- T4: plain-TCP share round trip ------------------------------------------
run_pool_test "mock-pool share round-trip (tcp)" "" "stratum+tcp"

# ---- T5: TLS share round trip -------------------------------------------------
if ! "$BIN" --version >/dev/null 2>&1; then
    result skip "mock-pool share round-trip (ssl)" "binary not runnable"
elif ! command -v openssl >/dev/null 2>&1; then
    result skip "mock-pool share round-trip (ssl)" "no openssl for test cert"
else
    # Probe whether this build/libcurl can do TLS at all: a stratum+ssl URL
    # against a closed port must fail with "connect", not "unsupported".
    openssl req -x509 -newkey rsa:2048 -nodes -batch -days 2 \
        -keyout cert.pem -out cert.pem.crt -subj "/CN=127.0.0.1" >/dev/null 2>&1
    cat cert.pem.crt >> cert.pem
    if "$BIN" -a sha256d -o stratum+ssl://127.0.0.1:1 -u x -p x -t 1 -r 0 \
        2>&1 | grep -qi "TLS.*not supported\|Unsupported protocol"; then
        result skip "mock-pool share round-trip (ssl)" "libcurl built without TLS"
    else
        run_pool_test "mock-pool share round-trip (ssl)" "cert.pem" "stratum+ssl"
    fi
fi

# ---- mock XMR pool round-trip helper -----------------------------------------
# run_xmr_pool_test <name> <expect-nicehash 0|1> [extra mock args...]
run_xmr_pool_test() {
    local name=$1 expect_nicehash=$2; shift 2
    local mock_log=$name.mock.log miner_log=$name.miner.log

    python3 "$REPO/tests/mock_xmr_pool.py" --once --slice a7 "$@" > "$mock_log" 2>&1 &
    MOCK_PID=$!
    local port="" i
    for i in $(seq 1 20); do
        port=$(sed -n 's/^MOCK_READY //p' "$mock_log")
        [ -n "$port" ] && break
        sleep 0.2
    done
    if [ -z "$port" ]; then
        result fail "$name" "mock pool did not start"
        kill -9 "$MOCK_PID" 2>/dev/null; MOCK_PID=""
        return
    fi

    # Light mode keeps the test to a 256 MiB cache instead of a 2.1 GiB dataset.
    PRIMO_RANDOMX_LIGHT=1 "$BIN" -a randomx -o "stratum+tcp://127.0.0.1:$port" \
        -u test.worker -p x -t 2 > "$miner_log" 2>&1 &
    MINER_PID=$!

    local ok="" unbuilt=""
    for i in $(seq 1 90); do
        grep -q "randomx support was not built in" "$miner_log" && { unbuilt=1; break; }
        grep -q "^SUBMIT_OK" "$mock_log" && grep -q "Accepted" "$miner_log" \
            && { ok=1; break; }
        grep -q "^SUBMIT_BAD" "$mock_log" && break
        kill -0 "$MINER_PID" 2>/dev/null || break
        sleep 0.5
    done
    kill -TERM "$MINER_PID" 2>/dev/null; wait "$MINER_PID" 2>/dev/null; MINER_PID=""
    kill -9 "$MOCK_PID" 2>/dev/null; wait "$MOCK_PID" 2>/dev/null; MOCK_PID=""

    local mode_seen=0
    grep -q "nicehash nonce mode" "$miner_log" && mode_seen=1

    if [ -n "$unbuilt" ]; then
        result skip "$name" "PRIMO_RANDOMX=0 build"
    elif grep -q "^SUBMIT_BAD" "$mock_log"; then
        result fail "$name" "$(sed -n 's/^SUBMIT_BAD //p' "$mock_log" | head -1)"
    elif [ -z "$ok" ]; then
        result fail "$name" "no accepted share (see $WORK/$miner_log)"
    elif [ "$mode_seen" != "$expect_nicehash" ]; then
        result fail "$name" "nicehash mode=$mode_seen, expected $expect_nicehash"
    else
        result ok "$name"
    fi
}

# ---- T6: RandomX nicehash nonce contract --------------------------------------
run_xmr_pool_test "randomx nicehash nonce slice" 1 --nicehash

# ---- T7: RandomX without the extension (full 32-bit space) --------------------
run_xmr_pool_test "randomx full nonce space (no extension)" 0

echo "----------------------------------------"
echo "tests: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
