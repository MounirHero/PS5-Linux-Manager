/*
 * fsapi.h — real filesystem operations backing the /api/fs/ endpoints
 * (contract v1.1, powers the Files page; no mock data).
 *
 * All operations accept absolute paths and are path-traversal safe: every
 * path is normalized lexically ("."/".." resolved) before use, so tricks
 * like "/a/../../etc" collapse to a canonical absolute path instead of
 * escaping anywhere unexpected.  Names are passed through byte-for-byte
 * (UTF-8 tolerant), dotfiles are included in listings, and file sizes are
 * read via stat64 where the platform provides it.
 */
#ifndef PS5LM_FSAPI_H
#define PS5LM_FSAPI_H

#include <stddef.h>

#include "json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FSAPI_PATH_MAX  1024
#define FSAPI_READ_MAX  (256 * 1024)   /* text read cap: 256 KiB */

/*
 * Normalize `path` into `out`: must start with '/', '.' and '..' segments
 * are resolved lexically, duplicate slashes collapse, a trailing slash is
 * dropped (except for "/").  Returns 0 on success, -1 on a relative or
 * overlong path.
 */
int fsapi_normalize(const char *path, char *out, size_t len);

/*
 * List a directory into `b` as a JSON array of
 * {name, path, type:"file"|"dir", size, mtime} — dotfiles included,
 * "." and ".." skipped, entries sorted by name.  Returns 0 on success,
 * -1 with a message in `err` otherwise.
 */
int fsapi_list_json(const char *path, jbuf_t *b, char *err, size_t errlen);

/*
 * Emit {"path", "type", "size", "mtime"} for `path` into `b`.
 * Returns 0 on success, -1 with a message in `err`.
 */
int fsapi_stat_json(const char *path, jbuf_t *b, char *err, size_t errlen);

/*
 * Read up to FSAPI_READ_MAX bytes of a (text) file.  Returns a malloc'd
 * NUL-terminated buffer in *content (free with free()) and sets
 * *truncated when the file is larger than the cap.  0 ok / -1 with `err`.
 */
int fsapi_read_text(const char *path, char **content, int *truncated,
                    char *err, size_t errlen);

/* Atomically write `content` to `path` (0 ok / -1 with `err`). */
int fsapi_write_text(const char *path, const char *content,
                     char *err, size_t errlen);

/* Create a directory (parents created as needed).  0 ok / -1 with `err`. */
int fsapi_mkdir(const char *path, char *err, size_t errlen);

/* Delete a file or recursively delete a directory.  0 ok / -1 with `err`. */
int fsapi_delete(const char *path, char *err, size_t errlen);

/* Move/rename `from` to `to` (cross-device fallback: copy + delete).
 * 0 ok / -1 with `err`. */
int fsapi_rename(const char *from, const char *to, char *err, size_t errlen);

/* Recursively copy `from` to `to`.  0 ok / -1 with `err`. */
int fsapi_copy(const char *from, const char *to, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* PS5LM_FSAPI_H */
