/*
 * appdb.c — implementation of the home-screen app install (appdb.h).
 */
#if defined(HOST_TEST) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include "appdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsops.h"
#include "util.h"

#ifdef PS5LM_HAVE_SQLITE3
#include "third_party/sqlite3.h"
#endif

/* First-run marker inside the manager data dir. */
#define APPDB_MARKER "app.installed"

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

static int marker_exists(void) {
    char path[FSOPS_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", PS5LM_DATA_DIR, APPDB_MARKER);
    return util_file_exists(path);
}

static void marker_write(void) {
    char err[128];
    static const char text[] =
        "PS5 Linux Manager home-screen bubble installed into app.db.\n"
        "A console reboot is required for the bubble to appear in the "
        "Media tab.\n";
    err[0] = '\0';
    if (fsops_write_data_file(APPDB_MARKER, text, sizeof(text) - 1,
                              err, sizeof(err)) != 0)
        util_log("appdb: cannot write marker: %s", err);
}

/* Raw byte scan of app.db for our titleId (presence heuristic; SQLite
 * stores text records inline, so this is reliable enough for status). */
static int db_raw_contains_title(void) {
    FILE *f = fopen(PS5LM_APPDB_PATH, "rb");
    char window[64];
    size_t wlen = 0;
    int found = 0;
    int c;
    const char *needle = PS5LM_APP_TITLEID;
    size_t nlen = strlen(needle);

    if (!f)
        return 0;
    while ((c = fgetc(f)) != EOF) {
        if (wlen < nlen) {
            window[wlen++] = (char)c;
        } else {
            memmove(window, window + 1, nlen - 1);
            window[nlen - 1] = (char)c;
        }
        if (wlen == nlen && memcmp(window, needle, nlen) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

#ifdef PS5LM_HAVE_SQLITE3
/* ------------------------------------------------------------------ */
/* Real implementation (vendored sqlite3 amalgamation)                 */
/* ------------------------------------------------------------------ */

static int db_row_exists(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    int found = 0;

    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM tbl_contentinfo WHERE titleId=?1 LIMIT 1",
            -1, &st, NULL) != SQLITE_OK)
        return db_raw_contains_title();
    sqlite3_bind_text(st, 1, PS5LM_APP_TITLEID, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        found = 1;
    sqlite3_finalize(st);
    return found;
}

/* Non-zero when `table` has a column named `col`. */
static int db_has_column(sqlite3 *db, const char *table, const char *col) {
    sqlite3_stmt *st = NULL;
    char sql[160];
    int found = 0;

    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 1);
        if (name && strcmp((const char *)name, col) == 0) {
            found = 1;
            break;
        }
    }
    sqlite3_finalize(st);
    return found;
}

/* First table matching 'tbl_iconinfo%' (name suffix varies by firmware). */
static int db_icon_table(sqlite3 *db, char *out, size_t len) {
    sqlite3_stmt *st = NULL;
    int rc = -1;

    if (sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master WHERE type='table' AND "
            "name LIKE 'tbl_iconinfo%' LIMIT 1", -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 0);
        if (name) {
            util_copy(out, len, (const char *)name);
            rc = 0;
        }
    }
    sqlite3_finalize(st);
    return rc;
}

/*
 * Media-tab placement: read the category/viewCategory of an already
 * installed MEDIA app (YouTube, Netflix, Media Player, Spotify) and
 * reuse those exact values.  Fallback (documented): "gda", the category
 * used by media applications in Orbis/Prospero app metadata (games use
 * "gde"); combined with deeplinkUri + a non-game title this places the
 * bubble in the Media tab.
 */
static void db_probe_media_category(sqlite3 *db, char *cat, size_t clen,
                                    char *vcat, size_t vlen) {
    static const char *media_ids[] = {
        "PPSA01650",   /* YouTube          */
        "PPSA01651",   /* Netflix          */
        "PPSA01652",   /* Amazon Prime     */
        "NPXS20102",   /* Media Player     */
        NULL
    };
    sqlite3_stmt *st = NULL;
    int i;

    util_copy(cat, clen, "gda");
    util_copy(vcat, vlen, "gda");
    if (!db_has_column(db, "tbl_contentinfo", "category"))
        return;

    for (i = 0; media_ids[i]; i++) {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT category FROM tbl_contentinfo WHERE titleId=?1 "
                "LIMIT 1", -1, &q, NULL) != SQLITE_OK)
            continue;
        sqlite3_bind_text(q, 1, media_ids[i], -1, SQLITE_STATIC);
        if (sqlite3_step(q) == SQLITE_ROW) {
            const unsigned char *v = sqlite3_column_text(q, 0);
            if (v && *v)
                util_copy(cat, clen, (const char *)v);
        }
        sqlite3_finalize(q);
        if (strcmp(cat, "gda") != 0)
            break;                         /* got a real value */
    }

    if (db_has_column(db, "tbl_contentinfo", "viewCategory") &&
        sqlite3_prepare_v2(db,
            "SELECT viewCategory FROM tbl_contentinfo WHERE category=?1 "
            "LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, cat, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *v = sqlite3_column_text(st, 0);
            if (v && *v)
                util_copy(vcat, vlen, (const char *)v);
        }
        sqlite3_finalize(st);
    }
}

/* INSERT INTO `table` only the columns that actually exist. */
static int db_insert(sqlite3 *db, const char *table,
                     const char *const cols[][2], size_t ncols) {
    char sql[2048];
    char values[512];
    size_t i, n = 0;
    int rc;

    util_copy(sql, sizeof(sql), "INSERT OR REPLACE INTO ");
    util_copy(sql + strlen(sql), sizeof(sql) - strlen(sql), table);
    util_copy(sql + strlen(sql), sizeof(sql) - strlen(sql), " (");
    values[0] = '\0';
    for (i = 0; i < ncols; i++) {
        const char *col = cols[i][0];
        const char *val = cols[i][1];
        if (!db_has_column(db, table, col))
            continue;                      /* schema drift: skip */
        if (n) {
            util_copy(sql + strlen(sql), sizeof(sql) - strlen(sql), ",");
            util_copy(values + strlen(values),
                      sizeof(values) - strlen(values), ",");
        }
        util_copy(sql + strlen(sql), sizeof(sql) - strlen(sql), col);
        util_copy(values + strlen(values), sizeof(values) - strlen(values),
                  "'");
        util_copy(values + strlen(values), sizeof(values) - strlen(values),
                  val);
        util_copy(values + strlen(values), sizeof(values) - strlen(values),
                  "'");
        n++;
    }
    if (!n)
        return -1;                         /* nothing insertable */
    util_copy(sql + strlen(sql), sizeof(sql) - strlen(sql), ") VALUES (");
    if (strlen(sql) + strlen(values) + 2 >= sizeof(sql))
        return -1;
    strcat(sql, values);
    strcat(sql, ")");

    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        util_log("appdb: insert into %s failed: %s", table,
                 sqlite3_errmsg(db));
    return rc == SQLITE_OK ? 0 : -1;
}

static int appdb_install_sqlite(void) {
    sqlite3 *db = NULL;
    char cat[32], vcat[32];
    char icon_table[128];
    int ok = 0;

    if (sqlite3_open_v2(PS5LM_APPDB_PATH, &db,
                        SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        util_log("appdb: cannot open %s (%s) — skipping bubble install",
                 PS5LM_APPDB_PATH, db ? sqlite3_errmsg(db) : "no db");
        if (db)
            sqlite3_close(db);
        return 0;
    }

    if (!db_has_column(db, "tbl_contentinfo", "titleId")) {
        util_log("appdb: tbl_contentinfo missing — not a PS5 app.db?");
        sqlite3_close(db);
        return 0;
    }

    if (db_row_exists(db)) {
        sqlite3_close(db);
        return 1;                          /* already installed */
    }

    db_probe_media_category(db, cat, sizeof(cat), vcat, sizeof(vcat));
    util_log("appdb: media category = %s / %s", cat, vcat);

    sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

    {
        const char *const cols[][2] = {
            { "titleId",       PS5LM_APP_TITLEID   },
            { "contentId",     PS5LM_APP_CONTENTID },
            { "titleName",     PS5LM_APP_TITLE     },
            { "deeplinkUri",   PS5LM_APP_DEEPLINK  },
            { "category",      cat                 },
            { "viewCategory",  vcat                },
            { "appDrmType",    "5"                 },
            { "contentType",   "0"                 },
            { "dispLocation",  "1"                 },
            { "isVisible",     "1"                 },
            { "lastUsedIndex", "0"                 },
            { "contentSize",   "0"                 },
        };
        if (db_insert(db, "tbl_contentinfo", cols,
                      sizeof(cols) / sizeof(cols[0])) == 0)
            ok = 1;
    }
    {
        const char *const cols[][2] = {
            { "titleId",      PS5LM_APP_TITLEID   },
            { "contentId",    PS5LM_APP_CONTENTID },
            { "conceptId",    PS5LM_APP_CONTENTID },
            { "titleName",    PS5LM_APP_TITLE     },
            { "deeplinkUri",  PS5LM_APP_DEEPLINK  },
            { "category",     cat                 },
            { "viewCategory", vcat                },
        };
        db_insert(db, "tbl_conceptmetadata", cols,
                  sizeof(cols) / sizeof(cols[0]));
    }
    if (db_icon_table(db, icon_table, sizeof(icon_table)) == 0) {
        const char *const cols[][2] = {
            { "titleId",     PS5LM_APP_TITLEID  },
            { "appDrmType",  "5"                },
            { "deeplinkUri", PS5LM_APP_DEEPLINK },
            { "titleName",   PS5LM_APP_TITLE    },
        };
        db_insert(db, icon_table, cols, sizeof(cols) / sizeof(cols[0]));
    }

    if (ok) {
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        util_log("appdb: bubble installed (%s) — REBOOT required for it "
                 "to appear in the Media tab", PS5LM_APP_TITLEID);
    } else {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        util_log("appdb: insert failed, rolled back");
    }
    sqlite3_close(db);
    return ok;
}
#endif /* PS5LM_HAVE_SQLITE3 */

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int appdb_is_installed(void) {
    if (marker_exists())
        return 1;
    if (!util_file_exists(PS5LM_APPDB_PATH))
        return 0;
#ifdef PS5LM_HAVE_SQLITE3
    {
        sqlite3 *db = NULL;
        int found;
        if (sqlite3_open_v2(PS5LM_APPDB_PATH, &db, SQLITE_OPEN_READONLY,
                            NULL) != SQLITE_OK)
            return db_raw_contains_title();
        found = db_row_exists(db);
        sqlite3_close(db);
        return found;
    }
#else
    return db_raw_contains_title();
#endif
}

int appdb_install_if_needed(void) {
    if (appdb_is_installed()) {
        util_log("appdb: bubble already installed");
        return 1;
    }
    if (!util_file_exists(PS5LM_APPDB_PATH)) {
        util_log("appdb: %s not found — skipping bubble install "
                 "(no harm done)", PS5LM_APPDB_PATH);
        return 0;
    }

#ifdef PS5LM_HAVE_SQLITE3
    if (appdb_install_sqlite()) {
        marker_write();
        return 1;
    }
    return 0;
#else
    /*
     * Smallest viable alternative when the build has no sqlite3
     * (PS5LM_HAVE_SQLITE3 undefined): we cannot safely edit the SQLite
     * b-tree in place, so we write the exact SQL script next to the
     * manager data for manual application (via any sqlite3 client) and
     * log clear instructions.  The manager keeps running normally.
     */
    {
        char err[128];
        static const char script[] =
            "-- PS5 Linux Manager home-screen bubble (Media tab)\n"
            "-- Apply against /system_data/priv/mms/app.db, then reboot:\n"
            "INSERT OR REPLACE INTO tbl_contentinfo\n"
            "  (titleId, contentId, titleName, deeplinkUri, category,"
            " viewCategory)\n"
            "VALUES ('" PS5LM_APP_TITLEID "', '" PS5LM_APP_CONTENTID
            "', '" PS5LM_APP_TITLE "', '" PS5LM_APP_DEEPLINK
            "', 'gda', 'gda');\n";
        err[0] = '\0';
        if (fsops_write_data_file("app_install.sql", script,
                                  sizeof(script) - 1, err,
                                  sizeof(err)) == 0)
            util_log("appdb: sqlite3 unavailable in this build — wrote "
                     "app_install.sql for manual application");
        else
            util_log("appdb: cannot write app_install.sql: %s", err);
    }
    return 0;
#endif
}
