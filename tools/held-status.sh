#!/bin/bash
# held-status: the packages BENCtix-CAT holds back (IgnorePkg) against what
# ALARM and Artix currently ship, so stale ones are noticed and rebuilt.
#
#   tools/held-status.sh [user@host]      (default root@172.16.42.1)
#
# Read-only on the phone: it syncs pacman's databases (pacman -Sy, which
# changes no package) and reads versions. For each held package:
#   ok        installed is ALARM's current version or newer
#   behind    ALARM is newer; pacman will keep skipping it ([ignored])
#   rebuild   ...and Artix already has a version at least as new as ALARM's:
#             rebuild from Artix (stage 30's ARTIX_BUILD, or BENCtix's overlay)
#   artix-newer  ALARM does not ship it (udev, openrc...) but Artix is newer
# Packages neither ships (ours, kernels) show "-".
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
. ./config.sh
PHONE=${1:-root@172.16.42.1}
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "$PHONE")

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Artix's current versions (arch-independent strings) from its x86_64 repos
for repo in system world galaxy; do
	curl -sfL --max-time 60 "$ARTIX_MIRROR/$repo/os/x86_64/$repo.db" -o "$tmp/$repo.db" || true
	[ -s "$tmp/$repo.db" ] && bsdtar -xf "$tmp/$repo.db" -C "$tmp" 2>/dev/null || true
done
artix_ver() {
	local d
	for d in "$tmp/$1"-[0-9]*/; do
		[ -f "$d/desc" ] || continue
		[ "$(sed -n '/^%NAME%$/{n;p;q}' "$d/desc")" = "$1" ] || continue
		sed -n '/^%VERSION%$/{n;p;q}' "$d/desc"
		return
	done
}

"${SSH[@]}" 'pacman -Sy >/dev/null 2>&1 || pacman -Sy --disable-sandbox >/dev/null
	for p in $(sed -n "s/^IgnorePkg *= *//p" /etc/pacman.conf); do
		i=$(pacman -Q "$p" 2>/dev/null | cut -d" " -f2)
		a=$(pacman -Si "$p" 2>/dev/null | sed -n "s/^Version *: //p" | head -1)
		echo "$p ${i:--} ${a:--}"
	done' > "$tmp/phone"

printf '%-22s %-26s %-26s %-26s %s\n' PACKAGE INSTALLED ALARM ARTIX STATUS
while read -r p inst alarm; do
	[ "$inst" = - ] && continue		# held but not installed
	art=$(artix_ver "$p")
	status=ok
	if [ "$alarm" = - ]; then
		status=-
		if [ -n "$art" ] && [ "$(vercmp "${art%-*}" "${inst%-*}")" -gt 0 ]; then
			status=artix-newer	# ALARM lacks it (udev...); Artix moved on
		fi
	elif [ "$(vercmp "$alarm" "$inst")" -gt 0 ]; then
		status=behind
		if [ -n "$art" ] && [ "$(vercmp "${art%-*}" "${alarm%-*}")" -ge 0 ]; then
			status=rebuild
		fi
	fi
	printf '%-22s %-26s %-26s %-26s %s\n' "$p" "$inst" "$alarm" "${art:--}" "$status"
done < "$tmp/phone"
