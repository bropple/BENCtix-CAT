// SPDX-License-Identifier: MIT
/*
 * Now playing (D059): the Music app's player (s22-musicd, mpv on
 * $XDG_RUNTIME_DIR/s22-music.sock) for whatever shows it: the notification
 * list and the lock screen follow it here, and the app shares its covers,
 * the record drawn without one, and the card.
 *
 * The socket is mpv's JSON IPC; mpv takes several clients, so each
 * follower has its own connection, made when the socket appears (inotify
 * on the runtime directory, no polling). Writes never block (D058).
 *
 * Covers: the song folder's albumart.jpg (as MediaMonkey writes it),
 * cover.jpg, folder.jpg or front.jpg, else any JPEG or PNG there, else the
 * song's own picture. A JPEG is decoded at once; a PNG or a song's picture
 * goes through ffmpeg in the background (a second or more here) and the
 * cover shows when it comes. One song's cover is kept, scaled once a size.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <jpeglib.h>
#include "s22ui.h"

struct ui_np ui_np;

static long long np_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* ---------------------------------------------------------- the player --- */

enum { P_PAUSE = 1, P_TITLE, P_ARTIST, P_ALBUM, P_DURATION, P_PATH, P_IDLE };
#define R_TIME	100

static struct ui *np_ui;
static int np_fd = -1, np_in = -1, np_timer = -1;
static char np_rbuf[1 << 16];
static size_t np_rlen;
static char *np_obuf;
static size_t np_olen, np_ocap;
static bool np_live, np_idle = true;
static int np_retry;			/* connects left to try, 300 ms apart */

/* Left/Right held: scrubbing, 10 s a second, 30 after two */
static uint32_t scrub_key;
static bool scrubbing;
static long long scrub_since, scrub_last, scrub_sent;
static double scrub_pos;

static void np_arm(void);

static void np_sock_path(char *buf, size_t n)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");

	snprintf(buf, n, "%s/s22-music.sock", dir ? dir : "/run/s22-gui");
}

static void np_changed(void)
{
	bool on = np_fd >= 0 && !np_idle && ui_np.path[0];

	if (on != ui_np.on) {
		ui_np.on = on;
		np_arm();
	}
	if (np_ui)
		ui_redraw(np_ui);
}

static void np_drop(void)
{
	if (np_fd < 0)
		return;
	ui_unwatch(np_ui, np_fd);
	close(np_fd);
	np_fd = -1;
	np_olen = np_rlen = 0;
	np_idle = true;
	scrub_key = 0;
	scrubbing = false;
	np_changed();
}

static void np_flush(void)
{
	while (np_fd >= 0 && np_olen) {
		ssize_t n = write(np_fd, np_obuf, np_olen);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;		/* the rest after mpv's next answer */
		if (n <= 0) {
			np_drop();
			return;
		}
		memmove(np_obuf, np_obuf + n, np_olen - n);
		np_olen -= n;
	}
}

static void np_write(const char *s, size_t n)
{
	if (np_fd < 0)
		return;
	if (np_olen + n > np_ocap) {
		size_t cap = np_ocap ? np_ocap : 1024;
		while (cap < np_olen + n)
			cap *= 2;
		char *b = cap <= 1 << 20 ? realloc(np_obuf, cap) : NULL;
		if (!b) {
			np_drop();
			return;
		}
		np_obuf = b;
		np_ocap = cap;
	}
	memcpy(np_obuf + np_olen, s, n);
	np_olen += n;
	np_flush();
}

/* {"command":[ARGS...]}, with a request_id if id */
static void np_cmdv(int id, const char *const *args)
{
	char line[2400];
	size_t o = 0;

	o += snprintf(line + o, sizeof(line) - o, "{\"command\":[");
	for (int i = 0; args[i] && o < sizeof(line) - 16; i++) {
		if (i)
			line[o++] = ',';
		line[o++] = '"';
		for (const char *p = args[i]; *p && o < sizeof(line) - 16; p++) {
			if (*p == '"' || *p == '\\')
				line[o++] = '\\';
			if ((unsigned char)*p < 0x20)
				continue;
			line[o++] = *p;
		}
		line[o++] = '"';
	}
	if (id)
		o += snprintf(line + o, sizeof(line) - o, "],\"request_id\":%d}\n", id);
	else
		o += snprintf(line + o, sizeof(line) - o, "]}\n");
	np_write(line, o);
}

#define np_cmd(...) np_cmdv(0, (const char *const[]){ __VA_ARGS__, NULL })

static void np_ask_time(void)
{
	np_cmdv(R_TIME, (const char *const[]){ "get_property", "time-pos", NULL });
}

/* A member of mpv's answer: what follows "key": (mpv escapes quotes inside
 * strings, so the pattern cannot be inside one) */
static const char *jat(const char *line, const char *key)
{
	char pat[32];

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	const char *p = strstr(line, pat);
	if (!p)
		return NULL;
	p += strlen(pat);
	while (*p == ' ')
		p++;
	return p;
}

/* A JSON string at p into out ("" if it is not one) */
static void jstr_at(const char *p, char *out, size_t n)
{
	size_t o = 0;

	out[0] = 0;
	if (!p || *p != '"')
		return;
	for (p++; *p && *p != '"' && o + 4 < n; p++) {
		if (*p != '\\') {
			out[o++] = *p;
			continue;
		}
		p++;
		if (*p == 'u') {
			unsigned u = 0;
			for (int i = 1; i <= 4 && isxdigit((unsigned char)p[i]); i++)
				u = u * 16 + (isdigit((unsigned char)p[i]) ? p[i] - '0' :
					      tolower((unsigned char)p[i]) - 'a' + 10);
			p += 4;
			if (u >= 0xd800 && u < 0xe000) {
				out[o++] = '?';
			} else if (u < 0x80) {
				out[o++] = u;
			} else if (u < 0x800) {
				out[o++] = 0xc0 | u >> 6;
				out[o++] = 0x80 | (u & 63);
			} else {
				out[o++] = 0xe0 | u >> 12;
				out[o++] = 0x80 | ((u >> 6) & 63);
				out[o++] = 0x80 | (u & 63);
			}
			continue;
		}
		out[o++] = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
		if (!*p)
			break;
	}
	out[o] = 0;
}

static void np_line(const char *line)
{
	const char *rid = jat(line, "request_id"), *data = jat(line, "data");

	if (rid) {
		if (atoi(rid) == R_TIME && data && (isdigit((unsigned char)*data) || *data == '-'))
			ui_np.time = strtod(data, NULL);
		if (np_ui)
			ui_redraw(np_ui);
		return;
	}
	const char *ev = jat(line, "event"), *id = jat(line, "id");
	if (!ev || strncmp(ev, "\"property-change\"", 17) || !id)
		return;
	bool t = data && !strncmp(data, "true", 4);
	switch (atoi(id)) {
	case P_PAUSE:
		ui_np.paused = t;
		np_arm();
		break;
	case P_TITLE: jstr_at(data, ui_np.title, sizeof(ui_np.title)); break;
	case P_ARTIST: jstr_at(data, ui_np.artist, sizeof(ui_np.artist)); break;
	case P_ALBUM: jstr_at(data, ui_np.album, sizeof(ui_np.album)); break;
	case P_DURATION:
		ui_np.duration = data && isdigit((unsigned char)*data) ? strtod(data, NULL) : 0;
		break;
	case P_PATH:
		jstr_at(data, ui_np.path, sizeof(ui_np.path));
		ui_np.time = 0;
		break;
	case P_IDLE: np_idle = t; break;
	}
	np_changed();
}

static void np_on_read(struct ui *ui, int fd)
{
	ssize_t n = read(fd, np_rbuf + np_rlen, sizeof(np_rbuf) - 1 - np_rlen);

	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return;
	if (n <= 0) {
		np_drop();
		return;
	}
	np_rlen += n;
	np_rbuf[np_rlen] = 0;
	char *start = np_rbuf, *nl;
	while ((nl = strchr(start, '\n'))) {
		*nl = 0;
		np_line(start);
		start = nl + 1;
	}
	np_rlen -= start - np_rbuf;
	memmove(np_rbuf, start, np_rlen);
	if (np_rlen >= sizeof(np_rbuf) - 1)
		np_rlen = 0;		/* a line too long (the queue): not ours */
	np_flush();
}

static bool np_connect(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };

	if (np_fd >= 0)
		return true;
	np_sock_path(sa.sun_path, sizeof(sa.sun_path));
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0) {
		close(fd);
		return false;
	}
	np_fd = fd;
	np_rlen = np_olen = 0;
	ui_watch(np_ui, fd, np_on_read);
	static const char *const props[] = { NULL, "pause", "media-title",
		"metadata/by-key/artist", "metadata/by-key/album", "duration", "path",
		"idle-active" };
	for (int i = P_PAUSE; i <= P_IDLE; i++) {
		char line[120];
		int n = snprintf(line, sizeof(line),
				 "{\"command\":[\"observe_property\",%d,\"%s\"]}\n", i, props[i]);
		np_write(line, n);
	}
	np_ask_time();
	return true;
}

/* The scrub moves on; sought every 250 ms */
static void scrub_seek(void)
{
	char t[32];

	snprintf(t, sizeof(t), "%.1f", scrub_pos);
	np_cmd("seek", t, "absolute");
	np_ask_time();
}

static void scrub_step(void)
{
	long long t = np_now_ms();

	if (!scrubbing) {
		if (t - scrub_since < 400)
			return;
		scrubbing = true;
		scrub_pos = ui_np.time;
		scrub_last = t;
	}
	double speed = t - scrub_since > 2400 ? 30 : 10;
	scrub_pos += (scrub_key == KEY_RIGHT ? speed : -speed) * (t - scrub_last) / 1000.0;
	scrub_last = t;
	if (ui_np.duration > 1 && scrub_pos > ui_np.duration - 1)
		scrub_pos = ui_np.duration - 1;
	if (scrub_pos < 0)
		scrub_pos = 0;
	if (t - scrub_sent >= 250) {
		scrub_seek();
		scrub_sent = t;
	}
	if (np_ui)
		ui_redraw(np_ui);
}

/* The timer: retrying the connection, the scrub, the time each second
 * while shown and playing; off otherwise */
static void np_arm(void)
{
	int ms = scrub_key ? 100 : np_retry ? 300 :
		 np_live && ui_np.on && !ui_np.paused ? 1000 : 0;
	struct itimerspec it = { .it_value = { ms / 1000, (ms % 1000) * 1000000L } };

	if (np_timer >= 0)
		timerfd_settime(np_timer, 0, &it, NULL);
}

static void np_on_timer(struct ui *ui, int fd)
{
	uint64_t n;

	if (read(fd, &n, sizeof(n)) < 0 && errno == EAGAIN)
		return;
	if (np_retry) {
		np_retry--;
		if (np_connect())
			np_retry = 0;
	}
	if (scrub_key)
		scrub_step();
	else if (np_live && ui_np.on && !ui_np.paused)
		np_ask_time();
	np_arm();
}

/* The socket appeared: mpv listens just after making it, so a few tries */
static void np_on_dir(struct ui *ui, int fd)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;
	bool ours = false;

	while ((n = read(fd, buf, sizeof(buf))) > 0)
		for (char *p = buf; p < buf + n;) {
			struct inotify_event *e = (struct inotify_event *)p;
			if (e->len && !strcmp(e->name, "s22-music.sock"))
				ours = true;
			p += sizeof(*e) + e->len;
		}
	if (ours && np_fd < 0 && !np_connect()) {
		np_retry = 10;
		np_arm();
	}
}

void ui_np_follow(struct ui *ui)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");

	if (np_ui)
		return;
	np_ui = ui;
	np_timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (np_timer >= 0)
		ui_watch(ui, np_timer, np_on_timer);
	np_in = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (np_in >= 0 && inotify_add_watch(np_in, dir ? dir : "/run/s22-gui",
					    IN_CREATE | IN_MOVED_TO) >= 0)
		ui_watch(ui, np_in, np_on_dir);
	np_connect();
}

void ui_np_live(struct ui *ui, bool live)
{
	np_live = live;
	if (live && ui_np.on)
		np_ask_time();
	np_arm();
}

void ui_np_toggle(void)
{
	np_cmd("cycle", "pause");
}

void ui_np_stop(void)
{
	np_cmd("stop", "keep-playlist");
}

void ui_np_skip(int dir)
{
	if (dir > 0)
		np_cmd("playlist-next");
	else if (ui_np.time > 3)
		np_cmd("seek", "0", "absolute");
	else
		np_cmd("playlist-prev");
	np_ask_time();
}

void ui_np_seek_to(double t)
{
	char s[32];

	snprintf(s, sizeof(s), "%.1f", t < 0 ? 0 : t);
	np_cmd("seek", s, "absolute");
	np_ask_time();
}

double ui_np_shown_time(void)
{
	return scrubbing ? scrub_pos : ui_np.time;
}

bool ui_np_key(struct ui *ui, uint32_t code, enum ui_key_state st)
{
	if (code != KEY_LEFT && code != KEY_RIGHT)
		return false;
	if (st == UI_PRESSED) {
		if (!ui_np.on)
			return false;
		scrub_key = code;
		scrubbing = false;
		scrub_since = np_now_ms();
		np_arm();
	} else if (st == UI_RELEASED && code == scrub_key) {
		bool was = scrubbing;
		if (scrubbing)
			scrub_seek();
		scrub_key = 0;
		scrubbing = false;
		if (!was)
			ui_np_skip(code == KEY_RIGHT ? 1 : -1);
		np_arm();
		ui_redraw(ui);
	}
	return true;
}

void ui_np_key_cancel(void)
{
	if (scrubbing)
		scrub_seek();
	scrub_key = 0;
	scrubbing = false;
	np_arm();
}

/* ---------------------------------------------------------- the covers --- */

static struct {
	char song[1024];	/* what it is for ("" none yet) */
	uint32_t *px;		/* decoded, XRGB; NULL: none (yet) */
	int w, h;
	uint32_t *thumb;
	int tsize;
	uint32_t *back;		/* the backdrop, as last asked */
	int bw, bh;
	struct ui_fade bf;
	uint32_t bbg;
} art;

struct jerr {
	struct jpeg_error_mgr mgr;
	jmp_buf jump;
};

static void jerr_exit(j_common_ptr ci)
{
	longjmp(((struct jerr *)ci->err)->jump, 1);
}

/* A JPEG to XRGB, at the smallest of 1/1 to 1/8 whose shorter side is
 * still min pixels */
static uint32_t *jpeg_decode(const unsigned char *buf, size_t len, int min, int *w, int *h)
{
	struct jpeg_decompress_struct ci;
	struct jerr jerr;
	uint32_t *volatile out = NULL;
	uint8_t *volatile row = NULL;

	ci.err = jpeg_std_error(&jerr.mgr);
	jerr.mgr.error_exit = jerr_exit;
	if (setjmp(jerr.jump)) {
		jpeg_destroy_decompress(&ci);
		free(out);
		free(row);
		return NULL;
	}
	jpeg_create_decompress(&ci);
	jpeg_mem_src(&ci, buf, len);
	jpeg_read_header(&ci, TRUE);
	if (ci.image_width > 10000 || ci.image_height > 10000) {
		jpeg_destroy_decompress(&ci);
		return NULL;
	}
	int side = ci.image_width < ci.image_height ? ci.image_width : ci.image_height;
	int denom = 1;
	while (denom < 8 && side / (denom * 2) >= min)
		denom *= 2;
	ci.scale_num = 1;
	ci.scale_denom = denom;
	ci.out_color_space = JCS_RGB;
	jpeg_start_decompress(&ci);
	*w = ci.output_width;
	*h = ci.output_height;
	out = malloc((size_t)*w * *h * 4);
	row = malloc((size_t)*w * 3);
	if (!out || !row)
		longjmp(jerr.jump, 1);
	while (ci.output_scanline < ci.output_height) {
		uint8_t *rp = row;
		uint32_t *o = out + (size_t)ci.output_scanline * *w;
		jpeg_read_scanlines(&ci, &rp, 1);
		for (int x = 0; x < *w; x++)
			o[x] = 0xff000000 | row[x * 3] << 16 | row[x * 3 + 1] << 8 | row[x * 3 + 2];
	}
	jpeg_finish_decompress(&ci);
	jpeg_destroy_decompress(&ci);
	free(row);
	return out;
}

/* All of a file, up to 16 MiB */
static unsigned char *slurp(const char *path, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t cap = 1 << 16, n = 0;
	unsigned char *buf = fd >= 0 ? malloc(cap) : NULL;
	ssize_t r;

	while (buf && (r = read(fd, buf + n, cap - n)) != 0) {
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		n += r;
		if (n == cap) {
			unsigned char *b = cap < 16 << 20 ? realloc(buf, cap *= 2) : NULL;
			if (!b)
				break;
			buf = b;
		}
	}
	if (fd >= 0)
		close(fd);
	*len = n;
	return buf;
}

static bool ext_is(const char *name, const char *const *list)
{
	const char *dot = strrchr(name, '.');

	for (int i = 0; dot && list[i]; i++)
		if (!strcasecmp(dot + 1, list[i]))
			return true;
	return false;
}

/* ffmpeg turning a song's picture (or a PNG) into a JPEG on a pipe, in the
 * background; five seconds at most */
static struct {
	struct ui *ui;
	pid_t pid;
	int fd;
	char song[1024];	/* the song it is for */
	unsigned char *buf;
	size_t len, cap;
} job = { .pid = -1, .fd = -1 };

static void job_end(void)
{
	int st;

	if (job.fd >= 0) {
		ui_unwatch(job.ui, job.fd);
		close(job.fd);
		job.fd = -1;
	}
	if (job.pid > 0) {
		kill(job.pid, SIGTERM);
		while (waitpid(job.pid, &st, 0) < 0 && errno == EINTR)
			;
		job.pid = -1;
	}
	free(job.buf);
	job.buf = NULL;
	job.len = job.cap = 0;
}

static void on_job(struct ui *ui, int fd)
{
	if (job.len == job.cap) {
		size_t cap = job.cap ? job.cap * 2 : 1 << 16;
		unsigned char *b = cap <= 16 << 20 ? realloc(job.buf, cap) : NULL;
		if (!b) {
			job_end();
			return;
		}
		job.buf = b;
		job.cap = cap;
	}
	ssize_t n = read(fd, job.buf + job.len, job.cap - job.len);
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n > 0) {
		job.len += n;
		return;
	}
	if (job.len > 4 && !strcmp(job.song, art.song) && !art.px)
		art.px = jpeg_decode(job.buf, job.len, 480, &art.w, &art.h);
	job_end();
	ui_redraw(ui);
}

static void job_start(struct ui *ui, const char *path)
{
	int fd[2];

	job_end();
	if (!ui || pipe2(fd, O_CLOEXEC) != 0)
		return;
	pid_t pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR);
		dup2(nul, 0);
		dup2(fd[1], 1);
		dup2(nul, 2);
		alarm(5);
		execlp("ffmpeg", "ffmpeg", "-v", "error", "-nostdin", "-i", path, "-an",
		       "-map", "0:v:0", "-frames:v", "1", "-c:v", "mjpeg", "-f", "image2pipe",
		       "-", (char *)NULL);
		_exit(127);
	}
	close(fd[1]);
	fcntl(fd[0], F_SETFL, fcntl(fd[0], F_GETFL) | O_NONBLOCK);
	job.ui = ui;
	job.pid = pid;
	job.fd = fd[0];
	snprintf(job.song, sizeof(job.song), "%s", art.song);
	if (!ui_watch(ui, fd[0], on_job))
		job_end();
}

/* The folder's cover: the names first, else any JPEG or PNG */
static bool folder_image(const char *dir, char *out, size_t n)
{
	static const char *const names[] = { "albumart.jpg", "cover.jpg", "folder.jpg",
					     "front.jpg", NULL };
	DIR *d = opendir(dir);
	struct dirent *de;
	int best = 99;

	while (d && (de = readdir(d))) {
		int rank = 99;
		for (int i = 0; names[i]; i++)
			if (!strcasecmp(de->d_name, names[i]))
				rank = i;
		if (rank == 99 && de->d_name[0] != '.' &&
		    ext_is(de->d_name, (const char *const[]){ "jpg", "jpeg", "png", NULL }))
			rank = 50;
		if (rank < best) {
			best = rank;
			snprintf(out, n, "%s/%s", dir, de->d_name);
		}
	}
	if (d)
		closedir(d);
	return best < 99;
}

/* The cover of song, found and decoded if it is a new song */
static void art_for(struct ui *ui, const char *song)
{
	char dir[1024], img[1300];

	if (!strcmp(art.song, song))
		return;
	free(art.px);
	free(art.thumb);
	free(art.back);
	art.px = art.thumb = art.back = NULL;
	job_end();
	snprintf(art.song, sizeof(art.song), "%s", song);
	if (!song[0] || strstr(song, "://"))
		return;
	snprintf(dir, sizeof(dir), "%s", song);
	char *slash = strrchr(dir, '/');
	if (slash)
		*slash = 0;
	bool found = folder_image(dir, img, sizeof(img));
	bool png = found && ext_is(img, (const char *const[]){ "png", NULL });
	if (found && !png) {
		size_t len;
		unsigned char *buf = slurp(img, &len);
		if (buf)
			art.px = jpeg_decode(buf, len, 480, &art.w, &art.h);
		free(buf);
		if (art.px)
			return;
	}
	job_start(ui, png ? img : song);
}

/* src scaled to fill dw x dh, cropped to fit (from the top, or the middle),
 * each pixel the average of what it covers */
static uint32_t *scale_fill(const uint32_t *src, int sw, int sh, int dw, int dh, bool top)
{
	uint32_t *dst = malloc((size_t)dw * dh * 4);
	double s = (double)dw / sw > (double)dh / sh ? (double)dw / sw : (double)dh / sh;
	double ox = (sw - dw / s) / 2, oy = top ? 0 : (sh - dh / s) / 2;

	for (int y = 0; dst && y < dh; y++) {
		int y0 = (int)(oy + y / s), y1 = (int)(oy + (y + 1) / s);
		y0 = y0 < 0 ? 0 : y0 >= sh ? sh - 1 : y0;
		y1 = y1 <= y0 ? y0 + 1 : y1 > sh ? sh : y1;
		for (int x = 0; x < dw; x++) {
			int x0 = (int)(ox + x / s), x1 = (int)(ox + (x + 1) / s);
			x0 = x0 < 0 ? 0 : x0 >= sw ? sw - 1 : x0;
			x1 = x1 <= x0 ? x0 + 1 : x1 > sw ? sw : x1;
			unsigned r = 0, g = 0, b = 0, n = 0;
			for (int yy = y0; yy < y1; yy++)
				for (int xx = x0; xx < x1; xx++) {
					uint32_t p = src[(size_t)yy * sw + xx];
					r += p >> 16 & 255;
					g += p >> 8 & 255;
					b += p & 255;
					n++;
				}
			dst[(size_t)y * dw + x] = 0xff000000 | r / n << 16 | g / n << 8 | b / n;
		}
	}
	return dst;
}

/* a toward b, by t of 255 */
static uint32_t mix(uint32_t a, uint32_t b, int t)
{
	uint32_t r = ((a >> 16 & 255) * (255 - t) + (b >> 16 & 255) * t) / 255;
	uint32_t g = ((a >> 8 & 255) * (255 - t) + (b >> 8 & 255) * t) / 255;
	uint32_t bl = ((a & 255) * (255 - t) + (b & 255) * t) / 255;

	return 0xff000000 | r << 16 | g << 8 | bl;
}

static double smooth(double u)
{
	u = u < 0 ? 0 : u > 1 ? 1 : u;
	return u * u * (3 - 2 * u);
}

const uint32_t *ui_art_thumb(struct ui *ui, const char *song, int size)
{
	art_for(ui, song);
	if (!art.px || size <= 0)
		return NULL;
	if (!art.thumb || art.tsize != size) {
		free(art.thumb);
		art.thumb = scale_fill(art.px, art.w, art.h, size, size, false);
		art.tsize = size;
	}
	return art.thumb;
}

const uint32_t *ui_art_backdrop(struct ui *ui, const char *song, int w, int h,
				const struct ui_fade *f)
{
	art_for(ui, song);
	if (!art.px || w <= 0 || h <= 0)
		return NULL;
	if (art.back && art.bw == w && art.bh == h && art.bbg == UI_BG &&
	    !memcmp(&art.bf, f, sizeof(*f)))
		return art.back;
	free(art.back);
	art.back = scale_fill(art.px, art.w, art.h, w, h, true);
	art.bw = w;
	art.bh = h;
	art.bf = *f;
	art.bbg = UI_BG;
	for (int y = 0; art.back && y < h; y++) {
		double a = f->mid_a;
		if (y < f->solid)
			a = f->top_a;
		else if (f->ease > 0 && y < f->solid + f->ease)
			a = f->top_a + (f->mid_a - f->top_a) * smooth((double)(y - f->solid) / f->ease);
		if (y >= f->bottom && h > f->bottom) {
			double b = f->mid_a + (f->bottom_a - f->mid_a) *
				   smooth((double)(y - f->bottom) / (h - f->bottom));
			a = a > b ? a : b;
		}
		int k = (int)a;
		if (k > 0)
			for (int x = 0; x < w; x++)
				art.back[(size_t)y * w + x] = mix(art.back[(size_t)y * w + x], UI_BG, k);
	}
	return art.back;
}

void ui_blit(struct ui_canvas *c, int x, int y, const uint32_t *px, int w, int h)
{
	for (int j = 0; j < h; j++) {
		if (y + j < 0 || y + j >= c->h)
			continue;
		for (int i = 0; i < w; i++)
			if (x + i >= 0 && x + i < c->w)
				c->px[(size_t)(y + j) * c->stride + x + i] = px[(size_t)j * w + i];
	}
}

void ui_dim(struct ui_canvas *c, int x, int y, int w, int h, uint32_t color, int alpha)
{
	for (int j = y < 0 ? 0 : y; j < y + h && j < c->h; j++)
		for (int i = x < 0 ? 0 : x; i < x + w && i < c->w; i++) {
			uint32_t *p = &c->px[(size_t)j * c->stride + i];
			*p = mix(*p, color, alpha);
		}
}

void ui_vinyl(struct ui_canvas *c, int cx, int cy, int r)
{
	int rim = r > 40 ? 2 : 1;

	ui_round(c, cx - r, cy - r, 2 * r, 2 * r, r, 0xff4a4a4a);	/* the edge */
	r -= rim;
	ui_round(c, cx - r, cy - r, 2 * r, 2 * r, r, 0xff262626);
	for (int k = 0; k < 4; k++) {	/* grooves */
		int g = r * (88 - k * 9) / 100;
		ui_round(c, cx - g, cy - g, 2 * g, 2 * g, g, 0xff3c3c3c);
		g -= rim;
		ui_round(c, cx - g, cy - g, 2 * g, 2 * g, g, 0xff262626);
	}
	int l = r * 36 / 100, hole = r > 40 ? r * 5 / 100 : 2;
	ui_round(c, cx - l, cy - l, 2 * l, 2 * l, l, UI_ACCENT);
	ui_round(c, cx - hole, cy - hole, 2 * hole, 2 * hole, hole, UI_BG);
}

/* ------------------------------------------------- text over a cover --- */

/*
 * A halo: the text drawn white into a mask, blurred (a box blur, twice
 * each way, near a gaussian), and the picture under it blended toward
 * color by that, before the text itself goes on top. Text then reads over
 * any part of any cover; color is the theme's background, so the halo is
 * dark under light text and light under dark.
 */
#define HALO_R	5

static uint32_t *mask;
static int mask_w, mask_h;
static uint16_t *blur_a, *blur_b;

static bool halo_mask(struct ui_canvas *c, struct ui_canvas *m)
{
	if (!mask || mask_w != c->w || mask_h != c->h) {
		free(mask);
		free(blur_a);
		free(blur_b);
		mask = malloc((size_t)c->w * c->h * 4);
		blur_a = malloc((size_t)c->w * c->h * 2);
		blur_b = malloc((size_t)c->w * c->h * 2);
		if (!mask || !blur_a || !blur_b) {
			free(mask);
			free(blur_a);
			free(blur_b);
			mask = NULL;
			blur_a = blur_b = NULL;
			return false;
		}
		mask_w = c->w;
		mask_h = c->h;
	}
	*m = (struct ui_canvas){ mask, c->w, c->h, c->w };
	return true;
}

/* One pass of a box blur, radius r, along x (dx 1) or y */
static void box(const uint16_t *src, uint16_t *dst, int x0, int y0, int x1, int y1, int r,
		bool along_x)
{
	for (int j = y0; j < y1; j++)
		for (int i = x0; i < x1; i++) {
			unsigned sum = 0, n = 0;
			for (int k = -r; k <= r; k++) {
				int x = along_x ? i + k : i, y = along_x ? j : j + k;
				if (x < x0 || x >= x1 || y < y0 || y >= y1)
					continue;
				sum += src[(size_t)y * mask_w + x];
				n++;
			}
			dst[(size_t)j * mask_w + i] = sum / n;
		}
}

/* The halo under the mask's region, then clear it */
static void halo_apply(struct ui_canvas *c, int x0, int y0, int x1, int y1, uint32_t color)
{
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > c->w ? c->w : x1;
	y1 = y1 > c->h ? c->h : y1;
	for (int j = y0; j < y1; j++)
		for (int i = x0; i < x1; i++)
			blur_a[(size_t)j * mask_w + i] = mask[(size_t)j * mask_w + i] & 255;
	for (int pass = 0; pass < 2; pass++) {
		box(blur_a, blur_b, x0, y0, x1, y1, HALO_R, true);
		box(blur_b, blur_a, x0, y0, x1, y1, HALO_R, false);
	}
	for (int j = y0; j < y1; j++)
		for (int i = x0; i < x1; i++) {
			int a = blur_a[(size_t)j * mask_w + i] * 3;	/* thin strokes blur faint */
			if (a > 215)
				a = 215;
			if (a > 0) {
				uint32_t *p = &c->px[(size_t)j * c->stride + i];
				*p = mix(*p, color, a);
			}
		}
}

static void mask_clear(int x0, int y0, int x1, int y1)
{
	for (int j = y0 < 0 ? 0 : y0; j < y1 && j < mask_h; j++)
		for (int i = x0 < 0 ? 0 : x0; i < x1 && i < mask_w; i++)
			mask[(size_t)j * mask_w + i] = 0;
}

void ui_text_box_halo(struct ui_canvas *c, int x, int y, int w, enum ui_font font, int size,
		      uint32_t color, enum ui_align align, const char *text)
{
	struct ui_canvas m;
	int h = ui_line_height(size), r = 3 * HALO_R;

	if (halo_mask(c, &m)) {
		mask_clear(x - r, y - r, x + w + r, y + h + r);
		ui_text_box(&m, x, y, w, font, size, 0xffffffff, align, text);
		halo_apply(c, x - r, y - r, x + w + r, y + h + r, UI_BG);
	}
	ui_text_box(c, x, y, w, font, size, color, align, text);
}

int ui_text_wrap_halo(struct ui_canvas *c, int x, int y, int w, enum ui_font font, int size,
		      uint32_t color, const char *text)
{
	struct ui_canvas m;
	int h = ui_text_wrap(NULL, x, y, w, font, size, color, text), r = 3 * HALO_R;

	if (halo_mask(c, &m)) {
		mask_clear(x - r, y - r, x + w + r, y + h + r);
		ui_text_wrap(&m, x, y, w, font, size, 0xffffffff, text);
		halo_apply(c, x - r, y - r, x + w + r, y + h + r, UI_BG);
	}
	return ui_text_wrap(c, x, y, w, font, size, color, text);
}

/* ------------------------------------------------------------ the card --- */

/* A rounded rectangle blended at alpha (of 255), edges antialiased; with
 * border, only a ring that wide */
static void rrect(struct ui_canvas *c, int x, int y, int w, int h, int r, uint32_t color,
		  int alpha, int border)
{
	for (int j = 0; j < h; j++) {
		if (y + j < 0 || y + j >= c->h)
			continue;
		for (int i = 0; i < w; i++) {
			if (x + i < 0 || x + i >= c->w)
				continue;
			/* distance outside the rounded rectangle, from the inside */
			double px = i + 0.5, py = j + 0.5;
			double qx = (px < w / 2.0 ? w / 2.0 - px : px - w / 2.0) - (w / 2.0 - r);
			double qy = (py < h / 2.0 ? h / 2.0 - py : py - h / 2.0) - (h / 2.0 - r);
			double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
			double d = __builtin_sqrt(ox * ox + oy * oy) +
				   (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
			double cov = 0.5 - d;
			cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
			if (border > 0) {	/* less what is inside the ring */
				double in = 0.5 - (d + border);
				cov -= in < 0 ? 0 : in > 1 ? 1 : in;
			}
			int a = (int)(cov * alpha);
			if (a > 0) {
				uint32_t *p = &c->px[(size_t)(y + j) * c->stride + x + i];
				*p = mix(*p, color, a);
			}
		}
	}
}

void ui_round_alpha(struct ui_canvas *c, int x, int y, int w, int h, int r, uint32_t color,
		    int alpha)
{
	rrect(c, x, y, w, h, r, color, alpha, 0);
}

/* A triangle, antialiased (4 x 4 samples a pixel) */
static void tri(struct ui_canvas *c, double ax, double ay, double bx, double by, double cx,
		double cy, uint32_t color)
{
	int x0 = (int)(ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx));
	int x1 = (int)(ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx)) + 1;
	int y0 = (int)(ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy));
	int y1 = (int)(ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy)) + 1;
	double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);

	for (int y = y0; y <= y1; y++) {
		if (y < 0 || y >= c->h)
			continue;
		for (int x = x0; x <= x1; x++) {
			if (x < 0 || x >= c->w)
				continue;
			int in = 0;
			for (int s = 0; s < 16; s++) {
				double px = x + (s % 4 + 0.5) / 4, py = y + (s / 4 + 0.5) / 4;
				double w0 = (bx - px) * (cy - py) - (by - py) * (cx - px);
				double w1 = (cx - px) * (ay - py) - (cy - py) * (ax - px);
				double w2 = (ax - px) * (by - py) - (ay - py) * (bx - px);
				if (area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) :
					       (w0 <= 0 && w1 <= 0 && w2 <= 0))
					in++;
			}
			if (in) {
				uint32_t *p = &c->px[(size_t)y * c->stride + x];
				*p = mix(*p, color, in * 255 / 16);
			}
		}
	}
}

void ui_np_icon(struct ui_canvas *c, int cx, int cy, int s, int which, uint32_t color)
{
	double h = s / 2.0;

	switch (which) {
	case UI_NP_PLAY:
		tri(c, cx - h * 0.7, cy - h, cx - h * 0.7, cy + h, cx + h, cy, color);
		break;
	case UI_NP_PAUSE:
		ui_fill(c, cx - h * 0.8, cy - h, h * 0.55, s, color);
		ui_fill(c, cx + h * 0.25, cy - h, h * 0.55, s, color);
		break;
	case UI_NP_PREV:
		ui_fill(c, cx - h, cy - h, h * 0.3, s, color);
		tri(c, cx + h, cy - h, cx + h, cy + h, cx - h * 0.65, cy, color);
		break;
	case UI_NP_NEXT:
		ui_fill(c, cx + h * 0.7, cy - h, h * 0.3, s, color);
		tri(c, cx - h, cy - h, cx - h, cy + h, cx + h * 0.65, cy, color);
		break;
	}
}

static void fmt_time(double t, char *buf, size_t n)
{
	int s = t > 0 ? (int)t : 0;

	if (s >= 3600)
		snprintf(buf, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(buf, n, "%d:%02d", s / 60, s % 60);
}

#define CARD_THUMB	64

void ui_np_card(struct ui *ui, struct ui_canvas *c, int x, int y, int w, bool sel, int alpha)
{
	char a[24], b[24];
	int h = UI_NP_CARD_H, cx = x + w / 2;

	rrect(c, x, y, w, h, 14, UI_BAR, alpha, 0);
	if (sel)
		rrect(c, x, y, w, h, 14, UI_ACCENT, 255, 3);

	/* The cover (or a record), title and artist */
	const uint32_t *th = ui_art_thumb(ui, ui_np.path, CARD_THUMB);
	if (th)
		ui_blit(c, x + 12, y + 12, th, CARD_THUMB, CARD_THUMB);
	else
		ui_vinyl(c, x + 12 + CARD_THUMB / 2, y + 12 + CARD_THUMB / 2, CARD_THUMB / 2 - 1);
	int tx = x + 12 + CARD_THUMB + 12;
	ui_text_box(c, tx, y + 16, x + w - 12 - tx, UI_BOLD, 18, UI_FG, UI_LEFT,
		    ui_np.title[0] ? ui_np.title : "…");
	ui_text_box(c, tx, y + 44, x + w - 12 - tx, UI_SANS, 15, UI_DIM, UI_LEFT, ui_np.artist);

	/* Before, play or pause, after */
	int by = y + 100;
	ui_np_icon(c, cx - 80, by, 18, UI_NP_PREV, UI_FG);
	ui_round(c, cx - 22, by - 22, 44, 44, 22, UI_ACCENT);
	ui_np_icon(c, ui_np.paused ? cx + 2 : cx, by, 18, ui_np.paused ? UI_NP_PLAY : UI_NP_PAUSE,
		   UI_BG);
	ui_np_icon(c, cx + 80, by, 18, UI_NP_NEXT, UI_FG);

	/* Where it is: the bar from the left edge to the right, the times
	 * above its ends */
	double t = ui_np_shown_time(), f = ui_np.duration > 0 ? t / ui_np.duration : 0;
	f = f < 0 ? 0 : f > 1 ? 1 : f;
	int bx = x + 12, bw = w - 24, py = y + h - 14;
	ui_fill(c, bx, py, bw, 4, UI_SEL);
	ui_fill(c, bx, py, (int)(bw * f), 4, UI_ACCENT);
	fmt_time(t, a, sizeof(a));
	fmt_time(ui_np.duration, b, sizeof(b));
	ui_text(c, bx, by - 8, UI_SANS, 13, UI_DIM, a);
	ui_text(c, x + w - 12 - ui_text_width(UI_SANS, 13, b), by - 8, UI_SANS, 13, UI_DIM, b);
}

bool ui_np_card_tap(struct ui *ui, int x, int y, int w, int tx, int ty)
{
	int h = UI_NP_CARD_H, cx = x + w / 2;

	if (tx < x || tx >= x + w || ty < y || ty >= y + h)
		return false;
	if (ty >= y + h - 26 && ui_np.duration > 0) {		/* the bar */
		double f = (double)(tx - x - 12) / (w - 24);
		ui_np_seek_to((f < 0 ? 0 : f > 1 ? 1 : f) * ui_np.duration);
	} else if (ty >= y + 76) {				/* the buttons */
		if (tx < cx - 40)
			ui_np_skip(-1);
		else if (tx > cx + 40)
			ui_np_skip(1);
		else
			ui_np_toggle();
	}
	ui_redraw(ui);
	return true;
}
