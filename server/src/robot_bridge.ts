import { WebSocket } from "ws";
import { EventEmitter } from "events";

const robotEvents = new EventEmitter();

type RobotState = {
  // Server-side transport state: can the server currently send commands to the ESP32?
  transport_connected: boolean;

  // ESP32-reported state: does the ESP32 believe its WebSocket is connected?
  esp_ws: boolean;
  body_state?: string;
  body_connected?: boolean;
  mip_body_connected?: boolean;
  body_last_rx_age_ms?: number;

  mode?: string;
  recording?: boolean;
  action?: string;
  ir_blocked?: boolean;
  radar_code?: number;
  radar?: string;
  mip_position_code?: number;
  mip_position?: string;
  gesture_code?: number;
  gesture?: string;
  radar_gesture_mode?: number;
  detected_mip_id?: number;
  shake_detected?: boolean;
  battery_mv?: number;
  battery_percent?: number;
  lastEvent?: string;
  lastUpdatedAt?: number;
  body_type?: string;

};



type RobotSensorEvent = {
  kind: "radar" | "position";
  text: string;
  state: RobotState;
};

let lastInjectedRadarAt = 0;
let lastInjectedPositionAt = 0;

const SENSOR_INJECT_COOLDOWN_MS = 1500; // Minimum time between injected sensor events to avoid flooding prompts with noise.

export function onRobotSensorEvent(
  listener: (event: RobotSensorEvent) => void
) {
  // MiP has one active robot session. Prevent old/reconnected agents from
  // stacking listeners and causing duplicate conversation injections.
  robotEvents.removeAllListeners("sensor");

  robotEvents.on("sensor", listener);

  return () => {
    robotEvents.off("sensor", listener);
  };
}


let currentDeviceSocket: WebSocket | null = null;
let robotState: RobotState = {
  transport_connected: false,

  
  esp_ws: false,


  body_state: "unknown",
  body_connected: false,
  mip_body_connected: false,
  body_last_rx_age_ms: -1,

  mode: "unknown",
  recording: false,
  action: "idle",
  ir_blocked: false,
  radar_code: 0,
  radar: "unknown",
  mip_position_code: 255,
  mip_position: "unknown",
  gesture_code: 0,
  gesture: "none",
  radar_gesture_mode: 0,
  detected_mip_id: -1,
  shake_detected: false,
  battery_mv: -1,
  battery_percent: -1,
};

let lastStateLogAt = 0;
let lastLoggedEvent = "";
let lastLoggedIrBlocked: boolean | undefined = undefined;
let lastLoggedAction: string | undefined = undefined;

export function setRobotSocket(ws: WebSocket | null) {
  if (!ws) {
    clearRobotSocket();
    return;
  }

  currentDeviceSocket = ws;
  robotState.transport_connected = ws.readyState === WebSocket.OPEN;
  robotState.lastUpdatedAt = Date.now();
  console.log("[MIP BRIDGE] Robot socket registered");
}

export function clearRobotSocket(ws?: WebSocket | null) {
  // Important: do not let an old/stale socket close clear the active robot socket.
  if (ws && currentDeviceSocket && ws !== currentDeviceSocket) {
    console.log("[MIP BRIDGE] Ignored close from stale socket");
    return;
  }

  currentDeviceSocket = null;
  robotState.transport_connected = false;
  robotState.lastUpdatedAt = Date.now();
  console.log("[MIP BRIDGE] Robot socket cleared");
}

export function isRobotConnected() {
  return currentDeviceSocket?.readyState === WebSocket.OPEN;
}

function booleanValue(value: unknown, fallback?: boolean): boolean | undefined {
  if (typeof value === "boolean") return value;
  if (typeof value === "string") {
    if (value === "true") return true;
    if (value === "false") return false;
  }
  return fallback;
}

function allowsSensorInjection(state: RobotState): boolean {
  return (
    state.transport_connected === true &&
    state.recording !== true &&
    (state.mode === "gpt_assisted" || state.mode === "gpt_vad" || state.mode === "gpt_autonomous")
  );
}

function numberValue(value: unknown, fallback?: number): number | undefined {
  if (typeof value === "number" && Number.isFinite(value)) return value;
  if (typeof value === "string" && value.trim() !== "") {
    const parsed = Number(value);
    if (Number.isFinite(parsed)) return parsed;
  }
  return fallback;
}

function shouldLogState(next: RobotState): boolean {
  const now = Date.now();
  const event = next.lastEvent ?? "";
  const actionChanged = next.action !== lastLoggedAction;
  const irChanged = next.ir_blocked !== lastLoggedIrBlocked;
  const eventChanged = event !== lastLoggedEvent;

  // Always log important changes.
  if (eventChanged || actionChanged || irChanged) return true;

  // Periodic status can be noisy; keep an occasional heartbeat only.
  if (event === "periodic") return now - lastStateLogAt > 10000;

  // Radar can repeat quickly while blocked; keep it to a readable rate.
  if (event.startsWith("radar")) return now - lastStateLogAt > 2000;

  return now - lastStateLogAt > 5000;
}

function logStateIfNeeded() {
  if (!shouldLogState(robotState)) return;

  lastStateLogAt = Date.now();
  lastLoggedEvent = robotState.lastEvent ?? "";
  lastLoggedIrBlocked = robotState.ir_blocked;
  lastLoggedAction = robotState.action;

  console.log("[MIP BRIDGE] State", {
    transport_connected: robotState.transport_connected,
    esp_ws: robotState.esp_ws,
    mode: robotState.mode,
    body_type: robotState.body_type,
    body_state: robotState.body_state,
    body_connected: robotState.body_connected,
    mip_body_connected: robotState.mip_body_connected,
    body_last_rx_age_ms: robotState.body_last_rx_age_ms,
    action: robotState.action,
    ir_blocked: robotState.ir_blocked,
    radar: robotState.radar,
    mip_position: robotState.mip_position,
    lastEvent: robotState.lastEvent,
  });
}
/*
export function updateRobotStateFromMessage(message: unknown, sourceSocket?: WebSocket) {
  if (!message || typeof message !== "object") return;

  const msg = message as Record<string, unknown>;
  if (msg.type !== "robot_state") return;

  // If a valid robot_state arrives from an open socket, that socket is the active robot.
  // This recovers cleanly if a stale close event previously cleared the bridge.
  if (sourceSocket && sourceSocket.readyState === WebSocket.OPEN && currentDeviceSocket !== sourceSocket) {
    currentDeviceSocket = sourceSocket;
    console.log("[MIP BRIDGE] Robot socket refreshed from state message");
  }

  robotState = {
    ...robotState,
    transport_connected: isRobotConnected(),
    esp_ws: booleanValue(msg.ws, robotState.esp_ws) ?? false,
    mode: typeof msg.mode === "string" ? msg.mode : robotState.mode,
    recording: booleanValue(msg.recording, robotState.recording),
    action: typeof msg.action === "string" ? msg.action : robotState.action,
    ir_blocked: booleanValue(msg.ir_blocked, robotState.ir_blocked),
    radar_code: numberValue(msg.radar_code, robotState.radar_code),
    radar: typeof msg.radar === "string" ? msg.radar : robotState.radar,
    mip_position_code: numberValue(msg.mip_position_code, robotState.mip_position_code),
    mip_position: typeof msg.mip_position === "string" ? msg.mip_position : robotState.mip_position,
    gesture_code: numberValue(msg.gesture_code, robotState.gesture_code),
    gesture: typeof msg.gesture === "string" ? msg.gesture : robotState.gesture,
    radar_gesture_mode: numberValue(msg.radar_gesture_mode, robotState.radar_gesture_mode),
    detected_mip_id: numberValue(msg.detected_mip_id, robotState.detected_mip_id),
    shake_detected: booleanValue(msg.shake_detected, robotState.shake_detected),
    battery_mv: numberValue(msg.battery_mv, robotState.battery_mv),
    battery_percent: numberValue(msg.battery_percent, robotState.battery_percent),
    lastEvent: typeof msg.event === "string" ? msg.event : robotState.lastEvent,
    lastUpdatedAt: Date.now(),
  };

  logStateIfNeeded();
}
*/
export function updateRobotStateFromMessage(message: unknown, sourceSocket?: WebSocket) {
  if (!message || typeof message !== "object") return;

  const msg = message as Record<string, unknown>;
  if (msg.type !== "robot_state") return;

  // If a valid robot_state arrives from an open socket, that socket is the active robot.
  // This recovers cleanly if a stale close event previously cleared the bridge.
  if (sourceSocket && sourceSocket.readyState === WebSocket.OPEN && currentDeviceSocket !== sourceSocket) {
    currentDeviceSocket = sourceSocket;
    console.log("[MIP BRIDGE] Robot socket refreshed from state message");
  }

  const previous = { ...robotState };
  const eventName = typeof msg.event === "string" ? msg.event : robotState.lastEvent;

  robotState = {
    ...robotState,
    transport_connected: isRobotConnected(),
    esp_ws: booleanValue(msg.ws, robotState.esp_ws),
    mode: typeof msg.mode === "string" ? msg.mode : robotState.mode,

    body_type: typeof msg.body_type === "string" ? msg.body_type : robotState.body_type,
    body_state: typeof msg.body_state === "string" ? msg.body_state : robotState.body_state,
    body_connected: booleanValue(msg.body_connected, robotState.body_connected),
    mip_body_connected: booleanValue(msg.mip_body_connected, robotState.mip_body_connected),
    body_last_rx_age_ms: numberValue(msg.body_last_rx_age_ms, robotState.body_last_rx_age_ms),

    recording: booleanValue(msg.recording, robotState.recording),
    action: typeof msg.action === "string" ? msg.action : robotState.action,

    ir_blocked: booleanValue(msg.ir_blocked, robotState.ir_blocked),
    radar_code: numberValue(msg.radar_code, robotState.radar_code),
    radar: typeof msg.radar === "string" ? msg.radar : robotState.radar,

    mip_position_code: numberValue(msg.mip_position_code, robotState.mip_position_code),
    mip_position: typeof msg.mip_position === "string" ? msg.mip_position : robotState.mip_position,

    battery_mv: numberValue(msg.battery_mv, robotState.battery_mv),
    battery_percent: numberValue(msg.battery_percent, robotState.battery_percent),

    lastEvent: eventName,
    lastUpdatedAt: Date.now(),
  };

  //console.log("[MIP BRIDGE] State", robotState);
  logStateIfNeeded();
  // Always store robot state, but only inject live sensor events when GPT is allowed
  // to react to the robot body.
  if (!allowsSensorInjection(robotState)) {
    return;
  }
  if (robotState.lastEvent === "action_done") {
    robotEvents.emit("action_done", robotState);
  }
  // Do not inject routine heartbeat/status noise.
  if (eventName === "periodic") return;

  const now = Date.now();

  const radarChanged =
    previous.ir_blocked !== robotState.ir_blocked ||
    previous.radar_code !== robotState.radar_code ||
    previous.radar !== robotState.radar;

  if (
    radarChanged &&
    typeof robotState.ir_blocked === "boolean" &&
    now - lastInjectedRadarAt > SENSOR_INJECT_COOLDOWN_MS
  ) {
    lastInjectedRadarAt = now;

    const text = robotState.ir_blocked
      ? `Robot sensor update: MiP's IR/radar now detects an obstacle. Radar state is ${robotState.radar ?? "blocked"}. Avoid forward movement.`
      : `Robot sensor update: MiP's IR/radar is now clear. Forward movement may be allowed if appropriate.`;

    robotEvents.emit("sensor", {
      kind: "radar",
      text,
      state: getRobotState(),
    } satisfies RobotSensorEvent);
  }

  const positionChanged =
    previous.mip_position !== robotState.mip_position ||
    previous.mip_position_code !== robotState.mip_position_code;

  if (
    positionChanged &&
    robotState.mip_position &&
    robotState.mip_position !== "unknown" &&
    now - lastInjectedPositionAt > SENSOR_INJECT_COOLDOWN_MS
  ) {
    lastInjectedPositionAt = now;

    const upright = robotState.mip_position === "upright";

    const text = upright
      ? `Robot sensor update: MiP is upright again.`
      : `Robot sensor update: MiP is not upright. Current position is ${robotState.mip_position}. Avoid movement that assumes MiP is balanced upright.`;

    robotEvents.emit("sensor", {
      kind: "position",
      text,
      state: getRobotState(),
    } satisfies RobotSensorEvent);
  }
}



export function getRobotState() {
  const transport_connected = isRobotConnected();

  return {
    ...robotState,
    transport_connected,

    // Compatibility alias for existing tools/prompts that may still read `connected`.
    // Prefer `transport_connected` in new code.
    connected: transport_connected,
   
  };
}


function commandShouldWaitForAction(command: Record<string, unknown>): boolean {
  const name = String(command.command ?? "");

  // Wait for physical/body actions.
  return [
    "stand_up",
    "set_position",
    "move_forward",
    "move_backward",
    "drive_distance",
    "turn_left",
    "turn_right",
    "spin",
    "continuous_drive",
    "crazy_drive",
    "game_mode",
    "expression_preset",
    "sound",
    "sound_sequence",
    "chest_led",
    "flash_chest_led",
    "head_leds",
  ].includes(name);
}

function waitForActionDone(timeoutMs = 12000): Promise<string> {
  return new Promise((resolve) => {
    const startedAt = Date.now();

    const timeout = setTimeout(() => {
      robotEvents.off("action_done", onDone);
      resolve(`Action wait timed out after ${timeoutMs} ms.`);
    }, timeoutMs);

    const onDone = () => {
      clearTimeout(timeout);
      robotEvents.off("action_done", onDone);

      const elapsed = Date.now() - startedAt;
      resolve(`Action completed after ${elapsed} ms.`);
    };

    robotEvents.on("action_done", onDone);
  });
}

export async function sendMipCommand(command: Record<string, unknown>) {
  
  if (!isRobotConnected() || !currentDeviceSocket) {
    console.warn("[MIP TOOL] Robot WebSocket is not connected");
    return "MiP robot is not connected.";
  }

  const payload = {
    type: "mip_command",
    ...command,
  };

  const json = JSON.stringify(payload);
  console.log("[MIP TOOL] TX", json);
  currentDeviceSocket.send(json);

  if (!commandShouldWaitForAction(command)) {
    return `Sent MiP command: ${String(command.command ?? "unknown")}`;
  }

  const result = await waitForActionDone(15000);

  return `MiP command ${String(command.command ?? "unknown")} finished. ${result}`;
}
/*
export function sendMipCommand(command: Record<string, unknown>) {
  if (!isRobotConnected() || !currentDeviceSocket) {
    console.warn("[MIP TOOL] Robot transport is not connected");
    return "MiP robot transport is not connected.";
  }

  const payload = {
    type: "mip_command",
    ...command,
  };

  const json = JSON.stringify(payload);
  console.log("[MIP TOOL] TX", json);
  currentDeviceSocket.send(json);

  return `Sent MiP command: ${String(command.command ?? "unknown")}`;
}
*/
export function sendFaceCommand(command: Record<string, unknown>) {
  if (!isRobotConnected() || !currentDeviceSocket) {
    console.warn("[FACE TOOL] Robot WebSocket is not connected");
    return "Robot head/face is not connected.";
  }

  const payload = {
    type: "face_command",
    ...command,
  };

  const json = JSON.stringify(payload);
  console.log("[FACE TOOL] TX", json);
  currentDeviceSocket.send(json);

  return `Sent face command: ${String(command.command ?? "unknown")}`;
}
