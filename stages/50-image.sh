#!/bin/bash
# Stage 50: the two images lk2nd's fastboot flashes (D011).
#   boot.img   ext2, the cache partition: /boot (kernel, DTB, extlinux.conf)
#   root.simg  btrfs (zstd:3), the userdata partition, sparse: subvolumes
#              @ (the root, default) and @snapshots
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/../lib/common.sh"
load_config
enter_userns "$@"
require_stamp configured "run stages/40-configure.sh first"

OUT=$WORK/images
mkdir -p "$OUT"
[ -f "$BOOTDIR/extlinux/extlinux.conf" ] || die "no extlinux.conf in $BOOTDIR"
[ -z "$(ls -A "$ROOTFS/boot")" ] || die "$ROOTFS/boot is not empty; /boot belongs on the cache partition"
[ -z "$(ls -A "$ROOTFS/var/cache/pacman/pkg")" ] || die "the package cache is not empty in the rootfs"

# ------------------------------------------------------------- boot ---
# Plain ext2: lk2nd's driver has no extents (D002)
log "boot.img: ext2 from $BOOTDIR"
truncate -s 0 "$OUT/boot.img.new"
truncate -s $((CACHE_SECTORS * 512)) "$OUT/boot.img.new"
mke2fs -q -t ext2 -L s22boot -O ^ext_attr -E root_owner=0:0 -d "$BOOTDIR" "$OUT/boot.img.new"
feat=$(dumpe2fs -h "$OUT/boot.img.new" 2>/dev/null | sed -n 's/^Filesystem features: *//p')
case " $feat " in *" extent "*|*" 64bit "*|*" has_journal "*) die "boot.img has features lk2nd cannot read: $feat" ;; esac
e2fsck -fn "$OUT/boot.img.new" >/dev/null || die "boot.img does not check clean"
mv -f "$OUT/boot.img.new" "$OUT/boot.img"
ok "boot.img ($feat)"

# ------------------------------------------------------------- root ---
log "root.img: btrfs from $ROOTFS"
# The chroot's bind-mount points (empty outside the namespace)
rmdir "$ROOTFS"/mnt/{overlay,artix,local} 2>/dev/null || true
mkdir -p "$WORK/fsroot/@snapshots"
truncate -s 0 "$OUT/root.img.new"
truncate -s $((USERDATA_SECTORS / 8 * 4096)) "$OUT/root.img.new"
mkfs.btrfs -q -f -L s22root --rootdir "$WORK/fsroot" \
	-u default:@ -u @snapshots --compress zstd:3 "$OUT/root.img.new"
btrfs check --readonly "$OUT/root.img.new" >/dev/null 2>&1 || die "root.img does not check clean"
mv -f "$OUT/root.img.new" "$OUT/root.img"
log "root.simg: sparse image for fastboot"
img2simg "$OUT/root.img" "$OUT/root.simg.new"
mv -f "$OUT/root.simg.new" "$OUT/root.simg"

(cd "$OUT" && sha256sum boot.img root.simg) > "$OUT/SHA256SUMS"
{
	echo "BENCtix-CAT images, $(date -u +%FT%TZ)"
	echo "kernel   $(ls "$ROOTFS/usr/lib/modules" | grep BENCkernel-CAT | sort -V | tail -1)"
	echo "packages $(wc -l < "$WORK/snapshot/rootfs-packages.txt")"
	echo "boot.img $(du -h --apparent-size "$OUT/boot.img" | cut -f1) for cache ($CACHE_SECTORS sectors)"
	echo "root.img $(du -h --apparent-size "$OUT/root.img" | cut -f1), $(du -h "$OUT/root.simg" | cut -f1) sparse, for userdata ($USERDATA_SECTORS sectors)"
	cat "$OUT/SHA256SUMS"
} | tee "$OUT/MANIFEST"
ok "stage 50 complete: flash with tools/flash-benctix-cat.sh (needs the phone in lk2nd fastboot)"
