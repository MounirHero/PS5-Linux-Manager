package com.insidematrix.ps5linuxmanager;

import org.json.JSONException;
import org.json.JSONObject;

import java.net.URI;
import java.util.UUID;

/** A saved system connection (name + host + port). */
public class Connection {
    public String id;
    public String name;
    public String host;
    public int port;

    public String url() {
        return "http://" + host + ":" + port;
    }

    public boolean isPlaceholder() {
        return host == null || host.trim().isEmpty() || host.contains("<");
    }

    /**
     * Build a Connection from free-form user input. The host field accepts
     * a bare IP/host, host:port, or a full URL like http://192.168.1.10:8084/path.
     */
    public static Connection fromInput(String name, String hostOrUrl, String portStr) {
        Connection c = new Connection();
        c.id = UUID.randomUUID().toString();
        c.name = name == null ? "" : name.trim();
        String h = hostOrUrl == null ? "" : hostOrUrl.trim();
        int port = -1;
        try {
            if (portStr != null && !portStr.trim().isEmpty()) {
                port = Integer.parseInt(portStr.trim());
            }
        } catch (NumberFormatException ignored) {
        }

        if (h.contains("://")) {
            try {
                URI u = new URI(h);
                if (u.getHost() != null) h = u.getHost();
                if (u.getPort() > 0) port = u.getPort();
            } catch (Exception ignored) {
                h = h.substring(h.indexOf("://") + 3);
            }
        }
        // strip any remaining path and scheme leftovers
        int slash = h.indexOf('/');
        if (slash >= 0) h = h.substring(0, slash);
        // host:port without scheme
        int colon = h.lastIndexOf(':');
        if (colon > 0 && colon < h.length() - 1) {
            try {
                port = Integer.parseInt(h.substring(colon + 1));
                h = h.substring(0, colon);
            } catch (NumberFormatException ignored) {
            }
        }
        c.host = h;
        c.port = port > 0 ? port : 8090;
        if (c.name.isEmpty()) c.name = c.host + ":" + c.port;
        return c;
    }

    public JSONObject toJson() throws JSONException {
        JSONObject o = new JSONObject();
        o.put("id", id);
        o.put("name", name);
        o.put("host", host);
        o.put("port", port);
        return o;
    }

    public static Connection fromJson(JSONObject o) {
        Connection c = new Connection();
        c.id = o.optString("id", UUID.randomUUID().toString());
        c.name = o.optString("name", "");
        c.host = o.optString("host", "");
        c.port = o.optInt("port", 8090);
        return c;
    }
}
