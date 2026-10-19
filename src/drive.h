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
 * Never logs client_secret, refresh_token, access_token, or Authorization. */
int sd_drive_put_io(const sd_cfg_t *c, const char *zip_path,
                    const char *drive_name, sd_io_t *io);

#ifdef SD_PS4
/* Production entry: connects to Google over TLS and calls sd_drive_put_io. */
int sd_drive_put(const sd_cfg_t *c, const char *zip_path,
                 const char *drive_name);
#endif

#endif
