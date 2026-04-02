/*
 * dsl.c — ESP32 audio driver for Duke3D Multivoc.
 *
 * Replaces the original SDL_OpenAudio-based driver with a FreeRTOS task that
 * runs on Core 1 (same core as the game task) at lower priority (4 vs 5).
 *
 * Architecture:
 *   A single audio task sleeps for ~23 ms (one buffer period at 11025 Hz /
 *   256 samples) then:
 *     1. Acquires a portMUX critical section to protect the voice list.
 *     2. Calls MV_ServiceVoc() to mix one buffer.
 *     3. Releases the critical section.
 *     4. Calls platform_audio_write() — i2s_write with ticks_to_wait=0
 *        (non-blocking; game task is never stalled waiting for I2S hardware).
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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
/* Core 1, priority 4 — below game task at priority 5.                */
/* Runs when game task is blocked (SD reads, WiFi, vTaskDelay, etc.)  */
/* ------------------------------------------------------------------ */

static void audio_pump_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "audio pump started (Core %d prio 4, buf %d bytes @ %d Hz)",
             xPortGetCoreID(), _BufferSize, _SampleRate);

    for (;;) {
        /* Sleep one buffer period.  During this sleep the game task runs. */
        vTaskDelay(pdMS_TO_TICKS(23));

        if (!_mixer_initialized || !_CallBackFunc || !_BufferStart)
            continue;

        /* Critical section: prevents game task from preempting us mid-
         * iteration of the voice linked list (same-core, nested-safe).  */
        portENTER_CRITICAL(&g_audio_mux);
        _CallBackFunc();   /* MV_ServiceVoc: increments MV_MixPage, mixes */
        portEXIT_CRITICAL(&g_audio_mux);

        /* Push the freshly-mixed page; non-blocking (ticks=0). */
        const int16_t *buf =
            (const int16_t *)(_BufferStart + MV_MixPage * _BufferSize);
        platform_audio_write(buf, _BufferSize / (int)sizeof(int16_t));
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

    ESP_LOGI(TAG, "DSL_BeginBufferedPlayback rate=%u mode=0x%x pages=%d buf=%d B",
             SampleRate, MixMode, NumDivisions, _BufferSize);

    if (xTaskCreatePinnedToCore(audio_pump_task, "duke_audio",
                                4096, NULL, 4, &g_audio_task, 1) != pdPASS) {
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
