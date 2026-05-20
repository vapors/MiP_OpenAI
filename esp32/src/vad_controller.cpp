#include "vad_controller.h"

#include "ble_control.h"
#include "control_modes.h"
#include "mic.h"
#include "lib_websocket.h"
#include <math.h>

static const uint32_t VAD_START_RMS = 420;        // Tune: raise if false triggers, lower if it misses speech.
static const uint32_t VAD_STOP_RMS = 260;         // Hysteresis threshold below start.
static const uint32_t VAD_START_HOLD_MS = 140;    // Require speech energy for this long before auto-start.
static const uint32_t VAD_SILENCE_MS = 900;       // Auto-stop after this much silence.
static const uint32_t VAD_MIN_RECORD_MS = 750;    // Match server minimum guard.
static const uint32_t VAD_MAX_RECORD_MS = 9000;   // Safety cap for open-ended noise.
static const uint32_t VAD_RETRIGGER_GUARD_MS = 900;
static const uint32_t VAD_LOG_MS = 500;

static bool g_speechActive = false;
static uint32_t g_aboveStartSinceMs = 0;
static uint32_t g_lastSpeechMs = 0;
static uint32_t g_recordStartedMs = 0;
static uint32_t g_lastStopMs = 0;
static uint32_t g_suppressedUntilMs = 0;
static uint32_t g_lastRms = 0;
static uint32_t g_lastLogMs = 0;

static uint32_t computeRms(const int16_t* samples, size_t frames)
{
  if (!samples || frames == 0) return 0;

  uint64_t sumSq = 0;
  for (size_t i = 0; i < frames; i++)
  {
    int32_t s = samples[i];
    sumSq += (uint64_t)(s * s);
  }

  return (uint32_t)sqrt((double)sumSq / (double)frames);
}

void setupVadController()
{
  vadReset();
  Serial.printf("[VAD] Ready rmsStart=%lu rmsStop=%lu silence=%lums\n",
                (unsigned long)VAD_START_RMS,
                (unsigned long)VAD_STOP_RMS,
                (unsigned long)VAD_SILENCE_MS);
}

void vadReset()
{
  g_speechActive = false;
  g_aboveStartSinceMs = 0;
  g_lastSpeechMs = 0;
  g_recordStartedMs = 0;
  g_lastRms = 0;
}

void vadSuppressForMs(uint32_t ms)
{
  uint32_t until = millis() + ms;
  if (until > g_suppressedUntilMs)
  {
    g_suppressedUntilMs = until;
  }
}

bool vadIsListening()
{
  return isGptVadMode();
}

bool vadSpeechActive()
{
  return g_speechActive;
}

uint32_t vadLastRms()
{
  return g_lastRms;
}

void vadProcessFrames(const int16_t* samples, size_t frames)
{
  if (!vadIsListening())
  {
    if (g_speechActive || g_aboveStartSinceMs != 0)
    {
      vadReset();
    }
    return;
  }

  const uint32_t now = millis();

  // Avoid triggering on the assistant's own speaker audio or immediately after
  // a response. This is a simple first-pass echo guard.
  if ((int32_t)(g_suppressedUntilMs - now) > 0)
  {
    g_aboveStartSinceMs = 0;
    return;
  }

  // Do not start if the network is not ready.
  if (!client.available())
  {
    g_aboveStartSinceMs = 0;
    return;
  }

  const uint32_t rms = computeRms(samples, frames);
  g_lastRms = rms;

  if ((now - g_lastLogMs) >= VAD_LOG_MS && (g_speechActive || rms > VAD_START_RMS))
  {
    Serial.printf("[VAD] rms=%lu active=%d recording=%d\n",
                  (unsigned long)rms,
                  g_speechActive ? 1 : 0,
                  getRecordingState() ? 1 : 0);
    g_lastLogMs = now;
  }

  if (!g_speechActive)
  {
    if ((now - g_lastStopMs) < VAD_RETRIGGER_GUARD_MS)
    {
      return;
    }

    if (rms >= VAD_START_RMS)
    {
      if (g_aboveStartSinceMs == 0)
      {
        g_aboveStartSinceMs = now;
      }

      if ((now - g_aboveStartSinceMs) >= VAD_START_HOLD_MS)
      {
        g_speechActive = true;
        g_lastSpeechMs = now;
        g_recordStartedMs = now;

        Serial.println("[VAD] speech_start -> beginPttRecording");
        beginPttRecording("vad");
      }
    }
    else
    {
      g_aboveStartSinceMs = 0;
    }

    return;
  }

  // Already active: update speech/silence state.
  if (rms >= VAD_STOP_RMS)
  {
    g_lastSpeechMs = now;
  }

  const bool minRecordMet = (now - g_recordStartedMs) >= VAD_MIN_RECORD_MS;
  const bool silenceTimedOut = (now - g_lastSpeechMs) >= VAD_SILENCE_MS;
  const bool maxRecordMet = (now - g_recordStartedMs) >= VAD_MAX_RECORD_MS;

  if ((minRecordMet && silenceTimedOut) || maxRecordMet)
  {
    g_speechActive = false;
    g_aboveStartSinceMs = 0;
    g_lastStopMs = now;

    Serial.println(maxRecordMet ? "[VAD] max_record -> endPttRecording"
                                : "[VAD] silence -> endPttRecording");
    endPttRecording("vad");
  }
}
