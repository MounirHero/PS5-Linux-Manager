/*
 * api.c — implementation of the HTTP API declared in api.h (contract v1.1).
 *
 * Field names and JSON shapes here are SACRED: they must match
 * api-contract.md exactly because the React web UI is built against them.
 */
#if defined(HOST_TEST) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "appdb.h"
#include "fsapi.h"
#include "fsops.h"
#include "ftpsrv.h"
#include "grub.h"
#include "json.h"
#include "launch.h"
#include "util.h"

/* ================================================================== */
/* Settings model (config.json):                                       */
/*   port/autostart/notifications/theme + linuxDevice + ftp            */
/* ================================================================== */

typedef struct {
    int  port;
    int  autostart;
    int  notifications;
    char theme[16];
    /* Safety switches (contract v1.2), both OPT-IN / default OFF:      */
    int  grub_mirror_usb;    /* mirror grub.cfg to the USB device root  */
    int  bios_sync_cmdline;  /* sync vram.txt/cmdline.txt to USB root   */
    char linux_device[32];
    ftp_config_t ftp;
} settings_t;

static settings_t g_settings;

static void settings_defaults(settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->port = 8090;
    s->autostart = 0;
    s->notifications = 1;
    util_copy(s->theme, sizeof(s->theme), "dark");
    /* grub_mirror_usb / bios_sync_cmdline stay 0 (memset): existing
     * installs without the keys get the safe opt-out default. */
    util_copy(s->linux_device, sizeof(s->linux_device), "usb0");
    s->ftp.enabled = 0;
    s->ftp.port = 2121;
    s->ftp.user[0] = '\0';
    s->ftp.pass[0] = '\0';
}

static void settings_apply(const settings_t *s) {
    util_set_notifications_enabled(s->notifications);
    fsops_set_linux_device(s->linux_device);
    ftp_configure(&s->ftp);
}

/* GET /api/settings shape (exactly these six keys, per contract v1.2). */
static void settings_to_json(const settings_t *s, jbuf_t *b) {
    jb_begin_obj(b);
    jb_key(b, "port");            jb_int(b, s->port);
    jb_key(b, "autostart");       jb_bool(b, s->autostart);
    jb_key(b, "notifications");   jb_bool(b, s->notifications);
    jb_key(b, "theme");           jb_str(b, s->theme);
    jb_key(b, "grubMirrorUsb");   jb_bool(b, s->grub_mirror_usb);
    jb_key(b, "biosSyncCmdline"); jb_bool(b, s->bios_sync_cmdline);
    jb_end_obj(b);
}

/* FTP object shape shared by /api/status and /api/ftp (passSet, never
 * the password itself). */
static void ftp_to_json(const ftp_config_t *f, jbuf_t *b) {
    jb_begin_obj(b);
    jb_key(b, "enabled"); jb_bool(b, f->enabled);
    jb_key(b, "port");    jb_int(b, f->port);
    jb_key(b, "user");    jb_str(b, f->user);
    jb_key(b, "passSet"); jb_bool(b, f->pass[0] != '\0');
    jb_end_obj(b);
}

static void settings_load(settings_t *s) {
    char *raw;
    json_value_t *root;

    settings_defaults(s);
    raw = fsops_read_data_file("config.json");
    if (!raw)
        return;
    root = json_parse(raw);
    free(raw);
    if (!root)
        return;
    s->port = json_get_int(root, "port", s->port);
    if (s->port <= 0 || s->port > 65535)
        s->port = 8090;
    s->autostart = json_get_bool(root, "autostart", s->autostart);
    s->notifications = json_get_bool(root, "notifications",
                                     s->notifications);
    util_copy(s->theme, sizeof(s->theme),
              json_get_string(root, "theme", s->theme));
    s->grub_mirror_usb = json_get_bool(root, "grubMirrorUsb",
                                       s->grub_mirror_usb);
    s->bios_sync_cmdline = json_get_bool(root, "biosSyncCmdline",
                                         s->bios_sync_cmdline);
    util_copy(s->linux_device, sizeof(s->linux_device),
              json_get_string(root, "linuxDevice", s->linux_device));
    if (!fsops_device_valid(s->linux_device))
        util_copy(s->linux_device, sizeof(s->linux_device), "usb0");
    {
        const json_value_t *f = json_obj_get(root, "ftp");
        if (f && f->type == JSON_OBJECT) {
            s->ftp.enabled = json_get_bool(f, "enabled", s->ftp.enabled);
            s->ftp.port = json_get_int(f, "port", s->ftp.port);
            if (s->ftp.port <= 0 || s->ftp.port > 65535)
                s->ftp.port = 2121;
            util_copy(s->ftp.user, sizeof(s->ftp.user),
                      json_get_string(f, "user", s->ftp.user));
            util_copy(s->ftp.pass, sizeof(s->ftp.pass),
                      json_get_string(f, "pass", s->ftp.pass));
        }
    }
    json_free(root);
}

static int settings_save(const settings_t *s, char *err, size_t errlen) {
    jbuf_t b;
    char *text;
    int rc;

    jb_init(&b);
    jb_begin_obj(&b);
    jb_key(&b, "port");          jb_int(&b, s->port);
    jb_key(&b, "autostart");     jb_bool(&b, s->autostart);
    jb_key(&b, "notifications"); jb_bool(&b, s->notifications);
    jb_key(&b, "theme");         jb_str(&b, s->theme);
    jb_key(&b, "grubMirrorUsb");   jb_bool(&b, s->grub_mirror_usb);
    jb_key(&b, "biosSyncCmdline"); jb_bool(&b, s->bios_sync_cmdline);
    jb_key(&b, "linuxDevice");   jb_str(&b, s->linux_device);
    jb_key(&b, "ftp");
    jb_begin_obj(&b);
    jb_key(&b, "enabled"); jb_bool(&b, s->ftp.enabled);
    jb_key(&b, "port");    jb_int(&b, s->ftp.port);
    jb_key(&b, "user");    jb_str(&b, s->ftp.user);
    jb_key(&b, "pass");    jb_str(&b, s->ftp.pass);
    jb_end_obj(&b);
    jb_end_obj(&b);

    text = jb_steal(&b);
    rc = fsops_write_data_file("config.json", text, strlen(text),
                               err, errlen);
    free(text);
    return rc;
}

void api_init(void) {
    settings_load(&g_settings);
    settings_apply(&g_settings);
}

int api_settings_port(void) {
    return g_settings.port;
}

/* ================================================================== */
/* BIOS model (bios.json)                                              */
/* ================================================================== */

typedef struct {
    char resolution[16];
    int  refresh_hz;
    char output[16];
    int  vram_gb;
    char cpu_governor[16];
    int  ssh_enabled;
    char boot_mode[16];          /* normal|recovery|single */
    char root_device[64];
    char kernel_params[512];
} bios_t;

static void bios_defaults(bios_t *b) {
    memset(b, 0, sizeof(*b));
    util_copy(b->resolution, sizeof(b->resolution), "1920x1080");
    b->refresh_hz = 60;
    util_copy(b->output, sizeof(b->output), "HDMI");
    b->vram_gb = 2;
    util_copy(b->cpu_governor, sizeof(b->cpu_governor), "performance");
    b->ssh_enabled = 1;
    util_copy(b->boot_mode, sizeof(b->boot_mode), "normal");
    util_copy(b->root_device, sizeof(b->root_device), "/dev/sda2");
    util_copy(b->kernel_params, sizeof(b->kernel_params),
              "rw rootwait mitigations=off");
}

static void bios_to_json(const bios_t *bio, jbuf_t *b) {
    jb_begin_obj(b);
    jb_key(b, "resolution");    jb_str(b, bio->resolution);
    jb_key(b, "refreshHz");     jb_int(b, bio->refresh_hz);
    jb_key(b, "output");        jb_str(b, bio->output);
    jb_key(b, "vramGb");        jb_int(b, bio->vram_gb);
    jb_key(b, "cpuGovernor");   jb_str(b, bio->cpu_governor);
    jb_key(b, "sshEnabled");    jb_bool(b, bio->ssh_enabled);
    jb_key(b, "bootMode");      jb_str(b, bio->boot_mode);
    jb_key(b, "rootDevice");    jb_str(b, bio->root_device);
    jb_key(b, "kernelParams");  jb_str(b, bio->kernel_params);
    jb_end_obj(b);
}

/* bootMode whitelist per the API contract. */
static int bios_boot_mode_valid(const char *mode) {
    return mode &&
           (strcmp(mode, "normal") == 0 ||
            strcmp(mode, "recovery") == 0 ||
            strcmp(mode, "single") == 0);
}

/*
 * Merge a posted BIOS object into `bio`.  A non-object `root` is a no-op
 * (historical lenient behavior).  Returns -1 only when the object carries
 * a bootMode outside the whitelist — callers surface that as HTTP 400
 * "invalid bootMode" and `bio` is left untouched.
 */
static int bios_from_json(bios_t *bio, const json_value_t *root) {
    const json_value_t *v;

    if (!root || root->type != JSON_OBJECT)
        return 0;

    /* Validate before mutating anything: an invalid bootMode rejects the
     * whole POST instead of being silently persisted. */
    v = json_obj_get(root, "bootMode");
    if (v && v->type == JSON_STRING && !bios_boot_mode_valid(v->string))
        return -1;

    util_copy(bio->resolution, sizeof(bio->resolution),
              json_get_string(root, "resolution", bio->resolution));
    bio->refresh_hz = json_get_int(root, "refreshHz", bio->refresh_hz);
    util_copy(bio->output, sizeof(bio->output),
              json_get_string(root, "output", bio->output));
    bio->vram_gb = json_get_int(root, "vramGb", bio->vram_gb);
    if (bio->vram_gb < 0)
        bio->vram_gb = 0;
    util_copy(bio->cpu_governor, sizeof(bio->cpu_governor),
              json_get_string(root, "cpuGovernor", bio->cpu_governor));
    bio->ssh_enabled = json_get_bool(root, "sshEnabled", bio->ssh_enabled);
    util_copy(bio->boot_mode, sizeof(bio->boot_mode),
              json_get_string(root, "bootMode", bio->boot_mode));
    util_copy(bio->root_device, sizeof(bio->root_device),
              json_get_string(root, "rootDevice", bio->root_device));
    util_copy(bio->kernel_params, sizeof(bio->kernel_params),
              json_get_string(root, "kernelParams", bio->kernel_params));
    return 0;
}

static void bios_load(bios_t *bio) {
    char *raw;
    json_value_t *root;

    bios_defaults(bio);
    raw = fsops_read_data_file("bios.json");
    if (!raw)
        return;
    root = json_parse(raw);
    free(raw);
    if (!root)
        return;
    if (bios_from_json(bio, root) != 0) {
        /* A persisted invalid bootMode must not poison runtime state. */
        util_log("bios.json rejected (invalid bootMode), using defaults");
        bios_defaults(bio);
    }
    json_free(root);
}

/*
 * Compose the effective kernel cmdline for ps5-linux-loader from the
 * BIOS model: kernelParams + " root=<rootDevice>" + boot-mode flag.
 */
static void bios_compose_cmdline(const bios_t *bio, char *out, size_t len) {
    size_t n = 0;
    out[0] = '\0';
    if (*bio->kernel_params) {
        util_copy(out, len, bio->kernel_params);
        n = strlen(out);
    }
    if (*bio->root_device && n + 6 + strlen(bio->root_device) < len) {
        snprintf(out + n, len - n, "%sroot=%s", n ? " " : "",
                 bio->root_device);
        n = strlen(out);
    }
    if (strcmp(bio->boot_mode, "single") == 0 && n + 8 < len)
        util_copy(out + n, len - n, " single");
    else if (strcmp(bio->boot_mode, "recovery") == 0 && n + 10 < len)
        util_copy(out + n, len - n, " recovery");
}

/*
 * Side effect of POST /api/bios: sync vramGb -> vram.txt and the
 * composed cmdline -> cmdline.txt at the SELECTED DEVICE ROOT.
 * A missing device is NOT an error (the loader simply keeps its old
 * files); a write failure with the device present is reported via `err`.
 */
static int bios_sync_device(const bios_t *bio, char *err, size_t errlen) {
    char vram[16];
    char cmdline[640];

    if (!fsops_device_present()) {
        util_log("bios sync: device %s not present, skipping",
                 fsops_linux_device());
        return 0;
    }

    snprintf(vram, sizeof(vram), "%d", bio->vram_gb);
    if (fsops_write_linux_config(FSOPS_NAME_VRAM, vram, err, errlen) != 0)
        return -1;

    bios_compose_cmdline(bio, cmdline, sizeof(cmdline));
    if (fsops_write_linux_config(FSOPS_NAME_CMDLINE, cmdline, err,
                                 errlen) != 0)
        return -1;

    util_log("bios sync: vram.txt=%s, cmdline.txt=\"%s\"", vram, cmdline);
    return 0;
}

/* ================================================================== */
/* Small helpers                                                       */
/* ================================================================== */

/* Extract and URL-decode query parameter `key` ("?a=1&b=2"). */
static int query_get(const char *query, const char *key, char *out,
                     size_t len) {
    size_t klen = strlen(key);
    const char *p = query;

    if (out && len)
        out[0] = '\0';
    if (!p)
        return -1;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t plen = amp ? (size_t)(amp - p) : strlen(p);
        if (plen > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            char raw[1024];
            size_t vlen = plen - klen - 1;
            if (vlen >= sizeof(raw))
                vlen = sizeof(raw) - 1;
            memcpy(raw, p + klen + 1, vlen);
            raw[vlen] = '\0';
            util_url_decode(raw, out, len);
            return 0;
        }
        p = amp ? amp + 1 : p + plen;
    }
    return -1;
}

/* Parse the JSON request body; NULL-safe. */
static json_value_t *parse_body(const httpd_request_t *req) {
    if (!req->body || req->body_len == 0)
        return NULL;
    return json_parse(req->body);
}

static void respond_ok(httpd_response_t *res) {
    httpd_respond_json(res, 200, "{\"ok\":true}");
}

/* Respond {"ok":true,"message":"..."} (+optional extra already-closed). */
static void respond_ok_message(httpd_response_t *res, const char *message) {
    jbuf_t b;
    char *json;
    jb_init(&b);
    jb_begin_obj(&b);
    jb_key(&b, "ok");      jb_bool(&b, 1);
    jb_key(&b, "message"); jb_str(&b, message);
    jb_end_obj(&b);
    json = jb_steal(&b);
    httpd_respond_json(res, 200, json);
    free(json);
}

/* Normalize a client-supplied fs path; 400s are emitted by the caller. */
static int fs_path_arg(const char *raw, char *out, size_t len) {
    return fsapi_normalize(raw, out, len);
}

/* ================================================================== */
/* Route handlers                                                      */
/* ================================================================== */

/* GET /api/status */
static void h_status(const httpd_request_t *req, httpd_response_t *res) {
    jbuf_t b;
    char ip[64], kit[16], fw[32], loader[FSOPS_PATH_MAX];
    char console[64], model[64];
    usb_status_t usbs[FSOPS_MAX_USB];
    int i, loader_found;
    char *json;

    (void)req;
    util_get_ip(ip, sizeof(ip));
    util_kit_type(kit, sizeof(kit));
    util_firmware(fw, sizeof(fw));
    util_console_name(console, sizeof(console));
    util_model_code(model, sizeof(model));
    loader_found = (fsops_find_loader(loader, sizeof(loader)) == 0);
    fsops_scan_usbs(usbs);

    jb_init(&b);
    jb_begin_obj(&b);
    jb_key(&b, "consoleName"); jb_str(&b, console);
    jb_key(&b, "modelCode");   jb_str(&b, model);
    jb_key(&b, "firmware");    jb_str(&b, fw);
    jb_key(&b, "kitType");     jb_str(&b, kit);
    jb_key(&b, "ip");          jb_str(&b, ip);
    jb_key(&b, "port");        jb_int(&b, g_settings.port);
    jb_key(&b, "uptimeSec");   jb_int(&b, (long long)util_uptime_sec());
    jb_key(&b, "loaderPresent"); jb_bool(&b, loader_found);
    jb_key(&b, "loaderPath");    jb_str(&b, loader_found ? loader : "");
    jb_key(&b, "linuxDevice");   jb_str(&b, fsops_linux_device());
    jb_key(&b, "usbs");
    jb_begin_arr(&b);
    for (i = 0; i < FSOPS_MAX_USB; i++) {
        jb_begin_obj(&b);
        jb_key(&b, "mount");      jb_str(&b, usbs[i].mount);
        jb_key(&b, "name");       jb_str(&b, usbs[i].name);
        jb_key(&b, "present");    jb_bool(&b, usbs[i].present);
        jb_key(&b, "linuxFiles"); jb_bool(&b, usbs[i].linux_files);
        jb_end_obj(&b);
    }
    jb_end_arr(&b);
    jb_key(&b, "ftp");
    ftp_to_json(&g_settings.ftp, &b);
    jb_key(&b, "appInstalled"); jb_bool(&b, appdb_is_installed());
    jb_key(&b, "version");      jb_str(&b, PS5LM_VERSION);
    jb_key(&b, "author");       jb_str(&b, PS5LM_AUTHOR);
    jb_end_obj(&b);

    json = jb_steal(&b);
    httpd_respond_json(res, 200, json);
    free(json);
}

/* GET /api/linux/device  →  {device, dir} */
static void h_linux_device_get(const httpd_request_t *req,
                               httpd_response_t *res) {
    jbuf_t b;
    char root[FSOPS_PATH_MAX];
    char *json;

    (void)req;
    fsops_device_root(root, sizeof(root));
    jb_init(&b);
    jb_begin_obj(&b);
    jb_key(&b, "device"); jb_str(&b, fsops_linux_device());
    jb_key(&b, "dir");    jb_str(&b, root);
    jb_end_obj(&b);
    json = jb_steal(&b);
    httpd_respond_json(res, 200, json);
    free(json);
}

/* POST /api/linux/device  {"device":"usb2"} (persisted) */
static void h_linux_device_post(const httpd_request_t *req,
                                httpd_response_t *res) {
    json_value_t *root;
    const char *dev;
    char err[256];

    root = parse_body(req);
    if (!root) {
        httpd_respond_error(res, 400, "JSON body required");
        return;
    }
    dev = json_get_string(root, "device", NULL);
    if (!fsops_device_valid(dev)) {
        httpd_respond_error(res, 400, "invalid device (expected e.g. "
                            "\"usb0\")");
        json_free(root);
        return;
    }
    util_copy(g_settings.linux_device, sizeof(g_settings.linux_device),
              dev);
    json_free(root);

    if (settings_save(&g_settings, err, sizeof(err)) != 0) {
        httpd_respond_error(res, 500, err);
        return;
    }
    fsops_set_linux_device(g_settings.linux_device);
    util_log("linux device switched to %s", g_settings.linux_device);
    respond_ok(res);
}

/* GET /api/linux/files */
static void h_linux_files(const httpd_request_t *req,
                          httpd_response_t *res) {
    linux_file_t *files = NULL;
    size_t count = 0, i;
    char dir[FSOPS_PATH_MAX];
    int has_bz, has_initrd, has_cmdline;
    jbuf_t b;
    char *json;

    (void)req;
    fsops_device_root(dir, sizeof(dir));
    fsops_list_linux_files(NULL, &files, &count, NULL, 0);
    fsops_required_presence(&has_bz, &has_initrd, &has_cmdline);

    jb_init(&b);
    jb_begin_obj(&b);
    jb_key(&b, "device"); jb_str(&b, fsops_linux_device());
    jb_key(&b, "dir");    jb_str(&b, dir);
    jb_key(&b, "files");
    jb_begin_arr(&b);
    for (i = 0; i < count; i++) {
        jb_begin_obj(&b);
        jb_key(&b, "name");    jb_str(&b, files[i].name);
        jb_key(&b, "path");    jb_str(&b, files[i].path);
        jb_key(&b, "size");    jb_int(&b, files[i].size);
        jb_key(&b, "mtime");   jb_int(&b, (long long)files[i].mtime);
        jb_key(&b, "kind");    jb_str(&b, files[i].kind);
        jb_key(&b, "present"); jb_bool(&b, files[i].present);
        jb_end_obj(&b);
    }
    jb_end_arr(&b);
    jb_key(&b, "required");
    jb_begin_obj(&b);
    jb_key(&b, FSOPS_NAME_KERNEL);  jb_bool(&b, has_bz);
    jb_key(&b, FSOPS_NAME_INITRD);  jb_bool(&b, has_initrd);
    jb_key(&b, FSOPS_NAME_CMDLINE); jb_bool(&b, has_cmdline);
    jb_end_obj(&b);
    jb_end_obj(&b);

    json = jb_steal(&b);
    httpd_respond_json(res, 200, json);
    free(json);
    free(files);
}

/* GET /api/linux/config?name=cmdline.txt|vram.txt */
static void h_linux_config_get(const httpd_request_t *req,
                               httpd_response_t *res) {
    char name[64];
    char content[2048];
    int exists = 0;
    jbuf_t b;
    char *json;

    if (query_get(req->query, "name", name, sizeof(name)) != 0 ||
        (strcmp(name, FSOPS_NAME_CMDLINE) != 0 &&
         strcmp(name, FSOPS_NAME_VRAM) != 0)) {
        httpd_respond_error(res, 400,
            "query parameter name=cmdline.txt|vram.txt required");
        return;
    }

    if (fsops_read_linux_config(name, content, sizeof(content),
                                &exists) != 0) {
        httpd_respond_error(res, 500, "failed to read config file");
        return;
    }

    jb_init(&b);
    jb_begin_obj(&b);
    jb_key(&b, "name");    jb_str(&b, name);
    jb_key(&b, "content"); jb_str(&b, content);
    jb_key(&b, "exists");  jb_bool(&b, exists);
    jb_end_obj(&b);
    json = jb_steal(&b);
    httpd_respond_json(res, 200, json);
    free(json);
}

/* POST /api/linux/config  {"name":"cmdline.txt","content":"..."} */
static void h_linux_config_post(const httpd_request_t *req,
                                httpd_response_t *res) {
    json_value_t *root;
    const char *name, *content;
    char err[256];

    root = parse_body(req);
    if (!root) {
        httpd_respond_error(res, 400, "JSON body required");
        return;
    }
    name = json_get_string(root, "name", NULL);
    content = json_get_string(root, "content", NULL);
    if (!name || !content) {
        httpd_respond_error(res, 400, "\"name\" and \"content\" required");
        json_free(root);
        return;
    }
    if (fsops_write_linux_config(name, content, err, sizeof(err)) != 0) {
        httpd_respond_error(res, 400, err);
        json_free(root);
        return;
    }
    util_log("wrote %s (%zu bytes)", name, strlen(content));
    json_free(root);
    respond_ok(res);
}

/* GET /api/bios */
static void h_bios_get(const httpd_request_t *req, httpd_response_t *res) {
    bios_t bio;
    jbuf_t b;
    char *json;

    (void)req;
    bios_load(&bio);
    jb_init(&b);
    bios_to_json(&bio, &b);
    json = jb_steal(&b);
    httpd_respond_json(res, 200, json);
    free(json);
}

/*
 * POST /api/bios  (persists bios.json; syncs vram.txt/cmdline.txt to the
 * device root only when the biosSyncCmdline opt-in is enabled — v1.2).
 * bootMode whitelist validation happens in bios_from_json before anything
 * is persisted, unconditionally.
 */
static void h_bios_post(const httpd_request_t *req, httpd_response_t *res) {
    json_value_t *root;
    bios_t bio;
    jbuf_t b;
    char *json;
    char err[256];

    root = parse_body(req);
    if (!root) {
        httpd_respond_error(res, 400, "JSON body required");
        return;
    }
    bios_load(&bio);                     /* start from current values */
    if (bios_from_json(&bio, root) != 0) {
        httpd_respond_error(res, 400, "invalid bootMode");
        json_free(root);
        return;
    }
    json_free(root);

    jb_init(&b);
    bios_to_json(&bio, &b);
    json = jb_steal(&b);
    if (fsops_write_data_file("bios.json", json, strlen(json), err,
                              sizeof(err)) != 0) {
        httpd_respond_error(res, 500, err);
        free(json);
        return;
    }
    free(json);

    if (g_settings.bios_sync_cmdline) {
        if (bios_sync_device(&bio, err, sizeof(err)) != 0) {
            /* Persisted but device sync failed: report it honestly. */
            httpd_respond_error(res, 500, err);
            return;
        }
    } else {
        /* Default (v1.2): never touch cmdline.txt/vram.txt on the USB —
         * a distro bootloader may read them. */
        util_log("bios sync disabled (biosSyncCmdline=false); "
                 "bios.json persisted only");
    }
    respond_ok(res);
}

/* GET /api/boot/grub */
static void h_grub_get(const httpd_request_t *req, httpd_response_t *res) {
    boot_config_t cfg;
    jbuf_t b;
    char *json;

    (void)req;
    grub_load(&cfg);
    jb_init(&b);
    grub_to_json(&cfg, &b);
    json = jb_steal(&b);
    grub_free(&cfg);
    httpd_respond_json(res, 200, json);
    free(json);
}

/* POST /api/boot/grub  (persists entries.json + regenerates grub.cfg) */
static void h_grub_post(const httpd_request_t *req, httpd_response_t *res) {
    json_value_t *root;
    boot_config_t cfg;
    char err[256];

    root = parse_body(req);
    if (!root) {
        httpd_respond_error(res, 400, "JSON body required");
        return;
    }
    grub_load(&cfg);
    if (grub_from_json(&cfg, root) != 0) {
        httpd_respond_error(res, 400, "invalid GRUB config object");
        grub_free(&cfg);
        json_free(root);
        return;
    }
    json_free(root);

    if (grub_save(&cfg, err, sizeof(err)) != 0) {
        httpd_respond_error(res, 500, err);
        grub_free(&cfg);
        return;
    }
    if (grub_write_cfg(&cfg, g_settings.grub_mirror_usb,
                       err, sizeof(err)) != 0) {
        httpd_respond_error(res, 500, err);
        grub_free(&cfg);
        return;
    }
    util_log("grub.cfg regenerated (%d entries)", cfg.n_entries);
    grub_free(&cfg);
    respond_ok(res);
}

/* GET /api/payloads — uploads only (the PAYLOADS dir, *.elf) */
static void h_payloads(const httpd_request_t *req, httpd_response_t *res) {
    payload_info_t *list = NULL;
    size_t count = 0, i;
    jbuf_t b;
    char *json;

    (void)req;
    fsops_list_payloads(&list, &count);

    jb_init(&b);
    jb_begin_obj(&b);
    jb_key(&b, "payloads");
    jb_begin_arr(&b);
    for (i = 0; i < count; i++) {
        jb_begin_obj(&b);
        jb_key(&b, "name");   jb_str(&b, list[i].name);
        jb_key(&b, "path");   jb_str(&b, list[i].path);
        jb_key(&b, "size");   jb_int(&b, list[i].size);
        jb_key(&b, "mtime");  jb_int(&b, (long long)list[i].mtime);
        jb_end_obj(&b);
    }
    jb_end_arr(&b);
    jb_end_obj(&b);

    json = jb_steal(&b);
    httpd_respond_json(res, 200, json);
    free(json);
    free(list);
}

/* POST /api/payloads/upload?name=x.elf  (raw octet-stream ELF body) */
static void h_payload_upload(const httpd_request_t *req,
                             httpd_response_t *res) {
    char name[256];
    char err[256];
    char path[FSOPS_PATH_MAX];
    jbuf_t b;
    char *json;

    if (query_get(req->query, "name", name, sizeof(name)) != 0) {
        httpd_respond_error(res, 400, "query parameter name=x.elf "
                            "required");
        return;
    }
    if (!req->stream_path || req->stream_size == 0) {
        httpd_respond_error(res, 400, "raw application/octet-stream ELF "
                            "body required");
        return;
    }
    if (fsops_finalize_upload(req->stream_path, name, err,
                              sizeof(err)) != 0) {
        httpd_respond_error(res, 400, err);
        return;
    }

    snprintf(path, sizeof(path), "%s/%s", FSOPS_PAYLOADS_DIR, name);
    jb_init(&b);
    jb_begin_obj(&b);
    jb_key(&b, "ok");   jb_bool(&b, 1);
    jb_key(&b, "path"); jb_str(&b, path);
    jb_end_obj(&b);
    json = jb_steal(&b);
    httpd_respond_json(res, 200, json);
    free(json);
}

/* DELETE /api/payloads?name=x.elf */
static void h_payload_delete(const httpd_request_t *req,
                             httpd_response_t *res) {
    char name[256];
    char err[256];

    if (query_get(req->query, "name", name, sizeof(name)) != 0) {
        httpd_respond_error(res, 400, "query parameter name=x.elf "
                            "required");
        return;
    }
    if (fsops_delete_payload(name, err, sizeof(err)) != 0) {
        httpd_respond_error(res, 404, err);
        return;
    }
    respond_ok(res);
}

/* POST /api/launch  {"path":"/data/PS5_LINUX_MANAGER/PAYLOADS/x.elf"} */
static void h_launch(const httpd_request_t *req, httpd_response_t *res) {
    json_value_t *root;
    const char *path;
    char err[512];
    char msg[64];
    long long sent = 0;

    root = parse_body(req);
    if (!root) {
        httpd_respond_error(res, 400, "JSON body required");
        return;
    }
    path = json_get_string(root, "path", NULL);
    if (!path) {
        httpd_respond_error(res, 400, "\"path\" required");
        json_free(root);
        return;
    }
    if (serve_elf_9021(path, &sent, err, sizeof(err)) != 0) {
        httpd_respond_error(res, 400, err);
        json_free(root);
        return;
    }
    json_free(root);

    util_notify("Payload served on port 9021");
    snprintf(msg, sizeof(msg), "Payload served on port %d",
             PS5LM_ELF_LOADER_PORT);
    respond_ok_message(res, msg);
}

/* POST /api/boot/linux — serve the discovered loader ELF to :9021 */
static void h_boot_linux(const httpd_request_t *req,
                         httpd_response_t *res) {
    char loader[FSOPS_PATH_MAX];
    char err[512];
    char msg[64];
    long long sent = 0;

    (void)req;
    if (fsops_find_loader(loader, sizeof(loader)) != 0) {
        httpd_respond_error(res, 404,
            "no Linux loader found (upload ps5-linux-loader*.elf or "
            "place a *.elf at the selected device root)");
        return;
    }
    util_log("booting Linux via %s", loader);
    if (serve_elf_9021(loader, &sent, err, sizeof(err)) != 0) {
        httpd_respond_error(res, 500, err);
        return;
    }
    util_notify("Booting Linux...");
    snprintf(msg, sizeof(msg), "Booting Linux (%s served on port %d)",
             loader, PS5LM_ELF_LOADER_PORT);
    respond_ok_message(res, msg);
}

/* POST /api/boot/orbis — exit the manager, back to Orbis OS */
static void h_boot_orbis(const httpd_request_t *req,
                         httpd_response_t *res) {
    (void)req;
    util_log("returning to Orbis OS (manager exiting)");
    util_notify("Returning to Orbis OS");
    respond_ok(res);
    /* The response is flushed by httpd before the next loop iteration,
     * where main() notices the stop request and exits gracefully. */
    manager_request_stop();
}

/* ================================================================== */
/* Real filesystem API (/api/fs/ endpoints)                            */
/* ================================================================== */

/* GET /api/fs/list?path=/... */
static void h_fs_list(const httpd_request_t *req, httpd_response_t *res) {
    char raw[FSAPI_PATH_MAX], path[FSAPI_PATH_MAX];
    char err[256];
    jbuf_t b, entries;
    char *json, *arr;

    if (query_get(req->query, "path", raw, sizeof(raw)) != 0 ||
        fs_path_arg(raw, path, sizeof(path)) != 0) {
        httpd_respond_error(res, 400, "query parameter path=<absolute> "
                            "required");
        return;
    }
    jb_init(&entries);
    if (fsapi_list_json(path, &entries, err, sizeof(err)) != 0) {
        jb_free(&entries);
        httpd_respond_error(res, 404, err);
        return;
    }
    arr = jb_steal(&entries);
    jb_init(&b);
    jb_begin_obj(&b);
    jb_key(&b, "path");    jb_str(&b, path);
    jb_key(&b, "entries"); jb_raw(&b, arr);
    jb_end_obj(&b);
    free(arr);
    json = jb_steal(&b);
    httpd_respond_json(res, 200, json);
    free(json);
}

/* GET /api/fs/stat?path=/... */
static void h_fs_stat(const httpd_request_t *req, httpd_response_t *res) {
    char raw[FSAPI_PATH_MAX], path[FSAPI_PATH_MAX];
    char err[256];
    jbuf_t b;
    char *json;

    if (query_get(req->query, "path", raw, sizeof(raw)) != 0 ||
        fs_path_arg(raw, path, sizeof(path)) != 0) {
        httpd_respond_error(res, 400, "query parameter path=<absolute> "
                            "required");
        return;
    }
    jb_init(&b);
    if (fsapi_stat_json(path, &b, err, sizeof(err)) != 0) {
        jb_free(&b);
        httpd_respond_error(res, 404, err);
        return;
    }
    json = jb_steal(&b);
    httpd_respond_json(res, 200, json);
    free(json);
}

/* GET /api/fs/read?path=/...  (text <= 256 KiB) */
static void h_fs_read(const httpd_request_t *req, httpd_response_t *res) {
    char raw[FSAPI_PATH_MAX], path[FSAPI_PATH_MAX];
    char err[256];
    char *content = NULL;
    int truncated = 0;
    jbuf_t b;
    char *json;

    if (query_get(req->query, "path", raw, sizeof(raw)) != 0 ||
        fs_path_arg(raw, path, sizeof(path)) != 0) {
        httpd_respond_error(res, 400, "query parameter path=<absolute> "
                            "required");
        return;
    }
    if (fsapi_read_text(path, &content, &truncated, err,
                        sizeof(err)) != 0) {
        httpd_respond_error(res, 404, err);
        return;
    }
    jb_init(&b);
    jb_begin_obj(&b);
    jb_key(&b, "path");      jb_str(&b, path);
    jb_key(&b, "content");   jb_str(&b, content ? content : "");
    jb_key(&b, "truncated"); jb_bool(&b, truncated);
    jb_end_obj(&b);
    free(content);
    json = jb_steal(&b);
    httpd_respond_json(res, 200, json);
    free(json);
}

/* Shared: read one string field from a JSON body and normalize it. */
static json_value_t *fs_post_path(const httpd_request_t *req,
                                  const char *key, char *out, size_t len,
                                  httpd_response_t *res) {
    json_value_t *root = parse_body(req);
    const char *raw;
    if (!root) {
        httpd_respond_error(res, 400, "JSON body required");
        return NULL;
    }
    raw = json_get_string(root, key, NULL);
    if (!raw || fs_path_arg(raw, out, len) != 0) {
        httpd_respond_error(res, 400, "an absolute path is required");
        json_free(root);
        return NULL;
    }
    return root;
}

/* POST /api/fs/write  {"path":"...","content":"..."} (atomic) */
static void h_fs_write(const httpd_request_t *req, httpd_response_t *res) {
    char path[FSAPI_PATH_MAX];
    char err[256];
    json_value_t *root;
    const char *content;

    root = fs_post_path(req, "path", path, sizeof(path), res);
    if (!root)
        return;
    content = json_get_string(root, "content", NULL);
    if (!content) {
        httpd_respond_error(res, 400, "\"content\" required");
        json_free(root);
        return;
    }
    if (fsapi_write_text(path, content, err, sizeof(err)) != 0) {
        httpd_respond_error(res, 500, err);
        json_free(root);
        return;
    }
    json_free(root);
    respond_ok(res);
}

/* POST /api/fs/mkdir  {"path":"..."} */
static void h_fs_mkdir(const httpd_request_t *req, httpd_response_t *res) {
    char path[FSAPI_PATH_MAX];
    char err[256];
    json_value_t *root;

    root = fs_post_path(req, "path", path, sizeof(path), res);
    if (!root)
        return;
    if (fsapi_mkdir(path, err, sizeof(err)) != 0) {
        httpd_respond_error(res, 500, err);
        json_free(root);
        return;
    }
    json_free(root);
    respond_ok(res);
}

/* POST /api/fs/delete  {"path":"..."} (recursive for dirs) */
static void h_fs_delete(const httpd_request_t *req, httpd_response_t *res) {
    char path[FSAPI_PATH_MAX];
    char err[256];
    json_value_t *root;

    root = fs_post_path(req, "path", path, sizeof(path), res);
    if (!root)
        return;
    if (fsapi_delete(path, err, sizeof(err)) != 0) {
        httpd_respond_error(res, 500, err);
        json_free(root);
        return;
    }
    json_free(root);
    respond_ok(res);
}

/* Shared two-path handler for rename/copy. */
static void h_fs_two_path(const httpd_request_t *req,
                          httpd_response_t *res,
                          int (*op)(const char *, const char *,
                                    char *, size_t),
                          const char *opname) {
    char from[FSAPI_PATH_MAX], to[FSAPI_PATH_MAX];
    char err[256];
    json_value_t *root;
    const char *raw;

    root = fs_post_path(req, "from", from, sizeof(from), res);
    if (!root)
        return;
    raw = json_get_string(root, "to", NULL);
    if (!raw || fs_path_arg(raw, to, sizeof(to)) != 0) {
        httpd_respond_error(res, 400, "an absolute \"to\" path is "
                            "required");
        json_free(root);
        return;
    }
    if (op(from, to, err, sizeof(err)) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s failed: %s", opname, err);
        httpd_respond_error(res, 500, msg);
        json_free(root);
        return;
    }
    json_free(root);
    respond_ok(res);
}

static void h_fs_rename(const httpd_request_t *req, httpd_response_t *res) {
    h_fs_two_path(req, res, fsapi_rename, "rename");
}

static void h_fs_copy(const httpd_request_t *req, httpd_response_t *res) {
    h_fs_two_path(req, res, fsapi_copy, "copy");
}

/* ================================================================== */
/* FTP settings (/api/ftp)                                             */
/* ================================================================== */

/* GET /api/ftp → {enabled, port, user, passSet} */
static void h_ftp_get(const httpd_request_t *req, httpd_response_t *res) {
    jbuf_t b;
    char *json;

    (void)req;
    jb_init(&b);
    ftp_to_json(&g_settings.ftp, &b);
    json = jb_steal(&b);
    httpd_respond_json(res, 200, json);
    free(json);
}

/* POST /api/ftp {enabled, port, user, pass} → restart the daemon */
static void h_ftp_post(const httpd_request_t *req, httpd_response_t *res) {
    json_value_t *root;
    ftp_config_t f = g_settings.ftp;
    char err[256];

    root = parse_body(req);
    if (!root) {
        httpd_respond_error(res, 400, "JSON body required");
        return;
    }
    f.enabled = json_get_bool(root, "enabled", f.enabled);
    f.port = json_get_int(root, "port", f.port);
    if (f.port <= 0 || f.port > 65535)
        f.port = 2121;
    util_copy(f.user, sizeof(f.user),
              json_get_string(root, "user", f.user));
    /* The password is only replaced when the key is present, so the UI
     * can save other settings without re-typing it. */
    {
        const json_value_t *p = json_obj_get(root, "pass");
        if (p && p->type == JSON_STRING)
            util_copy(f.pass, sizeof(f.pass), p->string);
    }
    json_free(root);

    g_settings.ftp = f;
    if (settings_save(&g_settings, err, sizeof(err)) != 0) {
        httpd_respond_error(res, 500, err);
        return;
    }
    ftp_configure(&g_settings.ftp);        /* restart with new settings */
    util_log("ftp %s on port %d", g_settings.ftp.enabled ?
             "enabled" : "disabled", g_settings.ftp.port);
    respond_ok(res);
}

/* GET /api/settings */
static void h_settings_get(const httpd_request_t *req,
                           httpd_response_t *res) {
    jbuf_t b;
    char *json;

    (void)req;
    jb_init(&b);
    settings_to_json(&g_settings, &b);
    json = jb_steal(&b);
    httpd_respond_json(res, 200, json);
    free(json);
}

/* POST /api/settings (the six GET keys; linuxDevice/ftp have their own
 * endpoints and are preserved here via the struct copy). */
static void h_settings_post(const httpd_request_t *req,
                            httpd_response_t *res) {
    json_value_t *root;
    settings_t s = g_settings;
    char err[256];

    root = parse_body(req);
    if (!root) {
        httpd_respond_error(res, 400, "JSON body required");
        return;
    }
    s.port = json_get_int(root, "port", s.port);
    if (s.port <= 0 || s.port > 65535)
        s.port = 8090;
    s.autostart = json_get_bool(root, "autostart", s.autostart);
    s.notifications = json_get_bool(root, "notifications",
                                    s.notifications);
    util_copy(s.theme, sizeof(s.theme),
              json_get_string(root, "theme", s.theme));
    s.grub_mirror_usb = json_get_bool(root, "grubMirrorUsb",
                                      s.grub_mirror_usb);
    s.bios_sync_cmdline = json_get_bool(root, "biosSyncCmdline",
                                        s.bios_sync_cmdline);
    json_free(root);

    if (settings_save(&s, err, sizeof(err)) != 0) {
        httpd_respond_error(res, 500, err);
        return;
    }
    g_settings = s;
    settings_apply(&g_settings);
    /* Note: a changed port takes effect after the manager restarts. */
    respond_ok(res);
}

/* ================================================================== */
/* Dispatcher                                                          */
/* ================================================================== */

void api_handle(const httpd_request_t *req, httpd_response_t *res,
                void *user) {
    const char *p = req->path;
    int is_get = strcmp(req->method, "GET") == 0;
    int is_post = strcmp(req->method, "POST") == 0;
    int is_del = strcmp(req->method, "DELETE") == 0;

    (void)user;
    httpd_response_init(res);

    if (is_get && strcmp(p, "/api/status") == 0)
        h_status(req, res);
    else if (is_get && strcmp(p, "/api/linux/device") == 0)
        h_linux_device_get(req, res);
    else if (is_post && strcmp(p, "/api/linux/device") == 0)
        h_linux_device_post(req, res);
    else if (is_get && strcmp(p, "/api/linux/files") == 0)
        h_linux_files(req, res);
    else if (is_get && strcmp(p, "/api/linux/config") == 0)
        h_linux_config_get(req, res);
    else if (is_post && strcmp(p, "/api/linux/config") == 0)
        h_linux_config_post(req, res);
    else if (is_get && strcmp(p, "/api/bios") == 0)
        h_bios_get(req, res);
    else if (is_post && strcmp(p, "/api/bios") == 0)
        h_bios_post(req, res);
    else if (is_get && strcmp(p, "/api/boot/grub") == 0)
        h_grub_get(req, res);
    else if (is_post && strcmp(p, "/api/boot/grub") == 0)
        h_grub_post(req, res);
    else if (is_get && strcmp(p, "/api/payloads") == 0)
        h_payloads(req, res);
    else if (is_post && strcmp(p, "/api/payloads/upload") == 0)
        h_payload_upload(req, res);
    else if (is_del && strcmp(p, "/api/payloads") == 0)
        h_payload_delete(req, res);
    else if (is_post && strcmp(p, "/api/launch") == 0)
        h_launch(req, res);
    else if (is_post && strcmp(p, "/api/boot/linux") == 0)
        h_boot_linux(req, res);
    else if (is_post && strcmp(p, "/api/boot/orbis") == 0)
        h_boot_orbis(req, res);
    else if (is_get && strcmp(p, "/api/fs/list") == 0)
        h_fs_list(req, res);
    else if (is_get && strcmp(p, "/api/fs/stat") == 0)
        h_fs_stat(req, res);
    else if (is_get && strcmp(p, "/api/fs/read") == 0)
        h_fs_read(req, res);
    else if (is_post && strcmp(p, "/api/fs/write") == 0)
        h_fs_write(req, res);
    else if (is_post && strcmp(p, "/api/fs/mkdir") == 0)
        h_fs_mkdir(req, res);
    else if (is_post && strcmp(p, "/api/fs/delete") == 0)
        h_fs_delete(req, res);
    else if (is_post && strcmp(p, "/api/fs/rename") == 0)
        h_fs_rename(req, res);
    else if (is_post && strcmp(p, "/api/fs/copy") == 0)
        h_fs_copy(req, res);
    else if (is_get && strcmp(p, "/api/ftp") == 0)
        h_ftp_get(req, res);
    else if (is_post && strcmp(p, "/api/ftp") == 0)
        h_ftp_post(req, res);
    else if (is_get && strcmp(p, "/api/settings") == 0)
        h_settings_get(req, res);
    else if (is_post && strcmp(p, "/api/settings") == 0)
        h_settings_post(req, res);
    else if (!is_get && !is_post && !is_del)
        httpd_respond_error(res, 405, "method not allowed");
    else
        httpd_respond_error(res, 404, "unknown endpoint");
}
