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

## 3. ACTIVE, most important finding: loading forceAudioJack.so crashes MPC

**This supersedes the "boot race" framing below** — kept for the historical
trail, but the boot-timing angle was a red herring. Here's what's actually
true, most-confirmed-first:

**`forceAudioJack.so` loading into MPC is what triggers a 100% reproducible
crash** (`terminate called after throwing an instance of
'cereal::RapidJSONException'`, deep in MPC's own settings/project JSON
loading, before MPC's own startup banner even prints). Confirmed via
`/tmp/forceAudioJack.log`: every one of ~20 consecutive crashing PIDs in
one run shows `[forceAudioJack] loaded into pid N`. This isn't a rare
race — once the `.so` is actually loaded, the crash has followed every
single time observed so far, regardless of *how* it got loaded.

**Ruled out: constructor timing.** The library used to create its
background thread from `__attribute__((constructor))` (library-load time,
potentially concurrent with other libraries' own constructors/MPC's static
initializers). Moved that work into `ai_resolve()`, lazily triggered by
MPC's own first interposed ALSA call — guaranteed to run only after MPC's
own startup is complete. The exact same crash still happened. So it's not
about *when* relative to process startup; something else is wrong.

**Leading hypothesis, not yet tested**: the *original* `forceAudioIn.so`
(readi-only — no `snd_pcm_writei` interposition, no out-bus injection, no
skipback extraction) ran in production for weeks with no sign of this.
Today added the entire `writei` hook as genuinely new code. That's the
most likely place a real bug lives.

**Next step**: bisect by disabling the `writei` hook (pass straight
through, no mixing/extraction) and testing whether that loads cleanly. A
clean load pins the bug to the new out-bus/extraction code; a crash even
then rules out today's additions and points elsewhere.

**Current device state**: `manage.sh DISABLE`'d — `forceAudioJack.so` is
NOT in `LD_PRELOAD`. Safe, stable baseline. Don't re-enable without
checking this section's latest state first.

---

### Historical trail (superseded, kept for context)

A real, separate bug was found and fixed along the way:
`run_ForceAudioJack.sh`'s own retry loop never terminated (a variable-name
collision between its own counter and `lock_preload()`'s internal one),
leaving a permanently-running zombie process on every boot. Fixed by
renaming the colliding variable.

Also found and fixed: `run_ForceAudioJack.sh` used to strip its own
`LD_PRELOAD` entry on every boot-triggered `kill` (not just DISABLE),
re-entering the write race from scratch every single restart, unlike
`mockbaMagic`/`MidiLoop`'s own idempotent pattern (write only if not
already present). Fixed to match their pattern.

Three separate `boot.sh` read-side timing fixes were tried and reverted
after each correlated with the same crash loop — but that correlation
turned out to be because each one *happened to get the `.so` loaded*, not
because of anything about `boot.sh`'s own timing. Once the
idempotent-persistence fix above got the `.so` loading reliably *without*
touching `boot.sh` at all, the same crash still occurred every time,
which is what revealed the real trigger. **`boot.sh` itself was very
likely never the problem** — don't avoid editing it based on the earlier
"4-for-4 correlation" framing without re-reading this section first.

## Remaining checklist before force-audio-jack is "finished" / releasable

Nothing below has ever actually run on real hardware yet — do this as a
calm, dedicated session, not appended to other troubleshooting:

- [ ] **Blocking everything else**: root-cause and fix the
      `cereal::RapidJSONException` crash from section 3 above. Bisect the
      `writei` hook first (see "Next step").
- [ ] Once loading is crash-free: confirm `forceAudioJack.so` stays loaded
      across a normal restart. Confirm via `/proc/<MPC-pid>/environ`.
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
