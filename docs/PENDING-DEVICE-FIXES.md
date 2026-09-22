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

## 3. Still open: the tap has never actually loaded on this device

Despite step 2's `manage.sh ENABLE`, `forceAudioJack.so` has lost the
pre-existing `LD_PRELOAD` boot-file race on **every** restart attempted so
far (5+ in a row on 2026-09-22) — confirmed via `/proc/<MPC-pid>/environ`
each time, even though `/dev/shm/.LD_PRELOAD` itself always has the correct
entry. This is the same race that already affected the old
`forceAudioIn.so`; nothing about the rebrand made it worse, but it's never
actually won on this device across the whole 2026-09-22 session.

**Root cause investigated**: `/media/662522/boot.sh` (the actual live boot
sequence — confirmed via `az01-launch-MPC`, NOT `boot_old.sh` despite an
earlier note in the mockbamod-module-creator skill's `gotchas.md` claiming
otherwise for this fork) backgrounds every `AddOns/*.sh` script with zero
synchronization, then does a single flat `sleep 1` before reading
`LD_PRELOAD` and exec'ing MPC. Neither `boot.sh` nor `boot_old.sh` actually
has the content-stability-polling fix that `gotchas.md` describes as
already applied — **that skill doc's claim doesn't hold for this device's
current files** and should be corrected (see the memory note below).

**A fix was written and briefly deployed** (poll `$mmLD_PRELOAD_VAR`'s
content for 4 consecutive unchanged reads, bounded to ~4s, replacing the
flat `sleep 1`) but was **reverted** after a severe, non-self-resolving
`acvs` crash loop (`cereal::RapidJSONException`, an MPC-internal
config-loading crash, unrelated in signature to the LD_PRELOAD race)
appeared shortly after deploying it. Correlation, not proven causation —
the same crash type had appeared and self-resolved earlier in the session
*before* this fix was ever deployed, and disk/settings-file integrity
checks at the time found nothing corrupted. Most likely explanation: the
sheer number of rapid restarts performed across the whole 2026-09-22
session (troubleshooting DrmVncServer, the rebrand deploy, then this race)
is what exposed a rare, pre-existing MPC-internal bug — not the `boot.sh`
edit itself. But this isn't proven, so the edit was reverted rather than
kept on an unproven correlation.

The written fix is preserved on-device as
`/media/662522/boot.sh.experimental-ldpreload-fix` for reference. **Do not
redeploy it without a calmer, more controlled test** (ideally: device
rested, no other testing happening that session, one restart, done) —
don't chain it with any other change or restart in the same sitting.

## Remaining checklist before force-audio-jack is "finished" / releasable

Nothing below has ever actually run on real hardware yet — do this as a
calm, dedicated session, not appended to other troubleshooting:

- [ ] Get `forceAudioJack.so` actually loaded (win the boot race — retry
      `manage.sh ENABLE` calmly, or reattempt the `boot.sh` fix above under
      controlled conditions). Confirm via `/proc/<MPC-pid>/environ`.
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
