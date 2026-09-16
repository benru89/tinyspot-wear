#pragma once

#include <android/log.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "BellLogger.h"

namespace tinyspot {

// Routes cspot/bell logging to logcat as TinySpot-Cspot. Debug is dropped and
// the per-packet chatter from MercurySession is filtered out.
class AndroidLogger : public bell::AbstractLogger {
 public:
  void debug(std::string, int, std::string, const char*, ...) override {}

  void error(std::string file, int line, std::string, const char* fmt, ...) override {
    va_list a;
    va_start(a, fmt);
    write(ANDROID_LOG_ERROR, file, line, fmt, a);
    va_end(a);
  }

  void info(std::string file, int line, std::string, const char* fmt, ...) override {
    if (strncmp(fmt, "Received packet", 15) == 0) return;
    va_list a;
    va_start(a, fmt);
    write(ANDROID_LOG_INFO, file, line, fmt, a);
    va_end(a);
  }

 private:
  static void write(int prio, const std::string& file, int line, const char* fmt, va_list a) {
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, a);
    auto slash = file.find_last_of('/');
    const char* base = file.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    __android_log_print(prio, "TinySpot-Cspot", "%s:%d %s", base, line, msg);
  }
};

}  // namespace tinyspot
