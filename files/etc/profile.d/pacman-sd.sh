# BENCtix-CAT (D046): programs pacman-sd put on the SD card, by name; the
# phone's own commands come first
case ":$PATH:" in
*:/home/.pacman-sd/bin:*) ;;
*) PATH="$PATH:/home/.pacman-sd/bin" ;;
esac
