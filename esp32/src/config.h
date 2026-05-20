#ifndef CONFIG_H
#define CONFIG_H

// WiFi credentials

const int WEBSOCKET_PORT = 8888;
extern const char* WIFI_SSID;
extern const char* WIFI_PASSWORD;
extern const char* WEBSOCKET_HOST;

// Board pins I2c 
#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 7
// Board pins I2c 
#define I2C_SDA 8
#define I2C_SCL 7
// I2S Microphone pins
#define I2S_SD 14  // Serial Data
#define I2S_WS 15  // Word Select (LRCLK)
#define I2S_SCK 13 // Serial Clock
// Speaker pins
#define I2S_SPEAKER_BCLK 13 // Bit Clock
#define I2S_SPEAKER_LRC 15  // Left Right Clock (Word Select)
#define I2S_SPEAKER_DIN 16  // Data Input
#define I2S_MCLK  12 // Master Clock (MCLK)
// I2S Microphone configuration
// #define SAMPLE_RATE 44100
#define SAMPLE_BITS 32
#define CHANNELS 1

#define I2S_PORT_MIC I2S_NUM_0
#define I2S_PORT_SPEAKER I2S_NUM_0

// Buffer configuration
#define bufferCnt 10
// Audio detection thresholds
#define MIC_THRESHOLD 2300 // Adjust based on testing
#define LED_DELAY 1        // ms to keep LED on after sound stops

// Test tone configuration
#define TONE_FREQUENCY 440 // Hz (A4 note)
#define TONE_DURATION 2000 // ms
#define TONE_INTERVAL 5000 // ms
// LED pins
#define LED_MIC 39   // RED LED for microphone activity
#define LED_SPKR 40 // BLUE LED for speaker activity
// Button pin
#define BUTTON_PIN 21



//duplicate definitions for migration
// ---- Board pins (match your working demo) ----

#define I2S_MCLK_PIN  12
#define I2S_BCLK_PIN  13
#define I2S_LRCK_PIN  15

// IMPORTANT: match your working demo
#define I2S_DOUT_PIN  16 // ESP -> ES8311 (playback)
#define I2S_DIN_PIN   14 // ES8311 -> ESP (mic)


// ---- Audio format ----


#define SAMPLE_RATE 16000
#define SAMPLE_RATE_HZ  16000
#define bufferLen 512
#define SAMPLES_PER_BUFFER 512


//#define SAMPLE_RATE_HZ  24000
//#define BITS_PER_SAMPLE  16
#define MCLK_MULTIPLE  256
#define MCLK_FREQ_HZ  (SAMPLE_RATE_HZ * MCLK_MULTIPLE)
#define VOICE_VOLUME_0_100  75




struct LedThreshold
{
    int ledPin;
    int threshold;
};

const LedThreshold ledThresholds[] = {
    {LED_MIC, 100},
    // {LED_SPKR, 200},
};

enum AudioQuality
{
  LOW_DEFINITION = 16000,
  OPENAI_DEFINITION = 22050,
  MID_DEFINITION = 24000,
  HIGH_DEFINITION = 44100,
  ULTRA_HIGH_DEFINITION = 96000
};

const AudioQuality AUDIO_QUALITY_SPEAKER = AudioQuality::MID_DEFINITION;
const AudioQuality AUDIO_QUALITY = AudioQuality::MID_DEFINITION;
const AudioQuality AUDIO_QUALITY_MIC = AudioQuality::MID_DEFINITION;


#endif