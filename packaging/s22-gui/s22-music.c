// SPDX-License-Identifier: MIT
/*
 * s22-music: music from ~/Music (D055).
 *
 * The playing is s22-musicd's (mpv, idle in the GUI session, on its JSON
 * IPC socket): this app is its remote, so the music goes on when the app
 * closes (Home closes apps), and the outer screen's keys reach it over
 * MPRIS. mpv plays nearly any audio format (through ffmpeg) and reads
 * .m3u, .m3u8 and .pls playlists itself.
 *
 * Six lists, Left/Right between them:
 *   Folders    ~/Music as folders; OK on a song plays its folder from it,
 *              on a folder opens it, on a playlist plays it
 *   Artists    everyone the tags name (D083); OK opens one: all their
 *              songs, their albums, then the albums they are on
 *   Albums     every album by its tags; OK opens one, OK on a song plays
 *              the album from it
 *   Songs      every song in ~/Music, however deep, by name without the
 *              track number; OK plays them all from that song (D058)
 *   Playlists  ~/Music/Playlists (.m3u8, made here) and any playlist in
 *              ~/Music; OK opens one: OK plays from a song, C removes it,
 *              * moves it; the first row makes a new playlist
 *   Queue      what plays, in order; OK jumps to a song, C removes it
 * In any list: 4/6 a page up or down, 1/3 the letter before or after (in
 * the lists by name), # the now playing screen, * what can be done with the
 * song, folder, artist, album or playlist chosen (play next, add to the
 * queue, add to a playlist, shuffle a folder, ...), Back up a folder or out
 * of a playlist, artist or album, and closes the app from the top.
 *
 * A bar above the hints, while a song is loaded: its progress, title,
 * artist and time; a tap opens now playing, 5 in a list plays or pauses.
 *
 * Opened with a song loaded (playing or paused), it opens on now playing.
 *
 * Now playing: OK or 5 play/pause, C stop (the queue kept), Left/Right the
 * song before or after (held: scrubbing), 4/6 ten seconds back or ahead, 1/3 a minute, 7
 * shuffle, 9 repeat (all, one, off), * add the song to a playlist, Back
 * the list it came from. A tap on the bar seeks there, elsewhere plays or
 * pauses.
 *
 * Our playlists are .m3u8 files in ~/Music/Playlists, songs in ~/Music
 * written relative to it (../Album/song.flac), so they move with the folder.
 * Playlists written on Windows read too: backslashes, and paths on the
 * drive letter the share was mapped as (its root is the home folder).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "s22ui.h"

#define MAX_ITEMS	4000
#define ROW_H		56
#define ROW2_H		62		/* a row with a second, dimmer line */

/* ------------------------------------------------------------- JSON --- */

/* Enough JSON for mpv's answers: a tree, freed in one go */
enum jt { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
struct jv {
	enum jt t;
	bool b;
	double n;
	char *s;
	int len;		/* items (array) or members (object) */
	struct jv *items;
	char **keys;
};

static void jv_free(struct jv *v)
{
	if (!v)
		return;
	free(v->s);
	for (int i = 0; i < v->len; i++) {
		jv_free(&v->items[i]);
		if (v->keys)
			free(v->keys[i]);
	}
	free(v->items);
	free(v->keys);
}

static void ws(const char **p)
{
	while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')
		(*p)++;
}

static char *jstr(const char **p)
{
	size_t cap = 32, n = 0;
	char *out = malloc(cap);

	(*p)++;			/* the opening quote */
	while (out && **p && **p != '"') {
		char c = **p;
		if (c == '\\') {
			(*p)++;
			switch (**p) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'u': {	/* to UTF-8; surrogate pairs as '?' */
				unsigned u = 0;
				for (int i = 1; i <= 4 && isxdigit((unsigned char)(*p)[i]); i++)
					u = u * 16 + (isdigit((unsigned char)(*p)[i]) ? (*p)[i] - '0' :
						      (tolower((unsigned char)(*p)[i]) - 'a' + 10));
				*p += 4;
				char b[4];
				int k = 0;
				if (u >= 0xd800 && u < 0xe000)
					b[k++] = '?';
				else if (u < 0x80)
					b[k++] = u;
				else if (u < 0x800) {
					b[k++] = 0xc0 | u >> 6;
					b[k++] = 0x80 | (u & 63);
				} else {
					b[k++] = 0xe0 | u >> 12;
					b[k++] = 0x80 | ((u >> 6) & 63);
					b[k++] = 0x80 | (u & 63);
				}
				for (int i = 0; i < k; i++) {
					if (n + 2 > cap)
						out = realloc(out, cap *= 2);
					if (out)
						out[n++] = b[i];
				}
				(*p)++;
				continue;
			}
			default: c = **p; break;	/* \" \\ \/ */
			}
		}
		if (n + 2 > cap)
			out = realloc(out, cap *= 2);
		if (out)
			out[n++] = c;
		(*p)++;
	}
	if (**p == '"')
		(*p)++;
	if (out)
		out[n] = 0;
	return out;
}

static bool jparse(const char **p, struct jv *v, int depth)
{
	memset(v, 0, sizeof(*v));
	ws(p);
	if (depth > 32)
		return false;
	if (**p == '"') {
		v->t = J_STR;
		v->s = jstr(p);
		return v->s != NULL;
	}
	if (**p == '{' || **p == '[') {
		bool obj = **p == '{';
		char close = obj ? '}' : ']';
		v->t = obj ? J_OBJ : J_ARR;
		(*p)++;
		ws(p);
		while (**p && **p != close) {
			char *key = NULL;
			if (obj) {
				if (**p != '"')
					return false;
				key = jstr(p);
				ws(p);
				if (**p != ':') {
					free(key);
					return false;
				}
				(*p)++;
			}
			v->items = realloc(v->items, (v->len + 1) * sizeof(*v->items));
			if (obj)
				v->keys = realloc(v->keys, (v->len + 1) * sizeof(*v->keys));
			if (!v->items || (obj && !v->keys)) {
				free(key);
				return false;
			}
			if (obj)
				v->keys[v->len] = key;
			if (!jparse(p, &v->items[v->len], depth + 1)) {
				v->len++;
				return false;
			}
			v->len++;
			ws(p);
			if (**p == ',') {
				(*p)++;
				ws(p);
			}
		}
		if (**p == close)
			(*p)++;
		return true;
	}
	if (!strncmp(*p, "true", 4) || !strncmp(*p, "false", 5)) {
		v->t = J_BOOL;
		v->b = **p == 't';
		*p += v->b ? 4 : 5;
		return true;
	}
	if (!strncmp(*p, "null", 4)) {
		*p += 4;
		return true;
	}
	char *end;
	v->t = J_NUM;
	v->n = strtod(*p, &end);
	if (end == *p)
		return false;
	*p = end;
	return true;
}

static const struct jv *jget(const struct jv *o, const char *key)
{
	for (int i = 0; o && o->t == J_OBJ && i < o->len; i++)
		if (o->keys[i] && !strcmp(o->keys[i], key))
			return &o->items[i];
	return NULL;
}

/* A string for mpv's JSON: quoted, escaped */
static void jquote(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fprintf(f, "\\%c", *s);
		else if ((unsigned char)*s < 0x20)
			fprintf(f, "\\u%04x", (unsigned char)*s);
		else
			fputc(*s, f);
	}
	fputc('"', f);
}

/* ------------------------------------------------------- the player --- */

enum { P_PAUSE = 1, P_TITLE, P_ARTIST, P_ALBUM, P_DURATION, P_PPOS, P_PCOUNT,
       P_LOOP_PL, P_LOOP_FILE, P_IDLE, P_PATH };
enum { R_TIME = 100, R_PLAYLIST };

static struct {
	bool paused, idle, loop_pl, loop_file, shuffle;
	char title[256], artist[256], album[256], path[1024];
	double duration, time;
	int ppos, pcount;
} pl = { .idle = true, .ppos = -1 };

static int mpv_fd = -1;
static char rbuf[1 << 20];		/* a line: the whole queue, at 4000 songs */
static size_t rlen;
static int connect_tries;
static struct ui *mpv_ui;

/* What is still to go to mpv. The socket never blocks: a write that
 * blocked while mpv's answers filled the other way hung both (D058) */
static char *obuf;
static size_t olen, ocap;

/* The queue, as mpv last told it */
static char (*queue)[256];
static char (*queue_path)[1024];
static int nqueue, queue_cur = -1;
static bool queue_stale;

static void queue_refresh(void);

static void mpv_drop(void)
{
	if (mpv_fd < 0)
		return;
	ui_unwatch(mpv_ui, mpv_fd);
	close(mpv_fd);
	mpv_fd = -1;
	olen = 0;
	pl.idle = true;
	ui_redraw(mpv_ui);
}

/* As much as the socket takes now; the timer tries the rest */
static void mpv_flush(void)
{
	while (mpv_fd >= 0 && olen) {
		ssize_t n = write(mpv_fd, obuf, olen);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			ui_timer(mpv_ui, 50);
			return;
		}
		if (n <= 0) {
			mpv_drop();
			return;
		}
		memmove(obuf, obuf + n, olen - n);
		olen -= n;
	}
}

static void mpv_write(const char *line)
{
	size_t n = strlen(line);

	if (mpv_fd < 0)
		return;
	if (olen + n > ocap) {
		size_t cap = ocap ? ocap : 4096;
		while (cap < olen + n)
			cap *= 2;
		char *b = realloc(obuf, cap);
		if (!b) {
			mpv_drop();
			return;
		}
		obuf = b;
		ocap = cap;
	}
	memcpy(obuf + olen, line, n);
	olen += n;
	mpv_flush();
}

/* {"command":[ARGS...]} with an optional request_id */
static void mpv_cmdv(int id, const char *const *args)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&buf, &len);

	if (!f)
		return;
	fputs("{\"command\":[", f);
	for (int i = 0; args[i]; i++) {
		if (i)
			fputc(',', f);
		jquote(f, args[i]);
	}
	fputc(']', f);
	if (id)
		fprintf(f, ",\"request_id\":%d", id);
	fputs("}\n", f);
	fclose(f);
	mpv_write(buf);
	free(buf);
}

#define mpv_cmd(...) mpv_cmdv(0, (const char *const[]){ __VA_ARGS__, NULL })

static void mpv_observe(int id, const char *prop)
{
	char line[160];

	snprintf(line, sizeof(line), "{\"command\":[\"observe_property\",%d,\"%s\"]}\n", id, prop);
	mpv_write(line);
}

static void on_mpv(struct ui *ui, int fd);

static bool mpv_connect(struct ui *ui)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un sa = { .sun_family = AF_UNIX };

	if (mpv_fd >= 0)
		return true;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/s22-music.sock", dir ? dir : "/run/s22-gui");
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0) {
		close(fd);
		return false;
	}
	mpv_fd = fd;
	mpv_ui = ui;
	rlen = 0;
	olen = 0;
	ui_watch(ui, fd, on_mpv);
	mpv_observe(P_PAUSE, "pause");
	mpv_observe(P_TITLE, "media-title");
	mpv_observe(P_ARTIST, "metadata/by-key/artist");
	mpv_observe(P_ALBUM, "metadata/by-key/album");
	mpv_observe(P_DURATION, "duration");
	mpv_observe(P_PPOS, "playlist-pos");
	mpv_observe(P_PCOUNT, "playlist-count");
	mpv_observe(P_LOOP_PL, "loop-playlist");
	mpv_observe(P_LOOP_FILE, "loop-file");
	mpv_observe(P_IDLE, "idle-active");
	mpv_observe(P_PATH, "path");
	/* Where the song is: the clock only asks while it plays, so a paused
	 * one showed 0:00 until it played again */
	mpv_cmdv(R_TIME, (const char *const[]){ "get_property", "time-pos", NULL });
	queue_refresh();
	return true;
}

/* Connected: at once, or after starting the player (or waiting for the
 * one starting), for up to 4 s */
static bool mpv_ready(struct ui *ui)
{
	if (mpv_connect(ui))
		return true;
	if (connect_tries == 0)
		ui_spawn("s22-musicd");
	for (int i = 0; i < 40; i++) {
		usleep(100000);
		if (mpv_connect(ui)) {
			connect_tries = 0;
			return true;
		}
	}
	return false;
}

/* Just opened: the player's first word on what is loaded decides the view */
static bool opening = true;
static void opened(struct ui *ui);

static void copy_str(char *dst, size_t n, const struct jv *v)
{
	snprintf(dst, n, "%s", v && v->t == J_STR ? v->s : "");
}

static void on_event(struct ui *ui, const struct jv *ev)
{
	const struct jv *id = jget(ev, "id"), *data = jget(ev, "data");
	const struct jv *rid = jget(ev, "request_id");

	if (rid && rid->t == J_NUM) {
		if ((int)rid->n == R_TIME && data && data->t == J_NUM)
			pl.time = data->n;
		if ((int)rid->n == R_PLAYLIST && data && data->t == J_ARR) {
			nqueue = 0;
			queue_cur = -1;
			for (int i = 0; i < data->len && i < MAX_ITEMS; i++) {
				const struct jv *e = &data->items[i];
				const struct jv *t = jget(e, "title"), *fn = jget(e, "filename");
				const struct jv *cur = jget(e, "current");
				const char *name = t && t->t == J_STR ? t->s :
					fn && fn->t == J_STR ? fn->s : "?";
				const char *slash = strrchr(name, '/');
				if (!(t && t->t == J_STR) && slash)
					name = slash + 1;
				snprintf(queue[nqueue], sizeof(queue[0]), "%s", name);
				snprintf(queue_path[nqueue], sizeof(queue_path[0]), "%s",
					 fn && fn->t == J_STR ? fn->s : "");
				if (!(t && t->t == J_STR)) {
					char *dot = strrchr(queue[nqueue], '.');
					if (dot && dot != queue[nqueue])
						*dot = 0;
				}
				if (cur && cur->t == J_BOOL && cur->b)
					queue_cur = nqueue;
				nqueue++;
			}
		}
		ui_redraw(ui);
		return;
	}
	const struct jv *evn = jget(ev, "event");
	if (!evn || evn->t != J_STR || strcmp(evn->s, "property-change") || !id)
		return;
	switch ((int)id->n) {
	case P_PAUSE:
		pl.paused = data && data->t == J_BOOL && data->b;
		if (!pl.paused)
			ui_timer(ui, 100);	/* the bar's clock, again */
		else			/* where it stopped, paused from anywhere */
			mpv_cmdv(R_TIME, (const char *const[]){ "get_property", "time-pos", NULL });
		break;
	case P_TITLE: copy_str(pl.title, sizeof(pl.title), data); break;
	case P_ARTIST: copy_str(pl.artist, sizeof(pl.artist), data); break;
	case P_ALBUM: copy_str(pl.album, sizeof(pl.album), data); break;
	case P_DURATION: pl.duration = data && data->t == J_NUM ? data->n : 0; break;
	case P_PPOS:
		pl.ppos = data && data->t == J_NUM ? (int)data->n : -1;
		queue_stale = true;
		break;
	case P_PCOUNT:
		pl.pcount = data && data->t == J_NUM ? (int)data->n : 0;
		queue_stale = true;
		break;
	case P_LOOP_PL:
		pl.loop_pl = data && ((data->t == J_STR && strcmp(data->s, "no")) ||
				      (data->t == J_BOOL && data->b));
		break;
	case P_LOOP_FILE:
		pl.loop_file = data && ((data->t == J_STR && strcmp(data->s, "no")) ||
					(data->t == J_BOOL && data->b));
		break;
	case P_IDLE: pl.idle = data && data->t == J_BOOL && data->b; break;
	case P_PATH:
		copy_str(pl.path, sizeof(pl.path), data);
		mpv_cmdv(R_TIME, (const char *const[]){ "get_property", "time-pos", NULL });
		/* Observed last: everything else is known by now */
		if (opening) {
			opening = false;
			opened(ui);
		}
		break;
	}
	ui_redraw(ui);
}

static void on_mpv(struct ui *ui, int fd)
{
	ssize_t n = read(fd, rbuf + rlen, sizeof(rbuf) - 1 - rlen);

	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return;
	if (n <= 0) {
		mpv_drop();
		return;
	}
	rlen += n;
	rbuf[rlen] = 0;
	char *start = rbuf, *nl;
	while ((nl = strchr(start, '\n'))) {
		*nl = 0;
		const char *p = start;
		struct jv v;
		if (jparse(&p, &v, 0))
			on_event(ui, &v);
		jv_free(&v);
		start = nl + 1;
	}
	rlen -= start - rbuf;
	memmove(rbuf, start, rlen);
	if (rlen >= sizeof(rbuf) - 1)
		rlen = 0;		/* a line too long: dropped */
	/* The queue asked for once per batch of changes, not once per song */
	if (queue_stale) {
		queue_stale = false;
		queue_refresh();
	}
	mpv_flush();
}

static void queue_refresh(void)
{
	mpv_cmdv(R_PLAYLIST, (const char *const[]){ "get_property", "playlist", NULL });
}

/* Songs for mpv's loadlist: one command however many there are (D058).
 * Four files in turn, so one mpv has yet to read is not rewritten. */
static const char *load_file(char (*paths)[1024], int n)
{
	static char file[600];
	static unsigned turn;
	char tmp[620];
	const char *dir = getenv("XDG_RUNTIME_DIR");

	snprintf(file, sizeof(file), "%s/s22-music-load-%u.m3u8", dir ? dir : "/run/s22-gui",
		 turn++ % 4);
	snprintf(tmp, sizeof(tmp), "%s.new", file);
	FILE *f = fopen(tmp, "we");
	if (!f)
		return NULL;
	fputs("#EXTM3U\n", f);
	for (int i = 0; i < n; i++)
		if (!strchr(paths[i], '\n'))
			fprintf(f, "%s\n", paths[i]);
	if (fclose(f) != 0 || rename(tmp, file) != 0) {
		unlink(tmp);
		return NULL;
	}
	return file;
}

/* Play paths, starting at start (or shuffled), as the new queue */
static void play_paths(struct ui *ui, char (*paths)[1024], int n, int start, bool shuffle)
{
	char idx[16];

	if (!n || !mpv_ready(ui))
		return;
	if (start < 0 || start >= n)
		start = 0;
	const char *file = load_file(paths, n);
	if (!file)
		return;
	/* Paused while the list goes in, so the first song does not sound on
	 * the way to the one chosen */
	mpv_cmd("set", "pause", "yes");
	mpv_cmd("loadlist", file, "replace");
	if (shuffle) {
		mpv_cmd("playlist-shuffle");
		mpv_cmd("playlist-play-index", "0");
	} else if (start) {
		snprintf(idx, sizeof(idx), "%d", start);
		mpv_cmd("playlist-play-index", idx);
	}
	mpv_cmd("set", "pause", "no");
	pl.shuffle = shuffle;
}

/* ------------------------------------------------------------ files --- */

static const char *const audio_ext[] = {
	"mp3", "flac", "ogg", "oga", "opus", "m4a", "m4b", "aac", "alac", "wav",
	"wma", "ape", "wv", "mpc", "mp2", "aif", "aiff", "aifc", "dsf", "dff",
	"tta", "mka", "webm", "spx", "caf", "ac3", "dts", "amr", "mod", "xm",
	"s3m", "it", "mp4", "3gp", "au", "snd", "voc", "w64", "rf64", NULL,
};
static const char *const list_ext[] = { "m3u", "m3u8", "pls", NULL };

static bool ext_in(const char *name, const char *const *list)
{
	const char *dot = strrchr(name, '.');

	if (!dot)
		return false;
	for (int i = 0; list[i]; i++)
		if (!strcasecmp(dot + 1, list[i]))
			return true;
	return false;
}

static char music_dir[512], lists_dir[600];

/* A name to show: the file's, without its folder and extension */
static void display_name(const char *path, char *out, size_t n)
{
	const char *slash = strrchr(path, '/');

	snprintf(out, n, "%s", slash ? slash + 1 : path);
	char *dot = strrchr(out, '.');
	if (dot && dot != out)
		*dot = 0;
}

/* A playlist's songs, as paths */
static int read_list(const char *file, char (*out)[1024], int max)
{
	FILE *f = fopen(file, "re");
	char line[2048], dir[1024];
	int n = 0;
	bool pls = ext_in(file, (const char *const[]){ "pls", NULL }), first = true;

	if (!f)
		return 0;
	snprintf(dir, sizeof(dir), "%s", file);
	char *slash = strrchr(dir, '/');
	if (slash)
		*slash = 0;
	while (n < max && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *p = line;
		/* A UTF-8 byte order mark, as MediaMonkey writes: not the
		 * first song's name */
		if (first && !strncmp(p, "\xef\xbb\xbf", 3))
			p += 3;
		first = false;
		if (pls) {
			if (strncasecmp(p, "File", 4))
				continue;
			p = strchr(p, '=');
			if (!p)
				continue;
			p++;
		} else if (!*p || *p == '#') {
			continue;
		}
		if (!strncmp(p, "file://", 7))
			p += 7;
		/* Written on Windows (MediaMonkey syncing to the mapped share):
		 * backslashes, and maybe the drive the share was mapped as,
		 * whose root is the home folder */
		if (!strstr(p, "://"))
			for (char *q = p; *q; q++)
				if (*q == '\\')
					*q = '/';
		if (isalpha((unsigned char)p[0]) && p[1] == ':' && p[2] == '/') {
			const char *home = getenv("HOME");
			snprintf(out[n++], 1024, "%s/%s", home ? home : "", p + 3);
		} else if (*p == '/' || strstr(p, "://")) {
			snprintf(out[n++], 1024, "%s", p);
		} else {
			snprintf(out[n++], 1024, "%s/%s", dir, p);
		}
	}
	fclose(f);
	return n;
}

/* Our playlists: songs in ~/Music relative to ~/Music/Playlists */
static bool write_list(const char *file, char (*paths)[1024], int n)
{
	char tmp[700];
	size_t ml = strlen(music_dir);

	snprintf(tmp, sizeof(tmp), "%s.new", file);
	FILE *f = fopen(tmp, "we");
	if (!f)
		return false;
	fputs("#EXTM3U\n", f);
	for (int i = 0; i < n; i++) {
		if (!strncmp(paths[i], music_dir, ml) && paths[i][ml] == '/')
			fprintf(f, "..%s\n", paths[i] + ml);
		else
			fprintf(f, "%s\n", paths[i]);
	}
	bool ok = fclose(f) == 0 && rename(tmp, file) == 0;
	if (!ok)
		unlink(tmp);
	return ok;
}

/* ------------------------------------------------------------ views --- */

enum view { V_FOLDERS, V_ARTISTS, V_ALBUMS, V_SONGS, V_LISTS, V_QUEUE, V_LIST, V_ARTIST,
	    V_TRACKS, V_NOW, V_OPTS, V_PICK, V_NAME };
static enum view view = V_FOLDERS, opts_from;

struct item { char name[256]; char path[1024]; bool dir, list; };
static struct item *items;		/* the folder shown */
static int nitems;
static char cur_dir[1024];
static struct ui_list folders = { .row_h = ROW_H };

static struct item *library;		/* every song in ~/Music, for Songs */
static int nlibrary;
static struct ui_list library_l = { .row_h = ROW_H };

static struct item *lists;		/* the playlists */
static int nlists;
static struct ui_list lists_l = { .row_h = ROW_H };

static char (*songs)[1024];		/* the playlist open */
static int nsongs;
static char open_list[1024];
static bool open_editable;
static struct ui_list songs_l = { .row_h = ROW_H };
static int moving = -1;			/* a song being moved with Up/Down */

static struct ui_list queue_l = { .row_h = ROW_H };

/* The * menu: what it acts on, and its rows */
enum opt { O_PLAY, O_NEXT, O_QUEUE, O_ADD, O_SHUFFLE, O_MOVE, O_REMOVE,
	   O_DELETE, O_COUNT };
static const char *const opt_names[O_COUNT] = {
	"Play", "Play next", "Add to the queue", "Add to a playlist",
	"Shuffle all", "Move (Up/Down, OK when done)", "Remove from the playlist",
	"Delete this playlist",
};
static enum opt opts[O_COUNT];
static int nopts;
static struct ui_list opts_l = { .row_h = 52 };
static char (*target)[1024];		/* the songs an action takes */
static int ntarget;
static char target_name[256];
static bool delete_armed;

static struct ui_list pick_l = { .row_h = ROW_H };
static struct ui_entry name_entry;
static char flash[96];
static long long flash_until;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void say(struct ui *ui, const char *text)
{
	snprintf(flash, sizeof(flash), "%s", text);
	flash_until = now_ms() + 2000;
	ui_redraw(ui);
}

/* Left or Right held in now playing scrubs: 10 s a second, 30 after two.
 * Timed by the app (tick), not by key repeats; a tap is still the song
 * before or after, on release. */
static uint32_t scrub_key;
static bool scrubbing;
static long long scrub_since, scrub_last, scrub_sent;
static double scrub_pos;

static double shown_time(void)
{
	return scrubbing ? scrub_pos : pl.time;
}

static void scrub_seek(void)
{
	char t[32];

	snprintf(t, sizeof(t), "%.1f", scrub_pos);
	mpv_cmd("seek", t, "absolute");
	mpv_cmdv(R_TIME, (const char *const[]){ "get_property", "time-pos", NULL });
}

static void scrub_step(struct ui *ui)
{
	long long t = now_ms();

	if (!scrubbing) {
		if (t - scrub_since < 400)
			return;
		scrubbing = true;
		scrub_pos = pl.time;
		scrub_last = t;
	}
	double speed = t - scrub_since > 2400 ? 30 : 10;
	scrub_pos += (scrub_key == KEY_RIGHT ? speed : -speed) * (t - scrub_last) / 1000.0;
	scrub_last = t;
	if (pl.duration > 1 && scrub_pos > pl.duration - 1)
		scrub_pos = pl.duration - 1;
	if (scrub_pos < 0)
		scrub_pos = 0;
	if (t - scrub_sent >= 250) {
		scrub_seek();
		scrub_sent = t;
	}
	ui_redraw(ui);
}

/* Released: the last place sought, or (a tap) nothing yet: done by the
 * caller */
static bool scrub_end(void)
{
	bool was = scrubbing;

	if (scrubbing)
		scrub_seek();
	scrub_key = 0;
	scrubbing = false;
	return was;
}

/* To the now playing screen, its clock running; Back returns to the list
 * it came from */
static void show_now(struct ui *ui)
{
	if (view != V_NOW && view != V_OPTS && view != V_PICK && view != V_NAME)
		opts_from = view;
	view = V_NOW;
	ui_timer(ui, 100);
}

/* A song loaded (playing or paused) as the app opens: now playing */
static void opened(struct ui *ui)
{
	if (pl.path[0] && !pl.idle && view == V_FOLDERS)
		show_now(ui);
}

/* Folders first, then names in natural order (Track 2 before Track 10),
 * case aside */
static int by_item(const void *a, const void *b)
{
	const struct item *x = a, *y = b;
	char lx[256], ly[256];

	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	for (int i = 0; i < 256; i++) {
		lx[i] = tolower((unsigned char)x->name[i]);
		if (!x->name[i])
			break;
	}
	for (int i = 0; i < 256; i++) {
		ly[i] = tolower((unsigned char)y->name[i]);
		if (!y->name[i])
			break;
	}
	lx[255] = ly[255] = 0;
	return strverscmp(lx, ly);
}

static void folder_load(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	snprintf(cur_dir, sizeof(cur_dir), "%s", dir);
	nitems = 0;
	while (d && (de = readdir(d)) && nitems < MAX_ITEMS) {
		if (de->d_name[0] == '.')
			continue;
		struct item *it = &items[nitems];
		struct stat st;
		snprintf(it->path, sizeof(it->path), "%s/%s", dir, de->d_name);
		if (stat(it->path, &st) != 0)
			continue;
		it->dir = S_ISDIR(st.st_mode);
		it->list = !it->dir && ext_in(de->d_name, list_ext);
		if (!it->dir && !it->list && !ext_in(de->d_name, audio_ext))
			continue;
		if (it->dir)
			snprintf(it->name, sizeof(it->name), "%s", de->d_name);
		else
			display_name(de->d_name, it->name, sizeof(it->name));
		nitems++;
	}
	if (d)
		closedir(d);
	qsort(items, nitems, sizeof(*items), by_item);
	folders.count = nitems;
	folders.sel = folders.top = 0;
}

/* Every playlist: ours, then any in ~/Music (two levels down) */
static void lists_scan(const char *dir, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	while (d && (de = readdir(d)) && nlists < MAX_ITEMS) {
		if (de->d_name[0] == '.')
			continue;
		char path[1024];
		struct stat st;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 2 && strcmp(path, lists_dir))
				lists_scan(path, depth + 1);
			continue;
		}
		if (!ext_in(de->d_name, list_ext))
			continue;
		struct item *it = &lists[nlists++];
		snprintf(it->path, sizeof(it->path), "%s", path);
		display_name(de->d_name, it->name, sizeof(it->name));
		it->list = true;
		it->dir = false;
	}
	if (d)
		closedir(d);
}

static int by_name(const void *a, const void *b)
{
	return strcasecmp(((const struct item *)a)->name, ((const struct item *)b)->name);
}

static void lists_load(void)
{
	nlists = 0;
	lists_scan(lists_dir, 2);
	qsort(lists, nlists, sizeof(*lists), by_name);
	int ours = nlists;
	lists_scan(music_dir, 0);
	qsort(lists + ours, nlists - ours, sizeof(*lists), by_name);
	lists_l.count = nlists + 1;		/* + "New playlist" */
	if (lists_l.sel >= lists_l.count)
		lists_l.sel = 0;
}

static void list_open(const char *file)
{
	snprintf(open_list, sizeof(open_list), "%s", file);
	nsongs = read_list(file, songs, MAX_ITEMS);
	open_editable = ext_in(file, (const char *const[]){ "m3u", "m3u8", NULL });
	songs_l.count = nsongs;
	songs_l.sel = songs_l.top = 0;
	moving = -1;
	view = V_LIST;
}

static void list_save(struct ui *ui)
{
	if (!write_list(open_list, songs, nsongs))
		say(ui, "Could not save the playlist");
}

/* The songs in a folder (not its folders), sorted */
static int folder_songs(const char *dir, char (*out)[1024], int max, bool deep)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	struct item *tmp = calloc(MAX_ITEMS, sizeof(*tmp));
	int n = 0, k = 0;

	while (tmp && d && (de = readdir(d)) && n < MAX_ITEMS) {
		if (de->d_name[0] == '.')
			continue;
		struct stat st;
		snprintf(tmp[n].path, sizeof(tmp[n].path), "%s/%s", dir, de->d_name);
		if (stat(tmp[n].path, &st) != 0)
			continue;
		tmp[n].dir = S_ISDIR(st.st_mode);
		if (!tmp[n].dir && !ext_in(de->d_name, audio_ext))
			continue;
		if (tmp[n].dir && !deep)
			continue;
		snprintf(tmp[n].name, sizeof(tmp[n].name), "%s", de->d_name);
		n++;
	}
	if (d)
		closedir(d);
	if (tmp)
		qsort(tmp, n, sizeof(*tmp), by_item);
	for (int i = 0; tmp && i < n && k < max; i++) {
		if (tmp[i].dir)
			k += folder_songs(tmp[i].path, out + k, max - k, true);
		else
			snprintf(out[k++], 1024, "%s", tmp[i].path);
	}
	free(tmp);
	return k;
}

/* A song's name in Songs: without the track number its file name starts
 * with ("06 Artist - Title", as MediaMonkey names them), which orders an
 * album but would scatter a list of every song */
static void song_name(const char *path, char *out, size_t n)
{
	display_name(path, out, n);
	if (isdigit((unsigned char)out[0]) && isdigit((unsigned char)out[1]) &&
	    out[2] == ' ' && out[3])
		memmove(out, out + 3, strlen(out + 3) + 1);
}

/* Every song under dir, however deep (eight folders at most) */
static void library_scan(const char *dir, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	while (d && (de = readdir(d)) && nlibrary < MAX_ITEMS) {
		if (de->d_name[0] == '.')
			continue;
		char path[1024];
		struct stat st;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 8)
				library_scan(path, depth + 1);
			continue;
		}
		if (!ext_in(de->d_name, audio_ext))
			continue;
		struct item *it = &library[nlibrary++];
		snprintf(it->path, sizeof(it->path), "%s", path);
		song_name(path, it->name, sizeof(it->name));
		it->dir = it->list = false;
	}
	if (d)
		closedir(d);
}

/* Songs: read again each time the tab opens, so new songs show */
static void library_load(void)
{
	nlibrary = 0;
	library_scan(music_dir, 0);
	qsort(library, nlibrary, sizeof(*library), by_item);
	library_l.count = nlibrary;
	if (library_l.sel >= nlibrary)
		library_l.sel = library_l.top = 0;
}

/* Every song, from the one chosen on, or shuffled */
static void library_play(struct ui *ui, int start, bool shuffle)
{
	char (*p)[1024] = calloc(nlibrary ? nlibrary : 1, sizeof(*p));

	if (!p)
		return;
	for (int i = 0; i < nlibrary; i++)
		memcpy(p[i], library[i].path, sizeof(p[i]));
	play_paths(ui, p, nlibrary, start, shuffle);
	free(p);
}

/* ------------------------------------------------------------- tags --- */

/* Artists and Albums go by the songs' tags, which s22-music-tags reads in
 * the background and keeps in ~/.cache/s22-music/tags (D083). An album is
 * its album artist's (without one, its folder's) and its name (without one,
 * the folder's name). An artist is anyone an album or a song names; ";"
 * separates several (as MediaMonkey writes them), a comma does not
 * ("Tyler, the Creator"). */
struct tsong {
	const char *path, *artist, *aartist, *album, *title;
	char *dir;			/* the song's folder */
	int track, disc, year;
};
struct album { const char *name, *by; int year, first, n; };
struct artist { char *name; int nalbums, nsongs; };

static char *tag_buf;
static struct tsong *tsongs;
static int ntsongs;
static int *order;			/* the songs by album, disc and track */
static struct album *albums;
static int nalbums;
static int *album_order;		/* Albums: by name */
static struct ui_list albums_l = { .row_h = ROW2_H };
static struct artist *artists;
static int nartists;
static struct ui_list artists_l = { .row_h = ROW2_H };

/* An artist's page: all their songs, their albums, then those they are on */
struct arow { int album; bool on; };
static struct arow *arows;
static int narows;
static char artist_open[256];
static struct ui_list artist_l = { .row_h = ROW2_H };

/* The songs of an album, or all of an artist's */
static int *tracks;
static int ntracks;
static int tracks_album = -1;		/* or -1: artist_open's songs */
static enum view tracks_from;
static struct ui_list tracks_l = { .row_h = ROW2_H };

static char tag_cache[600];
static int tags_fd = -1, tags_todo = -1, tags_done;
static bool tags_changed;
static pid_t tags_pid;
static char tags_in[256];
static int tags_inlen;

/* Whether the list of names (a;b;c) has name, case aside */
static bool has_name(const char *list, const char *name)
{
	size_t n = strlen(name);

	while (*list) {
		while (*list == ' ')
			list++;
		const char *end = strchr(list, ';');
		const char *e = end ? end : list + strlen(list);
		while (e > list && e[-1] == ' ')
			e--;
		if ((size_t)(e - list) == n && !strncasecmp(list, name, n))
			return true;
		if (!end)
			break;
		list = end + 1;
	}
	return false;
}

static int cmp_name(const char *a, const char *b)
{
	char la[256], lb[256];
	int i;

	for (i = 0; i < 255 && a[i]; i++)
		la[i] = tolower((unsigned char)a[i]);
	la[i] = 0;
	for (i = 0; i < 255 && b[i]; i++)
		lb[i] = tolower((unsigned char)b[i]);
	lb[i] = 0;
	return strverscmp(la, lb);
}

/* What makes songs one album: the album artist, else the folder */
static const char *album_key(const struct tsong *t)
{
	return t->aartist[0] ? t->aartist : t->dir;
}

static int by_album(const void *a, const void *b)
{
	const struct tsong *x = &tsongs[*(const int *)a], *y = &tsongs[*(const int *)b];
	int r = strcasecmp(album_key(x), album_key(y));

	if (!r)
		r = strcasecmp(x->album, y->album);
	if (!r)
		r = x->disc - y->disc;
	if (!r)
		r = x->track - y->track;
	return r ? r : strverscmp(x->path, y->path);
}

static int by_album_name(const void *a, const void *b)
{
	const struct album *x = &albums[*(const int *)a], *y = &albums[*(const int *)b];
	int r = cmp_name(x->name, y->name);

	return r ? r : cmp_name(x->by, y->by);
}

static int by_artist(const void *a, const void *b)
{
	return cmp_name(((const struct artist *)a)->name, ((const struct artist *)b)->name);
}

static char *next_field(char **p)
{
	char *s = *p, *t = strchr(s, '\t');

	if (t) {
		*t = 0;
		*p = t + 1;
	} else {
		*p = s + strlen(s);
	}
	return s;
}

/* Each name in a;b;c as an artist, once */
static void add_artists(const char *list, int *n)
{
	while (*list && *n < MAX_ITEMS) {
		while (*list == ' ')
			list++;
		const char *end = strchr(list, ';');
		const char *e = end ? end : list + strlen(list);
		while (e > list && e[-1] == ' ')
			e--;
		if (e > list) {
			char *name = strndup(list, e - list);
			int i;
			for (i = 0; i < *n && strcasecmp(artists[i].name, name); i++)
				;
			if (i < *n)
				free(name);
			else
				artists[(*n)++] = (struct artist){ name, 0, 0 };
		}
		if (!end)
			break;
		list = end + 1;
	}
}

/* The cache read again, and the albums and artists made from it */
static void tags_load(void)
{
	for (int i = 0; i < ntsongs; i++)
		free(tsongs[i].dir);
	for (int i = 0; i < nartists; i++)
		free(artists[i].name);
	free(tag_buf);
	tag_buf = NULL;
	ntsongs = nalbums = nartists = 0;

	FILE *f = fopen(tag_cache, "re");
	if (f) {
		struct stat st;
		if (fstat(fileno(f), &st) == 0 && (tag_buf = malloc(st.st_size + 1))) {
			size_t n = fread(tag_buf, 1, st.st_size, f);
			tag_buf[n] = 0;
		}
		fclose(f);
	}
	char *p = tag_buf, *line;
	bool first = true;
	while (p && (line = strsep(&p, "\n")) && ntsongs < MAX_ITEMS) {
		if (first) {
			first = false;
			if (strcmp(line, "s22-music-tags 1"))
				break;
			continue;
		}
		char *q = line;
		struct tsong *t = &tsongs[ntsongs];
		t->path = next_field(&q);
		if (!t->path[0])
			continue;
		next_field(&q);		/* the time and size: the helper's */
		next_field(&q);
		t->artist = next_field(&q);
		t->aartist = next_field(&q);
		t->album = next_field(&q);
		t->track = atoi(next_field(&q));
		t->disc = atoi(next_field(&q));
		t->year = atoi(next_field(&q));
		t->title = next_field(&q);
		const char *slash = strrchr(t->path, '/');
		t->dir = strndup(t->path, slash ? (size_t)(slash - t->path) : 0);
		if (!t->dir)
			continue;
		if (!t->album[0]) {
			const char *d = strrchr(t->dir, '/');
			t->album = d ? d + 1 : t->dir;
		}
		order[ntsongs] = ntsongs;
		ntsongs++;
	}
	qsort(order, ntsongs, sizeof(*order), by_album);

	/* The albums: runs of songs alike in key and name */
	for (int i = 0; i < ntsongs; i++) {
		const struct tsong *t = &tsongs[order[i]];
		struct album *a = nalbums ? &albums[nalbums - 1] : NULL;
		if (a && !strcasecmp(album_key(&tsongs[order[a->first]]), album_key(t)) &&
		    !strcasecmp(a->name, t->album)) {
			a->n++;
			if (!a->year)
				a->year = t->year;
			/* Without an album artist: the artist if all agree */
			if (!t->aartist[0] && strcasecmp(a->by, t->artist[0] ? t->artist : "Unknown artist"))
				a->by = "Various artists";
			continue;
		}
		albums[nalbums] = (struct album){
			.name = t->album, .first = i, .n = 1, .year = t->year,
			.by = t->aartist[0] ? t->aartist : t->artist[0] ? t->artist : "Unknown artist",
		};
		album_order[nalbums] = nalbums;
		nalbums++;
	}
	qsort(album_order, nalbums, sizeof(*album_order), by_album_name);

	/* The artists: every name, then what is theirs */
	for (int i = 0; i < nalbums; i++)
		if (strcmp(albums[i].by, "Various artists"))
			add_artists(albums[i].by, &nartists);
	for (int i = 0; i < ntsongs; i++)
		add_artists(tsongs[i].artist, &nartists);
	for (int k = 0; k < nartists; k++) {
		struct artist *ar = &artists[k];
		for (int i = 0; i < nalbums; i++) {
			const struct album *a = &albums[i];
			if (has_name(a->by, ar->name)) {
				ar->nalbums++;
				ar->nsongs += a->n;
				continue;
			}
			int on = 0;
			for (int j = a->first; j < a->first + a->n; j++)
				on += has_name(tsongs[order[j]].artist, ar->name);
			if (on) {
				ar->nalbums++;
				ar->nsongs += on;
			}
		}
	}
	qsort(artists, nartists, sizeof(*artists), by_artist);
	artists_l.count = nartists;
	albums_l.count = nalbums;
}

/* An artist's songs, as song numbers: their albums whole, then their songs
 * on others' */
static int artist_songs(const char *name, int *out)
{
	int n = 0;

	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < nalbums; i++) {
			const struct album *a = &albums[i];
			bool own = has_name(a->by, name);
			if (own != (pass == 0))
				continue;
			for (int j = a->first; j < a->first + a->n && n < MAX_ITEMS; j++)
				if (own || has_name(tsongs[order[j]].artist, name))
					out[n++] = order[j];
		}
	return n;
}

static int by_year(const void *a, const void *b)
{
	const struct arow *x = a, *y = b;
	const struct album *p = &albums[x->album], *q = &albums[y->album];

	if (x->on != y->on)
		return x->on ? 1 : -1;
	if (!x->on && p->year != q->year)
		return (p->year ? p->year : 9999) - (q->year ? q->year : 9999);
	return cmp_name(p->name, q->name);
}

/* The page of name: all their songs, their albums by year, then the
 * albums they are on */
static void artist_page(const char *name)
{
	snprintf(artist_open, sizeof(artist_open), "%s", name);
	narows = 0;
	arows[narows++] = (struct arow){ -1, false };
	for (int i = 0; i < nalbums && narows < MAX_ITEMS; i++) {
		const struct album *a = &albums[i];
		if (has_name(a->by, name)) {
			arows[narows++] = (struct arow){ i, false };
			continue;
		}
		for (int j = a->first; j < a->first + a->n; j++)
			if (has_name(tsongs[order[j]].artist, name)) {
				arows[narows++] = (struct arow){ i, true };
				break;
			}
	}
	qsort(arows + 1, narows - 1, sizeof(*arows), by_year);
	artist_l.count = narows;
}

/* An album's songs (album >= 0), or all of artist_open's */
static void tracks_open(int album)
{
	tracks_album = album;
	ntracks = 0;
	if (album >= 0)
		for (int j = 0; j < albums[album].n; j++)
			tracks[ntracks++] = order[albums[album].first + j];
	else
		ntracks = artist_songs(artist_open, tracks);
	tracks_l.count = ntracks;
}

static int song_paths(const int *idx, int n, char (*out)[1024])
{
	for (int i = 0; i < n; i++)
		snprintf(out[i], 1024, "%s", tsongs[idx[i]].path);
	return n;
}

/* After a new read: the same artist, album and rows chosen, by name */
static void tags_reload(void)
{
	char artist[256] = "", album[256] = "", by[256] = "", page_album[256] = "";
	int artist_sel = artist_l.sel, track_sel = tracks_l.sel;

	if (nartists && artists_l.sel < nartists)
		snprintf(artist, sizeof(artist), "%s", artists[artists_l.sel].name);
	if (nalbums && albums_l.sel < nalbums) {
		snprintf(album, sizeof(album), "%s", albums[album_order[albums_l.sel]].name);
		snprintf(by, sizeof(by), "%s", albums[album_order[albums_l.sel]].by);
	}
	if (tracks_album >= 0 && tracks_album < nalbums)
		snprintf(page_album, sizeof(page_album), "%s\t%s", albums[tracks_album].name,
			 albums[tracks_album].by);
	tags_load();
	for (int i = 0; i < nartists; i++)
		if (!strcmp(artists[i].name, artist))
			artists_l.sel = i;
	for (int i = 0; i < nalbums; i++) {
		const struct album *a = &albums[album_order[i]];
		if (!strcmp(a->name, album) && !strcmp(a->by, by))
			albums_l.sel = i;
	}
	/* The pages open (or under the * menu) hold album and song numbers */
	if (artist_open[0]) {
		artist_page(artist_open);
		artist_l.sel = artist_sel;
	}
	if (ntracks) {
		int a = -1;
		for (int i = 0; page_album[0] && i < nalbums; i++) {
			char key[600];
			snprintf(key, sizeof(key), "%s\t%s", albums[i].name, albums[i].by);
			if (!strcmp(key, page_album))
				a = i;
		}
		if (page_album[0] && a < 0) {		/* the album is gone */
			ntracks = tracks_l.count = 0;
			if (view == V_TRACKS)
				view = tracks_from;
		} else {
			tracks_open(a);
			tracks_l.sel = track_sel;
		}
	}
}

static void on_tags(struct ui *ui, int fd)
{
	ssize_t n;

	while ((n = read(fd, tags_in + tags_inlen, sizeof(tags_in) - 1 - tags_inlen)) > 0) {
		tags_inlen += n;
		tags_in[tags_inlen] = 0;
		char *p = tags_in, *nl;
		while ((nl = strchr(p, '\n'))) {
			*nl = 0;
			if (!strncmp(p, "read ", 5))
				tags_todo = atoi(p + 5);
			else if (!strncmp(p, "done ", 5)) {
				tags_done = atoi(p + 5);
				tags_changed = true;
			}
			p = nl + 1;
		}
		tags_inlen -= p - tags_in;
		memmove(tags_in, p, tags_inlen);
		if (tags_inlen >= (int)sizeof(tags_in) - 1)
			tags_inlen = 0;
	}
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	ui_unwatch(ui, fd);
	close(fd);
	tags_fd = -1;
	waitpid(tags_pid, NULL, 0);
	tags_todo = -1;
	if (tags_changed)
		tags_reload();
	ui_redraw(ui);
}

/* The tags read again (new and changed songs only), unless already */
static void tags_start(struct ui *ui)
{
	int p[2];

	if (tags_fd >= 0 || pipe2(p, O_CLOEXEC) != 0)
		return;
	pid_t pid = fork();
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		dup2(p[1], 1);
		execlp("s22-music-tags", "s22-music-tags", music_dir, tag_cache, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		return;
	}
	tags_pid = pid;
	tags_fd = p[0];
	tags_todo = tags_done = tags_inlen = 0;
	tags_changed = false;
	fcntl(tags_fd, F_SETFL, fcntl(tags_fd, F_GETFL) | O_NONBLOCK);
	ui_watch(ui, tags_fd, on_tags);
}

/* ------------------------------------------------------- the menu --- */

static void opts_open(struct ui *ui, enum view from)
{
	nopts = 0;
	ntarget = 0;
	delete_armed = false;
	target_name[0] = 0;
	opts_from = from;
	if (from == V_FOLDERS && nitems) {
		struct item *it = &items[folders.sel];
		snprintf(target_name, sizeof(target_name), "%s", it->name);
		if (it->dir)
			ntarget = folder_songs(it->path, target, MAX_ITEMS, true);
		else if (it->list)
			ntarget = read_list(it->path, target, MAX_ITEMS);
		else
			snprintf(target[ntarget++], 1024, "%s", it->path);
		opts[nopts++] = O_PLAY;
		opts[nopts++] = O_NEXT;
		opts[nopts++] = O_QUEUE;
		opts[nopts++] = O_ADD;
		if (ntarget > 1)
			opts[nopts++] = O_SHUFFLE;
	} else if (from == V_SONGS && nlibrary) {
		struct item *it = &library[library_l.sel];
		snprintf(target_name, sizeof(target_name), "%s", it->name);
		snprintf(target[ntarget++], 1024, "%s", it->path);
		opts[nopts++] = O_PLAY;
		opts[nopts++] = O_NEXT;
		opts[nopts++] = O_QUEUE;
		opts[nopts++] = O_ADD;
		if (nlibrary > 1)
			opts[nopts++] = O_SHUFFLE;
	} else if ((from == V_ARTISTS && nartists) || (from == V_ALBUMS && nalbums) ||
		   (from == V_ARTIST && narows)) {
		int *idx = calloc(MAX_ITEMS, sizeof(*idx)), n = 0;
		int album = from == V_ALBUMS ? album_order[albums_l.sel] :
			    from == V_ARTIST ? arows[artist_l.sel].album : -1;
		if (!idx)
			return;
		if (album >= 0) {
			snprintf(target_name, sizeof(target_name), "%s", albums[album].name);
			for (int j = 0; j < albums[album].n; j++)
				idx[n++] = order[albums[album].first + j];
		} else {
			const char *name = from == V_ARTISTS ? artists[artists_l.sel].name : artist_open;
			snprintf(target_name, sizeof(target_name), "%s", name);
			n = artist_songs(name, idx);
		}
		ntarget = song_paths(idx, n, target);
		free(idx);
		opts[nopts++] = O_PLAY;
		if (ntarget > 1)
			opts[nopts++] = O_SHUFFLE;
		opts[nopts++] = O_NEXT;
		opts[nopts++] = O_QUEUE;
		opts[nopts++] = O_ADD;
	} else if (from == V_TRACKS && ntracks) {
		const struct tsong *t = &tsongs[tracks[tracks_l.sel]];
		if (t->title[0])
			snprintf(target_name, sizeof(target_name), "%s", t->title);
		else
			song_name(t->path, target_name, sizeof(target_name));
		snprintf(target[ntarget++], 1024, "%s", t->path);
		opts[nopts++] = O_PLAY;
		opts[nopts++] = O_NEXT;
		opts[nopts++] = O_QUEUE;
		opts[nopts++] = O_ADD;
	} else if (from == V_LISTS && lists_l.sel > 0) {
		struct item *it = &lists[lists_l.sel - 1];
		snprintf(target_name, sizeof(target_name), "%s", it->name);
		ntarget = read_list(it->path, target, MAX_ITEMS);
		opts[nopts++] = O_PLAY;
		opts[nopts++] = O_SHUFFLE;
		opts[nopts++] = O_NEXT;
		opts[nopts++] = O_QUEUE;
		opts[nopts++] = O_DELETE;
	} else if (from == V_LIST && nsongs) {
		display_name(songs[songs_l.sel], target_name, sizeof(target_name));
		snprintf(target[ntarget++], 1024, "%s", songs[songs_l.sel]);
		opts[nopts++] = O_PLAY;
		opts[nopts++] = O_NEXT;
		opts[nopts++] = O_QUEUE;
		opts[nopts++] = O_ADD;
		if (open_editable) {
			opts[nopts++] = O_MOVE;
			opts[nopts++] = O_REMOVE;
		}
	} else if (from == V_QUEUE && nqueue && queue_path[queue_l.sel][0]) {
		snprintf(target_name, sizeof(target_name), "%s", queue[queue_l.sel]);
		snprintf(target[ntarget++], 1024, "%s", queue_path[queue_l.sel]);
		opts[nopts++] = O_ADD;
	} else if (from == V_NOW && pl.path[0]) {
		snprintf(target_name, sizeof(target_name), "%s", pl.title);
		snprintf(target[ntarget++], 1024, "%s", pl.path);
		opts[nopts++] = O_ADD;
	}
	if (!nopts)
		return;
	opts_l.count = nopts;
	opts_l.sel = opts_l.top = 0;
	view = V_OPTS;
	ui_redraw(ui);
}

static void enqueue(struct ui *ui, bool next)
{
	if (!ntarget || !mpv_ready(ui))
		return;
	const char *file = load_file(target, ntarget);
	if (!file) {
		say(ui, "Could not add the songs");
		return;
	}
	if (next && pl.ppos >= 0)
		mpv_cmd("loadlist", file, "insert-next");
	else
		mpv_cmd("loadlist", file, pl.idle && !pl.pcount ? "append-play" : "append");
	say(ui, next ? "Plays next" : "Added to the queue");
}

static void add_to(struct ui *ui, const char *file)
{
	char (*all)[1024] = calloc(MAX_ITEMS, sizeof(*all));
	int n = all ? read_list(file, all, MAX_ITEMS) : 0;

	for (int i = 0; all && i < ntarget && n < MAX_ITEMS; i++)
		snprintf(all[n++], 1024, "%s", target[i]);
	if (all && write_list(file, all, n))
		say(ui, "Added to the playlist");
	else
		say(ui, "Could not save the playlist");
	free(all);
}

static void opt_choose(struct ui *ui)
{
	enum opt o = opts[opts_l.sel];

	view = opts_from;
	switch (o) {
	case O_PLAY:
		if (opts_from == V_SONGS)
			library_play(ui, library_l.sel, false);
		else
			play_paths(ui, target, ntarget, 0, false);
		show_now(ui);
		break;
	case O_SHUFFLE:
		if (opts_from == V_SONGS)
			library_play(ui, 0, true);
		else
			play_paths(ui, target, ntarget, 0, true);
		show_now(ui);
		break;
	case O_NEXT:
		enqueue(ui, true);
		break;
	case O_QUEUE:
		enqueue(ui, false);
		break;
	case O_ADD:
		lists_load();
		pick_l.count = nlists + 1;
		pick_l.sel = pick_l.top = 0;
		view = V_PICK;
		break;
	case O_MOVE:
		moving = songs_l.sel;
		break;
	case O_REMOVE:
		memmove(songs[songs_l.sel], songs[songs_l.sel + 1],
			(size_t)(nsongs - songs_l.sel - 1) * sizeof(songs[0]));
		nsongs--;
		songs_l.count = nsongs;
		if (songs_l.sel >= nsongs && nsongs)
			songs_l.sel = nsongs - 1;
		list_save(ui);
		break;
	case O_DELETE:
		if (!delete_armed) {
			delete_armed = true;
			view = V_OPTS;
			say(ui, "OK again deletes the playlist file");
			return;
		}
		if (lists_l.sel > 0 && unlink(lists[lists_l.sel - 1].path) == 0)
			say(ui, "Playlist deleted");
		lists_load();
		break;
	case O_COUNT:
		break;
	}
	ui_redraw(ui);
}

/* ------------------------------------------------------- visualizer --- */

/* Under the bar in now playing (D084): what plays, recorded from the
 * speaker's monitor by parec (PipeWire's Pulse side: mono, 32 kHz, floats)
 * on a pipe. Bars and Spectrum are an FFT of the last 2048 samples (64 ms),
 * Scope the wave itself. Recorded only while it shows: now playing on
 * screen (not with the display off or the lid closed), a song playing, the
 * visualizer on. 0 in now playing changes it; the music_vis setting keeps
 * the choice. Off, the bar sits lower and the cover shows more. */
enum vis { VIS_OFF, VIS_BARS, VIS_SPECTRUM, VIS_SCOPE, VIS_COUNT };
static const char *const vis_keys[VIS_COUNT] = { "off", "bars", "spectrum", "scope" };
static const char *const vis_titles[VIS_COUNT] = { "Off", "Bars", "Spectrum", "Scope" };
static enum vis vis_mode = VIS_BARS;

#define VIS_RATE	32000
#define FFT_N		2048
#define NBARS		28
#define NSPEC		120
#define VIS_LO		40.0f		/* Hz, the lowest band's start */
#define VIS_HI		14000.0f
#define VIS_FLOOR	-60.0f		/* dB shown as nothing ... */
#define VIS_TOP		-8.0f		/* ... and as full */

static float vis_ring[FFT_N];		/* the last samples, vis_pos next */
static int vis_pos;
static int vis_fd = -1;
static pid_t vis_pid;
static unsigned char vis_part[4];	/* a sample cut between reads */
static int vis_npart;
static bool vis_fresh;			/* samples since the last frame */
static float bars[NBARS], peaks[NBARS], spec[NSPEC];
static long long peak_at[NBARS], vis_last;

static void vis_stop(struct ui *ui)
{
	if (vis_fd < 0)
		return;
	ui_unwatch(ui, vis_fd);
	close(vis_fd);
	vis_fd = -1;
	kill(vis_pid, SIGTERM);
	waitpid(vis_pid, NULL, 0);
}

static void on_vis(struct ui *ui, int fd)
{
	unsigned char buf[8192];
	ssize_t n;

	while ((n = read(fd, buf + vis_npart, sizeof(buf) - vis_npart)) > 0) {
		memcpy(buf, vis_part, vis_npart);
		n += vis_npart;
		int whole = n / 4 * 4;
		for (int i = 0; i < whole; i += 4) {
			float s;
			memcpy(&s, buf + i, 4);
			vis_ring[vis_pos] = s;
			vis_pos = (vis_pos + 1) % FFT_N;
		}
		vis_npart = n - whole;
		memcpy(vis_part, buf + whole, vis_npart);
		vis_fresh = true;
	}
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
		vis_stop(ui);		/* parec went: tried again by tick */
}

static void vis_start(struct ui *ui)
{
	int p[2];

	if (vis_fd >= 0 || pipe2(p, O_CLOEXEC) != 0)
		return;
	pid_t pid = fork();
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		dup2(p[1], 1);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0)
			dup2(null, 2);
		execlp("parec", "parec", "--raw", "--format=float32le", "--channels=1",
		       "--rate=32000", "--latency-msec=10", "--client-name=Music visualizer",
		       "-d", "@DEFAULT_MONITOR@", (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		return;
	}
	vis_pid = pid;
	vis_fd = p[0];
	vis_npart = 0;
	fcntl(vis_fd, F_SETFL, fcntl(vis_fd, F_GETFL) | O_NONBLOCK);
	ui_watch(ui, vis_fd, on_vis);
}

/* Recording just while it shows */
static void vis_update(struct ui *ui)
{
	bool want = vis_mode != VIS_OFF && view == V_NOW && mpv_fd >= 0 && !pl.paused &&
		    !pl.idle && ui_visible(ui);

	if (want && vis_fd < 0)
		vis_start(ui);
	else if (!want && vis_fd >= 0)
		vis_stop(ui);
}

/* Still moving: recording, or bars and peaks on their way down */
static bool vis_moving(void)
{
	if (vis_mode == VIS_OFF || view != V_NOW)
		return false;
	if (vis_fd >= 0)
		return true;
	for (int i = 0; i < NBARS; i++)
		if (bars[i] > 0.002f || peaks[i] > 0.002f)
			return true;
	for (int i = 0; i < NSPEC; i++)
		if (spec[i] > 0.002f)
			return true;
	return false;
}

static void fft(float *re, float *im, int n)
{
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			float t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		double a = -2 * M_PI / len;
		float wr = cos(a), wi = sin(a);
		for (int i = 0; i < n; i += len) {
			float cr = 1, ci = 0;
			for (int k = 0; k < len / 2; k++) {
				float *xr = &re[i + k], *xi = &im[i + k];
				float *yr = &re[i + k + len / 2], *yi = &im[i + k + len / 2];
				float vr = *yr * cr - *yi * ci, vi = *yr * ci + *yi * cr;
				*yr = *xr - vr;
				*yi = *xi - vi;
				*xr += vr;
				*xi += vi;
				float t = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = t;
			}
		}
	}
}

/* The power between f0 and f1 Hz, 0 to 1 as shown: the bins' power summed
 * (so each band of a log scale weighs alike: pink noise is flat), a band
 * narrower than a bin from the bins either side */
static float band(const float *pw, float f0, float f1)
{
	float k0 = f0 * FFT_N / VIS_RATE, k1 = f1 * FFT_N / VIS_RATE, p = 0;
	int a = (int)ceilf(k0), b = (int)ceilf(k1);

	if (b <= a) {
		float k = (k0 + k1) / 2;
		int i = (int)k;
		float f = k - i;
		p = (pw[i] * (1 - f) + pw[i + 1] * f) * (k1 - k0);
	} else {
		for (int i = a; i < b && i <= FFT_N / 2; i++)
			p += pw[i];
	}
	float db = 10 * log10f(p + 1e-12f);
	float v = (db - VIS_FLOOR) / (VIS_TOP - VIS_FLOOR);
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* Toward the new levels: up at once (mostly), down at a steady pace;
 * peaks held 0.4 s, then falling */
static void settle(float *v, float to, float dt)
{
	if (to > *v)
		*v += (to - *v) * 0.7f;
	else
		*v = fmaxf(to, *v - 1.6f * dt);
}

static void vis_compute(void)
{
	static float re[FFT_N], im[FFT_N], win[FFT_N], pw[FFT_N / 2 + 2];
	long long t = now_ms();
	float dt = vis_last ? (t - vis_last) / 1000.0f : 0.03f;

	vis_last = t;
	if (dt > 0.2f)
		dt = 0.2f;
	if (vis_mode == VIS_SCOPE)
		return;
	if (!win[1])
		for (int i = 0; i < FFT_N; i++)
			win[i] = 0.5f - 0.5f * cosf(2 * M_PI * i / (FFT_N - 1));
	bool live = vis_fd >= 0 && vis_fresh;
	if (live) {
		for (int i = 0; i < FFT_N; i++) {
			re[i] = vis_ring[(vis_pos + i) % FFT_N] * win[i];
			im[i] = 0;
		}
		fft(re, im, FFT_N);
		/* A full-scale sine's bin at 1 (the window halves it) */
		float norm = 4.0f / FFT_N;
		for (int i = 0; i <= FFT_N / 2; i++)
			pw[i] = (re[i] * re[i] + im[i] * im[i]) * norm * norm;
		pw[FFT_N / 2 + 1] = 0;
		vis_fresh = false;
	}
	float ratio = VIS_HI / VIS_LO;
	for (int i = 0; i < NBARS; i++) {
		float f0 = VIS_LO * powf(ratio, (float)i / NBARS);
		float f1 = VIS_LO * powf(ratio, (float)(i + 1) / NBARS);
		settle(&bars[i], vis_fd >= 0 ? (live ? band(pw, f0, f1) : bars[i]) : 0, dt);
		if (bars[i] >= peaks[i]) {
			peaks[i] = bars[i];
			peak_at[i] = t;
		} else if (t - peak_at[i] > 400) {
			peaks[i] = fmaxf(bars[i], peaks[i] - 0.7f * dt);
		}
	}
	for (int i = 0; i < NSPEC; i++) {
		float f0 = VIS_LO * powf(ratio, (float)i / NSPEC);
		float f1 = VIS_LO * powf(ratio, (float)(i + 1) / NSPEC);
		/* Narrower bands than the bars' hold less power: shown alike */
		float v = live ? band(pw, f0, f1) + 0.07f : spec[i];
		settle(&spec[i], vis_fd >= 0 ? fminf(v, 1) : 0, dt);
	}
}

/* The visualizer in x, y, w, h */
static void vis_draw(struct ui_canvas *c, int x, int y, int w, int h)
{
	vis_compute();
	if (vis_mode == VIS_BARS) {
		int gap = 4, bw = (w - gap * (NBARS - 1)) / NBARS;
		int x0 = x + (w - (bw * NBARS + gap * (NBARS - 1))) / 2;
		for (int i = 0; i < NBARS; i++) {
			int bx = x0 + i * (bw + gap);
			int bh = (int)(bars[i] * (h - 6));
			if (bh > 0)
				ui_round(c, bx, y + h - bh, bw, bh, bh > 6 ? 2 : 0, UI_ACCENT);
			int py = y + h - 3 - (int)(peaks[i] * (h - 6));
			if (peaks[i] > 0.01f)
				ui_fill(c, bx, py, bw, 3, UI_FG);
		}
	} else if (vis_mode == VIS_SPECTRUM) {
		int prev = -1;
		for (int i = 0; i < w; i++) {
			/* Between the two points either side, on the log scale */
			float p = (float)i * (NSPEC - 1) / (w - 1);
			int k = (int)p;
			float f = p - k, v = k + 1 < NSPEC ? spec[k] * (1 - f) + spec[k + 1] * f : spec[k];
			int top = y + h - (int)(v * h);
			if (top < y + h)
				ui_dim(c, x + i, top, 1, y + h - top, UI_ACCENT, 120);
			/* The line along the top, joined to the column before */
			int a = prev < 0 ? top : prev < top ? prev : top;
			int b = prev < 0 ? top : prev > top ? prev : top;
			ui_fill(c, x + i, a - 1, 1, b - a + 2, UI_ACCENT);
			prev = top;
		}
	} else if (vis_mode == VIS_SCOPE) {
		/* 20 ms across, from a rising zero so the wave stands still */
		int n = VIS_RATE / 50, start = FFT_N - n;
		for (int i = FFT_N - n - 1; i > FFT_N - n - 800 && i > 0; i--) {
			float s0 = vis_ring[(vis_pos + i - 1) % FFT_N];
			float s1 = vis_ring[(vis_pos + i) % FFT_N];
			if (s0 < 0 && s1 >= 0) {
				start = i;
				break;
			}
		}
		int mid = y + h / 2, prev = mid;
		for (int i = 0; i < w; i++) {
			float s = vis_fd >= 0 ? vis_ring[(vis_pos + start + i * n / w) % FFT_N] : 0;
			int sy = mid - (int)(fmaxf(-1, fminf(1, s * 1.4f)) * (h / 2 - 2));
			int a = i ? (prev < sy ? prev : sy) : sy, b = i ? (prev > sy ? prev : sy) : sy;
			ui_fill(c, x + i, a - 1, 1, b - a + 2, UI_ACCENT);
			prev = sy;
		}
	}
}

#define THUMB	64		/* the bar's cover */

/* Now playing from the bottom: the "N of M" line, the visualizer (if on),
 * the bar with its times and play or pause between them */
#define VIS_H	136
#define NOW_LINE_Y(h)	((h) - UI_HINTS_H - 28)
static int now_bar_y(int h)
{
	return NOW_LINE_Y(h) - 50 - (vis_mode != VIS_OFF ? VIS_H + 4 : 0);
}

/* ------------------------------------------------------------ drawing */

static void fmt_time(double t, char *buf, size_t n)
{
	int s = t > 0 ? (int)t : 0;

	if (s >= 3600)
		snprintf(buf, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(buf, n, "%d:%02d", s / 60, s % 60);
}

/* The tabs, Left/Right between them */
static const enum view tab_views[] = { V_FOLDERS, V_ARTISTS, V_ALBUMS, V_SONGS, V_LISTS, V_QUEUE };
#define NTABS	6

static int tab_of(enum view v)
{
	for (int i = 0; i < NTABS; i++)
		if (tab_views[i] == v)
			return i;
	return -1;
}

/* Each tab as wide as its name, the room left shared out: six names in
 * equal widths cut "Playlists" at 480 pixels */
static void tab_spans(int width, int *x)
{
	static const char *const names[NTABS] = { "Folders", "Artists", "Albums", "Songs",
						  "Playlists", "Queue" };
	int w[NTABS], sum = 0;

	for (int i = 0; i < NTABS; i++)
		sum += w[i] = ui_text_width(UI_BOLD, 16, names[i]);
	int pad = (width - sum) / NTABS;
	x[0] = 0;
	for (int i = 0; i < NTABS; i++)
		x[i + 1] = x[i] + w[i] + pad;
	x[NTABS] = width;
}

static void tabs(struct ui_canvas *c, int sel)
{
	static const char *const names[NTABS] = { "Folders", "Artists", "Albums", "Songs",
						  "Playlists", "Queue" };
	int x[NTABS + 1];

	tab_spans(c->w, x);
	ui_fill(c, 0, 0, c->w, UI_HEADER_H, UI_BAR);
	for (int i = 0; i < NTABS; i++) {
		int w = x[i + 1] - x[i];
		if (i == sel)
			ui_fill(c, x[i] + 4, UI_HEADER_H - 3, w - 8, 3, UI_ACCENT);
		ui_text_box(c, x[i], 8, w, i == sel ? UI_BOLD : UI_SANS, 16,
			    i == sel ? UI_FG : UI_DIM, UI_CENTER, names[i]);
	}
}

/* The player bar above the hints, while something is loaded: the song's
 * progress, play or pause, title, artist and time. A tap (or #) opens now
 * playing; 5 plays or pauses from any list. */
static int mini_h(void)
{
	return pl.idle && !pl.path[0] ? 0 : 76;
}

static void mini(struct ui *ui, struct ui_canvas *c)
{
	int h = mini_h(), y = c->h - UI_HINTS_H - h;
	char a[32], b[32], line[80];

	if (!h)
		return;
	ui_fill(c, 0, y, c->w, h, UI_SEL);
	double f = pl.duration > 0 ? shown_time() / pl.duration : 0;
	f = f < 0 ? 0 : f > 1 ? 1 : f;
	ui_fill(c, 0, y, c->w, 5, UI_BAR);
	ui_fill(c, 0, y, (int)(c->w * f), 5, UI_ACCENT);

	/* The cover (or a record); paused, it dims under ❚❚ */
	int tx = 6, ty = y + 9;
	const uint32_t *th = ui_art_thumb(ui, pl.path, THUMB);
	if (th) {
		ui_blit(c, tx, ty, th, THUMB, THUMB);
		if (pl.paused)
			ui_dim(c, tx, ty, THUMB, THUMB, 0xff000000, 140);
	} else {
		ui_vinyl(c, tx + THUMB / 2, ty + THUMB / 2, THUMB / 2 - 1);
	}
	if (pl.paused)
		ui_text_box(c, tx, ty + 20, THUMB, UI_BOLD, 20, 0xffffffff, UI_CENTER, "❚❚");

	int x = tx + THUMB + 12;
	ui_text_box(c, x, y + 14, c->w - x - 12, UI_BOLD, 18, UI_FG, UI_LEFT,
		    pl.title[0] ? pl.title : "…");
	fmt_time(shown_time(), a, sizeof(a));
	fmt_time(pl.duration, b, sizeof(b));
	snprintf(line, sizeof(line), "%s / %s", a, b);
	ui_text_box(c, x, y + 44, c->w - x - 12 - 120, UI_SANS, 15, UI_DIM, UI_LEFT, pl.artist);
	ui_text_box(c, c->w - 132, y + 44, 120, UI_SANS, 15, UI_DIM, UI_RIGHT, line);
}

static void row_folder(struct ui *ui, struct ui_canvas *c, int i, int x, int y, int w,
		       int h, bool sel)
{
	struct item *it = &items[i];

	ui_text_box(c, x + 16, y + 16, w - 70, it->dir ? UI_BOLD : UI_SANS, 19, UI_FG,
		    UI_LEFT, it->name);
	ui_text_box(c, x + w - 60, y + 18, 46, UI_SANS, 16, UI_DIM, UI_RIGHT,
		    it->dir ? "›" : it->list ? "list" : "");
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void row_library(struct ui *ui, struct ui_canvas *c, int i, int x, int y, int w,
			int h, bool sel)
{
	ui_text_box(c, x + 16, y + 16, w - 32, UI_SANS, 19, UI_FG, UI_LEFT, library[i].name);
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

/* A row of two lines: name, and what it holds dimmer under it (if any) */
static void row2(struct ui_canvas *c, int x, int y, int w, int h, const char *name,
		 const char *detail, enum ui_font font)
{
	if (detail && detail[0]) {
		ui_text_box(c, x + 16, y + 8, w - 32, font, 19, UI_FG, UI_LEFT, name);
		ui_text_box(c, x + 16, y + 35, w - 32, UI_SANS, 15, UI_DIM, UI_LEFT, detail);
	} else {
		ui_text_box(c, x + 16, y + (h - 24) / 2, w - 32, font, 19, UI_FG, UI_LEFT, name);
	}
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void songs_count(char *out, size_t n, int k)
{
	snprintf(out, n, k == 1 ? "1 song" : "%d songs", k);
}

static void row_artist(struct ui *ui, struct ui_canvas *c, int i, int x, int y, int w,
		       int h, bool sel)
{
	const struct artist *a = &artists[i];
	char d[96], s[32];

	songs_count(s, sizeof(s), a->nsongs);
	snprintf(d, sizeof(d), "%d %s · %s", a->nalbums, a->nalbums == 1 ? "album" : "albums", s);
	row2(c, x, y, w, h, a->name, d, UI_SANS);
}

static void album_detail(const struct album *a, bool by, char *out, size_t n)
{
	char s[32], yr[16] = "";

	songs_count(s, sizeof(s), a->n);
	if (a->year)
		snprintf(yr, sizeof(yr), "%d · ", a->year);
	snprintf(out, n, "%s%s%s%s", by ? a->by : "", by ? " · " : "", yr, s);
}

static void row_album(struct ui *ui, struct ui_canvas *c, int i, int x, int y, int w,
		      int h, bool sel)
{
	char d[400];

	album_detail(&albums[album_order[i]], true, d, sizeof(d));
	row2(c, x, y, w, h, albums[album_order[i]].name, d, UI_SANS);
}

static void row_artist_page(struct ui *ui, struct ui_canvas *c, int i, int x, int y, int w,
			    int h, bool sel)
{
	char d[400];

	if (arows[i].album < 0) {
		int n = 0;
		for (int k = 0; k < nartists; k++)
			if (!strcmp(artists[k].name, artist_open))
				n = artists[k].nsongs;
		songs_count(d, sizeof(d), n);
		row2(c, x, y, w, h, "All songs", d, UI_BOLD);
		return;
	}
	const struct album *a = &albums[arows[i].album];
	if (arows[i].on)
		snprintf(d, sizeof(d), "On %s's album", a->by);
	else
		album_detail(a, false, d, sizeof(d));
	row2(c, x, y, w, h, a->name, d, UI_SANS);
}

/* A song of an album (its number, and its artist if not the album's), or
 * of all an artist's (its album) */
static void row_track(struct ui *ui, struct ui_canvas *c, int i, int x, int y, int w,
		      int h, bool sel)
{
	const struct tsong *t = &tsongs[tracks[i]];
	char name[300], title[256];
	const char *detail = "";

	if (t->title[0])
		snprintf(title, sizeof(title), "%s", t->title);
	else
		song_name(t->path, title, sizeof(title));
	if (tracks_album >= 0 && t->track > 0)
		snprintf(name, sizeof(name), "%d  %s", t->track, title);
	else
		snprintf(name, sizeof(name), "%s", title);
	if (tracks_album < 0)
		detail = t->album;
	else if (t->artist[0] && strcasecmp(t->artist, albums[tracks_album].by))
		detail = t->artist;
	row2(c, x, y, w, h, name, detail, UI_SANS);
}

static void row_list(struct ui *ui, struct ui_canvas *c, int i, int x, int y, int w,
		     int h, bool sel)
{
	if (i == 0) {
		ui_text_box(c, x + 16, y + 16, w - 32, UI_SANS, 19, UI_ACCENT, UI_LEFT,
			    "+ New playlist");
	} else {
		struct item *it = &lists[i - 1];
		ui_text_box(c, x + 16, y + 16, w - 32, UI_SANS, 19, UI_FG, UI_LEFT, it->name);
	}
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void row_song(struct ui *ui, struct ui_canvas *c, int i, int x, int y, int w,
		     int h, bool sel)
{
	char name[256];

	display_name(songs[i], name, sizeof(name));
	ui_text_box(c, x + 16, y + 16, w - 32, UI_SANS, 19, i == moving ? UI_ACCENT : UI_FG,
		    UI_LEFT, name);
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void row_queue(struct ui *ui, struct ui_canvas *c, int i, int x, int y, int w,
		      int h, bool sel)
{
	bool cur = i == queue_cur;

	ui_text_box(c, x + 16, y + 16, w - 52, cur ? UI_BOLD : UI_SANS, 19,
		    cur ? UI_ACCENT : UI_FG, UI_LEFT, queue[i]);
	if (cur)
		ui_text_box(c, x + w - 40, y + 16, 26, UI_SANS, 17, UI_ACCENT, UI_RIGHT,
			    pl.paused ? "❚❚" : "▶");
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void row_opt(struct ui *ui, struct ui_canvas *c, int i, int x, int y, int w,
		    int h, bool sel)
{
	ui_text_box(c, x + 16, y + 14, w - 32, UI_SANS, 19,
		    opts[i] == O_DELETE ? UI_WARN : UI_FG, UI_LEFT, opt_names[opts[i]]);
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void row_pick(struct ui *ui, struct ui_canvas *c, int i, int x, int y, int w,
		     int h, bool sel)
{
	ui_text_box(c, x + 16, y + 16, w - 32, UI_SANS, 19, i ? UI_FG : UI_ACCENT, UI_LEFT,
		    i ? lists[i - 1].name : "+ New playlist");
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

static void draw_now(struct ui *ui, struct ui_canvas *c)
{
	char a[32], b[32], line[64];
	int y = UI_HEADER_H + 40;

	ui_header(ui, c, "Now playing");
	if (pl.idle && !pl.path[0]) {
		ui_text_wrap(c, 24, y, c->w - 48, UI_SANS, 19, UI_DIM,
			     "Nothing is playing. Back to the lists, then OK on a song, a folder or a playlist.");
		ui_hints(c, "Lists", NULL, NULL);
		return;
	}
	/* The cover from under the header to the hints, its top whole, behind
	 * the title and the controls: blended with the background behind the
	 * text, clear between, partly again behind the controls */
	int by = now_bar_y(c->h);
	int art_h = c->h - UI_HINTS_H - UI_HEADER_H;
	/* The text's height first (no canvas: measured only), for the fade */
	int text_h = ui_text_wrap(NULL, 20, y, c->w - 40, UI_BOLD, 24, UI_FG,
				  pl.title[0] ? pl.title : "…") + 10;
	if (pl.artist[0])
		text_h += ui_text_wrap(NULL, 20, y, c->w - 40, UI_SANS, 19, UI_ACCENT, pl.artist) + 4;
	if (pl.album[0])
		text_h += ui_text_wrap(NULL, 20, y, c->w - 40, UI_SANS, 17, UI_DIM, pl.album) + 4;
	struct ui_fade fade = {
		.solid = y + text_h + 8 - UI_HEADER_H, .ease = 120, .top_a = 150,
		.mid_a = 0, .bottom = by - 40 - UI_HEADER_H, .bottom_a = 165,
	};
	const uint32_t *big = ui_art_backdrop(ui, pl.path, c->w, art_h, &fade);
	snprintf(line, sizeof(line), "%d of %d%s%s", pl.ppos + 1, pl.pcount,
		 pl.shuffle ? "   · shuffle" : "",
		 pl.loop_file ? "   · repeat one" : pl.loop_pl ? "   · repeat all" : "");

	/* What stands still (the cover, the text with its halo, the bar's
	 * panel), drawn once and kept: the halos alone took most of a core at
	 * the visualizer's frame rate (D084). Each frame then copies it and
	 * draws what moves. */
	static uint32_t *still;
	static size_t still_n;
	static char still_key[1800];
	char key[1800];
	snprintf(key, sizeof(key), "%p %d %d %d %d %08x %08x %08x %08x|%s|%s|%s|%s|%s",
		 (const void *)big, c->w, c->h, c->stride, vis_mode != VIS_OFF, UI_BG, UI_FG,
		 UI_ACCENT, UI_BAR, pl.path, pl.title, pl.artist, pl.album, line);
	size_t n = (size_t)c->w * art_h;
	if (still && still_n == n && !strcmp(key, still_key)) {
		for (int r = 0; r < art_h; r++)
			memcpy(&c->px[(size_t)(UI_HEADER_H + r) * c->stride], &still[(size_t)r * c->w],
			       c->w * sizeof(*still));
	} else {
		if (big)
			ui_blit(c, 0, UI_HEADER_H, big, c->w, art_h);

		/* The text over the cover with a halo under it, to read on any picture */
		y += ui_text_wrap_halo(c, 20, y, c->w - 40, UI_BOLD, 24, UI_FG,
				       pl.title[0] ? pl.title : "…") + 10;
		if (pl.artist[0])
			y += ui_text_wrap_halo(c, 20, y, c->w - 40, UI_SANS, 19, UI_ACCENT, pl.artist) + 4;
		if (pl.album[0])
			y += ui_text_wrap_halo(c, 20, y, c->w - 40, UI_SANS, 17, UI_FG, pl.album) + 4;

		/* No cover: a record in the room left under the text */
		if (!big) {
			int room = by - 30 - (y + 10), r = room / 2;
			if (r > c->w * 32 / 100)
				r = c->w * 32 / 100;
			if (r >= 24)
				ui_vinyl(c, c->w / 2, y + 10 + room / 2, r);
		}
		/* The slider and its times on a panel of their own, mostly
		 * opaque, so they stand out from any cover */
		ui_round_alpha(c, 8, by - 14, c->w - 16, 58, 14, UI_BG, 205);
		ui_text_box_halo(c, 40, NOW_LINE_Y(c->h), c->w - 80, UI_SANS, 15, UI_FG, UI_CENTER,
				 line);
		if (still_n != n) {
			free(still);
			still = malloc(n * sizeof(*still));
			still_n = still ? n : 0;
		}
		for (int r = 0; still && r < art_h; r++)
			memcpy(&still[(size_t)r * c->w], &c->px[(size_t)(UI_HEADER_H + r) * c->stride],
			       c->w * sizeof(*still));
		snprintf(still_key, sizeof(still_key), "%s", key);
	}

	int bx = 20, bw = c->w - 40;
	double f = pl.duration > 0 ? shown_time() / pl.duration : 0;
	f = f < 0 ? 0 : f > 1 ? 1 : f;
	ui_round(c, bx, by, bw, 8, 4, UI_BAR);
	ui_round(c, bx, by, (int)(bw * f), 8, 4, UI_ACCENT);
	ui_round(c, bx + (int)(bw * f) - 7, by - 3, 14, 14, 7, UI_FG);
	fmt_time(shown_time(), a, sizeof(a));
	fmt_time(pl.duration, b, sizeof(b));
	ui_text(c, bx, by + 16, UI_SANS, 15, UI_FG, a);
	ui_text_box(c, bx, by + 16, bw, UI_SANS, 15, UI_FG, UI_RIGHT, b);
	/* Play or pause small between the times, the visualizer under */
	ui_np_icon(c, c->w / 2, by + 24, 18, pl.paused ? UI_NP_PLAY : UI_NP_PAUSE, UI_FG);
	if (vis_mode != VIS_OFF)
		vis_draw(c, bx, by + 44, bw, VIS_H);
	ui_hints(c, "Lists", pl.paused ? "Play" : "Pause", "C stop  ◂▸ scrub");
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	int top = UI_HEADER_H, h = c->h - top - UI_HINTS_H - mini_h();

	switch (view) {
	case V_FOLDERS: {
		folders.count = nitems;
		ui_list_draw(ui, c, &folders, top, h, row_folder);
		tabs(c, 0);
		if (!nitems) {
			char t[700];
			snprintf(t, sizeof(t), strcmp(cur_dir, music_dir) ? "Nothing to play here." :
				 "No music yet. Put songs in Music on the SD card (any folders).");
			ui_text_wrap(c, 24, top + 40, c->w - 48, UI_SANS, 19, UI_DIM, t);
		}
		mini(ui, c);
		ui_hints(c, strcmp(cur_dir, music_dir) ? "Up" : "Close", nitems ? "Play" : NULL,
			 "* more  # now");
		break;
	}
	case V_SONGS:
		library_l.count = nlibrary;
		ui_list_draw(ui, c, &library_l, top, h, row_library);
		tabs(c, 3);
		if (!nlibrary)
			ui_text_wrap(c, 24, top + 40, c->w - 48, UI_SANS, 19, UI_DIM,
				     "No music yet. Put songs in Music on the SD card (any folders).");
		mini(ui, c);
		ui_hints(c, "Close", nlibrary ? "Play" : NULL, "* more  # now");
		break;
	case V_ARTISTS:
	case V_ALBUMS: {
		bool ar = view == V_ARTISTS;
		struct ui_list *l = ar ? &artists_l : &albums_l;
		l->count = ar ? nartists : nalbums;
		ui_list_draw(ui, c, l, top, h, ar ? row_artist : row_album);
		tabs(c, ar ? 1 : 2);
		if (!l->count) {
			char t[160];
			if (tags_fd >= 0 && tags_todo > 0)
				snprintf(t, sizeof(t), "Reading the songs' tags: %d of %d…", tags_done,
					 tags_todo);
			else if (tags_fd >= 0)
				snprintf(t, sizeof(t), "Reading the songs' tags…");
			else
				snprintf(t, sizeof(t), "No music yet. Put songs in Music on the SD card (any folders).");
			ui_text_wrap(c, 24, top + 40, c->w - 48, UI_SANS, 19, UI_DIM, t);
		}
		mini(ui, c);
		ui_hints(c, "Close", l->count ? "Open" : NULL, "* more  # now");
		break;
	}
	case V_ARTIST:
		ui_list_draw(ui, c, &artist_l, top, h, row_artist_page);
		ui_header(ui, c, artist_open);
		mini(ui, c);
		ui_hints(c, "Artists", "Open", "* more  # now");
		break;
	case V_TRACKS:
		tracks_l.count = ntracks;
		ui_list_draw(ui, c, &tracks_l, top, h, row_track);
		ui_header(ui, c, tracks_album >= 0 ? albums[tracks_album].name : artist_open);
		mini(ui, c);
		ui_hints(c, tracks_from == V_ARTIST ? "Artist" : "Albums", ntracks ? "Play" : NULL,
			 "* more  # now");
		break;
	case V_LISTS:
		ui_list_draw(ui, c, &lists_l, top, h, row_list);
		tabs(c, 4);
		mini(ui, c);
		ui_hints(c, "Close", "Open", "* more  # now");
		break;
	case V_QUEUE:
		queue_l.count = nqueue;
		ui_list_draw(ui, c, &queue_l, top, h, row_queue);
		tabs(c, 5);
		if (!nqueue)
			ui_text_wrap(c, 24, top + 40, c->w - 48, UI_SANS, 19, UI_DIM,
				     "The queue is empty.");
		mini(ui, c);
		ui_hints(c, "Close", nqueue ? "Play" : NULL, nqueue ? "C remove  # now" : NULL);
		break;
	case V_LIST: {
		char title[256];
		display_name(open_list, title, sizeof(title));
		ui_list_draw(ui, c, &songs_l, top, h, row_song);
		ui_header(ui, c, title);
		if (!nsongs)
			ui_text_wrap(c, 24, top + 40, c->w - 48, UI_SANS, 19, UI_DIM,
				     "Empty. In Folders, * on a song or folder adds it here.");
		mini(ui, c);
		ui_hints(c, moving >= 0 ? "Done" : "Playlists", moving >= 0 ? "Done" : nsongs ? "Play" : NULL,
			 moving >= 0 ? "▴▾ move" : open_editable && nsongs ? "* more  C remove" : "* more");
		break;
	}
	case V_NOW:
		draw_now(ui, c);
		break;
	case V_OPTS:
		ui_list_draw(ui, c, &opts_l, top, h, row_opt);
		ui_header(ui, c, target_name[0] ? target_name : "Options");
		ui_hints(c, "Back", "Choose", NULL);
		break;
	case V_PICK:
		ui_list_draw(ui, c, &pick_l, top, h, row_pick);
		ui_header(ui, c, "Add to a playlist");
		ui_hints(c, "Back", "Add", NULL);
		break;
	case V_NAME:
		ui_header(ui, c, "New playlist");
		ui_text_box(c, 20, top + 20, c->w - 40, UI_SANS, 17, UI_DIM, UI_LEFT, "Its name:");
		ui_round(c, 16, top + 48, c->w - 32, 50, 10, UI_BAR);
		ui_entry_draw(c, &name_entry, 28, top + 60, c->w - 56, 20, 1);
		ui_entry_indicator(c, &name_entry, 0, 0, c->w);
		ui_hints(c, "Cancel", name_entry.text[0] ? "Make" : NULL, NULL);
		break;
	}
	if (flash[0] && now_ms() < flash_until) {
		ui_round(c, 30, c->h - UI_HINTS_H - mini_h() - 56, c->w - 60, 44, 12, UI_BAR);
		ui_text_box(c, 30, c->h - UI_HINTS_H - mini_h() - 45, c->w - 60, UI_SANS, 17,
			    UI_FG, UI_CENTER, flash);
	}
}

/* ------------------------------------------------------------- keys */

static void now_key(struct ui *ui, uint32_t code)
{
	if (!mpv_ready(ui))
		return;
	switch (code) {
	case KEY_OK:
	case KEY_ENTER:
	case KEY_5:
		mpv_cmd("cycle", "pause");
		break;
	case KEY_LEFT:		/* tapped: see scrub_end */
		if (pl.time > 3)
			mpv_cmd("seek", "0", "absolute");
		else
			mpv_cmd("playlist-prev");
		break;
	case KEY_RIGHT:
		mpv_cmd("playlist-next");
		break;
	case KEY_4: mpv_cmd("seek", "-10"); break;
	case KEY_6: mpv_cmd("seek", "10"); break;
	case KEY_1: mpv_cmd("seek", "-60"); break;
	case KEY_3: mpv_cmd("seek", "60"); break;
	case KEY_7:
		pl.shuffle = !pl.shuffle;
		mpv_cmd(pl.shuffle ? "playlist-shuffle" : "playlist-unshuffle");
		say(ui, pl.shuffle ? "Shuffle on" : "Shuffle off");
		break;
	case KEY_BACKSPACE:	/* C: stop, keeping the queue (D060) */
		mpv_cmd("stop", "keep-playlist");
		say(ui, "Stopped");
		break;
	case KEY_0:		/* the visualizer: bars, spectrum, scope, off */
		vis_mode = (vis_mode + 1) % VIS_COUNT;
		ui_setting_set("music_vis", vis_keys[vis_mode]);
		if (vis_mode == VIS_OFF)
			say(ui, "Visualizer off");
		else {
			char t[48];
			snprintf(t, sizeof(t), "Visualizer: %s", vis_titles[vis_mode]);
			say(ui, t);
		}
		break;
	case KEY_9:		/* off -> all -> one -> off */
		if (!pl.loop_pl && !pl.loop_file) {
			mpv_cmd("set", "loop-playlist", "inf");
			say(ui, "Repeat all");
		} else if (pl.loop_pl) {
			mpv_cmd("set", "loop-playlist", "no");
			mpv_cmd("set", "loop-file", "inf");
			say(ui, "Repeat one");
		} else {
			mpv_cmd("set", "loop-file", "no");
			say(ui, "Repeat off");
		}
		break;
	}
}

static void name_done(struct ui *ui)
{
	char file[1200], clean[256];
	int o = 0;

	for (const char *p = name_entry.text; *p && o < (int)sizeof(clean) - 1; p++)
		clean[o++] = *p == '/' ? '-' : *p;
	clean[o] = 0;
	if (!clean[0])
		return;
	mkdir(lists_dir, 0755);
	snprintf(file, sizeof(file), "%s/%s.m3u8", lists_dir, clean);
	if (access(file, F_OK) == 0) {
		say(ui, "A playlist has that name");
		return;
	}
	if (ntarget)
		add_to(ui, file);		/* "Add to a playlist", a new one */
	else if (!write_list(file, NULL, 0))
		say(ui, "Could not make the playlist");
	lists_load();
	view = opts_from == V_LISTS || !ntarget ? V_LISTS : opts_from;
	ui_redraw(ui);
}

static int page_rows(struct ui *ui, struct ui_list *l)
{
	int page = (ui_height(ui) - UI_HEADER_H - UI_HINTS_H - mini_h()) / l->row_h;

	return page > 1 ? page : 1;
}

/* The name a list is in order of, for 1 and 3: the lists by name only */
static bool by_letter(void)
{
	return view == V_FOLDERS || view == V_ARTISTS || view == V_ALBUMS || view == V_SONGS ||
	       view == V_LISTS;
}

static int letter(int i)
{
	const char *name = view == V_FOLDERS ? items[i].name : view == V_ARTISTS ? artists[i].name :
		view == V_ALBUMS ? albums[album_order[i]].name : view == V_SONGS ? library[i].name :
		view == V_LISTS && i ? lists[i - 1].name : "";
	unsigned char ch = name[0];

	return isalpha(ch) ? toupper(ch) : '#';
}

/* 1 and 3: the start of this letter (or the one before), or of the next */
static void letter_jump(struct ui *ui, struct ui_list *l, bool next)
{
	int i = l->sel, cur = letter(i);

	if (next) {
		while (i < l->count - 1 && letter(i) == cur)
			i++;
		if (letter(i) == cur)
			return;
	} else {
		while (i > 0 && letter(i - 1) == cur)
			i--;
		if (i == l->sel && i > 0) {
			int before = letter(--i);
			while (i > 0 && letter(i - 1) == before)
				i--;
		}
	}
	l->sel = i;
	/* The letter's first row at the top */
	int rows = page_rows(ui, l), last = l->count - rows;
	l->top = i < last ? i : last > 0 ? last : 0;
	char t[8] = { (char)letter(i), 0 };
	say(ui, t);
}

static void list_key(struct ui *ui, uint32_t code, struct ui_list *l)
{
	if (code == KEY_2)
		code = KEY_UP;
	if (code == KEY_8)
		code = KEY_DOWN;
	if ((code == KEY_4 || code == KEY_6) && l->count) {	/* a page up or down */
		int page = page_rows(ui, l);
		l->sel += code == KEY_4 ? -page : page;
		if (l->sel >= l->count)
			l->sel = l->count - 1;
		if (l->sel < 0)
			l->sel = 0;
		return;
	}
	if ((code == KEY_1 || code == KEY_3) && l->count && by_letter()) {
		letter_jump(ui, l, code == KEY_3);
		return;
	}
	ui_list_key(l, code);
}

/* A tab opened: its list read again where that is quick */
static void tab_open(struct ui *ui, int t)
{
	view = tab_views[t];
	if (view == V_SONGS)
		library_load();
	if (view == V_LISTS)
		lists_load();
	if (view == V_QUEUE)
		queue_refresh();
	if (view == V_ARTISTS || view == V_ALBUMS)
		tags_start(ui);		/* new songs since: a moment */
}

/* The list on screen, for taps and drags */
static struct ui_list *view_list(void)
{
	switch (view) {
	case V_FOLDERS: return &folders;
	case V_ARTISTS: return &artists_l;
	case V_ALBUMS: return &albums_l;
	case V_SONGS: return &library_l;
	case V_LISTS: return &lists_l;
	case V_QUEUE: return &queue_l;
	case V_LIST: return &songs_l;
	case V_ARTIST: return &artist_l;
	case V_TRACKS: return &tracks_l;
	case V_OPTS: return &opts_l;
	case V_PICK: return &pick_l;
	default: return NULL;
	}
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st, const char *text)
{
	if (view == V_NAME) {
		if (st == UI_PRESSED && (code == KEY_BACK || code == KEY_ESC)) {
			view = V_LISTS;
		} else if (st == UI_PRESSED && (code == KEY_OK || code == KEY_ENTER)) {
			name_done(ui);
		} else {
			ui_entry_key(&name_entry, code, st);
		}
		ui_redraw(ui);
		return;
	}
	if (st == UI_RELEASED && scrub_key && code == scrub_key) {
		if (!scrub_end() && view == V_NOW)
			now_key(ui, code);	/* a tap: the song before or after */
		ui_redraw(ui);
		return;
	}
	if (st == UI_RELEASED)
		return;
	bool press = st == UI_PRESSED;

	/* 5 in a list: play or pause, while something is loaded */
	if (press && code == KEY_5 && mini_h() &&
	    (tab_of(view) >= 0 || view == V_LIST || view == V_ARTIST || view == V_TRACKS)) {
		if (mpv_ready(ui))
			mpv_cmd("cycle", "pause");
		return;
	}

	/* The tabs */
	if (tab_of(view) >= 0 && (code == KEY_LEFT || code == KEY_RIGHT) && press) {
		tab_open(ui, (tab_of(view) + (code == KEY_RIGHT ? 1 : NTABS - 1)) % NTABS);
		ui_redraw(ui);
		return;
	}
	if (press && code == KEY_NUMERIC_POUND && view != V_NOW && view != V_OPTS &&
	    view != V_PICK) {
		show_now(ui);
		mpv_ready(ui);
		ui_redraw(ui);
		return;
	}

	switch (view) {
	case V_FOLDERS:
		if (code == KEY_BACK || code == KEY_ESC) {
			if (!press)
				break;
			if (strcmp(cur_dir, music_dir)) {
				char up[1024];
				snprintf(up, sizeof(up), "%s", cur_dir);
				char *slash = strrchr(up, '/');
				char was[256] = "";
				if (slash) {
					snprintf(was, sizeof(was), "%s", slash + 1);
					*slash = 0;
				}
				folder_load(up);
				for (int i = 0; i < nitems; i++)
					if (items[i].dir && !strcmp(items[i].name, was))
						folders.sel = i;
			} else {
				ui_quit(ui);
			}
		} else if ((code == KEY_OK || code == KEY_ENTER) && press && nitems) {
			struct item *it = &items[folders.sel];
			if (it->dir) {
				folder_load(it->path);
			} else if (it->list) {
				char (*p)[1024] = calloc(MAX_ITEMS, sizeof(*p));
				int n = p ? read_list(it->path, p, MAX_ITEMS) : 0;
				play_paths(ui, p, n, 0, false);
				free(p);
				show_now(ui);
			} else {
				char (*p)[1024] = calloc(MAX_ITEMS, sizeof(*p));
				int n = p ? folder_songs(cur_dir, p, MAX_ITEMS, false) : 0, start = 0;
				for (int i = 0; i < n; i++)
					if (!strcmp(p[i], it->path))
						start = i;
				play_paths(ui, p, n, start, false);
				free(p);
				show_now(ui);
			}
		} else if (code == KEY_NUMERIC_STAR && press) {
			opts_open(ui, V_FOLDERS);
		} else {
			list_key(ui, code, &folders);
		}
		break;
	case V_SONGS:
		if ((code == KEY_BACK || code == KEY_ESC) && press) {
			ui_quit(ui);
		} else if ((code == KEY_OK || code == KEY_ENTER) && press && nlibrary) {
			library_play(ui, library_l.sel, false);
			show_now(ui);
		} else if (code == KEY_NUMERIC_STAR && press) {
			opts_open(ui, V_SONGS);
		} else {
			list_key(ui, code, &library_l);
		}
		break;
	case V_ARTISTS:
		if ((code == KEY_BACK || code == KEY_ESC) && press) {
			ui_quit(ui);
		} else if ((code == KEY_OK || code == KEY_ENTER) && press && nartists) {
			artist_page(artists[artists_l.sel].name);
			artist_l.sel = artist_l.top = 0;
			view = V_ARTIST;
		} else if (code == KEY_NUMERIC_STAR && press) {
			opts_open(ui, V_ARTISTS);
		} else {
			list_key(ui, code, &artists_l);
		}
		break;
	case V_ALBUMS:
		if ((code == KEY_BACK || code == KEY_ESC) && press) {
			ui_quit(ui);
		} else if ((code == KEY_OK || code == KEY_ENTER) && press && nalbums) {
			tracks_open(album_order[albums_l.sel]);
			tracks_l.sel = tracks_l.top = 0;
			tracks_from = V_ALBUMS;
			view = V_TRACKS;
		} else if (code == KEY_NUMERIC_STAR && press) {
			opts_open(ui, V_ALBUMS);
		} else {
			list_key(ui, code, &albums_l);
		}
		break;
	case V_ARTIST:
		if ((code == KEY_BACK || code == KEY_ESC) && press) {
			view = V_ARTISTS;
		} else if ((code == KEY_OK || code == KEY_ENTER) && press && narows) {
			tracks_open(arows[artist_l.sel].album);
			tracks_l.sel = tracks_l.top = 0;
			tracks_from = V_ARTIST;
			view = V_TRACKS;
		} else if (code == KEY_NUMERIC_STAR && press) {
			opts_open(ui, V_ARTIST);
		} else {
			list_key(ui, code, &artist_l);
		}
		break;
	case V_TRACKS:
		if ((code == KEY_BACK || code == KEY_ESC) && press) {
			view = tracks_from;
		} else if ((code == KEY_OK || code == KEY_ENTER) && press && ntracks) {
			char (*p)[1024] = calloc(ntracks, sizeof(*p));
			if (p) {
				song_paths(tracks, ntracks, p);
				play_paths(ui, p, ntracks, tracks_l.sel, false);
				free(p);
				show_now(ui);
			}
		} else if (code == KEY_NUMERIC_STAR && press) {
			opts_open(ui, V_TRACKS);
		} else {
			list_key(ui, code, &tracks_l);
		}
		break;
	case V_LISTS:
		if ((code == KEY_BACK || code == KEY_ESC) && press) {
			ui_quit(ui);
		} else if ((code == KEY_OK || code == KEY_ENTER) && press) {
			if (lists_l.sel == 0) {
				ntarget = 0;
				opts_from = V_LISTS;
				ui_entry_init(ui, &name_entry, false);
				view = V_NAME;
			} else {
				list_open(lists[lists_l.sel - 1].path);
			}
		} else if (code == KEY_NUMERIC_STAR && press) {
			opts_open(ui, V_LISTS);
		} else {
			list_key(ui, code, &lists_l);
		}
		break;
	case V_QUEUE:
		if ((code == KEY_BACK || code == KEY_ESC) && press) {
			ui_quit(ui);
		} else if ((code == KEY_OK || code == KEY_ENTER) && press && nqueue) {
			char idx[16];
			snprintf(idx, sizeof(idx), "%d", queue_l.sel);
			mpv_cmd("playlist-play-index", idx);
			mpv_cmd("set", "pause", "no");
		} else if (code == KEY_BACKSPACE && press && nqueue) {
			char idx[16];
			snprintf(idx, sizeof(idx), "%d", queue_l.sel);
			mpv_cmd("playlist-remove", idx);
			queue_refresh();
		} else if (code == KEY_NUMERIC_STAR && press) {
			opts_open(ui, V_QUEUE);
		} else {
			list_key(ui, code, &queue_l);
		}
		break;
	case V_LIST:
		if (moving >= 0) {
			int to = moving;
			if (code == KEY_UP || code == KEY_2)
				to = moving - 1;
			else if (code == KEY_DOWN || code == KEY_8)
				to = moving + 1;
			else if (press && (code == KEY_OK || code == KEY_ENTER || code == KEY_BACK ||
					   code == KEY_ESC)) {
				moving = -1;
				list_save(ui);
				break;
			}
			if (to != moving && to >= 0 && to < nsongs) {
				char t[1024];
				memcpy(t, songs[to], sizeof(t));
				memcpy(songs[to], songs[moving], sizeof(t));
				memcpy(songs[moving], t, sizeof(t));
				moving = to;
				songs_l.sel = to;
			}
			break;
		}
		if ((code == KEY_BACK || code == KEY_ESC) && press) {
			view = V_LISTS;
			lists_load();
		} else if ((code == KEY_OK || code == KEY_ENTER) && press && nsongs) {
			play_paths(ui, songs, nsongs, songs_l.sel, false);
			show_now(ui);
		} else if (code == KEY_BACKSPACE && press && nsongs && open_editable) {
			memmove(songs[songs_l.sel], songs[songs_l.sel + 1],
				(size_t)(nsongs - songs_l.sel - 1) * sizeof(songs[0]));
			nsongs--;
			songs_l.count = nsongs;
			if (songs_l.sel >= nsongs && nsongs)
				songs_l.sel = nsongs - 1;
			list_save(ui);
		} else if (code == KEY_NUMERIC_STAR && press) {
			opts_open(ui, V_LIST);
		} else {
			list_key(ui, code, &songs_l);
		}
		break;
	case V_NOW:
		if ((code == KEY_BACK || code == KEY_ESC) && press) {
			view = opts_from == V_NOW || opts_from == V_OPTS || opts_from == V_PICK ||
			       opts_from == V_NAME ? V_FOLDERS : opts_from;
			if (view == V_QUEUE)
				queue_refresh();
		} else if (code == KEY_NUMERIC_STAR && press) {
			opts_open(ui, V_NOW);
		} else if (code == KEY_LEFT || code == KEY_RIGHT) {
			if (press) {	/* held, the timer scrubs; released, see above */
				scrub_key = code;
				scrubbing = false;
				scrub_since = now_ms();
				ui_timer(ui, 100);
			}
		} else if (press || code == KEY_4 || code == KEY_6) {
			now_key(ui, code);
		}
		break;
	case V_OPTS:
		if ((code == KEY_BACK || code == KEY_ESC) && press)
			view = opts_from;
		else if ((code == KEY_OK || code == KEY_ENTER) && press)
			opt_choose(ui);
		else
			list_key(ui, code, &opts_l);
		break;
	case V_PICK:
		if ((code == KEY_BACK || code == KEY_ESC) && press) {
			view = opts_from;
		} else if ((code == KEY_OK || code == KEY_ENTER) && press) {
			if (pick_l.sel == 0) {
				ui_entry_init(ui, &name_entry, false);
				view = V_NAME;
			} else {
				add_to(ui, lists[pick_l.sel - 1].path);
				view = opts_from;
			}
		} else {
			list_key(ui, code, &pick_l);
		}
		break;
	case V_NAME:
		break;
	}
	if (view == V_NOW || vis_fd >= 0)
		ui_timer(ui, 20);	/* the visualizer started or stopped */
	ui_redraw(ui);
}

/* ------------------------------------------------------------- touch */

static void tap(struct ui *ui, int x, int y)
{
	int h = ui_height(ui);

	if (view == V_NOW) {
		int by = now_bar_y(h);
		if (y >= by - 20 && y <= by + 12 && pl.duration > 0) {	/* the bar, not its times */
			char t[32];
			double f = (double)(x - 20) / (ui_width(ui) - 40);
			f = f < 0 ? 0 : f > 1 ? 1 : f;
			snprintf(t, sizeof(t), "%.1f", f * pl.duration);
			mpv_cmd("seek", t, "absolute");
		} else {
			now_key(ui, KEY_OK);
		}
		return;
	}
	if (mini_h() && y >= h - UI_HINTS_H - mini_h() && y < h - UI_HINTS_H) {
		show_now(ui);
		ui_redraw(ui);
		return;
	}
	if (y < UI_HEADER_H && tab_of(view) >= 0) {
		int sx[NTABS + 1], i = 0;
		tab_spans(ui_width(ui), sx);
		while (i < NTABS - 1 && x >= sx[i + 1])
			i++;
		tab_open(ui, i);
		ui_redraw(ui);
		return;
	}
	struct ui_list *l = view_list();
	if (!l)
		return;
	int i = ui_list_at(l, UI_HEADER_H, y);
	if (i < 0)
		return;
	l->sel = i;
	key(ui, KEY_OK, UI_PRESSED, "");
}

static void drag(struct ui *ui, int dx, int dy, bool done)
{
	struct ui_list *l = view_list();

	if (l) {
		ui_list_drag(l, dy, ui_height(ui) - UI_HEADER_H - UI_HINTS_H - mini_h());
		ui_redraw(ui);
	}
}

/* The timer: reconnecting to the player, and the clock of the song */
static void tick(struct ui *ui)
{
	mpv_flush();
	if (mpv_fd < 0 && connect_tries) {
		if (mpv_connect(ui))
			connect_tries = 0;
		else if (connect_tries < 30) {
			connect_tries++;
			ui_timer(ui, 300);
			return;
		}
	}
	/* The song's clock: twice a second at most, however fast the frames */
	static long long asked;
	if (mpv_fd >= 0 && now_ms() - asked >= 400) {
		mpv_cmdv(R_TIME, (const char *const[]){ "get_property", "time-pos", NULL });
		asked = now_ms();
	}
	bool playing = mpv_fd >= 0 && !pl.paused && mini_h();
	vis_update(ui);
	if (scrub_key) {
		scrub_step(ui);
		ui_timer(ui, 100);
	} else if (vis_moving() && ui_visible(ui)) {
		ui_redraw(ui);
		ui_timer(ui, 16);	/* the visualizer's frames: 60 a second */
	} else if (view == V_NOW || playing || (flash[0] && now_ms() < flash_until + 500)) {
		ui_redraw(ui);
		ui_timer(ui, view == V_NOW && playing ? 500 : 1000);
	}
	if (olen)
		ui_timer(ui, 50);	/* more for mpv, as soon as it reads */
}

static void focus(struct ui *ui, bool focused)
{
	if (!focused)
		ui_timer(ui, 600);	/* covered: the visualizer stops (ui_visible) */
	if (!focused && scrub_key) {
		scrub_end();
		ui_redraw(ui);
	}
}

static void start(struct ui *ui)
{
	const char *home = getenv("HOME");

	snprintf(music_dir, sizeof(music_dir), "%s/Music", home ? home : ".");
	snprintf(lists_dir, sizeof(lists_dir), "%s/Playlists", music_dir);
	mkdir(music_dir, 0755);
	items = calloc(MAX_ITEMS, sizeof(*items));
	lists = calloc(MAX_ITEMS, sizeof(*lists));
	library = calloc(MAX_ITEMS, sizeof(*library));
	songs = calloc(MAX_ITEMS, sizeof(*songs));
	target = calloc(MAX_ITEMS, sizeof(*target));
	queue = calloc(MAX_ITEMS, sizeof(*queue));
	queue_path = calloc(MAX_ITEMS, sizeof(*queue_path));
	tsongs = calloc(MAX_ITEMS, sizeof(*tsongs));
	order = calloc(MAX_ITEMS, sizeof(*order));
	albums = calloc(MAX_ITEMS, sizeof(*albums));
	album_order = calloc(MAX_ITEMS, sizeof(*album_order));
	artists = calloc(MAX_ITEMS, sizeof(*artists));
	arows = calloc(MAX_ITEMS, sizeof(*arows));
	tracks = calloc(MAX_ITEMS, sizeof(*tracks));
	if (!items || !library || !lists || !songs || !target || !queue || !queue_path ||
	    !tsongs || !order || !albums || !album_order || !artists || !arows || !tracks) {
		fprintf(stderr, "s22-music: out of memory\n");
		ui_quit(ui);
		return;
	}
	folder_load(music_dir);
	char vis[16];
	ui_setting("music_vis", "bars", vis, sizeof(vis));
	for (int i = 0; i < VIS_COUNT; i++)
		if (!strcmp(vis, vis_keys[i]))
			vis_mode = i;
	/* The tags as last read, and read again for new songs */
	const char *cache = getenv("XDG_CACHE_HOME");
	if (cache && cache[0])
		snprintf(tag_cache, sizeof(tag_cache), "%s/s22-music/tags", cache);
	else
		snprintf(tag_cache, sizeof(tag_cache), "%s/.cache/s22-music/tags", home ? home : ".");
	tags_load();
	tags_start(ui);
	/* The player: connected if it runs, else started now, so it is up by
	 * the time a song is chosen */
	if (!mpv_connect(ui)) {
		ui_spawn("s22-musicd");
		connect_tries = 1;
		ui_timer(ui, 300);
	}
}

int main(void)
{
	struct ui_app app = {
		.app_id = "s22-music",
		.title = "Music",
		.draw = draw,
		.key = key,
		.tick = tick,
		.start = start,
		.focus = focus,
		.tap = tap,
		.drag = drag,
	};

	return ui_main(&app);
}
