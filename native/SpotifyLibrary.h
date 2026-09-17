#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cspot {
struct Context;
}

namespace tinyspot {

// Library access over the logged-in session (Mercury), not the Web API: the
// Web API is rate-limited for librespot/cspot client IDs, and this needs no
// extra login. Callbacks run on the session thread and must not block.
class SpotifyLibrary {
 public:
  using Playlists = std::vector<std::pair<std::string, std::string>>;  // uri, name
  using Tracks = std::vector<std::string>;                             // track URIs

  explicit SpotifyLibrary(std::shared_ptr<cspot::Context> ctx) : ctx(std::move(ctx)) {}

  // The account's playlists. ok=false on failure.
  void playlists(std::function<void(bool ok, Playlists)> done);

  // Track URIs of a playlist URI or of spotify:user:<name>:collection.
  void tracks(const std::string& contextUri, std::function<void(bool ok, Tracks)> done);

 private:
  void resolvePage(const std::string& url, std::shared_ptr<Tracks> acc,
                   std::function<void(bool, Tracks)> done);

  std::shared_ptr<cspot::Context> ctx;
};

}  // namespace tinyspot
