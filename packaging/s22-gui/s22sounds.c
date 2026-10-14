// SPDX-License-Identifier: MIT
/*
 * Sounds for the apps that pick them (Settings, Contacts): the files of a
 * kind, their names to show, and previews through s22-play.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#include "s22ui.h"

static char (*files)[160];
static int nfiles, capfiles;

static int by_name(const void *a, const void *b)
{
	return strcasecmp(ui_sound_name(a), ui_sound_name(b));
}

int ui_sounds(const char *kind, char (**list)[160])
{
	char dir[256];
	DIR *d;
	struct dirent *de;

	nfiles = 0;
	snprintf(dir, sizeof(dir), "%s/%s", UI_SOUNDS, kind);
	d = opendir(dir);
	while (d && (de = readdir(d))) {
		size_t n = strlen(de->d_name);
		if (n < 5 || strcasecmp(de->d_name + n - 4, ".ogg"))
			continue;
		if (nfiles == capfiles) {
			int cap = capfiles ? capfiles * 2 : 128;
			void *p = realloc(files, cap * sizeof(*files));
			if (!p)
				break;
			files = p;
			capfiles = cap;
		}
		snprintf(files[nfiles++], sizeof(files[0]), "%s/%s", dir, de->d_name);
	}
	if (d)
		closedir(d);
	qsort(files, nfiles, sizeof(*files), by_name);
	if (list)
		*list = files;
	return nfiles;
}

const char *ui_sound_name(const char *path)
{
	static char name[2][96];
	static int which;
	const char *base;
	char *n = name[which ^= 1];

	if (!path || !path[0] || !strcmp(path, "default"))
		return "Default";
	if (!strcmp(path, "silent"))
		return "Silent";
	base = strrchr(path, '/');
	snprintf(n, sizeof(name[0]), "%s", base ? base + 1 : path);
	char *dot = strrchr(n, '.');
	if (dot)
		*dot = 0;
	for (char *p = n; *p; p++)
		if (*p == '_')
			*p = ' ';
	if (islower((unsigned char)n[0]))
		n[0] = toupper((unsigned char)n[0]);
	return n;
}

static pid_t preview;

void ui_sound_stop(void)
{
	if (preview > 0) {
		kill(preview, SIGTERM);
		waitpid(preview, NULL, 0);
	}
	preview = 0;
}

void ui_sound_preview(const char *path)
{
	ui_sound_stop();
	if (!path || path[0] != '/')
		return;
	preview = fork();
	if (preview == 0) {
		setsid();
		execlp("s22-play", "s22-play", path, (char *)NULL);
		_exit(127);
	}
}

/* ------------------------------------------------------ vibration --- */

static const struct ui_vib notify_vibs[] = {
	{ "Default", "200,150,200" },
	{ "Short", "150" },
	{ "Long", "600" },
	{ "Double", "100,120,100" },
	{ "Triple", "100,100,100,100,100" },
	{ "Heartbeat", "90,110,180" },
	{ "Rapid", "60,60,60,60,60,60,60" },
	{ "Morse V", "100,100,100,100,100,100,400" },
};
static const struct ui_vib ring_vibs[] = {
	{ "Standard", "900,600" },
	{ "Pulses", "300,300,300,1100" },
	{ "Heartbeat", "150,150,300,900" },
	{ "Rapid", "150,150" },
	{ "Long", "1500,1500" },
};

const struct ui_vib *ui_vib_presets(bool ring, int *n)
{
	*n = ring ? (int)(sizeof(ring_vibs) / sizeof(ring_vibs[0])) :
		    (int)(sizeof(notify_vibs) / sizeof(notify_vibs[0]));
	return ring ? ring_vibs : notify_vibs;
}

int ui_vib_customs(char (*out)[96])
{
	char key[24];

	for (int i = 0; i < UI_VIB_CUSTOMS; i++) {
		snprintf(key, sizeof(key), "vib_custom_%d", i + 1);
		ui_setting(key, "", out[i], sizeof(out[0]));
	}
	return UI_VIB_CUSTOMS;
}

const char *ui_vib_name(const char *pattern, bool ring)
{
	static char name[2][32];
	static int which;
	char customs[UI_VIB_CUSTOMS][96];
	const struct ui_vib *p;
	int n;

	if (!pattern || !pattern[0] || !strcmp(pattern, "default"))
		return "Default";
	if (!strcmp(pattern, "none"))
		return "None";
	p = ui_vib_presets(ring, &n);
	for (int i = 0; i < n; i++)
		if (!strcmp(pattern, p[i].pattern))
			return p[i].name;
	p = ui_vib_presets(!ring, &n);
	for (int i = 0; i < n; i++)
		if (!strcmp(pattern, p[i].pattern))
			return p[i].name;
	ui_vib_customs(customs);
	for (int i = 0; i < UI_VIB_CUSTOMS; i++)
		if (!strcmp(pattern, customs[i])) {
			snprintf(name[which ^= 1], sizeof(name[0]), "Custom %d", i + 1);
			return name[which];
		}
	return "Custom";
}

static pid_t buzz;

void ui_vibrate_stop(void)
{
	if (buzz > 0) {
		kill(buzz, SIGTERM);
		waitpid(buzz, NULL, 0);
	}
	buzz = 0;
}

void ui_vibrate(const char *pattern)
{
	ui_vibrate_stop();
	if (!pattern || !pattern[0] || !strcmp(pattern, "none") ||
	    !strcmp(pattern, "default"))
		return;
	buzz = fork();
	if (buzz == 0) {
		setsid();
		execlp("s22-vibrate", "s22-vibrate", pattern, (char *)NULL);
		_exit(127);
	}
}
