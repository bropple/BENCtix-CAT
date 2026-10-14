// SPDX-License-Identifier: MIT
/*
 * s22-launcher: the home screen, a grid of apps.
 *
 * The apps are the .app files (see ui_apps() in s22ui.h), in Order.
 * D-pad moves, OK or Call opens, 1-9 open the app in that place. Apps
 * with Folder= share a folder tile, at the place of the first of them; it
 * opens to a grid of its own, Back closes it, and so does coming back to
 * the home screen, and Home (s22-wm sends home: when it is in front). The
 * wallpaper chosen in Settings is behind it (D066). An app with Badge=
 * shows what waits in it (unread texts, missed calls: D075) at its tile's
 * corner, and a folder the sum of its apps'.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "s22ui.h"

#define MAX_APPS	64
#define COLS		3
#define TILE_FG		0xffffffff	/* on the app's own color, any theme */
#define FOLDER_BG	0xff455a64

/* What the grid shows: apps, and folders (is_folder, named name) */
static struct ui_appinfo all[MAX_APPS], apps[MAX_APPS];
static bool is_folder[MAX_APPS];
static int nall, napps, sel;
static char folder[32];		/* the open folder, "" for none */

static void load_all(void)
{
	nall = ui_apps(all, MAX_APPS);
	napps = 0;
	for (int i = 0; i < nall; i++) {
		struct ui_appinfo *a = &all[i];
		if (a->hidden)
			continue;
		if (folder[0]) {
			if (!strcmp(a->folder, folder))
				apps[napps] = *a, is_folder[napps++] = false;
			continue;
		}
		if (!a->folder[0]) {
			apps[napps] = *a, is_folder[napps++] = false;
			continue;
		}
		int j;
		for (j = 0; j < napps; j++)
			if (is_folder[j] && !strcmp(apps[j].name, a->folder))
				break;
		if (j == napps) {
			memset(&apps[napps], 0, sizeof(apps[napps]));
			snprintf(apps[napps].name, sizeof(apps[napps].name), "%s", a->folder);
			apps[napps].color = FOLDER_BG;
			is_folder[napps++] = true;
		}
	}
	if (folder[0] && napps == 0) {	/* emptied: back to the top */
		folder[0] = 0;
		load_all();
		return;
	}
	if (sel >= napps)
		sel = napps ? napps - 1 : 0;
}

static void folder_close(struct ui *ui)
{
	char was[32];

	if (!folder[0])
		return;
	snprintf(was, sizeof(was), "%s", folder);
	folder[0] = 0;
	load_all();
	for (int i = 0; i < napps; i++)
		if (is_folder[i] && !strcmp(apps[i].name, was))
			sel = i;
	ui_redraw(ui);
}

static void open_entry(struct ui *ui, int i)
{
	if (!is_folder[i]) {
		ui_open_app(&apps[i]);
		return;
	}
	snprintf(folder, sizeof(folder), "%s", apps[i].name);
	sel = 0;
	load_all();
	ui_redraw(ui);
}

/* The grid on a w x h screen: tile size, rows shown, first row, top */
struct grid {
	int gap, tw, th, rows, first, y0;
};

#define MIN_TH		120	/* tiles get shorter, down to this, to fit */

static struct grid grid(int w, int hh)
{
	struct grid g = { .gap = 8 };
	int top = UI_HEADER_H, h = hh - UI_HEADER_H - UI_HINTS_H;
	int need = (napps + COLS - 1) / COLS;

	g.tw = (w - g.gap * (COLS + 1)) / COLS;
	/* As many rows as there are, if tiles of MIN_TH fit them; tiles no
	 * taller than wide, from the top */
	g.rows = (h - g.gap) / (MIN_TH + g.gap);
	if (need > 0 && need < g.rows)
		g.rows = need;
	if (g.rows < 1)
		g.rows = 1;
	g.th = (h - g.gap) / g.rows - g.gap;
	if (g.th > g.tw)
		g.th = g.tw;
	/* Scroll by whole rows so the selected one shows */
	if (sel / COLS >= g.rows)
		g.first = sel / COLS - g.rows + 1;
	g.y0 = top + g.gap;
	return g;
}

static void tap(struct ui *ui, int x, int y)
{
	struct grid g = grid(ui_width(ui), ui_height(ui));
	int col = (x - g.gap / 2) / (g.tw + g.gap);
	int row = (y - g.y0 + g.gap / 2) / (g.th + g.gap);

	if (y < g.y0 - g.gap / 2 || col < 0 || col >= COLS || row >= g.rows)
		return;
	int i = (g.first + row) * COLS + col;
	if (i >= 0 && i < napps) {
		sel = i;
		ui_redraw(ui);
		open_entry(ui, i);
	}
}

static void drag(struct ui *ui, int dx, int dy, bool done)
{
	static int acc;
	struct grid g = grid(ui_width(ui), ui_height(ui));

	/* Rows of the grid, like the list: finger up shows later rows */
	acc = done ? 0 : acc + dy;
	while (acc <= -(g.th + g.gap) && sel + COLS < napps) {
		sel += COLS;
		acc += g.th + g.gap;
	}
	while (acc >= g.th + g.gap && sel >= COLS) {
		sel -= COLS;
		acc -= g.th + g.gap;
	}
	ui_redraw(ui);
}

/* What waits in a tile's app, or in a folder's apps */
static int badge_of(int i)
{
	int n = 0;

	if (!is_folder[i])
		return ui_badge(apps[i].badge);
	for (int k = 0; k < nall; k++)
		if (!all[k].hidden && !strcmp(all[k].folder, apps[i].name))
			n += ui_badge(all[k].badge);
	return n;
}

/* How light a color is, 0 to 255 */
static int luma(uint32_t c)
{
	return (299 * (c >> 16 & 0xff) + 587 * (c >> 8 & 0xff) + 114 * (c & 0xff)) / 1000;
}

/* The count in a round mark at the tile's top right corner: the theme's
 * warning color, its number dark on a light one (the light theme's) */
static void draw_badge(struct ui_canvas *c, int x, int y, int tw, int n)
{
	char s[8];
	int size = 30, w;

	snprintf(s, sizeof(s), n > 99 ? "99+" : "%d", n);
	w = ui_text_width(UI_BOLD, 17, s) + 14;
	if (w < size)
		w = size;
	ui_round(c, x + tw - w - 6, y + 6, w, size, size / 2, UI_WARN);
	ui_text_box(c, x + tw - w - 6, y + 6 + (size - ui_line_height(17)) / 2, w, UI_BOLD, 17,
		    luma(UI_WARN) > 150 ? UI_FG : TILE_FG, UI_CENTER, s);
}

/* A folder's tile: its first four apps' tiles, small, and its name */
static void draw_folder(struct ui_canvas *c, const char *name, int x, int y,
			int tw, int th)
{
	int m = 4, area = th - 32 - 2 * m, s = (area - m) / 2;
	int x0 = x + (tw - 2 * s - m) / 2, y0 = y + 2 * m, n = 0;

	if (s > (tw - 3 * m) / 2)
		s = (tw - 3 * m) / 2;
	for (int i = 0; i < nall && n < 4; i++) {
		struct ui_appinfo *a = &all[i];
		if (a->hidden || strcmp(a->folder, name))
			continue;
		int sx = x0 + (n % 2) * (s + m), sy = y0 + (n / 2) * (s + m);
		char first[8];
		snprintf(first, sizeof(first), "%.1s", a->name);
		ui_round(c, sx, sy, s, s, 8, a->color);
		ui_text_box(c, sx, sy + s / 2 - s * 3 / 8, s, UI_BOLD, s / 2, TILE_FG,
			    UI_CENTER, a->icon[0] ? a->icon : first);
		n++;
	}
	ui_text_box(c, x + 4, y + th - 32, tw - 8, UI_SANS, 19, TILE_FG, UI_CENTER, name);
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	int top = UI_HEADER_H, h = c->h - UI_HEADER_H - UI_HINTS_H;
	struct grid g = grid(c->w, c->h);
	int gap = g.gap, tw = g.tw, th = g.th, rows_visible = g.rows;

	/* The wallpaper, if one is chosen (Settings, Display), 1:1 with the
	 * screen: the window is the whole screen, the status bar over it.
	 * Without its title bar, which the status bar hides but for a band
	 * below it, cutting off the wallpaper's top */
	if (!ui_wallpaper(c, UI_WALL_HOME))
		ui_header(ui, c, folder[0] ? folder : "Apps");
	if (napps == 0) {
		ui_text_box(c, 0, top + h / 2 - 12, c->w, UI_SANS, 20, UI_DIM,
			    UI_CENTER, "No apps installed");
		ui_hints(c, NULL, NULL, NULL);
		return;
	}
	int first_row = g.first, y0 = g.y0;

	for (int i = first_row * COLS; i < napps; i++) {
		int r = i / COLS - first_row, col = i % COLS;
		if (r >= rows_visible)
			break;
		int x = gap + col * (tw + gap), y = y0 + r * (th + gap);
		struct ui_appinfo *a = &apps[i];

		/* The ring round the chosen tile; a dark accent (the light
		 * theme's) inside a halo of the background, to show on any
		 * wallpaper */
		if (i == sel && luma(UI_ACCENT) < 110) {
			ui_round(c, x - 4, y - 4, tw + 8, th + 8, 18, UI_BG);
			ui_round(c, x - 2, y - 2, tw + 4, th + 4, 16, UI_ACCENT);
		} else if (i == sel) {
			ui_round(c, x - 4, y - 4, tw + 8, th + 8, 18, UI_ACCENT);
		}
		ui_round(c, x, y, tw, th, 14, a->color);
		if (i < 9) {
			char num[4];
			snprintf(num, sizeof(num), "%d", i + 1);
			ui_text(c, x + 9, y + 5, UI_SANS, 15, TILE_FG, num);
		}
		int waiting = badge_of(i);
		if (is_folder[i]) {
			draw_folder(c, a->name, x, y, tw, th);
			if (waiting)
				draw_badge(c, x, y, tw, waiting);
			continue;
		}
		const char *icon = a->icon[0] ? a->icon : a->name;
		char first[8];
		if (!a->icon[0])
			snprintf(first, sizeof(first), "%.1s", a->name);
		int isz = th >= 140 ? 56 : 46;
		ui_text_box(c, x, y + (th - 30) / 2 - isz * 2 / 3, tw, UI_BOLD, isz,
			    TILE_FG, UI_CENTER, a->icon[0] ? icon : first);
		ui_text_box(c, x + 4, y + th - 32, tw - 8, UI_SANS, 19, TILE_FG,
			    UI_CENTER, a->name);
		if (waiting)
			draw_badge(c, x, y, tw, waiting);
	}
	ui_hints(c, folder[0] ? "Close" : NULL, "Open", NULL);
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st,
		const char *text)
{
	if (st == UI_RELEASED || napps == 0)
		return;
	switch (code) {
	case KEY_BACK:
	case KEY_ESC:
		if (st == UI_PRESSED)
			folder_close(ui);
		return;
	case KEY_LEFT:
		if (sel > 0)
			sel--;
		break;
	case KEY_RIGHT:
		if (sel < napps - 1)
			sel++;
		break;
	case KEY_UP:
		if (sel >= COLS)
			sel -= COLS;
		break;
	case KEY_DOWN:
		if (sel + COLS < napps)
			sel += COLS;
		else if (sel / COLS < (napps - 1) / COLS)
			sel = napps - 1;
		break;
	case KEY_ENTER:
	case KEY_OK:
	case KEY_PHONE:
		if (st == UI_PRESSED)
			open_entry(ui, sel);
		return;
	case KEY_1 ... KEY_9:
		if (st == UI_PRESSED && code - KEY_1 < (unsigned)napps) {
			sel = code - KEY_1;
			open_entry(ui, sel);
		}
		break;
	default:
		return;
	}
	ui_redraw(ui);
}

static void tick(struct ui *ui)
{
	/* Keep the clock right, and pick up newly installed apps */
	load_all();
	ui_redraw(ui);
	ui_timer(ui, 30000);
}

/* Back on the home screen (Home, or an app closed): folders shut */
static void focus(struct ui *ui, bool in)
{
	if (in) {
		folder_close(ui);
		ui_redraw(ui);		/* the badges as they are now */
	}
}

/* A text came, a call was missed, or one was read: the badges again */
static void on_badges(struct ui *ui, int fd)
{
	char buf[4096];

	while (read(fd, buf, sizeof(buf)) > 0)
		;
	ui_redraw(ui);
}

/* home: s22-wm's Home, with the home screen in front already */
static void open_uri(struct ui *ui, const char *uri)
{
	if (strcmp(uri, "home:") == 0)
		folder_close(ui);
}

static void start(struct ui *ui)
{
	int fd = ui_badge_watch();

	if (fd >= 0)
		ui_watch(ui, fd, on_badges);
	load_all();
	ui_timer(ui, 30000);
}

int main(void)
{
	struct ui_app app = {
		.app_id = "s22-launcher",
		.title = "Apps",
		.draw = draw,
		.key = key,
		.tick = tick,
		.start = start,
		.tap = tap,
		.drag = drag,
		.focus = focus,
		.open = open_uri,
	};

	return ui_main(&app);
}
