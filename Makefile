CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
PREFIX ?= /usr/local
DESTDIR ?=
VERSION := $(shell cat version.txt)
OUTPUT := build/GnuChanSoundRecord
PACKAGES := gtk+-3.0 gio-unix-2.0
SOURCES := src/main.c

.PHONY: all build clean install uninstall run check

all: build

build: $(OUTPUT)

$(OUTPUT): $(SOURCES) version.txt
	@command -v $(PKG_CONFIG) >/dev/null || { echo "error: pkg-config is required" >&2; exit 1; }
	@$(PKG_CONFIG) --exists $(PACKAGES) || { echo "error: install build-essential, pkg-config, libgtk-3-dev, and libglib2.0-dev" >&2; exit 1; }
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Wpedantic $(CFLAGS) \
		-DGNUCHAN_SOUND_RECORD_VERSION='"$(VERSION)"' \
		$(SOURCES) -o $(OUTPUT) \
		$$($(PKG_CONFIG) --cflags --libs $(PACKAGES)) -lm

run: build
	./$(OUTPUT)

check: build
	./$(OUTPUT) --version
	./$(OUTPUT) --self-test
	./$(OUTPUT) --check-dependencies
	OUTPUT="$(OUTPUT)" sh tests/install.sh

install: build
	PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" sh ./install.sh install

uninstall:
	PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" sh ./install.sh uninstall

clean:
	rm -rf build
