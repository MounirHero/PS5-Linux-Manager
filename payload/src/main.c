/*
 * main.c — PS5 Linux Manager by InsideMatrix, payload entry point.
 *
 * Boot sequence (per SPEC, contract v1.1):
 *   1. create /data/PS5_LINUX_MANAGER{,/PAYLOADS}
 *   2. load settings (config.json) via api_init() — this also applies the
 *      linuxDevice selection and starts/restarts the embedded FTP daemon
 *      with the persisted settings
 *   3. load the boot-entry model (entries.json) and schedule auto-boot
 *      when autoBoot == "linux" (countdown of grub timeout) or exit
 *      immediately when autoBoot == "orbis"
 *   4. start the poll()-based HTTP server (default port 8090) with the
 *      embedded web UI and the JSON API
 *   5. install the home-screen bubble into app.db on first run
 *      (best effort; never fatal — see appdb.c)
 *   6. send the three startup notifications (welcome / credits / port)
 *   7. serve loop: HTTP + FTP + auto-boot countdown
 *
 * On target builds the ps5-payload-sdk crt (crt1.o) initializes the kernel
 * oracle and libc and then calls the classic main() entry point; under
 * HOST_TEST the same main() is used so the logic can be exercised on a
 * development host.
 */

/* Feature-test macros first (see util.c note). */
#if defined(HOST_TEST) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include <stdio.h>
#include <string.h>

#include "api.h"
#include "appdb.h"
#include "fsops.h"
#include "ftpsrv.h"
#include "grub.h"
#include "httpd.h"
#include "launch.h"
#include "util.h"

/* Graceful-shutdown flag toggled by POST /api/boot/orbis. */
static volatile int g_running = 1;

void manager_request_stop(void) {
    g_running = 0;
}

/* ------------------------------------------------------------------ */
/* Auto-boot scheduling                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    int  pending;          /* countdown armed                          */
    long fire_at;          /* uptime_sec at which to launch the loader */
} autoboot_t;

static void autoboot_setup(autoboot_t *ab, const boot_config_t *cfg) {
    ab->pending = 0;
    ab->fire_at = 0;

    if (!cfg->enabled)
        return;

    if (strcmp(cfg->auto_boot, GRUB_AUTO_ORBIS) == 0) {
        /* Dual-boot config says: go straight back to Orbis OS. */
        util_log("autoBoot=orbis: exiting manager");
        manager_request_stop();
        return;
    }

    if (strcmp(cfg->auto_boot, GRUB_AUTO_LINUX) == 0) {
        ab->pending = 1;
        ab->fire_at = util_uptime_sec() +
                      (cfg->timeout_sec > 0 ? cfg->timeout_sec : 0);
        util_log("autoBoot=linux: launching loader in %d s",
                 cfg->timeout_sec);
    }
}

/*
 * Check the auto-boot countdown; when it expires, serve the discovered
 * loader ELF to the console's ELF loader on 127.0.0.1:9021.
 * Fires at most once.
 */
static void autoboot_tick(autoboot_t *ab) {
    char loader[FSOPS_PATH_MAX];
    char err[512];
    long long sent = 0;

    if (!ab->pending || util_uptime_sec() < ab->fire_at)
        return;
    ab->pending = 0;                       /* single shot, success or not */

    if (fsops_find_loader(loader, sizeof(loader)) != 0) {
        util_log("autoBoot: loader not found, staying in manager");
        return;
    }
    util_log("autoBoot: serving %s on port %d", loader,
             PS5LM_ELF_LOADER_PORT);
    util_notify("Auto-booting Linux...");
    if (serve_elf_9021(loader, &sent, err, sizeof(err)) != 0) {
        util_log("autoBoot: serve failed: %s", err);
        util_notify("Auto-boot failed to serve the loader");
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

static int manager_run(void) {
    boot_config_t boot_cfg;
    autoboot_t autoboot;
    int server_fd, port;
    char note[64];

    util_log("=== " PS5LM_NAME " " PS5LM_VERSION " by " PS5LM_AUTHOR
             " starting ===");

    /* 1. Persistent storage must exist before anything reads/writes. */
    if (fsops_ensure_data_dirs() != 0)
        util_log("warning: cannot create %s (settings will not persist)",
                 PS5LM_DATA_DIR);

    /* 2. Settings (port, notifications, linuxDevice, ftp).  api_init()
     *    applies them, including (re)starting the FTP daemon. */
    api_init();
    port = api_settings_port();

    /* 3. Boot-entry model + auto-boot scheduling. */
    grub_load(&boot_cfg);
    autoboot_setup(&autoboot, &boot_cfg);
    grub_free(&boot_cfg);

    if (!g_running)
        return 0;                          /* autoBoot=orbis short-circuit */

    /* 4. HTTP server (embedded web UI + JSON API). */
    server_fd = httpd_listen(port);
    if (server_fd < 0) {
        util_log("fatal: cannot listen on port %d", port);
        util_notify("Failed to start the HTTP server");
        return 1;
    }

    /* 5. First-run home-screen bubble install (Media tab).  Best effort:
     *    missing/locked app.db is logged and skipped, never fatal. */
    appdb_install_if_needed();

    /* 6. Startup notifications: exactly these three, in this order. */
    util_notify("Welcome To PS5 Linux Manager");
    util_notify("credits: InsideMatrix");
    snprintf(note, sizeof(note), "Using port :%d", port);
    util_notify(note);

    /* 7. Serve loop: httpd_poll()/ftp_poll() do all network I/O; between
     *    iterations we check the auto-boot countdown and the shutdown
     *    flag.  ftp_poll() is called in a short burst so bulk transfers
     *    keep moving between HTTP iterations. */
    while (g_running) {
        int i;
        if (httpd_poll(server_fd, 100, api_handle, NULL) != 0)
            break;                         /* fatal socket error */
        for (i = 0; i < 8; i++)
            ftp_poll(5);
        autoboot_tick(&autoboot);
    }

    util_log("shutting down");
    ftp_shutdown();
    httpd_shutdown();
    return 0;
}

/*
 * Entry point: ps5-payload-sdk's crt1.o calls main() after the kernel
 * oracle and libc are initialized; no explicit payload args are needed
 * (payload_get_args() in <ps5/payload.h> would provide them).
 */
int main(void) {
    return manager_run();
}
