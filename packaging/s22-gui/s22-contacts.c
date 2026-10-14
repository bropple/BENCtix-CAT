// SPDX-License-Identifier: MIT
/*
 * s22-contacts: the contacts that Phone and Messages show by name (s22ui's
 * UI_CONTACTS, a vCard file).
 *
 * The list: digits search it as smart dial does (the letters on the keys,
 * or a part of a number), C takes one back; the first rows make a new
 * contact or import a vCard file (.vcf in the home folder or Downloads;
 * numbers already there are skipped). A contact: OK on a number calls it,
 * # texts it, * changes it, C removes it (twice); and rows for its speed
 * dials (keys 2-9 held on Phone's keypad call the number last picked, or
 * the first), its ringtone and text tone (a picker that plays each), its
 * call and text vibrations (one that buzzes each), to rename it, add a
 * number and delete it (twice).
 *
 * Links: contact:list; contact:new:NUMBER starts a new contact with that
 * number (Phone's and Messages' "add to contacts").
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "s22ui.h"

enum view { V_LIST, V_VIEW, V_NAME, V_NUM, V_IMPORT, V_SPEED, V_TONE, V_VIB };
static enum view view = V_LIST;

static struct ui_list list = { .row_h = 66 };
static struct ui_list detail = { .row_h = 66 };
static struct ui_list files = { .row_h = 62 };
static struct ui_list speed = { .row_h = 62, .count = 8 };	/* keys 2-9 */
static int num_pick;			/* the number speed dial gives */
static struct ui_list tone = { .row_h = 62 };
static bool tone_text;			/* V_TONE: the text tone, else ringtone */
static char (*tone_files)[160];
static int ntone_files;
static struct ui_list vibl = { .row_h = 62 };
static bool vib_text;			/* V_VIB: the text vibration, else call */
static char vib_opts[24][96];		/* "", "none", presets, customs */
static char vib_names[24][32];
static int nvib_opts;
static char search[24];

static struct ui_contact cur;		/* the contact open, as last saved */
static struct ui_contact work;		/* being made or changed */
static bool is_new;
static int num_idx;			/* V_NUM: which number; nnums: a new one */
static int kind_idx;
static struct ui_entry name_entry, num_entry;
static const char *kinds[] = { "mobile", "home", "work", "other" };

static char flash[96];
static int confirm_row = -1;
static time_t confirm_until;

static char vcf[16][256];
static int nvcf;

static void say(const char *msg)
{
	snprintf(flash, sizeof(flash), "%s", msg);
}

/* Press twice within a few seconds */
static bool confirmed(int row, const char *what)
{
	if (confirm_row == row && time(NULL) <= confirm_until) {
		confirm_row = -1;
		return true;
	}
	confirm_row = row;
	confirm_until = time(NULL) + 4;
	snprintf(flash, sizeof(flash), "Press again to %s", what);
	return false;
}

/* ------------------------------------------------------------ the book --- */

static int find(const struct ui_contact *k)
{
	struct ui_contact *l;
	int n = ui_contacts(&l);

	for (int i = 0; i < n; i++)
		if (!strcmp(l[i].name, k->name) && l[i].nnums == k->nnums &&
		    (!k->nnums || !strcmp(l[i].num[0], k->num[0])))
			return i;
	return -1;
}

/* Save with old replaced by k (old NULL: k added; k NULL: old removed) */
static bool save(const struct ui_contact *old, const struct ui_contact *k)
{
	struct ui_contact *l;
	int n = ui_contacts(&l), at = old ? find(old) : -1;
	struct ui_contact *all = malloc((n + 1) * sizeof(*all));

	if (!all)
		return false;
	memcpy(all, l, n * sizeof(*all));
	if (at >= 0 && k)
		all[at] = *k;
	else if (at >= 0) {
		memmove(&all[at], &all[at + 1], (n - at - 1) * sizeof(*all));
		n--;
	} else if (k)
		all[n++] = *k;
	bool ok = ui_contacts_save(all, n);
	free(all);
	if (!ok)
		say("Cannot save the contacts");
	return ok;
}

/* The list's rows: [new, import,] the matching contacts */
static int shown[1024], nshown;

static void filter(void)
{
	struct ui_contact *l;
	int n = ui_contacts(&l);

	nshown = 0;
	for (int i = 0; i < n && nshown < 1024; i++) {
		bool hit = !search[0] || ui_t9_match(l[i].name, search);
		for (int k = 0; k < l[i].nnums && !hit; k++)
			hit = strstr(l[i].num[k], search) != NULL;
		if (hit)
			shown[nshown++] = i;
	}
	list.count = nshown + (search[0] ? 0 : 2);
	if (list.sel >= list.count)
		list.sel = list.count ? list.count - 1 : 0;
}

static void find_vcf(void)
{
	const char *home = getenv("HOME");
	char dir[256];

	nvcf = 0;
	for (int d = 0; d < 2 && home; d++) {
		snprintf(dir, sizeof(dir), "%s%s", home, d ? "/Downloads" : "");
		DIR *dp = opendir(dir);
		struct dirent *de;
		while (dp && (de = readdir(dp)) && nvcf < 16) {
			size_t len = strlen(de->d_name);
			if (len > 4 && !strcasecmp(de->d_name + len - 4, ".vcf"))
				snprintf(vcf[nvcf++], sizeof(vcf[0]), "%s/%s", dir, de->d_name);
		}
		if (dp)
			closedir(dp);
	}
	files.count = nvcf;
	files.sel = files.top = 0;
}

/* ------------------------------------------------------------- drawing --- */

static void list_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
		     int w, int h, bool sel)
{
	struct ui_contact *l;
	char line[96];
	int extra = search[0] ? 0 : 2;

	ui_contacts(&l);
	if (i < extra) {
		ui_text_box(c, x + 16, y + 18, w - 32, UI_SANS, 20, UI_ACCENT, UI_LEFT,
			    i == 0 ? "+ New contact" : "Import from a file ›");
	} else {
		const struct ui_contact *k = &l[shown[i - extra]];
		snprintf(line, sizeof(line), "%s%s", k->num[0], k->nnums > 1 ? " and more" : "");
		ui_text_box(c, x + 16, y + 8, w - 32, UI_SANS, 20, UI_FG, UI_LEFT, k->name);
		ui_text_box(c, x + 16, y + 36, w - 32, UI_SANS, 16, UI_DIM, UI_LEFT, line);
	}
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

/* The speed dial keys on any of the contact's numbers ("3, 5"), or "" */
static void speed_keys(char *buf, int len)
{
	char n[48];
	int o = 0;

	buf[0] = 0;
	for (int k = 2; k <= 9; k++) {
		if (!ui_speed_dial(k, n, sizeof(n)))
			continue;
		for (int i = 0; i < cur.nnums; i++)
			if (ui_same_number(n, cur.num[i])) {
				o += snprintf(buf + o, len - o, "%s%d", o ? ", " : "", k);
				break;
			}
	}
}

/* A contact's rows: its numbers, then speed dial, rename, add a number,
 * delete */
#define MORE	8
enum { M_SPEED, M_RING, M_TEXT, M_CALLVIB, M_TEXTVIB, M_RENAME, M_ADD, M_DELETE };
static void detail_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
		       int w, int h, bool sel)
{
	static const char *more[MORE] = { "Speed dial", "Ringtone", "Text tone",
					  "Call vibration", "Text vibration", "Rename",
					  "Add a number", "Delete contact" };

	if (i < cur.nnums) {
		ui_text_box(c, x + 16, y + 8, w - 32, UI_SANS, 21, UI_FG, UI_LEFT, cur.num[i]);
		ui_text_box(c, x + 16, y + 36, w - 32, UI_SANS, 16, UI_DIM, UI_LEFT,
			    cur.kind[i]);
	} else if (i == cur.nnums) {
		char keys[32], v[64];
		speed_keys(keys, sizeof(keys));
		snprintf(v, sizeof(v), keys[0] ? "Hold %s on Phone's keypad" : "None", keys);
		ui_text_box(c, x + 16, y + 8, w - 32, UI_SANS, 20, UI_ACCENT, UI_LEFT, more[0]);
		ui_text_box(c, x + 16, y + 36, w - 32, UI_SANS, 16, UI_DIM, UI_LEFT, v);
	} else if (i == cur.nnums + M_CALLVIB || i == cur.nnums + M_TEXTVIB) {
		bool call = i == cur.nnums + M_CALLVIB;
		const char *v = call ? cur.callvib : cur.textvib;
		ui_text_box(c, x + 16, y + 8, w - 32, UI_SANS, 20, UI_ACCENT, UI_LEFT,
			    more[i - cur.nnums]);
		ui_text_box(c, x + 16, y + 36, w - 32, UI_SANS, 16, UI_DIM, UI_LEFT,
			    v[0] ? ui_vib_name(v, call) : "Default");
	} else if (i == cur.nnums + M_RING || i == cur.nnums + M_TEXT) {
		const char *t = i == cur.nnums + M_RING ? cur.ringtone : cur.texttone;
		ui_text_box(c, x + 16, y + 8, w - 32, UI_SANS, 20, UI_ACCENT, UI_LEFT,
			    more[i - cur.nnums]);
		ui_text_box(c, x + 16, y + 36, w - 32, UI_SANS, 16, UI_DIM, UI_LEFT,
			    t[0] ? ui_sound_name(t) : "Default");
	} else {
		int k = i - cur.nnums;
		ui_text_box(c, x + 16, y + 20, w - 32, UI_SANS, 20,
			    k == M_DELETE ? UI_WARN : UI_ACCENT, UI_LEFT, more[k]);
	}
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

/* The tone picker: Default, Silent, then the sounds */
static void tone_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
		     int w, int h, bool sel)
{
	const char *now = tone_text ? cur.texttone : cur.ringtone;
	const char *path = i == 0 ? "" : i == 1 ? "silent" : tone_files[i - 2];
	bool chosen = !strcmp(now, path);

	ui_text_box(c, x + 16, y + 8, w - 32, UI_SANS, 20, UI_FG, UI_LEFT,
		    i == 0 ? (tone_text ? "Default (the Messages sound)" :
			      "Default (the phone's ringtone)") : ui_sound_name(path));
	if (chosen)
		ui_text_box(c, x + 16, y + 34, w - 32, UI_SANS, 15, UI_ACCENT, UI_LEFT,
			    "Chosen");
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void tone_open(bool text)
{
	const char *now = text ? cur.texttone : cur.ringtone;

	tone_text = text;
	ntone_files = ui_sounds(text ? "notifications" : "ringtones", &tone_files);
	tone.count = ntone_files + 2;
	tone.sel = !now[0] ? 0 : !strcmp(now, "silent") ? 1 : 0;
	for (int i = 0; i < ntone_files; i++)
		if (!strcmp(now, tone_files[i]))
			tone.sel = i + 2;
	tone.top = tone.sel > 3 ? tone.sel - 3 : 0;
	view = V_TONE;
}

/* The vibration picker: Default, None, the presets, the recorded ones */
static void vib_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
		    int w, int h, bool sel)
{
	const char *now = vib_text ? cur.textvib : cur.callvib;

	ui_text_box(c, x + 16, y + 8, w - 32, UI_SANS, 20, UI_FG, UI_LEFT, vib_names[i]);
	if (!strcmp(now, vib_opts[i]))
		ui_text_box(c, x + 16, y + 34, w - 32, UI_SANS, 15, UI_ACCENT, UI_LEFT,
			    "Chosen");
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void vib_open(bool text)
{
	const char *now = text ? cur.textvib : cur.callvib;
	const struct ui_vib *p;
	char customs[UI_VIB_CUSTOMS][96];
	int n;

	vib_text = text;
	nvib_opts = 0;
	snprintf(vib_opts[nvib_opts], 96, "%s", "");
	snprintf(vib_names[nvib_opts++], 32, "%s", text ? "Default (the Messages one)" :
		 "Default (the phone's)");
	snprintf(vib_opts[nvib_opts], 96, "none");
	snprintf(vib_names[nvib_opts++], 32, "None");
	p = ui_vib_presets(!text, &n);
	for (int i = 0; i < n && nvib_opts < 24; i++) {
		snprintf(vib_opts[nvib_opts], 96, "%s", p[i].pattern);
		snprintf(vib_names[nvib_opts++], 32, "%s", p[i].name);
	}
	ui_vib_customs(customs);
	for (int i = 0; i < UI_VIB_CUSTOMS && nvib_opts < 24; i++)
		if (customs[i][0]) {
			snprintf(vib_opts[nvib_opts], 96, "%s", customs[i]);
			snprintf(vib_names[nvib_opts++], 32, "Custom %d", i + 1);
		}
	vibl.count = nvib_opts;
	vibl.sel = 0;
	for (int i = 0; i < nvib_opts; i++)
		if (!strcmp(now, vib_opts[i]))
			vibl.sel = i;
	vibl.top = vibl.sel > 3 ? vibl.sel - 3 : 0;
	view = V_VIB;
}

/* A call's pattern twice round, as it repeats while ringing */
static void vib_feel(const char *v)
{
	char twice[200];

	if (!vib_text && v[0] && strcmp(v, "none")) {
		snprintf(twice, sizeof(twice), "%s,%s", v, v);
		ui_vibrate(twice);
	} else {
		ui_vibrate(v);
	}
}

/* Keys 2-9: whose they are */
static void speed_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
		      int w, int h, bool sel)
{
	char n[48], label[16], v[96];
	int key = i + 2;

	snprintf(label, sizeof(label), "%d", key);
	if (!ui_speed_dial(key, n, sizeof(n)))
		snprintf(v, sizeof(v), "Free");
	else if (ui_same_number(n, cur.num[num_pick]))
		snprintf(v, sizeof(v), "This number: OK takes it off");
	else
		snprintf(v, sizeof(v), "%s%s%s", ui_contact_name(n) ? ui_contact_name(n) : n,
			 ui_contact_name(n) ? ", " : "", ui_contact_name(n) ? n : "");
	ui_text_box(c, x + 16, y + 14, 40, UI_BOLD, 24, UI_FG, UI_LEFT, label);
	ui_text_box(c, x + 64, y + 18, w - 80, UI_SANS, 18, UI_DIM, UI_LEFT, v);
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void file_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
		     int w, int h, bool sel)
{
	const char *base = strrchr(vcf[i], '/');

	ui_text_box(c, x + 16, y + 8, w - 32, UI_SANS, 20, UI_FG, UI_LEFT,
		    base ? base + 1 : vcf[i]);
	ui_text_box(c, x + 16, y + 34, w - 32, UI_SANS, 15, UI_DIM, UI_LEFT, vcf[i]);
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void draw_entry(struct ui *ui, struct ui_canvas *c, struct ui_entry *e,
		       const char *label)
{
	int y = UI_HEADER_H + 16;

	ui_text_box(c, 16, y, c->w - 32, UI_SANS, 17, UI_DIM, UI_LEFT, label);
	ui_round(c, 12, y + 28, c->w - 24, 60, 10, UI_BAR);
	ui_entry_draw(c, e, 24, y + 40, c->w - 48, 24, 1);
	ui_entry_indicator(c, e, 0, 0, 0);
	if (view == V_NUM) {
		char k[48];
		snprintf(k, sizeof(k), "◂ %s ▸", kinds[kind_idx]);
		ui_text_box(c, 16, y + 104, c->w - 32, UI_SANS, 20, UI_FG, UI_CENTER, k);
	}
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	int h = c->h - UI_HEADER_H - UI_HINTS_H;
	const char *ok = NULL, *right = NULL;

	switch (view) {
	case V_LIST: {
		filter();
		int y = UI_HEADER_H;
		if (search[0]) {
			char s[64];
			snprintf(s, sizeof(s), "Search: %s", search);
			ui_fill(c, 0, y, c->w, 30, UI_BAR);
			ui_text_box(c, 16, y + 4, c->w - 32, UI_SANS, 17, UI_FG, UI_LEFT, s);
			y += 30;
			h -= 30;
		}
		ui_header(ui, c, "Contacts");
		ui_list_draw(ui, c, &list, y, h, list_row);
		if (!nshown && search[0])
			ui_text_box(c, 0, y + 40, c->w, UI_SANS, 18, UI_DIM, UI_CENTER,
				    "No match");
		ok = list.count ? "Open" : NULL;
		right = search[0] ? "C deletes" : "Keys search";
		break;
	}
	case V_VIEW:
		ui_header(ui, c, cur.name);
		detail.count = cur.nnums + MORE;
		if (detail.sel < cur.nnums)
			num_pick = detail.sel;
		ui_list_draw(ui, c, &detail, UI_HEADER_H, h, detail_row);
		if (detail.sel < cur.nnums) {
			ok = "Call";
			right = "# Text  * Edit";
		} else {
			ok = detail.sel == cur.nnums + M_DELETE ? "Delete" : "Open";
		}
		break;
	case V_VIB:
		ui_header(ui, c, vib_text ? "Text vibration" : "Call vibration");
		ui_list_draw(ui, c, &vibl, UI_HEADER_H, h, vib_row);
		ok = "Choose";
		break;
	case V_TONE:
		ui_header(ui, c, tone_text ? "Text tone" : "Ringtone");
		ui_list_draw(ui, c, &tone, UI_HEADER_H, h, tone_row);
		ok = "Choose";
		break;
	case V_SPEED: {
		char t[64];
		snprintf(t, sizeof(t), "Speed dial: %s", cur.num[num_pick]);
		ui_header(ui, c, t);
		ui_list_draw(ui, c, &speed, UI_HEADER_H, h, speed_row);
		ok = "Set";
		break;
	}
	case V_NAME:
		ui_header(ui, c, is_new ? "New contact" : "Rename");
		draw_entry(ui, c, &name_entry, "Name");
		ok = name_entry.text[0] ? "Next" : NULL;
		if (!is_new)
			ok = name_entry.text[0] ? "Save" : NULL;
		break;
	case V_NUM:
		ui_header(ui, c, work.name);
		draw_entry(ui, c, &num_entry, "Number");
		ok = num_entry.text[0] ? "Save" : NULL;
		right = "◂▸ type";
		break;
	case V_IMPORT:
		ui_header(ui, c, "Import contacts");
		if (!nvcf)
			ui_text_box(c, 16, UI_HEADER_H + 30, c->w - 32, UI_SANS, 18, UI_DIM,
				    UI_CENTER, "No .vcf file in the home folder or Downloads");
		ui_list_draw(ui, c, &files, UI_HEADER_H, h, file_row);
		ok = nvcf ? "Import" : NULL;
		break;
	}
	if (flash[0]) {
		int y = c->h - UI_HINTS_H - 44;
		ui_round(c, 20, y, c->w - 40, 38, 10, UI_WARN);
		ui_text_box(c, 30, y + 8, c->w - 60, UI_SANS, 18, UI_FG, UI_CENTER, flash);
	}
	ui_hints(c, view == V_LIST ? "Close" : view == V_NAME || view == V_NUM ?
		 "Cancel" : "Back", ok, right);
}

/* ---------------------------------------------------------------- keys --- */

static void open_contact(const struct ui_contact *k)
{
	cur = *k;
	detail.sel = detail.top = 0;
	view = V_VIEW;
}

static void start_new(const char *number)
{
	memset(&work, 0, sizeof(work));
	if (number && number[0]) {
		snprintf(work.num[0], sizeof(work.num[0]), "%s", number);
		snprintf(work.kind[0], sizeof(work.kind[0]), "mobile");
		work.nnums = 1;
	}
	is_new = true;
	ui_entry_set(&name_entry, "");
	view = V_NAME;
}

static void edit_number(int idx)
{
	work = cur;
	num_idx = idx;
	kind_idx = 0;
	if (idx < cur.nnums)
		for (int k = 0; k < 4; k++)
			if (!strcmp(cur.kind[idx], kinds[k]))
				kind_idx = k;
	ui_entry_set(&num_entry, idx < cur.nnums ? cur.num[idx] : "");
	is_new = false;
	view = V_NUM;
}

static void finish_name(void)
{
	snprintf(work.name, sizeof(work.name), "%s", name_entry.text);
	if (is_new && !work.nnums) {		/* a number next */
		num_idx = 0;
		kind_idx = 0;
		ui_entry_set(&num_entry, "");
		view = V_NUM;
		return;
	}
	if (save(is_new ? NULL : &cur, &work)) {
		say(is_new ? "Saved" : "Renamed");
		open_contact(&work);
	}
}

static void finish_number(void)
{
	snprintf(work.num[num_idx], sizeof(work.num[0]), "%s", num_entry.text);
	snprintf(work.kind[num_idx], sizeof(work.kind[0]), "%s", kinds[kind_idx]);
	if (num_idx >= work.nnums)
		work.nnums = num_idx + 1;
	if (save(is_new ? NULL : &cur, &work)) {
		say("Saved");
		open_contact(&work);
	}
}

static void call_or_text(const char *number, bool text)
{
	char uri[64];

	snprintf(uri, sizeof(uri), "%s:%s", text ? "sms" : "tel", number);
	ui_open_uri(uri);
}

static void key_entry(struct ui *ui, uint32_t code, enum ui_key_state st)
{
	struct ui_entry *e = view == V_NAME ? &name_entry : &num_entry;

	if (st == UI_PRESSED && (code == KEY_BACK || code == KEY_ESC)) {
		view = is_new ? V_LIST : V_VIEW;
	} else if (st == UI_PRESSED && (code == KEY_ENTER || code == KEY_OK)) {
		if (e->text[0])
			view == V_NAME ? finish_name() : finish_number();
	} else if (view == V_NUM && st == UI_PRESSED &&
		   (code == KEY_LEFT || code == KEY_RIGHT)) {
		kind_idx = (kind_idx + (code == KEY_RIGHT ? 1 : 3)) % 4;
	} else {
		ui_entry_key(e, code, st);
	}
	ui_redraw(ui);
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st, const char *text)
{
	if (view == V_NAME || view == V_NUM) {
		key_entry(ui, code, st);
		return;
	}
	if (st != UI_PRESSED)
		return;
	flash[0] = 0;
	if (view == V_LIST) {
		int extra = search[0] ? 0 : 2;
		struct ui_contact *l;
		ui_contacts(&l);
		filter();
		if (ui_list_key(&list, code)) {
		} else if (code >= KEY_1 && code <= KEY_0) {
			size_t n = strlen(search);
			if (n + 1 < sizeof(search)) {
				search[n] = code == KEY_0 ? '0' : '1' + (code - KEY_1);
				search[n + 1] = 0;
			}
			list.sel = list.top = 0;
		} else if (code == KEY_BACKSPACE && search[0]) {
			search[strlen(search) - 1] = 0;
		} else if (code == KEY_BACK || code == KEY_ESC) {
			if (search[0])
				search[0] = 0;
			else
				ui_quit(ui);
		} else if (code == KEY_ENTER || code == KEY_OK) {
			if (list.sel == 0 && extra) {
				start_new(NULL);
			} else if (list.sel == 1 && extra) {
				find_vcf();
				view = V_IMPORT;
			} else if (list.sel - extra < nshown)
				open_contact(&l[shown[list.sel - extra]]);
		}
	} else if (view == V_VIEW) {
		int s = detail.sel;
		detail.count = cur.nnums + MORE;
		if (ui_list_key(&detail, code)) {
		} else if (code == KEY_BACK || code == KEY_ESC) {
			view = V_LIST;
		} else if (s < cur.nnums) {
			if (code == KEY_ENTER || code == KEY_OK || code == KEY_PHONE)
				call_or_text(cur.num[s], false);
			else if (code == KEY_NUMERIC_POUND)
				call_or_text(cur.num[s], true);
			else if (code == KEY_NUMERIC_STAR)
				edit_number(s);
			else if (code == KEY_BACKSPACE && confirmed(s, "remove this number")) {
				work = cur;
				memmove(&work.num[s], &work.num[s + 1], (work.nnums - s - 1) *
					sizeof(work.num[0]));
				memmove(&work.kind[s], &work.kind[s + 1], (work.nnums - s - 1) *
					sizeof(work.kind[0]));
				work.nnums--;
				if (save(&cur, work.nnums ? &work : NULL)) {
					if (work.nnums)
						open_contact(&work);
					else
						view = V_LIST;
				}
			}
		} else if (code == KEY_ENTER || code == KEY_OK) {
			int k = s - cur.nnums;
			if (k == M_SPEED) {
				if (num_pick >= cur.nnums)
					num_pick = 0;
				speed.sel = speed.top = 0;
				view = V_SPEED;
			} else if (k == M_RING || k == M_TEXT) {
				tone_open(k == M_TEXT);
			} else if (k == M_CALLVIB || k == M_TEXTVIB) {
				vib_open(k == M_TEXTVIB);
			} else if (k == M_RENAME) {
				work = cur;
				is_new = false;
				ui_entry_set(&name_entry, cur.name);
				view = V_NAME;
			} else if (k == M_ADD && cur.nnums < UI_MAX_NUMS) {
				edit_number(cur.nnums);
			} else if (k == M_ADD) {
				say("A contact has four numbers at most");
			} else if (k == M_DELETE && confirmed(100, "delete this contact")) {
				if (save(&cur, NULL)) {
					say("Deleted");
					view = V_LIST;
				}
			}
		}
	} else if (view == V_VIB) {
		if (ui_list_key(&vibl, code)) {
			vib_feel(vib_opts[vibl.sel]);	/* feel each */
		} else if (code == KEY_BACK || code == KEY_ESC) {
			ui_vibrate_stop();
			view = V_VIEW;
		} else if (code == KEY_ENTER || code == KEY_OK) {
			ui_vibrate_stop();
			work = cur;
			snprintf(vib_text ? work.textvib : work.callvib, sizeof(work.callvib),
				 "%s", vib_opts[vibl.sel]);
			if (save(&cur, &work)) {
				open_contact(&work);
				detail.sel = cur.nnums + (vib_text ? M_TEXTVIB : M_CALLVIB);
			}
		}
	} else if (view == V_TONE) {
		if (ui_list_key(&tone, code)) {
			if (tone.sel >= 2)		/* hear each as it is chosen */
				ui_sound_preview(tone_files[tone.sel - 2]);
			else
				ui_sound_stop();
		} else if (code == KEY_BACK || code == KEY_ESC) {
			ui_sound_stop();
			view = V_VIEW;
		} else if (code == KEY_ENTER || code == KEY_OK) {
			const char *v = tone.sel == 0 ? "" : tone.sel == 1 ? "silent" :
					tone_files[tone.sel - 2];
			ui_sound_stop();
			work = cur;
			snprintf(tone_text ? work.texttone : work.ringtone,
				 sizeof(work.ringtone), "%s", v);
			if (save(&cur, &work)) {
				open_contact(&work);
				detail.sel = cur.nnums + (tone_text ? M_TEXT : M_RING);
			}
		}
	} else if (view == V_SPEED) {
		if (ui_list_key(&speed, code)) {
		} else if (code == KEY_BACK || code == KEY_ESC) {
			view = V_VIEW;
		} else if (code == KEY_ENTER || code == KEY_OK) {
			char n[48];
			int key = speed.sel + 2;
			bool mine = ui_speed_dial(key, n, sizeof(n)) &&
				    ui_same_number(n, cur.num[num_pick]);
			if (!ui_speed_dial_set(key, mine ? NULL : cur.num[num_pick]))
				say("Cannot save the speed dials");
			else {
				if (mine)
					snprintf(flash, sizeof(flash), "%d is free", key);
				else
					snprintf(flash, sizeof(flash), "Holding %d calls %s",
						 key, cur.name);
				view = V_VIEW;
			}
		}
	} else if (view == V_IMPORT) {
		if (ui_list_key(&files, code)) {
		} else if (code == KEY_BACK || code == KEY_ESC) {
			view = V_LIST;
		} else if ((code == KEY_ENTER || code == KEY_OK) && nvcf) {
			int n = ui_contacts_import(vcf[files.sel]);
			if (n < 0)
				say("Cannot read that file");
			else {
				snprintf(flash, sizeof(flash), "%d new contact%s", n, n == 1 ? "" : "s");
				view = V_LIST;
			}
		}
	}
	ui_redraw(ui);
}

static struct ui_list *cur_list(int *top)
{
	*top = UI_HEADER_H + (view == V_LIST && search[0] ? 30 : 0);
	return view == V_LIST ? &list : view == V_VIEW ? &detail :
	       view == V_IMPORT ? &files : view == V_SPEED ? &speed :
	       view == V_TONE ? &tone : view == V_VIB ? &vibl : NULL;
}

static void tap(struct ui *ui, int x, int y)
{
	int top;
	struct ui_list *l = cur_list(&top);

	if (!l)
		return;
	int i = ui_list_at(l, top, y);
	if (i < 0)
		return;
	l->sel = i;
	key(ui, KEY_OK, UI_PRESSED, "");
}

static void drag(struct ui *ui, int dx, int dy, bool done)
{
	int top;
	struct ui_list *l = cur_list(&top);

	if (l) {
		ui_list_drag(l, dy, ui_height(ui) - top - UI_HINTS_H);
		ui_redraw(ui);
	}
}

static void open_uri(struct ui *ui, const char *uri)
{
	if (!strncmp(uri, "contact:new:", 12)) {
		const struct ui_contact *k = ui_contact_for(uri + 12, NULL);
		if (k)
			open_contact(k);	/* known already: show it */
		else
			start_new(uri + 12);
	} else if (!strcmp(uri, "contact:list")) {
		view = V_LIST;
	}
	ui_redraw(ui);
}

static void tick(struct ui *ui)
{
	flash[0] = 0;
	ui_redraw(ui);
	ui_timer(ui, 4000);
}

static void start(struct ui *ui)
{
	ui_entry_init(ui, &name_entry, false);
	ui_entry_init(ui, &num_entry, true);
	ui_timer(ui, 4000);
}

int main(int argc, char **argv)
{
	struct ui_app app = {
		.app_id = "s22-contacts",
		.title = "Contacts",
		.draw = draw,
		.key = key,
		.tick = tick,
		.start = start,
		.open = open_uri,
		.tap = tap,
		.drag = drag,
		.uri = argc > 1 ? argv[1] : NULL,
	};

	return ui_main(&app);
}
