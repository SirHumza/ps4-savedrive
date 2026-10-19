#include "sd_cfg.h"

#include <stdio.h>
#include <string.h>

#include "jsonlite.h"

#define SD_CFG_FILE_MAX 8192

static int copy_field(const jl_val_t *root, const char *key, char *dst,
                      size_t dst_size) {
  const jl_val_t *v = jl_obj_get(root, key);
  if (!v || v->type != JL_STRING || !v->str || v->strlen == 0)
    return -1;
  if (v->strlen >= dst_size || memchr(v->str, '\0', v->strlen))
    return -1;
  memcpy(dst, v->str, v->strlen);
  dst[v->strlen] = '\0';
  return 0;
}

int sd_cfg_load(const char *path, sd_cfg_t *c) {
  char buf[SD_CFG_FILE_MAX];
  size_t len;
  jl_val_t *root;
  FILE *f;

  if (!path || !c)
    return -1;
  memset(c, 0, sizeof *c);

  f = fopen(path, "rb");
  if (!f)
    return -1;
  len = fread(buf, 1, sizeof buf - 1, f);
  if (ferror(f) || len == sizeof buf - 1) {
    fclose(f);
    return -1;
  }
  fclose(f);
  buf[len] = '\0';

  root = jl_parse(buf, len);
  if (!root || root->type != JL_OBJECT) {
    jl_free(root);
    return -1;
  }

  if (copy_field(root, "client_id", c->client_id, sizeof c->client_id) != 0 ||
      copy_field(root, "client_secret", c->client_secret,
                 sizeof c->client_secret) != 0 ||
      copy_field(root, "refresh_token", c->refresh_token,
                 sizeof c->refresh_token) != 0 ||
      copy_field(root, "folder_id", c->folder_id, sizeof c->folder_id) != 0) {
    memset(c, 0, sizeof *c);
    jl_free(root);
    return -1;
  }

  jl_free(root);
  return 0;
}

const char *sd_cfg_redact(const char *secret) {
  return (secret && *secret) ? "***" : "(none)";
}
