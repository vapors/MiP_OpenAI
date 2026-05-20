import { AudioConfig, AudioManager } from "./audio";
import { convertAudioToPCM16, createStreamFromWebsocket } from "./utils";
import WebSocket from "ws";

const DEFAULT_MODEL = "gpt-realtime";
const DEFAULT_URL = "wss://api.openai.com/v1/realtime";
//const DEFAULT_MODEL = "gpt-realtime-2";
//const DEFAULT_URL = "wss://api.openai.com/v1/realtime?model=gpt-realtime-2";


const DEFAULT_AUDIO_CONFIG: AudioConfig = {
    sampleRate: 24000,  // ESP32 default sample rate
    channels: 1,        // Mono audio
    bitDepth: 16        // 16-bit PCM
};

class OpenAIWebSocketConnection {
    ws?: WebSocket;
    url: string;
    apiKey?: string;
    model: string;
    audioConfig: AudioConfig;
    private isRecording: boolean = true;
    private audioManager: AudioManager;

    constructor(params: {
        url?: string;
        apiKey?: string;
        model?: string;
        audioConfig?: AudioConfig;
        audioManager: AudioManager;
    }) {
        this.url = params.url ?? DEFAULT_URL;
        this.model = params.model ?? DEFAULT_MODEL;
        this.apiKey = params.apiKey ?? process.env.OPENAI_API_KEY;
        this.audioConfig = params.audioConfig ?? DEFAULT_AUDIO_CONFIG;
        this.audioManager = params.audioManager;
    }

    async connect() {
        const headers = {
            Authorization: `Bearer ${this.apiKey}`,
            //"OpenAI-Beta": "realtime=v1",
        };

        const finalUrl = `${this.url}?model=${this.model}`;
        this.ws = new WebSocket(finalUrl, { headers });
        await new Promise<void>((resolve, reject) => {
            if (!this.ws) {
                reject(new Error("WebSocket was not initialized"));
                return;
            }

            const timeout = setTimeout(() => {
                reject(new Error("Connection timed out after 10 seconds."));
            }, 10000);

            const onOpen = () => {
                clearTimeout(timeout);
                resolve();
            };

            const onError = (error: Error) => {
                clearTimeout(timeout);
                reject(error);
            };

            if (this.ws.readyState === WebSocket.OPEN) {
                onOpen();
            } else {
                this.ws.once("open", onOpen);
                this.ws.once("error", onError);
            }
        });
    }

    sendEvent(event: Record<string, unknown>) {
        const formattedEvent = JSON.stringify(event);
        if (this.ws === undefined) {
            throw new Error("Socket connection is not active, call .connect() first");
        }
        this.ws?.send(formattedEvent);
    }

    async *eventStream() {
        if (!this.ws) {
            throw new Error("Socket connection is not active, call .connect() first");
        }
        yield* createStreamFromWebsocket(this.ws);
    }


    handleButtonState(buttonState: boolean) {
        this.isRecording = buttonState;
        if (buttonState) {
            console.log('Started recording');
            // Optionally send a start recording event to OpenAI
            // this.sendEvent({
            //     type: "session.update",
            //     session: {
            //         recording_started: true
            //     }
            // });
        } else {
            console.log('Stopped recording');
            // Send end of audio stream marker
            // this.sendEvent({
            //   type: "input_audio_buffer.commit"
            // });
        }
    }

    handleIncomingAudio(data: Buffer) {
        this.audioManager.handleAudioBuffer(data);
    }
}

export { OpenAIWebSocketConnection };