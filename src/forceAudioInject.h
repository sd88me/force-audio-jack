/*
 * Shared memory layout between the injector process(es) (e.g. injectTone,
 * maze_host, or any future audio-rendering voice host built the same way)
 * and forceAudioIn.so, the LD_PRELOAD shim that mixes injected audio into
 * what /usr/bin/MPC reads from its capture device.
 *
 * The producer (injector) always writes 32-bit float, mono or stereo, at a
 * fixed rate it declares in the header. forceAudioIn.so does the conversion
 * to whatever format/channel count MPC actually configured on the capture
 * handle - the producer never needs to know or care what MPC is doing.
 *
 * MULTIPLE SIMULTANEOUS VOICES: each voice host is the sole producer of its
 * OWN ring, in its own shared-memory segment, named by a small integer slot
 * (AI_SHM_NAME_FMT). forceAudioIn.so attaches to every slot that exists
 * (0..AI_MAX_VOICES-1) and sums them all into the one real capture buffer -
 * this keeps every ring genuinely single-producer/single-consumer (no
 * cross-process synchronization needed beyond what already exists per ring)
 * rather than trying to make one ring handle multiple writers. A voice with
 * no shared-memory segment present is simply not attached - same
 * "disabled by default, passthrough-only" contract as before, per slot.
 *
 * PER-VOICE MIX CONTROL: `enabled`/`gain`/`channel_mask` are the host-level
 * "voice out" controls (on/off, volume, L/R/L+R routing) a voice's own
 * control socket writes directly - there is no separate central mixer
 * process. `enabled` mutes the *mix*, not the render: forceAudioIn.so still
 * drains the ring at the normal rate while muted (see forceAudioIn.c's
 * mix_in) so a re-enabled voice doesn't resume from a stale backlog, and the
 * producer's render cadence - which is the synth's actual clock for envelopes
 * and filters - is never told to skip a tick.
 */
#ifndef FORCE_AUDIO_INJECT_H
#define FORCE_AUDIO_INJECT_H

#include <stdint.h>
#include <stdio.h>

#define AI_SHM_NAME_FMT "/forceAudioInject%u"  /* %u = voice slot, 0..AI_MAX_VOICES-1 */
#define AI_MAX_VOICES  4              /* how many simultaneous voice hosts forceAudioIn.so will attach to */
#define AI_MAGIC       0x414e4a49u   /* 'AINJ' */
#define AI_RING_FRAMES (1u << 16)    /* 65536 frames of ring, per channel slot */
#define AI_MAX_CH      2

/* channel_mask values - which real capture channel(s) this voice lands on.
 * The tapped capture handle is confirmed 2-channel (see DESIGN.md): L/R are
 * the only two real destinations, so a voice's "channel select" is really
 * "L, R, or both", not an arbitrary channel count. Two voices routed to the
 * same channel simply sum there, same as two synths sharing a mixer channel. */
#define AI_CHAN_L      1u
#define AI_CHAN_R      2u
#define AI_CHAN_LR     (AI_CHAN_L | AI_CHAN_R)

typedef struct {
    uint32_t magic;
    uint32_t rate;                 /* producer's sample rate, e.g. 44100      */
    uint32_t channels;             /* producer's channel count: 1 or 2        */

    /* Host-level mix controls - written by the voice's own control socket
     * (e.g. maze_host's "SET mix.gain"), read by forceAudioIn.so's mix_in()
     * on the audio thread. Plain volatile reads/writes, not the acquire/
     * release pattern used for head/tail below: these are independent
     * scalars (not part of the ring's producer/consumer handshake), each a
     * single naturally-aligned 32-bit word, which is atomic on ARMv7 by
     * hardware guarantee alone - a torn read is not possible, so the extra
     * ceremony of __atomic_* would add nothing here. */
    volatile uint32_t enabled;      /* 1 = mixed into output, 0 = muted (ring still drained) */
    volatile float    gain;         /* linear gain applied at mix time, default 1.0 */
    volatile uint32_t channel_mask; /* AI_CHAN_L | AI_CHAN_R, default AI_CHAN_LR */

    /* SPSC ring, in frames (not bytes) - one producer (injector), one
     * consumer (the tap, running on MPC's real-time capture thread). Samples
     * are interleaved float32, `channels` per frame. */
    volatile uint32_t head;        /* producer writes (frames produced)       */
    volatile uint32_t tail;        /* consumer writes (frames consumed)       */

    volatile uint64_t frames_written;   /* producer-side stats               */
    volatile uint64_t frames_consumed;  /* consumer-side stats               */
    volatile uint64_t underruns;        /* consumer had nothing to mix in    */

    float ring[AI_RING_FRAMES * AI_MAX_CH];
} ai_shm_t;

#define AI_SHM_BYTES (sizeof(ai_shm_t))

/* Formats the shm name for voice `slot` into `buf` (>= 24 bytes). A plain
 * static-buffer sprintf would race across threads/processes calling it
 * concurrently with different slots - callers each pass their own buffer,
 * so there's nothing to race here. */
static inline void ai_shm_name(unsigned slot, char *buf, size_t buflen) {
    snprintf(buf, buflen, AI_SHM_NAME_FMT, slot);
}

#endif
