// Milestone 1 probe: prove cspot runs inside the Android process.
#include <jni.h>
#include <android/log.h>

#include "BellLogger.h"
#include "LoginBlob.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "TinySpot-JNI", __VA_ARGS__)

extern "C" JNIEXPORT jstring JNICALL
Java_com_rubfer_tinyspot_NativePlayer_nativeProbe(JNIEnv* env, jclass) {
  auto blob = std::make_shared<cspot::LoginBlob>("TinySpot");
  std::string info = blob->buildZeroconfInfo();
  LOGI("cspot LoginBlob ready, deviceId=%s", blob->getDeviceId().c_str());
  return env->NewStringUTF(info.c_str());
}
