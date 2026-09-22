# Proposal: force-audio-jack (merging output injection, force-link-audio, skipback)

Status: **all four build-order steps are done** (out-bus injection, the
skipback extraction ring + `.so` side, `skipbackHost` itself + live
MidiLoop wiring, and the force-audioin → force-audio-jack rebrand) —
**none of the new functionality has been deployed to the device or
live-tested against real MPC/ALSA yet**, only unit-tested (57 checks
against the real mixing code) and, for `skipbackHost`, smoke-tested
natively against a synthetic producer. A live-test pass on real hardware
(per the still-open pads/buttons risk in Open Question #1) is the
remaining work before any of this is considered done. This doc is kept up
to date as work lands — see `DESIGN.md` for the parts of the *original*
force-audioin that are shipped/stable on real hardware today.

## Confirmed hardware facts (live on 192.168.1.187, MPC pid 997)

- `card 3 [ADA2]`, device 0, is the one PCM device MPC holds exclusively for
  both directions (matches `DESIGN.md`'s existing finding).
- **Playback** (`/proc/asound/ADA2/pcm0p/sub0/hw_params`): **4 channels**,
  S32_LE, 44100 Hz, one interleaved handle. There is no separate ALSA device
  for outputs 3/4 — they are channels 2/3 of the *same* handle whose
  channels 0/1 are the main stereo mix.
- **Capture** (`pcm0c`): 2 channels — confirms the existing `readi` hook
  already sees everything capture-side has to offer; audio-in 1/2 injection
  needs no changes.

This settles the open design question from the initial discussion: output
3/4 injection cannot be done by a producer opening a separate device
directly (MPC already holds the only playback handle exclusively). It needs
a `snd_pcm_writei` interposition hook, symmetric to the existing `readi`
hook — and that same `writei` hook is also exactly what's needed to extract
the main mix (force-link-audio's approach) and to source skipback.

## Architecture: one `.so`, two interposed symbols

```
                    ┌─────────────────────────────────────┐
producers ─(shm)──▶ │ forceAudioJack.so (in MPC's process) │
 (in1/2 voices)     │  snd_pcm_readi hook (existing)       │──▶ Audio-In 1/2
                    │    mixes N "In" rings additively      │
                    │                                       │
producers ─(shm)──▶ │  snd_pcm_writei hook (new)            │──▶ physical Out 3/4
 (out3/4 voices)    │    mixes N "Out" rings into ch 2/3    │    (ch 0/1 untouched)
                    │    only, additively                   │
                    │                                       │
                    │    extracts ch 0/1 (post-mix) into a  │──(shm)──▶ skipbackHost
                    │    ring, .so as PRODUCER this time     │           (rolling buffer,
                    └─────────────────────────────────────┘              WAV on trigger)

(force-link-audio runs as a separate, unmodified addon alongside this one -
its own snd_pcm_writei interposer coexists via normal LD_PRELOAD chaining,
not through anything in this diagram - see "Link Audio" below.)
```

Both hooks stay real-time-safe by the same rule already documented in
`DESIGN.md`: no allocation, syscalls, or logging on the hot path. Everything
past the ring boundary — UDP sends, WAV writes, SD-card I/O — happens in
separate consumer processes, never inside the interposed call. This is a
deliberate deviation from force-link-audio's own `forceStream.so`, which
calls `sendto()` directly from inside the interposed `snd_pcm_writei` — it
apparently works in practice, but it's a hot-path syscall this project's own
discipline has avoided from day one, and there's no need to inherit that
risk when the SPSC-ring-to-daemon pattern is already proven here.

## Ring taxonomy (backward compatibility)

**Existing `/forceAudioInject%u` rings (in1/2 injection) are not touched.**
`force-maze` vendors `forceAudioInject.h` byte-for-byte today; renaming or
reshaping that struct would break it. All new functionality gets new ring
names/types instead:

| Ring | Direction | Producer | Consumer | Struct | Name | Status |
|---|---|---|---|---|---|---|
| In (existing) | inject | voice host | `.so` (`readi`) | `ai_shm_t` (unchanged) | `/forceAudioInject%u`, 0..3 | shipped (original v1) |
| Out (new) | inject | voice host | `.so` (`writei`) | `ai_shm_t` (reused as-is — same shape works for either bus) | `/forceAudioJackOut%u`, 0..1 | **implemented, unit-tested, not yet live-tested** |
| Skipback (new) | extract | `.so` (`writei`) | `skipbackHost` | `ax_shm_t` (reversed roles) | `/forceAudioJackSkipback0` | **`.so` side implemented, unit-tested**; `skipbackHost` itself not started |

(There is no Link ring — see "Link Audio" below for why.)

**Out-bus reuses `ai_shm_t` unchanged** — a voice host doesn't need a new
producer library, just a different ring name and a `--bus in|out` style
argument choosing which prefix to open. This is the mechanism that answers
the original ask: "let jv880/maze-voice choose whether generated audio
streams into Force OS via Audio-In 1/2, or out to an external
device/mixer via Out 3/4" — it's purely which ring name the producer opens,
same producer code otherwise. A producer could even attach to both an In
and an Out slot simultaneously if a future need called for it.

**The extraction ring needed a new, direction-reversed struct** — the `.so`
is the producer here (not a consumer), writing the post-mix ch0/1 samples
after each real `writei` call succeeds; the consumer is the sole reader
draining `tail`. Same SPSC discipline, roles swapped. Implemented in
`src/forceAudioJackExtract.h`:

```c
#define AX_SHM_NAME_SKIPBACK "/forceAudioJackSkipback0"
#define AX_MAGIC       0x414a4b53u   /* 'AJKS' - distinct from AI_MAGIC */
#define AX_RING_FRAMES (1u << 16)    /* same conveyor size as the injection rings - just the handoff, not skipbackHost's own large rolling buffer */
#define AX_CHANNELS    2

typedef struct {
    uint32_t magic;
    uint32_t rate;
    uint32_t channels;
    volatile uint32_t head;   /* .so (producer) writes */
    volatile uint32_t tail;   /* skipbackHost (consumer) writes */
    volatile uint64_t frames_written;
    volatile uint64_t frames_consumed;
    volatile uint64_t overruns;   /* consumer too slow to keep up - .so overwrites oldest, never blocks */
    float ring[AX_RING_FRAMES * AX_CHANNELS];
} ax_shm_t;
```

Same latency-management concern as the existing mix path, but inverted: here
it's the *consumer* that must never fall behind, since the `.so` can't block
MPC's audio thread waiting for a slow consumer. `ax_extract_skipback()` in
`forceAudioJack.c` implements this: it always writes the full frame count at
`head` regardless of how far `tail` has fallen behind, and bumps `overruns`
by however many frames got overwritten before `skipbackHost` ever read them
— unit-tested directly (pushing a full ring lap with the consumer's `tail`
frozen produces exactly one overrun frame, `tests/test_mix.c`).

**Ownership is the other way round from a voice ring**: for In/Out
injection, the voice host creates the segment and `.so` attaches as
consumer; for the skipback ring, `skipbackHost` will create the segment
(on demand, via its own Modules-page entry — same operational rule as every
voice host) and `.so` attaches as *producer* (`ax_try_attach_skipback()`,
same identity-by-inode re-attach logic as the injection rings' lazy attach,
just for a single fixed segment). This keeps the "nothing attached until
something is deliberately started, picked up within ~2s" safety model
completely intact.

## Feature: Link Audio — coexistence, not reimplementation (revised)

`force-link-audio`'s actual mechanism (confirmed via its README): taps
`snd_pcm_writei`, sends the stereo mix as UDP to `127.0.0.1:9000`, and a
separate daemon converts to int16 and publishes it as a discoverable Link
Audio channel (peer name, channel name, and UDP port all configurable) that
Ableton Live, an iPad, or another LAN machine can pick up.

**Original plan (superseded)**: build a `Link` extraction ring plus a new
`linkAudioHost` daemon reimplementing the UDP-send-and-discovery logic.
**Dropped after checking**: `force-link-audio` is not installed on this
Force (192.168.1.187), so there's no way to capture its real wire format
and verify a reimplementation against it. There is no public spec for the
exact packet/discovery format beyond the high-level description above.
Building a from-scratch daemon against a guessed format risks shipping
something that compiles and sends packets but silently fails to
interoperate with an actual Ableton Live/iPad receiver — exactly the kind
of unverified claim this project's own testing discipline (28+ real
checks against the actual mixing code, not assumptions) argues against.

**Revised plan: install macdigi's real, unmodified `force-link-audio`
addon alongside `force-audio-jack`, don't reimplement it.** Its own
`snd_pcm_writei` interposer coexists safely with this project's — multiple
LD_PRELOAD interposers already stack in this ecosystem via each one's own
`dlsym(RTLD_NEXT, ...)` chaining (that's how mockbaMagic, MidiLoop's
`tkgl_anyctrl_lt.so`, and this addon already coexist in MPC's process
today). "Merging" force-link-audio becomes a packaging/branding decision —
one unified addon family, install docs that mention it as a companion — not
a protocol-level one. **Consequence: no `Link` extraction ring is needed.**
The extraction-ring mechanism built for step 2 (see below) exists solely
for Skipback, which has no existing implementation anywhere to reuse.

## Feature: Skipback

Schwung's Skipback: continuously records from the main output so that
hitting "save" captures audio from *before* you decided to record — no need
to pre-arm a take.

Proposed split:
- `.so`'s `writei` hook feeds the same ch0/1 copy into the `Skipback`
  extraction ring (small, same `AI_RING_FRAMES` conveyor size as everything
  else — this is just the low-latency handoff, not the actual buffer).
- A `skipbackHost` daemon owns the real rolling buffer *in its own process
  memory*, not shared memory — a useful window (e.g. a few minutes) at
  stereo float32/44.1kHz is on the order of 100+ MB, far bigger than
  anything that belongs in a fixed-size shm segment sized for real-time
  handoff. The daemon continuously drains the small ring into its own large
  circular buffer.
- **Trigger — confirmed and chosen**: checked the live `midiloop.config` and
  `USER-SCRIPTS.sh` on 192.168.1.187 directly rather than guessing. Every
  modifier-button combo family is already mostly or fully claimed by this
  same addon family (force-shadow's page jumps, engine toggles, system
  utilities) except `SHIFT+RECORD`, which doesn't appear anywhere in the
  config at all — and `RECORD` is already a valid button token elsewhere
  (`KNOBS+RECORD`, `LAUNCH+RECORD`, `MIXER+RECORD`, `NOTE+RECORD`,
  `SELECT+RECORD` all exist), so the syntax is proven. **Chosen: `SHIFT+RECORD`**
  bound to a new `SCRIPT-26` (`USER-SCRIPTS.sh` already has 25 numbered
  slots sourced with no hardcoded ceiling — this family has already
  extended it from the stock 8 several times) running one line:
  `touch /tmp/forceAudioJack.skipback`. `skipbackHost` polls for that marker
  (same pattern this project already uses for `AI_DIAG_MARKER`/
  `AI_DUMP_MARKER`), and on seeing it, flushes the current rolling-buffer
  contents to a WAV file on a configured SD-card folder, on its own thread
  (never blocking the ring-drain thread), then deletes the marker. Not yet
  written to the live `midiloop.config`/`USER-SCRIPTS.sh` — that's a shared
  file other addons also depend on, so it'll be edited (with a backup) once
  `skipbackHost` actually exists to respond to the trigger, not before.
- **Config**: destination folder path and rolling-window length (seconds),
  in a plain config file next to the addon, same convention as
  force-link-audio's `config` file.
- **Naming, with project name + BPM** (confirmed feasible, checked live on
  192.168.1.187): `/data/Settings/MPC/CurrentProject` is a small plain-XML
  file with a `lastProjectPath` field pointing at the loaded `.xpj`; the
  `.xpj` itself is gzip-compressed JSON (confirmed via magic bytes `1f 8b`)
  containing a top-level `"Tempo": 128.0` field. Both are cheap, off-hot-path
  reads the `skipbackHost` daemon can do at trigger time only (not per-frame,
  not from the `.so`): read `CurrentProject` → gunzip the referenced `.xpj`
  → extract `Tempo`. Proposed filename:
  `Skipback_<ProjectName>_<BPM>bpm_<YYYYMMDD>_<HHMMSS>.wav`, e.g.
  `Skipback_Testw_128bpm_20260921_143022.wav` (project name sanitized for
  filesystem-safe characters; BPM rounded to an integer). One caveat worth
  knowing: this reads the last-*saved* project state, so a tempo change made
  live since the last autosave/save won't be reflected until the next save —
  fine for a filename tag, not something to rely on for precision.

## Rebrand mechanics

- Repo: `force-audioin` → `force-audio-jack` (rename on GitHub, update
  local remote).
- `.so`: `forceAudioIn.so` → `forceAudioJack.so`. Existing `/forceAudioInject%u`
  ring *name* stays as-is regardless of the `.so`'s own filename — it's an
  independent ABI contract with `force-maze`, not tied to the binary name.
- AddOns dir / `NSMODULE.json` entries: `ForceAudioIn` → `ForceAudioJack`,
  with each producer/consumer role (in-voice test tone, out-voice test
  tone, `skipbackHost`) as its own Modules-page entry, same as `injectTone`
  today. No `linkAudioHost` entry — Link Audio is a separate, unmodified
  `force-link-audio` install (see above), not part of this addon's package.
- `manage.sh ENABLE` on the renamed addon must clean up the old
  `ForceAudioIn` LD_PRELOAD entry if present, so a reinstall-under-new-name
  doesn't leave a stale `forceAudioIn.so` reference in
  `/dev/shm/.LD_PRELOAD` pointing at a path that no longer exists.

## Open questions / risks — status after review

1. **Pads/buttons-dead-on-`acvs`-restart bug, new ring types unknown.**
   Unresolved — still needs its own live test pass once the `writei` hook
   exists. Until then, the existing hard rule (never restart `acvs` while
   anything is attached) applies defensively to *every* ring type in this
   proposal, not just the existing In rings it was proven against.
2. **Out 3/4 already in occasional use** (click track / external
   monitor send, not constant). **Decision: additive, same contract as
   in1/2 injection** — out-bus injection sums into ch2/3 the same way
   in-bus injection sums into the real capture signal, never replacing it.
   When isolation is actually needed, that's an operational step (turning
   off the relevant engine/producer), not something the tap itself
   enforces — exactly the same model already documented for real
   instruments on Audio-In.
3. **RAM budget: default 30s, hard cap 1 min.** At stereo float32/44.1kHz
   that's ≈10.6 MB default / ≈21.2 MB worst case — small enough that actual
   RSS validation in testing is a sanity check, not expected to be a real
   constraint on an embedded device already running MPC, MockbaMod, and
   active voice hosts. The 1-minute cap is enforced by `skipbackHost`
   itself (fixed-size buffer, config value clamped at load), not left to
   the user's config file to accidentally set higher.
4. **SD card write path — resolved by poking around the actual filesystem**
   (192.168.1.187): the Force's own in-flight recordings land in
   `Force Documents/Temporary Files/Temporary Recordings/` as 0-byte,
   UUID-named placeholder `.wav` files (MPC's own arm-to-record mechanism,
   not a model to imitate — these are transient and not meant to be
   browsed). The user-facing, browsable location is
   `Force Documents/Samples/` — confirmed it holds real, finished, named
   recordings (`JazzOrganShifter.wav`) that Force's own sample browser
   surfaces for immediate use elsewhere in a project, matching schwung's
   own framing of Skipback output as usable "stems," not an archival dump.
   **Proposed default**: a labelled subfolder,
   `Force Documents/Samples/Skipback/` — inside the location Force already
   scans, so skipback captures show up in the sample browser immediately,
   without cluttering the top-level `Samples/` listing or landing in the
   throwaway `Temporary Recordings/` folder. (Not yet confirmed live
   whether Force's sample browser descends into subfolders of `Samples/` —
   worth a quick check before this is final, but it's the same convention
   `Expansions/Projects` and other stock folders already use, so it's a
   safe bet.) Config file should let this be overridden, same as
   force-link-audio's own `config` file pattern.

## Build order and status

1. ✅ **Done.** `writei` hook + Out-bus injection (reuses `ai_shm_t` as-is).
   `mix_out_one`/`mix_out`/`chan_allowed_out` in `forceAudioJack.c`,
   `--bus in|out` added to `injectTone` for smoke-testing either bus.
   41 unit tests green (28 original In-bus + 13 new Out-bus), cross-compiles
   clean for armhf. **Not yet deployed or live-tested on hardware** — the
   pads/buttons-dead-on-`acvs`-restart risk is unverified for this new ring
   type (see Open Questions #1).
2. ✅ **`.so` side done** (Link Audio dropped in favor of coexistence — see
   above). `ax_extract_skipback`/`ax_try_attach_skipback` in
   `forceAudioJack.c`, `src/forceAudioJackExtract.h` for the ring ABI.
   57 unit tests green total (41 above + 16 new: extraction correctness,
   overrun-under-stall, lazy attach as producer). Cross-compiles clean.
   **`skipbackHost` itself (the consumer) does not exist yet** — nothing
   will actually attach to this ring on a real device until it's built.
3. ✅ **Done.** `src/skipbackHost.c`: owns the real rolling buffer (30s
   default / 60s hard cap via `--window-sec`, clamped regardless of what's
   passed), drains the extraction ring on a ~20ms cadence, and on the
   trigger marker (checked every ~200ms) spawns a detached thread that
   snapshots the buffer, looks up the current project name + tempo (best
   effort, never blocks the save if either lookup fails), and writes a WAV
   to `--output-dir` (default `/sdcard/Force Documents/Samples/Skipback`,
   confirmed live: `/sdcard` is the Force's own SD card, a *different*
   filesystem from `/media/662522` where MockbaMod/AddOns actually live).
   Smoke-tested end to end on this dev machine (native build, a synthetic
   producer writing a tone into the ring, triggering a save, and verifying
   the resulting WAV's sample rate/channel count/frame count/content with
   Python's `wave` module) - not yet run against the real `.so` or on
   device. Cross-compiles clean for armhf, added to `scripts/build.sh`.
   MidiLoop wiring is live: `SHIFT+RECORD = SCRIPT-26` in
   `midiloop.config`, and a `SCRIPT-26` block in `USER-SCRIPTS.sh` running
   `touch /tmp/forceAudioJack.skipback` — both files backed up
   (`*.bak-force-audio-jack-<timestamp>`) before editing. **This needs a
   MidiLoop config reload to take effect (its own `RELOAD-CONFIG` command,
   not an `acvs` restart)** - not triggered yet, since that changes live
   MIDI-controller behavior the user should apply when ready, not silently.
   **Packaging note for the eventual deploy**: nodeServer's Modules page
   (`moduler`) scans exactly one `NSMODULE.json` per `AddOns/*` folder, so
   `skipbackHost` needs its own addon folder to get its own start/stop
   toggle - the same reason `ForceDX7`/`ForceJV880`/`ForceMazeVoice` are
   separate top-level folders today despite all depending on this same
   tap. Resolved in step 4 below: `addon-skipback/` is now `skipbackHost`'s
   own local packaging folder (maps to `AddOns/ForceAudioJackSkipback`),
   built automatically by `scripts/build.sh` alongside `addon/`.
4. ✅ **Done.** Rebrand: GitHub repo renamed
   (`sd88me/force-audioin` → `sd88me/force-audio-jack`, local git remote
   updated automatically via `gh repo rename`); `src/forceAudioIn.c` →
   `src/forceAudioJack.c`; binary output renamed
   `forceAudioIn.so` → `forceAudioJack.so`; all internal log/marker paths
   (`/tmp/forceAudioIn.log` etc.) → `forceAudioJack.*` equivalents;
   `addon/manage.sh`/`run_ForceAudioJack.sh` (renamed from
   `run_ForceAudioIn.sh`) updated to arm/clean up under the new name, with
   explicit cleanup of a stale pre-rename `LD_PRELOAD` entry and a stale
   pre-rename top-level launcher file, so an in-place upgrade from the old
   name can't leave both scripts active at once. `forceAudioInject.h`
   (filename and the `/forceAudioInject%u` ring name/ABI itself)
   deliberately **not** renamed — that's the byte-for-byte contract
   `force-maze` vendors a copy of, and renaming it would break that
   dependency for zero benefit. 57/57 unit tests still pass after the
   rename; both binaries cross-compile clean under the new names.
   Deployed to the device and verified live on 2026-09-23 — the tap loads
   and stays loaded across app restarts and cold reboots. The audio paths
   themselves are still unheard; see DESIGN.md's
   [Known limitations](../DESIGN.md#known-limitations) for what remains
   to verify by ear.
