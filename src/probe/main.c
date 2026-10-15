// sdprobe: walk /user/home/*/savedata/* read-only, report per-title
// file counts + byte totals to /data/savedrive/probe.txt.
// Line format: USER/TITLE/FILES/BYTES/READ_OK
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int walk(const char *dir, long *files, long long *bytes, int *ok) {
  DIR *d = opendir(dir);
  if (!d) {
    *ok = 0;
    return -1;
  }
  struct dirent *e;
  char path[1024];
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(path, &st) != 0) {
      *ok = 0;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      walk(path, files, bytes, ok);
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
  mkdir("/data/savedrive", 0777);
  FILE *out = fopen("/data/savedrive/probe.txt", "w");
  if (!out)
    return 1;
  DIR *home = opendir("/user/home");
  if (!home) {
    fprintf(out, "ERROR: cannot open /user/home\n");
    fclose(out);
    return 1;
  }
  struct dirent *u;
  char spath[1024], tpath[1024];
  while ((u = readdir(home))) {
    if (!strcmp(u->d_name, ".") || !strcmp(u->d_name, ".."))
      continue;
    snprintf(spath, sizeof(spath), "/user/home/%s/savedata", u->d_name);
    DIR *sd = opendir(spath);
    if (!sd) {
      fprintf(out, "%s/-/0/0/NO_SAVEDATA_DIR\n", u->d_name);
      continue;
    }
    struct dirent *t;
    while ((t = readdir(sd))) {
      if (!strcmp(t->d_name, ".") || !strcmp(t->d_name, ".."))
        continue;
      snprintf(tpath, sizeof(tpath), "%s/%s", spath, t->d_name);
      struct stat st;
      if (stat(tpath, &st) != 0 || !S_ISDIR(st.st_mode))
        continue;
      long files = 0;
      long long bytes = 0;
      int ok = 1;
      walk(tpath, &files, &bytes, &ok);
      fprintf(out, "%s/%s/%ld/%lld/%s\n", u->d_name, t->d_name, files,
              bytes, ok ? "READ_OK" : "READ_FAIL");
    }
    closedir(sd);
  }
  closedir(home);
  fclose(out);
  return 0;
}
