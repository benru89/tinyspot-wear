#pragma once

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <deque>
#include <mutex>
#include <vector>

namespace tinyspot {

// 44.1 kHz / stereo / s16 PCM -> OpenSL ES buffer queue.
//
// The decoder thread write()s into a ring buffer; the OpenSL callback pulls
// whole 4096-frame buffers out of it (~11 wakeups/s). write() never blocks:
// it returns how much fit, and cspot's player retries. The ring holds
// kBufferSeconds of audio so a slow CDN chunk or a weak signal can't be heard;
// it is allocated while the output is open, not for the life of the process.
class AndroidAudioSink {
 public:
  // Fired on the OpenSL callback thread when the first sample written with a
  // new track key is handed to the device. Must be cheap and must not block.
  using TrackReachedFn = std::function<void()>;
  // Fired on the OpenSL callback thread when the buffer runs dry.
  using DrainedFn = std::function<void()>;

  AndroidAudioSink();
  ~AndroidAudioSink();

  bool open(TrackReachedFn onTrackReached, DrainedFn onDrained);
  void close();

  size_t write(const uint8_t* pcm, size_t bytes, size_t trackKey);
  void setPlaying(bool playing);
  void flush();
  void setVolume(int volume0to65535);

 private:
  static constexpr int kSampleRate = 44100;
  static constexpr int kChannels = 2;
  static constexpr size_t kFrameBytes = 4;
  static constexpr size_t kCallbackFrames = 4096;  // ~93 ms
  static constexpr size_t kBufferSeconds = 15;     // ~2.6 MB of PCM
  static constexpr size_t kRingBytes = kSampleRate * kFrameBytes * kBufferSeconds;
  // Don't start the device until this much is buffered, or the long ring never
  // gets a head start and the first seconds can still stutter.
  static constexpr size_t kPrefillBytes = kSampleRate * kFrameBytes * 3 / 2;  // 1.5 s

  static void bufferQueueCallback(SLAndroidSimpleBufferQueueItf bq, void* ctx);
  void onBufferNeeded();
  void startOutput();

  SLObjectItf engineObj = nullptr;
  SLEngineItf engine = nullptr;
  SLObjectItf mixObj = nullptr;
  SLObjectItf playerObj = nullptr;
  SLPlayItf play = nullptr;
  SLVolumeItf volume = nullptr;
  SLAndroidSimpleBufferQueueItf queue = nullptr;

  std::mutex ringMutex;
  std::vector<uint8_t> ring;
  size_t readPos = 0;
  size_t fill = 0;
  uint64_t totalRead = 0;
  uint64_t totalWritten = 0;
  size_t lastWriteKey = 0;
  // Absolute byte offsets where a new track's audio starts. A 15 s buffer can
  // hold more than one boundary, so they queue.
  std::deque<uint64_t> markers;

  std::vector<uint8_t> outBuf;
  std::atomic<bool> playing{false};
  std::atomic<bool> waitingForPrefill{false};
  bool draining = false;
  uint32_t underruns = 0;

  TrackReachedFn trackReached;
  DrainedFn drained;
};

}  // namespace tinyspot
