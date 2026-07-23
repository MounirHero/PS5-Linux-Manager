# PS5 Linux Manager v1.2.0 — by InsideMatrix

First public release.

## Highlights

- **Web control panel** served by the payload at `http://<ps5-ip>:8090`:
  Dashboard, Boot Config (USB device root), BIOS, Dual Boot/GRUB, Payloads,
  real-FS Files, Console, Settings.
- **Boot & payload execution via port 9021** — ELFs are served to the console's
  own ELF loader; nothing is executed from memory.
- **Real file management** — full filesystem browser (copy/cut/paste, rename,
  delete, mkdir, text editor) plus an embedded **FTP server** (port 2121).
- **Media-tab home-screen bubble** installed on first run + startup notifications.
- **USB write safety** — `grub.cfg` mirroring and BIOS `cmdline.txt`/`vram.txt`
  sync are opt-in (off by default); BIOS shows the exact cmdline preview.
- **Android companion APK** — WebView into the manager (and other webUIs) +
  payload sender (IP/port, default 9021).

## Assets

| Asset | Description |
|---|---|
| `ps5-linux-manager.elf` | The payload — send to your PS5's ELF loader (port 9021) |
| `PS5LinuxManager.apk` | Android companion app (debug-signed; sideload) |
| Source code (zip/tar.gz) | Full project: payload, webui, android, docs |

## Notes

- Linux files are read from the **root of the selected USB device**
  (`/mnt/usbX/bzImage`, `initrd.img`, `cmdline.txt`, `vram.txt`).
- Home-screen bubble appears after a **reboot**.
- Report issues with your firmware version, distro, and USB layout.
