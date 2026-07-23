/*
 * json.c — implementation of the minimal JSON writer and tolerant parser.
 * See json.h for the design notes.
 */
#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Writer                                                              */
/* ================================================================== */

void jb_init(jbuf_t *b) {
    memset(b, 0, sizeof(*b));
    b->cap = 512;
    b->data = (char *)malloc(b->cap);
    if (!b->data) {
        b->cap = 0;
        b->error = 1;
        return;
    }
    b->data[0] = '\0';
}

void jb_free(jbuf_t *b) {
    if (b) {
        free(b->data);
        memset(b, 0, sizeof(*b));
    }
}

char *jb_steal(jbuf_t *b) {
    char *out;
    if (b->error || !b->data) {
        jb_free(b);
        out = (char *)malloc(3);
        if (out) strcpy(out, "{}");
        return out;
    }
    out = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    jb_free(b);
    return out;
}

/* Ensure at least `extra` more bytes fit in the buffer. */
static void jb_reserve(jbuf_t *b, size_t extra) {
    size_t need = b->len + extra + 1;
    char *nd;
    size_t nc;
    if (b->error || need <= b->cap)
        return;
    nc = b->cap ? b->cap : 512;
    while (nc < need)
        nc *= 2;
    nd = (char *)realloc(b->data, nc);
    if (!nd) {
        b->error = 1;
        return;
    }
    b->data = nd;
    b->cap = nc;
}

static void jb_putn(jbuf_t *b, const char *s, size_t n) {
    if (b->error)
        return;
    jb_reserve(b, n);
    if (b->error)
        return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void jb_putc(jbuf_t *b, char c) {
    jb_putn(b, &c, 1);
}

/*
 * Emit the separator that precedes the next element: a comma when the
 * enclosing container already holds items, and mark a new item present.
 * `is_key` distinguishes object keys (counted) from object values (not).
 */
static void jb_sep(jbuf_t *b, int is_key) {
    if (b->depth <= 0)
        return;                          /* root scalar: nothing to do */
    if (b->kinds[b->depth - 1] == 'o' && !is_key)
        return;                          /* object value follows ':'    */
    if (b->counts[b->depth - 1] > 0)
        jb_putc(b, ',');
    b->counts[b->depth - 1]++;
}

static void jb_push(jbuf_t *b, char kind, char brace) {
    jb_sep(b, 0);
    if (b->depth < JSON_MAX_DEPTH) {
        b->kinds[b->depth] = kind;
        b->counts[b->depth] = 0;
        b->depth++;
    } else {
        b->error = 1;                    /* pathological nesting        */
    }
    jb_putc(b, brace);
}

static void jb_pop(jbuf_t *b, char brace) {
    if (b->depth > 0)
        b->depth--;
    jb_putc(b, brace);
}

void jb_begin_obj(jbuf_t *b) { jb_push(b, 'o', '{'); }
void jb_end_obj(jbuf_t *b)   { jb_pop(b, '}'); }
void jb_begin_arr(jbuf_t *b) { jb_push(b, 'a', '['); }
void jb_end_arr(jbuf_t *b)   { jb_pop(b, ']'); }

void jb_key(jbuf_t *b, const char *key) {
    jb_sep(b, 1);
    jb_str(b, key);          /* jb_str calls jb_sep(0): no-op for objects */
    jb_putc(b, ':');
}

/* Escape `s` per RFC 8259 and emit it quoted.  NULL becomes "". */
void jb_str(jbuf_t *b, const char *s) {
    const unsigned char *p;
    char tmp[8];
    jb_sep(b, 0);
    jb_putc(b, '"');
    if (s) {
        for (p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
            case '"':  jb_putn(b, "\\\"", 2); break;
            case '\\': jb_putn(b, "\\\\", 2); break;
            case '\b': jb_putn(b, "\\b", 2);  break;
            case '\f': jb_putn(b, "\\f", 2);  break;
            case '\n': jb_putn(b, "\\n", 2);  break;
            case '\r': jb_putn(b, "\\r", 2);  break;
            case '\t': jb_putn(b, "\\t", 2);  break;
            default:
                if (*p < 0x20) {             /* other control chars */
                    snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                    jb_putn(b, tmp, 6);
                } else {
                    jb_putc(b, (char)*p);
                }
            }
        }
    }
    jb_putc(b, '"');
}

void jb_raw(jbuf_t *b, const char *s) {
    jb_sep(b, 0);
    if (s)
        jb_putn(b, s, strlen(s));
}

void jb_int(jbuf_t *b, long long v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", v);
    jb_raw(b, tmp);
}

void jb_uint(jbuf_t *b, unsigned long long v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%llu", v);
    jb_raw(b, tmp);
}

void jb_double(jbuf_t *b, double v) {
    char tmp[48];
    /* %g keeps the JSON compact; %.10g preserves useful precision. */
    snprintf(tmp, sizeof(tmp), "%.10g", v);
    jb_raw(b, tmp);
}

void jb_bool(jbuf_t *b, int v) {
    jb_raw(b, v ? "true" : "false");
}

void jb_null(jbuf_t *b) {
    jb_raw(b, "null");
}

/* ================================================================== */
/* Tolerant parser                                                     */
/* ================================================================== */

typedef struct {
    const char *cur;                     /* cursor into the input text */
    int         error;                   /* sticky hard-error flag     */
} pstate_t;

static void p_ws(pstate_t *p) {
    for (;;) {
        while (*p->cur && isspace((unsigned char)*p->cur))
            p->cur++;
        /* tolerate C++-style and C-style comments: handy when
         * hand-editing the persisted JSON files */
        if (p->cur[0] == '/' && p->cur[1] == '/') {
            while (*p->cur && *p->cur != '\n')
                p->cur++;
        } else if (p->cur[0] == '/' && p->cur[1] == '*') {
            p->cur += 2;
            while (p->cur[0] && !(p->cur[0] == '*' && p->cur[1] == '/'))
                p->cur++;
            if (p->cur[0])
                p->cur += 2;
        } else {
            return;
        }
    }
}

static json_value_t *p_new(pstate_t *p, json_type_t t) {
    json_value_t *v = (json_value_t *)calloc(1, sizeof(*v));
    if (!v) {
        p->error = 1;
        return NULL;
    }
    v->type = t;
    return v;
}

static json_value_t *p_value(pstate_t *p, int depth);

/* Parse a quoted string starting at the current quote char.
 * Accepts both " and ' quoting for tolerance.  Returns owned string. */
static char *p_string_raw(pstate_t *p) {
    char quote = *p->cur;
    size_t cap = 32, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) {
        p->error = 1;
        return NULL;
    }
    p->cur++;                              /* opening quote */
    while (*p->cur && *p->cur != quote) {
        char c = *p->cur++;
        if (c == '\\' && *p->cur) {
            char e = *p->cur++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                /* Decode \uXXXX to UTF-8 (BMP only; surrogates become
                 * the replacement char, good enough for our data). */
                unsigned code = 0;
                int i;
                for (i = 0; i < 4 && isxdigit((unsigned char)*p->cur); i++) {
                    char h = *p->cur++;
                    code <<= 4;
                    code |= (unsigned)(h <= '9' ? h - '0'
                                       : (h | 32) - 'a' + 10);
                }
                if (code < 0x80) {
                    c = (char)code;
                    goto emit;
                } else if (code < 0x800) {
                    if (len + 2 >= cap) { cap *= 2; out = (char *)realloc(out, cap); if (!out) { p->error = 1; return NULL; } }
                    out[len++] = (char)(0xC0 | (code >> 6));
                    out[len++] = (char)(0x80 | (code & 0x3F));
                    continue;
                } else {
                    if (len + 3 >= cap) { cap *= 2; out = (char *)realloc(out, cap); if (!out) { p->error = 1; return NULL; } }
                    out[len++] = (char)(0xE0 | (code >> 12));
                    out[len++] = (char)(0x80 | ((code >> 6) & 0x3F));
                    out[len++] = (char)(0x80 | (code & 0x3F));
                    continue;
                }
            }
            default: c = e; break;         /* \" \\ \/ etc. */
            }
        }
    emit:
        if (len + 1 >= cap) {
            cap *= 2;
            out = (char *)realloc(out, cap);
            if (!out) {
                p->error = 1;
                return NULL;
            }
        }
        out[len++] = c;
    }
    if (*p->cur == quote)
        p->cur++;                          /* closing quote */
    out[len] = '\0';
    return out;
}

static json_value_t *p_string(pstate_t *p) {
    json_value_t *v = p_new(p, JSON_STRING);
    if (!v)
        return NULL;
    v->string = p_string_raw(p);
    if (!v->string) {
        free(v);
        return NULL;
    }
    return v;
}

static int p_lit(pstate_t *p, const char *word) {
    size_t n = strlen(word);
    if (strncmp(p->cur, word, n) == 0) {
        p->cur += n;
        return 1;
    }
    return 0;
}

static json_value_t *p_number(pstate_t *p) {
    const char *start = p->cur;
    json_value_t *v;
    /* strtod is lenient and stops at the first invalid char — exactly
     * the tolerance we want. */
    double d = strtod(p->cur, (char **)&p->cur);
    if (p->cur == start)
        return NULL;                       /* no digits at all */
    v = p_new(p, JSON_NUMBER);
    if (!v)
        return NULL;
    v->number = d;
    return v;
}

/* Append `child` to array `arr`; tolerant of allocation failure. */
static void arr_push(pstate_t *p, json_value_t *arr, json_value_t *child) {
    json_value_t **ni = (json_value_t **)realloc(
        arr->items, (arr->n_items + 1) * sizeof(*ni));
    if (!ni) {
        p->error = 1;
        json_free(child);
        return;
    }
    arr->items = ni;
    arr->items[arr->n_items++] = child;
}

static json_value_t *p_array(pstate_t *p, int depth) {
    json_value_t *arr = p_new(p, JSON_ARRAY);
    if (!arr)
        return NULL;
    p->cur++;                              /* '[' */
    for (;;) {
        json_value_t *child;
        p_ws(p);
        if (*p->cur == ']') {
            p->cur++;
            return arr;
        }
        if (!*p->cur || depth > 64)        /* truncated / runaway */
            return arr;
        child = p_value(p, depth + 1);
        if (!child)
            return arr;                    /* tolerate, keep what we got */
        arr_push(p, arr, child);
        p_ws(p);
        if (*p->cur == ',') {
            p->cur++;
            continue;                      /* trailing commas tolerated */
        }
        if (*p->cur == ']') {
            p->cur++;
            return arr;
        }
        /* Missing comma: tolerate by looping on. */
    }
}

static json_value_t *p_object(pstate_t *p, int depth) {
    json_value_t *obj = p_new(p, JSON_OBJECT);
    if (!obj)
        return NULL;
    p->cur++;                              /* '{' */
    for (;;) {
        char *key;
        json_value_t *val;
        char **nk;
        json_value_t **nv;
        p_ws(p);
        if (*p->cur == '}') {
            p->cur++;
            return obj;
        }
        if (!*p->cur || depth > 64)
            return obj;
        if (*p->cur != '"' && *p->cur != '\'')
            return obj;                    /* keys must be quoted */
        key = p_string_raw(p);
        if (!key)
            return obj;
        p_ws(p);
        if (*p->cur == ':')
            p->cur++;
        /* Tolerate a missing ':' by simply parsing the next value. */
        val = p_value(p, depth + 1);
        if (!val) {
            free(key);
            return obj;
        }
        nk = (char **)realloc(obj->keys, (obj->n_pairs + 1) * sizeof(*nk));
        if (!nk) {
            free(key);
            json_free(val);
            p->error = 1;
            return obj;
        }
        obj->keys = nk;
        nv = (json_value_t **)realloc(obj->values,
                                      (obj->n_pairs + 1) * sizeof(*nv));
        if (!nv) {
            free(key);
            json_free(val);
            p->error = 1;
            return obj;
        }
        obj->values = nv;
        obj->keys[obj->n_pairs] = key;
        obj->values[obj->n_pairs] = val;
        obj->n_pairs++;
        p_ws(p);
        if (*p->cur == ',') {
            p->cur++;
            continue;                      /* trailing commas tolerated */
        }
        if (*p->cur == '}') {
            p->cur++;
            return obj;
        }
    }
}

static json_value_t *p_value(pstate_t *p, int depth) {
    json_value_t *v;
    p_ws(p);
    switch (*p->cur) {
    case '{':
        return p_object(p, depth);
    case '[':
        return p_array(p, depth);
    case '"':
    case '\'':
        return p_string(p);
    case 't':
        if (p_lit(p, "true")) {
            v = p_new(p, JSON_BOOL);
            if (v) v->boolean = 1;
            return v;
        }
        return NULL;
    case 'f':
        if (p_lit(p, "false")) {
            v = p_new(p, JSON_BOOL);
            return v;
        }
        return NULL;
    case 'n':
        if (p_lit(p, "null"))
            return p_new(p, JSON_NULL);
        return NULL;
    default:
        return p_number(p);
    }
}

json_value_t *json_parse(const char *text) {
    pstate_t p;
    json_value_t *root;
    if (!text || !*text)
        return NULL;
    p.cur = text;
    p.error = 0;
    root = p_value(&p, 0);
    /*
     * Tolerance stops at trailing garbage: comments and whitespace after
     * the root value are fine, anything else means the document was not
     * really JSON (e.g. a malformed POST body) and is rejected.
     */
    if (root && !p.error) {
        p_ws(&p);
        if (*p.cur != '\0') {
            json_free(root);
            return NULL;
        }
    }
    if (p.error) {
        json_free(root);
        return NULL;
    }
    return root;
}

void json_free(json_value_t *v) {
    size_t i;
    if (!v)
        return;
    free(v->string);
    for (i = 0; i < v->n_items; i++)
        json_free(v->items[i]);
    free(v->items);
    for (i = 0; i < v->n_pairs; i++) {
        free(v->keys[i]);
        json_free(v->values[i]);
    }
    free(v->keys);
    free(v->values);
    free(v);
}

const json_value_t *json_obj_get(const json_value_t *obj, const char *key) {
    size_t i;
    if (!obj || obj->type != JSON_OBJECT || !key)
        return NULL;
    for (i = 0; i < obj->n_pairs; i++)
        if (obj->keys[i] && strcmp(obj->keys[i], key) == 0)
            return obj->values[i];
    return NULL;
}

const json_value_t *json_arr_get(const json_value_t *arr, size_t idx) {
    if (!arr || arr->type != JSON_ARRAY || idx >= arr->n_items)
        return NULL;
    return arr->items[idx];
}

const char *json_as_string(const json_value_t *v, const char *dflt) {
    if (v && v->type == JSON_STRING && v->string)
        return v->string;
    return dflt;
}

int json_as_bool(const json_value_t *v, int dflt) {
    if (v && v->type == JSON_BOOL)
        return v->boolean;
    if (v && v->type == JSON_NUMBER)
        return v->number != 0.0;
    return dflt;
}

double json_as_number(const json_value_t *v, double dflt) {
    if (v && v->type == JSON_NUMBER)
        return v->number;
    if (v && v->type == JSON_BOOL)
        return v->boolean ? 1.0 : 0.0;
    return dflt;
}

const char *json_get_string(const json_value_t *obj, const char *key,
                            const char *dflt) {
    return json_as_string(json_obj_get(obj, key), dflt);
}

int json_get_int(const json_value_t *obj, const char *key, int dflt) {
    return (int)json_as_number(json_obj_get(obj, key), (double)dflt);
}

double json_get_number(const json_value_t *obj, const char *key, double dflt) {
    return json_as_number(json_obj_get(obj, key), dflt);
}

int json_get_bool(const json_value_t *obj, const char *key, int dflt) {
    return json_as_bool(json_obj_get(obj, key), dflt);
}
