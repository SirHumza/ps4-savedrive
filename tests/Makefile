CC ?= cc
CFLAGS ?= -Wall -Wextra -Werror -O1 -g

.PHONY: test test_sd_cfg test_sdzip clean

MINIZ := ../src/miniz.c ../src/miniz_tdef.c ../src/miniz_tinfl.c ../src/miniz_zip.c

test: test_sd_cfg test_sdzip

test_sd_cfg: test_sd_cfg.c ../src/sd_cfg.c ../src/jsonlite.c
	$(CC) $(CFLAGS) -I../src -o $@ test_sd_cfg.c ../src/sd_cfg.c ../src/jsonlite.c
	./$@

test_sdzip: test_sdzip.c ../src/zip.c ../src/manifest.c $(MINIZ)
	$(CC) $(CFLAGS) -I../src -o $@ test_sdzip.c ../src/zip.c ../src/manifest.c $(MINIZ)
	./$@

clean:
	rm -f test_sd_cfg test_sdzip
