import path from 'node:path';
import nodeGypBuild from 'node-gyp-build';

type NativeBindings = {
  startReceiver(options: Record<string, unknown>, handler: (event: RaopEvent) => void): number;
  stopReceiver(handle: number): void;
};

const bindings: NativeBindings = nodeGypBuild(path.join(__dirname, '..'));

export type StreamEvent = { type: 'stream'; port: number };
export type PlayEvent = { type: 'play' };
export type FlushEvent = { type: 'flush' };
export type PauseEvent = { type: 'pause' };
export type StopEvent = { type: 'stop' };
export type VolumeEvent = { type: 'volume'; value: number };
export type MetadataEvent = {
  type: 'metadata';
  title?: string;
  artist?: string;
  album?: string;
};
export type ArtworkEvent = {
  type: 'artwork';
  title?: string;
  artist?: string;
  album?: string;
  data: Buffer;
};
export type PcmEvent = {
  type: 'pcm';
  sampleRate: number;
  channels: number;
  data: Buffer;
};

export type RaopEvent =
  | StreamEvent
  | PlayEvent
  | FlushEvent
  | PauseEvent
  | StopEvent
  | VolumeEvent
  | MetadataEvent
  | ArtworkEvent
  | PcmEvent;

export type EventHandler = (event: RaopEvent) => void;

export type ReceiverOptions = {
  name?: string;
  model?: string;
  mac?: string;
  latencies?: string;
  metadata?: boolean;
  portBase?: number;
  portRange?: number;
  host?: string;
};

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
