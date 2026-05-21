#ifndef LIB_WEBSOCKET_H
#define LIB_WEBSOCKET_H

#include <ArduinoWebsockets.h>

using namespace websockets;

extern WebsocketsClient client;
bool isWebSocketClientConnected();
void log_audio_rate(size_t frames);
void onMessageCallback(WebsocketsMessage message);
void onEventsCallback(WebsocketsEvent event, String data);
void connectToWebSocket();
void checkWebSocketConnection();
// Real-time mic PCM upload. This is intentionally not routed through the text TX queue.
void sendBinaryData(const int16_t* buffer, size_t bytesIn);
void sendMessage(const char* message);
void loopWebsocket();
void sendButtonState(bool buttonState);

// Clears any queued outbound WebSocket messages.
void clearWebSocketTxQueue();

// Called by WebSocket event/reconnect logic when transport is unavailable.
void handleWebSocketTransportLost(const char* reason = "disconnect");
#endif
