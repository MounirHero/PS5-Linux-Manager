/*
 * json.h — minimal, dependency-free JSON writer and tolerant parser.
 *
 * Two independent halves:
 *
 *  1. Writer (jbuf_t): a growable byte buffer with helper calls to emit
 *     objects, arrays and scalars.  Commas and string escaping are handled
 *     automatically, so API handlers can stream out responses without
 *     tracking punctuation themselves.
 *
 *  2. Parser (json_value_t): a tolerant recursive-descent parser producing
 *     a small DOM.  "Tolerant" means it accepts trailing commas, C/C++
 *     comments and single-quoted strings — the web UI is the only client,
 *     but a forgiving parser makes manual curl testing far less painful.
 *     Trailing garbage after the root value is still rejected.
 *
 * Everything is C99/C11, malloc/free only.  No external dependencies.
 */
#ifndef PS5LM_JSON_H
#define PS5LM_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Writer                                                              */
/* ------------------------------------------------------------------ */

/* Maximum nesting depth tracked for comma insertion. */
#define JSON_MAX_DEPTH 32

typedef struct jbuf {
    char  *data;                        /* NUL-terminated output buffer  */
    size_t len;                         /* bytes used (excluding NUL)    */
    size_t cap;                         /* allocated capacity            */
    int    counts[JSON_MAX_DEPTH];      /* items emitted per container   */
    char   kinds[JSON_MAX_DEPTH];       /* 'o' or 'a' per container      */
    int    depth;                       /* current container depth       */
    int    error;                       /* sticky OOM / overflow flag    */
} jbuf_t;

void jb_init(jbuf_t *b);
void jb_free(jbuf_t *b);
/* Detach the heap buffer (caller frees).  Returns "" on error. */
char *jb_steal(jbuf_t *b);

void jb_begin_obj(jbuf_t *b);
void jb_end_obj(jbuf_t *b);
void jb_begin_arr(jbuf_t *b);
void jb_end_arr(jbuf_t *b);

/* Emit a key inside an object: jb_key(b, "name"); then a value call. */
void jb_key(jbuf_t *b, const char *key);

void jb_str(jbuf_t *b, const char *s);          /* escaped; NULL -> ""   */
void jb_raw(jbuf_t *b, const char *s);          /* verbatim fragment     */
void jb_int(jbuf_t *b, long long v);
void jb_uint(jbuf_t *b, unsigned long long v);
void jb_double(jbuf_t *b, double v);
void jb_bool(jbuf_t *b, int v);
void jb_null(jbuf_t *b);

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type_t;

typedef struct json_value {
    json_type_t type;
    int         boolean;                /* JSON_BOOL                   */
    double      number;                 /* JSON_NUMBER                 */
    char       *string;                 /* JSON_STRING (owned)         */
    /* JSON_ARRAY */
    struct json_value **items;
    size_t              n_items;
    /* JSON_OBJECT */
    char              **keys;
    struct json_value **values;
    size_t              n_pairs;
} json_value_t;

/* Parse `text`.  Returns NULL on hard failure (OOM / empty input /
 * trailing garbage); minor deviations (trailing commas, comments,
 * single quotes) are accepted. */
json_value_t *json_parse(const char *text);
void          json_free(json_value_t *v);

/* Object accessors (safe on NULL / wrong type). */
const json_value_t *json_obj_get(const json_value_t *obj, const char *key);
const json_value_t *json_arr_get(const json_value_t *arr, size_t idx);

const char *json_get_string(const json_value_t *obj, const char *key,
                            const char *dflt);
int         json_get_int(const json_value_t *obj, const char *key, int dflt);
double      json_get_number(const json_value_t *obj, const char *key,
                            double dflt);
int         json_get_bool(const json_value_t *obj, const char *key, int dflt);

/* Scalar coercion helpers. */
const char *json_as_string(const json_value_t *v, const char *dflt);
int         json_as_bool(const json_value_t *v, int dflt);
double      json_as_number(const json_value_t *v, double dflt);

#ifdef __cplusplus
}
#endif

#endif /* PS5LM_JSON_H */
