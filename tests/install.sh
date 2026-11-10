#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
STAGE="$ROOT/build/install-stage"
rm -rf "$STAGE"
DESTDIR="$STAGE" PREFIX=/usr sh "$ROOT/install.sh" install
test -x "$STAGE/usr/bin/GnuChanSoundRecord"
test -L "$STAGE/usr/bin/gnuchan-sound-record"
test "$(readlink "$STAGE/usr/bin/gnuchan-sound-record")" = "GnuChanSoundRecord"
test -f "$STAGE/usr/share/applications/gnuchansoundrecord.desktop"
PREFIX=/usr DESTDIR="$STAGE" sh "$ROOT/install.sh" uninstall
test ! -e "$STAGE/usr/bin/GnuChanSoundRecord"
test ! -e "$STAGE/usr/bin/gnuchan-sound-record"
rm -rf "$STAGE"
echo "install layout check: ok"
