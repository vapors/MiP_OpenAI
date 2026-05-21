#ifndef ROBOT_BODY_STATE_H
#define ROBOT_BODY_STATE_H

#include <Arduino.h>

// Generic body tracking. Today the only supported body is MiP, but this keeps
// the "brain" side ready for future UART-attached robot bodies.
enum RobotBodyType : uint8_t
{
  ROBOT_BODY_NONE = 0,
  ROBOT_BODY_MIP = 1,
};

enum RobotBodyConnectionState : uint8_t
{
  ROBOT_BODY_UNKNOWN = 0,
  ROBOT_BODY_NOT_PRESENT,
  ROBOT_BODY_DETECTED,
  ROBOT_BODY_READY,
  ROBOT_BODY_LOST,
};

void setupRobotBodyState();
void loopRobotBodyState();

// Safe to call from UART listener tasks. Keep this lightweight.
void notifyRobotBodyPacketSeen(RobotBodyType type);

RobotBodyType getRobotBodyType();
RobotBodyConnectionState getRobotBodyConnectionState();

const char* robotBodyTypeToString(RobotBodyType type);
const char* robotBodyStateToString(RobotBodyConnectionState state);

bool robotBodyConnected();
uint32_t robotBodyLastRxAgeMs();

#endif // ROBOT_BODY_STATE_H
