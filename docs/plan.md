# PS5 Linux Manager — Build Plan

## Goal
A PS5 payload ("ps5-linux-manager.elf") in the style of Payload Manager (pldmgr):
an ELF that serves a React web UI (accessible from PS5 browser / LAN browser) to:
- Configure boot: edit `cmdline.txt`, `vram.txt`, `path-override.txt`, pick kernel/initrd
  in `/mnt/usb{0..3}/PS5/Linux/` (exact contract used by ps5-linux-loader 2.4)
- BIOS settings: VRAM size, display, CPU governor, SSH, boot mode (extends bios_configurator concept)
- Launch `ps5-linux-loader.elf` itself (and other payloads) from the UI
- Dual boot / GRUB: boot entries, default entry, timeout, auto-boot countdown, grub.cfg generator/editor

## Reverse-engineering findings (from uploaded ELFs)
- pldmgr.elf: React+Tailwind webapp embedded in ELF, HTTP server (libmicrohttpd),
  deeplink http://127.0.0.1:8084/, routes `/list_payloads`, `/loadpayload:<path>`,
  `/repository_payloads`, scans `/data/pldmgr/payloads` + USB payloads.
- ps5-linux-loader_2.4.elf: ps5-payload-sdk ELF, reads `bzImage`, `initrd.img`,
  `cmdline.txt`, `vram.txt`, `path-override.txt` from `/mnt/usb0..3/PS5/Linux/`;
  uses sceKernelSendNotificationRequest, enter_rest_mode; maps GPU dmem sized by vram.txt.
- bios_configurator.elf: raw asm stub writing BIOS-style HTML UI to /tmp/linux_bios.html
  (settings: resolution, refresh, root device, boot mode, kernel params, VRAM, governor, SSH).

## Architecture
```
ps5-linux-manager/
├── webui/        React + Vite + Tailwind + shadcn/ui app (Payload-Manager style, dark)
│                 API layer with 2 adapters: live (fetch to payload HTTP API) / mock (browser preview)
├── payload/      C source (ps5-payload-sdk): HTTP server + JSON API + embedded webui assets
│                 endpoints: /api/status, /api/linux/files, /api/linux/config (GET/POST),
│                 /api/bios (GET/POST), /api/boot/grub (GET/POST), /api/payloads,
│                 /api/launch, /api/reboot, static / (embedded gzipped webui)
│                 persistence: /data/ps5-linux-manager/{config.json, grub.cfg, entries}
├── dist/         built webui
└── docs/         build + install instructions, GRUB/dual-boot notes
```

## Stages
- Stage 1 (done): Recon uploaded ELFs → API/UI contract (above).
- Stage 2 — WebUI: load `vibecoding-webapp-swarm` + `webapp-building-swarm` (+`swarm-workspace`).
  Delegate React app build to subagent(s). Pages: Dashboard, Boot Loader, Linux Files,
  BIOS, Dual Boot/GRUB, Payloads, Settings. Mock adapter for preview. Validate: build passes.
- Stage 3 — Payload C host: load `vibecoding-general-swarm`. Delegate C source + Makefile +
  grub.cfg template + README. Must mirror the webui API contract exactly.
- Stage 4 — Integrate & validate: build webui, reviewer subagent cross-checks API contract
  webui↔C, fix issues.
- Stage 5 — Deliver: website_version_manager (preview), zip full project to /mnt/agents/output.

## Constraints
- No clang / no ps5-payload-sdk in sandbox → C payload delivered as build-ready source, not compiled ELF.
- UI style: dark, low-saturation, Payload-Manager-like (sidebar nav, cards), not Google-y.
