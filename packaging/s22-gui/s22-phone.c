// SPDX-License-Identifier: MIT
/*
 * s22-phone: the Phone app, on s22-calld (voice calls, D028).
 *
 * Three tabs, Left/Right between them: the keypad, the recent calls
 * (s22-calld's log, /var/lib/s22-call/log) and the contacts (s22ui's,
 * shared with Messages and Contacts). The keypad types a number; matching
 * contacts show below it, by number or by the letters on the keys (smart
 * dial: 5-6-2 finds "Jo..."), Up/Down to pick one. The Call key (or OK)
 * calls; on Recents and Contacts # writes a text instead (Messages), and
 * on Recents * saves the number to the contacts (or shows its contact).
 * Holding 1 on an empty keypad calls voicemail (the SIM's mailbox number,
 * which s22-calld reads), and that number shows as "Voicemail"; holding
 * 2-9 calls that key's speed dial (s22ui's, set in Contacts).
 *
 * The call screen follows s22-calld's state (/run/s22-call/state): a
 * ringing call is answered with the Call key or OK, rejected with Back or
 * C; during a call the keypad sends tones, OK mutes, Right (or the speaker
 * key, which s22-calld takes anywhere) switches the speaker, Back hides the screen
 * (the call goes on; the Call key brings it back). The End key and
 * closing the lid hang up, as s22-calld has them. When a call ends the
 * keypad comes back empty; a call that rang while another app was in
 * front closes Phone instead, back to that app.
 *
 * Showing the recent calls clears the missed calls' badge on the home
 * screen (D075), not their notifications.
 *
 * Each key typed on the keypad sounds its DTMF tone on the speaker at the
 * media volume (D085), unless sounds are off; in a call s22-calld sounds it at the ear as it
 * sends it.
 *
 * Links: tel:NUMBER calls it; call:incoming shows the call screen (s22-shell
 * opens it when a call rings); call:recents opens the recent calls.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "s22ui.h"

#define CALL_SOCK	"/run/s22-call.sock"
#define CALL_DIR	"/run/s22-call"
#define CALL_STATE	CALL_DIR "/state"
#define LOG_DIR		"/var/lib/s22-call"
#define CALL_LOG	LOG_DIR "/log"
#define VOICEMAIL	CALL_DIR "/voicemail"
#define HOLD_MS		500
#define MAX_LOG		200
#define MAX_SUGGEST	6
#define TABS_H		34
#define ENDED_MS	2000

enum view { V_DIAL, V_RECENTS, V_CONTACTS, V_CALL, V_PICK };
static enum view view = V_DIAL, before_pick;

struct call {
	int id;
	char dir[4], state[24], number[40];
	time_t since;
};
static struct call calls[4];
static int ncalls;
static bool muted, speaker;
static time_t ended_at;			/* the call screen says "ended" */
static bool focused;			/* the window in front */
static bool go_back;			/* the call came over another app: close
					 * when it ends */
static char ended_who[64];

struct logent {
	time_t t;
	char dir[4], number[40], result[16];
	int secs;
};
static struct logent logs[MAX_LOG];
static int nlogs;

static struct ui_entry num;		/* the keypad's number */
static char tones[64];			/* tones sent in this call */
static int suggest_sel = -1;
static const struct ui_contact *suggest[MAX_SUGGEST];
static int suggest_num[MAX_SUGGEST], nsuggest;
static struct ui_list recents_list = { .row_h = 66 };
static struct ui_list contacts_list = { .row_h = 66 };
static struct ui_list pick_list = { .row_h = 62 };
static const struct ui_contact *pick;	/* V_PICK: whose numbers */
static bool pick_text;			/* V_PICK: text rather than call */
static char flash[96];
static char voicemail[40];		/* the SIM's mailbox number */
static uint32_t held_key;		/* 1-9 down on an empty keypad, until
					 * it is let go or counts as held */
static int hold_timer = -1;		/* fires HOLD_MS after it went down */

static void load_voicemail(void)
{
	FILE *f = fopen(VOICEMAIL, "re");

	voicemail[0] = 0;
	if (f) {
		if (fgets(voicemail, sizeof(voicemail), f))
			voicemail[strcspn(voicemail, "\n")] = 0;
		fclose(f);
	}
}

/* ------------------------------------------------------------ s22-calld --- */

/* A command; its one-line answer in reply */
static bool call_cmd(char *reply, int len, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
static bool call_cmd(char *reply, int len, const char *fmt, ...)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	char line[128];
	va_list ap;
	int fd, n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", CALL_SOCK);
	reply[0] = 0;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
	    write(fd, line, n) != n) {
		snprintf(reply, len, "Calls are not available (s22-calld)");
		if (fd >= 0)
			close(fd);
		return false;
	}
	n = read(fd, reply, len - 1);
	close(fd);
	reply[n > 0 ? n : 0] = 0;
	reply[strcspn(reply, "\n")] = 0;
	return true;
}

static void load_state(void)
{
	char line[160];
	FILE *f = fopen(CALL_STATE, "re");

	ncalls = 0;
	muted = speaker = false;
	while (f && fgets(line, sizeof(line), f)) {
		struct call *c = &calls[ncalls];
		long since;
		if (ncalls < 4 && sscanf(line, "call %d %3s %23s %ld %39s", &c->id, c->dir,
					 c->state, &since, c->number) == 5) {
			c->since = since;
			if (!strcmp(c->number, "-"))
				c->number[0] = 0;
			ncalls++;
		} else if (!strncmp(line, "mute=", 5)) {
			muted = line[5] == '1';
		} else if (!strncmp(line, "speaker=", 8)) {
			speaker = line[8] == '1';
		}
	}
	if (f)
		fclose(f);
}

static void load_log(void)
{
	char line[200];
	struct logent all[MAX_LOG];
	int n = 0;
	FILE *f = fopen(CALL_LOG, "re");

	while (f && fgets(line, sizeof(line), f)) {
		struct logent *e = &all[n % MAX_LOG];
		long t;
		if (sscanf(line, "%ld\t%3s\t%39[^\t]\t%15s\t%d", &t, e->dir, e->number,
			   e->result, &e->secs) == 5) {
			e->t = t;
			n++;
		}
	}
	if (f)
		fclose(f);
	nlogs = n < MAX_LOG ? n : MAX_LOG;
	for (int i = 0; i < nlogs; i++)		/* newest first */
		logs[i] = all[(n - 1 - i) % MAX_LOG];
	recents_list.count = nlogs;
}

/* The call the screen is about: a ringing one first */
static struct call *main_call(void)
{
	for (int i = 0; i < ncalls; i++)
		if (!strcmp(calls[i].state, "incoming") || !strcmp(calls[i].state, "waiting"))
			return &calls[i];
	return ncalls ? &calls[0] : NULL;
}

static bool ringing(void)
{
	struct call *c = main_call();

	return c && (!strcmp(c->state, "incoming") || !strcmp(c->state, "waiting"));
}

static const char *who(const char *number)
{
	const char *name = ui_contact_name(number);

	if (!name && voicemail[0] && number[0] && ui_same_number(number, voicemail))
		return "Voicemail";
	return name ? name : number[0] ? number : "Unknown number";
}

static void say(struct ui *ui, const char *msg)
{
	snprintf(flash, sizeof(flash), "%s", msg);
	ui_redraw(ui);
}

static void dial(struct ui *ui, const char *number)
{
	char reply[160];

	if (!number[0])
		return;
	if (ncalls) {
		say(ui, "Already in a call");
		return;
	}
	if (!call_cmd(reply, sizeof(reply), "dial %s", number) ||
	    strncmp(reply, "dialling", 8) != 0) {
		say(ui, reply[0] ? reply : "Could not call");
		return;
	}
	tones[0] = 0;
	go_back = false;		/* placed here: back to the keypad */
	view = V_CALL;
	ui_entry_set(&num, "");
	ui_redraw(ui);
}

/* ------------------------------------------------------------- drawing --- */

static void fmt_when(time_t t, char *buf, int len)
{
	struct tm tm, now_tm;
	time_t now = time(NULL);
	char clock[16];

	localtime_r(&t, &tm);
	localtime_r(&now, &now_tm);
	char set[8];
	bool h24 = !strcmp(ui_setting("clock", "12", set, sizeof(set)), "24");
	if (h24)
		strftime(clock, sizeof(clock), "%H:%M", &tm);
	else {
		strftime(clock, sizeof(clock), "%l:%M %p", &tm);
		memmove(clock, clock + (clock[0] == ' '), strlen(clock));
	}
	if (tm.tm_year == now_tm.tm_year && tm.tm_yday == now_tm.tm_yday)
		snprintf(buf, len, "%s", clock);
	else if (now - t < 2 * 86400 && (now_tm.tm_yday - tm.tm_yday + 366) % 366 == 1)
		snprintf(buf, len, "Yesterday %s", clock);
	else
		strftime(buf, len, "%b %e", &tm);
}

static void fmt_secs(int s, char *buf, int len)
{
	if (s >= 3600)
		snprintf(buf, len, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(buf, len, "%d:%02d", s / 60, s % 60);
}

static void draw_tabs(struct ui_canvas *c)
{
	static const char *names[] = { "Keypad", "Recents", "Contacts" };
	int w = c->w / 3, y = UI_HEADER_H;

	ui_fill(c, 0, y, c->w, TABS_H, UI_BG);
	for (int i = 0; i < 3; i++) {
		bool on = (int)view == i;
		ui_text_box(c, i * w, y + 6, w, on ? UI_BOLD : UI_SANS, 18,
			    on ? UI_FG : UI_DIM, UI_CENTER, names[i]);
		if (on)
			ui_fill(c, i * w + 16, y + TABS_H - 3, w - 32, 3, UI_ACCENT);
	}
	ui_fill(c, 0, y + TABS_H - 1, c->w, 1, UI_BAR);
}

/* A bar back to the call, above the tabs' content, while one is hidden */
static int draw_back_to_call(struct ui_canvas *c, int y)
{
	char text[96], d[16];
	struct call *k = main_call();

	if (!k)
		return y;
	if (!strcmp(k->state, "conversation"))
		fmt_secs((int)(time(NULL) - k->since), d, sizeof(d));
	else
		snprintf(d, sizeof(d), "%s", ringing() ? "ringing" : "calling");
	snprintf(text, sizeof(text), "In call with %s, %s: Call key returns",
		 who(k->number), d);
	ui_fill(c, 0, y, c->w, 32, UI_ACCENT);
	ui_text_box(c, 10, y + 6, c->w - 20, UI_SANS, 16, UI_BG, UI_CENTER, text);
	return y + 32;
}

static void find_suggestions(void)
{
	struct ui_contact *list;
	int n = ui_contacts(&list);
	const char *d = num.text;

	nsuggest = 0;
	if (!d[0])
		return;
	for (int i = 0; i < n && nsuggest < MAX_SUGGEST; i++) {
		int k = -1;
		for (int j = 0; j < list[i].nnums && k < 0; j++)
			if (strstr(list[i].num[j], d))
				k = j;
		if (k < 0 && strspn(d, "0123456789") == strlen(d) &&
		    ui_t9_match(list[i].name, d))
			k = 0;
		if (k >= 0) {
			suggest[nsuggest] = &list[i];
			suggest_num[nsuggest++] = k;
		}
	}
	if (suggest_sel >= nsuggest)
		suggest_sel = nsuggest - 1;
}

static void draw_dial(struct ui *ui, struct ui_canvas *c)
{
	int y = draw_back_to_call(c, UI_HEADER_H + TABS_H);

	find_suggestions();
	ui_round(c, 10, y + 10, c->w - 20, 56, 10, UI_BAR);
	if (!num.text[0]) {
		ui_text(c, 24, y + 20, UI_SANS, 26, UI_DIM, "Number");
		ui_text_box(c, 16, y + 76, c->w - 32, UI_SANS, 16, UI_DIM, UI_CENTER,
			    voicemail[0] ? "Hold 1 for voicemail, 2-9 for speed dials" :
			    "Hold 2-9 for speed dials");
	}
	ui_entry_draw(c, &num, 24, y + 18, c->w - 48, 28, 1);
	ui_entry_indicator(c, &num, 0, 0, 0);
	y += 76;
	const char *name = num.text[0] ? ui_contact_name(num.text) : NULL;
	if (name) {
		ui_text_box(c, 16, y, c->w - 32, UI_BOLD, 19, UI_FG, UI_LEFT, name);
		y += 30;
	}
	for (int i = 0; i < nsuggest; i++) {
		bool sel = i == suggest_sel;
		if (sel)
			ui_fill(c, 0, y, c->w, 56, UI_SEL);
		ui_text_box(c, 16, y + 6, c->w - 32, UI_SANS, 19, UI_FG, UI_LEFT,
			    suggest[i]->name);
		ui_text_box(c, 16, y + 31, c->w - 32, UI_SANS, 16, UI_DIM, UI_LEFT,
			    suggest[i]->num[suggest_num[i]]);
		y += 56;
		if (y > c->h - UI_HINTS_H - 56)
			break;
	}
	ui_hints(c, "Close", suggest_sel >= 0 || num.text[0] ? "Call" : NULL,
		 num.text[0] ? "C deletes" : NULL);
}

static const char *result_label(const struct logent *e)
{
	return !strcmp(e->result, "missed") ? "↙ Missed" :
	       !strcmp(e->result, "rejected") ? "↙ Declined" :
	       !strcmp(e->result, "answered") ? "↙ Incoming" :
	       !strcmp(e->result, "unanswered") ? "↗ No answer" : "↗ Outgoing";
}

static void recents_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
			int w, int h, bool sel)
{
	const struct logent *e = &logs[i];
	char when[32], line[96], d[16] = "";
	bool missed = !strcmp(e->result, "missed");

	fmt_when(e->t, when, sizeof(when));
	if (e->secs > 0)
		fmt_secs(e->secs, d, sizeof(d));
	snprintf(line, sizeof(line), "%s · %s%s%s", result_label(e), when,
		 d[0] ? " · " : "", d);
	ui_text_box(c, x + 16, y + 8, w - 32, UI_SANS, 20, missed ? UI_WARN : UI_FG,
		    UI_LEFT, who(e->number));
	ui_text_box(c, x + 16, y + 36, w - 32, UI_SANS, 16, UI_DIM, UI_LEFT, line);
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void contacts_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
			 int w, int h, bool sel)
{
	struct ui_contact *list;
	char line[96];

	ui_contacts(&list);
	if (i == 0) {
		ui_text_box(c, x + 16, y + 18, w - 32, UI_SANS, 20, UI_ACCENT, UI_LEFT,
			    "Add or change contacts ›");
	} else {
		const struct ui_contact *k = &list[i - 1];
		snprintf(line, sizeof(line), "%s%s", k->num[0],
			 k->nnums > 1 ? " and more" : "");
		ui_text_box(c, x + 16, y + 8, w - 32, UI_SANS, 20, UI_FG, UI_LEFT, k->name);
		ui_text_box(c, x + 16, y + 36, w - 32, UI_SANS, 16, UI_DIM, UI_LEFT, line);
	}
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void pick_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
		     int w, int h, bool sel)
{
	ui_text_box(c, x + 16, y + 8, w - 32, UI_SANS, 20, UI_FG, UI_LEFT, pick->num[i]);
	ui_text_box(c, x + 16, y + 34, w - 32, UI_SANS, 16, UI_DIM, UI_LEFT, pick->kind[i]);
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void draw_list(struct ui *ui, struct ui_canvas *c)
{
	int y = draw_back_to_call(c, UI_HEADER_H + TABS_H);
	int h = c->h - y - UI_HINTS_H;

	if (view == V_RECENTS) {
		/* Shown: the missed calls are seen (the home screen's badge) */
		if (ui_badge("calls") > 0)
			ui_calls_seen();
		if (!nlogs)
			ui_text_box(c, 0, y + 40, c->w, UI_SANS, 18, UI_DIM, UI_CENTER,
				    "No calls yet");
		ui_list_draw(ui, c, &recents_list, y, h, recents_row);
		ui_hints(c, "Close", nlogs ? "Call" : NULL, !nlogs ? NULL :
			 ui_contact_name(logs[recents_list.sel].number) ? "# Text" :
			 "# Text  * Save");
	} else {
		contacts_list.count = ui_contacts(NULL) + 1;
		ui_list_draw(ui, c, &contacts_list, y, h, contacts_row);
		ui_hints(c, "Close", contacts_list.sel ? "Call" : "Open",
			 contacts_list.sel ? "# Text" : NULL);
	}
}

static void draw_call(struct ui *ui, struct ui_canvas *c)
{
	struct call *k = main_call();
	const char *number = k ? k->number : "";
	char status[64];
	int y = UI_HEADER_H + 40;

	if (!k) {
		ui_text_box(c, 16, y + 40, c->w - 32, UI_BOLD, 26, UI_FG, UI_CENTER,
			    ended_who);
		ui_text_box(c, 16, y + 84, c->w - 32, UI_SANS, 20, UI_DIM, UI_CENTER,
			    "Call ended");
		ui_hints(c, NULL, NULL, NULL);
		return;
	}
	if (!strcmp(k->state, "conversation"))
		fmt_secs((int)(time(NULL) - k->since), status, sizeof(status));
	else
		snprintf(status, sizeof(status), "%s",
			 ringing() ? "Incoming call" :
			 !strcmp(k->state, "alerting") ? "Ringing…" :
			 !strcmp(k->state, "hold") ? "On hold" :
			 !strcmp(k->state, "disconnecting") ? "Ending…" : "Calling…");
	ui_text_box(c, 16, y, c->w - 32, UI_SANS, 20, ringing() ? UI_ACCENT : UI_DIM,
		    UI_CENTER, status);
	ui_text_box(c, 16, y + 44, c->w - 32, UI_BOLD, 30, UI_FG, UI_CENTER,
		    who(number));
	if (ui_contact_name(number))
		ui_text_box(c, 16, y + 92, c->w - 32, UI_SANS, 20, UI_DIM, UI_CENTER,
			    number);
	if (tones[0])
		ui_text_box(c, 16, y + 150, c->w - 32, UI_SANS, 26, UI_FG, UI_CENTER,
			    tones);
	if (muted)
		ui_text_box(c, 16, y + 200, c->w - 32, UI_BOLD, 20, UI_WARN, UI_CENTER,
			    "Microphone muted");
	if (speaker)
		ui_text_box(c, 16, y + 230, c->w - 32, UI_BOLD, 20, UI_ACCENT, UI_CENTER,
			    "Speaker on");
	if (ringing())
		ui_hints(c, "Decline", "Answer", NULL);
	else
		ui_hints(c, "Hide", muted ? "Unmute" : "Mute",
			 speaker ? "▸ earpiece" : "▸ speaker");
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	if (view == V_CALL)
		ui_header(ui, c, "Call");
	else if (view == V_PICK)
		ui_header(ui, c, pick ? pick->name : "Contact");
	else
		ui_header(ui, c, "Phone");
	if (view == V_CALL) {
		draw_call(ui, c);
	} else if (view == V_PICK && pick) {
		pick_list.count = pick->nnums;
		ui_list_draw(ui, c, &pick_list, UI_HEADER_H,
			     c->h - UI_HEADER_H - UI_HINTS_H, pick_row);
		ui_hints(c, "Back", pick_text ? "Text" : "Call", NULL);
	} else {
		draw_tabs(c);
		if (view == V_DIAL)
			draw_dial(ui, c);
		else
			draw_list(ui, c);
	}
	if (flash[0]) {
		int y = c->h - UI_HINTS_H - 44;
		ui_round(c, 20, y, c->w - 40, 38, 10, UI_WARN);
		ui_text_box(c, 30, y + 8, c->w - 60, UI_SANS, 18, UI_FG, UI_CENTER, flash);
	}
}

/* ---------------------------------------------------------------- keys --- */

static void text_to(const char *number)
{
	char uri[64];

	snprintf(uri, sizeof(uri), "sms:%s", number);
	ui_open_uri(uri);
}

/* To Contacts: a new one with this number, or the one that has it */
static void save_contact(const char *number)
{
	char uri[80];

	if (!number[0])
		return;
	snprintf(uri, sizeof(uri), "contact:new:%s", number);
	ui_open_uri(uri);
}

/* Call or text a contact: its number, or a choice of them */
static void use_contact(struct ui *ui, const struct ui_contact *k, bool text)
{
	if (k->nnums == 1) {
		if (text)
			text_to(k->num[0]);
		else
			dial(ui, k->num[0]);
		return;
	}
	pick = k;
	pick_text = text;
	pick_list.sel = pick_list.top = 0;
	before_pick = view;
	view = V_PICK;
}

static void key_call(struct ui *ui, uint32_t code)
{
	char reply[128];

	if (!ncalls)
		return;
	if (ringing()) {
		if (code == KEY_PHONE || code == KEY_ENTER || code == KEY_OK)
			call_cmd(reply, sizeof(reply), "answer");
		else if (code == KEY_BACK || code == KEY_ESC || code == KEY_BACKSPACE)
			call_cmd(reply, sizeof(reply), "reject");
		return;
	}
	char ch = code >= KEY_1 && code <= KEY_9 ? '1' + (code - KEY_1) :
		  code == KEY_0 ? '0' : code == KEY_NUMERIC_STAR ? '*' :
		  code == KEY_NUMERIC_POUND ? '#' : 0;
	if (ch) {
		if (call_cmd(reply, sizeof(reply), "dtmf %c", ch) && !strcmp(reply, "ok")) {
			size_t n = strlen(tones);
			if (n + 1 < sizeof(tones)) {
				tones[n] = ch;
				tones[n + 1] = 0;
			}
		}
	} else if (code == KEY_ENTER || code == KEY_OK) {
		call_cmd(reply, sizeof(reply), "mute %s", muted ? "off" : "on");
	} else if (code == KEY_RIGHT) {
		call_cmd(reply, sizeof(reply), "speaker toggle");
	} else if (code == KEY_BACK || code == KEY_ESC) {
		view = V_DIAL;		/* hidden: the call goes on */
	}
}

/* A digit held long enough: voicemail on 1, the speed dials on 2-9. The
 * key is forgotten now, so its release (wherever it lands) types nothing */
static void on_held(struct ui *ui, int fd)
{
	uint64_t n;
	char reply[128], number[48];
	int digit = held_key ? (int)(held_key - KEY_1) + 1 : 0;

	if (read(fd, &n, sizeof(n)) != sizeof(n) || !digit)
		return;
	held_key = 0;
	if (ncalls)
		return;
	if (digit == 1) {
		if (!voicemail[0])
			say(ui, "No voicemail number on the SIM");
		else if (call_cmd(reply, sizeof(reply), "voicemail") &&
			 !strncmp(reply, "dialling", 8)) {
			tones[0] = 0;
			go_back = false;
			view = V_CALL;
		} else {
			say(ui, reply[0] ? reply : "Could not call voicemail");
		}
	} else if (ui_speed_dial(digit, number, sizeof(number))) {
		dial(ui, number);
	} else {
		snprintf(reply, sizeof(reply), "Nothing on %d: set a speed dial in Contacts",
			 digit);
		say(ui, reply);
	}
	ui_redraw(ui);
}

/* The key's DTMF tone, its row's and its column's frequencies together
 * for 120 ms, a mono WAV into s22-play: at the media volume, on its cubic
 * scale as s22-wm sets the sound server's (9 of 15 is -13 dB) */
#define TONE_RATE	48000
#define TONE_N		(TONE_RATE * 120 / 1000)

static void dial_tone(uint32_t code)
{
	static const int row[4] = { 697, 770, 852, 941 }, col[3] = { 1209, 1336, 1477 };
	int k = code >= KEY_1 && code <= KEY_9 ? (int)(code - KEY_1) : code == KEY_NUMERIC_STAR ? 9 :
		code == KEY_0 ? 10 : code == KEY_NUMERIC_POUND ? 11 : -1;
	char v[16];

	int level = ui_setting_int("volume", 15);
	if (k < 0 || level <= 0 || !strcmp(ui_setting("sounds", "on", v, sizeof(v)), "off"))
		return;
	double gain = level >= 15 ? 1 : pow(level / 15.0, 3);
	static uint8_t wav[44 + TONE_N * 2];
	uint32_t data = TONE_N * 2;
	memcpy(wav, "RIFF", 4);
	*(uint32_t *)(wav + 4) = 36 + data;
	memcpy(wav + 8, "WAVEfmt ", 8);
	*(uint32_t *)(wav + 16) = 16;
	*(uint16_t *)(wav + 20) = 1;		/* PCM */
	*(uint16_t *)(wav + 22) = 1;		/* mono */
	*(uint32_t *)(wav + 24) = TONE_RATE;
	*(uint32_t *)(wav + 28) = TONE_RATE * 2;
	*(uint16_t *)(wav + 32) = 2;
	*(uint16_t *)(wav + 34) = 16;
	memcpy(wav + 36, "data", 4);
	*(uint32_t *)(wav + 40) = data;
	int16_t *pcm = (int16_t *)(wav + 44);
	int ramp = TONE_RATE / 200;		/* 5 ms in and out: no clicks */
	for (int i = 0; i < TONE_N; i++) {
		double t = (double)i / TONE_RATE;
		double env = i < ramp ? (double)i / ramp : i >= TONE_N - ramp ?
			     (double)(TONE_N - 1 - i) / ramp : 1;
		pcm[i] = (int16_t)(32767 * 0.2 * gain * env * (sin(2 * M_PI * row[k / 3] * t) +
							 sin(2 * M_PI * col[k % 3] * t)));
	}
	int p[2];
	if (pipe2(p, O_CLOEXEC) != 0)
		return;
	/* Twice forked, so nobody waits for s22-play; the WAV fits the pipe */
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			sigset_t set;
			sigemptyset(&set);
			sigprocmask(SIG_SETMASK, &set, NULL);
			dup2(p[0], 0);
			execlp("s22-play", "s22-play", "-v", "15", "-", (char *)NULL);
		}
		_exit(0);
	}
	close(p[0]);
	if (pid > 0) {
		waitpid(pid, NULL, 0);
		(void)!write(p[1], wav, sizeof(wav));
	}
	close(p[1]);
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st, const char *text)
{
	if (view == V_DIAL && st == UI_PRESSED)
		dial_tone(code);
	/* On an empty keypad, 1-9 held call voicemail or a speed dial as soon
	 * as they count as held (on_held); let go before that, a digit */
	if (view == V_DIAL && code >= KEY_1 && code <= KEY_9 &&
	    (held_key == code || !num.text[0])) {
		struct itimerspec it = { .it_value = { 0, HOLD_MS * 1000000L } };
		struct itimerspec off = { 0 };
		if (st == UI_PRESSED && !held_key) {
			held_key = code;
			timerfd_settime(hold_timer, 0, &it, NULL);
		} else if (st == UI_RELEASED && held_key == code) {
			held_key = 0;
			timerfd_settime(hold_timer, 0, &off, NULL);
			ui_entry_key(&num, code, UI_PRESSED);
			ui_entry_key(&num, code, UI_RELEASED);
		}
		ui_redraw(ui);
		return;
	}
	if (view == V_DIAL &&
	    ((code >= KEY_1 && code <= KEY_0) || code == KEY_NUMERIC_STAR ||
	     code == KEY_NUMERIC_POUND || code == KEY_BACKSPACE)) {
		ui_entry_key(&num, code, st);
		suggest_sel = -1;
		flash[0] = 0;
		ui_redraw(ui);
		return;
	}
	if (st != UI_PRESSED)
		return;
	flash[0] = 0;
	if (view == V_CALL) {
		key_call(ui, code);
		ui_redraw(ui);
		return;
	}
	/* The Call key returns to a hidden call */
	if (code == KEY_PHONE && ncalls) {
		view = V_CALL;
		ui_redraw(ui);
		return;
	}
	if (view == V_PICK) {
		if (ui_list_key(&pick_list, code)) {
		} else if (code == KEY_BACK || code == KEY_ESC) {
			view = before_pick;
		} else if (code == KEY_ENTER || code == KEY_OK || code == KEY_PHONE) {
			const char *n = pick->num[pick_list.sel];
			view = before_pick;
			if (pick_text)
				text_to(n);
			else
				dial(ui, n);
		}
		ui_redraw(ui);
		return;
	}
	if (code == KEY_LEFT || code == KEY_RIGHT) {
		view = (view + (code == KEY_RIGHT ? 1 : 2)) % 3;
		if (view == V_RECENTS)
			load_log();
		ui_redraw(ui);
		return;
	}
	if (code == KEY_BACK || code == KEY_ESC) {
		ui_quit(ui);
		return;
	}
	if (view == V_DIAL) {
		if (code == KEY_UP && nsuggest)
			suggest_sel = suggest_sel <= 0 ? nsuggest - 1 : suggest_sel - 1;
		else if (code == KEY_DOWN && nsuggest)
			suggest_sel = (suggest_sel + 1) % nsuggest;
		else if (code == KEY_ENTER || code == KEY_OK || code == KEY_PHONE) {
			if (suggest_sel >= 0 && suggest_sel < nsuggest)
				dial(ui, suggest[suggest_sel]->num[suggest_num[suggest_sel]]);
			else
				dial(ui, num.text);
		}
	} else if (view == V_RECENTS) {
		if (ui_list_key(&recents_list, code)) {
		} else if (nlogs && (code == KEY_ENTER || code == KEY_OK || code == KEY_PHONE))
			dial(ui, logs[recents_list.sel].number);
		else if (nlogs && code == KEY_NUMERIC_POUND)
			text_to(logs[recents_list.sel].number);
		else if (nlogs && code == KEY_NUMERIC_STAR)
			save_contact(logs[recents_list.sel].number);
	} else if (view == V_CONTACTS) {
		struct ui_contact *list;
		ui_contacts(&list);
		if (ui_list_key(&contacts_list, code)) {
		} else if (contacts_list.sel == 0) {
			if (code == KEY_ENTER || code == KEY_OK)
				ui_open_uri("contact:list");
		} else if (code == KEY_ENTER || code == KEY_OK || code == KEY_PHONE) {
			use_contact(ui, &list[contacts_list.sel - 1], false);
		} else if (code == KEY_NUMERIC_POUND) {
			use_contact(ui, &list[contacts_list.sel - 1], true);
		}
	}
	ui_redraw(ui);
}

static void tap(struct ui *ui, int x, int y)
{
	struct ui_list *l = view == V_RECENTS ? &recents_list :
			    view == V_CONTACTS ? &contacts_list :
			    view == V_PICK ? &pick_list : NULL;

	if (y >= UI_HEADER_H && y < UI_HEADER_H + TABS_H && view <= V_CONTACTS) {
		view = x * 3 / ui_width(ui);
		if (view == V_RECENTS)
			load_log();
		ui_redraw(ui);
		return;
	}
	if (!l)
		return;
	int top = view == V_PICK ? UI_HEADER_H : UI_HEADER_H + TABS_H + (ncalls ? 32 : 0);
	int i = ui_list_at(l, top, y);
	if (i < 0)
		return;
	l->sel = i;
	key(ui, KEY_OK, UI_PRESSED, "");
}

static void drag(struct ui *ui, int dx, int dy, bool done)
{
	struct ui_list *l = view == V_RECENTS ? &recents_list :
			    view == V_CONTACTS ? &contacts_list :
			    view == V_PICK ? &pick_list : NULL;

	if (l) {
		ui_list_drag(l, dy, ui_height(ui) - UI_HEADER_H - TABS_H - UI_HINTS_H);
		ui_redraw(ui);
	}
}

/* -------------------------------------------------------------- events --- */

static void tick(struct ui *ui);

static void calls_changed(struct ui *ui)
{
	int before = ncalls;
	char last[64];

	snprintf(last, sizeof(last), "%s", ncalls ? who(main_call()->number) : "");
	load_state();
	if (ncalls && !before) {		/* a new call: show it */
		tones[0] = 0;
		if (ringing() && !focused)
			go_back = true;
		view = V_CALL;
	} else if (!ncalls && before) {
		load_log();
		if (view == V_CALL) {		/* "ended" for a moment */
			snprintf(ended_who, sizeof(ended_who), "%s", last);
			ended_at = time(NULL);
			ui_timer(ui, ENDED_MS);
		}
	}
	tick(ui);
}

static void on_inotify(struct ui *ui, int fd)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

	while (read(fd, buf, sizeof(buf)) > 0)
		;
	calls_changed(ui);
	ui_redraw(ui);
}

static void tick(struct ui *ui)
{
	if (ended_at && !ncalls && time(NULL) - ended_at >= ENDED_MS / 1000) {
		ended_at = 0;
		ui_entry_set(&num, "");
		suggest_sel = -1;
		view = V_DIAL;
		if (go_back) {
			go_back = false;
			ui_quit(ui);
			return;
		}
	}
	ui_redraw(ui);
	/* The duration counts every second while there is a call */
	ui_timer(ui, ncalls ? 1000 : ended_at ? ENDED_MS : 60000);
}

static void open_uri(struct ui *ui, const char *uri)
{
	load_state();
	if (!strncmp(uri, "tel:", 4) && uri[4]) {
		dial(ui, uri + 4);
	} else if (!strcmp(uri, "call:incoming")) {
		if (ncalls && !focused)
			go_back = true;
		if (ncalls)
			view = V_CALL;
	} else if (!strcmp(uri, "call:recents")) {
		load_log();
		view = V_RECENTS;
		recents_list.sel = recents_list.top = 0;
	}
	ui_redraw(ui);
}

static void focus(struct ui *ui, bool in)
{
	focused = in;
	if (in) {
		calls_changed(ui);
		load_log();
	}
}

static void start(struct ui *ui)
{
	int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);

	ui_entry_init(ui, &num, true);
	hold_timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (hold_timer >= 0)
		ui_watch(ui, hold_timer, on_held);
	load_voicemail();
	load_state();
	load_log();
	if (ncalls)
		view = V_CALL;
	if (fd >= 0) {
		inotify_add_watch(fd, CALL_DIR, IN_MOVED_TO | IN_CLOSE_WRITE);
		inotify_add_watch(fd, LOG_DIR, IN_MODIFY | IN_CLOSE_WRITE);
		ui_watch(ui, fd, on_inotify);
	}
	tick(ui);
}

int main(int argc, char **argv)
{
	struct ui_app app = {
		.app_id = "s22-phone",
		.title = "Phone",
		.draw = draw,
		.key = key,
		.tick = tick,
		.start = start,
		.focus = focus,
		.open = open_uri,
		.tap = tap,
		.drag = drag,
		.uri = argc > 1 ? argv[1] : NULL,
	};

	return ui_main(&app);
}
