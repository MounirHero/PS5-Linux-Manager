/*
 * httpd.h — tiny poll()-based HTTP/1.1 server, zero external dependencies.
 *
 * Single-threaded, non-blocking: one poll() loop drives the listen socket
 * and up to HTTPD_MAX_CLIENTS concurrent connections through
 * read-headers -> read-body -> dispatch -> write-response.
 *
 * Features (per SPEC):
 *   - GET and POST with JSON request bodies up to 1 MiB
 *   - static route: GET / serves the embedded gzipped web UI, with SPA
 *     fallback (any non-/api GET returns index.html so client-side routes
 *     like /bios survive a refresh)
 *   - paths under /api/... are forwarded to a handler callback (api.c)
 *   - connections are closed after each response ("Connection: close");
 *     keep-alive is optional per HTTP/1.1 and deliberately left out to
 *     keep the state machine minimal
 *
 * The server is driven by repeatedly calling httpd_poll() from main();
 * between iterations main() can do its own housekeeping (auto-boot timer).
 */
#ifndef PS5LM_HTTPD_H
#define PS5LM_HTTPD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTPD_MAX_CLIENTS   16
#define HTTPD_MAX_HEADER    (16 * 1024)        /* request header cap   */
#define HTTPD_MAX_BODY      (1 * 1024 * 1024)  /* JSON body cap: 1 MiB */
#define HTTPD_MAX_UPLOAD    (64 * 1024 * 1024) /* ELF upload cap: 64 MiB */

/*
 * Path whose POST body is streamed straight to a temporary file instead
 * of being buffered in memory (payload uploads).  The handler finds the
 * temporary path in httpd_request_t.stream_path.
 */
#define HTTPD_UPLOAD_PATH   "/api/payloads/upload"

/* Parsed request handed to the API handler. */
typedef struct {
    char        method[8];      /* "GET" / "POST" / "DELETE"             */
    char        path[512];      /* URL-decoded path, without query       */
    char        query[1024];    /* raw query string after '?', or ""     */
    const char *body;           /* request body (NUL-terminated) or NULL */
    size_t      body_len;       /* body length in bytes                  */
    /* Streamed upload (HTTPD_UPLOAD_PATH only): temporary file holding
     * the raw request body.  The handler must rename(2) it to its final
     * destination (or unlink(2) it on error). */
    const char *stream_path;    /* tmp file path or NULL                */
    size_t      stream_size;    /* bytes written to the tmp file        */
} httpd_request_t;

/* Response the API handler fills in.  `body` is heap-owned by the
 * response; the server frees it after the bytes are sent. */
typedef struct {
    int    status;                       /* e.g. 200, 400, 404, 500      */
    char   content_type[64];             /* e.g. "application/json"      */
    char  *body;                         /* owned response body          */
    size_t body_len;                     /* body length                  */
    int    gzip;                         /* add Content-Encoding: gzip   */
} httpd_response_t;

typedef void (*httpd_handler_fn)(const httpd_request_t *req,
                                 httpd_response_t *res, void *user);

/* Initialize a response to an empty 200 JSON document. */
void httpd_response_init(httpd_response_t *res);

/* Replace the response body with a copy of `json` (status/content-type
 * set to application/json). */
void httpd_respond_json(httpd_response_t *res, int status, const char *json);

/* Respond with {"error":"msg"} using proper JSON escaping. */
void httpd_respond_error(httpd_response_t *res, int status, const char *msg);

/*
 * Create the listen socket on 0.0.0.0:`port` (non-blocking, SO_REUSEADDR).
 * Returns the fd, or -1 on failure.
 */
int httpd_listen(int port);

/*
 * One iteration of the server loop: poll() with `timeout_ms`, accept new
 * clients, service existing ones, dispatch complete requests to `handler`
 * (for API paths) or the static web UI route (everything else).
 * Returns 0 on success, -1 on fatal socket error.
 */
int httpd_poll(int server_fd, int timeout_ms, httpd_handler_fn handler,
               void *user);

/* Close all open client connections (server shutdown). */
void httpd_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PS5LM_HTTPD_H */
