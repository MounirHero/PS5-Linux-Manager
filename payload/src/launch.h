/*
 * launch.h — hand ELF payloads to the console's own ELF loader on
 * 127.0.0.1:9021 (elfldr / ps5-payload-sdk loader convention).
 *
 * v1.1: the manager NEVER execs payloads from memory.  Instead it opens
 * a TCP connection to the console's ELF loader (port 9021 on loopback),
 * streams the whole ELF file, and closes the connection; the loader then
 * runs the payload.  Used by:
 *   - POST /api/launch      (serve an arbitrary uploaded payload by path)
 *   - POST /api/boot/linux  (serve the discovered ps5-linux-loader ELF)
 */
#ifndef PS5LM_LAUNCH_H
#define PS5LM_LAUNCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TCP port of the console's ELF loader.  Overridable at compile time so
 * the host smoke test can point serve_elf_9021() at a local listener. */
#ifndef PS5LM_ELF_LOADER_PORT
#define PS5LM_ELF_LOADER_PORT 9021
#endif

/*
 * Serve the ELF at `path` to 127.0.0.1:PS5LM_ELF_LOADER_PORT: connect,
 * send the whole file, close.  On success returns 0 and stores the
 * number of bytes sent in *sent (when non-NULL).  Returns -1 with a
 * human-readable reason in `err` (when provided) otherwise.
 *
 * Safety rails: the path must be absolute, must exist, and must end in
 * ".elf" — the web UI only ever sends paths from /api/payloads or the
 * discovered loader, and this keeps accidental sends of random files out.
 */
int serve_elf_9021(const char *path, long long *sent, char *err,
                   size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* PS5LM_LAUNCH_H */
