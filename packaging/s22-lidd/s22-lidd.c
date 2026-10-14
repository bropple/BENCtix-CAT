// SPDX-License-Identifier: GPL-2.0-only
/*
 * s22-lidd: the main display follows the Cat S22 Flip's lid. Closed: the
 * main panel's framebuffer is blanked and its backlight powered down; open:
 * both come back. Suspending while it is closed is s22-sleepd's (D070),
 * told the lid's state by a hook; elogind ignores the lid.
 *
 * After a suspend the display driver powers the panel up again, lid or no
 * lid, so the lid state is re-applied after every resume: detected as a jump
 * between CLOCK_BOOTTIME (counts suspend) and CLOCK_MONOTONIC (does not),
 * checked every 2 s, or at once on SIGUSR1.
 *
 * Hooks: on every lid change, and once at start, each executable in the
 * hook directory runs (in name order, not waited for) with "open" or
 * "closed" as its argument.
 *
 *   s22-lidd [-f FB] [-b BACKLIGHT] [-d HOOKDIR]
 *            (defaults fb0, backlight, /etc/s22-lidd.d)
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BIT(arr, b)	((arr)[(b) / 8] & (1 << ((b) % 8)))

static const char *fb = "fb0", *bl = "backlight", *hookdir = "/etc/s22-lidd.d";

static void put(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0 || write(fd, val, strlen(val)) < 0)
		fprintf(stderr, "s22-lidd: %s: %s\n", path, strerror(errno));
	if (fd >= 0)
		close(fd);
}

static void display(int on)
{
	char p[128];

	snprintf(p, sizeof(p), "/sys/class/graphics/%s/blank", fb);
	put(p, on ? "0" : "4");		/* FB_BLANK_UNBLANK / POWERDOWN */
	snprintf(p, sizeof(p), "/sys/class/backlight/%s/bl_power", bl);
	put(p, on ? "0" : "4");
}

static void run_hooks(int open)
{
	const char *arg = open ? "open" : "closed";
	struct dirent **list;
	char p[512];
	struct stat st;
	int i, n = scandir(hookdir, &list, NULL, alphasort);

	for (i = 0; i < n; i++) {
		snprintf(p, sizeof(p), "%s/%s", hookdir, list[i]->d_name);
		free(list[i]);
		if (stat(p, &st) || !S_ISREG(st.st_mode) || access(p, X_OK))
			continue;
		if (fork() == 0) {
			execl(p, p, arg, (char *)NULL);
			fprintf(stderr, "s22-lidd: %s: %s\n", p, strerror(errno));
			_exit(127);
		}
	}
	if (n >= 0)
		free(list);
}

static void lid(int open)
{
	display(open);
	run_hooks(open);
}

static volatile sig_atomic_t reapply;

static void on_usr1(int sig)
{
	(void)sig;
	reapply = 1;
}

/* Time spent suspended so far, in ms */
static long long slept_ms(void)
{
	struct timespec b, m;

	clock_gettime(CLOCK_BOOTTIME, &b);
	clock_gettime(CLOCK_MONOTONIC, &m);
	return (b.tv_sec - m.tv_sec) * 1000LL + (b.tv_nsec - m.tv_nsec) / 1000000;
}

static int lid_open(int fd)
{
	unsigned char state[SW_MAX / 8 + 1] = { 0 };

	ioctl(fd, EVIOCGSW(sizeof(state)), state);
	return !BIT(state, SW_LID);
}

/* The input device that reports SW_LID */
static int open_lid(void)
{
	unsigned char sw[SW_MAX / 8 + 1];
	char p[300];
	struct dirent *de;
	DIR *d = opendir("/dev/input");
	int fd;

	if (!d)
		return -1;
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(p, sizeof(p), "/dev/input/%s", de->d_name);
		fd = open(p, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(sw, 0, sizeof(sw));
		if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sw)), sw) >= 0 && BIT(sw, SW_LID)) {
			closedir(d);
			return fd;
		}
		close(fd);
	}
	closedir(d);
	errno = ENODEV;
	return -1;
}

int main(int argc, char **argv)
{
	struct input_event ev;
	struct pollfd pfd;
	long long slept;
	int fd, opt, n;

	while ((opt = getopt(argc, argv, "f:b:d:")) != -1) {
		switch (opt) {
		case 'f': fb = optarg; break;
		case 'b': bl = optarg; break;
		case 'd': hookdir = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-f FB] [-b BACKLIGHT] [-d HOOKDIR]\n",
				argv[0]);
			return 2;
		}
	}
	signal(SIGCHLD, SIG_IGN);	/* hooks are not waited for */
	signal(SIGUSR1, on_usr1);
	fd = open_lid();
	if (fd < 0) {
		perror("s22-lidd: lid switch");
		return 1;
	}
	/* Start from the lid's current position */
	lid(lid_open(fd));
	slept = slept_ms();
	pfd.fd = fd;
	pfd.events = POLLIN;
	for (;;) {
		n = poll(&pfd, 1, 2000);
		if (n < 0 && errno != EINTR)
			break;
		if (slept_ms() - slept > 500) {	/* resumed from suspend */
			slept = slept_ms();
			reapply = 1;
		}
		if (reapply) {
			reapply = 0;
			lid(lid_open(fd));
		}
		if (n <= 0)
			continue;
		if (read(fd, &ev, sizeof(ev)) != sizeof(ev))
			break;
		if (ev.type == EV_SW && ev.code == SW_LID)
			lid(!ev.value);
	}
	perror("s22-lidd");
	return 1;
}
