/*
 * forceAudioJack.so — LD_PRELOAD audio injector for the Akai Force's MPC
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
 *       -o forceAudioJack.so forceAudioJack.c -lpthread -lrt
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
#include "forceAudioJackExtract.h"

/* Opaque ALSA types - we never dereference these, so no ALSA headers needed. */
typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

#define LOG_PATH "/tmp/forceAudioJack.log"

/* Open incident 2026-09-13 (see DESIGN.md for the full live-tested
 * elimination sequence): pads/buttons go dead partway through repeated
 * acvs restarts (typically the 2nd or 3rd, not the 1st) whenever a voice
 * is actually attached - LD_PRELOAD content confirmed correct throughout,
 * so NOT the known file-race. Ruled out so far, each with a live test:
 * an ALSA symbol collision with MidiLoop's tkgl_anyctrl_lt.so (disjoint
 * symbol sets); the diagnostics thread's mere existence (still failed with
 * it confirmed not spawning); forceAudioJack.so merely being loaded with zero
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
 * forceAudioJack.so is LD_PRELOAD'd into MPC by a boot script, not launched
 * directly, so there's no practical way to set an environment variable in
 * MPC's own exec environment - a file checked once at library-load time is
 * trivially toggleable over SSH with no rebuild and no boot-script change. */
#define AI_DIAG_MARKER "/tmp/forceAudioJack.diag"

static void ai_log(const char *fmt, ...);   /* defined below; forward-declared for ai_dump_events */
static void *bg_main(void *unused);         /* defined below; forward-declared for ai_resolve() */
static void ai_try_attach(unsigned slot);          /* defined below; forward-declared for ai_resolve() */
static void ai_try_attach_out(unsigned slot);      /* defined below; forward-declared for ai_resolve() */
static void ax_try_attach_skipback(void);          /* defined below; forward-declared for ai_resolve() */

/* ---- lightweight in-process event trace (2026-09-17) --------------------
 * Added after three separate live attempts to observe the open pads-death
 * incident above via external strace all failed to reproduce it - the
 * leading theory (see DESIGN.md) being that ptrace's own overhead perturbs
 * the race enough to avoid it every single time it's attached, in any form
 * tried so far. This replaces external tracing with self-instrumentation: a
 * fixed-size ring of tiny event records, written with a single atomic
 * increment and no syscalls on the hot path other than clock_gettime (which
 * is vDSO-backed where available, and which the process would effectively
 * need anyway) - nothing here pays for an external tracer's per-syscall
 * context switch into and out of a separate process.
 *
 * Flushed to a file only on request (AI_DUMP_MARKER - same file-trigger
 * pattern as AI_DIAG_MARKER below, since there's no way to reach an env
 * var in MPC's own exec environment). Confirmed live
 * (2026-09-17): MPC itself does NOT crash when pads go dead, it stays
 * running, just unresponsive - so a dump can be requested well AFTER
 * physically confirming the failure, no need to catch anything in flight. */
#define AI_DUMP_MARKER "/tmp/forceAudioJack.dumpreq"
#define AI_EVT_CAP     65536u   /* ring capacity, indexed mod this (power of 2) */

enum {
    AI_EVT_CTOR_START = 1,
    AI_EVT_CTOR_DELAY,
    AI_EVT_ATTACH_TRY,
    AI_EVT_ATTACHED,
    AI_EVT_REATTACHED,
    AI_EVT_CTOR_DONE,
    AI_EVT_THREAD_CREATED,
    AI_EVT_HW_PARAMS,
    AI_EVT_FIRST_READ,
    AI_EVT_READI,     /* one per snd_pcm_readi call on the tapped handle - the "heartbeat" */
    AI_EVT_MIX_ONE,    /* one per attached voice mixed into a given readi call */
    AI_EVT_TRIM,
    AI_EVT_UNDERRUN,
    AI_EVT_BG_WAKE,
    /* Out-bus (physical Out 3/4 injection) events, added alongside the
     * snd_pcm_writei hook. ATTACH_TRY/ATTACHED/REATTACHED/MIX_ONE/TRIM/
     * UNDERRUN above are reused for out-bus slots too (d16 carries the slot
     * number offset by AI_MAX_VOICES, so a dump reader can tell which bus a
     * given slot event belongs to from the number alone) - only the two
     * call-site markers below, which mark a genuinely different hook
     * (writei vs readi) rather than just a different slot, get their own
     * codes. */
    AI_EVT_FIRST_WRITE,
    AI_EVT_WRITEI,
};

typedef struct {
    uint64_t ts_ns;
    uint32_t tid;
    uint16_t code;
    uint16_t d16;
    uint32_t d32;
} ai_evt_t;

static ai_evt_t          g_evt[AI_EVT_CAP];
static volatile uint64_t g_evt_next = 0;   /* monotonically increasing; index is this mod AI_EVT_CAP */

static inline uint64_t ai_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void ai_evt(uint16_t code, uint16_t d16, uint32_t d32)
{
    uint64_t idx = __atomic_fetch_add(&g_evt_next, 1, __ATOMIC_RELAXED) & (AI_EVT_CAP - 1);
    ai_evt_t *e = &g_evt[idx];
    e->ts_ns = ai_now_ns();
    e->tid   = (uint32_t)gettid();
    e->code  = code;
    e->d16   = d16;
    e->d32   = d32;
}

static const char *ai_evt_name(uint16_t code)
{
    switch (code) {
        case AI_EVT_CTOR_START:     return "CTOR_START";
        case AI_EVT_CTOR_DELAY:     return "CTOR_DELAY_MS";
        case AI_EVT_ATTACH_TRY:     return "ATTACH_TRY_SLOT";
        case AI_EVT_ATTACHED:       return "ATTACHED_SLOT_INO";
        case AI_EVT_REATTACHED:     return "REATTACHED_SLOT_INO";
        case AI_EVT_CTOR_DONE:      return "CTOR_DONE_NATTACHED";
        case AI_EVT_THREAD_CREATED: return "BG_THREAD_CREATED";
        case AI_EVT_HW_PARAMS:      return "HW_PARAMS_CH_RATE";
        case AI_EVT_FIRST_READ:     return "FIRST_READ_TAP_CLAIMED";
        case AI_EVT_READI:          return "READI_SLOT_FRAMES";
        case AI_EVT_MIX_ONE:        return "MIX_ONE_SLOT_AVAIL";
        case AI_EVT_TRIM:           return "TRIM_SLOT_SKIPFRAMES";
        case AI_EVT_UNDERRUN:       return "UNDERRUN_SLOT_AVAIL";
        case AI_EVT_BG_WAKE:        return "BG_WAKE_TICK";
        case AI_EVT_FIRST_WRITE:    return "FIRST_WRITE_TAP_CLAIMED";
        case AI_EVT_WRITEI:         return "WRITEI_SLOT_FRAMES";
        default:                    return "?";
    }
}

/* Dumps the ring in chronological order to a fresh timestamped-by-pid file.
 * Only ever called from bg_main's own thread in response to AI_DUMP_MARKER -
 * never from the hot path or a signal handler - so plain stdio is fine. */
static void ai_dump_events(void)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/forceAudioJack.dump.%d", (int)getpid());
    FILE *f = fopen(path, "w");
    if (!f) return;

    uint64_t next  = __atomic_load_n(&g_evt_next, __ATOMIC_RELAXED);
    uint64_t count = next < AI_EVT_CAP ? next : AI_EVT_CAP;
    uint64_t start = next < AI_EVT_CAP ? 0 : next;   /* oldest surviving slot, if the ring has wrapped */

    fprintf(f, "# forceAudioJack event dump - pid %d, %llu events (capacity %u)\n",
            (int)getpid(), (unsigned long long)count, AI_EVT_CAP);
    fprintf(f, "# ts_ns tid event d16 d32\n");

    uint64_t i;
    for (i = 0; i < count; i++) {
        uint64_t idx = (start + i) & (AI_EVT_CAP - 1);
        ai_evt_t *e = &g_evt[idx];
        fprintf(f, "%llu %u %s %u %u\n",
                (unsigned long long)e->ts_ns, e->tid, ai_evt_name(e->code), e->d16, e->d32);
    }
    fclose(f);
    ai_log("[forceAudioJack] event dump written to %s (%llu events)", path, (unsigned long long)count);
}

/* ---- originals ----------------------------------------------------------*/
static snd_pcm_sframes_t (*orig_readi)(snd_pcm_t *, void *, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*orig_readn)(snd_pcm_t *, void **, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*orig_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
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

/* Out-bus (physical Out 3/4 injection) - same shape as the In-bus state
 * above, distinct arrays/namespace (see AI_SHM_NAME_FMT_OUT in
 * forceAudioInject.h). Kept as parallel arrays rather than widening the
 * existing ones to AI_MAX_VOICES+AI_MAX_OUT_VOICES so every existing
 * In-bus call site (mix_in, the diagnostics loop, the test suite) keeps
 * indexing g_shm[] exactly as it always has, with zero risk of an
 * off-by-one dragging an out-bus slot into the in-bus mix path by mistake. */
static ai_shm_t *g_shm_out[AI_MAX_OUT_VOICES];
static unsigned  g_n_attached_out = 0;
static ino_t     g_shm_out_ino[AI_MAX_OUT_VOICES];

/* Skipback extraction ring - forceAudioJack.so is the PRODUCER here (see
 * forceAudioJackExtract.h). skipbackHost creates the segment on demand (its
 * own Modules-page toggle, same rule as every voice host); this is the
 * lazy-attach state for it, same shape as the arrays above but singular -
 * there is exactly one skipback consumer, not a slotted array of voices. */
static ax_shm_t *g_ax_skipback = NULL;
static ino_t     g_ax_skipback_ino;

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
static uint64_t g_trim_events_out[AI_MAX_OUT_VOICES];
static uint64_t g_trim_frames_out[AI_MAX_OUT_VOICES];

/* ---- per-handle capture shape, same pattern as forceStream.so's g_pcms[] */
#define MAX_PCMS 8
static struct {
    void *pcm;
    unsigned frame_bytes, channels, rate;
    int format;
    volatile uint64_t frames;
} g_caps[MAX_PCMS];
static volatile void *g_tap_pcm = NULL;   /* the one capture handle we mix into */
static volatile void *g_tap_pcm_out = NULL;   /* the one playback handle we inject into */

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static volatile int g_announce_claim = 0;
static volatile int g_announce_claim_out = 0;

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
/* ROOT CAUSE FOUND 2026-09-22 (see the long historical comment further down,
 * near where this used to run from __attribute__((constructor)), for the
 * full incident this closes): this library's own background thread was
 * being created from a constructor, which runs at library-LOAD time - i.e.
 * potentially concurrently with OTHER shared libraries' own constructors
 * and MPC's/JUCE's own static initializers, all still running on the main
 * thread, before MPC's main() has even started. Starting a thread that
 * early is a well-known class of bug: any global/static state that isn't
 * yet safe for concurrent access (because whoever owns it hasn't finished
 * single-threaded startup) is now racing against it. Confirmed live: with
 * this library's constructor doing exactly this, MPC crashed with
 * 'cereal::RapidJSONException' - an exception thrown deep inside MPC's own
 * settings/project JSON loading, a code path with no direct relationship
 * to anything this library does - on 100% of the (rare) boots where this
 * library actually got loaded, across ~20 consecutive attempts.
 *
 * The fix: do ALL of this library's own setup here, in ai_resolve(), which
 * runs via ensure_init()'s pthread_once - triggered by MPC's own FIRST
 * call into an interposed ALSA function (snd_pcm_hw_params/readi/writei),
 * not by library-load time. By the time MPC makes that first call, it has
 * necessarily finished its own startup (opened its audio codec, which only
 * happens well into its own main()) - every other library's constructors
 * and MPC's own static initializers are long done, so spinning up a
 * background thread here can never race any of that. Same reasoning as
 * why the dlsym() resolution below was already deferred this way from the
 * start - this just extends it to the attach-loops and thread creation
 * that used to run separately, and earlier, in the library constructor. */
static void ai_resolve(void)
{
    orig_readi     = dlsym(RTLD_NEXT, "snd_pcm_readi");
    orig_readn     = dlsym(RTLD_NEXT, "snd_pcm_readn");
    orig_writei    = dlsym(RTLD_NEXT, "snd_pcm_writei");
    orig_hw_params = dlsym(RTLD_NEXT, "snd_pcm_hw_params");
    q_get_format   = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_format");
    q_get_channels = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_channels");
    q_get_rate     = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_rate");
    q_pcm_name     = dlsym(RTLD_NEXT, "snd_pcm_name");

    ai_evt(AI_EVT_CTOR_START, 0, 0);
    ai_log("[forceAudioJack] loaded into pid %d", (int)getpid());

    unsigned slot;
    for (slot = 0; slot < AI_MAX_VOICES; slot++)
        ai_try_attach(slot);
    for (slot = 0; slot < AI_MAX_OUT_VOICES; slot++)
        ai_try_attach_out(slot);
    ax_try_attach_skipback();
    ai_log("[forceAudioJack] %u voice(s) attached at load (%u out-bus)", g_n_attached, g_n_attached_out);
    ai_evt(AI_EVT_CTOR_DONE, 0, g_n_attached);

    /* Safe to create here, unlike from a library constructor - see the
     * long comment above this function for exactly why. */
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, bg_main, NULL);
    pthread_attr_destroy(&attr);
    ai_evt(AI_EVT_THREAD_CREATED, 0, 0);
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
/* Generic attach/re-attach for one bus's array of slots. `is_out` only
 * controls which shm namespace to look in (ai_shm_name vs ai_shm_name_out)
 * and the event-trace slot offset (see AI_EVT_FIRST_WRITE's comment above) -
 * everything else (identity-by-inode re-attach, never munmap'ing the old
 * mapping, publishing the pointer last with release semantics) is identical
 * between the two buses. */
static void ai_try_attach_generic(unsigned slot, ai_shm_t **shm_arr, ino_t *ino_arr,
                                   unsigned *attached_counter, int is_out)
{
    uint16_t evt_slot = (uint16_t)(slot + (is_out ? AI_MAX_VOICES : 0));
    ai_evt(AI_EVT_ATTACH_TRY, evt_slot, 0);

    char name[24];
    if (is_out) ai_shm_name_out(slot, name, sizeof(name));
    else        ai_shm_name(slot, name, sizeof(name));

    /* ai_shm_name()/ai_shm_name_out() return a name like
     * "/forceAudioInject0" or "/forceAudioJackOut0" - POSIX shm objects are
     * visible by that same leaf name under /dev/shm, so a plain stat-by-path
     * is enough to check identity without opening anything. */
    char path[40];
    snprintf(path, sizeof(path), "/dev/shm%s", name);
    struct stat st;
    int have_stat = (stat(path, &st) == 0);

    ai_shm_t *cur = __atomic_load_n(&shm_arr[slot], __ATOMIC_ACQUIRE);
    int is_reattach = (cur != NULL);
    if (cur) {
        if (!have_stat || st.st_ino == ino_arr[slot]) return;  /* unchanged, or gone - nothing to do */
        ai_log("[forceAudioJack] %s slot %u's ring was replaced (inode %llu -> %llu) - re-attaching",
               is_out ? "out-bus voice" : "voice", slot,
               (unsigned long long)ino_arr[slot], (unsigned long long)st.st_ino);
    } else if (!have_stat) {
        return;   /* not present yet - not an error, just try again next cycle */
    }

    int fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) return;   /* raced with the producer unlinking it again - try next cycle */
    void *m = mmap(NULL, AI_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        ai_log("[forceAudioJack] mmap failed for %s - %s slot %u passthrough-only",
               name, is_out ? "out-bus voice" : "voice", slot);
        return;
    }
    ai_shm_t *shm = (ai_shm_t *)m;
    if (shm->magic != AI_MAGIC) {
        ai_log("[forceAudioJack] bad magic in %s - ignoring", name);
        munmap(m, AI_SHM_BYTES);
        return;
    }

    /* Publish the pointer LAST, with release semantics, only after the
     * struct is fully mapped and magic-verified - mix_in()/mix_out() on the
     * audio thread load shm_arr[slot] with acquire semantics, so they either
     * see NULL/the old valid pointer, or the new fully-valid one, never a
     * half-published one. */
    ino_arr[slot] = st.st_ino;
    __atomic_store_n(&shm_arr[slot], shm, __ATOMIC_RELEASE);
    if (!cur) __atomic_fetch_add(attached_counter, 1, __ATOMIC_RELAXED);
    ai_evt(is_reattach ? AI_EVT_REATTACHED : AI_EVT_ATTACHED, evt_slot, (uint32_t)st.st_ino);
    ai_log("[forceAudioJack] %s slot %u attached: %s, %u Hz, %u ch",
           is_out ? "out-bus voice" : "voice", slot, name, shm->rate, shm->channels);
}

static inline void ai_try_attach(unsigned slot)
{
    ai_try_attach_generic(slot, g_shm, g_shm_ino, &g_n_attached, 0);
}

static inline void ai_try_attach_out(unsigned slot)
{
    ai_try_attach_generic(slot, g_shm_out, g_shm_out_ino, &g_n_attached_out, 1);
}

/* Attach (or re-attach) the skipback extraction ring. Same identity-by-inode
 * re-attach logic as ai_try_attach_generic above (skipbackHost restarting is
 * exactly analogous to a voice host restarting: it may shm_unlink()+recreate
 * on every start), just for a single fixed segment where forceAudioJack.so is
 * the PRODUCER instead of the consumer - so there's no "voice enabled/gain"
 * state to read, only the ring header and rate/channels. */
static void ax_try_attach_skipback(void)
{
    const char *name = AX_SHM_NAME_SKIPBACK;
    char path[48];
    snprintf(path, sizeof(path), "/dev/shm%s", name);
    struct stat st;
    int have_stat = (stat(path, &st) == 0);

    ax_shm_t *cur = __atomic_load_n(&g_ax_skipback, __ATOMIC_ACQUIRE);
    int is_reattach = (cur != NULL);
    if (cur) {
        if (!have_stat || st.st_ino == g_ax_skipback_ino) return;
        ai_log("[forceAudioJack] skipback ring was replaced (inode %llu -> %llu) - re-attaching",
               (unsigned long long)g_ax_skipback_ino, (unsigned long long)st.st_ino);
    } else if (!have_stat) {
        return;   /* skipbackHost not running - not an error, just try again next cycle */
    }

    int fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) return;
    void *m = mmap(NULL, AX_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        ai_log("[forceAudioJack] mmap failed for %s - skipback passthrough-only", name);
        return;
    }
    ax_shm_t *ax = (ax_shm_t *)m;
    if (ax->magic != AX_MAGIC) {
        ai_log("[forceAudioJack] bad magic in %s - ignoring", name);
        munmap(m, AX_SHM_BYTES);
        return;
    }

    g_ax_skipback_ino = st.st_ino;
    __atomic_store_n(&g_ax_skipback, ax, __ATOMIC_RELEASE);
    ai_log("[forceAudioJack] skipback ring %s: %s, %u Hz, %u ch",
           is_reattach ? "re-attached" : "attached", name, ax->rate, ax->channels);
}

/* Background thread: three jobs, all off the hot path:
 *   1. Lazy re-attach - retry any slot that was empty when ai_resolve()
 *      first ran (or still is), so a voice host started after MPC comes up
 *      gets picked up without needing another acvs restart. This is now the
 *      reason this thread exists unconditionally (see ai_resolve() above,
 *      which is what creates it) - it used
 *      to be diagnostics-only and gated off by default (2026-09-13 open
 *      incident, see DESIGN.md); that gate now only controls job 2 below,
 *      since lazy re-attach needs this thread to always run to do its job.
 *      Still on the original ~2s cadence (every 10th 200ms tick below).
 *   2. Diagnostics logging - unchanged from before, still gated behind
 *      AI_DIAG_MARKER so it stays off unless explicitly wanted for
 *      debugging a live session. Same ~2s cadence as job 1.
 *   3. Event-dump polling (2026-09-17) - checks AI_DUMP_MARKER every ~200ms,
 *      independent of the ~2s cadence above, so a requested dump comes back
 *      promptly rather than waiting up to 2s. This is the only reason the
 *      sleep granularity dropped from 2s to 200ms - jobs 1/2 are unchanged
 *      in real-world frequency, just gated by a tick counter now instead of
 *      being the sleep duration itself. */
static void *bg_main(void *unused)
{
    (void)unused;
    unsigned tick = 0;
    for (;;) {
        struct timespec ts = {0, 200000000L};
        nanosleep(&ts, NULL);
        tick++;

        if (access(AI_DUMP_MARKER, F_OK) == 0) {
            ai_dump_events();
            unlink(AI_DUMP_MARKER);
        }

        if (tick % 10 != 0) continue;   /* jobs 1/2 below stay on the original ~2s cadence */
        ai_evt(AI_EVT_BG_WAKE, 0, tick);

        unsigned slot;
        for (slot = 0; slot < AI_MAX_VOICES; slot++)
            ai_try_attach(slot);
        for (slot = 0; slot < AI_MAX_OUT_VOICES; slot++)
            ai_try_attach_out(slot);
        ax_try_attach_skipback();

        if (access(AI_DIAG_MARKER, F_OK) != 0) continue;

        if (g_announce_claim) {
            g_announce_claim = 0;
            ai_log("[forceAudioJack] tapping capture handle");
        }
        if (g_announce_claim_out) {
            g_announce_claim_out = 0;
            ai_log("[forceAudioJack] tapping playback handle (out-bus)");
        }
        int i;
        for (i = 0; i < MAX_PCMS; i++) {
            if (!g_caps[i].pcm) continue;
            ai_log("[forceAudioJack] handle %d: %u ch %u Hz, %llu frames (%s)",
                   i, g_caps[i].channels, g_caps[i].rate,
                   (unsigned long long)g_caps[i].frames,
                   (g_caps[i].pcm == g_tap_pcm) ? "TAPPED-IN" :
                   (g_caps[i].pcm == g_tap_pcm_out) ? "TAPPED-OUT" : "idle");
        }
        for (i = 0; i < AI_MAX_VOICES; i++) {
            ai_shm_t *shm = __atomic_load_n(&g_shm[i], __ATOMIC_ACQUIRE);
            if (!shm) continue;
            uint32_t h = __atomic_load_n(&shm->head, __ATOMIC_ACQUIRE);
            uint32_t t = shm->tail;
            uint32_t backlog = (h - t) & (AI_RING_FRAMES - 1);
            ai_log("[forceAudioJack] voice %d: backlog %u frames (%.1fms) trims %llu ev/%llu fr, "
                   "enabled=%u gain=%.3f chan=%u, %u Hz %u ch, produced %llu consumed %llu underruns %llu",
                   i, backlog, backlog * 1000.0 / (shm->rate ? shm->rate : 44100),
                   (unsigned long long)g_trim_events[i], (unsigned long long)g_trim_frames[i],
                   shm->enabled, shm->gain, shm->channel_mask, shm->rate, shm->channels,
                   (unsigned long long)shm->frames_written,
                   (unsigned long long)shm->frames_consumed,
                   (unsigned long long)shm->underruns);
        }
        for (i = 0; i < AI_MAX_OUT_VOICES; i++) {
            ai_shm_t *shm = __atomic_load_n(&g_shm_out[i], __ATOMIC_ACQUIRE);
            if (!shm) continue;
            uint32_t h = __atomic_load_n(&shm->head, __ATOMIC_ACQUIRE);
            uint32_t t = shm->tail;
            uint32_t backlog = (h - t) & (AI_RING_FRAMES - 1);
            ai_log("[forceAudioJack] out-bus voice %d: backlog %u frames (%.1fms) trims %llu ev/%llu fr, "
                   "enabled=%u gain=%.3f chan=%u, %u Hz %u ch, produced %llu consumed %llu underruns %llu",
                   i, backlog, backlog * 1000.0 / (shm->rate ? shm->rate : 44100),
                   (unsigned long long)g_trim_events_out[i], (unsigned long long)g_trim_frames_out[i],
                   shm->enabled, shm->gain, shm->channel_mask, shm->rate, shm->channels,
                   (unsigned long long)shm->frames_written,
                   (unsigned long long)shm->frames_consumed,
                   (unsigned long long)shm->underruns);
        }
        {
            ax_shm_t *ax = __atomic_load_n(&g_ax_skipback, __ATOMIC_ACQUIRE);
            if (ax) {
                uint32_t h = __atomic_load_n(&ax->head, __ATOMIC_ACQUIRE);
                uint32_t t = ax->tail;
                uint32_t backlog = (h - t) & (AX_RING_FRAMES - 1);
                ai_log("[forceAudioJack] skipback ring: backlog %u frames (%.1fms), %u Hz %u ch, "
                       "produced %llu consumed %llu overruns %llu",
                       backlog, backlog * 1000.0 / (ax->rate ? ax->rate : 44100),
                       ax->rate, ax->channels,
                       (unsigned long long)ax->frames_written,
                       (unsigned long long)ax->frames_consumed,
                       (unsigned long long)ax->overruns);
            }
        }
    }
    return NULL;
}

/* HISTORICAL NOTE: this used to be a `__attribute__((constructor))`
 * function (`ai_ctor`), gated behind an opt-in delay marker
 * (`AI_CTOR_DELAY_MARKER`) added 2026-09-13 as a controlled experiment
 * after a live test accidentally showed that extra timing perturbation
 * during MPC's own startup (a concurrent ps-polling loop) flipped a
 * previously 100%-reproducible pads-dead failure. That experiment's own
 * premise turned out to be right, but the fix wasn't "add a delay" - it
 * was "don't create a thread from a library constructor at all." See the
 * long comment on `ai_resolve()` above (where all of this now actually
 * lives) for the full 2026-09-22 root-cause finding and fix. */

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
    ai_evt(AI_EVT_MIX_ONE, (uint16_t)slot, avail);

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
        ai_evt(AI_EVT_TRIM, (uint16_t)slot, skip);
    }

    uint32_t take = (uint32_t)frames;
    if (take > avail) {
        shm->underruns++;
        take = avail;                                   /* mix what we have, then stop */
        ai_evt(AI_EVT_UNDERRUN, (uint16_t)slot, avail);
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

/* Is physical output channel `c` one this out-bus voice should land on,
 * given its `mask`? Confirmed live (192.168.1.187): the tapped playback
 * handle is a 4-channel interleaved PCM where channels 0/1 are MPC's own
 * main mix and channels 2/3 are the physical Out 3/4 jacks - unlike
 * chan_allowed() above (which maps onto whichever 1/2-channel capture shape
 * MPC configured), this is NOT relative to dst_channels: channels 0/1 must
 * never be touched by an out-bus voice regardless of dst_channels, and
 * channels 2/3 are the only valid targets. AI_CHAN_L/AI_CHAN_R mean "Out
 * 3"/"Out 4" here (see forceAudioInject.h). Fails closed if the handle
 * somehow isn't the confirmed 4-channel shape (checked by the caller before
 * this is ever reached) - there is no channel 2/3 to inject into otherwise. */
static inline int chan_allowed_out(unsigned c, uint32_t mask)
{
    if (c == 2) return (mask & AI_CHAN_L) != 0;
    if (c == 3) return (mask & AI_CHAN_R) != 0;
    return 0;   /* channels 0/1 (MPC's own main mix) are never touched */
}

/* Out-bus equivalent of mix_in_one: same SPSC ring draining/latency-trim/
 * underrun mechanics (see mix_in_one for the rationale, identical here),
 * but adds into `buffer` BEFORE it's handed to the real snd_pcm_writei
 * rather than after a real read - by the time a real write returns, the
 * samples are already gone to hardware, so out-bus injection has to happen
 * on the way in, not the way out. `buffer` holds MPC's own real playback
 * audio (channels 0/1) plus whatever earlier out-bus voices in this call
 * already added into channels 2/3. */
static inline void mix_out_one(unsigned slot, ai_shm_t *shm, unsigned char *buffer, size_t frames,
                               unsigned dst_channels, unsigned dst_frame_bytes, int dst_format)
{
    if (!shm || !frames || dst_channels < 4) return;

    uint32_t head = __atomic_load_n(&shm->head, __ATOMIC_ACQUIRE);
    uint32_t tail = shm->tail;                          /* we are the only consumer */
    uint32_t avail = (head - tail) & (AI_RING_FRAMES - 1);
    uint16_t evt_slot = (uint16_t)(slot + AI_MAX_VOICES);
    ai_evt(AI_EVT_MIX_ONE, evt_slot, avail);

    if (avail > AI_LATENCY_TRIGGER_FRAMES) {
        uint32_t skip = avail - AI_LATENCY_TARGET_FRAMES;
        tail = (tail + skip) & (AI_RING_FRAMES - 1);
        avail = AI_LATENCY_TARGET_FRAMES;
        g_trim_events_out[slot]++;
        g_trim_frames_out[slot] += skip;
        ai_evt(AI_EVT_TRIM, evt_slot, skip);
    }

    uint32_t take = (uint32_t)frames;
    if (take > avail) {
        shm->underruns++;
        take = avail;
        ai_evt(AI_EVT_UNDERRUN, evt_slot, avail);
    }
    if (!take) return;

    uint32_t enabled = shm->enabled;
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
            for (c = 2; c < dst_channels && c < 4; c++) {
                if (!chan_allowed_out(c, mask)) continue;
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

/* Sums every attached out-bus voice into `buffer`'s channels 2/3 - see
 * mix_out_one for the per-voice mechanics. Called from snd_pcm_writei
 * BEFORE the real write, unlike mix_in which runs after the real read. */
static inline void mix_out(unsigned char *buffer, size_t frames,
                           unsigned dst_channels, unsigned dst_frame_bytes, int dst_format)
{
    if (!__atomic_load_n(&g_n_attached_out, __ATOMIC_RELAXED) || !frames || dst_channels < 4) return;
    unsigned slot;
    for (slot = 0; slot < AI_MAX_OUT_VOICES; slot++) {
        ai_shm_t *shm = __atomic_load_n(&g_shm_out[slot], __ATOMIC_ACQUIRE);
        if (shm)
            mix_out_one(slot, shm, buffer, frames, dst_channels, dst_frame_bytes, dst_format);
    }
}

/* Copies `frames` frames of MPC's own real main-mix audio (channels 0/1 of
 * `buffer`, which by this point already includes any out-bus injection into
 * 2/3, irrelevant here since those are different channels) into the
 * skipback extraction ring. Called from snd_pcm_writei AFTER the real write
 * succeeds - see the "producer never blocks" overrun policy in
 * forceAudioJackExtract.h. `src_channels`/`src_frame_bytes`/`src_format`
 * describe `buffer`'s real shape (the confirmed 4-channel ADA2 handle in
 * practice), not the extraction ring's fixed AX_CHANNELS=2 shape. */
static inline void ax_extract_skipback(const unsigned char *buffer, size_t frames,
                                       unsigned src_channels, unsigned src_frame_bytes, int src_format)
{
    ax_shm_t *ax = __atomic_load_n(&g_ax_skipback, __ATOMIC_ACQUIRE);
    if (!ax || !frames || src_channels < 1) return;

    uint32_t head = ax->head;   /* we are the sole producer */
    uint32_t tail = __atomic_load_n(&ax->tail, __ATOMIC_ACQUIRE);
    uint32_t backlog = (head - tail) & (AX_RING_FRAMES - 1);
    uint32_t space = (AX_RING_FRAMES - 1) - backlog;

    unsigned dw = width_of(src_format);
    uint32_t i;
    for (i = 0; i < frames; i++) {
        uint32_t fr = (head + (uint32_t)i) & (AX_RING_FRAMES - 1);
        float *dst = &ax->ring[(size_t)fr * AX_CHANNELS];
        size_t off = (size_t)i * src_frame_bytes;
        float l = sample_to_float(buffer, off, src_format);
        float r = (src_channels >= 2) ? sample_to_float(buffer, off + dw, src_format) : l;
        dst[0] = l;
        dst[1] = r;
    }

    /* Producer never blocks: if skipbackHost has fallen more than a full
     * ring lap behind, we've just overwritten frames it never read - log it
     * as an overrun rather than ever waiting on the consumer. */
    if ((uint32_t)frames > space) ax->overruns += ((uint32_t)frames - space);

    __atomic_store_n(&ax->head, (head + (uint32_t)frames) & (AX_RING_FRAMES - 1), __ATOMIC_RELEASE);
    ax->frames_written += frames;
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
            /* Generic across both directions - this hook fires for MPC's
             * playback handle too now (out-bus injection), not just capture
             * as before, so the log no longer assumes which one. */
            ai_log("[forceAudioJack] configured handle %s: %u Hz, %u ch, fmt %d (%u B/frame)",
                   (q_pcm_name && pcm) ? q_pcm_name(pcm) : "?", rate, ch, fmt, ch * width_of(fmt));
            ai_evt(AI_EVT_HW_PARAMS, (uint16_t)ch, rate);
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
            ai_evt(AI_EVT_FIRST_READ, (uint16_t)slot, (uint32_t)r);
        }
        if ((void *)pcm != g_tap_pcm) return r;          /* a second handle - ignore it */

        ai_evt(AI_EVT_READI, (uint16_t)slot, (uint32_t)r);
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

/* Out-bus injection tap. Unlike snd_pcm_readi (mix AFTER the real read,
 * into the result buffer), injection here has to happen BEFORE the real
 * write - once orig_writei returns, these samples are already gone to
 * hardware, so there's no "after" to mix into. `buffer` is `const` per
 * ALSA's own API (the caller, MPC, doesn't expect it mutated), but casting
 * that away and adding into it in place is safe here: MPC hands this buffer
 * to snd_pcm_writei purely to be written to the codec and never reads it
 * back afterward (same assumption force-link-audio's forceStream.so already
 * relies on for its own writei hook, just reading rather than mutating). */
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
    ensure_init();
    if (!orig_writei) return -1;

    if (size > 0) {
        int i, slot = -1;
        for (i = 0; i < MAX_PCMS; i++)
            if (g_caps[i].pcm == (void *)pcm) { slot = i; break; }

        if (slot >= 0) {
            g_caps[slot].frames += (uint64_t)size;

            if (!g_tap_pcm_out) {
                g_tap_pcm_out = (void *)pcm;
                g_announce_claim_out = 1;
                ai_evt(AI_EVT_FIRST_WRITE, (uint16_t)slot, (uint32_t)size);
            }
            if ((void *)pcm == g_tap_pcm_out) {
                ai_evt(AI_EVT_WRITEI, (uint16_t)slot, (uint32_t)size);
                mix_out((unsigned char *)buffer, (size_t)size,
                        g_caps[slot].channels, g_caps[slot].frame_bytes, g_caps[slot].format);
            }
        }

        snd_pcm_sframes_t r = orig_writei(pcm, buffer, size);

        /* Extraction happens AFTER the real write succeeds (mirrors readi's
         * own r>0 check) - only count audio that actually made it to
         * hardware, and only from the one handle we're tapping. */
        if (r > 0 && slot >= 0 && (void *)pcm == g_tap_pcm_out)
            ax_extract_skipback((const unsigned char *)buffer, (size_t)r,
                                 g_caps[slot].channels, g_caps[slot].frame_bytes, g_caps[slot].format);
        return r;
    }
    return orig_writei(pcm, buffer, size);
}
