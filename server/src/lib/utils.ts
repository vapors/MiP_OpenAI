import WebSocket from "ws";
import fs from 'fs';
import decodeAudio from 'audio-decode';
import { AudioHandler } from "./agent";

/**
 * Merge multiple streams into one stream.
 */
export async function* mergeStreams<T>(
  streams: Record<string, AsyncGenerator<T>>
): AsyncGenerator<[string, T]> {
  // start the first iteration of each output iterator
  const tasks = new Map(
    Object.entries(streams).map(([key, stream], i) => {
      return [key, stream.next().then((result) => ({ key, stream, result }))];
    })
  );
  // yield chunks as they become available,
  // starting new iterations as needed,
  // until all iterators are done
  while (tasks.size) {
    const { key, result, stream } = await Promise.race(tasks.values());
    tasks.delete(key);
    if (!result.done) {
      yield [key, result.value];
      tasks.set(
        key,
        stream.next().then((result) => ({ key, stream, result }))
      );
    }
  }
}


export function resamplePcm16Mono(
  input: Buffer,
  sourceRate: number,
  targetRate: number
): Buffer {
  if (sourceRate === targetRate) return input;

  const inputSamples = new Int16Array(
    input.buffer,
    input.byteOffset,
    Math.floor(input.byteLength / 2)
  );

  const outputLength = Math.floor(inputSamples.length * targetRate / sourceRate);
  const output = Buffer.alloc(outputLength * 2);

  for (let i = 0; i < outputLength; i++) {
    const sourceIndex = i * sourceRate / targetRate;
    const index0 = Math.floor(sourceIndex);
    const index1 = Math.min(index0 + 1, inputSamples.length - 1);
    const frac = sourceIndex - index0;

    const sample =
      inputSamples[index0] * (1 - frac) +
      inputSamples[index1] * frac;

    output.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(sample))), i * 2);
  }

  return output;
}

export async function* createStreamFromWebsocket(ws: WebSocket) {
  const messageQueue: string[] = [];
  let resolveMessage: ((value: string | PromiseLike<string>) => void) | null = null;
  let rejectMessage: ((reason?: any) => void) | null = null;

  const onMessage = (data: WebSocket.Data, isBinary: boolean) => {
    // IMPORTANT:
    // The robot sends mic audio as binary frames. Those binary frames are handled
    // separately by agent.setupBinaryMessageHandler(). If we also convert them to
    // strings here, the input stream gets flooded with junk messages and can delay
    // START_RECORD / STOP_RECORD processing.
    if (isBinary) {
      return;
    }

    const message = Buffer.isBuffer(data) ? data.toString("utf8") : data.toString();

    if (resolveMessage) {
      resolveMessage(message);
      resolveMessage = null;
      rejectMessage = null;
    } else {
      messageQueue.push(message);
    }
  };

  const onError = (error: Error) => {
    if (rejectMessage) {
      rejectMessage(error);
      resolveMessage = null;
      rejectMessage = null;
    }
  };

  ws.on("message", onMessage);
  ws.on("error", onError);

  try {
    while (ws.readyState === WebSocket.OPEN) {
      let message: string;
      if (messageQueue.length > 0) {
        message = messageQueue.shift()!;
      } else {
        message = await new Promise<string>((resolve, reject) => {
          resolveMessage = resolve;
          rejectMessage = reject;
        });
      }

      try {
        yield JSON.parse(message);
      } catch (e) {
        yield message;
      }
    }
  } finally {
    ws.off("message", onMessage);
    ws.off("error", onError);
  }
}


export function createAsyncIterableFromWebSocket(ws: WebSocket): AsyncIterable<string> {
  return {
      [Symbol.asyncIterator]() {
          return {
              async next() {
                  try {
                      const value = await new Promise<string>((resolve, reject) => {
                          ws.addEventListener('message', (data) => {
                              if (data instanceof Buffer) {
                                  resolve(data.toString('base64'));
                              } else {
                                  resolve(data.toString());
                              }
                          });
                          ws.addEventListener('error', reject);
                          ws.addEventListener('close', () => resolve(''));
                      });
                      
                      return { done: value === '', value };
                  } catch (error) {
                      return { done: true, value: undefined };
                  }
              }
          };
      }
  };
}


// Converts Float32Array of audio data to PCM16 ArrayBuffer
function floatTo16BitPCM(float32Array: Float32Array): ArrayBuffer {
  const buffer = new ArrayBuffer(float32Array.length * 2);
  const view = new DataView(buffer);
  let offset = 0;
  for (let i = 0; i < float32Array.length; i++, offset += 2) {
    let s = Math.max(-1, Math.min(1, float32Array[i]));
    view.setInt16(offset, s < 0 ? s * 0x8000 : s * 0x7fff, true);
  }
  return buffer;
}

// Converts a Float32Array to base64-encoded PCM16 data
export function base64EncodeAudio(float32Array: Float32Array): string {
  const arrayBuffer = floatTo16BitPCM(float32Array);
  let binary = '';
  let bytes = new Uint8Array(arrayBuffer);
  const chunkSize = 0x8000; // 32KB chunk size
  for (let i = 0; i < bytes.length; i += chunkSize) {
    let chunk = bytes.subarray(i, i + chunkSize);
    binary += String.fromCharCode.apply(null, Array.from(chunk));
  }
  return btoa(binary);
}


export const appendToBuffer = (newData: Uint8Array, handler: AudioHandler): void => {
  const newBuffer = new Uint8Array(handler.buffer.length + newData.length);
  newBuffer.set(handler.buffer);
  newBuffer.set(newData, handler.buffer.length);
  handler.buffer = newBuffer;
};
export const processAudioBuffer = (buffer: Buffer): Buffer => {
  // Convert Int16 PCM to Float32
  // return buffer;
  const int16Data = new Int16Array(buffer.buffer);
  const float32Data = new Float32Array(int16Data.length);

  for (let i = 0; i < int16Data.length; i++) {
    float32Data[i] = int16Data[i] / 32768.0;
  }


  return Buffer.from(float32Data.buffer);
}

export function convertAudioToPCM16(audioFile: Buffer, sourceRate: number = 44100, targetRate: number = 24000): string {
  // Convert audio file to PCM16 24kHz mono format from 44.1kHz
  const audioBuffer = audioFile.buffer;
  const view = new DataView(audioBuffer);
  const ratio = sourceRate / targetRate;

  // Calculate resampled length
  const resampledLength = Math.floor(audioBuffer.byteLength / 2 / ratio);
  const pcm16Data = new Int16Array(resampledLength);

  // Simple linear interpolation resampling
  for (let i = 0; i < resampledLength; i++) {
    const sourceIndex = i * ratio;
    const sourceIndexInt = Math.floor(sourceIndex);
    const fraction = sourceIndex - sourceIndexInt;

    // Get samples for interpolation
    const sample1 = view.getInt16(sourceIndexInt * 2, true);
    const sample2 = sourceIndexInt < (audioBuffer.byteLength / 2 - 1) ?
      view.getInt16((sourceIndexInt + 1) * 2, true) : sample1;

    // Linear interpolation
    pcm16Data[i] = Math.round(sample1 * (1 - fraction) + sample2 * fraction);
  }

  // Convert to base64
  const uint8Array = new Uint8Array(pcm16Data.buffer);
  return Buffer.from(uint8Array).toString('base64');
}