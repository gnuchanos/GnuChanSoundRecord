#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROGRAM=GnuChanSoundRecord
OUTPUT=${OUTPUT:-build/$PROGRAM}
PREFIX=${PREFIX:-/usr/local}
DESTDIR=${DESTDIR:-}
BIN_DIR="$DESTDIR$PREFIX/bin"
APP_DIR="$DESTDIR$PREFIX/share/applications"
BIN="$ROOT/$OUTPUT"

action=${1:-install}
case "$action" in
    build)
        exec make -C "$ROOT" build
        ;;
    run)
        make -C "$ROOT" build
        exec "$ROOT/build/$PROGRAM"
        ;;
    check)
        exec make -C "$ROOT" check
        ;;
    install)
        make -C "$ROOT" build OUTPUT="$OUTPUT"
        if [ -z "$DESTDIR" ] && [ "$(id -u)" -ne 0 ]; then
            echo "error: installing to $BIN_DIR needs root; run with sudo" >&2
            exit 1
        fi
        install -D -m 0755 "$BIN" "$BIN_DIR/$PROGRAM"
        ln -sfn "$PROGRAM" "$BIN_DIR/gnuchan-sound-record"
        install -D -m 0644 "$ROOT/gnuchansoundrecord.desktop" "$APP_DIR/gnuchansoundrecord.desktop"
        echo "Installed $PROGRAM and $BIN_DIR/gnuchan-sound-record"
        ;;
    uninstall)
        if [ -z "$DESTDIR" ] && [ "$(id -u)" -ne 0 ]; then
            echo "error: uninstalling from $BIN_DIR needs root; run with sudo" >&2
            exit 1
        fi
        rm -f "$BIN_DIR/$PROGRAM" "$BIN_DIR/gnuchan-sound-record" \
            "$APP_DIR/gnuchansoundrecord.desktop"
        echo "Removed $PROGRAM, gnuchan-sound-record, and its desktop entry"
        ;;
    *)
        echo "Usage: $0 [build|run|check|install|uninstall]" >&2
        exit 2
        ;;
esac
