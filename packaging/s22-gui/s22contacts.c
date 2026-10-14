// SPDX-License-Identifier: MIT
/*
 * Contacts, shared by Phone, Messages and Contacts: UI_CONTACTS, a vCard
 * file (3.0 when written; FN or N, and TEL lines, are what is read), kept
 * in memory sorted by name and read again when the file changes.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "s22ui.h"

static struct ui_contact *book;
static int nbook, capbook;
static struct timespec book_mtime;
static off_t book_size = -1;

/* Undo vCard escapes (\, \; \\ \n) in place */
static void unescape(char *s)
{
	char *o = s;

	for (; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			*o++ = *s == 'n' || *s == 'N' ? ' ' : *s;
		} else {
			*o++ = *s;
		}
	}
	*o = 0;
}

static const char *tel_kind(const char *params)
{
	char p[128];

	snprintf(p, sizeof(p), "%s", params);
	for (char *c = p; *c; c++)
		*c = tolower((unsigned char)*c);
	return strstr(p, "cell") || strstr(p, "mobile") ? "mobile" :
	       strstr(p, "home") ? "home" : strstr(p, "work") ? "work" : "other";
}

static int by_name(const void *a, const void *b)
{
	return strcasecmp(((const struct ui_contact *)a)->name,
			  ((const struct ui_contact *)b)->name);
}

/* A list being read: the address book, or a file being imported */
struct book {
	struct ui_contact *c;
	int n, cap;
};

static struct ui_contact *grow(struct book *b)
{
	if (b->n == b->cap) {
		int cap = b->cap ? b->cap * 2 : 64;
		struct ui_contact *n = realloc(b->c, cap * sizeof(*b->c));
		if (!n)
			return NULL;
		b->c = n;
		b->cap = cap;
	}
	memset(&b->c[b->n], 0, sizeof(*b->c));
	return &b->c[b->n];
}

/* One logical line (continuation lines unfolded) into buf; false at EOF */
static bool next_line(FILE *f, char *buf, size_t len)
{
	int c;
	size_t n = 0;

	for (;;) {
		c = fgetc(f);
		if (c == EOF)
			break;
		if (c == '\r')
			continue;
		if (c == '\n') {
			int d = fgetc(f);
			if (d == ' ' || d == '\t')
				continue;	/* folded: the line goes on */
			if (d != EOF)
				ungetc(d, f);
			break;
		}
		if (n + 1 < len)
			buf[n++] = c;
	}
	buf[n] = 0;
	return c != EOF || n > 0;
}

static void parse(FILE *f, struct book *b)
{
	char line[1024];
	struct ui_contact *c = NULL;
	char n_name[64] = "";

	b->n = 0;
	while (next_line(f, line, sizeof(line))) {
		char *colon = strchr(line, ':');
		if (!colon)
			continue;
		*colon = 0;
		char *key = line, *val = colon + 1;
		char *dot = strchr(key, '.');	/* item1.TEL */
		if (dot && dot < key + strcspn(key, ";"))
			key = dot + 1;
		if (!strcasecmp(key, "BEGIN") && !strcasecmp(val, "VCARD")) {
			c = grow(b);
			n_name[0] = 0;
		} else if (!c) {
			continue;
		} else if (!strcasecmp(key, "END")) {
			if (!c->name[0])
				snprintf(c->name, sizeof(c->name), "%s",
					 n_name[0] ? n_name : c->nnums ? c->num[0] : "");
			if (c->name[0] && c->nnums)
				b->n++;
			c = NULL;
		} else if (!strncasecmp(key, "FN", 2) && (!key[2] || key[2] == ';')) {
			unescape(val);
			snprintf(c->name, sizeof(c->name), "%s", val);
		} else if (!strncasecmp(key, "N", 1) && (!key[1] || key[1] == ';')) {
			/* Family;Given;Middle;Prefix;Suffix -> "Given Family" */
			char *parts[5] = { 0 }, *p = val;
			for (int i = 0; i < 5 && p; i++) {
				parts[i] = p;
				p = strchr(p, ';');
				if (p)
					*p++ = 0;
			}
			for (int i = 0; i < 2; i++)
				if (parts[i])
					unescape(parts[i]);
			snprintf(n_name, sizeof(n_name), "%s%s%s",
				 parts[1] ? parts[1] : "",
				 parts[1] && parts[1][0] && parts[0] && parts[0][0] ? " " : "",
				 parts[0] ? parts[0] : "");
		} else if (!strcasecmp(key, "X-S22-RINGTONE")) {
			snprintf(c->ringtone, sizeof(c->ringtone), "%s", val);
		} else if (!strcasecmp(key, "X-S22-TEXTTONE")) {
			snprintf(c->texttone, sizeof(c->texttone), "%s", val);
		} else if (!strcasecmp(key, "X-S22-CALLVIB")) {
			snprintf(c->callvib, sizeof(c->callvib), "%s", val);
		} else if (!strcasecmp(key, "X-S22-TEXTVIB")) {
			snprintf(c->textvib, sizeof(c->textvib), "%s", val);
		} else if (!strncasecmp(key, "TEL", 3) && c->nnums < UI_MAX_NUMS) {
			char num[32];
			int o = 0;
			for (char *p = val; *p && o < 31; p++)
				if (isdigit((unsigned char)*p) || (*p == '+' && o == 0) ||
				    *p == '*' || *p == '#')
					num[o++] = *p;
			num[o] = 0;
			if (!o)
				continue;
			snprintf(c->num[c->nnums], sizeof(c->num[0]), "%s", num);
			snprintf(c->kind[c->nnums], sizeof(c->kind[0]), "%s",
				 tel_kind(key + 3));
			c->nnums++;
		}
	}
	qsort(b->c, b->n, sizeof(*b->c), by_name);
}

int ui_contacts(struct ui_contact **list)
{
	struct stat sb;

	if (stat(UI_CONTACTS, &sb) != 0) {
		nbook = 0;
		book_size = -1;
	} else if (sb.st_size != book_size ||
		   sb.st_mtim.tv_sec != book_mtime.tv_sec ||
		   sb.st_mtim.tv_nsec != book_mtime.tv_nsec) {
		FILE *f = fopen(UI_CONTACTS, "re");
		if (f) {
			struct book b = { book, nbook, capbook };
			parse(f, &b);
			book = b.c;
			nbook = b.n;
			capbook = b.cap;
			fclose(f);
		}
		book_size = sb.st_size;
		book_mtime = sb.st_mtim;
	}
	if (list)
		*list = book;
	return nbook;
}

static void put_escaped(FILE *f, const char *s)
{
	for (; *s; s++) {
		if (*s == ',' || *s == ';' || *s == '\\')
			fputc('\\', f);
		fputc(*s, f);
	}
}

bool ui_contacts_save(const struct ui_contact *list, int n)
{
	char tmp[256];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.%d", UI_CONTACTS, (int)getpid());
	f = fopen(tmp, "we");
	if (!f)
		return false;
	for (int i = 0; i < n; i++) {
		const struct ui_contact *c = &list[i];
		fputs("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:", f);
		put_escaped(f, c->name);
		fputs("\r\nN:;", f);
		put_escaped(f, c->name);
		fputs(";;;\r\n", f);
		for (int k = 0; k < c->nnums; k++)
			fprintf(f, "TEL;TYPE=%s:%s\r\n",
				!strcmp(c->kind[k], "mobile") ? "CELL" :
				!strcmp(c->kind[k], "home") ? "HOME" :
				!strcmp(c->kind[k], "work") ? "WORK" : "VOICE", c->num[k]);
		if (c->ringtone[0])
			fprintf(f, "X-S22-RINGTONE:%s\r\n", c->ringtone);
		if (c->texttone[0])
			fprintf(f, "X-S22-TEXTTONE:%s\r\n", c->texttone);
		if (c->callvib[0])
			fprintf(f, "X-S22-CALLVIB:%s\r\n", c->callvib);
		if (c->textvib[0])
			fprintf(f, "X-S22-TEXTVIB:%s\r\n", c->textvib);
		fputs("END:VCARD\r\n", f);
	}
	if (fclose(f) != 0) {
		unlink(tmp);
		return false;
	}
	chmod(tmp, 0660);			/* group wheel: the directory's */
	if (rename(tmp, UI_CONTACTS) != 0) {
		unlink(tmp);
		return false;
	}
	book_size = -1;				/* read it back next time */
	return true;
}

/* The digits of a number, the last ten of them (the national number) */
static int tail_digits(const char *num, char *out)
{
	char d[48];
	int n = 0;

	for (; *num && n < 47; num++)
		if (isdigit((unsigned char)*num))
			d[n++] = *num;
	d[n] = 0;
	snprintf(out, 11, "%s", n > 10 ? d + n - 10 : d);
	return n;
}

bool ui_same_number(const char *a, const char *b)
{
	char ta[11], tb[11];
	int na = tail_digits(a, ta), nb = tail_digits(b, tb);

	if (na < 7 || nb < 7)			/* short codes: exactly */
		return na == nb && !strcmp(ta, tb);
	return !strcmp(ta, tb);
}

const struct ui_contact *ui_contact_for(const char *number, int *which)
{
	int n = ui_contacts(NULL);

	if (!number || !number[0])
		return NULL;
	for (int i = 0; i < n; i++)
		for (int k = 0; k < book[i].nnums; k++)
			if (ui_same_number(book[i].num[k], number)) {
				if (which)
					*which = k;
				return &book[i];
			}
	return NULL;
}

const char *ui_contact_name(const char *number)
{
	const struct ui_contact *c = ui_contact_for(number, NULL);

	return c ? c->name : NULL;
}

static char key_of(char ch)
{
	static const char *keys[] = { "abc", "def", "ghi", "jkl", "mno", "pqrs",
				      "tuv", "wxyz" };

	ch = tolower((unsigned char)ch);
	for (int k = 0; k < 8; k++)
		if (strchr(keys[k], ch))
			return '2' + k;
	return isdigit((unsigned char)ch) ? ch : 0;
}

bool ui_t9_match(const char *name, const char *digits)
{
	size_t nd = strlen(digits);

	if (!nd)
		return true;
	/* Any word of the name starts with the letters of the digits */
	for (const char *w = name; *w; ) {
		while (*w && !isalnum((unsigned char)*w))
			w++;
		size_t i = 0;
		const char *p = w;
		while (i < nd && *p && key_of(*p) == digits[i]) {
			i++;
			p++;
		}
		if (i == nd)
			return true;
		while (*w && isalnum((unsigned char)*w))
			w++;
	}
	return false;
}

int ui_contacts_import(const char *path)
{
	struct book in = { 0 };
	struct ui_contact *list;
	FILE *f = fopen(path, "re");
	int n, added = 0;

	if (!f)
		return -1;
	parse(f, &in);
	fclose(f);
	n = ui_contacts(&list);
	struct ui_contact *all = malloc((n + in.n + 1) * sizeof(*all));
	if (!all) {
		free(in.c);
		return -1;
	}
	memcpy(all, list, n * sizeof(*all));
	for (int i = 0; i < in.n; i++) {
		bool known = false;		/* a number already there: skip */
		for (int k = 0; k < in.c[i].nnums && !known; k++)
			known = ui_contact_for(in.c[i].num[k], NULL) != NULL;
		if (!known)
			all[n + added++] = in.c[i];
	}
	bool ok = !added || ui_contacts_save(all, n + added);
	free(all);
	free(in.c);
	return ok ? added : -1;
}

/* ---------------------------------------------------------- speed dial --- */

/* UI_SPEED_DIAL: "KEY NUMBER" lines, keys 2-9 */
bool ui_speed_dial(int key, char *number, int len)
{
	char line[96];
	int k;
	FILE *f = fopen(UI_SPEED_DIAL, "re");

	number[0] = 0;
	while (f && fgets(line, sizeof(line), f)) {
		char n[48];
		if (sscanf(line, "%d %47s", &k, n) == 2 && k == key)
			snprintf(number, len, "%s", n);
	}
	if (f)
		fclose(f);
	return number[0] != 0;
}

bool ui_speed_dial_set(int key, const char *number)
{
	char nums[10][48] = { { 0 } }, tmp[256];
	FILE *f;

	for (int k = 2; k <= 9; k++)
		ui_speed_dial(k, nums[k], sizeof(nums[k]));
	if (key < 2 || key > 9)
		return false;
	snprintf(nums[key], sizeof(nums[key]), "%s", number ? number : "");
	snprintf(tmp, sizeof(tmp), "%s.%d", UI_SPEED_DIAL, (int)getpid());
	f = fopen(tmp, "we");
	if (!f)
		return false;
	for (int k = 2; k <= 9; k++)
		if (nums[k][0])
			fprintf(f, "%d %s\n", k, nums[k]);
	if (fclose(f) != 0) {
		unlink(tmp);
		return false;
	}
	chmod(tmp, 0660);
	if (rename(tmp, UI_SPEED_DIAL) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}
