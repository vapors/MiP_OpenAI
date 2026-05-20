#ifndef VAD_CONTROLLER_H
#define VAD_CONTROLLER_H

#include <Arduino.h>

// Basic RMS voice activity detector used for the first hands-free test mode.
// It does not stream audio by itself; it calls the existing PTT helpers when
// speech starts/ends. The stable 16 kHz PCM upload path remains unchanged.

void setupVadController();
void vadReset();
void vadSuppressForMs(uint32_t ms);
void vadProcessFrames(const int16_t* samples, size_t frames);

bool vadIsListening();
bool vadSpeechActive();
uint32_t vadLastRms();

#endif // VAD_CONTROLLER_H
