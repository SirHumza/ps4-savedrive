#include "manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SD_MANIFEST_DEFAULT "/data/savedrive/manifest.txt"
#define SD_MANIFEST_LINE 1200

static const char *manifest_path(void) {
  const char *env = getenv("SD_MANIFEST_PATH");
  return (env && *env) ? env : SD_MANIFEST_DEFAULT;
}

int sd_manifest_skip(const char *user, const char *title, size_t bytes,
                     long mtime) {
  char line[SD_MANIFEST_LINE];
  char key[SD_MANIFEST_LINE];
  FILE *f;
  int skip = 0;

  if (!user || !title)
    return 0;
  snprintf(key, sizeof key, "%s/%s", user, title);

  f = fopen(manifest_path(), "r");
  if (!f)
    return 0;

  while (fgets(line, sizeof line, f)) {
    char seen_key[SD_MANIFEST_LINE];
    unsigned long long seen_bytes;
    long seen_mtime;

    if (sscanf(line, "%1199s %llu %ld", seen_key, &seen_bytes, &seen_mtime) !=
        3)
      continue;
    if (strcmp(seen_key, key) != 0)
      continue;
    skip = ((unsigned long long)bytes == seen_bytes && mtime == seen_mtime);
  }
  fclose(f);
  return skip;
}

int sd_manifest_record(const char *user, const char *title, size_t bytes,
                       long mtime) {
  FILE *f;

  if (!user || !title)
    return -1;
  mkdir("/data/savedrive", 0777);
  f = fopen(manifest_path(), "a");
  if (!f)
    return -1;
  fprintf(f, "%s/%s %llu %ld\n", user, title, (unsigned long long)bytes, mtime);
  fclose(f);
  return 0;
}
