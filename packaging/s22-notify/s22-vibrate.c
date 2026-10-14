// SPDX-License-Identifier: GPL-2.0-only
/*
 * s22-vibrate: play a vibration pattern once (Settings' previews and its
 * pattern recorder, D042). PATTERN is on,off,on,... in milliseconds (up to
 * 16 steps, each up to 5 s); STRENGTH is in percent, by default the
 * vib_strength setting (1-15, as Settings has it). SIGTERM stops it at
 * once, the vibrator quiet: a long single step and a kill make "vibrate
 * while a key is held".
 *
 *   s22-vibrate [-s STRENGTH] PATTERN
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_STEPS	16
#define SETTINGS	"/var/lib/s22/settings"
#define BIT(arr, b)	((arr)[(b) / 8] & (1 << ((b) % 8)))

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int open_vibrator(void)
{
	unsigned char ff[FF_MAX / 8 + 1];
	char p[300];
	struct dirent *de;
	DIR *d = opendir("/dev/input");
	int fd;

	while (d && (de = readdir(d))) {
		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(p, sizeof(p), "/dev/input/%s", de->d_name);
		fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(ff, 0, sizeof(ff));
		if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff) >= 0 && BIT(ff, FF_RUMBLE)) {
			closedir(d);
			return fd;
		}
		close(fd);
	}
	if (d)
		closedir(d);
	errno = ENODEV;
	return -1;
}

/* vib_strength (1-15) as a percentage; 100 without it */
static int strength_setting(void)
{
	char line[128];
	int v = 15;
	FILE *f = fopen(SETTINGS, "re");

	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, "vib_strength=", 13))
			v = atoi(line + 13);
	if (f)
		fclose(f);
	v = v < 1 ? 1 : v > 15 ? 15 : v;
	return v * 100 / 15;
}

/* Sleep ms, waking for a signal */
static void nap(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	while (!stop && nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

int main(int argc, char **argv)
{
	int steps[MAX_STEPS], n = 0, strength = -1, opt;

	while ((opt = getopt(argc, argv, "s:")) != -1) {
		if (opt == 's')
			strength = atoi(optarg);
		else
			goto usage;
	}
	if (optind != argc - 1)
		goto usage;
	for (const char *p = argv[optind]; *p && n < MAX_STEPS; ) {
		int v = atoi(p);
		steps[n++] = v < 0 ? 0 : v > 5000 ? 5000 : v;
		p += strcspn(p, ",");
		if (*p == ',')
			p++;
	}
	if (strength < 0)
		strength = strength_setting();
	if (strength > 100)
		strength = 100;

	int fd = open_vibrator();
	if (fd < 0) {
		fprintf(stderr, "s22-vibrate: no vibrator: %s\n", strerror(errno));
		return 1;
	}
	struct sigaction sa = { .sa_handler = on_signal };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	struct ff_effect fx = {
		.type = FF_RUMBLE,
		.id = -1,
		.u.rumble.strong_magnitude = (unsigned short)(0xffff * strength / 100),
	};
	struct input_event ev = { .type = EV_FF };
	for (int i = 0; i < n && !stop && strength > 0; i++) {
		if (i % 2 == 0 && steps[i] > 0) {
			fx.replay.length = steps[i];
			if (ioctl(fd, EVIOCSFF, &fx) < 0)
				break;
			ev.code = fx.id;
			ev.value = 1;
			if (write(fd, &ev, sizeof(ev)) < 0)
				break;
		}
		nap(steps[i]);
	}
	if (fx.id >= 0) {
		ev.code = fx.id;
		ev.value = 0;			/* quiet now, whatever is left */
		if (write(fd, &ev, sizeof(ev)) < 0) {
			/* removing the effect below stops it too */
		}
		ioctl(fd, EVIOCRMFF, fx.id);
	}
	close(fd);
	return 0;
usage:
	fprintf(stderr, "usage: s22-vibrate [-s STRENGTH] PATTERN (on,off,on,... ms)\n");
	return 2;
}
