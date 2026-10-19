#ifndef SD_ZIP_H
#define SD_ZIP_H

/* Zips every regular file under src_dir into out_zip.
 * Returns 0 on success, -1 on error, empty dir, or a file changed mid-read
 * (the partial zip is removed). */
int sd_zip_dir(const char *src_dir, const char *out_zip);

/* Convenience wrapper: /user/home/<user>/savedata/<title>. */
int sd_zip_title(const char *user, const char *title, const char *out_zip);

#endif
