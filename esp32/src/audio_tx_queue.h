#ifndef AUDIO_TX_QUEUE_H
#define AUDIO_TX_QUEUE_H

#include <Arduino.h>

// Queue for mic PCM chunks waiting to be sent over the WebSocket.
// This keeps the mic task from calling the WebSocket client directly.
// Call setupAudioTxQueue() once in setup(), enqueueAudioChunk() from micTask(),
// and serviceAudioTxQueue() frequently from loop().

bool setupAudioTxQueue();
void clearAudioTxQueue();
bool enqueueAudioChunk(const int16_t* samples, size_t frames);

// Sends up to maxChunks queued chunks. Call from main loop only.
void serviceAudioTxQueue(uint8_t maxChunks = 4);

// Useful for status/debug.
size_t audioTxQueueDepth();

#endif // AUDIO_TX_QUEUE_H
