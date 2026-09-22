/*
 * skipbackHost - the Skipback consumer for force-audio-jack's extraction
 * ring (see src/forceAudioJackExtract.h and docs/PROPOSAL-force-audio-jack.md).
 *
 * WHAT IT DOES
 *   Creates the /forceAudioJackSkipback0 shared-memory ring (forceAudioJack.so
 *   lazily attaches to it as PRODUCER within ~2s - see ax_try_attach_skipback
 *   in forceAudioJack.c) and continuously drains MPC's real main-mix audio
 *   (channels 0/1, post-mix) out of it into its own large in-process rolling
 *   buffer. On a trigger file appearing (touched by a MidiLoop SCRIPT-N
 *   shortcut bound to SHIFT+RECORD - see the PROPOSAL doc), it flushes the
 *   current rolling-buffer contents to a WAV file, named with the current
 *   project and tempo when available, and deletes the marker.
 *
 * This is Schwung's Skipback feature (see
 * https://schwung.dev/manual.html#recording): continuous background
 * recording so that hitting "save" captures audio from BEFORE you decided
 * to save, not just from that moment forward.
 *
 * THREADING
 *   Main thread: drains the extraction ring into the rolling buffer at a
 *   ~20ms cadence (fast enough that the ring - sized for ~1.49s of headroom -
 *   never has a chance to fill even under scheduling jitter) and polls the
 *   trigger marker every ~200ms.
 *   On trigger: a short-lived detached thread takes a mutex-protected
 *   snapshot copy of the rolling buffer, then does all the slow work (project
 *   name/tempo lookup, WAV write to the SD card) OUTSIDE the lock, so a slow
 *   SD card write never blocks the drain loop from keeping up with live
 *   audio.
 *
 * BUILD:
 *   zig cc -target arm-linux-gnueabihf.2.39 -O2 \
 *       -o skipbackHost skipbackHost.c -lpthread -lrt -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>

#include "forceAudioJackExtract.h"

#define TRIGGER_MARKER      "/tmp/forceAudioJack.skipback"
#define CURRENT_PROJECT_XML "/data/Settings/MPC/CurrentProject"
#define DEFAULT_WINDOW_SEC  30u
#define MAX_WINDOW_SEC      60u   /* hard cap - see docs/PROPOSAL-force-audio-jack.md's RAM-budget decision */
#define DEFAULT_OUTPUT_DIR  "/sdcard/Force Documents/Samples/Skipback"
#define DEFAULT_RATE        44100u

/* Optional "now playing" override for the filename's name AND bpm
 * components, in place of the project-name/project-tempo lookups below -
 * a generic convention any addon can use, not something specific to one.
 * Any addon that knows what's actually playing (e.g. force-cratedigger,
 * mid-Discogs-crate-dig search result) writes up to two lines here on
 * track change and removes the file on stop; skipbackHost doesn't care
 * who wrote it or why, just that it's recent:
 *   line 1: track name (required for the override to apply at all)
 *   line 2: track-specific tempo/bpm, plain number (optional)
 *
 * IMPORTANT: once the override applies (a fresh line 1), the Force
 * project's own tempo becomes IRRELEVANT and must never appear in the
 * filename in its place - it's the sequencer's tempo, unrelated to
 * whatever external track was actually playing, and showing it would
 * misrepresent the recording. So: line 2 present and > 0 -> that tempo
 * appears in the filename; line 2 absent/blank/<= 0 (e.g. Discogs
 * releases carry no BPM data at all, so force-cratedigger's own
 * crate-dig results usually won't set this line) -> the filename gets
 * NO bpm segment at all, not the project tempo. See save_worker()'s own
 * comment on the call site for exactly how that's kept separate from the
 * ordinary (no override - ordinary studio jam) case, where the real
 * project tempo is still exactly as meaningful as ever.
 *
 * Falls back entirely to the project name/tempo exactly as before if the
 * file is missing, empty, or older than NOWPLAYING_MAX_AGE_SEC (stale -
 * e.g. the writer crashed or was killed without cleaning up). */
#define NOWPLAYING_OVERRIDE_PATH "/tmp/force_nowplaying.txt"
#define NOWPLAYING_MAX_AGE_SEC   600

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

/* ---- rolling buffer -------------------------------------------------- */
static pthread_mutex_t g_buf_lock = PTHREAD_MUTEX_INITIALIZER;
static float    *g_buf = NULL;        /* capacity_frames * AX_CHANNELS floats, protected by g_buf_lock */
static uint32_t  g_capacity_frames = 0;
static uint32_t  g_write_pos = 0;     /* next frame index to write, mod capacity */
static uint64_t  g_total_written = 0; /* monotonic - tells a save whether the buffer has wrapped yet */
static unsigned  g_rate = DEFAULT_RATE;

static const char *g_output_dir = DEFAULT_OUTPUT_DIR;

static void rb_push(const float *frames, uint32_t n)
{
    pthread_mutex_lock(&g_buf_lock);
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint32_t idx = (g_write_pos + i) % g_capacity_frames;
        g_buf[(size_t)idx * AX_CHANNELS + 0] = frames[(size_t)i * AX_CHANNELS + 0];
        g_buf[(size_t)idx * AX_CHANNELS + 1] = frames[(size_t)i * AX_CHANNELS + 1];
    }
    g_write_pos = (g_write_pos + n) % g_capacity_frames;
    g_total_written += n;
    pthread_mutex_unlock(&g_buf_lock);
}

/* Copies out a private, chronologically-ordered snapshot of everything
 * currently in the rolling buffer. Returns a malloc'd buffer (caller frees)
 * and the frame count via *out_frames - fewer than g_capacity_frames if the
 * buffer hasn't wrapped yet (early in a session). */
static float *rb_snapshot(uint32_t *out_frames)
{
    pthread_mutex_lock(&g_buf_lock);
    uint32_t n = (g_total_written < g_capacity_frames) ? (uint32_t)g_total_written : g_capacity_frames;
    float *out = malloc((size_t)n * AX_CHANNELS * sizeof(float));
    if (out) {
        if (g_total_written < g_capacity_frames) {
            /* Buffer hasn't wrapped - valid content is [0, write_pos), already in order. */
            memcpy(out, g_buf, (size_t)n * AX_CHANNELS * sizeof(float));
        } else {
            /* Wrapped - oldest frame is at write_pos, wrapping around to write_pos-1 (newest). */
            uint32_t tail_frames = g_capacity_frames - g_write_pos;
            memcpy(out, &g_buf[(size_t)g_write_pos * AX_CHANNELS],
                   (size_t)tail_frames * AX_CHANNELS * sizeof(float));
            memcpy(out + (size_t)tail_frames * AX_CHANNELS, g_buf,
                   (size_t)g_write_pos * AX_CHANNELS * sizeof(float));
        }
    }
    pthread_mutex_unlock(&g_buf_lock);
    *out_frames = out ? n : 0;
    return out;
}

/* ---- project name / tempo lookup (best-effort, never blocks a save) --- */

/* Reads CURRENT_PROJECT_XML's lastProjectPath attribute and extracts the
 * bare project name (no directory, no .xpj extension). On any failure,
 * leaves *name untouched (caller pre-fills a fallback). */
static void lookup_project_name(char *name, size_t name_len, char *xpj_path, size_t xpj_len)
{
    FILE *f = fopen(CURRENT_PROJECT_XML, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "lastProjectPath");
        if (!p) continue;
        p = strstr(p, "val=\"");
        if (!p) continue;
        p += 5;
        char *end = strchr(p, '"');
        if (!end) continue;
        size_t len = (size_t)(end - p);
        if (len >= xpj_len) len = xpj_len - 1;
        memcpy(xpj_path, p, len);
        xpj_path[len] = '\0';

        /* basename, minus a trailing .xpj */
        const char *base = strrchr(xpj_path, '/');
        base = base ? base + 1 : xpj_path;
        size_t blen = strlen(base);
        if (blen > 4 && strcmp(base + blen - 4, ".xpj") == 0) blen -= 4;
        if (blen >= name_len) blen = name_len - 1;
        memcpy(name, base, blen);
        name[blen] = '\0';
        break;
    }
    fclose(f);
}

/* Gunzips the given .xpj (it's gzip-compressed JSON - confirmed live, see
 * PROPOSAL doc) via a shelled-out gunzip and scans for the top-level
 * "Tempo": field. Shelling out rather than linking zlib keeps this small
 * producer dependency-free, same tradeoff this whole project already makes
 * elsewhere (shell scripts over libraries) - this only ever runs once per
 * skipback trigger, nowhere near a hot path. Returns 0.0 on any failure. */
static double lookup_tempo(const char *xpj_path)
{
    if (!xpj_path[0]) return 0.0;
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "gunzip -c '%s' 2>/dev/null", xpj_path);
    FILE *p = popen(cmd, "r");
    if (!p) return 0.0;

    double tempo = 0.0;
    char buf[65536];
    size_t got;
    /* Scan in overlapping-ish chunks for a top-level "Tempo": - the very
     * first occurrence in a fresh project export is the master tempo (see
     * the live capture in the PROPOSAL doc: "TempoEnabled": false immediately
     * followed by "Tempo": 128.0). Good enough for a filename tag; this is
     * explicitly documented as not authoritative for anything more precise. */
    while ((got = fread(buf, 1, sizeof(buf) - 1, p)) > 0) {
        buf[got] = '\0';
        char *t = strstr(buf, "\"Tempo\"");
        if (t) {
            t = strchr(t, ':');
            if (t) { tempo = atof(t + 1); break; }
        }
    }
    pclose(p);
    return tempo;
}

/* Reads NOWPLAYING_OVERRIDE_PATH's first line into `name` (truncated to
 * name_len - 1, trailing newline stripped) if the file exists, is
 * non-empty, and was modified within NOWPLAYING_MAX_AGE_SEC. On success
 * (returns 1), also checks a second line for a track-specific tempo: if
 * present and it parses as a positive number, *tempo_out is set to it
 * (overriding the caller's project-tempo default); otherwise *tempo_out
 * is left untouched, so the caller's existing lookup_tempo() result
 * still applies. Returns 0 (name and *tempo_out both untouched - caller
 * already has the project-name/project-tempo fallbacks) if there's no
 * usable override at all. */
static int lookup_nowplaying_override(char *name, size_t name_len, double *tempo_out)
{
    struct stat st;
    if (stat(NOWPLAYING_OVERRIDE_PATH, &st) != 0) return 0;
    time_t age = time(NULL) - st.st_mtime;
    if (age < 0 || age > NOWPLAYING_MAX_AGE_SEC) return 0;

    FILE *f = fopen(NOWPLAYING_OVERRIDE_PATH, "r");
    if (!f) return 0;
    char line1[256] = "";
    char line2[64] = "";
    char *got1 = fgets(line1, sizeof(line1), f);
    char *got2 = fgets(line2, sizeof(line2), f);
    fclose(f);
    if (!got1) return 0;

    size_t len = strcspn(line1, "\r\n");
    line1[len] = '\0';
    if (len == 0) return 0;

    if (len >= name_len) len = name_len - 1;
    memcpy(name, line1, len);
    name[len] = '\0';

    if (got2) {
        line2[strcspn(line2, "\r\n")] = '\0';
        double t = atof(line2);
        if (t > 0.0) *tempo_out = t;
    }
    return 1;
}

static void sanitize_for_filename(char *s)
{
    for (; *s; s++)
        if (*s == '/' || *s == '\\' || (unsigned char)*s < 0x20) *s = '_';
}

/* ---- WAV writer (16-bit PCM, stereo) ----------------------------------- */
static void put_u32le(unsigned char *b, uint32_t v) { b[0]=v; b[1]=v>>8; b[2]=v>>16; b[3]=v>>24; }
static void put_u16le(unsigned char *b, uint16_t v) { b[0]=v; b[1]=v>>8; }

static int write_wav(const char *path, const float *frames, uint32_t n_frames, unsigned rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    uint32_t data_bytes = n_frames * AX_CHANNELS * 2u;
    unsigned char hdr[44];
    memcpy(hdr, "RIFF", 4);
    put_u32le(hdr + 4, 36 + data_bytes);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    put_u32le(hdr + 16, 16);
    put_u16le(hdr + 20, 1);                    /* PCM */
    put_u16le(hdr + 22, AX_CHANNELS);
    put_u32le(hdr + 24, rate);
    put_u32le(hdr + 28, rate * AX_CHANNELS * 2u);
    put_u16le(hdr + 32, AX_CHANNELS * 2u);
    put_u16le(hdr + 34, 16);
    memcpy(hdr + 36, "data", 4);
    put_u32le(hdr + 40, data_bytes);
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return -1; }

    /* Convert+write in blocks rather than one huge malloc'd int16 copy -
     * this only runs on the save thread, off any real-time path, but no
     * reason to double the peak memory use for a multi-MB buffer. */
    enum { BLOCK = 4096 };
    int16_t out[BLOCK * AX_CHANNELS];
    uint32_t i = 0;
    while (i < n_frames) {
        uint32_t take = n_frames - i;
        if (take > BLOCK) take = BLOCK;
        uint32_t j;
        for (j = 0; j < take; j++) {
            float l = frames[(size_t)(i + j) * AX_CHANNELS + 0];
            float r = frames[(size_t)(i + j) * AX_CHANNELS + 1];
            if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
            if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
            out[j * AX_CHANNELS + 0] = (int16_t)(l * 32767.0f);
            out[j * AX_CHANNELS + 1] = (int16_t)(r * 32767.0f);
        }
        if (fwrite(out, sizeof(int16_t) * AX_CHANNELS, take, f) != take) { fclose(f); return -1; }
        i += take;
    }
    fclose(f);
    return 0;
}

/* mkdir -p, since g_output_dir may not exist yet on a fresh install. */
static void mkdir_p(const char *dir)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

/* ---- trigger handling --------------------------------------------------- */
static void *save_worker(void *unused)
{
    (void)unused;
    uint32_t n_frames;
    float *snap = rb_snapshot(&n_frames);
    if (!snap || !n_frames) { free(snap); return NULL; }

    char project[128] = "Untitled";
    char xpj_path[400] = "";
    lookup_project_name(project, sizeof(project), xpj_path, sizeof(xpj_path));
    double tempo = lookup_tempo(xpj_path);

    /* A fresh now-playing override (see its own comment) replaces the
     * project name in the filename - and, once it does, the project
     * tempo above is no longer relevant at all (it's the Force
     * sequencer's own tempo, unrelated to whatever external track was
     * actually playing) and must NOT leak into the filename just
     * because no track-specific tempo was available either. So this
     * looks up into a fresh 0.0, not into `tempo` directly: if the
     * override applies, `tempo` becomes the track's own tempo when the
     * source provided one, or 0.0 (which the write below already
     * renders as "no bpm segment at all") when it didn't - never the
     * unrelated project tempo. If the override doesn't apply (no fresh
     * now-playing file - an ordinary, non-cratedigger recording),
     * `tempo` is left as the real project lookup above, unchanged. */
    double override_tempo = 0.0;
    if (lookup_nowplaying_override(project, sizeof(project), &override_tempo))
        tempo = override_tempo;
    sanitize_for_filename(project);

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);

    mkdir_p(g_output_dir);
    char path[700];
    if (tempo > 0.0)
        snprintf(path, sizeof(path), "%s/Skipback_%s_%dbpm_%s.wav",
                 g_output_dir, project, (int)(tempo + 0.5), stamp);
    else
        snprintf(path, sizeof(path), "%s/Skipback_%s_%s.wav", g_output_dir, project, stamp);

    if (write_wav(path, snap, n_frames, g_rate) == 0)
        printf("[skipbackHost] saved %.1fs -> %s\n", (double)n_frames / g_rate, path);
    else
        fprintf(stderr, "[skipbackHost] failed to write %s\n", path);

    free(snap);
    return NULL;
}

int main(int argc, char **argv)
{
    unsigned window_sec = DEFAULT_WINDOW_SEC;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--window-sec") && i + 1 < argc) {
            window_sec = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--output-dir") && i + 1 < argc) {
            g_output_dir = argv[++i];
        }
    }
    if (window_sec < 1) window_sec = 1;
    if (window_sec > MAX_WINDOW_SEC) window_sec = MAX_WINDOW_SEC;   /* hard cap, never trust the caller past this */

    g_capacity_frames = window_sec * g_rate;
    g_buf = calloc((size_t)g_capacity_frames * AX_CHANNELS, sizeof(float));
    if (!g_buf) { fprintf(stderr, "[skipbackHost] out of memory\n"); return 1; }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    shm_unlink(AX_SHM_NAME_SKIPBACK);   /* start clean - we are the sole owner of this segment */
    int fd = shm_open(AX_SHM_NAME_SKIPBACK, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(fd, AX_SHM_BYTES) != 0) { perror("ftruncate"); return 1; }
    ax_shm_t *ax = mmap(NULL, AX_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ax == MAP_FAILED) { perror("mmap"); return 1; }

    memset(ax, 0, AX_SHM_BYTES);
    ax->rate = g_rate;
    ax->channels = AX_CHANNELS;
    __atomic_store_n(&ax->magic, AX_MAGIC, __ATOMIC_RELEASE);

    printf("[skipbackHost] rolling %us window (%u frames, %.1f MB), output dir: %s\n",
           window_sec, g_capacity_frames,
           (double)g_capacity_frames * AX_CHANNELS * sizeof(float) / (1024.0 * 1024.0),
           g_output_dir);

    struct timespec drain_period = { 0, 20000000L };   /* ~20ms - fast relative to the ring's ~1.49s headroom */
    unsigned tick = 0;
    float pop_buf[1024 * AX_CHANNELS];

    while (g_run) {
        nanosleep(&drain_period, NULL);
        tick++;

        /* Drain whatever's newly available - we are the sole consumer. */
        uint32_t head = __atomic_load_n(&ax->head, __ATOMIC_ACQUIRE);
        uint32_t tail = ax->tail;
        uint32_t avail = (head - tail) & (AX_RING_FRAMES - 1);
        while (avail > 0) {
            uint32_t take = avail;
            if (take > 1024) take = 1024;
            uint32_t i;
            for (i = 0; i < take; i++) {
                uint32_t fr = (tail + i) & (AX_RING_FRAMES - 1);
                pop_buf[i * AX_CHANNELS + 0] = ax->ring[(size_t)fr * AX_CHANNELS + 0];
                pop_buf[i * AX_CHANNELS + 1] = ax->ring[(size_t)fr * AX_CHANNELS + 1];
            }
            rb_push(pop_buf, take);
            tail = (tail + take) & (AX_RING_FRAMES - 1);
            __atomic_store_n(&ax->tail, tail, __ATOMIC_RELEASE);
            ax->frames_consumed += take;
            avail -= take;
        }

        if (tick % 10 != 0) continue;   /* trigger check on a slower ~200ms cadence */
        if (access(TRIGGER_MARKER, F_OK) == 0) {
            unlink(TRIGGER_MARKER);
            pthread_t t;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            pthread_create(&t, &attr, save_worker, NULL);
            pthread_attr_destroy(&attr);
        }
    }

    munmap(ax, AX_SHM_BYTES);
    shm_unlink(AX_SHM_NAME_SKIPBACK);
    free(g_buf);
    printf("[skipbackHost] stopped\n");
    return 0;
}
