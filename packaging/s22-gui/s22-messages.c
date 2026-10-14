// SPDX-License-Identifier: MIT
/*
 * s22-messages: text messages (SMS).
 *
 * Every message s22-smsd receives is in its inbox (/var/lib/s22-sms/inbox)
 * and every one it sent in /var/lib/s22-sms/sent (group wheel, both
 * "TIME\tNUMBER\tTEXT" lines with \\, \t and \n escaped), so a dismissed
 * notification never loses one. This shows them as conversations, newest
 * first, with the unread ones counted; which were read is kept per user in
 * ~/.local/share/s22/messages-read ("NUMBER\tTIME" of the last one seen).
 * Opening a conversation marks it read and dismisses its notifications.
 * Both files are watched, so new messages show up at once.
 *
 * In a conversation the keypad writes a reply (s22ui's multi-tap entry);
 * OK sends it through s22-smsd (/run/s22-sms.sock), which sends only when
 * asked. "New message" asks for the number first. Links: sms:NUMBER opens
 * that conversation.
 *
 * Numbers in the contacts (s22ui's, shared with Phone and Contacts) show
 * as names. The Call key calls the conversation open or picked (Phone);
 * * on a conversation in the list saves its number to the contacts (or
 * shows the contact that has it).
 *
 * MMS (pictures, group messages) needs mobile data and comes later.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
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

#define SMS_DIR		"/var/lib/s22-sms"
#define INBOX		SMS_DIR "/inbox"
#define SENT		SMS_DIR "/sent"
#define SMS_SOCK	"/run/s22-sms.sock"
#define MAX_MSGS	2000
#define MAX_CONVS	200

struct msg {
	time_t t;
	char *peer, *text;
	bool out;		/* sent from here */
	bool unconfirmed;	/* sent, the modem's answer lost */
};

struct conv {
	char peer[64];
	time_t last, read;
	int count, unread, last_msg;
};

static struct msg msgs[MAX_MSGS];
static int nmsgs;
static struct conv convs[MAX_CONVS];
static int nconvs;
static struct ui_list list = { .row_h = 78 };	/* row 0: New message */

enum view { V_LIST, V_NEW, V_THREAD };
static enum view view;
static char peer[64];		/* the conversation shown */
static int scroll;		/* thread: pixels up from the bottom */
static struct ui_entry reply, number;
static char flash[128];

/* A send in progress: the socket to s22-smsd, and what was sent */
static int send_fd = -1;
static char send_peer[64], send_text[1024];

/* ------------------------------------------------------- numbers --- */

static struct conv *find_conv(const char *p)
{
	for (int i = 0; i < nconvs; i++)
		if (ui_sms_same_number(convs[i].peer, p))
			return &convs[i];
	return NULL;
}

/* ------------------------------------------------------- the files --- */

static char *unescape(const char *s)
{
	char *out = malloc(strlen(s) + 1), *o = out;

	for (; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			*o++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
		} else {
			*o++ = *s;
		}
	}
	*o = 0;
	return out;
}

static void load_file(const char *path, bool out)
{
	FILE *f = fopen(path, "re");
	char *line = NULL;
	size_t cap = 0;

	while (f && getline(&line, &cap, f) > 0 && nmsgs < MAX_MSGS) {
		line[strcspn(line, "\n")] = 0;
		char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		if (t2 == NULL)
			continue;
		/* An optional fourth field: "unconfirmed" (s22-smsd) */
		char *t3 = strchr(t2 + 1, '\t');
		bool unconfirmed = t3 && strcmp(t3 + 1, "unconfirmed") == 0;
		if (t3)
			*t3 = 0;
		*t1 = *t2 = 0;
		msgs[nmsgs++] = (struct msg){ ui_sms_time(line), unescape(t1 + 1),
					      unescape(t2 + 1), out, unconfirmed };
	}
	free(line);
	if (f)
		fclose(f);
}

static const char *read_path(void)
{
	static char path[512];
	const char *home = getenv("HOME");

	snprintf(path, sizeof(path), "%s/.local/share/s22/messages-read",
		 home ? home : "/tmp");
	return path;
}

static time_t read_mark(const char *p)
{
	FILE *f = fopen(read_path(), "re");
	char line[256];
	time_t t = 0;

	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t');
		if (tab == NULL)
			continue;
		*tab = 0;
		if (ui_sms_same_number(line, p))
			t = strtoll(tab + 1, NULL, 10);
	}
	fclose(f);
	return t;
}

static void mkdirs(const char *path)
{
	char dir[512];

	snprintf(dir, sizeof(dir), "%s", path);
	for (char *p = dir + 1; (p = strchr(p, '/')) != NULL; p++) {
		*p = 0;
		mkdir(dir, 0755);
		*p = '/';
	}
}

static void mark_read(struct conv *c)
{
	char tmp[600];
	FILE *out;

	c->read = c->last;
	c->unread = 0;
	mkdirs(read_path());
	snprintf(tmp, sizeof(tmp), "%s.new", read_path());
	out = fopen(tmp, "we");
	if (out == NULL)
		return;
	for (int i = 0; i < nconvs; i++)
		if (convs[i].read)
			fprintf(out, "%s\t%lld\n", convs[i].peer,
				(long long)convs[i].read);
	if (fclose(out) == 0)
		rename(tmp, read_path());
}

static int by_last(const void *pa, const void *pb)
{
	const struct conv *a = pa, *b = pb;

	return a->last < b->last ? 1 : a->last > b->last ? -1 : 0;
}

static int by_time(const void *pa, const void *pb)
{
	const struct msg *a = pa, *b = pb;

	return a->t < b->t ? -1 : a->t > b->t;
}

static void load(void)
{
	for (int i = 0; i < nmsgs; i++) {
		free(msgs[i].peer);
		free(msgs[i].text);
	}
	nmsgs = nconvs = 0;
	load_file(INBOX, false);
	load_file(SENT, true);
	qsort(msgs, nmsgs, sizeof(msgs[0]), by_time);

	for (int i = 0; i < nmsgs; i++) {
		struct conv *c = find_conv(msgs[i].peer);
		if (c == NULL) {
			if (nconvs == MAX_CONVS)
				continue;
			c = &convs[nconvs++];
			*c = (struct conv){ .read = read_mark(msgs[i].peer) };
			snprintf(c->peer, sizeof(c->peer), "%s", msgs[i].peer);
		}
		c->count++;
		c->last = msgs[i].t;
		c->last_msg = i;
		if (!msgs[i].out && msgs[i].t > c->read)
			c->unread++;
	}
	qsort(convs, nconvs, sizeof(convs[0]), by_last);
	list.count = nconvs + 1;
}

static void when_text(time_t t, char *buf, int len, bool full)
{
	time_t now = time(NULL);
	struct tm tm, today;
	char clock[16];
	static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

	localtime_r(&t, &tm);
	localtime_r(&now, &today);
	if (ui_setting_int("clock", 12) == 24)
		strftime(clock, sizeof(clock), "%H:%M", &tm);
	else
		snprintf(clock, sizeof(clock), "%d:%02d %s", tm.tm_hour % 12 ?
			 tm.tm_hour % 12 : 12, tm.tm_min,
			 tm.tm_hour < 12 ? "AM" : "PM");
	if (tm.tm_yday == today.tm_yday && tm.tm_year == today.tm_year)
		snprintf(buf, len, "%s", clock);
	else if (full)
		snprintf(buf, len, "%d %s, %s", tm.tm_mday, mon[tm.tm_mon], clock);
	else
		snprintf(buf, len, "%s %d", mon[tm.tm_mon], tm.tm_mday);
}

/* -------------------------------------------------------- sending --- */

static void escape(const char *s, char *out, int len)
{
	int n = 0;

	for (; *s && n < len - 3; s++) {
		if (*s == '\\' || *s == '\t' || *s == '\n') {
			out[n++] = '\\';
			out[n++] = *s == '\t' ? 't' : *s == '\n' ? 'n' : '\\';
		} else {
			out[n++] = *s;
		}
	}
	out[n] = 0;
}

static void on_sent(struct ui *ui, int fd)
{
	char buf[256] = "";
	ssize_t n = read(fd, buf, sizeof(buf) - 1);

	ui_unwatch(ui, fd);
	close(fd);
	send_fd = -1;
	buf[n > 0 ? n : 0] = 0;
	buf[strcspn(buf, "\n")] = 0;
	if (strncmp(buf, "ok", 2) != 0) {
		snprintf(flash, sizeof(flash), "Not sent: %s",
			 buf[0] ? (strncmp(buf, "error ", 6) == 0 ? buf + 6 : buf) :
			 "no answer from s22-smsd");
		/* Give the text back to try again */
		if (ui_sms_same_number(peer, send_peer) && reply.text[0] == 0)
			ui_entry_set(&reply, send_text);
		ui_timer(ui, 5000);
	}
	send_text[0] = 0;
	ui_redraw(ui);		/* the sent file brings the message itself */
}

static void send_reply(struct ui *ui)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	char line[2200], esc[2100];
	int fd, len;

	if (send_fd >= 0 || reply.text[0] == 0)
		return;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", SMS_SOCK);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		snprintf(flash, sizeof(flash), "Not sent: s22-smsd is not running");
		if (fd >= 0)
			close(fd);
		ui_timer(ui, 5000);
		return;
	}
	escape(reply.text, esc, sizeof(esc));
	len = snprintf(line, sizeof(line), "send %s\t%s\n", peer, esc);
	if (send(fd, line, len, MSG_NOSIGNAL) != len) {
		close(fd);
		snprintf(flash, sizeof(flash), "Not sent: s22-smsd did not listen");
		ui_timer(ui, 5000);
		return;
	}
	send_fd = fd;
	snprintf(send_peer, sizeof(send_peer), "%s", peer);
	snprintf(send_text, sizeof(send_text), "%s", reply.text);
	ui_entry_set(&reply, "");
	reply.mode = 1;		/* Abc for the next one */
	ui_watch(ui, fd, on_sent);
}

/* ------------------------------------------------------- drawing --- */

static void draw_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
		     int w, int h, bool sel)
{
	if (i == 0) {
		ui_round(c, x + 16, y + 17, 44, 44, 22, UI_ACCENT);
		ui_text_box(c, x + 16, y + 26, 44, UI_BOLD, 20, UI_BG, UI_CENTER, "+");
		ui_text(c, x + 76, y + 26, UI_SANS, 20, UI_FG, "New message");
		ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
		return;
	}
	struct conv *cv = &convs[i - 1];
	char when[32], preview[256];

	when_text(cv->last, when, sizeof(when), false);
	int ww = ui_text_width(UI_SANS, 15, when);
	ui_text_box(c, x + 16, y + 10, w - 40 - ww, cv->unread ? UI_BOLD : UI_SANS,
		    20, UI_FG, UI_LEFT, ui_contact_name(cv->peer) ? ui_contact_name(cv->peer) :
		    cv->peer);
	ui_text(c, x + w - 14 - ww, y + 14, UI_SANS, 15,
		cv->unread ? UI_ACCENT : UI_DIM, when);
	snprintf(preview, sizeof(preview), "%s%s",
		 msgs[cv->last_msg].out ? "You: " : "", msgs[cv->last_msg].text);
	for (char *p = preview; *p; p++)
		if (*p == '\n')
			*p = ' ';
	int right = 16;
	if (cv->unread) {
		char n[8];
		snprintf(n, sizeof(n), "%d", cv->unread);
		int bw = ui_text_width(UI_BOLD, 14, n) + 14;
		bw = bw < 24 ? 24 : bw;
		ui_round(c, x + w - 14 - bw, y + 42, bw, 24, 12, UI_ACCENT);
		ui_text_box(c, x + w - 14 - bw, y + 45, bw, UI_BOLD, 14, UI_BG,
			    UI_CENTER, n);
		right += bw + 8;
	}
	ui_text_box(c, x + 16, y + 44, w - 16 - right, UI_SANS, 17,
		    cv->unread ? UI_FG : UI_DIM, UI_LEFT, preview);
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static int bubble_h(const char *text, int bw)
{
	return ui_text_wrap(NULL, 0, 0, bw - 24, UI_SANS, 18, 0, text) + 40;
}

static void draw_bubble(struct ui_canvas *c, int y, int h, int bw,
			const char *text, const char *note, bool out)
{
	int tw = strchr(text, '\n') ? 0 : ui_text_width(UI_SANS, 18, text) + 24;
	int nw = ui_text_width(UI_SANS, 14, note) + 24;
	int w = tw && tw < bw ? tw : bw;

	if (w < nw)
		w = nw;
	int x = out ? c->w - 12 - w : 12;
	ui_round(c, x, y, w, h, 14, out ? UI_ACCENT : UI_SEL);
	ui_text_wrap(c, x + 12, y + 10, bw - 24, UI_SANS, 18, out ? UI_BG : UI_FG,
		     text);
	ui_text(c, x + 12, y + h - 24, UI_SANS, 14, out ? UI_BAR : UI_DIM, note);
}

static void draw_flash(struct ui_canvas *c, int y)
{
	if (!flash[0])
		return;
	ui_round(c, 16, y - 46, c->w - 32, 40, 10, UI_WARN);
	ui_text_box(c, 26, y - 37, c->w - 52, UI_SANS, 17, UI_FG, UI_CENTER, flash);
}

static void draw_thread(struct ui *ui, struct ui_canvas *c)
{
	int bw = c->w - 90, lh = ui_line_height(19);
	int top = UI_HEADER_H, compose_top;
	int idx[MAX_MSGS], n = 0, total = 0;

	/* The reply box grows to four lines, from the bottom */
	int eh = ui_text_wrap(NULL, 0, 0, c->w - 36, UI_SANS, 19, 0, reply.text);
	eh = eh < lh ? lh : eh > 4 * lh ? 4 * lh : eh;
	compose_top = c->h - UI_HINTS_H - eh - 18;

	for (int i = 0; i < nmsgs; i++)
		if (ui_sms_same_number(msgs[i].peer, peer))
			idx[n++] = i;
	for (int k = 0; k < n; k++)
		total += bubble_h(msgs[idx[k]].text, bw) + 10;
	if (send_text[0])
		total += bubble_h(send_text, bw) + 10;
	int max_scroll = total + 10 - (compose_top - top);
	if (max_scroll < 0)
		max_scroll = 0;
	scroll = scroll < 0 ? 0 : scroll > max_scroll ? max_scroll : scroll;

	/* From the bottom up: the newest last */
	int y = compose_top - 10 + scroll;
	if (send_text[0]) {
		int h = bubble_h(send_text, bw);
		y -= h;
		draw_bubble(c, y, h, bw, send_text, "Sending…", true);
		y -= 10;
	}
	for (int k = n - 1; k >= 0 && y > top; k--) {
		struct msg *m = &msgs[idx[k]];
		int h = bubble_h(m->text, bw);
		char when[64];
		y -= h;
		if (y < compose_top) {
			when_text(m->t, when, sizeof(when), true);
			if (m->unconfirmed)
				snprintf(when + strlen(when), sizeof(when) - strlen(when),
					 " · not confirmed");
			draw_bubble(c, y, h, bw, m->text, when, m->out);
		}
		y -= 10;
	}
	if (n == 0 && !send_text[0])
		ui_text_box(c, 0, top + 40, c->w, UI_SANS, 18, UI_DIM, UI_CENTER,
			    "No messages yet");

	/* The reply box */
	ui_fill(c, 0, compose_top, c->w, c->h - compose_top, UI_BG);
	ui_fill(c, 0, compose_top, c->w, 1, UI_BAR);
	ui_entry_indicator(c, &reply, 0, 0, 0);	/* in the status bar */
	ui_round(c, 8, compose_top + 6, c->w - 16, eh + 8, 10, UI_BAR);
	if (reply.text[0] == 0 && !reply.pend_key)
		ui_text(c, 24, compose_top + 9, UI_SANS, 19, UI_DIM, "Text message");
	ui_entry_draw(c, &reply, 18, compose_top + 9, c->w - 36, 19, 4);
	draw_flash(c, compose_top);

	ui_header(ui, c, ui_contact_name(peer) ? ui_contact_name(peer) : peer);
	ui_hints(c, "Back", reply.text[0] && send_fd < 0 ? "Send" : NULL,
		 scroll < max_scroll ? "▲ older" : NULL);
}

static void draw_new(struct ui *ui, struct ui_canvas *c)
{
	int y = UI_HEADER_H + 30;

	ui_header(ui, c, "New message");
	ui_text(c, 16, y, UI_SANS, 18, UI_DIM, "To (number)");
	ui_round(c, 8, y + 30, c->w - 16, 48, 10, UI_BAR);
	ui_entry_draw(c, &number, 20, y + 40, c->w - 40, 24, 1);
	ui_entry_indicator(c, &number, 0, 0, 0);	/* in the status bar */
	ui_text(c, 16, y + 100, UI_SANS, 15, UI_DIM, "Hold * for +");
	draw_flash(c, c->h - UI_HINTS_H);
	ui_hints(c, "Back", strlen(number.text) >= 3 ? "Next" : NULL, NULL);
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	if (view == V_THREAD) {
		draw_thread(ui, c);
		return;
	}
	if (view == V_NEW) {
		draw_new(ui, c);
		return;
	}
	ui_header(ui, c, "Messages");
	ui_list_draw(ui, c, &list, UI_HEADER_H, c->h - UI_HEADER_H - UI_HINTS_H,
		     draw_row);
	ui_hints(c, "Close", "Open", list.sel > 0 && !ui_contact_name(convs[list.sel - 1].peer) ?
		 "* Save contact" : NULL);
}

/* ---------------------------------------------------------- keys --- */

static void open_thread(const char *p)
{
	struct conv *c = find_conv(p);
	char prefix[96];

	snprintf(peer, sizeof(peer), "%s", c ? c->peer : p);
	view = V_THREAD;
	scroll = 0;
	ui_entry_set(&reply, "");
	reply.mode = 1;
	if (c != NULL)
		mark_read(c);
	/* s22-smsd's notifications read "NUMBER: TEXT" */
	snprintf(prefix, sizeof(prefix), "%s: ", peer);
	ui_notify_dismiss_matching("sms", prefix);
}

static void call(const char *number)
{
	char uri[80];

	snprintf(uri, sizeof(uri), "tel:%s", number);
	ui_open_uri(uri);
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st,
		const char *text)
{
	if (code == KEY_PHONE && st == UI_PRESSED &&
	    (view == V_THREAD || (view == V_LIST && list.sel > 0))) {
		call(view == V_THREAD ? peer : convs[list.sel - 1].peer);
		return;
	}
	if (view == V_THREAD) {
		if (code == KEY_UP || code == KEY_DOWN) {
			if (st != UI_RELEASED)
				scroll += code == KEY_UP ? 80 : -80;
		} else if (code == KEY_BACK || code == KEY_ESC) {
			if (st == UI_PRESSED)
				view = V_LIST;
		} else if (code == KEY_ENTER || code == KEY_OK) {
			if (st == UI_PRESSED)
				send_reply(ui);
		} else if (!ui_entry_key(&reply, code, st)) {
			return;
		}
		ui_redraw(ui);
		return;
	}
	if (view == V_NEW) {
		if (code == KEY_BACK || code == KEY_ESC) {
			if (st == UI_PRESSED)
				view = V_LIST;
		} else if (code == KEY_ENTER || code == KEY_OK) {
			if (st == UI_PRESSED && strlen(number.text) >= 3)
				open_thread(number.text);
		} else if (!ui_entry_key(&number, code, st)) {
			return;
		}
		ui_redraw(ui);
		return;
	}
	if (st == UI_RELEASED)
		return;
	if (ui_list_key(&list, code)) {
		ui_redraw(ui);
		return;
	}
	if (st != UI_PRESSED)
		return;
	switch (code) {
	case KEY_NUMERIC_STAR:
		if (list.sel > 0) {
			char uri[96];
			snprintf(uri, sizeof(uri), "contact:new:%s", convs[list.sel - 1].peer);
			ui_open_uri(uri);
		}
		return;
	case KEY_ENTER:
	case KEY_OK:
		if (list.sel == 0) {
			ui_entry_set(&number, "");
			view = V_NEW;
		} else {
			open_thread(convs[list.sel - 1].peer);
		}
		break;
	case KEY_BACK:
	case KEY_ESC:
		ui_quit(ui);
		return;
	default:
		return;
	}
	ui_redraw(ui);
}

static void tap(struct ui *ui, int x, int y)
{
	if (view != V_LIST)
		return;		/* threads: the keypad writes, the bar sends */
	int i = ui_list_at(&list, UI_HEADER_H, y);
	if (i < 0)
		return;
	list.sel = i;
	key(ui, KEY_ENTER, UI_PRESSED, "");
}

static void drag(struct ui *ui, int dx, int dy, bool done)
{
	if (view == V_THREAD)
		scroll += dy;		/* finger down: older messages */
	else if (view == V_LIST)
		ui_list_drag(&list, dy, ui_height(ui) - UI_HEADER_H - UI_HINTS_H);
	ui_redraw(ui);
}

static void open_uri(struct ui *ui, const char *uri)
{
	if (strncmp(uri, "sms:", 4) == 0 && uri[4]) {
		load();
		open_thread(uri + 4);
	}
}

static void on_inotify(struct ui *ui, int fd)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

	while (read(fd, buf, sizeof(buf)) > 0)
		;
	load();
	/* A new message in the conversation on screen is read already */
	struct conv *c = view == V_THREAD ? find_conv(peer) : NULL;
	if (c != NULL && c->unread)
		open_thread(peer);
	ui_redraw(ui);
}

static void focus(struct ui *ui, bool focused)
{
	if (focused) {
		load();
		ui_redraw(ui);
	}
}

static void tick(struct ui *ui)
{
	flash[0] = 0;
	ui_redraw(ui);		/* and the clock */
	ui_timer(ui, 30000);
}

static void start(struct ui *ui)
{
	int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);

	ui_entry_init(ui, &reply, false);
	ui_entry_init(ui, &number, true);
	load();
	if (fd >= 0 && inotify_add_watch(fd, SMS_DIR,
	    IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO | IN_ATTRIB) >= 0)
		ui_watch(ui, fd, on_inotify);
	else
		fprintf(stderr, "s22-messages: cannot watch %s: %s\n", SMS_DIR,
			strerror(errno));
	ui_timer(ui, 30000);
}

int main(int argc, char **argv)
{
	struct ui_app app = {
		.app_id = "s22-messages",
		.title = "Messages",
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
