import "dotenv/config";
import { WebSocket } from "ws";
import { serve } from "@hono/node-server";
import { Hono } from "hono";
import { createNodeWebSocket } from "@hono/node-ws";
import { serveStatic } from "@hono/node-server/serve-static";

import { INSTRUCTIONS } from "./prompt";
import { TOOLS } from "./tools";
import { OpenAIVoiceReactAgent } from "./lib/agent";
import { clearRobotSocket, setRobotSocket, updateRobotStateFromMessage } from "./robot_bridge";

const app = new Hono();
const WS_PORT = 8888;
const connectedClients = new Set<WebSocket>();

const { injectWebSocket, upgradeWebSocket } = createNodeWebSocket({ app });

app.use("/", serveStatic({ path: "./static/index.html" }));
app.use("/static/*", serveStatic({ root: "./" }));

app.get(
  "/device",
  upgradeWebSocket((c) => ({
    onOpen: async (c, ws) => {
      if (!process.env.OPENAI_API_KEY) {
        return ws.close();
      }

      const rawWs = ws.raw as WebSocket;
      connectedClients.add(rawWs);
      setRobotSocket(rawWs);

      rawWs.on("message", (data, isBinary) => {
        if (isBinary) return;

        const text = Buffer.isBuffer(data) ? data.toString("utf8") : String(data);
        try {
          updateRobotStateFromMessage(JSON.parse(text), rawWs);
        } catch {
          // Non-JSON text frames like START_RECORD / STOP_RECORD are handled by the agent stream.
        }
      });

      const sendOutputToThisRobot = (data: string) => {
        if (rawWs.readyState !== WebSocket.OPEN) return;

        try {
          const parsed = JSON.parse(data);
          if (parsed.type === "response.audio.delta" && parsed.delta) {
            const audioBuffer = Buffer.from(parsed.delta, "base64");
            const CHUNK_SIZE = 1024;
            for (let i = 0; i < audioBuffer.length; i += CHUNK_SIZE) {
              rawWs.send(audioBuffer.slice(i, i + CHUNK_SIZE));
            }
          }
        } catch {
          // Non-audio text messages are not sent to the robot by default.
        }
      };

      const agent = new OpenAIVoiceReactAgent({
        instructions: INSTRUCTIONS,
        tools: TOOLS,
        model: "gpt-realtime",
        audioConfig: {
          sampleRate: 24000,
          channels: 1,
          bitDepth: 16,
        },
      });

      try {
        await new Promise((resolve) => setTimeout(resolve, 1000));
        await agent.connect(rawWs, sendOutputToThisRobot);
      } catch (error) {
        console.error("[DEVICE] Agent connection ended with error:", error);
      } finally {
        connectedClients.delete(rawWs);
        clearRobotSocket(rawWs);
      }
    },
    onClose: (c, ws) => {
      const rawWs = ws.raw as WebSocket;
      connectedClients.delete(rawWs);
      clearRobotSocket(rawWs);
      console.log("Client disconnected");
    },
  }))
);

const server = serve({
  fetch: app.fetch,
  port: WS_PORT,
});

injectWebSocket(server);

console.log(`Server is running on port ${WS_PORT}`);
