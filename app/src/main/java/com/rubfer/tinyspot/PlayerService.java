package com.rubfer.tinyspot;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;

/**
 * Owns the native player for the life of the app: Wi-Fi, mDNS advertisement,
 * stored credentials and the foreground notification that keeps playback
 * alive with the screen off.
 */
public class PlayerService extends Service implements NativePlayer.Listener {
    private static final String TAG = "TinySpot-Java";
    private static final String DEVICE_NAME = "TinySpot";
    private static final int ZEROCONF_PORT = 7864;
    private static final String CHANNEL = "playback";

    /** UI-facing snapshot; mutated on the main thread only. */
    static final class State {
        int auth;
        int playback;
        String title = "";
        String artist = "";
        int durationMs;
        String error;
        /** "uri\tname\n" lines, null until loaded. */
        String playlists;
    }

    interface UiListener {
        void onStateChanged(State s);
    }

    static final State state = new State();
    private static UiListener uiListener;

    private final Handler main = new Handler(Looper.getMainLooper());
    private ConnectivityManager cm;
    private ConnectivityManager.NetworkCallback wifiCallback;
    private NsdManager nsd;
    private NsdManager.RegistrationListener nsdListener;
    private boolean nativeStarted;
    private Network boundNetwork;
    private MediaSessionManager media;

    static void setUiListener(UiListener l) {
        uiListener = l;
        if (l != null) l.onStateChanged(state);
    }

    static void start(Context c) {
        Intent i = new Intent(c, PlayerService.class);
        if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i);
        else c.startService(i);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        startForeground(1, buildNotification());
        NativePlayer.setListener(this);
        media = new MediaSessionManager(this);
        cm = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
        nsd = (NsdManager) getSystemService(NSD_SERVICE);
        requestWifi();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        unregisterNsd();
        if (wifiCallback != null) {
            cm.bindProcessToNetwork(null);
            cm.unregisterNetworkCallback(wifiCallback);
        }
        NativePlayer.setListener(null);
        if (media != null) media.release();
        if (nativeStarted) NativePlayer.nativeShutdown();
        super.onDestroy();
    }

    /**
     * Wear OS routes traffic through the phone over Bluetooth and powers the
     * radios down when it can. Ask for a direct link (Wi-Fi or LTE, never the
     * Bluetooth proxy) and bind the whole process, native sockets included.
     * When the bound network drops, cspot's session reconnects on the next one.
     */
    private void requestWifi() {
        NetworkRequest req = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build();
        wifiCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                NetworkCapabilities caps = cm.getNetworkCapabilities(network);
                boolean wifi = caps != null && caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI);
                Log.i(TAG, (wifi ? "Wi-Fi" : "LTE") + " available, binding process to it");
                cm.bindProcessToNetwork(network);
                boolean switched = boundNetwork != null && !boundNetwork.equals(network);
                boundNetwork = network;
                main.post(() -> {
                    if (nativeStarted && switched) NativePlayer.nativeNetworkChanged();
                    startNative();
                });
            }

            @Override
            public void onLost(Network network) {
                Log.w(TAG, "network lost");
            }
        };
        cm.requestNetwork(req, wifiCallback);
    }

    private void startNative() {
        if (nativeStarted) return;
        nativeStarted = true;
        NativePlayer.nativeInitialize(DEVICE_NAME);
        int port = NativePlayer.nativeStartDiscovery(ZEROCONF_PORT);
        if (port > 0) registerNsd(port);

        String creds = prefs().getString("credentials", null);
        if (creds != null) {
            Log.i(TAG, "logging in with stored credentials");
            NativePlayer.nativeLogin(creds);
        }
    }

    private void registerNsd(int port) {
        NsdServiceInfo info = new NsdServiceInfo();
        info.setServiceName(DEVICE_NAME);
        info.setServiceType("_spotify-connect._tcp");
        info.setPort(port);
        info.setAttribute("VERSION", "1.0");
        info.setAttribute("CPath", "/spotify_info");
        info.setAttribute("Stack", "SP");
        nsdListener = new NsdManager.RegistrationListener() {
            @Override
            public void onServiceRegistered(NsdServiceInfo s) {
                Log.i(TAG, "mDNS registered: " + s.getServiceName());
            }

            @Override
            public void onRegistrationFailed(NsdServiceInfo s, int err) {
                Log.e(TAG, "mDNS registration failed: " + err);
                nsdListener = null;
            }

            @Override
            public void onServiceUnregistered(NsdServiceInfo s) {}

            @Override
            public void onUnregistrationFailed(NsdServiceInfo s, int err) {}
        };
        nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, nsdListener);
    }

    private void unregisterNsd() {
        if (nsdListener != null) {
            nsd.unregisterService(nsdListener);
            nsdListener = null;
        }
    }

    private SharedPreferences prefs() {
        return getSharedPreferences("auth", MODE_PRIVATE);
    }

    // ------------------------------------------------------------------
    // Native events (native thread) -> main thread.
    // ------------------------------------------------------------------
    @Override
    public void onNativeEvent(int type, int arg, String text) {
        if (type == NativePlayer.EV_CREDENTIALS) {
            prefs().edit().putString("credentials", text).apply();
            Log.i(TAG, "stored reusable credentials");
            return;
        }
        main.post(() -> applyEvent(type, arg, text));
    }

    private void applyEvent(int type, int arg, String text) {
        switch (type) {
            case NativePlayer.EV_AUTH_STATE:
                state.auth = arg;
                if (arg == 2 && state.playlists == null) NativePlayer.nativeRequestPlaylists();
                if (arg == 3) prefs().edit().remove("credentials").apply();
                break;
            case NativePlayer.EV_PLAYBACK_STATE:
                state.playback = arg;
                break;
            case NativePlayer.EV_TRACK_CHANGED: {
                String[] f = text != null ? text.split("\n", -1) : new String[0];
                state.title = f.length > 0 ? f[0] : "";
                state.artist = f.length > 1 ? f[1] : "";
                state.durationMs = arg;
                break;
            }
            case NativePlayer.EV_PLAYLISTS:
                state.playlists = arg == 1 ? text : "";
                break;
            case NativePlayer.EV_ERROR:
                state.error = text;
                Log.w(TAG, "native error: " + text);
                break;
            default:
                return;
        }
        if (media != null) media.update(state);
        if (uiListener != null) uiListener.onStateChanged(state);
    }

    private Notification buildNotification() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(
                    new NotificationChannel(CHANNEL, "Playback", NotificationManager.IMPORTANCE_LOW));
            b = new Notification.Builder(this, CHANNEL);
        } else {
            b = new Notification.Builder(this);
        }
        PendingIntent open = PendingIntent.getActivity(
                this, 0, new Intent(this, MainActivity.class), PendingIntent.FLAG_UPDATE_CURRENT);
        return b.setSmallIcon(android.R.drawable.ic_media_play)
                .setContentTitle("TinySpot")
                .setContentText("Spotify Connect ready")
                .setContentIntent(open)
                .setOngoing(true)
                .build();
    }
}
