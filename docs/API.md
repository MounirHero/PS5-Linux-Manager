# PS5 Linux Manager — HTTP API Contract v1.1 (webui ↔ C payload)

Base: `http://<ps5-ip>:8090`. All JSON unless noted. **No mock adapter anywhere** — the
web UI always talks to the live payload. Project name: **PS5 Linux Manager by InsideMatrix**.

## GET /api/status
→ `{ "consoleName": "PS5-720", "modelCode": "CFI-1215A", "firmware": "4.51",
     "kitType": "retail|devkit|testkit", "ip": "192.168.1.50", "port": 8090,
     "uptimeSec": 1234, "loaderPresent": true,
     "loaderPath": "/mnt/usb0/ps5-linux-loader.elf",
     "linuxDevice": "usb0",
     "usbs": [{ "mount": "/mnt/usb0", "name": "usb0", "present": true, "linuxFiles": true }],
     "ftp": { "enabled": true, "port": 2121, "user": "", "passSet": false },
     "appInstalled": true, "version": "1.1.0", "author": "InsideMatrix" }`
(consoleName/modelCode are read from the real console — registry/kernel — never hardcoded.)

## Linux files — selected USB device ROOT (no PS5/Linux subdirectory)
## GET /api/linux/device   → `{ "device": "usb0", "dir": "/mnt/usb0" }`
## POST /api/linux/device  body `{ "device": "usb2" }` → `{ "ok": true }` (persisted)
## GET /api/linux/files    → `{ "device": "usb0", "dir": "/mnt/usb0",
     "files": [{ "name": "bzImage", "path": "/mnt/usb0/bzImage", "size": 123, "mtime": 1721692800,
                 "kind": "kernel|initrd|config|loader|other", "present": true }],
     "required": { "bzImage": true, "initrd.img": true, "cmdline.txt": false } }`
   (scans the ROOT of the selected device for bzImage, initrd.img, cmdline.txt, vram.txt,
    and *.elf loaders. No path-override — it is replaced by the device selector.)
## GET /api/linux/config?name=cmdline.txt|vram.txt
→ `{ "name": "cmdline.txt", "content": "...", "exists": true }` (read from selected device root)
## POST /api/linux/config  body `{ "name": "...", "content": "..." }` → `{ "ok": true }`

## GET /api/bios → BIOS object (unchanged keys: resolution, refreshHz, output, vramGb,
   cpuGovernor, sshEnabled, bootMode(normal|recovery|single), rootDevice, kernelParams)
## POST /api/bios → `{ "ok": true }`; side effects: vramGb → vram.txt and
   kernelParams+bootMode+rootDevice → cmdline.txt ON THE SELECTED DEVICE ROOT.
   Invalid bootMode → HTTP 400 `{"error":"invalid bootMode"}`.

## Dual boot / GRUB (entries point at device ROOT paths, e.g. /mnt/usb0/bzImage)
## GET /api/boot/grub
→ `{ "enabled": true, "timeoutSec": 5, "defaultEntry": "linux-usb0",
     "autoBoot": "manager|linux|orbis", "entries": [
       { "id": "linux-usb0", "title": "PS5 Linux (usb0)", "type": "linux",
         "kernel": "/mnt/usb0/bzImage", "initrd": "/mnt/usb0/initrd.img",
         "cmdline": "...", "enabled": true },
       { "id": "orbis", "title": "Orbis OS (PS5 System)", "type": "orbis", "enabled": true }],
     "grubCfg": "<text>" }`
## POST /api/boot/grub → `{ "ok": true }` (entries.json + grub.cfg in
   /data/PS5_LINUX_MANAGER/, grub.cfg mirrored to selected device root)

## Payloads — ONLY manually uploaded files, stored in /data/PS5_LINUX_MANAGER/PAYLOADS/
## GET /api/payloads
→ `{ "payloads": [{ "name": "ps5-linux-loader.elf", "path": "/data/PS5_LINUX_MANAGER/PAYLOADS/ps5-linux-loader.elf",
                     "size": 179472, "mtime": 1721692800 }] }`
## POST /api/payloads/upload?name=x.elf   body = raw ELF bytes (application/octet-stream)
→ `{ "ok": true, "path": "/data/PS5_LINUX_MANAGER/PAYLOADS/x.elf" }`
## DELETE /api/payloads?name=x.elf → `{ "ok": true }`
## POST /api/launch  body `{ "path": "/data/PS5_LINUX_MANAGER/PAYLOADS/x.elf" }`
→ `{ "ok": true, "message": "Payload served on port 9021" }`
   (the payload CONNECTS to 127.0.0.1:9021 — the console's own ELF loader — and sends the
    ELF bytes; it never execs payloads from memory)
## POST /api/boot/linux → `{ "ok": true, "message": "..." }`
   (finds the Linux loader ELF — uploaded ps5-linux-loader*.elf preferred, else *.elf on
    selected device root — and serves it to 127.0.0.1:9021)
## POST /api/boot/orbis → `{ "ok": true }` (exit manager / return to Orbis OS)

## Real console filesystem (powers the Files page; no mock data)
## GET /api/fs/list?path=/mnt/usb0
→ `{ "path": "/mnt/usb0", "entries": [{ "name": "bzImage", "path": "/mnt/usb0/bzImage",
     "type": "file|dir", "size": 123, "mtime": 1721692800 }] }`
## GET /api/fs/stat?path=... → `{ "path": "...", "type": "file|dir", "size": 0, "mtime": 0 }`
## GET /api/fs/read?path=...  → `{ "path": "...", "content": "...", "truncated": false }` (text ≤ 256 KiB)
## POST /api/fs/write   body `{ "path": "...", "content": "..." }` → `{ "ok": true }` (atomic)
## POST /api/fs/mkdir   body `{ "path": "..." }` → `{ "ok": true }`
## POST /api/fs/delete  body `{ "path": "..." }` → `{ "ok": true }` (recursive for dirs)
## POST /api/fs/rename  body `{ "from": "...", "to": "..." }` → `{ "ok": true }` (move/rename)
## POST /api/fs/copy    body `{ "from": "...", "to": "..." }` → `{ "ok": true }` (recursive for dirs)

## Embedded FTP server (for PC FTP clients; like Elf Arsenal's ftpsrv)
## GET /api/ftp  → `{ "enabled": true, "port": 2121, "user": "", "passSet": false }`
## POST /api/ftp body `{ "enabled": true, "port": 2121, "user": "", "pass": "" }` → `{ "ok": true }`
   (restarts the in-payload FTP daemon; anonymous when user empty; full FS access)

## GET /api/settings → `{ "port": 8090, "autostart": false, "notifications": true,
     "theme": "dark", "grubMirrorUsb": false, "biosSyncCmdline": false }`
## POST /api/settings → `{ "ok": true }`
   SAFETY (v1.2): when `grubMirrorUsb` is false (DEFAULT), POST /api/boot/grub writes
   grub.cfg ONLY to /data/PS5_LINUX_MANAGER/ — never to the USB device. When
   `biosSyncCmdline` is false (DEFAULT), POST /api/bios persists bios.json only —
   it does NOT rewrite cmdline.txt/vram.txt on the USB. Both are opt-in so the
   manager can never silently alter files a distro's bootloader may read.

Errors: HTTP 4xx/5xx `{ "error": "message" }`.
Static: `GET /` → embedded single-file SPA (gzip), SPA fallback.
Startup behavior (C-side, not API): on first run install the home-screen app bubble
**"PS5 Linux Manager" into the Media tab** (app.db, deeplink http://127.0.0.1:8090/) and
send 3 system notifications: "Welcome To PS5 Linux Manager", "credits: InsideMatrix",
"Using port :8090".
