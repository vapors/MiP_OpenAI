#include "face_task.h"
#include <Arduino.h>

// Your display is created in main.cpp as a global:
// Arduino_GFX *gfx = new Arduino_ST7796(...);
extern Arduino_GFX* gfx;

// Mouth level comes from your speaker pipeline (lib_speaker.cpp)
extern volatile uint8_t g_mouthLevel;

static TaskHandle_t s_faceTaskHandle = nullptr;

static FaceRenderer s_face;

// Simple command queue so main/websocket/audio threads can trigger animations safely
enum CmdType : uint8_t { CMD_SET_EYE_ANIM = 1, CMD_SET_EYE_FRAME = 2, CMD_AUTOBLINK = 3 };

struct FaceCmd {
  uint8_t type;
  uint8_t a;       // id or frame or enable
  uint8_t b;       // mode
  uint16_t fps;    // fps
  int8_t hold;     // hold frame
};

static QueueHandle_t s_cmdQ = nullptr;

static void faceTask(void* arg) {
  (void)arg;

  // Renderer expects LittleFS + LCD already initialized in main()
  s_face.begin(gfx);

  // Anchor tuning (these are the “center-ish” defaults)
  s_face.setEyesAnchor(240, 110, 195, 90);
  s_face.setMouthAnchor(240, 210, 160, 90);

  Serial.println("[FACE] Preloading PNGs into PSRAM...");
  if (!s_face.preloadAll()) {
    Serial.println("[FACE] preloadAll failed");
    vTaskDelete(nullptr);
    return;
  }

  // Render loop timing (smooth but light)
  const TickType_t frameDelay = pdMS_TO_TICKS(16); // ~60Hz
  while (true) {
    // Drain commands (non-blocking)
    FaceCmd cmd;
    while (s_cmdQ && xQueueReceive(s_cmdQ, &cmd, 0) == pdTRUE) {
      switch (cmd.type) {
        case CMD_SET_EYE_ANIM:
          s_face.setEyeAnim((FaceAnim::EyeAnimId)cmd.a, (FaceAnim::PlayMode)cmd.b, cmd.fps, cmd.hold);
          break;
        case CMD_SET_EYE_FRAME:
          s_face.setEyeFrame(cmd.a);
          break;
        case CMD_AUTOBLINK:
          s_face.enableAutoBlink(cmd.a != 0);
          break;
        default:
          break;
      }
    }

    // Tick render
    const uint8_t mouth = (uint8_t)g_mouthLevel; // copy once
    s_face.tick(mouth);

    vTaskDelay(frameDelay);
  }
}

void startFaceTask() {
  if (!s_cmdQ) {
    s_cmdQ = xQueueCreate(8, sizeof(FaceCmd));
  }
  if (s_faceTaskHandle) return;
  xTaskCreatePinnedToCore(faceTask, "faceTask", 8192, nullptr, 1, &s_faceTaskHandle, 0);
}

// --- Control API (queue to face task) ---

void face_set_eye_anim(FaceAnim::EyeAnimId id, FaceAnim::PlayMode mode, uint16_t fps, int8_t hold_frame) {
  if (!s_cmdQ) return;
  FaceCmd c{};
  c.type = CMD_SET_EYE_ANIM;
  c.a = (uint8_t)id;
  c.b = (uint8_t)mode;
  c.fps = fps;
  c.hold = hold_frame;
  xQueueSend(s_cmdQ, &c, 0);
}

void face_set_eye_frame(uint8_t frame) {
  if (!s_cmdQ) return;
  FaceCmd c{};
  c.type = CMD_SET_EYE_FRAME;
  c.a = frame;
  xQueueSend(s_cmdQ, &c, 0);
}

void face_enable_auto_blink(bool en) {
  if (!s_cmdQ) return;
  FaceCmd c{};
  c.type = CMD_AUTOBLINK;
  c.a = en ? 1 : 0;
  xQueueSend(s_cmdQ, &c, 0);
}
