# force-audio-jack — Technical Design & Architecture

This document explains **how** force-audio-jack works, for anyone
maintaining it, porting it to a different device/firmware revision, or
building a new voice-producing add-on that injects audio through it.
For installation and everyday use, see [README.md](README.md).

**Scope note**: this document describes the original, stable In-bus
(Audio-In 1/2) injection tap — everything below is shipped and verified
on real hardware. The newer Out-bus (physical Out 3/4) injection and
Skipback features are documented separately in
[docs/PROPOSAL-force-audio-jack.md](docs/PROPOSAL-force-audio-jack.md),
since they're unit-tested but not yet verified on real hardware.

## Contents

- [Why this needs LD_PRELOAD at all](#why-this-needs-ld_preload-at-all)
- [This is LD_PRELOAD, but not the risky kind](#this-is-ld_preload-but-not-the-risky-kind)
- [Symbol export scope — the sharp edge of interposition](#symbol-export-scope--the-sharp-edge-of-interposition)
- [Architecture: producer process + shared-memory ring + interposed shim](#architecture-producer-process--shared-memory-ring--interposed-shim)
- [Multi-voice mixing](#multi-voice-mixing)
- [Ring re-attach: handling a voice host that restarts](#ring-re-attach-handling-a-voice-host-that-restarts)
- [Boot sequence & the operational safety rule](#boot-sequence--the-operational-safety-rule)
- [The shared boot-time LD_PRELOAD file race (already mitigated)](#the-shared-boot-time-ld_preload-file-race-already-mitigated)
- [Latency management](#latency-management)
- [Clock-rate mismatch (a producer-side concern)](#clock-rate-mismatch-a-producer-side-concern)
- [Diagnostics](#diagnostics)
- [Known limitations](#known-limitations)
- [Building a new voice producer](#building-a-new-voice-producer)

---

## Why this needs LD_PRELOAD at all

Confirmed live via `/proc/<mpc-pid>/fd`: the Force's main app
(`/usr/bin/MPC`, a JUCE binary) opens its audio codec with raw `hw:`
device names and holds both playback and capture **exclusively** —
continuously, independent of whether anything is actually record-armed.
There is no JACK server running on the device (a bundled `libjack`
some add-ons link against is not the same thing as a running server)
and no ALSA loopback module (`snd-aloop`) built into this kernel. So
there is no host-level audio bus, loopback device, or software mixer
to hook into from outside the process — the only seam available is
interposing the ALSA calls MPC itself makes, from inside its own
process.

## This is LD_PRELOAD, but not the risky kind

Not all `LD_PRELOAD` use carries the same risk. The
mockbamod-module-creator skill's own gotchas reference ranks
"`LD_PRELOAD` injection into the main app process" as highest-risk,
citing `mockbaMagic` as the example — **raw in-memory binary patching
at addresses from a table keyed to an exact firmware version**.

force-audio-jack uses a meaningfully different, lower-risk technique:
**symbol interposition** against `libasound`'s stable public ABI.

```c
static ssize_t (*orig_readi)(snd_pcm_t*, void*, snd_pcm_uframes_t);
orig_readi = dlsym(RTLD_NEXT, "snd_pcm_readi");
```

This hooks by *symbol name* against a public library's documented ABI,
not raw offsets inside the closed MPC binary — no dependency on the
exact firmware version the way `mockbaMagic`'s address table has — and
it fails closed by construction: on any error, the real ALSA function
runs untouched.

**[force-link-audio](https://github.com/macdigi/force-link-audio)**'s
`forceStream.so`, by [macdigi](https://github.com/macdigi), is the
mirror-image add-on: it taps `snd_pcm_writei` to *extract* what MPC
plays, instead of *injecting* into what it captures. Its interposition
approach was the reference point this add-on's own design started
from.

Real-time-safe by design: no allocation, syscalls, or logging on the
hot path (the interposed call itself). Setup happens exactly once, in
`ai_resolve()`, triggered lazily by MPC's own first interposed ALSA call
— never on the audio thread's steady-state path. It deliberately does
*not* run from an `__attribute__((constructor))`: a constructor runs at
library-load time, concurrently with other libraries' constructors and
JUCE's static initializers, which is a worse neighbourhood to be doing
setup work in.

## Symbol export scope — the sharp edge of interposition

Symbol interposition is the low-risk technique, but it has one genuinely
dangerous edge, and this project fell straight off it: **a preloaded
library must export only the symbols it actually intends to interpose.**

Every exported symbol in an `LD_PRELOAD`'d library is an interposition,
whether you meant it or not. glibc's dynamic linker resolves to the
first definition it finds in the global scope, and a preloaded object
sits at the front of that scope. It does **not** honour the weak/strong
distinction — that's a static-linker concept. So a stray exported
`memcpy` is not a harmless duplicate; it silently becomes *the*
`memcpy` for the entire process.

This bit us hard. `zig cc` statically links its own compiler-rt and libm
into every `-shared` output, and nothing in the build hid them. The tap
exported **389** symbols while intending to export 4 — the extras
included `memcpy`, `memmove`, `memcmp`, `memset`, all of libm, the
`__aeabi_*` helpers, and `__stack_chk_guard` (the stack canary, as a
*data* symbol). Result: every `memcpy`/`memcmp`/math call in the whole
MPC process — MPC itself, libstdc++, RapidJSON, cereal, JUCE,
libfreetype — was redirected into our library. MPC aborted with
`cereal::RapidJSONException` on every single boot the tap successfully
loaded, because RapidJSON is heavily `memcpy`/`memcmp`-driven and was
being handed subtly different implementations than glibc's tuned ARM
ones.

**The guard**: `scripts/forceAudioJack.map` is a linker version script
listing the four `snd_pcm_*` entry points as `global` and everything
else as `local`, applied via `-Wl,--version-script`. `scripts/build.sh`
additionally *fails the build* if the exported set is ever anything but
those four, so this cannot silently regress. Hiding the dead weight also
took the `.so` from 174KB to 17KB.

**How to verify any interposer, in one command** — preload it onto an
unrelated binary and watch the linker's own bindings:

```sh
readelf --dyn-syms -W lib.so | awk '$7!="UND" && ($5=="GLOBAL"||$5=="WEAK")'
LD_DEBUG=bindings LD_PRELOAD=./lib.so /bin/true 2>&1 | grep 'normal symbol'
```

If anything you didn't intend shows up bound into your library, that's a
process-wide hijack waiting to happen. This test needs no device
restart, no MPC involvement, and no core dumps. For reference, healthy
interposers on this device export very little: `force_shadow.so` 11
symbols, `MidiLoop`'s `tkgl_anyctrl_lt.so` 39, and the original
In-bus-only `forceAudioIn.so` just 3.

**Two debugging lessons worth more than the fix itself:**

- **"Absent from the stack at crash time" does not mean "not causal."** A
  core dump was read as exonerating this library: all 17 threads' stacks
  were scanned for addresses inside its mapped range, and none were
  found. But `memcpy` and friends are *leaf* functions — they corrupt a
  buffer, return, and are long gone from the stack before the resulting
  bad JSON is detected and `abort()` fires. The measurement was correct;
  the conclusion drawn from it was too strong.
- **When several careful bisections along one axis all come back
  negative, suspect the axis.** Neutering the `writei` hook to a pure
  passthrough, moving setup out of the constructor, and shrinking the
  event-trace ring all failed to help — because all three changed the C
  logic and left the exported symbol table byte-for-byte identical. The
  bug was in the link line, which no amount of source bisection could
  reach.

## Architecture: producer process + shared-memory ring + interposed shim

```
your synth/generator process (any language, e.g. maze_host)
        │  renders audio, writes into a POSIX shared-memory ring
        ▼
forceAudioJack.so (LD_PRELOAD'd into /usr/bin/MPC)
        │  interposes snd_pcm_readi by symbol name (dlsym(RTLD_NEXT, ...))
        │  mixes (sums, never replaces) up to 4 simultaneous voice rings
        │  into whatever real hardware audio MPC reads
        ▼
Audio-In track on the Force
```

`forceAudioInject.h` is the reusable shared-memory ring layout (magic,
sample rate, channel count, head/tail indices, the sample buffer) —
any producer add-on vendors an exact copy of this header rather than
inventing its own; it's the ABI contract between producer and tap, and
any copy must stay byte-for-byte identical to this repo's. It handles
the SPSC (single-producer/single-consumer) atomics correctly for one
producer process and one consumer thread inside MPC.

The producer always writes 32-bit float, mono or stereo, at a fixed
rate it declares in the ring's own header — `forceAudioJack.so` does the
conversion to whatever format/channel count MPC actually configured on
the real capture handle, so the producer never needs to know or care
what MPC is doing.

**Additive mixing, not replacement.** The tap adds ring samples to
whatever real hardware audio is already there, so a real instrument
plugged into the physical input keeps working unmodified alongside the
injected signal.

`injectTone.c` is a minimal stand-in producer (a fixed sine wave),
started on demand from the nodeServer Modules page — useful to prove
the injection path works before wiring up a real DSP engine. A real
voice host (like `maze_host`) replaces it, writing rendered audio into
its own slot's ring instead.

## Multi-voice mixing

`forceAudioJack.so` mixes up to `AI_MAX_VOICES` (4) independent voice
hosts at once, each in its own named shared-memory ring
(`/forceAudioInject0`, `/forceAudioInject1`, ...). Every ring stays
genuinely single-producer/single-consumer — one voice host writes its
own ring; `forceAudioJack.so` is the sole reader of all of them — so this
scales without adding any cross-process synchronization beyond what
already exists per ring.

**Deliberately no central "mixer" control panel.** Each ring's
`enabled`/`gain`/`channel_mask` fields are the on/off, volume, and
L/R/L+R routing for *that* voice, written directly by that voice's own
control path (e.g. Maze Voice's own `SET mix.gain`). A separate
coordinating mixer page would need its own IPC into each voice's
socket for no real benefit.

**Channel select is L / R / L+R, not an arbitrary voice-count.** The
tapped capture handle is confirmed 2-channel — that's the actual
hardware ceiling, not a software choice. Two voices routed to the same
channel simply sum there, the same as two synths sharing one mixer
channel.

**Mute happens at the consumer, never by pausing the producer.** A
voice's own render cadence is its own synth's clock; `enabled=0` only
skips adding that voice's samples into the output — the ring is still
drained at the normal rate, so a re-enabled voice resumes from live
backlog, not a stale one.

**`LD_PRELOAD` is armed once, not per voice.** `forceAudioJack.so` itself
needs loading into MPC exactly once — it already attaches to every
slot that has a ring present. A second or third simultaneous voice
add-on just needs a distinct `--slot`; its own startup script must
never touch the shared `LD_PRELOAD` environment file itself (see
[The shared boot-time LD_PRELOAD file race](#the-shared-boot-time-ld_preload-file-race-already-mitigated)
below) — this add-on is the only one that should ever write its own
entry there.

Confirmed live on real hardware with two simultaneous voices attached
and audibly mixed together correctly (`maze_host` on one slot,
`injectTone` on another, routed to different channels).

## Ring re-attach: handling a voice host that restarts

A voice host that's stopped and later restarted (toggled off/on via
the Modules page) may very plausibly `shm_unlink()` and recreate its
ring from scratch on every start ("start clean") — a **new inode under
the same name**. `forceAudioJack.c`'s attach logic doesn't treat "already
attached" as permanent: it `stat()`s the ring's path (cheap, off the
hot path, from the same background thread that does lazy re-attach)
and compares inodes, re-attaching if the segment was replaced.

Without this, a stopped-then-restarted voice would silently mix from
an orphaned, no-longer-written-to ring — no crash, just silence, a
confusing failure mode to debug blind since everything *looks*
running.

The old mapping is deliberately never `munmap`'d — the audio thread's
hot path may be mid-read of it via its own lock-free pointer load at
the exact moment a background thread would want to swap it. Leaking
one ring's worth of memory (~512 KB) per voice-restart is a bounded,
deliberate trade-off against ever risking a use-after-unmap there.

Verified with unit tests against a real POSIX shm segment
(`tests/test_mix.c`, 28/28 checks): stays unattached when absent,
attaches once present, doesn't double-count on repeat calls, the hot
path picks up a late attach with zero call-site changes, and re-attach
correctly picks up a replaced segment's new data.

**Lazy re-attach** (the same background thread, on a roughly 2-second
cadence) means a voice host started *after* MPC comes up — the normal
case, since voices are always started via the Modules page rather than
at boot — is picked up live, with no restart of any kind needed.

## Boot sequence & the operational safety rule

A voice host's producer process should be started **only** on demand,
via its own NSMODULE.json/nodeServer Modules-page toggle — never by a
voice add-on's own boot script.

- `run_ForceAudioJack.sh` **only arms `forceAudioJack.so`** at boot, with
  zero voices ever attached at that point — proven safe across every
  repeated-restart test run against it, including a real physical
  reboot. It does not start any producer itself.
- A voice host is started **only** on demand, via the Modules page —
  this spawns the process directly, with no `LD_PRELOAD`/`acvs`
  involvement at all, so it never touches the restart path. Lazy
  re-attach picks the new ring up within about 2 seconds, no restart
  needed.
- **The hard rule this depends on**: once a voice has been started
  this way, don't restart `acvs` again until it's been stopped first
  (same toggle). Extensive live testing found that restarting `acvs`
  while *any* voice ring is attached can reliably kill pads/buttons
  (occasionally Wi-Fi) — with no known-safe way to do it on purpose
  yet. `forceAudioJack.so` armed with zero voices, by contrast, has never
  failed a single test. See
  [Known limitations](#known-limitations) below for the investigation
  into the root cause.

Verified end-to-end on real hardware: enabled persistently, survived a
real physical reboot (zero voices, pads/Wi-Fi fine), started via the
Modules toggle (lazy-attach confirmed via `/proc/<mpc-pid>/maps`, no
restart, pads/Wi-Fi fine, audio audibly playing), stopped via the same
toggle (clean kill, no restart, pads/Wi-Fi fine). This is the current
shipped baseline.

## The shared boot-time LD_PRELOAD file race (already mitigated)

A separate, unrelated race exists in how MockbaMod's own boot sequence
populates `/dev/shm/.LD_PRELOAD` (the file `apps.sh` reads into
`LD_PRELOAD` before exec'ing MPC): several add-ons' own startup scripts
(this one, `mockbaMagic`'s, `MidiLoop`'s) each read the file, check
their own library isn't already listed, and write the whole file back
— with no locking. MockbaMod's own boot sequence backgrounds every
top-level add-on script concurrently, so three or more unsynchronized
read-modify-write scripts racing on one shared file at boot is a
textbook lost-update: whichever write lands last wins, silently
dropping another script's entry.

This bug is pre-existing in MockbaMod itself, not introduced by this
add-on. **Mitigation applied here**: `run_ForceAudioJack.sh` and
`addon/manage.sh` wrap every read-modify-write of the shared
`LD_PRELOAD` file in an `mkdir`-based mutex (atomic even on BusyBox;
bounded retry, fails open rather than risking a hung boot on a stale
lock). This add-on's own locking is defensive regardless of whether
the same fix has also been applied to other add-ons' own scripts on a
given device/fork.

**Second, more important mitigation — don't enter the race at all.**
MockbaMod's boot sequence calls every add-on script with `kill` on every
`acvs` restart, not just on DISABLE/UNINSTALL. `ForceShadow`'s pattern
(which this add-on originally copied) strips its own `LD_PRELOAD` entry
on every such `kill` and unconditionally rewrites it on load — which
re-enters the boot write race from scratch on every single restart,
forever. `mockbaMagic` and `MidiLoop` instead never touch the file on
`kill`, and on load write only if their own entry isn't already present;
once their entry lands it simply persists. `run_ForceAudioJack.sh` now
follows that idempotent pattern, so `kill` only stops
`injectTone`/`skipbackHost` processes and never rewrites the shared file.
Removal is still handled independently by `manage.sh`'s own `STOP()`, so
DISABLE/UNINSTALL are unaffected. This is what got the tap loading
reliably in the first place — which is what finally exposed the real
crash bug described in
[Symbol export scope](#symbol-export-scope--the-sharp-edge-of-interposition).

## Latency management

Bounding backlog needs **hysteresis, not a hard ceiling**. A
persistent, tiny clock-rate mismatch between a producer's own
wall-clock render timer and this consumer's real ALSA-clocked read
rate means backlog is always drifting toward *some* threshold —
trimming right at that threshold on every call fires almost
continuously once backlog reaches it, and each trim is a small phase
discontinuity (an audible click). A train of those, many times a
second, sounds like a fast, continuous glitching artifact.

Instead: trim only once backlog exceeds a **trigger** threshold
(~200 ms) and drop it all the way down to a lower **target** (~100 ms)
when it does — one bigger, much rarer correction instead of many small
ones. These specific values were tuned from live testing: backlog was
observed to wander in *both* directions over longer sessions (not
drift one-way, as first assumed), with underruns climbing when the
cushion was too small (an earlier, tighter 25 ms/100 ms target/trigger
pair). ~100/200 ms still reads as responsive on a played note while
remaining far below the original, much larger full-ring latency this
mechanism replaced.

## Clock-rate mismatch (a producer-side concern)

The Force's real ALSA-clocked capture rate runs slightly faster than
any software timer's notion of elapsed time (on the order of ~1000 ppm
measured). This add-on's ring is agnostic to a producer's own timing —
it just drains whatever's there — but a producer that doesn't
compensate for this will see its own backlog drift over time. See
[force-maze](https://github.com/sd88me/force-maze)'s own `DESIGN.md`
for the fixes tried on the producer side (a hard latency ceiling,
hysteresis, a fixed multiplicative correction) and the one that didn't
work (an adaptive controller tuned against a live signal contaminated
by the ring's own startup transient).

## Diagnostics

Runtime logging is written to `/tmp/forceAudioJack.log` on request, via
a plain marker file (`/tmp/forceAudioJack.diag` — checked because there's
no practical way to set an environment variable in MPC's own exec
environment, since this library is loaded by a boot script rather than
launched directly). When present, a background thread periodically
logs per-voice backlog, gain, routing, and underrun counts.

For deeper investigation, a lightweight **in-process event trace** is
built directly into `forceAudioJack.c`: a fixed-size ring of 65,536 tiny
timestamped event records (constructor start, every attach/re-attach,
every `snd_pcm_readi` call on the tapped handle, every per-voice mix,
every trim and underrun), written with a single atomic increment and a
`clock_gettime` call — no locks, no syscalls beyond the clock read, on
the hot path. Touching `/tmp/forceAudioJack.dumpreq` makes the background
thread dump the ring, in chronological order, to
`/tmp/forceAudioJack.dump.<pid>`. Since MPC does not crash when pads go
unresponsive — it stays running, just unresponsive — a dump can be
requested well after a failure is physically confirmed, with nothing
needing to be caught in flight.

This mechanism exists specifically because external tracing
(`strace`-based approaches, in several different forms) was found
during investigation to reliably avoid reproducing the pads-death
issue described below — the leading theory being that `ptrace`'s own
overhead is enough to perturb a narrow timing window and mask the
underlying race. An in-process trace adds only a few CPU cycles per
call, rather than a full `ptrace` trap per syscall, and was built as a
lower-overhead alternative once several tracer-based attempts had
already ruled themselves out as valid measurement tools for this
specific issue.

### Core dumps when MPC actually crashes

The Force has a real, undocumented Akai coredump facility at
`/usr/bin/az01-coredump` (found via `strings`). Create
`/data/coredumps.enabled` and each crash writes a
`.core.zst` + `.log.zst` + `.metadata` triple to `/data/coredumps/`. The
`.metadata` alone is often enough — it names the crashing thread, signal,
and MPC version. It is left enabled on the device; clean the directory
out periodically, as each capture is 50-150MB.

Practical notes, learned the hard way:

- Dumps rotate fast during a crash loop (every ~13s), so a `ls` followed
  by an `scp` can race and fail. Snapshot to a stable directory in one
  `ssh` command first, then copy from there.
- `zstd` is not available on the dev machine but *is* on the device —
  decompress there and copy the plain file back, which works fine even
  for a 150MB core.
- No `gdb` or `pyelftools` needed to read one: `readelf -n` gives the
  `NT_FILE` mapping table, and the ARM `NT_PRSTATUS` notes can be
  unpacked with plain `struct.unpack` — `pr_reg` is 18 words at byte
  offset 72, with `sp`/`lr`/`pc` at indices 13/14/15.
- Read what a dump says narrowly. Mapping a library into the process
  proves only that it was loaded; finding no trace of it on any stack
  proves only that it wasn't executing *at abort time*. Neither settles
  causation for a corrupt-then-crash-later bug. See
  [Symbol export scope](#symbol-export-scope--the-sharp-edge-of-interposition).

### An `acvs` crash loop is not necessarily this add-on

Two distinct non-force-audio-jack causes have each produced an
indefinite `acvs` restart loop on this device, both of which survive
physical power-cycles and look identical from the outside ("the Force
won't boot"):

- **DrmVncServer winning a race for `/dev/dri/card0`** against MPC's own
  display init. Tell-tale: `Failed to initialise display (another
  process running?), aborting!` in `journalctl -u acvs`. Root cause was
  `MidiLoop`'s `SHIFT+SCENE-8` shortcut calling DrmVncServer's
  `manage.sh ENABLE`, which sets a *persistent* auto-launch-at-boot flag
  rather than just toggling the process for that session; that script
  has been rewritten to start/kill the process directly.
- **`connmand` abort loops**, which present as Wi-Fi simply never coming
  up rather than as an MPC problem.

So check `journalctl -u acvs` for the actual signature before assuming
the audio tap is involved — and equally, don't reflexively exonerate it
either, which is the mistake that cost this project several days.

## Known limitations

- **The pads/buttons-dead-on-restart issue is not fully root-caused.**
  Restarting `acvs` while a voice is attached can reliably make
  pads/buttons unresponsive (occasionally Wi-Fi); this has never
  happened with zero voices attached. Investigation to date has ruled
  out, via direct evidence: a symbol collision with `MidiLoop`'s own
  interposer (confirmed disjoint symbol tables — though note that check
  looked at the wrong counterparty, since the collision that did later
  turn out to matter was with **libc/libm itself**, see
  [Symbol export scope](#symbol-export-scope--the-sharp-edge-of-interposition));
  `mockbaMagic`'s
  address-patching mechanism (confirmed dormant/inert on the device
  tested, via disassembly of its actual constructors); the diagnostics
  thread's mere existence; and the per-sample mixing/write loop
  specifically (still failed with a voice attached but muted, i.e.
  that inner loop skipped). What remains unseparated: the ring
  bookkeeping/atomics/backlog-trim path that runs on every read
  regardless of mute state, versus something specific to a real voice
  host's own process behaviour (its own threads/MIDI client) that a
  minimal single-threaded test producer wouldn't exhibit — the
  highest-value untested variable. Evidence to date is consistent with
  a genuine, narrow race condition rather than a fixed logical bug (an
  accidental extra-scheduling-load pass survived once out of many
  otherwise-consistent failures). **Operational mitigation**: the hard
  rule in [Boot sequence & the operational safety rule](#boot-sequence--the-operational-safety-rule)
  — never restart `acvs` while a voice is attached — is fully
  sufficient for safe normal use; it is not a workaround pending a fix,
  it is the current permanent operating model.
- **A confirmed ring-backlog aliasing bug can cause audio glitches**
  (not related to the pads/buttons issue above). The backlog
  calculation (`avail = (head - tail) & (AI_RING_FRAMES - 1)`) can
  alias if the true unconsumed gap exceeds one full ring lap (65,536
  frames, ~1.49 s at 44.1 kHz) — plausible during an `acvs` restart,
  since a voice host keeps rendering into its ring across the gap
  where no consumer exists to drain it. This has been directly
  observed (a high underrun rate immediately following a restart gap
  with a producer still rendering) and is not yet fixed.
- **`SCHED_FIFO` is not a valid fix for timing-related glitches in this
  system.** It was tried as an apparently obvious fix for glitching
  symptoms and made things measurably worse — both the glitching itself
  and, separately, a serious system-wide stability issue. Diagnose the
  ring/clock-rate layer first; see the mockbamod-module-creator skill's
  own gotchas reference for the underlying case study.
- **A proper automated regression test for the `acvs`-restart-while-
  attached failure mode does not yet exist**, pending a safe way to
  reproduce it on demand.
- **The audio paths have not yet been confirmed by ear on real
  hardware.** As of 2026-09-23 the tap is verified to load and stay
  loaded across app restarts and cold reboots, with its interposed
  `snd_pcm_hw_params` hook firing (`44100 Hz`, 4-out/2-in) — but nothing
  below has been heard yet, because until the symbol-scope bug was fixed
  the tap could never stay loaded long enough to try:
  - In-bus (`injectTone --bus in`) still behaving as it did before the
    rebrand, on an Audio-In track.
  - Out-bus (`injectTone --bus out`) actually reaching the physical
    Out 3/4 jacks.
  - Skipback end-to-end: a WAV landing in
    `Force Documents/Samples/Skipback/` with the right project/tempo in
    its name.
  - Open Question #1 from `docs/PROPOSAL-force-audio-jack.md`: whether
    restarting `acvs` with an **Out-bus or Skipback** ring attached kills
    pads/buttons the way it does for In-bus rings. Test deliberately,
    expecting to have to recover.
- **The Skipback trigger combo is unsettled.** `SHIFT+RECORD` (the
  original design) is not a valid MidiLoop combo — SHIFT's combo set
  doesn't include RECORD — and was removed; `SELECT+RECORD` was ruled out
  too. It is currently wired to `KNOBS+SCENE-1` as a **temporary test
  binding**, which overrides ForceShadow's `SCRIPT-19` DX7-page
  placeholder (a genuine no-op, so low risk, but not free). A permanent
  combo needs deciding, and ForceShadow's binding restored or
  deliberately reassigned.

## Building a new voice producer

Any process, in any language or toolchain, can inject audio through
this tap:

1. Vendor an exact, byte-for-byte copy of `forceAudioInject.h` — it's
   the ABI contract between your producer and the tap; don't
   reimplement it independently.
2. Create your own slot's shared-memory ring (`/forceAudioInject<N>`,
   `N` in `0..AI_MAX_VOICES-1`), write your fixed sample rate and
   channel count into the ring's header once, and render interleaved
   32-bit float audio into it continuously.
3. Expose a control socket that lets you (or a UI) write your own
   ring's `enabled`/`gain`/`channel_mask` fields directly — there is no
   central mixer to register with.
4. Start your producer process only via its own NSMODULE.json/Modules
   page entry — never from your own add-on's boot script — and never
   restart `acvs` while it's running (see
   [The hard rule](README.md#the-hard-rule)).
5. If you restart your own process (e.g. a Modules-page toggle
   off/on), it's safe to `shm_unlink()` and recreate your ring from
   scratch on each start — the tap detects the new inode and
   re-attaches automatically within about 2 seconds, with no
   coordination needed on your side beyond using the same ring name.
