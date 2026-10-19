// sdprobe: walk /user/home/*/savedata/* read-only, report per-title
// file counts + byte totals to /data/savedrive/probe.txt.
// Line format: USER/TITLE/FILES/BYTES/READ_OK
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PROBE_PATH_MAX 1024
#define PROBE_MAX_DEPTH 64

static int walk(const char *dir, long *files, long long *bytes, int *ok,
                int depth) {
  DIR *d = opendir(dir);
  struct dirent *e;
  char path[PROBE_PATH_MAX];

  if (!d || depth > PROBE_MAX_DEPTH) {
    *ok = 0;
    return -1;
  }
  while ((e = readdir(d))) {
    struct stat st;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >=
        (int)sizeof(path)) {
      *ok = 0;
      continue;
    }
    if (lstat(path, &st) != 0) {
      *ok = 0;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      walk(path, files, bytes, ok, depth + 1);
    } else if (S_ISREG(st.st_mode)) {
      FILE *f = fopen(path, "rb");
      if (!f) {
        *ok = 0;
        continue;
      }
      fclose(f);
      (*files)++;
      *bytes += st.st_size;
    }
  }
  closedir(d);
  return 0;
}

int main(void) {
  FILE *out;
  DIR *home;
  struct dirent *u;
  char spath[PROBE_PATH_MAX], tpath[PROBE_PATH_MAX];

  mkdir("/data/savedrive", 0700);
  out = fopen("/data/savedrive/probe.txt", "w");
  if (!out)
    return 1;
  home = opendir("/user/home");
  if (!home) {
    fprintf(out, "ERROR: cannot open /user/home\n");
    fclose(out);
    return 1;
  }
  while ((u = readdir(home))) {
    DIR *sd;
    struct dirent *t;
    if (!strcmp(u->d_name, ".") || !strcmp(u->d_name, ".."))
      continue;
    if (snprintf(spath, sizeof(spath), "/user/home/%s/savedata", u->d_name) >=
        (int)sizeof(spath))
      continue;
    sd = opendir(spath);
    if (!sd) {
      fprintf(out, "%s/-/0/0/NO_SAVEDATA_DIR\n", u->d_name);
      continue;
    }
    while ((t = readdir(sd))) {
      struct stat st;
      long files = 0;
      long long bytes = 0;
      int ok = 1;
      if (!strcmp(t->d_name, ".") || !strcmp(t->d_name, ".."))
        continue;
      if (snprintf(tpath, sizeof(tpath), "%s/%s", spath, t->d_name) >=
          (int)sizeof(tpath))
        continue;
      if (lstat(tpath, &st) != 0 || !S_ISDIR(st.st_mode))
        continue;
      walk(tpath, &files, &bytes, &ok, 0);
      fprintf(out, "%s/%s/%ld/%lld/%s\n", u->d_name, t->d_name, files,
              bytes, ok ? "READ_OK" : "READ_FAIL");
    }
    closedir(sd);
  }
  closedir(home);
  fclose(out);
  return 0;
}
