#!/bin/sh
############################################################
# ForceAudioJack — autostart hook (formerly ForceAudioIn - renamed when
# out-bus/skipback features were merged in; see docs/PROPOSAL-force-audio-jack.md).
# Copy this file into the AddOns FOLDER ROOT to enable.
# MockbaMod's boot.sh runs every *.sh in AddOns/ at startup.
#
# ARMS THE TAP ONLY. Does not start any producer (not injectTone, not any
# voice-host addon's binary, not skipbackHost) - see manage.sh's header
# comment for why: every live test of "acvs restart while a voice is
# attached" has failed, while "forceAudioJack.so armed with zero voices"
# has survived every repeated-restart test run against it, including a
# real physical reboot. So this script's whole job at boot is: arm the
# tap, attach nothing.
#
# Voices attach later, entirely through their own nodeServer Modules-page
# toggle (which spawns a process directly - no LD_PRELOAD, no acvs
# restart). forceAudioJack.so's own background thread lazily re-attaches
# any voice/skipback ring that appears (or reappears, e.g. after a
# stop/restart) within ~2s, with no restart needed. This addon being the
# ONLY one that ever touches $mmLD_PRELOAD_VAR's forceAudioJack entry is
# what makes that safe - if two addons both armed the tap, they'd race on
# this same file exactly like mockbaMagic/MidiLoop once did (see README.md).
############################################################

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh
APPDIR="$mmPath/AddOns/ForceAudioJack"
LIB="$APPDIR/forceAudioJack.so"

# ── Locking around $mmLD_PRELOAD_VAR ────────────────────────
# CONFIRMED live (2026-09-13): mockbaMagic's and MidiLoop's own run_*.sh
# scripts both read this same file, check their library isn't already in
# it, and write the whole file back - with NO locking at all - while
# boot.sh backgrounds every top-level addon script concurrently. That's a
# real, observed lost-update race: whichever write lands last wins,
# silently dropping another script's entry. This can't fix their side of
# it, only make ours safe. mkdir is atomic even on busybox; the retry is
# bounded and fails OPEN (proceeds unlocked) rather than risk hanging boot
# forever on a stale lock from a crashed process.
#
# BUG FIXED 2026-09-22: this function's own retry counter MUST use a name
# no caller could plausibly also use for its own loop counter - POSIX sh
# functions share the caller's variable scope (no `local` in plain sh/ash),
# so a same-named `i` here silently clobbers a caller's own `i` every time
# this is called. That's exactly what happened: the "arm the tap" retry
# loop below used `i` as its own counter, calling this function each
# iteration - which reset that `i` back to 0 on every call (mkdir succeeds
# immediately when uncontended, so the while body below never runs and `i`
# is left at the `i=0` on the very next line), so the outer loop's own
# `i=$((i+1))` never accumulated past 1 and the outer `while [ $i -lt 5 ]`
# never terminated. Confirmed live: this left a permanently-running,
# never-exiting copy of this script on every single boot, hammering this
# same lock forever - never diagnosed as a hang because it kept re-writing
# perfectly valid content the whole time, just never actually finishing.
PRELOAD_LOCK="/dev/shm/.LD_PRELOAD.lock"
lock_preload() {
    _lp_tries=0
    while ! mkdir "$PRELOAD_LOCK" 2>/dev/null; do
        _lp_tries=$((_lp_tries + 1))
        [ $_lp_tries -ge 50 ] && return 1   # ~5s of retries, then fail open
        sleep 0.1
    done
    return 0
}
unlock_preload() { rmdir "$PRELOAD_LOCK" 2>/dev/null; }

# boot.sh calls addon scripts with "kill" on shutdown/restart - full
# teardown - unconditionally, on EVERY restart, not just DISABLE/UNINSTALL.
#
# REDESIGNED 2026-09-22: this used to strip our own LD_PRELOAD entry here,
# every single restart, then unconditionally re-add it in the "load" path
# below - meaning this addon re-entered the boot-time write race from
# scratch on every single restart, forever. Direct comparison against
# mockbaMagic's and MidiLoop's own run_*.sh (see architecture.md/gotchas.md)
# found why THEY almost always win it and this addon almost never did:
# their own "kill" handlers never touch $mmLD_PRELOAD_VAR at all, and their
# "load" logic only writes if their entry ISN'T already present - so once
# their entry lands (typically the very first boot after being enabled), it
# just persists across every future restart untouched, and they're never
# exposed to the race again. This addon's own strip-then-reader-add pattern
# (copied from ForceShadow's own run_ForceShadow.sh, which has the SAME
# flaw - force_shadow.so was also intermittently missing in testing) meant
# every single restart replayed the race from zero. Confirmed live
# (2026-09-22): forceAudioJack.so lost this race on every one of 20+
# restart attempts under the old strip-and-readd design, across every
# variant tried (single write, 5x retry-write, two different boot.sh
# read-side fixes - see docs/PENDING-DEVICE-FIXES.md for why those were
# abandoned). Switching to the same idempotent pattern mockbaMagic/MidiLoop
# already use, below, needs to win the race at most ONCE (the very first
# boot after `manage.sh ENABLE`) rather than every single restart forever.
if [ "$1" = "kill" ]; then
    for p in $(ps 2>/dev/null | grep -E "\[i\]njectTone|\[s\]kipbackHost" | awk '{print $1}'); do
        kill -9 $p 2>/dev/null
    done
    exit 0
fi

# ── ARM THE TAP - nothing else ─────────────────────────────
# Idempotent, matching mockbaMagic's/MidiLoop's own proven pattern: if our
# entry is already present (the common case, from any previous successful
# boot), do nothing at all - no lock, no write, no exposure to the race.
# Only write if genuinely absent (first-ever activation, or DISABLE/
# UNINSTALL removed it via manage.sh's own STOP() - not this script's
# "kill" path, which no longer touches this file at all, see above).
if [ -f "$mmLD_PRELOAD_VAR" ] && grep -qF "$LIB" "$mmLD_PRELOAD_VAR" 2>/dev/null; then
    exit 0
fi

lock_preload
if [ -f "$mmLD_PRELOAD_VAR" ]; then
    # Still strip a stale pre-rename forceAudioIn entry here too, in case
    # a device upgraded from the old name has one left over from before
    # manage.sh ENABLE's own cleanup existed - a dangling path to a folder
    # that no longer exists, otherwise never removed since this addon's
    # "kill" no longer touches the file at all.
    FC=$(cat "$mmLD_PRELOAD_VAR" | tr " " "\n" | grep -v -E "forceAudioJack|forceAudioIn" | tr "\n" " ")
    echo "$LIB $FC" > "$mmLD_PRELOAD_VAR"
else
    echo "$LIB" > "$mmLD_PRELOAD_VAR"
fi
unlock_preload
