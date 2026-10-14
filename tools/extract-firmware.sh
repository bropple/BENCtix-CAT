#!/usr/bin/env bash
# The phone's firmware, laid out where the build takes it (config.sh's
# FIRMWARE_DIR, SENSORS_REG, SENSORS_GROUPS), from its own partition images
# (tools/fetch-from-lk2nd.sh, or a backup of them):
#   modem    mba.mbn, and modem, adsp, wcnss, venus squashed into .mbn files;
#            the modem's carrier configs (modem_pr)
#   persist  the WiFi NV file, the sensor registry (sns.reg)
#   super    vendor's amplifier profile; the sensor daemon, for the registry's
#            group table
#   dtbobak  the stock dtbo: the outer screen's init sequence
# plus the GPU's microcode from linux-firmware (this PC's, else downloaded
# from kernel.org). All of it stays local, never
# published (the phone's own, under the vendor's terms).
#
#   tools/extract-firmware.sh PARTDIR [OUTROOT]
#
# OUTROOT (lib/firmware/, src/sensors/ under it) defaults to where config.sh
# points. Needs mtools, e2fsprogs (debugfs), android-tools (lpunpack,
# mkdtboimg), python3, pil-squasher (github.com/linux-msm/pil-squasher, in
# PATH or as PIL_SQUASHER) and s22flip-linux's tools (S22FLIP_LINUX, default
# ../s22flip-linux next to this repo).
set -euo pipefail

parts=${1:?usage: tools/extract-firmware.sh PARTDIR [OUTROOT]}
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck source=../config.sh
source "$REPO_ROOT/config.sh"
if [ -n "${2:-}" ]; then
	FW=$2/lib/firmware REG=$2/src/sensors/sns.reg GRP=$2/src/sensors/sns-groups.txt
else
	FW=$REPO_ROOT/$FIRMWARE_DIR REG=$REPO_ROOT/$SENSORS_REG GRP=$REPO_ROOT/$SENSORS_GROUPS
fi
S22=${S22FLIP_LINUX:-$REPO_ROOT/../s22flip-linux}
SQUASH=${PIL_SQUASHER:-$(command -v pil-squasher || true)}
DEST=$FW/qcom/qm215/cat/s22flip
PANEL_NODE=/fragment@31/__overlay__/qcom,mdss_spi_st7789v2_qvga_cmd
LINUX_FIRMWARE=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain

die() { echo "extract-firmware: $*" >&2; exit 1; }
for t in mcopy debugfs lpunpack mkdtboimg python3; do
	command -v "$t" >/dev/null || die "$t is missing"
done
[ -x "$SQUASH" ] || die "pil-squasher not found (PATH or PIL_SQUASHER)"
[ -f "$S22/tools/mkmipidbi.py" ] || die "s22flip-linux's tools not at $S22 (S22FLIP_LINUX)"
for p in modem persist super dtbobak; do
	[ -s "$parts/$p.img" ] || die "no $parts/$p.img (tools/fetch-from-lk2nd.sh)"
done

tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
mkdir -p "$DEST" "$(dirname "$REG")" "$(dirname "$GRP")"
# debugfs prints a banner on stderr and does not fail on a missing file
dump() {	# IMAGE PATH OUT
	debugfs -R "dump $2 $3" "$1" 2>/dev/null
	[ -s "$3" ] || die "$2 not found in $(basename "$1")"
}

echo "modem: the signed firmware"
mcopy -s -n -i "$parts/modem.img" ::/image "$tmp/"
cp "$tmp/image/mba.mbn" "$DEST/"
for f in modem adsp wcnss venus; do
	"$SQUASH" "$DEST/$f.mbn" "$tmp/image/$f.mdt" >/dev/null
done
cp -r "$tmp/image/modem_pr" "$DEST/"

echo "persist: WiFi calibration, the sensor registry"
dump "$parts/persist.img" /WCNSS_qcom_wlan_nv.bin "$DEST/WCNSS_qcom_wlan_nv.bin"
dump "$parts/persist.img" /sensors/sns.reg "$REG"

echo "super: vendor's amplifier profile, the sensor registry's groups"
super=$parts/super.img
if [ "$(od -An -tx4 -N4 "$super" | tr -d ' ')" = ed26ff3a ]; then	# sparse
	simg2img "$super" "$tmp/super.raw"
	super=$tmp/super.raw
fi
lpunpack -p vendor "$super" "$tmp" >/dev/null
dump "$tmp/vendor.img" /firmware/aw881xx_acf.bin "$DEST/aw881xx_acf.bin"
dump "$tmp/vendor.img" /bin/sensors.qti "$tmp/sensors.qti"
python3 "$S22/tools/sns-reg-groups.py" "$tmp/sensors.qti" "$REG" > "$GRP"

echo "dtbobak: the outer screen's init sequence"
mkdtboimg dump "$parts/dtbobak.img" -b "$tmp/entry" > "$tmp/dtbo.txt"
[ -s "$tmp/entry.27" ] || die "dtbobak.img is not the stock dtbo (no entry 27)"
python3 "$S22/tools/mkmipidbi.py" "$tmp/entry.27" "$PANEL_NODE" "$FW/cat,s22flip-ext-panel.bin"

echo "linux-firmware: the GPU's microcode"
mkdir -p "$FW/qcom"
for f in a300_pm4.fw a300_pfp.fw; do
	src=/usr/lib/firmware/qcom/$f
	if [ -f "$src" ]; then cp "$src" "$FW/qcom/$f"
	elif [ -f "$src.zst" ]; then zstd -dqf "$src.zst" -o "$FW/qcom/$f"
	elif [ -f "$src.xz" ]; then xz -dc "$src.xz" > "$FW/qcom/$f"
	# Not installed here: from linux-firmware itself (redistributable)
	elif curl -sfL -o "$FW/qcom/$f" "$LINUX_FIRMWARE/qcom/$f"; then echo "  $f: downloaded"
	else echo "  $f: get it from linux-firmware (qcom/$f) into $FW/qcom/" >&2
	fi
done
echo "Done: $FW, $(dirname "$REG")"
