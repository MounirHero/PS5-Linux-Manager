# PS5 Linux Manager

**by InsideMatrix**

A Payload Manager–style control center for PS5 Linux on jailbroken consoles.
An ELF payload that serves a dark, firmware-grade web UI (`http://<ps5-ip>:8090`)
to configure everything about booting Linux on your PS5 — and to boot it.

![webui](https://img.shields.io/badge/webui-React%2019%20%2B%20Tailwind-0072CE)
![payload](https://img.shields.io/badge/payload-ps5--payload--sdk%20C11-FFA028)
![android](https://img.shields.io/badge/companion-Android%20APK-34D399)
![license](https://img.shields.io/badge/license-GPL--3.0-blue)

---

## Features

| Area | What you get |
|---|---|
| **Dashboard** | Live console status (real console name + CFI model code), boot-readiness checklist, one-click **Boot Linux** |
| **Boot Config** | USB device selector (usb0–usb3) + management of the device **root**: `bzImage`, `initrd.img`, `cmdline.txt` (chip editor + presets), `vram.txt` (1–16 GB slider), pre-flight validation |
| **BIOS Settings** | Console-BIOS-utility aesthetic: display, boot (root device, boot mode, kernel params), hardware (VRAM, CPU governor), services (SSH) — CMOS-style save bar |
| **Dual Boot / GRUB** | Drag-to-reorder boot entries (PS5 Linux / Orbis OS / custom payloads), default entry, timeout, auto-boot, live GRUB menu preview with ticking countdown, generated `grub.cfg` |
| **Payloads** | Upload `.elf` files (stored in `/data/PS5_LINUX_MANAGER/PAYLOADS/`) and **serve them to the console's ELF loader on port 9021** |
| **Files** | Real console file manager: browse the whole filesystem, edit text configs, copy/cut/paste, rename, delete, new folder — files **and** folders |
| **FTP Server** | Embedded FTP daemon (default port 2121, optional auth) for PC clients |
| **Console** | Live session log, boot-sequence replay, API request log |
| **Settings** | Port, autostart, notifications, FTP, USB write-safety switches, backup/restore |

### Platform integration

- **Boot execution = port 9021.** Booting Linux or launching a payload *serves the
  ELF to the console's own ELF loader at `127.0.0.1:9021`* — nothing is exec'd
  from memory.
- **Home-screen bubble.** On first run a "PS5 Linux Manager" app is installed into
  the **Media tab** (deeplink to the web UI; reboot required).
- **Startup notifications.** `Welcome To PS5 Linux Manager` ·
  `credits: InsideMatrix` · `Using port :8090`.
- **USB write safety (v1.2+).** `grub.cfg` mirroring and BIOS `cmdline.txt`/`vram.txt`
  syncing to the USB are **opt-in and off by default**, so the manager can never
  silently alter files your distro's bootloader reads.

## Repository layout

```
ps5-linux-manager/
├── payload/            # ps5-linux-manager.elf — C11, ps5-payload-sdk
│   ├── src/            #   httpd, api, fsapi, fsops, ftpsrv, appdb, grub, launch, json, util
│   ├── templates/      #   grub.cfg / cmdline.txt / vram.txt samples
│   └── README.md       #   full build & install guide
├── webui/              # React 19 + Vite + Tailwind SPA (embedded into the payload)
├── android/            # Android companion app (WebView + payload sender)
├── scripts/            # inline-dist.py (single-file webui build for embedding)
└── docs/               # API contract, engineering spec, build plan
```

## Quick start

```bash
# 1. Web UI
cd webui && npm install && npm run build
cd .. && python3 scripts/inline-dist.py        # single-file dist for embedding

# 2. Payload (needs ps5-payload-sdk + clang-18 — see payload/README.md)
cd payload && make embed && make               # → ps5-linux-manager.elf

# 3. Send to the console (ELF loader on port 9021) and open the UI
nc -q0 <PS5_IP> 9021 < ps5-linux-manager.elf
#    http://<PS5_IP>:8090  — from the PS5 browser or any LAN device
```

## Android companion app

`android/` builds `PS5LinuxManager.apk` — a dark-themed companion with:

- **Systems** — saved connections (name + IP + port); preset for PS5 Linux
  Manager `:8090`, plus custom entries for other webUIs (Payload Manager, etaHEN…)
- **WebView console** — opens the selected system full-screen
- **Payload Sender** — pick an `.elf` on your phone, enter IP + port (default
  9021), stream it to the console with live progress

See `android/README.md` for build instructions (Gradle, no Android Studio needed).

## Safety & recovery

The manager **never** touches your distro (`bzImage`, `initrd`, rootfs, `/boot`,
EFI). The only files it can write to your USB stick are `cmdline.txt`, `vram.txt`
and `grub.cfg` — and since v1.2 **only when you explicitly opt in**. If your
distro ever fails to boot, delete those three files from the USB root to restore.

## Credits

- Inspired by **Payload Manager (pldmgr)**, **Elf Arsenal** (Sonic ISO), and
  John Törnblom's **ps5-payload** ecosystem (incl. ps5-linux-loader).
- Author: **InsideMatrix**
- For educational/homebrew use on consoles you own. Use at your own risk.

## License

[GPL-3.0](LICENSE) (ps5-payload-sdk is GPL-3.0; vendored sqlite3 is public domain).
