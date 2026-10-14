# s22-browser (D050)
PREFIX ?= /usr
CFLAGS ?= -O2
PKGS = webkitgtk-6.0 gtk4 gio-unix-2.0
LIBDIR = $(PREFIX)/lib/s22-browser

all: s22-browser-bin

# -rpath-link: WebKit's libsystemd.so.0 need is met by the stand-in
# (s22-standin, D056)
s22-browser-bin: s22-browser.c
	$(CC) $(CFLAGS) -Wall -Wno-unused-parameter `pkg-config --cflags $(PKGS)` \
		-o $@ s22-browser.c $(LDFLAGS) -Wl,-rpath-link,/usr/lib/s22-standin `pkg-config --libs $(PKGS)`

install: all
	install -Dm755 s22-browser $(DESTDIR)$(PREFIX)/bin/s22-browser
	install -Dm755 s22-browser-bin $(DESTDIR)$(LIBDIR)/s22-browser-bin
	install -Dm755 s22-browser-filters $(DESTDIR)$(LIBDIR)/s22-browser-filters
	install -Dm644 youtube.js $(DESTDIR)$(LIBDIR)/youtube.js
	install -Dm644 browser.app $(DESTDIR)$(PREFIX)/share/s22/apps/browser.app

clean:
	rm -f s22-browser-bin

.PHONY: all install clean
