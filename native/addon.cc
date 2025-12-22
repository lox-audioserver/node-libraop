#include <napi.h>
#include <cctype>
#include <cstdarg>
#include <cstring>
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
#include "mdnssvc.h"
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

static void DispatchEvent(Instance* inst, Event evt) {
  if (!inst || !inst->tsfn) return;
  napi_status status = inst->tsfn->BlockingCall(
      new Event(std::move(evt)),
      [](Napi::Env env, Napi::Function jsCallback, Event* event) {
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
            obj.Set("title", Napi::String::New(env, event->title));
            obj.Set("artist", Napi::String::New(env, event->artist));
            obj.Set("album", Napi::String::New(env, event->album));
            break;
          case EventType::Artwork: {
            obj.Set("type", "artwork");
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
  (void)status;
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
  unsigned short portBase = opts.Has("portBase") ? static_cast<unsigned short>(opts.Get("portBase").ToNumber().Uint32Value()) : 6000;
  unsigned short portRange = opts.Has("portRange") ? static_cast<unsigned short>(opts.Get("portRange").ToNumber().Uint32Value()) : 100;
  int httpLength = -3;  // chunked
  std::string hostOverride = opts.Has("host") ? opts.Get("host").ToString().Utf8Value() : "";

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

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("startReceiver", Napi::Function::New(env, Start));
  exports.Set("stopReceiver", Napi::Function::New(env, Stop));
  return exports;
}

}  // namespace raop

static Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
  return raop::Init(env, exports);
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, InitAll)
