CC ?= cc
CFLAGS ?= -Wall -Wextra -Werror -O1 -g

.PHONY: test test_sd_cfg clean

test: test_sd_cfg

test_sd_cfg: test_sd_cfg.c ../src/sd_cfg.c ../src/jsonlite.c
	$(CC) $(CFLAGS) -I../src -o $@ test_sd_cfg.c ../src/sd_cfg.c ../src/jsonlite.c
	./$@

clean:
	rm -f test_sd_cfg
