#include "AndroidAudioSink.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#define TAG "TinySpot-Audio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace tinyspot {

AndroidAudioSink::AndroidAudioSink() : outBuf(kCallbackFrames * kFrameBytes) {}

AndroidAudioSink::~AndroidAudioSink() { close(); }

bool AndroidAudioSink::open(TrackReachedFn onTrackReached, DrainedFn onDrained) {
  if (engineObj) return playerObj != nullptr;
  ring.assign(kRingBytes, 0);
  trackReached = std::move(onTrackReached);
  drained = std::move(onDrained);

  SLresult r = slCreateEngine(&engineObj, 0, nullptr, 0, nullptr, nullptr);
  if (r != SL_RESULT_SUCCESS) return LOGE("slCreateEngine %u", r), false;
  (*engineObj)->Realize(engineObj, SL_BOOLEAN_FALSE);
  (*engineObj)->GetInterface(engineObj, SL_IID_ENGINE, &engine);

  (*engine)->CreateOutputMix(engine, &mixObj, 0, nullptr, nullptr);
  (*mixObj)->Realize(mixObj, SL_BOOLEAN_FALSE);

  SLDataLocator_AndroidSimpleBufferQueue locQueue = {
      SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
  SLDataFormat_PCM fmt = {SL_DATAFORMAT_PCM,
                          kChannels,
                          SL_SAMPLINGRATE_44_1,
                          SL_PCMSAMPLEFORMAT_FIXED_16,
                          SL_PCMSAMPLEFORMAT_FIXED_16,
                          SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
                          SL_BYTEORDER_LITTLEENDIAN};
  SLDataSource src = {&locQueue, &fmt};
  SLDataLocator_OutputMix locMix = {SL_DATALOCATOR_OUTPUTMIX, mixObj};
  SLDataSink snk = {&locMix, nullptr};

  // Android media stream: routes to BT A2DP when connected, speaker otherwise.
  const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME,
                               SL_IID_ANDROIDCONFIGURATION};
  const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};
  r = (*engine)->CreateAudioPlayer(engine, &playerObj, &src, &snk, 3, ids, req);
  if (r != SL_RESULT_SUCCESS) return LOGE("CreateAudioPlayer %u", r), false;

  SLAndroidConfigurationItf cfg;
  if ((*playerObj)->GetInterface(playerObj, SL_IID_ANDROIDCONFIGURATION, &cfg) ==
      SL_RESULT_SUCCESS) {
    SLint32 stream = SL_ANDROID_STREAM_MEDIA;
    (*cfg)->SetConfiguration(cfg, SL_ANDROID_KEY_STREAM_TYPE, &stream,
                             sizeof(stream));
  }

  r = (*playerObj)->Realize(playerObj, SL_BOOLEAN_FALSE);
  if (r != SL_RESULT_SUCCESS) return LOGE("Realize player %u", r), false;
  (*playerObj)->GetInterface(playerObj, SL_IID_PLAY, &play);
  (*playerObj)->GetInterface(playerObj, SL_IID_VOLUME, &volume);
  (*playerObj)->GetInterface(playerObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue);
  (*queue)->RegisterCallback(queue, bufferQueueCallback, this);

  LOGI("OpenSL ES ready: 44100 Hz s16 stereo, %zu-frame buffers, %zus (%zu KB) ring",
       kCallbackFrames, kBufferSeconds, kRingBytes / 1024);
  return true;
}

void AndroidAudioSink::close() {
  if (playerObj) {
    (*playerObj)->Destroy(playerObj);
    playerObj = nullptr;
    play = nullptr;
    volume = nullptr;
    queue = nullptr;
  }
  if (mixObj) (*mixObj)->Destroy(mixObj), mixObj = nullptr;
  if (engineObj) (*engineObj)->Destroy(engineObj), engineObj = nullptr;
  ring.clear();
  ring.shrink_to_fit();
}

size_t AndroidAudioSink::write(const uint8_t* pcm, size_t bytes, size_t trackKey) {
  bool prefilled = false;
  size_t n;
  {
    std::lock_guard<std::mutex> lock(ringMutex);
    n = std::min(bytes, kRingBytes - fill);
    n -= n % kFrameBytes;
    if (n == 0) return 0;

    if (trackKey != lastWriteKey) {
      lastWriteKey = trackKey;
      markers.push_back(totalWritten);
    }
    draining = false;

    size_t writePos = (readPos + fill) % kRingBytes;
    size_t first = std::min(n, kRingBytes - writePos);
    memcpy(ring.data() + writePos, pcm, first);
    memcpy(ring.data(), pcm + first, n - first);
    fill += n;
    totalWritten += n;
    prefilled = fill >= kPrefillBytes;
  }

  // Enough buffered: let the device start.
  if (prefilled && waitingForPrefill.exchange(false)) startOutput();
  return n;
}

void AndroidAudioSink::bufferQueueCallback(SLAndroidSimpleBufferQueueItf, void* ctx) {
  static_cast<AndroidAudioSink*>(ctx)->onBufferNeeded();
}

void AndroidAudioSink::onBufferNeeded() {
  const size_t want = outBuf.size();
  bool reached = false, dry = false;
  size_t got;
  {
    std::lock_guard<std::mutex> lock(ringMutex);
    got = std::min(want, fill);
    size_t first = std::min(got, kRingBytes - readPos);
    memcpy(outBuf.data(), ring.data() + readPos, first);
    memcpy(outBuf.data() + first, ring.data(), got - first);
    readPos = (readPos + got) % kRingBytes;
    fill -= got;

    uint64_t before = totalRead;
    totalRead += got;
    while (!markers.empty() && markers.front() >= before && markers.front() < totalRead) {
      markers.pop_front();
      reached = true;
    }
    if (got < want && totalWritten > 0 && !draining) {
      draining = true;  // report once per dry spell
      dry = true;
    }
  }

  if (got < want) {
    memset(outBuf.data() + got, 0, want - got);
    if (got > 0 && (++underruns % 20) == 1) {
      LOGW("buffer underrun (%u so far)", underruns);
    }
  }

  (*queue)->Enqueue(queue, outBuf.data(), want);

  if (reached && trackReached) trackReached();
  if (dry && drained) drained();
}

void AndroidAudioSink::setPlaying(bool p) {
  if (!play) return;
  if (!p) {
    waitingForPrefill = false;
    if (!playing.exchange(false)) return;
    (*play)->SetPlayState(play, SL_PLAYSTATE_PAUSED);
    LOGI("output paused");
    return;
  }
  if (playing) return;

  size_t buffered;
  {
    std::lock_guard<std::mutex> lock(ringMutex);
    buffered = fill;
  }
  if (buffered < kPrefillBytes) {
    waitingForPrefill = true;  // write() starts us once there is a cushion
    LOGI("buffering %zu/%zu KB before starting", buffered / 1024, kPrefillBytes / 1024);
    return;
  }
  startOutput();
}

void AndroidAudioSink::startOutput() {
  if (!play || playing.exchange(true)) return;
  (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);
  SLAndroidSimpleBufferQueueState st;
  (*queue)->GetState(queue, &st);
  if (st.count == 0) onBufferNeeded();  // (re)start the callback chain
  LOGI("output playing");
}

void AndroidAudioSink::flush() {
  bool wasPlaying = playing;
  {
    std::lock_guard<std::mutex> lock(ringMutex);
    readPos = 0;
    fill = 0;
    totalRead = totalWritten = 0;
    lastWriteKey = 0;
    markers.clear();
    draining = false;
  }
  // The buffer is empty again: hold the device until there is a cushion,
  // otherwise a seek or track change plays out of an empty ring.
  if (wasPlaying && playing.exchange(false)) {
    (*play)->SetPlayState(play, SL_PLAYSTATE_PAUSED);
    waitingForPrefill = true;
  }
}

void AndroidAudioSink::setVolume(int v) {
  if (!volume) return;
  // Spotify's 0..65535 -> millibels on a log curve.
  double lin = std::clamp(v, 0, 65535) / 65535.0;
  SLmillibel mb = lin <= 0.0001 ? SL_MILLIBEL_MIN : (SLmillibel)(2000.0 * std::log10(lin));
  (*volume)->SetVolumeLevel(volume, mb);
}

}  // namespace tinyspot
