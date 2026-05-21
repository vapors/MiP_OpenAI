#include <Arduino.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <string.h>

#include "lib_speaker.h"
#include "config.h"
#include "mip_action_queue.h"
#include "robot_status.h"
#include "vad_controller.h"
#include "speaker_audio_queue.h"
#include "ble_control.h"

using namespace websockets;

WebsocketsClient client;

static uint32_t last_ms = 0;
static uint64_t total_samples = 0;

// -----------------------------------------------------------------------------
// WebSocket TX queue
// -----------------------------------------------------------------------------
// Important: ArduinoWebsockets/LwIP is not safe to write from multiple FreeRTOS
// tasks at the same time. Robot-state/control text traffic is copied into this
// queue. Live microphone audio is intentionally NOT queued here; it uses a
// direct mutex-protected send path so the stream stays real-time and does not
// build stale backlog.
//
// client.poll()/connect/send/sendBinary are protected with the same mutex so
// tasks do not touch the WebSocket/LwIP internals simultaneously.
// -----------------------------------------------------------------------------

enum WsTxType : uint8_t
{
  WS_TX_TEXT = 1,
  WS_TX_BINARY = 2,
};

static const uint16_t WS_TX_MAX_BYTES = 1200;  // enough for 1024-byte audio chunks + JSON state
static const uint8_t WS_TX_QUEUE_LEN = 24;
static const uint32_t WS_TX_LOG_INTERVAL_MS = 1000;

struct WsTxMessage
{
  uint8_t type;
  uint16_t len;
  uint8_t data[WS_TX_MAX_BYTES];
};

static QueueHandle_t g_wsTxQueue = nullptr;
static TaskHandle_t g_wsTxTaskHandle = nullptr;
static SemaphoreHandle_t g_wsClientMutex = nullptr;

static volatile bool g_wsConnected = false;
static uint32_t g_wsTxTextCount = 0;
static uint32_t g_wsTxBinaryCount = 0;
static uint32_t g_wsTxDropped = 0;
static uint32_t g_wsTxLastLogMs = 0;
static uint32_t g_wsTxLastReportedDropped = 0;

static void setupWebSocketTxCore();

bool isWebSocketClientConnected()
{
  return g_wsConnected;
}

static bool wsClientAvailableLocked()
{
  bool available = false;

  if (!g_wsClientMutex)
  {
    return false;
  }

  if (xSemaphoreTake(g_wsClientMutex, pdMS_TO_TICKS(100)) == pdTRUE)
  {
    available = client.available();
    xSemaphoreGive(g_wsClientMutex);
  }

  return available;
}

static void logWsTxStatus(const char* reason, bool force = false)
{
  if (!g_wsTxQueue) return;

  const uint32_t now = millis();
  const bool droppedChanged = (g_wsTxDropped != g_wsTxLastReportedDropped);
  const bool intervalElapsed = (now - g_wsTxLastLogMs) >= WS_TX_LOG_INTERVAL_MS;

  if (!force && !droppedChanged && !intervalElapsed)
  {
    return;
  }

  Serial.printf("[WS TXQ] %s depth=%u text=%lu bin=%lu dropped=%lu heap=%u\n",
                reason,
                (unsigned int)uxQueueMessagesWaiting(g_wsTxQueue),
                (unsigned long)g_wsTxTextCount,
                (unsigned long)g_wsTxBinaryCount,
                (unsigned long)g_wsTxDropped,
                ESP.getFreeHeap());

  g_wsTxLastLogMs = now;
  g_wsTxLastReportedDropped = g_wsTxDropped;
}

void clearWebSocketTxQueue()
{
  if (g_wsTxQueue)
  {
    xQueueReset(g_wsTxQueue);
  }
  logWsTxStatus("cleared", true);
}

void handleWebSocketTransportLost(const char* reason)
{
  if (!reason) reason = "disconnect";

  if (g_wsConnected)
  {
    Serial.printf("[WS] transport lost: %s\n", reason);
  }

  g_wsConnected = false;

  // Live audio is disposable. If the transport disappears, discard stale queued
  // audio/status frames and abort the active recording locally.
  clearWebSocketTxQueue();
  forcePttAbortFromWebSocketDisconnect();
}

static bool enqueueWsTx(uint8_t type, const uint8_t* data, size_t len, bool important)
{
  if (!data || len == 0)
  {
    return false;
  }

  setupWebSocketTxCore();

  if (!g_wsTxQueue)
  {
    return false;
  }

  // Do not build a backlog while disconnected. Audio frames are stale if they
  // cannot be sent immediately; status will be republished after reconnect.
  if (!g_wsConnected)
  {
    if (type == WS_TX_BINARY)
    {
      g_wsTxDropped++;
      return false;
    }

    // Allow only a very small set of important text messages to attempt queueing
    // while the connection state is transitioning.
    if (!important)
    {
      g_wsTxDropped++;
      return false;
    }
  }

  WsTxMessage msg;
  msg.type = type;
  msg.len = (uint16_t)min(len, (size_t)WS_TX_MAX_BYTES);
  memcpy(msg.data, data, msg.len);

  // Important text/status/control messages get a short chance to enqueue.
  // Audio uses no-wait so the mic task does not block.
  BaseType_t ok = xQueueSend(
    g_wsTxQueue,
    &msg,
    important ? pdMS_TO_TICKS(30) : 0
  );

  if (ok != pdTRUE)
  {
    // If the queue is full, drop the oldest queued audio chunk first when
    // possible. This keeps recent mic/control traffic moving and avoids LwIP
    // re-entrancy crashes from direct sends in worker tasks.
    WsTxMessage oldMsg;
    bool madeRoom = false;

    const UBaseType_t queued = uxQueueMessagesWaiting(g_wsTxQueue);
    for (UBaseType_t i = 0; i < queued; i++)
    {
      if (xQueueReceive(g_wsTxQueue, &oldMsg, 0) != pdTRUE)
      {
        break;
      }

      if (!madeRoom && oldMsg.type == WS_TX_BINARY)
      {
        madeRoom = true;
        g_wsTxDropped++;
        continue;
      }

      xQueueSend(g_wsTxQueue, &oldMsg, 0);
    }

    if (madeRoom)
    {
      ok = xQueueSend(g_wsTxQueue, &msg, 0);
    }
  }

  if (ok != pdTRUE)
  {
    g_wsTxDropped++;
    logWsTxStatus(type == WS_TX_TEXT ? "drop_text" : "drop_binary", true);
    return false;
  }

  logWsTxStatus("enqueue");
  return true;
}

static bool wsSendRawTextLocked(const uint8_t* data, size_t len)
{
  if (!data || len == 0) return false;

  // WebsocketsClient::send expects a String/char data. The payload is copied so
  // it is safe even if it contains no trailing null.
  String text;
  text.reserve(len + 1);
  for (size_t i = 0; i < len; i++)
  {
    text += (char)data[i];
  }

  return client.available() && client.send(text);
}

static bool wsSendRawBinaryLocked(const uint8_t* data, size_t len)
{
  if (!data || len == 0) return false;
  return client.available() && client.sendBinary((const char*)data, len);
}

// Live microphone audio is handled separately from the text/status TX queue.
// The queue is good for robot_state/control text, but live audio must stay
// real-time and disposable. This direct path still uses the WebSocket mutex so
// only one task touches ArduinoWebsockets/LwIP at a time.
static bool wsSendMicAudioDirect(const uint8_t* data, size_t len, bool* transportLost)
{
  if (transportLost) *transportLost = false;

  if (!data || len == 0)
  {
    return false;
  }

  if (!g_wsConnected || !g_wsClientMutex)
  {
    return false;
  }

  // Do not block the mic task for long. If another task has the WebSocket for a
  // moment, drop this audio chunk rather than building latency.
  if (xSemaphoreTake(g_wsClientMutex, pdMS_TO_TICKS(35)) != pdTRUE)
  {
    return false;
  }

  bool ok = false;
  bool lost = false;

  if (client.available())
  {
    ok = client.sendBinary((const char*)data, len);

    // Give the websocket client a chance to service ACKs/frames immediately
    // after a binary write. This helped the older direct-streaming path.
    client.poll();

    if (!ok || !client.available())
    {
      lost = true;
    }
  }
  else
  {
    lost = true;
  }

  xSemaphoreGive(g_wsClientMutex);

  if (transportLost) *transportLost = lost;
  return ok;
}

static void websocketTxTask(void* parameter)
{
  Serial.println("[WS TXQ] task started");

  WsTxMessage msg;

  while (true)
  {
    if (xQueueReceive(g_wsTxQueue, &msg, pdMS_TO_TICKS(100)) != pdTRUE)
    {
      continue;
    }

    bool ok = false;

    if (g_wsClientMutex && xSemaphoreTake(g_wsClientMutex, pdMS_TO_TICKS(250)) == pdTRUE)
    {
      if (client.available())
      {
        if (msg.type == WS_TX_TEXT)
        {
          ok = wsSendRawTextLocked(msg.data, msg.len);
        }
        else if (msg.type == WS_TX_BINARY)
        {
          ok = wsSendRawBinaryLocked(msg.data, msg.len);
        }
      }

      xSemaphoreGive(g_wsClientMutex);
    }

    if (ok)
    {
      if (msg.type == WS_TX_TEXT)
      {
        g_wsTxTextCount++;
      }
      else if (msg.type == WS_TX_BINARY)
      {
        g_wsTxBinaryCount++;
      }
    }
    else
    {
      g_wsTxDropped++;
      logWsTxStatus(msg.type == WS_TX_TEXT ? "send_text_failed" : "send_binary_failed", true);
      handleWebSocketTransportLost(msg.type == WS_TX_TEXT ? "text_send_failed" : "binary_send_failed");
    }

    logWsTxStatus("sent");
    taskYIELD();
  }
}

static void setupWebSocketTxCore()
{
  if (!g_wsClientMutex)
  {
    g_wsClientMutex = xSemaphoreCreateMutex();
  }

  if (!g_wsTxQueue)
  {
    g_wsTxQueue = xQueueCreate(WS_TX_QUEUE_LEN, sizeof(WsTxMessage));

    if (!g_wsTxQueue)
    {
      Serial.println("[WS TXQ] ERROR: failed to create queue");
      return;
    }

    Serial.printf("[WS TXQ] ready: %u messages x %u bytes\n",
                  (unsigned int)WS_TX_QUEUE_LEN,
                  (unsigned int)WS_TX_MAX_BYTES);
  }

  if (!g_wsTxTaskHandle)
  {
    xTaskCreatePinnedToCore(
      websocketTxTask,
      "websocketTxTask",
      8192,
      nullptr,
      1,
      &g_wsTxTaskHandle,
      1
    );
  }
}

// -----------------------------------------------------------------------------
// Incoming WebSocket handling
// -----------------------------------------------------------------------------

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

    // Do not write to I2S from the WebSocket callback. Copy the audio into a
    // queue and let speakerAudioTask handle playback so client.poll() is not
    // blocked by speaker writes.
    vadSuppressForMs(1800);
    bool ok = enqueueSpeakerAudioChunk(pcm, frames);

    if (!ok)
    {
      Serial.printf("[SPK Q] enqueue failed from WS frames=%u bytes=%u heap=%u\n",
                    (unsigned int)frames,
                    (unsigned int)bytes,
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
    g_wsConnected = true;
    Serial.println("Connection Opened");
    requestRobotStatePublish("ws_open");
  }
  else if (event == WebsocketsEvent::ConnectionClosed)
  {
    Serial.println("Connection Closed");
    handleWebSocketTransportLost("event_closed");
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
  setupWebSocketTxCore();

  const char *websockets_server_host = "mio.pixelandpiece.com";
  const uint16_t websockets_server_port = 8888;

  bool connected = false;

  while (!connected)
  {
    if (g_wsClientMutex && xSemaphoreTake(g_wsClientMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
      client.onMessage(onMessageCallback);
      client.onEvent(onEventsCallback);

      connected = client.connect(websockets_server_host, websockets_server_port, "/device");

      if (connected)
      {
        g_wsConnected = true;
        Serial.println("WebSocket Connected!");
        client.send("Hello Server");
        client.ping();
      }

      xSemaphoreGive(g_wsClientMutex);
    }

    if (connected)
    {
      publishRobotState("ws_connected", true);
    }
    else
    {
      g_wsConnected = false;
      Serial.println("WebSocket Connection Failed! Retrying in 2 seconds...");
      delay(2000);
    }
  }
}

void checkWebSocketConnection()
{
  if (!g_wsClientMutex)
  {
    return;
  }

  // If another task is currently sending audio/text, do not assume the socket is
  // dead just because we cannot grab the mutex. The previous version treated a
  // mutex timeout as a lost WebSocket, which caused reconnects right after audio
  // streaming began.
  if (xSemaphoreTake(g_wsClientMutex, pdMS_TO_TICKS(10)) != pdTRUE)
  {
    return;
  }

  bool availableBefore = client.available();

  if (availableBefore)
  {
    client.poll();
  }

  bool availableAfter = client.available();
  xSemaphoreGive(g_wsClientMutex);

  g_wsConnected = availableAfter;

  if (!availableAfter)
  {
    Serial.println("WebSocket connection lost. Reconnecting...");
    handleWebSocketTransportLost("availability_check");
    connectToWebSocket();
  }
}

void sendMessage(const char *message)
{
  if (!message || message[0] == '\0')
  {
    return;
  }

  const bool ok = enqueueWsTx(
    WS_TX_TEXT,
    (const uint8_t*)message,
    strlen(message),
    true
  );

  if (!ok)
  {
    Serial.printf("[WS TXQ] text enqueue failed: %.80s\n", message);
  }
}

void sendButtonState(bool buttonState)
{
  uint8_t buttonMessage = buttonState ? 1 : 0;
  enqueueWsTx(WS_TX_BINARY, &buttonMessage, sizeof(buttonMessage), false);
}

void sendBinaryData(const int16_t *buffer, size_t bytesIn)
{
  static uint32_t chunkCount = 0;
  static uint32_t lastLogMs = 0;
  static uint32_t droppedAudioChunks = 0;

  if (!buffer || bytesIn == 0)
  {
    return;
  }

  if (!g_wsConnected)
  {
    droppedAudioChunks++;
    return;
  }

  bool transportLost = false;
  const bool ok = wsSendMicAudioDirect((const uint8_t*)buffer, bytesIn, &transportLost);

  chunkCount++;

  const uint32_t now = millis();

  if (!ok)
  {
    droppedAudioChunks++;
  }

  if (chunkCount <= 3 || (now - lastLogMs) >= 1500 || transportLost)
  {
    Serial.printf("[WS AUDIO %lu ms] chunk=%lu bytes=%u sent=%d lost=%d dropped=%lu\n",
                  (unsigned long)now,
                  (unsigned long)chunkCount,
                  (unsigned int)bytesIn,
                  ok ? 1 : 0,
                  transportLost ? 1 : 0,
                  (unsigned long)droppedAudioChunks);
    lastLogMs = now;
  }

  if (transportLost)
  {
    Serial.println("[WS AUDIO] transport lost during direct mic send");
    handleWebSocketTransportLost("audio_send_failed");
  }
}

void reconnectWSServer()
{
  if (!wsClientAvailableLocked())
  {
    Serial.println("WebSocket connection lost. Attempting to reconnect...");
    connectToWebSocket();
  }
}

void loopWebsocket()
{
  checkWebSocketConnection();
}
