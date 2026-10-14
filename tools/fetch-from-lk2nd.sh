#!/usr/bin/env bash
# Read the phone's own partitions through lk2nd's fastboot (fastboot fetch),
# for a backup and for the firmware (INSTALL.md). No root needed; it only
# reads, never writes to the phone.
#
#   tools/fetch-from-lk2nd.sh OUTDIR
#
# OUTDIR gets PART.img for each partition below and SHA256SUMS. Keep it
# private and safe: modemst1, modemst2, fsg, fsc and persist hold the
# phone's IMEI and radio calibration, which nobody else's copy can replace.
#   persist modemst1 modemst2 fsg fsc   the backup (calibration, EFS)
#   modem                               the signed firmware
#   dtbobak                             the stock dtbo (lk2nd's took dtbo)
#   super                               vendor: amplifier profile, sensors
set -euo pipefail

out=${1:?usage: tools/fetch-from-lk2nd.sh OUTDIR}
PARTS=(persist modemst1 modemst2 fsg fsc modem dtbobak super)

product=$(fastboot getvar product 2>&1 | sed -n 's/^product: *//p')
case "$product" in
lk2nd*) ;;
*)
	echo "not lk2nd's fastboot (product: ${product:-no device})." >&2
	echo "With no system installed lk2nd stays in fastboot; else see INSTALL.md." >&2
	exit 1 ;;
esac

mkdir -p "$out"
chmod 700 "$out"
for p in "${PARTS[@]}"; do
	if [ -s "$out/$p.img" ]; then
		echo "$p: already fetched"
		continue
	fi
	fastboot fetch "$p" "$out/$p.img.part"
	mv "$out/$p.img.part" "$out/$p.img"
done
(cd "$out" && sha256sum ./*.img > SHA256SUMS)
echo "Fetched into $out. Keep it private: it holds your IMEI and calibration."
