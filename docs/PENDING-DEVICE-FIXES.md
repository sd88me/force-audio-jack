# Pending device work — status as of 2026-09-22

Both items below that were previously "pending" are now **applied and live**
on the device. What's left is the actual hardware verification before this
can be called finished/released — see the checklist at the bottom.

## 1. DrmVncServer autolaunch-via-shortcut bug — ✅ applied 2026-09-22

`SHIFT+SCENE-8` (`SCRIPT-2` in `MidiLoop/USER-SCRIPTS.sh`) was calling
`DrmVncServer/manage.sh ENABLE`/`DISABLE`, which sets a *persistent*
auto-launch-at-boot flag rather than just toggling the process for that
session. That's what put DrmVncServer in a boot-time race against MPC's own
display init for `/dev/dri/card0`, causing an indefinite `acvs` crash loop
(`Failed to initialise display (another process running?), aborting!`) that
survived physical power-cycles, twice (2026-09-21 and 2026-09-22 — it had
been re-triggered by pressing the shortcut again between sessions).

Fixed: `SCRIPT-2` now starts/kills the `drmvncserver` process directly
(same pattern as `SCRIPT-3`/`4`/`5`), never touching the persistent
auto-launch flag. Backed up first (`USER-SCRIPTS.sh.bak-drmvnc-fix-*`).
DrmVncServer's auto-launch is currently OFF.

## 2. Rebrand deploy — ✅ applied 2026-09-22

`AddOns/ForceAudioIn` removed; `AddOns/ForceAudioJack` and
`AddOns/ForceAudioJackSkipback` deployed and checksum-verified against the
local repo. `manage.sh ENABLE` run once (arms `forceAudioJack.so`, cleans up
any stale pre-rename `LD_PRELOAD` entry automatically).

## 3. RESOLVED: the cereal::RapidJSONException crash is NOT caused by this addon

Long investigation, short answer: **`forceAudioJack.so` does not cause
this crash.** Confirmed via an actual core-dump analysis, not just
behavioral correlation — see "How this was actually settled" below. The
crash is a pre-existing MockbaMod/MPC-environment issue unrelated to
force-audio-jack, and it's safe to have the tap enabled.

Two real, unrelated bugs were found and fixed along the way (both worth
keeping regardless of the crash mystery):

- **`forceAudioJack.c`**: the library's `__attribute__((constructor))`
  used to create a background thread at library-load time, potentially
  concurrent with other libraries' own constructors. Moved that work into
  `ai_resolve()`, lazily triggered by MPC's own first interposed ALSA
  call — safer regardless of the crash investigation's outcome.
- **`run_ForceAudioJack.sh`**: used to strip its own `LD_PRELOAD` entry on
  every boot-triggered `kill` (not just DISABLE), re-entering the boot's
  write race from scratch every single restart, unlike `mockbaMagic`/
  `MidiLoop`'s own idempotent pattern (write only if not already present).
  Fixed to match their pattern — this is what got the `.so` loading
  reliably for the first time, which is what let the crash investigation
  actually happen.

**How this was actually settled**: three separate `boot.sh` timing edits,
and later a `writei`-hook bisection and an event-trace-ring-size
bisection, all correlated with the same crash — but a live `/proc/maps`
poll during one crash episode showed `forceAudioJack.so` wasn't even
loaded in the crashing processes, undermining the whole premise. Settled
it properly: enabled `/data/coredumps.enabled` (a real Akai coredump
facility at `/usr/bin/az01-coredump`, undocumented but found via `strings`
on the binary — writes `.core.zst`/`.log.zst`/`.metadata` to
`/data/coredumps/`), pulled a genuine core file, and parsed it directly
with Python (`readelf -n` for the mapping table, manual `struct.unpack`
of the ARM `NT_PRSTATUS` notes — no `gdb`/`pyelftools` needed). Confirmed
`forceAudioJack.so` WAS mapped into the crashing process, then scanned
all 17 threads' stacks (64KB each) for any address inside its range —
**zero hits, across every thread**. The crashing thread's PC/LR were both
in `libc`'s own `abort()` (expected), and its stack showed real references
to `libc`/`libstdc++`/`libfreetype`/`ld-linux`, and notably `MidiLoop`'s
own `tkgl_anyctrl_lt.so` — but never `forceAudioJack.so`. The library was
loaded but never part of the actual call chain. The day's "loading causes
the crash" theory was a coincidence: both loading successfully and the
pre-existing crash flaring up are likely downstream of the same thing
(the day's extraordinary restart count), not one causing the other.

**Don't reflexively revert this addon's own changes** if this crash
recurs — check `/proc/<pid>/maps` or pull a core dump first to confirm
`forceAudioJack.so` is actually involved before assuming causation.
`tkgl_anyctrl_lt.so`'s appearance on the crashing stack is a real lead for
whoever chases the actual root cause, but that's `MidiLoop`'s own code,
out of this project's scope. `/data/coredumps.enabled` was left in place
on the device (harmless, useful for that future investigation) - the
`/data/coredumps/` directory should be cleaned up periodically (each
capture is 50-150MB).

## Remaining checklist before force-audio-jack is "finished" / releasable

Nothing below has ever actually run on real hardware yet — do this as a
calm, dedicated session:

- [ ] Re-enable (`manage.sh ENABLE`) and confirm `forceAudioJack.so` loads
      and stays loaded across a normal restart. Confirm via
      `/proc/<MPC-pid>/environ`.
- [ ] Confirm the original In-bus path still works exactly as before the
      rebrand: `injectTone --bus in`, hear/confirm it on an Audio-In track.
- [ ] Out-bus: `injectTone --bus out`, confirm audio actually reaches the
      physical Out 3/4 jacks (needs ears on the real output, not just logs).
- [ ] Skipback: start `skipbackHost`, let it run a bit, trigger
      `SHIFT+RECORD`, confirm a WAV lands in
      `Force Documents/Samples/Skipback/` named with the right project/tempo.
- [ ] The still-open safety question from `docs/PROPOSAL-force-audio-jack.md`
      Open Question #1: does restarting `acvs` while an Out-bus or Skipback
      ring is attached kill pads/buttons the same way it does for the
      existing In-bus rings? (Only test this deliberately, expecting to
      have to recover from it if so — don't restart with anything attached
      during normal use until this is answered.)
- [ ] Only after all of the above: tag a GitHub release.
