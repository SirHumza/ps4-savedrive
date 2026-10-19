# savedrive payload (daemon world).
#
# Toolchain pitfalls are documented in probe/Makefile: build the SDK from git,
# and point LLVM_CONFIG at an llvm-config whose bindir also holds ld.lld.
PS4_PAYLOAD_SDK ?= /Users/mac/ps4-payload-sdk/ps4-payload-sdk
export PS4_PAYLOAD_SDK
LLVM_CONFIG ?= /usr/local/opt/llvm/bin/llvm-config
export LLVM_CONFIG

include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk

BUILD := ../build
OBJ   := $(BUILD)/obj
ELF   := $(BUILD)/savedrive.elf

CFLAGS := -Wall -Wextra -O2 -g -I. -Ithird_party/mbedtls/include \
          -DORBISRPC_SDK_PAYLOAD -DSD_PS4
LDFLAGS ?=

# Everything in src/ except the standalone probe, plus the vendored mbedTLS
# backend for tls.c. net_sockets/timing/entropy_poll are replaced by our own
# socket layer, clock and /dev/urandom poll.
SRCS := $(filter-out probe/%,$(wildcard *.c))
MBEDTLS_SRCS := $(filter-out %/net_sockets.c %/timing.c %/entropy_poll.c, \
                 $(wildcard third_party/mbedtls/library/*.c))
MBEDTLS_OBJS := $(patsubst %.c,$(OBJ)/%.o,$(MBEDTLS_SRCS))
OBJS := $(patsubst %.c,$(OBJ)/%.o,$(SRCS) $(MBEDTLS_SRCS))

$(MBEDTLS_OBJS): CFLAGS += -Wno-unused-parameter

READELF ?= /usr/local/opt/llvm/bin/llvm-readelf

.PHONY: all clean check

all: $(ELF)

$(ELF): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJ) $(ELF)

# An app-world import kills a spawned payload before main() with no output.
check: $(ELF)
	@if $(READELF) -d $(ELF) | grep -q 'libkernel\.so'; then \
		echo "FAIL: payload imports libkernel.so (app-world)"; exit 1; \
	fi
	@echo "NEEDED:"
	@$(READELF) -d $(ELF) | grep NEEDED
