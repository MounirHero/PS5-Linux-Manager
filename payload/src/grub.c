/*
 * grub.c — implementation of the boot-entry model and grub.cfg generation.
 */
#if defined(HOST_TEST) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include "grub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsops.h"
#include "util.h"

/* ------------------------------------------------------------------ */
/* Defaults                                                            */
/* ------------------------------------------------------------------ */

void grub_defaults(boot_config_t *cfg) {
    boot_entry_t *e;

    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 1;
    cfg->timeout_sec = 5;
    util_copy(cfg->default_entry, sizeof(cfg->default_entry), "linux-ps5");
    util_copy(cfg->auto_boot, sizeof(cfg->auto_boot), GRUB_AUTO_MANAGER);

    /* Entry 1: boot the PS5 Linux kernel found at the device root. */
    e = &cfg->entries[cfg->n_entries++];
    memset(e, 0, sizeof(*e));
    util_copy(e->id, sizeof(e->id), "linux-ps5");
    util_copy(e->title, sizeof(e->title), "PS5 Linux (USB)");
    util_copy(e->type, sizeof(e->type), GRUB_TYPE_LINUX);
    util_copy(e->kernel, sizeof(e->kernel),
              "/mnt/usb0/" FSOPS_NAME_KERNEL);
    util_copy(e->initrd, sizeof(e->initrd),
              "/mnt/usb0/" FSOPS_NAME_INITRD);
    util_copy(e->cmdline, sizeof(e->cmdline),
              "rw rootwait mitigations=off");
    e->enabled = 1;

    /* Entry 2: exit the menu and return to the Orbis OS boot chain. */
    e = &cfg->entries[cfg->n_entries++];
    memset(e, 0, sizeof(*e));
    util_copy(e->id, sizeof(e->id), "orbis");
    util_copy(e->title, sizeof(e->title), "Orbis OS (PS5 System)");
    util_copy(e->type, sizeof(e->type), GRUB_TYPE_ORBIS);
    e->enabled = 1;
}

void grub_free(boot_config_t *cfg) {
    if (!cfg)
        return;
    free(cfg->grub_cfg_raw);
    cfg->grub_cfg_raw = NULL;
}

/* ------------------------------------------------------------------ */
/* JSON <-> model                                                      */
/* ------------------------------------------------------------------ */

void grub_to_json(const boot_config_t *cfg, jbuf_t *b) {
    int i;
    char *rendered;

    jb_begin_obj(b);
    jb_key(b, "enabled");      jb_bool(b, cfg->enabled);
    jb_key(b, "timeoutSec");   jb_int(b, cfg->timeout_sec);
    jb_key(b, "defaultEntry"); jb_str(b, cfg->default_entry);
    jb_key(b, "autoBoot");     jb_str(b, cfg->auto_boot);
    jb_key(b, "entries");
    jb_begin_arr(b);
    for (i = 0; i < cfg->n_entries; i++) {
        const boot_entry_t *e = &cfg->entries[i];
        jb_begin_obj(b);
        jb_key(b, "id");      jb_str(b, e->id);
        jb_key(b, "title");   jb_str(b, e->title);
        jb_key(b, "type");    jb_str(b, e->type);
        if (strcmp(e->type, GRUB_TYPE_LINUX) == 0 ||
            strcmp(e->type, GRUB_TYPE_CUSTOM) == 0) {
            jb_key(b, "kernel");  jb_str(b, e->kernel);
            jb_key(b, "initrd");  jb_str(b, e->initrd);
            jb_key(b, "cmdline"); jb_str(b, e->cmdline);
        }
        jb_key(b, "enabled"); jb_bool(b, e->enabled);
        jb_end_obj(b);
    }
    jb_end_arr(b);
    jb_key(b, "grubCfg");
    rendered = grub_render_cfg(cfg);
    jb_str(b, rendered ? rendered : "");
    free(rendered);
    jb_end_obj(b);
}

int grub_from_json(boot_config_t *cfg, const json_value_t *root) {
    const json_value_t *v, *arr;
    size_t i;

    if (!root || root->type != JSON_OBJECT)
        return -1;

    cfg->enabled = json_get_bool(root, "enabled", cfg->enabled);
    cfg->timeout_sec = json_get_int(root, "timeoutSec", cfg->timeout_sec);
    if (cfg->timeout_sec < 0)
        cfg->timeout_sec = 0;

    v = json_obj_get(root, "defaultEntry");
    if (v && v->type == JSON_STRING)
        util_copy(cfg->default_entry, sizeof(cfg->default_entry),
                  v->string);
    v = json_obj_get(root, "autoBoot");
    if (v && v->type == JSON_STRING &&
        (strcmp(v->string, GRUB_AUTO_MANAGER) == 0 ||
         strcmp(v->string, GRUB_AUTO_LINUX) == 0 ||
         strcmp(v->string, GRUB_AUTO_ORBIS) == 0))
        util_copy(cfg->auto_boot, sizeof(cfg->auto_boot), v->string);

    arr = json_obj_get(root, "entries");
    if (arr && arr->type == JSON_ARRAY) {
        cfg->n_entries = 0;
        for (i = 0; i < arr->n_items && cfg->n_entries < GRUB_MAX_ENTRIES;
             i++) {
            const json_value_t *je = arr->items[i];
            boot_entry_t *e;
            if (!je || je->type != JSON_OBJECT)
                continue;
            e = &cfg->entries[cfg->n_entries];
            memset(e, 0, sizeof(*e));
            util_copy(e->id, sizeof(e->id),
                      json_get_string(je, "id", ""));
            util_copy(e->title, sizeof(e->title),
                      json_get_string(je, "title", e->id));
            util_copy(e->type, sizeof(e->type),
                      json_get_string(je, "type", GRUB_TYPE_LINUX));
            util_copy(e->kernel, sizeof(e->kernel),
                      json_get_string(je, "kernel", ""));
            util_copy(e->initrd, sizeof(e->initrd),
                      json_get_string(je, "initrd", ""));
            util_copy(e->cmdline, sizeof(e->cmdline),
                      json_get_string(je, "cmdline", ""));
            e->enabled = json_get_bool(je, "enabled", 1);
            if (!*e->id)
                continue;                  /* entries need a stable id */
            cfg->n_entries++;
        }
    }

    /*
     * A posted "grubCfg" string is a raw override: stored verbatim and
     * rendered instead of the generated config from then on.
     */
    v = json_obj_get(root, "grubCfg");
    if (v && v->type == JSON_STRING && *v->string) {
        free(cfg->grub_cfg_raw);
        cfg->grub_cfg_raw = strdup(v->string);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* grub.cfg generation                                                 */
/* ------------------------------------------------------------------ */

/*
 * Escape a double-quoted GRUB string (backslash and quote) into `out`.
 * GRUB's parser treats backslash specially inside double quotes.
 */
static void grub_escape(const char *in, char *out, size_t len) {
    size_t o = 0;
    if (!out || len == 0)
        return;
    while (in && *in && o + 3 < len) {
        if (*in == '"' || *in == '\\')
            out[o++] = '\\';
        out[o++] = *in++;
    }
    out[o] = '\0';
}

char *grub_generate_cfg(const boot_config_t *cfg) {
    jbuf_t b;
    int idx;

    jb_init(&b);
    jb_raw(&b, "# grub.cfg — generated by ps5-linux-manager\n");
    jb_raw(&b, "# Edit via the web UI (Dual Boot page); manual edits are\n");
    jb_raw(&b, "# kept when posted as a raw grubCfg override.\n\n");

    {
        char line[256];
        snprintf(line, sizeof(line), "set default=\"%s\"\n",
                 cfg->default_entry);
        jb_raw(&b, line);
        snprintf(line, sizeof(line), "set timeout=%d\n\n",
                 cfg->timeout_sec);
        jb_raw(&b, line);
    }

    for (idx = 0; idx < cfg->n_entries; idx++) {
        const boot_entry_t *e = &cfg->entries[idx];
        char esc[1024];
        char line[1400];

        if (!e->enabled)
            continue;

        grub_escape(e->title, esc, sizeof(esc));
        if (strcmp(e->type, GRUB_TYPE_LINUX) == 0) {
            snprintf(line, sizeof(line),
                     "menuentry \"%s\" --id %s {\n", esc, e->id);
            jb_raw(&b, line);
            /* linux/initrd reference the USB Linux dir contents. */
            grub_escape(e->kernel, esc, sizeof(esc));
            snprintf(line, sizeof(line), "    linux /%s", esc);
            jb_raw(&b, line);
            if (*e->cmdline) {
                grub_escape(e->cmdline, esc, sizeof(esc));
                snprintf(line, sizeof(line), " %s\n", esc);
            } else {
                snprintf(line, sizeof(line), "\n");
            }
            jb_raw(&b, line);
            if (*e->initrd) {
                grub_escape(e->initrd, esc, sizeof(esc));
                snprintf(line, sizeof(line), "    initrd /%s\n", esc);
                jb_raw(&b, line);
            }
            jb_raw(&b, "}\n\n");
        } else if (strcmp(e->type, GRUB_TYPE_ORBIS) == 0) {
            /* Orbis OS: leave GRUB, resume the stock boot chain. */
            snprintf(line, sizeof(line),
                     "menuentry \"%s\" --id %s {\n"
                     "    exit\n"
                     "}\n\n",
                     esc, e->id);
            jb_raw(&b, line);
        } else if (strcmp(e->type, GRUB_TYPE_CUSTOM) == 0) {
            /*
             * Custom payload entry: chainloader-style handoff to an
             * arbitrary .elf / payload path.  The payload path is carried
             * in the entry's cmdline field and kept as a comment; when a
             * kernel (and optionally an initrd) is set we emit real
             * linux/initrd lines, otherwise a clearly-commented stub.
             * An enabled custom entry is never silently dropped.
             */
            snprintf(line, sizeof(line),
                     "menuentry \"%s\" --id %s {\n", esc, e->id);
            jb_raw(&b, line);
            snprintf(line, sizeof(line), "    # custom payload: %s\n",
                     *e->cmdline ? e->cmdline : "(no path set)");
            jb_raw(&b, line);
            if (*e->kernel) {
                grub_escape(e->kernel, esc, sizeof(esc));
                snprintf(line, sizeof(line), "    linux /%s", esc);
                jb_raw(&b, line);
                if (*e->cmdline) {
                    grub_escape(e->cmdline, esc, sizeof(esc));
                    snprintf(line, sizeof(line), " %s\n", esc);
                } else {
                    snprintf(line, sizeof(line), "\n");
                }
                jb_raw(&b, line);
                if (*e->initrd) {
                    grub_escape(e->initrd, esc, sizeof(esc));
                    snprintf(line, sizeof(line), "    initrd /%s\n", esc);
                    jb_raw(&b, line);
                }
            } else {
                jb_raw(&b, "    # chainloader-style handoff — handled by "
                           "ps5-linux-manager\n");
            }
            jb_raw(&b, "}\n\n");
        }
        /* Truly unknown types are skipped (enabled custom entries are not). */
    }

    return jb_steal(&b);
}

char *grub_render_cfg(const boot_config_t *cfg) {
    if (cfg->grub_cfg_raw && *cfg->grub_cfg_raw)
        return strdup(cfg->grub_cfg_raw);
    return grub_generate_cfg(cfg);
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

void grub_load(boot_config_t *cfg) {
    char *raw;
    json_value_t *root;

    grub_defaults(cfg);
    raw = fsops_read_data_file("entries.json");
    if (!raw)
        return;                            /* first boot: defaults */
    root = json_parse(raw);
    free(raw);
    if (!root)
        return;
    grub_from_json(cfg, root);             /* merge over defaults */
    json_free(root);
}

int grub_save(const boot_config_t *cfg, char *err, size_t errlen) {
    jbuf_t b;
    char *text;
    int rc;

    jb_init(&b);
    jb_begin_obj(&b);
    jb_key(&b, "enabled");      jb_bool(&b, cfg->enabled);
    jb_key(&b, "timeoutSec");   jb_int(&b, cfg->timeout_sec);
    jb_key(&b, "defaultEntry"); jb_str(&b, cfg->default_entry);
    jb_key(&b, "autoBoot");     jb_str(&b, cfg->auto_boot);
    jb_key(&b, "entries");
    jb_begin_arr(&b);
    {
        int i;
        for (i = 0; i < cfg->n_entries; i++) {
            const boot_entry_t *e = &cfg->entries[i];
            jb_begin_obj(&b);
            jb_key(&b, "id");      jb_str(&b, e->id);
            jb_key(&b, "title");   jb_str(&b, e->title);
            jb_key(&b, "type");    jb_str(&b, e->type);
            jb_key(&b, "kernel");  jb_str(&b, e->kernel);
            jb_key(&b, "initrd");  jb_str(&b, e->initrd);
            jb_key(&b, "cmdline"); jb_str(&b, e->cmdline);
            jb_key(&b, "enabled"); jb_bool(&b, e->enabled);
            jb_end_obj(&b);
        }
    }
    jb_end_arr(&b);
    if (cfg->grub_cfg_raw) {
        jb_key(&b, "grubCfg");
        jb_str(&b, cfg->grub_cfg_raw);
    }
    jb_end_obj(&b);

    text = jb_steal(&b);
    rc = fsops_write_data_file("entries.json", text, strlen(text),
                               err, errlen);
    free(text);
    return rc;
}

int grub_write_cfg(const boot_config_t *cfg, int mirror_usb,
                   char *err, size_t errlen) {
    char *text = grub_render_cfg(cfg);
    int rc;
    if (!text) {
        if (err && errlen)
            snprintf(err, errlen, "out of memory");
        return -1;
    }
    rc = fsops_write_data_file("grub.cfg", text, strlen(text), err, errlen);
    if (rc == 0) {
        if (!mirror_usb) {
            /*
             * Safe default (contract v1.2): only the data-dir copy is
             * written — never place a grub.cfg where a distro's own
             * bootloader may pick it up.
             */
            util_log("usb mirror disabled (grubMirrorUsb=false); "
                     "grub.cfg kept in the data dir only");
        } else {
            /*
             * Best-effort mirror to the selected Linux device root so the
             * GRUB on the stick picks the config up directly.  A missing
             * device (or a failed mirror) is tolerated and never fails
             * the request — the canonical copy in the data dir was
             * already written.
             */
            char dir[FSOPS_PATH_MAX];
            if (fsops_device_present() &&
                fsops_device_root(dir, sizeof(dir)) == 0) {
                char path[FSOPS_PATH_MAX];
                char werr[256];
                werr[0] = '\0';
                snprintf(path, sizeof(path), "%s/grub.cfg", dir);
                if (util_atomic_write(path, text, strlen(text), werr,
                                      sizeof(werr)) != 0)
                    util_log("warning: cannot mirror grub.cfg to %s: %s",
                             path, werr);
                else
                    util_log("grub.cfg mirrored to %s", path);
            }
        }
    }
    free(text);
    return rc;
}
