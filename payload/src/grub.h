/*
 * grub.h — dual-boot boot-entry model and grub.cfg generation.
 *
 * The manager keeps a small boot-entry model (persisted as entries.json
 * in /data/PS5_LINUX_MANAGER) and can regenerate a readable grub.cfg from
 * it.  A generated config always contains:
 *   - "set default=..." and "set timeout=..." header lines,
 *   - one menuentry per enabled Linux entry with linux/initrd lines,
 *   - an "Orbis OS (PS5 System)" entry that simply exits GRUB back to the
 *     Orbis boot chain,
 *   - one menuentry per enabled "custom" entry: a payload/chainloader
 *     handoff stub (never silently dropped).
 *
 * The web UI may also post a raw grubCfg string; in that case the raw text
 * is stored verbatim and used instead of the generated one (the model is
 * still persisted so the UI can keep rendering the entries).
 */
#ifndef PS5LM_GRUB_H
#define PS5LM_GRUB_H

#include <stddef.h>

#include "json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRUB_MAX_ENTRIES 16

/* entry type strings used in entries.json and the API contract */
#define GRUB_TYPE_LINUX  "linux"
#define GRUB_TYPE_ORBIS  "orbis"
#define GRUB_TYPE_CUSTOM "custom"

/* autoBoot values from the API contract */
#define GRUB_AUTO_MANAGER "manager"
#define GRUB_AUTO_LINUX   "linux"
#define GRUB_AUTO_ORBIS   "orbis"

typedef struct {
    char id[64];
    char title[128];
    char type[16];        /* "linux" | "orbis" | "custom"           */
    char kernel[128];     /* e.g. "bzImage" (linux entries only)      */
    char initrd[128];     /* e.g. "initrd.img"                        */
    char cmdline[512];    /* kernel cmdline for this entry            */
    int  enabled;
} boot_entry_t;

typedef struct {
    int  enabled;                  /* dual-boot menu on/off            */
    int  timeout_sec;              /* countdown before default boots   */
    char default_entry[64];        /* id of the default entry          */
    char auto_boot[16];            /* "manager" | "linux" | "orbis"    */
    boot_entry_t entries[GRUB_MAX_ENTRIES];
    int  n_entries;
    char *grub_cfg_raw;            /* optional raw override (owned)    */
} boot_config_t;

/* Fill `cfg` with the factory default: one PS5 Linux entry + Orbis OS. */
void grub_defaults(boot_config_t *cfg);

/* Release owned memory (grub_cfg_raw). */
void grub_free(boot_config_t *cfg);

/*
 * Load entries.json from the data dir into `cfg`.  Missing/invalid file
 * falls back to grub_defaults().  Always leaves `cfg` usable.
 */
void grub_load(boot_config_t *cfg);

/* Persist `cfg` to entries.json (atomic).  0 ok / -1 error. */
int grub_save(const boot_config_t *cfg, char *err, size_t errlen);

/*
 * Render the effective grub.cfg text: the raw override when one is set,
 * otherwise a freshly generated config.  Returns a malloc'd string.
 */
char *grub_render_cfg(const boot_config_t *cfg);

/* Generate grub.cfg text purely from the entry model (malloc'd). */
char *grub_generate_cfg(const boot_config_t *cfg);

/*
 * Regenerate (or copy raw) grub.cfg into the data dir (atomic).  When
 * `mirror_usb` is non-zero the same text is additionally mirrored to the
 * selected Linux device root (/mnt/<linuxDevice>/grub.cfg) if that device
 * is present; a missing device is tolerated and never an error.  With
 * `mirror_usb` == 0 (the safe default, settings key grubMirrorUsb) only
 * the data-dir copy is written — the USB stick is never touched.
 */
int grub_write_cfg(const boot_config_t *cfg, int mirror_usb,
                   char *err, size_t errlen);

/* Serialize `cfg` into the exact JSON shape of GET /api/boot/grub. */
void grub_to_json(const boot_config_t *cfg, jbuf_t *b);

/*
 * Merge a posted /api/boot/grub object into `cfg` (fields absent from the
 * JSON keep their current values).  Returns 0 on success, -1 if `root`
 * is not an object.
 */
int grub_from_json(boot_config_t *cfg, const json_value_t *root);

#ifdef __cplusplus
}
#endif

#endif /* PS5LM_GRUB_H */
