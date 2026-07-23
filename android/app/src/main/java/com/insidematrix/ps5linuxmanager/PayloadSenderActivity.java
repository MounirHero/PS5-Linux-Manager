package com.insidematrix.ps5linuxmanager;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/** Screen 3: stream an .elf payload to the PS5 over a raw TCP socket. */
public class PayloadSenderActivity extends Activity {

    private static final int REQ_PICK = 41;
    private static final int CHUNK = 64 * 1024; // 64 KiB
    private static final int MAX_HISTORY = 5;

    private SharedPreferences prefs;
    private Uri fileUri;
    private String fileName;
    private long fileSize;

    private TextView tvFile, tvStatus, tvHistory;
    private EditText etIp, etPort;
    private Button btnSend;
    private ProgressBar sendProgress;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_sender);

        prefs = getSharedPreferences("sender", MODE_PRIVATE);

        tvFile = findViewById(R.id.tv_file);
        tvStatus = findViewById(R.id.tv_status);
        tvHistory = findViewById(R.id.tv_history);
        etIp = findViewById(R.id.et_ip);
        etPort = findViewById(R.id.et_port);
        btnSend = findViewById(R.id.btn_send);
        sendProgress = findViewById(R.id.send_progress);

        etIp.setText(prefs.getString("ip", ""));
        etPort.setText(prefs.getString("port", "9021"));
        renderHistory();

        findViewById(R.id.btn_pick).setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            startActivityForResult(i, REQ_PICK);
        });

        btnSend.setOnClickListener(v -> send());
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_PICK && resultCode == RESULT_OK && data != null) {
            fileUri = data.getData();
            fileName = "payload.elf";
            fileSize = -1;
            try (Cursor c = getContentResolver().query(fileUri, null, null, null, null)) {
                if (c != null && c.moveToFirst()) {
                    int ni = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    int si = c.getColumnIndex(OpenableColumns.SIZE);
                    if (ni >= 0) fileName = c.getString(ni);
                    if (si >= 0 && !c.isNull(si)) fileSize = c.getLong(si);
                }
            } catch (Exception ignored) {
            }
            tvFile.setText(fileName + (fileSize >= 0 ? "  (" + humanSize(fileSize) + ")" : ""));
        }
    }

    private void send() {
        if (fileUri == null) {
            Toast.makeText(this, "Choose an .elf file first", Toast.LENGTH_SHORT).show();
            return;
        }
        String ip = etIp.getText().toString().trim();
        String portStr = etPort.getText().toString().trim();
        if (ip.isEmpty()) {
            Toast.makeText(this, "Enter the PS5 IP address", Toast.LENGTH_SHORT).show();
            return;
        }
        final int port;
        try {
            port = Integer.parseInt(portStr);
        } catch (NumberFormatException e) {
            Toast.makeText(this, "Invalid port", Toast.LENGTH_SHORT).show();
            return;
        }

        prefs.edit().putString("ip", ip).putString("port", portStr).apply();

        btnSend.setEnabled(false);
        sendProgress.setProgress(0);
        tvStatus.setText("Connecting to " + ip + ":" + port + " ...");

        final Uri uri = fileUri;
        final String name = fileName;
        final long total = fileSize;
        new Thread(() -> {
            long sent = 0;
            String error = null;
            try (Socket sock = new Socket()) {
                sock.connect(new InetSocketAddress(ip, port), 10000);
                sock.setTcpNoDelay(true);
                try (OutputStream out = sock.getOutputStream();
                     InputStream in = getContentResolver().openInputStream(uri)) {
                    if (in == null) throw new Exception("cannot open file");
                    byte[] buf = new byte[CHUNK];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                        sent += n;
                        if (total > 0) {
                            final int p = (int) (sent * 1000 / total);
                            final long s = sent;
                            runOnUiThread(() -> {
                                sendProgress.setProgress(p);
                                tvStatus.setText("Sending " + name + "  " + humanSize(s)
                                        + " / " + humanSize(total) + "  (" + (p / 10) + "%)");
                            });
                        } else {
                            final long s = sent;
                            runOnUiThread(() -> tvStatus.setText("Sending " + name
                                    + "  " + humanSize(s) + " ..."));
                        }
                    }
                    out.flush();
                }
            } catch (Exception e) {
                error = e.getClass().getSimpleName() + ": " + e.getMessage();
            }
            final long fSent = sent;
            final String fError = error;
            runOnUiThread(() -> {
                btnSend.setEnabled(true);
                if (fError == null) {
                    sendProgress.setProgress(1000);
                    tvStatus.setText("OK — sent " + fSent + " bytes to " + ip + ":" + port);
                    addHistory(name + " -> " + ip + ":" + port + "  (" + fSent + " bytes)");
                } else {
                    tvStatus.setText("FAILED after " + fSent + " bytes — " + fError);
                    addHistory(name + " -> " + ip + ":" + port + "  FAILED");
                }
            });
        }, "payload-sender").start();
    }

    private void addHistory(String entry) {
        String stamp = new SimpleDateFormat("MM-dd HH:mm", Locale.US).format(new Date());
        try {
            JSONArray arr = new JSONArray(prefs.getString("history", "[]"));
            JSONArray next = new JSONArray();
            next.put(stamp + "  " + entry);
            for (int i = 0; i < arr.length() && i < MAX_HISTORY - 1; i++) {
                next.put(arr.getString(i));
            }
            prefs.edit().putString("history", next.toString()).apply();
        } catch (Exception ignored) {
        }
        renderHistory();
    }

    private void renderHistory() {
        try {
            JSONArray arr = new JSONArray(prefs.getString("history", "[]"));
            if (arr.length() == 0) {
                tvHistory.setText("No payloads sent yet");
                return;
            }
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < arr.length(); i++) {
                sb.append(arr.getString(i));
                if (i < arr.length() - 1) sb.append('\n');
            }
            tvHistory.setText(sb.toString());
        } catch (Exception e) {
            tvHistory.setText("No payloads sent yet");
        }
    }

    private static String humanSize(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return String.format(Locale.US, "%.1f KB", bytes / 1024.0);
        return String.format(Locale.US, "%.2f MB", bytes / (1024.0 * 1024.0));
    }
}
