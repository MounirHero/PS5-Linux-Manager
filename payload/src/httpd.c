/*
 * httpd.c — implementation of the poll()-based HTTP server in httpd.h.
 */
#if defined(HOST_TEST) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include "httpd.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fsops.h"
#include "json.h"
#include "util.h"
#include "webui_embed.h"

/* ------------------------------------------------------------------ */
/* Client state machine                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    CS_FREE = 0,        /* slot unused                                  */
    CS_READ_HDR,        /* accumulating request headers                 */
    CS_READ_BODY,       /* accumulating request body                    */
    CS_WRITE            /* sending the response                         */
} client_state_t;

typedef struct {
    int           fd;
    client_state_t state;

    char          hdr[HTTPD_MAX_HEADER];   /* request header block      */
    size_t        hdr_len;

    httpd_request_t req;                   /* parsed at header completion */
    int           req_ready;               /* req fields are valid        */

    char         *body;                    /* malloc'd request body     */
    size_t        body_len;                /* bytes received so far     */
    size_t        body_expected;           /* Content-Length            */

    FILE         *stream;                  /* streamed upload tmp file  */
    char          stream_path[800];        /* tmp path ("" = none)      */
    int           is_upload;               /* HTTPD_UPLOAD_PATH route   */

    char         *out;                     /* malloc'd full response    */
    size_t        out_len;
    size_t        out_sent;
} client_t;

/* Single server instance: the manager runs exactly one HTTP server. */
static client_t g_clients[HTTPD_MAX_CLIENTS];

static void client_reset(client_t *c) {
    if (c->fd >= 0)
        close(c->fd);
    if (c->stream) {
        fclose(c->stream);
        c->stream = NULL;
    }
    if (c->stream_path[0]) {
        /* Leftover streamed upload the handler did not consume. */
        unlink(c->stream_path);
        c->stream_path[0] = '\0';
    }
    free(c->body);
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->state = CS_FREE;
}

static void client_close_all(void) {
    int i;
    for (i = 0; i < HTTPD_MAX_CLIENTS; i++)
        if (g_clients[i].state != CS_FREE)
            client_reset(&g_clients[i]);
}

static int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ------------------------------------------------------------------ */
/* Listen socket                                                       */
/* ------------------------------------------------------------------ */

int httpd_listen(int port) {
    int fd, one = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        util_log("httpd: socket() failed: %s", strerror(errno));
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef TCP_NODELAY
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        util_log("httpd: bind(:%d) failed: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, HTTPD_MAX_CLIENTS) != 0) {
        util_log("httpd: listen() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (set_nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/* Response helpers                                                    */
/* ------------------------------------------------------------------ */

void httpd_response_init(httpd_response_t *res) {
    memset(res, 0, sizeof(*res));
    res->status = 200;
    util_copy(res->content_type, sizeof(res->content_type),
              "application/json");
}

void httpd_respond_json(httpd_response_t *res, int status, const char *json) {
    free(res->body);
    res->body = NULL;
    res->body_len = 0;
    res->status = status;
    util_copy(res->content_type, sizeof(res->content_type),
              "application/json");
    if (!json)
        json = "{}";
    res->body = strdup(json);
    if (res->body)
        res->body_len = strlen(res->body);
    else
        res->status = 500;               /* OOM: empty 500 response */
}

void httpd_respond_error(httpd_response_t *res, int status, const char *msg) {
    jbuf_t b;
    char *json;
    jb_init(&b);
    jb_begin_obj(&b);
    jb_key(&b, "error");
    jb_str(&b, msg ? msg : "error");
    jb_end_obj(&b);
    json = jb_steal(&b);
    httpd_respond_json(res, status, json);
    free(json);
}

/* ------------------------------------------------------------------ */
/* Request parsing                                                     */
/* ------------------------------------------------------------------ */

/* Find the end of the header block; returns offset just past CRLFCRLF,
 * or 0 when the headers are not complete yet. */
static size_t find_hdr_end(const char *hdr, size_t len) {
    size_t i;
    for (i = 0; i + 3 < len; i++)
        if (hdr[i] == '\r' && hdr[i + 1] == '\n' &&
            hdr[i + 2] == '\r' && hdr[i + 3] == '\n')
            return i + 4;
    return 0;
}

/* Case-insensitive search for a header value; writes the trimmed value
 * into `out`.  Returns 0 when found. */
static int get_header(const char *hdr, const char *name, char *out,
                      size_t len) {
    size_t name_len = strlen(name);
    const char *p = hdr;

    /* Skip the request line. */
    p = strstr(p, "\r\n");
    if (!p)
        return -1;
    p += 2;
    while (*p && !(p[0] == '\r' && p[1] == '\n')) {
        if (strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            const char *v = p + name_len + 1;
            const char *e;
            size_t n;
            while (*v == ' ' || *v == '\t')
                v++;
            e = strstr(v, "\r\n");
            if (!e)
                e = v + strlen(v);
            n = (size_t)(e - v);
            if (n >= len)
                n = len - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return 0;
        }
        p = strstr(p, "\r\n");
        if (!p)
            break;
        p += 2;
    }
    return -1;
}

/*
 * Parse a complete header block into `req`; *body_expected receives the
 * Content-Length.  Returns 0 on success, -1 on a malformed request.
 */
static int parse_request(client_t *c, httpd_request_t *req) {
    char *sp1, *sp2, *q;
    char target[1600];
    char cl[32];

    memset(req, 0, sizeof(*req));

    /* Request line: METHOD SP TARGET SP VERSION CRLF */
    sp1 = memchr(c->hdr, ' ', c->hdr_len);
    if (!sp1)
        return -1;
    sp2 = memchr(sp1 + 1, ' ', c->hdr + c->hdr_len - (sp1 + 1));
    if (!sp2)
        return -1;
    if ((size_t)(sp1 - c->hdr) >= sizeof(req->method))
        return -1;
    memcpy(req->method, c->hdr, (size_t)(sp1 - c->hdr));
    req->method[sp1 - c->hdr] = '\0';

    if ((size_t)(sp2 - sp1 - 1) >= sizeof(target))
        return -1;
    memcpy(target, sp1 + 1, (size_t)(sp2 - sp1 - 1));
    target[sp2 - sp1 - 1] = '\0';

    /* Split query string; URL-decode the path portion only. */
    q = strchr(target, '?');
    if (q) {
        util_copy(req->query, sizeof(req->query), q + 1);
        *q = '\0';
    }
    util_url_decode(target, req->path, sizeof(req->path));
    if (req->path[0] != '/')
        return -1;

    /* Raw ELF uploads stream to a tmp file (cap 64 MiB); every other
     * body is a small JSON document buffered in memory (cap 1 MiB). */
    c->is_upload = strcmp(req->method, "POST") == 0 &&
                   strcmp(req->path, HTTPD_UPLOAD_PATH) == 0;

    /* Body bookkeeping. */
    c->body_expected = 0;
    if (get_header(c->hdr, "Content-Length", cl, sizeof(cl)) == 0) {
        long v = strtol(cl, NULL, 10);
        if (v > 0)
            c->body_expected = (size_t)v;
    }
    if (c->body_expected >
        (c->is_upload ? (size_t)HTTPD_MAX_UPLOAD : (size_t)HTTPD_MAX_BODY))
        return -2;                        /* 413 Payload Too Large */

    if (c->is_upload && c->body_expected) {
        /* Stream into PAYLOADS/.upload-<fd>.tmp; the API handler renames
         * it (atomic publish) or unlinks it on error. */
        fsops_ensure_data_dirs();
        snprintf(c->stream_path, sizeof(c->stream_path),
                 "%s/.upload-%d.tmp", FSOPS_PAYLOADS_DIR, c->fd);
        c->stream = fopen(c->stream_path, "wb");
        if (!c->stream) {
            c->stream_path[0] = '\0';
            return -1;
        }
    }

    /* Bytes already read past the headers count toward the body. */
    {
        size_t hdr_end = find_hdr_end(c->hdr, c->hdr_len);
        size_t avail = c->hdr_len - hdr_end;
        if (c->body_expected) {
            if (avail > c->body_expected)
                avail = c->body_expected;
            if (c->stream) {
                if (avail && fwrite(c->hdr + hdr_end, 1, avail,
                                    c->stream) != avail)
                    return -1;
                c->body_len = avail;
            } else {
                c->body = (char *)malloc(c->body_expected + 1);
                if (!c->body)
                    return -1;
                memcpy(c->body, c->hdr + hdr_end, avail);
                c->body_len = avail;
                /* The parser needs a NUL terminator even when the whole
                 * body already arrived with the headers. */
                c->body[c->body_len] = '\0';
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Static web UI route                                                 */
/* ------------------------------------------------------------------ */

/* Placeholder page served when the payload was built without a real
 * webui (the checked-in stub webui_embed.c); see webui_embed.h. */
static const char FALLBACK_PAGE[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>PS5 Linux Manager by InsideMatrix</title></head><body "
    "style=\"background:#111;color:#eee;font-family:monospace;"
    "padding:2rem\">"
    "<h1>PS5 Linux Manager by InsideMatrix</h1>"
    "<p>The web UI bundle was not embedded in this build.</p>"
    "<p>Build <code>../webui</code> first, then re-run <code>make</code> "
    "so <code>src/webui_embed.c</code> is regenerated from "
    "<code>../webui/dist</code>.</p>"
    "<p>The JSON API is live under <code>/api/</code> "
    "(try <a style=\"color:#8cf\" href=\"/api/status\">/api/status</a>).</p>"
    "</body></html>";

/*
 * Serve the embedded SPA: every non-/api GET returns index.html (gzip),
 * which implements the SPA fallback for client-side routes.  Assets are
 * expected to be inlined into index.html by the Vite single-file build.
 */
static void serve_static(const httpd_request_t *req, httpd_response_t *res) {
    httpd_response_init(res);

    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0) {
        httpd_respond_error(res, 405, "method not allowed");
        return;
    }

    if (webui_embed_available && webui_index_html_gz_len > 0) {
        res->status = 200;
        util_copy(res->content_type, sizeof(res->content_type),
                  "text/html; charset=utf-8");
        res->body = (char *)malloc(webui_index_html_gz_len);
        if (!res->body) {
            httpd_respond_error(res, 500, "out of memory");
            return;
        }
        memcpy(res->body, webui_index_html_gz, webui_index_html_gz_len);
        res->body_len = webui_index_html_gz_len;
        res->gzip = 1;
    } else {
        httpd_response_init(res);
        util_copy(res->content_type, sizeof(res->content_type),
                  "text/html; charset=utf-8");
        res->body = strdup(FALLBACK_PAGE);
        res->body_len = res->body ? strlen(res->body) : 0;
    }
}

/* ------------------------------------------------------------------ */
/* Response transmission                                               */
/* ------------------------------------------------------------------ */

static const char *status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default:  return "Status";
    }
}

/* Serialize `res` into the client's output buffer (headers + body). */
static int client_stage_response(client_t *c, const httpd_response_t *res) {
    char head[512];
    int head_len;

    head_len = snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "%s"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        res->status, status_text(res->status),
        res->content_type,
        (unsigned long)res->body_len,
        res->gzip ? "Content-Encoding: gzip\r\n" : "");
    if (head_len < 0 || (size_t)head_len >= sizeof(head))
        return -1;

    free(c->out);
    c->out_len = (size_t)head_len + res->body_len;
    c->out = (char *)malloc(c->out_len ? c->out_len : 1);
    if (!c->out) {
        c->out_len = 0;
        return -1;
    }
    memcpy(c->out, head, (size_t)head_len);
    if (res->body_len)
        memcpy(c->out + head_len, res->body, res->body_len);
    c->out_sent = 0;
    c->state = CS_WRITE;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Per-client I/O                                                      */
/* ------------------------------------------------------------------ */

/* Read available bytes; returns 1 progress, 0 peer closed, -1 error. */
static int client_read(client_t *c) {
    ssize_t n;

    if (c->state == CS_READ_HDR) {
        if (c->hdr_len >= sizeof(c->hdr) - 1)
            return -1;                     /* header too large */
        n = recv(c->fd, c->hdr + c->hdr_len,
                 sizeof(c->hdr) - 1 - c->hdr_len, 0);
        if (n > 0) {
            c->hdr_len += (size_t)n;
            c->hdr[c->hdr_len] = '\0';
            return 1;
        }
    } else if (c->state == CS_READ_BODY) {
        if (c->body_len >= c->body_expected)
            return 1;
        if (c->stream) {
            /* Streamed upload: read a chunk, append to the tmp file. */
            char chunk[64 * 1024];
            size_t want = c->body_expected - c->body_len;
            if (want > sizeof(chunk))
                want = sizeof(chunk);
            n = recv(c->fd, chunk, want, 0);
            if (n > 0) {
                if (fwrite(chunk, 1, (size_t)n, c->stream) != (size_t)n)
                    return -1;
                c->body_len += (size_t)n;
                return 1;
            }
        } else {
            n = recv(c->fd, c->body + c->body_len,
                     c->body_expected - c->body_len, 0);
            if (n > 0) {
                c->body_len += (size_t)n;
                c->body[c->body_len] = '\0';
                return 1;
            }
        }
    } else {
        return 1;
    }
    if (n == 0)
        return 0;                          /* orderly shutdown */
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return 1;                          /* no data right now */
    return -1;
}

/* Write pending response bytes; returns 1 progress, -1 error. */
static int client_write(client_t *c) {
    ssize_t n = send(c->fd, c->out + c->out_sent, c->out_len - c->out_sent,
                     0);
    if (n > 0) {
        c->out_sent += (size_t)n;
        return 1;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 1;
    return -1;
}

/*
 * Advance one client as far as its state allows.  Returns non-zero while
 * the connection should stay open; 0 when it must be closed.
 */

/* Respond to a fully-received request: API dispatch, CORS preflight, or
 * the static web UI route; then stage the bytes for transmission. */
static int client_dispatch(client_t *c, httpd_handler_fn handler,
                           void *user) {
    httpd_response_t res;
    int rc;

    httpd_response_init(&res);
    c->req.body = c->body;
    c->req.body_len = c->body_len;
    if (c->stream) {
        /* Finish the streamed upload before the handler sees it; the
         * handler renames the tmp file (consume) or we unlink it below. */
        fclose(c->stream);
        c->stream = NULL;
        c->req.stream_path = c->stream_path;
        c->req.stream_size = c->body_len;
    }
    if (strcmp(c->req.method, "OPTIONS") == 0)
        httpd_respond_json(&res, 204, "");
    else if (strncmp(c->req.path, "/api/", 5) == 0 && handler)
        handler(&c->req, &res, user);
    else
        serve_static(&c->req, &res);

    if (c->req.stream_path) {
        c->req.stream_path = NULL;
        if (c->stream_path[0] && util_file_exists(c->stream_path))
            unlink(c->stream_path);      /* not consumed: drop tmp file */
        c->stream_path[0] = '\0';
    }

    rc = client_stage_response(c, &res);
    free(res.body);
    return rc == 0;
}

/* Helper: answer a protocol-level error without a parsed request. */
static int client_error_response(client_t *c, int status,
                                 const char *msg) {
    httpd_response_t res;
    int rc;
    httpd_response_init(&res);
    httpd_respond_error(&res, status, msg);
    rc = client_stage_response(c, &res);
    free(res.body);
    return rc == 0;
}

static int client_service(client_t *c, httpd_handler_fn handler,
                          void *user) {
    if (c->state == CS_READ_HDR) {
        if (client_read(c) != 1 && c->hdr_len == 0)
            return 0;
        if (find_hdr_end(c->hdr, c->hdr_len)) {
            int rc = parse_request(c, &c->req);
            if (rc == -2)                  /* 413 */
                return client_error_response(c, 413,
                                             "request body too large");
            if (rc != 0)
                return client_error_response(c, 400, "malformed request");
            c->req_ready = 1;
            if (c->body_len < c->body_expected) {
                c->state = CS_READ_BODY;    /* wait for the rest */
            } else if (!client_dispatch(c, handler, user)) {
                return 0;                   /* OOM: drop connection */
            }
        } else if (c->hdr_len >= sizeof(c->hdr) - 1) {
            return 0;                       /* oversized header */
        }
    }

    if (c->state == CS_READ_BODY) {
        if (client_read(c) != 1 && c->body_len < c->body_expected)
            return 0;                       /* peer gave up mid-body */
        if (c->body_len >= c->body_expected) {
            if (!client_dispatch(c, handler, user))
                return 0;                   /* OOM: drop connection */
        }
    }

    if (c->state == CS_WRITE) {
        if (client_write(c) != 1)
            return 0;
        if (c->out_sent >= c->out_len)
            return 0;                       /* Connection: close */
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Main poll iteration                                                 */
/* ------------------------------------------------------------------ */

int httpd_poll(int server_fd, int timeout_ms, httpd_handler_fn handler,
               void *user) {
    struct pollfd pfds[HTTPD_MAX_CLIENTS + 1];
    int idx[HTTPD_MAX_CLIENTS + 1];         /* pollfd -> client slot     */
    int nfds = 0, i, rc;

    /* Serve listen socket readiness at slot 0 of the poll array. */
    pfds[nfds].fd = server_fd;
    pfds[nfds].events = POLLIN;
    pfds[nfds].revents = 0;
    idx[nfds] = -1;
    nfds++;

    for (i = 0; i < HTTPD_MAX_CLIENTS; i++) {
        client_t *c = &g_clients[i];
        if (c->state == CS_FREE)
            continue;
        pfds[nfds].fd = c->fd;
        pfds[nfds].events = (c->state == CS_WRITE) ? POLLOUT : POLLIN;
        pfds[nfds].revents = 0;
        idx[nfds] = i;
        nfds++;
    }

    rc = poll(pfds, (nfds_t)nfds, timeout_ms);
    if (rc < 0) {
        if (errno == EINTR)
            return 0;
        util_log("httpd: poll() failed: %s", strerror(errno));
        return -1;
    }
    if (rc == 0)
        return 0;                           /* idle tick */

    /* New connections first so fresh clients get serviced this round. */
    if (pfds[0].revents & POLLIN) {
        for (;;) {
            struct sockaddr_in sa;
            socklen_t sl = sizeof(sa);
            int cfd = accept(server_fd, (struct sockaddr *)&sa, &sl);
            int slot;
            if (cfd < 0)
                break;                      /* EAGAIN: no more pending */
            set_nonblocking(cfd);
            for (slot = 0; slot < HTTPD_MAX_CLIENTS; slot++)
                if (g_clients[slot].state == CS_FREE)
                    break;
            if (slot == HTTPD_MAX_CLIENTS) {
                close(cfd);                 /* pool exhausted */
                continue;
            }
            memset(&g_clients[slot], 0, sizeof(g_clients[slot]));
            g_clients[slot].fd = cfd;
            g_clients[slot].state = CS_READ_HDR;
        }
    }

    for (i = 1; i < nfds; i++) {
        client_t *c;
        if (idx[i] < 0)
            continue;
        c = &g_clients[idx[i]];
        if (c->state == CS_FREE)
            continue;
        if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            /* A HUP can still carry readable data; try one last service
             * before tearing down so short responses are not lost. */
            if (!(pfds[i].revents & POLLIN) || !client_service(c, handler, user)) {
                client_reset(c);
                continue;
            }
        }
        if (pfds[i].revents & (POLLIN | POLLOUT)) {
            if (!client_service(c, handler, user))
                client_reset(c);
        }
    }

    /* Defensive: never leak slots on logic errors above. */
    for (i = 0; i < HTTPD_MAX_CLIENTS; i++) {
        client_t *c = &g_clients[i];
        if (c->state == CS_WRITE && c->out_sent >= c->out_len)
            client_reset(c);
    }
    return 0;
}

/* Tear down every open connection (used when the manager exits). */
void httpd_shutdown(void) {
    client_close_all();
}
