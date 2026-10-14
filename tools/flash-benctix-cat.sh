#!/bin/bash
# Flash BENCtix-CAT from lk2nd's fastboot (D011). Writes exactly two
# partitions, cache (boot.img, /boot) and userdata (root.simg, /), and
# nothing else: never boot, aboot, sbl1, tz, rpm or any other partition.
#
#   tools/flash-benctix-cat.sh          asks before flashing
#
# The phone must be in lk2nd's fastboot mode.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
. ./config.sh
IMG=$(readlink -f "$WORK")/images

die() { echo "error: $*" >&2; exit 1; }
[ -s "$IMG/boot.img" ] && [ -s "$IMG/root.simg" ] || die "no images in $IMG; run stages/50-image.sh"
echo "Checking image checksums..."
(cd "$IMG" && sha256sum -c --quiet SHA256SUMS) || die "image checksums do not match"
echo "Looking for the phone..."
fastboot devices | grep -q . || die "no fastboot device (is the phone in lk2nd's fastboot?)"
# A fastboot that is listed but stuck answers nothing; do not wait forever
timeout 10 fastboot getvar product 2>&1 | grep -i '^product' ||
	die "the phone does not answer fastboot commands; unplug and replug USB, or restart lk2nd's fastboot"
cat "$IMG/MANIFEST"
echo
echo "This ERASES the phone's cache and userdata partitions (the Android data)"
echo "and writes BENCtix-CAT to them. No other partition is touched."
read -r -p 'Type "flash" to continue: ' ans
[ "$ans" = flash ] || die "cancelled"
fastboot flash cache "$IMG/boot.img"
fastboot flash userdata "$IMG/root.simg"
echo "Done. Boot it with: fastboot reboot"
