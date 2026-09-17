#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AndroidAudioSink.h"
#include "NativeSpotifyPlayer.h"

struct mg_context;
struct mg_connection;
namespace cspot {
struct Context;
class LoginBlob;
class SpircHandler;
}  // namespace cspot

namespace tinyspot {

class CspotPlayer : public NativeSpotifyPlayer {
 public:
  CspotPlayer(PlayerListener* listener, std::string deviceName);
  ~CspotPlayer() override;

  int startDiscovery(int port) override;
  void loginStored(const std::string& credentialsJson) override;

  void requestPlaylists() override;
  void playContext(const std::string& contextUri, bool shuffle) override;

  void pause() override;
  void resume() override;
  void next() override;
  void previous() override;
  void seek(int positionMs) override;
  void setVolume(int volume) override;
  void shutdown() override;

 private:
  void startSession(std::shared_ptr<cspot::LoginBlob> blob);
  void sessionLoop(std::shared_ptr<cspot::LoginBlob> blob);
  void onSpircEvent(int type, int intData, bool boolData, void* trackInfo);
  void workerLoop();
  void emit(Event e, int arg1, const std::string& text = std::string());
  static int handleZeroconf(mg_connection* conn, void* self);
  std::string zeroconfInfo();
  std::shared_ptr<cspot::SpircHandler> currentHandler();
  void loadWindow(size_t start);

  PlayerListener* listener;
  std::string deviceName;
  std::shared_ptr<cspot::LoginBlob> discoveryBlob;  // owns the DH keypair
  mg_context* http = nullptr;
  AndroidAudioSink sink;

  std::mutex stateMutex;
  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::SpircHandler> handler;
  std::shared_ptr<class SpotifyLibrary> library;

  // Locally started context. cspot re-sends its whole queue in every Spirc
  // notify, so only a window of it is loaded at a time.
  static constexpr size_t kWindow = 100;
  std::string contextUri;
  std::vector<std::string> contextTracks;
  size_t windowStart = 0;
  bool localContext = false;
  std::thread sessionThread;
  std::atomic<bool> running{false};
  std::atomic<bool> sessionActive{false};
  std::atomic<bool> depleted{false};

  // Work the OpenSL callback must not do itself (it talks to Spotify).
  std::thread worker;
  std::mutex workMutex;
  std::condition_variable workCv;
  bool pendingTrackReached = false;
  bool pendingDrained = false;
  bool workerStop = false;
};

}  // namespace tinyspot
