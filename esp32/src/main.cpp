#include <Arduino.h>
#include <HardwareSerial.h>

#include <Audio.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <driver/i2s.h>
#include <math.h>

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <LittleFS.h>
#include "FS.h"
#include "TCA9554.h"

//debug
#include "esp_heap_caps.h"
#include <esp_system.h>
#include <esp_mac.h>

#include "mic.h"
#include "config.h"
#include "lib_wifi.h"
#include "utils.h"
#include "lib_speaker.h"
#include "lib_button.h"
#include "lib_websocket.h"
#include "face_task.h"
#include "control_modes.h"
#include "ble_control.h"
#include "MiP_commands.h"
#include "mip_action_queue.h"
#include "mip_uart_listener.h"
#include "robot_status.h"
#include "vad_controller.h"
#include "speaker_audio_queue.h"
#include "robot_body_state.h"
/*
old task pinning and priorities for reference:
speakerAudioTask -> core 1, priority 2
websocketTxTask  -> core 1, priority 2
micTask          -> core 1, priority 1
mipActionTask    -> core 1, priority 1
mipUartRx        -> core 1, priority 1
Arduino loop     -> core 1
faceTask         -> core 0, priority 2

*/
int16_t sBuffer[bufferLen];
ButtonChecker button;

// Function declarations
//Additional uart
#define RX1_PIN 45
#define TX1_PIN 46 
//#define RX2_PIN 47
//#define TX2_PIN 48
// ---- Waveshare LCD + IO expander ----
#define SPI_MISO 2
#define SPI_MOSI 1
#define SPI_SCLK 5

#define LCD_CS  -1
#define LCD_DC  3
#define LCD_RST -1
#define LCD_HOR_RES 320
#define LCD_VER_RES 480

#define GFX_BL 6


#define MIP_RX_PIN 47
#define MIP_TX_PIN 48

#define ENABLE_FACE_IDLE_BEHAVIOR 0

HardwareSerial MiPSerial(1);
MiP MyMiP(MiPSerial, 2, 3);

unsigned long lastMipKeepAliveMs = 0;
const unsigned long MIP_KEEP_ALIVE_INTERVAL_MS = 10000;


TCA9554 TCA(0x20);
void initLCD();
bool initFS();
void setupAudioIO();
void setupLEDs();

Arduino_DataBus *bus = new Arduino_ESP32SPI(
  LCD_DC /* DC */, LCD_CS /* CS */,
  SPI_SCLK /* SCK */, SPI_MOSI /* MOSI */, SPI_MISO /* MISO */
);

Arduino_GFX *gfx = new Arduino_ST7796(
  bus, LCD_RST /* RST */, 2 /* rotation */, true, LCD_HOR_RES, LCD_VER_RES
);


static const char* resetReasonName(esp_reset_reason_t reason)
{
  switch (reason)
  {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC_EXCEPTION";
    case ESP_RST_INT_WDT:   return "INTERRUPT_WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK_WATCHDOG";
    case ESP_RST_WDT:       return "OTHER_WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

void printResetInfo()
{
  esp_reset_reason_t reason = esp_reset_reason();

  Serial.printf("[RESET] reason=%d %s\n", (int)reason, resetReasonName(reason));
  Serial.printf("[HEAP] free=%u largest=%u psramFree=%u\n",
                ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                ESP.getFreePsram());
}

bool initFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount failed");
    return false;
  }
  Serial.println("[FS] LittleFS mounted");
  Serial.printf("[FS] bg=%d mouth00=%d\n",
                LittleFS.exists("/bg.png"),
                LittleFS.exists("/mouth00.png"));
  return true;
}


static void lcd_reset(){
  TCA.write1(1, 1);
  delay(10);
  TCA.write1(1, 0);
  delay(10);
  TCA.write1(1, 1);
  delay(200);
}

void initLCD(){
  // --- I2C for TCA + ES8311 ---
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  // --- LCD reset via TCA9554 ---
  TCA.begin();
  TCA.pinMode1(1, OUTPUT);
  lcd_reset();
  // --- LCD init ---
  if (!gfx->begin()) {
    Serial.println("[LCD] gfx->begin() failed!");
  }
  gfx->fillScreen(RGB565_BLACK);
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
}

void setupAudioIO(){
  setupMicrophone();
    delay(400);  // Increased delay for better initialization
  Serial.printf(
  "[AUDIO CFG] Fs=%d Hz | bits=%d | MCLK=%d Hz | MCLK/Fs=%d\n",
  SAMPLE_RATE_HZ,
  16,
  MCLK_FREQ_HZ,
  MCLK_FREQ_HZ / SAMPLE_RATE_HZ
  );
  // i2s_start(I2S_PORT_MIC);
  delay(400);  // Increased delay for better initialization
}

void setupLEDs(){
  pinMode(LED_MIC, OUTPUT);
  pinMode(LED_SPKR, OUTPUT);
  digitalWrite(LED_MIC, LOW);
  digitalWrite(LED_SPKR, LOW);
}

void printPsramInfo() {
  bool ok = psramInit();
  Serial.printf("[PSRAM] psramInit=%d psramFound=%d size=%u free=%u\n",
                ok,
                psramFound(),
                ESP.getPsramSize(),
                ESP.getFreePsram());

  Serial.printf("[HEAP] freeHeap=%u largest=%u\n",
                ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}


void keepMipAwake()
{
  unsigned long now = millis();

  if (now - lastMipKeepAliveMs < MIP_KEEP_ALIVE_INTERVAL_MS)
  {
    return;
  }
  lastMipKeepAliveMs = now;
  // Gentle keep-alive: refresh the current mode LED.
  // This avoids moving the robot but still sends a command to the MiP body.
  //applyModeLED();
  //Serial.println("[MIP] Keep-alive LED refresh");
}


void setup()
{
  Serial.begin(115200);
  delay(3000); // Give time for serial monitor to connect
  //printPsramInfo();
  printResetInfo();
  //Serial.println("ESP32 Serial (UART0) initialized");
  MiPSerial.begin(115200, SERIAL_8N1, MIP_RX_PIN, MIP_TX_PIN);
  MyMiP.init(); // Serial port is configured for 115200



  Serial.println("MiP is Alive!!!");
  delay(200); // Need a delay to let the MiP serial buffer clear.
      MyMiP.setChestLED(255, 0, 0);
    delay(200);
          MyMiP.setChestLED(255, 0, 0);
    delay(200);
          MyMiP.setChestLED(0, 255, 0);
    delay(200);
          MyMiP.setChestLED(0, 0, 255);
    delay(200);


  // Start status, action, and manual BLE-control systems.
  setupRobotStatus();
  setupRobotBodyState();
  setupMipActionQueue();
  setupVadController();
  setupBleControl();
  setControlMode(MODE_MANUAL, false);
  setupMipUartListener();
  // Enable passive radar events from MiP.
  //enableMipRadarMode();

  delay(200);

  // --- init filesystem ---
  initFS();
  delay(100);

  // --- LCD init ---
  initLCD();
  delay(300);

Serial.printf("[HEAP] internal free=%u\n", ESP.getFreeHeap());



startFaceTask();
delay(500);

face_enable_idle_behavior(ENABLE_FACE_IDLE_BEHAVIOR);

if (!ENABLE_FACE_IDLE_BEHAVIOR)
{
  face_enable_auto_blink(true);
  face_set_eye_anim(FaceAnim::EyeAnimId::Blink, FaceAnim::PlayMode::Loop, 28);
}

  connectToWiFi();
  connectToWebSocket();
  delay(500);  
//face_eyes_blink();          // back to normal auto-blink loop
// delay(2000); 
// face_eyes_look_left_hold();  // plays once, then holds
// delay(4000);
// face_eyes_look_right_hold(); // plays once, then holds
// delay(4000);
//face_eyes_blink_slowly();
//  delay(4000);
 //face_eyes_sleep();          // hold closed (sleep)
 //delay(2000); 
// delay(2000); 
//face_set_eye_anim(FaceAnim::EyeAnimId::LookLeft, FaceAnim::PlayMode::PingPong, 30);
//delay(2000);
//face_eyes_blink();    
  // --- Audio ---

  setRecording(false);
  setupAudioIO();
  setupSpeakerAudioQueue();
  xTaskCreatePinnedToCore(micTask, "micTask", 16000, NULL, 2, NULL, 1);
}

void loop()
{
  button.loop();
  if (button.justPressed())
  {
    // Use the same push-to-talk path as BLE so recording timing,
    // minimum duration, and STOP_RECORD flush order stay consistent.
    beginPttRecording("button");
  }
  else if (button.justReleased())
  {
    endPttRecording("button");
  }
  loopRobotBodyState();
  loopRobotStatusPublisher();
  loopBleControl();
  loopWebsocket();
  loopRobotStatus();
//  keepMipAwake(); 
}