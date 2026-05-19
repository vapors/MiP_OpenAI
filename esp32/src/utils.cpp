#include <Arduino.h>
#include "config.h"

void *audio_malloc(size_t size) {
    void *ptr = psramFound() ? heap_caps_malloc(size, MALLOC_CAP_SPIRAM) : malloc(size);
    if (!ptr) {
        Serial.println("Failed to allocate memory");
    }
    return ptr;
}



float calculateRMS(int32_t *samples, size_t count)
{
  float sum = 0;
  for (size_t i = 0; i < count; i++)
  {
    sum += samples[i] * samples[i];
  }
  return sqrt(sum / count);
}

