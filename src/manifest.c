#include "manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SD_MANIFEST_DEFAULT "/data/savedrive/manifest.txt"
#define SD_MANIFEST_LINE 1200
#define SD_MANIFEST_NAME_MAX 512

static const char *manifest_path(void) {
  const char *env = getenv("SD_MANIFEST_PATH");
  return (env && *env) ? env : SD_MANIFEST_DEFAULT;
}

/* One line per record: "<user> <title> <bytes> <mtime>". Separate fields so
 * a '/' in either name cannot collide two different titles. */
int sd_manifest_skip(const char *user, const char *title, size_t bytes,
                     long mtime) {
  char line[SD_MANIFEST_LINE];
  FILE *f;
  int skip = 0;

  if (!user || !title)
    return 0;

  f = fopen(manifest_path(), "r");
  if (!f)
    return 0;

  while (fgets(line, sizeof line, f)) {
    char seen_user[SD_MANIFEST_NAME_MAX];
    char seen_title[SD_MANIFEST_NAME_MAX];
    unsigned long long seen_bytes;
    long seen_mtime;

    if (sscanf(line, "%511s %511s %llu %ld", seen_user, seen_title,
               &seen_bytes, &seen_mtime) != 4)
      continue;
    if (strcmp(seen_user, user) != 0 || strcmp(seen_title, title) != 0)
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
  if (strlen(user) >= SD_MANIFEST_NAME_MAX ||
      strlen(title) >= SD_MANIFEST_NAME_MAX)
    return -1;
  mkdir("/data/savedrive", 0700);
  f = fopen(manifest_path(), "a");
  if (!f)
    return -1;
  fprintf(f, "%s %s %llu %ld\n", user, title, (unsigned long long)bytes,
          mtime);
  fclose(f);
  return 0;
}
