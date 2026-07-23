# ============================================================================
# ps5-linux-manager.elf — Makefile (ps5-payload-sdk conventions)
#
# Requirements:
#   - ps5-payload-sdk installed (https://github.com/ps5-payload-dev/sdk)
#   - PS5_PAYLOAD_SDK pointing at it (default: /opt/ps5-payload-sdk)
#
# Targets:
#   make            build ps5-linux-manager.elf (regenerating webui_embed.c
#                   when ../webui/dist/index.html exists)
#   make embed      force regeneration of src/webui_embed.c from the webui
#   make check      host-side syntax check of every .c/.h (gcc -fsyntax-only
#                   -std=c11 -Wall with the HOST_TEST shims in src/shims/)
#   make clean      remove build artifacts
# ============================================================================

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk

# ps5-payload-sdk toolchain fragment (prospero target: clang, freestanding
# libc, libkernel).  It provides CC and the base CFLAGS/LDFLAGS used below.
# When the SDK is not installed (e.g. a documentation checkout), only the
# `check`/`clean` targets are usable.
ifneq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
HAVE_SDK := 1
else
HAVE_SDK := 0
endif

TARGET  := ps5-linux-manager.elf
BUILDDIR := build

# Web UI bundle produced by the React app (Vite single-file build).
WEBUI_DIST  := ../webui/dist
WEBUI_INDEX := $(WEBUI_DIST)/index.html

SRCS := \
	src/main.c \
	src/httpd.c \
	src/api.c \
	src/fsapi.c \
	src/fsops.c \
	src/ftpsrv.c \
	src/appdb.c \
	src/grub.c \
	src/launch.c \
	src/json.c \
	src/util.c \
	src/webui_embed.c

OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SRCS)) $(BUILDDIR)/sqlite3.o

CC      ?= cc
CFLAGS  += -O2 -Wall -Wextra -Wno-unused-parameter -Isrc \
	-DPS5LM_HAVE_SQLITE3
# libkernel_sys provides sceKernelGetHwModelName (see the SDK's hwinfo
# sample); libSceRegMgr provides the registry fallback for the model code.
LDLIBS  += -lkernel -lkernel_sys -lSceRegMgr

# Vendored sqlite3 amalgamation (src/third_party/, same copy Elf Arsenal
# builds against this SDK).  The ps5-payload-sdk ships no sqlite3.
SQLITE_DEFS := -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
	-DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED

$(BUILDDIR)/sqlite3.o: src/third_party/sqlite3.c src/third_party/sqlite3.h
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -w $(SQLITE_DEFS) -c -o $@ $<

.PHONY: all embed check clean smoke

# `all` stays the default goal even though pattern/explicit rules for
# objects are declared above it.
.DEFAULT_GOAL := all

all: $(TARGET)

ifeq ($(HAVE_SDK),1)
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)
else
$(TARGET):
	$(error ps5-payload-sdk not found at '$(PS5_PAYLOAD_SDK)'; \
		set PS5_PAYLOAD_SDK or use 'make check' for a host syntax check)
endif

$(BUILDDIR)/%.o: src/%.c
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# ----------------------------------------------------------------------------
# webui_embed.c — generated from the built React SPA.
#
# The web UI is compiled to a single self-contained index.html (assets
# inlined by the Vite single-file plugin), gzipped, and converted to a C
# byte array.  xxd symbol names are normalized to webui_index_html_gz and
# webui_embed_available is set to 1 so httpd.c knows it is serving the real
# UI rather than the checked-in placeholder stub.
# ----------------------------------------------------------------------------
embed: $(WEBUI_INDEX)
	@echo "  EMBED   $(WEBUI_INDEX) -> src/webui_embed.c"
	@mkdir -p $(BUILDDIR)
	gzip -9 -n -c $(WEBUI_INDEX) > $(BUILDDIR)/index.html.gz
	cd $(BUILDDIR) && xxd -i index.html.gz index_html_gz.inc
	@{ \
		echo '/* AUTO-GENERATED from ../webui/dist/index.html — do not edit. */'; \
		echo '#include "webui_embed.h"'; \
		echo; \
		sed -e 's/unsigned char index_html_gz\[\]/const unsigned char webui_index_html_gz[]/' \
		    -e 's/unsigned int index_html_gz_len/const unsigned int webui_index_html_gz_len/' \
		    $(BUILDDIR)/index_html_gz.inc; \
		echo; \
		echo 'const int webui_embed_available = 1;'; \
	} > src/webui_embed.c

# Regenerate automatically whenever the built SPA is newer than the stub.
ifneq ($(wildcard $(WEBUI_INDEX)),)
src/webui_embed.c: $(WEBUI_INDEX)
	@$(MAKE) --no-print-directory embed
$(BUILDDIR)/webui_embed.o: src/webui_embed.c
endif

# ----------------------------------------------------------------------------
# Host-side syntax check (no PS5 SDK required).
# ----------------------------------------------------------------------------
check:
	@failed=0; \
	for f in $(SRCS); do \
		echo "  CHECK   $$f"; \
		gcc -fsyntax-only -std=c11 -Wall -DHOST_TEST -Isrc $$f || failed=1; \
	done; \
	for h in src/*.h src/shims/*.h src/third_party/*.h; do \
		echo "  CHECK   $$h"; \
		gcc -fsyntax-only -std=c11 -Wall -DHOST_TEST -Isrc -x c $$h || failed=1; \
	done; \
	echo "  CHECK   src/third_party/sqlite3.c (warnings suppressed)"; \
	gcc -fsyntax-only -std=c11 -w -DHOST_TEST -Isrc $(SQLITE_DEFS) \
		src/third_party/sqlite3.c || failed=1; \
	if [ $$failed -ne 0 ]; then exit 1; fi; \
	echo "all files passed"

# ----------------------------------------------------------------------------
# Host smoke test: builds the full payload (with the real HTTP server, FTP
# daemon, filesystem API, 9021 serve path, app.db install and notification
# shims) for the host and exercises every contract v1.1 endpoint over raw
# TCP.  See tests/smoke.c for the assertions.
# ----------------------------------------------------------------------------
TESTDIR ?= /tmp/ps5lm-smoke

SMOKE_DEFS := -DHOST_TEST -DPS5LM_HAVE_SQLITE3 \
	-DPS5LM_DATA_DIR='"$(TESTDIR)/data"' \
	-DFSOPS_MNT_PREFIX='"$(TESTDIR)/mnt"' \
	-DPS5LM_ELF_LOADER_PORT=19921 \
	-DPS5LM_APPDB_PATH='"$(TESTDIR)/app.db"'

smoke: $(SRCS) tests/smoke.c tests/host_stubs.c tests/webui_stub.c
	@mkdir -p $(BUILDDIR)
	gcc -std=c11 -O1 -g -Wall -Isrc $(SMOKE_DEFS) -Dmain=ps5lm_main \
		-c src/main.c -o $(BUILDDIR)/smoke_main.o
	gcc -std=c11 -O1 -g -Wall -Isrc $(SMOKE_DEFS) \
		$(filter-out src/main.c src/webui_embed.c,$(SRCS)) \
		tests/host_stubs.c tests/webui_stub.c tests/smoke.c \
		src/third_party/sqlite3.c $(SQLITE_DEFS) \
		$(BUILDDIR)/smoke_main.o -lpthread -o /tmp/ps5lm-smoke.bin
	/tmp/ps5lm-smoke.bin

clean:
	rm -rf $(BUILDDIR) $(TARGET)
