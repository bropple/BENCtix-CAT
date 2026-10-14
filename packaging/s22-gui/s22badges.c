// SPDX-License-Identifier: MIT
/*
 * Badges (D075): what waits in an app, for the home screen's tiles. A
 * notification dismissed takes the alert away, never this; it goes when the
 * app has been looked at:
 *   messages  texts in s22-smsd's inbox newer than their conversation's
 *             read mark (~/.local/share/s22/messages-read, s22-messages)
 *   calls     missed calls in s22-calld's log newer than the last look at
 *             Phone's recent calls (~/.local/share/s22/calls-seen)
 * Counted again only when a file changed.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include "s22ui.h"

#define SMS_DIR		"/var/lib/s22-sms"
#define SMS_INBOX	SMS_DIR "/inbox"
#define CALL_DIR	"/var/lib/s22-call"
#define CALL_LOG	CALL_DIR "/log"
#define MAX_MARKS	256

static void digits_of(const char *s, char *out, int len)
{
	int n = 0;

	for (; *s && n < len - 1; s++)
		if (isdigit((unsigned char)*s))
			out[n++] = *s;
	out[n] = 0;
}

bool ui_sms_same_number(const char *a, const char *b)
{
	char da[32], db[32];
	int la, lb;

	if (strcmp(a, b) == 0)
		return true;
	digits_of(a, da, sizeof(da));
	digits_of(b, db, sizeof(db));
	la = strlen(da);
	lb = strlen(db);
	if (la < 7 || lb < 7)	/* short codes: exactly */
		return false;
	if (la >= 10 && lb >= 10)
		return strcmp(da + la - 10, db + lb - 10) == 0;
	return strcmp(da, db) == 0;
}

time_t ui_sms_time(const char *s)
{
	struct tm tm = { 0 };
	int oh = 0, om = 0;
	char sign = '+';

	if (sscanf(s, "%d-%d-%dT%d:%d:%d%c%d:%d", &tm.tm_year, &tm.tm_mon,
		   &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &sign,
		   &oh, &om) < 6)
		return 0;
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	time_t t = timegm(&tm);
	int off = (oh * 60 + om) * 60;
	return sign == '-' ? t + off : sign == '+' ? t - off : t;
}

static const char *home_file(const char *name)
{
	static char path[2][512];
	static int which;
	const char *home = getenv("HOME");

	which ^= 1;
	snprintf(path[which], sizeof(path[0]), "%s/.local/share/s22/%s",
		 home ? home : "/tmp", name);
	return path[which];
}

/* ~/.local/share/s22, made if it is not there; its path ends in "/" */
static const char *home_dir(void)
{
	static char dir[512];

	snprintf(dir, sizeof(dir), "%s", home_file(""));
	for (char *p = dir + 1; (p = strchr(p, '/')) != NULL; p++) {
		*p = 0;
		mkdir(dir, 0755);
		*p = '/';
	}
	return dir;
}

/* A file's change, to count again only after one */
struct stamp {
	time_t mtime;
	long nsec;
	off_t size;
	bool there;
};

static bool changed(const char *path, struct stamp *was)
{
	struct stat st;
	struct stamp now = { 0 };

	if (stat(path, &st) == 0)
		now = (struct stamp){ st.st_mtim.tv_sec, st.st_mtim.tv_nsec, st.st_size, true };
	if (memcmp(&now, was, sizeof(now)) == 0)
		return false;
	*was = now;
	return true;
}

static int unread_texts(void)
{
	static struct stamp inbox_st, read_st;
	static int count;
	bool a = changed(SMS_INBOX, &inbox_st), b = changed(home_file("messages-read"), &read_st);

	if (!a && !b)
		return count;

	/* The read marks, "NUMBER\tTIME"; the last one for a number counts */
	static struct { char number[40]; time_t t; } marks[MAX_MARKS];
	int nmarks = 0;
	char line[1024];
	FILE *f = fopen(home_file("messages-read"), "re");
	while (f && fgets(line, sizeof(line), f) && nmarks < MAX_MARKS) {
		char *tab = strchr(line, '\t');
		if (tab == NULL)
			continue;
		*tab = 0;
		snprintf(marks[nmarks].number, sizeof(marks[0].number), "%s", line);
		marks[nmarks++].t = strtoll(tab + 1, NULL, 10);
	}
	if (f)
		fclose(f);

	/* The inbox, "TIME\tNUMBER\tTEXT" */
	char *l = NULL;
	size_t cap = 0;
	count = 0;
	f = fopen(SMS_INBOX, "re");
	while (f && getline(&l, &cap, f) > 0) {
		char *t1 = strchr(l, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		if (t2 == NULL)
			continue;
		*t1 = *t2 = 0;
		time_t t = ui_sms_time(l), mark = 0;
		for (int i = 0; i < nmarks; i++)
			if (ui_sms_same_number(marks[i].number, t1 + 1))
				mark = marks[i].t;
		if (t > mark)
			count++;
	}
	free(l);
	if (f)
		fclose(f);
	return count;
}

/* When Recents was last looked at; the first time, now (older calls were
 * seen before badges) */
static time_t calls_seen_mark(void)
{
	FILE *f = fopen(home_file("calls-seen"), "re");
	long long t = 0;

	if (f) {
		if (fscanf(f, "%lld", &t) != 1)
			t = 0;
		fclose(f);
		return t;
	}
	ui_calls_seen();
	return time(NULL);
}

static int missed_calls(void)
{
	static struct stamp log_st, seen_st;
	static int count;
	bool a = changed(CALL_LOG, &log_st), b = changed(home_file("calls-seen"), &seen_st);

	if (!a && !b)
		return count;
	time_t seen = calls_seen_mark();
	char line[200], dir[8], number[40], result[16];
	long long t;
	int secs;
	FILE *f = fopen(CALL_LOG, "re");

	count = 0;
	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "%lld\t%7s\t%39[^\t]\t%15s\t%d", &t, dir, number, result,
			   &secs) == 5 && !strcmp(result, "missed") && t > seen)
			count++;
	if (f)
		fclose(f);
	return count;
}

void ui_calls_seen(void)
{
	char tmp[600];
	FILE *f;

	home_dir();
	snprintf(tmp, sizeof(tmp), "%s.new", home_file("calls-seen"));
	if (!(f = fopen(tmp, "we")))
		return;
	fprintf(f, "%lld\n", (long long)time(NULL));
	if (fclose(f) == 0)
		rename(tmp, home_file("calls-seen"));
}

int ui_badge(const char *kind)
{
	if (!kind || !kind[0])
		return 0;
	if (!strcmp(kind, "messages"))
		return unread_texts();
	if (!strcmp(kind, "calls"))
		return missed_calls();
	return 0;
}

int ui_badge_watch(void)
{
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	uint32_t mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE;

	if (fd < 0)
		return -1;
	inotify_add_watch(fd, home_dir(), mask);
	inotify_add_watch(fd, SMS_DIR, mask);
	inotify_add_watch(fd, CALL_DIR, mask);
	return fd;
}
