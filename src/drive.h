#ifndef SD_DRIVE_H
#define SD_DRIVE_H

#include <stddef.h>

#include "sd_cfg.h"

/* Transport seam: production wraps mbedTLS (tls.c); host tests wrap a plain
 * socket. write returns bytes written or <0; read returns >0 bytes, 0 on EOF,
 * <0 on error. */
typedef struct {
  void *ctx;
  int (*write)(void *ctx, const void *buf, size_t len);
  int (*read)(void *ctx, void *buf, size_t cap);
} sd_io_t;

/* Uploads zip_path to Drive as drive_name using a resumable session.
 * 0 = uploaded, -1 = fatal (bad config/auth/4xx), -2 = retryable (network/5xx).
 * Never logs client_secret, refresh_token, access_token, or Authorization.
 *
 * The _state variant persists the session path, size and caller signature
 * (sig_bytes/sig_mtime, normally the savedata signature the zip was built
 * from) into state_path so a later run resumes the same session instead of
 * re-uploading. state_path may be NULL to disable. The state file is removed
 * on success, on fatal errors, and when dead; it is kept only for -2 so the
 * next boot can continue. */
int sd_drive_put_io_state(const sd_cfg_t *c, const char *zip_path,
                          const char *drive_name, sd_io_t *io,
                          const char *state_path, unsigned long long sig_bytes,
                          long sig_mtime);

/* Wrapper for callers with no session-state file. */
int sd_drive_put_io(const sd_cfg_t *c, const char *zip_path,
                    const char *drive_name, sd_io_t *io);

/* 1 when state_path parses, its signature equals sig_bytes/sig_mtime, and
 * zip_path exists with the size the state recorded. Pure local check, no
 * network. 0 otherwise. */
int sd_drive_state_resumable(const char *state_path, const char *zip_path,
                             unsigned long long sig_bytes, long sig_mtime);

#ifdef SD_PS4
/* Production entry: connects to Google over TLS and calls the state variant. */
int sd_drive_put(const sd_cfg_t *c, const char *zip_path,
                 const char *drive_name, const char *state_path,
                 unsigned long long sig_bytes, long sig_mtime);
#endif

#endif
