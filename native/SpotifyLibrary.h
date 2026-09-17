#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tinyspot {

class SpClient;

// Library access over Spotify's HTTP API (spclient). Not the public Web API,
// which rate-limits librespot/cspot client IDs, and not Mercury, which
// Spotify is retiring. Each call runs on its own short-lived thread;
// callbacks fire there.
class SpotifyLibrary {
 public:
  using Playlists = std::vector<std::pair<std::string, std::string>>;  // uri, name
  using Tracks = std::vector<std::string>;                             // track URIs

  explicit SpotifyLibrary(std::shared_ptr<SpClient> sp) : sp(std::move(sp)) {}

  // "Liked Songs" first, then the account's playlists. ok=false on failure.
  void playlists(const std::string& username, std::function<void(bool ok, Playlists)> done);

  // Track URIs of a playlist or of spotify:user:<name>:collection (Liked Songs).
  void tracks(const std::string& contextUri, std::function<void(bool ok, Tracks)> done);

 private:
  std::shared_ptr<SpClient> sp;
};

}  // namespace tinyspot
