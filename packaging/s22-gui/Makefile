PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wno-unused-parameter -Wno-format-truncation -DWLR_USE_UNSTABLE -I.
PREFIX ?= /usr
WL_PROTOCOLS != $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols
XDG_SHELL = $(WL_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
LAYER_SHELL = wlr-layer-shell-unstable-v1.xml
SESSION_LOCK = $(WL_PROTOCOLS)/staging/ext-session-lock/ext-session-lock-v1.xml
IDLE_INHIBIT = $(WL_PROTOCOLS)/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml

WM_PKGS = wlroots-0.20 wayland-server xkbcommon libinput
UI_PKGS = wayland-client xkbcommon freetype2 fontconfig

APPS = s22-launcher s22-settings s22-shell s22-statusbar s22-messages s22-touchtest \
	s22-phone s22-contacts s22-lock s22-emu s22-help s22-music s22-match s22-pick

all: s22-wm $(APPS) s22-camera s22-photos s22-music-tags

xdg-shell-client-protocol.h:
	wayland-scanner client-header $(XDG_SHELL) $@
xdg-shell-protocol.c:
	wayland-scanner private-code $(XDG_SHELL) $@
wlr-layer-shell-unstable-v1-protocol.h: $(LAYER_SHELL)
	wayland-scanner server-header $< $@
wlr-layer-shell-unstable-v1-client-protocol.h: $(LAYER_SHELL)
	wayland-scanner client-header $< $@
wlr-layer-shell-unstable-v1-protocol.c: $(LAYER_SHELL)
	wayland-scanner private-code $< $@

s22-wm: s22-wm.c wlr-layer-shell-unstable-v1-protocol.h
	$(CC) $(CFLAGS) `$(PKG_CONFIG) --cflags $(WM_PKGS)` $< -o $@ \
		$(LDFLAGS) `$(PKG_CONFIG) --libs $(WM_PKGS)`

ext-session-lock-v1-client-protocol.h:
	wayland-scanner client-header $(SESSION_LOCK) $@
ext-session-lock-v1-protocol.c:
	wayland-scanner private-code $(SESSION_LOCK) $@
idle-inhibit-unstable-v1-client-protocol.h:
	wayland-scanner client-header $(IDLE_INHIBIT) $@
idle-inhibit-unstable-v1-protocol.c:
	wayland-scanner private-code $(IDLE_INHIBIT) $@

libs22ui.a: s22ui.o s22contacts.o s22sounds.o s22pin.o s22np.o s22badges.o xdg-shell-protocol.o \
		wlr-layer-shell-unstable-v1-protocol.o ext-session-lock-v1-protocol.o \
		idle-inhibit-unstable-v1-protocol.o
	$(AR) rcs $@ $^
s22ui.o: s22ui.c s22ui.h xdg-shell-client-protocol.h \
		wlr-layer-shell-unstable-v1-client-protocol.h ext-session-lock-v1-client-protocol.h \
		idle-inhibit-unstable-v1-client-protocol.h
	$(CC) $(CFLAGS) `$(PKG_CONFIG) --cflags $(UI_PKGS)` -c $< -o $@
s22contacts.o s22sounds.o s22pin.o s22np.o s22badges.o: %.o: %.c s22ui.h
	$(CC) $(CFLAGS) `$(PKG_CONFIG) --cflags $(UI_PKGS)` -c $< -o $@
%-protocol.o: %-protocol.c
	$(CC) $(CFLAGS) `$(PKG_CONFIG) --cflags wayland-client` -c $< -o $@

# Settings draws the Tailscale login link as a QR code
s22-settings: APP_LIBS = -lqrencode -lcrypt
s22-lock: APP_LIBS = -lcrypt -ljpeg
s22-emu: APP_LIBS = -ldl -lasound
s22-match: APP_LIBS = -lasound
# Music, the notification list and the lock screen show album art (s22np.c)
s22-music s22-shell: APP_LIBS = -ljpeg

$(APPS): %: %.c libs22ui.a s22ui.h
	$(CC) $(CFLAGS) $< libs22ui.a -o $@ $(LDFLAGS) \
		`$(PKG_CONFIG) --libs $(UI_PKGS)` $(APP_LIBS) -lm

# Music's Artists and Albums: the songs' tags, read by ffmpeg's libraries
s22-music-tags: s22-music-tags.c
	$(CC) $(CFLAGS) `$(PKG_CONFIG) --cflags libavformat libavutil` $< -o $@ $(LDFLAGS) \
		`$(PKG_CONFIG) --libs libavformat libavutil`

s22-camera s22-photos: %: %.c libs22ui.a s22ui.h
	$(CC) $(CFLAGS) -O3 $< libs22ui.a -o $@ $(LDFLAGS) \
		`$(PKG_CONFIG) --libs $(UI_PKGS) libjpeg` -lm

# The help pages: in help/ here, side by side in makepkg's src/
HELP = $(wildcard help/[0-9][0-9]-*.txt [0-9][0-9]-*.txt)

install: all
	install -Dm755 s22-wm $(DESTDIR)$(PREFIX)/bin/s22-wm
	install -m755 $(APPS) s22-camera s22-photos s22-music-tags $(DESTDIR)$(PREFIX)/bin/
	install -Dm644 foot.ini $(DESTDIR)$(PREFIX)/share/s22/foot.ini
	install -Dm644 tmux.conf $(DESTDIR)$(PREFIX)/share/s22/tmux.conf
	install -Dm755 s22-screenshot $(DESTDIR)$(PREFIX)/bin/s22-screenshot
	install -Dm755 s22-wallpaper $(DESTDIR)$(PREFIX)/bin/s22-wallpaper
	install -Dm755 s22-musicd $(DESTDIR)$(PREFIX)/bin/s22-musicd
	install -d $(DESTDIR)$(PREFIX)/share/s22/help
	install -m644 $(HELP) $(DESTDIR)$(PREFIX)/share/s22/help/
	install -d $(DESTDIR)$(PREFIX)/share/s22/apps
	install -m644 *.app $(DESTDIR)$(PREFIX)/share/s22/apps/
	install -d $(DESTDIR)$(PREFIX)/share/s22/themes
	install -m644 *.theme $(DESTDIR)$(PREFIX)/share/s22/themes/

clean:
	rm -f s22-wm $(APPS) s22-camera s22-photos s22-music-tags libs22ui.a *.o xdg-shell-* wlr-layer-shell-*-protocol.[ch] ext-session-lock-* idle-inhibit-*

.PHONY: all install clean
