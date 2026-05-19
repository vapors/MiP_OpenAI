#ifndef MIC_H
#define MIC_H

#include <Arduino.h>

void detectSound(int16_t *buffer, size_t length);
bool setupMicrophone();
size_t handleMicrophone(int16_t* out_mono, size_t frames, uint32_t timeout_ms);
void micTask(void *parameter);
void setRecording(bool recording);

#endif
