/*
 * ftpsrv.h — minimal but real embedded FTP daemon (contract v1.1).
 *
 * Poll-based and single-threaded, exactly like httpd.c: main() calls
 * ftp_poll() between HTTP iterations.  Passive data connections only
 * (PASV/EPSV), no TLS.  Full filesystem access (paths are normalized
 * with fsapi_normalize).  Anonymous login when no user is configured,
 * otherwise USER/PASS auth.
 *
 * Supported commands: USER PASS QUIT NOOP SYST TYPE PWD XPWD CWD CDUP
 * SIZE MLSD MLST LIST NLST PASV EPSV RETR STOR DELE RMD MKD RNFR RNTO.
 */
#ifndef PS5LM_FTPSRV_H
#define PS5LM_FTPSRV_H

#ifdef __cplusplus
extern "C" {
#endif

#define FTP_USER_MAX 32
#define FTP_PASS_MAX 64

typedef struct {
    int  enabled;                 /* daemon on/off                    */
    int  port;                    /* control port (2121 default)      */
    char user[FTP_USER_MAX];      /* empty = anonymous login allowed  */
    char pass[FTP_PASS_MAX];      /* password (empty + user = any)    */
} ftp_config_t;

/*
 * Apply a new configuration; the daemon is fully restarted (existing
 * sessions are dropped).  Safe to call at any time.
 */
void ftp_configure(const ftp_config_t *cfg);

/* Current configuration (never NULL). */
void ftp_get_config(ftp_config_t *cfg);

/* One poll iteration of the daemon; `timeout_ms` bounds the wait. */
int ftp_poll(int timeout_ms);

/* Drop every session and close the listen socket. */
void ftp_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PS5LM_FTPSRV_H */
