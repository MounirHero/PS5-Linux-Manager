/*
 * fsapi.c — implementation of the real filesystem API declared in fsapi.h.
 */
#if defined(HOST_TEST) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#define _LARGEFILE64_SOURCE 1           /* stat64 on glibc hosts */
#endif

#include "fsapi.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

/* ------------------------------------------------------------------ */
/* stat64 wrapper (64-bit sizes where the platform provides stat64)    */
/* ------------------------------------------------------------------ */

typedef struct {
    int       is_dir;
    int       is_reg;
    long long size;
    long      mtime;
} fsapi_meta_t;

static int fsapi_stat(const char *path, fsapi_meta_t *m) {
#if defined(HOST_TEST) && defined(__linux__) && defined(_LARGEFILE64_SOURCE)
    struct stat64 st;
    if (stat64(path, &st) != 0)
        return -1;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
#endif
    m->is_dir = S_ISDIR(st.st_mode);
    m->is_reg = S_ISREG(st.st_mode);
    m->size = (long long)st.st_size;
    m->mtime = (long)st.st_mtime;
    return 0;
}

static void set_err(char *err, size_t errlen, const char *msg,
                    const char *path) {
    if (!err || !errlen)
        return;
    if (path)
        snprintf(err, errlen, "%s: %s (%s)", msg, strerror(errno), path);
    else
        snprintf(err, errlen, "%s", msg);
}

/* ------------------------------------------------------------------ */
/* Path normalization (traversal safety)                               */
/* ------------------------------------------------------------------ */

int fsapi_normalize(const char *path, char *out, size_t len) {
    char tmp[FSAPI_PATH_MAX];
    char *segs[128];
    size_t nseg = 0, o = 0;
    char *save, *tok;

    if (!out || len == 0)
        return -1;
    out[0] = '\0';
    if (!path || path[0] != '/' || strlen(path) >= sizeof(tmp))
        return -1;

    util_copy(tmp, sizeof(tmp), path);
    for (tok = strtok_r(tmp, "/", &save); tok;
         tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, ".") == 0)
            continue;
        if (strcmp(tok, "..") == 0) {
            if (nseg > 0)
                nseg--;                 /* never pops past the root */
            continue;
        }
        if (nseg >= sizeof(segs) / sizeof(segs[0]))
            return -1;
        segs[nseg++] = tok;
    }

    if (nseg == 0) {
        if (len < 2)
            return -1;
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }
    {
        size_t i;
        for (i = 0; i < nseg; i++) {
            size_t sl = strlen(segs[i]);
            if (o + 1 + sl >= len)
                return -1;
            out[o++] = '/';
            memcpy(out + o, segs[i], sl);
            o += sl;
        }
    }
    out[o] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* Directory listing                                                   */
/* ------------------------------------------------------------------ */

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int fsapi_list_json(const char *path, jbuf_t *b, char *err, size_t errlen) {
    DIR *d;
    struct dirent *de;
    char **names = NULL;
    size_t n = 0, i;
    fsapi_meta_t m;

    if (fsapi_stat(path, &m) != 0) {
        set_err(err, errlen, "cannot stat", path);
        return -1;
    }
    if (!m.is_dir) {
        set_err(err, errlen, "not a directory", NULL);
        return -1;
    }
    d = opendir(path);
    if (!d) {
        set_err(err, errlen, "cannot open directory", path);
        return -1;
    }

    /* Collect names first so the listing is deterministic. */
    while ((de = readdir(d)) != NULL) {
        char **na;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;                     /* dotfiles ARE included */
        na = (char **)realloc(names, (n + 1) * sizeof(*na));
        if (!na)
            break;
        names = na;
        names[n] = strdup(de->d_name);    /* raw bytes: UTF-8 tolerant */
        if (!names[n])
            break;
        n++;
    }
    closedir(d);
    if (n > 1)
        qsort(names, n, sizeof(*names), cmp_names);

    jb_begin_arr(b);
    for (i = 0; i < n; i++) {
        char child[FSAPI_PATH_MAX];
        if (strcmp(path, "/") == 0)
            snprintf(child, sizeof(child), "/%s", names[i]);
        else
            snprintf(child, sizeof(child), "%s/%s", path, names[i]);
        jb_begin_obj(b);
        jb_key(b, "name"); jb_str(b, names[i]);
        jb_key(b, "path"); jb_str(b, child);
        if (fsapi_stat(child, &m) == 0) {
            jb_key(b, "type");  jb_str(b, m.is_dir ? "dir" : "file");
            jb_key(b, "size");  jb_int(b, m.size);
            jb_key(b, "mtime"); jb_int(b, (long long)m.mtime);
        } else {
            jb_key(b, "type");  jb_str(b, "file");
            jb_key(b, "size");  jb_int(b, 0);
            jb_key(b, "mtime"); jb_int(b, 0);
        }
        jb_end_obj(b);
        free(names[i]);
    }
    jb_end_arr(b);
    free(names);
    return 0;
}

int fsapi_stat_json(const char *path, jbuf_t *b, char *err, size_t errlen) {
    fsapi_meta_t m;
    if (fsapi_stat(path, &m) != 0) {
        set_err(err, errlen, "cannot stat", path);
        return -1;
    }
    jb_begin_obj(b);
    jb_key(b, "path");  jb_str(b, path);
    jb_key(b, "type");  jb_str(b, m.is_dir ? "dir" : "file");
    jb_key(b, "size");  jb_int(b, m.is_dir ? 0 : m.size);
    jb_key(b, "mtime"); jb_int(b, (long long)m.mtime);
    jb_end_obj(b);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Read / write                                                        */
/* ------------------------------------------------------------------ */

int fsapi_read_text(const char *path, char **content, int *truncated,
                    char *err, size_t errlen) {
    FILE *f;
    char *buf;
    size_t n;
    fsapi_meta_t m;

    if (content)
        *content = NULL;
    if (truncated)
        *truncated = 0;
    if (!content || !truncated)
        return -1;
    if (fsapi_stat(path, &m) != 0) {
        set_err(err, errlen, "cannot stat", path);
        return -1;
    }
    if (m.is_dir) {
        set_err(err, errlen, "cannot read a directory", NULL);
        return -1;
    }

    buf = (char *)malloc(FSAPI_READ_MAX + 1);
    if (!buf) {
        set_err(err, errlen, "out of memory", NULL);
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        free(buf);
        set_err(err, errlen, "cannot open", path);
        return -1;
    }
    n = fread(buf, 1, FSAPI_READ_MAX, f);
    if (ferror(f)) {
        fclose(f);
        free(buf);
        set_err(err, errlen, "read failed", path);
        return -1;
    }
    /* One more byte tells us whether the cap truncated the file. */
    {
        int c = fgetc(f);
        if (c != EOF)
            *truncated = 1;
    }
    fclose(f);
    buf[n] = '\0';
    *content = buf;
    return 0;
}

int fsapi_write_text(const char *path, const char *content,
                     char *err, size_t errlen) {
    return util_atomic_write(path, content ? content : "",
                             content ? strlen(content) : 0, err, errlen);
}

int fsapi_mkdir(const char *path, char *err, size_t errlen) {
    if (util_mkdir_p(path) != 0) {
        set_err(err, errlen, "cannot create directory", path);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Delete / rename / copy                                              */
/* ------------------------------------------------------------------ */

static int delete_recursive(const char *path, char *err, size_t errlen) {
    fsapi_meta_t m;

    if (fsapi_stat(path, &m) != 0) {
        set_err(err, errlen, "cannot stat", path);
        return -1;
    }
    if (!m.is_dir) {
        if (unlink(path) != 0) {
            set_err(err, errlen, "cannot delete", path);
            return -1;
        }
        return 0;
    }
    {
        DIR *d = opendir(path);
        struct dirent *de;
        if (!d) {
            set_err(err, errlen, "cannot open directory", path);
            return -1;
        }
        while ((de = readdir(d)) != NULL) {
            char child[FSAPI_PATH_MAX];
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0)
                continue;
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            if (delete_recursive(child, err, errlen) != 0) {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
    }
    if (rmdir(path) != 0) {
        set_err(err, errlen, "cannot remove directory", path);
        return -1;
    }
    return 0;
}

int fsapi_delete(const char *path, char *err, size_t errlen) {
    if (!path || strcmp(path, "/") == 0) {
        set_err(err, errlen, "refusing to delete the filesystem root",
                NULL);
        return -1;
    }
    return delete_recursive(path, err, errlen);
}

/* Copy one regular file (used by fsapi_copy and the EXDEV fallback). */
static int copy_file(const char *from, const char *to, char *err,
                     size_t errlen) {
    char buf[64 * 1024];
    size_t n;
    FILE *in, *out;

    in = fopen(from, "rb");
    if (!in) {
        set_err(err, errlen, "cannot open source", from);
        return -1;
    }
    out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        set_err(err, errlen, "cannot open destination", to);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            set_err(err, errlen, "write failed", to);
            return -1;
        }
    }
    if (ferror(in)) {
        fclose(in);
        fclose(out);
        set_err(err, errlen, "read failed", from);
        return -1;
    }
    fclose(in);
    if (fclose(out) != 0) {
        set_err(err, errlen, "close failed", to);
        return -1;
    }
    return 0;
}

static int copy_recursive(const char *from, const char *to, char *err,
                          size_t errlen) {
    fsapi_meta_t m;

    if (fsapi_stat(from, &m) != 0) {
        set_err(err, errlen, "cannot stat source", from);
        return -1;
    }
    if (!m.is_dir)
        return copy_file(from, to, err, errlen);

    if (mkdir(to, 0755) != 0 && errno != EEXIST) {
        set_err(err, errlen, "cannot create directory", to);
        return -1;
    }
    {
        DIR *d = opendir(from);
        struct dirent *de;
        if (!d) {
            set_err(err, errlen, "cannot open directory", from);
            return -1;
        }
        while ((de = readdir(d)) != NULL) {
            char cs[FSAPI_PATH_MAX], cd[FSAPI_PATH_MAX];
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0)
                continue;
            snprintf(cs, sizeof(cs), "%s/%s", from, de->d_name);
            snprintf(cd, sizeof(cd), "%s/%s", to, de->d_name);
            if (copy_recursive(cs, cd, err, errlen) != 0) {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
    }
    return 0;
}

int fsapi_copy(const char *from, const char *to, char *err, size_t errlen) {
    if (!from || !to || !*from || !*to || strcmp(from, "/") == 0) {
        set_err(err, errlen, "invalid copy arguments", NULL);
        return -1;
    }
    /* Guard against copying a directory into itself. */
    if (strncmp(to, from, strlen(from)) == 0 &&
        (to[strlen(from)] == '/' || to[strlen(from)] == '\0')) {
        set_err(err, errlen, "cannot copy a path into itself", NULL);
        return -1;
    }
    return copy_recursive(from, to, err, errlen);
}

int fsapi_rename(const char *from, const char *to, char *err,
                 size_t errlen) {
    if (!from || !to || !*from || !*to || strcmp(from, "/") == 0) {
        set_err(err, errlen, "invalid rename arguments", NULL);
        return -1;
    }
    if (rename(from, to) == 0)
        return 0;
    if (errno == EXDEV) {
        /* Cross-device move: copy the tree, then delete the source. */
        if (copy_recursive(from, to, err, errlen) != 0)
            return -1;
        return delete_recursive(from, err, errlen);
    }
    set_err(err, errlen, "rename failed", from);
    return -1;
}
