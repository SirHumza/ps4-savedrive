#ifndef SD_CFG_H
#define SD_CFG_H

#include <stddef.h>

#define SD_CFG_CLIENT_ID_MAX 128
#define SD_CFG_CLIENT_SECRET_MAX 128
#define SD_CFG_REFRESH_TOKEN_MAX 512
#define SD_CFG_FOLDER_ID_MAX 128

typedef struct {
  char client_id[SD_CFG_CLIENT_ID_MAX];
  char client_secret[SD_CFG_CLIENT_SECRET_MAX];
  char refresh_token[SD_CFG_REFRESH_TOKEN_MAX];
  char folder_id[SD_CFG_FOLDER_ID_MAX];
} sd_cfg_t;

/* 0 on success, -1 on missing file, bad JSON, missing field or overlong value. */
int sd_cfg_load(const char *path, sd_cfg_t *c);

/* Safe value for log lines: never returns the secret itself. */
const char *sd_cfg_redact(const char *secret);

#endif
