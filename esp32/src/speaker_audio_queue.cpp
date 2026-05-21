#include "speaker_audio_queue.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

#include "lib_speaker.h"
#include "vad_controller.h"

// Most server chunks are 1024 bytes = 512 PCM16 mono frames.
// This supports slightly larger chunks while keeping the FreeRTOS queue predictable.
static const size_t SPEAKER_QUEUE_MAX_FRAMES = 1024;

// 48 chunks gives about:
// 512 frames/chunk / 16000 Hz = 32 ms per chunk
// 48 chunks = about 1.5 seconds of buffer
static const uint8_t SPEAKER_QUEUE_LEN = 48;

// Shorter I2S timeout keeps the speaker task from blocking too long.
static const uint32_t SPEAKER_WRITE_TIMEOUT_MS = 80;

// Keep VAD muted while assistant audio is queued/playing and briefly afterward.
static const uint32_t SPEAKER_VAD_SUPPRESS_MS = 1800;
//speaker prebuffer
static const uint8_t SPEAKER_PREBUFFER_CHUNKS = 4;
// Minimal log interval.
static const uint32_t SPEAKER_LOG_INTERVAL_MS = 1500;
static const uint32_t SPEAKER_PREBUFFER_TIMEOUT_MS = 120;
struct SpeakerAudioChunk
{
  uint16_t frames;
  int16_t samples[SPEAKER_QUEUE_MAX_FRAMES];
};

static QueueHandle_t g_speakerQueue = nullptr;
static TaskHandle_t g_speakerTaskHandle = nullptr;

static volatile bool g_speakerPlaying = false;

static uint32_t g_chunksPlayed = 0;
static uint32_t g_chunksDropped = 0;
static uint32_t g_lastLogMs = 0;
static uint32_t g_lastReportedDropped = 0;

bool speakerAudioQueueIsPlaying()
{
  return g_speakerPlaying;
}

void clearSpeakerAudioQueue()
{
  if (g_speakerQueue)
  {
    xQueueReset(g_speakerQueue);
  }

  g_speakerPlaying = false;
  g_mouthLevel = 0;
}

static void logSpeakerQueueStatus(const char* reason, bool force = false)
{
  if (!g_speakerQueue) return;

  const uint32_t now = millis();

  const bool droppedChanged = (g_chunksDropped != g_lastReportedDropped);
  const bool intervalElapsed = (now - g_lastLogMs) >= SPEAKER_LOG_INTERVAL_MS;

  if (!force && !droppedChanged && !intervalElapsed)
  {
    return;
  }
/*
  Serial.printf("[SPK Q] %s depth=%u played=%lu dropped=%lu heap=%u\n",
                reason,
                (unsigned int)uxQueueMessagesWaiting(g_speakerQueue),
                (unsigned long)g_chunksPlayed,
                (unsigned long)g_chunksDropped,
                ESP.getFreeHeap());
*/
  g_lastLogMs = now;
  g_lastReportedDropped = g_chunksDropped;
}

bool enqueueSpeakerAudioChunk(const int16_t* pcm, size_t frames)
{
  if (!pcm || frames == 0)
  {
    return false;
  }

  if (!g_speakerQueue)
  {
    Serial.println("[SPK Q] enqueue failed; queue not ready");
    return false;
  }

  SpeakerAudioChunk chunk;
  chunk.frames = (uint16_t)min(frames, SPEAKER_QUEUE_MAX_FRAMES);
  memcpy(chunk.samples, pcm, chunk.frames * sizeof(int16_t));

  // Keep VAD suppressed as soon as assistant audio arrives,
  // even before the speaker task has time to play it.
  vadSuppressForMs(SPEAKER_VAD_SUPPRESS_MS);

  BaseType_t ok = xQueueSend(g_speakerQueue, &chunk, 0);

  if (ok != pdTRUE)
  {
    // Queue is full. Drop the oldest chunk, then enqueue the newest one.
    // This helps preserve the end of the current response.
    SpeakerAudioChunk discarded;

    if (xQueueReceive(g_speakerQueue, &discarded, 0) == pdTRUE)
    {
      g_chunksDropped++;
      ok = xQueueSend(g_speakerQueue, &chunk, 0);
    }
    else
    {
      g_chunksDropped++;
    }
  }

  if (ok != pdTRUE)
  {
    logSpeakerQueueStatus("enqueue_failed", true);
    return false;
  }

  // Minimal status logging only.
  logSpeakerQueueStatus("enqueue");

  return true;
}

static void speakerAudioTask(void* parameter)
{
  Serial.println("[SPK Q] speaker audio task started");

  SpeakerAudioChunk chunk;

  while (true)
  {
    if (!g_speakerPlaying)
    {
      const uint32_t prebufferStart = millis();

      while (uxQueueMessagesWaiting(g_speakerQueue) < SPEAKER_PREBUFFER_CHUNKS)
      {
        const UBaseType_t depth = uxQueueMessagesWaiting(g_speakerQueue);

        if (depth == 0)
        {
          break;
        }

        if ((millis() - prebufferStart) >= SPEAKER_PREBUFFER_TIMEOUT_MS)
        {
          break;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    if (xQueueReceive(g_speakerQueue, &chunk, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      g_speakerPlaying = true;
      vadSuppressForMs(SPEAKER_VAD_SUPPRESS_MS);

      const uint32_t t0 = millis();
      const bool ok = speaker_write_mono_i16(
        chunk.samples,
        chunk.frames,
        SPEAKER_WRITE_TIMEOUT_MS
      );
      const uint32_t dt = millis() - t0;

      g_chunksPlayed++;
/*
      // Only log slow/problem writes. Normal chunks stay quiet.
      if (!ok || dt > 90)
      {
        Serial.printf("[SPK Q] slow_play frames=%u writeMs=%lu ok=%d depth=%u played=%lu dropped=%lu heap=%u\n",
                      (unsigned int)chunk.frames,
                      (unsigned long)dt,
                      ok ? 1 : 0,
                      (unsigned int)uxQueueMessagesWaiting(g_speakerQueue),
                      (unsigned long)g_chunksPlayed,
                      (unsigned long)g_chunksDropped,
                     ESP.getFreeHeap());
      }
*/
      // If more audio is queued, keep the playing flag high.
      // Otherwise allow the face to close after a tiny settle time.
      if (uxQueueMessagesWaiting(g_speakerQueue) == 0)
      {
        vTaskDelay(pdMS_TO_TICKS(80));

        if (uxQueueMessagesWaiting(g_speakerQueue) == 0)
        {
          g_speakerPlaying = false;
          g_mouthLevel = 0;
          vadSuppressForMs(900);
        }
      }
    }
    else
    {
      if (g_speakerPlaying)
      {
        g_speakerPlaying = false;
        g_mouthLevel = 0;
        vadSuppressForMs(900);
      }
    }
  }
}

void setupSpeakerAudioQueue()
{
  if (g_speakerQueue)
  {
    return;
  }

  g_speakerQueue = xQueueCreate(SPEAKER_QUEUE_LEN, sizeof(SpeakerAudioChunk));

  if (!g_speakerQueue)
  {
    Serial.println("[SPK Q] ERROR: failed to create speaker queue");
    return;
  }
/*
  xTaskCreatePinnedToCore(
    speakerAudioTask,
    "speakerAudioTask",
    8192,
    nullptr,
    2,
    &g_speakerTaskHandle,
    1
  );
*/

  xTaskCreatePinnedToCore(
    speakerAudioTask,
    "speakerAudioTask",
    8192,
    nullptr,
    3,
    &g_speakerTaskHandle,
    0
  );

 // Serial.printf("[SPK Q] ready: %u chunks x %u frames\n",
  //              (unsigned int)SPEAKER_QUEUE_LEN,
  //              (unsigned int)SPEAKER_QUEUE_MAX_FRAMES);
}