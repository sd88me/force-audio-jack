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
MIN_ZIG_VERSION="0.14.0"          # see the version check below for why

if ! command -v "$ZIG" >/dev/null 2>&1; then
    echo "zig not found (set ZIG=/path/to/zig, or put it on PATH)." >&2
    exit 1
fi

# zig 0.13.0 has a real ARM codegen bug: a variadic double argument to
# printf/fprintf/snprintf gets marshaled incorrectly for arm-linux-gnueabihf,
# causing a segfault deep inside glibc's vfprintf on the device - confirmed
# by bisection down to a bare `printf("%.1f\n", 10.1);` with nothing else in
# the program. This is exactly what skipbackHost's startup banner
# (`... %.1f MB ...`) and injectTone's own banner hit. Fixed in zig 0.14.1;
# not otherwise worked around here (no known 0.13.x-safe formatting
# workaround was found, and there is no reason to keep supporting 0.13.x).
zig_version=$("$ZIG" version)
zig_ver_num=${zig_version%%-*}   # strip a "-dev.N+hash" suffix if present
ver_ge() {   # ver_ge A B -> true if version A >= version B (dotted numeric)
    [ "$1" = "$2" ] && return 0
    [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n1)" = "$2" ]
}
if ! ver_ge "$zig_ver_num" "$MIN_ZIG_VERSION"; then
    echo "ERROR: zig $zig_version is too old (need >= $MIN_ZIG_VERSION)." >&2
    echo "zig 0.13.0 miscompiles variadic doubles in printf-family calls on" >&2
    echo "arm-linux-gnueabihf - a real crash, not a style nit. Get a newer" >&2
    echo "zig from https://ziglang.org/download/." >&2
    exit 1
fi

echo "== forceAudioJack.so (LD_PRELOAD tap) =="
# --version-script is load-bearing: it keeps the dynamic symbol table down to
# just the four interposed snd_pcm_* entry points. Without it zig cc exports
# its whole statically-linked compiler-rt/libm (memcpy, memset, the math
# functions, __stack_chk_guard - ~385 symbols), and under LD_PRELOAD those
# hijack the same calls process-wide in MPC. See scripts/forceAudioJack.map.
"$ZIG" cc -target "$TARGET" -shared -fPIC -O2 -s \
    -Wl,--version-script=scripts/forceAudioJack.map \
    -o addon/forceAudioJack.so src/forceAudioJack.c -lpthread -lrt

# Guard against a regression here silently reintroducing the crash: the tap
# must export exactly the four snd_pcm_* symbols it interposes, nothing else.
exported=$(readelf --dyn-syms -W addon/forceAudioJack.so \
    | awk '$7!="UND" && ($5=="GLOBAL"||$5=="WEAK") {print $8}' | sort)
expected=$(printf '%s\n' snd_pcm_hw_params snd_pcm_readi snd_pcm_readn snd_pcm_writei | sort)
if [ "$exported" != "$expected" ]; then
    echo "ERROR: forceAudioJack.so's exported symbols are not the expected four." >&2
    echo "This will hijack libc/libm calls process-wide inside MPC. Got:" >&2
    printf '%s\n' "$exported" >&2
    exit 1
fi
echo "   exported symbols OK (exactly the 4 interposed snd_pcm_* entry points)"

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
