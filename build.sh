#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
ROOT_BINARY="$SCRIPT_DIR/ghuleh-miner"
CMAKE_BIN="${CMAKE:-cmake}"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

cmake_args=(
    -S "$SCRIPT_DIR"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE=Release
)

if [[ -n "${CC:-}" ]]; then
    cmake_args+=("-DCMAKE_C_COMPILER=$CC")
fi

if [[ -n "${CXX:-}" ]]; then
    cmake_args+=("-DCMAKE_CXX_COMPILER=$CXX")
fi

if [[ "${PRIMO_LINKER+x}" == "x" ]]; then
    cmake_args+=("-DPRIMO_LINKER=$PRIMO_LINKER")
fi

"$CMAKE_BIN" "${cmake_args[@]}"

"$CMAKE_BIN" --build "$BUILD_DIR" -j"$(nproc)"

install -m 755 "$BUILD_DIR/ghuleh-miner" "$ROOT_BINARY"

echo "Build complete! Binary: $ROOT_BINARY"
echo "Size: $(ls -lh "$ROOT_BINARY" | awk '{print $5}')"
