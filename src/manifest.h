#ifndef SD_MANIFEST_H
#define SD_MANIFEST_H

#include <stddef.h>

/* 1 = this user/title was uploaded with the same size+mtime, skip it. */
int sd_manifest_skip(const char *user, const char *title, size_t bytes,
                     long mtime);

/* Record a successful upload. Appends; latest matching line wins on skip. */
int sd_manifest_record(const char *user, const char *title, size_t bytes,
                       long mtime);

#endif
