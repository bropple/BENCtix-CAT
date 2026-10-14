// SPDX-License-Identifier: MIT
/*
 * s22-shell: notifications in the GUI.
 *
 * Each new notification (a file in /run/s22-notify, see s22-notify) drops
 * in as a banner over the top of the screen for a few seconds, and wakes the
 * display if it went off with the lid open. The notification list, a
 * full-screen overlay, opens when s22-wm passes on the side key ("list" on
 * $XDG_RUNTIME_DIR/s22-shell.sock):
 *
 *   Up/Down choose; OK opens it: its link (open=, see ui_open_uri) or else
 *   the app for its source (the .app file with that Notify=), and then
 *   dismisses it; one with neither is shown whole instead. C dismisses
 *   it, # dismisses all, Back or the side key closes; so does any window
 *   coming to the front.
 *
 * Dismissing removes the alert only; the message stays in its app.
 *
 * While music plays (D059) the Music player's card heads the list, chosen
 * when it opens: OK plays or pauses, C stops, Left/Right the song before or after
 * (held, scrubbing), Down to the notifications (Up from the first returns),
 * and its buttons and bar by touch. Over the lock too.
 *
 * A ringing call (source "call", s22-calld) gets no banner: its link opens
 * at once (the Phone app's call screen), as the display wakes.
 *
 * "volume KIND LEVEL MAX" on the control socket (s22-wm, at a volume key)
 * shows the level for a moment in a small bar at the top.
 *
 * Quick settings: the side key pressed again with the list open shows them
 * instead, and a third press closes them. Tiles for Wi-Fi, Bluetooth,
 * cellular (the modem's radio, s22-celld), mobile data, the sound mode (sounds, vibrate only, silent), the
 * flashlight (the camera flash in torch mode; s22-wm also toggles it on a
 * held side key) and Mullvad, and the brightness under them: arrows move,
 * OK switches, Left/Right on the brightness change it.
 *
 * The power menu ("power", s22-wm, End held): power off, reboot, reboot to
 * lk2nd's fastboot, through sudo s22-power (D043).
 *
 * On the lock screen (D049) the list, quick settings and banners show
 * over the lock (s22-wm puts them there). A notification chosen there
 * opens once the PIN unlocks the phone ("unlocked" from s22-wm), if that
 * is within PENDING_S; until then the lock screen comes back. s22-wm
 * says "locked" when the lock starts, and the list closes. The cellular,
 * mobile data and Mullvad tiles show a padlock there and do not switch:
 * whoever holds the phone should not cut it off the network.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/rfkill.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "s22ui.h"

#define MAX_NOTES	64
#define BANNER_H	104
#define BANNER_MS	5000

struct note {
	char name[64];
	char source[32];
	char text[512];
	char open[256];		/* a link to open it with, or "" */
	time_t when;
};

static struct note notes[MAX_NOTES];	/* newest first */
static int nnotes;
static struct ui_list list = { .row_h = 92 };
static bool listing;			/* the list is open */
static bool np_sel;			/* ... the player's card chosen */
static bool quick;			/* ... showing quick settings instead */
static int quick_sel;
static bool powermenu;			/* ... showing the power menu instead */
static int power_sel;
static const char *power_doing;		/* what it is doing, once chosen */
static char detail[64];			/* the note shown whole, in the list */
static char banner[64];			/* the note shown as a banner */
static bool osd;			/* the volume bar is showing */
static char osd_kind[16];
static int osd_level, osd_max;
#define OSD_H		64
#define OSD_MS		1500
static int ctl_fd = -1, in_fd = -1;
static bool locked;			/* the session lock is on */
static char pending[64];		/* a note to open at the unlock */
static time_t pending_at;
#define PENDING_S	120

/* ------------------------------------------------------ the notes --- */

static void read_note(const char *name, struct note *n)
{
	char path[512], line[1024];
	struct stat st;
	FILE *f;

	memset(n, 0, sizeof(*n));
	snprintf(n->name, sizeof(n->name), "%s", name);
	snprintf(path, sizeof(path), "%s/%s", UI_NOTIFY_DIR, name);
	n->when = stat(path, &st) == 0 ? st.st_mtime : time(NULL);
	f = fopen(path, "re");
	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (strncmp(line, "source=", 7) == 0)
			snprintf(n->source, sizeof(n->source), "%s", line + 7);
		else if (strncmp(line, "open=", 5) == 0)
			snprintf(n->open, sizeof(n->open), "%s", line + 5);
		else if (strncmp(line, "text=", 5) == 0)
			snprintf(n->text, sizeof(n->text), "%s", line + 5);
		else if (!n->text[0])	/* a plain-text notification */
			snprintf(n->text, sizeof(n->text), "%s", line);
	}
	fclose(f);
	if (!n->source[0])
		snprintf(n->source, sizeof(n->source), "default");
}

static int newest_first(const void *pa, const void *pb)
{
	const struct note *a = pa, *b = pb;

	if (a->when != b->when)
		return a->when < b->when ? 1 : -1;
	return strcmp(b->name, a->name);
}

static bool in(const struct note *arr, int n, const char *name)
{
	for (int i = 0; i < n; i++)
		if (strcmp(arr[i].name, name) == 0)
			return true;
	return false;
}

/* Re-read the directory; returns the newest note not seen before, or NULL */
static const char *reload(void)
{
	static struct note old[MAX_NOTES];
	static char fresh[64];
	int nold = nnotes;
	DIR *d = opendir(UI_NOTIFY_DIR);
	struct dirent *e;

	memcpy(old, notes, sizeof(notes));
	nnotes = 0;
	fresh[0] = 0;
	if (d != NULL) {
		while ((e = readdir(d)) != NULL && nnotes < MAX_NOTES) {
			if (e->d_name[0] == '.' ||
			    strlen(e->d_name) >= sizeof(notes[0].name))
				continue;
			read_note(e->d_name, &notes[nnotes++]);
		}
		closedir(d);
	}
	qsort(notes, nnotes, sizeof(notes[0]), newest_first);
	list.count = nnotes;
	for (int i = 0; i < nnotes && !fresh[0]; i++)
		if (!in(old, nold, notes[i].name))
			snprintf(fresh, sizeof(fresh), "%s", notes[i].name);
	return fresh[0] ? fresh : NULL;
}

static struct note *find(const char *name)
{
	for (int i = 0; i < nnotes; i++)
		if (strcmp(notes[i].name, name) == 0)
			return &notes[i];
	return NULL;
}

static void when_text(time_t t, char *buf, int len)
{
	time_t now = time(NULL);
	struct tm tm, today;

	if (now - t < 60) {
		snprintf(buf, len, "now");
		return;
	}
	if (now - t < 3600) {
		snprintf(buf, len, "%d min", (int)(now - t) / 60);
		return;
	}
	localtime_r(&t, &tm);
	localtime_r(&now, &today);
	if (tm.tm_yday == today.tm_yday && tm.tm_year == today.tm_year) {
		if (ui_setting_int("clock", 12) == 24)
			strftime(buf, len, "%H:%M", &tm);
		else
			snprintf(buf, len, "%d:%02d %s", tm.tm_hour % 12 ?
				 tm.tm_hour % 12 : 12, tm.tm_min,
				 tm.tm_hour < 12 ? "AM" : "PM");
	} else {
		strftime(buf, len, "%b %d", &tm);
	}
}

/* The app's name and icon for a source, or the source itself */
static void source_label(const char *source, char *name, int nlen,
			 char *icon, int ilen, uint32_t *color)
{
	struct ui_appinfo app;

	if (ui_app_for(source, &app)) {
		snprintf(name, nlen, "%s", app.name);
		snprintf(icon, ilen, "%s", app.icon[0] ? app.icon : "•");
		*color = app.color;
	} else {
		snprintf(name, nlen, "%c%s", source[0] >= 'a' && source[0] <= 'z' ?
			 source[0] - 32 : source[0], source + 1);
		snprintf(icon, ilen, "•");
		*color = UI_ACCENT;
	}
}

/* -------------------------------------------------------- drawing --- */

static void draw_note(struct ui_canvas *c, const struct note *n, int x, int y,
		      int w, int h)
{
	char name[64], icon[16], when[32];
	uint32_t color;

	source_label(n->source, name, sizeof(name), icon, sizeof(icon), &color);
	when_text(n->when, when, sizeof(when));
	ui_round(c, x + 12, y + 12, 40, 40, 20, color);
	ui_text_box(c, x + 12, y + 20, 40, UI_BOLD, 20, 0xffffffff, UI_CENTER,
		    icon);
	int ww = ui_text_width(UI_SANS, 15, when);
	ui_text_box(c, x + 64, y + 8, w - 84 - ww, UI_BOLD, 17, UI_FG, UI_LEFT,
		    name);
	ui_text(c, x + w - 12 - ww, y + 10, UI_SANS, 15, UI_DIM, when);
	/* Two lines of the text at most */
	char line1[512];
	snprintf(line1, sizeof(line1), "%s", n->text);
	int lh = ui_line_height(17);
	if (ui_text_wrap(NULL, 0, 0, w - 80, UI_SANS, 17, 0, line1) <= lh) {
		ui_text(c, x + 64, y + 34, UI_SANS, 17, UI_FG, line1);
		return;
	}
	/* Cut where the first line ends, then ellipsize the rest */
	int cut = 0, len = strlen(line1);
	for (int i = 1; i <= len; i++) {
		if (i < len && (line1[i] & 0xc0) == 0x80)
			continue;
		char save = line1[i];
		line1[i] = 0;
		int wd = ui_text_width(UI_SANS, 17, line1);
		line1[i] = save;
		if (wd > w - 80)
			break;
		if (line1[i] == ' ' || i == len)
			cut = i;
	}
	if (cut == 0)
		cut = len / 2;
	char first[512];
	snprintf(first, sizeof(first), "%.*s", cut, line1);
	ui_text(c, x + 64, y + 34, UI_SANS, 17, UI_FG, first);
	ui_text_box(c, x + 64, y + 34 + lh, w - 80, UI_SANS, 17, UI_FG, UI_LEFT,
		    line1 + cut + (line1[cut] == ' '));
}

static void draw_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
		     int w, int h, bool sel)
{
	draw_note(c, &notes[i], x, y, w, h);
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void draw_quick(struct ui *ui, struct ui_canvas *c);
static void draw_power(struct ui *ui, struct ui_canvas *c);

/* The notifications start under the player's card, while there is one */
static int list_top(void)
{
	return UI_HEADER_H + (ui_np.on ? UI_NP_CARD_H + 16 : 0);
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	if (!listing && osd) {
		ui_fill(c, 0, 0, c->w, c->h, UI_BAR);
		ui_fill(c, 0, c->h - 3, c->w, 3, UI_ACCENT);
		ui_text(c, 16, 8, UI_SANS, 17, UI_FG,
			!strcmp(osd_kind, "call") ? "Call volume" : "Volume");
		char n[16];
		snprintf(n, sizeof(n), "%d", osd_level);
		ui_text(c, c->w - 16 - ui_text_width(UI_BOLD, 17, n), 8, UI_BOLD, 17,
			UI_FG, n);
		int bx = 16, bw = c->w - 32, seg = osd_max ? bw / osd_max : bw;
		for (int i = 0; i < osd_max; i++)
			ui_round(c, bx + i * seg + 1, 36, seg - 3, 16, 4,
				 i < osd_level ? UI_ACCENT : UI_BG);
		return;
	}
	if (!listing) {
		struct note *n = find(banner);
		ui_fill(c, 0, 0, c->w, c->h, UI_BAR);
		ui_fill(c, 0, c->h - 3, c->w, 3, UI_ACCENT);
		if (n != NULL)
			draw_note(c, n, 0, 6, c->w, c->h - 6);
		return;
	}
	if (powermenu) {
		draw_power(ui, c);
		return;
	}
	if (quick) {
		draw_quick(ui, c);
		return;
	}
	struct note *d = detail[0] ? find(detail) : NULL;
	if (d != NULL) {
		char name[64], icon[16], when[32];
		uint32_t color;
		source_label(d->source, name, sizeof(name), icon, sizeof(icon),
			     &color);
		when_text(d->when, when, sizeof(when));
		ui_header(ui, c, name);
		ui_text(c, 16, UI_HEADER_H + 12, UI_SANS, 16, UI_DIM, when);
		ui_text_wrap(c, 16, UI_HEADER_H + 40, c->w - 32, UI_SANS, 20,
			     UI_FG, d->text);
		ui_hints(c, "Back", NULL, "C clear");
		return;
	}
	detail[0] = 0;

	char title[48];
	snprintf(title, sizeof(title), nnotes ? "Notifications (%d)" :
		 "Notifications", nnotes);
	ui_header(ui, c, title);
	int ly = list_top();
	if (ui_np.on)
		ui_np_card(ui, c, 12, UI_HEADER_H + 8, c->w - 24, np_sel, 255);
	else
		np_sel = false;
	if (nnotes == 0) {
		ui_text_box(c, 0, (ly + c->h - UI_HINTS_H) / 2 - 12, c->w, UI_SANS, 20,
			    UI_DIM, UI_CENTER, "Nothing new");
	} else {
		ui_list_draw(ui, c, &list, ly, c->h - ly - UI_HINTS_H, draw_row);
		/* The card chosen: the row the list marked, unmarked */
		int rows = (c->h - ly - UI_HINTS_H) / list.row_h;
		if (np_sel && list.sel >= list.top && list.sel < list.top + rows) {
			int w = list.count > rows ? c->w - 6 : c->w;
			int ry = ly + (list.sel - list.top) * list.row_h;
			ui_fill(c, 0, ry, w, list.row_h, UI_BG);
			draw_row(ui, c, list.sel, 0, ry, w, list.row_h, false);
		}
	}
	if (np_sel)
		ui_hints(c, "Close", ui_np.paused ? "Play" : "Pause", "C stop  ◂▸ scrub");
	else
		ui_hints(c, "Close", nnotes ? (locked ? "Unlock" : "Open") : NULL,
			 nnotes ? "C clear  # all" : NULL);
}

/* -------------------------------------------------- quick settings --- */

#define FLASH		"/sys/class/leds/white:flash/brightness"
#define BACKLIGHT	"/sys/class/backlight/backlight"
#define NTILES		7
#define TILE_ROWS	((NTILES + 1) / 2)
enum { T_WIFI, T_BT, T_CELL, T_DATA, T_SOUND, T_FLASH, T_VPN };

static int read_int(const char *path, int def)
{
	char buf[32];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;

	if (fd >= 0)
		close(fd);
	if (n <= 0)
		return def;
	buf[n] = 0;
	return atoi(buf);
}

static bool write_str(const char *path, const char *v)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	bool ok = fd >= 0 && write(fd, v, strlen(v)) == (ssize_t)strlen(v);

	if (fd >= 0)
		close(fd);
	return ok;
}

/* 1 on, 0 off (soft blocked), -1 none of that type */
static int radio_on(const char *type)
{
	char path[64], t[32];

	for (int i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%d/type", i);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		ssize_t n = fd >= 0 ? read(fd, t, sizeof(t) - 1) : -1;
		if (fd >= 0)
			close(fd);
		if (n <= 0)
			continue;
		t[n] = 0;
		t[strcspn(t, "\n")] = 0;
		if (strcmp(t, type))
			continue;
		snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%d/soft", i);
		return read_int(path, 0) ? 0 : 1;
	}
	return -1;
}

static void radio_set(int type, bool on)
{
	struct rfkill_event ev = { .type = type, .op = RFKILL_OP_CHANGE_ALL,
		.soft = !on };
	int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);

	if (fd >= 0) {
		if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
			fprintf(stderr, "s22-shell: rfkill: %s\n", strerror(errno));
		close(fd);
	}
	ui_setting_set(type == RFKILL_TYPE_WLAN ? "wifi" : "bluetooth", on ? "on" : "off");
}

/* 0 sounds, 1 vibrate only, 2 silent */
static int sound_mode(void)
{
	char v[16];
	bool sounds = strcmp(ui_setting("sounds", "on", v, sizeof(v)), "off") != 0;
	bool vib = strcmp(ui_setting("vibrate", "on", v, sizeof(v)), "off") != 0;

	return sounds ? 0 : vib ? 1 : 2;
}

static const char *vpn_state(char *buf, int len)
{
	char line[128];
	FILE *f = fopen("/run/s22-vpn/state", "re");

	snprintf(buf, len, "none");
	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, "mullvad=", 8)) {
			line[strcspn(line, "\n")] = 0;
			snprintf(buf, len, "%s", line + 8);
		}
	if (f)
		fclose(f);
	return buf;
}

static int brightness_pct(void)
{
	int max = read_int(BACKLIGHT "/max_brightness", 255);

	return max > 0 ? (read_int(BACKLIGHT "/brightness", max) * 100 + max / 2) / max : 0;
}

static void brightness_step(int d)
{
	static const int levels[] = { 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
	int max = read_int(BACKLIGHT "/max_brightness", 255), cur = brightness_pct();
	int i = 0, n = 11;
	char buf[16];

	while (i < n - 1 && levels[i + 1] <= cur)
		i++;
	i += d;
	i = i < 0 ? 0 : i >= n ? n - 1 : i;
	snprintf(buf, sizeof(buf), "%d", levels[i] * max / 100);
	write_str(BACKLIGHT "/brightness", buf);
	ui_setting_set("brightness", buf);
}

/* A tile: its name, its state in words, and whether it is on */
static bool tile(int t, const char **name, char *state, int len)
{
	char v[32];
	int r;

	switch (t) {
	case T_WIFI:
		*name = "Wi-Fi";
		r = radio_on("wlan");
		snprintf(state, len, "%s", r < 0 ? "Not found" : r ? "On" : "Off");
		return r == 1;
	case T_BT:
		*name = "Bluetooth";
		r = radio_on("bluetooth");
		snprintf(state, len, "%s", r < 0 ? "Not found" : r ? "On" : "Off");
		return r == 1;
	case T_CELL:
		*name = "Cellular";
		r = strcmp(ui_setting("cellular", "on", v, sizeof(v)), "off") != 0;
		snprintf(state, len, "%s", r ? "On" : "Off");
		return r;
	case T_DATA:
		*name = "Mobile data";
		r = !strcmp(ui_setting("mobile_data", "off", v, sizeof(v)), "on");
		snprintf(state, len, "%s", r ? "When Wi-Fi is off" : "Off");
		return r;
	case T_SOUND:
		*name = "Sound";
		r = sound_mode();
		snprintf(state, len, "%s", r == 0 ? "On" : r == 1 ? "Vibrate only" : "Silent");
		return r == 0;
	case T_FLASH:
		*name = "Flashlight";
		r = read_int(FLASH, 0) > 0;
		snprintf(state, len, "%s", r ? "On" : "Off");
		return r;
	default:
		*name = "Mullvad VPN";
		vpn_state(v, sizeof(v));
		snprintf(state, len, "%s", !strcmp(v, "none") ? "Not set up" :
			 !strcmp(v, "up") ? "Connected" : !strcmp(v, "connecting") ?
			 "Connecting" : !strcmp(v, "off") ? "Off" : "Not connected");
		return strcmp(v, "none") && strcmp(v, "off");
	}
}

static bool wm_locked(void);

/* Tiles that need the phone unlocked to switch */
static bool tile_guarded(int t)
{
	return locked && (t == T_CELL || t == T_DATA || t == T_VPN);
}

static char quick_flash[48];		/* "Unlock to switch", under the tiles */

static void tile_switch(int t)
{
	char v[32];
	int m;

	quick_flash[0] = 0;
	if (tile_guarded(t) && wm_locked()) {
		snprintf(quick_flash, sizeof(quick_flash), "Unlock to switch this");
		return;
	}
	switch (t) {
	case T_WIFI:
		radio_set(RFKILL_TYPE_WLAN, radio_on("wlan") != 1);
		break;
	case T_BT:
		radio_set(RFKILL_TYPE_BLUETOOTH, radio_on("bluetooth") != 1);
		break;
	case T_CELL:
		ui_setting_set("cellular", strcmp(ui_setting("cellular", "on", v,
				sizeof(v)), "off") ? "off" : "on");
		break;
	case T_DATA:
		ui_setting_set("mobile_data", strcmp(ui_setting("mobile_data", "off", v,
				sizeof(v)), "on") ? "on" : "off");
		break;
	case T_SOUND:			/* sounds, vibrate only, silent, round */
		m = (sound_mode() + 1) % 3;
		ui_setting_set("sounds", m == 0 ? "on" : "off");
		ui_setting_set("vibrate", m == 2 ? "off" : "on");
		break;
	case T_FLASH:
		write_str(FLASH, read_int(FLASH, 0) > 0 ? "0" : "1");
		break;
	case T_VPN:
		if (strcmp(vpn_state(v, sizeof(v)), "none"))
			ui_setting_set("mullvad", strcmp(ui_setting("mullvad", "off", v,
					sizeof(v)), "on") ? "on" : "off");
		break;
	}
}

/* The tiles two a row, then the brightness */
#define TILE_H		80
static void draw_quick(struct ui *ui, struct ui_canvas *c)
{
	int w = (c->w - 36) / 2, y0 = UI_HEADER_H + 12;

	ui_header(ui, c, "Quick settings");
	for (int t = 0; t < NTILES; t++) {
		const char *name;
		char state[48];
		bool on = tile(t, &name, state, sizeof(state));
		int x = 12 + (t % 2) * (w + 12), y = y0 + (t / 2) * (TILE_H + 12);
		if (t == quick_sel)
			ui_round(c, x - 4, y - 4, w + 8, TILE_H + 8, 16, UI_ACCENT);
		ui_round(c, x, y, w, TILE_H, 12, on ? UI_SEL : UI_BAR);
		ui_text_box(c, x + 14, y + 12, w - 28, UI_BOLD, 20, on ? UI_FG : UI_DIM,
			    UI_LEFT, name);
		ui_text_box(c, x + 14, y + 46, w - 28, UI_SANS, 16, on ? UI_ACCENT : UI_DIM,
			    UI_LEFT, state);
		if (tile_guarded(t)) {	/* a padlock, top right */
			int px = x + w - 26, py = y + 12;
			uint32_t lc = on ? UI_FG : UI_DIM;
			ui_fill(c, px + 2, py, 8, 2, lc);
			ui_fill(c, px + 2, py, 2, 8, lc);
			ui_fill(c, px + 8, py, 2, 8, lc);
			ui_round(c, px, py + 7, 12, 10, 2, lc);
		}
	}
	int y = y0 + TILE_ROWS * (TILE_H + 12) + 8, pct = brightness_pct();
	char b[32];
	if (quick_sel == NTILES)
		ui_round(c, 8, y - 4, c->w - 16, 76, 16, UI_ACCENT);
	ui_round(c, 12, y, c->w - 24, 68, 12, UI_BAR);
	snprintf(b, sizeof(b), "Brightness  %d%%", pct);
	ui_text_box(c, 26, y + 10, c->w - 52, UI_SANS, 18, UI_FG, UI_LEFT, b);
	ui_fill(c, 26, y + 46, c->w - 52, 6, UI_BG);
	ui_fill(c, 26, y + 46, (c->w - 52) * pct / 100, 6, UI_ACCENT);
	if (quick_flash[0])
		ui_text_box(c, 12, y + 84, c->w - 24, UI_SANS, 17, UI_WARN, UI_CENTER,
			    quick_flash);
	ui_hints(c, "Close", quick_sel == NTILES ? NULL :
		 tile_guarded(quick_sel) ? "Locked" : "Switch",
		 quick_sel == NTILES ? "◂▸ change" : NULL);
}

static void key_quick(struct ui *ui, uint32_t code)
{
	quick_flash[0] = 0;
	switch (code) {
	case KEY_UP:
		quick_sel = quick_sel == NTILES ? (NTILES - 1) / 2 * 2 :
			    quick_sel >= 2 ? quick_sel - 2 : quick_sel;
		break;
	case KEY_DOWN:
		quick_sel = quick_sel + 2 >= NTILES ? NTILES : quick_sel + 2;
		break;
	case KEY_LEFT:
	case KEY_RIGHT:
		if (quick_sel == NTILES)
			brightness_step(code == KEY_RIGHT ? 1 : -1);
		else {
			int t = (quick_sel / 2) * 2 + (code == KEY_RIGHT);
			quick_sel = t < NTILES ? t : quick_sel;
		}
		break;
	case KEY_ENTER:
	case KEY_OK:
		if (quick_sel < NTILES)
			tile_switch(quick_sel);
		break;
	}
	ui_redraw(ui);
}

/* ------------------------------------------------------ power menu --- */

static const struct {
	const char *label, *detail, *arg, *doing;
} power_items[] = {
	{ "Power off", "Turn the phone off", "off", "Powering off…" },
	{ "Reboot", "Start again", "reboot", "Rebooting…" },
	{ "Reboot to lk2nd", "Its fastboot, for flashing over USB", "lk2nd",
	  "Rebooting to lk2nd…" },
};
#define NPOWER	3

static void draw_power(struct ui *ui, struct ui_canvas *c)
{
	int y = UI_HEADER_H + 16;

	ui_header(ui, c, "Power");
	if (power_doing) {
		ui_text_box(c, 16, c->h / 2 - 20, c->w - 32, UI_BOLD, 24, UI_FG, UI_CENTER,
			    power_doing);
		return;
	}
	for (int i = 0; i < NPOWER; i++) {
		int ry = y + i * 84;
		if (i == power_sel)
			ui_round(c, 8, ry - 4, c->w - 16, 80, 14, UI_ACCENT);
		ui_round(c, 12, ry, c->w - 24, 72, 12, UI_BAR);
		ui_text_box(c, 28, ry + 10, c->w - 56, UI_BOLD, 21, UI_FG, UI_LEFT,
			    power_items[i].label);
		ui_text_box(c, 28, ry + 42, c->w - 56, UI_SANS, 16, UI_DIM, UI_LEFT,
			    power_items[i].detail);
	}
	ui_hints(c, "Cancel", "Choose", NULL);
}

static void power_choose(struct ui *ui, int i)
{
	char cmd[96];

	power_doing = power_items[i].doing;
	ui_redraw(ui);
	snprintf(cmd, sizeof(cmd), "sudo -n /usr/local/bin/s22-power %s", power_items[i].arg);
	ui_spawn(cmd);
}

static void key_power(struct ui *ui, uint32_t code)
{
	if (power_doing)
		return;
	if (code == KEY_UP && power_sel > 0)
		power_sel--;
	else if (code == KEY_DOWN && power_sel < NPOWER - 1)
		power_sel++;
	else if (code == KEY_ENTER || code == KEY_OK)
		power_choose(ui, power_sel);
	ui_redraw(ui);
}

/* ---------------------------------------------------------- modes --- */

/* Ask s22-wm: our flag follows its "locked" and "unlocked", but a word may
 * be lost (the shell started while locked) */
static bool wm_locked(void)
{
	char b[16] = "";

	return ui_wm("locked", b, sizeof(b)) && !strncmp(b, "yes", 3);
}

static void show_list(struct ui *ui)
{
	locked = wm_locked();
	listing = true;
	quick = false;
	powermenu = false;
	osd = false;
	banner[0] = 0;
	detail[0] = 0;
	ui_timer(ui, 0);
	list.sel = list.top = 0;
	np_sel = ui_np.on;
	ui_np_live(ui, true);
	ui_layer_show(ui, 0, true);
}

static void hide(struct ui *ui)
{
	if (power_doing)
		return;			/* going down: stays on screen */
	listing = false;
	quick = false;
	powermenu = false;
	osd = false;
	banner[0] = 0;
	np_sel = false;
	ui_np_key_cancel();
	ui_np_live(ui, false);
	ui_layer_hide(ui);
}

/* What a notification links to, or its app; then it is dismissed */
static void open_note(struct note *n)
{
	struct ui_appinfo app;

	if (n->open[0] && ui_open_uri(n->open)) {
		ui_notify_dismiss(n->name);
	} else if (ui_app_for(n->source, &app)) {
		ui_notify_dismiss(n->name);
		ui_open_app(&app);
	}
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st,
		const char *text)
{
	if (!listing)
		return;
	/* The player's card, chosen */
	if (np_sel && !quick && !powermenu && !detail[0]) {
		if (ui_np_key(ui, code, st))
			return;
		if (st == UI_PRESSED) {
			switch (code) {
			case KEY_OK:
			case KEY_ENTER:
				ui_np_toggle();
				return;
			case KEY_DOWN:
				if (nnotes) {
					np_sel = false;
					list.sel = list.top = 0;
				}
				ui_redraw(ui);
				return;
			case KEY_BACKSPACE:	/* C: stop, as dismissing */
			case KEY_DELETE:
				ui_np_stop();
				np_sel = false;
				return;
			case KEY_UP:
				return;
			}
		}
	}
	if (st == UI_RELEASED)
		return;
	if (powermenu) {
		if (code == KEY_BACK || code == KEY_ESC)
			hide(ui);
		else if (st == UI_PRESSED)
			key_power(ui, code);
		return;
	}
	if (quick) {
		if (code == KEY_BACK || code == KEY_ESC)
			hide(ui);
		else if (st == UI_PRESSED || code == KEY_LEFT || code == KEY_RIGHT)
			key_quick(ui, code);
		return;
	}
	if (code == KEY_UP && list.sel == 0 && ui_np.on && !detail[0]) {
		np_sel = true;		/* up to the player's card */
		ui_redraw(ui);
		return;
	}
	if (ui_list_key(&list, code)) {
		ui_redraw(ui);
		return;
	}
	if (st != UI_PRESSED)
		return;
	struct note *n = nnotes ? &notes[list.sel] : NULL;
	struct ui_appinfo app;

	if (detail[0]) {
		n = find(detail);
		if (code == KEY_BACK || code == KEY_ESC) {
			detail[0] = 0;
			ui_redraw(ui);
		} else if ((code == KEY_BACKSPACE || code == KEY_DELETE) && n) {
			ui_notify_dismiss(n->name);
			detail[0] = 0;
		} else if (code == KEY_NUMERIC_B) {
			hide(ui);
		}
		return;
	}
	switch (code) {
	case KEY_BACK:
	case KEY_ESC:
	case KEY_NUMERIC_B:
		hide(ui);
		return;
	case KEY_ENTER:
	case KEY_OK:
		if (n == NULL)
			return;
		/* Dismissed only when something opens; else shown whole */
		if (!n->open[0] && !ui_app_for(n->source, &app)) {
			snprintf(detail, sizeof(detail), "%s", n->name);
			ui_redraw(ui);
		} else if (wm_locked()) {
			/* After the PIN; the lock screen meanwhile */
			snprintf(pending, sizeof(pending), "%s", n->name);
			pending_at = time(NULL);
			hide(ui);
		} else {
			hide(ui);
			open_note(n);
		}
		return;
	case KEY_BACKSPACE:
	case KEY_DELETE:
		if (n != NULL)
			ui_notify_dismiss(n->name);
		return;		/* inotify redraws */
	case KEY_NUMERIC_POUND:
		for (int i = 0; i < nnotes; i++)
			ui_notify_dismiss(notes[i].name);
		return;
	}
}

static void tap(struct ui *ui, int x, int y)
{
	if (!listing) {
		/* The banner: open the list on it */
		char name[64];
		snprintf(name, sizeof(name), "%s", banner);
		show_list(ui);
		for (int i = 0; i < nnotes; i++)
			if (strcmp(notes[i].name, name) == 0)
				list.sel = i;
		return;
	}
	if (powermenu) {
		int i = (y - UI_HEADER_H - 16) / 84;
		if (!power_doing && y >= UI_HEADER_H + 16 && i < NPOWER) {
			power_sel = i;
			power_choose(ui, i);
		}
		return;
	}
	if (quick) {
		int w = (ui_width(ui) - 36) / 2, y0 = UI_HEADER_H + 12;
		int row = (y - y0) / (TILE_H + 12), col = x > 12 + w ? 1 : 0;
		if (y >= y0 && row < TILE_ROWS && row * 2 + col < NTILES) {
			quick_sel = row * 2 + col;
			tile_switch(quick_sel);
		} else if (row >= TILE_ROWS) {
			quick_sel = NTILES;
			brightness_step(x < ui_width(ui) / 2 ? -1 : 1);
		}
		ui_redraw(ui);
		return;
	}
	if (detail[0])
		return;
	if (ui_np.on && ui_np_card_tap(ui, 12, UI_HEADER_H + 8, ui_width(ui) - 24, x, y)) {
		np_sel = true;
		return;
	}
	int i = ui_list_at(&list, list_top(), y);
	if (i < 0)
		return;
	np_sel = false;
	list.sel = i;
	key(ui, KEY_ENTER, UI_PRESSED, "");
	ui_redraw(ui);
}

static void drag(struct ui *ui, int dx, int dy, bool done)
{
	if (!listing || detail[0] || quick || powermenu)
		return;
	ui_list_drag(&list, dy, ui_height(ui) - list_top() - UI_HINTS_H);
	ui_redraw(ui);
}

static void focus(struct ui *ui, bool focused)
{
	/* A window came to the front (Home, End, a call): get out of the way */
	if (!focused && listing)
		hide(ui);
}

static void tick(struct ui *ui)
{
	/* The banner's time is up */
	if (!listing)
		hide(ui);
	else if (quick) {		/* follow changes from elsewhere */
		ui_redraw(ui);
		ui_timer(ui, 1000);
	}
}

static void on_inotify(struct ui *ui, int fd)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

	while (read(fd, buf, sizeof(buf)) > 0)
		;
	const char *fresh = reload();
	if (listing) {
		ui_redraw(ui);
		return;
	}
	struct note *fn = fresh ? find(fresh) : NULL;
	if (fn && !strcmp(fn->source, "call") && fn->open[0]) {
		ui_wm("wake", NULL, 0);
		ui_open_uri(fn->open);
		return;
	}
	if (fresh != NULL) {
		osd = false;
		snprintf(banner, sizeof(banner), "%s", fresh);
		ui_wm("wake", NULL, 0);
		ui_layer_show(ui, BANNER_H, false);
		ui_redraw(ui);
		ui_timer(ui, BANNER_MS);
	} else if (banner[0] && find(banner) == NULL) {
		hide(ui);		/* dismissed while shown */
	}
}

static void on_ctl(struct ui *ui, int fd)
{
	char buf[64];
	int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	ssize_t n;

	if (c < 0)
		return;
	struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
	setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	n = read(c, buf, sizeof(buf) - 1);
	close(c);
	if (n <= 0)
		return;
	buf[n] = 0;
	if (sscanf(buf, "volume %15s %d %d", osd_kind, &osd_level, &osd_max) == 3) {
		if (listing)
			return;
		osd = true;
		banner[0] = 0;
		ui_layer_show(ui, OSD_H, false);
		ui_redraw(ui);
		ui_timer(ui, OSD_MS);
		return;
	}
	if (strncmp(buf, "locked", 6) == 0) {
		locked = true;
		pending[0] = 0;
		if (listing)
			hide(ui);
		return;
	}
	if (strncmp(buf, "unlocked", 8) == 0) {
		struct note *pn = pending[0] ? find(pending) : NULL;
		locked = false;
		if (pn && time(NULL) - pending_at <= PENDING_S)
			open_note(pn);
		pending[0] = 0;
		return;
	}
	if (strncmp(buf, "power", 5) == 0) {
		listing = true;
		quick = false;
		powermenu = true;
		power_sel = 0;
		osd = false;
		banner[0] = 0;
		detail[0] = 0;
		ui_layer_show(ui, 0, true);
		ui_redraw(ui);
		return;
	}
	if (strncmp(buf, "list", 4) == 0) {
		/* Notifications, then quick settings, then closed */
		if (listing && powermenu) {
			hide(ui);
		} else if (listing && !quick) {
			quick = true;
			quick_sel = 0;
			detail[0] = 0;
			ui_timer(ui, 1000);
			ui_redraw(ui);
		} else if (listing) {
			hide(ui);
		} else {
			reload();
			show_list(ui);
		}
	}
}

static void start(struct ui *ui)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un sa = { .sun_family = AF_UNIX };

	reload();		/* what is there now gets no banner */
	ui_np_follow(ui);
	in_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (in_fd >= 0 && inotify_add_watch(in_fd, UI_NOTIFY_DIR,
	    IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM) >= 0)
		ui_watch(ui, in_fd, on_inotify);
	else
		fprintf(stderr, "s22-shell: cannot watch %s\n", UI_NOTIFY_DIR);

	if (dir == NULL)
		return;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/s22-shell.sock", dir);
	unlink(sa.sun_path);
	ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (ctl_fd < 0 || bind(ctl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    listen(ctl_fd, 4) < 0) {
		perror("s22-shell: socket");
		return;
	}
	ui_watch(ui, ctl_fd, on_ctl);
}

int main(void)
{
	struct ui_app app = {
		.app_id = "s22-shell",
		.title = "Notifications",
		.draw = draw,
		.key = key,
		.tick = tick,
		.start = start,
		.focus = focus,
		.tap = tap,
		.drag = drag,
		.layer = true,
	};

	return ui_main(&app);
}
