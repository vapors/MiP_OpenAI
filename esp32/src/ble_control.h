#ifndef BLE_CONTROL_H
#define BLE_CONTROL_H

#include <Arduino.h>
#include "control_modes.h"

// Custom BLE GATT IDs used by the Web Bluetooth page.
#define MIP_BLE_DEVICE_NAME        "MiP-Bot"
#define MIP_BLE_SERVICE_UUID       "7b4a0001-9f7c-4b2a-8f2d-000000000001"
#define MIP_BLE_COMMAND_CHAR_UUID  "7b4a0002-9f7c-4b2a-8f2d-000000000002"
#define MIP_BLE_STATUS_CHAR_UUID   "7b4a0003-9f7c-4b2a-8f2d-000000000003"

void setupBleControl();
void loopBleControl();

// Main command entry point. This is useful for BLE, Serial testing, or future local UI events.
void handleBleCommand(String cmd);

// Shared push-to-talk helpers used by BLE and the physical button.
void beginPttRecording(const char* source = "unknown");
void endPttRecording(const char* source = "unknown");

// Hard-abort an active/pending PTT session when the WebSocket drops.
// Does not send STOP_RECORD because the server connection is already gone.
void forcePttAbortFromWebSocketDisconnect();

// Status notification helper.
void publishBleStatus(const char* eventName = "status");

#endif // BLE_CONTROL_H
