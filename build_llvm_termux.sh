#!/usr/bin/env bash
# Build any LLVM/Clang version from source in Termux.
# Usage: bash build_llvm_termux.sh [VERSION]
#
# VERSION can be a major (e.g. "17") or full version (e.g. "17.0.6").
# Defaults to 17 if omitted.
#
# Known stable patch releases (used when only major is given):
#   16 → 16.0.6   17 → 17.0.6   18 → 18.1.8   19 → 19.1.7
#
# Requirements:
#   ~15 GB free disk space (source + build + install)
#   ~8 GB RAM (2 parallel link jobs)
#   2-4 hours build time
#
# Result: clang-NN installed to ~/clang-NN/
# Use with build_termux.sh via: CLANG_PREFIX=$HOME/clang-NN bash build_termux.sh
set -euo pipefail

die()  { echo "Error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

# ── Version resolution ────────────────────────────────────────────────────────

INPUT="${1:-17}"

# If only a major version given, map to latest known stable patch release
case "$INPUT" in
    16) LLVM_VERSION="16.0.6" ;;
    17) LLVM_VERSION="17.0.6" ;;
    18) LLVM_VERSION="18.1.8" ;;
    19) LLVM_VERSION="19.1.7" ;;
    *.*.*) LLVM_VERSION="$INPUT" ;;  # full version passed, use as-is
    *) die "Unknown major version '$INPUT'. Pass a full version like '17.0.6' or a known major (16-19)." ;;
esac

LLVM_MAJOR="${LLVM_VERSION%%.*}"
LLVM_TAG="llvmorg-${LLVM_VERSION}"
PREFIX="$HOME/clang-${LLVM_MAJOR}"
BUILD_DIR="$HOME/llvm-build-${LLVM_MAJOR}"

info "Building LLVM/Clang ${LLVM_VERSION} → install prefix: $PREFIX"

# ── Sanity checks ─────────────────────────────────────────────────────────────

# Check free space (need ~15 GB)
FREE_KB=$(df "$HOME" | awk 'NR==2 {print $4}')
FREE_GB=$((FREE_KB / 1024 / 1024))
if [ "$FREE_GB" -lt 14 ]; then
    die "Need at least 14 GB free in \$HOME, have ~${FREE_GB} GB. Free up space first."
fi
info "Free space: ~${FREE_GB} GB — OK"

# Warn if install prefix already exists (won't overwrite, just adds to it)
if [ -d "$PREFIX/bin" ]; then
    info "Note: $PREFIX already exists — ninja install will overwrite existing files"
fi

# ── Dependencies ──────────────────────────────────────────────────────────────

info "Installing build dependencies..."
pkg update -y || info "pkg update failed (stale mirror?) — continuing with cached index"
pkg install -y --no-upgrade cmake ninja python git wget xz-utils

# ── Download source ───────────────────────────────────────────────────────────

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

TARBALL="llvm-project-${LLVM_VERSION}.src.tar.xz"
TARBALL_URL="https://github.com/llvm/llvm-project/releases/download/${LLVM_TAG}/${TARBALL}"

if [ ! -f "$TARBALL" ]; then
    info "Downloading LLVM ${LLVM_VERSION} source (~100-130 MB)..."
    wget -O "$TARBALL" "$TARBALL_URL"
else
    info "Source tarball already present, skipping download"
fi

SOURCE_DIR="${BUILD_DIR}/llvm-project-${LLVM_VERSION}.src"
if [ ! -d "$SOURCE_DIR" ]; then
    info "Extracting source..."
    tar xf "$TARBALL"
fi

# ── Configure ─────────────────────────────────────────────────────────────────

CMAKE_BUILD_DIR="${BUILD_DIR}/build"
mkdir -p "$CMAKE_BUILD_DIR"
cd "$CMAKE_BUILD_DIR"

# Termux's cmake is patched to read android/api-level.h from
# CMAKE_INSTALL_PREFIX before it will configure any project.
# Seed the header so cmake can proceed with a native (non-NDK) build.
ANDROID_HDR="$PREFIX/include/android/api-level.h"
if [ ! -f "$ANDROID_HDR" ]; then
    info "Seeding android/api-level.h in install prefix for Termux cmake..."
    mkdir -p "$PREFIX/include/android"
    TERMUX_ANDROID_HDR="/data/data/com.termux/files/usr/include/android/api-level.h"
    if [ -f "$TERMUX_ANDROID_HDR" ]; then
        cp "$TERMUX_ANDROID_HDR" "$ANDROID_HDR"
    else
        # Minimal stub sufficient for cmake's check
        printf '#ifndef ANDROID_API_LEVEL_H\n#define ANDROID_API_LEVEL_H\n#define __ANDROID_API__ 33\n#endif\n' > "$ANDROID_HDR"
    fi
fi

info "Configuring LLVM ${LLVM_VERSION}..."
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_PROJECTS="clang;lld" \
    -DLLVM_TARGETS_TO_BUILD="AArch64" \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DCLANG_INCLUDE_DOCS=OFF \
    -DCLANG_INCLUDE_TESTS=OFF \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_PARALLEL_LINK_JOBS=2 \
    -DLLVM_DEFAULT_TARGET_TRIPLE="aarch64-linux-android33" \
    -DCLANG_DEFAULT_LINKER="lld" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    "${SOURCE_DIR}/llvm"

# ── Build ─────────────────────────────────────────────────────────────────────

JOBS=$(nproc 2>/dev/null || echo 4)
info "Building with $JOBS compile jobs, 2 link jobs..."
info "This will take 2-4 hours. Keep the screen alive (tmux recommended)."
echo ""

ninja -j"$JOBS" clang lld

# ── Install ───────────────────────────────────────────────────────────────────

info "Installing to $PREFIX..."
ninja install-clang install-clang-resource-headers install-lld

# Create ld.lld symlink if needed (build_termux.sh uses -fuse-ld=.../ld.lld)
if [ ! -x "$PREFIX/bin/ld.lld" ]; then
    ln -sf "$PREFIX/bin/lld" "$PREFIX/bin/ld.lld"
    info "Created $PREFIX/bin/ld.lld symlink"
fi

# ── Done ──────────────────────────────────────────────────────────────────────

echo ""
info "clang-${LLVM_MAJOR} (${LLVM_VERSION}) installed to $PREFIX"
info "Verify: $PREFIX/bin/clang --version"
echo ""
echo "To build ghuleh-miner with clang-${LLVM_MAJOR}:"
echo "  CLANG_PREFIX=$PREFIX bash ~/ghuleh-miner/build_termux.sh"
echo ""
echo "Version comparison:"
echo "  CLANG_PREFIX=\$HOME/clang-16 bash build_termux.sh   # clang-16 (LTO)"
echo "  CLANG_PREFIX=\$HOME/clang-17 bash build_termux.sh   # clang-17 (LTO)"
echo "  bash build_termux.sh                                 # Termux clang-21"
