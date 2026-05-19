/*MiP Turn Angle example
Written by 2stacks
2016
*/

#include <MiP_commands.h>
#include <MiP_Parameters.h>
#include <MiP_sounds.h>



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
  delay(3000);

  /*
   * MyMiP.turnAngle(Direction, Speed, Angle);
   *  Direction 
   *    0 = Right
   *    1 = Left
   *  Angle in intervals of 5 degrees (0~255)
   *    18 = 90 degrees
   *    36 = 180 degrees
   *    54 = 270 degrees
   *    72 = 360 degrees
   *    255 = 1275 degrees or 3.5 complete rotations
   *  Speed (0~24)
  */
  

  MyMiP.turnAngle(0, 24, 18);
  delay(3000);
  MyMiP.turnAngle(1, 12, 18);
  delay(3000);
  MyMiP.turnAngle(1, 24, 72);
  delay(3000);
  MyMiP.turnAngle(0, 12, 72);
  delay(3000);
  MyMiP.turnAngle(0, 24, 255);
  delay(3000);
  
}
