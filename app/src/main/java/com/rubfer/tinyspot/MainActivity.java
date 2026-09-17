package com.rubfer.tinyspot;

import android.app.Activity;
import android.os.Bundle;
import android.view.View;
import android.widget.ImageButton;
import android.widget.ToggleButton;
import android.widget.TextView;

public class MainActivity extends Activity implements PlayerService.UiListener {
    private static final String[] AUTH = {"Not logged in", "Connecting…", "Connected", "Login failed"};
    private static final String[] PLAYBACK = {"Stopped", "Playing", "Paused", "Buffering…"};

    private TextView spotify;
    private TextView player;
    private TextView track;
    private TextView position;
    private ImageButton playPause;
    private ToggleButton shuffle;
    private final android.os.Handler ticker = new android.os.Handler();
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            showPosition();
            ticker.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        spotify = findViewById(R.id.spotify);
        player = findViewById(R.id.player);
        track = findViewById(R.id.track);
        position = findViewById(R.id.position);
        playPause = findViewById(R.id.play_pause);
        shuffle = findViewById(R.id.shuffle);
        shuffle.setChecked(PlayerService.shuffle());
        shuffle.setOnCheckedChangeListener((v, on) -> PlayerService.setShuffle(on));

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

    /**
     * adb testing: am start -n .../.MainActivity --es play URI [--ez shuffle true]
     * or --es cmd pause|resume|next|prev
     */
    private static void debugPlay(android.content.Intent intent) {
        if (!BuildConfig.DEBUG && !BuildConfig.TEST_HOOKS) return;
        String play = intent.getStringExtra("play");
        if (play != null) {
            NativePlayer.nativePlayContext(play, intent.getBooleanExtra("shuffle", false));
        }
        String cmd = intent.getStringExtra("cmd");
        if (cmd == null) return;
        switch (cmd) {
            case "pause": NativePlayer.nativePause(); break;
            case "resume": NativePlayer.nativeResume(); break;
            case "next": NativePlayer.nativeNext(); break;
            case "prev": NativePlayer.nativePrevious(); break;
            default: break;
        }
    }

    // Only listen while visible: no UI work with the screen off.
    @Override
    protected void onStart() {
        super.onStart();
        PlayerService.setUiListener(this);
        ticker.post(tick);   // only while visible: no UI work with the screen off
    }

    @Override
    protected void onStop() {
        PlayerService.setUiListener(null);
        ticker.removeCallbacks(tick);
        super.onStop();
    }

    private static String mmss(int ms) {
        int total = Math.max(ms, 0) / 1000;
        return total / 60 + ":" + (total % 60 < 10 ? "0" : "") + total % 60;
    }

    private void showPosition() {
        PlayerService.State s = PlayerService.state;
        boolean show = s.durationMs > 0 && s.playback != 0;
        position.setText(show ? mmss(s.currentPositionMs()) + " / " + mmss(s.durationMs) : "");
    }

    @Override
    public void onStateChanged(PlayerService.State s) {
        showPosition();
        spotify.setText(s.auth == 0 && s.error == null
                ? "Open Spotify → Devices → TinySpot"
                : AUTH[s.auth]);
        player.setText(s.silent ? "Playing – volume is 0" : PLAYBACK[s.playback]);
        track.setText(s.title.isEmpty() ? "—" : s.artist + " – " + s.title);
        track.setVisibility(s.auth == 2 ? View.VISIBLE : View.INVISIBLE);
        playPause.setImageResource(s.playback == 1
                ? android.R.drawable.ic_media_pause
                : android.R.drawable.ic_media_play);
    }
}
