#include "led_manager.h"

Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

void leds_init() {
  strip.begin();
  strip.setBrightness(NEOPIXEL_BRIGHTNESS);
  strip.clear();
  strip.show();
}

void leds_set_all(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NEOPIXEL_COUNT; ++i) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}

void leds_set_pixel(int idx, uint8_t r, uint8_t g, uint8_t b) {
  if (idx < 0 || idx >= NEOPIXEL_COUNT) return;
  strip.setPixelColor(idx, strip.Color(r, g, b));
}

void leds_show() { strip.show(); }
