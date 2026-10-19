/* lock.c - single-instance guard (see lock.h).
 *
 * The lock file holds "<pid> <unix_time>". A lock is stale when its pid is
 * gone (kill(pid, 0) -> ESRCH), when its timestamp is unusable, when the
 * timestamp is implausibly far in the future (clock rollback), or when it is
 * older than SD_LOCK_STALE_SECS (bounds the damage from pid reuse after a
 * crash). Anything unreadable or malformed is treated as stale: the worst
 * case of a wrong takeover is the concurrent run we are guarding against,
 * the worst case of a stuck lock is a silent permanent backup outage.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lock.h"

#define SD_LOCK_STALE_SECS (24L * 60 * 60)
#define SD_LOCK_FUTURE_SECS 300L

static char lock_path[256];

static long parse_long(const char **sp) {
  const char *s = *sp;
  long v = 0;
  int any = 0;

  while (*s == ' ' || *s == '\t')
    s++;
  while (*s >= '0' && *s <= '9') {
    if (v > (LONG_MAX - 9) / 10)
      return -1;
    v = v * 10 + (*s - '0');
    s++;
    any = 1;
  }
  *sp = s;
  return any ? v : -1;
}

static int pid_alive(long pid) {
  if (pid <= 0)
    return 0;
  if (kill((pid_t)pid, 0) == 0)
    return 1;
  return errno == EPERM;
}

/* 1 = still held, 0 = stale. */
static int lock_alive(const char *contents) {
  const char *p = contents;
  long pid = parse_long(&p);
  long ts = parse_long(&p);

  if (pid_alive(pid)) {
    long now = (long)time(NULL);
    if (ts <= 0 || ts - now > SD_LOCK_FUTURE_SECS)
      return 0;
    if (now - ts > SD_LOCK_STALE_SECS)
      return 0;
    return 1;
  }
  return 0;
}

int sd_lock_acquire(const char *path) {
  int attempt;

  if (!path || strlen(path) >= sizeof lock_path)
    return -1;

  for (attempt = 0; attempt < 2; attempt++) {
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd >= 0) {
      char buf[64];
      int n = snprintf(buf, sizeof buf, "%ld %ld\n", (long)getpid(),
                       (long)time(NULL));
      if (n < 0 || write(fd, buf, (size_t)n) != n) {
        close(fd);
        unlink(path);
        return -1;
      }
      close(fd);
      strncpy(lock_path, path, sizeof lock_path - 1);
      lock_path[sizeof lock_path - 1] = 0;
      return 0;
    }
    if (errno != EEXIST)
      return -1;
    {
      char buf[128];
      ssize_t r = -1;
      int rfd = open(path, O_RDONLY);
      if (rfd >= 0) {
        r = read(rfd, buf, sizeof buf - 1);
        close(rfd);
      }
      buf[r > 0 ? r : 0] = 0;
      if (r > 0 && lock_alive(buf))
        return 1;
      unlink(path);
    }
  }
  return -1;
}

void sd_lock_release(void) {
  if (lock_path[0]) {
    char buf[64];
    int fd = open(lock_path, O_RDONLY);
    if (fd >= 0) {
      ssize_t n = read(fd, buf, sizeof buf - 1);
      const char *p = buf;
      close(fd);
      buf[n > 0 ? n : 0] = 0;
      if (parse_long(&p) == (long)getpid())
        unlink(lock_path);
    }
    lock_path[0] = 0;
  }
}
