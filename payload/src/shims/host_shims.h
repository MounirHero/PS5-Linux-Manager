/*
 * host_shims.h — host-side shims for ps5-payload-sdk / Orbis APIs.
 *
 * This header exists ONLY so that every payload source file can be syntax
 * checked on a normal development host with:
 *
 *     gcc -fsyntax-only -std=c11 -Wall -DHOST_TEST -Isrc (each .c file)
 *
 * It is included from .c files inside `#ifdef HOST_TEST` blocks and is
 * never compiled into a target (PS5) build.  Only *prototypes* live here;
 * the implementations used by the host smoke binary (with notification
 * recording, controllable regmgr answers, etc.) are provided by
 * tests/host_stubs.c.  For `-fsyntax-only` only declarations matter.
 *
 * On target builds the real declarations are used instead (see the
 * explicit prototypes in the .c files / <ps5/kernel.h>).
 */
#ifndef PS5LM_HOST_SHIMS_H
#define PS5LM_HOST_SHIMS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* libkernel notification API (sceKernelSendNotificationRequest).
 * Classic usage on Orbis: fd 0, NUL-free text buffer, length, flags. */
int sceKernelSendNotificationRequest(int fd, char *buf, size_t len,
                                     int flags);

/* Kit type detection: non-zero when the console is a DevKit / TestKit. */
int sceKernelIsDevKit(void);
int sceKernelIsTestKit(void);

/* libkernel hardware identity queries. */
int sceKernelGetHwModelName(char *name);
int sceKernelGetHwSerialNumber(char *serial);

/* libSceRegMgr.sprx registry reads (see util.c for the key candidates). */
int sceRegMgrGetStr(unsigned int key, char *out, size_t outlen);
int sceRegMgrGetBin(unsigned int key, void *out, size_t outlen);

/* ps5-payload-sdk kernel oracle: packed firmware, e.g. 0x04510000 = 4.51. */
uint32_t kernel_get_fw_version(void);

#ifdef __cplusplus
}
#endif

#endif /* PS5LM_HOST_SHIMS_H */
