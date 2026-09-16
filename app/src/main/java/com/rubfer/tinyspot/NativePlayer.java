package com.rubfer.tinyspot;

/** The single JNI boundary. Nothing else in the app touches native code. */
final class NativePlayer {
    static {
        System.loadLibrary("tinyspot");
    }

    private NativePlayer() {}

    /** Milestone 1 probe: builds a cspot LoginBlob and returns its zeroconf info JSON. */
    static native String nativeProbe();
}
