#!/data/data/com.termux/files/usr/bin/bash
#
# Build a SELF-CONTAINED Android miner binary for the APK and stage it (+ libc++)
# into jniLibs. The stock build_termux.sh binary dynamically links Termux's
# libcurl/libjansson/libc++ — none present in the app sandbox. This relinks the
# miner against STATIC curl + jansson (build_static_deps_termux.sh) so the only
# remaining dynamic deps are bionic system libs + libc++, which we bundle.
#
# Run from anywhere; resolves the repo root itself. Verified on a rooted S10+:
# resulting binary mines in-app at ~30-39 MH/s sha256d.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
SDEPS="${SDEPS:-$HOME/primo-sdeps}"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
JNI="$ROOT/android/app/src/main/jniLibs/arm64-v8a"

# 1. static curl + jansson
bash "$ROOT/android/build_static_deps_termux.sh"

# 2. sysroot-aware clang-16 wrappers (minted by build_termux.sh on first run)
W="$ROOT/.clang16-wrappers"
if [ ! -x "$W/clang++" ]; then
  echo "==> minting clang-16 wrappers via build_termux.sh"
  CLANG_PREFIX="${CLANG_PREFIX:-$HOME/clang-16}" bash "$ROOT/build_termux.sh"
fi

# RandomX (Monero) — build the vendored static lib on-device unless opted out.
# Needs `pkg install cmake`. PRIMO_RANDOMX=0 skips it and drops the algorithm.
PRIMO_RANDOMX="${PRIMO_RANDOMX:-1}"
RANDOMX_LIB="$ROOT/third_party/RandomX/build/librandomx.a"
RANDOMX_MAKEVARS=""
RANDOMX_LINK_LIB=""
if [ "$PRIMO_RANDOMX" != "0" ]; then
  command -v cmake >/dev/null 2>&1 || { echo "ERROR: cmake needed for RandomX (pkg install cmake), or run with PRIMO_RANDOMX=0" >&2; exit 1; }
  echo "==> building vendored RandomX static lib (its own cmake, our clang-16)"
  # Relative target: the Makefile rule is the relative path, so the absolute
  # $RANDOMX_LIB is "No rule to make target" to make.
  make -C "$ROOT" third_party/RandomX/build/librandomx.a CC="$W/clang" CXX="$W/clang++"
  [ -f "$RANDOMX_LIB" ] || { echo "ERROR: librandomx.a not produced" >&2; exit 1; }
  RANDOMX_LINK_LIB="$RANDOMX_LIB"
else
  # Keep the miner objects free of RandomX symbols so the relink stays clean.
  RANDOMX_MAKEVARS="PRIMO_RANDOMX=0"
fi

# 3. compile against the static-dep headers (the make link step is a throwaway —
#    it links dynamically and may fail; we relink static next). A nonzero make
#    is tolerated ONLY for the link step: verify below that every source
#    actually produced its object, so a real compile error fails loudly here
#    instead of surfacing as a confusing relink failure.
echo "==> compiling miner objects"
make clean >/dev/null 2>&1 || true
make -j"$(nproc)" $RANDOMX_MAKEVARS \
  CC="$W/clang" CXX="$W/clang++" PRIMO_LINKER=lld \
  PRIMO_A53_ERRATA=0 \
  PRIMO_EXTRA_CFLAGS="-I$SDEPS/include -DCURL_STATICLIB" \
  PRIMO_EXTRA_CXXFLAGS="-I$SDEPS/include -DCURL_STATICLIB" \
  || echo "==> make exited nonzero (OK if only the throwaway link failed) — verifying objects"
missing=0
# NOTE: third_party/yespower/ is NOT globbed wildcard-style on purpose.
# yespower-ref.c is the old (now-unused) implementation, kept only for
# reference/comparison and no longer in the Makefile's SOURCES_C, so it
# never produces a .o -- a wildcard here would wrongly flag that as a
# compile failure. yespower-platform.c is `#include`d directly by
# yespower-opt.c (not its own translation unit) and never produces a .o
# either. Only list files the Makefile actually compiles.
for s in src/*.cpp src/utils/*.cpp src/algorithm/*.c src/algorithm/*.S third_party/yespower/yespower-opt.c third_party/yespower/sha256.c; do
  [ -e "$s" ] || continue
  # stratum_xmr / randomx_algo aren't compiled when PRIMO_RANDOMX=0.
  if [ "$PRIMO_RANDOMX" = "0" ]; then
    case "$s" in *stratum_xmr.cpp|*randomx_algo.cpp) continue;; esac
  fi
  o="${s%.*}.o"
  if [ ! -f "$o" ]; then
    echo "ERROR: compile failed — missing $o" >&2
    missing=1
  fi
done
[ "$missing" -eq 0 ] || exit 1

# 4. relink explicitly against the static archives. (The Makefile's
#    PRIMO_LDLIBS_OVERRIDE didn't survive being passed over SSH; an explicit
#    relink is unambiguous. Objects are LTO bitcode — lld links them fine, and
#    librandomx.a is an ordinary (non-LTO) archive like curl/jansson.)
echo "==> relinking static"
#    C++ runtime: RandomX needs real libc++ symbols (std::__ndk1 string etc).
#    The driver's implicit -lc++ resolves to /system/lib64/libc++.so (std::__1
#    only) → undefined symbols. Link Termux's libc++_shared.so explicitly and
#    suppress the implicit one; we bundle that exact lib next to the binary.
#    mbedTLS archives follow libcurl (link order matters for static archives:
#    curl pulls tls -> x509 -> crypto).
"$W/clang++" $(ls src/*.o src/utils/*.o src/algorithm/*.o third_party/yespower/*.o) \
  -flto -pthread -fuse-ld=lld -nostdlib++ \
  "$SDEPS/lib/libcurl.a" \
  "$SDEPS/lib/libmbedtls.a" "$SDEPS/lib/libmbedx509.a" "$SDEPS/lib/libmbedcrypto.a" \
  "$SDEPS/lib/libjansson.a" $RANDOMX_LINK_LIB \
  "$PREFIX/lib/libc++_shared.so" -lm \
  -o ghuleh-miner

echo "==> NEEDED (want only libm/libc++/libdl/libc):"
readelf -d ghuleh-miner | grep NEEDED

# 5. stage libprimo.so + libc++_shared.so (the NEEDED name — MinerService sets
#    LD_LIBRARY_PATH to this dir) into jniLibs
mkdir -p "$JNI"
rm -f "$JNI/libc++.so"   # pre-RandomX builds staged it under this name
cp ghuleh-miner "$JNI/libprimo.so"
cp "$PREFIX/lib/libc++_shared.so" "$JNI/libc++_shared.so"
echo "==> staged $JNI/{libprimo.so,libc++_shared.so}"
echo "==> now: cd android && bash build_apk_termux.sh"
