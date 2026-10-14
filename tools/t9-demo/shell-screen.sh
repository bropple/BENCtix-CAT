#!/bin/sh
# The shell demo's console (tty7): a bare bash as nobody in /tmp, on the
# full 60x40 console.
export TERM=linux
clear
cd /tmp
exec setpriv --reuid=65534 --regid=65534 --clear-groups \
	env -i TERM=linux HOME=/tmp LANG=C.UTF-8 PS1='$ ' bash --norc --noprofile -i
