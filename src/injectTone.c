/*
 * injectTone — proof-of-concept generator for forceAudioJack.so.
 *
 * Creates the /forceAudioInject shared-memory ring and continuously writes a
 * sine wave into it, paced to real time. forceAudioJack.so (LD_PRELOAD'd into
 * MPC) mixes this into whatever MPC reads from its capture device.
 *
 * This is deliberately dumb - a real MIDI-generator renderer would replace
 * this process, writing rendered note audio into the same ring instead of a
 * fixed tone. The point of this program is only to prove the injection path
 * end-to-end: if the tone shows up in forceAudioJack.so's logged peak/consumed
 * counters (or audibly on an Audio-In track), the mechanism works.
 *
 * `--slot N` (default 0) picks which voice slot's shared-memory segment this
 * instance creates, so it can also be used to smoke-test forceAudioJack.so's
 * multi-voice mixing (e.g. run two instances at different `--slot`/freq/
 * `--channel` to confirm they mix independently).
 *
 * `--bus in|out` (default in) picks the In-bus (Audio-In 1/2 injection,
 * the original feature) or Out-bus (physical Out 3/4 injection) ring
 * namespace - see AI_SHM_NAME_FMT_OUT in forceAudioInject.h. On `--bus
 * out`, `--channel L|R|LR` means "Out 3"/"Out 4"/both rather than L/R, and
 * `--slot` is clamped to AI_MAX_OUT_VOICES instead of AI_MAX_VOICES.
 *
 * Usage: injectTone [freqHz] [gain0to1] [channels(1|2)] [--slot N] [--channel L|R|LR] [--bus in|out]
 *
 * BUILD:
 *   zig cc -target arm-linux-gnueabihf.2.39 -O2 \
 *       -o injectTone injectTone.c -lpthread -lrt -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "forceAudioInject.h"

#define GEN_RATE 44100
#define BLOCK_FRAMES 256

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

int main(int argc, char **argv)
{
    double freq = (argc > 1 && argv[1][0] != '-') ? atof(argv[1]) : 440.0;
    double gain = (argc > 2 && argv[2][0] != '-') ? atof(argv[2]) : 0.2;
    unsigned channels = (argc > 3 && argv[3][0] != '-') ? (unsigned)atoi(argv[3]) : 1;
    if (channels < 1 || channels > AI_MAX_CH) channels = 1;

    unsigned slot = 0;
    uint32_t chan_mask = AI_CHAN_LR;
    int bus_out = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--slot") && i + 1 < argc) {
            slot = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--channel") && i + 1 < argc) {
            const char *v = argv[++i];
            chan_mask = !strcmp(v, "L") ? AI_CHAN_L : !strcmp(v, "R") ? AI_CHAN_R : AI_CHAN_LR;
        } else if (!strcmp(argv[i], "--bus") && i + 1 < argc) {
            bus_out = !strcmp(argv[++i], "out");
        }
    }
    if (bus_out) { if (slot >= AI_MAX_OUT_VOICES) slot = 0; }
    else         { if (slot >= AI_MAX_VOICES)     slot = 0; }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    char shm_name[24];
    if (bus_out) ai_shm_name_out(slot, shm_name, sizeof(shm_name));
    else         ai_shm_name(slot, shm_name, sizeof(shm_name));

    shm_unlink(shm_name); /* start clean - we are the sole producer for this slot */
    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(fd, AI_SHM_BYTES) != 0) { perror("ftruncate"); return 1; }
    ai_shm_t *shm = mmap(NULL, AI_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) { perror("mmap"); return 1; }

    memset(shm, 0, AI_SHM_BYTES);
    shm->rate = GEN_RATE;
    shm->channels = channels;
    shm->enabled = 1;
    shm->gain = 1.0f;
    shm->channel_mask = chan_mask;
    __atomic_store_n(&shm->magic, AI_MAGIC, __ATOMIC_RELEASE);

    printf("[injectTone] writing %.1f Hz tone at gain %.2f, %u ch, %u Hz into %s (%s-bus slot %u, chan mask %u)\n",
           freq, gain, channels, (unsigned)GEN_RATE, shm_name, bus_out ? "out" : "in", slot, chan_mask);

    double phase = 0.0;
    const double phase_inc = 2.0 * M_PI * freq / GEN_RATE;
    struct timespec block_time = { 0, (long)(1e9 * BLOCK_FRAMES / GEN_RATE) };
    uint64_t reported = 0;
    struct timespec last_report; clock_gettime(CLOCK_MONOTONIC, &last_report);

    while (g_run) {
        uint32_t head = shm->head;                                   /* sole producer */
        uint32_t tail = __atomic_load_n(&shm->tail, __ATOMIC_ACQUIRE);
        uint32_t space = (AI_RING_FRAMES - 1) - ((head - tail) & (AI_RING_FRAMES - 1));

        /* Pace slightly FASTER than real-time, not exactly at it: sleeping
         * the full block-equivalent duration after every write (the
         * original code) has zero margin, and an ordinary nanosleep
         * overshoot - the norm, not the exception, on a non-realtime-
         * scheduled thread - then makes the producer fall a little behind
         * on every single iteration, with nothing to ever catch it back up.
         * That produced a steady ~5-6 underruns/sec on real hardware
         * regardless of system load (confirmed 2026-09-23: nearly identical
         * underrun rate whether or not anything else was running), each one
         * a real, audible 128-sample gap of silence in whatever this tone
         * was mixed into - this is what "glitchy skipback playback" traced
         * back to. Sleeping for a fraction of the block's real-time
         * duration instead (still throttled, so this never busy-loops or
         * floods the ring - just biased to arrive a bit early) lets a
         * small, bounded cushion build up for the consumer's own trim logic
         * (AI_LATENCY_TARGET_FRAMES) to draw down against, self-correcting
         * the drift instead of racing it every block. An earlier version of
         * this fix removed the sleep after a successful write entirely,
         * which is wrong: with nothing bounding the producer's rate at all,
         * it ran far faster than real-time between backpressure checks,
         * and the consumer's own trim logic ended up discarding nearly
         * everything produced (confirmed: 82M+ frames produced in
         * ~1s, 1380 trim events discarding almost all of it) while burning
         * CPU the whole system needed. Bounded pacing avoids both extremes. */
        if (space < BLOCK_FRAMES) {
            nanosleep(&block_time, NULL);
            continue;
        }

        unsigned i;
        for (i = 0; i < BLOCK_FRAMES; i++) {
            float v = (float)(gain * sin(phase));
            phase += phase_inc;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            uint32_t fr = (head + i) & (AI_RING_FRAMES - 1);
            float *dst = &shm->ring[(size_t)fr * AI_MAX_CH];
            unsigned c;
            for (c = 0; c < channels; c++) dst[c] = v;
        }

        __atomic_store_n(&shm->head, (head + BLOCK_FRAMES) & (AI_RING_FRAMES - 1),
                         __ATOMIC_RELEASE);
        shm->frames_written += BLOCK_FRAMES;

        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - last_report.tv_sec >= 5) {
            printf("[injectTone] produced %llu consumed %llu underruns %llu\n",
                   (unsigned long long)shm->frames_written,
                   (unsigned long long)shm->frames_consumed,
                   (unsigned long long)shm->underruns);
            last_report = now;
            (void)reported;
        }

        /* Sleep for 80% of the block's real-time duration, not 100% - see
         * the comment above the space check for why full-duration pacing
         * (the original bug) and no pacing at all (an intermediate, wrong
         * fix) both fail. This margin is deliberately generous: even a few
         * ms of scheduling jitter per block is smaller than the 20% margin
         * at BLOCK_FRAMES=256 (5.8ms/block, so ~1.2ms of slack per block). */
        struct timespec margin_sleep = { 0, block_time.tv_nsec * 4 / 5 };
        nanosleep(&margin_sleep, NULL);
    }

    munmap(shm, AI_SHM_BYTES);
    shm_unlink(shm_name);
    printf("[injectTone] stopped\n");
    return 0;
}
