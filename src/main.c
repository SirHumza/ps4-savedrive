/* savedrive payload: zip every save title and upload it to Google Drive.
 *
 * Runs once per jailbreak from the payload autorun list. It never blocks
 * boot: every failure is logged and the payload still exits 0. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "drive.h"
#include "log.h"
#include "manifest.h"
#include "sd_cfg.h"
#include "zip.h"

#define SD_HOME "/user/home"
#define SD_DATA "/data/savedrive"
#define SD_STATE_DIR SD_DATA "/state"
#define SD_CFG_FILE SD_DATA "/config.json"
#define SD_LOG_FILE SD_DATA "/run.log"
#define SD_PATH_MAX 1024

/* PS4 libc has the symbol but not always a declaration for it. */
extern struct tm *gmtime_r(const time_t *, struct tm *);

/* Signature of a title: total bytes of regular files plus the newest mtime.
 * lstat everywhere so a symlink is never followed. */
#define SD_WALK_MAX_DEPTH 64

/* Names become manifest fields, state fields and Drive file names, all of
 * which are whitespace-delimited or line-based. Real user ids and title ids
 * are alphanumeric; anything else is skipped rather than risk corruption. */
static int name_ok(const char *s) {
  if (!s || !*s)
    return 0;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (!(isalnum(c) || c == '-' || c == '_' || c == '.'))
      return 0;
  }
  return 1;
}

static int sig_walk(const char *path, size_t *bytes, long *mtime, int depth) {
  DIR *d;
  struct dirent *de;

  d = opendir(path);
  if (!d || depth > SD_WALK_MAX_DEPTH)
    return -1;
  while ((de = readdir(d)) != NULL) {
    char child[SD_PATH_MAX];
    struct stat st;

    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
      continue;
    if (snprintf(child, sizeof child, "%s/%s", path, de->d_name) >=
        (int)sizeof child)
      continue;
    if (lstat(child, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      if (sig_walk(child, bytes, mtime, depth + 1) != 0) {
        closedir(d);
        return -1;
      }
    } else if (S_ISREG(st.st_mode)) {
      *bytes += (size_t)st.st_size;
      if (st.st_mtime > *mtime)
        *mtime = st.st_mtime;
    }
  }
  closedir(d);
  return 0;
}

static void handle_title(const sd_cfg_t *cfg, const char *user,
                         const char *title, int *nzip, int *nskip,
                         int *nfail) {
  char src[SD_PATH_MAX];
  char zip_path[SD_PATH_MAX];
  char state_path[SD_PATH_MAX];
  char drive_name[SD_PATH_MAX];
  size_t bytes = 0;
  long mtime = 0;
  time_t now;
  struct tm tmv = {0};
  int rc, resume;

  if (snprintf(src, sizeof src, SD_HOME "/%s/savedata/%s", user, title) >=
      (int)sizeof src) {
    log_msg("TITLE %s+%s fail 0", user, title);
    (*nfail)++;
    return;
  }

  if (sig_walk(src, &bytes, &mtime, 0) != 0) {
    log_msg("TITLE %s+%s fail %zu", user, title, bytes);
    (*nfail)++;
    return;
  }

  if (bytes == 0) {
    log_msg("TITLE %s+%s skip 0", user, title);
    (*nskip)++;
    return;
  }

  if (sd_manifest_skip(user, title, bytes, mtime) == 1) {
    log_msg("TITLE %s+%s skip %zu", user, title, bytes);
    (*nskip)++;
    return;
  }

  if (snprintf(zip_path, sizeof zip_path, SD_DATA "/%s+%s.zip", user, title) >=
          (int)sizeof zip_path ||
      snprintf(state_path, sizeof state_path, SD_STATE_DIR "/%s+%s.state", user,
               title) >= (int)sizeof state_path) {
    log_msg("TITLE %s+%s fail %zu", user, title, bytes);
    (*nfail)++;
    return;
  }

  /* Resume an interrupted upload of the same saves; otherwise drop any
   * leftovers from a different save state and build a fresh zip. */
  resume = sd_drive_state_resumable(state_path, zip_path, bytes, mtime);
  if (resume) {
    log_msg("TITLE %s+%s resume %zu", user, title, bytes);
  } else {
    remove(state_path);
    remove(zip_path);
    if (sd_zip_title(user, title, zip_path) != 0) {
      log_msg("TITLE %s+%s fail %zu", user, title, bytes);
      (*nfail)++;
      return;
    }
  }

  now = time(NULL);
  if (!gmtime_r(&now, &tmv)) {
    log_msg("TITLE %s+%s fail %zu", user, title, bytes);
    (*nfail)++;
    remove(zip_path);
    return;
  }
  if (snprintf(drive_name, sizeof drive_name, "%s+%s+%04d-%02d-%02d.zip",
               user, title, tmv.tm_year + 1900, tmv.tm_mon + 1,
               tmv.tm_mday) >= (int)sizeof drive_name) {
    log_msg("TITLE %s+%s fail %zu", user, title, bytes);
    (*nfail)++;
    remove(zip_path);
    return;
  }

  rc = sd_drive_put(cfg, zip_path, drive_name, state_path,
                    (unsigned long long)bytes, mtime);
  if (rc == 0) {
    sd_manifest_record(user, title, bytes, mtime);
    remove(zip_path);
    log_msg("TITLE %s+%s zip %zu", user, title, bytes);
    (*nzip)++;
  } else if (rc == -2) {
    log_msg("TITLE %s+%s retry %zu", user, title, bytes);
    (*nfail)++;
  } else {
    log_msg("TITLE %s+%s fail %zu", user, title, bytes);
    (*nfail)++;
  }
}

static void walk_savedata(const sd_cfg_t *cfg, const char *user, int *nzip,
                          int *nskip, int *nfail) {
  char dir[SD_PATH_MAX];
  DIR *d;
  struct dirent *de;

  if (snprintf(dir, sizeof dir, SD_HOME "/%s/savedata", user) >=
      (int)sizeof dir)
    return;
  d = opendir(dir);
  if (!d)
    return;
  while ((de = readdir(d)) != NULL) {
    char path[SD_PATH_MAX];
    struct stat st;

    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
      continue;
    if (snprintf(path, sizeof path, "%s/%s", dir, de->d_name) >=
        (int)sizeof path)
      continue;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode))
      continue;
    if (!name_ok(de->d_name)) {
      log_msg("WALK skip name %s", de->d_name);
      continue;
    }
    handle_title(cfg, user, de->d_name, nzip, nskip, nfail);
  }
  closedir(d);
}

int main(void) {
  sd_cfg_t cfg;
  DIR *home;
  struct dirent *de;
  int nzip = 0;
  int nskip = 0;
  int nfail = 0;

  mkdir(SD_DATA, 0700);
  mkdir(SD_STATE_DIR, 0700);
  log_init(SD_LOG_FILE);

  if (sd_cfg_load(SD_CFG_FILE, &cfg) != 0) {
    log_msg("config %s unavailable", SD_CFG_FILE);
    return 0;
  }

  home = opendir(SD_HOME);
  if (home) {
    while ((de = readdir(home)) != NULL) {
      char path[SD_PATH_MAX];
      struct stat st;

      if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
        continue;
      if (snprintf(path, sizeof path, SD_HOME "/%s", de->d_name) >=
          (int)sizeof path)
        continue;
      if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        continue;
      if (!name_ok(de->d_name)) {
        log_msg("WALK skip name %s", de->d_name);
        continue;
      }
      walk_savedata(&cfg, de->d_name, &nzip, &nskip, &nfail);
    }
    closedir(home);
  }

  log_msg("SUMMARY zip=%d skip=%d fail=%d", nzip, nskip, nfail);
  return 0;
}
