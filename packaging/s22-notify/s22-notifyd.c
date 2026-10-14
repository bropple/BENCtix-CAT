// SPDX-License-Identifier: GPL-2.0-only
/*
 * s22-notifyd: alerts for new notifications on the Cat S22 Flip.
 *
 * Notifications are files in NOTIFYDIR (default /run/s22-notify), written by
 * s22-notify: "key=value" lines, at least "text=", usually "source=" (a
 * file without any key=value line is a plain-text notification from the
 * "default" source). For each new one the vibrator plays the pattern set for
 * its source in PATTERNS (default /var/lib/s22-notify/vibrate), re-read when
 * it changes:
 *
 *   default=200,150,200     on,off,on,... in ms (up to 16 steps)
 *   sms=400
 *   alarm=none              no vibration for this source
 *   strength=100            percent, for every pattern
 *
 * Without the file, every source gets 200,150,200. The phone's settings
 * (D042) come first: none with vibrate=off; the text vibration of the
 * contact it is from (X-S22-TEXTVIB); vib_SOURCE ("none", "default" or a
 * pattern); vib_notify; then the file. Their strength is vib_strength
 * (1-15), else the file's. Alerts closer together
 * than one second are merged. Display is someone else's job (s22-outerd,
 * and s22-shell in the GUI).
 *
 * Each alert also plays a sound (s22-play, at the ring and notification
 * volume, D041), chosen from the phone's settings (SETTINGS): none with
 * sounds=off or during a call; the text tone of the contact it is from
 * ("from=" in the notification; X-S22-TEXTTONE in CONTACTS); else
 * sound_SOURCE (a file, "silent" or "default"); else notify_sound, else
 * DEFAULT_SOUND. A ringing call ("call") gets none: s22-calld rings.
 *
 * A text (source "sms") is also read aloud after its sound with
 * tts_messages=on in the settings (Settings, Accessibility, D065): "Message
 * from NAME. TEXT" to s22-say (SAY_SOCK), NAME the contact's, else the
 * number digit by digit; not with sounds=off or during a call.
 *
 * Dismissing: the directory is sticky, so only a notification's owner (or
 * root) may delete it. Anyone else in group wheel asks by creating
 * ".dismiss-NAME" there (s22-notify -d NAME); this deletes NAME and the
 * marker.
 *
 *   s22-notifyd [-v] [-n NOTIFYDIR] [-p PATTERNS] [-d VIBRATOR]
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_STEPS 16
#define BIT(arr, b)	((arr)[(b) / 8] & (1 << ((b) % 8)))

static const char *notifydir = "/run/s22-notify";
static const char *patterns = "/var/lib/s22-notify/vibrate";
static const char *vibdev;

#define SETTINGS	"/var/lib/s22/settings"
#define CONTACTS	"/var/lib/s22/contacts.vcf"
#define CALL_STATE	"/run/s22-call/state"
#define DEFAULT_SOUND	"/usr/share/sounds/aosp/notifications/pixiedust.ogg"
#define SAY_SOCK	"/run/s22-say/sock"
static bool verbose;

#define dbg(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while (0)

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

/* ------------------------------------------------------------ vibrator --- */
static int vfd = -1;

static int open_vibrator(void)
{
	unsigned char ff[FF_MAX / 8 + 1];
	char p[300];
	struct dirent *de;
	DIR *d;
	int fd;

	if (vibdev)
		return open(vibdev, O_RDWR | O_CLOEXEC);
	d = opendir("/dev/input");
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(p, sizeof(p), "/dev/input/%s", de->d_name);
		fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(ff, 0, sizeof(ff));
		if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff) >= 0 && BIT(ff, FF_RUMBLE)) {
			dbg("vibrator %s\n", p);
			closedir(d);
			return fd;
		}
		close(fd);
	}
	closedir(d);
	errno = ENODEV;
	return -1;
}

/* Play on,off,on,... (ms) at strength 0-100 */
static void vibrate(const int *steps, int n, int strength)
{
	struct ff_effect fx = {
		.type = FF_RUMBLE,
		.id = -1,
		.u.rumble.strong_magnitude = (unsigned short)(0xffff * strength / 100),
	};
	struct input_event ev = { .type = EV_FF };
	int i;

	if (vfd < 0 || !strength)
		return;
	for (i = 0; i < n; i++) {
		if (i % 2 == 0 && steps[i] > 0) {
			fx.replay.length = steps[i];
			if (ioctl(vfd, EVIOCSFF, &fx) < 0) {
				fprintf(stderr, "s22-notifyd: upload effect: %s\n", strerror(errno));
				return;
			}
			ev.code = fx.id;
			ev.value = 1;
			if (write(vfd, &ev, sizeof(ev)) < 0)
				fprintf(stderr, "s22-notifyd: play: %s\n", strerror(errno));
		}
		sleep_ms(steps[i]);
	}
	if (fx.id >= 0)
		ioctl(vfd, EVIOCRMFF, fx.id);
}

/* ------------------------------------------------------------ patterns --- */
static time_t patterns_mtime = -1;
static char pattern_lines[64][128];
static int npatterns, strength = 100;

static void load_patterns(void)
{
	struct stat st;
	char line[128];
	FILE *f;

	if (stat(patterns, &st))
		st.st_mtime = 0;
	if (st.st_mtime == patterns_mtime)
		return;
	patterns_mtime = st.st_mtime;
	npatterns = 0;
	strength = 100;
	f = fopen(patterns, "re");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f) && npatterns < 64) {
		char *k = line;

		while (*k == ' ' || *k == '\t')
			k++;
		k[strcspn(k, "#\r\n")] = 0;
		if (!strchr(k, '='))
			continue;
		if (!strncmp(k, "strength=", 9)) {
			strength = atoi(k + 9);
			if (strength < 0 || strength > 100)
				strength = 100;
			continue;
		}
		snprintf(pattern_lines[npatterns++], sizeof(pattern_lines[0]), "%s", k);
	}
	fclose(f);
	dbg("%d patterns, strength %d\n", npatterns, strength);
}

/* on,off,on,... into steps; "none" is none */
static int parse_steps(const char *p, int *steps)
{
	int n = 0;

	while (*p == ' ')
		p++;
	if (!strncmp(p, "none", 4))
		return 0;
	while (*p && n < MAX_STEPS) {
		int v = atoi(p);

		steps[n++] = v < 0 ? 0 : v > 5000 ? 5000 : v;
		p += strcspn(p, ",");
		if (*p == ',')
			p++;
	}
	return n;
}

/* The steps for a source: its own line, else "default", else 200,150,200 */
static int pattern_for(const char *source, int *steps)
{
	const char *def = NULL, *mine = NULL, *p;
	size_t len = strlen(source);
	int i;

	load_patterns();
	for (i = 0; i < npatterns; i++) {
		char *l = pattern_lines[i];

		if (!strncmp(l, source, len) && (l[len] == '=' || l[len] == ' '))
			mine = strchr(l, '=') + 1;
		else if (!strncmp(l, "default", 7) && (l[7] == '=' || l[7] == ' '))
			def = strchr(l, '=') + 1;
	}
	p = mine ? mine : def ? def : "200,150,200";
	return parse_steps(p, steps);
}

/* --------------------------------------------------------------- main --- */
/* The source of a notification file ("default" if it has none) */
static void source_of(const char *path, char *src, size_t len)
{
	char line[256];
	FILE *f = fopen(path, "re");

	snprintf(src, len, "default");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "source=", 7)) {
			line[strcspn(line, "\r\n")] = 0;
			if (line[7])
				snprintf(src, len, "%.63s", line + 7);	/* names are short */
			break;
		}
	fclose(f);
}

/* "key=value" from a file; false if not there */
static bool file_value(const char *path, const char *key, char *buf, size_t len)
{
	char line[512];
	size_t kl = strlen(key);
	bool found = false;
	FILE *f = fopen(path, "re");

	buf[0] = 0;
	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, key, kl) && line[kl] == '=') {
			line[strcspn(line, "\r\n")] = 0;
			snprintf(buf, len, "%s", line + kl + 1);
			found = true;
		}
	if (f)
		fclose(f);
	return found;
}

static bool in_call(void)
{
	char line[128];
	bool call = false;
	FILE *f = fopen(CALL_STATE, "re");

	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, "call ", 5))
			call = true;
	if (f)
		fclose(f);
	return call;
}

/* The last ten digits, the national number (all if fewer than seven) */
static void tail_digits(const char *num, char *out, size_t len)
{
	char d[48];
	size_t n = 0;

	for (; *num && n < sizeof(d) - 1; num++)
		if (*num >= '0' && *num <= '9')
			d[n++] = *num;
	d[n] = 0;
	snprintf(out, len, "%s", n > 10 ? d + n - 10 : d);
}

/* A contact's FIELD (X-S22-TEXTTONE) for the number it is from, or "" */
static void contact_field(const char *number, const char *field, char *buf, size_t len)
{
	char line[512], want[16], have[16], val[256] = "";
	bool match = false;
	size_t fl = strlen(field);
	FILE *f;

	buf[0] = 0;
	tail_digits(number, want, sizeof(want));
	if (strlen(want) < 3 || !(f = fopen(CONTACTS, "re")))
		return;
	while (fgets(line, sizeof(line), f)) {
		char *key = line, *colon = strchr(line, ':');
		line[strcspn(line, "\r\n")] = 0;
		if (!colon)
			continue;
		*colon = 0;
		if (strchr(key, '.') && strchr(key, '.') < key + strcspn(key, ";"))
			key = strchr(key, '.') + 1;
		if (!strcasecmp(key, "BEGIN")) {
			match = false;
			val[0] = 0;
		} else if (!strncasecmp(key, "TEL", 3)) {
			tail_digits(colon + 1, have, sizeof(have));
			if (!strcmp(have, want))
				match = true;
		} else if (!strncasecmp(key, field, fl) && (!key[fl] || key[fl] == ';')) {
			snprintf(val, sizeof(val), "%s", colon + 1);
		} else if (!strcasecmp(key, "END") && match && val[0]) {
			snprintf(buf, len, "%s", val);
			break;
		}
	}
	fclose(f);
}

/* The sound for a notification, or false for none */
static bool sound_for(const char *path, const char *src, char *file, size_t len)
{
	char v[256], from[48], key[96];

	if (file_value(SETTINGS, "sounds", v, sizeof(v)) && !strcmp(v, "off"))
		return false;
	if (!strcmp(src, "call") || in_call())
		return false;
	if (file_value(path, "from", from, sizeof(from)) && from[0]) {
		contact_field(from, "X-S22-TEXTTONE", v, sizeof(v));
		if (!strcmp(v, "silent"))
			return false;
		if (v[0] == '/' && access(v, R_OK) == 0) {
			snprintf(file, len, "%s", v);
			return true;
		}
	}
	snprintf(key, sizeof(key), "sound_%s", src);
	if (file_value(SETTINGS, key, v, sizeof(v))) {
		if (!strcmp(v, "silent"))
			return false;
		if (v[0] == '/' && access(v, R_OK) == 0) {
			snprintf(file, len, "%s", v);
			return true;
		}
	}
	if (file_value(SETTINGS, "notify_sound", v, sizeof(v))) {
		if (!strcmp(v, "silent"))
			return false;
		if (v[0] == '/' && access(v, R_OK) == 0) {
			snprintf(file, len, "%s", v);
			return true;
		}
	}
	snprintf(file, len, "%s", DEFAULT_SOUND);
	return access(file, R_OK) == 0;
}

/* A pattern setting: true if it decides (into steps, *n), false to go on */
static bool vib_value(const char *v, int *steps, int *n)
{
	if (!v[0] || !strcmp(v, "default"))
		return false;
	*n = parse_steps(v, steps);
	return true;
}

/* The vibration of a notification and its strength (D042) */
static int vibration_for(const char *path, const char *src, int *steps, int *pct)
{
	char v[160], from[48], key[96];
	int n;

	*pct = strength;
	if (file_value(SETTINGS, "vib_strength", v, sizeof(v)) && atoi(v) > 0)
		*pct = (atoi(v) > 15 ? 15 : atoi(v)) * 100 / 15;
	if (file_value(SETTINGS, "vibrate", v, sizeof(v)) && !strcmp(v, "off"))
		return 0;
	if (file_value(path, "from", from, sizeof(from)) && from[0]) {
		contact_field(from, "X-S22-TEXTVIB", v, sizeof(v));
		if (vib_value(v, steps, &n))
			return n;
	}
	snprintf(key, sizeof(key), "vib_%s", src);
	if (file_value(SETTINGS, key, v, sizeof(v)) && vib_value(v, steps, &n))
		return n;
	if (file_value(SETTINGS, "vib_notify", v, sizeof(v)) && vib_value(v, steps, &n))
		return n;
	return pattern_for(src, steps);
}

static void play(const char *file)
{
	pid_t pid = fork();

	if (pid == 0) {
		execlp("s22-play", "s22-play", file, (char *)NULL);
		_exit(127);
	}
}

/* A text read aloud (tts_messages=on): after its sound, in the background */
static void say_message(const char *path)
{
	char v[64], from[48], name[256], text[1200], line[1400];

	if (!file_value(SETTINGS, "tts_messages", v, sizeof(v)) || strcmp(v, "on"))
		return;
	if ((file_value(SETTINGS, "sounds", v, sizeof(v)) && !strcmp(v, "off")) || in_call())
		return;
	file_value(path, "from", from, sizeof(from));
	if (!file_value(path, "text", text, sizeof(text)))
		return;
	/* s22-smsd's "NUMBER: TEXT": the number is said as who it is from */
	char *t = text;
	size_t fl = strlen(from);
	if (fl && !strncmp(t, from, fl) && !strncmp(t + fl, ": ", 2))
		t += fl + 2;
	contact_field(from, "FN", name, sizeof(name));
	if (!name[0]) {
		size_t n = 0;
		for (const char *d = from; *d && n < sizeof(name) - 3; d++)
			if (*d >= '0' && *d <= '9') {
				name[n++] = *d;
				name[n++] = ' ';
			}
		name[n] = 0;
	}
	int n = snprintf(line, sizeof(line), "say Message from %s. %s",
			 name[0] ? name : "an unknown sender", t);
	for (int i = 4; i < n && i < (int)sizeof(line); i++)
		if (line[i] == '\n' || line[i] == '\r')
			line[i] = ' ';
	if (fork() != 0)
		return;
	sleep_ms(1200);		/* the text tone first */
	struct sockaddr_un sa = { .sun_family = AF_UNIX, .sun_path = SAY_SOCK };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
		size_t len = strlen(line);
		line[len] = '\n';
		if (write(fd, line, len + 1) < 0)
			dbg("s22-say: %s\n", strerror(errno));
	}
	_exit(0);
}

static volatile sig_atomic_t quit;
static void on_term(int sig) { (void)sig; quit = 1; }

/* Root, or a member of group wheel */
static bool may_dismiss(uid_t uid)
{
	struct passwd *pw = uid ? getpwuid(uid) : NULL;
	struct group *wheel = getgrnam("wheel");
	gid_t groups[64];
	int n = 64;

	if (uid == 0)
		return true;
	if (pw == NULL || wheel == NULL ||
	    getgrouplist(pw->pw_name, pw->pw_gid, groups, &n) < 0)
		return false;
	for (int i = 0; i < n; i++)
		if (groups[i] == wheel->gr_gid)
			return true;
	return false;
}

static void dismiss(const char *marker)
{
	const char *name = marker + strlen(".dismiss-");
	char path[600], mpath[600];
	struct stat st;

	snprintf(mpath, sizeof(mpath), "%s/%s", notifydir, marker);
	if (lstat(mpath, &st) == 0 && S_ISREG(st.st_mode) && may_dismiss(st.st_uid) &&
	    name[0] && name[0] != '.' && !strchr(name, '/')) {
		snprintf(path, sizeof(path), "%s/%s", notifydir, name);
		if (unlink(path) == 0)
			dbg("dismissed %s (uid %u)\n", name, (unsigned)st.st_uid);
	}
	unlink(mpath);
}

int main(int argc, char **argv)
{
	struct sigaction sa = { 0 };
	long long last = 0;
	int opt, in;

	while ((opt = getopt(argc, argv, "vn:p:d:")) != -1) {
		switch (opt) {
		case 'v': verbose = true; break;
		case 'n': notifydir = optarg; break;
		case 'p': patterns = optarg; break;
		case 'd': vibdev = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-v] [-n NOTIFYDIR] [-p PATTERNS] [-d VIBRATOR]\n",
				argv[0]);
			return 2;
		}
	}
	vfd = open_vibrator();
	if (vfd < 0)
		fprintf(stderr, "s22-notifyd: no vibrator: %s\n", strerror(errno));
	mkdir(notifydir, 01777);
	chmod(notifydir, 01777);
	in = inotify_init1(IN_CLOEXEC);
	if (in < 0 || inotify_add_watch(in, notifydir, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
		perror("s22-notifyd: inotify");
		return 1;
	}
	/* No SA_RESTART: the signal must interrupt the blocking read() */
	sa.sa_handler = on_term;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	/* The sound players reap themselves */
	struct sigaction chld = { .sa_handler = SIG_IGN, .sa_flags = SA_NOCLDWAIT };
	sigemptyset(&chld.sa_mask);
	sigaction(SIGCHLD, &chld, NULL);
	fprintf(stderr, "s22-notifyd: watching %s\n", notifydir);

	while (!quit) {
		char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
		ssize_t len = read(in, buf, sizeof(buf));
		char *p;

		if (len <= 0) {
			if (len < 0 && errno == EINTR)
				continue;
			break;
		}
		for (p = buf; p < buf + len;) {
			struct inotify_event *ev = (struct inotify_event *)p;
			char path[600], src[64];
			int steps[MAX_STEPS], n;

			p += sizeof(*ev) + ev->len;
			if (ev->len && strncmp(ev->name, ".dismiss-", 9) == 0) {
				dismiss(ev->name);
				continue;
			}
			if (!ev->len || ev->name[0] == '.')
				continue;	/* s22-notify's temporary file */
			snprintf(path, sizeof(path), "%s/%s", notifydir, ev->name);
			source_of(path, src, sizeof(src));
			int pct;
			n = vibration_for(path, src, steps, &pct);
			dbg("%s: source %s, %d steps\n", ev->name, src, n);
			if (now_ms() - last < 1000)
				continue;	/* merge bursts */
			char sound[256];
			if (sound_for(path, src, sound, sizeof(sound)))
				play(sound);
			if (!strcmp(src, "sms"))
				say_message(path);
			vibrate(steps, n, pct);
			last = now_ms();
		}
	}
	return 0;
}
