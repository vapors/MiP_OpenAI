#include "audio_tx_queue.h"

// -----------------------------------------------------------------------------
// LEGACY / NOT USED BY CURRENT MIC STREAM
// -----------------------------------------------------------------------------
// The current stable input path streams microphone PCM directly through
// lib_websocket::sendBinaryData(), which uses a mutex-protected real-time send.
// Keep this file only as an experimental fallback. Do not call setupAudioTxQueue()
// or serviceAudioTxQueue() unless intentionally testing queued mic audio.
// -----------------------------------------------------------------------------

#include "config.h"
#include "lib_websocket.h"

struct AudioTxChunk
{
  uint16_t frames;
  int16_t samples[bufferLen];
};

static QueueHandle_t g_audioTxQueue = nullptr;
static const uint8_t AUDIO_TX_QUEUE_LEN = 8;
static uint32_t g_droppedChunks = 0;
static uint32_t g_enqueuedChunks = 0;
static uint32_t g_sentChunks = 0;
static uint32_t g_lastQueueLogMs = 0;
static const uint32_t AUDIO_TX_SLOW_SEND_WARN_MS = 80;

bool setupAudioTxQueue()
{
  if (g_audioTxQueue) return true;

  g_audioTxQueue = xQueueCreate(AUDIO_TX_QUEUE_LEN, sizeof(AudioTxChunk));
  if (!g_audioTxQueue)
  {
    Serial.println("[AUDIO TX] Failed to create audio TX queue");
    return false;
  }

  Serial.printf("[AUDIO TX] Queue ready: %u chunks x %u frames\n",
                AUDIO_TX_QUEUE_LEN, bufferLen);
  return true;
}

void clearAudioTxQueue()
{
  if (g_audioTxQueue)
  {
    xQueueReset(g_audioTxQueue);
  }
}

bool enqueueAudioChunk(const int16_t* samples, size_t frames)
{
  if (!g_audioTxQueue || !samples || frames == 0) return false;

  AudioTxChunk chunk;
  if (frames > bufferLen) frames = bufferLen;

  chunk.frames = (uint16_t)frames;
  memcpy(chunk.samples, samples, frames * sizeof(int16_t));

  if (xQueueSend(g_audioTxQueue, &chunk, 0) != pdTRUE)
  {
    // Drop oldest chunk, then try once more. This preserves recency and avoids
    // blocking the mic task if the network stalls.
    AudioTxChunk dropped;
    xQueueReceive(g_audioTxQueue, &dropped, 0);

    if (xQueueSend(g_audioTxQueue, &chunk, 0) != pdTRUE)
    {
      g_droppedChunks++;
      Serial.printf("[AUDIO TX %lu ms] Queue full; dropped mic chunk total=%lu\n", millis(), (unsigned long)g_droppedChunks);
      return false;
    }
  }

  g_enqueuedChunks++;
  uint32_t now = millis();
  if (g_enqueuedChunks <= 3 || (now - g_lastQueueLogMs) >= 500)
  {
    /*
    Serial.printf("[AUDIO TX %lu ms] enqueue=%lu depth=%u frames=%u dropped=%lu\n",
                  now,
                  (unsigned long)g_enqueuedChunks,
                  (unsigned)audioTxQueueDepth(),
                  (unsigned)chunk.frames,
                  (unsigned long)g_droppedChunks);
    */
    Serial.printf("[AUDIO TX %lu ms] enqueue=%lu depth=%u frames=%u dropped=%lu\n",
              (unsigned long)now,
              (unsigned long)g_enqueuedChunks,
              (unsigned int)audioTxQueueDepth(),
              (unsigned int)chunk.frames,
              (unsigned long)g_droppedChunks);
    g_lastQueueLogMs = now;
  }

  return true;
}

void serviceAudioTxQueue(uint8_t maxChunks)
{
  if (!g_audioTxQueue) return;

  AudioTxChunk chunk;
  uint8_t sent = 0;

  while (sent < maxChunks && xQueueReceive(g_audioTxQueue, &chunk, 0) == pdTRUE)
  {
    if (chunk.frames > 0)
    {
      uint32_t sendStart = millis();

      sendBinaryData(chunk.samples, chunk.frames * sizeof(int16_t));

      uint32_t sendElapsed = millis() - sendStart;

      g_sentChunks++;
      sent++;

      if (sendElapsed > 80)
      {
        Serial.printf("[AUDIO TX %lu ms] slow send=%lu ms sent=%lu depth=%u dropped=%lu\n",
                      (unsigned long)millis(),
                      (unsigned long)sendElapsed,
                      (unsigned long)g_sentChunks,
                      (unsigned int)audioTxQueueDepth(),
                      (unsigned long)g_droppedChunks);

        // If WebSocket send is blocking badly, do not keep looping here.
        // Let BLE/WebSocket polling breathe.
        break;
      }
    }
  }
}

size_t audioTxQueueDepth()
{
  if (!g_audioTxQueue) return 0;
  return uxQueueMessagesWaiting(g_audioTxQueue);
}
