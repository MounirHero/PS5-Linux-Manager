/*
 * util.c — implementation of the platform utilities declared in util.h.
 */

/* Feature-test macros must precede every libc include so that -std=c11
 * still exposes POSIX/BSD interfaces (getifaddrs, fileno, ...). */
#if defined(HOST_TEST) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include <sys/socket.h>         /* struct sockaddr, AF_INET (FreeBSD) */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <unistd.h>

#ifdef HOST_TEST
#include "shims/host_shims.h"   /* sceKernel*, kernel_get_fw_version stubs */
#else
#include <ps5/kernel.h>         /* kernel_get_fw_version (ps5-payload-sdk) */

/*
 * Classic Orbis libkernel calls; the SDK ships no headers for them but
 * its libkernel stub library exports them all.
 */
int sceKernelSendNotificationRequest(int device, char *buf, size_t len,
                                     int flags);
int sceKernelIsDevKit(void);
int sceKernelIsTestKit(void);
int sceKernelGetHwModelName(char *name);
int sceKernelGetHwSerialNumber(char *serial);

/*
 * libSceRegMgr.sprx registry reads (the SDK ships the stub library
 * target/lib/libSceRegMgr.so which exports all four symbols).  Linked
 * via -lSceRegMgr on target builds; every call site treats failure as
 * "not available" and falls back gracefully.
 */
int sceRegMgrGetStr(unsigned int key, char *out, size_t outlen);
int sceRegMgrGetBin(unsigned int key, void *out, size_t outlen);
#endif

/* Notifications default to on; /api/settings can flip this at runtime. */
static int g_notifications_enabled = 1;

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

void util_log(const char *fmt, ...) {
    va_list ap;
    char line[1024];
    FILE *f;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    fprintf(stderr, "[" PS5LM_NAME "] %s\n", line);

    /* Best-effort persistent log; never fatal if /data is unavailable. */
    f = fopen(PS5LM_LOG_PATH, "a");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

/* ------------------------------------------------------------------ */
/* On-console notifications                                            */
/* ------------------------------------------------------------------ */

void util_set_notifications_enabled(int enabled) {
    g_notifications_enabled = enabled ? 1 : 0;
}

int util_notify(const char *text) {
    char buf[256];
    int rc;

    if (!g_notifications_enabled || !text)
        return -1;

    /*
     * Classic Orbis notification call, the same form ps5-linux-loader
     * uses: fd 0, plain text, explicit length, flags 0.  The buffer is
     * NOT required to be NUL-terminated by the syscall, but we build it
     * with snprintf so it is safe to log too.
     */
    snprintf(buf, sizeof(buf), PS5LM_NAME "\n%s", text);
    rc = sceKernelSendNotificationRequest(0, buf, strlen(buf), 0);
    if (rc != 0)
        util_log("notify failed: 0x%x", rc);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Network identity                                                    */
/* ------------------------------------------------------------------ */

void util_get_ip(char *out, size_t len) {
    struct ifaddrs *ifa = NULL, *it;

    if (!out || len == 0)
        return;
    out[0] = '\0';
    if (getifaddrs(&ifa) != 0 || !ifa)
        return;

    for (it = ifa; it; it = it->ifa_next) {
        const struct sockaddr_in *sin;
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET)
            continue;
        sin = (const struct sockaddr_in *)it->ifa_addr;
        /* Skip loopback: the UI needs the LAN address. */
        if ((ntohl(sin->sin_addr.s_addr) >> 24) == 127)
            continue;
        if (inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)len))
            break;                          /* first usable address wins */
        out[0] = '\0';
    }
    freeifaddrs(ifa);
}

/* ------------------------------------------------------------------ */
/* Console identity                                                    */
/* ------------------------------------------------------------------ */

void util_kit_type(char *out, size_t len) {
    const char *kind = "retail";
    if (sceKernelIsDevKit())
        kind = "devkit";
    else if (sceKernelIsTestKit())
        kind = "testkit";
    util_copy(out, len, kind);
}

/* ------------------------------------------------------------------ */
/* Console identity (name + model code)                                */
/* ------------------------------------------------------------------ */

void util_console_name(char *out, size_t len) {
    if (!out || len == 0)
        return;
    out[0] = '\0';
    /* Orbis libc provides gethostname(); an empty result is normal on
     * some firmwares — fall back to a sane constant, never crash. */
    if (gethostname(out, len) != 0 || !*out)
        util_copy(out, len, "PS5");
    out[len - 1] = '\0';
}

/* Non-zero when `s` looks like a PS5 model code ("CFI-...."). */
static int looks_like_model(const char *s) {
    return s && s[0] == 'C' && s[1] == 'F' && s[2] == 'I' && s[3] == '-' &&
           s[4] != '\0';
}

void util_model_code(char *out, size_t len) {
    char buf[64];
    size_t i;

    static const struct {
        unsigned int key;
        int          is_bin;    /* 0: sceRegMgrGetStr, 1: sceRegMgrGetBin */
    } candidates[] = {
        /*
         * Candidate libSceRegMgr keys for the hardware model/serial,
         * collected from scene registry dumps (Orbis and Prospero share
         * the same key numbering for these).  Best effort: each call is
         * guarded and a miss simply advances to the next candidate.
         *   0x00060001  model name string   ("/DEVICE/PRODUCT/NAME")
         *   0x00060101  serial string       ("/DEVICE/PRODUCT/SERIAL")
         *   0x00060200  model code binary   ("/DEVICE/PRODUCT/MODEL")
         *   0x00060301  board/model binary  ("/DEVICE/INFO/MODEL")
         */
        { 0x00060001u, 0 },
        { 0x00060200u, 1 },
        { 0x00060301u, 1 },
        { 0x00060101u, 0 },
    };

    if (!out || len == 0)
        return;
    out[0] = '\0';

    /* 1. libkernel's own hardware model query (always available). */
    memset(buf, 0, sizeof(buf));
    if (sceKernelGetHwModelName(buf) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        /* The returned string is typically the model code itself
         * ("CFI-1215A"); if it is a longer marketing string, extract
         * the CFI-XXXX token when present. */
        if (looks_like_model(buf)) {
            util_copy(out, len, buf);
            return;
        }
        for (i = 0; buf[i]; i++) {
            if (looks_like_model(buf + i)) {
                char tok[16];
                size_t n = 0;
                while (buf[i + n] && buf[i + n] != ' ' && n < sizeof(tok) - 1) {
                    tok[n] = buf[i + n];
                    n++;
                }
                tok[n] = '\0';
                util_copy(out, len, tok);
                return;
            }
        }
        /* Not a CFI string but something real (e.g. devkit name): keep it. */
        if (*buf) {
            util_copy(out, len, buf);
            return;
        }
    }

    /* 2. libSceRegMgr.sprx registry candidates (documented above). */
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        int rc;
        memset(buf, 0, sizeof(buf));
        if (candidates[i].is_bin)
            rc = sceRegMgrGetBin(candidates[i].key, buf, sizeof(buf) - 1);
        else
            rc = sceRegMgrGetStr(candidates[i].key, buf, sizeof(buf) - 1);
        if (rc != 0)
            continue;
        buf[sizeof(buf) - 1] = '\0';
        if (looks_like_model(buf)) {
            util_copy(out, len, buf);
            return;
        }
    }

    /* 3. Sane fallback — never crash, never return empty. */
    util_copy(out, len, "CFI-XXXX");
}

long util_uptime_sec(void) {
    static long start = -1;              /* captured on first call */
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    if (start < 0)
        start = (long)ts.tv_sec;
    return (long)ts.tv_sec - start;
}

void util_firmware(char *out, size_t len) {
    /*
     * ps5-payload-sdk's kernel oracle (crt1.o) resolves the running
     * firmware as a packed integer, e.g. 0x04510000 -> "4.51".  The byte
     * layout is major.minor in the two most significant bytes
     * (binary-coded decimal), matching how homebrews report it.
     */
    uint32_t fw = kernel_get_fw_version();
    if (fw == 0) {
        util_copy(out, len, "unknown");
        return;
    }
    snprintf(out, len, "%x.%02x", (fw >> 24) & 0xff, (fw >> 16) & 0xff);
}

/* ------------------------------------------------------------------ */
/* Filesystem helpers                                                  */
/* ------------------------------------------------------------------ */

int util_file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int util_dir_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int util_mkdir_p(const char *path) {
    char tmp[512];
    size_t n, i;

    if (!path)
        return -1;
    n = strlen(path);
    if (n == 0 || n >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, n + 1);

    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int util_atomic_write(const char *path, const char *data, size_t len,
                      char *errbuf, size_t errlen) {
    char tmp[600];
    FILE *f;

    if (errbuf && errlen)
        errbuf[0] = '\0';
    if (!path || (!data && len)) {
        if (errbuf && errlen)
            snprintf(errbuf, errlen, "invalid write arguments");
        return -1;
    }

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) {
        if (errbuf && errlen)
            snprintf(errbuf, errlen, "cannot open %s: %s", tmp,
                     strerror(errno));
        return -1;
    }
    if (len && fwrite(data, 1, len, f) != len) {
        if (errbuf && errlen)
            snprintf(errbuf, errlen, "short write to %s", tmp);
        fclose(f);
        remove(tmp);
        return -1;
    }
    /* Flush stdio buffers before the rename so the swap is durable. */
    fflush(f);
    if (fclose(f) != 0) {
        remove(tmp);
        if (errbuf && errlen)
            snprintf(errbuf, errlen, "close failed on %s", tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        if (errbuf && errlen)
            snprintf(errbuf, errlen, "rename %s -> %s failed: %s", tmp,
                     path, strerror(errno));
        remove(tmp);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* String helpers                                                      */
/* ------------------------------------------------------------------ */

void util_url_decode(const char *in, char *out, size_t len) {
    size_t o = 0;

    if (!out || len == 0)
        return;
    if (!in) {
        out[0] = '\0';
        return;
    }
    while (*in && o + 1 < len) {
        char c = *in++;
        if (c == '%' && in[0] && in[1]) {
            int hi, lo;
            char h1 = in[0], h2 = in[1];
            hi = (h1 <= '9') ? h1 - '0' : (h1 | 32) - 'a' + 10;
            lo = (h2 <= '9') ? h2 - '0' : (h2 | 32) - 'a' + 10;
            if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
                out[o++] = (char)((hi << 4) | lo);
                in += 2;
                continue;
            }
            /* Malformed escape: emit literally (tolerant). */
        } else if (c == '+') {
            c = ' ';
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

void util_copy(char *dst, size_t len, const char *src) {
    size_t n;
    if (!dst || len == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= len)
        n = len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}
