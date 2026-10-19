CC ?= cc
CFLAGS ?= -Wall -Wextra -Werror -O1 -g

.PHONY: test test_sd_cfg test_sdzip test_sddrive test_sdlock clean

MINIZ := ../src/miniz.c ../src/miniz_tdef.c ../src/miniz_tinfl.c ../src/miniz_zip.c

test: test_sd_cfg test_sdzip test_sddrive test_sdlock

test_sd_cfg: test_sd_cfg.c ../src/sd_cfg.c ../src/jsonlite.c
	$(CC) $(CFLAGS) -I../src -o $@ test_sd_cfg.c ../src/sd_cfg.c ../src/jsonlite.c
	./$@

test_sdzip: test_sdzip.c ../src/zip.c ../src/manifest.c $(MINIZ)
	$(CC) $(CFLAGS) -I../src -o $@ test_sdzip.c ../src/zip.c ../src/manifest.c $(MINIZ) -lpthread
	./$@

test_sddrive: test_sddrive.c ../src/drive.c ../src/jsonlite.c ../src/log.c
	$(CC) $(CFLAGS) -DSD_DRIVE_CHUNK=64 -I../src -o $@ test_sddrive.c ../src/drive.c ../src/jsonlite.c ../src/log.c -lpthread
	./$@

test_sdlock: test_sdlock.c ../src/lock.c
	$(CC) $(CFLAGS) -I../src -o $@ test_sdlock.c ../src/lock.c
	./$@

clean:
	rm -f test_sd_cfg test_sdzip test_sddrive test_sdlock
