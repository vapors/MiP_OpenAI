#include "vad_controller.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "ble_control.h"
#include "control_modes.h"
#include "lib_websocket.h"
#include "mic.h"
#include "robot_status.h"

// -----------------------------------------------------------------------------
// RMS VAD tuning
// -----------------------------------------------------------------------------
// 16 kHz PCM16 mono, bufferLen = 512 means each VAD frame is ~32 ms.
//
// Start threshold should be higher than stop threshold. Your current file had
// VAD_START_RMS = 200 and VAD_STOP_RMS = 260, which makes false starts more
// likely and weakens the intended hysteresis.
//
// If VAD misses your voice: lower VAD_START_RMS in small steps.
// If VAD triggers on motors/speaker/noise: raise VAD_START_RMS.
static const uint32_t VAD_START_RMS = 360;
static const uint32_t VAD_STOP_RMS = 260;
static const uint32_t VAD_START_HOLD_MS = 160;
//static const uint32_t VAD_SILENCE_MS = 950;
static const uint32_t VAD_SILENCE_MS = 700;

// Keep this comfortably below the server-side 750 ms minimum because the BLE/PTT
// layer also enforces its own minimum before sending STOP_RECORD.
static const uint32_t VAD_MIN_RECORD_MS = 650;

static const uint32_t VAD_MAX_RECORD_MS = 9000;
static const uint32_t VAD_RETRIGGER_GUARD_MS = 1200;
static const uint32_t VAD_LOG_MS = 500;

// Additional local lockout after robot actions/motor motion. This prevents
// motor noise, wheel balancing, speaker output, and physical settling from
// immediately retriggering VAD.
static const uint32_t VAD_ACTION_BUSY_SUPPRESS_MS = 1500;
static const uint32_t VAD_ACTION_DONE_SUPPRESS_MS = 2000;

static bool g_speechActive = false;
static uint32_t g_aboveStartSinceMs = 0;
static uint32_t g_lastSpeechMs = 0;
static uint32_t g_recordStartedMs = 0;
static uint32_t g_lastStopMs = 0;
static uint32_t g_suppressedUntilMs = 0;
static uint32_t g_lastRms = 0;
static uint32_t g_lastLogMs = 0;
static bool g_wasActionBusy = false;

static uint32_t computeRms(const int16_t* samples, size_t frames)
{
  if (!samples || frames == 0) return 0;

  uint64_t sumSq = 0;
  for (size_t i = 0; i < frames; i++)
  {
    const int32_t s = samples[i];
    sumSq += (uint64_t)(s * s);
  }

  return (uint32_t)sqrt((double)sumSq / (double)frames);
}

static void resetStartDetector()
{
  g_aboveStartSinceMs = 0;
}

static void resetVadActivity()
{
  g_speechActive = false;
  g_aboveStartSinceMs = 0;
  g_lastSpeechMs = 0;
  g_recordStartedMs = 0;
}

static bool actionIsBusy()
{
  const char* actionState = getRobotActionState();

  return actionState &&
         strcmp(actionState, "idle") != 0 &&
         strcmp(actionState, "") != 0;
}

static bool vadSuppressed(uint32_t now)
{
  return ((int32_t)(g_suppressedUntilMs - now) > 0);
}

void setupVadController()
{
  vadReset();

  Serial.printf("[VAD] Ready rmsStart=%lu rmsStop=%lu hold=%lums silence=%lums min=%lums max=%lums\n",
                (unsigned long)VAD_START_RMS,
                (unsigned long)VAD_STOP_RMS,
                (unsigned long)VAD_START_HOLD_MS,
                (unsigned long)VAD_SILENCE_MS,
                (unsigned long)VAD_MIN_RECORD_MS,
                (unsigned long)VAD_MAX_RECORD_MS);
}

void vadReset()
{
  resetVadActivity();
  g_lastRms = 0;
  g_wasActionBusy = false;
}

void vadSuppressForMs(uint32_t ms)
{
  const uint32_t now = millis();
  const uint32_t until = now + ms;

  if ((int32_t)(until - g_suppressedUntilMs) > 0)
  {
    g_suppressedUntilMs = until;
  }

  // Suppression should also clear any partially accumulated start trigger.
  resetStartDetector();

  // If we are not actively recording, also clear speech activity.
  if (!getRecordingState())
  {
    g_speechActive = false;
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
  const uint32_t now = millis();

  // VAD should only operate in GPT VAD mode. Switching modes should fully reset
  // partial trigger state so manual/PTT modes remain unchanged.
  if (!vadIsListening())
  {
    if (g_speechActive || g_aboveStartSinceMs != 0)
    {
      vadReset();
    }
    return;
  }

  // Do not start if the network is not ready.
  if (!client.available())
  {
    resetStartDetector();
    return;
  }

  // Do not let motor/action noise trigger hands-free recording.
  // If an action starts while VAD is already recording, finalize that recording
  // quickly instead of continuing to capture motor noise.
  const bool robotBusy = actionIsBusy();

  if (robotBusy)
  {
    if (!g_wasActionBusy)
    {
      Serial.println("[VAD] suppress: robot action busy");
    }

    g_wasActionBusy = true;
    vadSuppressForMs(VAD_ACTION_BUSY_SUPPRESS_MS);

    if (g_speechActive || getRecordingState())
    {
      Serial.println("[VAD] action_started_while_recording -> endPttRecording");
      g_speechActive = false;
      g_lastStopMs = now;
      endPttRecording("vad_action_abort");
    }

    return;
  }

  // When an action just became idle, add an extra settle window. This catches
  // balancing noise, wheel braking, gear noise, and the robot's own movement.
  if (g_wasActionBusy)
  {
    g_wasActionBusy = false;
    Serial.println("[VAD] suppress: action settledown");
    vadSuppressForMs(VAD_ACTION_DONE_SUPPRESS_MS);
    g_lastStopMs = now;
    return;
  }

  // Avoid triggering on the assistant's own speaker audio or immediately after
  // any code path calls vadSuppressForMs().
  if (vadSuppressed(now))
  {
    resetStartDetector();

    if (!getRecordingState())
    {
      g_speechActive = false;
    }

    return;
  }

  const uint32_t rms = computeRms(samples, frames);
  g_lastRms = rms;

  if ((now - g_lastLogMs) >= VAD_LOG_MS && (g_speechActive || rms > VAD_START_RMS))
  {
    Serial.printf("[VAD] rms=%lu active=%d recording=%d action=%s suppressed=%d\n",
                  (unsigned long)rms,
                  g_speechActive ? 1 : 0,
                  getRecordingState() ? 1 : 0,
                  getRobotActionState(),
                  vadSuppressed(now) ? 1 : 0);
    g_lastLogMs = now;
  }

  if (!g_speechActive)
  {
    if ((now - g_lastStopMs) < VAD_RETRIGGER_GUARD_MS)
    {
      resetStartDetector();
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
      resetStartDetector();
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
    resetStartDetector();
    g_lastStopMs = now;

    Serial.println(maxRecordMet ? "[VAD] max_record -> endPttRecording"
                                : "[VAD] silence -> endPttRecording");
    endPttRecording("vad");
  }
}
