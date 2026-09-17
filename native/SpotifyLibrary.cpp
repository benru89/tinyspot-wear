#include "SpotifyLibrary.h"

#include <android/log.h>

#include <cstdint>
#include <string_view>

#include "CSpotContext.h"
#include "MercurySession.h"
#include "cJSON.h"

#define TAG "TinySpot-Cspot"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using cspot::MercurySession;

namespace tinyspot {
namespace {

constexpr size_t kMaxTracks = 2000;

// Minimal protobuf reader: walks one message level, handing out
// length-delimited fields. Enough for playlist4's nested strings.
class Pb {
 public:
  Pb(const uint8_t* p, size_t n) : p(p), end(p + n) {}

  // Next length-delimited field; skips varint/fixed fields.
  bool next(uint32_t& field, std::string_view& value) {
    while (p < end) {
      uint64_t key;
      if (!varint(key)) return false;
      field = (uint32_t)(key >> 3);
      switch (key & 7) {
        case 0: { uint64_t v; if (!varint(v)) return false; break; }
        case 1: if ((p += 8) > end) return false; break;
        case 5: if ((p += 4) > end) return false; break;
        case 2: {
          uint64_t len;
          if (!varint(len) || len > (uint64_t)(end - p)) return false;
          value = std::string_view((const char*)p, len);
          p += len;
          return true;
        }
        default: return false;
      }
    }
    return false;
  }

  static Pb of(std::string_view v) { return Pb((const uint8_t*)v.data(), v.size()); }

 private:
  bool varint(uint64_t& out) {
    out = 0;
    for (int shift = 0; p < end && shift < 64; shift += 7) {
      uint8_t b = *p++;
      out |= (uint64_t)(b & 0x7f) << shift;
      if (!(b & 0x80)) return true;
    }
    return false;
  }

  const uint8_t* p;
  const uint8_t* end;
};

std::string_view firstField(std::string_view msg, uint32_t wanted) {
  Pb pb = Pb::of(msg);
  uint32_t f;
  std::string_view v;
  while (pb.next(f, v)) {
    if (f == wanted) return v;
  }
  return {};
}

std::string joined(const MercurySession::Response& res) {
  std::string out;
  for (auto& part : res.parts) out.append(part.begin(), part.end());
  return out;
}

}  // namespace

void SpotifyLibrary::playlists(std::function<void(bool, Playlists)> done) {
  const std::string user = ctx->config.username;
  std::string uri = "hm://playlist/v2/user/" + user +
                    "/rootlist?decorate=revision,attributes,length&from=0&length=500";
  ctx->session->execute(
      MercurySession::RequestType::GET, uri, [done](MercurySession::Response& res) {
        // Liked Songs is not offered: context-resolve does not expand the
        // collection for this client, and there is no other simple route.
        Playlists out;
        if (res.fail || res.parts.empty()) {
          LOGE("rootlist request failed");
          done(false, out);
          return;
        }
        // SelectedListContent.contents(5): items(3).uri(1),
        // meta_items(4).attributes(2).name(1) in the same order.
        std::string body = joined(res);
        std::string_view contents = firstField(body, 5);
        std::vector<std::string> uris, names;
        Pb pb = Pb::of(contents);
        uint32_t f;
        std::string_view v;
        while (pb.next(f, v)) {
          if (f == 3) uris.emplace_back(firstField(v, 1));
          if (f == 4) names.emplace_back(firstField(firstField(v, 2), 1));
        }
        for (size_t i = 0; i < uris.size(); i++) {
          if (uris[i].rfind("spotify:playlist:", 0) != 0) continue;  // folder markers
          std::string name = i < names.size() && !names[i].empty() ? names[i] : uris[i];
          out.emplace_back(uris[i], name);
        }
        LOGI("library: %zu playlists", out.size());
        done(true, out);
      });
}

void SpotifyLibrary::tracks(const std::string& contextUri,
                            std::function<void(bool, Tracks)> done) {
  const std::string playlistPrefix = "spotify:playlist:";
  if (contextUri.rfind(playlistPrefix, 0) == 0) {
    std::string uri = "hm://playlist/v2/playlist/" + contextUri.substr(playlistPrefix.size()) +
                      "?from=0&length=" + std::to_string(kMaxTracks);
    ctx->session->execute(
        MercurySession::RequestType::GET, uri, [done](MercurySession::Response& res) {
          Tracks out;
          LOGI("playlist reply: fail=%d parts=%zu", res.fail, res.parts.size());
          if (res.fail || res.parts.empty()) {
            done(false, out);
            return;
          }
          std::string body = joined(res);
          Pb pb = Pb::of(firstField(body, 5));
          uint32_t f;
          std::string_view v;
          while (pb.next(f, v)) {
            if (f != 3) continue;
            std::string_view u = firstField(v, 1);
            if (u.rfind("spotify:track:", 0) == 0) out.emplace_back(u);  // skip local files/episodes
          }
          LOGI("library: playlist has %zu tracks", out.size());
          done(!out.empty(), out);
        });
    return;
  }

  // Liked Songs and anything else: let Spotify expand the context.
  resolvePage("hm://context-resolve/v1/" + contextUri, std::make_shared<Tracks>(), std::move(done));
}

// context-resolve JSON: {"pages":[{"tracks":[{"uri":...}],"next_page_url":"hm://..."}]}
void SpotifyLibrary::resolvePage(const std::string& url, std::shared_ptr<Tracks> acc,
                                 std::function<void(bool, Tracks)> done) {
  ctx->session->execute(
      MercurySession::RequestType::GET, url, [this, acc, done](MercurySession::Response& res) {
        if (res.fail || res.parts.empty()) {
          LOGE("context-resolve failed after %zu tracks", acc->size());
          done(!acc->empty(), *acc);
          return;
        }
        std::string body = joined(res);
        std::string nextUrl;
        if (cJSON* root = cJSON_Parse(body.c_str())) {
          cJSON* page;
          cJSON_ArrayForEach(page, cJSON_GetObjectItem(root, "pages")) {
            cJSON* track;
            cJSON_ArrayForEach(track, cJSON_GetObjectItem(page, "tracks")) {
              cJSON* uri = cJSON_GetObjectItem(track, "uri");
              if (cJSON_IsString(uri) && strncmp(uri->valuestring, "spotify:track:", 14) == 0 &&
                  acc->size() < kMaxTracks) {
                acc->emplace_back(uri->valuestring);
              }
            }
            cJSON* next = cJSON_GetObjectItem(page, "next_page_url");
            if (cJSON_IsString(next)) nextUrl = next->valuestring;
          }
          cJSON_Delete(root);
        }
        if (!nextUrl.empty() && acc->size() < kMaxTracks) {
          resolvePage(nextUrl, acc, done);
        } else {
          LOGI("library: resolved %zu tracks", acc->size());
          done(!acc->empty(), *acc);
        }
      });
}

}  // namespace tinyspot
