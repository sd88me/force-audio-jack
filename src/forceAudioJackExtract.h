/*
 * Shared memory layout for an EXTRACTION ring - the reverse of
 * forceAudioInject.h's ai_shm_t: here forceAudioJack.so (in its snd_pcm_writei
 * hook) is the PRODUCER, copying MPC's own real main-mix audio (channels
 * 0/1 of the confirmed 4-channel ADA2 playback handle) out for a separate
 * consumer process to use. A voice-host ring answers "how do I get audio
 * INTO MPC"; this answers "how do I get MPC's own output OUT to a separate
 * process" - same SPSC ring shape, producer/consumer roles swapped.
 *
 * Currently has exactly one consumer in this project: skipbackHost (see
 * docs/PROPOSAL-force-audio-jack.md). Link Audio is deliberately NOT built
 * on this ring - macdigi's force-link-audio has its own working tap+UDP+
 * discovery daemon already, and reimplementing that protocol from scratch
 * without its source risks producing something that compiles and sends
 * packets but doesn't actually interoperate with real Ableton Live/iPad
 * receivers. Its own snd_pcm_writei interposer coexists fine alongside this
 * one (multiple LD_PRELOAD interposers already stack safely in this
 * ecosystem via dlsym(RTLD_NEXT, ...) chaining) - no extraction ring needed
 * for that path.
 *
 * OWNERSHIP: unlike a voice-injection ring (voice host creates the segment,
 * forceAudioJack.so lazily attaches as consumer), here skipbackHost creates
 * the segment (shm_open O_CREAT, started on demand via its own Modules-page
 * entry - never at boot, same operational rule as every voice host) and
 * forceAudioJack.so lazily attaches to it as PRODUCER. This keeps the existing
 * "nothing is ever attached until something is deliberately started, lazy
 * re-attach picks it up within ~2s" safety model completely intact, just
 * with the producer/consumer roles swapped for this one ring.
 *
 * OVERRUN POLICY: the producer (forceAudioJack.so, on MPC's real-time audio
 * thread) must never block waiting for a slow consumer. If skipbackHost
 * falls behind by more than a full ring lap, the producer simply keeps
 * writing at `head` and overwrites the oldest still-unread frames, bumping
 * `overruns` - the mirror image of an injection ring's underrun (there, the
 * consumer has nothing to mix and skips; here, the producer has nowhere new
 * to write and overwrites). At AX_RING_FRAMES (~1.49s of headroom at
 * 44.1kHz) against skipbackHost's own ~200ms poll cadence, this should never
 * fire in practice - it exists as a fail-safe, not an expected path.
 */
#ifndef FORCE_AUDIO_JACK_EXTRACT_H
#define FORCE_AUDIO_JACK_EXTRACT_H

#include <stdint.h>
#include <stdio.h>

#define AX_SHM_NAME_SKIPBACK "/forceAudioJackSkipback0"
#define AX_MAGIC       0x414a4b53u   /* 'AJKS' - distinct from AI_MAGIC so a foreign/stale segment is never misread as the other ring type */
#define AX_RING_FRAMES (1u << 16)    /* same conveyor size as the injection rings - this is only the low-latency handoff, NOT skipbackHost's own (much larger) rolling buffer */
#define AX_CHANNELS    2             /* confirmed main-mix width - see DESIGN.md/PROPOSAL's hardware findings */

typedef struct {
    uint32_t magic;
    uint32_t rate;
    uint32_t channels;              /* always AX_CHANNELS in practice, kept explicit for the same reason ai_shm_t does */

    volatile uint32_t head;         /* forceAudioJack.so (producer) writes */
    volatile uint32_t tail;         /* skipbackHost (consumer) writes */

    volatile uint64_t frames_written;   /* producer-side stats */
    volatile uint64_t frames_consumed;  /* consumer-side stats */
    volatile uint64_t overruns;         /* producer had to overwrite unread frames - consumer too slow */

    float ring[AX_RING_FRAMES * AX_CHANNELS];
} ax_shm_t;

#define AX_SHM_BYTES (sizeof(ax_shm_t))

#endif
