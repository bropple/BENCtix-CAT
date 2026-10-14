// SPDX-License-Identifier: MIT
/*
 * s22-statusbar: the system status bar above every window. A layer surface
 * at the top that s22-wm keeps windows clear of (foot included): WiFi and
 * cellular signal on the left, clock and battery on the right (s22ui's
 * ui_status_bar()). Redrawn every 30 s; s22-wm also has it redraw before
 * the display comes back on. s22-wm starts it.
 *
 * The middle shows the keypad's text entry indicator, as the text console
 * has it: the mode, and the key being tapped with its characters, the
 * current one highlighted. For a window that takes typing as text (foot:
 * s22-t9d's GUI mode is "text", or a game typing) it is s22-t9d's
 * (T9_DIR/tap); for our apps, which do their own entry, the one in front
 * writes its own ($XDG_RUNTIME_DIR/s22-entry, see ui_entry_indicator).
 * Both are watched with inotify, as are s22-btd's state for the Bluetooth
 * icon, s22-gnssd's for the location arrow (while GPS runs) and the
 * settings for the vibrate-only and silent icons.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#include "s22ui.h"

#define T9_DIR	"/run/s22-t9"

static int read_file(const char *path, char *buf, int len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n = fd >= 0 ? read(fd, buf, len - 1) : -1;

	if (fd >= 0)
		close(fd);
	buf[n > 0 ? n : 0] = 0;
	return n > 0 ? n : 0;
}

static void draw_t9(struct ui_canvas *c)
{
	char gui[16], tap[192], mode[16] = "", key = 0, chars[64] = "";
	int sel = -1, x, w;

	char own[256];
	const char *dir = getenv("XDG_RUNTIME_DIR");

	snprintf(own, sizeof(own), "%s/s22-entry", dir ? dir : "/tmp");
	read_file(T9_DIR "/gui", gui, sizeof(gui));
	/* A game's too: s22-t9d types for it after Recents (and its tap is
	 * empty while it has the game's keys) */
	bool t9 = strncmp(gui, "text", 4) == 0 || strncmp(gui, "game", 4) == 0;
	if (!read_file(t9 ? T9_DIR "/tap" : own, tap, sizeof(tap)))
		return;
	char *nl = strchr(tap, '\n');
	if (nl) {
		*nl = 0;
		sscanf(nl + 1, "%c\t%d\t%63[^\n]", &key, &sel, chars);
	}
	snprintf(mode, sizeof(mode), "%s", tap);
	if (!mode[0])
		return;
	/*
	 * While a key is being tapped its characters get the whole bar: the
	 * clock and battery give way for those seconds. A list wider than
	 * that slides, the chosen character always in view, with arrows where
	 * more are hidden.
	 */
	int cw[64], n = 0, chars_w = 0;
	char k[4] = { ' ', key, ':', 0 };
	for (; key && chars[n] && n < 64; n++) {
		char ch[2] = { chars[n] == ' ' ? '_' : chars[n], 0 };
		cw[n] = ui_text_width(UI_SANS, 14, ch) + 8;
		chars_w += cw[n];
	}
	w = ui_text_width(UI_BOLD, 14, mode);
	if (key)
		w += 8 + ui_text_width(UI_SANS, 14, k) + chars_w;
	int right = key ? c->w - 6 : c->w - 110;	/* the clock's room, unless tapping */
	x = (c->w - w) / 2;
	if (x + w > right)
		x = right - w;
	if (x < 70)		/* clear of the signal icons */
		x = 70;
	if (key)
		ui_fill(c, x - 4, 0, c->w - x + 4, UI_STATUS_H, UI_BG);
	x += ui_text(c, x, 4, UI_BOLD, 14, UI_ACCENT, mode);
	if (!key)
		return;
	x += 8 + ui_text(c, x + 8, 4, UI_SANS, 14, UI_DIM, k);

	int first = 0, room = right - x;
	bool slide = chars_w > room;
	int arrow = slide ? ui_text_width(UI_SANS, 14, "‹") + 4 : 0;
	if (slide) {
		/* As few hidden on the left as keeps the chosen one in view */
		room -= 2 * arrow;
		int sum = 0;
		for (int i = 0; i <= sel && i < n; i++)
			sum += cw[i];
		while (sum > room && first < sel) {
			sum -= cw[first];
			first++;
		}
		if (first > 0)
			ui_text(c, x, 4, UI_SANS, 14, UI_DIM, "‹");
		x += arrow;
	}
	int i;
	for (i = first; i < n; i++) {
		if (x + cw[i] > right - arrow)
			break;
		char ch[2] = { chars[i] == ' ' ? '_' : chars[i], 0 };
		if (i == sel) {
			ui_round(c, x + 1, 3, cw[i] - 2, 20, 4, UI_ACCENT);
			ui_text(c, x + 4, 4, UI_BOLD, 14, UI_BG, ch);
		} else {
			ui_text(c, x + 4, 4, UI_SANS, 14, UI_FG, ch);
		}
		x += cw[i];
	}
	if (slide && i < n)
		ui_text(c, right - arrow + 2, 4, UI_SANS, 14, UI_DIM, "›");
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	ui_status_bar(c);
	draw_t9(c);
}

static void on_t9(struct ui *ui, int fd)
{
	char buf[4096];

	while (read(fd, buf, sizeof(buf)) > 0)
		;
	ui_redraw(ui);
}

/* Next redraw: at the next minute for the clock, at most 30 s away */
static void schedule(struct ui *ui)
{
	int ms = 60000 - (int)(time(NULL) % 60) * 1000;

	ui_timer(ui, ms > 30000 ? 30000 : ms);
}

static void tick(struct ui *ui)
{
	ui_redraw(ui);
	schedule(ui);
}

static void start(struct ui *ui)
{
	ui_layer_bar(ui, UI_STATUS_H);
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (fd >= 0) {
		inotify_add_watch(fd, T9_DIR, IN_CLOSE_WRITE | IN_MOVED_TO);
		inotify_add_watch(fd, "/run/s22-bt", IN_MOVED_TO);
		inotify_add_watch(fd, "/run/s22-cell", IN_MOVED_TO);	/* bars, radio */
		inotify_add_watch(fd, "/run/s22-gnss", IN_MOVED_TO);	/* GPS on */
		inotify_add_watch(fd, "/var/lib/s22", IN_MOVED_TO);	/* quiet modes */
		if (dir)
			inotify_add_watch(fd, dir, IN_MOVED_TO);
		ui_watch(ui, fd, on_t9);
	}
	schedule(ui);
}

int main(void)
{
	struct ui_app app = {
		.app_id = "s22-statusbar",
		.title = "Status",
		.draw = draw,
		.tick = tick,
		.start = start,
		.layer = true,
	};

	return ui_main(&app);
}
