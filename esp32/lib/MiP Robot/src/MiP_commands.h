/*
 * MiP_commands.h - ESP32-friendly MiP command wrapper
 */

#ifndef MIP_COMMANDS_H_
#define MIP_COMMANDS_H_

#include "MiP_sounds.h"
#include "MiP_Parameters.h"
#include <Arduino.h>

struct _LEDColor;

class MiP {
  public:
    MiP(int8_t UART_Select_S, int8_t UART_Select_R);
    MiP(HardwareSerial &serialPort, int8_t UART_Select_S, int8_t UART_Select_R);
    MiP(Stream &stream, int8_t UART_Select_S, int8_t UART_Select_R);
    ~MiP();

    typedef struct _LEDColor
    {
      int red;
      int green;
      int blue;
    } LEDColor;

    void init(uint32_t baud = 115200);

    // Sound
    void playSingleSound(Sounds MiPSound);
    void playSpecific(int8_t soundID);
    void playSoundSequence(const uint8_t* soundIds, uint8_t count, uint8_t delayTicks = 4, uint8_t repeat = 0);
    void stopSound();

    // Position / movement
    void setPosition(SetPosition pose);
    void distanceDrive(int16_t distance, int16_t angle);
    void driveForward(int8_t speed, int8_t time);
    void driveBackward(int8_t speed, int8_t time);
    void turnAngle(int8_t direction, int8_t speed, uint16_t angle);
    void continuousDrive(uint8_t driveCode);
    void stop(void);
    void standUp(int8_t state);

    // Game / behavior modes
    void setGameMode(uint8_t mode);
    void requestGameMode();

    // LEDs
    void requestChestLED(void);
    void setChestLED(uint8_t red, uint8_t green, uint8_t blue);
    void flashChestLED(uint8_t red, uint8_t green, uint8_t blue, uint8_t time_on, uint8_t time_off);
    void setHeadLEDs(uint8_t light1, uint8_t light2, uint8_t light3, uint8_t light4);
    void requestHeadLEDs(void);

    // Status / sensors / utility request commands.
    // Most readbacks are intentionally request-only for now. The ESP32 can later parse responses
    // and forward structured state to the server.
    void requestStatus(void);
    void requestWeightUpdate(void);
    void readOdometer(void);
    void resetOdometer(void);
    void setGestureRadarMode(uint8_t mode);
    void requestRadarMode(void);
    void setDetectionMode(uint8_t id, uint8_t power);
    void requestDetectionMode(void);
    void setIRControl(uint8_t enabled);
    void requestIRControl(void);
    void sendIRCode(uint32_t code, uint8_t bitCount, uint8_t power);
    void setClapDetection(uint8_t enabled);
    void requestClapDetection(void);
    void setClapDelay(uint16_t delayMs);

    void setVolume(int8_t volume);
    int8_t getVolume();

  private:
    Stream* _stream;
    HardwareSerial* _hwSerial;

    int8_t _UART_Select_S;
    int8_t _UART_Select_R;
    void sendMessage(unsigned char *message, uint8_t array_length);
    void getMessage(unsigned char *answer, int byteCount);
    uint8_t answer[8];
    void getData();
};

#endif /* MIP_COMMANDS_H_ */
