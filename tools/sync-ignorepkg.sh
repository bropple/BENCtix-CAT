#!/usr/bin/env bash
# Write config.sh's IgnorePkg (hand-picked + every package we build) into a
# running phone's /etc/pacman.conf, the line stage 40 writes into the image.
# RUNS ON: BENCbench, over ssh.
#
#   tools/sync-ignorepkg.sh [HOST]     (default root@172.16.42.1)
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
HOST=${1:-root@172.16.42.1}
source ./config.sh
line="IgnorePkg   = ${IGNORE_PKGS[*]}"
# shellcheck disable=SC2029  # expanded here on purpose
ssh "$HOST" "cp -n /etc/pacman.conf /etc/pacman.conf.bak-ignorepkg &&
	sed -i -E 's|^IgnorePkg[[:space:]]*=.*|$line|' /etc/pacman.conf &&
	grep -c '^IgnorePkg   = .*\\budev\\b' /etc/pacman.conf >/dev/null"
echo "IgnorePkg on $HOST: ${#IGNORE_PKGS[@]} packages"
