// SPDX-License-Identifier: MIT
/*
 * s22-settings: the phone's settings, in pages like Android's: Network &
 * internet (Wi-Fi, mobile network, VPN), connected devices, display, the
 * outer screen, system information and developer options.
 *
 * Choices are saved in /var/lib/s22/settings (see s22ui.h); the programs
 * that use them read that file (s22-wm the screen timeout, s22-outerd the
 * clock and the outer screen's, s22-settings-apply the rest at boot).
 * Brightness goes to the backlight at once (a udev rule lets group video
 * write it), Wi-Fi and Bluetooth power to rfkill (group rfkill).
 *
 * Wi-Fi networks are wpa_supplicant's, through its control socket (group
 * wheel): scanning, connecting, saving (update_config) and forgetting. A
 * new network is saved only once it has connected; a wrong password
 * removes it again. The cellular state comes from s22-celld, which also
 * owns mobile data: the "mobile_data" setting lets it use cellular only
 * while Wi-Fi is unavailable, and the mobile ping test holds the cellular
 * link through its control socket (Wi-Fi or not) for as long as it runs.
 * The VPN page shows s22-vpnd's state and switches Mullvad with the
 * "mullvad" setting; logins go to s22-vpnd's control socket: Tailscale's
 * login link as a QR code to scan with another phone, or an auth key;
 * Mullvad's account number (s22-vpnd registers the phone as a device, and
 * offers to replace one when the account is full), its server country,
 * and logging out. Tailscale with the lid closed (tailscale_lid, D082):
 * paused by s22-sleepd (the battery lasts longer), or kept connected. Passwords, keys and account numbers show as asterisks.
 *
 * Sounds (D041): sounds on or vibrate only, the ring and notification
 * volume (s22-play's), the media volume (PipeWire's Media output, as the
 * volume keys set it), the ringtone, the default notification sound and
 * one for each app that notifies; the picker plays each as it is chosen.
 * Vibration (D042) the same way: on or off, its strength, the ringing
 * pattern, the default notification pattern and one an app; presets, the
 * patterns recorded here (hold OK in the rhythm, Custom 1-4), each felt as
 * it is chosen.
 *
 * Security (D044): the screen lock, a PIN or none, changed or removed only
 * with the current PIN (only a salted hash is kept, s22pin.c), how long
 * after the display goes dark it locks, and locking now (s22-lock).
 *
 * Accessibility (D065): speech through s22-say (BENCmouth): who is calling
 * said between rings (tts_calls, s22-calld), texts read aloud (tts_messages,
 * s22-notifyd), the voice (tts_voice: BENCmouth's presets, from bm -l, each
 * heard as it is chosen), and a sample of it.
 *
 * Sleep with the lid closed (Display, lid_suspend, D070): s22-sleepd
 * suspends the phone while the lid is closed and nothing needs it.
 *
 * Wallpapers (D066): the home and the lock screen's (Display) and the
 * outer screen's, chosen in Photos' Wallpapers tab (wallpaper:home, :lock,
 * :outer links), where each is seen as it will look; the outer one's
 * speed when it moves.
 *
 * Bluetooth devices are s22-btd's (/run/s22-bt, watched): the paired ones
 * (connect, disconnect, forget), and a scan while "Pair new device" is
 * open, from which one is paired. A code to compare during pairing is
 * asked here, never accepted without the user.
 *
 * Up/Down choose a row, Left/Right or OK change it or open a page, Back
 * goes up a page, and closes the app from the first one.
 */
#define _GNU_SOURCE		/* pipe2 */
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <linux/rfkill.h>
#include <qrencode.h>
#include "s22ui.h"

#define BACKLIGHT	"/sys/class/backlight/backlight"
#define WPA_DIR		"/run/wpa_supplicant"
#define CELL_STATE	"/run/s22-cell/state"
#define CELL_CTL	"/run/s22-cell/ctl"
#define VPN_STATE	"/run/s22-vpn/state"
#define VPN_CTL		"/run/s22-vpn/ctl"
#define BT_DIR		"/run/s22-bt"
#define BT_STATE	BT_DIR "/state"
#define BT_CTL		BT_DIR "/ctl"

enum page {
	P_MAIN, P_NET, P_WIFI, P_WIFI_SAVED, P_WIFI_NET, P_WIFI_PASS, P_CELL,
	P_VPN, P_TS_QR, P_TS_KEY, P_MV_ACCOUNT, P_MV_DEVICES, P_MV_COUNTRY,
	P_DEVICES, P_BT_SCAN, P_BT_DEV, P_SOUNDS, P_TONE, P_VIB, P_VIB_REC,
	P_SECURITY, P_PIN, P_DISPLAY, P_OUTER, P_SYSTEM, P_DEV, P_SHARE, P_SHARE_PASS,
	P_ACCESS, P_VOICE, P_COUNT
};
static const char *titles[P_COUNT] = {
	"Settings", "Network & internet", "Wi-Fi", "Saved networks", "Network",
	"Wi-Fi password", "Mobile network", "VPN", "Tailscale login", "Tailscale auth key",
	"Mullvad login", "Mullvad devices", "Mullvad server", "Connected devices",
	"Pair new device", "Bluetooth device", "Sounds", "Sound", "Vibration",
	"Record a pattern", "Security", "Screen lock", "Display",
	"Outer screen", "System", "Developer options", "File sharing",
	"File sharing password", "Accessibility", "Voice",
};
static const enum page parent[P_COUNT] = {
	[P_NET] = P_MAIN, [P_WIFI] = P_NET, [P_WIFI_SAVED] = P_WIFI,
	[P_WIFI_NET] = P_WIFI_SAVED, [P_WIFI_PASS] = P_WIFI, [P_CELL] = P_NET,
	[P_VPN] = P_NET, [P_TS_QR] = P_VPN, [P_TS_KEY] = P_VPN,
	[P_MV_ACCOUNT] = P_VPN, [P_MV_DEVICES] = P_MV_ACCOUNT, [P_MV_COUNTRY] = P_VPN,
	[P_DEVICES] = P_MAIN, [P_BT_SCAN] = P_DEVICES, [P_BT_DEV] = P_DEVICES,
	[P_DISPLAY] = P_MAIN, [P_OUTER] = P_MAIN, [P_SYSTEM] = P_MAIN, [P_DEV] = P_MAIN,
	[P_SOUNDS] = P_MAIN, [P_TONE] = P_SOUNDS, [P_VIB] = P_SOUNDS,
	[P_VIB_REC] = P_VIB, [P_SECURITY] = P_MAIN, [P_PIN] = P_SECURITY,
	[P_SHARE] = P_NET, [P_SHARE_PASS] = P_SHARE, [P_ACCESS] = P_MAIN,
	[P_VOICE] = P_ACCESS,
};
static enum page page;
static struct ui_list lists[P_COUNT];
static char flash[96];		/* a message for a few seconds */
static struct ui *app_ui;

/* A choice among fixed values of one setting */
struct choice {
	const char *key;
	const char *values[8];
	const char *labels[8];
	int def;
};

static const struct choice screen_timeout = { "screen_timeout",
	{ "15", "30", "60", "120", "300", "0" },
	{ "15 seconds", "30 seconds", "1 minute", "2 minutes", "5 minutes",
	  "Never" }, 1 };
static const struct choice lid_suspend = { "lid_suspend", { "on", "off" },
	{ "On: sleeps while nothing needs it", "Off: stays awake" }, 0 };
static const struct choice clock_fmt = { "clock", { "12", "24" },
	{ "12-hour", "24-hour" }, 0 };
static const struct choice theme = { "theme", { "dark", "black", "light" },
	{ "Dark", "Black", "Light" }, 0 };
static const struct choice outer_timeout = { "timeout",
	{ "5", "10", "20", "30", "60" },
	{ "5 seconds", "10 seconds", "20 seconds", "30 seconds", "1 minute" }, 1 };
static const struct choice cellular = { "cellular", { "on", "off" },
	{ "On", "Off: no calls, texts or data" }, 0 };
static const struct choice mobile_data = { "mobile_data", { "off", "on" },
	{ "Off: Wi-Fi only", "On when Wi-Fi is unavailable" }, 0 };
static const struct choice mullvad = { "mullvad", { "off", "on" },
	{ "Off", "On: all traffic through Mullvad" }, 0 };
static const struct choice tailscale_lid = { "tailscale_lid", { "pause", "on" },
	{ "Paused: the battery lasts longer", "Connected: reachable on the tailnet" }, 0 };
static const struct choice sounds_on = { "sounds", { "on", "off" },
	{ "On", "Off: vibrate only" }, 0 };
static const struct choice vibrate_on = { "vibrate", { "on", "off" },
	{ "On", "Off" }, 0 };
static const struct choice tts_calls = { "tts_calls", { "off", "on" },
	{ "Off", "On: between rings" }, 0 };
static const struct choice tts_messages = { "tts_messages", { "off", "on" },
	{ "Off", "On: who it is from, then the text" }, 0 };
#define TTS_VOICE	"BENCmouth"

/* BENCmouth's voice presets, from bm -l ("  NAME   f0 ...") */
static char voices[48][32];
static int nvoices;

static void voices_load(void)
{
	char line[256];
	FILE *p;

	if (nvoices || !(p = popen("bm -l 2>/dev/null", "r")))
		return;
	while (nvoices < 48 && fgets(line, sizeof(line), p)) {
		char *f0 = strstr(line, " f0 ");
		if (strncmp(line, "  ", 2) || line[2] == ' ' || line[2] == '-' || !f0)
			continue;
		while (f0 > line + 2 && f0[-1] == ' ')
			f0--;
		*f0 = 0;
		if (!strchr(line + 2, '\''))	/* it goes in a shell command */
			snprintf(voices[nvoices++], sizeof(voices[0]), "%s", line + 2);
	}
	pclose(p);
}

/* Wallpapers: chosen in Photos (wallpaper:KIND), made by s22-wallpaper */
enum { WALL_HOME, WALL_LOCK, WALL_OUTER };
static const char *const wall_kind[] = { "home", "lock", "outer" };
static const struct choice wall_speed = { "wallpaper_speed",
	{ "0.25", "0.5", "1", "1.5", "2" },
	{ "Quarter speed", "Half speed", "Normal", "1.5 times", "Double" }, 2 };

/* The chosen one's file name, or "None" */
static const char *wall_name(int kind, char *buf, size_t len)
{
	char key[32];

	snprintf(key, sizeof(key), "wallpaper_%s", wall_kind[kind]);
	ui_setting(key, "none", buf, len);
	if (!strcmp(buf, "none") || !buf[0])
		return "None";
	return strrchr(buf, '/') ? strrchr(buf, '/') + 1 : buf;
}
static const struct choice lock_after = { "lock_after", { "0", "30", "60", "300" },
	{ "Immediately", "After 30 seconds", "After 1 minute", "After 5 minutes" }, 0 };
static const struct choice outer_blink = { "blink", { "yes", "no" },
	{ "Blinks", "Steady" }, 0 };

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(flash, sizeof(flash), fmt, ap);
	va_end(ap);
	if (app_ui)
		ui_timer(app_ui, 4000);
}

static int choice_get(const struct choice *c)
{
	char v[32];

	ui_setting(c->key, c->values[c->def], v, sizeof(v));
	for (int i = 0; c->values[i]; i++)
		if (strcmp(v, c->values[i]) == 0)
			return i;
	return c->def;
}

static void choice_step(const struct choice *c, int d)
{
	int n = 0, i = choice_get(c);

	while (c->values[n])
		n++;
	i = (i + d + n) % n;
	if (!ui_setting_set(c->key, c->values[i]))
		say("Cannot save settings");
}

/* ---------------------------------------------------- the hardware --- */

static int read_int(const char *path, int def)
{
	char buf[32];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return def;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return def;
	buf[n] = 0;
	return atoi(buf);
}

static int brightness_pct(void)
{
	int max = read_int(BACKLIGHT "/max_brightness", 255);
	int cur = read_int(BACKLIGHT "/brightness", max);

	return max > 0 ? (cur * 100 + max / 2) / max : 0;
}

static void brightness_step(int d)
{
	/* Never all the way off: a dark screen looks like a dead one */
	static const int levels[] = { 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
	int max = read_int(BACKLIGHT "/max_brightness", 255);
	int cur = brightness_pct(), i = 0, n = 11;
	char buf[16];

	while (i < n - 1 && levels[i + 1] <= cur)
		i++;
	i += d;
	i = i < 0 ? 0 : i >= n ? n - 1 : i;
	int pct = levels[i];
	snprintf(buf, sizeof(buf), "%d", pct * max / 100);
	int fd = open(BACKLIGHT "/brightness", O_WRONLY | O_CLOEXEC);
	if (fd < 0 || write(fd, buf, strlen(buf)) < 0)
		say("Cannot set the brightness");
	if (fd >= 0)
		close(fd);
	ui_setting_set("brightness", buf);
}

/* 1 on, 0 off (soft blocked), -1 none of that type */
static int radio_on(int type)
{
	char path[64];

	for (int i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%d/type", i);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		char t[32] = "";
		if (fd < 0)
			continue;
		ssize_t n = read(fd, t, sizeof(t) - 1);
		close(fd);
		if (n <= 0)
			continue;
		t[strcspn(t, "\n")] = 0;
		if (strcmp(t, type == RFKILL_TYPE_WLAN ? "wlan" : "bluetooth") != 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%d/soft", i);
		return read_int(path, 0) ? 0 : 1;
	}
	return -1;
}

static void radio_set(int type, bool on)
{
	struct rfkill_event ev = { .type = type, .op = RFKILL_OP_CHANGE_ALL,
		.soft = !on };
	int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);

	if (fd < 0 || write(fd, &ev, sizeof(ev)) != sizeof(ev))
		say("Cannot switch the radio");
	if (fd >= 0)
		close(fd);
	ui_setting_set(type == RFKILL_TYPE_WLAN ? "wifi" : "bluetooth",
		       on ? "on" : "off");
}

/* key=value from a daemon's state file ("" if not there) */
static void state_get(const char *path, const char *key, char *buf, int len)
{
	char line[256];
	size_t kl = strlen(key);
	FILE *f = fopen(path, "re");

	buf[0] = 0;
	while (f && fgets(line, sizeof(line), f))
		if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
			snprintf(buf, len, "%s", line + kl + 1);
			buf[strcspn(buf, "\n")] = 0;
		}
	if (f)
		fclose(f);
}

static void cell_get(const char *key, char *buf, int len)
{
	state_get(CELL_STATE, key, buf, len);
}

static void vpn_get(const char *key, char *buf, int len)
{
	state_get(VPN_STATE, key, buf, len);
}

/* ------------------------------------------- s22-vpnd and s22-btd --- */

/*
 * One request at a time to a daemon's control socket: a line out, the
 * answer read in the main loop until it closes, then handed to done().
 */
static int ctl_fd = -1;
static char ctl_reply[8192];
static int ctl_len;
static void (*ctl_done)(struct ui *ui, char *reply);

static void on_ctl(struct ui *ui, int fd)
{
	ssize_t n = read(fd, ctl_reply + ctl_len, sizeof(ctl_reply) - 1 - ctl_len);

	if (n > 0) {
		ctl_len += n;
		if (ctl_len < (int)sizeof(ctl_reply) - 1)
			return;
	}
	ui_unwatch(ui, fd);
	close(fd);
	ctl_fd = -1;
	ctl_reply[ctl_len] = 0;
	flash[0] = 0;
	if (ctl_done)
		ctl_done(ui, ctl_reply);
	ui_redraw(ui);
}

static bool ctl_request(const char *path, const char *daemon, struct ui *ui,
			void (*done)(struct ui *, char *), const char *busy,
			const char *fmt, ...) __attribute__((format(printf, 6, 7)));
#define vpn_request(...)	ctl_request(VPN_CTL, "s22-vpnd", __VA_ARGS__)
#define bt_request(...)		ctl_request(BT_CTL, "s22-btd", __VA_ARGS__)
static bool ctl_request(const char *path, const char *daemon, struct ui *ui,
			void (*done)(struct ui *, char *), const char *busy,
			const char *fmt, ...)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	char line[600];
	va_list ap;
	int fd, len;

	if (ctl_fd >= 0) {
		say("One moment…");
		return false;
	}
	va_start(ap, fmt);
	len = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	line[len++] = '\n';
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
	    write(fd, line, len) != len) {
		if (fd >= 0)
			close(fd);
		explicit_bzero(line, sizeof(line));
		say("%s is not running", daemon);
		return false;
	}
	explicit_bzero(line, sizeof(line));	/* keys, account numbers */
	ctl_fd = fd;
	ctl_len = 0;
	ctl_done = done;
	ui_watch(ui, fd, on_ctl);
	say("%s", busy);
	return true;
}

/* The answer's message: "error MSG" -> MSG */
static const char *ctl_msg(char *reply)
{
	reply[strcspn(reply, "\n")] = 0;
	return !strncmp(reply, "error ", 6) ? reply + 6 :
	       !strncmp(reply, "ok ", 3) ? reply + 3 : reply;
}

/* Mullvad's account devices, when it is full; its countries */
struct mv_device {
	char id[64], name[48], created[16];
};
static struct mv_device mv_devs[16];
static int nmv_devs;
struct mv_country {
	char code[8], name[48];
};
static struct mv_country mv_countries[60];
static int nmv_countries;
static struct ui_entry ts_entry, mv_entry, pin_entry;

/* The PIN pages: the current PIN first when there is one, then the new
 * one twice; or the current one alone, to turn the lock off */
enum pin_step { PS_CURRENT, PS_NEW, PS_CONFIRM };
static enum pin_step pin_step;
static bool pin_removing;
static char pin_new[16];

/* Destructive actions want a second press within a few seconds */
enum action { A_NONE, A_TS_LOGOUT, A_MV_LOGOUT, A_MV_REPLACE, A_BT_FORGET };
static enum action confirm_act;
static int confirm_arg;
static time_t confirm_until;

static bool confirmed(enum action a, int arg, const char *what)
{
	if (confirm_act == a && confirm_arg == arg && time(NULL) <= confirm_until) {
		confirm_act = A_NONE;
		return true;
	}
	confirm_act = a;
	confirm_arg = arg;
	confirm_until = time(NULL) + 4;
	say("Press again to %s", what);
	return false;
}

static void go(struct ui *ui, enum page to);

static void ts_key_done(struct ui *ui, char *reply)
{
	if (!strncmp(reply, "ok", 2)) {
		ui_entry_set(&ts_entry, "");
		say("Tailscale is logged in");
		page = P_VPN;
	} else {
		say("%s", ctl_msg(reply));
	}
}

static void mv_login_done(struct ui *ui, char *reply)
{
	if (!strncmp(reply, "full\n", 5)) {
		char *line = reply + 5, *nl;
		nmv_devs = 0;
		for (; *line && nmv_devs < 16; line = nl + 1) {
			struct mv_device *d = &mv_devs[nmv_devs];
			nl = strchr(line, '\n');
			if (!nl)
				break;
			*nl = 0;
			if (sscanf(line, "%63[^\t]\t%47[^\t]\t%15s", d->id, d->name,
				   d->created) >= 2)
				nmv_devs++;
		}
		go(ui, P_MV_DEVICES);
		say("The account is full: remove a device");
	} else if (!strncmp(reply, "ok", 2)) {
		ui_entry_set(&mv_entry, "");
		nmv_devs = 0;
		say("Mullvad: this phone is “%s”", ctl_msg(reply));
		page = P_VPN;
	} else {
		say("%s", ctl_msg(reply));
	}
}

/*
 * File sharing (D056): s22-fileshare through sudo, in the background (Samba
 * takes a moment to start, and a wrong password two seconds), its answer
 * read from a pipe.
 */
static struct { bool known, on, pass; char addr[64]; } share;
static int share_fd = -1;
static pid_t share_pid;
static char share_out[512];
static size_t share_len;
static void (*share_done)(struct ui *ui, const char *out);
static bool share_turn_on;	/* switch it on once the password is set */
static struct ui_entry share_entry;

static void share_read(struct ui *ui, int fd)
{
	ssize_t n = read(fd, share_out + share_len, sizeof(share_out) - 1 - share_len);

	if (n > 0) {
		share_len += n;
		share_out[share_len] = 0;
		if (share_len < sizeof(share_out) - 1)
			return;
	}
	ui_unwatch(ui, fd);
	close(fd);
	share_fd = -1;
	waitpid(share_pid, NULL, 0);
	share_out[share_len] = 0;
	void (*cb)(struct ui *, const char *) = share_done;
	share_done = NULL;
	if (cb)
		cb(ui, share_out);
	ui_redraw(ui);
}

static bool share_run(struct ui *ui, const char *arg, const char *input,
		      void (*done)(struct ui *, const char *))
{
	int out[2], in[2];

	if (share_fd >= 0 || pipe2(out, O_CLOEXEC) != 0)
		return false;
	if (pipe2(in, O_CLOEXEC) != 0) {
		close(out[0]);
		close(out[1]);
		return false;
	}
	pid_t pid = fork();
	if (pid == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		dup2(out[1], 2);
		execl("/usr/bin/sudo", "sudo", "-n", "/usr/local/bin/s22-fileshare", arg,
		      (char *)NULL);
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	if (pid > 0 && input) {
		size_t len = strlen(input);
		if (write(in[1], input, len) != (ssize_t)len || write(in[1], "\n", 1) != 1)
			;	/* the helper sees a short line and refuses it */
	}
	close(in[1]);
	if (pid < 0) {
		close(out[0]);
		return false;
	}
	share_pid = pid;
	share_fd = out[0];
	share_len = 0;
	share_out[0] = 0;
	share_done = done;
	ui_watch(ui, share_fd, share_read);
	return true;
}

/* "error: why" as "Why" for say() */
static const char *share_msg(const char *out)
{
	static char m[128];
	const char *p = strncmp(out, "error: ", 7) ? out : out + 7;

	snprintf(m, sizeof(m), "%.*s", (int)strcspn(p, "\n"), p);
	if (m[0] >= 'a' && m[0] <= 'z')
		m[0] -= 32;
	return m[0] ? m : "File sharing did not answer";
}

static void share_status_done(struct ui *ui, const char *out)
{
	const char *a = strstr(out, "address=");

	share.known = !strncmp(out, "on", 2) || !strncmp(out, "off", 3);
	share.on = !strncmp(out, "on", 2);
	share.pass = strstr(out, "password=yes") != NULL;
	share.addr[0] = 0;
	if (a)
		sscanf(a + 8, "%63[^\n]", share.addr);
}

static void share_refresh(struct ui *ui)
{
	share_run(ui, "status", NULL, share_status_done);
}

static void share_switch_done(struct ui *ui, const char *out)
{
	if (!strncmp(out, "ok", 2))
		say(share.on ? "File sharing is off" : "File sharing is on");
	else
		say("%s", share_msg(out));
	share_refresh(ui);
}

static void share_pass_done(struct ui *ui, const char *out)
{
	if (strncmp(out, "ok", 2)) {
		say("%s", share_msg(out));
		return;
	}
	ui_entry_set(&share_entry, "");
	page = P_SHARE;
	share.pass = true;
	if (share_turn_on) {
		share_turn_on = false;
		say("Password set; turning file sharing on…");
		share_run(ui, "on", NULL, share_switch_done);
	} else {
		say("The file sharing password is set");
	}
}

static void mv_countries_done(struct ui *ui, char *reply)
{
	char *line, *nl;

	if (strncmp(reply, "ok\n", 3) != 0) {
		say("%s", ctl_msg(reply));
		return;
	}
	nmv_countries = 0;
	for (line = reply + 3; *line && nmv_countries < 60; line = nl + 1) {
		struct mv_country *c = &mv_countries[nmv_countries];
		nl = strchr(line, '\n');
		if (!nl)
			break;
		*nl = 0;
		if (sscanf(line, "%7[^\t]\t%47[^\n]", c->code, c->name) == 2)
			nmv_countries++;
	}
}

static void vpn_said(struct ui *ui, char *reply)
{
	const char *m = ctl_msg(reply);

	say("%s", !strncmp(reply, "ok", 2) && !*m ? "Done" : m);
}

/* --------------------------------------------------------- Bluetooth --- */

struct bt_dev {
	char addr[20], name[64], icon[32];
	bool paired, connected;
	int rssi;			/* 0 if this scan has not seen it */
};
static struct bt_dev bt_devs[40];
static int nbt_devs;
static char bt_sel[20];			/* P_BT_DEV: its address */
static bool bt_scanning;		/* a scan asked for by the scan page */

static void bt_get(const char *key, char *buf, int len)
{
	state_get(BT_STATE, key, buf, len);
}

/* s22-btd's device list: ADDRESS NAME PAIRED CONNECTED RSSI ICON */
static void bt_load(void)
{
	char line[256];
	FILE *f = fopen(BT_DIR "/devices", "re");

	nbt_devs = 0;
	while (f && nbt_devs < 40 && fgets(line, sizeof(line), f)) {
		struct bt_dev *d = &bt_devs[nbt_devs];
		char *fld[6], *p = line;
		int n = 0;

		line[strcspn(line, "\n")] = 0;
		fld[n++] = p;
		while (n < 6 && (p = strchr(p, '\t'))) {
			*p++ = 0;
			fld[n++] = p;
		}
		if (n < 6)
			continue;
		snprintf(d->addr, sizeof(d->addr), "%s", fld[0]);
		snprintf(d->name, sizeof(d->name), "%s", fld[1][0] ? fld[1] : fld[0]);
		d->paired = fld[2][0] == '1';
		d->connected = fld[3][0] == '1';
		d->rssi = atoi(fld[4]);
		snprintf(d->icon, sizeof(d->icon), "%s", fld[5]);
		nbt_devs++;
	}
	if (f)
		fclose(f);
}

static const char *bt_kind(const char *icon)
{
	return !strncmp(icon, "audio", 5) ? "Audio" :
	       !strcmp(icon, "input-keyboard") ? "Keyboard" :
	       !strcmp(icon, "input-mouse") ? "Mouse" :
	       !strcmp(icon, "input-gaming") ? "Game controller" :
	       !strcmp(icon, "phone") ? "Phone" :
	       !strcmp(icon, "computer") ? "Computer" : "Device";
}

static void bt_said(struct ui *ui, char *reply)
{
	if (strncmp(reply, "ok", 2) != 0)
		say("%s", ctl_msg(reply));
}

static void bt_pair_done(struct ui *ui, char *reply)
{
	if (!strncmp(reply, "ok", 2)) {
		say("Paired with %s", ctl_msg(reply));
		page = P_DEVICES;
	} else {
		say("Could not pair: %s", ctl_msg(reply));
	}
}

static void bt_forget_done(struct ui *ui, char *reply)
{
	if (!strncmp(reply, "ok", 2)) {
		say("Forgotten");
		page = P_DEVICES;
	} else {
		say("%s", ctl_msg(reply));
	}
}

/* Right away, beside a request in flight (the pairing that asks for it) */
static void bt_now(const char *cmd)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	char buf[128];
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	snprintf(a.sun_path, sizeof(a.sun_path), "%s", BT_CTL);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
	    write(fd, cmd, strlen(cmd)) < 0 || write(fd, "\n", 1) < 0) {
		say("s22-btd is not running");
	} else {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = 0;
			if (strncmp(buf, "ok", 2) != 0)
				say("%s", ctl_msg(buf));
		}
	}
	if (fd >= 0)
		close(fd);
}

static void on_bt_files(struct ui *ui, int fd)
{
	char buf[4096];

	while (read(fd, buf, sizeof(buf)) > 0)
		;
	ui_redraw(ui);
}

static void mv_country_done(struct ui *ui, char *reply)
{
	if (!strncmp(reply, "ok", 2)) {
		say("Mullvad server: %s", ctl_msg(reply));
		page = P_VPN;
	} else {
		say("%s", ctl_msg(reply));
	}
}

/* ------------------------------------------------- wpa_supplicant --- */

/*
 * Two datagram sockets to wpa_supplicant's control interface: one for
 * requests and their replies, one ATTACHed for events (scan results in,
 * connected, wrong key), watched in the main loop.
 */
static int wpa_req = -1, wpa_ev = -1;
static char wpa_iface[32];
static char wpa_local[2][108];

static int wpa_socket(int which)
{
	struct sockaddr_un local = { .sun_family = AF_UNIX };
	struct sockaddr_un remote = { .sun_family = AF_UNIX };
	const char *dir = getenv("XDG_RUNTIME_DIR");
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	snprintf(wpa_local[which], sizeof(wpa_local[which]), "%s/s22-settings-wpa-%d-%d",
		 dir ? dir : "/tmp", (int)getpid(), which);
	snprintf(local.sun_path, sizeof(local.sun_path), "%s", wpa_local[which]);
	unlink(local.sun_path);
	snprintf(remote.sun_path, sizeof(remote.sun_path), "%s/%s", WPA_DIR, wpa_iface);
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0 ||
	    connect(fd, (struct sockaddr *)&remote, sizeof(remote)) < 0) {
		unlink(wpa_local[which]);
		wpa_local[which][0] = 0;
		close(fd);
		return -1;
	}
	return fd;
}

static void wpa_cleanup(void)
{
	for (int i = 0; i < 2; i++)
		if (wpa_local[i][0])
			unlink(wpa_local[i]);
}

/* A request and its reply (NUL-terminated); the reply's length or -1 */
static int wpa_cmd(const char *cmd, char *reply, int len)
{
	struct pollfd p = { .fd = wpa_req, .events = POLLIN };
	char dummy[16];

	if (reply == NULL) {
		reply = dummy;
		len = sizeof(dummy);
	}
	reply[0] = 0;
	if (wpa_req < 0 || send(wpa_req, cmd, strlen(cmd), 0) < 0)
		return -1;
	if (poll(&p, 1, 4000) <= 0)
		return -1;
	ssize_t n = recv(wpa_req, reply, len - 1, 0);
	if (n < 0)
		return -1;
	reply[n] = 0;
	return n;
}

static bool wpa_ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static bool wpa_ok(const char *fmt, ...)
{
	char cmd[512], rep[64];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return wpa_cmd(cmd, rep, sizeof(rep)) > 0 && strncmp(rep, "OK", 2) == 0;
}

struct net {
	char ssid[64];
	int dbm;
	bool secure, sae_only;
	int saved_id;		/* -1 if not saved */
	bool current;
};
static struct net nets[40];
static int nnets;

struct saved {
	int id;
	char ssid[64];
	bool current;
	bool private_mac;	/* joins with an address of its own (D076) */
};
static struct saved saved[32];
static int nsaved;

static char wifi_state[32], wifi_ssid[64], wifi_ip[48];
static int sel_saved = -1;		/* P_WIFI_NET: index into saved[] */
static struct net pass_net;		/* P_WIFI_PASS: the network */
static struct ui_entry pass_entry;
static int pending_id = -1;		/* a connection in progress */
static bool pending_new;		/* added by us: save on success */
static time_t pending_since;
static char pending_ssid[64];

/* "\xNN" escapes in wpa_supplicant's SSID output back to bytes */
static void unescape_ssid(const char *in, char *out, int len)
{
	int n = 0;

	for (; *in && n < len - 1; in++) {
		unsigned v;
		if (in[0] == '\\' && in[1] == 'x' && sscanf(in + 2, "%2x", &v) == 1) {
			out[n++] = (char)v;
			in += 3;
		} else if (in[0] == '\\' && in[1]) {
			out[n++] = *++in;
		} else {
			out[n++] = *in;
		}
	}
	out[n] = 0;
}

static int signal_bars(int dbm)
{
	return dbm >= -55 ? 4 : dbm >= -65 ? 3 : dbm >= -75 ? 2 : dbm >= -85 ? 1 : 0;
}

static int net_cmp(const void *a, const void *b)
{
	const struct net *x = a, *y = b;

	if (x->current != y->current)
		return y->current - x->current;
	if ((x->saved_id >= 0) != (y->saved_id >= 0))
		return (y->saved_id >= 0) - (x->saved_id >= 0);
	return y->dbm - x->dbm;
}

static void wpa_refresh(void)
{
	static char buf[16384];
	char *line, *save;

	wifi_state[0] = wifi_ssid[0] = wifi_ip[0] = 0;
	nsaved = nnets = 0;
	if (wpa_req < 0)
		return;
	if (wpa_cmd("STATUS", buf, sizeof(buf)) > 0)
		for (line = strtok_r(buf, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			if (!strncmp(line, "wpa_state=", 10))
				snprintf(wifi_state, sizeof(wifi_state), "%s", line + 10);
			else if (!strncmp(line, "ssid=", 5))
				unescape_ssid(line + 5, wifi_ssid, sizeof(wifi_ssid));
			else if (!strncmp(line, "ip_address=", 11))
				snprintf(wifi_ip, sizeof(wifi_ip), "%s", line + 11);
		}
	if (strcmp(wifi_state, "COMPLETED") != 0)
		wifi_ssid[0] = 0;
	/* network id / ssid / bssid / flags */
	if (wpa_cmd("LIST_NETWORKS", buf, sizeof(buf)) > 0) {
		line = strtok_r(buf, "\n", &save);	/* the column names */
		while ((line = strtok_r(NULL, "\n", &save)) && nsaved < 32) {
			char *f[4] = { line };
			int k = 1;
			for (char *p = line; *p && k < 4; p++)
				if (*p == '\t') {
					*p = 0;
					f[k++] = p + 1;
				}
			if (k < 2)
				continue;
			struct saved *s = &saved[nsaved++];
			s->id = atoi(f[0]);
			unescape_ssid(f[1], s->ssid, sizeof(s->ssid));
			s->current = k > 3 && strstr(f[3], "[CURRENT]");
		}
		for (int i = 0; i < nsaved; i++) {
			char cmd[48], rep[16];
			snprintf(cmd, sizeof(cmd), "GET_NETWORK %d mac_addr", saved[i].id);
			saved[i].private_mac = wpa_cmd(cmd, rep, sizeof(rep)) > 0 && atoi(rep) == 3;
		}
	}
	/* bssid / frequency / signal level / flags / ssid */
	if (wpa_cmd("SCAN_RESULTS", buf, sizeof(buf)) > 0) {
		line = strtok_r(buf, "\n", &save);
		while ((line = strtok_r(NULL, "\n", &save))) {
			char *f[5] = { line };
			int k = 1;
			for (char *p = line; *p && k < 5; p++)
				if (*p == '\t') {
					*p = 0;
					f[k++] = p + 1;
				}
			if (k < 5 || !f[4][0])
				continue;		/* hidden networks */
			char ssid[64];
			unescape_ssid(f[4], ssid, sizeof(ssid));
			int dbm = atoi(f[2]), i;
			for (i = 0; i < nnets; i++)
				if (!strcmp(nets[i].ssid, ssid))
					break;
			if (i == nnets) {
				if (nnets == 40)
					continue;
				memset(&nets[nnets], 0, sizeof(nets[0]));
				snprintf(nets[nnets].ssid, sizeof(nets[0].ssid), "%s", ssid);
				nets[nnets].dbm = -200;
				nets[nnets].saved_id = -1;
				nnets++;
			}
			struct net *n = &nets[i];
			if (dbm > n->dbm)
				n->dbm = dbm;
			n->secure = strstr(f[3], "WPA") || strstr(f[3], "RSN") ||
				    strstr(f[3], "WEP") || strstr(f[3], "SAE");
			n->sae_only = strstr(f[3], "SAE") && !strstr(f[3], "PSK");
		}
	}
	for (int i = 0; i < nnets; i++)
		for (int j = 0; j < nsaved; j++)
			if (!strcmp(nets[i].ssid, saved[j].ssid)) {
				nets[i].saved_id = saved[j].id;
				nets[i].current = saved[j].current && wifi_ssid[0];
			}
	qsort(nets, nnets, sizeof(nets[0]), net_cmp);
}

static void wpa_scan(void)
{
	if (radio_on(RFKILL_TYPE_WLAN) == 1)
		wpa_cmd("SCAN", NULL, 0);
}

/* The connection attempt ended: keep a new network only if it worked */
static void pending_end(bool connected)
{
	if (pending_id < 0)
		return;
	if (connected) {
		wpa_ok("ENABLE_NETWORK all");
		if (pending_new)
			wpa_ok("SAVE_CONFIG");
		say("Connected to %s", pending_ssid);
	} else {
		if (pending_new)
			wpa_ok("REMOVE_NETWORK %d", pending_id);
		wpa_ok("ENABLE_NETWORK all");
	}
	pending_id = -1;
}

static void on_wpa_event(struct ui *ui, int fd)
{
	char ev[512];
	ssize_t n;

	while ((n = recv(fd, ev, sizeof(ev) - 1, MSG_DONTWAIT)) > 0) {
		ev[n] = 0;
		if (strstr(ev, "CTRL-EVENT-CONNECTED")) {
			pending_end(true);
		} else if (strstr(ev, "reason=WRONG_KEY") ||
			   strstr(ev, "CTRL-EVENT-AUTH-REJECT")) {
			if (pending_id >= 0) {
				say("Wrong password for %s", pending_ssid);
				pending_end(false);
			}
		}
		/* scan results, (dis)connections: just look again */
	}
	wpa_refresh();
	ui_redraw(ui);
}

static bool wpa_open(struct ui *ui)
{
	struct dirent *de;
	DIR *d = opendir(WPA_DIR);

	if (wpa_req >= 0)
		return true;
	wpa_iface[0] = 0;
	while (d && (de = readdir(d)))
		if (de->d_name[0] != '.' && !strncmp(de->d_name, "wl", 2)) {
			snprintf(wpa_iface, sizeof(wpa_iface), "%s", de->d_name);
			break;
		}
	if (d)
		closedir(d);
	if (!wpa_iface[0])
		return false;
	wpa_req = wpa_socket(0);
	wpa_ev = wpa_socket(1);
	if (wpa_req < 0 || wpa_ev < 0) {
		if (wpa_req >= 0)
			close(wpa_req);
		if (wpa_ev >= 0)
			close(wpa_ev);
		wpa_req = wpa_ev = -1;
		wpa_cleanup();
		return false;
	}
	char rep[16];
	struct pollfd p = { .fd = wpa_ev, .events = POLLIN };
	if (send(wpa_ev, "ATTACH", 6, 0) == 6 && poll(&p, 1, 2000) > 0 &&
	    recv(wpa_ev, rep, sizeof(rep), 0) > 0)
		ui_watch(ui, wpa_ev, on_wpa_event);
	return true;
}

/* SSID as hex: any bytes, no quoting */
static void ssid_hex(const char *ssid, char *out, int len)
{
	int n = 0;

	for (const unsigned char *p = (const unsigned char *)ssid; *p && n < len - 3; p++)
		n += snprintf(out + n, len - n, "%02x", *p);
	out[n] = 0;
}

static void connect_start(int id, bool is_new, const char *ssid)
{
	if (!wpa_ok("SELECT_NETWORK %d", id)) {
		say("Cannot connect");
		if (is_new)
			wpa_ok("REMOVE_NETWORK %d", id);
		return;
	}
	pending_id = id;
	pending_new = is_new;
	pending_since = time(NULL);
	snprintf(pending_ssid, sizeof(pending_ssid), "%s", ssid);
	say("Connecting to %s…", ssid);
}

/*
 * A private address (D076): the network sees a random address of its own,
 * not this phone's. wpa_supplicant's mac_addr 3 with mac_value: the same
 * address every time it joins that network (a captive portal knows it
 * again), a new one only when the switch is turned on again. Locally
 * administered, unicast.
 */
static bool set_private_mac(int id, bool on)
{
	unsigned char m[6];
	int fd;
	bool got;

	if (!on)
		return wpa_ok("SET_NETWORK %d mac_addr 0", id);
	fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	got = fd >= 0 && read(fd, m, sizeof(m)) == sizeof(m);
	if (fd >= 0)
		close(fd);
	if (!got)
		return false;
	m[0] = (m[0] & 0xfc) | 0x02;
	return wpa_ok("SET_NETWORK %d mac_addr 3", id) &&
	       wpa_ok("SET_NETWORK %d mac_value %02x:%02x:%02x:%02x:%02x:%02x", id,
		      m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* A network not saved yet: open, or with the password typed. An open one
 * is a public one: it gets a private address */
static void connect_new(const struct net *n, const char *password)
{
	char rep[32], hex[132];
	int id;

	if (password && strchr(password, '"')) {
		say("Passwords with \" are not supported yet");
		return;
	}
	if (password && (strlen(password) < 8 || strlen(password) > 63)) {
		say("A Wi-Fi password has 8 to 63 characters");
		return;
	}
	if (wpa_cmd("ADD_NETWORK", rep, sizeof(rep)) <= 0 || rep[0] < '0' || rep[0] > '9') {
		say("Cannot add the network");
		return;
	}
	id = atoi(rep);
	ssid_hex(n->ssid, hex, sizeof(hex));
	bool ok = wpa_ok("SET_NETWORK %d ssid %s", id, hex);
	if (!n->secure) {
		ok = ok && wpa_ok("SET_NETWORK %d key_mgmt NONE", id) &&
		     set_private_mac(id, true);
	} else if (n->sae_only) {
		ok = ok && wpa_ok("SET_NETWORK %d key_mgmt SAE", id) &&
		     wpa_ok("SET_NETWORK %d ieee80211w 2", id) &&
		     wpa_ok("SET_NETWORK %d sae_password \"%s\"", id, password);
	} else {
		ok = ok && wpa_ok("SET_NETWORK %d psk \"%s\"", id, password);
	}
	if (!ok) {
		wpa_ok("REMOVE_NETWORK %d", id);
		say("Cannot set up the network");
		return;
	}
	connect_start(id, true, n->ssid);
}

static void forget(int idx)
{
	if (idx < 0 || idx >= nsaved)
		return;
	if (wpa_ok("REMOVE_NETWORK %d", saved[idx].id) && wpa_ok("SAVE_CONFIG"))
		say("Forgot %s", saved[idx].ssid);
	else
		say("Cannot forget the network");
	wpa_refresh();
}

/* ----------------------------------------------------- ping tests --- */

#define PING_HOST	"google.com"

struct ping {
	const char *label;
	bool hold;		/* hold the cellular link (s22-celld) for it */
	bool mic;		/* not a ping: the call microphone (s22-mictest) */
	char iface[32];		/* "" if there is no such link */
	int fd;			/* the running ping's output, or -1 */
	pid_t pid;
	char out[2048];
	int len;
	char result[96];
};
#define NPINGS	3
static struct ping pings[NPINGS] = {
	{ .label = "Ping over Wi-Fi", .fd = -1 },
	{ .label = "Ping over mobile data", .hold = true, .fd = -1 },
	{ .label = "Test the call microphone", .mic = true, .fd = -1 },
};

/* The call microphone's gains (s22-calld, s22-mictest) */
static const struct choice mic_adc = { "call_mic_adc",
	{ "0", "1", "2", "3", "4", "5" },
	{ "0 dB", "+6 dB", "+12 dB", "+18 dB", "+24 dB", "+30 dB" }, 4 };
static const struct choice mic_digital = { "call_mic_digital",
	{ "84", "86", "88", "90", "92", "94", "96" },
	{ "0 dB", "+2 dB", "+4 dB", "+6 dB", "+8 dB", "+10 dB", "+12 dB" }, 2 };

/* Which interface each test goes out of; the mobile one learns its own */
static void ping_ifaces(void)
{
	snprintf(pings[0].iface, sizeof(pings[0].iface), "%s", wpa_iface[0] ?
		 wpa_iface : "wlan0");
	snprintf(pings[1].iface, sizeof(pings[1].iface), "%s",
		 access(CELL_CTL, F_OK) == 0 ? "(held)" : "");
	snprintf(pings[2].iface, sizeof(pings[2].iface), "mic");
}

/*
 * In the ping's child: ask s22-celld for the cellular link and keep the
 * connection (the hold) open across exec, so it lasts as long as ping.
 * Fills iface; false (with the reason on stdout) if there is no link.
 */
static bool cell_hold(char *iface, int len)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	struct timeval tv = { .tv_sec = 90 };
	char buf[96];
	ssize_t n;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);	/* no CLOEXEC */

	snprintf(a.sun_path, sizeof(a.sun_path), "%s", CELL_CTL);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		printf("s22-celld is not running\n");
		return false;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (write(fd, "hold settings-ping\n", 19) != 19 ||
	    (n = read(fd, buf, sizeof(buf) - 1)) <= 0) {
		printf("no answer from s22-celld\n");
		return false;
	}
	buf[n] = 0;
	buf[strcspn(buf, "\n")] = 0;
	if (strncmp(buf, "ok ", 3) != 0) {
		printf("%s\n", strncmp(buf, "error ", 6) == 0 ? buf + 6 : buf);
		return false;
	}
	snprintf(iface, len, "%s", buf + 3);
	return true;
}

static void ping_done(struct ping *p)
{
	int sent = 0, got = 0;
	double mn, avg, mx;
	char *s;

	p->out[p->len] = 0;
	if ((s = strstr(p->out, " packets transmitted, "))) {
		while (s > p->out && s[-1] != '\n')
			s--;
		sscanf(s, "%d packets transmitted, %d", &sent, &got);
	}
	s = strstr(p->out, "rtt min/avg/max/mdev = ");
	if (got > 0 && s && sscanf(s + 23, "%lf/%lf/%lf", &mn, &avg, &mx) == 3)
		snprintf(p->result, sizeof(p->result), "%d/%d replies, %.0f ms average (%.0f–%.0f)",
			 got, sent, avg, mn, mx);
	else if (sent > 0)
		snprintf(p->result, sizeof(p->result), "No replies (%d sent)", sent);
	else {
		/* ping's own error, e.g. no route or an unknown host */
		char *nl = strchr(p->out, '\n');
		if (nl)
			*nl = 0;
		snprintf(p->result, sizeof(p->result), "Failed: %s",
			 p->len ? p->out : "no output");
	}
}

/* The last whole line of what came so far */
static void last_line(struct ping *p)
{
	char *end = p->out + p->len, *start;

	while (end > p->out && (end[-1] == '\n' || end[-1] == ' '))
		end--;
	for (start = end; start > p->out && start[-1] != '\n'; start--)
		;
	if (end > start)
		snprintf(p->result, sizeof(p->result), "%.*s", (int)(end - start), start);
}

static void on_ping(struct ui *ui, int fd)
{
	for (int i = 0; i < NPINGS; i++) {
		struct ping *p = &pings[i];
		if (p->fd != fd)
			continue;
		ssize_t n = read(fd, p->out + p->len, sizeof(p->out) - 1 - p->len);
		if (n > 0) {
			p->len += n;
			p->out[p->len] = 0;
			if (p->mic)		/* it says what it is doing */
				last_line(p);
			if (p->len < (int)sizeof(p->out) - 1) {
				ui_redraw(ui);
				return;
			}
		}
		ui_unwatch(ui, fd);
		close(fd);
		p->fd = -1;
		waitpid(p->pid, NULL, 0);
		if (p->mic)
			last_line(p);
		else
			ping_done(p);
	}
	ui_redraw(ui);
}

static void ping_start(struct ui *ui, struct ping *p)
{
	int pipefd[2];

	if (p->fd >= 0 || !p->iface[0] || pipe2(pipefd, O_CLOEXEC) < 0)
		return;
	p->pid = fork();
	if (p->pid == 0) {
		char iface[32];
		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		snprintf(iface, sizeof(iface), "%s", p->iface);
		if (p->mic) {
			execlp("s22-mictest", "s22-mictest", (char *)NULL);
			_exit(127);
		}
		if (p->hold && !cell_hold(iface, sizeof(iface))) {
			fflush(stdout);
			_exit(1);
		}
		/* The carrier's network is IPv6-only (DNS64 covers the rest) */
		if (p->hold)
			execlp("ping", "ping", "-6", "-n", "-I", iface, "-c", "4",
			       "-W", "2", "-i", "0.5", PING_HOST, (char *)NULL);
		else
			execlp("ping", "ping", "-n", "-I", iface, "-c", "4", "-W", "2",
			       "-i", "0.5", PING_HOST, (char *)NULL);
		_exit(127);
	}
	close(pipefd[1]);
	if (p->pid < 0) {
		close(pipefd[0]);
		return;
	}
	p->fd = pipefd[0];
	p->len = 0;
	if (p->mic)
		snprintf(p->result, sizeof(p->result), "Recording: speak as on a call");
	else
		snprintf(p->result, sizeof(p->result), "%s %s…", p->hold ?
			 "Connecting, then pinging" : "Pinging", PING_HOST);
	ui_watch(ui, p->fd, on_ping);
}

/* -------------------------------------------------------- the pages --- */

enum row_kind {
	R_CHOICE, R_BRIGHTNESS, R_WIFI, R_BT, R_PAGE, R_INFO, R_RUN,
	R_LEVEL,	/* a 0-15 volume: arg = index into levels[] */
	R_TONE,		/* a sound setting: arg = index into tones[] */
	R_TONE_PICK,	/* in the picker: arg -2 default, -1 silent, else a file */
	R_VIB,		/* a vibration setting: arg = index into vibs[] */
	R_VIB_PICK,	/* in its picker: arg VIB_* or a preset's index */
	R_LOCK,		/* the screen lock: on (a PIN) or off */
	R_PIN_CHANGE,
	R_LOCK_NOW,
	R_NET,		/* a scanned network: arg = index into nets[] */
	R_SAVED,	/* a saved network: arg = index into saved[] */
	R_CONNECT,	/* connect to saved[sel_saved] */
	R_FORGET,	/* forget saved[sel_saved] */
	R_PRIVATE,	/* saved[sel_saved]'s private address, on or off */
	R_SCAN,		/* scan again */
	R_PING,		/* ping test: arg = index into pings[] */
	R_ACTION,	/* arg = enum action */
	R_MV_DEVICE,	/* arg = index into mv_devs[] */
	R_MV_COUNTRY,	/* arg = index into mv_countries[] */
	R_BT_DEV,	/* a paired device: arg = index into bt_devs[] */
	R_BT_FOUND,	/* a device found by the scan: arg = index */
	R_BT_CONN,	/* bt_sel: arg 1 connect, 0 disconnect */
	R_BT_CONFIRM,	/* the pairing code: arg 1 matches, 0 not */
	R_SHARE,	/* file sharing on or off */
	R_SAY,		/* a sample of the speech voice */
	R_VOICE,	/* the speech voice: opens P_VOICE */
	R_VOICE_PICK,	/* in it: arg = index into voices[] */
	R_WALL,		/* a wallpaper: arg WALL_*, chosen in Photos */
};

struct row {
	enum row_kind kind;
	const char *label;
	const struct choice *choice;
	enum page to;
	const char *app;	/* R_RUN: the app_id to open */
	int arg;
	int bars;		/* R_NET: 0-4, -1 none */
	char value[96];
};

#define MAX_ROWS	256		/* the sound picker has a hundred */
static struct row rows[MAX_ROWS];
static int nrows;

static struct row *add(enum row_kind kind, const char *label)
{
	struct row *r = &rows[nrows < MAX_ROWS - 1 ? nrows++ : MAX_ROWS - 1];

	memset(r, 0, sizeof(*r));
	r->kind = kind;
	r->label = label;
	r->bars = -1;
	return r;
}

static void info(const char *label, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
static void info(const char *label, const char *fmt, ...)
{
	struct row *r = add(R_INFO, label);
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(r->value, sizeof(r->value), fmt, ap);
	va_end(ap);
}

static void page_row(const char *label, enum page to, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
static void page_row(const char *label, enum page to, const char *fmt, ...)
{
	struct row *r = add(R_PAGE, label);
	va_list ap;

	r->to = to;
	va_start(ap, fmt);
	vsnprintf(r->value, sizeof(r->value), fmt, ap);
	va_end(ap);
}

static const char *wifi_summary(void)
{
	static char s[96];
	int on = radio_on(RFKILL_TYPE_WLAN);

	snprintf(s, sizeof(s), "%s", on < 0 ? "Not found" : !on ? "Off" :
		 wifi_ssid[0] ? wifi_ssid : "On, not connected");
	return s;
}

static const char *cell_summary(void)
{
	static char s[96];
	char service[16], op[64], tech[16];

	cell_get("service", service, sizeof(service));
	cell_get("operator", op, sizeof(op));
	cell_get("tech", tech, sizeof(tech));
	if (choice_get(&cellular) == 1) {
		snprintf(s, sizeof(s), "Cellular off");
		return s;
	}
	for (char *p = tech; *p; p++)	/* "lte" -> "LTE" */
		if (*p >= 'a' && *p <= 'z')
			*p -= 32;
	if (!service[0])
		snprintf(s, sizeof(s), "Status unknown (s22-celld)");
	else if (!strcmp(service, "full"))
		snprintf(s, sizeof(s), "%s%s%s", op[0] ? op : "Connected",
			 tech[0] ? ", " : "", tech[0] ? tech : "");
	else if (!strcmp(service, "limited"))
		snprintf(s, sizeof(s), "Emergency calls only");
	else
		snprintf(s, sizeof(s), "No service");
	return s;
}

static const char *tailscale_summary(void)
{
	static char s[160];
	char ts[24], name[96], ip[48];

	vpn_get("tailscale", ts, sizeof(ts));
	vpn_get("tailscale_name", name, sizeof(name));
	vpn_get("tailscale_ip", ip, sizeof(ip));
	name[strcspn(name, ".")] = 0;		/* the machine name only */
	snprintf(s, sizeof(s), "%s", !ts[0] ? "Status unknown (s22-vpnd)" :
		 !strcmp(ts, "none") ? "Not running" :
		 !strcmp(ts, "NeedsLogin") ? "Needs a login" :
		 !strcmp(ts, "NeedsMachineAuth") ? "Waiting for the admin's approval" :
		 !strcmp(ts, "Starting") ? "Connecting…" :
		 !strcmp(ts, "Stopped") ? "Stopped" : "");
	if (!strcmp(ts, "Running"))
		snprintf(s, sizeof(s), "Connected%s%s%s%s", name[0] ? " as " : "",
			 name, ip[0] ? ", " : "", ip);
	return s;
}

static const char *mullvad_summary(void)
{
	static char s[128];
	char m[16], ep[64], hs[16];

	vpn_get("mullvad", m, sizeof(m));
	vpn_get("mullvad_endpoint", ep, sizeof(ep));
	vpn_get("mullvad_handshake", hs, sizeof(hs));
	ep[strcspn(ep, ":")] = 0;		/* the server, not the port */
	if (!strcmp(m, "up"))
		snprintf(s, sizeof(s), "Connected to %s", ep);
	else
		snprintf(s, sizeof(s), "%s", !m[0] ? "Status unknown (s22-vpnd)" :
			 !strcmp(m, "none") ? "Not set up: no /etc/wireguard/mullvad.conf" :
			 !strcmp(m, "off") ? "Off" :
			 !strcmp(m, "connecting") ? "Connecting…" :
			 "Could not start the tunnel");
	return s;
}

/* ---------------------------------------------------------- sounds --- */

#define DEFAULT_RINGTONE	UI_SOUNDS "/ringtones/Ring_Synth_04.ogg"
#define DEFAULT_NOTIFY		UI_SOUNDS "/notifications/pixiedust.ogg"

struct level {
	const char *key;
	int def;
};
static const struct level levels[] = {
	{ "ring_volume", 12 },		/* s22-play's */
	{ "volume", 15 },		/* the Media output's (s22-wm's keys) */
	{ "vib_strength", 15 },		/* s22-vibrate's, s22-notifyd's, s22-calld's */
};

/* A sound setting: the key, the kind of sound, and what "Default" is */
struct tone {
	char label[64], key[48];
	const char *kind, *def;		/* def NULL: no "Default" choice */
};
static struct tone tones[16];
static int ntones;
static int tone_cur;			/* P_TONE: which tones[] */

static int level_get(int i)
{
	char v[16];
	int n = atoi(ui_setting(levels[i].key, "", v, sizeof(v))[0] ? v : "-1");

	return n < 0 || n > 15 ? levels[i].def : n;
}

static void level_step(int i, int d)
{
	char v[16], cmd[96];
	int n = level_get(i) + d;

	n = n < 0 ? 0 : n > 15 ? 15 : n;
	snprintf(v, sizeof(v), "%d", n);
	ui_setting_set(levels[i].key, v);
	if (i == 2) {			/* feel it */
		ui_vibrate("300");
	} else if (i == 0) {		/* hear it: the notification sound */
		char t[160];
		ui_setting("notify_sound", DEFAULT_NOTIFY, t, sizeof(t));
		ui_sound_preview(t[0] == '/' ? t : DEFAULT_NOTIFY);
	} else {
		snprintf(cmd, sizeof(cmd), "wpctl set-volume @DEFAULT_AUDIO_SINK@ %.3f",
			 n / 15.0);
		ui_spawn(cmd);
	}
}

/* The settings' sound rows: ringtone, default, one an app that notifies */
static void tones_load(void)
{
	struct ui_appinfo apps[64];
	int n = ui_apps(apps, 64);

	ntones = 0;
	tones[ntones++] = (struct tone){ "Ringtone", "ringtone", "ringtones", NULL };
	tones[ntones++] = (struct tone){ "Default notification sound", "notify_sound",
					 "notifications", NULL };
	for (int i = 0; i < n && ntones < 16; i++) {
		if (!apps[i].notify[0])
			continue;
		struct tone *t = &tones[ntones++];
		snprintf(t->label, sizeof(t->label), "%s", apps[i].name);
		snprintf(t->key, sizeof(t->key), "sound_%s", apps[i].notify);
		t->kind = "notifications";
		t->def = "default";
	}
}

/* What a sound setting holds: a file, "silent", or "default" */
static const char *tone_value(const struct tone *t, char *buf, int len)
{
	ui_setting(t->key, "", buf, len);
	if (!buf[0])
		snprintf(buf, len, "%s", t->def ? t->def :
			 !strcmp(t->kind, "ringtones") ? DEFAULT_RINGTONE : DEFAULT_NOTIFY);
	return buf;
}

static char (*tone_files)[160];		/* P_TONE's sounds, from build() */

/* A vibration setting: the key, ringing or not, a "Default" choice */
struct vib {
	char label[64], key[48];
	bool ring, def;
};
static struct vib vibs[16];
static int nvibs, vib_cur;
enum { VIB_RECORD = -3, VIB_DEFAULT = -2, VIB_NONE = -1, VIB_CUSTOM = 100 };

static void vibs_load(void)
{
	struct ui_appinfo apps[64];
	int n = ui_apps(apps, 64);

	nvibs = 0;
	vibs[nvibs++] = (struct vib){ "Ringing vibration", "vib_ring", true, false };
	vibs[nvibs++] = (struct vib){ "Default notification vibration", "vib_notify",
				      false, false };
	for (int i = 0; i < n && nvibs < 16; i++) {
		if (!apps[i].notify[0])
			continue;
		struct vib *v = &vibs[nvibs++];
		snprintf(v->label, sizeof(v->label), "%s vibration", apps[i].name);
		snprintf(v->key, sizeof(v->key), "vib_%s", apps[i].notify);
		v->ring = false;
		v->def = true;
	}
}

static const char *vib_value(const struct vib *v, char *buf, int len)
{
	ui_setting(v->key, "", buf, len);
	if (!buf[0])
		snprintf(buf, len, "%s", v->def ? "default" : v->ring ? "900,600" :
			 "200,150,200");
	return buf;
}

/* What a picker row stands for: a pattern, "default" or "none" */
static const char *vib_pick(int arg, char *buf, int len)
{
	const struct ui_vib *p;
	char customs[UI_VIB_CUSTOMS][96];
	int n;

	buf[0] = 0;
	if (arg == VIB_DEFAULT)
		snprintf(buf, len, "default");
	else if (arg == VIB_NONE)
		snprintf(buf, len, "none");
	else if (arg >= VIB_CUSTOM) {
		ui_vib_customs(customs);
		snprintf(buf, len, "%s", customs[arg - VIB_CUSTOM]);
	} else if (arg >= 0) {
		p = ui_vib_presets(vibs[vib_cur].ring, &n);
		if (arg < n)
			snprintf(buf, len, "%s", p[arg].pattern);
	}
	return buf;
}

/* A ringing pattern felt twice round */
static void vib_feel(const char *pattern, bool ring)
{
	char twice[200];

	if (ring && pattern[0] && strcmp(pattern, "none") && strcmp(pattern, "default")) {
		snprintf(twice, sizeof(twice), "%s,%s", pattern, pattern);
		ui_vibrate(twice);
	} else {
		ui_vibrate(pattern);
	}
}

/* The recorder: OK held is on, let go is off, up to 16 steps */
static int rec_steps[16], rec_n;
static bool rec_down;
static long long rec_t;

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void rec_pattern(char *buf, int len)
{
	int o = 0;

	buf[0] = 0;
	for (int i = 0; i < rec_n && o < len - 8; i++)
		o += snprintf(buf + o, len - o, "%s%d", i ? "," : "", rec_steps[i]);
}

static void tone_preview(struct ui_list *l)
{
	char (*files)[160];
	int n = ui_sounds(tones[tone_cur].kind, &files), extra = tones[tone_cur].def ? 2 : 1;
	int i = l->sel - extra;

	if (i >= 0 && i < n)
		ui_sound_preview(files[i]);
	else
		ui_sound_stop();
}

static void build(void)
{
	struct row *r;
	int on;

	nrows = 0;
	switch (page) {
	case P_MAIN:
		page_row("Network & internet", P_NET, "%s", wifi_summary());
		on = radio_on(RFKILL_TYPE_BLUETOOTH);
		{
			char conn[8];
			bt_get("connected", conn, sizeof(conn));
			if (on == 1 && atoi(conn) > 0)
				page_row("Connected devices", P_DEVICES,
					 "Bluetooth on, %d connected", atoi(conn));
			else
				page_row("Connected devices", P_DEVICES, "Bluetooth %s",
					 on < 0 ? "not found" : on ? "on" : "off");
		}
		{
			char t[160];
			ui_setting("ringtone", DEFAULT_RINGTONE, t, sizeof(t));
			page_row("Sounds", P_SOUNDS, "%s, ringtone %s",
				 choice_get(&sounds_on) ? "Vibrate only" : "Sounds on",
				 ui_sound_name(t));
		}
		page_row("Security", P_SECURITY, "%s", ui_pin_set_up() ?
			 "Screen lock: PIN" : "No screen lock");
		page_row("Display", P_DISPLAY, "Brightness %d%%, %s theme",
			 brightness_pct(), theme.labels[choice_get(&theme)]);
		page_row("Outer screen", P_OUTER, "Status screen with the lid closed");
		page_row("Accessibility", P_ACCESS, "Speech: %s",
			 choice_get(&tts_calls) && choice_get(&tts_messages) ? "calls and texts" :
			 choice_get(&tts_calls) ? "calls" :
			 choice_get(&tts_messages) ? "texts" : "off");
		page_row("System", P_SYSTEM, "About the phone");
		page_row("Developer options", P_DEV, "Ping tests, call microphone, touch");
		break;
	case P_NET:
		page_row("Wi-Fi", P_WIFI, "%s", wifi_summary());
		page_row("Mobile network", P_CELL, "%s", cell_summary());
		{
			char m[16];
			vpn_get("mullvad", m, sizeof(m));
			page_row("VPN", P_VPN, "Tailscale %s, Mullvad %s",
				 !strncmp(tailscale_summary(), "Connected", 9) ? "on" : "off",
				 !strcmp(m, "up") ? "on" : !strcmp(m, "connecting") ?
				 "connecting" : "off");
		}
		if (share.on && share.addr[0])
			page_row("File sharing", P_SHARE, "On: \\\\%s\\home", share.addr);
		else
			page_row("File sharing", P_SHARE, "%s", !share.known ? "" :
				 share.on ? "On" : "Off");
		break;
	case P_SHARE:
		r = add(R_SHARE, "File sharing");
		snprintf(r->value, sizeof(r->value), "%s", !share.known ? "…" :
			 share.on ? "On" : "Off");
		if (share.on && share.addr[0]) {
			info("On Windows", "\\\\%s\\home", share.addr);
			info("On a Mac or Linux", "smb://%s/home", share.addr);
		} else if (share.on) {
			info("Address", "Connect to Wi-Fi to share");
		}
		info("User name", "micro");
		page_row(share.pass ? "Change the password" : "Set the password", P_SHARE_PASS,
			 "Your phone login password");
		info("What is shared", "Your home folder, to home and office networks only");
		break;
	case P_WIFI:
		r = add(R_WIFI, "Use Wi-Fi");
		on = radio_on(RFKILL_TYPE_WLAN);
		snprintf(r->value, sizeof(r->value), "%s",
			 on < 0 ? "No Wi-Fi hardware" : on ? "On" : "Off");
		if (on != 1)
			break;
		if (wpa_req < 0) {
			info("Networks", "wpa_supplicant is not running");
			break;
		}
		if (wifi_ssid[0])
			info("Connected", "%s%s%s", wifi_ssid, wifi_ip[0] ? ", " : "",
			     wifi_ip);
		page_row("Saved networks", P_WIFI_SAVED, "%d saved", nsaved);
		for (int i = 0; i < nnets && nrows < 60; i++) {
			r = add(R_NET, nets[i].ssid);
			r->arg = i;
			r->bars = signal_bars(nets[i].dbm);
			snprintf(r->value, sizeof(r->value), "%s%s",
				 nets[i].current ? "Connected" :
				 nets[i].saved_id >= 0 ? "Saved" :
				 nets[i].secure ? "Secured" : "Open",
				 pending_id >= 0 && !strcmp(nets[i].ssid, pending_ssid) ?
				 ", connecting…" : "");
		}
		r = add(R_SCAN, "Scan again");
		snprintf(r->value, sizeof(r->value), "%d networks found", nnets);
		break;
	case P_WIFI_SAVED:
		if (nsaved == 0)
			info("No saved networks", "Pick one from the Wi-Fi list");
		for (int i = 0; i < nsaved; i++) {
			r = add(R_SAVED, saved[i].ssid);
			r->arg = i;
			snprintf(r->value, sizeof(r->value), "%s",
				 saved[i].current && wifi_ssid[0] ? "Connected" : "Saved");
		}
		break;
	case P_WIFI_NET:
		if (sel_saved < 0 || sel_saved >= nsaved) {
			info("Network", "Not saved any more");
			break;
		}
		info("Network", "%s", saved[sel_saved].ssid);
		if (!(saved[sel_saved].current && wifi_ssid[0])) {
			r = add(R_CONNECT, "Connect");
			snprintf(r->value, sizeof(r->value), "Join this network now");
		}
		r = add(R_PRIVATE, "Private address");
		snprintf(r->value, sizeof(r->value), "%s", saved[sel_saved].private_mac ?
			 "On: it sees an address of its own" : "Off: it sees this phone's address");
		r = add(R_FORGET, "Forget");
		snprintf(r->value, sizeof(r->value), "Remove it and its password");
		break;
	case P_CELL: {
		char service[16], tech[16], bars[8], op[64], st[16] = "";
		char data[8], mode[16], iface[32], modem[32], apn[96], pdp[32];
		cell_get("service", service, sizeof(service));
		cell_get("tech", tech, sizeof(tech));
		cell_get("bars", bars, sizeof(bars));
		cell_get("operator", op, sizeof(op));
		r = add(R_CHOICE, "Cellular");
		r->choice = &cellular;
		info("Network", "%s", cell_summary());
		if (service[0])
			info("Signal", "%s of 4 bars", bars[0] ? bars : "0");
		int fd = open("/run/s22-imsd.state", O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			if (read(fd, st, sizeof(st) - 1) < 0)
				st[0] = 0;
			close(fd);
			st[strcspn(st, "\n")] = 0;
		}
		info("Calls and texts (VoLTE)", "%s",
		     strcmp(st, "up") == 0 ? "Ready" :
		     st[0] ? "Not connected" : "Service not running");
		r = add(R_CHOICE, "Mobile data");
		r->choice = &mobile_data;
		cell_get("data", data, sizeof(data));
		cell_get("data_mode", mode, sizeof(mode));
		cell_get("data_iface", iface, sizeof(iface));
		info("Data connection", "%s", strcmp(data, "up") != 0 ? "Not connected" :
		     strcmp(mode, "internet") == 0 ? "Connected, in use for internet" :
		     "Connected for one app only (MMS, ping test)");
		cell_get("apn", apn, sizeof(apn));
		cell_get("pdp", pdp, sizeof(pdp));
		info("Access point name (APN)", "%s%s%s", apn[0] ? apn : "Unknown",
		     pdp[0] ? ", " : "", !strcmp(pdp, "ipv4-or-ipv6") ? "IPv4v6" :
		     !strcmp(pdp, "ipv6") ? "IPv6" : !strcmp(pdp, "ipv4") ? "IPv4" : pdp);
		cell_get("modem", modem, sizeof(modem));
		info("Modem", "%s%s%s", modem[0] ? modem : "Unknown",
		     iface[0] ? ", data on " : "", iface);
		break;
	}
	case P_VPN: {
		char ts[24], m[16], dev[48], cc[8], relay[48];
		info("Tailscale", "%s", tailscale_summary());
		vpn_get("tailscale", ts, sizeof(ts));
		if (!strcmp(ts, "NeedsLogin")) {
			page_row("Log in with another phone", P_TS_QR, "Scan a QR code");
			page_row("Log in with an auth key", P_TS_KEY,
				 "A tskey-auth-… key from the admin console");
		} else if (!strcmp(ts, "Running")) {
			r = add(R_ACTION, "Log out of Tailscale");
			r->arg = A_TS_LOGOUT;
			snprintf(r->value, sizeof(r->value), "Leave the tailnet");
		}
		if (!strcmp(ts, "Running") || !strcmp(ts, "Stopped")) {
			r = add(R_CHOICE, "Tailscale with the lid closed");
			r->choice = &tailscale_lid;
		}
		vpn_get("mullvad", m, sizeof(m));
		if (!strcmp(m, "none")) {
			page_row("Log in to Mullvad", P_MV_ACCOUNT, "With your account number");
			break;
		}
		if (!m[0])
			break;
		r = add(R_CHOICE, "Mullvad");
		r->choice = &mullvad;
		info("Mullvad tunnel", "%s", mullvad_summary());
		vpn_get("mullvad_country", cc, sizeof(cc));
		vpn_get("mullvad_relay", relay, sizeof(relay));
		for (char *p = cc; *p; p++)
			*p = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
		page_row("Server location", P_MV_COUNTRY, "%s%s%s", cc,
			 relay[0] ? ", " : "", relay);
		vpn_get("mullvad_device", dev, sizeof(dev));
		r = add(R_ACTION, "Log out of Mullvad");
		r->arg = A_MV_LOGOUT;
		snprintf(r->value, sizeof(r->value), "Frees this phone's device%s%s%s",
			 dev[0] ? " (“" : "", dev, dev[0] ? "”)" : "");
		break;
	}
	case P_MV_DEVICES:
		info("The account has its maximum of devices",
		     "Remove one to make room for this phone");
		for (int i = 0; i < nmv_devs; i++) {
			r = add(R_MV_DEVICE, mv_devs[i].name);
			r->arg = i;
			snprintf(r->value, sizeof(r->value), "Added %s", mv_devs[i].created);
		}
		break;
	case P_MV_COUNTRY: {
		char cc[8];
		vpn_get("mullvad_country", cc, sizeof(cc));
		if (nmv_countries == 0)
			info("Server location", "%s", ctl_fd >= 0 ? "Loading the list…" :
			     "No list (no internet?)");
		for (int i = 0; i < nmv_countries; i++) {
			r = add(R_MV_COUNTRY, mv_countries[i].name);
			r->arg = i;
			snprintf(r->value, sizeof(r->value), "%s",
				 !strcmp(cc, mv_countries[i].code) ? "In use" : "");
		}
		break;
	}
	case P_DEVICES: {
		int paired = 0;
		r = add(R_BT, "Bluetooth");
		on = radio_on(RFKILL_TYPE_BLUETOOTH);
		snprintf(r->value, sizeof(r->value), "%s",
			 on < 0 ? "Not found" : on ? "On" : "Off");
		if (on != 1)
			break;
		page_row("Pair new device", P_BT_SCAN, "Find devices nearby");
		bt_load();
		for (int i = 0; i < nbt_devs && nrows < 60; i++) {
			struct bt_dev *d = &bt_devs[i];
			if (!d->paired)
				continue;
			r = add(R_BT_DEV, d->name);
			r->arg = i;
			snprintf(r->value, sizeof(r->value), "%s, %s", bt_kind(d->icon),
				 d->connected ? "connected" : "not connected");
			paired++;
		}
		if (!paired)
			info("Paired devices", "None yet");
		break;
	}
	case P_SOUNDS: {
		char v[160];
		r = add(R_CHOICE, "Sounds");
		r->choice = &sounds_on;
		r = add(R_LEVEL, "Ring and notification volume");
		r->arg = 0;
		r = add(R_LEVEL, "Media volume");
		r->arg = 1;
		tones_load();
		for (int i = 0; i < ntones && nrows < 60; i++) {
			r = add(R_TONE, tones[i].label);
			r->arg = i;
			snprintf(r->value, sizeof(r->value), "%s",
				 ui_sound_name(tone_value(&tones[i], v, sizeof(v))));
		}
		r = add(R_CHOICE, "Vibration");
		r->choice = &vibrate_on;
		r = add(R_LEVEL, "Vibration strength");
		r->arg = 2;
		vibs_load();
		for (int i = 0; i < nvibs && nrows < 60; i++) {
			r = add(R_VIB, vibs[i].label);
			r->arg = i;
			snprintf(r->value, sizeof(r->value), "%s",
				 ui_vib_name(vib_value(&vibs[i], v, sizeof(v)), vibs[i].ring));
		}
		break;
	}
	case P_VIB: {
		char v[96], customs[UI_VIB_CUSTOMS][96], w[96];
		const struct ui_vib *p;
		int n;
		vib_value(&vibs[vib_cur], v, sizeof(v));
		if (vibs[vib_cur].def) {
			r = add(R_VIB_PICK, "Default");
			r->arg = VIB_DEFAULT;
			snprintf(r->value, sizeof(r->value), "%s", !strcmp(v, "default") ?
				 "Chosen: as the default notification vibration" :
				 "As the default notification vibration");
		}
		r = add(R_VIB_PICK, "None");
		r->arg = VIB_NONE;
		snprintf(r->value, sizeof(r->value), "%s", !strcmp(v, "none") ? "Chosen" : "");
		p = ui_vib_presets(vibs[vib_cur].ring, &n);
		for (int i = 0; i < n; i++) {
			r = add(R_VIB_PICK, p[i].name);
			r->arg = i;
			snprintf(r->value, sizeof(r->value), "%s", !strcmp(v, p[i].pattern) ?
				 "Chosen" : "");
		}
		ui_vib_customs(customs);
		for (int i = 0; i < UI_VIB_CUSTOMS; i++) {
			static const char *names[] = { "Custom 1", "Custom 2", "Custom 3",
						       "Custom 4" };
			if (!customs[i][0])
				continue;
			r = add(R_VIB_PICK, names[i]);
			r->arg = VIB_CUSTOM + i;
			snprintf(r->value, sizeof(r->value), "%s", !strcmp(v, vib_pick(r->arg,
				 w, sizeof(w))) ? "Chosen (recorded)" : "Recorded");
		}
		r = add(R_VIB_PICK, "Record a new pattern");
		r->arg = VIB_RECORD;
		snprintf(r->value, sizeof(r->value), "Hold OK in the rhythm you want");
		break;
	}
	case P_TONE: {
		char (*files)[160], v[160];
		int n = ui_sounds(tones[tone_cur].kind, &files);
		tone_value(&tones[tone_cur], v, sizeof(v));
		if (tones[tone_cur].def) {
			r = add(R_TONE_PICK, "Default");
			r->arg = -2;
			snprintf(r->value, sizeof(r->value), "%s", !strcmp(v, "default") ?
				 "Chosen: as the default notification sound" :
				 "As the default notification sound");
		}
		r = add(R_TONE_PICK, "Silent");
		r->arg = -1;
		snprintf(r->value, sizeof(r->value), "%s", !strcmp(v, "silent") ? "Chosen" : "");
		tone_files = files;		/* names drawn from it (draw_row) */
		for (int i = 0; i < n && nrows < MAX_ROWS - 1; i++) {
			r = add(R_TONE_PICK, "");
			r->arg = i;
			snprintf(r->value, sizeof(r->value), "%s",
				 !strcmp(v, files[i]) ? "Chosen" : "");
		}
		break;
	}
	case P_ACCESS:
		r = add(R_CHOICE, "Say who is calling");
		r->choice = &tts_calls;
		r = add(R_CHOICE, "Read texts aloud");
		r->choice = &tts_messages;
		r = add(R_VOICE, "Voice");
		ui_setting("tts_voice", TTS_VOICE, r->value, sizeof(r->value));
		r = add(R_SAY, "Hear the voice");
		snprintf(r->value, sizeof(r->value), "At the ring volume");
		break;
	case P_VOICE: {
		char v[32];
		ui_setting("tts_voice", TTS_VOICE, v, sizeof(v));
		voices_load();
		if (!nvoices)
			info("Voices", "None: is bencmouth installed?");
		for (int i = 0; i < nvoices; i++) {
			r = add(R_VOICE_PICK, voices[i]);
			r->arg = i;
			snprintf(r->value, sizeof(r->value), "%s",
				 !strcmp(v, voices[i]) ? "Chosen" : "");
		}
		break;
	}
	case P_SECURITY: {
		bool on = ui_pin_set_up();
		r = add(R_LOCK, "Screen lock");
		snprintf(r->value, sizeof(r->value), "%s", on ? "PIN" : "None");
		if (on) {
			r = add(R_PIN_CHANGE, "Change PIN");
			snprintf(r->value, sizeof(r->value), "The current one first");
			r = add(R_CHOICE, "Lock");
			r->choice = &lock_after;
			r = add(R_LOCK_NOW, "Lock now");
			snprintf(r->value, sizeof(r->value), "Also when the display goes dark");
		}
		break;
	}
	case P_BT_DEV: {
		struct bt_dev *d = NULL;
		bt_load();
		for (int i = 0; i < nbt_devs; i++)
			if (!strcmp(bt_devs[i].addr, bt_sel))
				d = &bt_devs[i];
		if (!d || !d->paired) {
			info("Device", "Not paired any more");
			break;
		}
		info(bt_kind(d->icon), "%s", d->name);
		r = add(R_BT_CONN, d->connected ? "Disconnect" : "Connect");
		r->arg = !d->connected;
		snprintf(r->value, sizeof(r->value), "%s", d->connected ?
			 "Connected now" : "Not connected");
		r = add(R_ACTION, "Forget");
		r->arg = A_BT_FORGET;
		snprintf(r->value, sizeof(r->value), "Unpair it; pair again to use it");
		break;
	}
	case P_BT_SCAN: {
		char confirm[16], show[16], disc[4], pairing[24];
		int found = 0;
		bt_get("confirm", confirm, sizeof(confirm));
		bt_get("show", show, sizeof(show));
		bt_get("discovering", disc, sizeof(disc));
		bt_get("pairing", pairing, sizeof(pairing));
		if (confirm[0]) {
			r = add(R_BT_CONFIRM, "Pair: the codes match");
			r->arg = 1;
			snprintf(r->value, sizeof(r->value), "%s must be on the device too",
				 confirm);
			r = add(R_BT_CONFIRM, "Cancel: they differ");
			r->arg = 0;
			snprintf(r->value, sizeof(r->value), "Do not pair");
		}
		if (show[0])
			info("Type this code on the device", "%s, then Enter", show);
		if (pairing[0] && !confirm[0] && !show[0])
			info("Pairing…", "Follow any steps on the device");
		bt_load();
		for (int i = 0; i < nbt_devs && nrows < 60; i++) {
			struct bt_dev *d = &bt_devs[i];
			if (d->paired)
				continue;
			r = add(R_BT_FOUND, d->name);
			r->arg = i;
			snprintf(r->value, sizeof(r->value), "%s%s", bt_kind(d->icon),
				 !d->rssi ? "" : d->rssi >= -60 ? ", strong signal" :
				 d->rssi >= -75 ? ", good signal" : ", weak signal");
			found++;
		}
		if (!found && !pairing[0])
			info(disc[0] == '1' ? "Looking for devices…" : "No devices found",
			     "Put the device in pairing mode");
		break;
	}
	case P_DISPLAY:
		r = add(R_BRIGHTNESS, "Brightness");
		snprintf(r->value, sizeof(r->value), "%d%%", brightness_pct());
		r = add(R_CHOICE, "Screen timeout");
		r->choice = &screen_timeout;
		r = add(R_CHOICE, "Sleep with the lid closed");
		r->choice = &lid_suspend;
		r = add(R_CHOICE, "Clock");
		r->choice = &clock_fmt;
		r = add(R_CHOICE, "Theme");
		r->choice = &theme;
		{
			char v[256];
			r = add(R_WALL, "Home screen wallpaper");
			r->arg = WALL_HOME;
			snprintf(r->value, sizeof(r->value), "%s", wall_name(WALL_HOME, v, sizeof(v)));
			r = add(R_WALL, "Lock screen wallpaper");
			r->arg = WALL_LOCK;
			snprintf(r->value, sizeof(r->value), "%s", wall_name(WALL_LOCK, v, sizeof(v)));
		}
		break;
	case P_OUTER:
		r = add(R_CHOICE, "Stays lit for");
		r->choice = &outer_timeout;
		r = add(R_CHOICE, "Clock colon");
		r->choice = &outer_blink;
		{
			char v[256];
			r = add(R_WALL, "Wallpaper");
			r->arg = WALL_OUTER;
			snprintf(r->value, sizeof(r->value), "%s", wall_name(WALL_OUTER, v, sizeof(v)));
		}
		r = add(R_CHOICE, "Wallpaper speed");
		r->choice = &wall_speed;
		break;
	case P_DEV: {
		char cal[96];
		ping_ifaces();
		for (int i = 0; i < NPINGS; i++) {
			struct ping *p = &pings[i];
			if (p->mic) {
				r = add(R_CHOICE, "Call microphone, analog gain");
				r->choice = &mic_adc;
				r = add(R_CHOICE, "Call microphone, digital gain");
				r->choice = &mic_digital;
			}
			if (!p->iface[0]) {
				info(p->label, "%s", p->hold ? "s22-celld is not running" :
				     "No Wi-Fi interface");
				continue;
			}
			r = add(R_PING, p->label);
			r->arg = i;
			snprintf(r->value, sizeof(r->value), "%s", p->result[0] ?
				 p->result : p->mic ? "Records 4 s, plays it on the earpiece" :
				 p->hold ? "To " PING_HOST ", 4 pings, a few KB of data" :
				 "To " PING_HOST ", 4 pings");
		}
		r = add(R_RUN, "Touch test and calibration");
		r->app = "s22-touchtest";
		snprintf(r->value, sizeof(r->value), "%s",
			 ui_setting("touch_calibration", "", cal, sizeof(cal))[0] ?
			 "Calibrated" : "Not calibrated");
		break;
	}
	case P_SYSTEM: {
		struct utsname u;
		long total = 0, avail = 0;
		char line[128];
		FILE *f = fopen("/proc/meminfo", "re");
		while (f && fgets(line, sizeof(line), f)) {
			sscanf(line, "MemTotal: %ld", &total);
			sscanf(line, "MemAvailable: %ld", &avail);
		}
		if (f)
			fclose(f);
		uname(&u);
		info("Kernel", "%s", u.release);
		double up = 0;
		f = fopen("/proc/uptime", "re");
		if (f) {
			if (fscanf(f, "%lf", &up) != 1)
				up = 0;
			fclose(f);
		}
		info("Up for", "%dh %02dm", (int)up / 3600, (int)up / 60 % 60);
		info("Memory", "%ld MB used of %ld MB", (total - avail) / 1024,
		     total / 1024);
		struct statvfs sv;
		if (statvfs("/", &sv) == 0)
			info("Phone storage", "%.1f GB free",
			     (double)sv.f_bavail * sv.f_frsize / 1e9);
		if (statvfs("/home", &sv) == 0)
			info("Memory card", "%.1f GB free",
			     (double)sv.f_bavail * sv.f_frsize / 1e9);
		int pct = read_int("/run/s22-battery/capacity", -1);
		/* The fuel gauge (pm8916-bms-vm), not a supply named "battery" */
		int uv = read_int("/sys/class/power_supply/pm8916-bms-vm/voltage_now", 0);
		if (uv > 0)
			info("Battery", "%d%%, %.2f V", pct, uv / 1e6);
		else
			info("Battery", "%d%%", pct);
		break;
	}
	default:
		break;
	}
	for (int i = 0; i < nrows; i++)
		if (rows[i].kind == R_CHOICE)
			snprintf(rows[i].value, sizeof(rows[i].value), "%s",
				 rows[i].choice->labels[choice_get(rows[i].choice)]);
	lists[page].count = nrows;
	lists[page].row_h = 66;
	if (lists[page].sel >= nrows)
		lists[page].sel = nrows ? nrows - 1 : 0;
}

static void draw_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
		     int w, int h, bool sel)
{
	struct row *r = &rows[i];
	int right = r->bars >= 0 ? 90 : 60;
	const char *label = r->kind == R_TONE_PICK && r->arg >= 0 && tone_files ?
			    ui_sound_name(tone_files[r->arg]) : r->label;

	if (r->kind == R_LEVEL)
		snprintf(r->value, sizeof(r->value), "%d of 15", level_get(r->arg));
	ui_text_box(c, x + 16, y + 8, w - right, UI_SANS, 21, UI_FG, UI_LEFT,
		    label);
	ui_text_box(c, x + 16, y + 36, w - right, UI_SANS, 17,
		    r->kind == R_INFO ? UI_FG : UI_DIM, UI_LEFT, r->value);
	if (r->bars >= 0)
		ui_wifi_icon(c, w - 66, y + 16, 26, r->bars);
	if (r->kind == R_PAGE || r->kind == R_RUN || r->kind == R_SAVED ||
	    r->kind == R_BT_DEV || r->kind == R_TONE || r->kind == R_VIB)
		ui_text(c, w - 30, y + 18, UI_SANS, 24, UI_DIM, "›");
	else if ((r->kind == R_CHOICE || r->kind == R_BRIGHTNESS || r->kind == R_LEVEL) &&
		 sel)
		ui_text(c, w - 40, y + 20, UI_SANS, 20, UI_ACCENT, "◂▸");
	if (r->kind == R_BRIGHTNESS || r->kind == R_LEVEL) {
		int bw = w - 200, pct = r->kind == R_LEVEL ? level_get(r->arg) * 100 / 15 :
			  brightness_pct();
		ui_fill(c, x + 120, y + 45, bw, 6, UI_BAR);
		ui_fill(c, x + 120, y + 45, bw * pct / 100, 6, UI_ACCENT);
	}
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void draw_flash(struct ui_canvas *c)
{
	if (!flash[0])
		return;
	int y = c->h - UI_HINTS_H - 44;
	ui_round(c, 20, y, c->w - 40, 38, 10, UI_WARN);
	ui_text_box(c, 30, y + 8, c->w - 60, UI_SANS, 18, UI_FG, UI_CENTER, flash);
}

/* The text entry of an entry page, or NULL */
static struct ui_entry *page_entry(void)
{
	return page == P_WIFI_PASS ? &pass_entry : page == P_TS_KEY ? &ts_entry :
	       page == P_MV_ACCOUNT ? &mv_entry : page == P_PIN ? &pin_entry :
	       page == P_SHARE_PASS ? &share_entry : NULL;
}

static void pin_start(struct ui *ui, bool removing)
{
	pin_removing = removing;
	pin_step = ui_pin_set_up() ? PS_CURRENT : PS_NEW;
	ui_entry_set(&pin_entry, "");
	page = P_PIN;
}

/* OK on a PIN page */
static void pin_next(struct ui *ui)
{
	const char *t = pin_entry.text;
	size_t n = strlen(t);

	if (pin_step == PS_CURRENT) {
		if (!ui_pin_check(t)) {
			say("Wrong PIN");
		} else if (pin_removing) {
			say(ui_pin_set(NULL) ? "Screen lock off" : "Cannot save the settings");
			page = P_SECURITY;
		} else {
			pin_step = PS_NEW;
		}
	} else if (pin_step == PS_NEW) {
		if (n < 4 || n > 12) {
			say("4 to 12 digits");
			return;
		}
		snprintf(pin_new, sizeof(pin_new), "%s", t);
		pin_step = PS_CONFIRM;
	} else {
		if (strcmp(t, pin_new)) {
			say("They differ: the new PIN again");
			pin_step = PS_NEW;
		} else {
			say(ui_pin_set(t) ? "PIN set: the screen locks" :
			    "Cannot save the settings");
			page = P_SECURITY;
		}
		explicit_bzero(pin_new, sizeof(pin_new));
	}
	ui_entry_set(&pin_entry, "");
}

static void draw_entry(struct ui *ui, struct ui_canvas *c, struct ui_entry *e)
{
	const char *head = pass_net.ssid, *label = pass_net.sae_only ?
		"WPA3 password" : "Password", *ok = "Connect";

	if (page == P_PIN) {
		head = pin_removing ? "Turn the screen lock off" : "Screen lock PIN";
		label = pin_step == PS_CURRENT ? "Current PIN" :
			pin_step == PS_NEW ? "New PIN, 4 to 12 digits" : "The new PIN again";
		ok = pin_step == PS_CURRENT && pin_removing ? "Turn off" : "Next";
	} else if (page == P_TS_KEY) {
		head = "Tailscale";
		label = "Auth key: admin console, Settings, Keys";
		ok = "Log in";
	} else if (page == P_MV_ACCOUNT) {
		head = "Mullvad";
		label = "Account number (16 digits)";
		ok = "Log in";
	} else if (page == P_SHARE_PASS) {
		head = "File sharing";
		label = "Your phone login password";
		ok = share_turn_on ? "Turn on" : "Set";
	}
	ui_header(ui, c, titles[page]);
	ui_text_box(c, 16, UI_HEADER_H + 16, c->w - 32, UI_BOLD, 21, UI_FG, UI_LEFT,
		    head);
	ui_text_box(c, 16, UI_HEADER_H + 48, c->w - 32, UI_SANS, 17, UI_DIM, UI_LEFT,
		    label);
	ui_round(c, 12, UI_HEADER_H + 76, c->w - 24, 86, 10, UI_BAR);
	ui_entry_draw(c, e, 24, UI_HEADER_H + 88, c->w - 48, 20, 2);
	ui_entry_indicator(c, e, 0, 0, 0);	/* in the status bar */
	draw_flash(c);
	ui_hints(c, "Cancel", e->text[0] && ctl_fd < 0 ? ok : NULL, NULL);
}

/* Tailscale's login link as a QR code, black on white whatever the theme */
static void draw_qr(struct ui *ui, struct ui_canvas *c)
{
	char url[256];
	int top = UI_HEADER_H + 10;

	ui_header(ui, c, titles[page]);
	vpn_get("tailscale_url", url, sizeof(url));
	QRcode *q = url[0] ? QRcode_encodeString(url, 0, QR_ECLEVEL_M, QR_MODE_8, 1) :
		    NULL;
	if (!q) {
		ui_text_box(c, 16, top + 10, c->w - 32, UI_SANS, 19, UI_FG, UI_LEFT,
			    url[0] ? "Cannot make the QR code" :
			    "No login is waiting: the link appears here when Tailscale needs one");
		ui_hints(c, "Back", NULL, NULL);
		return;
	}
	int room = c->h - top - UI_HINTS_H - 96;
	room = room < c->w - 24 ? room : c->w - 24;
	int mod = room / (q->width + 8);	/* 4 modules of quiet zone a side */
	int size = mod * (q->width + 8);
	int x0 = (c->w - size) / 2;
	ui_fill(c, x0, top, size, size, 0xffffffff);
	for (int y = 0; y < q->width; y++)
		for (int x = 0; x < q->width; x++)
			if (q->data[y * q->width + x] & 1)
				ui_fill(c, x0 + (x + 4) * mod, top + (y + 4) * mod, mod, mod,
					0xff000000);
	QRcode_free(q);
	ui_text_box(c, 16, top + size + 8, c->w - 32, UI_SANS, 17, UI_FG, UI_CENTER,
		    "Scan with a phone signed in to Tailscale, or open:");
	ui_text_box(c, 8, top + size + 34, c->w - 16, UI_SANS, 15, UI_DIM, UI_CENTER,
		    url);
	ui_hints(c, "Back", NULL, NULL);
}

static void draw_rec(struct ui *ui, struct ui_canvas *c);

static void draw(struct ui *ui, struct ui_canvas *c)
{
	struct ui_list *l = &lists[page];
	const char *ok = NULL;

	if (page_entry()) {
		draw_entry(ui, c, page_entry());
		return;
	}
	if (page == P_TS_QR) {
		draw_qr(ui, c);
		return;
	}
	if (page == P_VIB_REC) {
		draw_rec(ui, c);
		return;
	}
	build();
	ui_header(ui, c, titles[page]);
	ui_list_draw(ui, c, l, UI_HEADER_H, c->h - UI_HEADER_H - UI_HINTS_H,
		     draw_row);
	if (nrows > 0) {
		switch (rows[l->sel].kind) {
		case R_PAGE: case R_RUN: case R_SAVED: ok = "Open"; break;
		case R_INFO: ok = NULL; break;
		case R_NET: ok = nets[rows[l->sel].arg].current ? NULL : "Connect"; break;
		case R_CONNECT: ok = "Connect"; break;
		case R_FORGET: ok = "Forget"; break;
		case R_PRIVATE: ok = saved[sel_saved].private_mac ? "Turn off" : "Turn on"; break;
		case R_SCAN: ok = "Scan"; break;
		case R_PING: ok = pings[rows[l->sel].arg].fd >= 0 ? NULL :
			   pings[rows[l->sel].arg].mic ? "Test" : "Ping"; break;
		case R_ACTION: ok = rows[l->sel].arg == A_BT_FORGET ? "Forget" : "Log out"; break;
		case R_BT_DEV: ok = "Open"; break;
		case R_TONE: ok = "Open"; break;
		case R_LOCK: ok = ui_pin_set_up() ? "Turn off" : "Set a PIN"; break;
		case R_PIN_CHANGE: ok = "Change"; break;
		case R_LOCK_NOW: ok = "Lock"; break;
		case R_SAY: ok = "Play"; break;
		case R_VOICE: ok = "Open"; break;
		case R_VOICE_PICK: ok = "Choose"; break;
		case R_WALL: ok = "Choose"; break;
		case R_SHARE: ok = share_fd >= 0 ? NULL : share.on ? "Turn off" : "Turn on"; break;
		case R_TONE_PICK: ok = "Choose"; break;
		case R_VIB: ok = "Open"; break;
		case R_VIB_PICK: ok = rows[l->sel].arg == VIB_RECORD ? "Record" : "Choose"; break;
		case R_BT_FOUND: ok = ctl_fd >= 0 ? NULL : "Pair"; break;
		case R_BT_CONN: ok = rows[l->sel].label; break;
		case R_BT_CONFIRM: ok = "OK"; break;
		case R_MV_DEVICE: ok = "Remove"; break;
		case R_MV_COUNTRY: ok = "Use"; break;
		case R_WIFI: case R_BT: ok = "Switch"; break;
		default: ok = "Change"; break;
		}
	}
	draw_flash(c);
	ui_hints(c, page == P_MAIN ? "Close" : "Back", ok, NULL);
}

static void go(struct ui *ui, enum page to)
{
	page = to;
	lists[page].sel = lists[page].top = 0;
	if (page == P_WIFI) {
		wpa_open(ui);
		wpa_refresh();
		wpa_scan();
	} else if (page == P_WIFI_SAVED || page == P_NET) {
		wpa_open(ui);
		wpa_refresh();
	} else if (page == P_BT_SCAN) {
		bt_scanning = true;
		bt_request(ui, bt_said, "Scanning…", "scan on");
	} else if (page == P_MV_COUNTRY && nmv_countries == 0) {
		vpn_request(ui, mv_countries_done, "Loading the server list…",
			    "mv-countries");
	}
	if (page == P_NET || page == P_SHARE)
		share_refresh(ui);
	if (page == P_SHARE_PASS)
		ui_entry_set(&share_entry, "");
}

static void change(struct ui *ui, struct row *r, int d)
{
	switch (r->kind) {
	case R_CHOICE:
		choice_step(r->choice, d);
		break;
	case R_BRIGHTNESS:
		brightness_step(d);
		break;
	case R_LEVEL:
		level_step(r->arg, d);
		break;
	case R_TONE:
		if (d > 0) {
			char v[160], (*files)[160];
			int n;
			tone_cur = r->arg;
			go(ui, P_TONE);
			/* Start on the sound chosen now */
			tone_value(&tones[tone_cur], v, sizeof(v));
			n = ui_sounds(tones[tone_cur].kind, &files);
			int extra = tones[tone_cur].def ? 2 : 1;
			lists[P_TONE].sel = !strcmp(v, "default") ? 0 :
					    !strcmp(v, "silent") ? extra - 1 : 0;
			for (int i = 0; i < n; i++)
				if (!strcmp(v, files[i]))
					lists[P_TONE].sel = i + extra;
			lists[P_TONE].top = lists[P_TONE].sel > 3 ? lists[P_TONE].sel - 3 : 0;
		}
		break;
	case R_LOCK:
		if (d > 0)
			pin_start(ui, ui_pin_set_up());	/* on: off with the PIN */
		break;
	case R_PIN_CHANGE:
		if (d > 0)
			pin_start(ui, false);
		break;
	case R_LOCK_NOW:
		if (d > 0)
			ui_spawn("s22-lock");
		break;
	case R_VOICE:
		if (d > 0) {
			char v[32];
			go(ui, P_VOICE);
			/* Start on the voice chosen now */
			ui_setting("tts_voice", TTS_VOICE, v, sizeof(v));
			voices_load();
			for (int i = 0; i < nvoices; i++)
				if (!strcmp(v, voices[i]))
					lists[P_VOICE].sel = i;
			lists[P_VOICE].top = lists[P_VOICE].sel > 3 ? lists[P_VOICE].sel - 3 : 0;
		}
		break;
	case R_VOICE_PICK:
		/* Chosen, and heard in it; the list stays for trying another */
		if (d > 0 && ui_setting_set("tts_voice", voices[r->arg])) {
			char cmd[160];
			snprintf(cmd, sizeof(cmd), "s22-say -s; s22-say 'This is %s.'",
				 voices[r->arg]);
			ui_spawn(cmd);
		}
		break;
	case R_WALL:
		/* Photos' Wallpapers tab, choosing for this screen */
		if (d > 0) {
			char uri[32];
			snprintf(uri, sizeof(uri), "wallpaper:%s", wall_kind[r->arg]);
			if (!ui_open_uri(uri))
				say("Photos cannot be opened");
		}
		break;
	case R_SAY:
		if (d > 0)
			ui_spawn("s22-say -s; s22-say 'This is how calls and texts "
				 "sound when they are read aloud.'");
		break;
	case R_VIB:
		if (d > 0) {
			char v[96], w[96];
			vib_cur = r->arg;
			go(ui, P_VIB);
			build();		/* start on the pattern chosen now */
			vib_value(&vibs[vib_cur], v, sizeof(v));
			for (int i = 0; i < nrows; i++)
				if (rows[i].kind == R_VIB_PICK && rows[i].arg != VIB_RECORD &&
				    !strcmp(v, vib_pick(rows[i].arg, w, sizeof(w))))
					lists[P_VIB].sel = i;
			lists[P_VIB].top = lists[P_VIB].sel > 3 ? lists[P_VIB].sel - 3 : 0;
		}
		break;
	case R_VIB_PICK:
		if (d > 0 && r->arg == VIB_RECORD) {
			ui_vibrate_stop();
			rec_n = 0;
			rec_down = false;
			page = P_VIB_REC;
		} else if (d > 0) {
			char v[96];
			ui_vibrate_stop();
			vib_pick(r->arg, v, sizeof(v));
			if (v[0] && ui_setting_set(vibs[vib_cur].key, v))
				say("%s: %s", vibs[vib_cur].label, ui_vib_name(v, vibs[vib_cur].ring));
			page = P_SOUNDS;
		}
		break;
	case R_TONE_PICK:
		if (d > 0) {
			const char *v = r->arg == -2 ? "default" : r->arg == -1 ? "silent" :
					tone_files ? tone_files[r->arg] : NULL;
			ui_sound_stop();
			if (v && ui_setting_set(tones[tone_cur].key, v))
				say("%s: %s", tones[tone_cur].label, ui_sound_name(v));
			page = P_SOUNDS;
		}
		break;
	case R_WIFI:
		radio_set(RFKILL_TYPE_WLAN, radio_on(RFKILL_TYPE_WLAN) != 1);
		if (radio_on(RFKILL_TYPE_WLAN) == 1) {
			wpa_open(ui);
			wpa_scan();
		}
		wpa_refresh();
		break;
	case R_BT:
		radio_set(RFKILL_TYPE_BLUETOOTH,
			  radio_on(RFKILL_TYPE_BLUETOOTH) != 1);
		break;
	case R_PAGE:
		if (d > 0)
			go(ui, r->to);
		break;
	case R_RUN: {
		struct ui_appinfo apps[64];
		int n = ui_apps(apps, 64);
		if (d < 0)
			break;
		for (int i = 0; i < n; i++)
			if (strcmp(apps[i].id, r->app) == 0)
				ui_open_app(&apps[i]);
		break;
	}
	case R_NET: {
		struct net *n = &nets[r->arg];
		if (d < 0 || n->current)
			break;
		if (n->saved_id >= 0) {
			connect_start(n->saved_id, false, n->ssid);
		} else if (!n->secure) {
			connect_new(n, NULL);
		} else {
			pass_net = *n;
			ui_entry_set(&pass_entry, "");
			page = P_WIFI_PASS;
		}
		break;
	}
	case R_SAVED:
		if (d > 0) {
			sel_saved = r->arg;
			go(ui, P_WIFI_NET);
		}
		break;
	case R_CONNECT:
		if (d > 0 && sel_saved >= 0 && sel_saved < nsaved) {
			connect_start(saved[sel_saved].id, false, saved[sel_saved].ssid);
			page = P_WIFI;
		}
		break;
	case R_PRIVATE:
		if (d > 0 && sel_saved >= 0 && sel_saved < nsaved) {
			struct saved *s = &saved[sel_saved];
			bool on = !s->private_mac;
			if (!set_private_mac(s->id, on) || !wpa_ok("SAVE_CONFIG")) {
				say("Cannot change the address");
			} else if (s->current && wifi_ssid[0]) {
				/* Joins again with the new address (and a new IP) */
				wpa_ok("DISCONNECT");
				wpa_ok("RECONNECT");
				say(on ? "Private address on; joining again…" :
				    "Private address off; joining again…");
			} else {
				say(on ? "Private address on" : "Private address off");
			}
			wpa_refresh();
		}
		break;
	case R_FORGET:
		if (d > 0) {
			forget(sel_saved);
			page = P_WIFI_SAVED;
		}
		break;
	case R_SCAN:
		if (d > 0) {
			wpa_scan();
			say("Scanning…");
		}
		break;
	case R_PING:
		if (d > 0)
			ping_start(ui, &pings[r->arg]);
		break;
	case R_SHARE:
		if (share_fd >= 0)
			break;			/* still busy */
		if (!share.known) {
			share_refresh(ui);
		} else if (share.on) {
			say("Turning file sharing off…");
			share_run(ui, "off", NULL, share_switch_done);
		} else if (!share.pass) {
			share_turn_on = true;	/* after the password */
			go(ui, P_SHARE_PASS);
		} else {
			say("Turning file sharing on…");
			share_run(ui, "on", NULL, share_switch_done);
		}
		break;
	case R_ACTION:
		if (d < 0)
			break;
		if (r->arg == A_TS_LOGOUT && confirmed(A_TS_LOGOUT, 0, "log out of Tailscale"))
			vpn_request(ui, vpn_said, "Logging out…", "ts-logout");
		else if (r->arg == A_MV_LOGOUT &&
			 confirmed(A_MV_LOGOUT, 0, "log out and free this device"))
			vpn_request(ui, vpn_said, "Logging out…", "mv-logout");
		else if (r->arg == A_BT_FORGET &&
			 confirmed(A_BT_FORGET, 0, "forget this device"))
			bt_request(ui, bt_forget_done, "Forgetting…", "forget %s", bt_sel);
		break;
	case R_BT_DEV:
		if (d > 0) {
			snprintf(bt_sel, sizeof(bt_sel), "%s", bt_devs[r->arg].addr);
			go(ui, P_BT_DEV);
		}
		break;
	case R_BT_FOUND:
		if (d > 0)
			bt_request(ui, bt_pair_done, "Pairing…", "pair %s",
				   bt_devs[r->arg].addr);
		break;
	case R_BT_CONN:
		if (d > 0)
			bt_request(ui, bt_said, r->arg ? "Connecting…" : "Disconnecting…",
				   "%s %s", r->arg ? "connect" : "disconnect", bt_sel);
		break;
	case R_BT_CONFIRM:
		if (d > 0)
			bt_now(r->arg ? "confirm yes" : "confirm no");
		break;
	case R_MV_DEVICE:
		if (d > 0 && confirmed(A_MV_REPLACE, r->arg, "remove this device"))
			vpn_request(ui, mv_login_done, "Removing it, logging in…",
				    "mv-replace %s %s", mv_entry.text, mv_devs[r->arg].id);
		break;
	case R_MV_COUNTRY:
		if (d > 0)
			vpn_request(ui, mv_country_done, "Changing the server…",
				    "mv-country %s", mv_countries[r->arg].code);
		break;
	case R_INFO:
		break;
	}
}

static void key_entry(struct ui *ui, struct ui_entry *e, uint32_t code,
		      enum ui_key_state st)
{
	if (st != UI_RELEASED && (code == KEY_BACK || code == KEY_ESC)) {
		page = parent[page];
	} else if (st == UI_PRESSED && (code == KEY_ENTER || code == KEY_OK)) {
		if (!e->text[0] || ctl_fd >= 0)
			;
		else if (page == P_TS_KEY)
			vpn_request(ui, ts_key_done, "Logging in to Tailscale…",
				    "ts-key %s", e->text);
		else if (page == P_MV_ACCOUNT)
			vpn_request(ui, mv_login_done, "Logging in to Mullvad…",
				    "mv-login %s", e->text);
		else if (page == P_PIN)
			pin_next(ui);
		else if (page == P_SHARE_PASS) {
			if (share_run(ui, "password", e->text, share_pass_done))
				say("Checking the password…");
		}
		else {
			connect_new(&pass_net, e->text);
			if (pending_id >= 0)
				page = P_WIFI;
		}
	} else {
		ui_entry_key(e, code, st);
	}
	ui_redraw(ui);
}

/* The recorder: OK held vibrates and is recorded; # plays it back, *
 * saves it (as the next Custom, and for the setting), C starts again */
static void key_rec(struct ui *ui, uint32_t code, enum ui_key_state st)
{
	long long now = mono_ms();
	char pat[200];

	if (code == KEY_ENTER || code == KEY_OK) {
		if (st == UI_PRESSED && !rec_down && rec_n < 15) {
			if (rec_n > 0) {		/* the pause since the last */
				long long gap = now - rec_t;
				rec_steps[rec_n++] = gap > 2000 ? 2000 : (int)gap;
			}
			rec_down = true;
			rec_t = now;
			ui_vibrate("5000");
		} else if (st == UI_RELEASED && rec_down) {
			long long on = now - rec_t;
			rec_down = false;
			ui_vibrate_stop();
			rec_steps[rec_n++] = on > 3000 ? 3000 : on < 30 ? 30 : (int)on;
			rec_t = now;
		}
	} else if (st != UI_PRESSED) {
		return;
	} else if (code == KEY_BACK || code == KEY_ESC) {
		ui_vibrate_stop();
		page = P_VIB;
	} else if (code == KEY_BACKSPACE) {
		rec_n = 0;
	} else if (code == KEY_NUMERIC_POUND && rec_n) {
		rec_pattern(pat, sizeof(pat));
		vib_feel(pat, vibs[vib_cur].ring);
	} else if (code == KEY_NUMERIC_STAR && rec_n && !rec_down) {
		char customs[UI_VIB_CUSTOMS][96], key[24];
		int slot = 0;
		rec_pattern(pat, sizeof(pat));
		ui_vib_customs(customs);
		while (slot < UI_VIB_CUSTOMS - 1 && customs[slot][0])
			slot++;			/* the first free one, else the last */
		snprintf(key, sizeof(key), "vib_custom_%d", slot + 1);
		if (ui_setting_set(key, pat) && ui_setting_set(vibs[vib_cur].key, pat))
			say("%s: Custom %d", vibs[vib_cur].label, slot + 1);
		page = P_SOUNDS;
	}
	ui_redraw(ui);
}

static void draw_rec(struct ui *ui, struct ui_canvas *c)
{
	int y = UI_HEADER_H + 16, total = 0;

	ui_header(ui, c, titles[P_VIB_REC]);
	ui_text_box(c, 16, y, c->w - 32, UI_SANS, 18, UI_FG, UI_LEFT,
		    vibs[vib_cur].label);
	ui_text_wrap(c, 16, y + 34, c->w - 32, UI_SANS, 17, UI_DIM,
		     "Hold OK to vibrate, let go to pause, in the rhythm you want "
		     "(16 steps at most). # plays it back, C starts again, * saves it.");
	/* The pattern as a timeline: on dark, off light */
	for (int i = 0; i < rec_n; i++)
		total += rec_steps[i];
	int bx = 16, bw = c->w - 32, by = y + 150;
	ui_round(c, bx, by, bw, 40, 8, UI_BAR);
	for (int i = 0, t = 0; i < rec_n && total; i++) {
		if (i % 2 == 0)
			ui_fill(c, bx + (int)((long long)bw * t / total), by + 6,
				(int)((long long)bw * rec_steps[i] / total) + 1, 28, UI_ACCENT);
		t += rec_steps[i];
	}
	char info[64];
	if (rec_down)
		snprintf(info, sizeof(info), "Vibrating…");
	else if (rec_n)
		snprintf(info, sizeof(info), "%d steps, %.1f s", rec_n, total / 1000.0);
	else
		snprintf(info, sizeof(info), "Nothing yet");
	ui_text_box(c, 16, by + 52, c->w - 32, UI_SANS, 17, UI_FG, UI_CENTER, info);
	draw_flash(c);
	ui_hints(c, "Cancel", "Hold OK", rec_n ? "# play  * save" : NULL);
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st,
		const char *text)
{
	struct ui_list *l = &lists[page];

	if (page == P_VIB_REC) {
		key_rec(ui, code, st);
		return;
	}
	if (page_entry()) {
		key_entry(ui, page_entry(), code, st);
		return;
	}
	if (st == UI_RELEASED)
		return;
	build();
	if (ui_list_key(l, code)) {
		if (page == P_TONE)		/* hear each as it is chosen */
			tone_preview(l);
		if (page == P_VIB && nrows) {	/* feel each */
			char v[96];
			vib_feel(vib_pick(rows[l->sel].arg, v, sizeof(v)), vibs[vib_cur].ring);
		}
		ui_redraw(ui);
		return;
	}
	if (st != UI_PRESSED && code != KEY_LEFT && code != KEY_RIGHT)
		return;
	if (page == P_TONE && (code == KEY_BACK || code == KEY_ESC))
		ui_sound_stop();
	if (page == P_VIB && (code == KEY_BACK || code == KEY_ESC))
		ui_vibrate_stop();
	struct row *r = nrows ? &rows[l->sel] : NULL;
	switch (code) {
	case KEY_BACK:
	case KEY_ESC:
		if (page == P_MAIN)
			ui_quit(ui);
		else
			page = parent[page];
		break;
	case KEY_LEFT:
		if (r && r->kind != R_PAGE)
			change(ui, r, -1);
		break;
	case KEY_RIGHT:
	case KEY_ENTER:
	case KEY_OK:
		if (r)
			change(ui, r, 1);
		break;
	default:
		return;
	}
	ui_redraw(ui);
}

static void tap(struct ui *ui, int x, int y)
{
	struct ui_list *l = &lists[page];
	int i;

	if (page_entry())
		return;
	build();
	i = ui_list_at(l, UI_HEADER_H, y);
	if (i < 0)
		return;
	int was = l->sel;
	l->sel = i;
	/* Left half of a value row steps back, the right half forward */
	if (rows[i].kind == R_CHOICE || rows[i].kind == R_BRIGHTNESS ||
	    rows[i].kind == R_LEVEL)
		change(ui, &rows[i], x < ui_width(ui) / 2 ? -1 : 1);
	else if (page == P_TONE && rows[i].kind == R_TONE_PICK && was != i)
		tone_preview(l);		/* a first tap plays it */
	else if (page == P_VIB && rows[i].kind == R_VIB_PICK && rows[i].arg != VIB_RECORD &&
		 was != i) {
		char v[96];			/* ... or buzzes it */
		vib_feel(vib_pick(rows[i].arg, v, sizeof(v)), vibs[vib_cur].ring);
	}
	else
		change(ui, &rows[i], 1);
	ui_redraw(ui);
}

static void drag(struct ui *ui, int dx, int dy, bool done)
{
	if (page_entry())
		return;
	build();
	ui_list_drag(&lists[page], dy,
		     ui_height(ui) - UI_HEADER_H - UI_HINTS_H);
	ui_redraw(ui);
}

static void tick(struct ui *ui)
{
	static int scans;

	/* Clears the message; keeps Wi-Fi current; rescans on the Wi-Fi page */
	if (flash[0] && pending_id < 0 && ctl_fd < 0)
		flash[0] = 0;
	if (pending_id >= 0 && time(NULL) - pending_since > 30) {
		say("Could not connect to %s", pending_ssid);
		pending_end(false);
	}
	wpa_refresh();
	if (page == P_WIFI && ++scans % 4 == 0)
		wpa_scan();
	/* The scan page keeps a scan going; leaving it ends it */
	if (ctl_fd < 0) {
		char disc[4], pairing[24];
		bt_get("discovering", disc, sizeof(disc));
		bt_get("pairing", pairing, sizeof(pairing));
		if (page == P_BT_SCAN && disc[0] != '1' && !pairing[0])
			bt_request(ui, bt_said, "", "scan on");
		else if (page != P_BT_SCAN && bt_scanning) {
			bt_scanning = false;
			bt_request(ui, bt_said, "", "scan off");
		}
	}
	ui_redraw(ui);
	ui_timer(ui, 5000);
}

static void start(struct ui *ui)
{
	app_ui = ui;
	ui_entry_init(ui, &pass_entry, false);
	pass_entry.mode = 0;		/* passwords start lower case */
	pass_entry.secret = true;
	ui_entry_init(ui, &ts_entry, false);
	ts_entry.mode = 0;
	ts_entry.secret = true;
	ui_entry_init(ui, &mv_entry, true);
	mv_entry.secret = true;
	ui_entry_init(ui, &pin_entry, true);
	pin_entry.secret = true;
	ui_entry_init(ui, &share_entry, false);
	share_entry.mode = 0;
	share_entry.secret = true;
	atexit(wpa_cleanup);
	/* Redrawn when Bluetooth changes, and when the settings do from
	 * elsewhere (s22-wm's volume keys) */
	int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ifd >= 0) {
		inotify_add_watch(ifd, BT_DIR, IN_CLOSE_WRITE | IN_MOVED_TO);
		inotify_add_watch(ifd, "/var/lib/s22", IN_MOVED_TO);
		ui_watch(ui, ifd, on_bt_files);
	}
	wpa_open(ui);
	wpa_refresh();
	ui_timer(ui, 5000);
}

int main(void)
{
	struct ui_app app = {
		.app_id = "s22-settings",
		.title = "Settings",
		.draw = draw,
		.key = key,
		.tick = tick,
		.start = start,
		.tap = tap,
		.drag = drag,
	};

	return ui_main(&app);
}
