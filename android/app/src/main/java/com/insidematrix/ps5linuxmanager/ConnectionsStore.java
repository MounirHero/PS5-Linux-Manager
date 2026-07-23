package com.insidematrix.ps5linuxmanager;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/** SharedPreferences-backed storage for saved system connections. */
public class ConnectionsStore {
    private static final String PREFS = "connections";
    private static final String KEY_LIST = "list";

    private final SharedPreferences prefs;

    public ConnectionsStore(Context ctx) {
        prefs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        if (!prefs.contains(KEY_LIST)) {
            // ship with one preset pointing at the project's web UI
            Connection preset = new Connection();
            preset.id = "preset-ps5-linux-manager";
            preset.name = "PS5 Linux Manager";
            preset.host = "<ip>";
            preset.port = 8090;
            List<Connection> list = new ArrayList<>();
            list.add(preset);
            save(list);
        }
    }

    public List<Connection> load() {
        List<Connection> list = new ArrayList<>();
        String raw = prefs.getString(KEY_LIST, "[]");
        try {
            JSONArray arr = new JSONArray(raw);
            for (int i = 0; i < arr.length(); i++) {
                JSONObject o = arr.optJSONObject(i);
                if (o != null) list.add(Connection.fromJson(o));
            }
        } catch (Exception ignored) {
        }
        return list;
    }

    public void save(List<Connection> list) {
        JSONArray arr = new JSONArray();
        try {
            for (Connection c : list) arr.put(c.toJson());
        } catch (Exception ignored) {
        }
        prefs.edit().putString(KEY_LIST, arr.toString()).apply();
    }

    public void add(Connection c) {
        List<Connection> list = load();
        list.add(c);
        save(list);
    }

    public void update(Connection c) {
        List<Connection> list = load();
        for (int i = 0; i < list.size(); i++) {
            if (list.get(i).id.equals(c.id)) {
                list.set(i, c);
                break;
            }
        }
        save(list);
    }

    public void delete(String id) {
        List<Connection> list = load();
        List<Connection> keep = new ArrayList<>();
        for (Connection c : list) {
            if (!c.id.equals(id)) keep.add(c);
        }
        save(keep);
    }

    public Connection findById(String id) {
        for (Connection c : load()) {
            if (c.id.equals(id)) return c;
        }
        return null;
    }
}
