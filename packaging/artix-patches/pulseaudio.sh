#!/bin/bash
# pulseaudio: only libpulse, the client library. ALARM's libpulse links
# libsystemd (D018), which is not here, and every PulseAudio client (mpv,
# ffmpeg) fails to start without it. The daemon is PipeWire's, so build the
# client side only (-D daemon=false): no GTK, Qt, GStreamer, valgrind or
# jack among the build dependencies, and no X11 (autospawn properties).
# Later definitions win when makepkg sources the PKGBUILD, so this appends.
#   pulseaudio.sh PKGBUILD
set -euo pipefail
f=$1
grep -q '^package_libpulse()' "$f" || { echo "pulseaudio.sh: no package_libpulse in $f" >&2; exit 1; }
cat >> "$f" <<'EOF'

# ---- BENCtix-CAT: libpulse only (packaging/artix-patches/pulseaudio.sh) ----
pkgname=(libpulse)
makedepends=(dbus git glib2 glibc libasyncns libsndfile meson)

build() {
  local meson_options=(
    -D daemon=false
    -D doxygen=disabled
    -D man=false
    -D tests=disabled
    -D x11=disabled
    -D gtk=disabled
    -D consolekit=disabled
    -D systemd=disabled
    -D tcpwrap=disabled
  )

  # arch-meson's flags, but auto features detected rather than all required
  # (it turns on valgrind and the rest, and refuses an override)
  meson setup --prefix /usr --libexecdir lib --sbindir bin --buildtype plain \
    --auto-features auto --wrap-mode nodownload -D b_lto=false -D b_pie=true \
    pulseaudio build "${meson_options[@]}"
  meson compile -C build
}

package_libpulse() {
  pkgdesc="$pkgdesc (client library)"
  depends=(bash dbus glib2 glibc libasyncns libsndfile)
  optdepends=('pulse-native-provider: PulseAudio backend')
  provides=(libpulse-mainloop-glib.so libpulse-simple.so libpulse.so)
  backup=(etc/pulse/client.conf)

  meson install -C build --destdir "$pkgdir"
}
EOF
