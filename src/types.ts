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
  durationMs?: number;
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

export type SenderOptions = {
  target: string;
  port?: number;
  sampleRate?: number;
  channels?: number;
  sampleSize?: number;
  frameLength?: number;
  latencyFrames?: number;
  volume?: number;
};

export type SendResult = {
  sent: boolean;
  playtime?: number;
  queuedFrames: number;
  queueSize: number;
  latencyFrames: number;
  reason?: 'not-ready' | 'disconnected';
};

export type LogLevel = 'error' | 'warn' | 'info' | 'debug' | 'sdebug';

export type LogEntry = {
  level: LogLevel;
  source: string;
  timestamp: number;
  line: string;
};

export type SenderState = {
  connected: boolean;
  queuedFrames?: number;
  queueSize?: number;
  latencyFrames?: number;
};
