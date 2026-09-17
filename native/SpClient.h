#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cspot {
struct Context;
class AccessKeyFetcher;
}  // namespace cspot

namespace tinyspot {

// Spotify's HTTP API ("spclient"), the replacement for Mercury requests:
// login5 bearer token (from the session's stored credentials) + client token.
// Calls block; use them off the session and audio threads.
class SpClient {
 public:
  explicit SpClient(std::shared_ptr<cspot::Context> ctx);
  ~SpClient();

  // Response body; empty on any failure (non-200 included).
  std::string get(const std::string& path, const std::string& accept = "application/x-protobuf");
  std::string post(const std::string& path, const std::string& body,
                   const std::string& contentType = "application/x-protobuf");

  // Raw Track/Episode protobuf for a spotify:track:/spotify:episode: URI,
  // via extended-metadata (hm://metadata/3's replacement). Empty on failure.
  std::vector<uint8_t> metadata(const std::string& uri, bool isEpisode);

 private:
  bool ensureBaseUrl();
  std::string clientToken();
  std::string request(bool post, const std::string& path, const std::string& body,
                      const std::string& type);

  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::AccessKeyFetcher> login5;
  std::mutex mutex;
  std::string baseUrl;
  std::string token;
  int64_t tokenExpiresAtMs = 0;
};

}  // namespace tinyspot
