#pragma once

#include <string>

// Backend-neutral player interface. CspotPlayer implements it today; a
// LibrespotPlayer could implement it later without touching JNI or Java.

namespace tinyspot {

// Event codes shared with NativePlayer.java (keep in sync).
enum class Event : int {
  AUTH_STATE = 1,      // arg1: 0 = logged out, 1 = connecting, 2 = connected, 3 = failed
  CREDENTIALS = 2,     // text: reusable credentials JSON, to persist
  PLAYBACK_STATE = 3,  // arg1: 0 = stopped, 1 = playing, 2 = paused, 3 = buffering
  TRACK_CHANGED = 4,   // text: "title\nartist\nalbum\nimageUrl", arg1: duration ms
  POSITION = 5,        // arg1: position ms at the moment of the event
  VOLUME = 6,          // arg1: 0..65535
  ERROR = 7,           // text: message
};

class PlayerListener {
 public:
  virtual ~PlayerListener() = default;
  // Called from native threads, only on state changes (never per audio frame).
  virtual void onEvent(Event type, int arg1, const std::string& text) = 0;
};

class NativeSpotifyPlayer {
 public:
  virtual ~NativeSpotifyPlayer() = default;

  // Starts the zeroconf HTTP endpoint; returns its TCP port (0 on failure).
  // The platform layer advertises it over mDNS.
  virtual int startDiscovery(int port) = 0;
  // Logs in with credentials previously emitted by Event::CREDENTIALS.
  virtual void loginStored(const std::string& credentialsJson) = 0;

  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual void next() = 0;
  virtual void previous() = 0;
  virtual void seek(int positionMs) = 0;
  virtual void setVolume(int volume0to65535) = 0;
  virtual void shutdown() = 0;
};

}  // namespace tinyspot
