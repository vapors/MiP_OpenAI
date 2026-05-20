//import { Tool, StructuredTool } from "@langchain/core/tools";
import { StructuredTool } from "@langchain/core/tools";
import path from "path";
import zodToJsonSchema from "zod-to-json-schema";
import { AudioManager } from "./audio";

import { createStreamFromWebsocket, base64EncodeAudio, mergeStreams, appendToBuffer, convertAudioToPCM16, resamplePcm16Mono } from "./utils";
import fs from 'fs';
import decodeAudio from 'audio-decode';
import WebSocket from "ws";
import { OpenAIWebSocketConnection } from "./connections";
import { VoiceToolExecutor } from "./executor";
import { onRobotSensorEvent } from "../robot_bridge";

// Constants
const EVENTS_TO_IGNORE = [
    "response.function_call_arguments.delta",
    "rate_limits.updated",
    "response.audio_transcript.delta",
    "response.output_audio_transcript.delta",
    "response.created",
    "response.content_part.added",
    "response.content_part.done",
    "conversation.item.created",
    "response.audio.done",
    "response.output_audio.done",
    "session.created",
    "session.updated",
    "response.done",
    "response.output_item.done",
];

// Interfaces
export interface AudioConfig {
    sampleRate: number;
    channels: number;
    bitDepth: number;
}

export interface AudioHandler {
    buffer: Uint8Array;
    BUFFER_SIZE: number;
    ws: WebSocket;
}

export interface OpenAIVoiceReactAgentOptions {
    model: string;
    apiKey?: string;
    instructions?: string;
    tools?: StructuredTool[];
    url?: string;
    audioConfig?: AudioConfig;
}

export class OpenAIVoiceReactAgent {
    // Protected properties
    protected connection: OpenAIWebSocketConnection;
    protected instructions?: string;
    protected tools: StructuredTool[];
    protected BUFFER_SIZE = 4800;

    // Public properties
    public buffer = new Uint8Array();
    public audioBuffer: Buffer | undefined;

    // Private properties
    private audioManager: AudioManager;
    private recording: boolean = false;
    private firstAudioChunkAtMs: number | null = null;
    private audioChunkCount: number = 0;
    private recordingStartedAtMs: number | null = null;

    private unsubscribeRobotSensorEvent?: () => void;
    private lastSensorResponseAtMs = 0;

    constructor(params: OpenAIVoiceReactAgentOptions) {
        this.audioManager = new AudioManager();
        this.connection = new OpenAIWebSocketConnection({
            url: params.url,
            apiKey: params.apiKey,
            model: params.model,
            audioConfig: params.audioConfig,
            audioManager: this.audioManager
        });
        this.instructions = params.instructions;
        this.tools = params.tools ?? [];
    }

    public startRecordingSession(): void {
        // Make sure we're not already recording
        if (this.recording) {
            this.audioManager.resetRecording();
        }
        this.recording = true;
        this.audioManager.startRecording();
        this.recordingStartedAtMs = Date.now();
        this.firstAudioChunkAtMs = null;
        this.audioChunkCount = 0;
        console.log('[TIMING] START_RECORD processed at', this.recordingStartedAtMs);
        console.log('Started new recording session');
    }

    public async stopRecordingAndProcessAudio(): Promise<void> {
        if (!this.recording) {
            console.log('No active recording to stop');
            return;
        }

        this.recording = false;

        try {

           
            this.audioManager.closeFile();


            const rawPcm16k = this.audioManager.getCurrentRawPcmBuffer();

            const MIN_RAW_BYTES = 16000 * 2 * 0.75; // 750 ms @ 16k PCM16 mono
            if (rawPcm16k.length < MIN_RAW_BYTES) {
            console.log("Raw PCM payload too short, skipping OpenAI commit:", {
                rawAudioBytes: rawPcm16k.length,
                minBytes: MIN_RAW_BYTES
            });
            return;
            }

            const pcm24k = resamplePcm16Mono(rawPcm16k, 16000, 24000);

            console.log("Preparing resampled PCM audio for OpenAI:", {
            inputBytes16k: rawPcm16k.length,
            inputApproxMs: Math.round(rawPcm16k.length / 2 / 16000 * 1000),
            outputBytes24k: pcm24k.length,
            outputApproxMs: Math.round(pcm24k.length / 2 / 24000 * 1000),
            chunks: this.audioChunkCount,
            firstChunkDelayMs: this.firstAudioChunkAtMs && this.recordingStartedAtMs
                ? this.firstAudioChunkAtMs - this.recordingStartedAtMs
                : null
            });

            const base64 = pcm24k.toString("base64");
            await this.sendAudioEvent(base64);

            /*

            // The ESP32 sends raw PCM16 mono @ 24 kHz. Send that directly to
            // Realtime instead of converting the debug WAV file.
            const rawPcm = this.audioManager.getCurrentRawPcmBuffer();

            const MIN_RAW_BYTES = 4800; // 100 ms @ 24 kHz, 16-bit mono
            if (rawPcm.length < MIN_RAW_BYTES) {
                console.log("Raw PCM payload too short, skipping OpenAI commit:", {
                    rawAudioBytes: rawPcm.length,
                    minBytes: MIN_RAW_BYTES
                });
                return;
            }

            console.log("Preparing raw PCM audio for OpenAI:", {
                rawAudioBytes: rawPcm.length,
                approxMs: Math.round(rawPcm.length / 2 / 24000 * 1000),
                chunks: this.audioChunkCount,
                firstChunkDelayMs: this.firstAudioChunkAtMs && this.recordingStartedAtMs
                    ? this.firstAudioChunkAtMs - this.recordingStartedAtMs
                    : null,
                recordWindowMs: this.recordingStartedAtMs ? Date.now() - this.recordingStartedAtMs : null
            });

            const base64 = rawPcm.toString("base64");
            await this.sendAudioEvent(base64);

            */


        } catch (error) {
            console.error('Error processing audio:', error);
        } finally {
            this.audioManager.resetRecording();
            console.log('Recording session ended and cleaned up');
        }
    }


    private async sendAudioEvent(base64Audio: string): Promise<void> {
        if (!base64Audio) {
            console.log("No audio data to send");
            return;
        }

        try {
            console.log("Sending audio to OpenAI input buffer:", {
                base64Length: base64Audio.length
            });

            this.connection.sendEvent({
                type: "input_audio_buffer.append",
                audio: base64Audio
            });

            this.connection.sendEvent({
                type: "input_audio_buffer.commit"
            });

            this.connection.sendEvent({
                type: "response.create",
                response: {
                    output_modalities: ["audio"]
                }
            });
        } catch (error) {
            console.error("Error sending audio event:", error);
            throw error;
        }
    }

    private setupRobotSensorInjection(): void {
    if (this.unsubscribeRobotSensorEvent) return;

    this.unsubscribeRobotSensorEvent = onRobotSensorEvent((event) => {
        const now = Date.now();

        // Avoid the robot talking over itself too often.
        const shouldSpeakImmediately =
        event.kind === "radar" || event.kind === "position";

        const mayCreateResponse = now - this.lastSensorResponseAtMs > 2500;

        console.log("[MIP SENSOR INJECT]", event.text);

        this.connection.sendEvent({
        type: "conversation.item.create",
        item: {
            type: "message",
            role: "system",
            content: [
            {
                type: "input_text",
                text: event.text,
            },
            ],
        },
        });

        if (shouldSpeakImmediately && mayCreateResponse) {
        this.lastSensorResponseAtMs = now;

        this.connection.sendEvent({
            type: "response.create",
            response: {
            output_modalities: ["audio"],
            instructions:
                "React briefly as MiP. If radar is blocked, acknowledge the obstacle and avoid forward movement. If MiP is not upright, acknowledge the position problem. Keep it under one sentence.",
            },
        });
        }
    });
    }


    // WebSocket Connection Methods
    async connect(
        websocketOrStream: AsyncGenerator<string> | WebSocket,
        sendOutputChunk: (chunk: string) => void | Promise<void>
    ): Promise<void> {
        let inputStream = await this.setupWebSocketConnection(websocketOrStream);
        const toolsByName = this.createToolsMap();
        const toolExecutor = new VoiceToolExecutor(toolsByName);

        await this.initializeConnection(toolsByName);
        this.setupRobotSensorInjection();

        await this.handleStreamEvents(inputStream, toolExecutor, sendOutputChunk);
    }

    private async setupWebSocketConnection(websocketOrStream: AsyncGenerator<string> | WebSocket) {
        if ("next" in websocketOrStream) {
            return websocketOrStream;
        }

        await this.waitForWebSocketOpen(websocketOrStream);
        websocketOrStream.binaryType = 'arraybuffer';

        this.setupBinaryMessageHandler(websocketOrStream);
        return createStreamFromWebsocket(websocketOrStream);
    }

    private async waitForWebSocketOpen(ws: WebSocket): Promise<void> {
        if (ws.readyState === WebSocket.OPEN) return;

        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                reject(new Error("WebSocket connection timed out after 10 seconds"));
            }, 10000);

            ws.once("connect", () => {
                clearTimeout(timeout);
                resolve();
            });

            ws.once("error", (error) => {
                clearTimeout(timeout);
                reject(error);
            });
        });
    }

    private setupBinaryMessageHandler(ws: WebSocket): void {
        ws.on("message", async (data, isBinary) => {
            // Text/control messages often arrive as Buffer in Node ws.
            // Do not treat them as audio unless the WebSocket frame is actually binary.
            if (!isBinary) {
                return;
            }

            const buffer =
                data instanceof Buffer
                    ? data
                    : Array.isArray(data)
                        ? Buffer.concat(data)
                        : Buffer.from(data as ArrayBuffer);

            if (this.recording && this.firstAudioChunkAtMs === null) {
                this.firstAudioChunkAtMs = Date.now();
                console.log("[TIMING] first binary audio chunk delay ms:", this.recordingStartedAtMs ? this.firstAudioChunkAtMs - this.recordingStartedAtMs : null);
            }

            if (this.recording) {
                this.audioChunkCount++;
            }

            console.log("===Received binary audio message:", {
                size: buffer.length,
                recording: this.recording,
                chunkCount: this.audioChunkCount,
                t: Date.now()
            });

            // Ignore accidental/tiny packets such as old button-state frames.
            if (!this.recording) {
                return;
            }

            if (buffer.length < 256) {
                console.log("Ignoring tiny binary frame:", buffer.length);
                return;
            }

            await this.connection.handleIncomingAudio(buffer);
        });
    }

    private createToolsMap(): Record<string, StructuredTool> {
        return this.tools.reduce((acc: Record<string, StructuredTool>, tool) => {
            acc[tool.name] = tool;
            return acc;
        }, {});
    }

    private async initializeConnection(toolsByName: Record<string, StructuredTool>): Promise<void> {
        await this.connection.connect();

        const toolDefs = Object.values(toolsByName).map((tool) => ({
            type: "function",
            name: tool.name,
            description: tool.description,
            parameters: zodToJsonSchema(tool.schema as any),
        }));

        this.connection.sendEvent({
            type: "session.update",
            session: {
                type: "realtime",
                instructions: this.instructions,
                output_modalities: ["audio"],
                tools: toolDefs,
                tool_choice: "auto",
                audio: {
                    input: {
                        format: {
                            type: "audio/pcm",
                            rate: 24000
                        },
                        transcription: {
                            model: "whisper-1"
                        },
                        turn_detection: null
                    },
                    output: {
                        format: {
                            type: "audio/pcm",
                            rate: 24000
                        },
                        voice: "marin"
                    }
                }
            },
        });
    }

    private async handleStreamEvents(
        inputStream: AsyncGenerator<string> | AsyncGenerator<any>,
        toolExecutor: VoiceToolExecutor,
        sendOutputChunk: (chunk: string) => void | Promise<void>
    ): Promise<void> {
        const modelReceiveStream = this.connection.eventStream();

        for await (const [streamKey, dataRaw] of mergeStreams({
            input_mic: inputStream,
            output_speaker: modelReceiveStream,
            tool_outputs: toolExecutor.outputIterator(),
        })) {
            await this.processStreamEvent(streamKey, dataRaw, toolExecutor, sendOutputChunk);
        }
    }

    private async processStreamEvent(
        streamKey: string,
        dataRaw: any,
        toolExecutor: VoiceToolExecutor,
        sendOutputChunk: (chunk: string) => void | Promise<void>
    ): Promise<void> {
        const data = typeof dataRaw === "string" ? dataRaw : dataRaw;

        if (data === "START_RECORD") {
            console.log("[TIMING] text START_RECORD received at", Date.now());
            this.startRecordingSession();
        } else if (data === "STOP_RECORD") {
            console.log("[TIMING] text STOP_RECORD received at", Date.now());
            await this.stopRecordingAndProcessAudio();
        } else {
            await this.handleStreamOutput(streamKey, data, toolExecutor, sendOutputChunk);
        }
    }

    private async handleStreamOutput(
        streamKey: string,
        data: any,
        toolExecutor: VoiceToolExecutor,
        sendOutputChunk: (chunk: string) => void | Promise<void>
    ): Promise<void> {
        switch (streamKey) {
            case "tool_outputs":
                this.connection.sendEvent(data);
                this.connection.sendEvent({ type: "response.create", response: {} });
                break;

            case "output_speaker":
                await this.handleSpeakerOutput(data, toolExecutor, sendOutputChunk);
                break;
        }
    }


    private async handleSpeakerOutput(
        data: any,
        toolExecutor: VoiceToolExecutor,
        sendOutputChunk: (chunk: string) => void | Promise<void>
    ): Promise<void> {
        const { type } = data;
        /*
        if (type === "response.output_audio.delta") {
            // GA Realtime audio event
            await sendOutputChunk(JSON.stringify({
                type: "response.audio.delta",
                delta: data.delta
            }));
        */

        
        if (type === "response.output_audio.delta") {
             const pcm24k = Buffer.from(data.delta, "base64");
            const pcm16k = resamplePcm16Mono(pcm24k, 24000, 16000);
            await sendOutputChunk(JSON.stringify({
                type: "response.audio.delta",
            delta: pcm16k.toString("base64")
            }));
        
        } else if (type === "response.audio.delta") {
            // Older beta event name, kept for compatibility
            await sendOutputChunk(JSON.stringify(data));
        } else if (type === "response.output_audio.done") {
            console.log("Audio output done");
        } else if (type === "response.output_audio_transcript.done") {
            console.log("model:", data.transcript);
        } else if (type === "conversation.item.input_audio_transcription.completed") {
            console.log("user:", data.transcript);
        } else if (type === "error") {
            console.error("error:", data);
        } else if (type === "response.function_call_arguments.done") {
            toolExecutor.addToolCall(data);
        } else if (!EVENTS_TO_IGNORE.includes(type)) {
            console.log(type);
        }
    }

    /*
    private async handleSpeakerOutput(
        data: any,
        toolExecutor: VoiceToolExecutor,
        sendOutputChunk: (chunk: string) => void | Promise<void>
    ): Promise<void> {
        const { type } = data;

        if (type === "response.audio.delta" || type === "response.audio_buffer.speech_started") {
            await sendOutputChunk(JSON.stringify(data));
        } else if (type === "error") {
            console.error("error:", data);
        } else if (type === "response.function_call_arguments.done") {
            toolExecutor.addToolCall(data);
        } else if (type === "response.audio_transcript.done") {
            console.log("model:", data.transcript);
        } else if (type === "conversation.item.input_audio_transcription.completed") {
            console.log("user:", data.transcript);
        } else if (!EVENTS_TO_IGNORE.includes(type)) {
            console.log(type);
        }
    }
    */
}