#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <PNGdec.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

// Cached sprite in PSRAM:
// - rgb565: w*h pixels
// - mask: 1-bit packed alpha mask, stride = (w+7)/8 bytes per row
//   (This matches Arduino_GFX::draw16bitRGBBitmapWithMask expectations.)
struct CachedSprite {
  int w = 0, h = 0;
  int maskStride = 0;          // bytes per row of mask (packed bits)
  uint16_t* rgb565 = nullptr;  // w*h
  uint8_t*  mask   = nullptr;  // maskStride*h

  bool ok() const { return rgb565 && mask && w > 0 && h > 0 && maskStride > 0; }
};

namespace FaceAnim {
  enum class PlayMode : uint8_t {
    Loop = 0,       // loop forward 0..N-1..0..N-1...
    Once = 1,       // play forward once then stop on last frame
    OnceHold = 2,   // play forward once then stop on hold_frame (or last if hold_frame < 0)
    PingPong = 3,   // 0..N-1..0..N-1...
    Manual = 4      // don't auto-advance; you set frame explicitly
  };

  enum class EyeAnimId : uint8_t {
    Blink = 0,
    LookLeft = 1,
    LookRight = 2,
    // Convenience "poses" (use Blink frames):
    Open = 3,
    Closed = 4,
  };

  enum class ExpressionId : uint8_t {
    Bashful = 0,
    Confused = 1,
    Silly = 2,
    Surprise = 3,
    Thinking = 4,
  };
}

class FaceRenderer {
public:
  bool begin(Arduino_GFX* display) {
    _gfx = display;
    return _gfx != nullptr;
  }

  // Anchor placement:
  // screenX/screenY = where the anchor lands on the screen
  // anchorX/anchorY = where that anchor point is in the sprite
  void setMouthAnchor(int screenX, int screenY, int anchorX, int anchorY) {
    _mouthScreenX = screenX; _mouthScreenY = screenY;
    _mouthAnchorX = anchorX; _mouthAnchorY = anchorY;
  }
  void setEyesAnchor(int screenX, int screenY, int anchorX, int anchorY) {
    _eyesScreenX = screenX; _eyesScreenY = screenY;
    _eyesAnchorX = anchorX; _eyesAnchorY = anchorY;
  }

  // Call once after LittleFS + LCD initialized
  bool preloadAll() {
    if (!_gfx) {
      Serial.println("[FACE] preloadAll: _gfx is null");
      return false;
    }

    // Background
    if (!loadPNG_RGB565("/bg.png", _bg, _bgW, _bgH)) {
      Serial.println("[FACE] Failed to load /bg.png");
      return false;
    }
    Serial.printf("[FACE] bg loaded %dx%d\n", _bgW, _bgH);

    // Mouth frames mouth00..mouth10
    for (int i = 0; i < MOUTH_FRAMES; ++i) {
      char path[32];
      snprintf(path, sizeof(path), "/mouth%02d.png", i);
      if (!loadPNG_Sprite(path, _mouth[i])) {
        Serial.printf("[FACE] Failed to load %s\n", path);
        return false;
      }
    }
    Serial.printf("[FACE] mouth sprites loaded (%d)\n", MOUTH_FRAMES);

    // Eyes: blink0..blink5
    for (int i = 0; i < EYE_FRAMES; ++i) {
      char path[32];
      snprintf(path, sizeof(path), "/eyes_blink%d.png", i);
      if (!loadPNG_Sprite(path, _eyesBlink[i])) {
        Serial.printf("[FACE] Failed to load %s\n", path);
        return false;
      }
    }
    Serial.printf("[FACE] eye blink sprites loaded (%d)\n", EYE_FRAMES);

    // Eyes: look_left00..05
    for (int i = 0; i < LOOK_FRAMES; ++i) {
      char path[32];
      snprintf(path, sizeof(path), "/look_left%02d.png", i);
      if (!loadPNG_Sprite(path, _eyesLeft[i])) {
        Serial.printf("[FACE] Failed to load %s\n", path);
        return false;
      }
    }
    Serial.printf("[FACE] eye look_left sprites loaded (%d)\n", LOOK_FRAMES);

    // Eyes: look_right00..05
    for (int i = 0; i < LOOK_FRAMES; ++i) {
      char path[32];
      snprintf(path, sizeof(path), "/look_right%02d.png", i);
      if (!loadPNG_Sprite(path, _eyesRight[i])) {
        Serial.printf("[FACE] Failed to load %s\n", path);
        return false;
      }
    }
    Serial.printf("[FACE] eye look_right sprites loaded (%d)\n", LOOK_FRAMES);

    const char* expEyePaths[EXP_FRAMES] = {
      "/exp_eyes_bashful00.png",
      "/exp_eyes_confused00.png",
      "/exp_eyes_silly00.png",
      "/exp_eyes_surprise00.png",
      "/exp_eyes_thinking00.png",
    };

    const char* expMouthPaths[EXP_FRAMES] = {
      // Note: current uploaded filename uses bashfull with two l's.
      "/exp_mouth_bashfull00.png",
      "/exp_mouth_confused00.png",
      "/exp_mouth_silly00.png",
      "/exp_mouth_surprise00.png",
      "/exp_mouth_thinking00.png",
    };

    for (int i = 0; i < EXP_FRAMES; ++i) {
      if (!loadPNG_Sprite(expEyePaths[i], _expEyes[i])) {
        Serial.printf("[FACE] Failed to load %s\n", expEyePaths[i]);
        return false;
      }
      if (!loadPNG_Sprite(expMouthPaths[i], _expMouth[i])) {
        Serial.printf("[FACE] Failed to load %s\n", expMouthPaths[i]);
        return false;
      }
    }
    Serial.printf("[FACE] expression sprites loaded (%d)\n", EXP_FRAMES);

    // First draw (eyes first, then mouth on top)
    drawBackgroundFull();
    drawEyesFrameRaw(_eyesBlink[0]);  // open
    drawMouthFrame(0);

    // Default: blink loop, with auto-blink FSM enabled
    setEyeAnim(FaceAnim::EyeAnimId::Blink, FaceAnim::PlayMode::Loop, 28 /*fps*/);
    _lastMouth = 0;

    randomSeed(esp_random());
    _nextBlinkDelayMs = 2500 + random(0, 2500);
    _lastBlinkMs = millis();

    Serial.println("[FACE] Ready (render loop)");
    return true;
  }

  // --- Public control API ---
  void setEyeAnim(FaceAnim::EyeAnimId id,
                  FaceAnim::PlayMode mode,
                  uint16_t fps = 28,
                  int8_t hold_frame = -1)
  {
    _eyeState.id = id;
    _eyeState.mode = mode;
    _eyeState.hold_frame = hold_frame;
    _eyeState.fps = (fps == 0) ? 1 : fps;
    _eyeState.frame = 0;
    _eyeState.dir = +1;
    _eyeState.playing = (mode != FaceAnim::PlayMode::Manual);
    _eyeState.lastFrameMs = millis();
    _eyeState.framePeriodMs = (uint16_t)max<uint32_t>(1, 1000UL / _eyeState.fps);

    // Immediate visual feedback
    drawEyesNow();
  }

  void setEyeFrame(uint8_t frame) {
    _eyeState.frame = frame;
    drawEyesNow();
  }

  void enableAutoBlink(bool en) { _autoBlinkEnabled = en; }

  void setExpression(FaceAnim::ExpressionId id, uint32_t duration_ms = 2500) {
    const uint8_t idx = (uint8_t)id;
    if (idx >= EXP_FRAMES) return;

    _expressionActive = true;
    _expressionId = idx;
    _expressionUntilMs = duration_ms > 0 ? millis() + duration_ms : 0;
    _expressionMouthVisible = false;

    // Expression eyes are held as a static pose; normal mouth animation can still
    // override the expression mouth while assistant audio is speaking.
    _autoBlinkEnabled = false;
    _eyeState.playing = false;
    drawEyesFrameRaw(_expEyes[_expressionId]);
    drawExpressionMouth();
  }

  void clearExpression() {
    _expressionActive = false;
    _expressionUntilMs = 0;
    _expressionMouthVisible = false;
    _autoBlinkEnabled = true;
    setEyeAnim(FaceAnim::EyeAnimId::Open, FaceAnim::PlayMode::OnceHold, 1, 0);
    drawMouthFrame(0);
    _lastMouth = 0;
  }

  // Call at ~30–60Hz from face_task.
  // mouthLevel0to255 typically derived from speaker audio envelope.
  void tick(uint8_t mouthLevel0to255) {
    const uint32_t now = millis();

    if (_expressionActive && _expressionUntilMs != 0 && (int32_t)(now - _expressionUntilMs) >= 0) {
      clearExpression();
    }

    if (!_expressionActive) {
      tickEyes();
    }

    // Draw mouth last so it stays "in front" if regions overlap.
    // During an expression, speaker audio mouth frames temporarily override the
    // static expression mouth. When speaking stops, return to the expression mouth.
    const bool speaking = mouthLevel0to255 > 8;

    if (_expressionActive && !speaking) {
      if (!_expressionMouthVisible) {
        drawExpressionMouth();
      }
      return;
    }

    const uint8_t m = (uint8_t)min<int>(MOUTH_FRAMES - 1, (mouthLevel0to255 * MOUTH_FRAMES) / 256);
    if (m != _lastMouth || _expressionMouthVisible) {
      drawMouthFrame(m);
      _lastMouth = m;
      _expressionMouthVisible = false;
    }
  }

  void drawBackgroundFull() {
    if (!_gfx || !_bg) return;
    _gfx->draw16bitRGBBitmap(0, 0, _bg, _bgW, _bgH);
  }

private:
  static constexpr int EYE_FRAMES   = 6;   // eyes_blink0..5
  static constexpr int LOOK_FRAMES  = 6;   // look_left00..05, look_right00..05
  static constexpr int MOUTH_FRAMES = 11;  // mouth00..mouth10
  static constexpr int EXP_FRAMES   = 5;   // bashful, confused, silly, surprise, thinking

  Arduino_GFX* _gfx = nullptr;

  // Background cache
  uint16_t* _bg = nullptr;
  int _bgW = 0, _bgH = 0;

  // Sprites
  CachedSprite _mouth[MOUTH_FRAMES];
  CachedSprite _eyesBlink[EYE_FRAMES];
  CachedSprite _eyesLeft[LOOK_FRAMES];
  CachedSprite _eyesRight[LOOK_FRAMES];
  CachedSprite _expEyes[EXP_FRAMES];
  CachedSprite _expMouth[EXP_FRAMES];

  // Anchor defaults (tweak in face_task)
  int _eyesScreenX  = 240, _eyesScreenY  = 202;
  int _eyesAnchorX  = 120, _eyesAnchorY  = 90;

  int _mouthScreenX = 240, _mouthScreenY = 211;
  int _mouthAnchorX = 150, _mouthAnchorY = 90;

  uint8_t _lastMouth = 255;

  bool _expressionActive = false;
  uint8_t _expressionId = 0;
  uint32_t _expressionUntilMs = 0;
  bool _expressionMouthVisible = false;

  // --- Eye animation state ---
  struct EyeAnimState {
    FaceAnim::EyeAnimId id = FaceAnim::EyeAnimId::Blink;
    FaceAnim::PlayMode mode = FaceAnim::PlayMode::Loop;
    uint8_t frame = 0;
    int8_t dir = +1;
    bool playing = true;

    uint16_t fps = 28;
    uint16_t framePeriodMs = 35;
    uint32_t lastFrameMs = 0;

    int8_t hold_frame = -1;
  } _eyeState;

  // Auto-blink FSM (only used when id==Blink and enabled)
  bool _autoBlinkEnabled = true;
  bool _blinking = false;
  uint8_t _blinkFrame = 0;
  uint32_t _lastBlinkMs = 0;
  uint32_t _lastBlinkFrameMs = 0;
  int blinktime = rand()%4000+2000;
  uint32_t _nextBlinkDelayMs = blinktime;

  // --- PNGdec plumbing ---
  PNG _png;
  File _file;
  static FaceRenderer* s_active;

  // Decode target
  uint16_t* _dst565 = nullptr;
  uint8_t*  _dstMask = nullptr;
  int _dstW = 0;
  int _dstMaskStride = 0;

  // --- Callbacks ---
  static void* openCB(const char* filename, int32_t* size) {
    if (!s_active) { *size = 0; return nullptr; }

    s_active->_file = LittleFS.open(filename, "r");
    if (!s_active->_file && filename && filename[0] == '/') {
      s_active->_file = LittleFS.open(filename + 1, "r");
    }
    if (!s_active->_file && filename && filename[0] != '/') {
      String alt = String("/") + filename;
      s_active->_file = LittleFS.open(alt.c_str(), "r");
    }

    if (!s_active->_file || s_active->_file.isDirectory()) {
      Serial.printf("[PNG] open fail: '%s'\n", filename);
      *size = 0;
      return nullptr;
    }

    *size = (int32_t)s_active->_file.size();
    return &s_active->_file;
  }

  static void closeCB(void* handle) {
    (void)handle;
    if (s_active && s_active->_file) s_active->_file.close();
  }

  static int32_t readCB(PNGFILE* handle, uint8_t* buffer, int32_t length) {
    (void)handle;
    if (!s_active || !s_active->_file) return 0;
    return (int32_t)s_active->_file.read(buffer, length);
  }

  static int32_t seekCB(PNGFILE* handle, int32_t position) {
    (void)handle;
    if (!s_active || !s_active->_file) return 0;
    return (int32_t)s_active->_file.seek(position);
  }

  // Newer PNGdec expects int return (OK=1)
  static int drawCB(PNGDRAW* pDraw) {
    if (!s_active || !s_active->_dst565) return 0;

    static uint16_t line565[480];
    static uint8_t  lineMaskPacked[64];

    s_active->_png.getLineAsRGB565(pDraw, line565, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);

    const int y = pDraw->y;
    const int w = pDraw->iWidth;

    // Copy RGB row
    uint16_t* row = s_active->_dst565 + (y * s_active->_dstW);
    memcpy(row, line565, (size_t)w * sizeof(uint16_t));

    // Copy packed 1-bit alpha row
    if (s_active->_dstMask) {
      const int stride = s_active->_dstMaskStride; // (dstW+7)/8
      const int packedLen = (w + 7) / 8;

      s_active->_png.getAlphaMask(pDraw, lineMaskPacked, 1);

      uint8_t* mrow = s_active->_dstMask + (y * stride);
      memcpy(mrow, lineMaskPacked, (size_t)packedLen);
    }
    return 1;
  }

  // --- Loaders ---
  bool loadPNG_RGB565(const char* path, uint16_t*& out565, int& outW, int& outH) {
    s_active = this;

    int rc = _png.open(path, openCB, closeCB, readCB, seekCB, drawCB);
    if (rc != PNG_SUCCESS) { s_active = nullptr; return false; }

    outW = _png.getWidth();
    outH = _png.getHeight();

    size_t bytes = (size_t)outW * (size_t)outH * sizeof(uint16_t);
    out565 = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out565) {
      Serial.println("[PNG] PSRAM alloc failed for bg");
      _png.close();
      s_active = nullptr;
      return false;
    }

    _dst565 = out565;
    _dstW = outW;

    _dstMask = nullptr;
    _dstMaskStride = 0;

    rc = _png.decode(nullptr, 0);
    _png.close();
    s_active = nullptr;
    return (rc == PNG_SUCCESS);
  }

  bool loadPNG_Sprite(const char* path, CachedSprite& s) {
    s_active = this;

    int rc = _png.open(path, openCB, closeCB, readCB, seekCB, drawCB);
    if (rc != PNG_SUCCESS) { s_active = nullptr; return false; }

    s.w = _png.getWidth();
    s.h = _png.getHeight();
    s.maskStride = (s.w + 7) / 8;

    size_t rgbBytes  = (size_t)s.w * (size_t)s.h * sizeof(uint16_t);
    size_t maskBytes = (size_t)s.maskStride * (size_t)s.h;

    s.rgb565 = (uint16_t*)heap_caps_malloc(rgbBytes,  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.mask   = (uint8_t*) heap_caps_malloc(maskBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!s.rgb565 || !s.mask) {
      Serial.printf("[PNG] PSRAM alloc failed for sprite %s (rgb=%p mask=%p)\n", path, s.rgb565, s.mask);
      _png.close();
      s_active = nullptr;
      return false;
    }

    _dst565 = s.rgb565;
    _dstMask = s.mask;
    _dstW = s.w;
    _dstMaskStride = s.maskStride;

    rc = _png.decode(nullptr, 0);
    _png.close();
    s_active = nullptr;
    return (rc == PNG_SUCCESS);
  }

  // --- Drawing helpers ---
  int eyesRectX()  const { return _eyesScreenX  - _eyesAnchorX; }
  int eyesRectY()  const { return _eyesScreenY  - _eyesAnchorY; }
  int mouthRectX() const { return _mouthScreenX - _mouthAnchorX; }
  int mouthRectY() const { return _mouthScreenY - _mouthAnchorY; }

  void drawMouthFrame(uint8_t idx) {
    if (!_gfx || idx >= MOUTH_FRAMES || !_mouth[idx].ok()) return;
    _gfx->draw16bitRGBBitmapWithMask(
      mouthRectX(), mouthRectY(),
      _mouth[idx].rgb565,
      _mouth[idx].mask,
      _mouth[idx].w,
      _mouth[idx].h
    );
  }

  void drawExpressionMouth() {
    if (!_gfx || _expressionId >= EXP_FRAMES || !_expMouth[_expressionId].ok()) return;
    _gfx->draw16bitRGBBitmapWithMask(
      mouthRectX(), mouthRectY(),
      _expMouth[_expressionId].rgb565,
      _expMouth[_expressionId].mask,
      _expMouth[_expressionId].w,
      _expMouth[_expressionId].h
    );
    _expressionMouthVisible = true;
    _lastMouth = 255;
  }

  void drawEyesFrameRaw(const CachedSprite& s) {
    if (!_gfx || !s.ok()) return;
    _gfx->draw16bitRGBBitmapWithMask(
      eyesRectX(), eyesRectY(),
      s.rgb565,
      s.mask,
      s.w,
      s.h
    );
  }

  void drawEyesNow() {
    switch (_eyeState.id) {
      case FaceAnim::EyeAnimId::Open:
        drawEyesFrameRaw(_eyesBlink[0]);
        return;
      case FaceAnim::EyeAnimId::Closed:
        drawEyesFrameRaw(_eyesBlink[EYE_FRAMES - 1]);
        return;
      case FaceAnim::EyeAnimId::Blink: {
        uint8_t f = min<uint8_t>(_eyeState.frame, (uint8_t)(EYE_FRAMES - 1));
        drawEyesFrameRaw(_eyesBlink[f]);
        return;
      }
      case FaceAnim::EyeAnimId::LookLeft: {
        uint8_t f = min<uint8_t>(_eyeState.frame, (uint8_t)(LOOK_FRAMES - 1));
        drawEyesFrameRaw(_eyesLeft[f]);
        return;
      }
      case FaceAnim::EyeAnimId::LookRight: {
        uint8_t f = min<uint8_t>(_eyeState.frame, (uint8_t)(LOOK_FRAMES - 1));
        drawEyesFrameRaw(_eyesRight[f]);
        return;
      }
      default:
        drawEyesFrameRaw(_eyesBlink[0]);
        return;
    }
  }

  uint8_t eyeFrameCount() const {
    switch (_eyeState.id) {
      case FaceAnim::EyeAnimId::Blink:     return EYE_FRAMES;
      case FaceAnim::EyeAnimId::LookLeft:  return LOOK_FRAMES;
      case FaceAnim::EyeAnimId::LookRight: return LOOK_FRAMES;
      case FaceAnim::EyeAnimId::Open:      return 1;
      case FaceAnim::EyeAnimId::Closed:    return 1;
      default:                             return EYE_FRAMES;
    }
  }

  void tickEyes() {
    const uint32_t now = millis();

    // Special case: classic auto-blink FSM when Blink + enabled
    if (_eyeState.id == FaceAnim::EyeAnimId::Blink && _autoBlinkEnabled) {
      if (!_blinking) {
        if (now - _lastBlinkMs > _nextBlinkDelayMs) {
          _blinking = true;
          _blinkFrame = 0;
          _lastBlinkFrameMs = now;
        }
      } else {
        if (now - _lastBlinkFrameMs >= 35) { // ~28fps
          drawEyesFrameRaw(_eyesBlink[_blinkFrame]);
          _blinkFrame++;
          _lastBlinkFrameMs = now;

          if (_blinkFrame >= EYE_FRAMES) {
            drawEyesFrameRaw(_eyesBlink[0]); // open
            _blinking = false;
            _lastBlinkMs = now;
            _nextBlinkDelayMs = 2500 + random(0, 2500);
          }
        }
      }
      return;
    }

    // Generic animation
    if (_eyeState.mode == FaceAnim::PlayMode::Manual) return;
    if (!_eyeState.playing) return;
    if (now - _eyeState.lastFrameMs < _eyeState.framePeriodMs) return;

    const uint8_t count = eyeFrameCount();
    if (count <= 1) {
      drawEyesNow();
      _eyeState.playing = false;
      return;
    }

    _eyeState.lastFrameMs = now;
    const int last = (int)count - 1;

    switch (_eyeState.mode) {
      case FaceAnim::PlayMode::Loop:
        _eyeState.frame = (uint8_t)((_eyeState.frame + 1) % count);
        drawEyesNow();
        break;

      case FaceAnim::PlayMode::Once:
        if (_eyeState.frame < last) {
          _eyeState.frame++;
          drawEyesNow();
        } else {
          _eyeState.playing = false;
        }
        break;

      case FaceAnim::PlayMode::OnceHold:
        if (_eyeState.frame < last) {
          _eyeState.frame++;
          drawEyesNow();
        } else {
          const int8_t hf = _eyeState.hold_frame;
          if (hf >= 0 && hf < (int8_t)count) _eyeState.frame = (uint8_t)hf;
          drawEyesNow();
          _eyeState.playing = false;
        }
        break;

      case FaceAnim::PlayMode::PingPong: {
        int nf = (int)_eyeState.frame + (int)_eyeState.dir;
        if (nf >= last) { nf = last; _eyeState.dir = -1; }
        if (nf <= 0)    { nf = 0;    _eyeState.dir = +1; }
        _eyeState.frame = (uint8_t)nf;
        drawEyesNow();
      } break;

      default:
        break;
    }
  }
};

inline FaceRenderer* FaceRenderer::s_active = nullptr;