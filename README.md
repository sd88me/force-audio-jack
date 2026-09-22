# force-audio-jack

**A shared audio tap for the Akai Force — the prerequisite add-on that
lets a background synth or generator process appear as live signal on
a real Audio-In track (or the physical Out 3/4 jacks), with no
hardware loopback cable, plus Skipback (continuous background
recording so a shortcut can save the last N seconds retroactively).**

force-audio-jack is not a synth or sequencer in its own right — it makes
no sound by itself. It's the shared, always-on tap that other
add-ons (Maze Voice, and any future voice-producing add-on) inject
their own rendered audio through. Formerly named force-audioin;
renamed when Out 3/4 injection and Skipback were merged in.

**Status: the original In-bus (Audio-In 1/2) injection tap is v1.0,
stable on real Force hardware.** Out-bus (physical Out 3/4) injection
and Skipback are newly built and unit-tested but **not yet verified on
real hardware** — see [docs/PROPOSAL-force-audio-jack.md](docs/PROPOSAL-force-audio-jack.md)
for exactly what's implemented vs. still pending. This document is the
install/usage manual for the stable In-bus tap. For internals and the
full technical design, see [DESIGN.md](DESIGN.md).

---

## Table of contents

- [What is the audio-injection tap?](#what-is-the-audio-injection-tap)
- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Using force-audio-jack](#using-force-audio-jack)
- [The hard rule](#the-hard-rule)
- [Building from source](#building-from-source)
- [Testing](#testing)
- [Diagnostics](#diagnostics)
- [Troubleshooting](#troubleshooting)
- [Project layout](#project-layout)
- [Related projects & credits](#related-projects--credits)
- [License](#license)

---

## What is the audio-injection tap?

The Force's own application (MPC) opens its audio codec directly and
holds both playback and capture exclusively — there's no JACK server,
no ALSA loopback device, and no software mixer sitting in between.
That's a real problem for any add-on that wants to render its own
audio in a separate background process (rather than as a plugin
inside MPC itself): there's no supported way for that audio to reach
a real Audio-In track.

**force-audio-jack solves this by mixing injected audio directly into
what MPC itself reads from its own capture device**, live, in real
time — no cable, no hardware loopback, no change to how MPC's Audio-In
track behaves. A separate process (Maze Voice's `maze_host`, or this
add-on's own test tone generator, `injectTone`) renders audio into a
small shared-memory ring buffer; force-audio-jack's tap reads from that
ring and adds it directly into MPC's own captured audio, sample by
sample, on the real audio thread. A real instrument plugged into the
physical input keeps working completely unmodified alongside whatever
gets injected.

This is the audio equivalent of a separate add-on in this family,
[force-shadow](https://github.com/sd88me/force-shadow), which does the
same kind of thing for the screen and touchscreen instead of audio —
both exist to give a background process a real, native-feeling
integration point with the Force's own hardware, without changing
anything about how MPC itself works.

force-audio-jack is the mirror image of
**[force-link-audio](https://github.com/macdigi/force-link-audio)**, an
add-on by [macdigi](https://github.com/macdigi) that taps the *output*
side (`snd_pcm_writei`) to extract what the Force is playing; this
add-on taps the *input* side (`snd_pcm_readi`) to inject audio into
what it captures. force-link-audio's own interposition approach was
the reference point this add-on's design started from.

## Features

- **Real Audio-In signal, no hardware loopback.** Injected audio
  appears on a normal Audio-In track exactly as if it were coming from
  a physical input — no cable, no extra audio interface.
- **Up to 4 simultaneous voices.** Multiple add-ons (or multiple
  instances) can each inject their own independent audio stream at
  once, mixed together automatically.
- **Per-voice volume, mute, and L/R/L+R routing** — controlled
  directly by each voice's own control socket, with no separate
  central mixer to configure.
- **Real instruments keep working.** Injection is purely additive: a
  real instrument on the physical audio input is never touched or
  replaced.
- **Starts on demand, not at boot.** force-audio-jack itself arms at boot
  with **zero voices ever attached** — a voice-producing process is
  only ever started later, on demand, from the Force's own nodeServer
  Modules page.
- **Picks up a new voice live, no restart needed.** A background
  thread checks for new (or replaced) voice rings roughly every 2
  seconds, so starting a voice's process is all that's needed — no
  reboot, no add-on restart.
- **Fails safe, always.** Missing shared memory for a given voice slot
  is not an error — that slot is simply not attached yet, and normal
  capture continues completely unaffected. Any other failure degrades
  the same way: real captured audio always passes through untouched.
- **No extra runtime dependencies on the Force.** Links against
  exactly `libasound`/`libpthread`/`librt` — nothing else to install
  on-device.
- **Includes a built-in test signal.** `injectTone`, a minimal sine
  wave generator, proves the whole injection path works before ever
  wiring up a real synth.
- **Out-bus injection (new, not yet hardware-verified).** The same
  additive mixing, symmetrically, into the physical Out 3/4 jacks
  instead of Audio-In 1/2 — lets a synth module choose whether its
  audio stays inside Force OS or goes out to an external
  device/mixer. See [docs/PROPOSAL-force-audio-jack.md](docs/PROPOSAL-force-audio-jack.md).
- **Skipback (new, not yet hardware-verified).** Continuously records
  the real main-mix output into a rolling buffer, so a `SHIFT+RECORD`
  shortcut saves the last N seconds retroactively as a WAV, named with
  the current project and tempo when available.

## Requirements

- An Akai Force running
  [MockbaMod](https://github.com/MockbaTheBorg/MockbaMod) — force-audio-jack
  is installed as a MockbaMod add-on.
- SSH access to the device for installation.
- No other software to install on the Force itself.
- Any add-on that wants to inject audio (e.g.
  [force-maze](https://github.com/sd88me/force-maze)'s Maze Voice) needs
  this add-on installed and enabled first — they don't bundle their own
  copy of the tap.

## Installation

1. **Copy the add-on onto the device**, replacing any previous copy
   (`scp -r` copies `addon` *into* an existing destination rather than
   replacing it, so remove any old copy first):
   ```
   ssh root@<force-ip> 'rm -rf /media/<serial>/AddOns/ForceAudioJack'
   scp -r addon root@<force-ip>:/media/<serial>/AddOns/ForceAudioJack
   ```
2. **Enable it:**
   ```
   ssh root@<force-ip> '/media/<serial>/AddOns/ForceAudioJack/manage.sh ENABLE'
   ```
   This arms the tap at boot with **zero voices ever attached** — it
   does not start `injectTone` or any other producer by itself. This
   behaviour has been proven safe across repeated restarts and a real
   physical reboot.
3. **Restart the Force** (or `systemctl restart acvs`) so the add-on
   is picked up — safe to do at this point, since no voice is attached
   yet (see [The hard rule](#the-hard-rule) below for why this matters).

That's it — the tap is now armed. Nothing is audible or different in
normal use until a voice is actually started (see below).

To remove or disable the add-on later, use `manage.sh`'s own commands
— it follows the same convention as other MockbaMod add-ons in this
family (see `manage.sh`'s own usage output on the device for the exact
options available).

## Using force-audio-jack

**Starting a voice:** open the Force's own nodeServer Modules page
(`/moduler`) and start the voice-producing process you want — this
add-on's own `injectTone` (a fixed test tone, useful for confirming
everything works), or a real synth like Maze Voice's `maze_host`. The
background re-attach thread picks up the new voice's shared-memory
ring within about 2 seconds — no restart needed, and no further action
required. Once attached, its audio is mixed live into the Audio-In
track.

**Stopping a voice:** stop it from the same Modules page toggle. This
does a clean process kill with no `acvs` restart involved.

**Adjusting a voice's volume, mute, or L/R routing:** these are
controlled directly by that voice's own control socket (e.g. Maze
Voice's own on-screen or web controls) — force-audio-jack itself has no
separate mixer page of its own.

## The hard rule

**Never restart `acvs` while any voice is attached.**

Extensive live testing found that doing so can reliably kill
pads/buttons (occasionally Wi-Fi) — on the very first restart, not
gradually — while force-audio-jack is armed with a voice actually
attached. With **zero** voices attached, by contrast, `acvs` restarts
(and full physical reboots) have never failed a single test.

This is why voices are always started via the Modules page rather than
at boot: that path never touches `acvs` at all, so it can never
trigger this. In everyday use, this only matters if you're doing your
own device-level maintenance:

- **Before restarting `acvs` or rebooting for any other reason, stop
  every running voice first** (via the Modules page), then restart.
- This add-on being *enabled* is always safe on its own, at any time,
  including across a real reboot — the risk is specifically tied to a
  voice being actively attached during the restart itself, not to the
  add-on being armed.

The underlying cause is not yet fully identified — see
[DESIGN.md's Known limitations](DESIGN.md#known-limitations) for what's
been ruled out and what's still suspected.

## Building from source

Cross-compiled with `zig cc`, matching the Force's exact glibc — no
Docker or QEMU needed for this add-on:

```
ZIG=/path/to/zig ./scripts/build.sh
```

Writes `addon/forceAudioJack.so` and `addon/injectTone`, ready to deploy
as-is. See `scripts/build.sh`'s own header comment for the exact `zig
cc` invocation and target triple.

## Testing

```
./tests/run.sh
```

Builds and runs `tests/test_mix.c` natively (host architecture, no
device or Docker needed). This test `#include`s the real
`forceAudioJack.c` mixing/attach code directly — it's testing the actual
shipped logic, not a separate reimplementation of it. It validates the
ring-attach and mixing logic; it does **not** validate the real ALSA
interposition itself (which can only be confirmed on-device). See
`tests/test_mix.c`'s own header comment for the exact scope.

## Diagnostics

Runtime logs are written to **`/tmp/forceAudioJack.log`** on the device.

For deeper investigation, two file-triggered mechanisms exist (no
device reboot or SSH environment-variable support needed — both are
plain marker files, checked by a background thread roughly every 200
ms to 2 seconds):

- **`/tmp/forceAudioJack.diag`** — touch this file to turn on verbose
  periodic logging (per-voice backlog, gain, routing, underrun counts,
  etc.) to the log file above.
- **`/tmp/forceAudioJack.dumpreq`** — touch this file to request a dump
  of the tap's internal event trace (a rolling record of every attach,
  read, mix, and trim event) to `/tmp/forceAudioJack.dump.<pid>`. Useful
  for reconstructing what happened around a specific failure, since
  MPC itself does not crash when pads go unresponsive — it stays
  running, so a dump can be requested well after the fact.

## Troubleshooting

**A voice doesn't seem to be making any sound.** Confirm it actually
attached: check `/tmp/forceAudioJack.log` for a `voice slot N attached`
line, or touch `/tmp/forceAudioJack.diag` and check the periodic
per-voice backlog log lines that follow. If nothing shows the voice
attaching at all, confirm its own process is actually running (via the
Modules page) and that it's using the same shared-memory ring layout
this add-on expects (see [DESIGN.md](DESIGN.md) if you're building your
own voice producer).

**Pads/buttons went unresponsive after restarting `acvs` or
rebooting.** This is the known issue described in
[The hard rule](#the-hard-rule) above — it only happens when a voice
was attached at the moment of the restart. Power-cycle the device to
recover, then make sure every voice is stopped via the Modules page
*before* the next `acvs` restart or reboot.

**Audio glitches or small dropouts during long playback.** A confirmed
(not just suspected) bug in the ring's backlog accounting can alias
once a voice's true backlog exceeds about 1.5 seconds — most likely if
a producer keeps rendering across a long gap (e.g. an `acvs` restart)
with no consumer draining it. See
[DESIGN.md's Known limitations](DESIGN.md#known-limitations). This is
a separate issue from the pads/buttons hard rule above and is an audio
quality issue only.

**Want to check what's going on right now.** Tail the log:
```
ssh root@<force-ip> 'tail -f /tmp/forceAudioJack.log'
```

## Project layout

```
DESIGN.md                       technical design & architecture reference (the stable In-bus tap)
docs/
  PROPOSAL-force-audio-jack.md  design + build-order status for Out-bus/Skipback (in progress)
  PENDING-DEVICE-FIXES.md       fixes staged but not yet applied to the live device
src/
  forceAudioJack.c              the interposer: readi hook (In-bus) + writei hook (Out-bus, Skipback extraction)
  forceAudioInject.h            In/Out-bus ring layout - the producer/consumer ABI contract
  forceAudioJackExtract.h       Skipback extraction-ring layout (reversed producer/consumer roles)
  injectTone.c                  fixed-tone test producer (smoke-test only; --bus in|out)
  skipbackHost.c                Skipback consumer: rolling buffer, WAV-on-trigger
addon/                          installable MockbaMod add-on (AddOns/ForceAudioJack)
                                 (manage.sh, run_ForceAudioJack.sh, NSMODULE.json, forceAudioJack.so, injectTone)
addon-skipback/                 skipbackHost's own AddOns folder (AddOns/ForceAudioJackSkipback) -
                                 needs a separate folder for its own Modules-page toggle
scripts/
  build.sh                      zig cross-build (no Docker needed)
tests/
  test_mix.c                    native unit test against the real forceAudioJack.c mixing/attach code
  run.sh                        builds and runs test_mix.c natively, no device needed
```

## Related projects & credits

Built by [sd88me](https://github.com/sd88me).

- **[MockbaMod](https://github.com/MockbaTheBorg/MockbaMod)** by
  [MockbaTheBorg](https://github.com/MockbaTheBorg) — the custom
  firmware add-on framework this project is built to run on top of,
  and a prerequisite for installing it (see
  [Requirements](#requirements)).
- **[force-link-audio](https://github.com/macdigi/force-link-audio)**
  by [macdigi](https://github.com/macdigi) — the mirror-image add-on
  (output-side tap) this project's own design is the inverse of, and
  the reference point its interposition approach started from (see
  [What is the audio-injection tap?](#what-is-the-audio-injection-tap)
  above).
- **[force-shadow](https://github.com/sd88me/force-shadow)** — the
  display/touchscreen equivalent of this project: a shared tap that
  gives background add-ons a real, native control surface on the
  Force's own screen, the same way this add-on gives them a real
  Audio-In signal.
- **[force-maze](https://github.com/sd88me/force-maze)** — Maze Voice,
  the first real synth voice to use this add-on's injection tap.

## License

[MIT](LICENSE) — this is original interposition/shared-memory code
written for this project, with no upstream license constraints (unlike
force-maze/force-acid, which inherit the ported Schwung DSP/generator
core's own terms). See the [LICENSE](LICENSE) file in this repository.
