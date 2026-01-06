import path from 'node:path';
import nodeGypBuild from 'node-gyp-build';
import { EventHandler, LogEntry, LogLevel, ReceiverOptions, RaopEvent, SendResult, SenderOptions, SenderState } from './types';

type NativeBindings = {
  startReceiver(options: Record<string, unknown>, handler: (event: RaopEvent) => void): number;
  stopReceiver(handle: number): void;
  startSender(options: Record<string, unknown>): number;
  stopSender(handle: number): void;
  sendChunk(handle: number, pcm: Buffer): SendResult;
  getSenderState(handle: number): SenderState;
  setLogHandler(handler: ((entry: LogEntry) => void) | null, level?: LogLevel, raopLevel?: LogLevel, utilLevel?: LogLevel): void;
};

const bindings: NativeBindings = nodeGypBuild(path.join(__dirname, '..'));

/**
 * Start a RAOP receiver. If only a handler is provided, libraop defaults are used.
 */
export function startReceiver(handler: EventHandler): number;
export function startReceiver(options: ReceiverOptions, handler: EventHandler): number;
export function startReceiver(optionsOrHandler: ReceiverOptions | EventHandler, handler?: EventHandler): number {
  if (typeof optionsOrHandler === 'function') {
    return bindings.startReceiver({}, optionsOrHandler);
  }
  if (typeof handler !== 'function') {
    throw new TypeError('startReceiver requires a callback handler');
  }
  return bindings.startReceiver(optionsOrHandler ?? {}, handler);
}

export function stopReceiver(handle: number): void {
  bindings.stopReceiver(handle);
}

/**
 * Connect to an AirPlay (RAOP) target for PCM sending.
 */
export function startSender(options: SenderOptions): number {
  return bindings.startSender(options);
}

/**
 * Attempt to enqueue a PCM chunk; returns queue/latency info and backpressure reason when not sent.
 */
export function sendChunk(handle: number, pcm: Buffer): SendResult {
  return bindings.sendChunk(handle, pcm);
}

export function stopSender(handle: number): void {
  bindings.stopSender(handle);
}

/**
 * Read-only sender health snapshot without sending audio.
 */
export function getSenderState(handle: number): SenderState {
  return bindings.getSenderState(handle);
}

/**
 * Forward native libraop logs into JavaScript; pass null to disable.
 */
export function setLogHandler(handler: ((entry: LogEntry) => void) | null, level: LogLevel = 'warn', raopLevel?: LogLevel, utilLevel?: LogLevel): void {
  bindings.setLogHandler(handler, level, raopLevel, utilLevel);
}
