#include "robot_body_state.h"

#include "robot_status.h"
#include "mip_uart_listener.h"

#ifndef ROBOT_BODY_DEBUG
#define ROBOT_BODY_DEBUG 1
#endif

// Conservative reconnect timings.
static const uint32_t BODY_PROBE_INTERVAL_READY_MS       = 5000;
static const uint32_t BODY_PROBE_INTERVAL_SEARCH_MS      = 2000;
static const uint32_t BODY_PROBE_INTERVAL_RECOVERY_MS    = 750;

static const uint32_t BODY_READY_DELAY_MS                = 250;
static const uint32_t BODY_LOST_TIMEOUT_MS               = 10000;
static const uint32_t BODY_LOST_CONFIRM_MS               = 1500;
static const uint32_t BODY_RECOVERY_FAST_PROBE_MS        = 15000;
static const uint32_t BODY_RECOVERY_REINIT_INTERVAL_MS   = 5000;
static const uint32_t BODY_INITIAL_REINIT_INTERVAL_MS = 5000;


static volatile RobotBodyType g_bodyType = ROBOT_BODY_NONE;
static volatile RobotBodyConnectionState g_bodyState = ROBOT_BODY_UNKNOWN;

static volatile uint32_t g_lastRxMs = 0;

static uint32_t g_detectedAtMs = 0;
static uint32_t g_lastProbeMs = 0;
static uint32_t g_lostCandidateAtMs = 0;
static uint32_t g_lostAtMs = 0;
static uint32_t g_lastRecoveryReinitMs = 0;

static bool g_readyInitDone = false;

const char* robotBodyTypeToString(RobotBodyType type)
{
  switch (type)
  {
    case ROBOT_BODY_MIP:  return "mip";
    case ROBOT_BODY_NONE: return "none";
    default:              return "unknown";
  }
}

const char* robotBodyStateToString(RobotBodyConnectionState state)
{
  switch (state)
  {
    case ROBOT_BODY_NOT_PRESENT: return "not_present";
    case ROBOT_BODY_DETECTED:    return "detected";
    case ROBOT_BODY_READY:       return "ready";
    case ROBOT_BODY_LOST:        return "lost";
    case ROBOT_BODY_UNKNOWN:
    default:                     return "unknown";
  }
}

void setupRobotBodyState()
{
  g_bodyType = ROBOT_BODY_NONE;
  g_bodyState = ROBOT_BODY_UNKNOWN;

  g_lastRxMs = 0;
  g_detectedAtMs = 0;
  g_lastProbeMs = 0;
  g_lostCandidateAtMs = 0;
  g_lostAtMs = 0;
  g_lastRecoveryReinitMs = 0;

  g_readyInitDone = false;
}

RobotBodyType getRobotBodyType()
{
  return (RobotBodyType)g_bodyType;
}

RobotBodyConnectionState getRobotBodyConnectionState()
{
  return (RobotBodyConnectionState)g_bodyState;
}

bool robotBodyConnected()
{
  const RobotBodyConnectionState state = (RobotBodyConnectionState)g_bodyState;
  return state == ROBOT_BODY_READY || state == ROBOT_BODY_DETECTED;
}

uint32_t robotBodyLastRxAgeMs()
{
  const uint32_t last = g_lastRxMs;
  if (last == 0) return 0xFFFFFFFF;
  return millis() - last;
}

void notifyRobotHeartbeat()
{
  notifyRobotBodyPacketSeen(ROBOT_BODY_MIP);
}

void notifyRobotBodyPacketSeen(RobotBodyType type)
{
  const uint32_t now = millis();

  const RobotBodyType oldType = (RobotBodyType)g_bodyType;
  const RobotBodyConnectionState oldState = (RobotBodyConnectionState)g_bodyState;

  // Any valid packet refreshes the heartbeat.
  g_lastRxMs = now;
  g_lostCandidateAtMs = 0;
  g_lostAtMs = 0;
  g_lastRecoveryReinitMs = 0;

  // First sighting, reattachment, or body type change.
  if (oldType != type ||
      oldState == ROBOT_BODY_UNKNOWN ||
      oldState == ROBOT_BODY_NOT_PRESENT ||
      oldState == ROBOT_BODY_LOST)
  {
    g_bodyType = type;
    g_bodyState = ROBOT_BODY_DETECTED;
    g_detectedAtMs = now;
    g_readyInitDone = false;

#if ROBOT_BODY_DEBUG
    Serial.printf(
      "[BODY] heartbeat type=%s oldState=%s -> detected now=%lu\n",
      robotBodyTypeToString(type),
      robotBodyStateToString(oldState),
      (unsigned long)now
    );
#endif

    requestRobotStatePublish("body_detected");
  }
}

void loopRobotBodyState()
{
  const uint32_t now = millis();

  const RobotBodyConnectionState state = (RobotBodyConnectionState)g_bodyState;
  const bool hasSeenBody = (g_lastRxMs != 0);


  const uint32_t rxAge = hasSeenBody ? (now - g_lastRxMs) : 0xFFFFFFFF;

  const bool connected =
    state == ROBOT_BODY_READY ||
    state == ROBOT_BODY_DETECTED;

const bool searchingWithoutBody =
  !hasSeenBody ||
  state == ROBOT_BODY_UNKNOWN ||
  state == ROBOT_BODY_NOT_PRESENT;

  uint32_t probeInterval = BODY_PROBE_INTERVAL_SEARCH_MS;

  if (connected)
  {
    probeInterval = BODY_PROBE_INTERVAL_READY_MS;
  }
  else if (state == ROBOT_BODY_LOST)
  {
    if ((now - g_lastRecoveryReinitMs) >= BODY_RECOVERY_REINIT_INTERVAL_MS)
    {
      g_lastRecoveryReinitMs = now;
      recoverMipUart();
    }

    const bool fastRecovery =
      g_lostAtMs != 0 &&
      (now - g_lostAtMs) < BODY_RECOVERY_FAST_PROBE_MS;

    probeInterval = fastRecovery
      ? BODY_PROBE_INTERVAL_RECOVERY_MS
      : BODY_PROBE_INTERVAL_SEARCH_MS;
  }
else if (searchingWithoutBody)
{
  if ((now - g_lastRecoveryReinitMs) >= BODY_INITIAL_REINIT_INTERVAL_MS)
  {
    g_lastRecoveryReinitMs = now;
    recoverMipUart();
  }

  probeInterval = BODY_PROBE_INTERVAL_SEARCH_MS;
}

  if ((now - g_lastProbeMs) >= probeInterval)
  {
    g_lastProbeMs = now;

#if ROBOT_BODY_DEBUG
    Serial.printf(
      "[BODY] probe status state=%s rxAge=%lu interval=%lu\n",
      robotBodyStateToString(state),
      hasSeenBody ? (unsigned long)rxAge : 0xFFFFFFFFUL,
      (unsigned long)probeInterval
    );
#endif

    requestMipPositionStatus();
  }

  // No valid body packet has ever been seen.
  if (!hasSeenBody)
  {
    return;
  }

  // Fresh heartbeat: maintain detected/ready state.
  if (rxAge <= BODY_LOST_TIMEOUT_MS)
  {
    g_lostCandidateAtMs = 0;

    if (state == ROBOT_BODY_DETECTED)
    {
      if ((now - g_detectedAtMs) >= BODY_READY_DELAY_MS)
      {
        g_bodyState = ROBOT_BODY_READY;
        requestRobotStatePublish("body_connected");

        if (!g_readyInitDone && g_bodyType == ROBOT_BODY_MIP)
        {
          g_readyInitDone = true;

          // Re-apply lightweight MiP setup after stable hot-plug/power-up.
          enableMipRadarMode();
          requestMipPositionStatus();
        }
      }
    }

    return;
  }

  // From here down, heartbeat is stale.
  if (state != ROBOT_BODY_READY && state != ROBOT_BODY_DETECTED)
  {
    return;
  }

  // First stale observation: start confirmation timer.
  if (g_lostCandidateAtMs == 0)
  {
    g_lostCandidateAtMs = now;

#if ROBOT_BODY_DEBUG
    Serial.printf(
      "[BODY] lost candidate state=%s rxAge=%lu\n",
      robotBodyStateToString(state),
      (unsigned long)rxAge
    );
#endif

    return;
  }

  // Require stale RX to remain stale for a confirmation window.
  if ((now - g_lostCandidateAtMs) < BODY_LOST_CONFIRM_MS)
  {
    return;
  }

  // Final re-check before declaring lost.
  const uint32_t finalLastRx = g_lastRxMs;
  if (finalLastRx != 0 && (now - finalLastRx) <= BODY_LOST_TIMEOUT_MS)
  {
    g_lostCandidateAtMs = 0;
    return;
  }

  g_bodyState = ROBOT_BODY_LOST;
  g_readyInitDone = false;
  g_lostCandidateAtMs = 0;
  g_lostAtMs = now;

  recoverMipUart();

#if ROBOT_BODY_DEBUG
  Serial.printf(
    "[BODY] LOST confirmed oldState=%s now=%lu lastRx=%lu rxAge=%lu\n",
    robotBodyStateToString(state),
    (unsigned long)now,
    (unsigned long)g_lastRxMs,
    (unsigned long)rxAge
  );
#endif

  requestRobotStatePublish("body_lost");
}