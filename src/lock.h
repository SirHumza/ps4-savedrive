/* lock.h - single-instance guard for the payload.
 *
 * A double payload send (two nc commands, or autorun plus a manual send)
 * must not run two backups at once: both would write the same zip and state
 * paths and could upload a corrupt zip that the manifest then skips forever.
 */
#ifndef SD_LOCK_H
#define SD_LOCK_H

/* Returns 0 when the lock was acquired by this process, 1 when another
 * instance appears to hold it, -1 when the lock file cannot be created or
 * inspected at all (caller may proceed cautiously). */
int sd_lock_acquire(const char *path);

/* Removes the lock if this process acquired it. Safe to call twice. */
void sd_lock_release(void);

#endif
