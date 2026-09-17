// The only JNI boundary. Java -> native: NativePlayer.native*(); native ->
// Java: NativePlayer.onNativeEvent(), fired only on state changes.
#include <android/log.h>
#include <jni.h>

#include <memory>
#include <mutex>

#include "AndroidLogger.h"
#include "CspotPlayer.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "TinySpot-JNI", __VA_ARGS__)

using namespace tinyspot;

namespace {

JavaVM* gVm = nullptr;
jclass gClass = nullptr;
jmethodID gOnEvent = nullptr;
std::unique_ptr<NativeSpotifyPlayer> gPlayer;
std::mutex gPlayerMutex;

class JavaListener : public PlayerListener {
 public:
  void onEvent(Event type, int arg1, const std::string& text) override {
    JNIEnv* env = nullptr;
    bool attached = false;
    if (gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
      gVm->AttachCurrentThread(&env, nullptr);
      attached = true;
    }
    jstring jtext = text.empty() ? nullptr : env->NewStringUTF(text.c_str());
    env->CallStaticVoidMethod(gClass, gOnEvent, (jint)type, (jint)arg1, jtext);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (jtext) env->DeleteLocalRef(jtext);
    if (attached) gVm->DetachCurrentThread();
  }
};

JavaListener gListener;

std::string toStd(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out(c);
  env->ReleaseStringUTFChars(s, c);
  return out;
}

// Run an action against the live player, if any.
template <typename F>
void withPlayer(F&& f) {
  std::lock_guard<std::mutex> lock(gPlayerMutex);
  if (gPlayer) f(*gPlayer);
}

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  gVm = vm;
  JNIEnv* env;
  vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  jclass cls = env->FindClass("com/rubfer/tinyspot/NativePlayer");
  gClass = static_cast<jclass>(env->NewGlobalRef(cls));
  gOnEvent = env->GetStaticMethodID(gClass, "onNativeEvent", "(IILjava/lang/String;)V");
  static AndroidLogger logger;
  bell::bellGlobalLogger = &logger;
  LOGI("libtinyspot loaded");
  return JNI_VERSION_1_6;
}

extern "C" {

JNIEXPORT void JNICALL Java_com_rubfer_tinyspot_NativePlayer_nativeInitialize(
    JNIEnv* env, jclass, jstring deviceName) {
  std::lock_guard<std::mutex> lock(gPlayerMutex);
  if (!gPlayer) gPlayer = std::make_unique<CspotPlayer>(&gListener, toStd(env, deviceName));
}

JNIEXPORT jint JNICALL Java_com_rubfer_tinyspot_NativePlayer_nativeStartDiscovery(
    JNIEnv*, jclass, jint port) {
  int result = 0;
  withPlayer([&](NativeSpotifyPlayer& p) { result = p.startDiscovery(port); });
  return result;
}

JNIEXPORT void JNICALL Java_com_rubfer_tinyspot_NativePlayer_nativeLogin(
    JNIEnv* env, jclass, jstring credentialsJson) {
  std::string json = toStd(env, credentialsJson);
  withPlayer([&](NativeSpotifyPlayer& p) { p.loginStored(json); });
}

JNIEXPORT void JNICALL Java_com_rubfer_tinyspot_NativePlayer_nativeRequestPlaylists(JNIEnv*, jclass) {
  withPlayer([](NativeSpotifyPlayer& p) { p.requestPlaylists(); });
}

JNIEXPORT void JNICALL Java_com_rubfer_tinyspot_NativePlayer_nativePlayContext(
    JNIEnv* env, jclass, jstring uri, jboolean shuffle) {
  std::string u = toStd(env, uri);
  withPlayer([&](NativeSpotifyPlayer& p) { p.playContext(u, shuffle == JNI_TRUE); });
}

JNIEXPORT void JNICALL Java_com_rubfer_tinyspot_NativePlayer_nativeNetworkChanged(JNIEnv*, jclass) {
  withPlayer([](NativeSpotifyPlayer& p) { p.networkChanged(); });
}

JNIEXPORT void JNICALL Java_com_rubfer_tinyspot_NativePlayer_nativePause(JNIEnv*, jclass) {
  withPlayer([](NativeSpotifyPlayer& p) { p.pause(); });
}

JNIEXPORT void JNICALL Java_com_rubfer_tinyspot_NativePlayer_nativeResume(JNIEnv*, jclass) {
  withPlayer([](NativeSpotifyPlayer& p) { p.resume(); });
}

JNIEXPORT void JNICALL Java_com_rubfer_tinyspot_NativePlayer_nativeNext(JNIEnv*, jclass) {
  withPlayer([](NativeSpotifyPlayer& p) { p.next(); });
}

JNIEXPORT void JNICALL Java_com_rubfer_tinyspot_NativePlayer_nativePrevious(JNIEnv*, jclass) {
  withPlayer([](NativeSpotifyPlayer& p) { p.previous(); });
}

JNIEXPORT void JNICALL Java_com_rubfer_tinyspot_NativePlayer_nativeSeek(
    JNIEnv*, jclass, jlong positionMs) {
  withPlayer([&](NativeSpotifyPlayer& p) { p.seek((int)positionMs); });
}

JNIEXPORT void JNICALL Java_com_rubfer_tinyspot_NativePlayer_nativeSetVolume(
    JNIEnv*, jclass, jfloat volume) {
  int v = (int)(volume * 65535.0f);
  withPlayer([&](NativeSpotifyPlayer& p) { p.setVolume(v); });
}

JNIEXPORT void JNICALL Java_com_rubfer_tinyspot_NativePlayer_nativeShutdown(JNIEnv*, jclass) {
  std::unique_ptr<NativeSpotifyPlayer> dying;
  {
    std::lock_guard<std::mutex> lock(gPlayerMutex);
    dying = std::move(gPlayer);
  }
  if (dying) dying->shutdown();  // outside the lock: joins native threads
}

}  // extern "C"
