import { z } from "zod";
import { tool } from "@langchain/core/tools";
import { getRobotState, sendMipCommand } from "./robot_bridge";

const headLedModeSchema = z.number().int().min(0).max(3).describe("Head LED mode: 0 off, 1 on, 2 slow blink, 3 fast blink.");

export const mipGetRobotState = tool(
  async () => JSON.stringify(getRobotState()),
  {
    name: "mip_get_robot_state",
    description: "Check whether the MiP robot websocket is connected and view the last known robot state/mode.",
    schema: z.object({}),
  }
);

export const mipStop = tool(
  async () => sendMipCommand({ command: "stop" }),
  {
    name: "mip_stop",
    description: "Immediately stop the MiP robot. Use this when the user says stop, freeze, halt, or when you want to end a movement.",
    schema: z.object({}),
  }
);

export const mipStandUp = tool(
  async ({ side = "either" }) => sendMipCommand({ command: "stand_up", side }),
  {
    name: "mip_stand_up",
    description: "Ask MiP to get up from its front, back, or either orientation. Use when MiP falls over or the user says get up or stand up.",
    schema: z.object({
      side: z.enum(["front", "back", "either"]).default("either"),
    }),
  }
);

export const mipSetPosition = tool(
  async ({ pose }) => sendMipCommand({ command: "set_position", pose }),
  {
    name: "mip_set_position",
    description: "Set MiP's known fallen position/orientation, such as face up or face down.",
    schema: z.object({
      pose: z.enum(["face_up", "face_down"]),
    }),
  }
);

export const mipMoveForward = tool(
  async ({ speed = 12, duration_ms = 700 }) => sendMipCommand({ command: "move_forward", speed, duration_ms }),
  {
    name: "mip_move_forward",
    description: "Move MiP forward for a short timed burst. Use this for casual forward motion or small steps.",
    schema: z.object({
      speed: z.number().int().min(0).max(30).default(12),
      duration_ms: z.number().int().min(35).max(3000).default(700),
    }),
  }
);

export const mipMoveBackward = tool(
  async ({ speed = 12, duration_ms = 700 }) => sendMipCommand({ command: "move_backward_timed", speed, duration_ms }),
  {
    name: "mip_move_backward",
    description: "Move MiP backward for a short timed burst. Use for backing up, acting startled, or reversing.",
    schema: z.object({
      speed: z.number().int().min(0).max(30).default(12),
      duration_ms: z.number().int().min(35).max(3000).default(700),
    }),
  }
);

export const mipDriveDistance = tool(
  async ({ distance_cm = 10, angle_deg = 0 }) => sendMipCommand({ command: "drive_distance", distance_cm, angle_deg }),
  {
    name: "mip_drive_distance",
    description: "Drive MiP a specific distance in centimeters while optionally rotating. Positive distance is forward; negative is backward.",
    schema: z.object({
      distance_cm: z.number().int().min(-255).max(255).default(10),
      angle_deg: z.number().int().min(-360).max(360).default(0),
    }),
  }
);

export const mipTurnLeft = tool(
  async ({ angle_deg = 90, speed = 12 }) => sendMipCommand({ command: "turn_left", angle_deg, speed }),
  {
    name: "mip_turn_left",
    description: "Turn MiP left by an angle. Use when the user asks MiP to turn left, rotate left, or look left.",
    schema: z.object({
      angle_deg: z.number().int().min(5).max(1275).default(90),
      speed: z.number().int().min(0).max(24).default(12),
    }),
  }
);

export const mipTurnRight = tool(
  async ({ angle_deg = 90, speed = 12 }) => sendMipCommand({ command: "turn_right", angle_deg, speed }),
  {
    name: "mip_turn_right",
    description: "Turn MiP right by an angle. Use when the user asks MiP to turn right, rotate right, or look right.",
    schema: z.object({
      angle_deg: z.number().int().min(5).max(1275).default(90),
      speed: z.number().int().min(0).max(24).default(12),
    }),
  }
);

export const mipSpin = tool(
  async ({ direction = "left", angle_deg = 360, speed = 14 }) => sendMipCommand({ command: "spin", direction, angle_deg, speed }),
  {
    name: "mip_spin",
    description: "Spin or twirl MiP in place. Use for show-off moves, celebration, dancing, or when the user says spin.",
    schema: z.object({
      direction: z.enum(["left", "right"]).default("left"),
      angle_deg: z.number().int().min(45).max(2160).default(360),
      speed: z.number().int().min(1).max(24).default(14),
    }),
  }
);

export const mipContinuousDrive = tool(
  async ({ direction = "forward", speed = 12, duration_ms = 800 }) => sendMipCommand({ command: "continuous_drive", direction, speed, duration_ms }),
  {
    name: "mip_continuous_drive",
    description: "Use MiP's continuous drive/spin command briefly. Good for playful movement, skittering, or more animated body language.",
    schema: z.object({
      direction: z.enum(["forward", "backward", "left", "right"]).default("forward"),
      speed: z.number().int().min(1).max(32).default(12),
      duration_ms: z.number().int().min(50).max(5000).default(800),
    }),
  }
);

export const mipCrazyDrive = tool(
  async ({ direction = "forward", speed = 14, duration_ms = 800 }) => sendMipCommand({ command: "crazy_drive", direction, speed, duration_ms }),
  {
    name: "mip_crazy_drive",
    description: "Use MiP's crazy drive/spin ranges for a more chaotic, playful motion burst. Use when the user asks MiP to go crazy, freak out, celebrate, or act wild.",
    schema: z.object({
      direction: z.enum(["forward", "backward", "left", "right"]).default("forward"),
      speed: z.number().int().min(1).max(31).default(14),
      duration_ms: z.number().int().min(50).max(5000).default(800),
    }),
  }
);

export const mipSetGameMode = tool(
  async ({ mode }) => sendMipCommand({ command: "game_mode", mode }),
  {
    name: "mip_set_game_mode",
    description: "Switch MiP into a built-in game/behavior mode: app, cage, tracking, dance, default, stack, tricks, or roam.",
    schema: z.object({
      mode: z.enum(["app", "cage", "tracking", "dance", "default", "stack", "tricks", "roam"]),
    }),
  }
);

export const mipDance = tool(
  async () => sendMipCommand({ command: "game_mode", mode: "dance" }),
  {
    name: "mip_dance",
    description: "Put MiP into its built-in dance mode. Use when the user asks MiP to dance, perform, party, or show off.",
    schema: z.object({}),
  }
);

export const mipRoam = tool(
  async () => sendMipCommand({ command: "game_mode", mode: "roam" }),
  {
    name: "mip_roam",
    description: "Put MiP into built-in roam mode. Use when the user asks MiP to explore, wander, or roam around.",
    schema: z.object({}),
  }
);

export const mipSetChestLed = tool(
  async ({ r = 0, g = 40, b = 255 }) => sendMipCommand({ command: "chest_led", r, g, b }),
  {
    name: "mip_set_chest_led",
    description: "Set MiP's chest LED to an RGB color. Use for emotion, feedback, or visual personality.",
    schema: z.object({
      r: z.number().int().min(0).max(255).default(0),
      g: z.number().int().min(0).max(255).default(40),
      b: z.number().int().min(0).max(255).default(255),
    }),
  }
);

export const mipFlashChestLed = tool(
  async ({ r = 0, g = 80, b = 255, on_ms = 200, off_ms = 200 }) => sendMipCommand({ command: "flash_chest_led", r, g, b, on_ms, off_ms }),
  {
    name: "mip_flash_chest_led",
    description: "Flash MiP's chest LED with color and timing. Use for excitement, warning, celebration, or emotion.",
    schema: z.object({
      r: z.number().int().min(0).max(255).default(0),
      g: z.number().int().min(0).max(255).default(80),
      b: z.number().int().min(0).max(255).default(255),
      on_ms: z.number().int().min(20).max(5100).default(200),
      off_ms: z.number().int().min(20).max(5100).default(200),
    }),
  }
);

export const mipSetHeadLeds = tool(
  async ({ l1 = 1, l2 = 1, l3 = 1, l4 = 1 }) => sendMipCommand({ command: "head_leds", l1, l2, l3, l4 }),
  {
    name: "mip_set_head_leds",
    description: "Set MiP's four head LEDs. Each LED uses 0 off, 1 on, 2 slow blink, or 3 fast blink.",
    schema: z.object({
      l1: headLedModeSchema.default(1),
      l2: headLedModeSchema.default(1),
      l3: headLedModeSchema.default(1),
      l4: headLedModeSchema.default(1),
    }),
  }
);

export const mipExpressionPreset = tool(
  async ({ expression }) => sendMipCommand({ command: "expression_preset", expression }),
  {
    name: "mip_expression_preset",
    description: "Trigger a combined light/sound/movement expression preset such as excited, scared, party, sleepy, curious, angry, or celebrate.",
    schema: z.object({
      expression: z.enum(["excited", "scared", "party", "sleepy", "curious", "angry", "celebrate"]),
    }),
  }
);

export const mipPlaySound = tool(
  async ({ sound_id = 1 }) => sendMipCommand({ command: "sound", sound_id }),
  {
    name: "mip_play_sound",
    description: "Play one of MiP's built-in sound effects. Sound IDs are 1 through 106. Sound 105 stops playback.",
    schema: z.object({
      sound_id: z.number().int().min(1).max(106).default(1),
    }),
  }
);

export const mipPlaySoundSequence = tool(
  async ({ sound_ids = "1,3,2", delay_ms = 120, repeat = 0 }) => sendMipCommand({ command: "sound_sequence", sound_ids, delay_ms, repeat }),
  {
    name: "mip_play_sound_sequence",
    description: "Play a short sequence of MiP sounds with a delay between them. Pass sound IDs as a comma-separated string, for example '1,3,2'.",
    schema: z.object({
      sound_ids: z.string().default("1,3,2"),
      delay_ms: z.number().int().min(0).max(7650).default(120),
      repeat: z.number().int().min(0).max(10).default(0),
    }),
  }
);

export const mipSetVolume = tool(
  async ({ volume = 5 }) => sendMipCommand({ command: "volume", volume }),
  {
    name: "mip_set_volume",
    description: "Set MiP's built-in sound volume from 0 silent to 7 loudest.",
    schema: z.object({
      volume: z.number().int().min(0).max(7).default(5),
    }),
  }
);

export const mipRequestStatus = tool(
  async () => sendMipCommand({ command: "request_status" }),
  {
    name: "mip_request_status",
    description: "Ask the MiP body for battery/orientation status. The ESP32 may log the response; full readback can be wired later.",
    schema: z.object({}),
  }
);

export const mipRequestWeight = tool(
  async () => sendMipCommand({ command: "request_weight" }),
  {
    name: "mip_request_weight",
    description: "Ask MiP for its weight/lean update. Useful for balance or body-state experiments; response parsing can be added later.",
    schema: z.object({}),
  }
);

export const mipReadOdometer = tool(
  async () => sendMipCommand({ command: "read_odometer" }),
  {
    name: "mip_read_odometer",
    description: "Request MiP's odometer reading. The ESP32 may log the response; structured readback can be added later.",
    schema: z.object({}),
  }
);

export const mipResetOdometer = tool(
  async () => sendMipCommand({ command: "reset_odometer" }),
  {
    name: "mip_reset_odometer",
    description: "Reset MiP's odometer counter.",
    schema: z.object({}),
  }
);

export const mipSetGestureRadar = tool(
  async ({ mode }) => sendMipCommand({ command: "gesture_radar", mode }),
  {
    name: "mip_set_gesture_radar",
    description: "Enable or disable MiP gesture/radar behavior: off, gesture, or radar.",
    schema: z.object({
      mode: z.enum(["off", "gesture", "radar"]),
    }),
  }
);

export const mipSetDetectionMode = tool(
  async ({ id = 1, power = 60 }) => sendMipCommand({ command: "detection_mode", id, power }),
  {
    name: "mip_set_detection_mode",
    description: "Enable MiP IR detection mode using an ID and transmit power. Use id 0 to disable.",
    schema: z.object({
      id: z.number().int().min(0).max(255).default(1),
      power: z.number().int().min(1).max(120).default(60),
    }),
  }
);

export const mipSetIRControl = tool(
  async ({ enabled = true }) => sendMipCommand({ command: "ir_control", enabled }),
  {
    name: "mip_set_ir_control",
    description: "Enable or disable MiP IR remote control reception.",
    schema: z.object({
      enabled: z.boolean().default(true),
    }),
  }
);

export const mipSendIRCode = tool(
  async ({ code, bit_count = 32, power = 60 }) => sendMipCommand({ command: "send_ir_code", code, bit_count, power }),
  {
    name: "mip_send_ir_code",
    description: "Send an IR dongle code from MiP. Useful for experiments with MiP-to-MiP games or IR accessories.",
    schema: z.object({
      code: z.number().int().min(0).max(0xffffffff),
      bit_count: z.number().int().min(1).max(32).default(32),
      power: z.number().int().min(1).max(120).default(60),
    }),
  }
);

export const mipSetClapDetection = tool(
  async ({ enabled = true }) => sendMipCommand({ command: "clap_detection", enabled }),
  {
    name: "mip_set_clap_detection",
    description: "Enable or disable MiP clap detection.",
    schema: z.object({
      enabled: z.boolean().default(true),
    }),
  }
);

export const mipSetClapDelay = tool(
  async ({ delay_ms = 500 }) => sendMipCommand({ command: "clap_delay", delay_ms }),
  {
    name: "mip_set_clap_delay",
    description: "Set the delay window between claps for MiP clap detection.",
    schema: z.object({
      delay_ms: z.number().int().min(0).max(65535).default(500),
    }),
  }
);

export const TOOLS = [
  mipGetRobotState,
  mipStop,
  mipStandUp,
  mipSetPosition,
  mipMoveForward,
  mipMoveBackward,
  mipDriveDistance,
  mipTurnLeft,
  mipTurnRight,
  mipSpin,
  mipContinuousDrive,
  mipCrazyDrive,
  mipSetGameMode,
  mipDance,
  mipRoam,
  mipSetChestLed,
  mipFlashChestLed,
  mipSetHeadLeds,
  mipExpressionPreset,
  mipPlaySound,
  mipPlaySoundSequence,
  mipSetVolume,
  mipRequestStatus,
  mipRequestWeight,
  mipReadOdometer,
  mipResetOdometer,
  mipSetGestureRadar,
  mipSetDetectionMode,
  mipSetIRControl,
  mipSendIRCode,
  mipSetClapDetection,
  mipSetClapDelay,
];
