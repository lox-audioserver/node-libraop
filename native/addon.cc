#include <napi.h>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

extern "C" {
#include "raop_server.h"
#include "raop_client.h"
#include "mdnssvc.h"
#include "cross_net.h"
#include "cross_ssl.h"
#include "cross_log.h"
}

namespace raop {

enum class EventType {
  Stream,
  Play,
  Flush,
  Pause,
  Stop,
  Volume,
  Metadata,
  Artwork,
  Pcm,
};

struct Event {
  EventType type{};
  uint32_t port{0};
  double volume{0};
  std::string title;
  std::string artist;
  std::string album;
  bool durationValid{false};
  uint32_t durationMs{0};
  bool elapsedValid{false};
  uint32_t elapsedMs{0};
  std::vector<uint8_t> artwork;
  std::vector<uint8_t> pcm;
  uint32_t pcmRate{44100};
  uint32_t pcmChannels{2};
};

struct Instance {
  std::mutex mutex;
  std::unique_ptr<Napi::ThreadSafeFunction> tsfn;
  raopsr_s* server{nullptr};
  mdnsd* mdns{nullptr};
  bool stopped{false};
};

static std::mutex g_instances_mutex;
static std::map<int, std::shared_ptr<Instance>> g_instances;
static int g_next_handle = 1;

struct SenderInstance {
  std::mutex mutex;
  raopcl_s* client{nullptr};
  uint32_t channels{2};
  uint32_t sampleSize{2};
};

static std::once_flag g_platform_once;
static std::mutex g_sender_mutex;
static std::map<int, std::shared_ptr<SenderInstance>> g_senders;
static int g_next_sender_handle = 10'000;
static std::mutex g_log_mutex;
static std::unique_ptr<Napi::ThreadSafeFunction> g_log_tsfn;
static std::once_flag g_log_cleanup_once;
static log_level g_default_level = lWARN;
static log_level g_util_level = lWARN;
static log_level g_raop_level = lWARN;
static std::once_flag g_sender_cleanup_once;
static std::once_flag g_receiver_cleanup_once;

extern "C" {
extern log_level util_loglevel;
extern log_level raop_loglevel;
}

static const char* LevelToString(log_level lvl) {
  switch (lvl) {
    case lERROR:
      return "error";
    case lWARN:
      return "warn";
    case lINFO:
      return "info";
    case lDEBUG:
      return "debug";
    case lSDEBUG:
      return "sdebug";
    default:
      return "warn";
  }
}

static log_level ParseLevel(const std::string& level) {
  if (level == "error") return lERROR;
  if (level == "warn") return lWARN;
  if (level == "info") return lINFO;
  if (level == "debug") return lDEBUG;
  if (level == "sdebug") return lSDEBUG;
  return lWARN;
}

static void ShutdownPlatform(void*) {
  cross_ssl_free();
  netsock_close();
}

static void CleanupSenders(void*) {
  std::lock_guard<std::mutex> guard(g_sender_mutex);
  for (auto& kv : g_senders) {
    auto inst = kv.second;
    if (!inst) continue;
    std::lock_guard<std::mutex> lock(inst->mutex);
    if (inst->client) {
      raopcl_disconnect(inst->client);
      raopcl_destroy(inst->client);
      inst->client = nullptr;
    }
  }
  g_senders.clear();
}

static void CleanupReceivers(void*) {
  std::lock_guard<std::mutex> guard(g_instances_mutex);
  for (auto& kv : g_instances) {
    auto inst = kv.second;
    if (!inst) continue;
    std::lock_guard<std::mutex> lock(inst->mutex);
    inst->stopped = true;
    if (inst->server) {
      raopsr_delete(inst->server);
      inst->server = nullptr;
    }
    if (inst->mdns) {
      mdnsd_stop(inst->mdns);
      inst->mdns = nullptr;
    }
    if (inst->tsfn) {
      inst->tsfn->Release();
      inst->tsfn.reset();
    }
  }
  g_instances.clear();
}

static void EnsurePlatformInitialized(Napi::Env env) {
  std::call_once(g_platform_once, [&]() {
    netsock_init();
    cross_ssl_load();
    env.AddCleanupHook([] { ShutdownPlatform(nullptr); });
    std::call_once(g_sender_cleanup_once, [&]() {
      env.AddCleanupHook([] { CleanupSenders(nullptr); });
    });
    std::call_once(g_receiver_cleanup_once, [&]() {
      env.AddCleanupHook([] { CleanupReceivers(nullptr); });
    });
  });
}

static void ClearLogHandler(void*) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  cross_set_logger(nullptr);
  if (g_log_tsfn) {
    g_log_tsfn->Release();
    g_log_tsfn.reset();
  }
}

static void LogSink(const char* source, const char* line, log_level lvl) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (!g_log_tsfn) return;
  std::string* msg = new std::string(line ? line : "");
  // capture a coarse timestamp for JS consumers
  uint64_t ts_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());
  napi_status status = g_log_tsfn->BlockingCall(
      msg, [lvl, ts_ms, source](Napi::Env env, Napi::Function cb, std::string* message) {
        Napi::Object obj = Napi::Object::New(env);
        obj.Set("level", LevelToString(lvl));
        obj.Set("source", Napi::String::New(env, source ? source : "unknown"));
        obj.Set("timestamp", Napi::Number::New(env, static_cast<double>(ts_ms)));
        obj.Set("line", Napi::String::New(env, *message));
        cb.Call({obj});
        delete message;
      });
  if (status != napi_ok) {
    delete msg;
  }
}

static void DispatchEvent(Instance* inst, Event evt) {
  if (!inst || !inst->tsfn) return;
  auto* payload = new Event(std::move(evt));
  napi_status status = inst->tsfn->BlockingCall(
      payload, [](Napi::Env env, Napi::Function jsCallback, Event* event) {
        Napi::Object obj = Napi::Object::New(env);
        switch (event->type) {
          case EventType::Stream:
            obj.Set("type", "stream");
            obj.Set("port", Napi::Number::New(env, event->port));
            break;
          case EventType::Play:
            obj.Set("type", "play");
            break;
          case EventType::Flush:
            obj.Set("type", "flush");
            break;
          case EventType::Pause:
            obj.Set("type", "pause");
            break;
          case EventType::Stop:
            obj.Set("type", "stop");
            break;
          case EventType::Volume:
            obj.Set("type", "volume");
            obj.Set("value", Napi::Number::New(env, event->volume));
            break;
          case EventType::Metadata:
            obj.Set("type", "metadata");
            if (!event->title.empty()) obj.Set("title", Napi::String::New(env, event->title));
            if (!event->artist.empty()) obj.Set("artist", Napi::String::New(env, event->artist));
            if (!event->album.empty()) obj.Set("album", Napi::String::New(env, event->album));
            if (event->durationValid) {
              obj.Set("durationMs", Napi::Number::New(env, event->durationMs));
            }
            if (event->elapsedValid) {
              obj.Set("elapsedMs", Napi::Number::New(env, event->elapsedMs));
            }
            break;
          case EventType::Artwork: {
            obj.Set("type", "artwork");
            if (!event->title.empty()) obj.Set("title", Napi::String::New(env, event->title));
            if (!event->artist.empty()) obj.Set("artist", Napi::String::New(env, event->artist));
            if (!event->album.empty()) obj.Set("album", Napi::String::New(env, event->album));
            Napi::Buffer<uint8_t> buf =
                Napi::Buffer<uint8_t>::Copy(env, event->artwork.data(),
                                            event->artwork.size());
            obj.Set("data", buf);
            break;
          }
          case EventType::Pcm: {
            obj.Set("type", "pcm");
            obj.Set("sampleRate", Napi::Number::New(env, event->pcmRate));
            obj.Set("channels", Napi::Number::New(env, event->pcmChannels));
            Napi::Buffer<uint8_t> buf =
                Napi::Buffer<uint8_t>::Copy(env, event->pcm.data(),
                                            event->pcm.size());
            obj.Set("data", buf);
            break;
          }
        }
        jsCallback.Call({obj});
        delete event;
      });
  if (status != napi_ok) {
    delete payload;
  }
}

static std::shared_ptr<SenderInstance> GetSenderInstance(int handle) {
  std::lock_guard<std::mutex> guard(g_sender_mutex);
  auto it = g_senders.find(handle);
  if (it != g_senders.end()) {
    return it->second;
  }
  return nullptr;
}

static void RaopCallback(void* owner, raopsr_event_t event, ...) {
  auto inst = static_cast<Instance*>(owner);
  if (!inst) return;
  std::lock_guard<std::mutex> lock(inst->mutex);
  if (inst->stopped) return;

  va_list args;
  va_start(args, event);
  switch (event) {
    case RAOP_STREAM: {
      uint32_t port = va_arg(args, uint32_t);
      Event evt{};
      evt.type = EventType::Stream;
      evt.port = port;
      DispatchEvent(inst, evt);
      break;
    }
    case RAOP_PLAY: {
      Event evt{};
      evt.type = EventType::Play;
      DispatchEvent(inst, evt);
      break;
    }
    case RAOP_FLUSH: {
      Event evt{};
      evt.type = EventType::Flush;
      DispatchEvent(inst, evt);
      break;
    }
    case RAOP_PAUSE: {
      Event evt{};
      evt.type = EventType::Pause;
      DispatchEvent(inst, evt);
      break;
    }
    case RAOP_STOP: {
      Event evt{};
      evt.type = EventType::Stop;
      DispatchEvent(inst, evt);
      break;
    }
    case RAOP_VOLUME: {
      double volume = va_arg(args, double);
      Event evt{};
      evt.type = EventType::Volume;
      evt.volume = volume;
      DispatchEvent(inst, evt);
      break;
    }
    case RAOP_METADATA: {
      raopsr_metadata_t* meta = va_arg(args, raopsr_metadata_t*);
      Event evt{};
      evt.type = EventType::Metadata;
      if (meta) {
        if (meta->title) evt.title = meta->title;
        if (meta->artist) evt.artist = meta->artist;
        if (meta->album) evt.album = meta->album;
        if (meta->duration_valid) {
          evt.durationValid = true;
          evt.durationMs = meta->duration_ms;
        }
        if (meta->elapsed_valid) {
          evt.elapsedValid = true;
          evt.elapsedMs = meta->elapsed_ms;
        }
      }
      DispatchEvent(inst, evt);
      break;
    }
    case RAOP_ARTWORK: {
      raopsr_metadata_t* meta = va_arg(args, raopsr_metadata_t*);
      uint8_t* data = va_arg(args, uint8_t*);
      size_t len = va_arg(args, size_t);
      Event evt{};
      evt.type = EventType::Artwork;
      if (meta) {
        if (meta->title) evt.title = meta->title;
        if (meta->artist) evt.artist = meta->artist;
        if (meta->album) evt.album = meta->album;
      }
      if (data && len > 0) {
        evt.artwork.assign(data, data + len);
      }
      DispatchEvent(inst, evt);
      break;
    }
#ifdef RAOP_PCM
    case RAOP_PCM: {
      uint8_t* data = va_arg(args, uint8_t*);
      size_t len = va_arg(args, size_t);
      Event evt{};
      evt.type = EventType::Pcm;
      if (data && len > 0) {
        evt.pcm.assign(data, data + len);
      }
      DispatchEvent(inst, evt);
      break;
    }
#endif
    default:
      break;
  }
  va_end(args);
}

static unsigned char HexByte(const std::string& hex, size_t idx) {
  auto hexval = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  return static_cast<unsigned char>((hexval(hex[idx]) << 4) | hexval(hex[idx + 1]));
}

static void ParseMac(const std::string& macStr, unsigned char mac[6]) {
  std::string hex;
  hex.reserve(12);
  for (char c : macStr) {
    if (std::isxdigit(static_cast<unsigned char>(c))) hex.push_back(c);
  }
  if (hex.size() < 12) {
    hex.append(12 - hex.size(), '0');
  }
  for (size_t i = 0; i < 6; i++) {
    mac[i] = HexByte(hex, i * 2);
  }
}

Napi::Value Start(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() != 2 || !info[0].IsObject() || !info[1].IsFunction()) {
    Napi::TypeError::New(env, "startReceiver(options, callback) expected").ThrowAsJavaScriptException();
    return env.Null();
  }
#ifdef _WIN32
  static bool wsa_initialized = false;
  if (!wsa_initialized) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      Napi::Error::New(env, "WSAStartup failed").ThrowAsJavaScriptException();
      return env.Null();
    }
    wsa_initialized = true;
  }
#endif
  Napi::Object opts = info[0].As<Napi::Object>();
  Napi::Function jsCb = info[1].As<Napi::Function>();

  std::string name = opts.Has("name") ? opts.Get("name").ToString().Utf8Value() : "LoxAirplay";
  std::string model = opts.Has("model") ? opts.Get("model").ToString().Utf8Value() : "Lox-RAOP";
  std::string macStr = opts.Has("mac") ? opts.Get("mac").ToString().Utf8Value() : "00:11:22:33:44:55";
  std::string latencies = opts.Has("latencies") ? opts.Get("latencies").ToString().Utf8Value() : "1000:0";
  bool metadata = opts.Has("metadata") ? opts.Get("metadata").ToBoolean().Value() : true;
  uint32_t portBaseVal = opts.Has("portBase") ? opts.Get("portBase").ToNumber().Uint32Value() : 6000;
  uint32_t portRangeVal = opts.Has("portRange") ? opts.Get("portRange").ToNumber().Uint32Value() : 100;
  int httpLength = -3;  // chunked
  std::string hostOverride = opts.Has("host") ? opts.Get("host").ToString().Utf8Value() : "";

  if (portBaseVal > 65535 || portRangeVal > 65535) {
    Napi::TypeError::New(env, "portBase/portRange must be <= 65535").ThrowAsJavaScriptException();
    return env.Null();
  }
  unsigned short portBase = static_cast<unsigned short>(portBaseVal);
  unsigned short portRange = static_cast<unsigned short>(portRangeVal);

  unsigned char mac[6];
  ParseMac(macStr, mac);

  in_addr host{};
  host.s_addr = htonl(INADDR_ANY);
  if (!hostOverride.empty()) {
    in_addr parsed{};
    if (inet_pton(AF_INET, hostOverride.c_str(), &parsed) == 1) {
      host = parsed;
    }
  }

  auto macToHex = [](const unsigned char addr[6]) {
    char buf[13]{};
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return std::string(buf);
  };

  auto inst = std::make_shared<Instance>();
  inst->tsfn = std::make_unique<Napi::ThreadSafeFunction>(
      Napi::ThreadSafeFunction::New(env, jsCb, "raop_events", 0, 1,
                                    [inst](Napi::Env) {
                                      std::lock_guard<std::mutex> lock(inst->mutex);
                                      inst->tsfn.reset();
                                    }));

  inst->mdns = mdnsd_start(host, false);
  if (!inst->mdns) {
    Napi::Error::New(env, "mdnsd_start failed").ThrowAsJavaScriptException();
    return env.Null();
  }
  // Ensure hostname is set for mdns responder
  const std::string hostLabel = macToHex(mac);
  const std::string hostname = hostLabel + ".local";
  mdnsd_set_hostname(inst->mdns, hostname.c_str(), host);

  inst->server = raopsr_create(host, inst->mdns, const_cast<char*>(name.c_str()),
                               const_cast<char*>(model.c_str()), mac,
                               const_cast<char*>("wav"), metadata,
                               false /*drift*/, true /*flush*/, const_cast<char*>(latencies.c_str()),
                               inst.get(), &RaopCallback, nullptr,
                               portBase, portRange, httpLength);
  if (!inst->server) {
    mdnsd_stop(inst->mdns);
    Napi::Error::New(env, "raopsr_create failed").ThrowAsJavaScriptException();
    return env.Null();
  }

  int handle;
  {
    std::lock_guard<std::mutex> guard(g_instances_mutex);
    handle = g_next_handle++;
    g_instances[handle] = inst;
  }

  return Napi::Number::New(env, handle);
}

Napi::Value Stop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "stopReceiver(handle) expected").ThrowAsJavaScriptException();
    return env.Null();
  }
  int handle = info[0].ToNumber().Int32Value();
  std::shared_ptr<Instance> inst;
  {
    std::lock_guard<std::mutex> guard(g_instances_mutex);
    auto it = g_instances.find(handle);
    if (it != g_instances.end()) {
      inst = it->second;
      g_instances.erase(it);
    }
  }
  if (!inst) return env.Null();

  {
    std::lock_guard<std::mutex> lock(inst->mutex);
    inst->stopped = true;
    if (inst->server) {
      raopsr_delete(inst->server);
      inst->server = nullptr;
    }
    if (inst->mdns) {
      mdnsd_stop(inst->mdns);
      inst->mdns = nullptr;
    }
  }
  if (inst->tsfn) {
    inst->tsfn->Release();
    inst->tsfn.reset();
  }
  return env.Null();
}

Napi::Value SendRemoteCommand(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsString()) {
    Napi::TypeError::New(env, "sendRemoteCommand(handle, command) expected").ThrowAsJavaScriptException();
    return env.Null();
  }
  int handle = info[0].ToNumber().Int32Value();
  std::string command = info[1].ToString().Utf8Value();

  std::shared_ptr<Instance> inst;
  {
    std::lock_guard<std::mutex> guard(g_instances_mutex);
    auto it = g_instances.find(handle);
    if (it != g_instances.end()) {
      inst = it->second;
    }
  }
  if (!inst) {
    Napi::Error::New(env, "unknown receiver handle").ThrowAsJavaScriptException();
    return env.Null();
  }

  const char* remoteCommand = nullptr;
  if (command == "play") {
    remoteCommand = "play";
  } else if (command == "pause") {
    remoteCommand = "pause";
  } else if (command == "stop") {
    remoteCommand = "stop";
  } else if (command == "next") {
    remoteCommand = "nextitem";
  } else if (command == "prev" || command == "previous") {
    remoteCommand = "previtem";
  } else {
    Napi::TypeError::New(env, "unsupported command").ThrowAsJavaScriptException();
    return env.Null();
  }

  bool sent = false;
  {
    std::lock_guard<std::mutex> lock(inst->mutex);
    if (!inst->server) {
      Napi::Error::New(env, "receiver is closed").ThrowAsJavaScriptException();
      return env.Null();
    }
    sent = raopsr_remote_command(inst->server, remoteCommand);
  }

  return Napi::Boolean::New(env, sent);
}

Napi::Value StartSender(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "startSender(options) expected").ThrowAsJavaScriptException();
    return env.Null();
  }

  EnsurePlatformInitialized(env);

  Napi::Object opts = info[0].As<Napi::Object>();
  if (!opts.Has("target")) {
    Napi::TypeError::New(env, "startSender requires a target IPv4 address").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string target = opts.Get("target").ToString().Utf8Value();
  uint32_t port = opts.Has("port") ? opts.Get("port").ToNumber().Uint32Value() : 5000;
  uint32_t sampleRate = opts.Has("sampleRate") ? opts.Get("sampleRate").ToNumber().Uint32Value() : 44100;
  uint32_t channels = opts.Has("channels") ? opts.Get("channels").ToNumber().Uint32Value() : 2;
  uint32_t sampleSize = opts.Has("sampleSize") ? opts.Get("sampleSize").ToNumber().Uint32Value() : 2;
  int frameLen = opts.Has("frameLength") ? static_cast<int>(opts.Get("frameLength").ToNumber().Uint32Value()) : DEFAULT_FRAMES_PER_CHUNK;
  int latencyFrames = opts.Has("latencyFrames") ? static_cast<int>(opts.Get("latencyFrames").ToNumber().Uint32Value()) : RAOP_LATENCY_MIN;
  int volume = opts.Has("volume") ? static_cast<int>(opts.Get("volume").ToNumber().Int32Value()) : 50;
  std::string et = opts.Has("et") ? opts.Get("et").ToString().Utf8Value() : "";
  std::string md = opts.Has("md") ? opts.Get("md").ToString().Utf8Value() : "";
  bool auth = opts.Has("auth") ? opts.Get("auth").ToBoolean().Value() : false;
  std::string secret = opts.Has("secret") ? opts.Get("secret").ToString().Utf8Value() : "";
  std::string passwd = opts.Has("passwd") ? opts.Get("passwd").ToString().Utf8Value() : "";
  std::string localStr = opts.Has("local") ? opts.Get("local").ToString().Utf8Value() : "";
  std::string dacpId = opts.Has("dacpId") ? opts.Get("dacpId").ToString().Utf8Value() : "";
  std::string activeRemote = opts.Has("activeRemote") ? opts.Get("activeRemote").ToString().Utf8Value() : "";
  // Codec selection: real AirPlay/RAOP devices generally require ALAC and reject
  // raw PCM (RTSP 406). Default to ALAC; allow "pcm" for receivers that need it.
  std::string codecName = opts.Has("codec") ? opts.Get("codec").ToString().Utf8Value() : "alac";
  raop_codec_t codec = (codecName == "pcm") ? RAOP_PCM : RAOP_ALAC;

  if (frameLen < 1) frameLen = 1;
  if (frameLen > MAX_FRAMES_PER_CHUNK) frameLen = MAX_FRAMES_PER_CHUNK;
  if (port == 0 || port > 65535) {
    Napi::TypeError::New(env, "port must be between 1 and 65535").ThrowAsJavaScriptException();
    return env.Null();
  }
  if (sampleSize == 0) {
    Napi::TypeError::New(env, "sampleSize must be > 0 bytes").ThrowAsJavaScriptException();
    return env.Null();
  }
  if (channels == 0) {
    Napi::TypeError::New(env, "channels must be > 0").ThrowAsJavaScriptException();
    return env.Null();
  }
  if (sampleRate == 0) {
    Napi::TypeError::New(env, "sampleRate must be > 0").ThrowAsJavaScriptException();
    return env.Null();
  }

  in_addr peer{};
  if (inet_pton(AF_INET, target.c_str(), &peer) != 1) {
    Napi::TypeError::New(env, "target must be an IPv4 address").ThrowAsJavaScriptException();
    return env.Null();
  }

  in_addr local{};
  local.s_addr = htonl(INADDR_ANY);
  if (!localStr.empty() && inet_pton(AF_INET, localStr.c_str(), &local) != 1) {
    Napi::TypeError::New(env, "local must be an IPv4 address").ThrowAsJavaScriptException();
    return env.Null();
  }

  auto inst = std::make_shared<SenderInstance>();
  inst->channels = channels;
  inst->sampleSize = sampleSize;

  char* etPtr = et.empty() ? nullptr : const_cast<char*>(et.c_str());
  char* mdPtr = md.empty() ? nullptr : const_cast<char*>(md.c_str());
  char* secretPtr = secret.empty() ? nullptr : const_cast<char*>(secret.c_str());
  char* passwdPtr = passwd.empty() ? nullptr : const_cast<char*>(passwd.c_str());
  char* dacpPtr = dacpId.empty() ? nullptr : const_cast<char*>(dacpId.c_str());
  char* activeRemotePtr = activeRemote.empty() ? nullptr : const_cast<char*>(activeRemote.c_str());

  // libraop wants the sample size in BITS (used for the SDP bit-depth field and
  // the ALAC encoder's mBitsPerChannel); our option is bytes-per-sample, so x8.
  // inst->sampleSize stays in bytes for sendChunk frame-alignment math.
  inst->client = raopcl_create(local, 0, 0, dacpPtr, activeRemotePtr,
                               codec, frameLen, latencyFrames,
                               RAOP_CLEAR, auth, secretPtr, passwdPtr, etPtr, mdPtr,
                               sampleRate, sampleSize * 8, channels, raopcl_float_volume(volume));
  if (!inst->client) {
    Napi::Error::New(env, "raopcl_create failed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (!raopcl_connect(inst->client, peer, static_cast<uint16_t>(port), true)) {
    raopcl_destroy(inst->client);
    inst->client = nullptr;
    Napi::Error::New(env, "raopcl_connect failed").ThrowAsJavaScriptException();
    return env.Null();
  }

  int handle;
  {
    std::lock_guard<std::mutex> guard(g_sender_mutex);
    handle = g_next_sender_handle++;
    g_senders[handle] = inst;
  }

  return Napi::Number::New(env, handle);
}

Napi::Value StopSender(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "stopSender(handle) expected").ThrowAsJavaScriptException();
    return env.Null();
  }
  int handle = info[0].ToNumber().Int32Value();
  std::shared_ptr<SenderInstance> inst;
  {
    std::lock_guard<std::mutex> guard(g_sender_mutex);
    auto it = g_senders.find(handle);
    if (it != g_senders.end()) {
      inst = it->second;
      g_senders.erase(it);
    }
  }
  if (!inst) return env.Null();

  std::lock_guard<std::mutex> lock(inst->mutex);
  if (inst->client) {
    raopcl_disconnect(inst->client);
    raopcl_destroy(inst->client);
    inst->client = nullptr;
  }
  return env.Null();
}

Napi::Value SendChunk(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsBuffer()) {
    Napi::TypeError::New(env, "sendChunk(handle, pcmBuffer) expected").ThrowAsJavaScriptException();
    return env.Null();
  }
  int handle = info[0].ToNumber().Int32Value();
  Napi::Buffer<uint8_t> buf = info[1].As<Napi::Buffer<uint8_t>>();

  std::shared_ptr<SenderInstance> inst = GetSenderInstance(handle);
  if (!inst) {
    Napi::Error::New(env, "unknown sender handle").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::lock_guard<std::mutex> lock(inst->mutex);
  if (!inst->client) {
    Napi::Error::New(env, "sender is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  size_t frameBytes = static_cast<size_t>(inst->channels) * inst->sampleSize;
  if (frameBytes == 0 || buf.Length() % frameBytes != 0) {
    Napi::TypeError::New(env, "PCM buffer must be aligned to frame size").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("sent", Napi::Boolean::New(env, false));
  result.Set("queuedFrames", Napi::Number::New(env, raopcl_queued_frames(inst->client)));
  result.Set("queueSize", Napi::Number::New(env, raopcl_queue_len(inst->client)));
  result.Set("latencyFrames", Napi::Number::New(env, raopcl_latency(inst->client)));

  if (!raopcl_accept_frames(inst->client)) {
    result.Set("reason", Napi::String::New(env, "not-ready"));
    if (!raopcl_is_connected(inst->client)) {
      result.Set("reason", Napi::String::New(env, "disconnected"));
    }
    return result;
  }

  uint64_t playtime = 0;
  int frames = static_cast<int>(buf.Length() / frameBytes);
  bool ok = raopcl_send_chunk(inst->client, buf.Data(), frames, &playtime);

  result.Set("sent", Napi::Boolean::New(env, ok));
  result.Set("queuedFrames", Napi::Number::New(env, raopcl_queued_frames(inst->client)));
  result.Set("queueSize", Napi::Number::New(env, raopcl_queue_len(inst->client)));
  result.Set("latencyFrames", Napi::Number::New(env, raopcl_latency(inst->client)));
  if (ok) {
    result.Set("playtime", Napi::Number::New(env, static_cast<double>(playtime)));
  }
  return result;
}

Napi::Value GetSenderState(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "getSenderState(handle) expected").ThrowAsJavaScriptException();
    return env.Null();
  }
  int handle = info[0].ToNumber().Int32Value();
  std::shared_ptr<SenderInstance> inst = GetSenderInstance(handle);
  if (!inst) {
    Napi::Error::New(env, "unknown sender handle").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::lock_guard<std::mutex> lock(inst->mutex);
  Napi::Object result = Napi::Object::New(env);
  bool connected = inst->client && raopcl_is_connected(inst->client);
  result.Set("connected", Napi::Boolean::New(env, connected));
  if (inst->client) {
    result.Set("queuedFrames", Napi::Number::New(env, raopcl_queued_frames(inst->client)));
    result.Set("queueSize", Napi::Number::New(env, raopcl_queue_len(inst->client)));
    result.Set("latencyFrames", Napi::Number::New(env, raopcl_latency(inst->client)));
  }
  return result;
}

Napi::Value SenderControl(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsString()) {
    Napi::TypeError::New(env, "senderControl(handle, command) expected").ThrowAsJavaScriptException();
    return env.Null();
  }
  int handle = info[0].ToNumber().Int32Value();
  std::string command = info[1].ToString().Utf8Value();

  std::shared_ptr<SenderInstance> inst = GetSenderInstance(handle);
  if (!inst) {
    Napi::Error::New(env, "unknown sender handle").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::lock_guard<std::mutex> lock(inst->mutex);
  if (!inst->client) {
    Napi::Error::New(env, "sender is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  bool ok = true;
  if (command == "pause") {
    raopcl_pause(inst->client);
    raopcl_flush(inst->client);
  } else if (command == "flush") {
    // Drop the device's buffered audio without pausing; streaming resumes on the
    // next frames (re-anchored by raopcl_accept_frames). Used for track-change /
    // seek so the new audio is heard promptly instead of after the old tail.
    raopcl_flush(inst->client);
  } else if (command == "stop") {
    raopcl_stop(inst->client);
  } else if (command == "play") {
    uint64_t now = raopcl_get_ntp(nullptr);
    uint64_t start_at =
        now + MS2NTP(200) - TS2NTP(raopcl_latency(inst->client), raopcl_sample_rate(inst->client));
    ok = raopcl_start_at(inst->client, start_at);
  } else {
    Napi::TypeError::New(env, "unsupported sender command").ThrowAsJavaScriptException();
    return env.Null();
  }

  return Napi::Boolean::New(env, ok);
}

Napi::Value SetSenderVolume(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "setSenderVolume(handle, volume) expected").ThrowAsJavaScriptException();
    return env.Null();
  }
  int handle = info[0].ToNumber().Int32Value();
  int volume = info[1].ToNumber().Int32Value();
  if (volume < 0 || volume > 100) {
    Napi::TypeError::New(env, "volume must be between 0 and 100").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::shared_ptr<SenderInstance> inst = GetSenderInstance(handle);
  if (!inst) {
    Napi::Error::New(env, "unknown sender handle").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::lock_guard<std::mutex> lock(inst->mutex);
  if (!inst->client) {
    Napi::Error::New(env, "sender is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  bool ok = raopcl_set_volume(inst->client, raopcl_float_volume(volume));
  return Napi::Boolean::New(env, ok);
}

Napi::Value SetSenderProgress(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "setSenderProgress(handle, elapsedMs, durationMs) expected")
        .ThrowAsJavaScriptException();
    return env.Null();
  }
  int handle = info[0].ToNumber().Int32Value();
  uint32_t elapsedMs = info[1].ToNumber().Uint32Value();
  uint32_t durationMs = info[2].ToNumber().Uint32Value();

  std::shared_ptr<SenderInstance> inst = GetSenderInstance(handle);
  if (!inst) {
    Napi::Error::New(env, "unknown sender handle").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::lock_guard<std::mutex> lock(inst->mutex);
  if (!inst->client) {
    Napi::Error::New(env, "sender is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  bool ok = raopcl_set_progress_ms(inst->client, elapsedMs, durationMs);
  return Napi::Boolean::New(env, ok);
}

Napi::Value SetSenderMetadata(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsObject()) {
    Napi::TypeError::New(env, "setSenderMetadata(handle, metadata) expected").ThrowAsJavaScriptException();
    return env.Null();
  }
  int handle = info[0].ToNumber().Int32Value();
  Napi::Object meta = info[1].As<Napi::Object>();

  std::string title = meta.Has("title") ? meta.Get("title").ToString().Utf8Value() : "";
  std::string artist = meta.Has("artist") ? meta.Get("artist").ToString().Utf8Value() : "";
  std::string album = meta.Has("album") ? meta.Get("album").ToString().Utf8Value() : "";

  std::shared_ptr<SenderInstance> inst = GetSenderInstance(handle);
  if (!inst) {
    Napi::Error::New(env, "unknown sender handle").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::lock_guard<std::mutex> lock(inst->mutex);
  if (!inst->client) {
    Napi::Error::New(env, "sender is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  bool ok = raopcl_set_daap(inst->client, 4,
                            "minm", 's', title.c_str(),
                            "asar", 's', artist.c_str(),
                            "asal", 's', album.c_str(),
                            "astn", 'i', 1);
  return Napi::Boolean::New(env, ok);
}

Napi::Value SetSenderArtwork(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsString() || !info[2].IsBuffer()) {
    Napi::TypeError::New(env, "setSenderArtwork(handle, contentType, data) expected")
        .ThrowAsJavaScriptException();
    return env.Null();
  }
  int handle = info[0].ToNumber().Int32Value();
  std::string contentType = info[1].ToString().Utf8Value();
  Napi::Buffer<uint8_t> buf = info[2].As<Napi::Buffer<uint8_t>>();

  std::shared_ptr<SenderInstance> inst = GetSenderInstance(handle);
  if (!inst) {
    Napi::Error::New(env, "unknown sender handle").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::lock_guard<std::mutex> lock(inst->mutex);
  if (!inst->client) {
    Napi::Error::New(env, "sender is closed").ThrowAsJavaScriptException();
    return env.Null();
  }
  if (contentType.empty()) {
    Napi::TypeError::New(env, "contentType must be a non-empty string").ThrowAsJavaScriptException();
    return env.Null();
  }

  bool ok = raopcl_set_artwork(inst->client, const_cast<char*>(contentType.c_str()),
                               static_cast<int>(buf.Length()),
                               reinterpret_cast<char*>(buf.Data()));
  return Napi::Boolean::New(env, ok);
}

Napi::Value SendKeepAlive(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "sendKeepAlive(handle) expected").ThrowAsJavaScriptException();
    return env.Null();
  }
  int handle = info[0].ToNumber().Int32Value();

  std::shared_ptr<SenderInstance> inst = GetSenderInstance(handle);
  if (!inst) {
    Napi::Error::New(env, "unknown sender handle").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::lock_guard<std::mutex> lock(inst->mutex);
  if (!inst->client) {
    Napi::Error::New(env, "sender is closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  bool ok = raopcl_keepalive(inst->client);
  return Napi::Boolean::New(env, ok);
}

Napi::Value PairWithAppleTv(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() > 0 && !info[0].IsUndefined() && !info[0].IsNull()) {
    Napi::TypeError::New(env, "pairWithAppleTv() expected").ThrowAsJavaScriptException();
    return env.Null();
  }

  EnsurePlatformInitialized(env);

  char* udn = nullptr;
  char* secret = nullptr;
  bool ok = AppleTVpairing(nullptr, &udn, &secret);

  Napi::Object result = Napi::Object::New(env);
  result.Set("ok", Napi::Boolean::New(env, ok));
  if (udn) {
    result.Set("udn", Napi::String::New(env, udn));
    free(udn);
  }
  if (secret) {
    result.Set("secret", Napi::String::New(env, secret));
    free(secret);
  }
  return result;
}

Napi::Value PairWithAppleTvByIp(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "pairWithAppleTvByIp(targetIp, port?) expected").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string targetIp = info[0].ToString().Utf8Value();
  int port = 7000;
  if (info.Length() > 1 && info[1].IsNumber()) {
    port = info[1].ToNumber().Int32Value();
  }

  if (targetIp.empty()) {
    Napi::TypeError::New(env, "targetIp must be a non-empty string").ThrowAsJavaScriptException();
    return env.Null();
  }

  EnsurePlatformInitialized(env);

  char* secret = nullptr;
  bool ok = AppleTVpairingByIp(nullptr, &secret, targetIp.c_str(), port);

  Napi::Object result = Napi::Object::New(env);
  result.Set("ok", Napi::Boolean::New(env, ok));
  if (secret) {
    result.Set("secret", Napi::String::New(env, secret));
    free(secret);
  }
  return result;
}

Napi::Value SetLogHandler(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Function cb;
  std::string levelStr = "warn";
  std::string raopLevelStr;
  std::string utilLevelStr;

  if (info.Length() > 0 && !info[0].IsUndefined() && !info[0].IsNull()) {
    if (!info[0].IsFunction()) {
      Napi::TypeError::New(env, "setLogHandler(handler?, level?) handler must be a function").ThrowAsJavaScriptException();
      return env.Null();
    }
    cb = info[0].As<Napi::Function>();
  }
  if (info.Length() > 1 && info[1].IsString()) {
    levelStr = info[1].ToString().Utf8Value();
  }
  if (info.Length() > 2 && info[2].IsString()) {
    raopLevelStr = info[2].ToString().Utf8Value();
  }
  if (info.Length() > 3 && info[3].IsString()) {
    utilLevelStr = info[3].ToString().Utf8Value();
  }

  g_default_level = ParseLevel(levelStr);
  g_raop_level = raopLevelStr.empty() ? g_default_level : ParseLevel(raopLevelStr);
  g_util_level = utilLevelStr.empty() ? g_default_level : ParseLevel(utilLevelStr);

  util_loglevel = g_util_level;
  raop_loglevel = g_raop_level;
  cross_set_levels(g_util_level, g_raop_level);

  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_log_tsfn) {
    g_log_tsfn->Release();
    g_log_tsfn.reset();
  }

  if (cb) {
    g_log_tsfn = std::make_unique<Napi::ThreadSafeFunction>(
        Napi::ThreadSafeFunction::New(env, cb, "raop_log", 0, 1));
    cross_set_logger(&LogSink);
    std::call_once(g_log_cleanup_once, [&]() {
      env.AddCleanupHook([] { ClearLogHandler(nullptr); });
    });
  } else {
    cross_set_logger(nullptr);
  }

  return env.Null();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("startReceiver", Napi::Function::New(env, Start));
  exports.Set("stopReceiver", Napi::Function::New(env, Stop));
  exports.Set("sendRemoteCommand", Napi::Function::New(env, SendRemoteCommand));
  exports.Set("startSender", Napi::Function::New(env, StartSender));
  exports.Set("stopSender", Napi::Function::New(env, StopSender));
  exports.Set("sendChunk", Napi::Function::New(env, SendChunk));
  exports.Set("getSenderState", Napi::Function::New(env, GetSenderState));
  exports.Set("senderControl", Napi::Function::New(env, SenderControl));
  exports.Set("setSenderVolume", Napi::Function::New(env, SetSenderVolume));
  exports.Set("setSenderProgress", Napi::Function::New(env, SetSenderProgress));
  exports.Set("setSenderMetadata", Napi::Function::New(env, SetSenderMetadata));
  exports.Set("setSenderArtwork", Napi::Function::New(env, SetSenderArtwork));
  exports.Set("sendKeepAlive", Napi::Function::New(env, SendKeepAlive));
  exports.Set("pairWithAppleTv", Napi::Function::New(env, PairWithAppleTv));
  exports.Set("pairWithAppleTvByIp", Napi::Function::New(env, PairWithAppleTvByIp));
  exports.Set("setLogHandler", Napi::Function::New(env, SetLogHandler));
  return exports;
}

}  // namespace raop

static Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
  return raop::Init(env, exports);
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, InitAll)
