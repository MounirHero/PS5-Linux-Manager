/*
 * appdb.h — first-run home-screen app install (Media tab bubble).
 *
 * Following Elf Arsenal's approach, the manager registers itself in the
 * console's app database (/system_data/priv/mms/app.db) so a
 * "PS5 Linux Manager" bubble appears on the PS5 home screen — in the
 * MEDIA tab — deep-linking to the web UI (http://127.0.0.1:8090/).
 *
 * The ps5-payload-sdk does NOT ship sqlite3, so this tree vendors the
 * sqlite3 amalgamation (src/third_party/sqlite3.c, the same copy Elf
 * Arsenal builds against this SDK).  When PS5LM_HAVE_SQLITE3 is not
 * defined at build time, a documented smallest-viable fallback kicks in
 * instead: presence is detected by a raw byte scan of app.db and the
 * exact SQL script is written to the data dir for manual application.
 *
 * Everything here is strictly best effort: a missing/locked/corrupt
 * app.db is logged and skipped, never fatal.  A marker file in the data
 * dir guarantees the install runs at most once.
 */
#ifndef PS5LM_APPDB_H
#define PS5LM_APPDB_H

#ifdef __cplusplus
extern "C" {
#endif

/* Console app.db location; overridable for host smoke tests. */
#ifndef PS5LM_APPDB_PATH
#define PS5LM_APPDB_PATH "/system_data/priv/mms/app.db"
#endif

/* Bubble identity (contract v1.1). */
#define PS5LM_APP_TITLEID   "PSLM00001"
#define PS5LM_APP_CONTENTID "IV9999-PSLM00001_00-PS5LINUXMANAGER0"
#define PS5LM_APP_TITLE     "PS5 Linux Manager"
#define PS5LM_APP_DEEPLINK  "http://127.0.0.1:8090/"

/*
 * Non-zero when the bubble is installed: the first-run marker exists, or
 * a row for our titleId is present in app.db (checked best effort).
 */
int appdb_is_installed(void);

/*
 * First-run install: no-op when appdb_is_installed() is already true.
 * Otherwise attempts the app.db inserts (never crashes on a missing,
 * locked or malformed db — the failure is logged and 0 is returned).
 * Returns non-zero when the bubble is (now) installed.  A reboot of the
 * console is required before the bubble shows up on the home screen.
 */
int appdb_install_if_needed(void);

#ifdef __cplusplus
}
#endif

#endif /* PS5LM_APPDB_H */
