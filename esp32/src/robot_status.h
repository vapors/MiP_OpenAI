#ifndef ROBOT_STATUS_H
#define ROBOT_STATUS_H

#include <Arduino.h>

// Lightweight robot-state publisher used by BLE, WebSocket, and the MiP action task.
// Battery and IR are placeholders for now; wire the actual sensors/protocol parser later.
void setupRobotStatus();
void loopRobotStatus();


void robotStatusSetMipPosition(uint8_t positionCode, const char* positionName);
void robotStatusSetMipBatteryRaw(uint8_t rawBattery);
void robotStatusSetRadar(uint8_t radarCode, const char* radarName, bool blocked);
void robotStatusSetGesture(uint8_t gestureCode, const char* gestureName);
void robotStatusSetRadarGestureMode(uint8_t modeCode);
void robotStatusSetDetectionStatus(uint8_t id, uint8_t power);
void robotStatusSetMipDetected(uint8_t id);
void robotStatusSetShakeDetected(bool detected);

void setRobotActionState(const char* action);
const char* getRobotActionState();

void setRobotIrBlocked(bool blocked);
bool getRobotIrBlocked();

void setRobotBatteryStatus(int millivolts, int percent);
int getRobotBatteryMillivolts();
int getRobotBatteryPercent();

// Request a status publish from the main loop. Safe to call from worker tasks.
void requestRobotStatePublish(const char* eventName = "status");

// Publishes a JSON robot_state message to the server immediately. Prefer
// requestRobotStatePublish() from worker tasks. If force=false, periodic status
// messages are skipped while recording to avoid competing with mic audio.
void publishRobotState(const char* eventName = "status", bool force = false);

#endif // ROBOT_STATUS_H
