#!/usr/bin/env bash
# Native (host-arch) build + run of the mixing/attach unit test. No Docker,
# no cross toolchain, no real device or ALSA needed - test_mix.c #includes
# the real src/forceAudioIn.c directly and exercises its static functions
# (mix_in_one, chan_allowed, ai_try_attach) against synthetic in-process
# ai_shm_t structs and real POSIX shared memory. See test_mix.c's own header
# comment for exactly what this can and can't validate.
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

$CC -O2 -Wall -Wextra -Wno-unused-function -Isrc -std=gnu11 \
    tests/test_mix.c -lpthread -lrt -lm \
    -o "$OUT/test_mix"

"$OUT/test_mix"
