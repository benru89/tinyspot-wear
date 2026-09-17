#include "CspotPlayer.h"

#include <android/log.h>

#include <functional>
#include <algorithm>
#include <map>
#include <chrono>
#include <random>
#include <string_view>
#include <variant>

#include <cstring>

#include "cJSON.h"
#include "CSpotContext.h"
#include "LoginBlob.h"
#include "SpotifyLibrary.h"
#include "SpClient.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"
#include "civetweb.h"

#define TAG "TinySpot-Cspot"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using cspot::SpircHandler;

namespace tinyspot {

CspotPlayer::CspotPlayer(PlayerListener* l, std::string name)
    : listener(l), deviceName(std::move(name)) {
  worker = std::thread(&CspotPlayer::workerLoop, this);
}

CspotPlayer::~CspotPlayer() { shutdown(); }

void CspotPlayer::emit(Event e, int arg1, const std::string& text) {
  if (listener) listener->onEvent(e, arg1, text);
}

std::shared_ptr<SpircHandler> CspotPlayer::currentHandler() {
  std::lock_guard<std::mutex> lock(stateMutex);
  return handler;
}

// ---------------------------------------------------------------------------
// Zeroconf: the Spotify app GETs our public key, then POSTs an encrypted blob.
// ---------------------------------------------------------------------------
int CspotPlayer::startDiscovery(int port) {
  if (http) return port;
  discoveryBlob = std::make_shared<cspot::LoginBlob>(deviceName);

  // "+port" = IPv4 and IPv6: phones often resolve the AAAA record first.
  std::string ports = "+" + std::to_string(port);
  const char* options[] = {"listening_ports", ports.c_str(), "num_threads", "1", nullptr};
  mg_init_library(0);
  http = mg_start(nullptr, nullptr, options);
  if (!http) {
    LOGE("zeroconf server failed on port %d", port);
    return 0;
  }
  mg_set_request_handler(http, "/spotify_info", &CspotPlayer::handleZeroconf, this);
  LOGI("zeroconf endpoint on port %d (IPv4+IPv6)", port);
  return port;
}

// cspot's getInfo lacks fields current Spotify apps expect; align it with
// librespot's (discovery/src/server.rs), which the apps list.
std::string CspotPlayer::zeroconfInfo() {
  cJSON* o = cJSON_Parse(discoveryBlob->buildZeroconfInfo().c_str());
  auto set = [o](const char* k, cJSON* v) {
    if (cJSON_HasObjectItem(o, k)) cJSON_ReplaceItemInObject(o, k, v);
    else cJSON_AddItemToObject(o, k, v);
  };
  set("spotifyError", cJSON_CreateNumber(0));
  set("version", cJSON_CreateString("2.9.0"));
  set("resolverVersion", cJSON_CreateString("1"));
  set("scope", cJSON_CreateString("streaming"));
  set("brandDisplayName", cJSON_CreateString("TinySpot"));
  set("clientID", cJSON_CreateString("65b708073fc0480ea92a077233ca87bd"));
  set("supported_drm_media_formats", cJSON_CreateArray());
  set("supported_capabilities", cJSON_CreateNumber(1));
  set("aliases", cJSON_CreateArray());
  char* str = cJSON_PrintUnformatted(o);
  std::string out(str);
  free(str);
  cJSON_Delete(o);
  return out;
}

int CspotPlayer::handleZeroconf(mg_connection* conn, void* selfPtr) {
  auto* self = static_cast<CspotPlayer*>(selfPtr);
  const mg_request_info* info = mg_get_request_info(conn);
  std::string body;

  if (strcmp(info->request_method, "POST") == 0) {
    std::string form(info->content_length > 0 ? info->content_length : 0, '\0');
    if (!form.empty()) mg_read(conn, form.data(), form.size());
    mg_header hd[16];
    int num = mg_split_form_urlencoded(form.data(), hd, 16);
    std::map<std::string, std::string> query;
    for (int i = 0; i < num; i++) query[hd[i].name] = hd[i].value;
    LOGI("zeroconf %s from %s", query.count("action") ? query["action"].c_str() : "POST",
         info->remote_addr);

    if (query.count("blob")) {
      if (self->sessionActive) {
        LOGI("zeroconf addUser ignored: session already active");
      } else {
        // Decrypting needs the same keypair getInfo advertised.
        self->discoveryBlob->loadZeroconfQuery(query);
        self->startSession(self->discoveryBlob);
      }
    }
    body = R"({"status":101,"spotifyError":0,"statusString":"OK"})";
  } else {
    LOGI("zeroconf getInfo from %s", info->remote_addr);
    body = self->zeroconfInfo();
  }

  mg_printf(conn,
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n",
            body.size());
  mg_write(conn, body.data(), body.size());
  return 200;
}

void CspotPlayer::loginStored(const std::string& json) {
  if (sessionActive) return;
  auto blob = std::make_shared<cspot::LoginBlob>(deviceName);
  blob->loadJson(json);
  startSession(blob);
}

void CspotPlayer::startSession(std::shared_ptr<cspot::LoginBlob> blob) {
  if (sessionActive.exchange(true)) return;
  if (sessionThread.joinable()) sessionThread.join();
  running = true;
  sessionThread = std::thread(&CspotPlayer::sessionLoop, this, blob);
}

// ---------------------------------------------------------------------------
// Session thread: AP connect -> auth -> Spirc -> packet loop.
// ---------------------------------------------------------------------------
// Retries with backoff: the watch is often asleep on Wi-Fi, or between
// networks, when the first attempt is made.
void CspotPlayer::sessionLoop(std::shared_ptr<cspot::LoginBlob> blob) {
  int delaySeconds = 2;
  while (running) {
    if (!runSession(blob)) break;  // credentials rejected: retrying won't help
    if (!running) break;
    LOGI("session ended, retrying in %ds", delaySeconds);
    for (int i = 0; i < delaySeconds * 10 && running; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    delaySeconds = std::min(delaySeconds * 2, 60);
  }
  sessionActive = false;
}

bool CspotPlayer::runSession(std::shared_ptr<cspot::LoginBlob> blob) {
  emit(Event::AUTH_STATE, 1);
  try {
    auto c = cspot::Context::createFromBlob(blob);
    LOGI("connecting to Spotify AP");
    c->session->connectWithRandomAp();
    c->config.authData = c->session->authenticate(blob);
    if (c->config.authData.empty()) {
      LOGE("authentication failed");
      emit(Event::AUTH_STATE, 3);
      emit(Event::ERROR, 0, "Spotify login declined");
      return false;
    }
    LOGI("authenticated as %s", c->config.username.c_str());
    emit(Event::CREDENTIALS, 0, c->getCredentialsJson());

    if (!sink.open([this] {
          std::lock_guard<std::mutex> l(workMutex);
          pendingTrackReached = true;
          workCv.notify_one();
        },
        [this] {
          std::lock_guard<std::mutex> l(workMutex);
          pendingDrained = true;
          workCv.notify_one();
        })) {
      emit(Event::ERROR, 0, "audio output unavailable");
    }

    // Track info over HTTP; cspot falls back to mercury if this returns empty.
    auto sp = std::make_shared<SpClient>(c);
    c->metadataProvider = [sp](const std::string& uri, bool isEpisode) {
      return sp->metadata(uri, isEpisode);
    };

    auto h = std::make_shared<SpircHandler>(c);
    h->getTrackPlayer()->setDataCallback(
        [this](uint8_t* data, size_t bytes, std::string_view trackId) {
          return sink.write(data, bytes, std::hash<std::string_view>()(trackId));
        });
    h->setEventHandler([this](std::unique_ptr<SpircHandler::Event> ev) {
      int i = 0;
      bool b = false;
      void* ti = nullptr;
      if (auto p = std::get_if<int>(&ev->data)) i = *p;
      if (auto p = std::get_if<bool>(&ev->data)) b = *p;
      if (auto p = std::get_if<cspot::TrackInfo>(&ev->data)) ti = p;
      onSpircEvent((int)ev->eventType, i, b, ti);
    });

    {
      std::lock_guard<std::mutex> lock(stateMutex);
      ctx = c;
      handler = h;
      library = std::make_shared<SpotifyLibrary>(sp);
      username = c->config.username;
    }
    c->session->startTask();
    emit(Event::AUTH_STATE, 2);
    LOGI("Spotify Connect device '%s' online", deviceName.c_str());



    while (running) c->session->handlePacket();

    h->disconnect();
  } catch (const std::exception& ex) {
    LOGE("session error: %s", ex.what());
    emit(Event::ERROR, 0, ex.what());
  }
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    handler.reset();
    library.reset();
    ctx.reset();
  }
  sink.setPlaying(false);
  emit(Event::AUTH_STATE, 0);
  return true;
}


void CspotPlayer::networkChanged() {
  std::lock_guard<std::mutex> lock(stateMutex);
  if (ctx) {
    LOGI("network changed, dropping the session socket");
    ctx->session->close();
  }
}

void CspotPlayer::onSpircEvent(int type, int i, bool b, void* ti) {
  using T = SpircHandler::EventType;
  switch ((T)type) {
    case T::PLAYBACK_START:
      depleted = false;
      sink.flush();
      emit(Event::POSITION, i);
      emit(Event::PLAYBACK_STATE, 3);
      break;
    case T::PLAY_PAUSE:
      sink.setPlaying(!b);
      emit(Event::PLAYBACK_STATE, b ? 2 : 1);
      break;
    case T::TRACK_INFO: {
      auto* t = static_cast<cspot::TrackInfo*>(ti);
      LOGI("track: %s - %s", t->artist.c_str(), t->name.c_str());
      emit(Event::TRACK_CHANGED, (int)t->duration,
           t->name + "\n" + t->artist + "\n" + t->album + "\n" + t->imageUrl);
      break;
    }
    case T::SEEK:
      sink.flush();
      emit(Event::POSITION, i);
      break;
    case T::NEXT:
    case T::PREV:
    case T::FLUSH:
      sink.flush();
      break;
    case T::DISC:
      sink.flush();
      sink.setPlaying(false);
      emit(Event::PLAYBACK_STATE, 0);
      break;
    case T::DEPLETED: {
      std::lock_guard<std::mutex> lock(stateMutex);
      if (localContext && windowStart + kWindow < contextTracks.size()) {
        // Window played out: the worker loads the next one once audio drains.
        LOGI("window finished, next starts at %zu", windowStart + kWindow);
      }
      depleted = true;
      break;
    }
    case T::VOLUME:
      sink.setVolume(i);
      emit(Event::VOLUME, i);
      break;
  }
}

void CspotPlayer::workerLoop() {
  std::unique_lock<std::mutex> lock(workMutex);
  while (!workerStop) {
    workCv.wait(lock, [this] { return workerStop || pendingTrackReached || pendingDrained; });
    bool reached = std::exchange(pendingTrackReached, false);
    bool drainedNow = std::exchange(pendingDrained, false);
    lock.unlock();

    if (auto h = currentHandler()) {
      if (reached) h->notifyAudioReachedPlayback();
      if (drainedNow && depleted.exchange(false)) {
        size_t next = 0;
        {
          std::lock_guard<std::mutex> l(stateMutex);
          if (localContext && windowStart + kWindow < contextTracks.size()) next = windowStart + kWindow;
        }
        if (next) {
          loadWindow(next);
          lock.lock();
          continue;
        }
        LOGI("queue finished");
        sink.setPlaying(false);
        h->notifyAudioEnded();
        emit(Event::PLAYBACK_STATE, 0);
      }
    }
    lock.lock();
  }
}

// ---------------------------------------------------------------------------
// Library: browse and start playback on the watch itself.
// ---------------------------------------------------------------------------
void CspotPlayer::requestPlaylists() {
  std::shared_ptr<SpotifyLibrary> lib;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    lib = library;
  }
  if (!lib) {
    emit(Event::PLAYLISTS, 0);
    return;
  }
  std::string user;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    user = username;
  }
  lib->playlists(user, [this](bool ok, SpotifyLibrary::Playlists list) {
    std::string text;
    for (auto& [uri, name] : list) {
      std::string clean = name;
      std::replace(clean.begin(), clean.end(), '\t', ' ');
      std::replace(clean.begin(), clean.end(), '\n', ' ');
      text += uri + "\t" + clean + "\n";
    }
    emit(Event::PLAYLISTS, ok ? 1 : 0, text);
  });
}

void CspotPlayer::playContext(const std::string& uri, bool shuffle) {
  std::shared_ptr<SpotifyLibrary> lib;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    lib = library;
  }
  LOGI("playContext %s shuffle=%d (library %s)", uri.c_str(), shuffle, lib ? "ready" : "missing");
  if (!lib) return;
  emit(Event::PLAYBACK_STATE, 3);
  lib->tracks(uri, [this, uri, shuffle](bool ok, SpotifyLibrary::Tracks tracks) {
    if (!ok) {
      LOGE("could not load %s", uri.c_str());
      emit(Event::ERROR, 0, "could not load " + uri);
      emit(Event::PLAYBACK_STATE, 0);
      return;
    }
    if (shuffle) std::shuffle(tracks.begin(), tracks.end(), std::mt19937(std::random_device{}()));
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      contextUri = uri;
      contextTracks = std::move(tracks);
      localContext = true;
    }
    loadWindow(0);
  });
}

void CspotPlayer::loadWindow(size_t start) {
  std::shared_ptr<SpircHandler> h;
  std::vector<std::string> window;
  std::string uri;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    h = handler;
    if (!h || start >= contextTracks.size()) return;
    windowStart = start;
    size_t end = std::min(contextTracks.size(), start + kWindow);
    window.assign(contextTracks.begin() + start, contextTracks.begin() + end);
    uri = contextUri;
  }
  depleted = false;
  sink.flush();
  h->loadTracks(window, 0, uri);
}

// ---------------------------------------------------------------------------
// Local controls (watch UI / media buttons) -> Spirc, which notifies Spotify.
// ---------------------------------------------------------------------------
void CspotPlayer::pause() {
  if (auto h = currentHandler()) h->setPause(true);
}
void CspotPlayer::resume() {
  if (auto h = currentHandler()) h->setPause(false);
}
void CspotPlayer::next() {
  if (auto h = currentHandler(); h && h->nextSong()) sink.flush();
}
void CspotPlayer::previous() {
  if (auto h = currentHandler(); h && h->previousSong()) sink.flush();
}
void CspotPlayer::seek(int ms) {
  if (auto h = currentHandler()) {
    h->getTrackPlayer()->seekMs(ms);
    sink.flush();
    h->updatePositionMs(ms);
  }
}
void CspotPlayer::setVolume(int v) {
  sink.setVolume(v);
  if (auto h = currentHandler()) h->setRemoteVolume(v);
}

void CspotPlayer::shutdown() {
  running = false;
  if (sessionThread.joinable()) sessionThread.join();
  if (http) {
    mg_stop(http);
    http = nullptr;
  }
  {
    std::lock_guard<std::mutex> l(workMutex);
    workerStop = true;
  }
  workCv.notify_one();
  if (worker.joinable()) worker.join();
  sink.close();
}

}  // namespace tinyspot
