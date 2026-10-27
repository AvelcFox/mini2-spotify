#!/bin/sh
# Build script for libpulse.so.0 shim (ARM 32-bit armhf)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$(dirname "$SCRIPT_DIR")/lib"
mkdir -p "$OUT_DIR"

CC="${CROSS_COMPILE:-arm-linux-gnueabihf-}gcc"
if ! command -v "$CC" >/dev/null 2>&1; then
    # Fallback to cached toolchain if available
    TOOLCHAIN="$HOME/.cache/spotiya/armv7-eabihf--glibc--stable-2020.02-2/bin/arm-linux-gcc"
    if [ -x "$TOOLCHAIN" ]; then
        CC="$TOOLCHAIN"
    else
        echo "❌ Cross compiler $CC not found. Please install gcc-arm-linux-gnueabihf"
        exit 1
    fi
fi

echo "🔨 Compiling libpulse.so.0 with $CC..."
"$CC" -shared -fPIC -O2 -pthread \
    -Wall -Wextra -Wno-unused-parameter \
    -o "$OUT_DIR/libpulse.so.0" \
    "$SCRIPT_DIR/libpulse.c" -lm

echo "✅ Success: $OUT_DIR/libpulse.so.0 built successfully!"
