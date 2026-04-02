/*
 * mv_stream.h — SD-streaming sound API for ESP32.
 *
 * Replaces the PSRAM-buffered sound approach: each playing voice reads raw PCM
 * bytes directly from DUKE3D.GRP on the SD card, eliminating all PSRAM usage
 * for sound data.
 *
 * Usage:
 *   1. Call MV_OpenGRPStream(path) once after FX_Init to register the GRP path.
 *      Opens one shared FILE*; prefetch and MV_GrpStreamReadAt() take a mutex
 *      so only one fseek/fread runs at a time (fewer VFS ops than fopen/fclose
 *      per chunk; reopen on fseek failure under contention).
 *   2. Call MV_PrefetchStreams() from the audio pump task BEFORE entering the
 *      portMUX critical section.  It fills the inactive half (MV_STREAM_HALF
 *      bytes) of each voice's ping-pong buffer (never overwriting the active half).
 *   3. Start a streaming voice with MV_PlayStream3D() instead of
 *      FX_PlayVOC3D()/FX_PlayWAV3D().  The caller must supply the first 512
 *      bytes (init_buf) so the very first mix period has data immediately.
 */

#pragma once
#ifndef PLATFORM_DOS

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open a persistent GRP FILE* used by MV_PrefetchStreams() and MV_GrpStreamReadAt().
 * Call once after FX_Init, before any MV_PlayStream3D calls. */
void MV_OpenGRPStream( const char *grp_path );

/* Read len bytes at absolute GRP offset; mutex-serialized. Returns bytes read or -1. */
int MV_GrpStreamReadAt( int64_t abs_off, void *buf, size_t len );

/* Close shared GRP handle (e.g. MV_Shutdown). Safe if never opened. */
void MV_CloseGRPStream( void );

/* Pre-fetch streaming voice buffers.  Called by audio_pump_task BEFORE
 * portENTER_CRITICAL.  Reads up to 512 bytes per empty streaming voice.
 * Safe to call without the portMUX: traversal doesn't yield; game task can
 * only run during fread (SPI DMA wait), at which point we've already captured
 * the voice pointer — a wasted write to a just-stopped voice is harmless. */
void MV_PrefetchStreams( void );

/* Monotonic counters for diagnostics (e.g. host FPS line). */
uint32_t MV_StreamUnderrunTotal( void );
uint32_t MV_StreamPrefetchShortTotal( void );

/* Begin 3D-positioned streaming playback.
 *   grp_pcm_offset  absolute byte offset of raw PCM data in the GRP file
 *   pcm_total_len   total PCM byte length
 *   rate            source sample rate (Hz), e.g. 8000 or 11025
 *   init_buf/len    first chunk of PCM (caller reads this from GRP; up to
 *                   MV_STREAM_HALF bytes; avoids silence on the first mix period)
 *   pitch/angle/distance/priority/callbackval — same as FX_PlayVOC3D */
int MV_PlayStream3D( int32_t grp_pcm_offset, int32_t pcm_total_len,
                     unsigned rate,
                     uint8_t *init_buf, int32_t init_len,
                     int pitch, int angle, int distance,
                     int priority, unsigned long callbackval );

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_DOS */
