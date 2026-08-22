#!/usr/bin/env bash
# Build LLVM/Clang 16 from source in Termux.
# Run from anywhere: bash build_clang16_termux.sh
#
# Requirements:
#   ~15 GB free disk space (source + build dir + install)
#   ~8 GB RAM (2 parallel link jobs)
#   2-4 hours build time
#
# Result: clang-16 installed to ~/clang-16/
# Use with build_termux.sh via: CLANG_PREFIX=$HOME/clang-16 bash build_termux.sh
set -euo pipefail

LLVM_VERSION="16.0.6"
LLVM_TAG="llvmorg-${LLVM_VERSION}"
PREFIX="$HOME/clang-16"
BUILD_DIR="$HOME/llvm-build"

die()  { echo "Error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

# ── Sanity checks ─────────────────────────────────────────────────────────────

[ -f Makefile ] || true  # Can run from anywhere

# Check free space (need ~15 GB)
FREE_KB=$(df "$HOME" | awk 'NR==2 {print $4}')
FREE_GB=$((FREE_KB / 1024 / 1024))
if [ "$FREE_GB" -lt 14 ]; then
    die "Need at least 14 GB free in \$HOME, have ~${FREE_GB} GB. Free up space first."
fi
info "Free space: ~${FREE_GB} GB — OK"

# ── Dependencies ──────────────────────────────────────────────────────────────

info "Installing build dependencies..."
pkg update -y
pkg install -y cmake ninja python git wget xz-utils

# ── Download source ───────────────────────────────────────────────────────────

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

TARBALL="llvm-project-${LLVM_VERSION}.src.tar.xz"
TARBALL_URL="https://github.com/llvm/llvm-project/releases/download/${LLVM_TAG}/${TARBALL}"

if [ ! -f "$TARBALL" ]; then
    info "Downloading LLVM ${LLVM_VERSION} source (~100 MB)..."
    wget -O "$TARBALL" "$TARBALL_URL"
else
    info "Source tarball already present, skipping download"
fi

SOURCE_DIR="${BUILD_DIR}/llvm-project-${LLVM_VERSION}.src"
if [ ! -d "$SOURCE_DIR" ]; then
    info "Extracting source (~800 MB uncompressed)..."
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
        echo '#ifndef ANDROID_API_LEVEL_H' > "$ANDROID_HDR"
        echo '#define ANDROID_API_LEVEL_H'  >> "$ANDROID_HDR"
        echo '#define __ANDROID_API__ 33'   >> "$ANDROID_HDR"
        echo '#endif'                       >> "$ANDROID_HDR"
    fi
fi

info "Configuring..."
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

# ── Done ──────────────────────────────────────────────────────────────────────

echo ""
info "clang-16 installed to $PREFIX"
info "Verify: $PREFIX/bin/clang --version"
echo ""
echo "To build ghuleh-miner with clang-16:"
echo "  CLANG_PREFIX=$PREFIX bash ~/ghuleh-miner/build_termux.sh"
