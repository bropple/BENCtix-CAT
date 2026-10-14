// SPDX-License-Identifier: MIT
/*
 * s22-photos: the photos in ~/Pictures (and its folders), newest first, and
 * the wallpapers in ~/Wallpapers (D066): two tabs, * between them (or a
 * tap on the tab).
 *
 * Grid: thumbnails, made in the background (libjpeg's 1/8 scale decode;
 * ffmpeg's for a wallpaper, any picture it reads) and kept in
 * ~/.cache/s22/thumbs. D-pad moves, OK opens, Back closes.
 *
 * A photo: Left/Right the one before or after (or a swipe), Up/Down zoom
 * (1x, 2x, 4x; decoded again at the size needed), 2/4/6/8 or a drag move
 * around a zoomed photo, # shows its details, C deletes it (OK to
 * confirm), OK moves it to the wallpapers (for the inner screen, or the
 * outer one), Back returns to the grid.
 *
 * A wallpaper (Wallpapers/inner: the home and lock screens; Wallpapers/
 * outer: the outer screen) is seen as it will look: s22-wallpaper converts
 * it as it would for the screen (once, kept in ~/.cache/s22/wallpreview),
 * the inner one shown as the home screen would show it, the outer one as
 * the outer screen's 128x128 moving at the speed set, three times the
 * size. OK sets it (home, lock or both; outer), Left/Right the one before
 * or after, C deletes it, Back returns to the grid.
 *
 * Links: file:PATH opens that photo (the Camera app sends one);
 * wallpaper:home, :lock or :outer (Settings) shows the wallpapers for that
 * screen only, to choose one: OK sets it, * sets none, and Photos closes.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <jpeglib.h>
#include <math.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "s22ui.h"

#define MAX_PHOTOS	4000
#define MAX_WALLS	500
#define OUTER		128	/* the outer screen */
#define OUTER_ZOOM	3	/* its preview, three times the size */
#define ANIM_MS		50	/* s22-wallpaper's 20 frames a second */
#define COLS		3
#define GAP		4
#define THUMB		((480 - GAP * (COLS + 1)) / COLS)	/* 154 */
#define SCREEN_W	480	/* the window, below the status bar */
#define SCREEN_H	614

struct photo {
	char *path;
	time_t mtime;
	off_t size;
	uint32_t *thumb;	/* THUMB x THUMB, or NULL */
	bool failed;
	bool outer;		/* a wallpaper for the outer screen */
};

/* The tab shown: photos points at its list; the other's place is kept */
enum tab { T_PHOTOS, T_WALLS };
static enum tab tab;
static struct photo photo_list[MAX_PHOTOS], wall_list[MAX_WALLS];
static struct photo *photos = photo_list;
static int nphotos, sel, top_row;
static struct { int n, sel, top_row; } other;
/* Chosen for Settings (wallpaper:KIND): "home", "lock", "outer", or "" */
static char choosing[8];

enum view { V_GRID, V_PHOTO, V_WALL };
static enum view view;
static bool info, confirm_delete;
static char message[128];

/* What OK offers on a photo or a wallpaper: a list over it */
static const char *menu[4];
static int nmenu, menu_sel;
static bool menu_open;

/* The open photo, decoded */
static uint32_t *img;
static int img_w, img_h, img_scale;	/* scale: 1/img_scale of full size */
static int full_w, full_h;
static int zoom = 1;			/* 1, 2, 4 */
static double pan_x = 0.5, pan_y = 0.5;	/* the centre, 0..1 */

/* ---------------------------------------------------------- the files --- */

static int newest_first(const void *pa, const void *pb)
{
	const struct photo *a = pa, *b = pb;

	return a->mtime < b->mtime ? 1 : a->mtime > b->mtime ? -1 :
		strcmp(b->path, a->path);
}

static bool is_jpeg(const char *name)
{
	const char *dot = strrchr(name, '.');

	return dot && (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0);
}

static void scan(const char *dir, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (d == NULL)
		return;
	while ((e = readdir(d)) != NULL && nphotos < MAX_PHOTOS) {
		char path[1024];
		struct stat st;
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode) && depth < 3)
			scan(path, depth + 1);
		else if (S_ISREG(st.st_mode) && is_jpeg(e->d_name))
			photos[nphotos++] = (struct photo){ strdup(path), st.st_mtime,
				st.st_size };
	}
	closedir(d);
}

/* A picture ffmpeg reads, as a wallpaper */
static bool is_picture(const char *name)
{
	static const char *const ext[] = { ".jpg", ".jpeg", ".png", ".gif", ".webp",
		".bmp", ".ppm", ".tif", ".tiff" };
	const char *dot = strrchr(name, '.');

	for (unsigned i = 0; dot && i < sizeof(ext) / sizeof(ext[0]); i++)
		if (strcasecmp(dot, ext[i]) == 0)
			return true;
	return false;
}

static void wall_dir(bool outer, char *buf, size_t len)
{
	const char *home = getenv("HOME");

	snprintf(buf, len, "%s/Wallpapers/%s", home ? home : "/tmp",
		 outer ? "outer" : "inner");
}

static void scan_walls(bool outer)
{
	char dir[512];
	struct dirent *e;
	DIR *d;

	wall_dir(outer, dir, sizeof(dir));
	if ((d = opendir(dir)) == NULL)
		return;
	while ((e = readdir(d)) != NULL && nphotos < MAX_WALLS) {
		char path[1024];
		struct stat st;
		if (e->d_name[0] == '.' || !is_picture(e->d_name))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
			photos[nphotos++] = (struct photo){ strdup(path), st.st_mtime,
				st.st_size, .outer = outer };
	}
	closedir(d);
}

static void load_list(void)
{
	char dir[512];
	const char *home = getenv("HOME");

	for (int i = 0; i < nphotos; i++) {
		free(photos[i].path);
		free(photos[i].thumb);
	}
	nphotos = 0;
	if (tab == T_WALLS) {
		/* The inner screen's, then the outer's; choosing, that one's */
		if (strcmp(choosing, "outer"))
			scan_walls(false);
		int inner = nphotos;
		if (!choosing[0] || !strcmp(choosing, "outer"))
			scan_walls(true);
		qsort(photos, inner, sizeof(photos[0]), newest_first);
		qsort(photos + inner, nphotos - inner, sizeof(photos[0]), newest_first);
	} else {
		snprintf(dir, sizeof(dir), "%s/Pictures", home ? home : "/tmp");
		scan(dir, 0);
		qsort(photos, nphotos, sizeof(photos[0]), newest_first);
	}
	if (sel >= nphotos)
		sel = nphotos ? nphotos - 1 : 0;
}

/* ------------------------------------------------------------ decoding --- */

struct jerr {
	struct jpeg_error_mgr mgr;
	jmp_buf jump;
};

static void jerr_exit(j_common_ptr ci)
{
	longjmp(((struct jerr *)ci->err)->jump, 1);
}

/*
 * Decode at 1/denom (1, 2, 4 or 8) into XRGB; returns NULL on failure. The
 * full size goes to *fw, *fh if they are not NULL.
 */
static uint32_t *decode(const char *path, int denom, int *w, int *h, int *fw,
			int *fh)
{
	struct jpeg_decompress_struct ci;
	struct jerr jerr;
	uint32_t *volatile out = NULL;
	uint8_t *volatile row = NULL;
	FILE *f = fopen(path, "rb");

	if (f == NULL)
		return NULL;
	ci.err = jpeg_std_error(&jerr.mgr);
	jerr.mgr.error_exit = jerr_exit;
	if (setjmp(jerr.jump)) {
		jpeg_destroy_decompress(&ci);
		fclose(f);
		free(out);
		free(row);
		return NULL;
	}
	jpeg_create_decompress(&ci);
	jpeg_stdio_src(&ci, f);
	jpeg_read_header(&ci, TRUE);
	if (fw)
		*fw = ci.image_width;
	if (fh)
		*fh = ci.image_height;
	ci.scale_num = 1;
	ci.scale_denom = denom;
	ci.out_color_space = JCS_RGB;
	/* Thumbnails decode fast; a photo shown whole decodes accurately */
	ci.dct_method = denom > 1 ? JDCT_IFAST : JDCT_ISLOW;
	jpeg_start_decompress(&ci);
	*w = ci.output_width;
	*h = ci.output_height;
	out = malloc((size_t)*w * *h * 4);
	row = malloc((size_t)*w * 3);
	while (ci.output_scanline < ci.output_height) {
		uint8_t *rp = row;
		uint32_t *o = out + (size_t)ci.output_scanline * *w;
		jpeg_read_scanlines(&ci, &rp, 1);
		for (int x = 0; x < *w; x++)
			o[x] = 0xff000000 | row[x * 3] << 16 | row[x * 3 + 1] << 8 |
				row[x * 3 + 2];
	}
	jpeg_finish_decompress(&ci);
	jpeg_destroy_decompress(&ci);
	fclose(f);
	free(row);
	return out;
}

/* ---------------------------------------------------------- thumbnails --- */

static void thumb_cache_path(const struct photo *p, char *buf, size_t len)
{
	const char *home = getenv("HOME");
	const char *base = strrchr(p->path, '/');
	char name[300];
	unsigned h = 5381;

	/* The whole path hashed: two folders may hold the same name */
	for (const char *c = p->path; *c; c++)
		h = h * 33 + (unsigned char)*c;
	snprintf(name, sizeof(name), "%s", base ? base + 1 : p->path);
	snprintf(buf, len, "%s/.cache/s22/thumbs/%08x-%lld-%s.xrgb",
		 home ? home : "/tmp", h, (long long)p->mtime, name);
}

static void mkdirs(const char *path)
{
	char dir[1024];

	snprintf(dir, sizeof(dir), "%s", path);
	for (char *c = dir + 1; (c = strchr(c, '/')) != NULL; c++) {
		*c = 0;
		mkdir(dir, 0755);
		*c = '/';
	}
}

/* Run a program and wait for it; true if it exited 0 */
static bool run(char *const argv[])
{
	pid_t pid = fork();
	int status;

	if (pid == 0) {
		int null = open("/dev/null", O_RDWR | O_CLOEXEC);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	if (pid < 0)
		return false;
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return false;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* A wallpaper's thumbnail, by ffmpeg (any picture; the middle square of
 * its first frame, transparency on black), into the cache file */
static bool thumb_ffmpeg(const char *path, const char *cache)
{
	char vf[256], tmp[1210];

	snprintf(vf, sizeof(vf), "color=c=black:s=%dx%d[b];[0:v]scale=%d:%d:"
		 "force_original_aspect_ratio=increase,crop=%d:%d,format=rgba[f];"
		 "[b][f]overlay=shortest=1,format=bgra", THUMB, THUMB, THUMB, THUMB,
		 THUMB, THUMB);
	snprintf(tmp, sizeof(tmp), "%s.tmp", cache);
	char *argv[] = { "ffmpeg", "-v", "error", "-nostdin", "-y", "-i", (char *)path,
		"-filter_complex", vf, "-frames:v", "1", "-f", "rawvideo",
		"-pix_fmt", "bgra", tmp, NULL };
	mkdirs(cache);
	if (!run(argv) || rename(tmp, cache) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

static bool thumb_make(struct photo *p)
{
	char cache[1200];
	int w, h;
	uint32_t *t = malloc(THUMB * THUMB * 4);

	if (t == NULL)
		return false;
	thumb_cache_path(p, cache, sizeof(cache));
	int fd = open(cache, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t n = read(fd, t, THUMB * THUMB * 4);
		close(fd);
		if (n == THUMB * THUMB * 4) {
			p->thumb = t;
			return true;
		}
	}
	if (tab == T_WALLS) {
		if (thumb_ffmpeg(p->path, cache) &&
		    (fd = open(cache, O_RDONLY | O_CLOEXEC)) >= 0) {
			ssize_t n = read(fd, t, THUMB * THUMB * 4);
			close(fd);
			if (n == THUMB * THUMB * 4) {
				for (int i = 0; i < THUMB * THUMB; i++)
					t[i] |= 0xff000000;
				p->thumb = t;
				return true;
			}
		}
		free(t);
		p->failed = true;
		return false;
	}
	uint32_t *src = decode(p->path, 8, &w, &h, NULL, NULL);
	if (src == NULL) {
		free(t);
		p->failed = true;
		return false;
	}
	/* The middle square, scaled */
	int side = w < h ? w : h, x0 = (w - side) / 2, y0 = (h - side) / 2;
	for (int y = 0; y < THUMB; y++)
		for (int x = 0; x < THUMB; x++)
			t[y * THUMB + x] = src[(size_t)(y0 + y * side / THUMB) * w +
					      x0 + x * side / THUMB];
	free(src);
	mkdirs(cache);
	fd = open(cache, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		if (write(fd, t, THUMB * THUMB * 4) < 0)
			;
		close(fd);
	}
	p->thumb = t;
	return true;
}

/* Keep the thumbnails near the screen; drop the rest */
static void thumbs_trim(void)
{
	int first = (top_row - 6) * COLS, last = (top_row + 10) * COLS;

	for (int i = 0; i < nphotos; i++)
		if ((i < first || i > last) && photos[i].thumb) {
			free(photos[i].thumb);
			photos[i].thumb = NULL;
		}
}

/* One missing thumbnail on screen (or just below); true if one was made */
static bool thumb_next(void)
{
	int rows = (640 - UI_HEADER_H - UI_HINTS_H) / (THUMB + GAP) + 2;

	for (int i = top_row * COLS; i < nphotos && i < (top_row + rows) * COLS; i++)
		if (!photos[i].thumb && !photos[i].failed)
			return thumb_make(&photos[i]);
	return false;
}

/* ------------------------------------------------------------- a photo --- */

/* The picture's full size, from its header */
static bool jpeg_size(const char *path, int *w, int *h)
{
	struct jpeg_decompress_struct ci;
	struct jerr jerr;
	FILE *f = fopen(path, "rbe");

	if (f == NULL)
		return false;
	ci.err = jpeg_std_error(&jerr.mgr);
	jerr.mgr.error_exit = jerr_exit;
	if (setjmp(jerr.jump)) {
		jpeg_destroy_decompress(&ci);
		fclose(f);
		return false;
	}
	jpeg_create_decompress(&ci);
	jpeg_stdio_src(&ci, f);
	jpeg_read_header(&ci, TRUE);
	*w = ci.image_width;
	*h = ci.image_height;
	jpeg_destroy_decompress(&ci);
	fclose(f);
	return *w > 0 && *h > 0;
}

/*
 * Decode at the smallest of 1/8, 1/4, 1/2 or full size that still has as
 * many pixels as the screen shows at this zoom: a camera photo (1944x2592)
 * at 1/4, a screenshot (480x640) whole. (Always 1/4 made a screenshot a
 * 120x160 picture blown up four times.)
 */
static void photo_load(void)
{
	int want = 1, w, h;

	if (jpeg_size(photos[sel].path, &w, &h)) {
		double fit = (double)SCREEN_W / w < (double)SCREEN_H / h ?
			(double)SCREEN_W / w : (double)SCREEN_H / h;
		for (int d = 8; d > 1; d /= 2)
			if (d * fit * zoom <= 1.0) {
				want = d;
				break;
			}
	}
	free(img);
	img = decode(photos[sel].path, want, &img_w, &img_h, &full_w, &full_h);
	img_scale = want;
	if (img == NULL)
		snprintf(message, sizeof(message), "Cannot open this photo");
}

static void photo_open(int i)
{
	sel = i;
	view = V_PHOTO;
	zoom = 1;
	pan_x = pan_y = 0.5;
	info = confirm_delete = false;
	message[0] = 0;
	photo_load();
}

/* ---------------------------------------------------------- wallpapers --- */

/* One program at a time in the background (a preview, or setting one): sh
 * -c CMD with its output on a pipe; done(ok) when the pipe closes, ok if it
 * printed "ok" (the commands end in && echo ok) */
static int job_fd = -1;
static pid_t job_pid;
static char job_out[64];
static void (*job_done)(struct ui *ui, bool ok);

static void on_job(struct ui *ui, int fd)
{
	size_t n = strlen(job_out);
	ssize_t r;

	while ((r = read(fd, job_out + n, sizeof(job_out) - 1 - n)) > 0) {
		n += r;
		if (n >= sizeof(job_out) - 1)
			n = 0;		/* only the end matters */
	}
	job_out[n] = 0;
	if (r < 0 && errno == EAGAIN)
		return;		/* more to come */
	ui_unwatch(ui, fd);
	close(fd);
	job_fd = -1;
	waitpid(job_pid, NULL, 0);
	if (job_done)
		job_done(ui, strstr(job_out, "ok") != NULL);
}

static bool job_start(struct ui *ui, const char *cmd, void (*done)(struct ui *, bool))
{
	int p[2];

	if (job_fd >= 0 || pipe2(p, O_CLOEXEC) != 0)
		return false;
	job_pid = fork();
	if (job_pid == 0) {
		dup2(p[1], 1);
		execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	if (job_pid < 0) {
		close(p[0]);
		return false;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	job_fd = p[0];
	job_out[0] = 0;
	job_done = done;
	ui_watch(ui, job_fd, on_job);
	return true;
}

/* path in single quotes, for sh */
static void quote(const char *path, char *out, size_t len)
{
	size_t n = 0;

	out[n++] = '\'';
	for (; *path && n < len - 6; path++) {
		if (*path == '\'') {
			memcpy(out + n, "'\\''", 4);
			n += 4;
		} else {
			out[n++] = *path;
		}
	}
	out[n++] = '\'';
	out[n] = 0;
}

/* The open wallpaper's preview: s22-wallpaper's conversion, kept */
static char prev_path[1200];
static bool prev_ready, prev_failed;
static uint32_t *anim;			/* the outer one's frames, OUTER x OUTER */
static int nanim;
static long long anim_t0;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void preview_path(const struct photo *p, char *buf, size_t len)
{
	const char *home = getenv("HOME");
	unsigned h = 5381;

	for (const char *c = p->path; *c; c++)
		h = h * 33 + (unsigned char)*c;
	snprintf(buf, len, "%s/.cache/s22/wallpreview/%08x-%lld-%s.ppm",
		 home ? home : "/tmp", h, (long long)p->mtime, p->outer ? "outer" : "inner");
}

static int ppm_token(FILE *f)
{
	int ch, v = 0;

	while ((ch = getc(f)) != EOF && (ch == ' ' || ch == '\t' || ch == '\n' ||
					 ch == '\r' || ch == '#'))
		if (ch == '#')
			while ((ch = getc(f)) != EOF && ch != '\n')
				;
	if (ch < '0' || ch > '9')
		return -1;
	for (; ch >= '0' && ch <= '9'; ch = getc(f))
		v = v * 10 + (ch - '0');
	return v;
}

/* The outer screen's frames, from the preview (each OUTER x OUTER) */
static void anim_load(void)
{
	FILE *f = fopen(prev_path, "rbe");
	unsigned char rgb[OUTER * OUTER * 3];

	free(anim);
	anim = NULL;
	nanim = 0;
	while (f && nanim < 400) {
		int w, h, max;
		if (getc(f) != 'P' || getc(f) != '6' || (w = ppm_token(f)) != OUTER ||
		    (h = ppm_token(f)) != OUTER || (max = ppm_token(f)) != 255 ||
		    fread(rgb, 3, OUTER * OUTER, f) != OUTER * OUTER)
			break;
		uint32_t *a = realloc(anim, (size_t)(nanim + 1) * OUTER * OUTER * 4);
		if (!a)
			break;
		anim = a;
		uint32_t *o = anim + (size_t)nanim * OUTER * OUTER;
		for (int i = 0; i < OUTER * OUTER; i++)
			o[i] = 0xff000000u | rgb[3 * i] << 16 | rgb[3 * i + 1] << 8 | rgb[3 * i + 2];
		nanim++;
	}
	if (f)
		fclose(f);
	anim_t0 = now_ms();
}

static void wall_prepare(struct ui *ui);

static void preview_done(struct ui *ui, bool ok)
{
	if (view != V_WALL)
		return;
	char want[1200];
	preview_path(&photos[sel], want, sizeof(want));
	if (strcmp(want, prev_path)) {
		wall_prepare(ui);	/* another was opened meanwhile */
		return;
	}
	prev_ready = ok && access(prev_path, R_OK) == 0;
	prev_failed = !prev_ready;
	if (prev_ready && photos[sel].outer)
		anim_load();
	ui_timer(ui, 1);
	ui_redraw(ui);
}

/* The open one's preview: kept, or made now in the background */
static void wall_prepare(struct ui *ui)
{
	char cmd[2600], q[1100];

	preview_path(&photos[sel], prev_path, sizeof(prev_path));
	prev_ready = prev_failed = false;
	free(anim);
	anim = NULL;
	nanim = 0;
	if (access(prev_path, R_OK) == 0) {
		prev_ready = true;
		if (photos[sel].outer)
			anim_load();
		return;
	}
	if (job_fd >= 0)
		return;		/* after the one running (preview_done) */
	mkdirs(prev_path);
	quote(photos[sel].path, q, sizeof(q));
	snprintf(cmd, sizeof(cmd), "s22-wallpaper -o '%s' %s %s >/dev/null 2>&1 && echo ok",
		 prev_path, photos[sel].outer ? "outer" : "home", q);
	job_start(ui, cmd, preview_done);
}

static void wall_open(struct ui *ui, int i)
{
	sel = i;
	view = V_WALL;
	info = confirm_delete = menu_open = false;
	message[0] = 0;
	wall_prepare(ui);
	ui_timer(ui, 1);
}

/* Setting one: s22-wallpaper for each screen (it records the choice) */
static void set_done(struct ui *ui, bool ok)
{
	snprintf(message, sizeof(message), ok ? "Wallpaper set" :
		 "Could not set the wallpaper");
	if (ok && choosing[0]) {
		ui_quit(ui);	/* back to Settings */
		return;
	}
	ui_redraw(ui);
}

static void wall_set(struct ui *ui, const char *kinds, const char *path)
{
	char cmd[2600], q[1100];
	int n = 0;

	if (path)
		quote(path, q, sizeof(q));
	for (const char *k = kinds; *k; ) {
		size_t l = strcspn(k, " ");
		n += snprintf(cmd + n, sizeof(cmd) - n, "s22-wallpaper %.*s %s >/dev/null 2>&1 && ",
			      (int)l, k, path ? q : "none");
		k += l;
		while (*k == ' ')
			k++;
	}
	snprintf(cmd + n, sizeof(cmd) - n, "echo ok");
	if (job_start(ui, cmd, set_done))
		snprintf(message, sizeof(message), "Setting the wallpaper…");
	else
		snprintf(message, sizeof(message), "Busy: try again in a moment");
}

/* A photo to the wallpapers, by its own name (another if that is taken) */
static void move_to_walls(struct ui *ui, bool outer)
{
	char dir[512], to[1200];
	const char *name = strrchr(photos[sel].path, '/');

	name = name ? name + 1 : photos[sel].path;
	wall_dir(outer, dir, sizeof(dir));
	snprintf(to, sizeof(to), "%s/", dir);
	mkdirs(to);
	snprintf(to, sizeof(to), "%s/%s", dir, name);
	for (int i = 2; access(to, F_OK) == 0 && i < 100; i++) {
		const char *dot = strrchr(name, '.');
		int stem = dot ? (int)(dot - name) : (int)strlen(name);
		snprintf(to, sizeof(to), "%s/%.*s-%d%s", dir, stem, name, i, dot ? dot : "");
	}
	if (access(to, F_OK) == 0 || rename(photos[sel].path, to) != 0) {
		snprintf(message, sizeof(message), "Could not move it: %s", strerror(errno));
		return;
	}
	free(photos[sel].path);
	free(photos[sel].thumb);
	memmove(&photos[sel], &photos[sel + 1], (nphotos - sel - 1) * sizeof(photos[0]));
	nphotos--;
	view = V_GRID;
	free(img);
	img = NULL;
	if (sel >= nphotos)
		sel = nphotos ? nphotos - 1 : 0;
	snprintf(message, sizeof(message), "Moved to Wallpapers (%s)", outer ? "outer" : "inner");
	ui_timer(ui, 1);
}

static void menu_show(void)
{
	nmenu = 0;
	if (view == V_PHOTO) {
		menu[nmenu++] = "To wallpapers: home and lock";
		menu[nmenu++] = "To wallpapers: outer screen";
	} else if (photos[sel].outer) {
		menu[nmenu++] = "Set for the outer screen";
	} else {
		menu[nmenu++] = "Set for the home screen";
		menu[nmenu++] = "Set for the lock screen";
		menu[nmenu++] = "Set for both";
	}
	menu_sel = 0;
	menu_open = true;
}

static void menu_choose(struct ui *ui)
{
	menu_open = false;
	if (view == V_PHOTO) {
		move_to_walls(ui, menu_sel == 1);
		return;
	}
	if (photos[sel].outer)
		wall_set(ui, "outer", photos[sel].path);
	else
		wall_set(ui, menu_sel == 0 ? "home" : menu_sel == 1 ? "lock" : "home lock",
			 photos[sel].path);
}

static void draw_menu(struct ui_canvas *c)
{
	int rh = 44, h = nmenu * rh + 16, y = c->h - UI_HINTS_H - h - 10;

	ui_round(c, 16, y, c->w - 32, h, 12, UI_BAR);
	for (int i = 0; i < nmenu; i++) {
		int ry = y + 8 + i * rh;
		if (i == menu_sel)
			ui_round(c, 22, ry, c->w - 44, rh, 8, UI_SEL);
		ui_text_box(c, 34, ry + 11, c->w - 68, i == menu_sel ? UI_BOLD : UI_SANS, 18,
			    UI_FG, UI_LEFT, menu[i]);
	}
}

/* The wallpaper as it will look */
static void draw_wall(struct ui *ui, struct ui_canvas *c)
{
	const struct photo *p = &photos[sel];
	char top[300], speed_s[16] = "1";
	const char *name = strrchr(p->path, '/');

	ui_fill(c, 0, 0, c->w, c->h, 0xff000000);
	if (prev_ready && !p->outer) {
		ui_wallpaper(c, prev_path);	/* as the home screen shows it */
	} else if (prev_ready && nanim) {
		double speed = atof(ui_setting("wallpaper_speed", "1", speed_s, sizeof(speed_s)));
		if (speed < 0.1 || speed > 4)
			speed = 1;
		int i = (int)((long long)((now_ms() - anim_t0) * speed) / ANIM_MS % nanim);
		const uint32_t *fr = anim + (size_t)i * OUTER * OUTER;
		int sz = OUTER * OUTER_ZOOM, x0 = (c->w - sz) / 2, y0 = (c->h - sz) / 2;
		for (int y = 0; y < sz; y++) {
			uint32_t *o = c->px + (size_t)(y0 + y) * c->stride + x0;
			const uint32_t *r = fr + (y / OUTER_ZOOM) * OUTER;
			for (int x = 0; x < sz; x++)
				o[x] = r[x / OUTER_ZOOM];
		}
	} else {
		ui_text_box(c, 0, c->h / 2 - 12, c->w, UI_SANS, 19, UI_DIM, UI_CENTER,
			    prev_failed ? "Cannot show this picture" : "Preparing the preview…");
	}
	snprintf(top, sizeof(top), "%s · %s", name ? name + 1 : p->path,
		 p->outer ? "outer screen" : "home and lock");
	ui_round_alpha(c, 8, 6, c->w - 16, 30, 8, UI_BAR, 200);
	ui_text_box(c, 16, 11, c->w - 32, UI_SANS, 15, UI_FG, UI_CENTER, top);
	if (confirm_delete) {
		ui_round(c, 30, c->h / 2 - 50, c->w - 60, 100, 12, UI_BAR);
		ui_text_box(c, 30, c->h / 2 - 36, c->w - 60, UI_BOLD, 20, UI_FG,
			    UI_CENTER, "Delete this wallpaper?");
		ui_text_box(c, 30, c->h / 2 + 4, c->w - 60, UI_SANS, 17, UI_DIM,
			    UI_CENTER, "OK deletes, Back keeps it");
	}
	if (menu_open)
		draw_menu(c);
	if (message[0]) {
		ui_round(c, 16, 44, c->w - 32, 38, 10, UI_BAR);
		ui_text_box(c, 26, 52, c->w - 52, UI_SANS, 17, UI_FG, UI_CENTER, message);
	}
	ui_hints(c, menu_open || confirm_delete ? "Back" : "Grid",
		 confirm_delete ? "Delete" : menu_open ? "Choose" : prev_ready ? "Set" : NULL,
		 menu_open || confirm_delete ? NULL : "C delete");
}

/* Bilinear: a b over c d, weights out of 256 */
static inline uint32_t mix4(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
			    unsigned wx, unsigned wy)
{
	uint32_t out = 0xff000000;

	for (int sh = 0; sh <= 16; sh += 8) {
		unsigned top = ((a >> sh) & 255) * (256 - wx) + ((b >> sh) & 255) * wx;
		unsigned bot = ((c >> sh) & 255) * (256 - wx) + ((d >> sh) & 255) * wx;
		out |= ((top * (256 - wy) + bot * wy) >> 16) << sh;
	}
	return out;
}

static void draw_photo(struct ui *ui, struct ui_canvas *c)
{
	ui_fill(c, 0, 0, c->w, c->h, 0xff000000);
	if (img) {
		/* Fit the screen at zoom 1; each zoom doubles it */
		double fit = (double)c->w / img_w < (double)c->h / img_h ?
			(double)c->w / img_w : (double)c->h / img_h;
		double s = fit * zoom;	/* fit is for the size decoded */
		double sw = img_w * s, sh = img_h * s;	/* the image on screen */
		double ox = sw > c->w ? -(pan_x * sw - c->w / 2.0) : (c->w - sw) / 2;
		double oy = sh > c->h ? -(pan_y * sh - c->h / 2.0) : (c->h - sh) / 2;
		if (sw > c->w) {
			if (ox > 0) ox = 0;
			if (ox < c->w - sw) ox = c->w - sw;
		}
		if (sh > c->h) {
			if (oy > 0) oy = 0;
			if (oy < c->h - sh) oy = c->h - sh;
		}
		/* Smaller than the image: each screen pixel blends the 2x2
		 * image pixels around its centre (nearest dropped whole rows and
		 * columns, which made text jagged); larger: nearest, so zoomed
		 * pixels stay sharp */
		bool blend = s < 1.0;
		for (int y = 0; y < c->h; y++) {
			double fy = (y + 0.5 - oy) / s - (blend ? 0.5 : 0);
			int iy = (int)(blend ? floor(fy) : (y - oy) / s);
			if (iy < -1 || iy >= img_h || (!blend && iy < 0))
				continue;
			uint32_t *o = c->px + (size_t)y * c->stride;
			int y0 = iy < 0 ? 0 : iy, y1 = iy + 1 >= img_h ? img_h - 1 : iy + 1;
			unsigned wy = blend ? (unsigned)((fy - iy) * 256) : 0;
			const uint32_t *r0 = img + (size_t)y0 * img_w;
			const uint32_t *r1 = img + (size_t)y1 * img_w;
			for (int x = 0; x < c->w; x++) {
				if (!blend) {
					int ix = (int)((x - ox) / s);
					if (ix >= 0 && ix < img_w)
						o[x] = r0[ix];
					continue;
				}
				double fx = (x + 0.5 - ox) / s - 0.5;
				int ix = (int)floor(fx);
				if (ix < -1 || ix >= img_w)
					continue;
				int x0 = ix < 0 ? 0 : ix, x1 = ix + 1 >= img_w ? img_w - 1 : ix + 1;
				unsigned wx = (unsigned)((fx - ix) * 256);
				o[x] = mix4(r0[x0], r0[x1], r1[x0], r1[x1], wx, wy);
			}
		}
	}
	char top[64];
	snprintf(top, sizeof(top), "%d of %d%s", sel + 1, nphotos,
		 zoom > 1 ? (zoom == 2 ? "  2x" : "  4x") : "");
	ui_text(c, 10, 8, UI_SANS, 15, 0xffffffff, top);
	if (info) {
		const struct photo *p = &photos[sel];
		char buf[512], when[64];
		struct tm tm;
		localtime_r(&p->mtime, &tm);
		strftime(when, sizeof(when), "%d %b %Y, %H:%M", &tm);
		const char *name = strrchr(p->path, '/');
		snprintf(buf, sizeof(buf), "%s\n%s\n%dx%d, %.1f MB", name ? name + 1 :
			 p->path, when, full_w, full_h, p->size / 1e6);
		ui_round(c, 10, c->h - UI_HINTS_H - 96, c->w - 20, 86, 10, UI_BAR);
		ui_text_wrap(c, 22, c->h - UI_HINTS_H - 88, c->w - 44, UI_SANS, 17,
			     UI_FG, buf);
	}
	if (confirm_delete) {
		ui_round(c, 30, c->h / 2 - 50, c->w - 60, 100, 12, UI_BAR);
		ui_text_box(c, 30, c->h / 2 - 36, c->w - 60, UI_BOLD, 20, UI_FG,
			    UI_CENTER, "Delete this photo?");
		ui_text_box(c, 30, c->h / 2 + 4, c->w - 60, UI_SANS, 17, UI_DIM,
			    UI_CENTER, "OK deletes, Back keeps it");
	}
	if (menu_open)
		draw_menu(c);
	if (message[0]) {
		ui_round(c, 16, 40, c->w - 32, 38, 10, UI_BAR);
		ui_text_box(c, 26, 48, c->w - 52, UI_SANS, 17, UI_FG, UI_CENTER,
			    message);
	}
	ui_hints(c, menu_open || confirm_delete ? "Back" : "Grid",
		 confirm_delete ? "Delete" : menu_open ? "Move" : "Wallpaper",
		 menu_open || confirm_delete ? NULL : "# info  C delete");
}

/* --------------------------------------------------------------- grid --- */

static void grid_scroll(void)
{
	if (sel < 0)
		sel = 0;
	int rows = (640 - UI_HEADER_H - UI_HINTS_H) / (THUMB + GAP);
	int r = sel / COLS;

	if (r < top_row)
		top_row = r;
	if (r >= top_row + rows)
		top_row = r - rows + 1;
	thumbs_trim();
}

/* The two tabs, the one shown underlined */
static void draw_tabs(struct ui_canvas *c)
{
	static const char *const names[2] = { "Photos", "Wallpapers" };
	int w = c->w / 2;

	ui_fill(c, 0, 0, c->w, UI_HEADER_H, UI_BAR);
	for (int i = 0; i < 2; i++) {
		if (i == (int)tab)
			ui_fill(c, i * w + 6, UI_HEADER_H - 3, w - 12, 3, UI_ACCENT);
		ui_text_box(c, i * w, 7, w, i == (int)tab ? UI_BOLD : UI_SANS, 17,
			    i == (int)tab ? UI_FG : UI_DIM, UI_CENTER, names[i]);
	}
}

static void draw_message(struct ui_canvas *c)
{
	if (!message[0])
		return;
	ui_round(c, 16, c->h - UI_HINTS_H - 50, c->w - 32, 38, 10, UI_BAR);
	ui_text_box(c, 26, c->h - UI_HINTS_H - 42, c->w - 52, UI_SANS, 17, UI_FG,
		    UI_CENTER, message);
}

static void draw_grid(struct ui *ui, struct ui_canvas *c)
{
	char title[48];
	int y0 = UI_HEADER_H + GAP;

	if (choosing[0]) {
		snprintf(title, sizeof(title), "Wallpaper: %s screen", choosing);
		ui_header(ui, c, title);
	} else {
		draw_tabs(c);
	}
	if (nphotos == 0) {
		char dir[512];
		wall_dir(!strcmp(choosing, "outer"), dir, sizeof(dir));
		ui_text_box(c, 0, c->h / 2 - 12, c->w, UI_SANS, 20, UI_DIM,
			    UI_CENTER, tab == T_PHOTOS ? "No photos yet" : "No wallpapers yet");
		if (tab == T_WALLS)
			ui_text_wrap(c, 30, c->h / 2 + 20, c->w - 60, UI_SANS, 16, UI_DIM,
				     "Put pictures in Wallpapers/inner (home and lock screens) "
				     "or Wallpapers/outer on the memory card, or move a photo "
				     "here from Photos (OK on it).");
		draw_message(c);
		ui_hints(c, "Close", NULL, choosing[0] ? "* none" : "* tab");
		return;
	}
	for (int i = top_row * COLS; i < nphotos; i++) {
		int r = i / COLS - top_row, col = i % COLS;
		int x = GAP + col * (THUMB + GAP), y = y0 + r * (THUMB + GAP);
		if (y >= c->h - UI_HINTS_H)
			break;
		if (i == sel)
			ui_fill(c, x - 3, y - 3, THUMB + 6, THUMB + 6, UI_ACCENT);
		if (photos[i].thumb) {
			for (int ty = 0; ty < THUMB && y + ty < c->h - UI_HINTS_H; ty++)
				memcpy(c->px + (size_t)(y + ty) * c->stride + x,
				       photos[i].thumb + ty * THUMB, THUMB * 4);
		} else {
			ui_fill(c, x, y, THUMB, THUMB, UI_BAR);
			if (photos[i].failed)
				ui_text_box(c, x, y + THUMB / 2 - 10, THUMB, UI_SANS, 15,
					    UI_DIM, UI_CENTER, "?");
		}
		if (tab == T_WALLS && photos[i].outer && !choosing[0] &&
		    y + THUMB - 24 < c->h - UI_HINTS_H) {
			ui_round_alpha(c, x + 4, y + THUMB - 26, 58, 22, 6, UI_BAR, 210);
			ui_text_box(c, x + 4, y + THUMB - 24, 58, UI_SANS, 14, UI_FG,
				    UI_CENTER, "outer");
		}
	}
	draw_message(c);
	ui_hints(c, "Close", "Open", choosing[0] ? "* none" : "* tab");
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	if (view == V_PHOTO)
		draw_photo(ui, c);
	else if (view == V_WALL)
		draw_wall(ui, c);
	else
		draw_grid(ui, c);
}

/* --------------------------------------------------------------- keys --- */

static void pan(double dx, double dy)
{
	pan_x += dx / zoom;
	pan_y += dy / zoom;
	pan_x = pan_x < 0 ? 0 : pan_x > 1 ? 1 : pan_x;
	pan_y = pan_y < 0 ? 0 : pan_y > 1 ? 1 : pan_y;
}

static void delete_photo(struct ui *ui)
{
	char cache[1200];

	thumb_cache_path(&photos[sel], cache, sizeof(cache));
	if (unlink(photos[sel].path) != 0) {
		snprintf(message, sizeof(message), "Could not delete: %s",
			 strerror(errno));
		return;
	}
	unlink(cache);
	if (tab == T_WALLS) {
		preview_path(&photos[sel], cache, sizeof(cache));
		unlink(cache);
	}
	free(photos[sel].path);
	free(photos[sel].thumb);
	memmove(&photos[sel], &photos[sel + 1],
		(nphotos - sel - 1) * sizeof(photos[0]));
	nphotos--;
	if (nphotos == 0) {
		view = V_GRID;
		return;
	}
	if (sel >= nphotos)
		sel = nphotos - 1;
	if (tab == T_WALLS)
		wall_open(ui, sel);
	else
		photo_open(sel);
	snprintf(message, sizeof(message), "Deleted");
}

/* The list over a photo or wallpaper: Up/Down, OK, Back */
static bool key_menu(struct ui *ui, uint32_t code, enum ui_key_state st)
{
	if (!menu_open)
		return false;
	if (st != UI_PRESSED && code != KEY_UP && code != KEY_DOWN)
		return true;
	switch (code) {
	case KEY_UP:
		if (menu_sel > 0)
			menu_sel--;
		break;
	case KEY_DOWN:
		if (menu_sel < nmenu - 1)
			menu_sel++;
		break;
	case KEY_ENTER:
	case KEY_OK:
		menu_choose(ui);
		break;
	case KEY_BACK:
	case KEY_ESC:
		menu_open = false;
		break;
	}
	ui_redraw(ui);
	return true;
}

static void grid_scroll(void);

static void key_wall(struct ui *ui, uint32_t code, enum ui_key_state st)
{
	if (key_menu(ui, code, st))
		return;
	if (st != UI_PRESSED)
		return;
	message[0] = 0;
	if (confirm_delete) {
		confirm_delete = false;
		if (code == KEY_ENTER || code == KEY_OK)
			delete_photo(ui);
		if (nphotos == 0)
			view = V_GRID;
		ui_redraw(ui);
		return;
	}
	switch (code) {
	case KEY_LEFT:
	case KEY_RIGHT: {
		int n = sel + (code == KEY_RIGHT ? 1 : -1);
		if (n >= 0 && n < nphotos)
			wall_open(ui, n);
		break;
	}
	case KEY_ENTER:
	case KEY_OK:
		if (!prev_ready)
			break;
		if (choosing[0])
			wall_set(ui, choosing, photos[sel].path);
		else
			menu_show();
		break;
	case KEY_BACKSPACE:
		confirm_delete = true;
		break;
	case KEY_BACK:
	case KEY_ESC:
		view = V_GRID;
		free(anim);
		anim = NULL;
		nanim = 0;
		grid_scroll();
		ui_timer(ui, 1);
		break;
	default:
		return;
	}
	ui_redraw(ui);
}

static void key_photo(struct ui *ui, uint32_t code, enum ui_key_state st)
{
	if (key_menu(ui, code, st))
		return;
	message[0] = 0;
	if (confirm_delete) {
		if (st != UI_PRESSED)
			return;
		confirm_delete = false;
		if (code == KEY_ENTER || code == KEY_OK)
			delete_photo(ui);
		return;
	}
	switch (code) {
	case KEY_ENTER:
	case KEY_OK:
		if (st == UI_PRESSED)
			menu_show();
		break;
	case KEY_LEFT:
	case KEY_RIGHT:
		if (zoom > 1) {
			pan(code == KEY_LEFT ? -0.1 : 0.1, 0);
		} else {
			int n = sel + (code == KEY_RIGHT ? 1 : -1);
			if (n >= 0 && n < nphotos)
				photo_open(n);
		}
		break;
	case KEY_UP:
	case KEY_DOWN:
		if (st != UI_PRESSED)
			return;
		zoom = code == KEY_UP ? (zoom < 4 ? zoom * 2 : 4) :
			(zoom > 1 ? zoom / 2 : 1);
		if (zoom == 1)
			pan_x = pan_y = 0.5;
		photo_load();
		break;
	case KEY_2: pan(0, -0.1); break;
	case KEY_8: pan(0, 0.1); break;
	case KEY_4: pan(-0.1, 0); break;
	case KEY_6: pan(0.1, 0); break;
	case KEY_NUMERIC_POUND:
		if (st == UI_PRESSED)
			info = !info;
		break;
	case KEY_BACKSPACE:
		if (st == UI_PRESSED)
			confirm_delete = true;
		break;
	case KEY_BACK:
	case KEY_ESC:
		if (st != UI_PRESSED)
			return;
		view = V_GRID;
		free(img);
		img = NULL;
		grid_scroll();
		ui_timer(ui, 1);	/* thumbnails the reload dropped */
		break;
	default:
		return;
	}
	ui_redraw(ui);
}

/* The other tab, where it was left */
static void tab_switch(struct ui *ui)
{
	int n = nphotos, s = sel, t = top_row;

	tab = tab == T_PHOTOS ? T_WALLS : T_PHOTOS;
	photos = tab == T_WALLS ? wall_list : photo_list;
	nphotos = other.n;
	sel = other.sel;
	top_row = other.top_row;
	other.n = n;
	other.sel = s;
	other.top_row = t;
	load_list();
	grid_scroll();
	ui_timer(ui, 1);
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st,
		const char *text)
{
	if (st == UI_RELEASED)
		return;
	if (view == V_PHOTO) {
		key_photo(ui, code, st);
		return;
	}
	if (view == V_WALL) {
		key_wall(ui, code, st);
		return;
	}
	if (st == UI_PRESSED)
		message[0] = 0;
	switch (code) {
	case KEY_NUMERIC_STAR:
		if (st != UI_PRESSED)
			return;
		if (choosing[0])
			wall_set(ui, choosing, NULL);	/* none */
		else
			tab_switch(ui);
		ui_redraw(ui);
		return;
	case KEY_LEFT:
		if (sel > 0)
			sel--;
		break;
	case KEY_RIGHT:
		if (sel < nphotos - 1)
			sel++;
		break;
	case KEY_UP:
		if (sel >= COLS)
			sel -= COLS;
		break;
	case KEY_DOWN:
		if (sel + COLS < nphotos)
			sel += COLS;
		break;
	case KEY_ENTER:
	case KEY_OK:
		if (st == UI_PRESSED && nphotos) {
			if (tab == T_WALLS)
				wall_open(ui, sel);
			else
				photo_open(sel);
		}
		ui_redraw(ui);
		return;
	case KEY_BACK:
	case KEY_ESC:
		if (st == UI_PRESSED)
			ui_quit(ui);
		return;
	default:
		return;
	}
	grid_scroll();
	ui_timer(ui, 1);
	ui_redraw(ui);
}

/* ---------------------------------------------------------------- touch --- */

static int swipe_dx;

static void tap(struct ui *ui, int x, int y)
{
	if (menu_open) {
		/* A row of the list (draw_menu's), or outside it: closed */
		int rh = 44, h = nmenu * rh + 16, my = ui_height(ui) - UI_HINTS_H - h - 10;
		int i = (y - my - 8) / rh;
		if (y >= my + 8 && i >= 0 && i < nmenu) {
			menu_sel = i;
			menu_choose(ui);
		} else {
			menu_open = false;
		}
	} else if (view == V_PHOTO) {
		info = !info;
	} else if (view == V_WALL) {
		if (prev_ready && !confirm_delete) {
			if (choosing[0])
				wall_set(ui, choosing, photos[sel].path);
			else
				menu_show();
		}
	} else if (y < UI_HEADER_H) {
		if (!choosing[0] && (x < ui_width(ui) / 2) != (tab == T_PHOTOS))
			tab_switch(ui);
	} else {
		int col = (x - GAP / 2) / (THUMB + GAP);
		int row = (y - UI_HEADER_H - GAP / 2) / (THUMB + GAP);
		int i = (top_row + row) * COLS + col;
		if (y < UI_HEADER_H || col >= COLS || i >= nphotos)
			return;
		photo_open(i);
	}
	ui_redraw(ui);
}

static void drag(struct ui *ui, int dx, int dy, bool done)
{
	static int acc;

	if (view == V_WALL || menu_open)
		return;

	if (view == V_PHOTO) {
		if (zoom > 1) {
			pan(-dx / 480.0, -dy / 640.0);
		} else if (!done) {
			swipe_dx += dx;
		} else {
			int n = sel + (swipe_dx < -60 ? 1 : swipe_dx > 60 ? -1 : 0);
			swipe_dx = 0;
			if (n != sel && n >= 0 && n < nphotos)
				photo_open(n);
		}
		ui_redraw(ui);
		return;
	}
	/* The grid: a row per thumbnail's height */
	acc = done ? 0 : acc + dy;
	while (acc <= -(THUMB + GAP) && sel + COLS < nphotos) {
		sel += COLS;
		acc += THUMB + GAP;
	}
	while (acc >= THUMB + GAP && sel >= COLS) {
		sel -= COLS;
		acc -= THUMB + GAP;
	}
	grid_scroll();
	ui_timer(ui, 1);
	ui_redraw(ui);
}

/* ------------------------------------------------------------- the rest --- */

static void tick(struct ui *ui)
{
	/* Thumbnails, one at a time so keys stay quick */
	if (view == V_GRID && thumb_next()) {
		ui_redraw(ui);
		ui_timer(ui, 1);
	}
	/* The outer screen's wallpaper moving, while it is seen */
	if (view == V_WALL && nanim > 1 && ui_visible(ui)) {
		ui_redraw(ui);
		ui_timer(ui, ANIM_MS);
	}
}

/* The list again, keeping the photo selected (or open) where it was */
static void reload(struct ui *ui)
{
	char keep[1024] = "";

	if (nphotos)
		snprintf(keep, sizeof(keep), "%s", photos[sel].path);
	load_list();
	for (int i = 0; i < nphotos && keep[0]; i++)
		if (strcmp(photos[i].path, keep) == 0)
			sel = i;
	if (view != V_GRID && (nphotos == 0 || strcmp(photos[sel].path, keep)))
		view = V_GRID;	/* the open one went away */
	grid_scroll();
	ui_timer(ui, 1);
	ui_redraw(ui);
}

static void focus(struct ui *ui, bool focused)
{
	if (focused)
		reload(ui);
}

/* New or deleted photos, by the camera or anyone */
static int watch_fd = -1;

static void on_change(struct ui *ui, int fd)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

	while (read(fd, buf, sizeof(buf)) > 0)
		;
	/* Camera/ may have just been made: watch it too (again is harmless) */
	const char *home = getenv("HOME");
	char dir[512];
	snprintf(dir, sizeof(dir), "%s/Pictures/Camera", home ? home : "/tmp");
	inotify_add_watch(fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE |
			  IN_MOVED_FROM);
	reload(ui);
}

static void watch_pictures(struct ui *ui)
{
	const char *home = getenv("HOME");
	char dir[512];

	watch_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (watch_fd < 0)
		return;
	snprintf(dir, sizeof(dir), "%s/Pictures", home ? home : "/tmp");
	inotify_add_watch(watch_fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE |
			  IN_MOVED_FROM | IN_CREATE);
	strncat(dir, "/Camera", sizeof(dir) - strlen(dir) - 1);
	inotify_add_watch(watch_fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE |
			  IN_MOVED_FROM);
	for (int outer = 0; outer < 2; outer++) {
		wall_dir(outer, dir, sizeof(dir));
		strcat(dir, "/");
		mkdirs(dir);		/* where wallpapers go */
		wall_dir(outer, dir, sizeof(dir));
		inotify_add_watch(watch_fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO |
				  IN_DELETE | IN_MOVED_FROM);
	}
	ui_watch(ui, watch_fd, on_change);
}

static void open_uri(struct ui *ui, const char *uri)
{
	if (strncmp(uri, "wallpaper:", 10) == 0) {
		const char *k = uri + 10;
		if (strcmp(k, "home") && strcmp(k, "lock") && strcmp(k, "outer"))
			return;
		snprintf(choosing, sizeof(choosing), "%s", k);
		if (tab != T_WALLS)
			tab_switch(ui);
		view = V_GRID;
		menu_open = confirm_delete = false;
		message[0] = 0;
		sel = top_row = 0;
		load_list();
		/* Start on the one chosen now */
		char key[32], cur[1024];
		snprintf(key, sizeof(key), "wallpaper_%s", k);
		ui_setting(key, "none", cur, sizeof(cur));
		for (int i = 0; i < nphotos; i++)
			if (!strcmp(photos[i].path, cur))
				sel = i;
		grid_scroll();
		ui_timer(ui, 1);
		ui_redraw(ui);
		return;
	}
	if (strncmp(uri, "file:", 5) != 0)
		return;
	choosing[0] = 0;
	if (tab != T_PHOTOS)
		tab_switch(ui);
	load_list();
	for (int i = 0; i < nphotos; i++)
		if (strcmp(photos[i].path, uri + 5) == 0) {
			photo_open(i);
			break;
		}
	ui_timer(ui, 1);	/* thumbnails for when the grid comes back */
}

static void start(struct ui *ui)
{
	load_list();
	watch_pictures(ui);
	ui_timer(ui, 1);
}

int main(int argc, char **argv)
{
	struct ui_app app = {
		.app_id = "s22-photos",
		.title = "Photos",
		.draw = draw,
		.key = key,
		.tick = tick,
		.start = start,
		.focus = focus,
		.tap = tap,
		.drag = drag,
		.open = open_uri,
		.uri = argc > 1 ? argv[1] : NULL,
	};

	return ui_main(&app);
}
