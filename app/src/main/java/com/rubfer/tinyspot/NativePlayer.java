package com.rubfer.tinyspot;

/** The single JNI boundary. Nothing else in the app touches native code. */
final class NativePlayer {
    // Event codes, mirrored in native/NativeSpotifyPlayer.h
    static final int EV_AUTH_STATE = 1;      // arg: 0 out, 1 connecting, 2 connected, 3 failed
    static final int EV_CREDENTIALS = 2;     // text: reusable credentials JSON
    static final int EV_PLAYBACK_STATE = 3;  // arg: 0 stopped, 1 playing, 2 paused, 3 buffering
    static final int EV_TRACK_CHANGED = 4;   // text: title\nartist\nalbum\nimage, arg: duration ms
    static final int EV_POSITION = 5;        // arg: position ms
    static final int EV_VOLUME = 6;          // arg: 0..65535
    static final int EV_ERROR = 7;           // text: message
    static final int EV_PLAYLISTS = 8;       // text: "uri\tname\n"..., Liked Songs first; arg: 1 ok

    interface Listener {
        /** Called on a native thread. */
        void onNativeEvent(int type, int arg, String text);
    }

    private static volatile Listener listener;

    static {
        System.loadLibrary("tinyspot");
    }

    private NativePlayer() {}

    static void setListener(Listener l) {
        listener = l;
    }

    /** Called from native code. */
    @SuppressWarnings("unused")
    private static void onNativeEvent(int type, int arg, String text) {
        Listener l = listener;
        if (l != null) l.onNativeEvent(type, arg, text);
    }

    static native void nativeInitialize(String deviceName);
    /** Starts the zeroconf HTTP endpoint; returns the port, 0 on failure. */
    static native int nativeStartDiscovery(int port);
    static native void nativeLogin(String credentialsJson);
    /** Replies with EV_PLAYLISTS. */
    static native void nativeRequestPlaylists();
    /** Plays a playlist or Liked Songs URI, resolved on the watch. */
    static native void nativePlayContext(String contextUri, boolean shuffle);
    static native void nativePause();
    static native void nativeResume();
    static native void nativeNext();
    static native void nativePrevious();
    static native void nativeSeek(long positionMs);
    static native void nativeSetVolume(float volume);
    static native void nativeShutdown();
}
