import "dotenv/config";
import { WebSocket } from "ws";
import { serve } from "@hono/node-server";
import { Hono } from "hono";
import { createNodeWebSocket } from "@hono/node-ws";
import { serveStatic } from "@hono/node-server/serve-static";

import { INSTRUCTIONS } from "./prompt";
import { TOOLS } from "./tools";
import { Tool } from "@langchain/core/tools";
import { OpenAIVoiceReactAgent } from "./lib/agent";
import { setRobotSocket, updateRobotStateFromMessage } from "./robot_bridge";

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
          updateRobotStateFromMessage(JSON.parse(text));
        } catch {
          // Non-JSON text frames like START_RECORD / STOP_RECORD are handled by the agent stream.
        }
      });

      const broadcastToClients = (data: string) => {
        connectedClients.forEach((client) => {
          if (client.readyState !== WebSocket.OPEN) return;

          try {
            const parsed = JSON.parse(data);
            if (parsed.type === "response.audio.delta" && parsed.delta) {
              const audioBuffer = Buffer.from(parsed.delta, "base64");
              const CHUNK_SIZE = 1024;
              for (let i = 0; i < audioBuffer.length; i += CHUNK_SIZE) {
                client.send(audioBuffer.slice(i, i + CHUNK_SIZE));
              }
              return;
            }
          } catch {
            // Non-audio text messages are not broadcast by default.
          }
        });
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

      await new Promise((resolve) => setTimeout(resolve, 1000));
      await agent.connect(rawWs, broadcastToClients);
    },
    onClose: (c, ws) => {
      const rawWs = ws.raw as WebSocket;
      connectedClients.delete(rawWs);
      setRobotSocket(null);
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
