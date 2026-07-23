#!/usr/bin/env python3
"""Inline a Vite build (webui/dist) into a single self-contained index.html.

The payload's HTTP server serves exactly one file (plus SPA fallback), so all
JS/CSS/images must live inside index.html. Run after `npm run build`:

    python3 scripts/inline-dist.py

Rewrites webui/dist/index.html in place: <script src> -> inline module,
<link stylesheet> -> <style>, and every public/ image reference (html, css
url(), js string literals) -> base64 data URI.
"""
import base64
import os
import re
import sys

DIST = os.path.join(os.path.dirname(__file__), "..", "webui", "dist")
MIME = {".svg": "image/svg+xml", ".png": "image/png", ".jpg": "image/jpeg"}


def main() -> int:
    index = os.path.join(DIST, "index.html")
    if not os.path.isfile(index):
        print("error: webui/dist/index.html not found - run `npm run build` in webui/ first", file=sys.stderr)
        return 1
    html = open(index, encoding="utf-8").read()

    def inline_js(m: re.Match) -> str:
        js = open(os.path.join(DIST, m.group(1)), encoding="utf-8").read()
        return f'<script type="module">{js}</script>'

    html = re.sub(r'<script type="module" crossorigin src="/?([^"]+)"></script>', inline_js, html)

    css_text = ""

    def inline_css(m: re.Match) -> str:
        nonlocal css_text
        css_text = open(os.path.join(DIST, m.group(1)), encoding="utf-8").read()
        return "@@CSS@@"

    html = re.sub(r'<link rel="stylesheet" crossorigin href="/?([^"]+)">', inline_css, html)

    data_uris = {}
    for f in os.listdir(DIST):
        ext = os.path.splitext(f)[1]
        if ext in MIME:
            b64 = base64.b64encode(open(os.path.join(DIST, f), "rb").read()).decode()
            data_uris[f] = f"data:{MIME[ext]};base64,{b64}"

    for name, uri in data_uris.items():
        css_text = re.sub(r'url\((["\']?)(\.\./|/)?' + re.escape(name) + r'\1\)', f"url({uri})", css_text)
    html = html.replace("@@CSS@@", f"<style>{css_text}</style>")

    for name, uri in data_uris.items():
        html = (html.replace(f'"/{name}"', f'"{uri}"')
                    .replace(f"'/{name}'", f"'{uri}'")
                    .replace(f'href="/{name}"', f'href="{uri}"'))

    external = len(re.findall(r'src="/|href="/|url\(/', html))
    if external:
        print(f"warning: {external} external references remain", file=sys.stderr)
    open(index, "w", encoding="utf-8").write(html)
    print(f"inlined -> {index} ({len(html)} bytes, {external} external refs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
