#!/bin/bash
# Copy BENCtix-CAT's generic phone tools into the public s22flip-linux repo,
# without references to this private repository (its name, decision
# numbers). Nothing phone-derived is copied: these are our own sources.
#
#   tools/publish-tools.sh [PUBLIC_REPO]     (default ../s22flip-linux)
#
# It only writes files; review, commit and push in the public repo.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
PUB=${1:-../s22flip-linux}
[ -d "$PUB/.git" ] || { echo "no git repository at $PUB" >&2; exit 1; }

# Phone numbers never go public: +1 NXX NXX XXXX in the usual spellings
PHONE_RE='(\+?1[-. ]?)?\(?\b[2-9][0-9]{2}\)?[-. ][0-9]{3}[-. ][0-9]{4}\b|\+?\b1?[2-9][0-9]{9}\b'
no_phone_numbers() {	# FILE...
	if grep -nE "$PHONE_RE" "$@"; then
		echo "phone number in a public file (above)" >&2
		exit 1
	fi
}

clean() {	# SRC DEST [MODE]
	sed -E \
		-e 's/ \((D[0-9]{3}(, D[0-9]{3})*)\)//g' \
		-e 's/\(D[0-9]{3}: /(/g' \
		-e 's/, D[0-9]{3}\)/)/g' \
		-e 's/BENCtix-CAT does not write/the installed system does not write/' \
		-e 's/BENCtix-CAT writes/the installed system writes/' \
		-e 's/ on BENCtix-CAT//' \
		-e 's/^# BENCtix-CAT: /# /' \
		-e 's/S22 Flip \(BENCtix-CAT\)/S22 Flip (Linux)/' \
		-e 's/"benctix-cat"/"s22flip-linux"/' \
		"$1" > "$2.new"
	# Case-insensitive: lower-case names (a USB serial) slipped through once
	if grep -niE 'benc|\bD[0-9]{3}\b' "$2.new"; then
		echo "private reference left in $2 (above)" >&2
		exit 1
	fi
	no_phone_numbers "$2.new"
	chmod "${3:-0644}" "$2.new"
	mv "$2.new" "$2"
}

mkdir -p "$PUB/tools/openrc/init.d" "$PUB/tools/openrc/conf.d" "$PUB/docs/img"
clean packaging/s22-t9d/s22-t9d.c "$PUB/tools/s22-t9d.c"
clean packaging/s22-lidd/s22-lidd.c "$PUB/tools/s22-lidd.c"
clean files/sbin/s22-partguard "$PUB/tools/s22-partguard" 0755
clean packaging/s22-sensord/s22-sensord.c "$PUB/tools/s22-sensord.c"
clean packaging/s22-sensord/s22-sensor "$PUB/tools/s22-sensor" 0755
mkdir -p "$PUB/tools/s22-outerd" "$PUB/tools/s22-lidd.d"
clean packaging/s22-outerd/s22-outerd.c "$PUB/tools/s22-outerd/s22-outerd.c"
clean packaging/s22-outerd/fonts.h "$PUB/tools/s22-outerd/fonts.h"
clean packaging/s22-outerd/media "$PUB/tools/s22-outerd/media" 0755
mkdir -p "$PUB/tools/s22-notify"
clean packaging/s22-notify/s22-notifyd.c "$PUB/tools/s22-notify/s22-notifyd.c"
clean packaging/s22-notify/s22-notify "$PUB/tools/s22-notify/s22-notify" 0755
clean packaging/s22-notify/vibrate "$PUB/tools/s22-notify/vibrate"
mkdir -p "$PUB/tools/s22-smsd"
clean packaging/s22-smsd/s22-smsd "$PUB/tools/s22-smsd/s22-smsd" 0755
mkdir -p "$PUB/tools/s22-imsd"
mkdir -p "$PUB/tools/s22-celld"
clean packaging/s22-celld/s22-celld "$PUB/tools/s22-celld/s22-celld" 0755
clean packaging/s22-imsd/s22-imsd "$PUB/tools/s22-imsd/s22-imsd" 0755
mkdir -p "$PUB/tools/s22-battery"
clean packaging/s22-battery/s22-batteryd "$PUB/tools/s22-battery/s22-batteryd" 0755
mkdir -p "$PUB/tools/s22-call"
clean packaging/s22-call/s22-calld "$PUB/tools/s22-call/s22-calld" 0755
clean packaging/s22-call/s22-call "$PUB/tools/s22-call/s22-call" 0755
clean packaging/s22-call/s22-pcmhold.c "$PUB/tools/s22-call/s22-pcmhold.c"
clean files/etc/s22-console.map "$PUB/tools/openrc/s22-console.map"
clean packaging/s22-lidd/10-wifi-powersave "$PUB/tools/s22-lidd.d/10-wifi-powersave" 0755
for f in s22-t9d s22-lidd s22-outer s22-outerd s22-notifyd s22-clock s22-partguard s22-sensord s22-usbnet \
	s22-zram rmtfs tqftpserv s22-smsd s22-imsd s22-celld s22-battery s22-call; do
	clean "files/init.d/$f" "$PUB/tools/openrc/init.d/$f" 0755
done
for f in files/conf.d/*; do
	[ "${f##*/}" = s22-gui ] && continue	# the GUI is not published
	clean "$f" "$PUB/tools/openrc/conf.d/${f##*/}"
done
cp docs/media/t9-keypad-demo.gif docs/media/t9-shell-demo.gif "$PUB/docs/img/"

# The keypad reference: MANUAL.md's keypad section only (the rest is about
# this install: accounts, addresses)
{
	echo "# Typing on the keypad (s22-t9d)"
	echo
	echo "Reference for \`tools/s22-t9d.c\`, the keypad text input daemon."
	echo
	sed -n '/^## Typing on the keypad/,/^## The phone/p' MANUAL.md | sed '1d;$d' |
		sed -E 's/^### /## /'
} | cat -s > "$PUB/docs/KEYPAD.md.new"
mv "$PUB/docs/KEYPAD.md.new" "$PUB/docs/KEYPAD.md"
# Everything else in the public repo too (HARDWARE.md is copied by hand)
if found=$(cd "$PUB" && git ls-files -z | xargs -0 grep -IlE "$PHONE_RE"); then
	echo "$found"
	echo "phone number in the public repo (files above)" >&2
	exit 1
fi
git -C "$PUB" status --short
