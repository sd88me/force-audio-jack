# force-audio-jack — Handoff: what's left, as of 2026-09-23

**Device:** Akai Force, MockbaMod (`sd88me` fork), reachable at `192.168.1.44` at
time of writing (DHCP — may differ next boot; try `.187`/`.188`/`.189` too, or
check the Force's own network screen).
**Repo HEAD:** `bebc8e7`
**Current status:** The crash that blocked this project for days is fixed and
verified stable. Basic tap functionality (In-bus → Main → Skipback) is proven
working with clean, real audio. What's left is real but bounded — see §2.

---

## 1. Current state — read this first

| Item | State |
|---|---|
| `forceAudioJack.so` | **Enabled, loaded, stable.** Verified across app restarts and a full cold reboot. |
| The `cereal::RapidJSONException` crash loop | **Fixed** (root cause: our build exported ~385 unintended libc/libm symbols under `LD_PRELOAD`; see DESIGN.md's "Symbol export scope"). |
| `zig` toolchain requirement | **Must be ≥ 0.14.0.** `scripts/build.sh` enforces this and explains why (a real 0.13.0 ARM codegen bug in variadic `printf` doubles). |
| In-bus → Main → Skipback path | **Verified working**, real audio, no glitches, on real hardware. |
| Out-bus → physical Out 3/4 | Confirmed reaching the right channels at the right rate numerically; **never confirmed by ear**. |
| MPC (`az01-launch-MPC`) | Stable, pid 19755 as of this writing, holding steady. |
| DrmVncServer | Disabled (its own unrelated display race recurred a 3rd time mid-session; see memory note). Re-enable cautiously if wanted. |
| Skipback trigger shortcut | **Temporary** — `KNOBS+SCENE-1`, not a permanent binding (see §2.6). |
| Leftover on device | One test WAV at `/sdcard/Force Documents/Samples/Skipback/Skipback_Testw_20260923_001428.wav` (harmless, real captured 440Hz tone — safe to delete or keep as a reference sample). |

Nothing is mid-flight. All test processes (`injectTone`, `skipbackHost`, `cratedigger_host`) were stopped cleanly and no diagnostic/trigger marker files are left set.

---

## 2. Open items, roughly in priority order

### 2.1 Pads/buttons go dead when restarting `acvs` with a voice attached — not root-caused
Restarting `acvs` while any voice (In-bus, Out-bus, or presumably Skipback) is
attached can reliably make pads/buttons unresponsive (occasionally Wi-Fi too).
Never observed with zero voices attached. Investigation has ruled out several
specific mechanisms (symbol collision with MidiLoop, `mockbaMagic`'s
address-patching, the diagnostics thread, the per-sample mix loop itself) but
not the ring bookkeeping/atomics/backlog-trim path, nor something specific to a
real voice host's own thread/MIDI-client behavior that a minimal test producer
wouldn't exhibit. See DESIGN.md's "Known limitations" for the full trail.

**Current operating rule (not a workaround, the permanent model until this is
root-caused): never restart `acvs` while any voice is attached.** See DESIGN.md's
"Boot sequence & the operational safety rule".

### 2.2 Out-bus → physical Out 3/4 jacks — needs ears, not more code
`injectTone --bus out`'s ring was confirmed via the diagnostics thread
(`touch /tmp/forceAudioJack.diag`, watch `/tmp/forceAudioJack.log`) to be mixed
into channels 2/3 of the real 4-channel hardware handle at the correct
real-time rate. Skipback can't verify this — it deliberately only records
channels 0/1 (Main mix). Nothing in software distinguishes "reaching the jacks"
from "a cable that happens to be unplugged." Needs someone physically at the
device with a cable into Out 3/4 (or a scope/interface) to actually confirm.

### 2.3 Open Question #1 (from `docs/PROPOSAL-force-audio-jack.md`)
Does restarting `acvs` while an **Out-bus or Skipback** ring is attached kill
pads/buttons the same way it does for In-bus rings (§2.1)? Untested — test
deliberately, expecting to have to recover, not during normal use.

### 2.4 Ring-backlog aliasing bug — confirmed, not fixed
`avail = (head - tail) & (AI_RING_FRAMES - 1)` can alias if the true unconsumed
gap exceeds one full ring lap (65,536 frames, ~1.49s at 44.1kHz) — plausible
during an `acvs` restart, since a voice host keeps rendering into its ring
across the gap with no consumer to drain it. Directly observed (a high underrun
rate right after a restart gap with a producer still rendering).

### 2.5 Parked: voice-clocking/glitching refinement category
Grouped together, lowest priority, likely share root-cause territory (ring
timing/backpressure under this device's real scheduling behavior):
- §2.4 above.
- The `SCHED_FIFO` anti-pattern note (tried once as an "obvious" fix, made
  things measurably worse — don't retry without reading DESIGN.md's note first).
- The residual **~7 small phase-discontinuity clicks per 30s** left in
  `injectTone`'s calibrated 96%-margin pacing fix (2026-09-23) — underruns and
  silence gaps are gone, this is a much smaller remaining artifact. Tightening
  further hits a real 32-bit-overflow ceiling in the current pacing approach
  (see `src/injectTone.c`'s comments) — needs a different strategy (e.g. a
  running deadline instead of a fixed sleep fraction), not just a smaller number.

### 2.6 Skipback trigger shortcut is still temporary
`SHIFT+RECORD` (original design) isn't a valid MidiLoop combo; `SELECT+RECORD`
was also ruled out. Currently wired to `KNOBS+SCENE-1`, which overrides
ForceShadow's `SCRIPT-19` DX7-page placeholder (a genuine no-op, low risk, but
not free). **Needs a permanent combo decided**, and ForceShadow's binding
either restored or deliberately reassigned for good.

### 2.7 No automated regression test for §2.1
Pending a safe way to reproduce the restart-while-attached failure on demand.

### 2.8 Crate Digger's real-content playback — blocked, different addon, flagged not fixed
`ForceCrateDigger`'s Discogs search genuinely works, but playback (yt-dlp →
ffmpeg → In-bus) fails because **the device's Python has no `zlib` at all** —
confirmed on both the bundled `AddOns/Python/python3.8` and the system
`/usr/bin/python3`, and there is no `libz.so*` anywhere on the filesystem.
`bz2`/`lzma` are likewise absent; `ssl` works. Fixing this means cross-compiling
zlib from source into a `_zlib.cpython-38-arm-linux-gnueabihf.so` extension (no
system libz to link against) — a real side-project, not attempted. This is a
different addon's dependency, not a force-audio-jack bug — only relevant here
because it blocked using Crate Digger as a real-content test source for Skipback.

### 2.9 DrmVncServer display race — recurred a 3rd time, trigger still unknown
Unrelated to force-audio-jack, but happened mid-session on 2026-09-23 with the
known fix (SCRIPT-2 rewritten to not touch the persistent autolaunch flag)
confirmed still correctly in place. Disabled again (`manage.sh DISABLE`); MPC
recovered clean. The actual trigger this time was never found — see the
`drmvncserver_display_race` memory note for the one remaining lead (nodeServer's
own Modules-page/Autoload mechanism as a possible second path to the same
persistent-autolaunch state, independent of the MIDI shortcut).

### 2.10 Release checklist (from the old `PENDING-DEVICE-FIXES.md`, folded into DESIGN.md)
- [x] `forceAudioJack.so` loads and stays loaded — done, verified.
- [ ] In-bus path unchanged from before the rebrand — not explicitly re-verified
      post-fix (the working 2026-09-23 test used a synthetic tone, not the
      original pre-rebrand In-bus use case specifically).
- [ ] Out-bus reaches physical jacks by ear — §2.2.
- [ ] Skipback WAV naming/tempo lookup — works (project name lands in the
      filename), but tempo lookup has consistently returned nothing in every
      test so far (filenames always land on the no-bpm naming branch). Not
      investigated — check `lookup_tempo()`'s project-file parsing against
      whatever project is active when this is picked back up.
- [ ] §2.3 (Open Question #1) — untested.
- [ ] Only after all of the above: tag a GitHub release.

---

## 3. Where to look for full detail

- **`DESIGN.md`** — the primary technical reference. "Known limitations" has
  the full detail on everything in §2 above. "Symbol export scope" and
  "Diagnostics" cover the two big fixes from this session and the tooling
  (coredump analysis, `LD_DEBUG=bindings`, the diagnostics thread) used to find
  them.
- **`docs/PROPOSAL-force-audio-jack.md`** — original design doc, Open Question #1.
- **Memory** (`~/.claude/projects/-home-sam-force-audioin/memory/`) — session-level
  investigation trails, most relevantly: `forceaudiojack_load_crash_investigation.md`,
  `ld_preload_export_scope_rule.md`, `zig_arm_variadic_double_printf_bug.md`,
  `skipback_channel_routing_verified.md`, `injecttone_pacing_underrun_bug.md`,
  `drmvncserver_display_race.md`.

## 4. Suggested next session

Pick ONE of §2.1–§2.3 (they're related — restart-safety) as a dedicated,
calm session per this project's own established rule (one issue at a time, pace
restarts, verify full stabilization between attempts — see the
`audio_jack_restart_caution` memory note). §2.2 (ears on Out 3/4) is the
cheapest to close and doesn't risk the device. §2.1/§2.3 need someone
physically present, expecting to recover from a possible pads/buttons freeze.
