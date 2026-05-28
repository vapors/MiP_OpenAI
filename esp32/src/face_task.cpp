#include "face_task.h"
#include <Arduino.h>
#include <string.h>

// Your display is created in main.cpp as a global:
// Arduino_GFX *gfx = new Arduino_ST7796(...);
extern Arduino_GFX* gfx;

// Mouth level comes from your speaker pipeline (lib_speaker.cpp)
extern volatile uint8_t g_mouthLevel;

static TaskHandle_t s_faceTaskHandle = nullptr;

static FaceRenderer s_face;

// Simple command queue so main/websocket/audio threads can trigger animations safely
enum CmdType : uint8_t {
  CMD_SET_EYE_ANIM = 1,
  CMD_SET_EYE_FRAME = 2,
  CMD_AUTOBLINK = 3,
  CMD_SET_EXPRESSION = 4,
  CMD_CLEAR_EXPRESSION = 5,
  CMD_IDLE_ENABLE = 6,
  CMD_RANDOM_IDLE_REACTION = 7,
};

struct FaceCmd {
  uint8_t type;
  uint8_t a;       // id or frame or enable
  uint8_t b;       // mode
  uint16_t fps;    // fps
  int8_t hold;     // hold frame
  uint32_t duration_ms;
  char name[20];
};

static QueueHandle_t s_cmdQ = nullptr;

// -----------------------------------------------------------------------------
// Local idle face behavior
// -----------------------------------------------------------------------------

static const uint32_t FACE_IDLE_MIN_DELAY_MS = 2500;
static const uint32_t FACE_IDLE_MAX_DELAY_MS = 8500;
static const uint32_t FACE_IDLE_LOOK_MIN_MS = 700;
static const uint32_t FACE_IDLE_LOOK_MAX_MS = 1800;
static const uint32_t FACE_IDLE_EXPR_MIN_MS = 900;
static const uint32_t FACE_IDLE_EXPR_MAX_MS = 2800;
static const uint32_t FACE_MANUAL_DEFAULT_HOLD_MS = 2200;
static const uint32_t FACE_BLINK_ONESHOT_HOLD_MS = 450;
static const uint8_t  FACE_SPEAKING_MOUTH_THRESHOLD = 2;

static bool s_idleBehaviorEnabled = true;
static bool s_idleActionActive = false;
static uint32_t s_idleActionUntilMs = 0;
static uint32_t s_nextIdleFaceMs = 0;

// Used for user/agent commands that hold a static pose, such as look_left_hold
// or a timed expression. When it expires, the face returns to neutral/open eyes
// with auto blink enabled.
static uint32_t s_manualReturnToIdleAtMs = 0;

static bool expressionNameToId(const char* name, FaceAnim::ExpressionId& out)
{
  if (!name) return false;

  if (strcmp(name, "bashful") == 0)  { out = FaceAnim::ExpressionId::Bashful; return true; }
  if (strcmp(name, "confused") == 0) { out = FaceAnim::ExpressionId::Confused; return true; }
  if (strcmp(name, "silly") == 0)    { out = FaceAnim::ExpressionId::Silly; return true; }
  if (strcmp(name, "surprise") == 0 || strcmp(name, "surprised") == 0) {
    out = FaceAnim::ExpressionId::Surprise;
    return true;
  }
  if (strcmp(name, "thinking") == 0) { out = FaceAnim::ExpressionId::Thinking; return true; }

  return false;
}

static void scheduleNextIdleFace(uint32_t now)
{
  s_nextIdleFaceMs = now + (uint32_t)random(FACE_IDLE_MIN_DELAY_MS, FACE_IDLE_MAX_DELAY_MS + 1);
}

static void returnToNeutralIdleFace(uint32_t now)
{
  s_idleActionActive = false;
  s_idleActionUntilMs = 0;
  s_manualReturnToIdleAtMs = 0;

  s_face.clearExpression();
  s_face.enableAutoBlink(true);

  // Hold open eyes as the neutral base. Auto blink can still fire on top.
  s_face.setEyeAnim(FaceAnim::EyeAnimId::Open, FaceAnim::PlayMode::OnceHold, 1, 0);

  scheduleNextIdleFace(now);
}

static uint32_t estimateManualEyeHoldMs(FaceAnim::EyeAnimId id, FaceAnim::PlayMode mode)
{
  if (id == FaceAnim::EyeAnimId::Closed) {
    // Sleep/closed-eyes should remain until another command wakes the face.
    return 0;
  }

  if (id == FaceAnim::EyeAnimId::Blink &&
      (mode == FaceAnim::PlayMode::Once || mode == FaceAnim::PlayMode::OnceHold || mode == FaceAnim::PlayMode::PingPong)) {
    return FACE_BLINK_ONESHOT_HOLD_MS;
  }

  if ((id == FaceAnim::EyeAnimId::LookLeft || id == FaceAnim::EyeAnimId::LookRight) &&
      (mode == FaceAnim::PlayMode::Once || mode == FaceAnim::PlayMode::OnceHold)) {
    return FACE_MANUAL_DEFAULT_HOLD_MS;
  }

  return 0;
}

static void startIdleReaction(uint32_t now)
{
  // The local idle system should only make small, temporary choices.
  // Agent/system commands still take priority through s_manualReturnToIdleAtMs.
  s_idleActionActive = true;

  const int choice = random(0, 10);

  switch (choice)
  {
    case 0:
      s_face.clearExpression();
      s_face.enableAutoBlink(false);
      s_face.setEyeAnim(FaceAnim::EyeAnimId::LookLeft, FaceAnim::PlayMode::OnceHold, 24, 5);
      s_idleActionUntilMs = now + (uint32_t)random(FACE_IDLE_LOOK_MIN_MS, FACE_IDLE_LOOK_MAX_MS + 1);
      break;

    case 1:
      s_face.clearExpression();
      s_face.enableAutoBlink(false);
      s_face.setEyeAnim(FaceAnim::EyeAnimId::LookRight, FaceAnim::PlayMode::OnceHold, 24, 5);
      s_idleActionUntilMs = now + (uint32_t)random(FACE_IDLE_LOOK_MIN_MS, FACE_IDLE_LOOK_MAX_MS + 1);
      break;

    case 2: {
      FaceAnim::ExpressionId id = FaceAnim::ExpressionId::Thinking;
      s_face.enableAutoBlink(false);
      s_face.setExpression(id, (uint32_t)random(FACE_IDLE_EXPR_MIN_MS, FACE_IDLE_EXPR_MAX_MS + 1));
      s_idleActionUntilMs = now + (uint32_t)random(FACE_IDLE_EXPR_MIN_MS, FACE_IDLE_EXPR_MAX_MS + 1);
    } break;

    case 3: {
      FaceAnim::ExpressionId id = FaceAnim::ExpressionId::Confused;
      s_face.enableAutoBlink(false);
      s_face.setExpression(id, (uint32_t)random(FACE_IDLE_EXPR_MIN_MS, FACE_IDLE_EXPR_MAX_MS + 1));
      s_idleActionUntilMs = now + (uint32_t)random(FACE_IDLE_EXPR_MIN_MS, FACE_IDLE_EXPR_MAX_MS + 1);
    } break;

    case 4: {
      FaceAnim::ExpressionId id = FaceAnim::ExpressionId::Bashful;
      s_face.enableAutoBlink(false);
      s_face.setExpression(id, (uint32_t)random(1000, 2200));
      s_idleActionUntilMs = now + (uint32_t)random(1000, 2200);
    } break;

    case 5: {
      FaceAnim::ExpressionId id = FaceAnim::ExpressionId::Silly;
      s_face.enableAutoBlink(false);
      s_face.setExpression(id, (uint32_t)random(700, 1600));
      s_idleActionUntilMs = now + (uint32_t)random(700, 1600);
    } break;

    case 6: {
      FaceAnim::ExpressionId id = FaceAnim::ExpressionId::Surprise;
      s_face.enableAutoBlink(false);
      s_face.setExpression(id, (uint32_t)random(450, 1100));
      s_idleActionUntilMs = now + (uint32_t)random(450, 1100);
    } break;

    case 7:
      // A subtle blink-like micro reaction.
      s_face.clearExpression();
      s_face.enableAutoBlink(false);
      s_face.setEyeAnim(FaceAnim::EyeAnimId::Blink, FaceAnim::PlayMode::OnceHold, 30, 0);
      s_idleActionUntilMs = now + FACE_BLINK_ONESHOT_HOLD_MS;
      break;

    default:
      // Sometimes do nothing. This keeps the face from feeling too busy.
      s_idleActionActive = false;
      scheduleNextIdleFace(now);
      break;
  }
}

static void serviceIdleFace(uint32_t now, uint8_t mouth)
{
  const bool speaking = mouth > FACE_SPEAKING_MOUTH_THRESHOLD;

  // Manual/agent command timed hold wins over autonomous idle.
  if (s_manualReturnToIdleAtMs != 0)
  {
    if (now >= s_manualReturnToIdleAtMs)
    {
      returnToNeutralIdleFace(now);
    }
    return;
  }

  // End local idle reaction and return to neutral/open eyes + auto blink.
  if (s_idleActionActive)
  {
    if (now >= s_idleActionUntilMs)
    {
      returnToNeutralIdleFace(now);
    }
    return;
  }

  if (!s_idleBehaviorEnabled || speaking)
  {
    return;
  }

  if (s_nextIdleFaceMs == 0)
  {
    scheduleNextIdleFace(now);
    return;
  }

  if (now >= s_nextIdleFaceMs)
  {
    startIdleReaction(now);
  }
}

static void faceTask(void* arg) {
  (void)arg;

  // Renderer expects LittleFS + LCD already initialized in main()
  s_face.begin(gfx);

  // Anchor tuning (these are the “center-ish” defaults) //image,
  s_face.setEyesAnchor(240, 95, 202, 45);
  s_face.setMouthAnchor(157, 210, 85, 10);

  Serial.println("[FACE] Preloading PNGs into PSRAM...");
  if (!s_face.preloadAll()) {
    Serial.println("[FACE] preloadAll failed");
    vTaskDelete(nullptr);
    return;
  }

  randomSeed((uint32_t)esp_random());
  returnToNeutralIdleFace(millis());

  // Render loop timing (smooth but light)
  const TickType_t frameDelay = pdMS_TO_TICKS(16); // ~60Hz
  while (true) {
    const uint32_t now = millis();

    // Drain commands (non-blocking)
    FaceCmd cmd;
    while (s_cmdQ && xQueueReceive(s_cmdQ, &cmd, 0) == pdTRUE) {
      switch (cmd.type) {
        case CMD_SET_EYE_ANIM: {
          const FaceAnim::EyeAnimId id = (FaceAnim::EyeAnimId)cmd.a;
          const FaceAnim::PlayMode mode = (FaceAnim::PlayMode)cmd.b;

          // Agent/manual eye commands interrupt local idle actions.
          s_idleActionActive = false;
          s_idleActionUntilMs = 0;

          s_face.setEyeAnim(id, mode, cmd.fps, cmd.hold);

          const uint32_t holdMs = estimateManualEyeHoldMs(id, mode);
          s_manualReturnToIdleAtMs = holdMs ? (millis() + holdMs) : 0;
        } break;

        case CMD_SET_EYE_FRAME:
          s_idleActionActive = false;
          s_face.setEyeFrame(cmd.a);
          s_manualReturnToIdleAtMs = millis() + FACE_MANUAL_DEFAULT_HOLD_MS;
          break;

        case CMD_AUTOBLINK:
          s_face.enableAutoBlink(cmd.a != 0);
          if (cmd.a != 0) {
            scheduleNextIdleFace(millis());
          }
          break;

        case CMD_SET_EXPRESSION: {
          FaceAnim::ExpressionId id;
          if (expressionNameToId(cmd.name, id)) {
            s_idleActionActive = false;
            s_face.enableAutoBlink(false);
            s_face.setExpression(id, cmd.duration_ms);
            s_manualReturnToIdleAtMs = cmd.duration_ms ? (millis() + cmd.duration_ms + 150) : 0;
          } else {
            Serial.printf("[FACE] Unknown expression: %s\n", cmd.name);
          }
        } break;

        case CMD_CLEAR_EXPRESSION:
          returnToNeutralIdleFace(millis());
          break;

        case CMD_IDLE_ENABLE:
          s_idleBehaviorEnabled = (cmd.a != 0);
          if (s_idleBehaviorEnabled) {
            scheduleNextIdleFace(millis());
          } else {
            s_idleActionActive = false;
            s_idleActionUntilMs = 0;
          }
          break;

        case CMD_RANDOM_IDLE_REACTION:
          s_manualReturnToIdleAtMs = 0;
          s_idleActionActive = false;
          startIdleReaction(millis());
          break;

        default:
          break;
      }
    }

    // Tick render
    const uint8_t mouth = (uint8_t)g_mouthLevel; // copy once
    serviceIdleFace(now, mouth);
    s_face.tick(mouth);

    vTaskDelay(frameDelay);
  }
}

void startFaceTask() {
  if (!s_cmdQ) {
    s_cmdQ = xQueueCreate(10, sizeof(FaceCmd));
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

void face_enable_idle_behavior(bool en) {
  if (!s_cmdQ) return;
  FaceCmd c{};
  c.type = CMD_IDLE_ENABLE;
  c.a = en ? 1 : 0;
  xQueueSend(s_cmdQ, &c, 0);
}

void face_trigger_random_idle_reaction() {
  if (!s_cmdQ) return;
  FaceCmd c{};
  c.type = CMD_RANDOM_IDLE_REACTION;
  xQueueSend(s_cmdQ, &c, 0);
}

void face_set_expression(const char* expression, uint32_t duration_ms) {
  if (!s_cmdQ || !expression) return;
  FaceCmd c{};
  c.type = CMD_SET_EXPRESSION;
  c.duration_ms = duration_ms;
  strncpy(c.name, expression, sizeof(c.name) - 1);
  c.name[sizeof(c.name) - 1] = '\0';
  xQueueSend(s_cmdQ, &c, 0);
}

void face_clear_expression() {
  if (!s_cmdQ) return;
  FaceCmd c{};
  c.type = CMD_CLEAR_EXPRESSION;
  xQueueSend(s_cmdQ, &c, 0);
}

void face_blink_once(uint16_t fps) {
  face_enable_auto_blink(false);
  face_set_eye_anim(FaceAnim::EyeAnimId::Blink, FaceAnim::PlayMode::OnceHold, fps, 0);
}

void face_look_center() {
  face_clear_expression();
}
