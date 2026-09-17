# TinySpot

A tiny standalone Spotify player for old Wear OS watches (built for a TicWatch
Pro 3 Ultra: Android 9 / API 28, `armeabi-v7a`, 890 MB RAM). No phone and no PC
are needed to play music; the watch streams from Spotify itself over Wi-Fi or
LTE and outputs to Bluetooth headphones or the watch speaker.

Spotify Premium is required.

## Status

Working on the watch:

- Pair once from the phone's Spotify app (zeroconf), then the watch logs in on
  its own with reusable credentials.
- Library on the watch: Liked Songs (1313 tracks tested) and playlists (34),
  with a shuffle toggle. Tap to play; audio starts ~0.5 s later.
- Play / pause / next / previous, Bluetooth headset buttons (MediaSession),
  audio focus, pause when headphones disconnect.
- Also acts as a Spotify Connect target for casting from the phone.

## To do

Unverified, in the order that matters:

1. **Requirement tests on the watch** — Bluetooth headset buttons and
   pause-on-disconnect (written, never tested with real headphones); playing
   with the phone off; playing on LTE; Wi-Fi <-> LTE handover mid-track. The
   code for all of these exists; none of it is proven.
2. **Battery over a longer session.** Measured ~11.5 %/h over 36 min on Wi-Fi
   with the screen off (see above); LTE and Bluetooth output will cost more.
3. **Credential lifetime.** The requirement is weeks. Reusable credentials
   normally last until the password changes, but that is unproven here.

Known rough edges:

4. **Playback loads 100 tracks at a time** because cspot re-sends its whole
   queue in every Spirc notify. Shuffling covers the whole list, but each
   window seam is an edge case (the Next fix above was one).
5. **No seek UI** (elapsed/total is shown; `nativeSeek` has no control), no
   search, no album/artist browsing, no artwork, no volume UI.
6. **Casting from the phone still uses SPIRC**, the protocol librespot dropped
   in 2025. Standalone playback no longer depends on it, so if Spotify retires
   it, only phone-casting breaks.
7. **The zeroconf endpoint keeps running after login** (civetweb, 1 thread). It
   could stop once credentials are stored, at the cost of needing a restart to
   pair a different account.
8. **Third-party code is built with `-Wno-everything`.** Building it with
   warnings once turned up the URL parser overflow (patch bell 0003); what is
   left is unused-variable noise and warnings in civetweb paths we never call.

## Measured (release build, on the watch)

| | |
|---|---|
| APK | 1.04 MB (`libtinyspot.so` ~1.01 MB, `classes.dex` 16 KB) |
| RAM idle (logged in) | 8.9 MB PSS |
| RAM playing | ~15.6 MB PSS (2.6 MB of that is the audio buffer) |
| CPU idle, logged in, screen off | 0.1 % of one core |
| CPU playing, screen off | 3.5-3.9 % of one core, measured over 3-5 min |
| Wake locks | none held by the app |
| Audio | 44.1 kHz s16 stereo, 4096-frame OpenSL ES buffers (~11 wakeups/s), 15 s ring buffer with 1.5 s prefill |
| Underruns | 0 in 5 min (was 41 in a comparable run with a 1 s buffer) |
| Battery while playing | 26 % -> 19 % in 36 min, screen off, Wi-Fi = ~11.5 %/h, so roughly 8-9 h of playback per charge |
| Continuous playback | 62/62 samples over 31 min, screen off, no interruption |
| Playback with the screen off | survived 60/60 samples over 10 min |

## Architecture

```
Wear OS UI (Java, Views/XML, no AndroidX)
  MainActivity, LibraryActivity
  PlayerService      foreground service, Wi-Fi/LTE binding, mDNS, credentials
  MediaSessionManager  media buttons, audio focus, becoming-noisy
        |  NativePlayer  (the only JNI boundary; events only on state changes)
        v
C++ (native/)
  CspotPlayer        sessions, zeroconf endpoint, local playback windows
  SpClient           Spotify HTTP API: login5 bearer + client token
  SpotifyLibrary     playlists, playlist tracks, Liked Songs
  AndroidAudioSink   ring buffer -> OpenSL ES
        v
cspot + bell + mbedTLS (third_party/, patched)
        v
Spotify network -> Ogg Vorbis (Tremor) -> PCM -> OpenSL ES -> Bluetooth / speaker
```

### What talks to which Spotify API

cspot's original transport was Mercury (`hm://` over the encrypted AP socket),
which Spotify is retiring. Moved to HTTP (`spclient`), as librespot did:

| | |
|---|---|
| Login, audio decryption keys | AP socket (librespot still uses it too) |
| Access token | `login5` |
| Client token | `clienttoken.spotify.com`, Linux desktop identity, granted without a hash-cash challenge, valid 14 days |
| Playlists, playlist tracks | `spclient` `/playlist/v2/...` |
| Liked Songs | `spclient` `/context-resolve/v1/...` (Mercury's collection endpoints return empty) |
| Track info | `spclient` `/extended-metadata/v0/...`, Mercury as fallback |
| Audio download | Spotify CDN over HTTPS |
| Casting from the phone | SPIRC over Mercury — legacy, not required for standalone use |

The public Web API is deliberately unused: its token for cspot/librespot client
IDs is rate-limited (HTTP 429) immediately.

## Build

```sh
tools/setup-host.sh                  # submodules + Python venv for nanopb
./gradlew assembleRelease            # or assembleDebug
adb install -r app/build/outputs/apk/release/app-release.apk
```

Requirements: Android SDK with NDK 28.2.13676358 and CMake 3.31.6, JDK 17.

- ABIs come from `tinyspot.abis` in `gradle.properties` (default
  `armeabi-v7a`); release APKs ship only what is listed.
- `-Ptinyspot.testhooks=true` enables adb test intents in a release build:
  `am start -f 0x20000000 -n com.rubfer.tinyspot/.MainActivity --es play <uri> [--ez shuffle true]`
  and `--es cmd pause|resume|next|prev`. Debug builds always have them.
- `patches/cspot/*.patch` and `patches/bell/*.patch` are applied to the pinned
  submodules at configure time, tracked by a `.tinyspot-patches` stamp file.

## Patches to third-party code

cspot upstream is unmaintained (last commit July 2024) and broken against
current Spotify. 0002-0006 are ported from the actively maintained
[philippe44/cspot](https://github.com/philippe44/cspot) fork; the rest are ours.

| Patch | Why |
|---|---|
| cspot 0001 | zeroconf `deviceType` was the literal string `"deviceType"` in the cJSON build |
| cspot 0002-0006 | CDN network-error handling, queue thread safety, stale track notifications |
| cspot 0007 | a network error while fetching a token aborted the process; reconnect recursed once per retry |
| cspot 0008 | `SpircHandler::loadTracks`, so the watch can start playback itself |
| cspot 0009 | `readBlock` added -1 to its offset on `EINTR`, shifting the stream: MAC mismatches and lost replies |
| cspot 0010 | optional HTTP metadata provider, replacing `hm://metadata/3` |
| bell 0001 | HTTP client dropped chunked bodies, hid status codes, and sent a duplicate `Accept` that made the token service return an empty body |
| bell 0002 | `WrappedSemaphore::twait` built a timespec with `tv_nsec >= 1e9`, so `sem_timedwait` returned EINVAL at once and every timed wait in cspot spun: 10.7 % of a core while idle, 12.3 % playing, down to 0.1 % / 3.5 % once fixed |

## Logging

Tags: `TinySpot-Java`, `TinySpot-JNI`, `TinySpot-Cspot`, `TinySpot-Audio`.
cspot's own debug level is dropped and its per-packet chatter filtered.

```sh
adb logcat -v time TinySpot-Cspot:V TinySpot-Audio:V TinySpot-Java:V '*:S'
```

Note: the watch's log buffer drops this app's lines under load; stream logcat
to a file rather than using `logcat -d` after the fact.

For measuring with the screen off (Wi-Fi and adb die with it), sample on the
device and collect afterwards. Two traps: this watch's shell has **no `awk`**,
and the shell user cannot read another app's `/proc/<pid>/stat` — use `top -b
-n 1 -q -p <pid>` and `dumpsys` instead.

## License

cspot and bell are GPL-3.0; this project links them, so it is GPL-3.0 too.
Not affiliated with Spotify.
