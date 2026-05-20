MiP RMS VAD Branch Update
=========================

This patch adds a first-pass RMS voice activity detector while keeping the stable 16 kHz audio pipeline unchanged.

New/changed ESP32 files:
- src/vad_controller.h
- src/vad_controller.cpp
- src/control_modes.h
- src/ble_control.cpp
- src/mic.cpp
- src/main.cpp
- src/lib_websocket.cpp
- src/config.h
- web/mip_ble_controller.html

Changed server files:
- robot_bridge.ts
- prompt.ts

Behavior:
- Adds MODE_GPT_VAD / "gpt_vad".
- BLE mode switch now cycles: manual -> gpt_assisted -> gpt_vad -> gpt_autonomous -> manual.
- Web UI adds a "GPT VAD" button.
- In GPT VAD mode, micTask continuously reads local audio for RMS VAD.
- When speech energy holds above threshold, VAD calls beginPttRecording("vad").
- After silence, VAD calls endPttRecording("vad").
- Assistant playback suppresses VAD briefly to reduce echo/retriggering.
- Manual, GPT Assist PTT, GPT Auto, BLE commands, MiP action queue, radar/status injection, and the 16 kHz audio path are otherwise preserved.

Initial tuning constants are in src/vad_controller.cpp:
- VAD_START_RMS = 420
- VAD_STOP_RMS = 260
- VAD_START_HOLD_MS = 140
- VAD_SILENCE_MS = 900
- VAD_MAX_RECORD_MS = 9000

If it false-triggers, raise VAD_START_RMS.
If it misses speech, lower VAD_START_RMS.
If it cuts off too early, raise VAD_SILENCE_MS.
