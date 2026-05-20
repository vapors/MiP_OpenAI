import { WebSocket } from "ws";

type RobotState = {
  // Server-side transport state: can the server currently send commands to the ESP32?
  transport_connected: boolean;

  // ESP32-reported state: does the ESP32 believe its WebSocket is connected?
  esp_ws: boolean;

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
};

let currentDeviceSocket: WebSocket | null = null;
let robotState: RobotState = {
  transport_connected: false,
  esp_ws: false,
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

  console.log("[MIP BRIDGE] State", robotState);
}

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
