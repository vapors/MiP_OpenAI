#ifndef LIB_SPEAKER_H
#define LIB_SPEAKER_H

#include <Audio.h>
#include <Arduino.h>
#include <driver/i2s.h>
#include <stdint.h>
// #include "audioBuffer.h"
enum AudioMode {
  MODE_MIC,
  MODE_SPK
};
void InitI2SSpeakerOrMic(AudioMode mode);
esp_err_t setupSpeakerI2S();
void loopAudio();
void setupAudio();
void generateTone(int16_t *buffer, size_t samples);
void writeToAudioBuffer(int16_t *buffer, size_t samples);
void playBuffer(int16_t *buffer, size_t samples);
void handleSpeaker();
void playBufferWithOffset(uint8_t *payload, size_t length);
void speaker_play(uint8_t *payload, uint32_t len);


extern volatile uint8_t g_mouthLevel;


// Write PCM16 mono frames to speaker and drive mouth animation.
// Returns true on success.
bool speaker_write_mono_i16(const int16_t* mono,
                            size_t frames,
                            uint32_t timeout_ms = 50);

// Optional helpers
void speaker_set_volume(float v01);   // 0.0..1.0


extern unsigned long lastMicActivity;
extern unsigned long lastSpkrActivity; 
extern unsigned long lastToneTime;
extern bool isPlayingTone;
extern unsigned long toneStartTime;
// Face animation driver (0..255)
extern volatile uint8_t g_mouthLevel;

#endif
