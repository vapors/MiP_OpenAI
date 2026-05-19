/*
 * MiP_commands.cpp - ESP32-friendly MiP command wrapper
 */

#include "MiP_commands.h"
#include "MiP_Parameters.h"

MiP::MiP(int8_t UART_Select_S, int8_t UART_Select_R)
: MiP((Stream&)Serial, UART_Select_S, UART_Select_R) {}

MiP::MiP(HardwareSerial &serialPort, int8_t UART_Select_S, int8_t UART_Select_R)
: _stream(&serialPort), _hwSerial(&serialPort) {
  _UART_Select_S = UART_Select_S;
  _UART_Select_R = UART_Select_R;
  if (_UART_Select_S >= 0) pinMode(_UART_Select_S, OUTPUT);
  if (_UART_Select_R >= 0) pinMode(_UART_Select_R, OUTPUT);
  if (_UART_Select_S >= 0) digitalWrite(_UART_Select_S, LOW);
  if (_UART_Select_R >= 0) digitalWrite(_UART_Select_R, LOW);
}

MiP::MiP(Stream &stream, int8_t UART_Select_S, int8_t UART_Select_R)
: _stream(&stream), _hwSerial(nullptr) {
  _UART_Select_S = UART_Select_S;
  _UART_Select_R = UART_Select_R;
  if (_UART_Select_S >= 0) pinMode(_UART_Select_S, OUTPUT);
  if (_UART_Select_R >= 0) pinMode(_UART_Select_R, OUTPUT);
  if (_UART_Select_S >= 0) digitalWrite(_UART_Select_S, LOW);
  if (_UART_Select_R >= 0) digitalWrite(_UART_Select_R, LOW);
}

MiP::~MiP() {}

void MiP::init(uint32_t baud){
  if (_hwSerial) {
    _hwSerial->begin(baud);
  }
  uint8_t data_array[] = {'T','T','M',':','O','K','\r','\n'};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::playSingleSound(Sounds MiPSound){
  playSpecific((int8_t)MiPSound);
}

void MiP::playSpecific(int8_t soundID){
  uint8_t data_array[4];
  data_array[0] = 0x06;
  data_array[1] = (uint8_t)soundID;
  data_array[2] = 0;
  data_array[3] = 0;
  sendMessage(data_array, sizeof(data_array));
}

void MiP::playSoundSequence(const uint8_t* soundIds, uint8_t count, uint8_t delayTicks, uint8_t repeat){
  if (!soundIds || count == 0) return;
  if (count > 8) count = 8;

  uint8_t data_array[1 + 8 * 2 + 1];
  uint8_t idx = 0;
  data_array[idx++] = 0x06;
  for (uint8_t i = 0; i < count; i++) {
    data_array[idx++] = soundIds[i];
    data_array[idx++] = delayTicks;
  }
  data_array[idx++] = repeat;
  sendMessage(data_array, idx);
}

void MiP::stopSound(){
  playSpecific(105);
}

void MiP::setPosition(SetPosition pose){
  uint8_t data_array[2];
  data_array[0] = 0x08;
  data_array[1] = pose;
  sendMessage(data_array, sizeof(data_array));
}

void MiP::driveForward(int8_t speed, int8_t time){
  if (speed < 0) speed = -speed;
  if (speed > 30) speed = 30;
  if (time < 0) time = -time;
  uint8_t data_array[3];
  data_array[0] = 0x71;
  data_array[1] = (uint8_t)speed;
  data_array[2] = (uint8_t)time;
  sendMessage(data_array, sizeof(data_array));
}

void MiP::driveBackward(int8_t speed, int8_t time){
  if (speed < 0) speed = -speed;
  if (speed > 30) speed = 30;
  if (time < 0) time = -time;
  uint8_t data_array[3];
  data_array[0] = 0x72;
  data_array[1] = (uint8_t)speed;
  data_array[2] = (uint8_t)time;
  sendMessage(data_array, sizeof(data_array));
}

void MiP::distanceDrive(int16_t distance_cm, int16_t angle_deg) {
  uint8_t data_array[6];
  data_array[0] = 0x70;

  uint16_t cm = (distance_cm >= 0) ? (uint16_t)distance_cm : (uint16_t)(-distance_cm);
  if (cm > 255) cm = 255;
  data_array[1] = (distance_cm >= 0) ? 0x00 : 0x01;
  data_array[2] = (uint8_t)cm;

  uint16_t deg = (angle_deg >= 0) ? (uint16_t)angle_deg : (uint16_t)(-angle_deg);
  if (deg > 360) deg = 360;
  data_array[3] = (angle_deg > 0) ? 0x00 : 0x01;
  data_array[4] = (uint8_t)(deg >> 8);
  data_array[5] = (uint8_t)(deg & 0xFF);
  sendMessage(data_array, sizeof(data_array));
}

void MiP::turnAngle(int8_t direction, int8_t speed, uint16_t angle_deg) {
  uint8_t data_array[3];
  data_array[0] = (direction == 0) ? 0x73 : 0x74;
  if (angle_deg > 1275) angle_deg = 1275;
  uint8_t angle_units = (uint8_t)(angle_deg / 5);
  if (speed < 0) speed = -speed;
  if (speed > 24) speed = 24;
  data_array[1] = angle_units;
  data_array[2] = (uint8_t)speed;
  sendMessage(data_array, sizeof(data_array));
}

void MiP::continuousDrive(uint8_t driveCode){
  uint8_t data_array[2];
  data_array[0] = 0x78;
  data_array[1] = driveCode;
  sendMessage(data_array, sizeof(data_array));
}

void MiP::stop(void){
  uint8_t data_array[1];
  data_array[0] = 0x77;
  sendMessage(data_array, sizeof(data_array));
}

void MiP::setGameMode(uint8_t mode){
  uint8_t data_array[2];
  data_array[0] = 0x76;
  data_array[1] = mode;
  sendMessage(data_array, sizeof(data_array));
}

void MiP::requestGameMode(){
  uint8_t data_array[1] = {0x82};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::standUp(int8_t state){
  uint8_t data_array[2];
  data_array[0] = 0x23;
  data_array[1] = (uint8_t)state;
  sendMessage(data_array, sizeof(data_array));
}

void MiP::requestChestLED(void){
  uint8_t data_array[1] = {0x83};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::setChestLED(uint8_t red, uint8_t green, uint8_t blue){
  uint8_t data_array[4];
  data_array[0] = 0x84;
  data_array[1] = red;
  data_array[2] = green;
  data_array[3] = blue;
  sendMessage(data_array, sizeof(data_array));
}

void MiP::flashChestLED(uint8_t red, uint8_t green, uint8_t blue, uint8_t time_on, uint8_t time_off){
  uint8_t data_array[6];
  data_array[0] = 0x89;
  data_array[1] = red;
  data_array[2] = green;
  data_array[3] = blue;
  data_array[4] = time_on;
  data_array[5] = time_off;
  sendMessage(data_array, sizeof(data_array));
}

void MiP::setHeadLEDs(uint8_t light1, uint8_t light2, uint8_t light3, uint8_t light4){
  uint8_t data_array[5];
  data_array[0] = 0x8A;
  data_array[1] = light1;
  data_array[2] = light2;
  data_array[3] = light3;
  data_array[4] = light4;
  sendMessage(data_array, sizeof(data_array));
}

void MiP::requestHeadLEDs(void){
  uint8_t data_array[1] = {0x8B};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::requestStatus(void){
  uint8_t data_array[1] = {0x79};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::requestWeightUpdate(void){
  uint8_t data_array[1] = {0x81};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::readOdometer(void){
  uint8_t data_array[1] = {0x85};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::resetOdometer(void){
  uint8_t data_array[1] = {0x86};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::setGestureRadarMode(uint8_t mode){
  uint8_t data_array[2] = {0x0C, mode};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::requestRadarMode(void){
  uint8_t data_array[1] = {0x0D};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::setDetectionMode(uint8_t id, uint8_t power){
  uint8_t data_array[3] = {0x0E, id, power};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::requestDetectionMode(void){
  uint8_t data_array[1] = {0x0F};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::setIRControl(uint8_t enabled){
  uint8_t data_array[2] = {0x10, enabled ? (uint8_t)1 : (uint8_t)0};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::requestIRControl(void){
  uint8_t data_array[1] = {0x11};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::sendIRCode(uint32_t code, uint8_t bitCount, uint8_t power){
  uint8_t data_array[7];
  data_array[0] = 0x8C;
  data_array[1] = (uint8_t)((code >> 24) & 0xFF);
  data_array[2] = (uint8_t)((code >> 16) & 0xFF);
  data_array[3] = (uint8_t)((code >> 8) & 0xFF);
  data_array[4] = (uint8_t)(code & 0xFF);
  data_array[5] = bitCount;
  data_array[6] = power;
  sendMessage(data_array, sizeof(data_array));
}

void MiP::setClapDetection(uint8_t enabled){
  uint8_t data_array[2] = {0x1E, enabled ? (uint8_t)1 : (uint8_t)0};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::requestClapDetection(void){
  uint8_t data_array[1] = {0x1F};
  sendMessage(data_array, sizeof(data_array));
}

void MiP::setClapDelay(uint16_t delayMs){
  uint8_t data_array[3];
  data_array[0] = 0x20;
  data_array[1] = (uint8_t)(delayMs >> 8);
  data_array[2] = (uint8_t)(delayMs & 0xFF);
  sendMessage(data_array, sizeof(data_array));
}

void MiP::setVolume(int8_t volume){
  if (volume < 0) volume = 0;
  if (volume > 7) volume = 7;
  uint8_t data_array[2];
  data_array[0] = 0x15;
  data_array[1] = (uint8_t)volume;
  sendMessage(data_array, sizeof(data_array));
}

int8_t MiP::getVolume(){
  int answerVal = -1;
  uint8_t answer[4];
  uint8_t question[] = {GET_VOLUME};
  int iteration = 0;
  while ((0 > answerVal || answerVal > 7) && iteration < MAX_RETRIES) {
    sendMessage(question, 1);
    delay(10);
    if (_stream->available() >= 4) {
      getMessage(answer, 4);
      if (answer[0] - 48 == 1 && answer[1] - 48 == 6 && answer[2] - 48 == 0)
        answerVal = answer[3] - 48;
    } else {
      while (_stream->available()) _stream->read();
    }
    iteration++;
  }
  if (0 > answerVal || answerVal > 7) return -1;
  return answerVal;
}

void MiP::sendMessage(unsigned char *message, uint8_t array_length){
  if (_UART_Select_S >= 0) digitalWrite(_UART_Select_S, HIGH);
  delay(5);
  for(uint8_t i = 0; i < array_length; i++){
    _stream->write(message[i]);
  }
  _stream->write((uint8_t)0x00);
  delay(5);
  if (_UART_Select_S >= 0) digitalWrite(_UART_Select_S, LOW);
}

void MiP::getMessage(unsigned char *answer, int byteCount) {
  if (_UART_Select_R >= 0) digitalWrite(_UART_Select_R, HIGH);
  for (int i = 0; i < byteCount; i++) {
    answer[i] = _stream->read();
  }
  if (_UART_Select_R >= 0) digitalWrite(_UART_Select_R, LOW);
}
