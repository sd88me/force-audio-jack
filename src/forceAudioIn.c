/*
 * forceAudioIn.so — LD_PRELOAD audio injector for the Akai Force's MPC
 * application. The inverse of MockbaMod's forceStream.so (which taps
 * snd_pcm_writei to EXTRACT what MPC plays): this hooks snd_pcm_readi to
 * MIX synthesized audio INTO what MPC reads from its capture device, so a
 * generator process elsewhere on the box can appear as live signal on an
 * Audio-In track without any hardware loopback cable.
 *
 * WHY THIS EXISTS
 *   MPC opens the ADA2 codec with raw hw: device names and holds capture
 *   exclusively, same as playback (confirmed live: fd open on
 *   /dev/snd/pcmC2D0c continuously, independent of whether anything is
 *   record-armed). There is no ALSA loopback (snd-aloop is not built into
 *   this kernel) and no JACK running, so the only seam is the one
 *   forceStream.so already uses: LD_PRELOAD symbol interposition inside the
 *   live process, against libasound's stable public ABI (not raw addresses
 *   in the closed MPC binary, so this is NOT firmware-version-pinned the
 *   way mockbaMagic's binary patching is).
 *
 * WHAT IT DOES
 *   Separate processes (e.g. injectTone, maze_host, or any future audio-
 *   rendering voice host) each write interleaved float32 audio into their
 *   OWN POSIX shared-memory ring, one per voice slot (forceAudioInject.h).
 *   Every time MPC calls snd_pcm_readi on the capture handle, this shim lets
 *   the real hardware read happen first, then ADDS (mixes) samples popped
 *   from every attached voice's ring into the buffer before returning it to
 *   MPC - each voice's own `gain`/`channel_mask`/`enabled` (also in shared
 *   memory, written by that voice's control socket) controls its volume,
 *   L/R/L+R routing, and mute independently. Real hardware input keeps
 *   working unmodified.
 *
 *   LAZY RE-ATTACH: a voice slot that doesn't exist yet at load time isn't
 *   given up on - a background thread (bg_main) retries every ~2s, so a
 *   voice host started AFTER MPC (e.g. via the nodeServer Modules-page
 *   toggle) gets picked up live, with no acvs restart needed. See the open-
 *   incident note near AI_DIAG_MARKER below for why this matters right now
 *   beyond convenience.
 *
 * SAFETY CONTRACT (same as forceStream.so)
 *   - Never breaks capture: on ANY failure the real read result is returned
 *     untouched. Missing shared memory = pure passthrough, not an error.
 *   - Audio-thread path does bounds checks, format conversion and adds -
 *     no allocation, no syscalls, no file I/O, no logging. All shm_open/
 *     mmap syscalls happen off the hot path: once per slot in the library
 *     constructor, and again from the background thread on every ~2s wake
 *     for whichever slots aren't attached yet (see ai_try_attach) - the
 *     audio thread only ever does an atomic pointer load to see the result.
 *   - Ring underrun leaves the real captured audio untouched and bumps a
 *     counter. It never blocks MPC waiting for the injector.
 *   - Disabled by default, per voice slot, until attached: with no
 *     /forceAudioInjectN shared memory segment present yet for a given
 *     slot, that slot is simply not attached - a no-op tap that only writes
 *     a one-line log so you can confirm the library itself loaded. Not
 *     permanent, though - see lazy re-attach above.
 *
 * BUILD (cross-compiled with zig, matching the Force's exact glibc):
 *   zig cc -target arm-linux-gnueabihf.2.39 -shared -fPIC -O2 \
 *       -o forceAudioIn.so forceAudioIn.c -lpthread -lrt
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "forceAudioInject.h"

/* Opaque ALSA types - we never dereference these, so no ALSA headers needed. */
typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

#define LOG_PATH "/tmp/forceAudioIn.log"

/* Open incident 2026-09-13 (see DESIGN.md for the full live-tested
 * elimination sequence): pads/buttons go dead partway through repeated
 * acvs restarts (typically the 2nd or 3rd, not the 1st) whenever a voice
 * is actually attached - LD_PRELOAD content confirmed correct throughout,
 * so NOT the known file-race. Ruled out so far, each with a live test:
 * an ALSA symbol collision with MidiLoop's tkgl_anyctrl_lt.so (disjoint
 * symbol sets); the diagnostics thread's mere existence (still failed with
 * it confirmed not spawning); forceAudioIn.so merely being loaded with zero
 * voices attached (survived repeated restarts cleanly); the per-sample
 * write loop in mix_in_one specifically (still failed with a voice attached
 * but muted, i.e. that inner loop skipped every call). Current standing:
 * whatever it is lives in "a voice's ring is attached and polled at all"
 * (the atomics/backlog-trim bookkeeping in mix_in_one that runs regardless
 * of `enabled`) or in something about the separate voice-host PROCESS
 * itself (maze_host's own threads/RtMidi client) - not yet distinguished
 * from each other. Root cause still open; this marker only ever controlled
 * the diagnostics LOGGING half of bg_main, never the lazy re-attach half
 * added afterward, since attach retry has to run regardless of whether
 * anyone wants it logged.
 *
 * Gated off by default behind a marker FILE rather than an env var:
 * forceAudioIn.so is LD_PRELOAD'd into MPC by a boot script, not launched
 * directly, so there's no practical way to set an environment variable in
 * MPC's own exec environment - a file checked once at library-load time is
 * trivially toggleable over SSH with no rebuild and no boot-script change. */
#define AI_DIAG_MARKER "/tmp/forceAudioIn.diag"

/* ---- originals ----------------------------------------------------------*/
static snd_pcm_sframes_t (*orig_readi)(snd_pcm_t *, void *, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*orig_readn)(snd_pcm_t *, void **, snd_pcm_uframes_t);
static int (*orig_hw_params)(snd_pcm_t *, snd_pcm_hw_params_t *);

static int (*q_get_format)(const snd_pcm_hw_params_t *, int *);
static int (*q_get_channels)(const snd_pcm_hw_params_t *, unsigned int *);
static int (*q_get_rate)(const snd_pcm_hw_params_t *, unsigned int *, int *);
static const char *(*q_pcm_name)(snd_pcm_t *);

/* ---- shared memory (the injection sources) --------------------------------
 * Opened once per slot, in the constructor, never touched again except
 * mmap'd memory reads/CAS on the hot path - no syscalls after startup. Each
 * attached slot is an independent voice with its own SPSC ring. */
static ai_shm_t *g_shm[AI_MAX_VOICES];
static unsigned  g_n_attached = 0;   /* how many of g_shm[] are non-NULL, for logging only */
static ino_t     g_shm_ino[AI_MAX_VOICES];  /* inode of the segment each slot is currently
                                              * mapped to - only ever read/written from the
                                              * background thread, see ai_try_attach below */

/* Bounding latency needs HYSTERESIS, not a hard ceiling: a persistent tiny
 * clock-rate mismatch between the producer's wall-clock timer and this
 * consumer's real ALSA-clocked read rate means backlog is ALWAYS drifting
 * toward a hard trim threshold - trimming AT that threshold on every call
 * fires almost continuously once backlog reaches it, and each trim is a
 * small phase discontinuity, i.e. a click. A train of those, many times a
 * second, is exactly "fine at first, then a fast glitching artifact
 * appears" (observed live): clean while backlog is below the ceiling, then
 * constant micro-clicks once drift catches up to it.
 *
 * Trim only once backlog exceeds TRIGGER (~100ms) and drop it all the way
 * down to TARGET (~25ms) when it does - one bigger, much rarer correction
 * instead of continuous small ones. */
/* 2026-09-12: a 25/100ms target/trigger pair looked fine in short live tests
 * (backlog stable, few trims) but under longer play the relationship between
 * the producer's wall-clock render rate and the consumer's real ALSA-clocked
 * read rate turned out to wander in BOTH directions over time, not drift
 * one-way as first measured - backlog was later observed sitting at ~2ms
 * with underruns climbing (hundreds of them, i.e. frequent small dropouts,
 * heard as "glitching a bit"). 25ms of cushion isn't enough margin against
 * that wander. Bigger targets trade a bit more latency for headroom in both
 * directions; ~100/200ms still reads as responsive on a played note and
 * remains far below the original ~1.5s (full-ring) latency bug this
 * mechanism replaced. */
#define AI_LATENCY_TARGET_FRAMES  4410u   /* ~100ms @ 44100Hz - trim DOWN to this */
#define AI_LATENCY_TRIGGER_FRAMES 8820u   /* ~200ms @ 44100Hz - only trim past this */

static uint64_t g_trim_events[AI_MAX_VOICES];   /* diagnostics only, read/logged from diag_main */
static uint64_t g_trim_frames[AI_MAX_VOICES];

/* ---- per-handle capture shape, same pattern as forceStream.so's g_pcms[] */
#define MAX_PCMS 8
static struct {
    void *pcm;
    unsigned frame_bytes, channels, rate;
    int format;
    volatile uint64_t frames;
} g_caps[MAX_PCMS];
static volatile void *g_tap_pcm = NULL;   /* the one capture handle we mix into */

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static volatile int g_announce_claim = 0;

static void ai_log(const char *fmt, ...)
{
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static unsigned width_of(int fmt)
{
    switch (fmt) {
        case 0:  case 1:  return 1;   /* S8 / U8             */
        case 2:  case 3:  return 2;   /* S16_LE / S16_BE     */
        case 6:  case 7:  return 3;   /* S24_3LE / 3BE       */
        case 10: case 11: return 4;   /* S32_LE / S32_BE     */
        case 14: case 15: return 4;   /* FLOAT_LE / FLOAT_BE */
        default:          return 4;
    }
}

/* Read one interleaved sample at byte offset `off` in `buf`, format `fmt`,
 * as a float in [-1, 1]. */
static inline float sample_to_float(const unsigned char *buf, size_t off, int fmt)
{
    switch (fmt) {
        case 2: { int16_t v; memcpy(&v, buf + off, 2); return v / 32768.0f; }
        case 10: { int32_t v; memcpy(&v, buf + off, 4); return v / 2147483648.0f; }
        case 14: { float v; memcpy(&v, buf + off, 4); return v; }
        default: return 0.0f;
    }
}

/* Write a float in [-1, 1] back as one interleaved sample, clamped. */
static inline void float_to_sample(unsigned char *buf, size_t off, int fmt, float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    switch (fmt) {
        case 2: { int16_t s = (int16_t)(v * 32767.0f); memcpy(buf + off, &s, 2); break; }
        case 10: { int32_t s = (int32_t)(v * 2147483647.0f); memcpy(buf + off, &s, 4); break; }
        case 14: { memcpy(buf + off, &v, 4); break; }
        default: break;
    }
}

/* ---- init ------------------------------------------------------------- */
static void ai_resolve(void)
{
    orig_readi     = dlsym(RTLD_NEXT, "snd_pcm_readi");
    orig_readn     = dlsym(RTLD_NEXT, "snd_pcm_readn");
    orig_hw_params = dlsym(RTLD_NEXT, "snd_pcm_hw_params");
    q_get_format   = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_format");
    q_get_channels = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_channels");
    q_get_rate     = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_rate");
    q_pcm_name     = dlsym(RTLD_NEXT, "snd_pcm_name");
}

static inline void ensure_init(void) { pthread_once(&g_once, ai_resolve); }

/* Try to attach voice `slot`'s ring if it isn't already - OR re-attach it if
 * the ring it was attached to has been replaced by a fresh one. Used both at
 * constructor time (all 4 slots, once) and from the background thread below
 * (every cycle, all slots) - this is the whole point of lazy re-attach: a
 * voice host started AFTER MPC (no acvs restart needed, e.g. the nodeServer
 * Modules-page toggle) gets picked up on the next background-thread wake
 * instead of never being noticed at all. Not called from the audio thread -
 * stat/shm_open/mmap are real syscalls, deliberately kept off the hot path
 * per this file's own safety contract.
 *
 * Re-attach case (2026-09-13): a voice host that's stopped and restarted
 * (e.g. toggled off/on via the nodeServer Modules page) very plausibly
 * shm_unlink()s and recreates its ring from scratch on each start (maze_host
 * does, "start clean") - a NEW inode under the SAME name. Without this
 * check, `already attached` above would stay true forever after the first
 * attach, silently mixing from an orphaned, no-longer-written-to ring - no
 * crash, just silence. So even an "already attached" slot gets its current
 * on-disk identity checked (a cheap stat by path) and re-attached if it
 * changed. The OLD mapping is deliberately never munmap'd here - the audio
 * thread's mix_in() may be off in the middle of reading it via its own
 * acquire-load of g_shm[slot] at the exact moment we'd swap the pointer, and
 * this file's whole design keeps syscalls (munmap included) off that hot
 * path; leaking one ~512KB mapping per voice-restart is a deliberate,
 * bounded tradeoff against ever risking a use-after-unmap there. */
static void ai_try_attach(unsigned slot)
{
    char name[24];
    ai_shm_name(slot, name, sizeof(name));

    /* ai_shm_name() returns a name like "/forceAudioInject0" - POSIX shm
     * objects are visible by that same leaf name under /dev/shm, so a plain
     * stat-by-path is enough to check identity without opening anything. */
    char path[40];
    snprintf(path, sizeof(path), "/dev/shm%s", name);
    struct stat st;
    int have_stat = (stat(path, &st) == 0);

    ai_shm_t *cur = __atomic_load_n(&g_shm[slot], __ATOMIC_ACQUIRE);
    if (cur) {
        if (!have_stat || st.st_ino == g_shm_ino[slot]) return;  /* unchanged, or gone - nothing to do */
        ai_log("[forceAudioIn] voice slot %u's ring was replaced (inode %llu -> %llu) - re-attaching",
               slot, (unsigned long long)g_shm_ino[slot], (unsigned long long)st.st_ino);
    } else if (!have_stat) {
        return;   /* not present yet - not an error, just try again next cycle */
    }

    int fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) return;   /* raced with the producer unlinking it again - try next cycle */
    void *m = mmap(NULL, AI_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        ai_log("[forceAudioIn] mmap failed for %s - voice slot %u passthrough-only", name, slot);
        return;
    }
    ai_shm_t *shm = (ai_shm_t *)m;
    if (shm->magic != AI_MAGIC) {
        ai_log("[forceAudioIn] bad magic in %s - ignoring", name);
        munmap(m, AI_SHM_BYTES);
        return;
    }

    /* Publish the pointer LAST, with release semantics, only after the
     * struct is fully mapped and magic-verified - mix_in() on the audio
     * thread loads g_shm[slot] with acquire semantics (see below), so it
     * either sees NULL/the old valid pointer, or the new fully-valid one,
     * never a half-published one. */
    g_shm_ino[slot] = st.st_ino;
    __atomic_store_n(&g_shm[slot], shm, __ATOMIC_RELEASE);
    if (!cur) __atomic_fetch_add(&g_n_attached, 1, __ATOMIC_RELAXED);
    ai_log("[forceAudioIn] voice slot %u attached: %s, %u Hz, %u ch", slot, name, shm->rate, shm->channels);
}

/* Background thread: wakes every ~2s. Two jobs, both off the hot path:
 *   1. Lazy re-attach - retry any slot that was empty at constructor time
 *      (or still is), so a voice host started after MPC comes up gets
 *      picked up without needing another acvs restart. This is now the
 *      reason this thread exists unconditionally (see ai_ctor) - it used
 *      to be diagnostics-only and gated off by default (2026-09-13 open
 *      incident, see DESIGN.md); that gate now only controls job 2 below,
 *      since lazy re-attach needs this thread to always run to do its job.
 *   2. Diagnostics logging - unchanged from before, still gated behind
 *      AI_DIAG_MARKER so it stays off unless explicitly wanted for
 *      debugging a live session. */
static void *bg_main(void *unused)
{
    (void)unused;
    for (;;) {
        struct timespec ts = {2, 0};
        nanosleep(&ts, NULL);

        unsigned slot;
        for (slot = 0; slot < AI_MAX_VOICES; slot++)
            ai_try_attach(slot);

        if (access(AI_DIAG_MARKER, F_OK) != 0) continue;

        if (g_announce_claim) {
            g_announce_claim = 0;
            ai_log("[forceAudioIn] tapping capture handle");
        }
        int i;
        for (i = 0; i < MAX_PCMS; i++) {
            if (!g_caps[i].pcm) continue;
            ai_log("[forceAudioIn] handle %d: %u ch %u Hz, %llu frames read (%s)",
                   i, g_caps[i].channels, g_caps[i].rate,
                   (unsigned long long)g_caps[i].frames,
                   (g_caps[i].pcm == g_tap_pcm) ? "TAPPED" : "idle");
        }
        for (i = 0; i < AI_MAX_VOICES; i++) {
            ai_shm_t *shm = __atomic_load_n(&g_shm[i], __ATOMIC_ACQUIRE);
            if (!shm) continue;
            uint32_t h = __atomic_load_n(&shm->head, __ATOMIC_ACQUIRE);
            uint32_t t = shm->tail;
            uint32_t backlog = (h - t) & (AI_RING_FRAMES - 1);
            ai_log("[forceAudioIn] voice %d: backlog %u frames (%.1fms) trims %llu ev/%llu fr, "
                   "enabled=%u gain=%.3f chan=%u, %u Hz %u ch, produced %llu consumed %llu underruns %llu",
                   i, backlog, backlog * 1000.0 / (shm->rate ? shm->rate : 44100),
                   (unsigned long long)g_trim_events[i], (unsigned long long)g_trim_frames[i],
                   shm->enabled, shm->gain, shm->channel_mask, shm->rate, shm->channels,
                   (unsigned long long)shm->frames_written,
                   (unsigned long long)shm->frames_consumed,
                   (unsigned long long)shm->underruns);
        }
    }
    return NULL;
}

/* Controlled experiment, 2026-09-13 (see DESIGN.md open incident): a live
 * test that accidentally ran `systemctl restart acvs` in the background
 * with a concurrent ps-polling loop - extra CPU/scheduling activity during
 * MPC's startup that was absent from every prior test - was the first
 * "voice already attached" restart to NOT kill pads/wifi, after that exact
 * repro shape had failed with zero exceptions across several prior tests.
 * That's a real, if accidental, timing perturbation flipping a previously
 * 100%-reproducible failure - strong evidence this is a race, not a fixed
 * logical bug. Rather than ask for the same accident to be repeated
 * (uncontrolled - we don't know if it was the extra CPU load, the process
 * creation, /proc access from ps, or something else about that shell
 * pipeline), this is a deliberate, controlled version of the same
 * manipulated variable: an opt-in, marker-gated delay at the very start of
 * this constructor, before any attach work happens. If a plain delay alone
 * reproduces the fix across MULTIPLE restarts (not the n=1 the accident
 * gave us), that's clean confirmation of a race resolved by not running
 * this constructor's work "too fast" relative to something else's own
 * startup - and a far better workaround than keeping a polling loop
 * running forever. If it doesn't help, that's equally informative: it
 * rules out simple "we're just too fast" and points back toward something
 * more specific about what ps/scheduling itself perturbed.
 *
 * Marker file content is the delay in milliseconds (e.g. "300"); present
 * but empty or unparseable defaults to 250ms. Absent = no delay (today's
 * default, unchanged behavior). Same file-not-env-var reasoning as
 * AI_DIAG_MARKER - no practical way to set an env var in MPC's own exec
 * environment. */
#define AI_CTOR_DELAY_MARKER "/tmp/forceAudioIn.delay"

static void ai_maybe_delay(void)
{
    FILE *f = fopen(AI_CTOR_DELAY_MARKER, "r");
    if (!f) return;
    long ms = 250;
    char buf[32];
    if (fgets(buf, sizeof(buf), f)) {
        long v = strtol(buf, NULL, 10);
        if (v > 0) ms = v;
    }
    fclose(f);
    ai_log("[forceAudioIn] AI_CTOR_DELAY_MARKER present - delaying constructor %ldms before any attach work", ms);
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

__attribute__((constructor))
static void ai_ctor(void)
{
    ai_maybe_delay();
    ai_log("[forceAudioIn] loaded into pid %d", (int)getpid());

    unsigned slot;
    for (slot = 0; slot < AI_MAX_VOICES; slot++)
        ai_try_attach(slot);
    ai_log("[forceAudioIn] %u voice(s) attached at load", g_n_attached);

    /* Unconditional, unlike before: this thread now also does lazy
     * re-attach (see ai_try_attach/bg_main above), which has to run
     * regardless of whether any voice happened to exist yet at load time -
     * that's the whole point. The AI_DIAG_MARKER gate still controls
     * whether it also logs. */
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, bg_main, NULL);
    pthread_attr_destroy(&attr);
}

/* Is destination channel `c` (of `dst_channels` total) one this voice should
 * land on, given its `mask` (AI_CHAN_L / AI_CHAN_R / AI_CHAN_LR)? The tapped
 * capture handle is confirmed 2-channel in practice (see DESIGN.md), so
 * channel 0 = L, channel 1 = R; a mono destination gets the voice if either
 * is selected; anything beyond stereo only gets it when both are (there is
 * no real "3rd channel" for a voice to be routed to on its own). */
static inline int chan_allowed(unsigned c, uint32_t mask, unsigned dst_channels)
{
    if (dst_channels == 1) return mask != 0;
    if (c == 0) return (mask & AI_CHAN_L) != 0;
    if (c == 1) return (mask & AI_CHAN_R) != 0;
    return mask == AI_CHAN_LR;
}

/* ---- the audio-thread hot path ------------------------------------------
 * Pops up to `frames` frames from one voice's ring (converting its fixed
 * float32/channels shape to the capture handle's real format/channel count)
 * and ADDS them into `buffer`, which already holds real captured audio plus
 * whatever earlier voices in this call already mixed in. `slot` indexes
 * g_trim_events/g_trim_frames, the only per-voice state that lives outside
 * shared memory (diagnostics only). */
static inline void mix_in_one(unsigned slot, ai_shm_t *shm, unsigned char *buffer, size_t frames,
                              unsigned dst_channels, unsigned dst_frame_bytes, int dst_format)
{
    if (!shm || !frames) return;

    uint32_t head = __atomic_load_n(&shm->head, __ATOMIC_ACQUIRE);
    uint32_t tail = shm->tail;                          /* we are the only consumer */
    uint32_t avail = (head - tail) & (AI_RING_FRAMES - 1);

    /* Bound latency: a live-audio producer (e.g. maze_host) necessarily
     * starts filling this ring before MPC's process - and therefore this
     * constructor - even exists (it has to win that race), and any tiny
     * clock-rate mismatch between the producer's wall-clock timer and this
     * consumer's real ALSA-clocked read rate accumulates over time either
     * way. Left unchecked, backlog grows toward the full ring (~1.5s) and
     * every note plays back that far behind wall-clock time. We are the
     * ring's sole tail-writer, so it's safe to skip forward past stale
     * backlog here rather than ever letting playback run behind live. */
    if (avail > AI_LATENCY_TRIGGER_FRAMES) {
        uint32_t skip = avail - AI_LATENCY_TARGET_FRAMES;
        tail = (tail + skip) & (AI_RING_FRAMES - 1);
        avail = AI_LATENCY_TARGET_FRAMES;
        g_trim_events[slot]++;
        g_trim_frames[slot] += skip;
    }

    uint32_t take = (uint32_t)frames;
    if (take > avail) {
        shm->underruns++;
        take = avail;                                   /* mix what we have, then stop */
    }
    if (!take) return;

    /* Muted: still drain the ring at the normal rate (above) so a re-enabled
     * voice resumes from live backlog, not a stale one - just skip adding
     * its samples into the output. The producer's render cadence is the
     * synth's actual clock (envelopes/filters advance every render_block
     * call) and must never be told to skip a tick, so mute can only happen
     * here, at the consumer, never by pausing the producer. */
    uint32_t enabled = shm->enabled;   /* plain volatile read - see forceAudioInject.h */
    if (enabled) {
        float gain = shm->gain;
        uint32_t mask = shm->channel_mask;
        unsigned src_ch = shm->channels ? shm->channels : 1;
        unsigned dw = width_of(dst_format);
        uint32_t i;

        for (i = 0; i < take; i++) {
            uint32_t ring_frame = (tail + i) & (AI_RING_FRAMES - 1);
            const float *src = &shm->ring[(size_t)ring_frame * AI_MAX_CH];

            unsigned c;
            for (c = 0; c < dst_channels; c++) {
                if (!chan_allowed(c, mask, dst_channels)) continue;
                /* Mono injector -> duplicate to every destination channel.
                 * Stereo injector -> map channel c mod src_ch (covers dst
                 * mono by folding L+R would need averaging; duplicating ch0
                 * is the simple, good-enough default for a mono/stereo
                 * generator). */
                float sv = (src_ch >= 2) ? src[c % src_ch] : src[0];
                sv *= gain;
                size_t off = (size_t)i * dst_frame_bytes + (size_t)c * dw;
                float real = sample_to_float(buffer, off, dst_format);
                float_to_sample(buffer, off, dst_format, real + sv);
            }
        }
    }

    __atomic_store_n(&shm->tail, (tail + take) & (AI_RING_FRAMES - 1), __ATOMIC_RELEASE);
    shm->frames_consumed += take;
}

/* Sums every attached voice's ring into `buffer` - see mix_in_one for the
 * per-voice mechanics. */
static inline void mix_in(unsigned char *buffer, size_t frames,
                          unsigned dst_channels, unsigned dst_frame_bytes, int dst_format)
{
    /* Relaxed: this is just a fast-path short-circuit hint, not a
     * synchronization point - the real one is the per-slot pointer load
     * below. Being stale by one call (a voice attached moments ago not yet
     * observed here) just means one extra ALSA read passes through
     * untouched; it self-corrects immediately next call. */
    if (!__atomic_load_n(&g_n_attached, __ATOMIC_RELAXED) || !frames) return;
    unsigned slot;
    for (slot = 0; slot < AI_MAX_VOICES; slot++) {
        ai_shm_t *shm = __atomic_load_n(&g_shm[slot], __ATOMIC_ACQUIRE);
        if (shm)
            mix_in_one(slot, shm, buffer, frames, dst_channels, dst_frame_bytes, dst_format);
    }
}

int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    ensure_init();
    if (!orig_hw_params) return -1;
    int r = orig_hw_params(pcm, params);
    if (r == 0) {
        int fmt = 14; unsigned ch = 2, rate = 44100, dir = 0;
        if (q_get_format)   q_get_format(params, &fmt);
        if (q_get_channels) q_get_channels(params, &ch);
        if (q_get_rate)     q_get_rate(params, &rate, (int *)&dir);
        if (ch >= 1 && ch <= 32 && rate >= 8000 && rate <= 192000) {
            int slot = -1, i;
            for (i = 0; i < MAX_PCMS; i++) {
                if (g_caps[i].pcm == (void *)pcm) { slot = i; break; }
                if (!g_caps[i].pcm && slot < 0) slot = i;
            }
            if (slot >= 0) {
                g_caps[slot].pcm = (void *)pcm;
                g_caps[slot].channels = ch;
                g_caps[slot].rate = rate;
                g_caps[slot].format = fmt;
                g_caps[slot].frame_bytes = ch * width_of(fmt);
            }
            ai_log("[forceAudioIn] configured capture %s: %u Hz, %u ch, fmt %d (%u B/frame)",
                   (q_pcm_name && pcm) ? q_pcm_name(pcm) : "?", rate, ch, fmt, ch * width_of(fmt));
        }
    }
    return r;
}

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t size)
{
    ensure_init();
    if (!orig_readi) return -1;
    snd_pcm_sframes_t r = orig_readi(pcm, buffer, size);
    if (r > 0) {
        int i, slot = -1;
        for (i = 0; i < MAX_PCMS; i++)
            if (g_caps[i].pcm == (void *)pcm) { slot = i; break; }
        if (slot < 0) return r;                         /* shape unknown - never guess */

        g_caps[slot].frames += (uint64_t)r;

        if (!g_tap_pcm) {
            g_tap_pcm = (void *)pcm;
            g_announce_claim = 1;
        }
        if ((void *)pcm != g_tap_pcm) return r;          /* a second handle - ignore it */

        mix_in((unsigned char *)buffer, (size_t)r,
               g_caps[slot].channels, g_caps[slot].frame_bytes, g_caps[slot].format);
    }
    return r;
}

snd_pcm_sframes_t snd_pcm_readn(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t size)
{
    ensure_init();
    if (!orig_readn) return -1;
    /* Non-interleaved: pass through untouched, same call this project made
     * for snd_pcm_writen in forceStream.so - MPC uses the interleaved path
     * in practice, and mixing per-plane here would cost more than it's
     * worth on the audio thread until proven otherwise. */
    return orig_readn(pcm, bufs, size);
}
