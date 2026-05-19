#ifndef UTILS_H
#define UTILS_H

#include <Arduino.h>

void *audio_malloc(size_t size);
float calculateRMS(int32_t *samples, size_t count);

#endif
