#include "SpotifyLibrary.h"

#include <android/log.h>

#include <cstring>
#include <thread>

#include "Protobuf.h"
#include "SpClient.h"
#include "cJSON.h"

#define TAG "TinySpot-Cspot"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace tinyspot {
namespace {

constexpr size_t kMaxTracks = 2000;
constexpr int kMaxPages = 50;

// context-resolve JSON: {"pages":[{"tracks":[{"uri":...}], "page_url":"hm://...",
// "next_page_url":"hm://..."}]}. Page URLs are spclient paths behind "hm://".
void collectContext(SpClient& sp, const std::string& firstPath, SpotifyLibrary::Tracks& out) {
  std::vector<std::string> pending = {firstPath};
  for (int fetched = 0; !pending.empty() && fetched < kMaxPages && out.size() < kMaxTracks;
       fetched++) {
    std::string path = pending.front();
    pending.erase(pending.begin());
    std::string body = sp.get(path, "application/json");
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
      LOGE("context page unreadable: %s", path.c_str());
      break;
    }
    // A context has "pages"; a follow-up page is itself a page object.
    cJSON* pages = cJSON_GetObjectItem(root, "pages");
    std::vector<cJSON*> list;
    if (cJSON_IsArray(pages)) {
      cJSON* p;
      cJSON_ArrayForEach(p, pages) list.push_back(p);
    } else {
      list.push_back(root);
    }
    for (cJSON* page : list) {
      cJSON* track;
      cJSON_ArrayForEach(track, cJSON_GetObjectItem(page, "tracks")) {
        cJSON* uri = cJSON_GetObjectItem(track, "uri");
        if (cJSON_IsString(uri) && strncmp(uri->valuestring, "spotify:track:", 14) == 0 &&
            out.size() < kMaxTracks) {
          out.emplace_back(uri->valuestring);
        }
      }
      bool hasTracks = cJSON_GetArraySize(cJSON_GetObjectItem(page, "tracks")) > 0;
      for (const char* key : {"page_url", "next_page_url"}) {
        cJSON* url = cJSON_GetObjectItem(page, key);
        if (!cJSON_IsString(url) || strncmp(url->valuestring, "hm://", 5) != 0) continue;
        if (strcmp(key, "page_url") == 0 && hasTracks) continue;  // already inline
        pending.push_back(std::string("/") + (url->valuestring + 5));
      }
    }
    cJSON_Delete(root);
  }
}

}  // namespace

void SpotifyLibrary::playlists(const std::string& username,
                               std::function<void(bool, Playlists)> done) {
  std::thread([sp = sp, username, done] {
    Playlists out;
    out.emplace_back("spotify:user:" + username + ":collection", "Liked Songs");

    std::string body = sp->get("/playlist/v2/user/" + username +
                               "/rootlist?decorate=revision,attributes,length&from=0&length=500");
    if (body.empty()) {
      done(false, out);
      return;
    }
    // SelectedListContent.contents(5): items(3).uri(1) and, in the same
    // order, meta_items(4).attributes(2).name(1).
    std::vector<std::string> uris, names;
    pb::Reader r(pb::field(body, 5));
    uint32_t f, w;
    std::string_view v;
    while (r.next(f, w, v)) {
      if (f == 3 && w == 2) uris.emplace_back(pb::field(v, 1));
      if (f == 4 && w == 2) names.emplace_back(pb::field(pb::field(v, 2), 1));
    }
    for (size_t i = 0; i < uris.size(); i++) {
      if (uris[i].rfind("spotify:playlist:", 0) != 0) continue;  // folder markers
      std::string name = i < names.size() && !names[i].empty() ? names[i] : uris[i];
      out.emplace_back(uris[i], name);
    }
    LOGI("library: %zu playlists", out.size() - 1);
    done(true, out);
  }).detach();
}

void SpotifyLibrary::tracks(const std::string& contextUri,
                            std::function<void(bool, Tracks)> done) {
  std::thread([sp = sp, contextUri, done] {
    Tracks out;
    const std::string playlistPrefix = "spotify:playlist:";
    if (contextUri.rfind(playlistPrefix, 0) == 0) {
      std::string body = sp->get("/playlist/v2/playlist/" +
                                 contextUri.substr(playlistPrefix.size()) +
                                 "?from=0&length=" + std::to_string(kMaxTracks));
      pb::Reader r(pb::field(body, 5));
      uint32_t f, w;
      std::string_view v;
      while (r.next(f, w, v)) {
        if (f != 3 || w != 2) continue;
        std::string_view u = pb::field(v, 1);
        if (u.rfind("spotify:track:", 0) == 0) out.emplace_back(u);  // skip local files/episodes
      }
    } else {
      collectContext(*sp, "/context-resolve/v1/" + contextUri, out);
    }
    LOGI("library: %s has %zu tracks", contextUri.c_str(), out.size());
    done(!out.empty(), out);
  }).detach();
}

}  // namespace tinyspot
