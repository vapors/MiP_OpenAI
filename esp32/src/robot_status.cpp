#include "robot_status.h"

#include "control_modes.h"
#include "lib_websocket.h"
#include "mic.h"

static char g_actionState[24] = "idle";
static bool g_irBlocked = false;
static int g_batteryMv = -1;
static int g_batteryPercent = -1;
static uint32_t g_lastPeriodicStatusMs = 0;
static const uint32_t ROBOT_STATUS_PERIOD_MS = 15000; // Publish a periodic status every 15 seconds.
static volatile bool g_pendingStatusPublish = false;
static char g_pendingStatusEvent[24] = "status";

static uint8_t g_mipPositionCode = 0xFF;
static String g_mipPositionName = "unknown";

static uint8_t g_mipBatteryRaw = 0;

static uint8_t g_radarCode = 0x00;
static String g_radarName = "unknown";
//static bool g_irBlocked = false;

static uint8_t g_gestureCode = 0x00;
static String g_gestureName = "none";

static uint8_t g_radarGestureMode = 0x00;

static uint8_t g_detectionId = 0x00;
static uint8_t g_detectionPower = 0x00;

static int g_detectedMipId = -1;
static bool g_shakeDetected = false;



void robotStatusSetMipPosition(uint8_t positionCode, const char* positionName)
{
  g_mipPositionCode = positionCode;
  g_mipPositionName = positionName ? positionName : "unknown";
}

void robotStatusSetMipBatteryRaw(uint8_t rawBattery)
{
  g_mipBatteryRaw = rawBattery;
}

void robotStatusSetRadar(uint8_t radarCode, const char* radarName, bool blocked)
{
  g_radarCode = radarCode;
  g_radarName = radarName ? radarName : "unknown";
  g_irBlocked = blocked;
}

void robotStatusSetGesture(uint8_t gestureCode, const char* gestureName)
{
  g_gestureCode = gestureCode;
  g_gestureName = gestureName ? gestureName : "unknown";
}

void robotStatusSetRadarGestureMode(uint8_t modeCode)
{
  g_radarGestureMode = modeCode;
}

void robotStatusSetDetectionStatus(uint8_t id, uint8_t power)
{
  g_detectionId = id;
  g_detectionPower = power;
}

void robotStatusSetMipDetected(uint8_t id)
{
  g_detectedMipId = id;
}

void robotStatusSetShakeDetected(bool detected)
{
  g_shakeDetected = detected;
}

void setupRobotStatus()
{
  strncpy(g_actionState, "idle", sizeof(g_actionState) - 1);
  g_actionState[sizeof(g_actionState) - 1] = '\0';
}

void setRobotActionState(const char* action)
{
  if (!action || action[0] == '\0') action = "idle";
  strncpy(g_actionState, action, sizeof(g_actionState) - 1);
  g_actionState[sizeof(g_actionState) - 1] = '\0';
}

void requestRobotStatePublish(const char* eventName)
{
  if (!eventName || eventName[0] == '\0') eventName = "status";
  strncpy(g_pendingStatusEvent, eventName, sizeof(g_pendingStatusEvent) - 1);
  g_pendingStatusEvent[sizeof(g_pendingStatusEvent) - 1] = '\0';
  g_pendingStatusPublish = true;
}

const char* getRobotActionState()
{
  return g_actionState;
}

void setRobotIrBlocked(bool blocked)
{
  if (g_irBlocked != blocked)
  {
    g_irBlocked = blocked;
    requestRobotStatePublish("ir_changed");
  }
}

bool getRobotIrBlocked()
{
  return g_irBlocked;
}

void setRobotBatteryStatus(int millivolts, int percent)
{
  g_batteryMv = millivolts;
  g_batteryPercent = percent;
}

int getRobotBatteryMillivolts()
{
  return g_batteryMv;
}

int getRobotBatteryPercent()
{
  return g_batteryPercent;
}

void publishRobotState(const char* eventName, bool force)
{
  if (!client.available()) return;

  // Avoid extra text frames during active mic upload except for forced state changes.
  if (getRecordingState() && !force) return;
/*
  String json = "{";
  json += "\"type\":\"robot_state\",";
  json += "\"event\":\"";
  json += (eventName ? eventName : "status");
  json += "\",";
  json += "\"mode\":\"";
  json += controlModeToString(currentMode);
  json += "\",";
  json += "\"recording\":";
  json += getRecordingState() ? "true" : "false";
  json += ",\"ws\":";
  json += client.available() ? "true" : "false";
  json += ",\"action\":\"";
  json += g_actionState;
  json += "\",";
  json += "\"ir_blocked\":";
  json += g_irBlocked ? "true" : "false";
  json += ",\"battery_mv\":";
  json += String(g_batteryMv);
  json += ",\"battery_percent\":";
  json += String(g_batteryPercent);
  json += "}";
*/

  String json = "{";
  json += "\"type\":\"robot_state\",";
  json += "\"event\":\"";
  json += (eventName ? eventName : "status");
  json += "\",";
  json += "\"mode\":\"";
  json += controlModeToString(currentMode);
  json += "\",";
  json += "\"recording\":";
  json += getRecordingState() ? "true" : "false";
  json += ",\"ws\":";
  json += client.available() ? "true" : "false";
  json += ",\"action\":\"";
  json += g_actionState;
  json += "\",";
  json += "\"ir_blocked\":";
  json += g_irBlocked ? "true" : "false";
  json += ",\"radar_code\":";
  json += String(g_radarCode);
  json += ",\"radar\":\"";
  json += g_radarName;
  json += "\"";
  json += ",\"mip_position_code\":";
  json += String(g_mipPositionCode);
  json += ",\"mip_position\":\"";
  json += g_mipPositionName;
  json += "\"";
  json += ",\"gesture_code\":";
  json += String(g_gestureCode);
  json += ",\"gesture\":\"";
  json += g_gestureName;
  json += "\"";
  json += ",\"radar_gesture_mode\":";
  json += String(g_radarGestureMode);
  json += ",\"detected_mip_id\":";
  json += String(g_detectedMipId);
  json += ",\"shake_detected\":";
  json += g_shakeDetected ? "true" : "false";
  json += "}";






  Serial.print("[ROBOT STATUS] ");
  Serial.println(json);
  sendMessage(json.c_str());
}

void loopRobotStatus()
{

  if (g_pendingStatusPublish)
  {
    g_pendingStatusPublish = false;
    publishRobotState(g_pendingStatusEvent, true);
  }

  const uint32_t now = millis();
  if (now - g_lastPeriodicStatusMs < ROBOT_STATUS_PERIOD_MS) return;
  g_lastPeriodicStatusMs = now;
  publishRobotState("periodic", false);
}
