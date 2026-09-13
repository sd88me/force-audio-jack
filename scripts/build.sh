#!/usr/bin/env bash
# =============================================================================
# Build the audio-injection tap (forceAudioIn.so, LD_PRELOAD'd into MPC) and
# its test-tone generator (injectTone), and drop them into addon/.
#
# This doesn't need ALSA headers or RtMidi - just libc/libpthread/librt
# symbol interposition - so a plain cross-compile with zig is enough, no
# Docker/QEMU armhf toolchain needed (contrast force-maze's/force-acid's own
# build.sh, which does need one for RtMidi+ALSA).
#
# Requires zig (https://ziglang.org, used purely as a cross-compiler - no
# Docker, no QEMU). Get it with:
#   curl -sL -o zig.tar.xz https://ziglang.org/download/<version>/zig-x86_64-linux-<version>.tar.xz
#   tar xf zig.tar.xz
# then point ZIG below at .../zig-x86_64-linux-<version>/zig, or put it on PATH.
#
# Output goes straight into addon/ - that folder is ready to copy onto a
# device's AddOns/ForceAudioIn as-is afterward (see README.md's "Deploy").
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

ZIG="${ZIG:-zig}"
TARGET=arm-linux-gnueabihf.2.39   # matches the Force's exact glibc (confirmed live)

if ! command -v "$ZIG" >/dev/null 2>&1; then
    echo "zig not found (set ZIG=/path/to/zig, or put it on PATH)." >&2
    exit 1
fi

echo "== forceAudioIn.so (LD_PRELOAD tap) =="
"$ZIG" cc -target "$TARGET" -shared -fPIC -O2 -s \
    -o addon/forceAudioIn.so src/forceAudioIn.c -lpthread -lrt

echo "== injectTone (test-tone generator, for standalone smoke-testing) =="
"$ZIG" cc -target "$TARGET" -O2 -s \
    -o addon/injectTone src/injectTone.c -lpthread -lrt -lm

chmod 0755 addon/forceAudioIn.so addon/injectTone
ls -la addon/forceAudioIn.so addon/injectTone
echo "== done -> addon/ =="
