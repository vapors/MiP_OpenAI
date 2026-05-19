#ifndef CONTROL_MODES_H
#define CONTROL_MODES_H

#include <Arduino.h>

/*
  MiP control mode state.

  MODE_MANUAL:
    BLE app controls MiP directly.
    GPT/websocket voice can stay connected, but remote movement commands should be ignored or limited.

  MODE_GPT_ASSISTED:
    BLE app can trigger push-to-talk.
    Local stop/safety still works.
    Server/GPT may eventually send bounded MiP commands.

  MODE_GPT_AUTONOMOUS:
    GPT/server may eventually issue movement commands without push-to-talk.
    ESP32 must still enforce safety limits, IR/collision checks, and emergency stop.
*/
enum ControlMode : uint8_t
{
  MODE_MANUAL = 0,
  MODE_GPT_ASSISTED = 1,
  MODE_GPT_AUTONOMOUS = 2
};

extern ControlMode currentMode;

const char* controlModeToString(ControlMode mode);
ControlMode controlModeFromString(const String& modeName);

bool isManualMode();
bool isGptAssistedMode();
bool isGptAutonomousMode();
bool allowsGptMotion();
bool allowsManualMotion();

void setControlMode(ControlMode mode, bool stopRobot = true);
void applyModeLED();

#endif // CONTROL_MODES_H
