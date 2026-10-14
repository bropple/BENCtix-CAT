#!/bin/bash
# bluez without its CUPS printer backend: ALARM's cups depends on systemd,
# and BENCtix-CAT installs only bluez, bluez-utils and bluez-libs.
#   bluez.sh PKGBUILD
set -euo pipefail
f=$1
sed -i -E \
	-e "s/ 'bluez-cups'//" \
	-e "s/ 'cups'//" \
	-e 's/^(\s*)--disable-systemd \\$/\1--disable-systemd \\\n\1--disable-cups \\/' \
	"$f"
grep -q -- '--disable-cups' "$f" || { echo "bluez.sh: --disable-cups not added" >&2; exit 1; }
! grep -qE "^pkgname=.*'bluez-cups'" "$f" || { echo "bluez.sh: bluez-cups still built" >&2; exit 1; }
! grep -qE "^makedepends=.*'cups'" "$f" || { echo "bluez.sh: cups still a makedepend" >&2; exit 1; }
