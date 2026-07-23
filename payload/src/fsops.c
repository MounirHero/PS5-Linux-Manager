/*
 * fsops.c — implementation of the filesystem operations declared in fsops.h.
 *
 * Guiding rules (from SPEC):
 *   - never crash on a missing device: every mount access is checked, every
 *     "not present" condition becomes data (present:false / exists:false),
 *     not an abort;
 *   - every write is atomic via util_atomic_write (tmp + rename).
 */
#if defined(HOST_TEST) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include "fsops.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Non-zero when `name` ends in .elf (case-insensitive). */
static int has_elf_ext(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n < 4)
        return 0;
    name += n - 4;
    return name[0] == '.' &&
           tolower((unsigned char)name[1]) == 'e' &&
           tolower((unsigned char)name[2]) == 'l' &&
           tolower((unsigned char)name[3]) == 'f';
}

/* Classify a file name for the "kind" column of /api/linux/files. */
static const char *classify_kind(const char *name) {
    if (strcmp(name, FSOPS_NAME_KERNEL) == 0)
        return "kernel";
    if (strncmp(name, "initrd", 6) == 0)
        return "initrd";
    if (has_elf_ext(name))
        return "loader";
    if (strstr(name, ".txt") || strstr(name, ".cfg"))
        return "config";
    return "other";
}

/* Strip trailing CR/LF/whitespace in place (config files are one-liners). */
static void rstrip(char *s) {
    size_t n;
    if (!s)
        return;
    n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

/* Read an entire small file into a malloc'd, NUL-terminated buffer. */
static char *read_whole(const char *path) {
    FILE *f;
    long sz;
    char *buf;

    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Linux device selection                                              */
/* ------------------------------------------------------------------ */

static char g_device[32] = "usb0";

int fsops_device_valid(const char *dev) {
    size_t i, n;
    if (!dev)
        return 0;
    n = strlen(dev);
    if (n < 4 || n >= sizeof(g_device))
        return 0;
    if (strncmp(dev, "usb", 3) != 0)
        return 0;
    for (i = 3; i < n; i++)
        if (!isdigit((unsigned char)dev[i]))
            return 0;
    return 1;
}

void fsops_set_linux_device(const char *dev) {
    if (fsops_device_valid(dev))
        util_copy(g_device, sizeof(g_device), dev);
}

const char *fsops_linux_device(void) {
    return g_device;
}

int fsops_device_root(char *out, size_t len) {
    if (!out || len == 0)
        return -1;
    snprintf(out, len, "%s/%s", FSOPS_MNT_PREFIX, g_device);
    return 0;
}

int fsops_device_present(void) {
    char root[FSOPS_PATH_MAX];
    if (fsops_device_root(root, sizeof(root)) != 0)
        return 0;
    return util_dir_exists(root);
}

int fsops_scan_usbs(usb_status_t out[FSOPS_MAX_USB]) {
    int i;
    for (i = 0; i < FSOPS_MAX_USB; i++) {
        char path[FSOPS_PATH_MAX];
        snprintf(out[i].name, sizeof(out[i].name), "usb%d", i);
        snprintf(out[i].mount, sizeof(out[i].mount), "%s/usb%d",
                 FSOPS_MNT_PREFIX, i);
        out[i].present = util_dir_exists(out[i].mount);
        out[i].linux_files = 0;
        if (out[i].present) {
            snprintf(path, sizeof(path), "%s/%s", out[i].mount,
                     FSOPS_NAME_KERNEL);
            out[i].linux_files = util_file_exists(path);
        }
    }
    return FSOPS_MAX_USB;
}

/* ------------------------------------------------------------------ */
/* Loader config files at the device root                              */
/* ------------------------------------------------------------------ */

/* Non-zero when `name` is one of the loader config files we manage. */
static int is_known_config_name(const char *name) {
    return name &&
           (strcmp(name, FSOPS_NAME_CMDLINE) == 0 ||
            strcmp(name, FSOPS_NAME_VRAM) == 0);
}

int fsops_read_linux_config(const char *name, char *out, size_t len,
                            int *exists) {
    char root[FSOPS_PATH_MAX];
    char path[FSOPS_PATH_MAX];
    char *raw;

    if (exists)
        *exists = 0;
    if (out && len)
        out[0] = '\0';
    if (!is_known_config_name(name) || !out || !len)
        return -1;

    if (!fsops_device_present())
        return 0;                           /* no device: exists=false */

    fsops_device_root(root, sizeof(root));
    snprintf(path, sizeof(path), "%s/%s", root, name);
    raw = read_whole(path);
    if (!raw)
        return 0;                           /* missing file: exists=false */

    rstrip(raw);                            /* one logical line of text */
    util_copy(out, len, raw);
    free(raw);
    if (exists)
        *exists = 1;
    return 0;
}

int fsops_write_linux_config(const char *name, const char *content,
                             char *err, size_t errlen) {
    char root[FSOPS_PATH_MAX];
    char path[FSOPS_PATH_MAX];

    if (!is_known_config_name(name)) {
        if (err && errlen)
            snprintf(err, errlen, "unknown config file '%s'",
                     name ? name : "(null)");
        return -1;
    }
    if (!fsops_device_present()) {
        if (err && errlen)
            snprintf(err, errlen, "device %s is not present",
                     fsops_linux_device());
        return -1;
    }

    fsops_device_root(root, sizeof(root));
    snprintf(path, sizeof(path), "%s/%s", root, name);
    return util_atomic_write(path, content ? content : "",
                             content ? strlen(content) : 0, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Device root listing                                                 */
/* ------------------------------------------------------------------ */

static int cmp_files(const void *a, const void *b) {
    return strcmp(((const linux_file_t *)a)->name,
                  ((const linux_file_t *)b)->name);
}

int fsops_list_linux_files(const char *dir, linux_file_t **out,
                           size_t *count, char *dir_used, size_t dir_len) {
    char resolved[FSOPS_PATH_MAX];
    DIR *d;
    struct dirent *de;
    linux_file_t *arr = NULL;
    size_t n = 0;

    if (out)
        *out = NULL;
    if (count)
        *count = 0;
    if (dir_used && dir_len)
        dir_used[0] = '\0';
    if (!out || !count)
        return -1;

    if (dir && *dir)
        util_copy(resolved, sizeof(resolved), dir);
    else if (!fsops_device_present())
        return 0;                 /* no device: empty listing, not an error */
    else
        fsops_device_root(resolved, sizeof(resolved));

    d = opendir(resolved);
    if (!d)
        return 0;                 /* vanished between checks: tolerate */

    while ((de = readdir(d)) != NULL) {
        char path[FSOPS_PATH_MAX];
        struct stat st;
        linux_file_t *na;

        if (de->d_name[0] == '.')
            continue;             /* skip dotfiles and . / .. */
        snprintf(path, sizeof(path), "%s/%s", resolved, de->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        na = (linux_file_t *)realloc(arr, (n + 1) * sizeof(*na));
        if (!na) {
            free(arr);
            closedir(d);
            return -1;
        }
        arr = na;
        memset(&arr[n], 0, sizeof(arr[n]));
        util_copy(arr[n].name, sizeof(arr[n].name), de->d_name);
        util_copy(arr[n].path, sizeof(arr[n].path), path);
        arr[n].size = (long long)st.st_size;
        arr[n].mtime = (long)st.st_mtime;
        util_copy(arr[n].kind, sizeof(arr[n].kind),
                  classify_kind(de->d_name));
        arr[n].present = 1;
        n++;
    }
    closedir(d);

    if (n > 1)
        qsort(arr, n, sizeof(*arr), cmp_files);
    *out = arr;
    *count = n;
    if (dir_used && dir_len)
        util_copy(dir_used, dir_len, resolved);
    return 0;
}

void fsops_required_presence(int *bzimage, int *initrd, int *cmdline) {
    char root[FSOPS_PATH_MAX];
    char path[FSOPS_PATH_MAX];

    if (bzimage)
        *bzimage = 0;
    if (initrd)
        *initrd = 0;
    if (cmdline)
        *cmdline = 0;
    if (!fsops_device_present())
        return;

    fsops_device_root(root, sizeof(root));
    if (bzimage) {
        snprintf(path, sizeof(path), "%s/%s", root, FSOPS_NAME_KERNEL);
        *bzimage = util_file_exists(path);
    }
    if (initrd) {
        snprintf(path, sizeof(path), "%s/%s", root, FSOPS_NAME_INITRD);
        *initrd = util_file_exists(path);
    }
    if (cmdline) {
        snprintf(path, sizeof(path), "%s/%s", root, FSOPS_NAME_CMDLINE);
        *cmdline = util_file_exists(path);
    }
}

/* ------------------------------------------------------------------ */
/* Uploaded payloads (PAYLOADS/)                                       */
/* ------------------------------------------------------------------ */

/* Append one payload entry to a growable array (0 ok / -1 OOM). */
static int payload_push(payload_info_t **arr, size_t *n,
                        const char *dir, const char *name) {
    char path[FSOPS_PATH_MAX];
    struct stat st;
    payload_info_t *na;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;                          /* raced unlink: just skip */

    na = (payload_info_t *)realloc(*arr, (*n + 1) * sizeof(*na));
    if (!na)
        return -1;
    *arr = na;
    memset(&(*arr)[*n], 0, sizeof((*arr)[*n]));
    util_copy((*arr)[*n].name, sizeof((*arr)[*n].name), name);
    util_copy((*arr)[*n].path, sizeof((*arr)[*n].path), path);
    (*arr)[*n].size = (long long)st.st_size;
    (*arr)[*n].mtime = (long)st.st_mtime;
    (*n)++;
    return 0;
}

static int cmp_payloads(const void *a, const void *b) {
    return strcmp(((const payload_info_t *)a)->name,
                  ((const payload_info_t *)b)->name);
}

/* Collect every *.elf directly inside `dir`. */
static void payload_collect_dir(payload_info_t **arr, size_t *n,
                                const char *dir) {
    DIR *d;
    struct dirent *de;

    d = opendir(dir);
    if (!d)
        return;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' || !has_elf_ext(de->d_name))
            continue;
        if (payload_push(arr, n, dir, de->d_name) != 0)
            break;                         /* OOM: return what we have */
    }
    closedir(d);
}

int fsops_list_payloads(payload_info_t **out, size_t *count) {
    payload_info_t *arr = NULL;
    size_t n = 0;

    if (!out || !count)
        return -1;

    payload_collect_dir(&arr, &n, FSOPS_PAYLOADS_DIR);
    if (n > 1)
        qsort(arr, n, sizeof(*arr), cmp_payloads);

    *out = arr;
    *count = n;
    return 0;
}

int fsops_payload_name_ok(const char *name) {
    if (!name || !*name)
        return 0;
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, ".."))
        return 0;
    return has_elf_ext(name);
}

int fsops_finalize_upload(const char *tmp_path, const char *name,
                          char *err, size_t errlen) {
    char dest[FSOPS_PATH_MAX];
    unsigned char magic[4];
    FILE *f;

    if (!fsops_payload_name_ok(name)) {
        if (err && errlen)
            snprintf(err, errlen, "invalid payload name (need a plain "
                     "*.elf file name)");
        return -1;
    }
    if (!tmp_path) {
        if (err && errlen)
            snprintf(err, errlen, "no upload body received");
        return -1;
    }

    /* Verify the ELF magic before accepting the file. */
    f = fopen(tmp_path, "rb");
    if (!f) {
        if (err && errlen)
            snprintf(err, errlen, "uploaded data missing");
        return -1;
    }
    if (fread(magic, 1, 4, f) != 4 || magic[0] != 0x7f ||
        magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
        fclose(f);
        if (err && errlen)
            snprintf(err, errlen, "not an ELF file (bad magic)");
        return -1;
    }
    fclose(f);

    fsops_ensure_data_dirs();
    snprintf(dest, sizeof(dest), "%s/%s", FSOPS_PAYLOADS_DIR, name);
    if (rename(tmp_path, dest) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "cannot store %s: %s", dest,
                     strerror(errno));
        return -1;
    }
    util_log("payload uploaded: %s", dest);
    return 0;
}

int fsops_delete_payload(const char *name, char *err, size_t errlen) {
    char path[FSOPS_PATH_MAX];

    if (!fsops_payload_name_ok(name)) {
        if (err && errlen)
            snprintf(err, errlen, "invalid payload name");
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s", FSOPS_PAYLOADS_DIR, name);
    if (unlink(path) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "cannot delete %s: %s", path,
                     strerror(errno));
        return -1;
    }
    util_log("payload deleted: %s", path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Boot-Linux loader discovery                                         */
/* ------------------------------------------------------------------ */

int fsops_find_loader(char *out, size_t len) {
    payload_info_t *list = NULL;
    size_t count = 0, i;

    if (len)
        out[0] = '\0';

    /* 1. an uploaded ps5-linux-loader*.elf (preferred). */
    if (fsops_list_payloads(&list, &count) == 0) {
        for (i = 0; i < count; i++) {
            if (strncmp(list[i].name, FSOPS_NAME_LOADER_PREFIX,
                        strlen(FSOPS_NAME_LOADER_PREFIX)) == 0) {
                util_copy(out, len, list[i].path);
                free(list);
                return 0;
            }
        }
        free(list);
    }

    /* 2. any *.elf at the selected device root. */
    if (fsops_device_present()) {
        char root[FSOPS_PATH_MAX];
        DIR *d;
        struct dirent *de;

        fsops_device_root(root, sizeof(root));
        d = opendir(root);
        if (d) {
            while ((de = readdir(d)) != NULL) {
                char path[FSOPS_PATH_MAX];
                if (de->d_name[0] == '.' || !has_elf_ext(de->d_name))
                    continue;
                snprintf(path, sizeof(path), "%s/%s", root, de->d_name);
                if (util_file_exists(path)) {
                    util_copy(out, len, path);
                    closedir(d);
                    return 0;
                }
            }
            closedir(d);
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Manager persistence                                                 */
/* ------------------------------------------------------------------ */

int fsops_ensure_data_dirs(void) {
    if (util_mkdir_p(PS5LM_DATA_DIR) != 0)
        return -1;
    if (util_mkdir_p(FSOPS_PAYLOADS_DIR) != 0)
        return -1;
    return 0;
}

char *fsops_read_data_file(const char *name) {
    char path[FSOPS_PATH_MAX];
    if (!name || strchr(name, '/'))
        return NULL;                       /* no path traversal */
    snprintf(path, sizeof(path), "%s/%s", PS5LM_DATA_DIR, name);
    return read_whole(path);
}

int fsops_write_data_file(const char *name, const char *data, size_t len,
                          char *err, size_t errlen) {
    char path[FSOPS_PATH_MAX];
    if (!name || strchr(name, '/')) {
        if (err && errlen)
            snprintf(err, errlen, "invalid data file name");
        return -1;
    }
    fsops_ensure_data_dirs();
    snprintf(path, sizeof(path), "%s/%s", PS5LM_DATA_DIR, name);
    return util_atomic_write(path, data, len, err, errlen);
}
