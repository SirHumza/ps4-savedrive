# Toolchain notes (verified 2026-10-15, macOS):
# - ps4-payload-sdk must be built from git (never the release ZIP: CRT firmware gate).
# - orbis-llvm-config delegates to $LLVM_CONFIG; Homebrew llvm ships no ld.lld,
#   so LLVM_CONFIG must resolve to an llvm-config whose bindir holds ld.lld
#   (here: MacPorts llvm-17's ld.lld symlinked into the Cellar bin dir).
# - PS4_PAYLOAD_SDK must be exported: orbis-ld resolves its linker script from it.
PS4_PAYLOAD_SDK ?= /Users/mac/ps4-payload-sdk/ps4-payload-sdk
export PS4_PAYLOAD_SDK
LLVM_CONFIG ?= /usr/local/opt/llvm/bin/llvm-config
export LLVM_CONFIG

include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk

ELF := probe.elf

CFLAGS := -Wall -Werror -g

all: $(ELF)

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)
