package com.rubfer.tinyspot;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.util.Log;

/**
 * Media session, audio focus and headphone handling, all on platform APIs.
 * Gives us Bluetooth headset buttons, lets Android know media is playing,
 * and keeps other apps from talking over us.
 */
final class MediaSessionManager {
    private static final String TAG = "TinySpot-Java";

    private final Context context;
    private final AudioManager audio;
    private final MediaSession session;
    private boolean hasFocus;
    private boolean pausedByFocusLoss;
    private boolean noisyRegistered;

    private final AudioManager.OnAudioFocusChangeListener focusListener = change -> {
        switch (change) {
            case AudioManager.AUDIOFOCUS_LOSS:
                Log.i(TAG, "audio focus lost");
                hasFocus = false;
                pausedByFocusLoss = false;
                NativePlayer.nativePause();
                break;
            case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT:
            case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK:
                // No ducking: pause and pick up again on regain.
                pausedByFocusLoss = PlayerService.state.playback == 1;
                if (pausedByFocusLoss) NativePlayer.nativePause();
                break;
            case AudioManager.AUDIOFOCUS_GAIN:
                if (pausedByFocusLoss) {
                    pausedByFocusLoss = false;
                    NativePlayer.nativeResume();
                }
                break;
            default:
                break;
        }
    };

    /** Headphones pulled or BT disconnected: pause, like every other player. */
    private final BroadcastReceiver noisyReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent intent) {
            if (PlayerService.state.playback == 1) {
                Log.i(TAG, "audio becoming noisy, pausing");
                NativePlayer.nativePause();
            }
        }
    };

    MediaSessionManager(Context context) {
        this.context = context;
        this.audio = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        session = new MediaSession(context, "TinySpot");
        session.setFlags(MediaSession.FLAG_HANDLES_MEDIA_BUTTONS
                | MediaSession.FLAG_HANDLES_TRANSPORT_CONTROLS);
        session.setCallback(new MediaSession.Callback() {
            @Override
            public void onPlay() {
                Log.i(TAG, "media session onPlay");
                NativePlayer.nativeResume();
            }

            @Override
            public void onPause() {
                Log.i(TAG, "media session onPause");
                NativePlayer.nativePause();
            }

            @Override
            public void onStop() {
                NativePlayer.nativePause();
            }

            @Override
            public void onSkipToNext() {
                NativePlayer.nativeNext();
            }

            @Override
            public void onSkipToPrevious() {
                NativePlayer.nativePrevious();
            }

            @Override
            public void onSeekTo(long pos) {
                NativePlayer.nativeSeek(pos);
            }
        });
        session.setActive(true);
    }

    void release() {
        abandonFocus();
        unregisterNoisy();
        session.setActive(false);
        session.release();
    }

    /** Mirrors the player state into the session; call on state changes only. */
    void update(PlayerService.State s) {
        int state;
        switch (s.playback) {
            case 1: state = PlaybackState.STATE_PLAYING; break;
            case 2: state = PlaybackState.STATE_PAUSED; break;
            case 3: state = PlaybackState.STATE_BUFFERING; break;
            default: state = PlaybackState.STATE_STOPPED; break;
        }
        if (s.playback == 1) {
            requestFocus();
            registerNoisy();
        } else {
            unregisterNoisy();
            if (s.playback == 0) abandonFocus();
        }

        session.setPlaybackState(new PlaybackState.Builder()
                .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE
                        | PlaybackState.ACTION_PLAY_PAUSE
                        | PlaybackState.ACTION_SKIP_TO_NEXT
                        | PlaybackState.ACTION_SKIP_TO_PREVIOUS
                        | PlaybackState.ACTION_SEEK_TO | PlaybackState.ACTION_STOP)
                .setState(state, PlaybackState.PLAYBACK_POSITION_UNKNOWN, 1.0f)
                .build());

        session.setMetadata(new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, s.title)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, s.artist)
                .putLong(MediaMetadata.METADATA_KEY_DURATION, s.durationMs)
                .build());
    }

    private void requestFocus() {
        if (hasFocus) return;
        int result = audio.requestAudioFocus(focusListener, AudioManager.STREAM_MUSIC,
                AudioManager.AUDIOFOCUS_GAIN);
        hasFocus = result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        if (!hasFocus) Log.w(TAG, "audio focus denied");
    }

    private void abandonFocus() {
        if (!hasFocus) return;
        audio.abandonAudioFocus(focusListener);
        hasFocus = false;
    }

    private void registerNoisy() {
        if (noisyRegistered) return;
        context.registerReceiver(noisyReceiver,
                new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY));
        noisyRegistered = true;
    }

    private void unregisterNoisy() {
        if (!noisyRegistered) return;
        context.unregisterReceiver(noisyReceiver);
        noisyRegistered = false;
    }
}
