#include "zip.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "miniz.h"

#define SD_ZIP_MAX_FILES 8192
#define SD_ZIP_PATH_MAX 1024

typedef struct {
  char rel[SD_ZIP_PATH_MAX];
  char full[SD_ZIP_PATH_MAX];
  long long size;
  long long mtime;
} entry_t;

typedef struct {
  entry_t *v;
  size_t n;
  size_t cap;
} entries_t;

static int add_entry(entries_t *e, const char *full, const char *rel,
                     const struct stat *st) {
  if (e->n == SD_ZIP_MAX_FILES)
    return -1;
  if (e->n == e->cap) {
    size_t cap = e->cap ? e->cap * 2 : 64;
    entry_t *v = realloc(e->v, cap * sizeof *v);
    if (!v)
      return -1;
    e->v = v;
    e->cap = cap;
  }
  if (strlen(rel) >= sizeof e->v[e->n].rel)
    return -1;
  strcpy(e->v[e->n].rel, rel);
  strcpy(e->v[e->n].full, full);
  e->v[e->n].size = (long long)st->st_size;
  e->v[e->n].mtime = (long long)st->st_mtime;
  e->n++;
  return 0;
}

static int walk(entries_t *e, const char *dir, const char *prefix) {
  DIR *d = opendir(dir);
  struct dirent *de;

  if (!d)
    return -1;
  while ((de = readdir(d))) {
    char full[SD_ZIP_PATH_MAX];
    char rel[SD_ZIP_PATH_MAX];
    struct stat st;

    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
      continue;
    if (snprintf(full, sizeof full, "%s/%s", dir, de->d_name) >=
        (int)sizeof full)
      continue;
    if (prefix && *prefix)
      snprintf(rel, sizeof rel, "%s/%s", prefix, de->d_name);
    else
      snprintf(rel, sizeof rel, "%s", de->d_name);

    if (lstat(full, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      if (walk(e, full, rel) != 0) {
        closedir(d);
        return -1;
      }
    } else if (S_ISREG(st.st_mode)) {
      if (add_entry(e, full, rel, &st) != 0) {
        closedir(d);
        return -1;
      }
    }
  }
  closedir(d);
  return 0;
}

static int unchanged(const entries_t *e) {
  size_t i;

  for (i = 0; i < e->n; i++) {
    struct stat st;
    if (lstat(e->v[i].full, &st) != 0)
      return 0;
    if (!S_ISREG(st.st_mode) || (long long)st.st_size != e->v[i].size ||
        (long long)st.st_mtime != e->v[i].mtime)
      return 0;
  }
  return 1;
}

static int write_zip(const entries_t *e, const char *out_zip) {
  mz_zip_archive z;
  size_t i;
  int ok = 0;

  memset(&z, 0, sizeof z);
  if (!mz_zip_writer_init_file(&z, out_zip, 0))
    return -1;

  for (i = 0; i < e->n; i++) {
    if (!mz_zip_writer_add_file(&z, e->v[i].rel, e->v[i].full, NULL, 0,
                                MZ_DEFAULT_COMPRESSION)) {
      ok = -1;
      break;
    }
  }

  if (ok == 0 && !mz_zip_writer_finalize_archive(&z))
    ok = -1;
  mz_zip_writer_end(&z);
  return ok;
}

int sd_zip_dir(const char *src_dir, const char *out_zip) {
  entries_t e = {0};
  int rc;

  if (!src_dir || !out_zip)
    return -1;

  rc = walk(&e, src_dir, "");
  if (rc == 0 && e.n == 0)
    rc = -1;
  if (rc == 0)
    rc = write_zip(&e, out_zip);
  if (rc == 0 && !unchanged(&e))
    rc = -1;
  if (rc != 0)
    remove(out_zip);

  free(e.v);
  return rc;
}

int sd_zip_title(const char *user, const char *title, const char *out_zip) {
  char src[SD_ZIP_PATH_MAX];

  if (!user || !title || !out_zip)
    return -1;
  if (snprintf(src, sizeof src, "/user/home/%s/savedata/%s", user, title) >=
      (int)sizeof src)
    return -1;
  return sd_zip_dir(src, out_zip);
}
