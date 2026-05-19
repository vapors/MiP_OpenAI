#pragma once
#include <Adafruit_NeoPixel.h>
#include "config.h"

#ifndef NEOPIXEL_PIN
  #define NEOPIXEL_PIN   -1      // GPIO2 on XIAO ESP32S3
#endif
#ifndef NEOPIXEL_COUNT
  #define NEOPIXEL_COUNT 9
#endif
#ifndef NEOPIXEL_BRIGHTNESS
  #define NEOPIXEL_BRIGHTNESS  32
#endif

// Semantic pixel slots (adjust as you like)

extern Adafruit_NeoPixel strip;

void leds_init();
void leds_set_all(uint8_t r, uint8_t g, uint8_t b);
void leds_set_pixel(int idx, uint8_t r, uint8_t g, uint8_t b);
void leds_show();
