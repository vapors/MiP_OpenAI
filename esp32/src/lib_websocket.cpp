#include <Arduino.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>

#include "lib_speaker.h"
#include "config.h"
#include "mip_action_queue.h"
#include "robot_status.h"
#include "vad_controller.h"

using namespace websockets;

WebsocketsClient client;

static uint32_t last_ms = 0;
static uint64_t total_samples = 0;

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

  Serial.print("[MIP COMMAND QUEUED] ");
  Serial.println(command);

  if (!enqueueMipActionFromJson(doc.as<JsonObjectConst>()))
  {
    publishRobotState("mip_command_rejected", true);
  }
}

void log_audio_rate(size_t frames)
{
  total_samples += frames;

  uint32_t now = millis();
  if (last_ms == 0)
  {
    last_ms = now;
    return;
  }

  uint32_t dt = now - last_ms;
  if (dt >= 1000)
  {
    float inferred_rate = (float)total_samples * 1000.0f / dt;
    Serial.printf("[AUDIO INFERRED] ~%.0f Hz (%llu samples in %lu ms)\n",
                  inferred_rate,
                  (unsigned long long)total_samples,
                  (unsigned long)dt);
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

    // Suppress VAD while the assistant is speaking and briefly after each chunk
    // to reduce echo/retrigger loops.
    vadSuppressForMs(2000);
    //speaker_write_mono_i16(pcm, frames);

      uint32_t t0 = millis();
      bool ok = speaker_write_mono_i16(pcm, frames, 20);
      uint32_t dt = millis() - t0;

      if (dt > 80 || !ok)
      {
        Serial.printf("[SPK RX] frames=%u writeMs=%lu ok=%d heap=%u\n",
                      (unsigned int)frames,
                      (unsigned long)dt,
                      ok ? 1 : 0,
                      ESP.getFreeHeap());
      }

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
    publishRobotState("ws_open", true);
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
      publishRobotState("ws_connected", true);
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
    Serial.printf("[WS TX %lu ms] text=%s\n", (unsigned long)millis(), message);
    client.send(message);
  }
  else
  {
    Serial.printf("[WS TX %lu ms] text failed; websocket not connected: %s\n", (unsigned long)millis(), message);
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
                    (unsigned long)now,
                    (unsigned long)chunkCount,
                    (unsigned int)bytesIn,
                    ok ? 1 : 0);
      lastLogMs = now;
    }
  }
  else
  {
    Serial.printf("[WS TX %lu ms] audio failed; websocket not connected bytes=%u\n",
                  (unsigned long)millis(),
                  (unsigned int)bytesIn);
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
