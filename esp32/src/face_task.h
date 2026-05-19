#pragma once
#include <stdint.h>
#include "face_renderer.h"
// EYE_FRAMES   = 6; eyes_blink0..5
// LOOK_FRAMES  = 6;  look_left00..05, look_right00..05
// MOUTH_FRAMES = 11; mouth00..mouth10
// Start FreeRTOS face task (call after LCD + LittleFS are ready)
void startFaceTask();

// Runtime control (thread-safe via queue)
void face_set_eye_anim(FaceAnim::EyeAnimId id, FaceAnim::PlayMode mode, uint16_t fps = 28, int8_t hold_frame = -1);
void face_set_eye_frame(uint8_t frame);
void face_enable_auto_blink(bool en);

// Convenience helpers
inline void face_eyes_blink()      { face_enable_auto_blink(true);  face_set_eye_anim(FaceAnim::EyeAnimId::Blink, FaceAnim::PlayMode::Loop, 28); }
inline void face_eyes_blink_slowly()      { face_enable_auto_blink(false);  face_set_eye_anim(FaceAnim::EyeAnimId::Blink, FaceAnim::PlayMode::PingPong, 10); }
inline void face_eyes_look_left()  { face_enable_auto_blink(false); face_set_eye_anim(FaceAnim::EyeAnimId::LookLeft, FaceAnim::PlayMode::Once, 30); }
inline void face_eyes_look_left_hold()  { face_enable_auto_blink(false); face_set_eye_anim(FaceAnim::EyeAnimId::LookLeft, FaceAnim::PlayMode::OnceHold, 30, 6); }
inline void face_eyes_look_right() { face_enable_auto_blink(false); face_set_eye_anim(FaceAnim::EyeAnimId::LookRight, FaceAnim::PlayMode::Once, 30); }
inline void face_eyes_look_right_hold() { face_enable_auto_blink(false); face_set_eye_anim(FaceAnim::EyeAnimId::LookRight, FaceAnim::PlayMode::OnceHold, 30, 6); }

// “Sleep”: hold on closed eyes
inline void face_eyes_sleep()      { face_enable_auto_blink(false);  face_set_eye_anim(FaceAnim::EyeAnimId::Closed, FaceAnim::PlayMode::OnceHold, 1, 0);}
