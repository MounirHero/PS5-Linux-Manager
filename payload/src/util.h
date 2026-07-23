/*
 * util.h — platform utilities: logging, on-console notifications, network
 * identity, console identity (kit/firmware/model/hostname), uptime,
 * atomic file writes.
 *
 * On the PS5 these wrap libkernel services (sceKernelSendNotificationRequest,
 * sceKernelIsDevKit/sceKernelIsTestKit, sceKernelGetHwModelName,
 * ps5-payload-sdk's kernel oracle) and, when loadable, libSceRegMgr.sprx
 * (sceRegMgrGetStr/sceRegMgrGetBin).  Under HOST_TEST the same prototypes
 * come from src/shims/host_shims.h so the whole tree can be syntax-checked
 * with a stock gcc/clang.
 */
#ifndef PS5LM_UTIL_H
#define PS5LM_UTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Manager branding (contract v1.1). */
#define PS5LM_NAME   "PS5 Linux Manager"
#define PS5LM_AUTHOR "InsideMatrix"

/* Where the manager persists its own state.  Overridable at compile time
 * (host smoke tests only; target builds always use /data). */
#ifndef PS5LM_DATA_DIR
#define PS5LM_DATA_DIR  "/data/PS5_LINUX_MANAGER"
#endif
#define PS5LM_LOG_PATH  PS5LM_DATA_DIR "/manager.log"

/* printf-style log line: stderr plus appended to manager.log (best effort). */
void util_log(const char *fmt, ...);

/* On-console popup notification.  Honors util_set_notifications_enabled(). */
int  util_notify(const char *text);
void util_set_notifications_enabled(int enabled);

/* First non-loopback IPv4 address, e.g. "192.168.1.50".  "" on failure. */
void util_get_ip(char *out, size_t len);

/* "retail" | "devkit" | "testkit". */
void util_kit_type(char *out, size_t len);

/*
 * Console host name via gethostname(), e.g. "PS5-720".  Falls back to
 * "PS5" when the hostname is empty/unavailable.  Never crashes.
 */
void util_console_name(char *out, size_t len);

/*
 * Console model code, e.g. "CFI-1215A".  Detection (best effort):
 *   1. sceKernelGetHwModelName() from libkernel (always loaded);
 *   2. libSceRegMgr.sprx sceRegMgrGetStr/sceRegMgrGetBin on a list of
 *      candidate registry keys known to carry the model/serial on
 *      Orbis/Prospero (documented in util.c);
 *   3. "CFI-XXXX" when nothing readable was found.
 * Never crashes; always writes a non-empty string.
 */
void util_model_code(char *out, size_t len);

/* Seconds since the manager process started (monotonic clock). */
long util_uptime_sec(void);

/* Firmware string, e.g. "4.51"; "unknown" when it cannot be determined. */
void util_firmware(char *out, size_t len);

/* Non-zero when `path` exists and is a regular file. */
int util_file_exists(const char *path);

/* Non-zero when `path` exists and is a directory. */
int util_dir_exists(const char *path);

/* mkdir -p (each intermediate component, ignoring EEXIST). */
int util_mkdir_p(const char *path);

/*
 * Atomic file write: data is written to "<path>.tmp", flushed, then
 * rename(2)ed over `path` so a crash mid-write can never leave a
 * truncated configuration behind.  Returns 0 on success, -1 on error
 * (with a human-readable message copied into errbuf when provided).
 */
int util_atomic_write(const char *path, const char *data, size_t len,
                      char *errbuf, size_t errlen);

/* In-place percent decoding of a URL query fragment ("a%20b" -> "a b").
 * '+' is decoded to space (form semantics). */
void util_url_decode(const char *in, char *out, size_t len);

/* Bounded copy that always NUL-terminates (strlcpy semantics). */
void util_copy(char *dst, size_t len, const char *src);

#ifdef __cplusplus
}
#endif

#endif /* PS5LM_UTIL_H */
