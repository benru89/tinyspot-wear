package com.rubfer.tinyspot;

import android.app.Activity;
import android.os.Bundle;
import android.view.View;
import android.widget.ImageButton;
import android.widget.TextView;

public class MainActivity extends Activity implements PlayerService.UiListener {
    private static final String[] AUTH = {"Not logged in", "Connecting…", "Connected", "Login failed"};
    private static final String[] PLAYBACK = {"Stopped", "Playing", "Paused", "Buffering…"};

    private TextView spotify;
    private TextView player;
    private TextView track;
    private ImageButton playPause;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        spotify = findViewById(R.id.spotify);
        player = findViewById(R.id.player);
        track = findViewById(R.id.track);
        playPause = findViewById(R.id.play_pause);

        findViewById(R.id.library).setOnClickListener(
                v -> startActivity(new android.content.Intent(this, LibraryActivity.class)));
        findViewById(R.id.prev).setOnClickListener(v -> NativePlayer.nativePrevious());
        findViewById(R.id.next).setOnClickListener(v -> NativePlayer.nativeNext());
        playPause.setOnClickListener(v -> {
            if (PlayerService.state.playback == 1) NativePlayer.nativePause();
            else NativePlayer.nativeResume();
        });

        PlayerService.start(this);

        debugPlay(getIntent());
    }

    @Override
    protected void onNewIntent(android.content.Intent intent) {
        super.onNewIntent(intent);
        debugPlay(intent);
    }

    /** adb testing: am start -n .../.MainActivity --es play URI [--ez shuffle true] */
    private static void debugPlay(android.content.Intent intent) {
        String play = intent.getStringExtra("play");
        if (BuildConfig.DEBUG && play != null) {
            NativePlayer.nativePlayContext(play, intent.getBooleanExtra("shuffle", false));
        }
    }

    // Only listen while visible: no UI work with the screen off.
    @Override
    protected void onStart() {
        super.onStart();
        PlayerService.setUiListener(this);
    }

    @Override
    protected void onStop() {
        PlayerService.setUiListener(null);
        super.onStop();
    }

    @Override
    public void onStateChanged(PlayerService.State s) {
        spotify.setText(s.auth == 0 && s.error == null
                ? "Open Spotify → Devices → TinySpot"
                : AUTH[s.auth]);
        player.setText(PLAYBACK[s.playback]);
        track.setText(s.title.isEmpty() ? "—" : s.artist + " – " + s.title);
        track.setVisibility(s.auth == 2 ? View.VISIBLE : View.INVISIBLE);
        playPause.setImageResource(s.playback == 1
                ? android.R.drawable.ic_media_pause
                : android.R.drawable.ic_media_play);
    }
}
