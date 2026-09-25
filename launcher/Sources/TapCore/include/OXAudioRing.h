// SPDX-License-Identifier: MPL-2.0
//
// OXAudioRing — shared-memory PCM ring between the OXRSys launcher (writer,
// Core Audio process tap, arm64) and the OXRSys runtime (reader, inside the
// Wine/CrossOver process, x86_64 under Rosetta).
//
// Layout of the file (mmap'd MAP_SHARED by both sides):
//   [OXAudioRingHeader: 256 bytes][float samples[capacityFrames * channels]]
// Samples are interleaved float32. Single writer, single reader, lock-free:
// the writer copies frames, then publishes writeFrame with release semantics;
// the reader loads writeFrame with acquire semantics, copies, and re-checks
// writeFrame to detect an overrun (then it discards and resynchronizes).
//
// Keep this header byte-identical in:
//   oxrsys-winebridge/launcher/Sources/TapCore/include/OXAudioRing.h
//   oxrsys-src/runtime/src/OXAudioRing.h

#ifndef OX_AUDIO_RING_H
#define OX_AUDIO_RING_H

#include <stdint.h>

#define OX_AUDIO_RING_MAGIC 0x5241584Fu /* "OXAR" little-endian */
#define OX_AUDIO_RING_VERSION 1u
#define OX_AUDIO_RING_HEADER_BYTES 256u
#define OX_AUDIO_RING_DEFAULT_CAPACITY_FRAMES 32768u /* ~0.68 s at 48 kHz */
/* Relative to $HOME. */
#define OX_AUDIO_RING_RELATIVE_PATH "Library/Application Support/OXRSys/headset-audio.ring"
/* The reader treats the ring as dead when the heartbeat is older than this. */
#define OX_AUDIO_RING_STALE_NS 1000000000ull

#ifdef __cplusplus
#include <atomic>
typedef std::atomic<uint64_t> ox_ring_atomic_u64;
typedef std::atomic<uint32_t> ox_ring_atomic_u32;
#else
#include <stdatomic.h>
typedef _Atomic uint64_t ox_ring_atomic_u64;
typedef _Atomic uint32_t ox_ring_atomic_u32;
#endif

typedef struct OXAudioRingHeader
{
    uint32_t magic;          /* OX_AUDIO_RING_MAGIC */
    uint32_t version;        /* OX_AUDIO_RING_VERSION */
    uint32_t sampleRateHz;   /* tap rate (normally 48000) */
    uint32_t channels;       /* 2 */
    uint32_t capacityFrames; /* power of two */
    int32_t writerPid;
    ox_ring_atomic_u64 writeFrame;  /* total frames written, monotonic */
    ox_ring_atomic_u64 heartbeatNs; /* writer CLOCK_REALTIME ns, updated every IO cycle */
    ox_ring_atomic_u32 active;      /* 1 while the writer is capturing */
    ox_ring_atomic_u32 generation;  /* bumped on every (re)start or format change */
    uint32_t scope;                 /* 0 = unknown, 1 = game processes, 2 = all system audio */
    uint8_t reserved[OX_AUDIO_RING_HEADER_BYTES - 52];
} OXAudioRingHeader;

#ifdef __cplusplus
static_assert(sizeof(OXAudioRingHeader) == OX_AUDIO_RING_HEADER_BYTES, "ring header size");
#else
_Static_assert(sizeof(OXAudioRingHeader) == OX_AUDIO_RING_HEADER_BYTES, "ring header size");
#endif

#endif /* OX_AUDIO_RING_H */
