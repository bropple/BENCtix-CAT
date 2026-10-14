// SPDX-License-Identifier: GPL-2.0-only
/*
 * t9-replay: a virtual S22 Flip keypad that plays a key script, for
 * recording s22-t9d demos. Prints its /dev/input/eventN path, waits
 * START_MS, then plays SCRIPT: one press per line, "KEY HOLD_MS AFTER_MS",
 * where KEY is 0-9, *, #, OK, C, Recents, Home or Back; lines whose first
 * field is "--" are comments.
 *
 *   t9-replay SCRIPT [START_MS]
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int fd;

static void msleep(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static void emit(int type, int code, int val)
{
	struct input_event ev = { .type = type, .code = code, .value = val };
	if (write(fd, &ev, sizeof(ev)) < 0)
		perror("write");
}

static int keycode(const char *k)
{
	if (strlen(k) == 1 && k[0] >= '1' && k[0] <= '9')
		return KEY_1 + (k[0] - '1');
	if (!strcmp(k, "0")) return KEY_0;
	if (!strcmp(k, "*")) return KEY_NUMERIC_STAR;
	if (!strcmp(k, "#")) return KEY_NUMERIC_POUND;
	if (!strcmp(k, "OK")) return KEY_ENTER;
	if (!strcmp(k, "C")) return KEY_BACKSPACE;
	if (!strcmp(k, "Recents")) return KEY_APPSELECT;
	if (!strcmp(k, "Home")) return KEY_HOMEPAGE;
	if (!strcmp(k, "Back")) return KEY_BACK;
	return -1;
}

int main(int argc, char **argv)
{
	static const int keys[] = { KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6,
		KEY_7, KEY_8, KEY_9, KEY_NUMERIC_STAR, KEY_NUMERIC_POUND, KEY_ENTER,
		KEY_BACKSPACE, KEY_APPSELECT, KEY_HOMEPAGE, KEY_BACK };
	struct uinput_setup us = { .id = { BUS_VIRTUAL, 0x5332, 0x0099, 1 } };
	char sys[64], path[300], line[128], k[16];
	struct dirent *de;
	DIR *d;
	FILE *f;
	int hold, after;

	if (argc < 2) {
		fprintf(stderr, "usage: %s SCRIPT [START_MS]\n", argv[0]);
		return 2;
	}
	f = fopen(argv[1], "r");
	fd = open("/dev/uinput", O_WRONLY);
	if (!f || fd < 0) {
		perror("open");
		return 1;
	}
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		ioctl(fd, UI_SET_KEYBIT, keys[i]);
	strcpy(us.name, "t9-replay keypad");
	ioctl(fd, UI_DEV_SETUP, &us);
	ioctl(fd, UI_DEV_CREATE);
	ioctl(fd, UI_GET_SYSNAME(sizeof(sys)), sys);
	msleep(200);
	snprintf(path, sizeof(path), "/sys/devices/virtual/input/%s", sys);
	d = opendir(path);
	while (d && (de = readdir(d)))
		if (!strncmp(de->d_name, "event", 5))
			printf("/dev/input/%s\n", de->d_name);
	fflush(stdout);
	msleep(argc > 2 ? atoi(argv[2]) : 3000);

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%15s %d %d", k, &hold, &after) != 3 || !strcmp(k, "--"))
			continue;
		int code = keycode(k);
		if (code < 0) {
			fprintf(stderr, "unknown key %s\n", k);
			continue;
		}
		emit(EV_KEY, code, 1); emit(EV_SYN, SYN_REPORT, 0);
		msleep(hold);
		emit(EV_KEY, code, 0); emit(EV_SYN, SYN_REPORT, 0);
		msleep(after);
	}
	msleep(1500);
	ioctl(fd, UI_DEV_DESTROY);
	return 0;
}
