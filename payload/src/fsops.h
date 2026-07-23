/*
 * fsops.h — filesystem operations against the ps5-linux-loader file
 * contract (v1.1) and the manager's own persistent state.
 *
 * v1.1 model: the user selects a Linux device (default "usb0"); every
 * ps5-linux-loader file lives at the ROOT of that device, i.e.
 * /mnt/<device>/bzImage, /mnt/<device>/initrd.img, /mnt/<device>/cmdline.txt,
 * /mnt/<device>/vram.txt and optional *.elf loaders.  There is no
 * PS5/Linux subdirectory and no path-override.txt anymore — the device
 * selector replaces both.
 *
 * This module centralizes every read/write of those files, always:
 *   - tolerating absent devices (callers get present:false, never a crash)
 *   - writing atomically (tmp file + rename, see util_atomic_write)
 *
 * Manager state lives in /data/PS5_LINUX_MANAGER/:
 *   config.json  settings (port, autostart, notifications, theme,
 *                linuxDevice, ftp)
 *   bios.json    BIOS-style settings edited via /api/bios
 *   entries.json GRUB boot-entry model (see grub.c)
 *   grub.cfg     generated GRUB configuration
 *   PAYLOADS/    manually uploaded .elf payloads (uploads only)
 */
#ifndef PS5LM_FSOPS_H
#define PS5LM_FSOPS_H

#include <stddef.h>

#include "util.h"   /* PS5LM_DATA_DIR */

#ifdef __cplusplus
extern "C" {
#endif

#define FSOPS_MAX_USB 4
#define FSOPS_PATH_MAX 768

/*
 * Mount root prefix: device "usb0" maps to FSOPS_MNT_PREFIX "/usb0"
 * ("/mnt/usb0" on the console).  Overridable at compile time for host
 * smoke tests (target builds always use /mnt).
 */
#ifndef FSOPS_MNT_PREFIX
#define FSOPS_MNT_PREFIX "/mnt"
#endif

/* Uploaded payload store (the ONLY payload source in v1.1). */
#define FSOPS_PAYLOADS_DIR PS5LM_DATA_DIR "/PAYLOADS"

/* Canonical names of the loader config files we manage. */
#define FSOPS_NAME_CMDLINE   "cmdline.txt"
#define FSOPS_NAME_VRAM      "vram.txt"
#define FSOPS_NAME_KERNEL    "bzImage"
#define FSOPS_NAME_INITRD    "initrd.img"
#define FSOPS_NAME_LOADER_PREFIX "ps5-linux-loader"

/* Per-USB status for GET /api/status. */
typedef struct {
    char mount[64];     /* "/mnt/usb0" .. "/mnt/usb3"                  */
    char name[32];      /* "usb0" .. "usb3"                            */
    int  present;       /* mountpoint exists and is a directory        */
    int  linux_files;   /* bzImage exists at the device root           */
} usb_status_t;

/* One entry for GET /api/linux/files. */
typedef struct {
    char      name[256];
    char      path[FSOPS_PATH_MAX];
    long long size;
    long      mtime;
    char      kind[12]; /* kernel|initrd|config|loader|other           */
    int       present;  /* always 1 for listed (existing) files        */
} linux_file_t;

/* One entry for GET /api/payloads (uploads only). */
typedef struct {
    char      name[256];
    char      path[FSOPS_PATH_MAX];
    long long size;
    long      mtime;
} payload_info_t;

/* ------------------------------------------------------------------ */
/* Linux device selection                                              */
/* ------------------------------------------------------------------ */

/* Non-zero when `dev` is a syntactically valid device name ("usb"+digits). */
int fsops_device_valid(const char *dev);

/* Select the active Linux device (e.g. "usb0").  Invalid names are
 * ignored.  Called by api.c when loading/persisting settings. */
void fsops_set_linux_device(const char *dev);

/* Currently selected Linux device name (never NULL; "usb0" default). */
const char *fsops_linux_device(void);

/* Root directory of the selected device, e.g. "/mnt/usb0". */
int fsops_device_root(char *out, size_t len);

/* Non-zero when the selected device root exists and is a directory. */
int fsops_device_present(void);

/* Fill `out` with the status of usb0..usb3.  Always returns FSOPS_MAX_USB. */
int fsops_scan_usbs(usb_status_t out[FSOPS_MAX_USB]);

/* ------------------------------------------------------------------ */
/* Loader config files at the device root (cmdline.txt / vram.txt)     */
/* ------------------------------------------------------------------ */

/*
 * Read cmdline.txt or vram.txt from the selected device root.
 * Returns 0 on success (content in `out`, *exists set), -1 on error.
 * Missing file or missing device is NOT an error: *exists is set to 0.
 */
int fsops_read_linux_config(const char *name, char *out, size_t len,
                            int *exists);

/*
 * Atomically write cmdline.txt or vram.txt at the selected device root.
 * Returns 0 on success, -1 with a message in `err` otherwise (including
 * the "device not present" case — callers surface it as a 4xx).
 */
int fsops_write_linux_config(const char *name, const char *content,
                             char *err, size_t errlen);

/* ------------------------------------------------------------------ */
/* Device root listing                                                 */
/* ------------------------------------------------------------------ */

/*
 * List files at the selected device root (or `dir` when given).
 * On success returns 0 and a malloc'd array in `out` (free with free());
 * *count receives the element count.  When the device is absent, returns
 * 0 with *out=NULL, *count=0 and *dir_used left empty.
 */
int fsops_list_linux_files(const char *dir, linux_file_t **out,
                           size_t *count, char *dir_used, size_t dir_len);

/* Presence of the files the loader needs, at the selected device root. */
void fsops_required_presence(int *bzimage, int *initrd, int *cmdline);

/*
 * Find the ELF used for "Boot Linux" (v1.1 order):
 *   1. an uploaded PAYLOADS/ps5-linux-loader*.elf (first match),
 *   2. any *.elf at the selected device root.
 * Returns 0 with the path in `out`, or -1 when nothing was found.
 */
int fsops_find_loader(char *out, size_t len);

/* ------------------------------------------------------------------ */
/* Uploaded payloads (PAYLOADS/)                                       */
/* ------------------------------------------------------------------ */

/* List uploaded payloads: *.elf files directly inside PAYLOADS/.
 * Returns 0 with a malloc'd array in `out` (free with free()). */
int fsops_list_payloads(payload_info_t **out, size_t *count);

/* Non-zero when `name` is a safe payload file name (no '/', ends .elf). */
int fsops_payload_name_ok(const char *name);

/*
 * Atomically install an uploaded payload: verifies the ELF magic of
 * `tmp_path` and rename(2)s it to PAYLOADS/`name`.  Returns 0 on
 * success, -1 with a message in `err` otherwise.
 */
int fsops_finalize_upload(const char *tmp_path, const char *name,
                          char *err, size_t errlen);

/* Delete PAYLOADS/`name`.  0 ok / -1 with message in `err`. */
int fsops_delete_payload(const char *name, char *err, size_t errlen);

/* ------------------------------------------------------------------ */
/* Manager persistence                                                 */
/* ------------------------------------------------------------------ */

/*
 * Create /data/PS5_LINUX_MANAGER and its PAYLOADS/ subdirectory.
 * Called once at startup from main.c.
 */
int fsops_ensure_data_dirs(void);

/*
 * Read a file from the manager's data dir.  Returns a malloc'd,
 * NUL-terminated buffer, or NULL when the file is missing/unreadable.
 */
char *fsops_read_data_file(const char *name);

/* Atomically write a file in the manager's data dir (0 ok / -1 error). */
int fsops_write_data_file(const char *name, const char *data, size_t len,
                          char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* PS5LM_FSOPS_H */
