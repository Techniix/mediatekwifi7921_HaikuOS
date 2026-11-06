#!/bin/sh
#
# Development helper for the mediatekwifi7921 Haiku driver.
# Copyright 2026, Techniix. Distributed under the terms of the MIT License.
#
# Usage: tools/mt7921dev.sh <command> [args]
#
#   build        jam -q mediatekwifi7921 in $HAIKU_GENERATED
#   install      copy the built driver to non-packaged add-ons
#   enable       link the driver into dev/net (loads at next boot)
#   disable      remove that link (driver stays installed)
#   stage [N]    show or set the bring-up stage (1-6)
#   firmware     check that the firmware files are in place
#   log [N]      show the last N (default 60) driver lines from the syslog
#   status       show what is installed and enabled
#
# Environment: HAIKU_TOP (default ~/haiku),
#              HAIKU_GENERATED (default $HAIKU_TOP/generated.x86_64)

set -e

NAME=mediatekwifi7921
HAIKU_TOP=${HAIKU_TOP:-$HOME/haiku}
HAIKU_GENERATED=${HAIKU_GENERATED:-$HAIKU_TOP/generated.x86_64}
ARCH=${ARCH:-x86_64}

NP=/boot/system/non-packaged
BIN_DIR=$NP/add-ons/kernel/drivers/bin
LINK_DIR=$NP/add-ons/kernel/drivers/dev/net
FW_DIR=$NP/data/firmware/$NAME
SETTINGS=$HOME/config/settings/kernel/drivers/$NAME
BUILT=$HAIKU_GENERATED/objects/haiku/$ARCH/release/add-ons/kernel/drivers/network/wlan/$NAME/$NAME

die() { echo "error: $*" >&2; exit 1; }

case "$1" in
build)
	[ -d "$HAIKU_GENERATED" ] || die "no $HAIKU_GENERATED (set HAIKU_GENERATED)"
	cd "$HAIKU_GENERATED"
	jam -q $NAME
	;;
install)
	[ -f "$BUILT" ] || die "not built yet: $BUILT"
	mkdir -p "$BIN_DIR"
	cp "$BUILT" "$BIN_DIR/$NAME"
	ls -l "$BIN_DIR/$NAME"
	;;
enable)
	[ -f "$BIN_DIR/$NAME" ] || die "not installed; run: $0 install"
	mkdir -p "$LINK_DIR"
	ln -sf ../../bin/$NAME "$LINK_DIR/$NAME"
	echo "enabled; the driver loads at the next boot"
	;;
disable)
	rm -f "$LINK_DIR/$NAME"
	echo "disabled; end the session with a full shutdown, not a restart"
	;;
stage)
	if [ -n "$2" ]; then
		case "$2" in [1-6]) ;; *) die "stage must be 1-6" ;; esac
		mkdir -p "$(dirname "$SETTINGS")"
		echo "stage $2" > "$SETTINGS"
	fi
	if [ -f "$SETTINGS" ]; then cat "$SETTINGS"; else echo "stage 6 (default)"; fi
	;;
firmware)
	ok=1
	for f in WIFI_MT7922_patch_mcu_1_1_hdr.bin WIFI_RAM_CODE_MT7922_1.bin \
		WIFI_MT7961_patch_mcu_1_2_hdr.bin WIFI_RAM_CODE_MT7961_1.bin; do
		if [ -f "$FW_DIR/$f" ]; then
			size=$(wc -c < "$FW_DIR/$f")
			echo "ok       $f ($size bytes)"
			[ "$size" -lt 50000 ] && echo "         ^ suspiciously small: a saved web page?"
		else
			echo "missing  $f"
			case "$f" in *MT7922*) ok=0 ;; esac
		fi
	done
	echo "(MT7922/RZ616 needs the MT7922 files, MT7921/RZ608 the MT7961 files)"
	[ $ok = 1 ] || echo "copy them to $FW_DIR/"
	;;
log)
	n=${2:-60}
	{ [ -f /var/log/syslog.1 ] && grep -ih mt7921 /var/log/syslog.1 || true
	  grep -ih mt7921 /var/log/syslog || true; } | tail -n "$n"
	;;
status)
	[ -f "$BIN_DIR/$NAME" ] && ls -l "$BIN_DIR/$NAME" || echo "not installed"
	[ -L "$LINK_DIR/$NAME" ] && echo "enabled" || echo "not enabled"
	"$0" stage
	;;
*)
	sed -n '5,17p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
	;;
esac
