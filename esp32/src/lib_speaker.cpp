#include <Audio.h>
#include <Arduino.h>
#include <driver/i2s.h>
#include <audioMemoryBuffer.h>
#include <math.h>
#include "config.h"
#include "lib_speaker.h"
#include "lib_websocket.h"
#include "lib_button.h"
#include "mic.h"
#include <esp_err.h>


// At the top of the file
static bool is_speaker_installed = false;
static bool is_mic_installed = false;
// Constants
const size_t SAMPLES_PER_WRITE = 1024;
const float TONE_VOLUME_PERCENT = 0.02;
const float MAX_AMPLITUDE = 32767.0f;

// Global state variables
unsigned long lastMicActivity = 0;
unsigned long lastSpkrActivity = 0;
unsigned long lastToneTime = 0;
bool isPlayingTone = false;
unsigned long toneStartTime = 0;
AudioMemoryBuffer audioMemoryBuffer = AudioMemoryBuffer();
Audio audio;
uint8_t speakerdata0[1024 * 1];
int speaker_offset;
int data_offset;

volatile uint8_t g_mouthLevel = 0;

// Default volume
static float s_volume01 = 0.85f;
// Tune these for your voice level
static const uint32_t MOUTH_ENERGY_MAX = 200; // raise if mouth opens too much, lower if barely moves
static const uint8_t  MOUTH_LEVELS     = 10;    // 0..4

void speaker_set_volume(float v01) {
  if (v01 < 0.0f) v01 = 0.0f;
  if (v01 > 1.0f) v01 = 1.0f;
  s_volume01 = v01;
}

// Simple energy→mouth mapping (fast, stable)
static inline uint8_t mouth_from_pcm(const int16_t* s, size_t n)
{
  uint32_t acc = 0;
  // Average absolute amplitude
  for (size_t i = 0; i < n; ++i) acc += (uint32_t)abs(s[i]);
  uint32_t avg = (n > 0) ? (acc / n) : 0;

  // Map 0..MOUTH_ENERGY_MAX → 0..4
  uint32_t lvl = (avg * (MOUTH_LEVELS - 1)) / MOUTH_ENERGY_MAX;
  if (lvl > (MOUTH_LEVELS - 1)) lvl = (MOUTH_LEVELS - 1);
  return (uint8_t)lvl;
}
 
// Scale with clipping (optional, in-place into scratch)
static inline int16_t scale_clip_i16(int16_t x, float v01)
{
  int32_t y = (int32_t)((float)x * v01);
  if (y > 32767) y = 32767;
  if (y < -32768) y = -32768;
  return (int16_t)y;
}

static uint16_t s_env = 0;  // smoothed envelope

static inline uint8_t levelFromPcm(const int16_t* pcm, size_t frames) {
  int32_t peak = 0;
  for (size_t i = 0; i < frames; i++) {
    int32_t a = abs((int32_t)pcm[i]);
    if (a > peak) peak = a;
  }

  // Smooth (attack fast, release slower)
  uint16_t p = (uint16_t)peak;
  if (p > s_env) s_env = (uint16_t)((s_env * 3 + p) / 4);      // attack
  else           s_env = (uint16_t)((s_env * 15 + p) / 16);    // release

  // Map to 0..255 (tune these)
  const int inMin = 100;     // noise floor
  const int inMax = 14000;    // loud speech
  
  int v = (int)s_env;
  v = (v - inMin) * 255 / (inMax - inMin);
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return (uint8_t)v;
}

bool speaker_write_mono_i16(const int16_t* mono, size_t frames, uint32_t timeout_ms)
{
  if (!mono || frames == 0) return false;

  g_mouthLevel = levelFromPcm(mono, frames);

  const size_t CHUNK_FRAMES = 512;
  const bool need_scale = (s_volume01 < 0.999f);

  size_t offset = 0;

  while (offset < frames)
  {
    size_t n = frames - offset;
    if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;

    // Stereo scratch buffer: L/R duplicated from mono.
    int16_t stereo[CHUNK_FRAMES * 2];

    for (size_t i = 0; i < n; ++i)
    {
      int16_t sample = mono[offset + i];

      if (need_scale)
      {
        sample = scale_clip_i16(sample, s_volume01);
      }

      stereo[i * 2]     = sample; // Left
      stereo[i * 2 + 1] = sample; // Right
    }

    size_t bytes_written = 0;
    esp_err_t err = i2s_write(
      I2S_PORT_SPEAKER,
      stereo,
      n * 2 * sizeof(int16_t),
      &bytes_written,
      pdMS_TO_TICKS(timeout_ms)
    );

    if (err != ESP_OK)
    {
      g_mouthLevel = 0;
      Serial.printf("[SPK] i2s_write failed: %s\n", esp_err_to_name(err));
      return false;
    }

    offset += n;
  }

  return true;
}


void writeToAudioBuffer(int16_t *buffer, size_t samples)
{
  // Write samples to both left and right channels
  int16_t stereoBuffer[samples * 2];
  for (size_t i = 0; i < samples; i++)
  {
    stereoBuffer[i * 2] = buffer[i];     // Left channel
    stereoBuffer[i * 2 + 1] = buffer[i]; // Right channel
  }

  size_t bytes_written = 0;
  esp_err_t result = i2s_write(I2S_PORT_SPEAKER, stereoBuffer, samples * 4, &bytes_written, portMAX_DELAY);

  if (result != ESP_OK)
  {
    Serial.println("Error writing to I2S speaker");
  }

  lastSpkrActivity = millis();
}

// I2S configuration helper
esp_err_t configureI2S(const i2s_config_t &config, const i2s_pin_config_t &pins)
{
  esp_err_t result = i2s_driver_install(I2S_PORT_SPEAKER, &config, 0, NULL);
  if (result != ESP_OK)
  {
    Serial.println("Error installing I2S speaker driver");
    return result;
  }

  result = i2s_set_pin(I2S_PORT_SPEAKER, &pins);
  if (result != ESP_OK)
  {
    Serial.println("Error setting I2S speaker pins");
    return result;
  }

  return ESP_OK;
}

void InitI2SSpeakerOrMic(AudioMode mode)
{
    Serial.printf("Initializing I2S for mode: %s\n", mode == MODE_MIC ? "Microphone" : "Speaker");
    esp_err_t err = ESP_OK;

    // Use different ports for mic and speaker
    i2s_port_t port = (mode == MODE_MIC) ? I2S_PORT_MIC : I2S_PORT_SPEAKER;
    
    // Uninstall existing driver for the specific port only
    i2s_driver_uninstall(port);

    // Base I2S config
    i2s_config_t i2s_config = {
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
#if ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(4, 1, 0)
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
#else
        .communication_format = I2S_COMM_FORMAT_I2S,
#endif
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = bufferCnt,
        .dma_buf_len = bufferLen,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    // Mode-specific configurations
    if (mode == MODE_MIC)
    {
        i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
        i2s_config.sample_rate = AUDIO_QUALITY_MIC;
        i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    }
    else
    {
        i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
        i2s_config.sample_rate = AUDIO_QUALITY_SPEAKER;
        i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    }

    // Install I2S driver for the specific port
    err = i2s_driver_install(port, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("Failed to install I2S driver: %s\n", esp_err_to_name(err));
        return;
    }

    // Configure pins
    i2s_pin_config_t pin_config;
    #if (ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(4, 3, 0))
        pin_config.mck_io_num = I2S_PIN_NO_CHANGE;
    #endif

    if (mode == MODE_MIC) {
        pin_config.bck_io_num = I2S_SCK;
        pin_config.ws_io_num = I2S_WS;
        pin_config.data_out_num = I2S_PIN_NO_CHANGE;
        pin_config.data_in_num = I2S_SD;
    } else {
        pin_config.bck_io_num = I2S_SPEAKER_BCLK;
        pin_config.ws_io_num = I2S_SPEAKER_LRC;
        pin_config.data_out_num = I2S_SPEAKER_DIN;
        pin_config.data_in_num = I2S_PIN_NO_CHANGE;
    }

    err = i2s_set_pin(port, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("Failed to set I2S pins: %s\n", esp_err_to_name(err));
        i2s_driver_uninstall(port);
        return;
    }

    // Set clock
    err = i2s_set_clk(port, 
                      (mode == MODE_MIC) ? AUDIO_QUALITY_MIC : AUDIO_QUALITY_SPEAKER,
                      I2S_BITS_PER_SAMPLE_16BIT,
                      (mode == MODE_MIC) ? I2S_CHANNEL_MONO : I2S_CHANNEL_STEREO);
    if (err != ESP_OK) {
        Serial.printf("Failed to set I2S clock: %s\n", esp_err_to_name(err));
        i2s_driver_uninstall(port);
        return;
    }

    // Start I2S
    err = i2s_start(port);
    if (err != ESP_OK) {
        Serial.printf("Failed to start I2S: %s\n", esp_err_to_name(err));
        i2s_driver_uninstall(port);
        return;
    }

    // Update state flags
    if (mode == MODE_MIC) {
        is_mic_installed = true;
        digitalWrite(LED_MIC, HIGH);
    } else {
        is_speaker_installed = true;
        digitalWrite(LED_SPKR, HIGH);
    }

    Serial.println("I2S initialized successfully");
}
esp_err_t setupSpeakerI2S()
{
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = AUDIO_QUALITY_SPEAKER,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = bufferCnt,
      .dma_buf_len = bufferLen,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0};

  i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_SPEAKER_BCLK,
      .ws_io_num = I2S_SPEAKER_LRC,
      .data_out_num = I2S_SPEAKER_DIN,
      .data_in_num = I2S_PIN_NO_CHANGE};

  // Check if I2S port is valid
  if (I2S_PORT_SPEAKER < I2S_NUM_0 || I2S_PORT_SPEAKER >= I2S_NUM_MAX)
  {
    Serial.println("Invalid I2S port");
    return ESP_ERR_INVALID_ARG;
  }

  // Install I2S driver
  esp_err_t i2s_err = i2s_driver_install(I2S_PORT_SPEAKER, &i2s_config, 0, NULL);
  if (i2s_err != ESP_OK)
  {
    Serial.printf("Failed to install I2S driver: %s\n", esp_err_to_name(i2s_err));
    return i2s_err;
  }

  // Set I2S pins
  i2s_err = i2s_set_pin(I2S_PORT_SPEAKER, &pin_config);
  if (i2s_err != ESP_OK)
  {
    Serial.printf("Failed to set I2S pins: %s\n", esp_err_to_name(i2s_err));
    i2s_driver_uninstall(I2S_PORT_SPEAKER);
    return i2s_err;
  }

  Serial.println("I2S initialized successfully");
  return ESP_OK;
}

void generateSimpleTone(int16_t *buffer, size_t samples)
{
  const float amplitude = MAX_AMPLITUDE * TONE_VOLUME_PERCENT;
  const float angular_frequency = 2 * PI * TONE_FREQUENCY;
  static float phase = 0;

  int16_t tempBuffer[samples];
  for (size_t i = 0; i < samples; i++)
  {
    tempBuffer[i] = amplitude * sin(phase);
    phase += angular_frequency / AUDIO_QUALITY_SPEAKER;
    if (phase >= 2 * PI)
    {
      phase -= 2 * PI;
    }
  }

  audioMemoryBuffer.write(tempBuffer, samples);
  audioMemoryBuffer.read(buffer, samples);
}
void generateTone(int16_t *buffer, size_t samples)
{
  const float amplitude = MAX_AMPLITUDE * TONE_VOLUME_PERCENT;
  static float time = 0;
  static float phase = 0;

  // Base frequency modulated by a slow sine wave
  const float base_freq = 440.0f;  // Base frequency in Hz
  const float mod_freq = 0.5f;     // Modulation frequency in Hz
  const float freq_depth = 200.0f; // Frequency deviation in Hz

  int16_t tempBuffer[samples];
  for (size_t i = 0; i < samples; i++)
  {
    // Calculate current frequency using sinusoidal modulation
    float current_freq = base_freq + freq_depth * sin(2 * PI * mod_freq * time);

    tempBuffer[i] = amplitude * sin(phase);
    phase += (2 * PI * current_freq) / AUDIO_QUALITY_SPEAKER;
    if (phase >= 2 * PI)
    {
      phase -= 2 * PI;
    }

    time += 1.0f / AUDIO_QUALITY_SPEAKER;
  }

  audioMemoryBuffer.write(tempBuffer, samples);
  audioMemoryBuffer.read(buffer, samples); // Uncommented this line to properly output the sound
}
void playBufferWithOffset(uint8_t *payload, size_t length)
{
  memcpy(speakerdata0 + speaker_offset, payload, length);
  speaker_offset += length;
  size_t bytes_written;
  i2s_write(I2S_PORT_SPEAKER, speakerdata0, speaker_offset, &bytes_written, portMAX_DELAY);
  speaker_offset = 0;
}

void playBuffer(int16_t *buffer, size_t samples)
{
  size_t bytes_written = 0;
  esp_err_t result = i2s_write(I2S_PORT_SPEAKER, buffer, samples * sizeof(int16_t), &bytes_written, portMAX_DELAY);
  // sendBinaryData(buffer, samples * sizeof(int16_t));

  if (result != ESP_OK)
  {
    Serial.println("Error writing to I2S");
  }
}

void speaker_play(uint8_t *payload, uint32_t len)
{
  if (len < 2) return;
  if (len & 1) len--;                 // ensure PCM16 alignment

  size_t bytes_written = 0;
  i2s_write(I2S_PORT_SPEAKER, payload, len, &bytes_written, portMAX_DELAY);
}

void speaker_play_original(uint8_t *payload, uint32_t len)
{
  const float volume = 0.7f;
  const float pitch = 0.8f; // 1.0 = normal speed, >1 = faster, <1 = slower
  Serial.printf("received %lu bytes", len);
  Serial.println();
  size_t bytes_written;

  // Create a buffer to store modified samples
  int16_t *samples = (int16_t *)payload;
  size_t num_samples = len / sizeof(int16_t);

  // Calculate new buffer size based on pitch
  size_t new_num_samples = (size_t)(num_samples / pitch);
  int16_t *pitched_samples = new int16_t[new_num_samples];

  // Resample audio for pitch/speed adjustment
  for (size_t i = 0; i < new_num_samples; i++)
  {
    float original_index = i * pitch;
    size_t index = (size_t)original_index;
    if (index < num_samples)
    {
      pitched_samples[i] = (int16_t)(samples[index] * volume);
    }
  }

  // InitI2SSpeakerOrMic(MODE_SPK);
  i2s_write(I2S_PORT_SPEAKER, pitched_samples, new_num_samples * sizeof(int16_t),
            &bytes_written, portMAX_DELAY);

  delete[] pitched_samples;

  // After playback completes, switch back to mic mode
  // InitI2SSpeakerOrMic(MODE_MIC);
  Serial.println("Playback complete, switched back to mic mode");
}

void updateToneState()
{
  unsigned long currentTime = millis();

  if (!isPlayingTone && currentTime - lastToneTime >= TONE_INTERVAL)
  {
    isPlayingTone = true;
    toneStartTime = currentTime;
    lastToneTime = currentTime;
    digitalWrite(LED_SPKR, HIGH);
  }

  if (isPlayingTone && currentTime - toneStartTime >= TONE_DURATION)
  {
    isPlayingTone = false;
    digitalWrite(LED_SPKR, LOW);
  }
}
void setupAudio()
{
  // setupSpeakerI2S();  // Call this first
  delay(100);

  audio.setPinout(I2S_SPEAKER_BCLK, I2S_SPEAKER_LRC, I2S_SPEAKER_DIN);
  audio.setVolume(2);
  // audio.
  audio.connecttohost("http://vis.media-ice.musicradio.com/CapitalMP3");
}

void loopAudio()
{
  audio.loop();
}

void setVolume(int volume)
{
  if (volume >= 0 && volume <= 21)
  {
    audio.setVolume(volume);
    Serial.printf("Volume set to %d\n", volume);
  }
  else
  {
    Serial.println("Invalid volume level. Please use a value between 0 and 21.");
  }
}

void playTestTone()
{
  static int16_t tone_buffer[SAMPLES_PER_WRITE];

  updateToneState();

  if (isPlayingTone)
  {
    generateTone(tone_buffer, SAMPLES_PER_WRITE);
    playBuffer(tone_buffer, SAMPLES_PER_WRITE);

    // delayMicroseconds(100);
  }
}

void handleSpeaker()
{
  static int16_t audio_buffer[SAMPLES_PER_WRITE];

  if (audioMemoryBuffer.available() > 0 && audioMemoryBuffer.read(audio_buffer, SAMPLES_PER_WRITE))
  {
    // playBuffer(audio_buffer, SAMPLES_PER_WRITE);
  }
  else
  {
    playTestTone();
  }
  // delay(10);
}
