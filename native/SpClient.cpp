#include "SpClient.h"

#include <android/log.h>

#include <chrono>
#include <cstdio>
#include <sys/utsname.h>

#include "AccessKeyFetcher.h"
#include "CSpotContext.h"
#include "HTTPClient.h"
#include "Protobuf.h"
#include "cJSON.h"

#define TAG "TinySpot-Cspot"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace tinyspot {
namespace {

// Identify as librespot does on Linux: the keymaster client ID with desktop
// Linux data is granted a client token directly (no hash-cash challenge).
constexpr const char* kClientId = "65b708073fc0480ea92a077233ca87bd";
constexpr const char* kClientVersion = "1.2.52.442";

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

SpClient::SpClient(std::shared_ptr<cspot::Context> c)
    : ctx(std::move(c)), login5(std::make_shared<cspot::AccessKeyFetcher>(ctx)) {}

SpClient::~SpClient() = default;

bool SpClient::ensureBaseUrl() {
  if (!baseUrl.empty()) return true;
  try {
    auto res = bell::HTTPClient::get("https://apresolve.spotify.com/?type=spclient");
    std::string body(res->body());
    if (cJSON* root = cJSON_Parse(body.c_str())) {
      cJSON* first = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "spclient"), 0);
      if (cJSON_IsString(first)) {
        std::string host = first->valuestring;
        auto colon = host.rfind(':');
        // Port 443 is the default; bell's URL parser handles it either way.
        baseUrl = "https://" + (colon != std::string::npos && host.substr(colon) == ":443"
                                    ? host.substr(0, colon)
                                    : host);
      }
      cJSON_Delete(root);
    }
  } catch (const std::exception& e) {
    LOGE("spclient apresolve failed: %s", e.what());
  }
  if (!baseUrl.empty()) LOGI("spclient: %s", baseUrl.c_str());
  return !baseUrl.empty();
}

std::string SpClient::clientToken() {
  if (!token.empty() && nowMs() < tokenExpiresAtMs) return token;

  struct utsname u = {};
  uname(&u);

  pb::Writer linuxData;
  linuxData.bytes(1, "Linux").bytes(2, u.release).bytes(3, u.version).bytes(4, u.machine);
  pb::Writer platform;
  platform.message(5, linuxData);  // desktop_linux
  pb::Writer connectivity;
  connectivity.message(1, platform).bytes(2, ctx->config.deviceId);
  pb::Writer clientData;
  clientData.bytes(1, kClientVersion).bytes(2, kClientId).message(3, connectivity);
  pb::Writer request;
  request.varint(1, 1 /* REQUEST_CLIENT_DATA_REQUEST */).message(2, clientData);

  try {
    std::vector<uint8_t> body(request.out.begin(), request.out.end());
    auto res = bell::HTTPClient::post(
        "https://clienttoken.spotify.com/v1/clienttoken",
        {{"Content-Type", "application/x-protobuf"}, {"Accept", "application/x-protobuf"}},
        body);
    std::string reply(res->body());
if (res->status() != 200) LOGE("client token HTTP %d", res->status());
    uint64_t type = pb::varintField(reply, 1);
    if (type != 1 /* RESPONSE_GRANTED_TOKEN_RESPONSE */) {
      LOGE("client token not granted (response type %llu, %zu bytes)",
           (unsigned long long)type, reply.size());
      return {};
    }
    std::string_view granted = pb::field(reply, 2);
    token = std::string(pb::field(granted, 1));
    uint64_t refreshAfter = pb::varintField(granted, 3);
    if (refreshAfter == 0) refreshAfter = 7200;
    tokenExpiresAtMs = nowMs() + (int64_t)refreshAfter * 1000;
    LOGI("client token granted, refresh after %llus", (unsigned long long)refreshAfter);
  } catch (const std::exception& e) {
    LOGE("client token request failed: %s", e.what());
    token.clear();
  }
  return token;
}

std::string SpClient::request(bool post, const std::string& path, const std::string& body,
                              const std::string& type) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!ensureBaseUrl()) return {};
  std::string bearer = login5->getAccessKey();
  std::string ct = clientToken();
  if (bearer.empty()) {
    LOGE("spclient: no login5 token");
    return {};
  }

  bell::HTTPClient::Headers headers = {{"Authorization", "Bearer " + bearer}};
  if (!ct.empty()) headers.push_back({"client-token", ct});
  try {
    std::unique_ptr<bell::HTTPClient::Response> res;
    if (post) {
      headers.push_back({"Content-Type", type});
      headers.push_back({"Accept", type});
      res = bell::HTTPClient::post(baseUrl + path, headers,
                                   std::vector<uint8_t>(body.begin(), body.end()));
    } else {
      headers.push_back({"Accept", type});
      res = bell::HTTPClient::get(baseUrl + path, headers);
    }
    std::string reply(res->body());
    if (res->status() != 200) {
      LOGE("spclient %s %s -> HTTP %d", post ? "POST" : "GET", path.c_str(), res->status());
      return {};
    }
    return reply;
  } catch (const std::exception& e) {
    LOGE("spclient %s %s failed: %s", post ? "POST" : "GET", path.c_str(), e.what());
    return {};
  }
}

std::vector<uint8_t> SpClient::metadata(const std::string& uri, bool isEpisode) {
  // BatchedEntityRequest{2: EntityRequest{1: uri, 2: ExtensionQuery{1: kind}}}
  // kind: TRACK_V4 = 10, EPISODE_V4 = 12 (extension_kind.proto)
  pb::Writer query;
  query.varint(1, isEpisode ? 12 : 10);
  pb::Writer entity;
  entity.bytes(1, uri).message(2, query);
  pb::Writer request;
  request.message(2, entity);

  std::string reply = post("/extended-metadata/v0/extended-metadata", request.out);
  if (reply.empty()) return {};

  // BatchedExtensionResponse{2: EntityExtensionDataArray{3: EntityExtensionData
  // {3: google.protobuf.Any{2: value}}}}
  std::string_view any =
      pb::field(pb::field(pb::field(reply, 2), 3), 3);
  std::string_view value = pb::field(any, 2);
  if (value.empty()) {
    LOGE("extended-metadata: no payload for %s (%zu bytes)", uri.c_str(), reply.size());
    return {};
  }
  return std::vector<uint8_t>(value.begin(), value.end());
}

std::string SpClient::get(const std::string& path, const std::string& accept) {
  return request(false, path, {}, accept);
}

std::string SpClient::post(const std::string& path, const std::string& body,
                           const std::string& contentType) {
  return request(true, path, body, contentType);
}

}  // namespace tinyspot
