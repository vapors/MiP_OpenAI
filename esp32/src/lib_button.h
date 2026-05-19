#ifndef LIB_BUTTON_H
#define LIB_BUTTON_H

#include <esp32-hal-gpio.h>
#include "config.h"


class ButtonChecker {
public:
    ButtonChecker() {
        pinMode(BUTTON_PIN, INPUT_PULLUP);
    }

    void begin() {
        lastTickState = false;
        thisTickState = false;
    }

    void loop() {
        lastTickState = thisTickState;
        thisTickState = !digitalRead(BUTTON_PIN);  // Input pullup means pressed = LOW
    }

    bool justPressed() {
        return thisTickState && !lastTickState;
    }

    bool justReleased() {
        return !thisTickState && lastTickState;
    }

    bool isPressed() {
        return thisTickState;
    }

private:
    bool lastTickState;
    bool thisTickState;
};

#endif