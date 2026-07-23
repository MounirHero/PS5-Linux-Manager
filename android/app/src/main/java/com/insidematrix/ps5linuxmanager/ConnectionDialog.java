package com.insidematrix.ps5linuxmanager;

import android.app.Activity;
import android.app.AlertDialog;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.EditText;
import android.widget.Toast;

/** Shared add/edit dialog for a saved system connection. */
public class ConnectionDialog {

    public interface Callback {
        void onSaved(Connection c);
    }

    /** @param existing null to add a new connection, otherwise pre-fills and keeps the id. */
    public static void show(Activity act, Connection existing, Callback cb) {
        View v = LayoutInflater.from(act).inflate(R.layout.dialog_connection, null);
        EditText etName = v.findViewById(R.id.et_name);
        EditText etHost = v.findViewById(R.id.et_host);
        EditText etPort = v.findViewById(R.id.et_port);
        if (existing != null) {
            etName.setText(existing.name);
            etHost.setText(existing.host);
            etPort.setText(String.valueOf(existing.port));
        }
        new AlertDialog.Builder(act)
                .setTitle(existing == null ? "Add system" : "Edit system")
                .setView(v)
                .setPositiveButton("Save", (d, w) -> {
                    Connection c = Connection.fromInput(
                            etName.getText().toString(),
                            etHost.getText().toString(),
                            etPort.getText().toString());
                    if (existing != null) c.id = existing.id;
                    if (c.host.isEmpty()) {
                        Toast.makeText(act, "Enter an IP / host", Toast.LENGTH_SHORT).show();
                        return;
                    }
                    cb.onSaved(c);
                })
                .setNegativeButton("Cancel", null)
                .show();
    }
}
