package com.insidematrix.ps5linuxmanager;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.ListView;
import android.widget.TextView;

import java.util.List;

/** Screen 1: saved-systems launcher. */
public class MainActivity extends Activity {

    private ConnectionsStore store;
    private List<Connection> conns;
    private ArrayAdapter<Connection> adapter;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        store = new ConnectionsStore(this);
        conns = store.load();

        ListView list = findViewById(R.id.list);
        adapter = new ArrayAdapter<Connection>(this, R.layout.row_connection, R.id.row_name, conns) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                View v = super.getView(position, convertView, parent);
                Connection c = getItem(position);
                TextView name = v.findViewById(R.id.row_name);
                TextView url = v.findViewById(R.id.row_url);
                if (c != null) {
                    name.setText(c.name);
                    url.setText(c.url());
                }
                return v;
            }
        };
        list.setAdapter(adapter);

        list.setOnItemClickListener((parent, view, position, id) -> {
            Connection c = conns.get(position);
            if (c.isPlaceholder()) {
                // preset still has "<ip>" — let the user fill it in first
                ConnectionDialog.show(this, c, saved -> {
                    store.update(saved);
                    refresh();
                });
                return;
            }
            Intent i = new Intent(this, WebConsoleActivity.class);
            i.putExtra(WebConsoleActivity.EXTRA_URL, c.url());
            i.putExtra(WebConsoleActivity.EXTRA_CONN_ID, c.id);
            i.putExtra(WebConsoleActivity.EXTRA_NAME, c.name);
            startActivity(i);
        });

        list.setOnItemLongClickListener((parent, view, position, id) -> {
            Connection c = conns.get(position);
            new AlertDialog.Builder(this)
                    .setTitle(c.name)
                    .setItems(new CharSequence[]{"Edit", "Delete"}, (d, which) -> {
                        if (which == 0) {
                            ConnectionDialog.show(this, c, saved -> {
                                store.update(saved);
                                refresh();
                            });
                        } else {
                            new AlertDialog.Builder(this)
                                    .setMessage("Delete \"" + c.name + "\"?")
                                    .setPositiveButton("Delete", (dd, ww) -> {
                                        store.delete(c.id);
                                        refresh();
                                    })
                                    .setNegativeButton("Cancel", null)
                                    .show();
                        }
                    })
                    .show();
            return true;
        });

        findViewById(R.id.btn_add).setOnClickListener(v ->
                ConnectionDialog.show(this, null, saved -> {
                    store.add(saved);
                    refresh();
                }));

        findViewById(R.id.btn_sender).setOnClickListener(v ->
                startActivity(new Intent(this, PayloadSenderActivity.class)));
    }

    private void refresh() {
        conns.clear();
        conns.addAll(store.load());
        adapter.notifyDataSetChanged();
    }
}
