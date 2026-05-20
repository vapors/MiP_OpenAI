#ifndef MIP_ACTION_QUEUE_H
#define MIP_ACTION_QUEUE_H

#include <Arduino.h>
#include <ArduinoJson.h>

bool setupMipActionQueue();
bool enqueueMipActionFromJson(JsonObjectConst doc);
bool enqueueMipStopAction();
size_t mipActionQueueDepth();

#endif // MIP_ACTION_QUEUE_H
