import WebSocket from "ws";
import { StructuredTool, Tool } from "@langchain/core/tools";
import { mergeStreams, createStreamFromWebsocket, base64EncodeAudio } from "./utils";
import { zodToJsonSchema } from "zod-to-json-schema";
import { tool } from "@langchain/core/tools";
import fs from 'fs';
import decodeAudio from 'audio-decode';
import path from "path";
import clipboardy from 'clipboardy';
import { AudioManager } from "./audio";






// const handleAudioData = (data: ArrayBuffer, handler: AudioHandler): void => {
//   const uint8Array = new Uint8Array(data);
//   appendToBuffer(uint8Array, handler);

//   if (handler.buffer.length >= handler.BUFFER_SIZE) {
//     const toSend = new Uint8Array(handler.buffer.slice(0, handler.BUFFER_SIZE));
//     handler.buffer = new Uint8Array(handler.buffer.slice(handler.BUFFER_SIZE));

//     const regularArray = String.fromCharCode(...toSend);
//     const base64 = btoa(regularArray);
//     handler.ws.send(JSON.stringify({
//       type: 'input_audio_buffer.append',
//       audio: base64
//     }));
//   }
// };
// const audioHandler: AudioHandler = {
//   buffer: new Uint8Array(),
//   BUFFER_SIZE: 8192, // Common audio buffer size, adjust as needed
//   ws: new WebSocket('ws://localhost:3000/ws')
// };


