# ForceAudioIn — design notes

Extracted from `force-maze/DESIGN.md` (2026-09-13) when this addon's source
moved out of `force-maze` into its own repo, since most of this history is
about `forceAudioIn.so` itself, not about `force-maze`'s DSP voice. Where a
finding was made *using* `force-maze`'s `maze_host` as the first real voice
consumer, that's noted inline - `force-maze/DESIGN.md` still has that
project's own side of the same story.

## Why this needs LD_PRELOAD at all

Confirmed live via `/proc/<mpc-pid>/fd`: the Force's main app
(`/usr/bin/MPC`, a JUCE binary) opens its audio codec (`hw:2` on the
hardware inspected so far - **card numbers shift depending on what USB
devices are plugged in**, always re-verify with `aplay -l`/checking open
fds rather than hardcoding a number) with raw `hw:` device names and holds
both playback and capture **exclusively**. No JACK server running (a
bundled `libjack` some addons need to *link* against is NOT the same thing)
and no `snd-aloop` kernel module built into this kernel. So there is no
host-level audio bus, loopback device, or software mixer to hook into from
outside the process - the **only** seam is interposing the ALSA calls `MPC`
itself makes, inside its own process.

## This is LD_PRELOAD, but not the risky kind

The `mockbamod-module-creator` skill's `gotchas.md` blast-radius ranking
puts "LD_PRELOAD injection into the main app process" at the top as
highest-risk, citing `mockbaMagic` as the example - **raw in-memory binary
patching at addresses from a table keyed to exact firmware version**. This
addon uses a meaningfully different, lower-risk technique: **symbol
interposition** against libasound's stable public ABI.

```c
static ssize_t (*orig_readi)(snd_pcm_t*, void*, snd_pcm_uframes_t);
orig_readi = dlsym(RTLD_NEXT, "snd_pcm_readi");
```

This hooks by *symbol name* against a public library's documented ABI, not
raw offsets in the closed `MPC` binary - no dependency on the exact
firmware version the way `mockbaMagic`'s address table has, and it fails
closed by construction: on any error, pass straight through to the real
ALSA function. `ForceLinkAudio`'s `forceStream.so` is the mirror-image
addon (taps `snd_pcm_writei` to *extract* what `MPC` plays, instead of
injecting into what it captures).

Real-time-safe by design: no allocation, syscalls, or logging on the hot
path (the interposed call itself) - all setup happens once in a library
constructor (`__attribute__((constructor))`), which runs at load time,
never on the audio thread.

## Design: a producer process + a shared-memory ring + the interposed shim

`forceAudioInject.h` is the reusable ring layout (a fixed `shm_open`'d
region: magic, sample rate, channel count, head/tail indices, the sample
buffer) - any producer addon vendors an exact copy of this header rather
than inventing a new one; it handles the SPSC (single-producer/single-
consumer) atomics correctly for one producer process and one consumer
thread inside `MPC`.

**Additive mixing, not replacement**: the tap adds ring samples to whatever
real hardware audio is already there, so a real instrument plugged into the
physical input keeps working unmodified alongside the injected signal.

## Multiple simultaneous voices (per-voice mix control, not a central mixer)

`forceAudioIn.so` mixes up to `AI_MAX_VOICES` (4) independent voice hosts at
once, each in its own named shared-memory ring (`/forceAudioInject0`,
`/forceAudioInject1`, ...). Every ring stays genuinely single-producer/
single-consumer (one voice host writes its own ring; `forceAudioIn.so` is
the sole reader of all of them), so this scales without adding any
cross-process synchronization beyond what already existed per ring.

**Deliberately no central "mixer" control panel.** Each ring's `enabled`/
`gain`/`channel_mask` fields are the on/off, volume, and L/R/L+R routing for
*that* voice, written by that voice's own control path - a separate
coordinating mixer page would need its own IPC into each voice's socket for
no real benefit.

**Channel select is L / R / L+R, not an arbitrary voice-count.** The tapped
capture handle is confirmed 2-channel - that's the actual hardware ceiling,
not a software choice. Two voices routed to the same channel simply sum
there, same as two synths sharing one mixer channel.

**Mute happens at the consumer, never by pausing the producer.** A voice's
own render cadence is its own synth's clock; `enabled=0` only skips adding
that voice's samples into the output - the ring is still drained at the
normal rate so a re-enabled voice resumes from live backlog, not a stale
one.

**LD_PRELOAD is armed once, not per voice.** `forceAudioIn.so` itself needs
loading into `MPC` exactly once - it already attaches to every slot that
has a ring present. A second/third simultaneous voice addon just needs a
distinct `--slot`; its own `run_*.sh` must never touch `$mmLD_PRELOAD_VAR`
itself (see "boot-time LD_PRELOAD race" below) - this addon is the only one
that should ever write the `forceAudioIn` entry.

**Confirmed live on real hardware (2026-09-13)**, using `force-maze`'s
`maze_host` (slot 0) alongside this addon's own `injectTone` (slot 1,
`--channel R`): both audible simultaneously, `/proc/<MPC-pid>/maps`
confirmed both `/forceAudioInject0` and `/forceAudioInject1` live and
current.

## Segment replacement: re-attach must handle a voice host restarting, not just starting

A voice host that's stopped and later restarted (toggled off/on via the
Modules page) may very plausibly `shm_unlink()` and recreate its ring from
scratch on every start ("start clean") - a **new inode under the same
name**. `forceAudioIn.c`'s attach logic must not treat "already attached"
as permanent: it `stat()`s the ring's path (cheap, off the hot path, from
the same background thread that does lazy re-attach) and compares inodes,
re-attaching if the segment was replaced. Without this, a stopped-then-
restarted voice silently mixes from an orphaned, no-longer-written-to ring
- no crash, just silence, a confusing failure mode to debug blind since
everything *looks* running. The old mapping is deliberately leaked (never
`munmap`'d) rather than freed, since the audio thread's hot path may be
mid-read of it via its own lock-free pointer load at the exact moment a
background thread would want to swap it - leaking one ring's worth of
memory (~512KB) per voice-restart is a bounded, deliberate tradeoff against
ever risking a use-after-unmap there.

Verified with unit tests against a real POSIX shm segment (`tests/test_mix.c`,
28/28 checks: stays unattached when absent, attaches once present, doesn't
double-count on repeat calls, hot path picks up a late attach with zero
call-site changes, re-attach picks up a replaced segment's new data).

## Starting a voice: always after boot, via the nodeServer Modules page

A voice host's producer process should be started **only** on demand, via
its own `NSMODULE.json`/nodeServer Modules-page toggle - never by a voice
addon's own `run_<name>.sh` at boot, and never gated behind a `ps | grep
"{MPC Main Thread}"` wait loop either.

- `forceAudioIn.so`'s background thread does **lazy re-attach**: it notices
  a new voice ring within ~2s of it appearing, with no `acvs` restart
  needed, and re-attaches again if that ring gets replaced (see above) - so
  there's no ordering race to lose.
- **The hard rule**: extensive live testing found that restarting `acvs`
  while *any* voice ring is attached reliably kills pads/buttons
  (occasionally wifi) - on the very first restart, not gradually, with the
  actual mechanism still unidentified (see the open incident below).
  `forceAudioIn.so` armed with zero voices, by contrast, has never failed a
  single test. So a voice must **never** be attached before an `acvs`
  restart happens - exactly what starting it only via the Modules page
  (never at boot) guarantees, since that path never touches `acvs` at all.

## Boot race: the tap's constructor must find the ring already there

`forceAudioIn.so`'s constructor needs a voice's `/forceAudioInjectN` shared-
memory ring to already exist the moment `MPC`'s process is `exec`'d
(constructors run at library load, before `main()`) *if* that voice is
expected to be attached from boot - which, per the shipped baseline below,
no voice ever is. A voice host that starts later relies entirely on the
lazy re-attach thread instead, not this boot-time path.

## Boot-time LD_PRELOAD race (confirmed 2026-09-13)

A serious, unrelated race discovered via real overnight boot failures:
pads/buttons dead, or WiFi dead, alternating unpredictably across
successive reboots, sometimes fine. Traced to `/dev/shm/.LD_PRELOAD` (the
file `apps.sh` reads into `LD_PRELOAD` before exec'ing `MPC`).

**Confirmed by reading the actual scripts on a live device**: `mockbaMagic`'s
and `MidiLoop`'s own `run_*.sh` scripts both read this file, check their
library isn't already present, and write the whole file back - with **no
locking at all**. MockbaMod's own boot sequence backgrounds every top-level
addon script concurrently. Three or more unsynchronized read-modify-write
scripts racing on one shared file at boot is a textbook lost-update:
whichever write lands last wins, silently dropping another script's entry.

This bug is pre-existing in MockbaMod itself (`mockbaMagic` and `MidiLoop`
already race each other). **Mitigation applied here**: `run_ForceAudioIn.sh`
and `addon/manage.sh` wrap every read-modify-write of `$mmLD_PRELOAD_VAR` in
an `mkdir`-based mutex (atomic even on busybox; bounded retry, fails open
rather than risking a hung boot on a stale lock).

**Actually fixed at the source (2026-09-13)**: the same `mkdir` lock was
patched directly into `mockbaMagic`'s and `MidiLoop`'s own `run_*.sh`
scripts in the `sd88me/MockbaMod` fork (`SD/AddOns/mockbaMagic/`,
`SD/AddOns/MidiLoop/`) - full writeup in the `mockbamod-module-creator`
skill's `references/gotchas.md`. Any *other* fork/device without that patch
still has the unlocked race on the other two addons' side, so this addon
keeps doing its own locking regardless.

Also learned along the way: `systemctl restart acvs` doesn't just restart
the touchscreen app - its cgroup includes `boot.sh` itself, so restarting
`acvs` **re-runs the entire top-level `AddOns/*.sh` kill+relaunch
sequence**, hitting this exact race again every time. That's actually
useful: `acvs` restarts (fast, no power-cycle needed) are a valid way to
repeatedly re-test this.

## `acvs`, not `inmusic-mpc`

The `mockbamod-module-creator` skill's older reference docs called the
service to restart for LD_PRELOAD-content changes `inmusic-mpc`. On this
device, `systemctl list-units` shows no such unit - the real service is
**`acvs`**, described by systemd itself as "InMusic MPC Application".
Confirmed live; `manage.sh` here calls `systemctl restart acvs`.

## Open incident: pads/buttons dead with forceAudioIn.so armed (2026-09-13, still unresolved)

Found while testing multi-voice mixing on real hardware: two live `acvs`
restarts with `/dev/shm/.LD_PRELOAD` content confirmed correct both times
(all libraries present) still killed pads/buttons. This is **not** the
boot-time file race above - that race is about the file's *content* being
wrong; here the content was right and it still happened. Root cause not
found. This section records the elimination sequence in order - **don't
re-test anything already ruled out here**.

**Ruled out via static analysis (no device access needed):**

- **Not a symbol collision with MidiLoop's `tkgl_anyctrl_lt.so`.**
  `forceAudioIn.so` interposes only `snd_pcm_readi`/`snd_pcm_readn`/
  `snd_pcm_hw_params` (PCM streaming). `tkgl_anyctrl_lt.so` interposes
  `snd_rawmidi_open`/`snd_rawmidi_read`/`snd_seq_create_simple_port`/
  `snd_midi_event_decode`/`aconnect` (sequencer/rawmidi) plus a
  `midiPortBlacklist.txt`-driven filter - confirmed directly from both
  `.so`'s dynamic symbol tables. Zero overlap.
- **Probably not mockbaMagic's raw address-patching either.**
  `mockbaMagic.din` genuinely is a firmware-version-keyed patch table (raw
  bytes show `FORCE` + version strings + address/offset/bytes records), but
  the script that actually invokes the ptrace-based patcher
  (`livePatcher.sh`) is commented out in this device's `run_mockbaMagic.sh`
  - the raw-patch mechanism looks dormant on this device right now.

**Live-tested elimination sequence, each a real `acvs`-restart cycle on the actual device:**

1. **Diagnostics thread's mere existence** - gated off by default behind a
   marker file (`AI_DIAG_MARKER` = `/tmp/forceAudioIn.diag`; `touch` it to
   re-enable for debugging). **Ruled out**: failed again with the thread
   confirmed not spawning.
2. **`forceAudioIn.so` merely being loaded, zero voices attached.**
   **Ruled out**: survived 3x rapid restarts cleanly.
3. **The per-sample write loop in `mix_in_one` specifically** - tested by
   attaching a voice but muting it (`mix.enabled = 0`) before the restarts,
   which still runs the atomic head/tail loads and backlog/trim math but
   skips the `chan_allowed`/`sample_to_float`/`float_to_sample` inner loop
   entirely. **Ruled out**: failed again, same restart-2/3 pattern.
4. **Idle-voice test, mixing disabled** - ring attached and drained on
   every ALSA read (backlog/trim math, atomic tail update all still run)
   but the entire per-sample write loop skipped. 3x `acvs` restart, `mix.
   enabled=0` reconfirmed as `0` after every one. **Pads still died on
   restart #3** - identical pattern to every prior failing test.

**Status after step 4**: symbol collision, the diagnostic thread,
"restarting `acvs` is just unsafe," "library merely loaded," and the
per-sample mixing/write loop are ALL ruled out. What's left: the ring
bookkeeping/atomics/backlog-trim path that runs on every ALSA read whenever
a ring is attached, regardless of `enabled` - or possibly just the act of
having any voice ring attached and being polled at all, independent of what
happens with the data.

**Lazy-attach workaround - built and tested (2026-09-13).** Rather than
find the root cause first, kept `forceAudioIn.so` armed permanently at boot
(proven safe with zero voices across repeated restarts) and only started a
voice host later, on demand, without ever restarting `acvs` again
afterward. This needed refactoring attach into `ai_try_attach(slot)`,
called both from the constructor (all 4 slots, once) and from a background
thread that runs unconditionally, waking every ~2s to retry any slot still
unattached - publish is a proper atomic release-store of the pointer plus
atomic increment of the attached count, with the audio-thread hot path
switched from plain reads to atomic acquire-loads of both, since they can
now legitimately change after constructor time from a different thread.

**The actual safety question got a worse-than-hoped answer.** The first
`acvs` restart after a voice attached this way (no prior restart while it
was running) failed immediately - not after 2-3 restarts like every prior
test. **This changes the model**: it isn't "takes a few restarts to build
up," it's "any `acvs` restart while a voice ring is attached is unsafe,"
full stop, with no cushion. Net effect on the workaround: still logically
sound (zero-voices-at-boot is safe; lazy-attach neither adds to nor removes
the "restart while attached" hazard) - but the safety margin is exactly
zero restarts while a voice is attached, not "use in moderation." This is a
**hard rule**, not a heuristic - including indirect triggers (a WiFi/
network change that bounces `acvs`, manual restarts for other debugging,
etc).

**`mockbaMagic.so` load-address shift: measured, then ruled out by
disassembly.** A voice-attached restart's `/proc/<MPC-pid>/maps`, diffed
against a zero-voice baseline, showed `mockbaMagic.so`'s own load base
shift by exactly `0x40000` (256KB) - mechanically expected (the extra
mappings load earlier in the sequence, pushing everything after them down).
Disassembling both co-loaded libraries' actual `.init_array` entries (real
ARM/Thumb-2 objdump, not inference from symbol names) found both provably
inert at load time: `mockbaMagic.so`'s three constructors are `frame_dummy`
(EH-frame boilerplate) and two `std::ios_base::Init` calls (automatic
iostream setup) - nothing touches `mockbaMagic.din` or does address
arithmetic; that logic exists but is only reached from the separate
standalone `mockbaMagic <pid>` executable's own `main()` (spawned by the
currently-disabled `livePatcher.sh`), not from anything that runs merely by
being `LD_PRELOAD`'d into `MPC`. `tkgl_anyctrl_lt.so`'s one constructor is
also just `frame_dummy`. **Ruled out**: neither suspect library's own
startup code can be broken by where it lands in memory.

**Breakthrough: this is a race, not a fixed bug.** A test running
`systemctl restart acvs &` in the background, with a concurrent `ps`-polling
loop in the same shell (incidental, there for an unrelated `strace` attempt)
- the first "voice already attached" restart that did NOT kill pads/wifi,
after that exact repro shape had failed with zero exceptions across every
prior test. The one difference: extra CPU/scheduling activity during MPC's
startup that no prior test had. n=1, not conclusive alone, but strong
evidence this is a race condition, not a fixed logical or address bug -
consistent with the address-shift finding (real, measurable, but inert on
its own).

A controlled follow-up (a deliberate, opt-in, marker-gated delay at the very
start of `ai_ctor`, gated behind `/tmp/forceAudioIn.delay`) was run live:
300ms, 2 restarts with a voice attached before each - 1 pass, 1 fail. Not
enough to confirm or rule out the race theory (a genuinely racy ~50/50
mechanism looks exactly like this by chance), but enough to rule out "300ms
alone is a reliable fix."

**Status: static analysis and the delay experiment have both run out of
road for now.** Everything compared so far (`LD_PRELOAD` content,
`/proc/maps`) has been an END-STATE snapshot. What hasn't been tried:
capturing what happens differently *during* a failing restart vs a clean
one - e.g. `strace -f -tt` attached across the `acvs` restart sequence, or
wrapped around `MPC`'s exec, diffing a voice-attached failing restart
against a clean baseline. Flagged as the most information-dense next step
if/when there's appetite for another live cycle - not run yet.

**A related but unconfirmed real bug**, found while reviewing this code
(not fixed - parked until the incident above is resolved): `avail = (head -
tail) & (AI_RING_FRAMES - 1)` can alias if the true unconsumed gap exceeds
one full lap (65536 frames, ~1.49s at 44100Hz) - plausible during an `acvs`
restart since a voice host keeps rendering into the ring across the gap
where no consumer exists. Likely cause of audio-quality glitches, not
input-handling symptoms like dead pads, so not believed to explain this
incident.

## Shipped baseline: zero-voices-at-boot + on-demand start (2026-09-13)

Rather than block a usable setup on finding the root cause above, shipped
the workaround this design was already built for:

- `run_ForceAudioIn.sh` **only arms `forceAudioIn.so`** at boot, with zero
  voices ever attached at that point - proven safe across every repeated-
  restart test run against it, including a real physical reboot. It does
  not start any producer itself.
- A voice host (`injectTone`, or a real synth like `force-maze`'s
  `maze_host`) is started **only** on demand, via its own nodeServer
  Modules-page toggle - this spawns the process directly, with no
  `LD_PRELOAD`/`acvs` involvement at all, so it never re-triggers the race.
  Lazy re-attach (the always-on background thread) picks the new ring up
  within ~2s, no restart needed.
- **The hard rule this depends on**: once a voice has been started this
  way, do not restart `acvs` again until it's been stopped first (same
  toggle). Every live test of "`acvs` restart while a voice is attached"
  has failed, with the single accidental exception above - there is no
  known-safe way to do it on purpose yet.

Verified end-to-end on real hardware the same day: enabled persistently,
survived a real physical reboot (zero voices, pads/wifi fine), started via
the nodeServer toggle (lazy-attach confirmed via `/proc/<MPC-pid>/maps`, no
restart, pads/wifi fine, audio audibly playing), stopped via the same
toggle (clean `killall`, no restart, pads/wifi fine). This is the current
shipped baseline - continuing to chase the actual root cause is separate,
ongoing work, not a blocker for normal use under the hard rule above.

## Clock-rate mismatch (a producer-side concern, not this addon's)

The Force's real ALSA-clocked capture rate runs slightly faster than any
software timer's notion of elapsed time (~1000ppm measured on the one
device measured so far). This addon's ring is agnostic to a producer's
timing - it just drains whatever's there - but a producer that doesn't
compensate will see its own backlog drift. See `force-maze/DESIGN.md`'s
"Clock-rate mismatch" section for the fixes tried (hard latency ceiling,
hysteresis, fixed multiplicative correction) and the one that didn't work
(an adaptive controller tuned against a live signal contaminated by the
ring's own startup transient).

## Do not reach for SCHED_FIFO to fix timing-related glitches

See the `mockbamod-module-creator` skill's `gotchas.md` for the dedicated
case study. This looked like the obvious fix for glitching symptoms and
made things measurably worse - both the glitching itself and, separately, a
serious system-wide stability incident. Diagnose the ring/clock-rate layer
first.

## Diagnostics worth building in from the start

- **Live ring backlog** (`(head - tail) & mask`, converted to ms), not just
  cumulative produced/consumed counters - cumulative counters become
  actively misleading once trimming exists.
- **Trim event count and total frames trimmed** - confirms whether
  hysteresis is working as intended (rare trims) or thrashing (frequent
  ones).

Add this logging *before* the first live test, not after a symptom shows
up - reconstructing "was this ever a problem, and since when" without it
means re-running the same live experiment multiple times.

## Not yet built

- Root cause of the open pads/buttons incident above.
- A proper regression test that exercises the actual `acvs`-restart-while-
  attached failure mode, if one is ever found that's safe to automate.
