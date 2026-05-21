#ifndef SPEAKER_AUDIO_QUEUE_H
#define SPEAKER_AUDIO_QUEUE_H

#include <Arduino.h>
#include <stdint.h>

// Queue assistant PCM16 mono audio chunks from the WebSocket callback and play
// them from a dedicated task. This prevents I2S speaker writes from blocking
// client.poll() / WebSocket callbacks.
void setupSpeakerAudioQueue();

// Enqueue PCM16 mono frames. The data is copied immediately, so the caller can
// return and the original buffer can go out of scope.
bool enqueueSpeakerAudioChunk(const int16_t* pcm, size_t frames);

// Clear any queued assistant audio, reset mouth level, and return to idle.
void clearSpeakerAudioQueue();

// True while the speaker task is actively draining queued assistant audio.
bool speakerAudioQueueIsPlaying();

#endif // SPEAKER_AUDIO_QUEUE_H
