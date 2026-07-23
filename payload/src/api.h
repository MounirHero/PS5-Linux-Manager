/*
 * api.h — HTTP API route handlers (contract v1.1).
 *
 * Implements the endpoints of api-contract.md exactly:
 *   GET    /api/status
 *   GET    /api/linux/device          POST /api/linux/device
 *   GET    /api/linux/files
 *   GET    /api/linux/config?name=cmdline.txt|vram.txt
 *   POST   /api/linux/config
 *   GET    /api/bios                  POST /api/bios
 *   GET    /api/boot/grub             POST /api/boot/grub
 *   GET    /api/payloads
 *   POST   /api/payloads/upload?name=x.elf   (raw octet-stream body)
 *   DELETE /api/payloads?name=x.elf
 *   POST   /api/launch                (serves the ELF to 127.0.0.1:9021)
 *   POST   /api/boot/linux            POST /api/boot/orbis
 *   GET    /api/fs/list|stat|read     POST /api/fs/write|mkdir|delete|
 *                                          rename|copy
 *   GET    /api/ftp                   POST /api/ftp
 *   GET    /api/settings              POST /api/settings
 *
 * Errors are HTTP 4xx/5xx with a {"error":"message"} body.
 */
#ifndef PS5LM_API_H
#define PS5LM_API_H

#include "httpd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Manager version reported by GET /api/status. */
#define PS5LM_VERSION "1.2.0"

/*
 * Load persisted settings (config.json) and apply the runtime ones
 * (notification toggle).  Called once from main() before the HTTP loop.
 */
void api_init(void);

/* httpd handler entry point (see httpd.h). */
void api_handle(const httpd_request_t *req, httpd_response_t *res,
                void *user);

/* Currently configured HTTP port (from settings; 8090 by default). */
int api_settings_port(void);

/*
 * Implemented by main.c: ask the manager to exit its serve loop
 * gracefully (used by POST /api/boot/orbis).  The HTTP response is sent
 * before the process actually exits.
 */
void manager_request_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* PS5LM_API_H */
