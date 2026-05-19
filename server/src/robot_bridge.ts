import { WebSocket } from "ws";

type RobotState = {
  connected: boolean;
  mode?: string;
  lastEvent?: string;
  lastUpdatedAt?: number;
};

let currentDeviceSocket: WebSocket | null = null;
let robotState: RobotState = { connected: false };

export function setRobotSocket(ws: WebSocket | null) {
  currentDeviceSocket = ws;
  robotState.connected = !!ws && ws.readyState === WebSocket.OPEN;
  robotState.lastUpdatedAt = Date.now();
  console.log(ws ? "[MIP BRIDGE] Robot socket registered" : "[MIP BRIDGE] Robot socket cleared");
}

export function isRobotConnected() {
  return currentDeviceSocket?.readyState === WebSocket.OPEN;
}

export function updateRobotStateFromMessage(message: unknown) {
  if (!message || typeof message !== "object") return;

  const msg = message as Record<string, unknown>;
  if (msg.type !== "robot_state") return;

  robotState = {
    connected: isRobotConnected(),
    mode: typeof msg.mode === "string" ? msg.mode : robotState.mode,
    lastEvent: typeof msg.event === "string" ? msg.event : robotState.lastEvent,
    lastUpdatedAt: Date.now(),
  };

  console.log("[MIP BRIDGE] State", robotState);
}

export function getRobotState() {
  return {
    ...robotState,
    connected: isRobotConnected(),
  };
}

export function sendMipCommand(command: Record<string, unknown>) {
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

  return `Sent MiP command: ${String(command.command ?? "unknown")}`;
}
