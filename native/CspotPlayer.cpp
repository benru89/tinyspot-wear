#include "CspotPlayer.h"

#include <android/log.h>

#include <functional>
#include <map>
#include <string_view>
#include <variant>

#include "BellHTTPServer.h"
#include "CSpotContext.h"
#include "LoginBlob.h"
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
  try {
    http = std::make_unique<bell::BellHTTPServer>(port);
  } catch (const std::exception& ex) {
    LOGE("zeroconf server failed on port %d: %s", port, ex.what());
    return 0;
  }

  http->registerGet("/spotify_info", [this](mg_connection*) {
    LOGI("zeroconf getInfo");
    return http->makeJsonResponse(discoveryBlob->buildZeroconfInfo());
  });

  http->registerPost("/spotify_info", [this](mg_connection* conn) {
    auto info = mg_get_request_info(conn);
    if (info->content_length > 0) {
      std::string body(info->content_length, '\0');
      mg_read(conn, body.data(), info->content_length);

      mg_header hd[10];
      int num = mg_split_form_urlencoded(body.data(), hd, 10);
      std::map<std::string, std::string> query;
      for (int i = 0; i < num; i++) query[hd[i].name] = hd[i].value;

      if (sessionActive) {
        LOGI("zeroconf addUser ignored: session already active");
      } else {
        LOGI("zeroconf addUser received, logging in");
        // Decrypting needs the same keypair getInfo advertised.
        discoveryBlob->loadZeroconfQuery(query);
        startSession(discoveryBlob);
      }
    }
    return http->makeJsonResponse(
        R"({"status":101,"spotifyError":0,"statusString":"ERROR-OK"})");
  });

  LOGI("zeroconf endpoint on port %d", port);
  return port;
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
void CspotPlayer::sessionLoop(std::shared_ptr<cspot::LoginBlob> blob) {
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
      sessionActive = false;
      return;
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
    ctx.reset();
  }
  sink.setPlaying(false);
  emit(Event::AUTH_STATE, 0);
  sessionActive = false;
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
    case T::DEPLETED:
      depleted = true;
      break;
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
    http->close();
    http.reset();
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
