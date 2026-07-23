package com.insidematrix.ps5linuxmanager;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.view.View;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.ProgressBar;
import android.widget.TextView;

/** Screen 2: full-screen WebView console for the selected system. */
public class WebConsoleActivity extends Activity {

    public static final String EXTRA_URL = "url";
    public static final String EXTRA_CONN_ID = "conn_id";
    public static final String EXTRA_NAME = "name";

    private WebView web;
    private View errorView;
    private ProgressBar progress;
    private String url;
    private String connId;
    private ConnectionsStore store;

    @SuppressLint("SetJavaScriptEnabled")
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_web);

        store = new ConnectionsStore(this);
        url = getIntent().getStringExtra(EXTRA_URL);
        connId = getIntent().getStringExtra(EXTRA_CONN_ID);
        if (url == null || url.isEmpty()) {
            finish();
            return;
        }

        web = findViewById(R.id.web);
        errorView = findViewById(R.id.error_view);
        progress = findViewById(R.id.progress);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        s.setLoadWithOverviewMode(true);
        s.setUseWideViewPort(true);
        s.setBuiltInZoomControls(true);
        s.setDisplayZoomControls(false);
        s.setMediaPlaybackRequiresUserGesture(false);

        web.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageStarted(WebView view, String u, Bitmap favicon) {
                showWeb();
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                if (request.isForMainFrame()) {
                    CharSequence desc = error.getDescription();
                    showError("error " + error.getErrorCode()
                            + (desc != null ? " — " + desc : ""));
                }
            }
        });

        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public void onProgressChanged(WebView view, int newProgress) {
                progress.setProgress(newProgress);
                progress.setVisibility(newProgress >= 100 ? View.GONE : View.VISIBLE);
            }
        });

        findViewById(R.id.btn_retry).setOnClickListener(v -> {
            showWeb();
            web.loadUrl(url);
        });

        findViewById(R.id.btn_edit_conn).setOnClickListener(v -> {
            Connection existing = connId != null ? store.findById(connId) : null;
            ConnectionDialog.show(this, existing, saved -> {
                if (existing != null) {
                    store.update(saved);
                } else {
                    store.add(saved);
                    connId = saved.id;
                }
                url = saved.url();
                setTitle(saved.name);
                showWeb();
                web.loadUrl(url);
            });
        });

        setTitle(getIntent().getStringExtra(EXTRA_NAME));
        web.loadUrl(url);
    }

    private void showWeb() {
        errorView.setVisibility(View.GONE);
        web.setVisibility(View.VISIBLE);
    }

    private void showError(String detail) {
        web.setVisibility(View.GONE);
        errorView.setVisibility(View.VISIBLE);
        ((TextView) findViewById(R.id.error_url)).setText(url);
        ((TextView) findViewById(R.id.error_detail)).setText(detail);
    }

    @Override
    public void onBackPressed() {
        if (errorView.getVisibility() == View.VISIBLE) {
            super.onBackPressed();
            return;
        }
        if (web.canGoBack()) {
            web.goBack();
        } else {
            super.onBackPressed();
        }
    }
}
