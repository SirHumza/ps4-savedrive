#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/manifest.h"
#include "../src/miniz.h"
#include "../src/zip.h"

static void put(const char *path, const char *body, int len) {
  FILE *f = fopen(path, "wb");
  assert(f);
  assert(fwrite(body, 1, len, f) == (size_t)len);
  fclose(f);
}

static void check_zip(const char *zip) {
  mz_zip_archive z;
  mz_zip_archive_file_stat st;
  int idx;

  memset(&z, 0, sizeof z);
  assert(mz_zip_reader_init_file(&z, zip, 0));
  assert(mz_zip_reader_get_num_files(&z) == 3);

  idx = mz_zip_reader_locate_file(&z, "a.txt", NULL, 0);
  assert(idx >= 0);
  assert(mz_zip_reader_file_stat(&z, idx, &st));
  assert(st.m_uncomp_size == 5);

  idx = mz_zip_reader_locate_file(&z, "b.bin", NULL, 0);
  assert(idx >= 0);
  assert(mz_zip_reader_file_stat(&z, idx, &st));
  assert(st.m_uncomp_size == 100);

  idx = mz_zip_reader_locate_file(&z, "sub/c.dat", NULL, 0);
  assert(idx >= 0);
  assert(mz_zip_reader_file_stat(&z, idx, &st));
  assert(st.m_uncomp_size == 7);

  mz_zip_reader_end(&z);
}

int main(void) {
  char root[] = "/tmp/sdzipXXXXXX";
  char dir[512], sub[512], zip[512], empty[512], manifest[512], cmd[768];
  char body[100];

  assert(mkdtemp(root));
  snprintf(dir, sizeof dir, "%s/fixture", root);
  snprintf(sub, sizeof sub, "%s/sub", dir);
  snprintf(zip, sizeof zip, "%s/out.zip", root);
  snprintf(empty, sizeof empty, "%s/empty", root);
  snprintf(manifest, sizeof manifest, "%s/manifest.txt", root);
  assert(mkdir(dir, 0700) == 0);
  assert(mkdir(sub, 0700) == 0);
  assert(mkdir(empty, 0700) == 0);

  put("/tmp/sdzip_a", "", 0);
  {
    char p[512];
    snprintf(p, sizeof p, "%s/a.txt", dir);
    put(p, "hello", 5);
    snprintf(p, sizeof p, "%s/b.bin", dir);
    memset(body, 'x', sizeof body);
    put(p, body, 100);
    snprintf(p, sizeof p, "%s/c.dat", sub);
    put(p, "1234567", 7);
  }

  assert(sd_zip_dir(dir, zip) == 0);
  check_zip(zip);

  assert(sd_zip_dir(empty, zip) == -1);
  assert(access(zip, F_OK) != 0);

  assert(setenv("SD_MANIFEST_PATH", manifest, 1) == 0);
  assert(sd_manifest_skip("1898cd02", "CUSA00001", 112, 1000) == 0);
  assert(sd_manifest_record("1898cd02", "CUSA00001", 112, 1000) == 0);
  assert(sd_manifest_skip("1898cd02", "CUSA00001", 112, 1000) == 1);
  assert(sd_manifest_skip("1898cd02", "CUSA00001", 113, 1000) == 0);
  assert(sd_manifest_skip("1898cd02", "CUSA00001", 112, 1001) == 0);
  assert(sd_manifest_record("1898cd02", "CUSA00001", 112, 1001) == 0);
  assert(sd_manifest_skip("1898cd02", "CUSA00001", 112, 1001) == 1);

  snprintf(cmd, sizeof cmd, "rm -rf '%s' /tmp/sdzip_a", root);
  assert(system(cmd) == 0);

  puts("PASS test_sdzip");
  return 0;
}
