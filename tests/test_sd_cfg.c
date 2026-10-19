#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/sd_cfg.h"

static char *write_tmp(const char *body) {
  char *path = strdup("/tmp/sdcfgXXXXXX");
  int fd = mkstemp(path);
  assert(fd >= 0);
  FILE *f = fdopen(fd, "w");
  assert(f);
  fputs(body, f);
  fclose(f);
  return path;
}

int main(void) {
  sd_cfg_t c;

  char *missing =
      write_tmp("{\"client_id\":\"id\",\"client_secret\":\"secret\","
                "\"folder_id\":\"folder\"}");
  assert(sd_cfg_load(missing, &c) == -1);
  unlink(missing);
  free(missing);

  char *empty_token =
      write_tmp("{\"client_id\":\"id\",\"client_secret\":\"secret\","
                "\"refresh_token\":\"\",\"folder_id\":\"folder\"}");
  assert(sd_cfg_load(empty_token, &c) == -1);
  unlink(empty_token);
  free(empty_token);

  char *bad_json = write_tmp("{\"client_id\":");
  assert(sd_cfg_load(bad_json, &c) == -1);
  unlink(bad_json);
  free(bad_json);

  char *ok =
      write_tmp("{\"client_id\":\"id\",\"client_secret\":\"secret\","
                "\"refresh_token\":\"refresh\",\"folder_id\":\"folder\"}");
  assert(sd_cfg_load(ok, &c) == 0);
  assert(strcmp(c.client_id, "id") == 0);
  assert(strcmp(c.client_secret, "secret") == 0);
  assert(strcmp(c.refresh_token, "refresh") == 0);
  assert(strcmp(c.folder_id, "folder") == 0);
  unlink(ok);
  free(ok);

  assert(sd_cfg_load("/nonexistent/config.json", &c) == -1);
  assert(sd_cfg_redact(c.refresh_token) != NULL);

  puts("PASS test_sd_cfg");
  return 0;
}
