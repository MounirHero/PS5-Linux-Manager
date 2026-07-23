/*
 * ftpsrv.c — implementation of the embedded FTP daemon declared in ftpsrv.h.
 */
#if defined(HOST_TEST) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include "ftpsrv.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fsapi.h"
#include "util.h"

#define FTP_MAX_SESSIONS 4
#define FTP_LINE_MAX     1024
#define FTP_OUT_MAX      4096
#define FTP_LIST_CAP     (512 * 1024)     /* listing buffer cap        */
#define FTP_XFER_CHUNK   (32 * 1024)      /* data connection chunk     */

/* Transfer kinds (session.xfer). */
enum { X_NONE = 0, X_LIST, X_RETR, X_STOR };

typedef struct {
    int    cfd;                   /* control connection (-1 = free)   */

    char   in[FTP_LINE_MAX];      /* command accumulation buffer      */
    size_t in_len;

    char   out[FTP_OUT_MAX];      /* pending reply bytes              */
    size_t out_len;
    size_t out_off;

    int    authed;                /* login completed                  */
    char   cwd[FSAPI_PATH_MAX];

    char   rnfr[FSAPI_PATH_MAX];  /* RNFR source awaiting RNTO        */
    int    has_rnfr;

    int    pfd;                   /* passive listen socket (-1 none)  */
    int    dfd;                   /* data connection (-1 none)        */
    int    xfer;                  /* X_* transfer kind                */
    int    xfer_accept;           /* waiting for the data connect     */
    FILE  *fp;                    /* RETR/STOR file                   */
    char  *listbuf;               /* LIST/MLSD/NLST payload           */
    size_t list_len;
    size_t list_off;
} ftp_session_t;

static ftp_config_t  g_cfg = { 0, 2121, "", "" };
static int           g_listen = -1;
static ftp_session_t g_sessions[FTP_MAX_SESSIONS];

/* ------------------------------------------------------------------ */
/* Low-level helpers                                                   */
/* ------------------------------------------------------------------ */

static int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void xfer_reset(ftp_session_t *s) {
    if (s->pfd >= 0)
        close(s->pfd);
    if (s->dfd >= 0)
        close(s->dfd);
    if (s->fp)
        fclose(s->fp);
    free(s->listbuf);
    s->pfd = -1;
    s->dfd = -1;
    s->fp = NULL;
    s->listbuf = NULL;
    s->list_len = 0;
    s->list_off = 0;
    s->xfer = X_NONE;
    s->xfer_accept = 0;
}

static void session_reset(ftp_session_t *s) {
    if (s->cfd >= 0)
        close(s->cfd);
    xfer_reset(s);
    memset(s, 0, sizeof(*s));
    s->cfd = -1;
    s->pfd = -1;
    s->dfd = -1;
}

static void ftp_close_all(void) {
    int i;
    for (i = 0; i < FTP_MAX_SESSIONS; i++)
        if (g_sessions[i].cfd >= 0)
            session_reset(&g_sessions[i]);
    if (g_listen >= 0) {
        close(g_listen);
        g_listen = -1;
    }
}

/* Queue a reply line (printf-less on purpose: small fixed buffer). */
static void reply(ftp_session_t *s, const char *line) {
    size_t n = strlen(line);
    if (s->out_len + n + 2 >= sizeof(s->out))
        return;                          /* drop rather than corrupt   */
    memcpy(s->out + s->out_len, line, n);
    s->out_len += n;
    s->out[s->out_len++] = '\r';
    s->out[s->out_len++] = '\n';
}

static void replyf(ftp_session_t *s, int code, const char *fmt,
                   const char *arg) {
    char line[FSAPI_PATH_MAX + 128];
    if (arg)
        snprintf(line, sizeof(line), fmt, code, arg);
    else
        snprintf(line, sizeof(line), fmt, code);
    reply(s, line);
}

/* Resolve an FTP path argument against the session cwd; the result is a
 * normalized absolute path (fsapi_normalize resolves . and ..). */
static int resolve(ftp_session_t *s, const char *arg, char *out,
                   size_t len) {
    char tmp[FSAPI_PATH_MAX];
    if (!arg || !*arg)
        arg = s->cwd;
    if (arg[0] == '/')
        util_copy(tmp, sizeof(tmp), arg);
    else if (strcmp(s->cwd, "/") == 0)
        snprintf(tmp, sizeof(tmp), "/%s", arg);
    else
        snprintf(tmp, sizeof(tmp), "%s/%s", s->cwd, arg);
    return fsapi_normalize(tmp, out, len);
}

/* ------------------------------------------------------------------ */
/* Listing builders (LIST / NLST / MLSD)                               */
/* ------------------------------------------------------------------ */

static void mlsd_time(long mtime, char *out, size_t len) {
    struct tm tmv;
    time_t t = (time_t)mtime;
    if (localtime_r(&t, &tmv))
        snprintf(out, len, "%04d%02d%02d%02d%02d%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    else
        util_copy(out, len, "19700101000000");
}

/* Append one LIST ("ls -l"-style) line for `name` at `dir`. */
static void list_line_long(const char *dir, const char *name, char *out,
                           size_t len) {
    char path[FSAPI_PATH_MAX];
    char tbuf[32];
    struct stat st;
    struct tm tmv;
    char perms[11] = "----------";
    time_t t;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (stat(path, &st) != 0) {
        snprintf(out, len, "?--------- 1 ps5 ps5 0 Jan 01 1970 %s", name);
        return;
    }
    perms[0] = S_ISDIR(st.st_mode) ? 'd' : '-';
    if (st.st_mode & S_IRUSR) perms[1] = 'r';
    if (st.st_mode & S_IWUSR) perms[2] = 'w';
    if (st.st_mode & S_IXUSR) perms[3] = 'x';
    if (st.st_mode & S_IRGRP) perms[4] = 'r';
    if (st.st_mode & S_IWGRP) perms[5] = 'w';
    if (st.st_mode & S_IXGRP) perms[6] = 'x';
    if (st.st_mode & S_IROTH) perms[7] = 'r';
    if (st.st_mode & S_IWOTH) perms[8] = 'w';
    if (st.st_mode & S_IXOTH) perms[9] = 'x';
    t = (time_t)st.st_mtime;
    if (!localtime_r(&t, &tmv) ||
        !strftime(tbuf, sizeof(tbuf), "%b %d %H:%M", &tmv))
        util_copy(tbuf, sizeof(tbuf), "Jan 01 00:00");
    snprintf(out, len, "%s 1 ps5 ps5 %lld %s %s", perms,
             (long long)st.st_size, tbuf, name);
}

/* Append one MLSD fact line for `name` at `dir`. */
static void list_line_mlsd(const char *dir, const char *name, char *out,
                           size_t len) {
    char path[FSAPI_PATH_MAX];
    char tbuf[32];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (stat(path, &st) != 0) {
        snprintf(out, len, "type=file;size=0;modify=19700101000000; %s",
                 name);
        return;
    }
    mlsd_time((long)st.st_mtime, tbuf, sizeof(tbuf));
    snprintf(out, len, "type=%s;size=%lld;modify=%s; %s",
             S_ISDIR(st.st_mode) ? "dir" : "file",
             S_ISDIR(st.st_mode) ? 0LL : (long long)st.st_size,
             tbuf, name);
}

/*
 * Build a full listing for `dir` into a malloc'd buffer.
 * mode: 0 = LIST (long), 1 = NLST (names only), 2 = MLSD (facts).
 */
static char *build_listing(const char *dir, int mode, size_t *out_len) {
    DIR *d;
    struct dirent *de;
    char *buf;
    size_t cap = 64 * 1024, len = 0;

    buf = (char *)malloc(cap);
    if (!buf)
        return NULL;
    d = opendir(dir);
    if (!d) {
        free(buf);
        return NULL;
    }
    while ((de = readdir(d)) != NULL) {
        char line[FSAPI_PATH_MAX + 128];
        size_t n;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (mode == 1)
            snprintf(line, sizeof(line), "%s", de->d_name);
        else if (mode == 2)
            list_line_mlsd(dir, de->d_name, line, sizeof(line));
        else
            list_line_long(dir, de->d_name, line, sizeof(line));
        n = strlen(line);
        if (len + n + 3 >= cap) {
            char *nb;
            if (cap >= FTP_LIST_CAP)
                break;                     /* cap: truncate huge dirs */
            cap *= 2;
            if (cap > FTP_LIST_CAP)
                cap = FTP_LIST_CAP;
            nb = (char *)realloc(buf, cap);
            if (!nb)
                break;
            buf = nb;
        }
        memcpy(buf + len, line, n);
        len += n;
        buf[len++] = '\r';
        buf[len++] = '\n';
    }
    closedir(d);
    *out_len = len;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Passive data connection                                             */
/* ------------------------------------------------------------------ */

/* Open a passive listen socket on an ephemeral port; replies with the
 * PASV/EPSV response.  Returns 0 on success. */
static int open_passive(ftp_session_t *s, int epsv) {
    struct sockaddr_in addr, ctl;
    socklen_t alen;
    int one = 1;

    if (s->pfd >= 0) {
        close(s->pfd);
        s->pfd = -1;
    }
    s->pfd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->pfd < 0)
        goto fail;
    setsockopt(s->pfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;                     /* ephemeral */
    if (bind(s->pfd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        goto fail;
    if (listen(s->pfd, 1) != 0)
        goto fail;
    set_nonblocking(s->pfd);

    alen = sizeof(addr);
    if (getsockname(s->pfd, (struct sockaddr *)&addr, &alen) != 0)
        goto fail;

    if (epsv) {
        char line[64];
        snprintf(line, sizeof(line), "229 Entering Extended Passive "
                 "Mode (|||%u|)", (unsigned)ntohs(addr.sin_port));
        reply(s, line);
    } else {
        char line[80];
        unsigned ip;
        unsigned short port = ntohs(addr.sin_port);
        alen = sizeof(ctl);
        if (getsockname(s->cfd, (struct sockaddr *)&ctl, &alen) != 0)
            ctl.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ip = ntohl(ctl.sin_addr.s_addr);
        snprintf(line, sizeof(line),
                 "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
                 (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff,
                 ip & 0xff, port >> 8, port & 0xff);
        reply(s, line);
    }
    return 0;

fail:
    if (s->pfd >= 0) {
        close(s->pfd);
        s->pfd = -1;
    }
    replyf(s, 425, "%d Cannot open passive connection", NULL);
    return -1;
}

/* Try to accept the pending data connection (non-blocking). */
static void xfer_try_accept(ftp_session_t *s) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int dfd;

    if (!s->xfer_accept || s->pfd < 0)
        return;
    dfd = accept(s->pfd, (struct sockaddr *)&sa, &sl);
    if (dfd < 0)
        return;                            /* not connected yet */
    set_nonblocking(dfd);
    close(s->pfd);
    s->pfd = -1;
    s->dfd = dfd;
    s->xfer_accept = 0;
}

/* Start a listing or file transfer after the 150 reply. */
static void xfer_begin(ftp_session_t *s, int kind, FILE *fp,
                       char *listbuf, size_t list_len) {
    s->xfer = kind;
    s->fp = fp;
    s->listbuf = listbuf;
    s->list_len = list_len;
    s->list_off = 0;
    s->xfer_accept = 1;
    replyf(s, 150, "%d Opening data connection", NULL);
    xfer_try_accept(s);
}

/* Finish a transfer: close data side and emit the final reply. */
static void xfer_done(ftp_session_t *s, int ok) {
    if (s->dfd >= 0) {
        close(s->dfd);
        s->dfd = -1;
    }
    if (s->fp) {
        fclose(s->fp);
        s->fp = NULL;
    }
    free(s->listbuf);
    s->listbuf = NULL;
    s->list_len = 0;
    s->list_off = 0;
    s->xfer = X_NONE;
    s->xfer_accept = 0;
    replyf(s, ok ? 226 : 426,
           ok ? "%d Transfer complete" : "%d Transfer aborted", NULL);
}

/* Service an outgoing transfer (LIST/RETR): send what we can. */
static void xfer_service_out(ftp_session_t *s) {
    char chunk[FTP_XFER_CHUNK];

    for (;;) {
        const char *data;
        size_t avail = 0;
        ssize_t w;

        if (s->xfer == X_LIST) {
            if (s->list_off >= s->list_len) {
                xfer_done(s, 1);
                return;
            }
            data = s->listbuf + s->list_off;
            avail = s->list_len - s->list_off;
        } else if (s->xfer == X_RETR) {
            avail = fread(chunk, 1, sizeof(chunk), s->fp);
            if (avail == 0) {
                xfer_done(s, ferror(s->fp) ? 0 : 1);
                return;
            }
            data = chunk;
        } else {
            return;
        }

        w = send(s->dfd, data, avail, 0);
        if (w > 0) {
            if (s->xfer == X_LIST)
                s->list_off += (size_t)w;
            else {
                /* partial file send: rewind the unsent tail */
                if ((size_t)w < avail)
                    fseek(s->fp, -(long)(avail - (size_t)w), SEEK_CUR);
            }
            if (s->xfer == X_RETR && (size_t)w < avail)
                return;                    /* socket back-pressured */
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;                        /* wait for POLLOUT */
        xfer_done(s, 0);                   /* peer vanished */
        return;
    }
}

/* Service an incoming transfer (STOR): drain what arrived. */
static void xfer_service_in(ftp_session_t *s) {
    char chunk[FTP_XFER_CHUNK];

    for (;;) {
        ssize_t r = recv(s->dfd, chunk, sizeof(chunk), 0);
        if (r > 0) {
            if (fwrite(chunk, 1, (size_t)r, s->fp) != (size_t)r) {
                xfer_done(s, 0);
                return;
            }
            continue;
        }
        if (r == 0) {
            xfer_done(s, 1);               /* client sent EOF: done */
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        xfer_done(s, 0);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Command dispatch                                                    */
/* ------------------------------------------------------------------ */

static int auth_required(void) {
    return g_cfg.user[0] != '\0';
}

static void cmd_user(ftp_session_t *s, const char *arg) {
    if (!auth_required()) {
        s->authed = 1;
        replyf(s, 230, "%d Anonymous login ok", NULL);
        return;
    }
    if (strcmp(arg, g_cfg.user) == 0) {
        if (g_cfg.pass[0] == '\0') {
            s->authed = 1;
            replyf(s, 230, "%d Login ok", NULL);
        } else {
            replyf(s, 331, "%d Password required", NULL);
        }
        return;
    }
    /* Anonymous is also accepted when a user is configured?  No: the
     * configured credentials gate access. */
    replyf(s, 331, "%d Password required", NULL);  /* probe PASS anyway */
    s->authed = -1;                        /* mark: must fail on PASS */
}

static void cmd_pass(ftp_session_t *s, const char *arg) {
    if (!auth_required()) {
        s->authed = 1;
        replyf(s, 230, "%d Login ok", NULL);
        return;
    }
    if (s->authed == 1) {
        replyf(s, 230, "%d Already logged in", NULL);
        return;
    }
    if (s->authed != -1 && g_cfg.pass[0] != '\0' &&
        strcmp(arg, g_cfg.pass) == 0) {
        s->authed = 1;
        replyf(s, 230, "%d Login ok", NULL);
        return;
    }
    s->authed = 0;
    replyf(s, 530, "%d Login incorrect", NULL);
}

static void cmd_pwd(ftp_session_t *s) {
    char line[FSAPI_PATH_MAX + 16];
    snprintf(line, sizeof(line), "257 \"%s\" is the current directory",
             s->cwd);
    reply(s, line);
}

static void cmd_cwd(ftp_session_t *s, const char *arg) {
    char path[FSAPI_PATH_MAX];
    struct stat st;
    if (resolve(s, arg, path, sizeof(path)) != 0 ||
        stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        replyf(s, 550, "%d No such directory", NULL);
        return;
    }
    util_copy(s->cwd, sizeof(s->cwd), path);
    replyf(s, 250, "%d Directory changed", NULL);
}

static void cmd_cdup(ftp_session_t *s) {
    char path[FSAPI_PATH_MAX];
    util_copy(path, sizeof(path), s->cwd);
    if (strcmp(path, "/") != 0) {
        char *slash = strrchr(path, '/');
        if (slash == path)
            path[1] = '\0';
        else if (slash)
            *slash = '\0';
    }
    util_copy(s->cwd, sizeof(s->cwd), path);
    replyf(s, 250, "%d Directory changed", NULL);
}

static void cmd_size(ftp_session_t *s, const char *arg) {
    char path[FSAPI_PATH_MAX];
    struct stat st;
    char line[64];
    if (resolve(s, arg, path, sizeof(path)) != 0 ||
        stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        replyf(s, 550, "%d No such file", NULL);
        return;
    }
    snprintf(line, sizeof(line), "213 %lld", (long long)st.st_size);
    reply(s, line);
}

static void cmd_mlst(ftp_session_t *s, const char *arg) {
    char path[FSAPI_PATH_MAX];
    char facts[FSAPI_PATH_MAX + 128];
    char line[FSAPI_PATH_MAX + 160];
    char tbuf[32];
    struct stat st;
    const char *base;

    if (resolve(s, arg, path, sizeof(path)) != 0 || stat(path, &st) != 0) {
        replyf(s, 550, "%d No such path", NULL);
        return;
    }
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    mlsd_time((long)st.st_mtime, tbuf, sizeof(tbuf));
    snprintf(facts, sizeof(facts), "type=%s;size=%lld;modify=%s; %s",
             S_ISDIR(st.st_mode) ? "dir" : "file",
             S_ISDIR(st.st_mode) ? 0LL : (long long)st.st_size,
             tbuf, *base ? base : "/");
    snprintf(line, sizeof(line), "250- Listing %s\r\n %s\r\n250 End",
             path, facts);
    reply(s, line);
}

static void cmd_list_like(ftp_session_t *s, const char *arg, int mode) {
    char path[FSAPI_PATH_MAX];
    char *buf;
    size_t len = 0;
    struct stat st;

    if (s->pfd < 0) {
        replyf(s, 425, "%d Use PASV or EPSV first", NULL);
        return;
    }
    if (resolve(s, arg && *arg ? arg : NULL, path, sizeof(path)) != 0 ||
        stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        replyf(s, 550, "%d No such directory", NULL);
        return;
    }
    buf = build_listing(path, mode, &len);
    if (!buf) {
        replyf(s, 550, "%d Cannot list directory", NULL);
        return;
    }
    xfer_begin(s, X_LIST, NULL, buf, len);
}

static void cmd_retr(ftp_session_t *s, const char *arg) {
    char path[FSAPI_PATH_MAX];
    FILE *fp;

    if (s->pfd < 0) {
        replyf(s, 425, "%d Use PASV or EPSV first", NULL);
        return;
    }
    if (resolve(s, arg, path, sizeof(path)) != 0 ||
        (fp = fopen(path, "rb")) == NULL) {
        replyf(s, 550, "%d No such file", NULL);
        return;
    }
    xfer_begin(s, X_RETR, fp, NULL, 0);
}

static void cmd_stor(ftp_session_t *s, const char *arg) {
    char path[FSAPI_PATH_MAX];
    FILE *fp;

    if (s->pfd < 0) {
        replyf(s, 425, "%d Use PASV or EPSV first", NULL);
        return;
    }
    if (resolve(s, arg, path, sizeof(path)) != 0 ||
        (fp = fopen(path, "wb")) == NULL) {
        replyf(s, 550, "%d Cannot create file", NULL);
        return;
    }
    xfer_begin(s, X_STOR, fp, NULL, 0);
}

static void cmd_dele(ftp_session_t *s, const char *arg) {
    char path[FSAPI_PATH_MAX];
    struct stat st;
    if (resolve(s, arg, path, sizeof(path)) != 0 ||
        stat(path, &st) != 0 || S_ISDIR(st.st_mode) ||
        unlink(path) != 0) {
        replyf(s, 550, "%d Delete failed", NULL);
        return;
    }
    replyf(s, 250, "%d Deleted", NULL);
}

static void cmd_rmd(ftp_session_t *s, const char *arg) {
    char path[FSAPI_PATH_MAX];
    if (resolve(s, arg, path, sizeof(path)) != 0 || rmdir(path) != 0) {
        replyf(s, 550, "%d Remove directory failed", NULL);
        return;
    }
    replyf(s, 250, "%d Directory removed", NULL);
}

static void cmd_mkd(ftp_session_t *s, const char *arg) {
    char path[FSAPI_PATH_MAX];
    if (resolve(s, arg, path, sizeof(path)) != 0 ||
        util_mkdir_p(path) != 0) {
        replyf(s, 550, "%d Create directory failed", NULL);
        return;
    }
    replyf(s, 257, "%d \"%s\" directory created", path);
}

static void cmd_rnfr(ftp_session_t *s, const char *arg) {
    char path[FSAPI_PATH_MAX];
    struct stat st;
    if (resolve(s, arg, path, sizeof(path)) != 0 ||
        stat(path, &st) != 0) {
        replyf(s, 550, "%d No such path", NULL);
        s->has_rnfr = 0;
        return;
    }
    util_copy(s->rnfr, sizeof(s->rnfr), path);
    s->has_rnfr = 1;
    replyf(s, 350, "%d Ready for RNTO", NULL);
}

static void cmd_rnto(ftp_session_t *s, const char *arg) {
    char path[FSAPI_PATH_MAX];
    if (!s->has_rnfr) {
        replyf(s, 503, "%d RNFR required first", NULL);
        return;
    }
    s->has_rnfr = 0;
    if (resolve(s, arg, path, sizeof(path)) != 0 ||
        rename(s->rnfr, path) != 0) {
        replyf(s, 550, "%d Rename failed", NULL);
        return;
    }
    replyf(s, 250, "%d Renamed", NULL);
}

/* Dispatch one command line (CRLF already stripped). */
static void ftp_command(ftp_session_t *s, char *line) {
    char *sp = strchr(line, ' ');
    char cmd[16];
    const char *arg = "";
    size_t i, n;

    if (sp) {
        arg = sp + 1;
        while (*arg == ' ')
            arg++;
        *sp = '\0';
    }
    n = strlen(line);
    if (n >= sizeof(cmd))
        n = sizeof(cmd) - 1;
    for (i = 0; i < n; i++)
        cmd[i] = (char)toupper((unsigned char)line[i]);
    cmd[n] = '\0';

    if (strcmp(cmd, "USER") == 0)
        cmd_user(s, arg);
    else if (strcmp(cmd, "PASS") == 0)
        cmd_pass(s, arg);
    else if (strcmp(cmd, "QUIT") == 0) {
        replyf(s, 221, "%d Goodbye", NULL);
        /* Flush happens in the poll loop; mark for close after drain by
         * forcing the session closed once output is written. */
        s->rnfr[0] = '\0';
        s->has_rnfr = -1;                  /* close-after-drain marker */
    }
    else if (strcmp(cmd, "NOOP") == 0)
        replyf(s, 200, "%d OK", NULL);
    else if (strcmp(cmd, "SYST") == 0)
        reply(s, "215 UNIX Type: L8");
    else if (strcmp(cmd, "TYPE") == 0)
        replyf(s, 200, "%d Type set", NULL);
    else if (auth_required() && s->authed != 1)
        replyf(s, 530, "%d Please login with USER and PASS", NULL);
    else if (strcmp(cmd, "PWD") == 0 || strcmp(cmd, "XPWD") == 0)
        cmd_pwd(s);
    else if (strcmp(cmd, "CWD") == 0)
        cmd_cwd(s, arg);
    else if (strcmp(cmd, "CDUP") == 0)
        cmd_cdup(s);
    else if (strcmp(cmd, "SIZE") == 0)
        cmd_size(s, arg);
    else if (strcmp(cmd, "MLST") == 0)
        cmd_mlst(s, arg);
    else if (strcmp(cmd, "MLSD") == 0)
        cmd_list_like(s, arg, 2);
    else if (strcmp(cmd, "LIST") == 0)
        cmd_list_like(s, arg, 0);
    else if (strcmp(cmd, "NLST") == 0)
        cmd_list_like(s, arg, 1);
    else if (strcmp(cmd, "PASV") == 0)
        open_passive(s, 0);
    else if (strcmp(cmd, "EPSV") == 0)
        open_passive(s, 1);
    else if (strcmp(cmd, "RETR") == 0)
        cmd_retr(s, arg);
    else if (strcmp(cmd, "STOR") == 0)
        cmd_stor(s, arg);
    else if (strcmp(cmd, "DELE") == 0)
        cmd_dele(s, arg);
    else if (strcmp(cmd, "RMD") == 0)
        cmd_rmd(s, arg);
    else if (strcmp(cmd, "MKD") == 0)
        cmd_mkd(s, arg);
    else if (strcmp(cmd, "RNFR") == 0)
        cmd_rnfr(s, arg);
    else if (strcmp(cmd, "RNTO") == 0)
        cmd_rnto(s, arg);
    else
        replyf(s, 502, "%d Command not implemented", NULL);
}

/* ------------------------------------------------------------------ */
/* Poll machinery                                                      */
/* ------------------------------------------------------------------ */

void ftp_configure(const ftp_config_t *cfg) {
    ftp_close_all();
    if (cfg)
        g_cfg = *cfg;
    if (!g_cfg.enabled)
        return;
    if (g_cfg.port <= 0 || g_cfg.port > 65535)
        g_cfg.port = 2121;

    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) {
        util_log("ftpsrv: socket() failed: %s", strerror(errno));
        return;
    }
    {
        int one = 1;
        struct sockaddr_in addr;
        setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((uint16_t)g_cfg.port);
        if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(g_listen, FTP_MAX_SESSIONS) != 0) {
            util_log("ftpsrv: cannot listen on :%d: %s", g_cfg.port,
                     strerror(errno));
            close(g_listen);
            g_listen = -1;
            return;
        }
    }
    set_nonblocking(g_listen);
    util_log("ftpsrv: listening on :%d (%s)", g_cfg.port,
             g_cfg.user[0] ? "auth required" : "anonymous");
}

void ftp_get_config(ftp_config_t *cfg) {
    if (cfg)
        *cfg = g_cfg;
}

void ftp_shutdown(void) {
    ftp_close_all();
}

static void accept_clients(void) {
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int cfd, i;
        ftp_session_t *s = NULL;

        cfd = accept(g_listen, (struct sockaddr *)&sa, &sl);
        if (cfd < 0)
            return;
        set_nonblocking(cfd);
        for (i = 0; i < FTP_MAX_SESSIONS; i++)
            if (g_sessions[i].cfd < 0) {
                s = &g_sessions[i];
                break;
            }
        if (!s) {
            close(cfd);                    /* pool exhausted */
            continue;
        }
        memset(s, 0, sizeof(*s));
        s->cfd = cfd;
        s->pfd = -1;
        s->dfd = -1;
        util_copy(s->cwd, sizeof(s->cwd), "/");
        replyf(s, 220, "%d PS5 Linux Manager FTP ready", NULL);
    }
}

/* Drain pending output / read commands for one session. */
static void session_service_ctl(ftp_session_t *s, short revents) {
    /* Pending replies first. */
    if (s->out_off < s->out_len && (revents & POLLOUT)) {
        ssize_t w = send(s->cfd, s->out + s->out_off,
                         s->out_len - s->out_off, 0);
        if (w > 0)
            s->out_off += (size_t)w;
        if (s->out_off >= s->out_len) {
            s->out_len = 0;
            s->out_off = 0;
        }
    }

    if (revents & POLLIN) {
        ssize_t r = recv(s->cfd, s->in + s->in_len,
                         sizeof(s->in) - 1 - s->in_len, 0);
        if (r == 0) {
            session_reset(s);
            return;
        }
        if (r < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                session_reset(s);
            return;
        }
        s->in_len += (size_t)r;
        s->in[s->in_len] = '\0';

        /* Process every complete line. */
        for (;;) {
            char *nl = memchr(s->in, '\n', s->in_len);
            size_t linelen;
            if (!nl)
                break;
            linelen = (size_t)(nl - s->in);
            *nl = '\0';
            if (linelen && s->in[linelen - 1] == '\r')
                s->in[linelen - 1] = '\0';
            ftp_command(s, s->in);
            memmove(s->in, nl + 1, s->in_len - linelen - 1);
            s->in_len -= linelen + 1;
        }
        if (s->in_len >= sizeof(s->in) - 1) {
            replyf(s, 500, "%d Line too long", NULL);
            s->in_len = 0;
        }
    }

    /* QUIT marker: close once the reply drained. */
    if (s->has_rnfr == -1 && s->out_len == 0)
        session_reset(s);
}

int ftp_poll(int timeout_ms) {
    struct pollfd pfds[1 + FTP_MAX_SESSIONS * 3];
    int   kind[1 + FTP_MAX_SESSIONS * 3];   /* 0 listen, 1 ctl, 2 pasv, 3 data */
    int   slot[1 + FTP_MAX_SESSIONS * 3];
    int   nfds = 0, i, rc;

    if (g_listen < 0)
        return 0;

    kind[nfds] = 0; slot[nfds] = -1;
    pfds[nfds].fd = g_listen;
    pfds[nfds].events = POLLIN;
    pfds[nfds].revents = 0;
    nfds++;

    for (i = 0; i < FTP_MAX_SESSIONS; i++) {
        ftp_session_t *s = &g_sessions[i];
        short ev;
        if (s->cfd < 0)
            continue;
        ev = POLLIN;
        if (s->out_len > s->out_off)
            ev |= POLLOUT;
        kind[nfds] = 1; slot[nfds] = i;
        pfds[nfds].fd = s->cfd;
        pfds[nfds].events = ev;
        pfds[nfds].revents = 0;
        nfds++;
        if (s->pfd >= 0) {
            kind[nfds] = 2; slot[nfds] = i;
            pfds[nfds].fd = s->pfd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (s->dfd >= 0) {
            kind[nfds] = 3; slot[nfds] = i;
            pfds[nfds].fd = s->dfd;
            pfds[nfds].events = (s->xfer == X_STOR) ? POLLIN : POLLOUT;
            pfds[nfds].revents = 0;
            nfds++;
        }
    }

    rc = poll(pfds, (nfds_t)nfds, timeout_ms);
    if (rc <= 0)
        return rc < 0 && errno != EINTR ? -1 : 0;

    for (i = 0; i < nfds; i++) {
        ftp_session_t *s;
        if (!pfds[i].revents)
            continue;
        if (kind[i] == 0) {
            accept_clients();
            continue;
        }
        s = &g_sessions[slot[i]];
        if (s->cfd < 0)
            continue;
        if (kind[i] == 1) {
            session_service_ctl(s, pfds[i].revents);
        } else if (kind[i] == 2) {
            xfer_try_accept(s);
            if (s->dfd >= 0)
                xfer_service_out(s);       /* kick LIST/RETR immediately */
        } else {
            if (s->xfer == X_STOR)
                xfer_service_in(s);
            else
                xfer_service_out(s);
        }
    }
    return 0;
}
