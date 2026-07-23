/*
 * smoke.c — host smoke test for PS5 Linux Manager (contract v1.1).
 *
 * Links the FULL payload (httpd, api, fsops, fsapi, ftpsrv, appdb,
 * launch, grub, util, json, vendored sqlite3) with HOST_TEST shims and
 * exercises every new/changed endpoint over raw TCP:
 *
 *   - status fields (consoleName/modelCode/linuxDevice/ftp/appInstalled/
 *     author, no socTempC)
 *   - linux device switch + root scanning (no PS5/Linux subdir)
 *   - payload upload (raw octet-stream -> file), listing, delete
 *   - launch + boot/linux served to the (stubbed) 9021 ELF loader with
 *     byte-for-byte integrity assertions
 *   - fs list/stat/read/write/mkdir/delete/rename/copy incl. nested
 *     recursive copy + move and path-traversal safety
 *   - a real FTP protocol session (anonymous + USER/PASS auth, PWD, CWD,
 *     PASV+LIST, RETR, STOR, RNFR/RNTO)
 *   - exactly 3 startup notifications, in order
 *   - app.db install degrading gracefully on a garbage db, then really
 *     inserting into a real sqlite db
 *   - v1.2 safety switches: with default settings neither POST /api/bios
 *     nor POST /api/boot/grub writes to the USB device root; enabling
 *     grubMirrorUsb/biosSyncCmdline (round-trip through /api/settings
 *     and config.json) restores those writes
 *
 * The manager's main() is compiled to ps5lm_main (-Dmain=ps5lm_main) and
 * runs on a pthread; the test drives it and finally stops it via
 * POST /api/boot/orbis.
 */
#if !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "appdb.h"
#include "fsops.h"
#include "third_party/sqlite3.h"

/* From tests/host_stubs.c. */
int         stub_notif_count(void);
const char *stub_notif(int i);

/* The manager entry point (main renamed by the Makefile). */
extern int ps5lm_main(void);

#define HTTP_PORT 18090
#define FTP_PORT  12121
#define ELF_PORT  PS5LM_ELF_LOADER_PORT   /* 19921 (Makefile) */

#define USB0 FSOPS_MNT_PREFIX "/usb0"
#define USB1 FSOPS_MNT_PREFIX "/usb1"
#define FSD  "/tmp/ps5lm-smoke/fs"

static int g_checks;
static int g_failures;

#define CHECK(cond, msg) do {                                   \
        g_checks++;                                             \
        if (cond) {                                             \
            printf("  ok    %s\n", msg);                        \
        } else {                                                \
            g_failures++;                                       \
            printf("  FAIL  %s  [%s:%d]\n", msg, __FILE__,      \
                   __LINE__);                                   \
        }                                                       \
    } while (0)

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

static void write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot create %s\n", path);
        exit(2);
    }
    if (len && fwrite(data, 1, len, f) != len) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    fclose(f);
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    if (len)
        *len = (size_t)n;
    return buf;
}

static int tcp_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    struct timeval tv = { 10, 0 };
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t w = send(fd, p, len, MSG_NOSIGNAL);
        if (w <= 0)
            return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Minimal HTTP client (Connection: close semantics)                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int   status;
    char *body;          /* malloc'd, NUL-terminated */
    size_t body_len;
} http_result_t;

static void http_result_free(http_result_t *r) {
    free(r->body);
    r->body = NULL;
}

/* Perform one request; returns 0 with `res` filled, -1 on I/O error. */
static int http_request(const char *method, const char *path,
                        const void *body, size_t body_len,
                        const char *content_type, http_result_t *res) {
    char hdr[2048];
    char *raw = NULL;
    size_t raw_len = 0, raw_cap = 0;
    int fd, n;
    char chunk[16384];
    char *sep;

    memset(res, 0, sizeof(*res));
    fd = tcp_connect(HTTP_PORT);
    if (fd < 0)
        return -1;
    n = snprintf(hdr, sizeof(hdr),
                 "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                 "Connection: close\r\n", method, path);
    if (body) {
        n += snprintf(hdr + n, sizeof(hdr) - (size_t)n,
                      "Content-Type: %s\r\nContent-Length: %zu\r\n",
                      content_type, body_len);
    }
    n += snprintf(hdr + n, sizeof(hdr) - (size_t)n, "\r\n");
    if (send_all(fd, hdr, (size_t)n) != 0 ||
        (body && send_all(fd, body, body_len) != 0)) {
        close(fd);
        return -1;
    }
    for (;;) {
        ssize_t r = recv(fd, chunk, sizeof(chunk), 0);
        if (r == 0)
            break;
        if (r < 0)
            break;                       /* timeout: use what we have */
        if (raw_len + (size_t)r + 1 > raw_cap) {
            raw_cap = (raw_len + (size_t)r + 1) * 2;
            raw = realloc(raw, raw_cap);
        }
        memcpy(raw + raw_len, chunk, (size_t)r);
        raw_len += (size_t)r;
    }
    close(fd);
    if (!raw || raw_len < 12) {
        free(raw);
        return -1;
    }
    raw[raw_len] = '\0';
    res->status = atoi(raw + 9);
    sep = strstr(raw, "\r\n\r\n");
    if (sep) {
        res->body_len = raw_len - (size_t)(sep + 4 - raw);
        res->body = malloc(res->body_len + 1);
        memcpy(res->body, sep + 4, res->body_len);
        res->body[res->body_len] = '\0';
    }
    free(raw);
    return 0;
}

static int http_json(const char *method, const char *path, const char *json,
                     http_result_t *res) {
    return http_request(method, path, json, json ? strlen(json) : 0,
                        "application/json", res);
}

/* ------------------------------------------------------------------ */
/* 9021 ELF-loader capture server                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t len;
    int    accepted;
} capture_t;

static void *capture_server(void *arg) {
    capture_t *cap = arg;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1, cfd;
    struct sockaddr_in a;
    char chunk[65536];

    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(ELF_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(lfd, 1) != 0) {
        close(lfd);
        return NULL;
    }
    cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) {
        close(lfd);
        return NULL;
    }
    cap->accepted = 1;
    for (;;) {
        ssize_t r = recv(cfd, chunk, sizeof(chunk), 0);
        if (r <= 0)
            break;
        cap->buf = realloc(cap->buf, cap->len + (size_t)r);
        memcpy(cap->buf + cap->len, chunk, (size_t)r);
        cap->len += (size_t)r;
    }
    close(cfd);
    close(lfd);
    return NULL;
}

/* Run `request_fn` while a capture listener collects the served ELF. */
static int capture_launch(const char *method, const char *path,
                          const char *json, capture_t *cap,
                          http_result_t *res) {
    pthread_t t;
    memset(cap, 0, sizeof(*cap));
    pthread_create(&t, NULL, capture_server, cap);
    usleep(150 * 1000);                    /* let the listener bind */
    if (http_json(method, path, json, res) != 0) {
        pthread_cancel(t);
        pthread_join(t, NULL);
        return -1;
    }
    pthread_join(t, NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Minimal FTP client                                                  */
/* ------------------------------------------------------------------ */

/* Read one reply line; returns the 3-digit code.  Multi-line replies
 * ("250- ... 250 End") are drained. */
static int ftp_reply(int fd, char *line, size_t len) {
    int code = -1;
    for (;;) {
        size_t n = 0;
        char c;
        while (n + 1 < len) {
            ssize_t r = recv(fd, &c, 1, 0);
            if (r <= 0)
                return code;
            if (c == '\n')
                break;
            if (c != '\r')
                line[n++] = c;
        }
        line[n] = '\0';
        if (code < 0)
            code = atoi(line);
        /* Multi-line: "250-..." continues until "250 " arrives. */
        if (strlen(line) < 4 || line[3] != '-')
            break;
    }
    return code;
}

static int ftp_cmd(int fd, const char *cmd, char *line, size_t len) {
    char buf[1200];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", cmd);
    if (send_all(fd, buf, (size_t)n) != 0)
        return -1;
    return ftp_reply(fd, line, len);
}

/* PASV -> data connection fd. */
static int ftp_pasv_data(int ctl) {
    char line[512];
    int code, h1, h2, h3, h4, p1, p2, port;
    if (ftp_cmd(ctl, "PASV", line, sizeof(line)) < 0)
        return -1;
    code = atoi(line);
    if (code != 227 || sscanf(line, "227 Entering Passive Mode (%d,%d,"
                              "%d,%d,%d,%d)", &h1, &h2, &h3, &h4,
                              &p1, &p2) != 6)
        return -1;
    port = p1 * 256 + p2;
    return tcp_connect(port);
}

/* ------------------------------------------------------------------ */
/* app.db helpers (linked against the vendored sqlite3)                */
/* ------------------------------------------------------------------ */

static void make_fake_db(void) {
    write_file(PS5LM_APPDB_PATH, "this is not a sqlite database", 29);
}

static void make_real_db(void) {
    sqlite3 *db = NULL;
    unlink(PS5LM_APPDB_PATH);
    if (sqlite3_open(PS5LM_APPDB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "cannot create app.db\n");
        exit(2);
    }
    sqlite3_exec(db,
        "CREATE TABLE tbl_contentinfo(titleId TEXT, contentId TEXT,"
        " titleName TEXT, deeplinkUri TEXT, category TEXT,"
        " viewCategory TEXT);"
        "CREATE TABLE tbl_conceptmetadata(titleId TEXT, contentId TEXT,"
        " titleName TEXT);"
        "CREATE TABLE tbl_iconinfo_ps5(titleId TEXT, titleName TEXT,"
        " deeplinkUri TEXT);"
        /* An existing MEDIA app for the category probe to reuse. */
        "INSERT INTO tbl_contentinfo VALUES('PPSA01650','UP0001-x',"
        "'YouTube','','gdx','gdxv');",
        NULL, NULL, NULL);
    sqlite3_close(db);
}

static int db_has_row(const char *title, const char **cat_out,
                      const char **vcat_out) {
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    static char cat[64], vcat[64];
    int found = 0;

    if (sqlite3_open_v2(PS5LM_APPDB_PATH, &db, SQLITE_OPEN_READONLY,
                        NULL) != SQLITE_OK)
        return 0;
    if (sqlite3_prepare_v2(db,
            "SELECT titleName, category, viewCategory FROM"
            " tbl_contentinfo WHERE titleId=?1", -1, &st,
            NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, PS5LM_APP_TITLEID, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *tn = (const char *)sqlite3_column_text(st, 0);
            const char *c = (const char *)sqlite3_column_text(st, 1);
            const char *v = (const char *)sqlite3_column_text(st, 2);
            if (tn && strcmp(tn, title) == 0)
                found = 1;
            snprintf(cat, sizeof(cat), "%s", c ? c : "");
            snprintf(vcat, sizeof(vcat), "%s", v ? v : "");
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (cat_out)
        *cat_out = cat;
    if (vcat_out)
        *vcat_out = vcat;
    return found;
}

/* ------------------------------------------------------------------ */
/* Test fixture setup                                                  */
/* ------------------------------------------------------------------ */

static void setup_fixture(void) {
    char cmd[512];
    static const char cfg[] =
        "{\"port\":18090,\"notifications\":true}";
    snprintf(cmd, sizeof(cmd), "rm -rf /tmp/ps5lm-smoke && "
             "mkdir -p %s %s/PS5/Linux %s %s %s/sub", PS5LM_DATA_DIR,
             USB0, USB1, FSD, FSD);
    if (system(cmd) != 0) {
        fprintf(stderr, "fixture setup failed\n");
        exit(2);
    }
    /* The manager listens on HTTP_PORT (persisted settings). */
    write_file(PS5LM_DATA_DIR "/config.json", cfg, sizeof(cfg) - 1);
    /* Linux files at the usb0 ROOT (v1.1: no PS5/Linux subdir). */
    write_file(USB0 "/bzImage", "kernel-bytes", 12);
    write_file(USB0 "/initrd.img", "initrd-bytes", 12);
    write_file(USB0 "/cmdline.txt", "rw rootwait", 11);
    write_file(USB0 "/vram.txt", "2", 1);
    write_file(USB0 "/test.elf", "\x7f" "ELF-usb0", 9);
    /* A legacy override file must be IGNORED by v1.1 code. */
    write_file(USB0 "/PS5/Linux/path-override.txt", "/nonexistent", 12);
}

static void wait_for_server(void) {
    int i;
    for (i = 0; i < 100; i++) {
        int fd = tcp_connect(HTTP_PORT);
        if (fd >= 0) {
            close(fd);
            return;
        }
        usleep(100 * 1000);
    }
    fprintf(stderr, "manager HTTP server never came up\n");
    exit(2);
}

/* ------------------------------------------------------------------ */
/* The tests                                                           */
/* ------------------------------------------------------------------ */

static void test_status(void) {
    http_result_t r;
    printf("[status]\n");
    if (http_json("GET", "/api/status", NULL, &r) != 0) {
        CHECK(0, "GET /api/status reachable");
        return;
    }
    CHECK(r.status == 200, "status 200");
    CHECK(r.body && strstr(r.body, "\"consoleName\""),
          "status has consoleName");
    CHECK(r.body && strstr(r.body, "\"modelCode\":\"CFI-1215A\""),
          "status modelCode from hw stub");
    CHECK(r.body && strstr(r.body, "\"linuxDevice\":\"usb0\""),
          "status linuxDevice default usb0");
    CHECK(r.body && strstr(r.body, "\"author\":\"InsideMatrix\""),
          "status author InsideMatrix");
    CHECK(r.body && strstr(r.body, "\"version\":\"1.2.0\""),
          "status version 1.2.0");
    CHECK(r.body && strstr(r.body, "\"ftp\":{\"enabled\":false,\"port\":2121"),
          "status ftp object");
    CHECK(r.body && strstr(r.body, "\"appInstalled\":false"),
          "status appInstalled false before install");
    CHECK(r.body && !strstr(r.body, "socTempC"), "status drops socTempC");
    CHECK(r.body && strstr(r.body, "\"loaderPresent\":true"),
          "status loaderPresent (usb0 root test.elf)");
    CHECK(r.body && strstr(r.body, "\"usbs\":["), "status usbs array");
    http_result_free(&r);
}

static void test_linux_device_and_files(void) {
    http_result_t r;
    char *content;
    printf("[linux device/files/config]\n");

    http_json("GET", "/api/linux/device", NULL, &r);
    CHECK(r.status == 200 && r.body &&
          strstr(r.body, "\"device\":\"usb0\""),
          "GET /api/linux/device -> usb0");
    http_result_free(&r);

    http_json("GET", "/api/linux/files", NULL, &r);
    CHECK(r.status == 200 && r.body &&
          strstr(r.body, "\"device\":\"usb0\"") &&
          strstr(r.body, "\"name\":\"bzImage\"") &&
          strstr(r.body, USB0 "/bzImage") &&
          strstr(r.body, "\"bzImage\":true"),
          "files scanned at device root with path fields");
    http_result_free(&r);

    http_json("GET", "/api/linux/config?name=cmdline.txt", NULL, &r);
    CHECK(r.status == 200 && r.body && strstr(r.body, "rw rootwait"),
          "GET linux/config cmdline.txt from device root");
    http_result_free(&r);

    /* path-override.txt is gone from the contract: 400. */
    http_json("GET", "/api/linux/config?name=path-override.txt", NULL, &r);
    CHECK(r.status == 400, "path-override.txt rejected (deleted logic)");
    http_result_free(&r);

    http_json("POST", "/api/linux/config",
              "{\"name\":\"cmdline.txt\",\"content\":\"rw new\"}", &r);
    CHECK(r.status == 200, "POST linux/config cmdline.txt");
    http_result_free(&r);
    content = read_file(USB0 "/cmdline.txt", NULL);
    CHECK(content && strcmp(content, "rw new") == 0,
          "cmdline.txt written to device root");
    free(content);

    /* Invalid device rejected. */
    http_json("POST", "/api/linux/device", "{\"device\":\"hdd0\"}", &r);
    CHECK(r.status == 400, "POST linux/device invalid -> 400");
    http_result_free(&r);

    /* Switch to usb1: scanning + writes follow the selection. */
    http_json("POST", "/api/linux/device", "{\"device\":\"usb1\"}", &r);
    CHECK(r.status == 200, "POST linux/device usb1");
    http_result_free(&r);
    http_json("GET", "/api/linux/files", NULL, &r);
    CHECK(r.status == 200 && r.body &&
          strstr(r.body, "\"device\":\"usb1\"") &&
          !strstr(r.body, "\"name\":\"bzImage\""),
          "files follow device switch (usb1 empty)");
    http_result_free(&r);
    http_json("POST", "/api/linux/config",
              "{\"name\":\"vram.txt\",\"content\":\"4\"}", &r);
    CHECK(r.status == 200, "POST config writes to usb1 root");
    http_result_free(&r);
    content = read_file(USB1 "/vram.txt", NULL);
    CHECK(content && strcmp(content, "4") == 0, "vram.txt at usb1 root");
    free(content);

    /* boot/linux with empty device and no uploads -> 404. */
    http_json("POST", "/api/boot/linux", "{}", &r);
    CHECK(r.status == 404, "boot/linux 404 when no loader anywhere");
    http_result_free(&r);

    /* Device switch persisted in config.json. */
    content = read_file(PS5LM_DATA_DIR "/config.json", NULL);
    CHECK(content && strstr(content, "\"linuxDevice\":\"usb1\""),
          "linuxDevice persisted");
    free(content);

    /* Settings POST must not clobber linuxDevice. */
    http_json("POST", "/api/settings", "{\"theme\":\"light\"}", &r);
    CHECK(r.status == 200, "POST settings theme");
    http_result_free(&r);
    http_json("GET", "/api/linux/device", NULL, &r);
    CHECK(r.body && strstr(r.body, "\"device\":\"usb1\""),
          "device survives settings POST");
    http_result_free(&r);
    http_json("GET", "/api/settings", NULL, &r);
    CHECK(r.body && strstr(r.body, "\"theme\":\"light\"") &&
          !strstr(r.body, "linuxDevice"),
          "settings shape keeps linuxDevice out (own endpoint)");
    http_result_free(&r);

    /* v1.2: config.json above has no opt-in keys -> defaults false. */
    http_json("GET", "/api/settings", NULL, &r);
    CHECK(r.body && strstr(r.body, "\"grubMirrorUsb\":false") &&
          strstr(r.body, "\"biosSyncCmdline\":false"),
          "opt-in flags default false when absent from config.json");
    http_result_free(&r);

    /* Back to usb0 for the remaining tests. */
    http_json("POST", "/api/linux/device", "{\"device\":\"usb0\"}", &r);
    CHECK(r.status == 200, "switch back to usb0");
    http_result_free(&r);
}

static void test_bios_and_grub(void) {
    http_result_t r;
    char *content;
    printf("[bios/grub]\n");

    /*
     * Contract v1.2 defaults (both opt-ins OFF): POST /api/bios and
     * POST /api/boot/grub must NOT write to the USB device root.
     * usb0 still holds the fixture files (vram.txt="2") plus the
     * cmdline.txt="rw new" written by the linux/config test above.
     */
    http_json("POST", "/api/bios",
              "{\"vramGb\":3,\"rootDevice\":\"/dev/sda2\","
              "\"kernelParams\":\"rw quiet\",\"bootMode\":\"single\"}",
              &r);
    CHECK(r.status == 200, "POST /api/bios (biosSyncCmdline off)");
    http_result_free(&r);
    content = read_file(PS5LM_DATA_DIR "/bios.json", NULL);
    CHECK(content && strstr(content, "\"vramGb\":3") &&
          strstr(content, "\"bootMode\":\"single\""),
          "bios.json persisted with sync off");
    free(content);
    content = read_file(USB0 "/vram.txt", NULL);
    CHECK(content && strcmp(content, "2") == 0,
          "vram.txt untouched when biosSyncCmdline off");
    free(content);
    content = read_file(USB0 "/cmdline.txt", NULL);
    CHECK(content && strcmp(content, "rw new") == 0,
          "cmdline.txt untouched when biosSyncCmdline off");
    free(content);

    /* bootMode whitelist validation stays unconditional. */
    http_json("POST", "/api/bios", "{\"bootMode\":\"bogus\"}", &r);
    CHECK(r.status == 400, "invalid bootMode -> 400 even with sync off");
    http_result_free(&r);
    content = read_file(PS5LM_DATA_DIR "/bios.json", NULL);
    CHECK(content && !strstr(content, "bogus"),
          "invalid bootMode not persisted");
    free(content);

    http_json("POST", "/api/boot/grub", "{\"timeoutSec\":9}", &r);
    CHECK(r.status == 200, "POST /api/boot/grub (grubMirrorUsb off)");
    http_result_free(&r);
    CHECK(access(USB0 "/grub.cfg", F_OK) != 0,
          "grub.cfg NOT mirrored to device root by default");
    content = read_file(PS5LM_DATA_DIR "/grub.cfg", NULL);
    CHECK(content && strstr(content, "set timeout=9"),
          "grub.cfg still written to the data dir");
    free(content);
    http_json("GET", "/api/boot/grub", NULL, &r);
    CHECK(r.body && strstr(r.body, "\"timeoutSec\":9"),
          "GET /api/boot/grub reflects POST");
    http_result_free(&r);

    /* Opt in: both flags round-trip through /api/settings + config.json. */
    http_json("POST", "/api/settings",
              "{\"grubMirrorUsb\":true,\"biosSyncCmdline\":true}", &r);
    CHECK(r.status == 200, "POST settings enables both opt-in flags");
    http_result_free(&r);
    http_json("GET", "/api/settings", NULL, &r);
    CHECK(r.body && strstr(r.body, "\"grubMirrorUsb\":true") &&
          strstr(r.body, "\"biosSyncCmdline\":true") &&
          strstr(r.body, "\"theme\":\"light\""),
          "GET /api/settings shows flags on, other keys intact");
    http_result_free(&r);
    content = read_file(PS5LM_DATA_DIR "/config.json", NULL);
    CHECK(content && strstr(content, "\"grubMirrorUsb\":true") &&
          strstr(content, "\"biosSyncCmdline\":true"),
          "opt-in flags persisted in config.json");
    free(content);

    /* With the flags on, the old USB-root writes are restored. */
    http_json("POST", "/api/bios",
              "{\"vramGb\":3,\"rootDevice\":\"/dev/sda2\","
              "\"kernelParams\":\"rw quiet\",\"bootMode\":\"single\"}",
              &r);
    CHECK(r.status == 200, "POST /api/bios (biosSyncCmdline on)");
    http_result_free(&r);
    content = read_file(USB0 "/vram.txt", NULL);
    CHECK(content && strcmp(content, "3") == 0,
          "BIOS sync wrote vram.txt to device root when enabled");
    free(content);
    content = read_file(USB0 "/cmdline.txt", NULL);
    CHECK(content && strstr(content, "rw quiet") &&
          strstr(content, "root=/dev/sda2") &&
          strstr(content, "single"),
          "BIOS sync wrote composed cmdline.txt when enabled");
    free(content);

    http_json("POST", "/api/boot/grub", "{\"timeoutSec\":9}", &r);
    CHECK(r.status == 200, "POST /api/boot/grub (grubMirrorUsb on)");
    http_result_free(&r);
    content = read_file(USB0 "/grub.cfg", NULL);
    CHECK(content && strstr(content, "set timeout=9"),
          "grub.cfg mirrored to device root when enabled");
    free(content);
}

static void test_payloads_and_launch(const char *elf_big, size_t elf_big_n,
                                     const char *elf_ldr, size_t elf_ldr_n) {
    http_result_t r;
    capture_t cap;
    char *content;
    size_t content_n;
    char path[512];
    printf("[payloads/upload/launch]\n");

    /* Upload (raw octet-stream) -> atomic save under PAYLOADS/. */
    if (http_request("POST", "/api/payloads/upload?name=test-payload.elf",
                     elf_big, elf_big_n, "application/octet-stream",
                     &r) != 0) {
        CHECK(0, "upload request");
        return;
    }
    CHECK(r.status == 200 && r.body && strstr(r.body, "\"ok\":true"),
          "POST /api/payloads/upload 200");
    http_result_free(&r);
    content = read_file(PS5LM_DATA_DIR "/PAYLOADS/test-payload.elf",
                        &content_n);
    CHECK(content && content_n == elf_big_n &&
          memcmp(content, elf_big, elf_big_n) == 0,
          "uploaded file stored byte-identical");
    free(content);

    /* Uploads only: usb0's test.elf must NOT be listed. */
    http_json("GET", "/api/payloads", NULL, &r);
    CHECK(r.body && strstr(r.body, "test-payload.elf") &&
          !strstr(r.body, "\"name\":\"test.elf\""),
          "GET /api/payloads lists uploads only");
    CHECK(r.body && strstr(r.body, "\"mtime\":"),
          "payload entries carry mtime");
    http_result_free(&r);

    /* Upload the preferred loader. */
    http_request("POST", "/api/payloads/upload?name=ps5-linux-loader.elf",
                 elf_ldr, elf_ldr_n, "application/octet-stream", &r);
    CHECK(r.status == 200, "upload ps5-linux-loader.elf");
    http_result_free(&r);

    /* Non-ELF upload rejected. */
    http_request("POST", "/api/payloads/upload?name=bad.elf",
                 "not an elf", 10, "application/octet-stream", &r);
    CHECK(r.status == 400, "non-ELF upload rejected");
    http_result_free(&r);
    CHECK(access(PS5LM_DATA_DIR "/PAYLOADS/bad.elf", F_OK) != 0,
          "rejected upload leaves no file (tmp cleaned)");

    /* Oversized upload (Content-Length > 64 MiB) -> 413. */
    {
        int fd = tcp_connect(HTTP_PORT);
        char hdr[512];
        int n = snprintf(hdr, sizeof(hdr),
                         "POST /api/payloads/upload?name=huge.elf "
                         "HTTP/1.1\r\nHost: x\r\nContent-Length: "
                         "70000000\r\n\r\n");
        char resp[256];
        ssize_t rn;
        send_all(fd, hdr, (size_t)n);
        rn = recv(fd, resp, sizeof(resp) - 1, 0);
        if (rn > 0) {
            resp[rn] = '\0';
            CHECK(strstr(resp, "413") != NULL,
                  "oversized upload -> 413");
        } else {
            CHECK(0, "oversized upload -> 413 (no response)");
        }
        close(fd);
    }

    /* POST /api/launch serves the whole ELF to :9021 (stub listener). */
    snprintf(path, sizeof(path), "{\"path\":\"%s/PAYLOADS/"
             "test-payload.elf\"}", PS5LM_DATA_DIR);
    if (capture_launch("POST", "/api/launch", path, &cap, &r) != 0) {
        CHECK(0, "launch request");
        return;
    }
    CHECK(r.status == 200 && r.body &&
          strstr(r.body, "Payload served on port 19921"),
          "launch message 'Payload served on port 9021'");
    http_result_free(&r);
    CHECK(cap.accepted && cap.len == elf_big_n &&
          memcmp(cap.buf, elf_big, elf_big_n) == 0,
          "9021 received byte-identical ELF");
    free(cap.buf);

    /* boot/linux prefers the uploaded ps5-linux-loader*.elf. */
    if (capture_launch("POST", "/api/boot/linux", "{}", &cap, &r) == 0) {
        CHECK(r.status == 200, "boot/linux 200");
        http_result_free(&r);
        CHECK(cap.len == elf_ldr_n &&
              memcmp(cap.buf, elf_ldr, elf_ldr_n) == 0,
              "boot/linux served the uploaded loader");
        free(cap.buf);
    } else {
        CHECK(0, "boot/linux request");
    }

    /* Delete. */
    http_request("DELETE", "/api/payloads?name=test-payload.elf", NULL, 0,
                 NULL, &r);
    CHECK(r.status == 200, "DELETE /api/payloads");
    http_result_free(&r);
    http_json("GET", "/api/payloads", NULL, &r);
    CHECK(r.body && !strstr(r.body, "test-payload.elf"),
          "deleted payload no longer listed");
    http_result_free(&r);
    http_request("DELETE", "/api/payloads?name=test-payload.elf", NULL, 0,
                 NULL, &r);
    CHECK(r.status == 404, "DELETE of missing payload -> 404");
    http_result_free(&r);
}

static void test_fs_api(void) {
    http_result_t r;
    char *content;
    printf("[fs api]\n");

    http_json("POST", "/api/fs/write",
              "{\"path\":\"" FSD "/a.txt\",\"content\":\"hello\"}", &r);
    CHECK(r.status == 200, "fs/write");
    http_result_free(&r);

    http_json("GET", "/api/fs/read?path=" FSD "/a.txt", NULL, &r);
    CHECK(r.status == 200 && r.body &&
          strstr(r.body, "\"content\":\"hello\"") &&
          strstr(r.body, "\"truncated\":false"),
          "fs/read round-trip");
    http_result_free(&r);

    http_json("GET", "/api/fs/stat?path=" FSD "/a.txt", NULL, &r);
    CHECK(r.status == 200 && r.body && strstr(r.body, "\"size\":5"),
          "fs/stat size");
    http_result_free(&r);

    /* Nested directory copy + move. */
    http_json("POST", "/api/fs/mkdir", "{\"path\":\"" FSD "/dir1/sub\"}",
              &r);
    CHECK(r.status == 200, "fs/mkdir nested");
    http_result_free(&r);
    http_json("POST", "/api/fs/write",
              "{\"path\":\"" FSD "/dir1/sub/deep.txt\",\"content\":"
              "\"deep\"}", &r);
    CHECK(r.status == 200, "write nested file");
    http_result_free(&r);
    http_json("POST", "/api/fs/copy",
              "{\"from\":\"" FSD "/dir1\",\"to\":\"" FSD "/dir2\"}", &r);
    CHECK(r.status == 200, "fs/copy recursive dir");
    http_result_free(&r);
    content = read_file(FSD "/dir2/sub/deep.txt", NULL);
    CHECK(content && strcmp(content, "deep") == 0,
          "nested copy landed");
    free(content);
    http_json("POST", "/api/fs/rename",
              "{\"from\":\"" FSD "/dir2\",\"to\":\"" FSD "/dir3\"}", &r);
    CHECK(r.status == 200, "fs/rename dir (move)");
    http_result_free(&r);
    content = read_file(FSD "/dir3/sub/deep.txt", NULL);
    CHECK(content != NULL, "moved tree intact");
    free(content);
    http_json("POST", "/api/fs/delete", "{\"path\":\"" FSD "/dir3\"}",
              &r);
    CHECK(r.status == 200, "fs/delete recursive");
    http_result_free(&r);
    CHECK(access(FSD "/dir3", F_OK) != 0, "deleted tree gone");

    /* UTF-8 file name + dotfile visibility. */
    http_json("POST", "/api/fs/write",
              "{\"path\":\"" FSD "/.hidden-\xc3\xa9.txt\",\"content\":"
              "\"utf8\"}", &r);
    CHECK(r.status == 200, "fs/write UTF-8 dotfile");
    http_result_free(&r);
    http_json("GET", "/api/fs/list?path=" FSD, NULL, &r);
    CHECK(r.status == 200 && r.body &&
          strstr(r.body, ".hidden-"),
          "fs/list includes dotfiles, UTF-8 tolerant");
    http_result_free(&r);

    /* Path traversal collapses to the canonical absolute path. */
    http_json("GET", "/api/fs/list?path=%2F..%2F..", NULL, &r);
    CHECK(r.status == 200 && r.body &&
          strstr(r.body, "\"path\":\"/\""),
          "traversal /../.. normalized to /");
    http_result_free(&r);

    /* Relative paths rejected. */
    http_json("POST", "/api/fs/write",
              "{\"path\":\"relative/x\",\"content\":\"x\"}", &r);
    CHECK(r.status == 400, "relative path -> 400");
    http_result_free(&r);
}

static void test_ftp(void) {
    http_result_t r;
    char line[512];
    int ctl, data, code;
    char buf[256];
    ssize_t n;
    printf("[ftp]\n");

    /* Enable anonymous FTP on FTP_PORT. */
    snprintf(buf, sizeof(buf),
             "{\"enabled\":true,\"port\":%d,\"user\":\"\",\"pass\":\"\"}",
             FTP_PORT);
    http_json("POST", "/api/ftp", buf, &r);
    CHECK(r.status == 200, "POST /api/ftp enable anonymous");
    http_result_free(&r);
    http_json("GET", "/api/ftp", NULL, &r);
    CHECK(r.body && strstr(r.body, "\"enabled\":true") &&
          strstr(r.body, "\"passSet\":false") &&
          !strstr(r.body, "\"pass\":"),
          "GET /api/ftp shape (never leaks pass)");
    http_result_free(&r);
    usleep(200 * 1000);

    ctl = tcp_connect(FTP_PORT);
    CHECK(ctl >= 0, "FTP control connect");
    if (ctl < 0)
        return;
    code = ftp_reply(ctl, line, sizeof(line));
    CHECK(code == 220, "FTP greeting 220");

    code = ftp_cmd(ctl, "USER anonymous", line, sizeof(line));
    CHECK(code == 230, "anonymous USER -> 230");
    code = ftp_cmd(ctl, "PWD", line, sizeof(line));
    CHECK(code == 257 && strstr(line, "\"/\""), "PWD -> /");
    code = ftp_cmd(ctl, "SYST", line, sizeof(line));
    CHECK(code == 215, "SYST");
    code = ftp_cmd(ctl, "TYPE I", line, sizeof(line));
    CHECK(code == 200, "TYPE I");
    code = ftp_cmd(ctl, "NOOP", line, sizeof(line));
    CHECK(code == 200, "NOOP");

    snprintf(buf, sizeof(buf), "CWD %s", FSD);
    code = ftp_cmd(ctl, buf, line, sizeof(line));
    CHECK(code == 250, "CWD into fs dir");
    code = ftp_cmd(ctl, "PWD", line, sizeof(line));
    CHECK(code == 257 && strstr(line, FSD), "PWD reflects CWD");
    code = ftp_cmd(ctl, "CDUP", line, sizeof(line));
    CHECK(code == 250, "CDUP");
    snprintf(buf, sizeof(buf), "CWD %s", FSD);
    ftp_cmd(ctl, buf, line, sizeof(line));

    code = ftp_cmd(ctl, "SIZE a.txt", line, sizeof(line));
    CHECK(code == 213 && strstr(line, "213 5"), "SIZE a.txt = 5");

    /* PASV + LIST. */
    data = ftp_pasv_data(ctl);
    CHECK(data >= 0, "PASV data connect");
    code = ftp_cmd(ctl, "LIST", line, sizeof(line));
    CHECK(code == 150, "LIST -> 150");
    {
        char listing[4096];
        ssize_t total = 0, rn;
        while ((rn = recv(data, listing + total,
                          sizeof(listing) - 1 - (size_t)total, 0)) > 0)
            total += rn;
        listing[total] = '\0';
        CHECK(strstr(listing, "a.txt") != NULL, "LIST contains a.txt");
    }
    close(data);
    code = ftp_reply(ctl, line, sizeof(line));
    CHECK(code == 226, "LIST -> 226");

    /* MLSD. */
    data = ftp_pasv_data(ctl);
    code = ftp_cmd(ctl, "MLSD", line, sizeof(line));
    {
        char listing[4096];
        ssize_t total = 0, rn;
        while ((rn = recv(data, listing + total,
                          sizeof(listing) - 1 - (size_t)total, 0)) > 0)
            total += rn;
        listing[total] = '\0';
        CHECK(code == 150 && strstr(listing, "type=file;size=5;") &&
              strstr(listing, " a.txt"),
              "MLSD facts for a.txt");
    }
    close(data);
    ftp_reply(ctl, line, sizeof(line));

    /* RETR. */
    data = ftp_pasv_data(ctl);
    code = ftp_cmd(ctl, "RETR a.txt", line, sizeof(line));
    CHECK(code == 150, "RETR -> 150");
    n = recv(data, buf, sizeof(buf) - 1, 0);
    if (n < 0)
        n = 0;
    buf[n] = '\0';
    close(data);
    code = ftp_reply(ctl, line, sizeof(line));
    CHECK(code == 226 && n == 5 && memcmp(buf, "hello", 5) == 0,
          "RETR content matches");

    /* STOR. */
    data = ftp_pasv_data(ctl);
    code = ftp_cmd(ctl, "STOR up.txt", line, sizeof(line));
    CHECK(code == 150, "STOR -> 150");
    send_all(data, "stor-bytes-123", 14);
    close(data);                           /* EOF completes the upload */
    code = ftp_reply(ctl, line, sizeof(line));
    CHECK(code == 226, "STOR -> 226");
    {
        char *c = read_file(FSD "/up.txt", NULL);
        CHECK(c && strcmp(c, "stor-bytes-123") == 0,
              "STOR file on disk");
        free(c);
    }

    /* RNFR/RNTO. */
    code = ftp_cmd(ctl, "RNFR up.txt", line, sizeof(line));
    CHECK(code == 350, "RNFR -> 350");
    code = ftp_cmd(ctl, "RNTO moved.txt", line, sizeof(line));
    CHECK(code == 250, "RNTO -> 250");
    CHECK(access(FSD "/moved.txt", F_OK) == 0, "rename happened");

    /* MKD / RMD / DELE. */
    code = ftp_cmd(ctl, "MKD newdir", line, sizeof(line));
    CHECK(code == 257, "MKD");
    code = ftp_cmd(ctl, "RMD newdir", line, sizeof(line));
    CHECK(code == 250, "RMD");
    code = ftp_cmd(ctl, "DELE moved.txt", line, sizeof(line));
    CHECK(code == 250, "DELE");

    code = ftp_cmd(ctl, "QUIT", line, sizeof(line));
    CHECK(code == 221, "QUIT -> 221");
    close(ctl);

    /* USER/PASS auth. */
    snprintf(buf, sizeof(buf),
             "{\"enabled\":true,\"port\":%d,\"user\":\"admin\","
             "\"pass\":\"s3cr3t\"}", FTP_PORT);
    http_json("POST", "/api/ftp", buf, &r);
    CHECK(r.status == 200, "FTP restart with credentials");
    http_result_free(&r);
    usleep(200 * 1000);

    ctl = tcp_connect(FTP_PORT);
    ftp_reply(ctl, line, sizeof(line));
    code = ftp_cmd(ctl, "USER admin", line, sizeof(line));
    CHECK(code == 331, "USER admin -> 331");
    code = ftp_cmd(ctl, "PASS wrong", line, sizeof(line));
    CHECK(code == 530, "wrong PASS -> 530");
    code = ftp_cmd(ctl, "USER admin", line, sizeof(line));
    CHECK(code == 331, "USER admin again -> 331");
    code = ftp_cmd(ctl, "PASS s3cr3t", line, sizeof(line));
    CHECK(code == 230, "good PASS -> 230");
    code = ftp_cmd(ctl, "PWD", line, sizeof(line));
    CHECK(code == 257, "authed PWD");
    ftp_cmd(ctl, "QUIT", line, sizeof(line));
    close(ctl);
}

static void test_notifications(void) {
    int n = stub_notif_count();
    printf("[notifications]\n");
    CHECK(n == 3, "exactly 3 startup notifications");
    CHECK(n >= 1 && strstr(stub_notif(0), "Welcome To PS5 Linux Manager"),
          "notif #1 welcome");
    CHECK(n >= 2 && strstr(stub_notif(1), "credits: InsideMatrix"),
          "notif #2 credits");
    CHECK(n >= 3 && strstr(stub_notif(2), "Using port :18090"),
          "notif #3 port");
}

static void test_appdb(void) {
    const char *cat, *vcat;
    http_result_t r;
    char marker[512];
    printf("[appdb]\n");
    snprintf(marker, sizeof(marker), "%s/app.installed", PS5LM_DATA_DIR);

    /* Garbage db: install degrades gracefully (no crash, returns 0). */
    make_fake_db();
    CHECK(appdb_install_if_needed() == 0,
          "garbage app.db handled gracefully");
    CHECK(appdb_is_installed() == 0,
          "garbage db not 'installed'");

    /* Real db: rows inserted, media category probed from YouTube. */
    make_real_db();
    CHECK(appdb_install_if_needed() == 1, "install into real app.db");
    CHECK(access(marker, F_OK) == 0, "first-run marker written");
    CHECK(db_has_row("PS5 Linux Manager", &cat, &vcat),
          "tbl_contentinfo row present");
    CHECK(strcmp(cat, "gdx") == 0 && strcmp(vcat, "gdxv") == 0,
          "media category/viewCategory probed from existing media app");

    /* Idempotent: second call is a no-op success. */
    CHECK(appdb_install_if_needed() == 1, "install runs once (marker)");

    http_json("GET", "/api/status", NULL, &r);
    CHECK(r.body && strstr(r.body, "\"appInstalled\":true"),
          "status appInstalled true after install");
    http_result_free(&r);

    /* Marker removed: db row still proves installation. */
    unlink(marker);
    CHECK(appdb_is_installed() == 1, "installed via db row (no marker)");
}

int main(void) {
    pthread_t mgr;
    http_result_t r;
    char *elf_big, *elf_ldr;
    size_t i, elf_big_n = 700001, elf_ldr_n = 5003;

    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setup_fixture();

    /* Deterministic ELF test payloads (magic + pattern). */
    elf_big = malloc(elf_big_n);
    elf_big[0] = 0x7f;
    elf_big[1] = 'E';
    elf_big[2] = 'L';
    elf_big[3] = 'F';
    for (i = 4; i < elf_big_n; i++)
        elf_big[i] = (char)((i * 7) & 0xff);
    elf_ldr = malloc(elf_ldr_n);
    elf_ldr[0] = 0x7f;
    elf_ldr[1] = 'E';
    elf_ldr[2] = 'L';
    elf_ldr[3] = 'F';
    for (i = 4; i < elf_ldr_n; i++)
        elf_ldr[i] = (char)((i * 13) & 0xff);

    pthread_create(&mgr, NULL, (void *(*)(void *))ps5lm_main, NULL);
    wait_for_server();

    test_status();
    test_notifications();
    test_linux_device_and_files();
    test_bios_and_grub();
    test_payloads_and_launch(elf_big, elf_big_n, elf_ldr, elf_ldr_n);
    test_fs_api();
    test_ftp();
    test_appdb();

    /* Graceful shutdown via the API. */
    printf("[shutdown]\n");
    if (http_json("POST", "/api/boot/orbis", "{}", &r) == 0) {
        CHECK(r.status == 200, "boot/orbis stops the manager");
        http_result_free(&r);
    } else {
        CHECK(0, "boot/orbis request");
    }
    pthread_join(mgr, NULL);
    CHECK(1, "manager thread exited cleanly");

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
