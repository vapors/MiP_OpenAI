#include <esp32-hal-gpio.h>
#include "config.h"
class ButtonChecker {
public:
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

private:
    bool lastTickState = false;
    bool thisTickState = false;
};