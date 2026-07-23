/*
 * host_stubs.c — implementations of the host shims declared in
 * src/shims/host_shims.h, linked into the host smoke binary.
 *
 * Unlike a pure syntax-check stub, these RECORD behavior so the smoke
 * test can assert on it:
 *   - every sceKernelSendNotificationRequest() is captured (count+text)
 *   - sceKernelGetHwModelName() returns a controllable model code
 *   - sceRegMgrGetStr/GetBin report "not available" (the fallback path)
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STUB_MAX_NOTIFS 32

static char g_notifs[STUB_MAX_NOTIFS][256];
static int  g_notif_count;

int sceKernelSendNotificationRequest(int fd, char *buf, size_t len,
                                     int flags) {
    (void)fd;
    (void)flags;
    if (g_notif_count < STUB_MAX_NOTIFS) {
        if (len >= sizeof(g_notifs[0]))
            len = sizeof(g_notifs[0]) - 1;
        memcpy(g_notifs[g_notif_count], buf, len);
        g_notifs[g_notif_count][len] = '\0';
        g_notif_count++;
    }
    return 0;
}

/* Accessors used by tests/smoke.c. */
int stub_notif_count(void) {
    return g_notif_count;
}
const char *stub_notif(int i) {
    return (i >= 0 && i < g_notif_count) ? g_notifs[i] : "";
}
void stub_notif_reset(void) {
    g_notif_count = 0;
}

/* Kit type: plain retail console. */
int sceKernelIsDevKit(void) {
    return 0;
}
int sceKernelIsTestKit(void) {
    return 0;
}

/* Controllable hardware identity (default: a real retail model code). */
static char g_model[64] = "CFI-1215A";

int sceKernelGetHwModelName(char *name) {
    if (!name || !g_model[0])
        return -1;
    strcpy(name, g_model);
    return 0;
}
int sceKernelGetHwSerialNumber(char *serial) {
    if (serial)
        strcpy(serial, "0000000000000000");
    return 0;
}
void stub_set_model(const char *model) {
    snprintf(g_model, sizeof(g_model), "%s", model ? model : "");
}

/* Registry reads unavailable on the host: exercises the fallback. */
int sceRegMgrGetStr(unsigned int key, char *out, size_t outlen) {
    (void)key;
    (void)out;
    (void)outlen;
    return -1;
}
int sceRegMgrGetBin(unsigned int key, void *out, size_t outlen) {
    (void)key;
    (void)out;
    (void)outlen;
    return -1;
}

/* Firmware 4.51. */
uint32_t kernel_get_fw_version(void) {
    return 0x04510000u;
}
