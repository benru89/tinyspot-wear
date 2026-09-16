package com.rubfer.tinyspot;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

public class MainActivity extends Activity {
    private static final String TAG = "TinySpot-Java";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView tv = new TextView(this);
        tv.setPadding(24, 48, 24, 24);
        String info = NativePlayer.nativeProbe();
        Log.i(TAG, "probe: " + info);
        tv.setText("TinySpot\n\ncspot: " + info);
        setContentView(tv);
    }
}
