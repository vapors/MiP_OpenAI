#include <Arduino.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include "lib_speaker.h"
#include "config.h"
#include "MiP_commands.h"

extern MiP MyMiP;

using namespace websockets;

WebsocketsClient client;

static uint32_t last_ms = 0;
static uint64_t total_samples = 0;

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

  // Protocol ranges:
  // normal forward 0x01-0x20, backward 0x21-0x40, right spin 0x41-0x60, left spin 0x61-0x80
  // crazy forward 0x81-0xA0, backward 0xA1-0xC0, right spin 0xC1-0xE0, left spin 0xE1-0xFF
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

static void runContinuousDrive(const char* direction, int speed, int durationMs, bool crazy)
{
  uint8_t code = continuousDriveCode(direction, speed, crazy);
  durationMs = constrain(durationMs, 50, 5000);

  uint32_t start = millis();
  while ((millis() - start) < (uint32_t)durationMs)
  {
    MyMiP.continuousDrive(code);
    delay(45); // Protocol recommends repeat while held, roughly every 50ms.
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
    // Simple dwell so a multi-turn spin does not stack too aggressively.
    delay(max(250, step * 2));
    remaining -= step;
  }
}

static void playSoundSequenceFromCsv(const String& csv, int delayMs, int repeat)
{
  uint8_t ids[8];
  uint8_t count = 0;
  int start = 0;

  while (count < 8 && start < csv.length())
  {
    int comma = csv.indexOf(',', start);
    String part = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
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

void handleMipCommand(const String& text)
{
  StaticJsonDocument<768> doc;
  DeserializationError error = deserializeJson(doc, text);

  if (error)
  {
    Serial.print("[WS JSON ERROR] ");
    Serial.println(error.c_str());
    Serial.println(text);
    return;
  }

  const char* type = doc["type"] | "";
  const char* command = doc["command"] | "";

  if (strcmp(type, "mip_command") != 0)
  {
    Serial.print("[WS TEXT IGNORED] ");
    Serial.println(text);
    return;
  }

  Serial.print("[MIP COMMAND] ");
  Serial.println(command);

  if (strcmp(command, "stop") == 0)
  {
    MyMiP.stop();
  }
  else if (strcmp(command, "stand_up") == 0)
  {
    const char* side = doc["side"] | "either";
    int state = 2;
    if (strcmp(side, "front") == 0) state = 0;
    else if (strcmp(side, "back") == 0) state = 1;
    MyMiP.standUp(state);
  }
  else if (strcmp(command, "set_position") == 0)
  {
    const char* pose = doc["pose"] | "face_up";
    MyMiP.setPosition(strcmp(pose, "face_down") == 0 ? FACEDOWN : FACEUP);
  }
  else if (strcmp(command, "move_forward") == 0)
  {
    int speed = constrain(doc["speed"] | 12, 0, 30);
    int durationMs = doc["duration_ms"] | 700;
    MyMiP.driveForward(speed, timeUnitsFromMs(durationMs));
  }
  else if (strcmp(command, "move_backward_timed") == 0)
  {
    int speed = constrain(doc["speed"] | 12, 0, 30);
    int durationMs = doc["duration_ms"] | 700;
    MyMiP.driveBackward(speed, timeUnitsFromMs(durationMs));
  }
  else if (strcmp(command, "move_backward") == 0)
  {
    int distance = constrain(doc["distance_cm"] | 10, 1, 255);
    MyMiP.distanceDrive(-distance, 0);
  }
  else if (strcmp(command, "drive_distance") == 0)
  {
    int distance = constrain(doc["distance_cm"] | 10, -255, 255);
    int angle = constrain(doc["angle_deg"] | 0, -360, 360);
    MyMiP.distanceDrive(distance, angle);
  }
  else if (strcmp(command, "turn_left") == 0)
  {
    int speed = constrain(doc["speed"] | 12, 0, 24);
    int angle = constrain(doc["angle_deg"] | 90, 5, 1275);
    MyMiP.turnAngle(0, speed, angle);
  }
  else if (strcmp(command, "turn_right") == 0)
  {
    int speed = constrain(doc["speed"] | 12, 0, 24);
    int angle = constrain(doc["angle_deg"] | 90, 5, 1275);
    MyMiP.turnAngle(1, speed, angle);
  }
  else if (strcmp(command, "spin") == 0)
  {
    runSpin(doc["direction"] | "left", doc["angle_deg"] | 360, doc["speed"] | 14);
  }
  else if (strcmp(command, "continuous_drive") == 0)
  {
    runContinuousDrive(doc["direction"] | "forward", doc["speed"] | 12, doc["duration_ms"] | 800, false);
  }
  else if (strcmp(command, "crazy_drive") == 0)
  {
    runContinuousDrive(doc["direction"] | "forward", doc["speed"] | 14, doc["duration_ms"] | 800, true);
  }
  else if (strcmp(command, "game_mode") == 0)
  {
    MyMiP.setGameMode(gameModeFromName(doc["mode"] | "default"));
  }
  else if (strcmp(command, "chest_led") == 0)
  {
    MyMiP.setChestLED(
      constrain(doc["r"] | 0, 0, 255),
      constrain(doc["g"] | 0, 0, 255),
      constrain(doc["b"] | 255, 0, 255)
    );
  }
  else if (strcmp(command, "flash_chest_led") == 0)
  {
    MyMiP.flashChestLED(
      constrain(doc["r"] | 0, 0, 255),
      constrain(doc["g"] | 80, 0, 255),
      constrain(doc["b"] | 255, 0, 255),
      ticks20msFromMs(doc["on_ms"] | 200),
      ticks20msFromMs(doc["off_ms"] | 200)
    );
  }
  else if (strcmp(command, "head_leds") == 0)
  {
    MyMiP.setHeadLEDs(
      constrain(doc["l1"] | 1, 0, 3),
      constrain(doc["l2"] | 1, 0, 3),
      constrain(doc["l3"] | 1, 0, 3),
      constrain(doc["l4"] | 1, 0, 3)
    );
  }
  else if (strcmp(command, "expression_preset") == 0)
  {
    runExpressionPreset(doc["expression"] | "excited");
  }
  else if (strcmp(command, "sound") == 0)
  {
    MyMiP.playSpecific(constrain(doc["sound_id"] | 1, 1, 106));
  }
  else if (strcmp(command, "sound_sequence") == 0)
  {
    String ids = doc["sound_ids"] | "1,3,2";
    playSoundSequenceFromCsv(ids, doc["delay_ms"] | 120, doc["repeat"] | 0);
  }
  else if (strcmp(command, "volume") == 0)
  {
    MyMiP.setVolume(constrain(doc["volume"] | 4, 0, 7));
  }
  else if (strcmp(command, "request_status") == 0)
  {
    MyMiP.requestStatus();
  }
  else if (strcmp(command, "request_weight") == 0)
  {
    MyMiP.requestWeightUpdate();
  }
  else if (strcmp(command, "read_odometer") == 0)
  {
    MyMiP.readOdometer();
  }
  else if (strcmp(command, "reset_odometer") == 0)
  {
    MyMiP.resetOdometer();
  }
  else if (strcmp(command, "gesture_radar") == 0)
  {
    MyMiP.setGestureRadarMode(gestureRadarModeFromName(doc["mode"] | "off"));
  }
  else if (strcmp(command, "detection_mode") == 0)
  {
    MyMiP.setDetectionMode(constrain(doc["id"] | 1, 0, 255), constrain(doc["power"] | 60, 1, 120));
  }
  else if (strcmp(command, "ir_control") == 0)
  {
    MyMiP.setIRControl((bool)(doc["enabled"] | true) ? 1 : 0);
  }
  else if (strcmp(command, "send_ir_code") == 0)
  {
    uint32_t code = doc["code"] | 0;
    MyMiP.sendIRCode(code, constrain(doc["bit_count"] | 32, 1, 32), constrain(doc["power"] | 60, 1, 120));
  }
  else if (strcmp(command, "clap_detection") == 0)
  {
    MyMiP.setClapDetection((bool)(doc["enabled"] | true) ? 1 : 0);
  }
  else if (strcmp(command, "clap_delay") == 0)
  {
    MyMiP.setClapDelay(constrain(doc["delay_ms"] | 500, 0, 65535));
  }
  else
  {
    Serial.print("[MIP COMMAND UNKNOWN] ");
    Serial.println(command);
  }
}

void log_audio_rate(size_t frames)
{
  total_samples += frames;

  uint32_t now = millis();
  if (last_ms == 0) {
    last_ms = now;
    return;
  }

  uint32_t dt = now - last_ms;
  if (dt >= 1000) {
    float inferred_rate = (float)total_samples * 1000.0f / dt;
    Serial.printf("[AUDIO INFERRED] ~%.0f Hz (%llu samples in %lu ms)\n",
                  inferred_rate, total_samples, dt);
    total_samples = 0;
    last_ms = now;
  }
}

void onMessageCallback(WebsocketsMessage message)
{
  if (message.isBinary())
  {
    const uint8_t* payload = (const uint8_t*)message.c_str();
    size_t bytes = message.length();

    if (bytes < 2) return;
    if (bytes & 1) bytes--;

    const int16_t* pcm = (const int16_t*)payload;
    const size_t frames = bytes / sizeof(int16_t);

    speaker_write_mono_i16(pcm, frames);
    return;
  }

  Serial.print("[WS TEXT] ");
  Serial.println(message.data());
  handleMipCommand(message.data());
}

void onEventsCallback(WebsocketsEvent event, String data)
{
  if (event == WebsocketsEvent::ConnectionOpened)
  {
    Serial.println("Connection Opened");
  }
  else if (event == WebsocketsEvent::ConnectionClosed)
  {
    Serial.println("Connection Closed");
  }
  else if (event == WebsocketsEvent::GotPing)
  {
    Serial.println("Got a Ping!");
  }
  else if (event == WebsocketsEvent::GotPong)
  {
    Serial.println("Got a Pong!");
  }
}

void connectToWebSocket()
{
  client.onMessage(onMessageCallback);
  client.onEvent(onEventsCallback);

  const char *websockets_server_host = "mio.pixelandpiece.com";
  const uint16_t websockets_server_port = 8888;

  bool connected = false;
  while (!connected)
  {
    if (client.connect(websockets_server_host, websockets_server_port, "/device"))
    {
      connected = true;
      Serial.println("WebSocket Connected!");
      client.send("Hello Server");
      client.ping();
    }
    else
    {
      Serial.println("WebSocket Connection Failed! Retrying in 2 seconds...");
      delay(2000);
    }
  }
}

void checkWebSocketConnection()
{
  if (!client.available())
  {
    Serial.println("WebSocket connection lost. Reconnecting...");
    connectToWebSocket();
  }
  client.poll();
}

void sendMessage(const char *message)
{
  if (client.available())
  {
    Serial.printf("[WS TX %lu ms] text=%s\n", millis(), message);
    client.send(message);
  }
  else
  {
    Serial.printf("[WS TX %lu ms] text failed; websocket not connected: %s\n", millis(), message);
  }
}

void sendButtonState(bool buttonState)
{
  if (client.available())
  {
    uint8_t buttonMessage = buttonState ? 1 : 0;
    client.sendBinary((const char *)&buttonMessage, sizeof(buttonMessage));
  }
  else
  {
    Serial.println("WebSocket not connected - cannot send button state");
  }
}

void sendBinaryData(const int16_t *buffer, size_t bytesIn)
{
  static uint32_t chunkCount = 0;
  static uint32_t lastLogMs = 0;

  if (client.available())
  {
    const char *charBuffer = reinterpret_cast<const char *>(buffer);
    bool ok = client.sendBinary(charBuffer, bytesIn);
    chunkCount++;

    uint32_t now = millis();
    if (chunkCount <= 3 || (now - lastLogMs) >= 500)
    {
      Serial.printf("[WS TX %lu ms] audio chunk=%lu bytes=%u ok=%d\n",
                    now,
                    (unsigned long)chunkCount,
                    (unsigned)bytesIn,
                    ok ? 1 : 0);
      lastLogMs = now;
    }
  }
  else
  {
    Serial.printf("[WS TX %lu ms] audio failed; websocket not connected bytes=%u\n",
                  millis(),
                  (unsigned)bytesIn);
  }
}

void reconnectWSServer()
{
  if (!client.available())
  {
    Serial.println("WebSocket connection lost. Attempting to reconnect...");
    connectToWebSocket();
  }
}

void loopWebsocket()
{
  client.poll();
}
