#include "mip_action_queue.h"

#include "MiP_commands.h"
#include "robot_status.h"

extern MiP MyMiP;

enum MipActionType : uint8_t
{
  MIP_ACTION_NONE = 0,
  MIP_ACTION_STOP,
  MIP_ACTION_STAND_UP,
  MIP_ACTION_SET_POSITION,
  MIP_ACTION_MOVE_FORWARD,
  MIP_ACTION_MOVE_BACKWARD_TIMED,
  MIP_ACTION_MOVE_BACKWARD,
  MIP_ACTION_DRIVE_DISTANCE,
  MIP_ACTION_TURN_LEFT,
  MIP_ACTION_TURN_RIGHT,
  MIP_ACTION_SPIN,
  MIP_ACTION_CONTINUOUS_DRIVE,
  MIP_ACTION_CRAZY_DRIVE,
  MIP_ACTION_GAME_MODE,
  MIP_ACTION_CHEST_LED,
  MIP_ACTION_FLASH_CHEST_LED,
  MIP_ACTION_HEAD_LEDS,
  MIP_ACTION_EXPRESSION_PRESET,
  MIP_ACTION_SOUND,
  MIP_ACTION_SOUND_SEQUENCE,
  MIP_ACTION_VOLUME,
  MIP_ACTION_REQUEST_STATUS,
  MIP_ACTION_REQUEST_WEIGHT,
  MIP_ACTION_READ_ODOMETER,
  MIP_ACTION_RESET_ODOMETER,
  MIP_ACTION_GESTURE_RADAR,
  MIP_ACTION_DETECTION_MODE,
  MIP_ACTION_IR_CONTROL,
  MIP_ACTION_SEND_IR_CODE,
  MIP_ACTION_CLAP_DETECTION,
  MIP_ACTION_CLAP_DELAY
};

struct MipAction
{
  MipActionType type = MIP_ACTION_NONE;
  int a = 0;
  int b = 0;
  int c = 0;
  int d = 0;
  int e = 0;
  bool flag = false;
  uint32_t code = 0;
  char text[80] = {0};
};

static QueueHandle_t g_mipActionQueue = nullptr;
static TaskHandle_t g_mipActionTaskHandle = nullptr;
static const uint8_t MIP_ACTION_QUEUE_LEN = 10;

static int timeUnitsFromMs(int durationMs)
{
  durationMs = constrain(durationMs, 35, 1785); // 255 * 7ms
  int units = durationMs / 7;
  return constrain(units, 1, 255);
}

static uint8_t ticks20msFromMs(int ms)
{
  ms = constrain(ms, 20, 5100);
  return (uint8_t)constrain(ms / 20, 1, 255);
}

static uint8_t gameModeFromName(const char* mode)
{
  if (!mode) return 0x05;
  if (strcmp(mode, "app") == 0) return 0x01;
  if (strcmp(mode, "cage") == 0) return 0x02;
  if (strcmp(mode, "tracking") == 0) return 0x03;
  if (strcmp(mode, "dance") == 0) return 0x04;
  if (strcmp(mode, "default") == 0) return 0x05;
  if (strcmp(mode, "stack") == 0) return 0x06;
  if (strcmp(mode, "tricks") == 0) return 0x07;
  if (strcmp(mode, "roam") == 0) return 0x08;
  return 0x05;
}

static uint8_t gestureRadarModeFromName(const char* mode)
{
  if (!mode) return 0x00;
  if (strcmp(mode, "gesture") == 0) return 0x02;
  if (strcmp(mode, "radar") == 0) return 0x04;
  return 0x00;
}

static uint8_t continuousDriveCode(const char* direction, int speed, bool crazy)
{
  if (!direction) direction = "forward";
  speed = constrain(speed, 1, crazy ? 31 : 32);

  if (!crazy)
  {
    if (strcmp(direction, "backward") == 0) return 0x20 + speed;
    if (strcmp(direction, "right") == 0) return 0x40 + speed;
    if (strcmp(direction, "left") == 0) return 0x60 + speed;
    return speed;
  }

  if (strcmp(direction, "backward") == 0) return 0xA0 + speed;
  if (strcmp(direction, "right") == 0) return 0xC0 + speed;
  if (strcmp(direction, "left") == 0) return 0xE0 + speed;
  return 0x80 + speed;
}

static void copyText(char* dst, size_t dstSize, const char* src, const char* fallback = "")
{
  if (!src) src = fallback;
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}

static const char* actionStateName(MipActionType type)
{
  switch (type)
  {
    case MIP_ACTION_STOP: return "stopping";
    case MIP_ACTION_STAND_UP: return "standing";
    case MIP_ACTION_MOVE_FORWARD:
    case MIP_ACTION_MOVE_BACKWARD_TIMED:
    case MIP_ACTION_MOVE_BACKWARD:
    case MIP_ACTION_DRIVE_DISTANCE:
    case MIP_ACTION_CONTINUOUS_DRIVE:
    case MIP_ACTION_CRAZY_DRIVE: return "moving";
    case MIP_ACTION_TURN_LEFT:
    case MIP_ACTION_TURN_RIGHT:
    case MIP_ACTION_SPIN: return "spinning";
    case MIP_ACTION_GAME_MODE: return "mode_change";
    case MIP_ACTION_CHEST_LED:
    case MIP_ACTION_FLASH_CHEST_LED:
    case MIP_ACTION_HEAD_LEDS: return "lights";
    case MIP_ACTION_EXPRESSION_PRESET: return "expression";
    case MIP_ACTION_SOUND:
    case MIP_ACTION_SOUND_SEQUENCE: return "sound";
    case MIP_ACTION_VOLUME: return "volume";
    default: return "command";
  }
}

static void runContinuousDrive(const char* direction, int speed, int durationMs, bool crazy)
{
  uint8_t code = continuousDriveCode(direction, speed, crazy);
  durationMs = constrain(durationMs, 50, 5000);

  uint32_t start = millis();
  while ((millis() - start) < (uint32_t)durationMs)
  {
    MyMiP.continuousDrive(code);
    delay(45);
  }
  MyMiP.stop();
}

static void runSpin(const char* direction, int angle, int speed)
{
  int dir = (direction && strcmp(direction, "right") == 0) ? 1 : 0;
  angle = constrain(angle, 5, 2160);
  speed = constrain(speed, 1, 24);

  int remaining = angle;
  while (remaining > 0)
  {
    int step = min(remaining, 1275);
    MyMiP.turnAngle(dir, speed, step);
    delay(max(250, step * 2));
    remaining -= step;
  }
}

static void playSoundSequenceFromCsv(const char* csv, int delayMs, int repeat)
{
  uint8_t ids[8];
  uint8_t count = 0;
  String s = csv ? String(csv) : String("1,3,2");
  int start = 0;

  while (count < 8 && start < s.length())
  {
    int comma = s.indexOf(',', start);
    String part = (comma < 0) ? s.substring(start) : s.substring(start, comma);
    part.trim();
    int id = constrain(part.toInt(), 1, 106);
    ids[count++] = (uint8_t)id;
    if (comma < 0) break;
    start = comma + 1;
  }

  uint8_t delayTicks = (uint8_t)constrain(delayMs / 30, 0, 255);
  MyMiP.playSoundSequence(ids, count, delayTicks, (uint8_t)constrain(repeat, 0, 255));
}

static void runExpressionPreset(const char* expression)
{
  if (!expression) expression = "excited";

  if (strcmp(expression, "scared") == 0)
  {
    MyMiP.flashChestLED(255, 40, 0, 4, 4);
    MyMiP.setHeadLEDs(3, 3, 3, 3);
    MyMiP.playSpecific(4);
    MyMiP.driveBackward(14, timeUnitsFromMs(450));
  }
  else if (strcmp(expression, "party") == 0 || strcmp(expression, "celebrate") == 0)
  {
    MyMiP.flashChestLED(0, 80, 255, 5, 5);
    MyMiP.setHeadLEDs(3, 3, 3, 3);
    MyMiP.playSpecific(1);
    runSpin("left", 360, 14);
  }
  else if (strcmp(expression, "sleepy") == 0)
  {
    MyMiP.setChestLED(20, 0, 60);
    MyMiP.setHeadLEDs(2, 0, 0, 2);
    MyMiP.playSpecific(2);
  }
  else if (strcmp(expression, "curious") == 0)
  {
    MyMiP.setChestLED(0, 255, 180);
    MyMiP.setHeadLEDs(1, 2, 2, 1);
    MyMiP.turnAngle(0, 8, 25);
    delay(250);
    MyMiP.turnAngle(1, 8, 50);
  }
  else if (strcmp(expression, "angry") == 0)
  {
    MyMiP.flashChestLED(255, 0, 0, 3, 3);
    MyMiP.setHeadLEDs(3, 0, 0, 3);
    MyMiP.playSpecific(3);
  }
  else
  {
    MyMiP.flashChestLED(0, 255, 80, 4, 4);
    MyMiP.setHeadLEDs(1, 1, 1, 1);
    MyMiP.playSpecific(1);
    runSpin("right", 180, 12);
  }
}

static void runMipAction(const MipAction& action)
{
  switch (action.type)
  {
    case MIP_ACTION_STOP:
      MyMiP.stop();
      break;
    case MIP_ACTION_STAND_UP:
      MyMiP.standUp(action.a);
      break;
    case MIP_ACTION_SET_POSITION:
      MyMiP.setPosition(action.a == 1 ? FACEDOWN : FACEUP);
      break;
    case MIP_ACTION_MOVE_FORWARD:
      MyMiP.driveForward(action.a, timeUnitsFromMs(action.b));
      break;
    case MIP_ACTION_MOVE_BACKWARD_TIMED:
      MyMiP.driveBackward(action.a, timeUnitsFromMs(action.b));
      break;
    case MIP_ACTION_MOVE_BACKWARD:
      MyMiP.distanceDrive(-action.a, 0);
      break;
    case MIP_ACTION_DRIVE_DISTANCE:
      MyMiP.distanceDrive(action.a, action.b);
      break;
    case MIP_ACTION_TURN_LEFT:
      MyMiP.turnAngle(0, action.a, action.b);
      break;
    case MIP_ACTION_TURN_RIGHT:
      MyMiP.turnAngle(1, action.a, action.b);
      break;
    case MIP_ACTION_SPIN:
      runSpin(action.text, action.a, action.b);
      break;
    case MIP_ACTION_CONTINUOUS_DRIVE:
      runContinuousDrive(action.text, action.a, action.b, false);
      break;
    case MIP_ACTION_CRAZY_DRIVE:
      runContinuousDrive(action.text, action.a, action.b, true);
      break;
    case MIP_ACTION_GAME_MODE:
      MyMiP.setGameMode(gameModeFromName(action.text));
      break;
    case MIP_ACTION_CHEST_LED:
      MyMiP.setChestLED(action.a, action.b, action.c);
      break;
    case MIP_ACTION_FLASH_CHEST_LED:
      MyMiP.flashChestLED(action.a, action.b, action.c, ticks20msFromMs(action.d), ticks20msFromMs(action.e));
      break;
    case MIP_ACTION_HEAD_LEDS:
      MyMiP.setHeadLEDs(action.a, action.b, action.c, action.d);
      break;
    case MIP_ACTION_EXPRESSION_PRESET:
      runExpressionPreset(action.text);
      break;
    case MIP_ACTION_SOUND:
      MyMiP.playSpecific(action.a);
      break;
    case MIP_ACTION_SOUND_SEQUENCE:
      playSoundSequenceFromCsv(action.text, action.a, action.b);
      break;
    case MIP_ACTION_VOLUME:
      MyMiP.setVolume(action.a);
      break;
    case MIP_ACTION_REQUEST_STATUS:
      MyMiP.requestStatus();
      break;
    case MIP_ACTION_REQUEST_WEIGHT:
      MyMiP.requestWeightUpdate();
      break;
    case MIP_ACTION_READ_ODOMETER:
      MyMiP.readOdometer();
      break;
    case MIP_ACTION_RESET_ODOMETER:
      MyMiP.resetOdometer();
      break;
    case MIP_ACTION_GESTURE_RADAR:
      MyMiP.setGestureRadarMode(gestureRadarModeFromName(action.text));
      break;
    case MIP_ACTION_DETECTION_MODE:
      MyMiP.setDetectionMode(action.a, action.b);
      break;
    case MIP_ACTION_IR_CONTROL:
      MyMiP.setIRControl(action.flag ? 1 : 0);
      break;
    case MIP_ACTION_SEND_IR_CODE:
      MyMiP.sendIRCode(action.code, action.a, action.b);
      break;
    case MIP_ACTION_CLAP_DETECTION:
      MyMiP.setClapDetection(action.flag ? 1 : 0);
      break;
    case MIP_ACTION_CLAP_DELAY:
      MyMiP.setClapDelay(action.a);
      break;
    default:
      break;
  }
}

static void mipActionTask(void* parameter)
{
  MipAction action;
  while (true)
  {
    if (xQueueReceive(g_mipActionQueue, &action, portMAX_DELAY) == pdTRUE)
    {
      const char* stateName = actionStateName(action.type);
      Serial.print("[MIP ACTION START] ");
      Serial.println(stateName);
      setRobotActionState(stateName);
      requestRobotStatePublish("action_start");

      runMipAction(action);

      setRobotActionState("idle");
      requestRobotStatePublish("action_done");
      Serial.println("[MIP ACTION DONE]");
    }
  }
}

bool setupMipActionQueue()
{
  if (g_mipActionQueue) return true;

  g_mipActionQueue = xQueueCreate(MIP_ACTION_QUEUE_LEN, sizeof(MipAction));
  if (!g_mipActionQueue)
  {
    Serial.println("[MIP ACTION] Failed to create action queue");
    return false;
  }

  BaseType_t ok = xTaskCreatePinnedToCore(
    mipActionTask,
    "mipActionTask",
    8192,
    nullptr,
    1,
    &g_mipActionTaskHandle,
    1
  );

  if (ok != pdPASS)
  {
    Serial.println("[MIP ACTION] Failed to start action task");
    return false;
  }

  Serial.printf("[MIP ACTION] Queue ready: %u actions\n", MIP_ACTION_QUEUE_LEN);
  return true;
}

static bool enqueueMipAction(const MipAction& action, bool front = false)
{
  if (!g_mipActionQueue && !setupMipActionQueue()) return false;

  if (action.type == MIP_ACTION_STOP)
  {
    xQueueReset(g_mipActionQueue);
    return xQueueSendToFront(g_mipActionQueue, &action, 0) == pdTRUE;
  }

  BaseType_t ok = front
    ? xQueueSendToFront(g_mipActionQueue, &action, 0)
    : xQueueSend(g_mipActionQueue, &action, 0);

  if (ok != pdTRUE)
  {
    Serial.println("[MIP ACTION] Queue full; command dropped");
    requestRobotStatePublish("action_queue_full");
    return false;
  }

  requestRobotStatePublish("action_queued");
  return true;
}

bool enqueueMipStopAction()
{
  MipAction action;
  action.type = MIP_ACTION_STOP;
  return enqueueMipAction(action, true);
}

size_t mipActionQueueDepth()
{
  if (!g_mipActionQueue) return 0;
  return uxQueueMessagesWaiting(g_mipActionQueue);
}

bool enqueueMipActionFromJson(JsonObjectConst doc)
{
  const char* command = doc["command"] | "";
  MipAction action;

  if (strcmp(command, "stop") == 0)
  {
    action.type = MIP_ACTION_STOP;
  }
  else if (strcmp(command, "stand_up") == 0)
  {
    action.type = MIP_ACTION_STAND_UP;
    const char* side = doc["side"] | "either";
    action.a = 2;
    if (strcmp(side, "front") == 0) action.a = 0;
    else if (strcmp(side, "back") == 0) action.a = 1;
  }
  else if (strcmp(command, "set_position") == 0)
  {
    action.type = MIP_ACTION_SET_POSITION;
    const char* pose = doc["pose"] | "face_up";
    action.a = strcmp(pose, "face_down") == 0 ? 1 : 0;
  }
  else if (strcmp(command, "move_forward") == 0)
  {
    action.type = MIP_ACTION_MOVE_FORWARD;
    action.a = constrain(doc["speed"] | 12, 0, 30);
    action.b = doc["duration_ms"] | 700;
  }
  else if (strcmp(command, "move_backward_timed") == 0)
  {
    action.type = MIP_ACTION_MOVE_BACKWARD_TIMED;
    action.a = constrain(doc["speed"] | 12, 0, 30);
    action.b = doc["duration_ms"] | 700;
  }
  else if (strcmp(command, "move_backward") == 0)
  {
    action.type = MIP_ACTION_MOVE_BACKWARD;
    action.a = constrain(doc["distance_cm"] | 10, 1, 255);
  }
  else if (strcmp(command, "drive_distance") == 0)
  {
    action.type = MIP_ACTION_DRIVE_DISTANCE;
    action.a = constrain(doc["distance_cm"] | 10, -255, 255);
    action.b = constrain(doc["angle_deg"] | 0, -360, 360);
  }
  else if (strcmp(command, "turn_left") == 0)
  {
    action.type = MIP_ACTION_TURN_LEFT;
    action.a = constrain(doc["speed"] | 12, 0, 24);
    action.b = constrain(doc["angle_deg"] | 90, 5, 1275);
  }
  else if (strcmp(command, "turn_right") == 0)
  {
    action.type = MIP_ACTION_TURN_RIGHT;
    action.a = constrain(doc["speed"] | 12, 0, 24);
    action.b = constrain(doc["angle_deg"] | 90, 5, 1275);
  }
  else if (strcmp(command, "spin") == 0)
  {
    action.type = MIP_ACTION_SPIN;
    copyText(action.text, sizeof(action.text), doc["direction"] | "left");
    action.a = doc["angle_deg"] | 360;
    action.b = doc["speed"] | 14;
  }
  else if (strcmp(command, "continuous_drive") == 0)
  {
    action.type = MIP_ACTION_CONTINUOUS_DRIVE;
    copyText(action.text, sizeof(action.text), doc["direction"] | "forward");
    action.a = doc["speed"] | 12;
    action.b = doc["duration_ms"] | 800;
  }
  else if (strcmp(command, "crazy_drive") == 0)
  {
    action.type = MIP_ACTION_CRAZY_DRIVE;
    copyText(action.text, sizeof(action.text), doc["direction"] | "forward");
    action.a = doc["speed"] | 14;
    action.b = doc["duration_ms"] | 800;
  }
  else if (strcmp(command, "game_mode") == 0)
  {
    action.type = MIP_ACTION_GAME_MODE;
    copyText(action.text, sizeof(action.text), doc["mode"] | "default");
  }
  else if (strcmp(command, "chest_led") == 0)
  {
    action.type = MIP_ACTION_CHEST_LED;
    action.a = constrain(doc["r"] | 0, 0, 255);
    action.b = constrain(doc["g"] | 0, 0, 255);
    action.c = constrain(doc["b"] | 255, 0, 255);
  }
  else if (strcmp(command, "flash_chest_led") == 0)
  {
    action.type = MIP_ACTION_FLASH_CHEST_LED;
    action.a = constrain(doc["r"] | 0, 0, 255);
    action.b = constrain(doc["g"] | 80, 0, 255);
    action.c = constrain(doc["b"] | 255, 0, 255);
    action.d = doc["on_ms"] | 200;
    action.e = doc["off_ms"] | 200;
  }
  else if (strcmp(command, "head_leds") == 0)
  {
    action.type = MIP_ACTION_HEAD_LEDS;
    action.a = constrain(doc["l1"] | 1, 0, 3);
    action.b = constrain(doc["l2"] | 1, 0, 3);
    action.c = constrain(doc["l3"] | 1, 0, 3);
    action.d = constrain(doc["l4"] | 1, 0, 3);
  }
  else if (strcmp(command, "expression_preset") == 0)
  {
    action.type = MIP_ACTION_EXPRESSION_PRESET;
    copyText(action.text, sizeof(action.text), doc["expression"] | "excited");
  }
  else if (strcmp(command, "sound") == 0)
  {
    action.type = MIP_ACTION_SOUND;
    action.a = constrain(doc["sound_id"] | 1, 1, 106);
  }
  else if (strcmp(command, "sound_sequence") == 0)
  {
    action.type = MIP_ACTION_SOUND_SEQUENCE;
    copyText(action.text, sizeof(action.text), doc["sound_ids"] | "1,3,2");
    action.a = doc["delay_ms"] | 120;
    action.b = doc["repeat"] | 0;
  }
  else if (strcmp(command, "volume") == 0)
  {
    action.type = MIP_ACTION_VOLUME;
    action.a = constrain(doc["volume"] | 4, 0, 7);
  }
  else if (strcmp(command, "request_status") == 0)
  {
    action.type = MIP_ACTION_REQUEST_STATUS;
  }
  else if (strcmp(command, "request_weight") == 0)
  {
    action.type = MIP_ACTION_REQUEST_WEIGHT;
  }
  else if (strcmp(command, "read_odometer") == 0)
  {
    action.type = MIP_ACTION_READ_ODOMETER;
  }
  else if (strcmp(command, "reset_odometer") == 0)
  {
    action.type = MIP_ACTION_RESET_ODOMETER;
  }
  else if (strcmp(command, "gesture_radar") == 0)
  {
    action.type = MIP_ACTION_GESTURE_RADAR;
    copyText(action.text, sizeof(action.text), doc["mode"] | "off");
  }
  else if (strcmp(command, "detection_mode") == 0)
  {
    action.type = MIP_ACTION_DETECTION_MODE;
    action.a = constrain(doc["id"] | 1, 0, 255);
    action.b = constrain(doc["power"] | 60, 1, 120);
  }
  else if (strcmp(command, "ir_control") == 0)
  {
    action.type = MIP_ACTION_IR_CONTROL;
    action.flag = (bool)(doc["enabled"] | true);
  }
  else if (strcmp(command, "send_ir_code") == 0)
  {
    action.type = MIP_ACTION_SEND_IR_CODE;
    action.code = doc["code"] | 0;
    action.a = constrain(doc["bit_count"] | 32, 1, 32);
    action.b = constrain(doc["power"] | 60, 1, 120);
  }
  else if (strcmp(command, "clap_detection") == 0)
  {
    action.type = MIP_ACTION_CLAP_DETECTION;
    action.flag = (bool)(doc["enabled"] | true);
  }
  else if (strcmp(command, "clap_delay") == 0)
  {
    action.type = MIP_ACTION_CLAP_DELAY;
    action.a = constrain(doc["delay_ms"] | 500, 0, 65535);
  }
  else
  {
    Serial.print("[MIP COMMAND UNKNOWN] ");
    Serial.println(command);
    return false;
  }

  Serial.print("[MIP ACTION QUEUE] ");
  Serial.println(command);
  return enqueueMipAction(action);
}
