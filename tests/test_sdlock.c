/* test_sdlock.c - host tests for the payload single-instance guard.
 *
 * The guard must say "busy" for a live holder (this process or a child),
 * say "take over" for a dead pid, an ancient timestamp or junk contents,
 * and release idempotently. Run: make -C tests test_sdlock */
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../src/lock.h"

static char dir[256];

static void lock_path(char *out, size_t cap) {
  snprintf(out, cap, "%s/run.lock", dir);
}

static void write_raw(const char *p, const char *s) {
  int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  assert(fd >= 0);
  assert(write(fd, s, strlen(s)) == (ssize_t)strlen(s));
  close(fd);
}

int main(void) {
  char lock[256];
  char raw[64];

  snprintf(dir, sizeof dir, "/tmp/sdlocktest.%ld", (long)getpid());
  assert(mkdir(dir, 0700) == 0);
  lock_path(lock, sizeof lock);

  /* Fresh acquire, a second acquire by the same live pid, then release. */
  assert(sd_lock_acquire(lock) == 0);
  assert(sd_lock_acquire(lock) == 1);
  sd_lock_release();
  assert(sd_lock_acquire(lock) == 0);
  sd_lock_release();

  /* A separate live process holds it. */
  {
    int fds[2];
    pid_t c;
    char b = 0;

    assert(pipe(fds) == 0);
    c = fork();
    assert(c >= 0);
    if (c == 0) {
      close(fds[0]);
      if (sd_lock_acquire(lock) != 0)
        _exit(2);
      if (write(fds[1], "x", 1) != 1)
        _exit(3);
      sleep(30);
      _exit(0);
    }
    close(fds[1]);
    assert(read(fds[0], &b, 1) == 1);
    close(fds[0]);
    assert(sd_lock_acquire(lock) == 1);
    kill(c, SIGKILL);
    waitpid(c, NULL, 0);
    unlink(lock); /* SIGKILL skips the child's release */
  }
  assert(sd_lock_acquire(lock) == 0);
  sd_lock_release();

  /* Dead pid -> takeover. 999999 is above any live host pid here. */
  snprintf(raw, sizeof raw, "%d %ld\n", 999999, (long)time(NULL));
  write_raw(lock, raw);
  assert(sd_lock_acquire(lock) == 0);
  sd_lock_release();

  /* Live pid but a timestamp past the stale bound (pid reuse guard). */
  snprintf(raw, sizeof raw, "%ld %ld\n", (long)getpid(),
           (long)time(NULL) - 25 * 60 * 60);
  write_raw(lock, raw);
  assert(sd_lock_acquire(lock) == 0);
  sd_lock_release();

  /* Live pid, fresh timestamp -> held. */
  snprintf(raw, sizeof raw, "%ld %ld\n", (long)getpid(), (long)time(NULL));
  write_raw(lock, raw);
  assert(sd_lock_acquire(lock) == 1);
  unlink(lock);

  /* Live pid but an unusable (missing or zero) timestamp -> takeover. */
  snprintf(raw, sizeof raw, "%ld\n", (long)getpid());
  write_raw(lock, raw);
  assert(sd_lock_acquire(lock) == 0);
  sd_lock_release();
  snprintf(raw, sizeof raw, "%ld 0\n", (long)getpid());
  write_raw(lock, raw);
  assert(sd_lock_acquire(lock) == 0);
  sd_lock_release();

  /* Live pid, timestamp far in the future (clock rollback) -> takeover. */
  snprintf(raw, sizeof raw, "%ld %ld\n", (long)getpid(),
           (long)time(NULL) + 25 * 60 * 60);
  write_raw(lock, raw);
  assert(sd_lock_acquire(lock) == 0);

  /* Release must not unlink a lock that belongs to another pid. */
  snprintf(raw, sizeof raw, "999999 %ld\n", (long)time(NULL));
  write_raw(lock, raw);
  sd_lock_release();
  assert(access(lock, F_OK) == 0);
  unlink(lock);

  /* Malformed or empty contents -> takeover. */
  write_raw(lock, "junk");
  assert(sd_lock_acquire(lock) == 0);
  sd_lock_release();
  write_raw(lock, "");
  assert(sd_lock_acquire(lock) == 0);
  sd_lock_release();

  /* Overlong path is refused, and release is idempotent. */
  {
    char big[300];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = 0;
    assert(sd_lock_acquire(big) == -1);
  }
  sd_lock_release();

  unlink(lock);
  rmdir(dir);
  printf("test_sdlock: PASS\n");
  return 0;
}
