// SPDX-License-Identifier: MIT
/*
 * s22-pick: a question for scripts, in our own window: a line of text, or
 * one of a list. What was picked goes to standard output; Back gives exit
 * status 1 and nothing.
 *
 *   s22-pick -e TITLE [-l LABEL] [-t TEXT] [-o "A|B|..." [-s N]]
 *       Type a line (T9, as everywhere): OK gives it. With -o, a choice
 *       under it too, ◂ ▸ to change (from N, default 0): then OK gives
 *       the line and, on a second line, the choice's number.
 *   s22-pick -c TITLE [-w WAITING]
 *       Choose from the lines read on standard input, "TEXT" or
 *       "TEXT<TAB>DETAIL" (a second, dimmer line): OK gives the chosen
 *       one's number, from 0. Lines show as they arrive; until the input
 *       ends WAITING shows (default "Searching…"), and with no lines at
 *       all, "Nothing found". 2 and 8 move too.
 *
 * Maps' search (s22-maps-search) asks with it.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "s22ui.h"

#define MAX_ITEMS	100

#define MAX_OPTS	12

static const char *title = "", *label = "", *text0 = "", *waiting = "Searching…";
static char *opts[MAX_OPTS];
static int nopts, opt_sel;
static bool choosing, input_done;
static struct ui_entry entry;
static struct ui_list list;
static char *items[MAX_ITEMS], *details[MAX_ITEMS];
static char inbuf[8192];
static int inlen;

static void add_line(char *line)
{
	if (list.count >= MAX_ITEMS || !line[0])
		return;
	char *tab = strchr(line, '\t');
	if (tab)
		*tab++ = 0;
	items[list.count] = strdup(line);
	details[list.count] = strdup(tab ? tab : "");
	list.count++;
}

static void on_input(struct ui *ui, int fd)
{
	ssize_t n;

	while ((n = read(fd, inbuf + inlen, sizeof(inbuf) - 1 - inlen)) > 0) {
		inlen += n;
		inbuf[inlen] = 0;
		char *nl, *p = inbuf;
		while ((nl = strchr(p, '\n')) != NULL) {
			*nl = 0;
			add_line(p);
			p = nl + 1;
		}
		inlen -= p - inbuf;
		memmove(inbuf, p, inlen);
		if (inlen == sizeof(inbuf) - 1)		/* a line too long: cut */
			inlen = 0;
	}
	if (n == 0 || (n < 0 && errno != EAGAIN)) {
		inbuf[inlen] = 0;
		add_line(inbuf);
		input_done = true;
		ui_unwatch(ui, fd);
	}
	ui_redraw(ui);
}

static int view_h(struct ui *ui)
{
	return ui_height(ui) - UI_HEADER_H - UI_HINTS_H;
}

static void row(struct ui *ui, struct ui_canvas *c, int i, int x, int y, int w,
		int h, bool sel)
{
	if (sel)
		ui_fill(c, x, y, w, h, UI_SEL);
	ui_text_box(c, x + 16, y + 8, w - 32, UI_SANS, 19, UI_FG, UI_LEFT, items[i]);
	ui_text_box(c, x + 16, y + 34, w - 32, UI_SANS, 15, UI_DIM, UI_LEFT, details[i]);
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	if (choosing) {
		ui_list_draw(ui, c, &list, UI_HEADER_H, view_h(ui), row);
		if (!list.count)
			ui_text_box(c, 0, c->h / 2 - 12, c->w, UI_SANS, 18, UI_DIM, UI_CENTER,
				    input_done ? "Nothing found" : waiting);
		ui_header(ui, c, title);
		ui_hints(c, "Cancel", list.count ? "Choose" : NULL, NULL);
		return;
	}
	int y = UI_HEADER_H + 16;
	ui_header(ui, c, title);
	ui_text_box(c, 16, y, c->w - 32, UI_SANS, 17, UI_DIM, UI_LEFT, label);
	ui_round(c, 12, y + 28, c->w - 24, 60, 10, UI_BAR);
	ui_entry_draw(c, &entry, 24, y + 40, c->w - 48, 24, 1);
	ui_entry_indicator(c, &entry, 0, 0, 0);
	if (nopts) {
		char o[160];
		snprintf(o, sizeof(o), "◂  %s  ▸", opts[opt_sel]);
		ui_text_box(c, 16, y + 108, c->w - 32, UI_SANS, 20, UI_FG, UI_CENTER, o);
	}
	ui_hints(c, "Cancel", entry.text[0] ? "OK" : NULL, NULL);
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st, const char *text)
{
	if (st == UI_PRESSED && (code == KEY_BACK || code == KEY_ESC)) {
		ui_quit(ui);
		return;
	}
	if (!choosing) {
		if (st == UI_PRESSED && (code == KEY_ENTER || code == KEY_OK)) {
			if (entry.text[0]) {
				printf("%s\n", entry.text);
				if (nopts)
					printf("%d\n", opt_sel);
				fflush(stdout);
				exit(0);
			}
		} else if (nopts && st != UI_RELEASED && (code == KEY_LEFT || code == KEY_RIGHT)) {
			opt_sel = (opt_sel + (code == KEY_RIGHT ? 1 : nopts - 1)) % nopts;
		} else {
			ui_entry_key(&entry, code, st);
		}
		ui_redraw(ui);
		return;
	}
	if (st == UI_RELEASED)
		return;
	if ((code == KEY_ENTER || code == KEY_OK) && list.count) {
		if (st == UI_PRESSED) {
			printf("%d\n", list.sel);
			fflush(stdout);
			exit(0);
		}
	} else if (code == KEY_2 || code == KEY_8) {
		ui_list_key(&list, code == KEY_2 ? KEY_UP : KEY_DOWN);
	} else {
		ui_list_key(&list, code);
	}
	ui_redraw(ui);
}

static void tap(struct ui *ui, int x, int y)
{
	if (!choosing)
		return;
	int i = ui_list_at(&list, UI_HEADER_H, y);
	if (i >= 0 && i < list.count) {
		printf("%d\n", i);
		fflush(stdout);
		exit(0);
	}
}

static void drag(struct ui *ui, int dx, int dy, bool done)
{
	if (choosing) {
		ui_list_drag(&list, dy, view_h(ui));
		ui_redraw(ui);
	}
}

static void start(struct ui *ui)
{
	if (choosing) {
		list.row_h = 60;
		fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
		ui_watch(ui, 0, on_input);
	} else {
		ui_entry_init(ui, &entry, false);
		ui_entry_set(&entry, text0);
	}
}

int main(int argc, char **argv)
{
	int opt;
	bool asked = false;

	while ((opt = getopt(argc, argv, "e:c:l:t:w:o:s:")) != -1) {
		switch (opt) {
		case 'e': title = optarg; choosing = false; asked = true; break;
		case 'c': title = optarg; choosing = true; asked = true; break;
		case 'l': label = optarg; break;
		case 't': text0 = optarg; break;
		case 'w': waiting = optarg; break;
		case 'o':
			for (char *p = strtok(optarg, "|"); p && nopts < MAX_OPTS; p = strtok(NULL, "|"))
				opts[nopts++] = p;
			break;
		case 's': opt_sel = atoi(optarg); break;
		default: asked = false; optind = argc; break;
		}
	}
	if (!asked) {
		fprintf(stderr, "usage: s22-pick -e TITLE [-l LABEL] [-t TEXT] [-o \"A|B\" [-s N]] | -c TITLE [-w WAITING]\n");
		return 2;
	}
	if (opt_sel < 0 || opt_sel >= nopts)
		opt_sel = 0;
	struct ui_app app = {
		.app_id = "s22-pick",
		.title = title,
		.draw = draw,
		.key = key,
		.start = start,
		.tap = tap,
		.drag = drag,
	};
	ui_main(&app);
	return 1;	/* Back: nothing picked */
}
