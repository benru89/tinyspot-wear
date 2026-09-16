#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "AndroidAudioSink.h"
#include "NativeSpotifyPlayer.h"

namespace bell {
class BellHTTPServer;
}
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
  std::shared_ptr<cspot::SpircHandler> currentHandler();

  PlayerListener* listener;
  std::string deviceName;
  std::shared_ptr<cspot::LoginBlob> discoveryBlob;  // owns the DH keypair
  std::unique_ptr<bell::BellHTTPServer> http;
  AndroidAudioSink sink;

  std::mutex stateMutex;
  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::SpircHandler> handler;
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
