package com.rubfer.tinyspot;

import android.app.Activity;
import android.os.Bundle;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.CheckBox;
import android.widget.ListView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/** The account's playlists, fetched and played entirely on the watch. */
public class LibraryActivity extends Activity implements PlayerService.UiListener {
    private final List<String> uris = new ArrayList<>();
    private ArrayAdapter<String> adapter;
    private TextView status;
    private CheckBox shuffle;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_library);
        status = findViewById(R.id.status);
        shuffle = findViewById(R.id.shuffle);
        ListView list = findViewById(R.id.list);
        adapter = new ArrayAdapter<>(this, R.layout.item_library);
        list.setAdapter(adapter);
        list.setOnItemClickListener((parent, view, position, id) -> {
            NativePlayer.nativePlayContext(uris.get(position), shuffle.isChecked());
            finish();
        });
    }

    @Override
    protected void onStart() {
        super.onStart();
        PlayerService.setUiListener(this);
        if (PlayerService.state.playlists == null) {
            status.setText(PlayerService.state.auth == 2 ? "Loading…" : "Not connected");
            if (PlayerService.state.auth == 2) NativePlayer.nativeRequestPlaylists();
        }
    }

    @Override
    protected void onStop() {
        PlayerService.setUiListener(null);
        super.onStop();
    }

    @Override
    public void onStateChanged(PlayerService.State s) {
        if (s.playlists == null) return;
        uris.clear();
        adapter.clear();
        for (String line : s.playlists.split("\n")) {
            int tab = line.indexOf('\t');
            if (tab <= 0) continue;
            uris.add(line.substring(0, tab));
            adapter.add(line.substring(tab + 1));
        }
        status.setVisibility(uris.isEmpty() ? View.VISIBLE : View.GONE);
        status.setText("Couldn't load playlists");
    }
}
