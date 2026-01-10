Node bindings for the [libraop](https://github.com/philippe44/libraop) RAOP (AirPlay 1) receiver. The module bundles the native libraop sources and ships prebuilt binaries so consumers do not need a compiler on the target machine.

## Features
- Native RAOP receiver backed by libraop, exposed through a minimal Node API.
- Thread-safe event delivery into JavaScript for stream lifecycle, metadata, artwork, PCM frames, and volume.
- Prebuilt binaries for Linux (x64, arm64) and macOS (x64, arm64) produced by GitHub Actions.
- Bundled vendor sources for reproducible local builds when needed.

## Installation
```bash
npm install @lox-audioserver/node-libraop
```

Prebuilt `.node` binaries are downloaded via `node-gyp-build` at runtime. If a prebuild is not available for your platform, the package will fall back to building from the vendored sources.

## Usage
```ts
import { startReceiver, stopReceiver, RaopEvent } from '@lox-audioserver/node-libraop';

const handle = startReceiver(
  {
    name: 'My AirPlay Target',
    model: 'Node-Libraop',
    metadata: true,
    portBase: 6000,
    portRange: 100,
  },
  (event: RaopEvent) => {
    switch (event.type) {
      case 'stream':
        console.log(`Incoming RAOP stream on port ${event.port}`);
        break;
      case 'metadata':
        console.log(`Now playing: ${event.artist} - ${event.title} (${event.album})`);
        break;
      case 'pcm':
        // event.data is a Buffer with raw PCM samples
        break;
      case 'stop':
        console.log('Playback stopped');
        break;
    }
  }
);

process.on('SIGINT', () => {
  stopReceiver(handle);
  process.exit(0);
});
```

### Sending to an AirPlay target (PCM)
```ts
import { startSender, sendChunk, stopSender } from '@lox-audioserver/node-libraop';
import fs from 'node:fs';

const sender = startSender({ target: '192.168.1.50', port: 5000, sampleRate: 44100, channels: 2 });
const pcmStream = fs.createReadStream('audio.pcm'); // 16-bit, little endian, stereo
pcmStream.on('data', (chunk) => {
  // Try to enqueue the chunk; if not ready yet the data is skipped
  const result = sendChunk(sender, chunk);
  if (!result.sent) {
    console.warn('Sender not ready yet; waiting for queue to drain');
  }
});
pcmStream.on('end', () => stopSender(sender));
```

### Sender pacing and health
Use `getSenderState` to check connectivity and buffer depth before pushing audio:
```ts
import { startSender, getSenderState, sendChunk } from '@lox-audioserver/node-libraop';

const sender = startSender({ target: '192.168.1.50', port: 5000 });

function maybeSend(pcm: Buffer) {
  const state = getSenderState(sender);
  if (!state.connected) {
    console.warn('Not connected yet');
    return;
  }
  const result = sendChunk(sender, pcm);
  if (!result.sent && result.reason === 'not-ready') {
    // consider waiting result.latencyFrames / sampleRate seconds before retrying
  }
}
```

### API
- `startReceiver(options?, handler): number`  
  Starts the RAOP receiver. Returns a handle that you should pass to `stopReceiver`. The `handler` callback receives `RaopEvent` objects.

- `stopReceiver(handle): void`  
  Stops the receiver associated with the provided handle.

- `sendRemoteCommand(handle, command): boolean`  
  Sends a transport command (`play`, `pause`, `stop`, `next`, `prev`/`previous`) to the active sender if available.

- `startSender(options): number`  
  Connects to an AirPlay (RAOP) target and returns a handle used by `sendChunk`/`stopSender`.

- `sendChunk(handle, pcmBuffer): SendResult`  
  Attempts to enqueue a PCM chunk (16-bit, little endian). Returns whether it was sent, queue details, latency frames, and optional `reason` (`not-ready` or `disconnected`).

- `stopSender(handle): void`  
  Disconnects from the AirPlay target and frees resources.

- `getSenderState(handle): SenderState`  
  Returns connection status plus queue/latency stats without sending audio.

- `setLogHandler(handler?, level?): void`  
  Forward libraop native logs into JavaScript. Pass `null` to disable. Levels: `error`, `warn` (default), `info`, `debug`, `sdebug`. Optional per-channel override: `setLogHandler(fn, 'info', 'debug', 'warn')` sets default `info`, RAOP to `debug`, util to `warn`. Callback receives `{ level, source, timestamp, line }`.

### Options
All fields are optional; libraop defaults are applied when omitted.

#### Receiver options
| Option      | Type    | Default            | Description                                              |
| ----------- | ------- | ------------------ | -------------------------------------------------------- |
| `name`      | string  | `LoxAirplay`       | Friendly name advertised over RAOP.                      |
| `model`     | string  | `Lox-RAOP`         | Model identifier included in mDNS advertisements.        |
| `mac`       | string  | `00:11:22:33:44:55`| MAC-like identifier used for the hostname.               |
| `latencies` | string  | `1000:0`           | Latency configuration string passed to libraop.          |
| `metadata`  | boolean | `true`             | Whether to emit metadata/artwork events.                 |
| `portBase`  | number  | `6000`             | Base port for RAOP listener sockets.                     |
| `portRange` | number  | `100`              | Number of ports available for the listener pool.         |
| `host`      | string  | `0.0.0.0`          | Optional host override for binding and mDNS.             |

#### Sender options
| Option          | Type    | Default       | Description                                           |
| --------------- | ------- | ------------- | ----------------------------------------------------- |
| `target`        | string  | required      | Target IPv4 address.                                  |
| `port`          | number  | `5000`        | Target RTSP port.                                     |
| `sampleRate`    | number  | `44100`       | PCM sample rate in Hz.                                |
| `channels`      | number  | `2`           | PCM channel count.                                    |
| `sampleSize`    | number  | `2`           | PCM bytes per sample.                                 |
| `frameLength`   | number  | `352`         | Frames per chunk (bounded by libraop limits).         |
| `latencyFrames` | number  | `11025`       | Requested playback latency in frames.                 |
| `volume`        | number  | `50`          | Initial volume (0-100).                               |
| `et`            | string  | empty         | mDNS TXT `et` value for RTSP auth setup.              |
| `md`            | string  | empty         | mDNS TXT `md` value for metadata capability flags.    |
| `auth`          | boolean | `false`       | Whether RTSP auth is enabled.                         |
| `secret`        | string  | empty         | Pairing secret (mDNS TXT `pk`-derived).               |
| `passwd`        | string  | empty         | AirPlay password (mDNS TXT `pw`).                     |
| `local`         | string  | `0.0.0.0`     | Local bind IPv4 address.                              |

### Events
- `stream` — `{ port }`: Emitted when a new stream announces the data port.
- `play`, `pause`, `flush`, `stop`: Playback lifecycle events.
- `volume` — `{ value }`: AirPlay volume updates.
- `metadata` — `{ title?, artist?, album?, durationMs?, elapsedMs? }`: Track metadata (duration/elapsed in milliseconds when available).
- `artwork` — `{ data, title?, artist?, album? }`: Artwork bytes.
- `pcm` — `{ sampleRate, channels, data }`: Raw PCM frames (16-bit signed).

## Prebuilt binaries
Prebuilds are produced by `.github/workflows/prebuild.yml` on `workflow_dispatch` or when a GitHub release is published. The matrix covers:
- Linux: x64, arm64
- macOS: x64 (Intel), arm64 (Apple Silicon)
- Windows: not prebuilt yet (source build required)

Artifacts are uploaded to the workflow run for download, and attached to releases when triggered from a tag. To build a missing prebuild locally:
```bash
npm ci
npm run build
npm run build:prebuilds -- --arch <arch> --platform <linux|darwin>
```

## Building from source
You only need this if you are on an unsupported platform or hacking on the addon.

Prerequisites:
- Node.js 18+
- Python 3, make, C/C++ toolchain
- OpenSSL headers (`libssl-dev` on Debian/Ubuntu, `openssl` via Homebrew on macOS, `choco install openssl` on Windows)
- `git` (the build script fetches libraop from GitHub if `vendor/libraop` is missing)
Notes:
- Windows builds require the MSVC toolchain and currently must be built from source; prebuilds are not shipped yet.
- Vendored libraop contains `#warning` directives; for MSVC these are mapped to `#pragma message` to keep builds green.
- Before publishing, `npm run prune:vendor` strips unused vendor assets (codecs, binaries, fuzz corpora) to keep the tarball small.

Commands:
```bash
npm ci
npm run build:native   # compiles the .node binding from vendored sources
npm run build          # builds the TypeScript wrapper
npm run build:prebuilds -- --arch $(node -p "process.arch") --platform $(node -p "process.platform") # optional
```
The build uses a pinned libraop commit (`81c2182649da8645ac2a58b78e9f370c79a4165b`) and will clone it automatically if `vendor/libraop` is absent.

## Development
- Native sources live in `native/` and are built via `binding.gyp`.
- Libraop sources are vendored under `vendor/`; `scripts/prepare-libraop.sh` validates their presence before compiling.
- TypeScript wrapper lives in `src/` and compiles to `dist/`.
- Clean the workspace with `npm run clean`.
