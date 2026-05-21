#ifndef MIP_UART_LISTENER_H
#define MIP_UART_LISTENER_H

#include <Arduino.h>

void setupMipUartListener();
void mipUartListenerTask(void* parameter);
void recoverMipUart();
// Optional request helpers.
// These do not run automatically unless you call them.
void requestMipPositionStatus();
void enableMipRadarMode();
void disableMipRadarGestureMode();

#endif