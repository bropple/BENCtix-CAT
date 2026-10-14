// SPDX-License-Identifier: MIT
/*
 * s22-help: how to use the phone and each of its apps (D052).
 *
 * The topics are text files in HELP_DIR, read in name order: the first line
 * is the title, the rest the page. In a page:
 *
 *   # Heading
 *   KEYS<TAB>what they do          a row: the keys in bold, then the text
 *   - an item                      a bullet
 *   anything else                  a paragraph (a blank line between)
 *
 * The list: Up/Down choose, OK opens. A page: Up/Down (or 2/8) scroll,
 * Left/Right the topic before or after, a drag scrolls, Back returns to the
 * list. Links: help:NAME opens that topic (its file name without the
 * number and .txt, as in help:shell).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s22ui.h"

#define HELP_DIR	"/usr/share/s22/help"
#define MAX_TOPICS	40
#define TEXT_SIZE	17
#define KEYS_W		118	/* the keys' column */
#define MARGIN		14

struct topic {
	char file[64];
	char title[64];
	char *text;		/* the page, read when first opened */
};

static struct topic topics[MAX_TOPICS];
static int ntopics;
static struct ui_list list = { .row_h = 54 };
static int open_topic = -1;	/* the page shown, or -1 for the list */
static int scroll, page_h;

static int by_file(const void *a, const void *b)
{
	return strcmp(((const struct topic *)a)->file, ((const struct topic *)b)->file);
}

static char *read_all(const char *path)
{
	FILE *f = fopen(path, "re");
	char *buf = NULL;
	size_t len = 0;

	if (!f)
		return NULL;
	FILE *m = open_memstream(&buf, &len);
	char chunk[4096];
	size_t n;
	while (m && (n = fread(chunk, 1, sizeof(chunk), f)) > 0)
		fwrite(chunk, 1, n, m);
	fclose(f);
	if (m)
		fclose(m);
	return buf;
}

static void topics_load(void)
{
	DIR *d = opendir(HELP_DIR);
	struct dirent *de;

	while (d && (de = readdir(d)) && ntopics < MAX_TOPICS) {
		size_t n = strlen(de->d_name);
		if (n < 5 || strcmp(de->d_name + n - 4, ".txt") || n >= sizeof(topics[0].file))
			continue;
		struct topic *t = &topics[ntopics];
		snprintf(t->file, sizeof(t->file), "%s", de->d_name);
		char path[512];
		snprintf(path, sizeof(path), HELP_DIR "/%s", de->d_name);
		FILE *f = fopen(path, "re");
		if (!f)
			continue;
		if (!fgets(t->title, sizeof(t->title), f))
			t->title[0] = 0;
		fclose(f);
		t->title[strcspn(t->title, "\n")] = 0;
		if (t->title[0])
			ntopics++;
	}
	if (d)
		closedir(d);
	qsort(topics, ntopics, sizeof(topics[0]), by_file);
	list.count = ntopics;
}

/* The page's body: the file without its title line */
static const char *body(struct topic *t)
{
	if (!t->text) {
		char path[512];
		snprintf(path, sizeof(path), HELP_DIR "/%s", t->file);
		t->text = read_all(path);
	}
	const char *p = t->text ? strchr(t->text, '\n') : NULL;
	return p ? p + 1 : "";
}

/*
 * The page at y (scrolled), clipped by the canvas; returns its height. A
 * canvas one pixel tall measures it without drawing anything to be seen.
 */
static int draw_page(struct ui_canvas *c, int y0, const char *text)
{
	int y = y0, w = c->w - 2 * MARGIN;
	const char *p = text;
	char line[1024];

	while (*p) {
		size_t n = strcspn(p, "\n");
		snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) ? n : sizeof(line) - 1), p);
		p += n + (p[n] == '\n');
		if (!line[0]) {
			y += 10;
		} else if (line[0] == '#' && line[1] == ' ') {
			y += 6;
			y += ui_text_wrap(c, MARGIN, y, w, UI_BOLD, 19, UI_ACCENT, line + 2) + 4;
		} else if (line[0] == '-' && line[1] == ' ') {
			ui_text(c, MARGIN + 2, y, UI_SANS, TEXT_SIZE, UI_DIM, "•");
			y += ui_text_wrap(c, MARGIN + 18, y, w - 18, UI_SANS, TEXT_SIZE, UI_FG,
					  line + 2) + 3;
		} else if (strchr(line, '\t')) {
			char *tab = strchr(line, '\t');
			*tab = 0;
			int hk = ui_text_wrap(c, MARGIN, y, KEYS_W - 8, UI_BOLD, TEXT_SIZE - 1,
					      UI_FG, line);
			int ht = ui_text_wrap(c, MARGIN + KEYS_W, y, w - KEYS_W, UI_SANS,
					      TEXT_SIZE, UI_FG, tab + 1);
			y += (hk > ht ? hk : ht) + 6;
		} else {
			y += ui_text_wrap(c, MARGIN, y, w, UI_SANS, TEXT_SIZE, UI_FG, line) + 4;
		}
	}
	return y - y0 + 16;
}

static int view_h(struct ui *ui)
{
	return ui_height(ui) - UI_HEADER_H - UI_HINTS_H;
}

static void page_open(struct ui *ui, int i)
{
	uint32_t px[480];
	struct ui_canvas m = { .px = px, .w = 480, .h = 1, .stride = 480 };

	open_topic = i;
	scroll = 0;
	m.w = ui_width(ui) < 480 ? ui_width(ui) : 480;
	page_h = draw_page(&m, -100000, body(&topics[i]));
	ui_redraw(ui);
}

static void scroll_by(struct ui *ui, int d)
{
	int max = page_h - view_h(ui);

	scroll += d;
	if (scroll > max)
		scroll = max;
	if (scroll < 0)
		scroll = 0;
	ui_redraw(ui);
}

static void row(struct ui *ui, struct ui_canvas *c, int i, int x, int y, int w,
		int h, bool sel)
{
	ui_text_box(c, x + 16, y + 15, w - 32, UI_SANS, 20, UI_FG, UI_LEFT, topics[i].title);
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	if (open_topic < 0) {
		ui_list_draw(ui, c, &list, UI_HEADER_H, view_h(ui), row);
		ui_header(ui, c, "Help");
		if (!ntopics)
			ui_text_box(c, 0, c->h / 2 - 12, c->w, UI_SANS, 18, UI_DIM, UI_CENTER,
				    "No help installed");
		ui_hints(c, "Close", ntopics ? "Open" : NULL, NULL);
		return;
	}
	draw_page(c, UI_HEADER_H + 8 - scroll, body(&topics[open_topic]));
	ui_header(ui, c, topics[open_topic].title);
	/* A scroll bar when the page is longer than the screen */
	int vh = view_h(ui);
	if (page_h > vh) {
		int bh = vh * vh / page_h, by = UI_HEADER_H + (vh - bh) * scroll / (page_h - vh);
		ui_fill(c, c->w - 4, by, 3, bh, UI_DIM);
	}
	ui_hints(c, "Topics", NULL, "◂ ▸ topics");
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st, const char *text)
{
	if (st == UI_RELEASED)
		return;
	if (open_topic < 0) {
		if (code == KEY_BACK || code == KEY_ESC) {
			if (st == UI_PRESSED)
				ui_quit(ui);
		} else if ((code == KEY_ENTER || code == KEY_OK) && ntopics) {
			if (st == UI_PRESSED)
				page_open(ui, list.sel);
		} else if (code == KEY_2 || code == KEY_8) {
			ui_list_key(&list, code == KEY_2 ? KEY_UP : KEY_DOWN);
			ui_redraw(ui);
		} else if (ui_list_key(&list, code)) {
			ui_redraw(ui);
		}
		return;
	}
	switch (code) {
	case KEY_BACK:
	case KEY_ESC:
		if (st == UI_PRESSED) {
			open_topic = -1;
			ui_redraw(ui);
		}
		break;
	case KEY_UP:
		scroll_by(ui, -60);
		break;
	case KEY_DOWN:
		scroll_by(ui, 60);
		break;
	case KEY_2:
		scroll_by(ui, -(view_h(ui) - 40));
		break;
	case KEY_8:
		scroll_by(ui, view_h(ui) - 40);
		break;
	case KEY_LEFT:
	case KEY_RIGHT:
		if (st == UI_PRESSED) {
			int i = open_topic + (code == KEY_RIGHT ? 1 : -1);
			if (i >= 0 && i < ntopics) {
				list.sel = i;
				page_open(ui, i);
			}
		}
		break;
	}
}

static void tap(struct ui *ui, int x, int y)
{
	if (open_topic >= 0)
		return;
	int i = ui_list_at(&list, UI_HEADER_H, y);
	if (i >= 0) {
		list.sel = i;
		page_open(ui, i);
	}
}

static void drag(struct ui *ui, int dx, int dy, bool done)
{
	if (open_topic >= 0)
		scroll_by(ui, -dy);
	else {
		ui_list_drag(&list, dy, view_h(ui));
		ui_redraw(ui);
	}
}

/* help:NAME, the topic's file name without its number and .txt */
static void open_uri(struct ui *ui, const char *uri)
{
	const char *name = strchr(uri, ':');

	if (!name)
		return;
	name++;
	for (int i = 0; i < ntopics; i++) {
		const char *f = topics[i].file;
		const char *dash = strchr(f, '-');
		const char *base = dash ? dash + 1 : f;
		size_t n = strlen(name);
		if (!strncmp(base, name, n) && !strcmp(base + n, ".txt")) {
			list.sel = i;
			page_open(ui, i);
			return;
		}
	}
}

static void start(struct ui *ui)
{
	topics_load();		/* a starting link comes after, through open */
}

int main(int argc, char **argv)
{
	struct ui_app app = {
		.app_id = "s22-help",
		.title = "Help",
		.draw = draw,
		.key = key,
		.start = start,
		.tap = tap,
		.drag = drag,
		.open = open_uri,
	};

	if (argc > 1)
		app.uri = argv[1];
	return ui_main(&app);
}
