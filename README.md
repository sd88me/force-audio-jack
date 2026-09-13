# force-audioin

The **shared audio-injection tap** for the Akai Force running
[MockbaMod](https://github.com/MockbaTheBorg/MockbaMod) - a prerequisite
addon, not a synth in its own right. It injects synthesized/generated audio
into the Force's own audio-in capture path, so a separate process's output
becomes audible on a normal Audio-In track - the inverse of the
`ForceLinkAudio` addon (which taps `snd_pcm_writei` to extract what the
Force plays; this taps `snd_pcm_readi` to inject audio into what it
captures).

Other addons that want to inject audio - [`force-maze`](https://github.com/sd88me/force-maze)'s
`ForceMazeVoice`, and any future ones - depend on this addon being enabled.
They don't bundle their own copy of `forceAudioIn.so` or arm `LD_PRELOAD`
themselves; they just attach to the tap this addon arms. See
`~/.claude/skills/mockbamod-module-creator`'s `references/audio-injection.md`
for the full design story from the consuming side, and this repo's own
[DESIGN.md](DESIGN.md) for the tap's own architecture and incident history.

This repo is the source for what ships pre-built in the
[`sd88me/MockbaMod`](https://github.com/sd88me/MockbaMod) fork at
`SD/AddOns/ForceAudioIn` - that tree is the deploy target (bundled directly
into the fork's SD image since so many addons depend on it), this repo is
where `forceAudioIn.c`/`injectTone.c` are actually developed, built, and
tested. Originally split out of `force-maze` (2026-09-13), which built this
into its own `addon/` as a staging output before manually copying it to the
fork - see `force-maze`'s git history (`scripts/build_audiotap.sh`) for
that prior arrangement.

## How it works

```
your synth/generator process (any language/toolchain, e.g. force-maze's maze_host)
        │  renders audio, writes into a POSIX shared-memory ring
        ▼
forceAudioIn.so (LD_PRELOAD'd into /usr/bin/MPC)
        │  interposes snd_pcm_readi by symbol name (dlsym(RTLD_NEXT, ...)) -
        │  not raw address patching, so no firmware-version dependency.
        │  mixes (sums, never replaces) up to 4 simultaneous voice rings
        │  into whatever real hardware audio MPC reads, so a real
        │  instrument on the physical input keeps working unmodified.
        ▼
Audio-In track on the Force
```

`forceAudioInject.h` is the reusable shared-memory ring layout (magic,
sample rate, channel count, head/tail indices, the sample buffer) -
handles the SPSC (single-producer/single-consumer) atomics for one producer
process and one consumer thread inside `MPC`. Every voice-producing addon
that wants to attach vendors a copy of this exact header (`force-maze`'s
`src/forceAudioInject.h` is one) - it's the ABI contract between producer
and tap, so any copy must stay byte-for-byte identical to this repo's.

`injectTone.c` is a minimal stand-in producer (a fixed sine wave), started
on demand from the nodeServer Modules page (`/moduler`) - useful to prove
the injection path works before wiring up a real DSP engine. A real voice
host (like `force-maze`'s `maze_host`) replaces it, writing rendered audio
into its own slot's ring instead.

## Layout

```
src/
  forceAudioIn.c          LD_PRELOAD tap: mixes voice rings into MPC's capture reads
  injectTone.c            fixed-tone test producer (smoke-test only)
  forceAudioInject.h      shared-memory ring layout - the producer/consumer ABI contract
addon/                  MockbaMod addon: manage.sh, run_ForceAudioIn.sh (autostart hook,
                        arms the tap only - zero voices ever attached at boot),
                        NSMODULE.json (nodeServer Modules-page entry for injectTone)
scripts/
  build.sh                zig cross-build (no Docker needed - see script header)
tests/
  test_mix.c              native unit test against the REAL forceAudioIn.c mixing/
                          attach code (#includes it directly - not a reimplementation)
  run.sh                  builds and runs test_mix.c natively, host-arch, no device needed
```

## Build

```bash
ZIG=/path/to/zig ./scripts/build.sh
```

Writes `addon/forceAudioIn.so` and `addon/injectTone`, ready to deploy as-is.

## Test

```bash
./tests/run.sh
```

Native, host-arch, no device or Docker needed - see `tests/test_mix.c`'s own
header comment for exactly what this can and can't validate (the mixing/
attach logic, not the real ALSA interposition itself).

## Deploy / enable

```
ssh root@<force-ip> 'rm -rf /media/<serial>/AddOns/ForceAudioIn'   # see note below
scp -r addon root@<force-ip>:/media/<serial>/AddOns/ForceAudioIn
ssh root@<force-ip> '/media/<serial>/AddOns/ForceAudioIn/manage.sh ENABLE'
```

`scp -r addon dest` copies `addon` itself as a subdirectory of `dest` if
`dest` already exists (`dest/addon/...`) rather than merging its contents
into `dest` - the `rm -rf` first avoids that (safe to skip only when
deploying to a path that doesn't exist yet).

`manage.sh ENABLE` arms the tap at boot with **zero voices ever attached** -
proven safe across every repeated-restart test run against it, including a
real physical reboot. It does not start `injectTone` or any other producer.
Start/stop a voice from the nodeServer Modules page instead, whenever you
actually want one running - never by editing this addon's own scripts.

Logs: `/tmp/forceAudioIn.log` (the shared tap).

## The hard rule

**Never restart `acvs` while any voice is attached.** Extensive live
testing found that doing so reliably kills pads/buttons (occasionally
wifi) - on the very first restart, not gradually. The mechanism is still
unidentified despite ruling out symbol collision, a background diagnostics
thread, the per-sample mix loop itself, and both co-loaded libraries'
constructors (confirmed inert via disassembly) - see [DESIGN.md](DESIGN.md)
for the full investigation. `forceAudioIn.so` armed with zero voices, by
contrast, has never failed a single test. So: enable this addon once (arms
the tap, persists across boots, zero voices), then only ever start/stop
voices via the Modules page, and never restart `acvs` manually while one is
running.

## Notes

- Multiple simultaneous voices from different addons are supported and
  intentional (each takes its own `--slot` 0-3).
- See [DESIGN.md](DESIGN.md) for the full design story (clock-rate
  handling, the boot-race constraint, why this class of `LD_PRELOAD` use is
  lower-risk than raw binary patching, and the still-open pads-dead-on-
  restart investigation) and the `mockbamod-module-creator` skill's
  `references/audio-injection.md` for the consuming-addon's-eye view.

## License

No upstream license constraints (unlike `force-maze`/`force-acid`, which
inherit `schwung-*`'s terms for the ported DSP/generator core) - this is
original interposition/shared-memory code written for this project.
