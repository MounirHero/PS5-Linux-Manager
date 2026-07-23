/*
 * launch.c — serve ELF payloads to the console's ELF loader (port 9021).
 */
#if defined(HOST_TEST) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include "launch.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "util.h"

/* Pathological path length guard for launch arguments. */
#define LAUNCH_PATH_MAX 768

static void set_err(char *err, size_t errlen, const char *msg) {
    if (err && errlen)
        util_copy(err, errlen, msg);
}

/* Connect to 127.0.0.1:`port`; returns the fd or -1. */
static int connect_loopback(int port) {
    int fd;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Stream `fd_file` to `fd_sock` in 64 KiB chunks; 0 ok / -1 error. */
static int stream_file(FILE *f, int sock, long long *sent) {
    char buf[64 * 1024];
    size_t n;

    if (sent)
        *sent = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = send(sock, buf + off, n - off, 0);
            if (w <= 0) {
                if (w < 0 && errno == EINTR)
                    continue;
                return -1;
            }
            off += (size_t)w;
        }
        if (sent)
            *sent += (long long)n;
    }
    return ferror(f) ? -1 : 0;
}

int serve_elf_9021(const char *path, long long *sent, char *err,
                   size_t errlen) {
    struct stat st;
    size_t n;
    FILE *f;
    int sock;

    if (sent)
        *sent = 0;
    if (!path || !*path) {
        set_err(err, errlen, "empty payload path");
        return -1;
    }
    n = strlen(path);
    if (n >= LAUNCH_PATH_MAX) {
        set_err(err, errlen, "payload path too long");
        return -1;
    }
    if (path[0] != '/' || strstr(path, "..")) {
        set_err(err, errlen, "payload path must be absolute");
        return -1;
    }
    if (n < 4 || strcmp(path + n - 4, ".elf") != 0) {
        set_err(err, errlen, "payload must be a .elf file");
        return -1;
    }
    if (stat(path, &st) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "payload not found: %s", path);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        set_err(err, errlen, "payload is not a regular file");
        return -1;
    }

    sock = connect_loopback(PS5LM_ELF_LOADER_PORT);
    if (sock < 0) {
        if (err && errlen)
            snprintf(err, errlen,
                     "cannot connect to the ELF loader on 127.0.0.1:%d "
                     "(is elfldr running?)", PS5LM_ELF_LOADER_PORT);
        return -1;
    }

    f = fopen(path, "rb");
    if (!f) {
        close(sock);
        if (err && errlen)
            snprintf(err, errlen, "cannot open %s", path);
        return -1;
    }

    util_log("serving %s to 127.0.0.1:%d", path, PS5LM_ELF_LOADER_PORT);
    if (stream_file(f, sock, sent) != 0) {
        fclose(f);
        close(sock);
        if (err && errlen)
            snprintf(err, errlen, "failed to send %s: %s", path,
                     strerror(errno));
        return -1;
    }
    fclose(f);
    /* Shutdown the write side so the loader sees a clean EOF, then close. */
    shutdown(sock, SHUT_WR);
    close(sock);

    util_log("served %lld bytes of %s on port %d",
             sent ? *sent : 0, path, PS5LM_ELF_LOADER_PORT);
    return 0;
}
