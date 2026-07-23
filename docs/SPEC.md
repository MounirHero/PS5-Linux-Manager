# SPEC — ps5-linux-manager.elf (C payload host)

Target: jailbroken PS5, built with **ps5-payload-sdk** (clang, freestanding + sceKernel).
Style: like pldmgr — ELF embeds the web UI and serves it over HTTP (default port **8090**).

## Files to produce under `/mnt/agents/output/ps5-linux-manager/payload/`
```
payload/
├── Makefile                 # ps5-payload-sdk style (PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk)
├── README.md                # build, install, USB layout, dual-boot/GRUB guide
├── src/
│   ├── main.c               # entry: init, mkdirs, load config, start httpd, notify
│   ├── httpd.c / httpd.h    # small poll()-based HTTP/1.1 server (no external deps):
│   │                        #   keep-alive optional, GET/POST, JSON bodies up to 1 MiB,
│   │                        #   static route GET / → embedded webui (gzip), SPA fallback
│   ├── api.c / api.h        # route handlers — MUST match api-contract.md exactly
│   ├── fsops.c / fsops.h    # scan /mnt/usb0..3/PS5/Linux, read/write cmdline.txt,
│   │                        #   vram.txt, path-override.txt; stat bzImage/initrd.img;
│   │                        #   atomic writes (tmp+rename); persistence in
│   │                        #   /data/ps5-linux-manager/{config.json,bios.json,entries.json,grub.cfg}
│   ├── grub.c / grub.h      # boot-entry model ↔ entries.json; generate grub.cfg text
│   │                        #   (menuentry blocks, set default=, set timeout=, PS5 Linux
│   │                        #   entry with linux/initrd lines, Orbis OS entry that exits)
│   ├── launch.c / launch.h  # launch ELF payloads: fork+exec via ps5-payload-sdk spawn
│   │                        #   (or libhijacker when available); used by /api/launch and
│   │                        #   /api/boot/linux (launches configured ps5-linux-loader.elf)
│   ├── json.c / json.h      # minimal JSON writer + tolerant parser (no deps)
│   ├── webui_embed.c        # generated: const arrays for index.html.gz (+assets)
│   └── util.c / util.h      # logging, sceKernelSendNotificationRequest wrapper,
│                            #   get ip, kit type (sceKernelIsDevKit/TestKit), uptime
└── templates/
    ├── grub.cfg             # annotated sample dual-boot grub.cfg
    ├── cmdline.txt          # "rw rootwait mitigations=off"
    └── vram.txt             # "2"
```

## API contract
`/mnt/agents/output/ps5-linux-manager/api-contract.md` is sacred — endpoints, field
names, and JSON shapes must match it exactly (the React UI is built against it).

## Key behaviors
- On boot: read `/data/ps5-linux-manager/config.json`; if `autoBoot=linux` and grub
  timeout expires (or /api/boot/linux invoked), launch loader ELF found via
  `path-override.txt` or the USB scan (ps5-linux-loader.elf in PS5/Linux dir or
  /data/ps5-linux-manager/payloads/).
- POST /api/bios syncs: vramGb → vram.txt; kernelParams+bootMode+rootDevice → cmdline.txt.
- POST /api/boot/grub regenerates grub.cfg from entries (also accepts raw grubCfg edits).
- Notifications via sceKernelSendNotificationRequest on start / boot events.
- All file writes atomic; never crash on missing USB — return `present:false`.
- C99/C11, no external libs beyond ps5-payload-sdk (libc, libkernel). Comment thoroughly.
