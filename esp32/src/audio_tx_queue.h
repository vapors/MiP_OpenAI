#ifndef AUDIO_TX_QUEUE_H
#define AUDIO_TX_QUEUE_H

#include <Arduino.h>

// LEGACY fallback for queued mic PCM chunks.
// The current recommended input path does NOT use this queue. micTask streams
// directly via lib_websocket::sendBinaryData(), which keeps audio real-time while
// still protecting the WebSocket client with a mutex.
// Keep this only for future experiments.

bool setupAudioTxQueue();
void clearAudioTxQueue();
bool enqueueAudioChunk(const int16_t* samples, size_t frames);

// Sends up to maxChunks queued chunks. Call from main loop only.
void serviceAudioTxQueue(uint8_t maxChunks = 4);

// Useful for status/debug.
size_t audioTxQueueDepth();

#endif // AUDIO_TX_QUEUE_H
