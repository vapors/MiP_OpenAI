#include "ble_control.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include <string>

#include "config.h"
#include "mic.h"
#include "lib_websocket.h"
#include "MiP_commands.h"
#include "robot_status.h"
#include "vad_controller.h"

// These live in main.cpp.
extern MiP MyMiP;

// -----------------------------------------------------------------------------
// Global mode state
// -----------------------------------------------------------------------------
ControlMode currentMode = MODE_MANUAL;

static NimBLECharacteristic* g_statusChar = nullptr;
static bool g_bleStarted = false;
static bool g_bleRecording = false;
static bool g_pendingPttStart = false;
static bool g_pendingPttStop = false;
static bool g_pttStopping = false;
static unsigned long g_pttStartedAtMs = 0;
static unsigned long g_pttStopRequestedAtMs = 0;

// -----------------------------------------------------------------------------
// BLE command queue
// -----------------------------------------------------------------------------
// NimBLE write callbacks run on the nimble_host task, which has a limited stack.
// Keep that callback tiny: copy the command into this queue and return.
// The command task below performs parsing, MiP actions, WebSocket status updates,
// and any String/JSON work on its own stack.
static const uint8_t BLE_CMD_QUEUE_LEN = 8;
static const size_t BLE_CMD_MAX_LEN = 96;

struct BleQueuedCommand
{
  char text[BLE_CMD_MAX_LEN];
};

static QueueHandle_t g_bleCommandQueue = nullptr;
static TaskHandle_t g_bleCommandTaskHandle = nullptr;
static uint32_t g_bleCommandsDropped = 0;

static bool enqueueBleCommand(const std::string& value)
{
  if (!g_bleCommandQueue)
  {
    return false;
  }

  BleQueuedCommand cmd;
  memset(&cmd, 0, sizeof(cmd));

  size_t n = value.length();
  if (n >= BLE_CMD_MAX_LEN)
  {
    n = BLE_CMD_MAX_LEN - 1;
  }

  memcpy(cmd.text, value.data(), n);
  cmd.text[n] = '\0';

  return xQueueSend(g_bleCommandQueue, &cmd, 0) == pdTRUE;
}

// 24 kHz, 16-bit mono = 48,000 bytes/sec, so 100 ms = 4,800 bytes.
// Keep the PTT gate comfortably above the OpenAI Realtime commit minimum.
static const unsigned long MIN_PTT_MS = 750;

// Short messages below this length are held until the minimum duration is met.
// 750 ms gives the server enough audio for OpenAI without forcing long phrases.

// Conservative MiP command defaults.
// The WowWee protocol documents drive-forward speed 0-30 and turn speed 0-24.
// We keep defaults lower while testing the robot from a phone/web app.
static const int DEFAULT_DRIVE_SPEED = 12;
static const int DEFAULT_DRIVE_TIME  = 15;
static const int DEFAULT_TURN_SPEED  = 12;
static const int DEFAULT_TURN_ANGLE  = 45;

const char* controlModeToString(ControlMode mode)
{
  switch (mode)
  {
    case MODE_MANUAL:         return "manual";
    case MODE_GPT_ASSISTED:   return "gpt_assisted";
    case MODE_GPT_VAD:        return "gpt_vad";
    case MODE_GPT_AUTONOMOUS: return "gpt_autonomous";
    default:                  return "unknown";
  }
}

ControlMode controlModeFromString(const String& modeName)
{
  String m = modeName;
  m.trim();
  m.toUpperCase();

  if (m == "MANUAL" || m == "MODE:MANUAL") return MODE_MANUAL;
  if (m == "GPT_ASSISTED" || m == "ASSISTED" || m == "MODE:GPT_ASSISTED") return MODE_GPT_ASSISTED;
  if (m == "GPT_VAD" || m == "VAD" || m == "VOICE" || m == "MODE:GPT_VAD" || m == "MODE:VAD") return MODE_GPT_VAD;
  if (m == "GPT_AUTONOMOUS" || m == "AUTONOMOUS" || m == "MODE:GPT_AUTONOMOUS") return MODE_GPT_AUTONOMOUS;

  return currentMode;
}

bool isManualMode()        { return currentMode == MODE_MANUAL; }
bool isGptAssistedMode()   { return currentMode == MODE_GPT_ASSISTED; }
bool isGptVadMode()        { return currentMode == MODE_GPT_VAD; }
bool isGptAutonomousMode() { return currentMode == MODE_GPT_AUTONOMOUS; }

bool allowsGptMotion()
{
  return currentMode == MODE_GPT_ASSISTED || currentMode == MODE_GPT_VAD || currentMode == MODE_GPT_AUTONOMOUS;
}

bool allowsManualMotion()
{
  return currentMode == MODE_MANUAL;
}

void applyModeLED()
{
  switch (currentMode)
  {
    case MODE_MANUAL:
      MyMiP.setChestLED(0, 40, 255);       // blue
      break;

    case MODE_GPT_ASSISTED:
      MyMiP.setChestLED(140, 0, 255);      // purple
      break;

    case MODE_GPT_VAD:
      MyMiP.setChestLED(255, 180, 0);      // amber = hands-free VAD
      break;

    case MODE_GPT_AUTONOMOUS:
      MyMiP.setChestLED(0, 255, 80);       // green
      break;
  }
}

void setControlMode(ControlMode mode, bool stopRobot)
{
  if (currentMode == mode)
  {
    applyModeLED();
    publishBleStatus("mode");
    return;
  }

  if (stopRobot)
  {
    MyMiP.stop();
  }

  currentMode = mode;
  applyModeLED();

  Serial.print("[MODE] ");
  Serial.println(controlModeToString(currentMode));

  publishBleStatus("mode");
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static int valueAfterPrefix(const String& cmd, const char* prefix, int fallback)
{
  String p(prefix);
  if (!cmd.startsWith(p)) return fallback;
  return cmd.substring(p.length()).toInt();
}

static bool parseCsv3(const String& payload, int& a, int& b, int& c)
{
  int first = payload.indexOf(',');
  int second = payload.indexOf(',', first + 1);

  if (first < 0 || second < 0) return false;

  a = payload.substring(0, first).toInt();
  b = payload.substring(first + 1, second).toInt();
  c = payload.substring(second + 1).toInt();
  return true;
}

void publishBleStatus(const char* eventName)
{
  String json = "{";
  json += "\"event\":\"";
  json += eventName;
  json += "\",";
  json += "\"mode\":\"";
  json += controlModeToString(currentMode);
  json += "\",";
  json += "\"recording\":";
  json += g_bleRecording ? "true" : "false";
  json += ",\"ws\":";
  json += isWebSocketClientConnected() ? "true" : "false";
  json += "}";

  Serial.print("[BLE STATUS] ");
  Serial.println(json);

  if (g_statusChar)
  {
    g_statusChar->setValue(json.c_str());
    g_statusChar->notify();
  }

  publishRobotState(eventName, true);
}

void beginPttRecording(const char* source)
{
  if (isManualMode())
  {
    Serial.println("[PTT] Ignored in manual mode");
    publishBleStatus("ptt_ignored_manual_mode");
    return;
  }

  if (g_bleRecording || g_pendingPttStart || g_pttStopping)
  {
    Serial.println("[PTT] Start ignored; already recording/pending");
    publishBleStatus("ptt_start_ignored");
    return;
  }

  Serial.print("[PTT] START requested from ");
  Serial.println(source);

  g_pendingPttStart = true;
}

void endPttRecording(const char* source)
{
  if (!g_bleRecording && !g_pendingPttStart)
  {
    Serial.println("[PTT] Stop ignored; not recording");
    publishBleStatus("ptt_stop_ignored");
    return;
  }

  if (g_pendingPttStop || g_pttStopping)
  {
    Serial.println("[PTT] Stop ignored; stop already pending");
    return;
  }

  Serial.print("[PTT] STOP requested from ");
  Serial.println(source);

  g_pendingPttStop = true;
}

void forcePttAbortFromWebSocketDisconnect()
{
  const bool wasActive = g_bleRecording || g_pendingPttStart || g_pendingPttStop || g_pttStopping || getRecordingState();

  g_pendingPttStart = false;
  g_pendingPttStop = false;
  g_pttStopping = false;
  g_bleRecording = false;
  g_pttStartedAtMs = 0;
  g_pttStopRequestedAtMs = 0;

  setRecording(false);

  // If VAD had started the session, clear its internal speech-active state so it
  // does not later emit a stale endPttRecording() after reconnect.
  vadReset();
  vadSuppressForMs(1200);

  if (wasActive)
  {
    Serial.println("[PTT] aborted because WebSocket disconnected");
    publishBleStatus("ptt_aborted_ws_disconnect");
  }
}

// Runs from loopBleControl(), not from the NimBLE callback.
// This keeps WebSocket sends and timing-sensitive audio work out of the BLE
// callback thread.
// Runs from loopBleControl(), not from the NimBLE callback.
// This keeps WebSocket sends and timing-sensitive audio work out of the BLE
// callback thread.
static void processPttState()
{
  if (g_pendingPttStart)
  {
    g_pendingPttStart = false;

    if (!isWebSocketClientConnected())
    {
      Serial.println("[PTT] Start ignored; websocket disconnected");
      publishBleStatus("ptt_start_no_ws");
      return;
    }

    // Tell the server to open a new recording before mic chunks begin.
    sendMessage("START_RECORD");
    delay(40);

    i2s_zero_dma_buffer(I2S_PORT_SPEAKER);
    i2s_zero_dma_buffer(I2S_PORT_MIC);
    i2s_start(I2S_PORT_MIC);

    g_pttStartedAtMs = millis();
    g_bleRecording = true;
    g_pttStopping = false;

    setRecording(true);

    Serial.println("[PTT] START active");
    publishBleStatus("ptt_start");
  }

  if (g_pendingPttStop && g_bleRecording)
  {
    unsigned long elapsed = millis() - g_pttStartedAtMs;

    if (elapsed < MIN_PTT_MS)
    {
      return; // wait until minimum duration is met
    }

    g_pendingPttStop = false;
    g_pttStopping = true;
    g_pttStopRequestedAtMs = millis();

    // Stop producing/sending mic chunks first.
    // micTask sends audio directly, so a short delay gives any in-flight
    // sendBinaryData() call time to finish before STOP_RECORD commits.
    setRecording(false);
    g_bleRecording = false;

    Serial.println("[PTT] STOP finalizing direct mic stream");
  }

  if (g_pttStopping)
  {
    if ((millis() - g_pttStopRequestedAtMs) >= 100)
    {
      sendMessage("STOP_RECORD");

      i2s_zero_dma_buffer(I2S_PORT_MIC);
      i2s_start(I2S_PORT_SPEAKER);

      g_pttStopping = false;

      Serial.println("[PTT] STOP_RECORD sent");
      publishBleStatus("ptt_stop");
    }
  }
}


// -----------------------------------------------------------------------------
// Command handler
// -----------------------------------------------------------------------------
void handleBleCommand(String cmd)
{
  cmd.trim();
  if (cmd.length() == 0) return;

  Serial.print("[BLE CMD RAW] ");
  Serial.println(cmd);

  String upper = cmd;
  upper.toUpperCase();

  if (upper == "STATUS")
  {
    publishBleStatus("status_request");
    return;
  }

  if (upper == "MODE:MANUAL")
  {
    setControlMode(MODE_MANUAL);
    return;
  }

  if (upper == "MODE:GPT_ASSISTED")
  {
    setControlMode(MODE_GPT_ASSISTED);
    return;
  }

  if (upper == "MODE:GPT_VAD" || upper == "MODE:VAD")
  {
    setControlMode(MODE_GPT_VAD);
    return;
  }

  if (upper == "MODE:GPT_AUTONOMOUS")
  {
    setControlMode(MODE_GPT_AUTONOMOUS);
    return;
  }

  if (upper == "MODE:NEXT")
  {
    ControlMode next = MODE_MANUAL;
    if (currentMode == MODE_MANUAL) next = MODE_GPT_ASSISTED;
    else if (currentMode == MODE_GPT_ASSISTED) next = MODE_GPT_VAD;
    else if (currentMode == MODE_GPT_VAD) next = MODE_GPT_AUTONOMOUS;
    else next = MODE_MANUAL;

    setControlMode(next);
    return;
  }

  if (upper == "PTT:START")
  {
    beginPttRecording("ble");
    return;
  }

  if (upper == "PTT:STOP")
  {
    endPttRecording("ble");
    return;
  }

  // Stop is always allowed.
  if (upper == "MIP:STOP" || upper == "STOP" || upper == "ESTOP")
  {
    MyMiP.stop();
    MyMiP.setChestLED(255, 0, 0);
    delay(120);
    applyModeLED();
    publishBleStatus("stop");
    return;
  }

  // Pass 1: local motion commands are only active in manual mode.
  // This prevents an accidental phone button press from fighting GPT later.
  if (!allowsManualMotion())
  {
    Serial.println("[BLE] Manual MiP command ignored outside manual mode");
    publishBleStatus("manual_command_ignored");
    return;
  }

  if (upper == "MIP:FORWARD")
  {
    MyMiP.driveForward(DEFAULT_DRIVE_SPEED, DEFAULT_DRIVE_TIME);
    publishBleStatus("forward");
  }
  else if (upper == "MIP:BACKWARD")
  {
    MyMiP.distanceDrive(-10, 0);
    publishBleStatus("backward");
  }
  else if (upper == "MIP:LEFT")
  {
    MyMiP.turnAngle(0, DEFAULT_TURN_SPEED, DEFAULT_TURN_ANGLE);
    publishBleStatus("left");
  }
  else if (upper == "MIP:RIGHT")
  {
    MyMiP.turnAngle(1, DEFAULT_TURN_SPEED, DEFAULT_TURN_ANGLE);
    publishBleStatus("right");
  }
  else if (upper.startsWith("MIP:DRIVE:"))
  {
    String payload = cmd.substring(String("MIP:DRIVE:").length());
    int comma = payload.indexOf(',');
    if (comma > 0)
    {
      int distance = payload.substring(0, comma).toInt();
      int angle = payload.substring(comma + 1).toInt();
      distance = constrain(distance, -100, 100);
      angle = constrain(angle, -360, 360);
      MyMiP.distanceDrive(distance, angle);
      publishBleStatus("drive_distance");
    }
  }
  else if (upper == "MIP:STAND" || upper == "MIP:STAND_ANY")
  {
    MyMiP.standUp(2);
    publishBleStatus("stand");
  }
  else if (upper == "MIP:STAND_FRONT")
  {
    MyMiP.standUp(0);
    publishBleStatus("stand_front");
  }
  else if (upper == "MIP:STAND_BACK")
  {
    MyMiP.standUp(1);
    publishBleStatus("stand_back");
  }
  else if (upper.startsWith("MIP:SOUND:"))
  {
    int soundId = valueAfterPrefix(upper, "MIP:SOUND:", 1);
    soundId = constrain(soundId, 1, 106);
    MyMiP.playSpecific(soundId);
    publishBleStatus("sound");
  }
  else if (upper.startsWith("MIP:VOLUME:"))
  {
    int volume = valueAfterPrefix(upper, "MIP:VOLUME:", 4);
    volume = constrain(volume, 0, 7);
    MyMiP.setVolume(volume);
    publishBleStatus("volume");
  }
  else if (upper.startsWith("MIP:LED:"))
  {
    String payload = cmd.substring(String("MIP:LED:").length());
    int r, g, b;
    if (parseCsv3(payload, r, g, b))
    {
      MyMiP.setChestLED(
        constrain(r, 0, 255),
        constrain(g, 0, 255),
        constrain(b, 0, 255)
      );
      publishBleStatus("chest_led");
    }
  }
  else if (upper.startsWith("MIP:HEAD:"))
  {
    String payload = cmd.substring(String("MIP:HEAD:").length());
    int a = 1, b = 1, c = 1, d = 1;

    int c1 = payload.indexOf(',');
    int c2 = payload.indexOf(',', c1 + 1);
    int c3 = payload.indexOf(',', c2 + 1);
    if (c1 > 0 && c2 > 0 && c3 > 0)
    {
      a = payload.substring(0, c1).toInt();
      b = payload.substring(c1 + 1, c2).toInt();
      c = payload.substring(c2 + 1, c3).toInt();
      d = payload.substring(c3 + 1).toInt();

      MyMiP.setHeadLEDs(
        constrain(a, 0, 3),
        constrain(b, 0, 3),
        constrain(c, 0, 3),
        constrain(d, 0, 3)
      );
      publishBleStatus("head_leds");
    }
  }
  else
  {
    Serial.print("[BLE] Unknown command: ");
    Serial.println(cmd);
    publishBleStatus("unknown_command");
  }
}


// -----------------------------------------------------------------------------
// BLE command task
// -----------------------------------------------------------------------------
static void bleCommandTask(void* parameter)
{
  Serial.println("[BLE] command task started");

  BleQueuedCommand cmd;

  while (true)
  {
    if (xQueueReceive(g_bleCommandQueue, &cmd, portMAX_DELAY) == pdTRUE)
    {
      handleBleCommand(String(cmd.text));

      // Let Wi-Fi, BLE, and audio tasks breathe after command/status work.
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

// -----------------------------------------------------------------------------
// BLE service
// -----------------------------------------------------------------------------
class CommandCallbacks : public NimBLECharacteristicCallbacks
{
public:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override
  {
    std::string value = characteristic->getValue();

    if (value.empty())
    {
      return;
    }

    if (!enqueueBleCommand(value))
    {
      g_bleCommandsDropped++;

      // Keep logging minimal in the NimBLE callback. Do not build JSON, publish
      // status, or call MiP/WebSocket functions here.
      if ((g_bleCommandsDropped == 1) || ((g_bleCommandsDropped % 10) == 0))
      {
        Serial.printf("[BLE] command queue full; dropped=%lu\n",
                      (unsigned long)g_bleCommandsDropped);
      }
    }
  }
};

void setupBleControl()
{
  if (g_bleStarted) return;

  Serial.println("[BLE] Starting MiP BLE control service");

  NimBLEDevice::init(MIP_BLE_DEVICE_NAME);
  NimBLEDevice::setDeviceName(MIP_BLE_DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEServer* server = NimBLEDevice::createServer();
  NimBLEService* service = server->createService(MIP_BLE_SERVICE_UUID);

  NimBLECharacteristic* commandChar = service->createCharacteristic(
    MIP_BLE_COMMAND_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );

  commandChar->setCallbacks(new CommandCallbacks());

  g_statusChar = service->createCharacteristic(
    MIP_BLE_STATUS_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();

  // Keep the primary advertisement small so it does not exceed the legacy BLE
  // payload length. The human-readable name goes in scan response data.
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setCompleteServices(NimBLEUUID(MIP_BLE_SERVICE_UUID));
  advertising->setAdvertisementData(advData);

  NimBLEAdvertisementData scanData;
  scanData.setName(MIP_BLE_DEVICE_NAME);
  advertising->setScanResponseData(scanData);

  // Create the BLE command queue/task before advertising starts so the write
  // callback can immediately enqueue commands after a phone connects.
  if (!g_bleCommandQueue)
  {
    g_bleCommandQueue = xQueueCreate(BLE_CMD_QUEUE_LEN, sizeof(BleQueuedCommand));

    if (!g_bleCommandQueue)
    {
      Serial.println("[BLE] ERROR: failed to create command queue");
    }
  }

  if (g_bleCommandQueue && !g_bleCommandTaskHandle)
  {
    xTaskCreatePinnedToCore(
      bleCommandTask,
      "bleCommandTask",
      6144,
      nullptr,
      1,
      &g_bleCommandTaskHandle,
      1
    );
  }

  advertising->start();

  g_bleStarted = true;

  Serial.println("[BLE] Advertising as MiP-Bot");
  publishBleStatus("ble_started");
}

void loopBleControl()
{
  processPttState();
}
