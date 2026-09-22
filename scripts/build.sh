#!/usr/bin/env bash
# =============================================================================
# Build the audio-injection tap (forceAudioJack.so, LD_PRELOAD'd into MPC),
# its test-tone generator (injectTone), and skipbackHost, and drop them into
# addon/.
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
# device's AddOns/ForceAudioJack as-is afterward (see README.md's "Deploy").
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

ZIG="${ZIG:-zig}"
TARGET=arm-linux-gnueabihf.2.39   # matches the Force's exact glibc (confirmed live)

if ! command -v "$ZIG" >/dev/null 2>&1; then
    echo "zig not found (set ZIG=/path/to/zig, or put it on PATH)." >&2
    exit 1
fi

echo "== forceAudioJack.so (LD_PRELOAD tap) =="
"$ZIG" cc -target "$TARGET" -shared -fPIC -O2 -s \
    -o addon/forceAudioJack.so src/forceAudioJack.c -lpthread -lrt

echo "== injectTone (test-tone generator, for standalone smoke-testing) =="
"$ZIG" cc -target "$TARGET" -O2 -s \
    -o addon/injectTone src/injectTone.c -lpthread -lrt -lm

echo "== skipbackHost (Skipback extraction-ring consumer) =="
"$ZIG" cc -target "$TARGET" -O2 -s \
    -o addon/skipbackHost src/skipbackHost.c -lpthread -lrt -lm

chmod 0755 addon/forceAudioJack.so addon/injectTone addon/skipbackHost
ls -la addon/forceAudioJack.so addon/injectTone addon/skipbackHost

# skipbackHost needs its OWN AddOns folder to get a Modules-page toggle -
# nodeServer's moduler scans exactly one NSMODULE.json per AddOns/* folder
# (see docs/PROPOSAL-force-audio-jack.md's packaging note), so addon-skipback/
# is a separate deploy target from addon/ (which maps to AddOns/ForceAudioJack),
# not a subfolder of it. Copy the just-built binary in so addon-skipback/ is
# deploy-ready on its own.
cp addon/skipbackHost addon-skipback/skipbackHost
chmod 0755 addon-skipback/skipbackHost

echo "== done -> addon/ (AddOns/ForceAudioJack) and addon-skipback/ (AddOns/ForceAudioJackSkipback) =="
