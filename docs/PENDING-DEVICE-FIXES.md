# Pending fixes — apply next time the Force is reachable

Nothing here has been applied yet. Each item is a self-contained, ready-to-run
change; apply and check off, don't batch them with anything else risky.

## 1. DrmVncServer autolaunch-via-shortcut bug (confirmed root cause, 2026-09-21)

**Symptom that led here**: `acvs` crash-looped indefinitely with `Failed to
initialise display (another process running?), aborting!` (MPC's own error,
not DrmVncServer's) — DrmVncServer was winning a boot-time race for
`/dev/dri/card0` against MPC's own display init, on every single restart,
surviving physical power-cycles.

**Root cause, confirmed**: `SHIFT+SCENE-8` is bound to `SCRIPT-2` in
`MidiLoop/USER-SCRIPTS.sh`, which currently calls
`DrmVncServer/manage.sh ENABLE` — and `manage.sh ENABLE`'s own output says
*"DrmVncServer is now running and will auto-start on every boot."* So a
shortcut meant to toggle VNC mirroring **for the current session** was
actually flipping a **persistent auto-launch-at-boot flag**, which is disk
state (survives power cycles, unlike a plain process).

**Fix**: replace `SCRIPT-2`'s body in
`/media/662522/AddOns/MidiLoop/USER-SCRIPTS.sh` with a session-only
start/kill of the process, matching the pattern every other lightweight
toggle in that same file already uses (`SCRIPT-3`/`4`/`5` for
Harpie/RiffMaker/Euclidier) instead of calling `manage.sh`:

```sh
if [ "$ID" = "SCRIPT-2" ]; then #SCRIPT-2
    #Toggle DrmVncServer for THIS SESSION ONLY - starts/kills the process
    #directly rather than manage.sh ENABLE/DISABLE, which sets persistent
    #auto-launch-at-boot state. That persistence is what put DrmVncServer
    #in a boot-time race against MPC's own display init for /dev/dri/card0
    #on every subsequent restart (2026-09-21 incident).
    app=drmvncserver
    if $(ISRUNNING $app); then
        killall $app
    else
        nohup /media/662522/AddOns/DrmVncServer/$app -f /dev/dri/card0 -t /dev/input/event0 -k /dev/input/ -r 90 -F 0 &
    fi
    exit 0
fi
```

The flags (`-f /dev/dri/card0 -t /dev/input/event0 -k /dev/input/ -r 90 -F 0`)
are the real invocation `manage.sh ENABLE` uses — captured live from `ps`
during today's incident — so the shortcut's behavior is otherwise unchanged.

**Steps**:
1. Back up `USER-SCRIPTS.sh` first (`cp` with a `.bak-<timestamp>` suffix,
   same convention as every other edit this project has made to that file).
2. Replace the `SCRIPT-2` block with the version above.
3. Confirm DrmVncServer's persistent auto-launch is currently OFF (it was
   explicitly `manage.sh DISABLE`'d during the 2026-09-21 incident) — check
   there's no top-level `AddOns/run_DrmVncServer.sh`-style launcher file.
4. No restart needed to apply this specific file edit (`USER-SCRIPTS.sh` is
   read by `midiloop_script.sh` at call time, not cached) — but the next time
   `acvs` *does* restart for any other reason, watch it settle fully before
   moving on, per the restart-pacing note from today.

## 2. Deploy the rebrand + re-arm the tap

The device currently still has the **pre-rename** `AddOns/ForceAudioIn`
folder installed (old `forceAudioIn.so`, old `manage.sh`/`run_ForceAudioIn.sh`).
Locally, the repo is now fully renamed: `src/forceAudioIn.c` →
`src/forceAudioJack.c`, binary → `forceAudioJack.so`, addon folder →
`ForceAudioJack` (+ a separate `ForceAudioJackSkipback` folder for
`skipbackHost`, since nodeServer's Modules page needs one `NSMODULE.json`
per folder). Deploy steps, in order, once the device is reachable:

1. **Stage first, don't overwrite in place** (per the staged-deploy
   convention): `scp` `addon/` to a `.new` path, verify with `md5sum`
   against the local copies, then `mv` into place — same pattern used for
   every deploy so far this project.
2. Remove the old `AddOns/ForceAudioIn` folder entirely *after* the new
   `AddOns/ForceAudioJack` is staged and verified (not before - keep a
   working fallback until the new one is confirmed).
3. Deploy `addon-skipback/` to a new `AddOns/ForceAudioJackSkipback` folder
   (separate from `ForceAudioJack`, so it gets its own Modules-page toggle).
4. Run `AddOns/ForceAudioJack/manage.sh ENABLE` — this arms
   `forceAudioJack.so` **and** cleans up any stale `forceAudioIn`/
   `ForceAudioIn` entry left in `LD_PRELOAD`/as a top-level launcher file
   from before the rename (handled automatically by the updated
   `manage.sh`/`run_ForceAudioJack.sh` - no manual cleanup needed).
5. This requires exactly **one** `acvs` restart. Watch it settle to a
   stable, unchanging MPC pid before doing anything else — per the
   restart-pacing memory note from the 2026-09-21 incident. Do not chain
   this with fix #1 above or with any further restart in the same session;
   verify each change independently.
6. Only after that's confirmed stable: consider live-testing the new
   out-bus/skipback functionality itself (separate, deliberate step - not
   part of this deploy).
