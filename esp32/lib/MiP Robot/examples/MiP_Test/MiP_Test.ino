/*MiP Test example
Written by Casey Kuhns
SparkFun Electronics

2015
*/
#include "MiP_commands.h"


#if defined(ARDUINO_ARCH_ESP32)
// For ESP32 / ESP32-S3, use a hardware UART connected to the MiP ProMini Pack.
// Set these to the pins you wired to the MiP pack's RX/TX.
#ifndef MIP_RX_PIN
#define MIP_RX_PIN 44
#endif
#ifndef MIP_TX_PIN
#define MIP_TX_PIN 43
#endif

HardwareSerial MiPSerial(1);
// NOTE: The MiP constructor also accepts Stream&, but using HardwareSerial lets init() set baud.
MiP MyMiP(MiPSerial, 2, 3);
#else
MiP MyMiP(2,3);
#endif
void setup(){

#if defined(ARDUINO_ARCH_ESP32)
  // UART1 begin with explicit pins
  MiPSerial.begin(115200, SERIAL_8N1, MIP_RX_PIN, MIP_TX_PIN);
#endif

  MyMiP.init(); //Serial port is configured for 115200
  Serial.print("MiP is Alive!!!");
  delay(100); //Need a delay to let the buffer clear

}



void loop() {

  MyMiP.playSingleSound(BURP);
  delay(100);
  MyMiP.distanceDrive(20, 45);
  loop_LEDs();

  delay(5000);
  

}

void loop_LEDs(){

    MyMiP.setChestLED(255, 0, 0);
    delay(200);
    MyMiP.setChestLED(0, 255, 0);
    delay(200);
    MyMiP.setChestLED(0, 0, 255);
    delay(200);

}
