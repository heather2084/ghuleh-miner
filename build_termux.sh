#!/usr/bin/env bash
# Build ghuleh-miner natively in Termux on Android.
# Run from the repo root: bash build_termux.sh
set -euo pipefail

BINARY="ghuleh-miner"

die()  { echo "Error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

# ── Sanity checks ─────────────────────────────────────────────────────────────

[ -f Makefile ] && [ -f src/miner.cpp ] \
    || die "Run this script from the ghuleh-miner repo root"

# ── Dependencies ──────────────────────────────────────────────────────────────

info "Installing build dependencies..."
# Index refresh is best-effort — a stale mirror won't block the build if the
# packages are already installed.  --no-upgrade prevents apt trying to fetch a
# newer version that the mirror may not carry yet.
pkg update -y || info "pkg update failed (stale mirror?) — continuing with cached index"
pkg install -y --no-upgrade clang lld make curl libjansson openssl

# ── Resolve compiler ──────────────────────────────────────────────────────────
#
# If CLANG_PREFIX is set (e.g. from build_clang16_termux.sh), use that clang.
# Otherwise fall back to whatever 'clang' is on PATH (Termux default).
#
# Auto-prefer the from-source clang-16 when present: it is the project's
# canonical compiler and is ~5% faster on Verus than Termux's default clang
# (measured 4.86 vs 4.61 MH/s heat-soaked on Exynos 9820, clang-16 vs clang-21).
# A bare `bash build_termux.sh` must NOT silently produce a slow binary.
# To force the PATH clang instead, run with PRIMO_FORCE_PATH_CLANG=1.
if [ -z "${CLANG_PREFIX:-}" ] && [ -z "${PRIMO_FORCE_PATH_CLANG:-}" ] \
   && [ -x "$HOME/clang-16/bin/clang" ]; then
    CLANG_PREFIX="$HOME/clang-16"
    info "Auto-selected from-source clang-16 at $CLANG_PREFIX"
    info "(set PRIMO_FORCE_PATH_CLANG=1 to use the slower default PATH clang)"
fi

if [ -n "${CLANG_PREFIX:-}" ]; then
    CLANG16_C="$CLANG_PREFIX/bin/clang"
    CLANG16_CXX="$CLANG_PREFIX/bin/clang++"
    [ -x "$CLANG16_C" ] || die "CLANG_PREFIX set but $CLANG16_C not found"
    info "Using CLANG_PREFIX: $CLANG_PREFIX (clang-16 compile + link, LTO enabled)"

    # clang-16 was built for aarch64-unknown-linux-gnu, but with --target and
    # --sysroot it can drive a full Android/Bionic link: it finds Android CRT
    # objects, shared libraries, and its own lld-16 links the LTO IR it emitted.
    # This enables full -flto end-to-end (clang-16 IR → lld-16 — version match).

    # Ensure ld.lld symlink exists (ninja install-lld installs 'lld'; the ELF
    # driver is 'ld.lld' which clang looks for when resolving -fuse-ld= paths).
    if [ ! -x "$CLANG_PREFIX/bin/ld.lld" ]; then
        ln -sf "$CLANG_PREFIX/bin/lld" "$CLANG_PREFIX/bin/ld.lld"
        info "Created $CLANG_PREFIX/bin/ld.lld symlink"
    fi

    TERMUX_USR="${PREFIX:-/data/data/com.termux/files/usr}"
    ARCH_INC="$TERMUX_USR/include/aarch64-linux-android"
    WRAPPER_DIR="$(pwd)/.clang16-wrappers"
    mkdir -p "$WRAPPER_DIR"

    # Symlink Termux's compiler-rt Android builtins into clang-16's resource
    # tree.  This lets clang-16's linker driver find
    # libclang_rt.builtins-aarch64-android.a without replacing clang-16's own
    # compiler headers (which would break compilation).
    CLANG16_RSRC=$("$CLANG16_C" -print-resource-dir 2>/dev/null || true)
    TERMUX_RSRC=$("$TERMUX_USR/bin/clang" -print-resource-dir 2>/dev/null || true)
    if [ -n "$CLANG16_RSRC" ] && [ -n "$TERMUX_RSRC" ] && [ -d "$TERMUX_RSRC/lib/linux" ]; then
        mkdir -p "$CLANG16_RSRC/lib"
        ln -sfn "$TERMUX_RSRC/lib/linux" "$CLANG16_RSRC/lib/linux"
        info "Linked Termux compiler-rt -> $CLANG16_RSRC/lib/linux"
    fi

    COMPILE_FLAGS="--target=aarch64-linux-android33 --sysroot=$TERMUX_USR"
    [ -d "$ARCH_INC" ] && COMPILE_FLAGS="$COMPILE_FLAGS -isystem $ARCH_INC"
    # /system/lib64 is needed for Android bionic stubs (libm, libc, libdl) that
    # clang-16 adds implicitly for --target=aarch64-linux-android33.  These live
    # in the Android system image, not in the Termux prefix.  No rpath needed
    # for /system/lib64 — Android's dynamic linker always searches it.
    LINK_EXTRA="-L$TERMUX_USR/lib -L/system/lib64 -Wl,-rpath,$TERMUX_USR/lib"
    # libc++ 21.1.8+ headers externalize std::__ndk1::__thread_local_data();
    # Termux ships libc++_shared.so (no libc++.so alias), and vanilla
    # clang-16's android driver asks for -lc++ — link it explicitly.
    # Harmless on older header sets. (Found in the termux-docker build env;
    # the phone hits the same wall after its next libc++ pkg upgrade.)
    #
    # -nostdlib++ is load-bearing, NOT cosmetic: without it the driver's
    # implicit -lc++ resolves against the ANDROID PLATFORM /system/lib64/libc++.so,
    # which (a) puts a second, ABI-different C++ runtime (std::__1 vs Termux's
    # std::__ndk1) in NEEDED, and (b) satisfies _Unwind_Resume from that DSO, so
    # lld never pulls Termux's static libunwind.a. On devices whose platform
    # libc++.so does not export the unwinder the binary then dies at exec with
    #   CANNOT LINK EXECUTABLE: cannot locate symbol "_Unwind_Resume"
    # (field report, CupofX, 2026-07-28 — affected every release since 1.0.7).
    # Dropping the implicit -lc++ makes libunwind.a link in statically and
    # leaves libc++_shared.so as the only C++ runtime. Verify after any change
    # to this link line:
    #   readelf -d ghuleh-miner | grep NEEDED        # must NOT list libc++.so
    #   readelf --dyn-syms -W ghuleh-miner | grep _Unwind   # must be empty
    [ -f "$TERMUX_USR/lib/libc++_shared.so" ] && \
        LINK_EXTRA="$LINK_EXTRA -nostdlib++ -lc++_shared"

    # Single wrapper: clang-16 for both compile and link.
    # Keep -L/-Wl flags out of compile-only steps to avoid "unused arg" noise.
    for _ext in clang clang++; do
        if [ "$_ext" = "clang" ]; then
            _c16="$CLANG16_C"
            _flags="$COMPILE_FLAGS"
        else
            _c16="$CLANG16_CXX"
            # Standalone clang-16 ships no libc++ headers and won't find
            # Termux's under this sysroot layout. The miner's own C++ never
            # includes libc++ headers so this went unnoticed until RandomX
            # (<cstring> etc). Termux libc++ (LLVM 21) warns "only supports
            # Clang 18+" but compiles fine with clang-16.
            _flags="$COMPILE_FLAGS"
            [ -d "$TERMUX_USR/include/c++/v1" ] && \
                _flags="$_flags -isystem $TERMUX_USR/include/c++/v1"
        fi
        cat > "$WRAPPER_DIR/$_ext" << EOF
#!/bin/sh
_link=1
for _a; do case "\$_a" in -c|-E|-S|-M|-MM) _link=0; break;; esac; done
if [ "\$_link" = "1" ]; then
  exec "$_c16" $_flags $LINK_EXTRA "\$@"
else
  exec "$_c16" $_flags "\$@"
fi
EOF
        chmod +x "$WRAPPER_DIR/$_ext"
    done

    CLANG_BIN="$WRAPPER_DIR/clang"
    CLANGXX_BIN="$WRAPPER_DIR/clang++"
    LLD_BIN="$CLANG_PREFIX/bin/ld.lld"   # → PRIMO_LINKER → -fuse-ld= in Makefile
else
    CLANG_BIN=clang
    CLANGXX_BIN=clang++
    LLD_BIN=lld
fi

CLANG_MAJOR=$("$CLANG_BIN" --version 2>/dev/null \
    | grep -oE 'clang version [0-9]+' \
    | grep -oE '[0-9]+$' \
    || echo 0)
info "Detected clang $CLANG_MAJOR"

# ── Android/Termux build knobs (no Makefile patching) ────────────────────────
#
# The Makefile is parameterised, so the Android adjustments are passed as make
# variables instead of sed-rewriting the repo Makefile (which used to dirty the
# worktree and leak profiles between syncs). The repo Makefile stays untouched.
#
#   PRIMO_A53_ERRATA=0  drops -mfix-cortex-a53-835769 — phone CPUs never had the
#                       A53 erratum, so skip the NOP overhead. The default
#                       rk3588 PROFILE is kept on purpose (NOT generic): its A76
#                       asm helpers measurably help even on the Mongoose M4.
#   PRIMO_EXTRA_C{,XX}FLAGS  append -Ofast (over -O3) and -falign-functions=32
#                       (over 16). Both are washes vs -O3/16 on Android in A/B
#                       tests, but kept so this build is byte-equivalent to the
#                       previous sed-patched one. align=64 wastes I-cache; 32
#                       suits the -fno-unroll-loops haraka/clhash loops.
#
# Do NOT bump -march to armv8.2-a: v8.2 implies v8.1 LSE, so clang hardcodes
# ldadd/cas/swp atomics that SIGILL on ARMv8.0 cores the moment mining threads
# start (field-confirmed 2026-06-11). v8.0 emits outline atomics that
# runtime-dispatch to LSE where present; the hot path (PMULL/AES/SHA2) is fully
# covered by +crypto. CC/CXX/PRIMO_LINKER use the Makefile's $(origin) guards.

[ "$CLANG_MAJOR" -ge 13 ] || die "clang 13+ required (need -ffinite-loops); detected clang $CLANG_MAJOR"

TERMUX_EXTRA_OPT="-Ofast -falign-functions=32"

# ── Build ─────────────────────────────────────────────────────────────────────

JOBS=$(nproc 2>/dev/null || echo 4)
info "Building with $JOBS parallel jobs (Android make-variable overrides, repo Makefile untouched)..."

make clean
make -j"$JOBS" \
    CC="$CLANG_BIN" CXX="$CLANGXX_BIN" PRIMO_LINKER="$LLD_BIN" \
    PRIMO_A53_ERRATA=0 \
    PRIMO_EXTRA_CFLAGS="$TERMUX_EXTRA_OPT" \
    PRIMO_EXTRA_CXXFLAGS="$TERMUX_EXTRA_OPT"

# ── Done ──────────────────────────────────────────────────────────────────────

SIZE=$(du -h "$BINARY" 2>/dev/null | cut -f1 || echo "?")
echo ""
info "Build complete: ./$BINARY  ($SIZE)"
echo ""
echo "Example commands:"
echo "  Verus:   ./$BINARY -a verus   -o stratum+tcp://pool.verus.io:9998 -u WALLET.worker -t $JOBS"
echo "  SHA256d: ./$BINARY -a sha256d -o stratum+tcp://POOL:PORT -u USER.worker -t $JOBS"
echo "  Scrypt:  ./$BINARY -a scrypt  -o stratum+tcp://POOL:PORT -u USER.worker -t $JOBS"
echo ""
echo "Note: -mtune=cortex-a53 is kept — empirically fastest on heterogeneous SoCs"
echo "including Exynos 9820 (Mongoose M4 has no clang model; a53 fits PMULL/AES"
echo "latency chains better than a76 on both RK3588 and Exynos)."
echo ""
echo "To use clang-16 (recommended — empirically faster on ARM):"
echo "  CLANG_PREFIX=\$HOME/clang-16 bash build_termux.sh"
echo "Build clang-16 from source: bash build_clang16_termux.sh  (~2-4 hours)"
