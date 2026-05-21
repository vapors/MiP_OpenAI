#include <Arduino.h>
#include <driver/i2s.h>
#include "lib_speaker.h"
#include "lib_websocket.h"
#include "utils.h"
#include "config.h"
#include <esp_task_wdt.h>
#include "esp_check.h"
#include <Wire.h>
#include "es8311.h"
#include "vad_controller.h"
// Global flags for system state
bool isSpeakerBusy = false;
// Legacy flag; use isWebSocketClientConnected() for live transport state.
bool isWebSocketConnected = true;
int16_t soundBuffer[bufferLen];
bool isRecording = false;
#define TAG "MIC"


// Shared init state
static bool g_audio_inited = false;
static es8311_handle_t g_es = nullptr;

// Weak hook (optional): if later you add AXP2101 EXIO6 PA enable, override this.
extern "C" __attribute__((weak)) void ws_pa_enable(bool /*en*/) {}

// ES8311 init copied in spirit from your demo
static esp_err_t es8311_codec_init()
{
  g_es = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  ESP_RETURN_ON_FALSE(g_es, ESP_FAIL, TAG, "es8311 create failed");

  const es8311_clock_config_t es_clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = MCLK_FREQ_HZ,
    .sample_frequency = SAMPLE_RATE_HZ,
  };

  ESP_ERROR_CHECK(es8311_init(g_es, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
  ESP_RETURN_ON_ERROR(es8311_voice_volume_set(g_es, VOICE_VOLUME_0_100, NULL), TAG, "set es8311 volume failed");
  ESP_RETURN_ON_ERROR(es8311_microphone_config(g_es, false), TAG, "set es8311 microphone failed");
  return ESP_OK;
}
static bool i2s_full_duplex_init()
{
  // If something previously installed it, clean up first
  i2s_driver_uninstall(I2S_PORT_MIC);

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE_HZ;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;           // stereo slots
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = true;                                      // important for stable MCLK
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = MCLK_FREQ_HZ;                             // Fs*256, like demo

  esp_err_t err = i2s_driver_install(I2S_PORT_MIC, &cfg, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] i2s_driver_install failed: %d\n", (int)err);
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_MCLK_PIN;
  pins.bck_io_num   = I2S_BCLK_PIN;
  pins.ws_io_num    = I2S_LRCK_PIN;
  pins.data_out_num = I2S_DOUT_PIN;
  pins.data_in_num  = I2S_DIN_PIN;

  err = i2s_set_pin(I2S_PORT_MIC, &pins);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] i2s_set_pin failed: %d\n", (int)err);
    return false;
  }

  i2s_zero_dma_buffer(I2S_PORT_MIC);
  i2s_start(I2S_PORT_MIC);
  return true;
}
// Public init called by your app
bool setupMicrophone()
{
  if (g_audio_inited) return true;

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  esp_err_t e = es8311_codec_init();
  if (e != ESP_OK) {
    Serial.printf("[AUDIO] ES8311 init failed: %d\n", (int)e);
    return false;
  }

  if (!i2s_full_duplex_init()) return false;

  // Enable speaker amp if board requires it (weak hook)
  ws_pa_enable(true);

  g_audio_inited = true;
  Serial.println("[AUDIO] ES8311 + I2S full-duplex initialized");
  return true;
}


void setRecording(bool recording)
{
  isRecording = recording;
}

bool getRecordingState()
{
  return isRecording;
}

void detectSound(int16_t *buffer, size_t length)
{
  if (!buffer || length == 0)
  {
    return;
  }

  for (const auto &lt : ledThresholds)
  {
    bool soundDetected = false;
    int16_t maxAmplitude = 0;

    // Find maximum amplitude in buffer
    for (size_t i = 0; i < length; i++)
    {
      int16_t amplitude = abs(buffer[i]);
      maxAmplitude = max(maxAmplitude, amplitude);

      if (amplitude > lt.threshold)
      {
        soundDetected = true;
        break;
      }
    }

    // Update LED state
    digitalWrite(lt.ledPin, soundDetected ? HIGH : LOW);

    // Only log and send data if sound detected
    if (soundDetected)
    {
      // Serial.print("Peak amplitude: ");
      // Serial.println(maxAmplitude);

      // Send the actual buffer length, not 0
      // sendMessage("Hello");
    }
  }
  // sendBinaryData(buffer, length * sizeof(int16_t));
}


/*
esp_err_t setupMicrophone()
{
  mic_init();
  // i2s_driver_uninstall(I2S_PORT_MIC);
  i2s_config_t i2s_config = {
      .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
      .sample_rate = AUDIO_QUALITY_MIC,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 64,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = MCLK_FREQ_HZ};

  i2s_pin_config_t pin_config = {
      .mck_io_num   = I2S_MCLK_PIN,
      .bck_io_num = I2S_BCLK_PIN,
      .ws_io_num = I2S_LRCK_PIN,
      .data_out_num = I2S_DOUT_PIN,
      .data_in_num = I2S_DIN_PIN
    };

  // Validate I2S port
  if (I2S_PORT_MIC < I2S_NUM_0 || I2S_PORT_MIC >= I2S_NUM_MAX)
  {
    Serial.println("Invalid I2S port number");
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t result = i2s_driver_install(I2S_PORT_MIC, &i2s_config, 0, NULL);
  if (result != ESP_OK)
  {
    Serial.printf("Error installing I2S driver: %s\n", esp_err_to_name(result));
    return result;
  }

  result = i2s_set_pin(I2S_PORT_MIC, &pin_config);
  if (result != ESP_OK)
  {
    Serial.printf("Error setting I2S pins: %s\n", esp_err_to_name(result));
    i2s_driver_uninstall(I2S_PORT_MIC);
    return result;
  }

  Serial.println("I2S microphone initialized successfully");
  return ESP_OK;
}
*/

/*
esp_err_t handleMicrophone()
{
  size_t bytes_read = 0;
  const size_t bufferSize = bufferLen;
  int16_t *buffer = (int16_t *)audio_malloc(bufferSize * sizeof(int16_t));

  if (!buffer)
  {
    Serial.println("Failed to allocate memory for audio buffer");
    return ESP_ERR_NO_MEM;
  }

  esp_err_t result = i2s_read(I2S_PORT_MIC, buffer, bufferSize * sizeof(int16_t), &bytes_read, portMAX_DELAY);
  if (result == ESP_OK && bytes_read > 0)
  {
    detectSound(buffer, bytes_read / sizeof(int16_t));
  }

  free(buffer);
  return result;
}
*/


// Read mic as MONO int16 frames (we read stereo slots and take one channel)
size_t handleMicrophone(int16_t* out_mono, size_t frames, uint32_t timeout_ms)
{
  if (!g_audio_inited) return 0;

  // stereo interleaved: L,R,L,R...
  const size_t stereo_samples = frames * 2;
  static int16_t* tmp = nullptr;
  static size_t tmp_cap = 0;

  if (tmp_cap < stereo_samples) {
    free(tmp);
    tmp = (int16_t*)malloc(stereo_samples * sizeof(int16_t));
    tmp_cap = stereo_samples;
    if (!tmp) return 0;
  }

  size_t bytes_read = 0;
  esp_err_t err = i2s_read(I2S_PORT_MIC,
                           tmp,
                           stereo_samples * sizeof(int16_t),
                           &bytes_read,
                           pdMS_TO_TICKS(timeout_ms));

  if (err != ESP_OK || bytes_read == 0) return 0;

  const size_t got_stereo_samples = bytes_read / sizeof(int16_t);
  const size_t got_frames = got_stereo_samples / 2;

  // Take Right channel by default
  for (size_t i = 0; i < got_frames; i++) {
    out_mono[i] = tmp[i * 2 + 1];
  }

  return got_frames;
}

/*
void micTask(void *parameter)
{
  while (true)
  {
    if (!isRecording)
    {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Read MONO frames from ES8311 (RIGHT channel) via handleMicrophone()
    size_t gotFrames = handleMicrophone(soundBuffer, bufferLen, 50);
    if (gotFrames > 0)
    {
      detectSound(soundBuffer, gotFrames);

      if (isWebSocketConnected)
      {
        // Send PCM16 mono bytes
        size_t bytesOut = gotFrames * sizeof(int16_t);
        sendBinaryData(soundBuffer, bytesOut);
      }
    }
    else
    {
      // read timeout/no data; keep loop light
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    esp_task_wdt_reset();
    taskYIELD();
  }
}
*/
/*
void micTask(void *parameter) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1));
        
        if (isRecording) {
            size_t bytesIn = 0;
            esp_err_t result = i2s_read(I2S_PORT_MIC, &soundBuffer, bufferLen, &bytesIn, portMAX_DELAY);
            
            if (result == ESP_OK) {
                detectSound(soundBuffer, bytesIn / sizeof(int16_t));
                if (isWebSocketConnected) {
                    sendBinaryData(soundBuffer, bytesIn);
                }
            } else {
                Serial.printf("I2S read error: %d\n", result);
                delay(100);  // Add delay on error
            }
            
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
*/
void micTask(void *parameter)
{
  while (true)
  {
    // In normal modes, only read the mic while recording. In VAD mode, read
    // continuously so the local RMS detector can trigger hands-free recording.
    if (!isRecording && !vadIsListening())
    {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Read clean MONO PCM16 frames from ES8311.
    size_t gotFrames = handleMicrophone(soundBuffer, bufferLen, 50);

    if (gotFrames > 0)
    {
      detectSound(soundBuffer, gotFrames);

      // VAD observes all mic frames in GPT_VAD mode. It only starts/stops the
      // existing PTT state machine; it does not change the audio upload format.
      vadProcessFrames(soundBuffer, gotFrames);

      if (isRecording && isWebSocketClientConnected())
      {
        size_t bytesOut = gotFrames * sizeof(int16_t);
        sendBinaryData(soundBuffer, bytesOut);
      }
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    esp_task_wdt_reset();
    taskYIELD();
  }
}
