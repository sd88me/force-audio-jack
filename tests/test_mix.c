#define _GNU_SOURCE   /* must precede every system header - forceAudioIn.c's own
                        * _GNU_SOURCE define comes too late once this file's
                        * earlier includes have already locked in glibc's
                        * default feature-test macros */
/* Native (x86_64) unit test against the REAL forceAudioIn.c mixing code -
 * not a reimplementation. #include the actual source so mix_in_one() and
 * chan_allowed() (both `static inline`) are directly callable, and exercise
 * them with synthetic in-process ai_shm_t structs instead of real POSIX
 * shared memory or a real ALSA capture handle. This can't validate anything
 * about the real audio thread/ALSA interposition (needs the actual device),
 * but it does validate the new multi-voice arithmetic: per-voice gain,
 * L/R/L+R channel routing, mute-still-drains, and the latency trim.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* forceAudioIn.c's own ai_ctor (constructor) runs at process start and tries
 * to shm_open real /forceAudioInjectN segments - harmless here (none exist
 * on this dev box, so it just logs to /tmp/forceAudioIn.log and leaves
 * g_shm[] all NULL); this test never touches that global array, it calls
 * mix_in_one() directly on synthetic structs instead. */
#include "forceAudioIn.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
    else printf("ok:   %s\n", msg); \
} while (0)

static int16_t s16_at(const unsigned char *buf, int frame, int ch, int nch) {
    int16_t v; memcpy(&v, buf + (size_t)frame * nch * 2 + (size_t)ch * 2, 2); return v;
}

static ai_shm_t *make_voice(float gain, uint32_t mask, uint32_t enabled, float ring_val, uint32_t n_frames) {
    ai_shm_t *shm = calloc(1, sizeof(ai_shm_t));
    shm->magic = AI_MAGIC;
    shm->rate = 44100;
    shm->channels = 1;              /* mono voice, duplicated per chan_allowed */
    shm->enabled = enabled;
    shm->gain = gain;
    shm->channel_mask = mask;
    for (uint32_t i = 0; i < n_frames; i++)
        shm->ring[(size_t)i * AI_MAX_CH] = ring_val;
    shm->head = n_frames;            /* sole producer for this synthetic test */
    shm->tail = 0;
    return shm;
}

int main(void) {
    printf("== chan_allowed ==\n");
    CHECK(chan_allowed(0, AI_CHAN_L, 2) == 1, "L-only mask allows dst channel 0");
    CHECK(chan_allowed(1, AI_CHAN_L, 2) == 0, "L-only mask blocks dst channel 1");
    CHECK(chan_allowed(0, AI_CHAN_R, 2) == 0, "R-only mask blocks dst channel 0");
    CHECK(chan_allowed(1, AI_CHAN_R, 2) == 1, "R-only mask allows dst channel 1");
    CHECK(chan_allowed(0, AI_CHAN_LR, 2) == 1 && chan_allowed(1, AI_CHAN_LR, 2) == 1, "L+R mask allows both");
    CHECK(chan_allowed(0, 0, 2) == 0 && chan_allowed(1, 0, 2) == 0, "disabled mask (0) allows neither");
    CHECK(chan_allowed(0, AI_CHAN_L, 1) == 1, "mono dst gets voice if either channel selected");
    CHECK(chan_allowed(0, 0, 1) == 0, "mono dst muted if mask is 0");

    printf("\n== mix_in_one: gain + channel routing ==\n");
    {
        /* dst: S16_LE, 2 channels, 4 frames, format code 2 (see width_of/sample_to_float) */
        unsigned char buf[4 * 2 * 2] = {0};
        ai_shm_t *voiceL = make_voice(/*gain*/0.5f, AI_CHAN_L, /*enabled*/1, /*ring_val*/0.4f, 4);
        mix_in_one(0, voiceL, buf, 4, /*dst_channels*/2, /*dst_frame_bytes*/4, /*dst_format*/2);

        int16_t expect = (int16_t)(0.4f * 0.5f * 32767.0f);   /* matches float_to_sample's own rounding */
        CHECK(s16_at(buf, 0, 0, 2) == expect, "L-routed voice writes into dst channel 0");
        CHECK(s16_at(buf, 0, 1, 2) == 0, "L-routed voice leaves dst channel 1 untouched");
        CHECK(voiceL->tail == 4, "ring fully drained after mix");
        CHECK(voiceL->frames_consumed == 4, "frames_consumed updated");
        free(voiceL);
    }

    printf("\n== mix_in_one: second voice sums onto the same buffer, R channel ==\n");
    {
        unsigned char buf[4 * 2 * 2] = {0};
        ai_shm_t *voiceL = make_voice(1.0f, AI_CHAN_L, 1, 0.3f, 4);
        ai_shm_t *voiceR = make_voice(1.0f, AI_CHAN_R, 1, 0.3f, 4);
        mix_in_one(0, voiceL, buf, 4, 2, 4, 2);
        mix_in_one(1, voiceR, buf, 4, 2, 4, 2);
        int16_t expect = (int16_t)(0.3f * 32767.0f);
        CHECK(s16_at(buf, 0, 0, 2) == expect, "voice A lands on L");
        CHECK(s16_at(buf, 0, 1, 2) == expect, "voice B lands on R independently");
        free(voiceL); free(voiceR);
    }

    printf("\n== mix_in_one: two voices sharing L+R sum (not overwrite) ==\n");
    {
        unsigned char buf[1 * 2 * 2] = {0};
        ai_shm_t *a = make_voice(1.0f, AI_CHAN_LR, 1, 0.2f, 1);
        ai_shm_t *b = make_voice(1.0f, AI_CHAN_LR, 1, 0.2f, 1);
        mix_in_one(0, a, buf, 1, 2, 4, 2);
        mix_in_one(1, b, buf, 1, 2, 4, 2);
        int16_t expect = (int16_t)(0.4f * 32767.0f);  /* 0.2 + 0.2, clamped/quantized like real path */
        CHECK(abs(s16_at(buf, 0, 0, 2) - expect) <= 1, "two L+R voices sum on channel 0 (not last-write-wins)");
        CHECK(abs(s16_at(buf, 0, 1, 2) - expect) <= 1, "two L+R voices sum on channel 1");
        free(a); free(b);
    }

    printf("\n== mix_in_one: muted voice drains ring but doesn't touch the buffer ==\n");
    {
        unsigned char buf[4 * 2 * 2];
        memset(buf, 0xAB, sizeof(buf));   /* sentinel - any write would corrupt this pattern */
        ai_shm_t *muted = make_voice(1.0f, AI_CHAN_LR, /*enabled*/0, 0.9f, 4);
        mix_in_one(0, muted, buf, 4, 2, 4, 2);
        int all_untouched = 1;
        for (size_t i = 0; i < sizeof(buf); i++) if (buf[i] != 0xAB) all_untouched = 0;
        CHECK(all_untouched, "muted voice leaves destination buffer byte-for-byte untouched");
        CHECK(muted->tail == 4, "muted voice's ring still drains at the normal rate");
        free(muted);
    }

    printf("\n== mix_in_one: latency trim still fires while muted (no unbounded backlog growth) ==\n");
    {
        unsigned char buf[8 * 2 * 2] = {0};
        ai_shm_t *v = make_voice(1.0f, AI_CHAN_LR, 0, 0.1f, AI_LATENCY_TRIGGER_FRAMES + 500);
        mix_in_one(2, v, buf, 8, 2, 4, 2);
        uint32_t backlog_after = (v->head - v->tail) & (AI_RING_FRAMES - 1);
        CHECK(g_trim_events[2] == 1, "one trim event recorded for this voice's slot");
        CHECK(backlog_after < AI_LATENCY_TRIGGER_FRAMES, "backlog pulled back under the trigger threshold");
        free(v);
    }

    printf("\n== ai_try_attach: lazy re-attach against a REAL POSIX shm segment ==\n");
    {
        unsigned slot = 3;   /* pick a slot no other test in this file touches */
        char name[24];
        ai_shm_name(slot, name, sizeof(name));
        shm_unlink(name);   /* start clean */

        CHECK(g_shm[slot] == NULL, "slot starts unattached");
        unsigned before = g_n_attached;

        ai_try_attach(slot);
        CHECK(g_shm[slot] == NULL, "attach attempt on a nonexistent segment stays unattached, no error");

        /* Now actually create the segment - mirrors what a voice host's
         * shm_setup() does, minus the ftruncate size (AI_SHM_BYTES is huge
         * because of the ring array; a real host also ftruncate()s to this
         * exact size, matching here so mmap succeeds identically). */
        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        CHECK(fd >= 0, "created real POSIX shm segment for the attach test");
        ftruncate(fd, AI_SHM_BYTES);
        void *m = mmap(NULL, AI_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        CHECK(m != MAP_FAILED, "mmap succeeded");
        ai_shm_t *producer_view = (ai_shm_t *)m;
        memset(producer_view, 0, AI_SHM_BYTES);
        producer_view->rate = 44100;
        producer_view->channels = 1;
        producer_view->enabled = 1;
        producer_view->gain = 1.0f;
        producer_view->channel_mask = AI_CHAN_LR;
        __atomic_store_n(&producer_view->magic, AI_MAGIC, __ATOMIC_RELEASE);

        ai_try_attach(slot);
        CHECK(g_shm[slot] != NULL, "attach succeeds once the segment exists (this is the whole point of lazy re-attach)");
        CHECK(g_n_attached == before + 1, "g_n_attached incremented exactly once");

        /* mix_in() (not just mix_in_one) should now pick this slot up on its
         * own, end to end, exactly like a real ALSA read would. */
        producer_view->ring[0] = 0.5f;
        producer_view->head = 1;
        unsigned char buf[1 * 2 * 2] = {0};
        mix_in(buf, 1, 2, 4, 2);
        CHECK(s16_at(buf, 0, 0, 2) == (int16_t)(0.5f * 32767.0f), "mix_in() picked up the newly-attached slot with no code change at the call site");

        ai_try_attach(slot);   /* already attached - must be a safe no-op */
        CHECK(g_n_attached == before + 1, "re-attaching an already-attached slot doesn't double-count");

        munmap(g_shm[slot], AI_SHM_BYTES);
        shm_unlink(name);
        g_shm[slot] = NULL;
        g_n_attached = before;
    }

    printf("\n== ai_try_attach: re-attach when a voice host restarts (2026-09-13 fix) ==\n");
    {
        /* Reproduces the live bug: a voice host that stops and restarts
         * shm_unlink()s + recreates its ring from scratch ("start clean"),
         * a NEW inode under the SAME name. Before this fix, ai_try_attach's
         * "already attached" check meant that new segment was never picked
         * up - forceAudioIn.so kept mixing from the old, orphaned one
         * forever: no crash, just silence. */
        unsigned slot = 3;
        char name[24];
        ai_shm_name(slot, name, sizeof(name));
        shm_unlink(name);
        unsigned before = g_n_attached;

        int fd1 = shm_open(name, O_CREAT | O_RDWR, 0666);
        ftruncate(fd1, AI_SHM_BYTES);
        void *m1 = mmap(NULL, AI_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd1, 0);
        close(fd1);
        ai_shm_t *first = (ai_shm_t *)m1;
        memset(first, 0, AI_SHM_BYTES);
        first->rate = 44100; first->channels = 1;
        first->enabled = 1; first->gain = 1.0f; first->channel_mask = AI_CHAN_LR;
        first->ring[0] = 0.5f; first->head = 1;
        __atomic_store_n(&first->magic, AI_MAGIC, __ATOMIC_RELEASE);

        ai_try_attach(slot);
        /* ai_try_attach does its OWN independent shm_open+mmap of the same
         * object - the kernel is free to (and typically does) hand back a
         * different virtual address than this test's own m1, even though
         * it's the same underlying memory. So check content, not pointer
         * identity - g_shm[slot] just needs to be attached at all. */
        CHECK(g_shm[slot] != NULL, "first attach picks up the original segment");
        CHECK(g_n_attached == before + 1, "g_n_attached incremented once for the first attach");

        unsigned char buf[1 * 2 * 2] = {0};
        mix_in(buf, 1, 2, 4, 2);
        CHECK(s16_at(buf, 0, 0, 2) == (int16_t)(0.5f * 32767.0f), "mix_in reads the original segment's data");

        /* Simulate the voice host restarting: unlink, then recreate under
         * the same name - a real second shm_open(O_CREAT) gets a fresh
         * inode, exactly like maze_host's shm_unlink()+shm_open() on every
         * start. */
        shm_unlink(name);
        int fd2 = shm_open(name, O_CREAT | O_RDWR, 0666);
        ftruncate(fd2, AI_SHM_BYTES);
        void *m2 = mmap(NULL, AI_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
        close(fd2);
        CHECK(m2 != m1, "sanity: the new segment is a different mapping than the old one");
        ai_shm_t *second = (ai_shm_t *)m2;
        memset(second, 0, AI_SHM_BYTES);
        second->rate = 44100; second->channels = 1;
        second->enabled = 1; second->gain = 1.0f; second->channel_mask = AI_CHAN_LR;
        second->ring[0] = -0.25f; second->head = 1;   /* different value - proves which segment gets read */
        __atomic_store_n(&second->magic, AI_MAGIC, __ATOMIC_RELEASE);

        ai_try_attach(slot);
        CHECK(g_n_attached == before + 1, "re-attach does not double-count g_n_attached (still one voice, not two)");

        memset(buf, 0, sizeof(buf));
        mix_in(buf, 1, 2, 4, 2);
        CHECK(s16_at(buf, 0, 0, 2) == (int16_t)(-0.25f * 32767.0f),
              "mix_in now reads the NEW segment's data (via ai_try_attach's own mapping) - the bug this fix closes");

        /* Clean up: this test's own m1/m2 mappings (separate from whatever
         * ai_try_attach itself mapped), plus g_shm[slot] (ai_try_attach's
         * own current mapping - its PREVIOUS one, from the first attach, is
         * deliberately leaked by design, see the comment in ai_try_attach -
         * that's real, bounded, intentional leakage this test doesn't need
         * to chase down). */
        munmap(m1, AI_SHM_BYTES);
        munmap(m2, AI_SHM_BYTES);
        if (g_shm[slot]) munmap(g_shm[slot], AI_SHM_BYTES);
        shm_unlink(name);
        g_shm[slot] = NULL;
        g_n_attached = before;
    }

    printf("\n== %d failure(s) ==\n", failures);
    return failures ? 1 : 0;
}
