CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g -Wall -Wextra -Wunused-function -Wunused-parameter -std=gnu11
CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LDLIBS ?= $(shell $(PKG_CONFIG) --libs expat)
PKG_CFLAGS = $(shell $(PKG_CONFIG) --cflags expat)
PREFIX ?= /usr/local

# AArch64 payload build tools.  Defaults use LLVM/Clang; override for GNU cross tools:
#   make AARCH64_CC=aarch64-linux-gnu-gcc AARCH64_CFLAGS= OBJCOPY=aarch64-linux-gnu-objcopy
AARCH64_CC ?= clang
AARCH64_CFLAGS ?= -target aarch64-none-elf
OBJCOPY ?= llvm-objcopy

MAGIC_ASM := src/cv_dl_magic.S
MAGIC_OBJ := src/cv_dl_magic.o
MAGIC_BIN := src/cv_dl_magic.bin
MAGIC_RUNTIME_COPY := cv_dl_magic.bin

# Optional compile-time absolute path, useful for Nix/store packaging:
#   make MAGIC_PATH=/nix/store/.../cv_dl_magic.bin
ifneq ($(MAGIC_PATH),)
CPPFLAGS += -DCV_DL_MAGIC_PATH=\"$(MAGIC_PATH)\"
endif

all: $(MAGIC_BIN) $(MAGIC_RUNTIME_COPY) usb_dl

$(MAGIC_BIN): $(MAGIC_ASM) tools/check_magic.sh
	$(AARCH64_CC) $(AARCH64_CFLAGS) -x assembler-with-cpp -c $< -o $(MAGIC_OBJ)
	$(OBJCOPY) -O binary --only-section=.text $(MAGIC_OBJ) $@
	tools/check_magic.sh $@
	rm -f $(MAGIC_OBJ)

$(MAGIC_RUNTIME_COPY): $(MAGIC_BIN)
	cp $< $@

usb_dl: src/usb_dl.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PKG_CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

check-magic: $(MAGIC_BIN)
	tools/check_magic.sh $(MAGIC_BIN)

install: usb_dl $(MAGIC_BIN)
	install -Dm755 usb_dl $(DESTDIR)$(PREFIX)/bin/usb_dl
	install -Dm644 $(MAGIC_BIN) $(DESTDIR)$(PREFIX)/share/usb_dl/cv_dl_magic.bin

clean:
	rm -f usb_dl *.o $(MAGIC_OBJ) $(MAGIC_BIN) $(MAGIC_RUNTIME_COPY)

.PHONY: all install clean check-magic
