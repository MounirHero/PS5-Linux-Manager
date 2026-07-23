# PS5 Linux Manager by InsideMatrix — payload

PS5 payload (ps5-payload-sdk ELF) that serves a web UI + JSON API for
managing a PS5 Linux dual-boot setup, uploading and launching ELF
payloads, browsing the console filesystem, and transferring files over
an embedded FTP server.

- **Version:** 1.1.0 (API contract v1.1 + v1.2 safety switches — see
  `../api-contract.md`)
- **Author/credits:** InsideMatrix
- **Default HTTP port:** 8090 (configurable in Settings)

## What's new in v1.2 (safety)

Two **opt-in** switches (both default **OFF**, persisted in
`config.json`, exposed via `GET/POST /api/settings`) control every
write the manager makes to the USB device root — because a distro's own
bootloader may read those files:

- **`biosSyncCmdline`** — when false (default), `POST /api/bios` only
  persists `bios.json`; it does **not** rewrite `vram.txt` /
  `cmdline.txt` at the device root.  When true, the old sync behavior
  is restored.  `bootMode` whitelist validation stays unconditional.
- **`grubMirrorUsb`** — when false (default), `POST /api/boot/grub`
  writes `grub.cfg` only to `/data/PS5_LINUX_MANAGER/`; it is **not**
  mirrored to the device root.  When true, the mirror is restored.

Existing installs (no such keys in `config.json`) get the safe OFF
defaults.

> **RECOVERY:** if a distro fails to boot after using the manager,
> delete `cmdline.txt`, `vram.txt` and `grub.cfg` from the **USB root**
> (`/mnt/<device>/`) to restore the distro's original boot files.

## What's new in v1.1

- Renamed to **PS5 Linux Manager by InsideMatrix** (version 1.1.0,
  `author: "InsideMatrix"` in `/api/status`).
- Data dir moved to **`/data/PS5_LINUX_MANAGER/`** with an uppercase
  **`PAYLOADS/`** subdirectory; `config.json`, `bios.json`,
  `entries.json` and `grub.cfg` all persist there.
- **Linux device selector** replaces the old `PS5/Linux` subdirectory
  and `path-override.txt` (both deleted): pick `usb0`..`usb3` and the
  loader files are read/written at the device **root**
  (`/mnt/<device>/bzImage`, `initrd.img`, `cmdline.txt`, `vram.txt`,
  `*.elf`).
- Payloads are **uploads only**: `GET /api/payloads` lists just
  `/data/PS5_LINUX_MANAGER/PAYLOADS/*.elf`; new raw upload + delete
  endpoints.
- Payload execution is **serve-to-9021**: the ELF is streamed over TCP
  to the console's ELF loader at `127.0.0.1:9021`; the old
  fork/execv in-memory launch path is gone.
- Real console identity (`consoleName`, `modelCode`), real `/api/fs/*`
  filesystem API, embedded FTP server, home-screen bubble install, and
  three startup notifications.

## Build

### Toolchain setup (once)

```sh
# 1) ps5-payload-sdk
mkdir -p ~/opt
curl -fSL -o /tmp/ps5-payload-sdk.zip \
  "https://ghfast.top/https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip"
unzip -q /tmp/ps5-payload-sdk.zip -d ~/opt/

# 2) LLVM-18 (the SDK's prospero-clang wrapper needs clang-18/lld-18)
B=https://apt.llvm.org/bookworm/pool/main/l/llvm-toolchain-18
S='18.1.8~++20240731024826+3b5b5c1ec4a3-1~exp1~20240731144843.145'
mkdir -p ~/llvm
for p in clang-18 libclang-common-18-dev libclang-cpp18 libllvm18 lld-18 llvm-18; do
  curl -fSL -o /tmp/$p.deb "$B/${p}_${S}_amd64.deb"
  dpkg-deb -x /tmp/$p.deb ~/llvm
done
export PATH=~/llvm/usr/bin:$PATH
export LD_LIBRARY_PATH=~/llvm/usr/lib/x86_64-linux-gnu
```

### Targets

```sh
export PS5_PAYLOAD_SDK=~/opt/ps5-payload-sdk
make            # builds ps5-linux-manager.elf (target: prospero)
make check      # host syntax check of every .c/.h (no SDK needed)
make smoke      # full host smoke test (see "Verification" below)
make clean
```

## Runtime layout

```
/data/PS5_LINUX_MANAGER/
  config.json          settings (port, autostart, notifications, theme,
                         grubMirrorUsb, biosSyncCmdline,
                         linuxDevice, ftp{enabled,port,user,pass})
  bios.json            BIOS-style settings edited via /api/bios
  entries.json         GRUB boot-entry model
  grub.cfg             generated GRUB config (mirrored to the device root
                         only when grubMirrorUsb is on)
  manager.log          log file
  app.installed        first-run marker for the home-screen bubble
  PAYLOADS/            uploaded .elf payloads (the ONLY payload source)
```

The selected Linux device (default `usb0`, persisted as `linuxDevice`)
maps to `/mnt/<device>/`; all loader files live at that root.

## API (contract v1.1)

### Status / settings

- `GET /api/status` — `{consoleName, modelCode, firmware, kitType, ip,
  port, uptimeSec, loaderPresent, loaderPath, linuxDevice, usbs[{mount,
  name, present, linuxFiles}], ftp{enabled, port, user, passSet},
  appInstalled, version, author}`.  `modelCode` (e.g. `"CFI-1215A"`) is
  detected best effort: `sceKernelGetHwModelName()` from
  `libkernel_sys.sprx` first, then `libSceRegMgr.sprx`
  `sceRegMgrGetStr`/`sceRegMgrGetBin` on documented candidate keys,
  finally `"CFI-XXXX"`; `consoleName` is `gethostname()` with a `"PS5"`
  fallback.  Detection never crashes the payload.
- `GET/POST /api/settings` — `{port, autostart, notifications, theme,
  grubMirrorUsb, biosSyncCmdline}` (exactly these six keys; device/FTP
  settings have their own endpoints).  The two opt-in safety switches
  default to `false` — see "What's new in v1.2".

### Linux device & files

- `GET /api/linux/device` → `{device, dir}`.
- `POST /api/linux/device {"device":"usb2"}` — validates the name,
  persists it in `config.json`, takes effect immediately.
- `GET /api/linux/files` → `{device, dir, files[{name, path, size,
  mtime, kind, present}], required:{bzImage, initrd.img, cmdline.txt}}`
  — files are scanned at the **device root**.
- `GET /api/linux/config?name=cmdline.txt|vram.txt` and
  `POST /api/linux/config {"name","content"}` — read/write those two
  files at the device root (atomic writes).  `path-override.txt` no
  longer exists (400).
- `GET/POST /api/bios` — POST persists `bios.json`; it writes
  `vram.txt` + the composed `cmdline.txt` to the device root **only
  when `biosSyncCmdline` is enabled** (off by default).  An invalid
  `bootMode` is always rejected with HTTP 400.
- `GET/POST /api/boot/grub` — POST persists `entries.json` and
  regenerates `grub.cfg` in the data dir; it is mirrored to the device
  root **only when `grubMirrorUsb` is enabled** (off by default).

### Payloads (uploads only)

- `GET /api/payloads` → `{payloads:[{name, path, size, mtime}]}` from
  `/data/PS5_LINUX_MANAGER/PAYLOADS/` only.
- `POST /api/payloads/upload?name=x.elf` with a raw
  `application/octet-stream` body: the body is **streamed** to a tmp
  file (cap 64 MiB; 413 above), ELF magic is verified, then the file is
  atomically rename(2)d into `PAYLOADS/`.
- `DELETE /api/payloads?name=x.elf`.

### Launching (serve-to-9021)

`serve_elf_9021(path)` opens a TCP connection to the console's ELF
loader at `127.0.0.1:9021`, sends the whole ELF file, and closes the
connection — the loader then executes it.

- `POST /api/launch {"path":"/data/PS5_LINUX_MANAGER/PAYLOADS/x.elf"}`
  → `{ok, message:"Payload served on port 9021"}`.
- `POST /api/boot/linux` picks an uploaded `ps5-linux-loader*.elf`
  first, else any `*.elf` at the selected device root; 404 when none.
- `POST /api/boot/orbis` exits the manager back to Orbis OS.

### Filesystem API

- `GET /api/fs/list?path=/...` → `{path, entries:[{name, path, type,
  size, mtime}]}` (dotfiles included, UTF-8 tolerant).
- `GET /api/fs/stat?path=/...` → `{path, type, size, mtime}`.
- `GET /api/fs/read?path=/...` → `{path, content, truncated}` (text,
  capped at 256 KiB).
- `POST /api/fs/write {"path","content"}` (atomic),
  `POST /api/fs/mkdir {"path"}`,
  `POST /api/fs/delete {"path"}` (recursive),
  `POST /api/fs/rename {"from","to"}`,
  `POST /api/fs/copy {"from","to"}` (recursive).

All paths are normalized lexically (`.`/`..` resolved), so traversal
tricks collapse to a canonical absolute path.

### FTP server

- `GET /api/ftp` → `{enabled, port, user, passSet}` (never the
  password).  `POST /api/ftp {enabled, port, user, pass}` persists the
  settings and **restarts** the daemon; omit `pass` to keep the old
  one.
- Default port **2121**, disabled by default.  Empty `user` = anonymous
  login; otherwise USER/PASS auth.
- Commands: `USER PASS QUIT NOOP SYST TYPE PWD XPWD CWD CDUP SIZE MLSD
  MLST LIST NLST PASV EPSV RETR STOR DELE RMD MKD RNFR RNTO`, passive
  data connections only (no TLS), full filesystem access.
- Example: `ftp 127.0.0.1 2121` (or any client in passive mode).

## Home-screen bubble (Media tab)

On first run the manager registers a **"PS5 Linux Manager"** bubble on
the PS5 home screen (following Elf Arsenal's approach): rows are
inserted into `/system_data/priv/mms/app.db`
(`tbl_contentinfo`, `tbl_conceptmetadata`, `tbl_iconinfo_*`) with

- `titleId = "PSLM00001"`
- `contentId = "IV9999-PSLM00001_00-PS5LINUXMANAGER0"`
- `deeplinkUri = "http://127.0.0.1:8090/"`

**Media tab placement:** the `category`/`viewCategory` values are
probed at install time from an already-installed media app (YouTube,
Netflix, Prime, Media Player) so the bubble lands exactly where those
live; the documented fallback is `"gda"` (media application category,
vs `"gde"` for games).  Column inserts are introspected per table, so
schema drift between firmwares is tolerated.

The ps5-payload-sdk ships **no sqlite3**, so this tree vendors the
official sqlite3 amalgamation 3.46.1 (`src/third_party/`), built with
`-DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION`.  If
`PS5LM_HAVE_SQLITE3` is ever undefined, a documented fallback writes
`app_install.sql` into the data dir for manual application instead.
The install is guarded by the `app.installed` first-run marker, sets
`appInstalled` in `/api/status` (marker **or** db row), and never
crashes on a missing/locked/corrupt db — it just logs and continues.

> **A console reboot is required** for the new bubble to appear in the
> Media tab (the home screen reads app.db at boot).

## Startup notifications

Once the HTTP server is up, exactly three notifications are sent via
`sceKernelSendNotificationRequest` (honoring `notifications` in
Settings):

1. `Welcome To PS5 Linux Manager`
2. `credits: InsideMatrix`
3. `Using port :8090`

## Linked system modules

`libkernel.sprx`, `libkernel_sys.sprx` (hardware model name),
`libSceRegMgr.sprx` (registry fallback), `libSceLibcInternal.sprx`,
`libSceNet.sprx` — all standard console modules, stubbed by the SDK.

## Verification

- `make check` — host syntax check of every source file (incl. the
  vendored sqlite3, warnings suppressed there).
- `make smoke` — builds the **entire payload for the host** (with
  `HOST_TEST` shims and a stubbed ELF-loader port) and runs
  `tests/smoke.c`: ~128 assertions over raw TCP covering every
  endpoint, device switching, root scanning, upload/list/delete,
  byte-identical 9021 serves for launch and boot/linux, fs round-trips
  with nested recursive copy/move, a full FTP protocol session
  (anonymous + USER/PASS), status fields, the 3 ordered startup
  notifications, app.db install against a real sqlite db plus a
  garbage-db degradation case, and the v1.2 opt-in switches (default:
  no USB-root writes from bios/grub POSTs; enabling the flags restores
  them and round-trips through config.json).

---

*PS5 Linux Manager by InsideMatrix — v1.1.0.*
