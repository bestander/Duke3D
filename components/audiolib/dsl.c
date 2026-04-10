/*
 * dsl.c — ESP32 audio driver for Duke3D Multivoc.
 *
 * Replaces the original SDL_OpenAudio-based driver with a FreeRTOS task that
 * runs on Core 1 (same core as the game task) at lower priority (4 vs 5).
 *
 * Architecture:
 *   A single audio task on Core 0 wakes each buffer period using vTaskDelayUntil
 *   (period = samples_per_buffer * 1000 / sample_rate ms) so cadence tracks MixRate
 *   and does not drift. Then:
 *     1. Pre-fetches streaming voice data from SD (outside critical section).
 *     2. Acquires a portMUX critical section to protect the voice list.
 *     3. Calls MV_ServiceVoc() to mix one buffer.
 *     4. Releases the critical section.
 *     5. Calls platform_audio_write() — i2s_write may block briefly (see i2s_audio).
 *
 *   During SD card reads the game task blocks on SPI DMA semaphores; the
 *   audio task gets the CPU and delivers near-continuous audio.
 *
 *   DisableInterrupts / RestoreInterrupts use the same portMUX, so voice-list
 *   mutations in MV_PlayVoice / MV_StopVoice are atomic with the mixing loop.
 *   The portMUX supports nested Enter/Exit (same-core recursion).
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "dsl.h"
#include "mv_stream.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

/* MV_MixPage is incremented and mixed into by MV_ServiceVoc */
extern volatile int MV_MixPage;

/* Declared extern to avoid pulling C++ esp32_hal.h into this C file.
 * platform_audio_write is defined in esp32_hal.cpp as extern "C".       */
extern void platform_audio_write(const int16_t *pcm, int n);

static const char *TAG = "dsl";

/* portMUX serialises voice-list access between the audio task and the game
 * task on Core 1.  Supports recursive Enter/Exit from the same core.    */
static portMUX_TYPE g_audio_mux = portMUX_INITIALIZER_UNLOCKED;

static int   DSL_ErrorCode     = DSL_Ok;
static void (*_CallBackFunc)(void);
static volatile char *_BufferStart;
static int   _BufferSize;          /* bytes per mix page */
static int   _SampleRate;
static int   _mixer_initialized;

static TaskHandle_t g_audio_task  = NULL;

/* ------------------------------------------------------------------ */

char *DSL_ErrorString(int ErrorNumber)
{
    switch (ErrorNumber) {
    case DSL_Warning:
    case DSL_Error:             return DSL_ErrorString(DSL_ErrorCode);
    case DSL_Ok:                return "DSL ok.";
    case DSL_SDLInitFailure:    return "DSL init failed.";
    case DSL_MixerActive:       return "DSL already active.";
    case DSL_MixerInitFailure:  return "DSL mixer init failed.";
    default:                    return "Unknown DSL error.";
    }
}

static void DSL_SetErrorCode(int code) { DSL_ErrorCode = code; }

int DSL_Init(void)
{
    DSL_SetErrorCode(DSL_Ok);
    return DSL_Ok;
}

void DSL_Shutdown(void) { DSL_StopPlayback(); }

/* ------------------------------------------------------------------ */
/* Audio pump task                                                     */
/* Core 0, priority 4 — game runs on Core 1 at priority 5.           */
/* ------------------------------------------------------------------ */

static void audio_pump_task(void *arg)
{
    (void)arg;

    const int n_samp_cfg = _BufferSize > 0 ? _BufferSize / (int)sizeof(int16_t) : 256;
    const int sr = _SampleRate > 0 ? _SampleRate : 11025;
    uint32_t period_ms = (uint32_t)((n_samp_cfg * 1000 + sr / 2) / sr);
    if (period_ms < 1u) {
        period_ms = 1u;
    }
    TickType_t period_ticks = pdMS_TO_TICKS(period_ms);
    if (period_ticks < 1) {
        period_ticks = 1;
    }

    ESP_LOGD(TAG,
             "audio pump started (Core %d prio 4, buf %d B = %d samples, %d Hz -> "
             "period %u ms = %u ticks)",
             xPortGetCoreID(), _BufferSize, n_samp_cfg, sr,
             (unsigned)period_ms, (unsigned)period_ticks);

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, period_ticks);

        if (!_mixer_initialized || !_CallBackFunc || !_BufferStart)
            continue;

        /* Pre-fetch SD data for streaming voices BEFORE the critical section.
         * fread may block on SPI DMA; that is fine here since we do not hold
         * the portMUX.  Ping-pong halves: inactive half is filled while the
         * mixer consumes the active half (see multivoc.c).
         * Run several passes: many streaming voices may each need a half
         * filled; one walk only schedules one chunk per voice per call. */
        for (int pf = 0; pf < 5; pf++)
            MV_PrefetchStreams();

        /* Critical section: prevents game task from preempting us mid-
         * iteration of the voice linked list (same-core, nested-safe).  */
        int mixed_page;
        portENTER_CRITICAL(&g_audio_mux);
        _CallBackFunc();   /* MV_ServiceVoc: increments MV_MixPage, mixes */
        mixed_page = MV_MixPage;
        portEXIT_CRITICAL(&g_audio_mux);

        /* Fill stream halves that became empty during the mix before next period. */
        for (int pf = 0; pf < 4; pf++)
            MV_PrefetchStreams();

        /* Push the freshly-mixed page (same index MV_ServiceVoc wrote). */
        const int16_t *buf =
            (const int16_t *)(_BufferStart + mixed_page * _BufferSize);
        const int n_samp = _BufferSize / (int)sizeof(int16_t);
        platform_audio_write(buf, n_samp);
    }
}

/* ------------------------------------------------------------------ */

int DSL_BeginBufferedPlayback(char *BufferStart, int BufferSize, int NumDivisions,
                               unsigned SampleRate, int MixMode,
                               void (*CallBackFunc)(void))
{
    if (_mixer_initialized) {
        DSL_SetErrorCode(DSL_MixerActive);
        return DSL_Error;
    }

    /* BufferSize / NumDivisions = MV_BufferSize = MixBufferSize * MV_SampleSize,
     * which is already in bytes and already accounts for bit depth and channels.
     * Do NOT multiply by sample_bytes/channels again — that double-counts and
     * produces a 2× buffer size, causing reads across page boundaries and
     * high-frequency garbage on the I2S output.                             */
    _CallBackFunc  = CallBackFunc;
    _BufferStart   = BufferStart;
    _BufferSize    = BufferSize / NumDivisions;  /* bytes per mix page */
    _SampleRate    = (int)SampleRate;

    ESP_LOGD(TAG, "DSL_BeginBufferedPlayback rate=%u mode=0x%x pages=%d buf=%d B",
             SampleRate, MixMode, NumDivisions, _BufferSize);

    /* Pin to Core 0 — game task runs on Core 1 at priority 5; keeping audio on
     * Core 0 means audio gets its own CPU with no priority competition.
     * Stack allocated from PSRAM to avoid exhausting internal RAM (BT host
     * threads + WiFi + Hub75 DMA already consume most internal RAM).         */
    if (xTaskCreatePinnedToCoreWithCaps(audio_pump_task, "duke_audio",
                                        4096, NULL, 4, &g_audio_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "failed to create audio pump task");
        DSL_SetErrorCode(DSL_MixerInitFailure);
        return DSL_Error;
    }

    _mixer_initialized = 1;
    return DSL_Ok;
}

void DSL_StopPlayback(void)
{
    _mixer_initialized = 0;
    if (g_audio_task) {
        vTaskDelete(g_audio_task);
        g_audio_task = NULL;
    }
}

unsigned DSL_GetPlaybackRate(void) { return (unsigned)_SampleRate; }

/* ------------------------------------------------------------------ */
/* DisableInterrupts / RestoreInterrupts                               */
/*                                                                     */
/* On DOS these masked the hardware timer ISR that calls MV_ServiceVoc,*/
/* making voice-list mutations atomic.  Here we use the same portMUX   */
/* as the audio pump task's critical section.  When the game task holds*/
/* the mux (during MV_PlayVoice / MV_StopVoice), the audio task's     */
/* portENTER_CRITICAL cannot run concurrently on the same core.        */
/* ------------------------------------------------------------------ */

uint32_t DisableInterrupts(void)
{
    portENTER_CRITICAL(&g_audio_mux);
    return 0;
}

void RestoreInterrupts(uint32_t flags)
{
    (void)flags;
    portEXIT_CRITICAL(&g_audio_mux);
}
