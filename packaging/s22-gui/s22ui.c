// SPDX-License-Identifier: MIT
/*
 * s22ui: see s22ui.h. Wayland (wl_shm, xdg-shell, wl_keyboard with
 * xkbcommon), FreeType for text, and plain loops for everything drawn.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>
#include "s22ui.h"
#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"

/* The text entry's indicator, for the status bar (see ui_entry_indicator) */
static char entry_frame[192];		/* what this frame's entry showed */
static bool entry_published;		/* ours is in the file */
static void entry_publish(const char *text);

#define MAX_WATCH	8

struct buffer {
	struct wl_buffer *wl;
	uint32_t *px;
	size_t size;
	int w, h;
	bool busy;
};

struct watch {
	int fd;
	void (*cb)(struct ui *ui, int fd);
};

struct ui {
	struct ui_app *app;
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_touch *touch;
	int touch_id;			/* the finger followed, -1 none */
	double touch_x0, touch_y0, touch_x, touch_y;
	bool dragging;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct ext_session_lock_manager_v1 *lock_manager;
	struct ext_session_lock_v1 *session_lock;
	struct ext_session_lock_surface_v1 *lock_surface;
	struct wl_output *output;
	struct zwp_idle_inhibit_manager_v1 *inhibit_manager;
	struct zwp_idle_inhibitor_v1 *inhibitor;
	long long frame_asked, frame_got;	/* ms: the last frame asked, and done */
	struct zwlr_layer_surface_v1 *layer_surface;
	int layer_h;
	bool layer_bar;			/* ui_layer_bar(): reserves its height */
	bool layer_kb;

	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;
	int repeat_rate, repeat_delay;
	uint32_t repeat_key;
	int repeat_fd, timer_fd;

	int w, h, next_w, next_h;
	bool configured, dirty, frame_pending, quit;
	bool active;			/* the window in front (xdg activated) */
	struct buffer buf[2];
	struct watch watch[MAX_WATCH];
	int nwatch;
};

/* ------------------------------------------------------------- fonts --- */

static const char *font_path[] = {
	[UI_SANS] = "/usr/share/fonts/TTF/DejaVuSans.ttf",
	[UI_BOLD] = "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
	[UI_MONO] = "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
};
static FT_Library ft;
static FT_Face face[3];

struct glyph {
	struct glyph *next;
	uint32_t cp;
	uint8_t font;
	uint16_t size;
	int w, h, left, top, advance;
	uint8_t *bitmap;
};
static struct glyph *glyphs[1024];

static FT_Face font_face(enum ui_font font)
{
	if (ft == NULL && FT_Init_FreeType(&ft) != 0)
		return NULL;
	if (face[font] == NULL &&
	    FT_New_Face(ft, font_path[font], 0, &face[font]) != 0) {
		fprintf(stderr, "s22ui: cannot load %s\n", font_path[font]);
		face[font] = NULL;
	}
	return face[font];
}

static int ascent(enum ui_font font, int size)
{
	FT_Face f = font_face(font);

	if (f == NULL)
		return size;
	FT_Set_Pixel_Sizes(f, 0, size);
	return (f->size->metrics.ascender + 63) >> 6;
}

int ui_line_height(int size)
{
	return size * 6 / 5;
}

/*
 * A character DejaVu does not have (Japanese, Chinese, Korean, Arabic,
 * Thai, ...): the font fontconfig finds for it among those installed
 * (Noto's, D068), Japanese first for the characters Chinese and Japanese
 * share, bold for UI_BOLD where there is one; NULL if none has it. Each
 * file is opened once; fontconfig is asked once per character.
 */
static FT_Face fallback_face(uint32_t cp, bool bold)
{
	static struct { char file[256]; int index; FT_Face face; } faces[24];
	static int nfaces;
	static bool fc_ok, fc_tried;
	FcChar8 *file = NULL;
	int index = 0;
	FT_Face found = NULL;

	if (!fc_tried) {
		fc_tried = true;
		fc_ok = FcInit();
	}
	if (!fc_ok || ft == NULL)
		return NULL;
	FcPattern *p = FcPatternCreate();
	FcCharSet *cs = FcCharSetCreate();
	FcCharSetAddChar(cs, cp);
	FcPatternAddCharSet(p, FC_CHARSET, cs);
	/* DejaVu's look where it has it; the Japanese face of Noto CJK for
	 * Han, kana and Hangul (alone, lang=ja picked its Korean one) */
	FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)"DejaVu Sans");
	FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)"Noto Sans CJK JP");
	FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)"sans-serif");
	FcLangSet *ls = FcLangSetCreate();
	FcLangSetAdd(ls, (const FcChar8 *)"ja");
	FcPatternAddLangSet(p, FC_LANG, ls);
	FcPatternAddInteger(p, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
	FcConfigSubstitute(NULL, p, FcMatchPattern);
	FcDefaultSubstitute(p);
	FcResult r;
	FcPattern *m = FcFontMatch(NULL, p, &r);
	FcCharSet *has = NULL;
	if (m && FcPatternGetCharSet(m, FC_CHARSET, 0, &has) == FcResultMatch &&
	    FcCharSetHasChar(has, cp) &&
	    FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) {
		FcPatternGetInteger(m, FC_INDEX, 0, &index);
		for (int i = 0; i < nfaces && !found; i++)
			if (faces[i].index == index && !strcmp(faces[i].file, (const char *)file))
				found = faces[i].face;
		if (!found && nfaces < (int)(sizeof(faces) / sizeof(faces[0])) &&
		    FT_New_Face(ft, (const char *)file, index, &found) == 0) {
			snprintf(faces[nfaces].file, sizeof(faces[0].file), "%s", file);
			faces[nfaces].index = index;
			faces[nfaces++].face = found;
		}
	}
	if (m)
		FcPatternDestroy(m);
	FcPatternDestroy(p);
	FcCharSetDestroy(cs);
	FcLangSetDestroy(ls);
	return found;
}

static struct glyph *glyph(enum ui_font font, int size, uint32_t cp)
{
	unsigned slot = (cp * 31 + size * 7 + font) % 1024;
	struct glyph *g;

	for (g = glyphs[slot]; g; g = g->next)
		if (g->cp == cp && g->size == size && g->font == font)
			return g;

	FT_Face f = font_face(font);
	g = calloc(1, sizeof(*g));
	g->cp = cp;
	g->size = size;
	g->font = font;
	/* Symbols missing from the font come from DejaVu Sans if it has them,
	 * else from whichever installed font has them */
	if (f != NULL && font != UI_SANS && FT_Get_Char_Index(f, cp) == 0)
		f = font_face(UI_SANS);
	if (f != NULL && cp >= 0x80 && FT_Get_Char_Index(f, cp) == 0) {
		FT_Face fb = fallback_face(cp, font == UI_BOLD);
		if (fb != NULL)
			f = fb;
	}
	if (f != NULL) {
		FT_Set_Pixel_Sizes(f, 0, size);
		if (FT_Load_Char(f, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
			FT_GlyphSlot s = f->glyph;
			g->w = s->bitmap.width;
			g->h = s->bitmap.rows;
			g->left = s->bitmap_left;
			g->top = s->bitmap_top;
			g->advance = (s->advance.x + 32) >> 6;
			g->bitmap = malloc(g->w * g->h + 1);
			for (int y = 0; y < g->h; y++)
				memcpy(g->bitmap + y * g->w,
				       s->bitmap.buffer + y * s->bitmap.pitch, g->w);
		}
	}
	g->next = glyphs[slot];
	glyphs[slot] = g;
	return g;
}

/* Next code point of a UTF-8 string; advances *s */
static uint32_t utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t cp;
	int n;

	if (p[0] < 0x80) { cp = p[0]; n = 1; }
	else if ((p[0] & 0xe0) == 0xc0) { cp = p[0] & 0x1f; n = 2; }
	else if ((p[0] & 0xf0) == 0xe0) { cp = p[0] & 0x0f; n = 3; }
	else if ((p[0] & 0xf8) == 0xf0) { cp = p[0] & 0x07; n = 4; }
	else { *s += 1; return 0xfffd; }
	for (int i = 1; i < n; i++) {
		if ((p[i] & 0xc0) != 0x80) { *s += i; return 0xfffd; }
		cp = cp << 6 | (p[i] & 0x3f);
	}
	*s += n;
	return cp;
}

static inline uint32_t blend(uint32_t dst, uint32_t src, unsigned a)
{
	unsigned r = ((src >> 16 & 0xff) * a + (dst >> 16 & 0xff) * (255 - a)) / 255;
	unsigned g = ((src >> 8 & 0xff) * a + (dst >> 8 & 0xff) * (255 - a)) / 255;
	unsigned b = ((src & 0xff) * a + (dst & 0xff) * (255 - a)) / 255;

	return 0xff000000 | r << 16 | g << 8 | b;
}

int ui_text(struct ui_canvas *c, int x, int y, enum ui_font font, int size,
	    uint32_t color, const char *s)
{
	int pen = x, base = y + ascent(font, size);

	while (*s) {
		uint32_t cp = utf8_next(&s);
		struct glyph *g = glyph(font, size, cp);
		int gx = pen + g->left, gy = base - g->top;

		for (int j = 0; j < g->h; j++) {
			int py = gy + j;
			if (py < 0 || py >= c->h)
				continue;
			for (int i = 0; i < g->w; i++) {
				int px = gx + i;
				unsigned a = g->bitmap[j * g->w + i];
				if (a == 0 || px < 0 || px >= c->w)
					continue;
				uint32_t *d = &c->px[py * c->stride + px];
				*d = a == 255 ? (color | 0xff000000) : blend(*d, color, a);
			}
		}
		pen += g->advance;
	}
	return pen - x;
}

int ui_text_width(enum ui_font font, int size, const char *s)
{
	int w = 0;

	while (*s)
		w += glyph(font, size, utf8_next(&s))->advance;
	return w;
}

void ui_text_box(struct ui_canvas *c, int x, int y, int w, enum ui_font font,
		 int size, uint32_t color, enum ui_align align, const char *s)
{
	char cut[512];
	int tw = ui_text_width(font, size, s);

	if (tw > w) {
		const char *p = s, *end = s;
		int ell = ui_text_width(font, size, "…"), used = 0;
		while (*p) {
			const char *q = p;
			int adv = glyph(font, size, utf8_next(&q))->advance;
			if (used + adv + ell > w || q - s >= (int)sizeof(cut) - 4)
				break;
			used += adv;
			p = end = q;
		}
		snprintf(cut, sizeof(cut), "%.*s…", (int)(end - s), s);
		s = cut;
		tw = used + ell;
	}
	if (align == UI_CENTER)
		x += (w - tw) / 2;
	else if (align == UI_RIGHT)
		x += w - tw;
	ui_text(c, x, y, font, size, color, s);
}

/*
 * Break text into lines of width w: words while they fit, a word wider
 * than w split. starts[i] and ends[i] are each line's byte range; returns
 * the number of lines (at most max).
 */
static int wrap(enum ui_font font, int size, int w, const char *text,
		int *starts, int *ends, int max)
{
	const char *s = text;
	int n = 0;

	while (*s && n < max) {
		const char *p = s, *brk = NULL;
		int used = 0;
		while (*p && *p != '\n') {
			const char *q = p;
			int adv = glyph(font, size, utf8_next(&q))->advance;
			if (used + adv > w && p > s)
				break;
			used += adv;
			p = q;
			if (*p == ' ')
				brk = p;
		}
		const char *end = p;
		if (*p && *p != '\n' && brk != NULL)
			end = brk;
		starts[n] = s - text;
		ends[n++] = end - text;
		s = end;
		while (*s == ' ')
			s++;
		if (*s == '\n')
			s++;
	}
	return n;
}

int ui_text_wrap(struct ui_canvas *c, int x, int y, int w, enum ui_font font,
		 int size, uint32_t color, const char *text)
{
	int starts[256], ends[256], lh = ui_line_height(size);
	int n = wrap(font, size, w, text, starts, ends, 256);
	char line[1024];

	for (int i = 0; c != NULL && i < n; i++) {
		int len = ends[i] - starts[i];
		if (len >= (int)sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, text + starts[i], len);
		line[len] = 0;
		ui_text(c, x, y + i * lh, font, size, color, line);
	}
	return n * lh;
}

/* ----------------------------------------------------------- shapes --- */

void ui_fill(struct ui_canvas *c, int x, int y, int w, int h, uint32_t color)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;

	for (int j = y0; j < y1; j++)
		for (int i = x0; i < x1; i++)
			c->px[j * c->stride + i] = color;
}

void ui_round(struct ui_canvas *c, int x, int y, int w, int h, int r,
	      uint32_t color)
{
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	ui_fill(c, x, y + r, w, h - 2 * r, color);
	for (int j = 0; j < r; j++) {
		/* Rows of the top and bottom caps, edges antialiased */
		float dy = r - j - 0.5f;
		for (int i = 0; i < w; i++) {
			float dx = i < r ? r - i - 0.5f : i >= w - r ? i - (w - r) + 0.5f : 0;
			float d = dx * dx + dy * dy, a;
			if (dx == 0 || d <= (r - 1) * (r - 1))
				a = 1;
			else {
				float dist = __builtin_sqrtf(d);
				a = r - dist + 0.5f;
				if (a <= 0)
					continue;
				if (a > 1)
					a = 1;
			}
			int px = x + i;
			if (px < 0 || px >= c->w)
				continue;
			int rows[2] = { y + j, y + h - 1 - j };
			for (int k = 0; k < 2; k++) {
				if (rows[k] < 0 || rows[k] >= c->h)
					continue;
				uint32_t *d32 = &c->px[rows[k] * c->stride + px];
				*d32 = a >= 1 ? color : blend(*d32, color, a * 255);
			}
		}
	}
}

/* --------------------------------------------------------- settings --- */

struct ui_theme ui_theme = {
	.bg = 0xff101418, .fg = 0xffe8eaed, .dim = 0xff9aa0a6,
	.accent = 0xff4fc3f7, .bar = 0xff1c2228, .sel = 0xff2a3f52,
	.ok = 0xff81c784, .warn = 0xff5d4037,
};
static const struct ui_theme theme_default = {
	.bg = 0xff101418, .fg = 0xffe8eaed, .dim = 0xff9aa0a6,
	.accent = 0xff4fc3f7, .bar = 0xff1c2228, .sel = 0xff2a3f52,
	.ok = 0xff81c784, .warn = 0xff5d4037,
};

static void theme_load(void)
{
	static const struct { const char *key; size_t off; } keys[] = {
		{ "bg", offsetof(struct ui_theme, bg) },
		{ "fg", offsetof(struct ui_theme, fg) },
		{ "dim", offsetof(struct ui_theme, dim) },
		{ "accent", offsetof(struct ui_theme, accent) },
		{ "bar", offsetof(struct ui_theme, bar) },
		{ "sel", offsetof(struct ui_theme, sel) },
		{ "ok", offsetof(struct ui_theme, ok) },
		{ "warn", offsetof(struct ui_theme, warn) },
	};
	char name[64], path[256], line[128];
	FILE *f;

	ui_theme = theme_default;
	ui_setting("theme", "dark", name, sizeof(name));
	if (strchr(name, '/') != NULL)
		return;
	snprintf(path, sizeof(path), "%s/%s.theme", UI_THEMES_DIR, name);
	f = fopen(path, "re");
	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *v = strchr(line, '=');
		if (v == NULL || v[1] != '#')
			continue;
		*v = 0;
		for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
			if (strcmp(line, keys[i].key) == 0)
				*(uint32_t *)((char *)&ui_theme + keys[i].off) =
					0xff000000 | strtoul(v + 2, NULL, 16);
	}
	fclose(f);
}

/* Re-read the theme if the settings changed since the last look */
static void theme_check(void)
{
	static struct timespec seen = { -1, 0 };
	struct stat st;

	if (stat(UI_SETTINGS_FILE, &st) != 0)
		st.st_mtim = (struct timespec){ 0, 0 };
	if (st.st_mtim.tv_sec == seen.tv_sec && st.st_mtim.tv_nsec == seen.tv_nsec)
		return;
	seen = st.st_mtim;
	theme_load();
}

const char *ui_setting(const char *key, const char *def, char *buf, int len)
{
	FILE *f = fopen(UI_SETTINGS_FILE, "re");
	char line[256];
	size_t kl = strlen(key);

	snprintf(buf, len, "%s", def ? def : "");
	if (f == NULL)
		return buf;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (strncmp(line, key, kl) == 0 && line[kl] == '=')
			snprintf(buf, len, "%s", line + kl + 1);
	}
	fclose(f);
	return buf;
}

int ui_setting_int(const char *key, int def)
{
	char buf[32];

	if (ui_setting(key, NULL, buf, sizeof(buf))[0] == 0)
		return def;
	return atoi(buf);
}

bool ui_setting_set(const char *key, const char *value)
{
	char tmp[] = UI_SETTINGS_FILE ".XXXXXX", line[256];
	size_t kl = strlen(key);
	bool done = false;
	int fd = mkstemp(tmp);

	if (fd < 0)
		return false;
	FILE *out = fdopen(fd, "w"), *in = fopen(UI_SETTINGS_FILE, "re");
	if (in != NULL) {
		while (fgets(line, sizeof(line), in)) {
			if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
				if (value != NULL && !done)
					fprintf(out, "%s=%s\n", key, value);
				done = true;
				continue;
			}
			fputs(line, out);
		}
		fclose(in);
	}
	if (!done && value != NULL)
		fprintf(out, "%s=%s\n", key, value);
	fchmod(fd, 0664);
	if (fclose(out) != 0 || rename(tmp, UI_SETTINGS_FILE) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

void ui_clock(char *buf, int len)
{
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	if (ui_setting_int("clock", 12) == 24)
		strftime(buf, len, "%H:%M", &tm);
	else
		snprintf(buf, len, "%d:%02d %s", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12,
			 tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
}

/* -------------------------------------------------------- wallpaper --- */

static int ppm_token(FILE *f)
{
	int ch, v = 0;

	while ((ch = getc(f)) != EOF && (isspace(ch) || ch == '#'))
		if (ch == '#')
			while ((ch = getc(f)) != EOF && ch != '\n')
				;
	if (ch == EOF || !isdigit(ch))
		return -1;
	for (; ch != EOF && isdigit(ch); ch = getc(f))
		v = v * 10 + (ch - '0');
	return v;	/* the one whitespace after it is read */
}

bool ui_wallpaper(struct ui_canvas *c, const char *path)
{
	/* The last two (the lock screen's and the home screen's), as read
	 * and as scaled for their canvas */
	static struct wall {
		char path[128];
		struct timespec mtime;
		uint32_t *px, *fit;
		int w, h, fw, fh;
		long long used;
	} walls[2];
	static long long use;
	struct stat st;
	struct wall *wl = NULL;

	if (stat(path, &st) != 0)
		return false;
	for (int i = 0; i < 2; i++)
		if (!strcmp(walls[i].path, path))
			wl = &walls[i];
	if (!wl) {
		wl = walls[0].used <= walls[1].used ? &walls[0] : &walls[1];
		free(wl->px);
		free(wl->fit);
		*wl = (struct wall){ 0 };
		snprintf(wl->path, sizeof(wl->path), "%s", path);
	}
	wl->used = ++use;
	if (!wl->px || st.st_mtim.tv_sec != wl->mtime.tv_sec ||
	    st.st_mtim.tv_nsec != wl->mtime.tv_nsec) {
		FILE *f = fopen(path, "rbe");
		int w, h, max;

		free(wl->px);
		free(wl->fit);
		wl->px = wl->fit = NULL;
		wl->mtime = st.st_mtim;
		if (!f)
			return false;
		if (getc(f) != 'P' || getc(f) != '6' || (w = ppm_token(f)) <= 0 ||
		    (h = ppm_token(f)) <= 0 || (max = ppm_token(f)) <= 0 || max > 255 ||
		    w > 4096 || h > 4096) {
			fclose(f);
			return false;
		}
		unsigned char *rgb = malloc((size_t)w * h * 3);
		wl->px = malloc((size_t)w * h * sizeof(uint32_t));
		if (!rgb || !wl->px || fread(rgb, 3, (size_t)w * h, f) != (size_t)w * h) {
			free(rgb);
			free(wl->px);
			wl->px = NULL;
			fclose(f);
			return false;
		}
		fclose(f);
		for (size_t i = 0; i < (size_t)w * h; i++)
			wl->px[i] = 0xff000000u | (uint32_t)(rgb[3 * i] * 255 / max) << 16 |
				    (uint32_t)(rgb[3 * i + 1] * 255 / max) << 8 |
				    (uint32_t)(rgb[3 * i + 2] * 255 / max);
		free(rgb);
		wl->w = w;
		wl->h = h;
	}
	if (!wl->fit || wl->fw != c->w || wl->fh != c->h) {
		/* By width, the bottom rows if it is taller; by height, the
		 * middle columns, if it is wider */
		free(wl->fit);
		wl->fit = malloc((size_t)c->w * c->h * sizeof(uint32_t));
		if (!wl->fit)
			return false;
		double s = (double)wl->w / c->w, left = 0;
		double top = wl->h - c->h * s;
		if (top < 0) {
			s = (double)wl->h / c->h;
			top = 0;
			left = (wl->w - c->w * s) / 2;
		}
		for (int y = 0; y < c->h; y++) {
			int sy = (int)(top + (y + 0.5) * s);
			if (sy >= wl->h)
				sy = wl->h - 1;
			for (int x = 0; x < c->w; x++) {
				int sx = (int)(left + (x + 0.5) * s);
				if (sx >= wl->w)
					sx = wl->w - 1;
				wl->fit[(size_t)y * c->w + x] = wl->px[(size_t)sy * wl->w + sx];
			}
		}
		wl->fw = c->w;
		wl->fh = c->h;
	}
	for (int y = 0; y < c->h; y++)
		memcpy(c->px + (size_t)y * c->stride, wl->fit + (size_t)y * c->w,
		       (size_t)c->w * sizeof(uint32_t));
	return true;
}

/* ----------------------------------------------------------- pieces --- */

static int battery(void)
{
	static const char *paths[] = { "/run/s22-battery/capacity",
		"/sys/class/power_supply/battery/capacity" };
	char buf[16];

	for (unsigned i = 0; i < 2; i++) {
		int fd = open(paths[i], O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = 0;
			return atoi(buf);
		}
	}
	return -1;
}

/* WiFi: 0-4 bars, -1 not connected, -2 no WiFi; from iw, at most every 10 s */
static int wifi_level(void)
{
	static time_t checked;
	static int level = -2;
	char line[256], cmd[96], ifname[32] = "";
	struct dirent *de;
	time_t now = time(NULL);
	DIR *d;
	FILE *f;

	if (checked && now - checked < 10)
		return level;
	checked = now;
	level = -2;
	d = opendir("/sys/class/net");
	if (d) {
		while ((de = readdir(d)))
			if (!strncmp(de->d_name, "wlan", 4)) {
				snprintf(ifname, sizeof(ifname), "%.16s", de->d_name);
				break;
			}
		closedir(d);
	}
	if (!ifname[0])
		return level;
	level = -1;
	snprintf(cmd, sizeof(cmd), "iw dev %s link 2>/dev/null", ifname);
	f = popen(cmd, "re");
	if (!f)
		return level;
	while (fgets(line, sizeof(line), f)) {
		int dbm;

		if (sscanf(line, " signal: %d dBm", &dbm) == 1)
			level = dbm >= -55 ? 4 : dbm >= -65 ? 3 : dbm >= -75 ? 2 :
				dbm >= -85 ? 1 : 0;
	}
	pclose(f);
	return level;
}

/* Cellular from s22-celld: 0-4 bars when registered, 0 in limited or no
 * service (*full false), -1 without s22-celld */
static int cell_level(bool *full)
{
	char line[64];
	int bars = 0;
	FILE *f = fopen("/run/s22-cell/state", "re");

	*full = false;
	if (!f)
		return -1;
	bool off = false;
	while (fgets(line, sizeof(line), f)) {
		if (!strcmp(line, "service=full\n"))
			*full = true;
		else if (!strcmp(line, "radio=off\n"))
			off = true;	/* cellular switched off: no icon */
		sscanf(line, "bars=%d", &bars);
	}
	fclose(f);
	return off ? -1 : *full ? bars : 0;
}

/* The WiFi fan, 3 arcs over a dot, in a SIZE x SIZE box: arcs up to LEVEL lit */
void ui_wifi_icon(struct ui_canvas *c, int x0, int y0, int size, int level)
{
	int cx = x0 + size / 2, cy = y0 + size - 1;
	int bands = 4, band = size / bands;

	for (int y = y0; y < y0 + size; y++)
		for (int x = x0; x < x0 + size; x++) {
			int dx = x - cx, dy = cy - y;
			if (dy < 0 || dy < (dx < 0 ? -dx : dx))	/* a 90 degree fan */
				continue;
			int r2 = dx * dx + dy * dy;
			for (int b = 0; b < bands; b++) {
				int ro = (b + 1) * band, ri = b ? ro - band * 2 / 3 : 0;
				if (r2 <= ro * ro && r2 >= ri * ri) {
					if (x >= 0 && x < c->w && y >= 0 && y < c->h)
						c->px[y * c->stride + x] = b < level ? UI_FG : UI_BAR;
					break;
				}
			}
		}
}

/* Bluetooth: -1 off or unknown, 0 on, 1 a device connected (s22-btd) */
static int bt_level(void)
{
	char buf[256];
	int fd = open("/run/s22-bt/state", O_RDONLY | O_CLOEXEC);
	ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;

	if (fd >= 0)
		close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	if (!strstr(buf, "powered=1"))
		return -1;
	char *c = strstr(buf, "connected=");
	return c && atoi(c + 10) > 0 ? 1 : 0;
}

static void thick_line(struct ui_canvas *c, double x0, double y0, double x1,
		       double y1, int t, uint32_t color)
{
	double dx = x1 - x0, dy = y1 - y0;
	int n = (int)((fabs(dx) > fabs(dy) ? fabs(dx) : fabs(dy)) * 2) + 1;

	for (int i = 0; i <= n; i++)
		ui_fill(c, (int)(x0 + dx * i / n) - t / 2, (int)(y0 + dy * i / n) - t / 2,
			t, t, color);
}

/* The Bluetooth rune in a W x H box */
void ui_bt_icon(struct ui_canvas *c, int x0, int y0, int w, int h, uint32_t color)
{
	double cx = x0 + w / 2.0, r = x0 + w - 1, l = x0 + 1;
	double top = y0, bot = y0 + h - 1, up = y0 + h * 0.27, lo = y0 + h * 0.73;

	thick_line(c, cx, top, cx, bot, 2, color);
	thick_line(c, cx, top, r, up, 2, color);
	thick_line(c, r, up, l, lo, 2, color);
	thick_line(c, cx, bot, r, lo, 2, color);
	thick_line(c, r, lo, l, up, 2, color);
}

/* GPS (s22-gnssd, D079): -1 off, 0 searching, 1 a satellite fix */
static int gnss_level(void)
{
	char buf[512];
	int fd = open("/run/s22-gnss/active", O_RDONLY | O_CLOEXEC);
	ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;

	if (fd >= 0)
		close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	if (!strstr(buf, "session=on"))
		return -1;
	return strstr(buf, "fix=gnss") ? 1 : 0;
}

/* The location arrow, pointing up and right, in a W x H box */
void ui_location_icon(struct ui_canvas *c, int x0, int y0, int w, int h, uint32_t color)
{
	/* tip, left wing, the notch, bottom wing */
	static const double px[] = { 1.0, 0.0, 0.48, 0.58 }, py[] = { 0.0, 0.44, 0.52, 1.0 };

	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			double fx = (x + 0.5) / w, fy = (y + 0.5) / h;
			bool in = false;
			for (int i = 0, j = 3; i < 4; j = i++)
				if ((py[i] > fy) != (py[j] > fy) &&
				    fx < px[j] + (px[i] - px[j]) * (fy - py[j]) / (py[i] - py[j]))
					in = !in;
			if (in)
				ui_fill(c, x0 + x, y0 + y, 1, 1, color);
		}
}

/* Silent or vibrate only (D041, D042): 0 neither, 1 vibrate, 2 silent */
static int quiet_mode(void)
{
	char v[16];
	bool sounds = strcmp(ui_setting("sounds", "on", v, sizeof(v)), "off") != 0 &&
		      ui_setting_int("ring_volume", 12) > 0;
	bool vib = strcmp(ui_setting("vibrate", "on", v, sizeof(v)), "off") != 0;

	return sounds ? 0 : vib ? 1 : 2;
}

/* A phone with buzz marks either side, in a W x H box */
void ui_vibrate_icon(struct ui_canvas *c, int x0, int y0, int w, int h, uint32_t color)
{
	int pw = w * 5 / 12, px = x0 + (w - pw) / 2;

	ui_fill(c, px, y0, pw, 2, color);			/* the phone */
	ui_fill(c, px, y0 + h - 2, pw, 2, color);
	ui_fill(c, px, y0, 2, h, color);
	ui_fill(c, px + pw - 2, y0, 2, h, color);
	for (int side = 0; side < 2; side++) {			/* the buzz */
		double bx = side ? px + pw + 2 : px - 3;
		double dx = side ? 2 : -2;
		thick_line(c, bx, y0 + h * 0.25, bx + dx, y0 + h * 0.42, 2, color);
		thick_line(c, bx + dx, y0 + h * 0.42, bx, y0 + h * 0.58, 2, color);
		thick_line(c, bx, y0 + h * 0.58, bx + dx, y0 + h * 0.75, 2, color);
	}
}

/* A loudspeaker struck through, in a W x H box */
void ui_silent_icon(struct ui_canvas *c, int x0, int y0, int w, int h, uint32_t color)
{
	double cy = y0 + h / 2.0;

	ui_fill(c, x0 + 1, (int)(cy - h * 0.18), w / 4, (int)(h * 0.36) + 1, color);
	for (int i = 0; i < w / 3; i++) {			/* the cone */
		int half = (int)(h * 0.18 + (h * 0.30) * i / (w / 3.0));
		ui_fill(c, x0 + 1 + w / 4 + i, (int)cy - half, 1, 2 * half + 1, color);
	}
	thick_line(c, x0, y0 + 1, x0 + w - 1, y0 + h - 2, 2, color);
}

/* Four rising bars in a W x H box */
void ui_cell_icon(struct ui_canvas *c, int x0, int y0, int w, int h, int bars)
{
	int bw = (w - 3 * 2) / 4;

	for (int i = 0; i < 4; i++) {
		int bh = h * (i + 1) / 4;
		ui_fill(c, x0 + i * (bw + 2), y0 + h - bh, bw, bh, i < bars ? UI_FG : UI_BAR);
	}
}

void ui_status_bar(struct ui_canvas *c)
{
	char right[32], clock[16];
	int pct = battery(), x = 10, wifi;
	bool full;

	ui_fill(c, 0, 0, c->w, UI_STATUS_H, UI_BG);
	wifi = wifi_level();
	if (wifi >= -1) {
		ui_wifi_icon(c, x, 4, 18, wifi > 0 ? wifi : 0);
		x += 26;
	}
	int cell = cell_level(&full);
	if (cell >= 0) {
		ui_cell_icon(c, x, 5, 18, 16, cell);
		if (!full)	/* no service: a cross after the bars */
			ui_text(c, x + 20, 2, UI_SANS, 14, UI_DIM, "×");
		x += full ? 26 : 34;
	}
	int bt = bt_level();	/* grey when on, blue with a device */
	if (bt >= 0) {
		ui_bt_icon(c, x, 5, 10, 16, bt ? 0xff3d8bfd : UI_DIM);
		x += 18;
	}
	int gnss = gnss_level();	/* grey while searching, white with a fix */
	if (gnss >= 0) {
		ui_location_icon(c, x, 6, 14, 14, gnss ? UI_FG : UI_DIM);
		x += 22;
	}
	int quiet = quiet_mode();
	if (quiet == 1)
		ui_vibrate_icon(c, x, 5, 18, 16, UI_FG);
	else if (quiet == 2)
		ui_silent_icon(c, x, 5, 18, 16, UI_FG);
	ui_clock(clock, sizeof(clock));
	if (pct >= 0)
		snprintf(right, sizeof(right), "%s  %d%%", clock, pct);
	else
		snprintf(right, sizeof(right), "%s", clock);
	int rw = ui_text_width(UI_SANS, 16, right);
	ui_text(c, c->w - rw - 10, 3, UI_SANS, 16, UI_FG, right);
}

void ui_header(struct ui *ui, struct ui_canvas *c, const char *title)
{
	ui_fill(c, 0, 0, c->w, UI_TITLE_H, UI_BAR);
	ui_text_box(c, 10, 6, c->w - 20, UI_BOLD, 19, UI_FG, UI_CENTER, title);
}

void ui_hints(struct ui_canvas *c, const char *back, const char *ok,
	      const char *right)
{
	int y = c->h - UI_HINTS_H, w3 = c->w / 3;
	char buf[64];

	ui_fill(c, 0, y, c->w, UI_HINTS_H, UI_BAR);
	if (back) {
		snprintf(buf, sizeof(buf), "◁ %s", back);
		ui_text_box(c, 10, y + 8, w3 - 10, UI_SANS, 17, UI_DIM, UI_LEFT, buf);
	}
	if (ok)
		ui_text_box(c, w3, y + 7, w3, UI_BOLD, 18, UI_FG, UI_CENTER, ok);
	if (right)
		ui_text_box(c, 2 * w3, y + 8, w3 - 10, UI_SANS, 17, UI_DIM,
			    UI_RIGHT, right);
}

bool ui_list_key(struct ui_list *l, uint32_t code)
{
	if (code == KEY_UP) {
		if (l->sel > 0)
			l->sel--;
		return true;
	}
	if (code == KEY_DOWN) {
		if (l->sel < l->count - 1)
			l->sel++;
		return true;
	}
	return false;
}

int ui_list_at(struct ui_list *l, int list_y, int y)
{
	int i;

	if (y < list_y || l->row_h <= 0)
		return -1;
	i = l->top + (y - list_y) / l->row_h;
	return i < l->count ? i : -1;
}

void ui_list_drag(struct ui_list *l, int dy, int h)
{
	int rows = l->row_h > 0 ? h / l->row_h : 1;

	if (rows < 1)
		rows = 1;
	l->drag += dy;
	/* Finger down: earlier rows come into view */
	while (l->drag >= l->row_h && l->top > 0) {
		l->top--;
		l->drag -= l->row_h;
	}
	while (l->drag <= -l->row_h && l->top + rows < l->count) {
		l->top++;
		l->drag += l->row_h;
	}
	if (l->drag >= l->row_h || l->drag <= -l->row_h)
		l->drag = 0;	/* at an end */
	/* The selection stays on screen, so drawing keeps this top */
	if (l->sel < l->top)
		l->sel = l->top;
	if (l->sel >= l->top + rows)
		l->sel = l->top + rows - 1;
}

void ui_list_draw(struct ui *ui, struct ui_canvas *c, struct ui_list *l,
		  int y, int h,
		  void (*row)(struct ui *ui, struct ui_canvas *c, int i,
			      int x, int y, int w, int h, bool sel))
{
	int rows = h / l->row_h;

	if (rows < 1)
		rows = 1;
	if (l->sel >= l->count)
		l->sel = l->count - 1;
	if (l->sel < 0)
		l->sel = 0;
	if (l->sel < l->top)
		l->top = l->sel;
	if (l->sel >= l->top + rows)
		l->top = l->sel - rows + 1;
	int w = l->count > rows ? c->w - 6 : c->w;
	for (int i = l->top; i < l->count && i < l->top + rows; i++) {
		int ry = y + (i - l->top) * l->row_h;
		if (i == l->sel)
			ui_fill(c, 0, ry, w, l->row_h, UI_SEL);
		row(ui, c, i, 0, ry, w, l->row_h, i == l->sel);
	}
	if (l->count > rows) {
		int bh = h * rows / l->count;
		int by = y + (h - bh) * l->top / (l->count - rows);
		ui_fill(c, c->w - 4, by, 3, bh, UI_DIM);
	}
}

/* ------------------------------------------------------ the window --- */

static void buffer_release(void *data, struct wl_buffer *wl)
{
	struct buffer *b = data;

	b->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void buffer_free(struct buffer *b)
{
	if (b->wl)
		wl_buffer_destroy(b->wl);
	if (b->px)
		munmap(b->px, b->size);
	memset(b, 0, sizeof(*b));
}

static bool buffer_alloc(struct ui *ui, struct buffer *b)
{
	int stride = ui->w * 4;
	int fd = memfd_create("s22ui", MFD_CLOEXEC);

	b->size = (size_t)stride * ui->h;
	if (fd < 0 || ftruncate(fd, b->size) < 0) {
		if (fd >= 0)
			close(fd);
		return false;
	}
	b->px = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (b->px == MAP_FAILED) {
		b->px = NULL;
		close(fd);
		return false;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(ui->shm, fd, b->size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, ui->w, ui->h, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b->wl, &buffer_listener, b);
	b->w = ui->w;
	b->h = ui->h;
	return true;
}

static long long now_msec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	struct ui *ui = data;

	wl_callback_destroy(cb);
	ui->frame_pending = false;
	ui->frame_got = now_msec();
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

static void draw(struct ui *ui)
{
	struct buffer *b = NULL;

	for (int i = 0; i < 2; i++) {
		if (ui->buf[i].busy)
			continue;
		if (ui->buf[i].w != ui->w || ui->buf[i].h != ui->h) {
			buffer_free(&ui->buf[i]);
			if (!buffer_alloc(ui, &ui->buf[i]))
				continue;
		}
		b = &ui->buf[i];
		break;
	}
	if (b == NULL)
		return;		/* both with the compositor; drawn on release */
	ui->dirty = false;
	theme_check();

	struct ui_canvas c = { .px = b->px, .w = ui->w, .h = ui->h,
		.stride = ui->w };
	if (!ui->app->opaque)
		ui_fill(&c, 0, 0, c.w, c.h, UI_BG);
	entry_frame[0] = 0;
	if (ui->app->draw)
		ui->app->draw(ui, &c);
	/* Only the window in front speaks for the status bar */
	if (ui->active && entry_frame[0])
		entry_publish(entry_frame);
	else if (entry_published)
		entry_publish("");
	wl_surface_attach(ui->surface, b->wl, 0, 0);
	wl_surface_damage_buffer(ui->surface, 0, 0, ui->w, ui->h);
	struct wl_callback *cb = wl_surface_frame(ui->surface);
	wl_callback_add_listener(cb, &frame_listener, ui);
	if (!ui->frame_pending || !ui->frame_asked)
		ui->frame_asked = now_msec();
	ui->frame_pending = true;
	wl_surface_commit(ui->surface);
	b->busy = true;
}

static void xdg_surface_configure(void *data, struct xdg_surface *s,
				  uint32_t serial)
{
	struct ui *ui = data;

	xdg_surface_ack_configure(s, serial);
	if (ui->next_w > 0 && ui->next_h > 0) {
		ui->w = ui->next_w;
		ui->h = ui->next_h;
	}
	ui->configured = true;
	ui->dirty = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t,
			       int32_t w, int32_t h, struct wl_array *states)
{
	struct ui *ui = data;
	uint32_t *st;
	bool active = false;

	ui->next_w = w;
	ui->next_h = h;
	wl_array_for_each(st, states)
		if (*st == XDG_TOPLEVEL_STATE_ACTIVATED)
			active = true;
	if (active != ui->active) {
		ui->active = active;
		ui->dirty = true;	/* publishes, or takes back, the indicator */
	}
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
	ui_quit(data);
}

static void toplevel_bounds(void *data, struct xdg_toplevel *t, int32_t w,
			    int32_t h) {}
static void toplevel_caps(void *data, struct xdg_toplevel *t,
			  struct wl_array *caps) {}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_bounds,
	.wm_capabilities = toplevel_caps,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
	xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

/* ---------------------------------------------------------- keyboard --- */

static void repeat_arm(struct ui *ui, int first_ms, int every_ms)
{
	struct itimerspec it = {
		.it_value = { first_ms / 1000, (first_ms % 1000) * 1000000L },
		.it_interval = { every_ms / 1000, (every_ms % 1000) * 1000000L },
	};

	timerfd_settime(ui->repeat_fd, 0, &it, NULL);
}

static void key_deliver(struct ui *ui, uint32_t key, enum ui_key_state st)
{
	char text[16] = "";

	if (ui->xkb_state)
		xkb_state_key_get_utf8(ui->xkb_state, key + 8, text, sizeof(text));
	if (ui->app->key)
		ui->app->key(ui, key, st, text);
}

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
		      int32_t fd, uint32_t size)
{
	struct ui *ui = data;

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	struct xkb_keymap *keymap = xkb_keymap_new_from_string(ui->xkb, map,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (keymap == NULL)
		return;
	xkb_state_unref(ui->xkb_state);
	xkb_keymap_unref(ui->keymap);
	ui->keymap = keymap;
	ui->xkb_state = xkb_state_new(keymap);
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
		     struct wl_surface *s, struct wl_array *keys)
{
	struct ui *ui = data;

	if (ui->app->focus)
		ui->app->focus(ui, true);
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
		     struct wl_surface *s)
{
	struct ui *ui = data;

	ui->repeat_key = 0;
	repeat_arm(ui, 0, 0);
	if (ui->app->focus)
		ui->app->focus(ui, false);
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
		   uint32_t time, uint32_t key, uint32_t state)
{
	struct ui *ui = data;
	bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;

	if (pressed && ui->repeat_rate > 0 && ui->keymap &&
	    xkb_keymap_key_repeats(ui->keymap, key + 8)) {
		ui->repeat_key = key;
		repeat_arm(ui, ui->repeat_delay, 1000 / ui->repeat_rate);
	} else if (!pressed && key == ui->repeat_key) {
		ui->repeat_key = 0;
		repeat_arm(ui, 0, 0);
	}
	key_deliver(ui, key, pressed ? UI_PRESSED : UI_RELEASED);
}

static void kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
			 uint32_t depressed, uint32_t latched, uint32_t locked,
			 uint32_t group)
{
	struct ui *ui = data;

	if (ui->xkb_state)
		xkb_state_update_mask(ui->xkb_state, depressed, latched, locked,
				      0, 0, group);
}

static void kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
			   int32_t delay)
{
	struct ui *ui = data;

	ui->repeat_rate = rate;
	ui->repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

/* ------------------------------------------------------------- touch --- */

#define DRAG_START	12	/* pixels a finger moves before a tap is a drag */

static void touch_down(void *data, struct wl_touch *t, uint32_t serial,
		       uint32_t time, struct wl_surface *s, int32_t id,
		       wl_fixed_t x, wl_fixed_t y)
{
	struct ui *ui = data;

	if (ui->app->touch)
		ui->app->touch(ui, id, UI_PRESSED, wl_fixed_to_int(x),
			       wl_fixed_to_int(y));
	if (ui->touch_id >= 0)
		return;		/* one finger at a time */
	ui->touch_id = id;
	ui->touch_x0 = ui->touch_x = wl_fixed_to_double(x);
	ui->touch_y0 = ui->touch_y = wl_fixed_to_double(y);
	ui->dragging = false;
}

static void touch_motion(void *data, struct wl_touch *t, uint32_t time,
			 int32_t id, wl_fixed_t x, wl_fixed_t y)
{
	struct ui *ui = data;
	double nx = wl_fixed_to_double(x), ny = wl_fixed_to_double(y);

	if (ui->app->touch)
		ui->app->touch(ui, id, UI_REPEATED, (int)nx, (int)ny);
	if (id != ui->touch_id)
		return;
	if (!ui->dragging && (nx - ui->touch_x0) * (nx - ui->touch_x0) +
	    (ny - ui->touch_y0) * (ny - ui->touch_y0) < DRAG_START * DRAG_START)
		return;
	ui->dragging = true;
	if (ui->app->drag)
		ui->app->drag(ui, (int)(nx - ui->touch_x), (int)(ny - ui->touch_y),
			      false);
	ui->touch_x = nx;
	ui->touch_y = ny;
}

static void touch_up(void *data, struct wl_touch *t, uint32_t serial,
		     uint32_t time, int32_t id)
{
	struct ui *ui = data;
	int x = ui->touch_x0, y = ui->touch_y0;

	if (ui->app->touch)
		ui->app->touch(ui, id, UI_RELEASED, -1, -1);
	if (id != ui->touch_id)
		return;
	ui->touch_id = -1;
	if (ui->dragging) {
		if (ui->app->drag)
			ui->app->drag(ui, 0, 0, true);
		return;
	}
	if (ui->app->tap)
		ui->app->tap(ui, x, y);
}

static void touch_frame(void *data, struct wl_touch *t) {}

static void touch_cancel(void *data, struct wl_touch *t)
{
	struct ui *ui = data;

	if (ui->touch_id >= 0 && ui->dragging && ui->app->drag)
		ui->app->drag(ui, 0, 0, true);
	ui->touch_id = -1;
}

static void touch_shape(void *data, struct wl_touch *t, int32_t id,
			wl_fixed_t major, wl_fixed_t minor) {}
static void touch_orientation(void *data, struct wl_touch *t, int32_t id,
			      wl_fixed_t orientation) {}

static const struct wl_touch_listener touch_listener = {
	.down = touch_down,
	.up = touch_up,
	.motion = touch_motion,
	.frame = touch_frame,
	.cancel = touch_cancel,
	.shape = touch_shape,
	.orientation = touch_orientation,
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
	struct ui *ui = data;
	bool has = caps & WL_SEAT_CAPABILITY_KEYBOARD;

	if (has && ui->keyboard == NULL) {
		ui->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(ui->keyboard, &keyboard_listener, ui);
	} else if (!has && ui->keyboard != NULL) {
		wl_keyboard_release(ui->keyboard);
		ui->keyboard = NULL;
	}
	has = caps & WL_SEAT_CAPABILITY_TOUCH;
	if (has && ui->touch == NULL) {
		ui->touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(ui->touch, &touch_listener, ui);
	} else if (!has && ui->touch != NULL) {
		wl_touch_release(ui->touch);
		ui->touch = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
	.name = seat_name,
};

static void registry_global(void *data, struct wl_registry *reg,
			    uint32_t name, const char *iface, uint32_t ver)
{
	struct ui *ui = data;

	if (strcmp(iface, wl_compositor_interface.name) == 0)
		ui->compositor = wl_registry_bind(reg, name,
			&wl_compositor_interface, 4);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		ui->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		ui->wm_base = wl_registry_bind(reg, name,
			&xdg_wm_base_interface, ver < 3 ? ver : 3);
		xdg_wm_base_add_listener(ui->wm_base, &wm_base_listener, ui);
	} else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
		ui->layer_shell = wl_registry_bind(reg, name,
			&zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
	} else if (strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0) {
		ui->lock_manager = wl_registry_bind(reg, name,
			&ext_session_lock_manager_v1_interface, 1);
	} else if (strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name) == 0) {
		ui->inhibit_manager = wl_registry_bind(reg, name,
			&zwp_idle_inhibit_manager_v1_interface, 1);
	} else if (strcmp(iface, wl_output_interface.name) == 0 && !ui->output) {
		ui->output = wl_registry_bind(reg, name, &wl_output_interface, 1);
	} else if (strcmp(iface, wl_seat_interface.name) == 0 && !ui->seat) {
		ui->seat = wl_registry_bind(reg, name, &wl_seat_interface,
			ver < 5 ? ver : 5);
		wl_seat_add_listener(ui->seat, &seat_listener, ui);
	}
}

static void registry_remove(void *data, struct wl_registry *reg,
			    uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

/* ------------------------------------------------------- the overlay --- */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
			    uint32_t serial, uint32_t w, uint32_t h)
{
	struct ui *ui = data;

	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if (w > 0 && h > 0) {
		ui->w = w;
		ui->h = h;
	}
	ui->configured = true;
	ui->dirty = true;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	ui_layer_hide(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void layer_apply(struct ui *ui)
{
	uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;

	if (ui->layer_h == 0)
		anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	zwlr_layer_surface_v1_set_anchor(ui->layer_surface, anchor);
	zwlr_layer_surface_v1_set_size(ui->layer_surface, 0, ui->layer_h);
	zwlr_layer_surface_v1_set_exclusive_zone(ui->layer_surface,
		ui->layer_bar ? ui->layer_h : -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(ui->layer_surface,
		ui->layer_kb ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
			     : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
}

void ui_layer_show(struct ui *ui, int height, bool keyboard)
{
	if (ui->layer_shell == NULL)
		return;
	if (ui->layer_surface != NULL && ui->layer_h == height &&
	    ui->layer_kb == keyboard) {
		ui->dirty = true;
		return;
	}
	ui->layer_h = height;
	ui->layer_kb = keyboard;
	if (ui->layer_surface == NULL) {
		ui->surface = wl_compositor_create_surface(ui->compositor);
		ui->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			ui->layer_shell, ui->surface, NULL,
			ui->layer_bar ? ZWLR_LAYER_SHELL_V1_LAYER_TOP
				      : ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
			ui->app->app_id);
		zwlr_layer_surface_v1_add_listener(ui->layer_surface,
			&layer_listener, ui);
		ui->configured = false;
	}
	layer_apply(ui);
	wl_surface_commit(ui->surface);
}

void ui_layer_bar(struct ui *ui, int height)
{
	ui->layer_bar = true;
	ui_layer_show(ui, height, false);
}

void ui_layer_hide(struct ui *ui)
{
	if (ui->layer_surface == NULL)
		return;
	zwlr_layer_surface_v1_destroy(ui->layer_surface);
	wl_surface_destroy(ui->surface);
	ui->layer_surface = NULL;
	ui->surface = NULL;
	ui->configured = false;
	ui->frame_pending = false;
	buffer_free(&ui->buf[0]);
	buffer_free(&ui->buf[1]);
}

bool ui_layer_shown(struct ui *ui)
{
	return ui->layer_surface != NULL;
}

/* ---------------------------------------------------- the session lock --- */

static void lock_locked(void *data, struct ext_session_lock_v1 *lock) {}

static void lock_finished(void *data, struct ext_session_lock_v1 *lock)
{
	struct ui *ui = data;

	/* Refused (locked already), or the lock is over: nothing to do here */
	fprintf(stderr, "%s: the compositor ended the lock\n", ui->app->app_id);
	ext_session_lock_v1_destroy(lock);
	ui->session_lock = NULL;
	ui->quit = true;
}

static const struct ext_session_lock_v1_listener lock_listener = {
	.locked = lock_locked,
	.finished = lock_finished,
};

static void lock_surface_configure(void *data, struct ext_session_lock_surface_v1 *ls,
				   uint32_t serial, uint32_t w, uint32_t h)
{
	struct ui *ui = data;

	ext_session_lock_surface_v1_ack_configure(ls, serial);
	if (w > 0 && h > 0) {
		ui->w = w;
		ui->h = h;
	}
	ui->configured = true;
	ui->dirty = true;
}

static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
	.configure = lock_surface_configure,
};

static bool lock_start(struct ui *ui)
{
	if (!ui->lock_manager || !ui->output)
		return false;
	ui->session_lock = ext_session_lock_manager_v1_lock(ui->lock_manager);
	ext_session_lock_v1_add_listener(ui->session_lock, &lock_listener, ui);
	ui->surface = wl_compositor_create_surface(ui->compositor);
	ui->lock_surface = ext_session_lock_v1_get_lock_surface(ui->session_lock,
		ui->surface, ui->output);
	ext_session_lock_surface_v1_add_listener(ui->lock_surface,
		&lock_surface_listener, ui);
	ui->active = true;
	return true;
}

void ui_inhibit_idle(struct ui *ui, bool on)
{
	if (on && !ui->inhibitor && ui->inhibit_manager && ui->surface)
		ui->inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
			ui->inhibit_manager, ui->surface);
	else if (!on && ui->inhibitor) {
		zwp_idle_inhibitor_v1_destroy(ui->inhibitor);
		ui->inhibitor = NULL;
	}
}

bool ui_visible(struct ui *ui)
{
	/* The compositor stops answering frames when nothing shows them */
	return !ui->frame_pending || now_msec() - ui->frame_asked < 500;
}

void ui_unlock(struct ui *ui)
{
	if (ui->session_lock) {
		ext_session_lock_v1_unlock_and_destroy(ui->session_lock);
		ui->session_lock = NULL;
		wl_display_roundtrip(ui->display);
	}
	ui->quit = true;
}

/* -------------------------------------------------------------- API --- */

void ui_quit(struct ui *ui) { ui->quit = true; }
void ui_redraw(struct ui *ui) { ui->dirty = true; }
void *ui_data(struct ui *ui) { return ui->app->data; }
int ui_width(struct ui *ui) { return ui->w; }
int ui_height(struct ui *ui) { return ui->h; }

void ui_timer(struct ui *ui, int ms)
{
	struct itimerspec it = {
		.it_value = { ms / 1000, (ms % 1000) * 1000000L },
	};

	timerfd_settime(ui->timer_fd, 0, &it, NULL);
}

bool ui_watch(struct ui *ui, int fd, void (*cb)(struct ui *ui, int fd))
{
	if (ui->nwatch == MAX_WATCH)
		return false;
	ui->watch[ui->nwatch++] = (struct watch){ fd, cb };
	return true;
}

void ui_unwatch(struct ui *ui, int fd)
{
	for (int i = 0; i < ui->nwatch; i++)
		if (ui->watch[i].fd == fd) {
			ui->watch[i] = ui->watch[--ui->nwatch];
			return;
		}
}

void ui_spawn(const char *cmd)
{
	pid_t pid = fork();

	if (pid == 0) {
		/* Twice, so the program is nobody's child to wait for */
		setsid();
		if (fork() == 0) {
			sigset_t set;
			sigemptyset(&set);
			sigprocmask(SIG_SETMASK, &set, NULL);
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
		}
		_exit(0);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

bool ui_wm(const char *cmd, char *buf, int len)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	char line[256];
	int fd, n;

	if (dir == NULL)
		return false;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/s22-wm.sock", dir);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return false;
	}
	n = snprintf(line, sizeof(line), "%s\n", cmd);
	if (send(fd, line, n, MSG_NOSIGNAL) != n) {
		close(fd);
		return false;
	}
	n = 0;
	if (buf != NULL && len > 0) {
		int r;
		while (n < len - 1 && (r = read(fd, buf + n, len - 1 - n)) > 0)
			n += r;
		buf[n] = 0;
	}
	close(fd);
	return true;
}

/* ------------------------------------------------------------- apps --- */

static int apps_load(const char *dir, struct ui_appinfo *apps, int n, int max)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (d == NULL)
		return n;
	while ((e = readdir(d)) != NULL) {
		size_t len = strlen(e->d_name);
		if (len < 5 || strcmp(e->d_name + len - 4, ".app") != 0 ||
		    len >= sizeof(apps[0].file))
			continue;
		char path[512], line[512];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		FILE *f = fopen(path, "re");
		if (f == NULL)
			continue;

		struct ui_appinfo a = { .color = 0xff37474f, .order = 50 };
		snprintf(a.file, sizeof(a.file), "%s", e->d_name);
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			char *v = strchr(line, '=');
			if (v == NULL || line[0] == '#')
				continue;
			*v++ = 0;
			if (strcmp(line, "Name") == 0)
				snprintf(a.name, sizeof(a.name), "%s", v);
			else if (strcmp(line, "Exec") == 0)
				snprintf(a.exec, sizeof(a.exec), "%s", v);
			else if (strcmp(line, "Id") == 0)
				snprintf(a.id, sizeof(a.id), "%s", v);
			else if (strcmp(line, "Icon") == 0)
				snprintf(a.icon, sizeof(a.icon), "%s", v);
			else if (strcmp(line, "Hidden") == 0)
				a.hidden = strcmp(v, "yes") == 0;
			else if (strcmp(line, "Handles") == 0)
				snprintf(a.handles, sizeof(a.handles), "%s", v);
			else if (strcmp(line, "Folder") == 0)
				snprintf(a.folder, sizeof(a.folder), "%s", v);
			else if (strcmp(line, "Notify") == 0)
				snprintf(a.notify, sizeof(a.notify), "%s", v);
			else if (strcmp(line, "Badge") == 0)
				snprintf(a.badge, sizeof(a.badge), "%s", v);
			else if (strcmp(line, "Color") == 0 && v[0] == '#')
				a.color = 0xff000000 | strtoul(v + 1, NULL, 16);
			else if (strcmp(line, "Order") == 0)
				a.order = atoi(v);
		}
		fclose(f);
		if (a.name[0] == 0 || a.exec[0] == 0)
			continue;

		int i;
		for (i = 0; i < n; i++)
			if (strcmp(apps[i].file, a.file) == 0)
				break;
		if (i == n) {
			if (n == max)
				continue;
			n++;
		}
		apps[i] = a;
	}
	closedir(d);
	return n;
}

static int by_order(const void *pa, const void *pb)
{
	const struct ui_appinfo *a = pa, *b = pb;

	if (a->order != b->order)
		return a->order - b->order;
	return strcmp(a->name, b->name);
}

int ui_apps(struct ui_appinfo *apps, int max)
{
	const char *home = getenv("HOME");
	char dir[512];
	int n = apps_load("/usr/share/s22/apps", apps, 0, max);

	/* Desktop applications on the SD card (pacman-sd, D046) */
	n = apps_load("/home/.pacman-sd/apps", apps, n, max);

	if (home) {
		snprintf(dir, sizeof(dir), "%s/.local/share/s22/apps", home);
		n = apps_load(dir, apps, n, max);
	}
	qsort(apps, n, sizeof(apps[0]), by_order);
	return n;
}

void ui_open_app(const struct ui_appinfo *a)
{
	char cmd[96], reply[16] = "";

	if (a->id[0]) {
		snprintf(cmd, sizeof(cmd), "raise %s", a->id);
		if (ui_wm(cmd, reply, sizeof(reply)) && strncmp(reply, "ok", 2) == 0)
			return;
	}
	ui_spawn(a->exec);
}

bool ui_app_for(const char *source, struct ui_appinfo *app)
{
	struct ui_appinfo apps[64];
	int n = ui_apps(apps, 64);

	for (int i = 0; i < n; i++)
		if (apps[i].notify[0] && strcmp(apps[i].notify, source) == 0) {
			*app = apps[i];
			return true;
		}
	return false;
}

/* ------------------------------------------------------------ links --- */

static bool uri_ok(const char *uri)
{
	const char *colon = strchr(uri, ':');

	if (colon == NULL || colon == uri || strlen(uri) > 200)
		return false;
	for (const char *p = uri; *p; p++)
		if (!(isalnum((unsigned char)*p) || strchr("+-_.:/@%=?&#*,", *p)))
			return false;
	return true;
}

static bool handles(const struct ui_appinfo *a, const char *scheme, size_t n)
{
	const char *p = a->handles;

	while (*p) {
		size_t len = strcspn(p, ",");
		if (len == n && strncmp(p, scheme, n) == 0)
			return true;
		p += len + (p[len] == ',');
	}
	return false;
}

static bool app_socket_path(const char *id, struct sockaddr_un *sa)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");

	if (dir == NULL || id[0] == 0)
		return false;
	*sa = (struct sockaddr_un){ .sun_family = AF_UNIX };
	snprintf(sa->sun_path, sizeof(sa->sun_path), "%s/s22-app-%s.sock", dir, id);
	return true;
}

bool ui_open_uri(const char *uri)
{
	struct ui_appinfo apps[64];
	int n = ui_apps(apps, 64);
	size_t slen = strcspn(uri, ":");
	struct sockaddr_un sa;
	char line[256], cmd[512];

	if (!uri_ok(uri))
		return false;
	for (int i = 0; i < n; i++) {
		if (!handles(&apps[i], uri, slen))
			continue;
		/* Running: hand it over, and raise it */
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd >= 0 && app_socket_path(apps[i].id, &sa) &&
		    connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
			int len = snprintf(line, sizeof(line), "open %s\n", uri);
			bool sent = send(fd, line, len, MSG_NOSIGNAL) == len;
			close(fd);
			if (sent) {
				snprintf(cmd, sizeof(cmd), "raise %s", apps[i].id);
				ui_wm(cmd, NULL, 0);
				return true;
			}
		} else if (fd >= 0) {
			close(fd);
		}
		/* Not running: start it with the link (uri_ok: nothing to quote) */
		snprintf(cmd, sizeof(cmd), "%s '%s'", apps[i].exec, uri);
		ui_spawn(cmd);
		return true;
	}
	return false;
}

static void on_app_socket(struct ui *ui, int fd)
{
	char buf[256];
	int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	ssize_t n;

	if (c < 0)
		return;
	struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
	setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	n = read(c, buf, sizeof(buf) - 1);
	close(c);
	if (n <= 0)
		return;
	buf[n] = 0;
	buf[strcspn(buf, "\r\n")] = 0;
	if (strncmp(buf, "open ", 5) == 0 && uri_ok(buf + 5) && ui->app->open) {
		ui->app->open(ui, buf + 5);
		ui->dirty = true;
	}
}

static void app_socket_listen(struct ui *ui)
{
	struct sockaddr_un sa;
	int fd;

	if (!ui->app->open || !app_socket_path(ui->app->app_id, &sa))
		return;
	unlink(sa.sun_path);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    listen(fd, 4) < 0) {
		fprintf(stderr, "%s: %s: %s\n", ui->app->app_id, sa.sun_path,
			strerror(errno));
		if (fd >= 0)
			close(fd);
		return;
	}
	ui_watch(ui, fd, on_app_socket);
}

/* ------------------------------------------------------- text entry --- */

static const char *const taps[10] = {
	" 0", ".,?!-/_:;'\"1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7",
	"tuv8", "wxyz9",
};
/* Every ASCII symbol, the shell's most used first (1 has the common
 * punctuation too) */
static const char symbols[] = ".,?!'\"-:;()@&*/#$%+=_~^|\\<>[]{}`";
enum { M_LOWER, M_CAP, M_UPPER, M_DIGITS, M_COUNT };
static const char *mode_names[] = { "abc", "Abc", "ABC", "123" };
#define TAP_MS	900
#define HOLD_MS	500

static struct ui_entry *entries[8];

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int key_digit(uint32_t code)
{
	if (code >= KEY_1 && code <= KEY_9)
		return code - KEY_1 + 1;
	if (code == KEY_0)
		return 0;
	return -1;
}

static const char *tap_list(uint32_t code)
{
	int d = key_digit(code);

	return code == KEY_NUMERIC_STAR ? symbols : d >= 0 ? taps[d] : NULL;
}

static void entry_append(struct ui_entry *e, char c)
{
	size_t len = strlen(e->text);

	if (len + 1 < sizeof(e->text)) {
		e->text[len] = c;
		e->text[len + 1] = 0;
	}
}

static void entry_commit(struct ui_entry *e)
{
	struct itimerspec off = { 0 };

	if (e->pend_key && e->mode == M_CAP && isalpha((unsigned char)
	    e->text[strlen(e->text) - 1]))
		e->mode = M_LOWER;	/* Abc: only the one letter */
	e->pend_key = 0;
	e->reveal = false;
	timerfd_settime(e->timer_fd, 0, &off, NULL);
}

/* A secret entry's newest character stays readable for a moment */
static void entry_reveal(struct ui_entry *e)
{
	struct itimerspec it = { .it_value = { TAP_MS / 1000,
		(TAP_MS % 1000) * 1000000L } };

	if (!e->secret)
		return;
	e->reveal = true;
	timerfd_settime(e->timer_fd, 0, &it, NULL);
}

static void on_entry_timer(struct ui *ui, int fd)
{
	uint64_t n;

	if (read(fd, &n, sizeof(n)) != sizeof(n))
		return;
	for (int i = 0; i < 8; i++)
		if (entries[i] && entries[i]->timer_fd == fd) {
			entry_commit(entries[i]);
			ui->dirty = true;
		}
}

void ui_entry_init(struct ui *ui, struct ui_entry *e, bool digits)
{
	memset(e, 0, sizeof(*e));
	e->ui = ui;
	e->digits = digits;
	e->mode = digits ? M_DIGITS : M_CAP;
	e->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	for (int i = 0; i < 8; i++)
		if (entries[i] == NULL) {
			entries[i] = e;
			break;
		}
	ui_watch(ui, e->timer_fd, on_entry_timer);
}

void ui_entry_set(struct ui_entry *e, const char *text)
{
	entry_commit(e);
	snprintf(e->text, sizeof(e->text), "%s", text);
}

static char cased(struct ui_entry *e, char c)
{
	if (e->mode == M_CAP || e->mode == M_UPPER)
		return toupper((unsigned char)c);
	return c;
}

/* A tap: a new letter, or the next one on the same key */
static void entry_tap(struct ui_entry *e, uint32_t code)
{
	const char *list = tap_list(code);
	int n = strlen(list);
	struct itimerspec it = { .it_value = { TAP_MS / 1000,
		(TAP_MS % 1000) * 1000000L } };

	if (e->pend_key == (int)code) {
		e->pend_idx = (e->pend_idx + 1) % n;
		e->text[strlen(e->text) - 1] = cased(e, list[e->pend_idx]);
	} else {
		entry_commit(e);
		e->pend_key = code;
		e->pend_idx = 0;
		entry_append(e, cased(e, list[0]));
	}
	e->reveal = e->secret;
	timerfd_settime(e->timer_fd, 0, &it, NULL);
}

bool ui_entry_key(struct ui_entry *e, uint32_t code, enum ui_key_state st)
{
	int d = key_digit(code);
	bool tap_key = d >= 0 || code == KEY_NUMERIC_STAR;

	if (code == KEY_BACKSPACE) {
		if (st == UI_RELEASED)
			return true;
		entry_commit(e);
		size_t len = strlen(e->text);
		while (len > 0 && (e->text[len - 1] & 0xc0) == 0x80)
			len--;		/* a whole UTF-8 character */
		if (len > 0)
			len--;
		e->text[len] = 0;
		return true;
	}
	if (code == KEY_NUMERIC_POUND && !e->digits) {
		if (st == UI_PRESSED) {
			entry_commit(e);
			e->mode = (e->mode + 1) % M_COUNT;
		}
		return true;
	}
	if (code == KEY_NUMERIC_POUND) {	/* digits: # is # */
		if (st == UI_PRESSED) {
			entry_commit(e);
			entry_append(e, '#');
			entry_reveal(e);
		}
		return true;
	}
	if (!tap_key)
		return false;
	/* Decided on release: a tap, or held long enough for the digit */
	if (st == UI_PRESSED) {
		e->held_key = code;
		e->held_since = mono_ms();
		return true;
	}
	if (st != UI_RELEASED || e->held_key != (int)code)
		return true;
	bool held = mono_ms() - e->held_since >= HOLD_MS;
	e->held_key = 0;
	if (e->digits) {
		entry_commit(e);
		entry_append(e, code == KEY_NUMERIC_STAR ? (held ? '+' : '*') :
			     '0' + d);
		entry_reveal(e);
	} else if (held || (e->mode == M_DIGITS && d >= 0)) {
		entry_commit(e);
		entry_append(e, code == KEY_NUMERIC_STAR ? '*' : '0' + d);
		entry_reveal(e);
	} else {
		entry_tap(e, code);
	}
	return true;
}

/* What a secret entry shows: an asterisk a character, but the newest */
static const char *entry_shown(struct ui_entry *e, char *buf, size_t size)
{
	size_t len = strlen(e->text), last = len, o = 0;

	if (!e->secret)
		return e->text;
	while (last > 0 && (e->text[last - 1] & 0xc0) == 0x80)
		last--;
	if (last > 0)
		last--;			/* where the newest character starts */
	for (size_t i = 0; i < len && o + 5 < size; i++) {
		if (e->reveal && i >= last)
			buf[o++] = e->text[i];
		else if ((e->text[i] & 0xc0) != 0x80)
			buf[o++] = '*';
	}
	buf[o] = 0;
	return buf;
}

int ui_entry_draw(struct ui_canvas *c, struct ui_entry *e, int x, int y, int w,
		  int size, int max_lines)
{
	int starts[256], ends[256], lh = ui_line_height(size);
	char masked[sizeof(e->text)];
	const char *text = entry_shown(e, masked, sizeof(masked));
	int n = wrap(UI_SANS, size, w - 4, text, starts, ends, 256);
	int first = n > max_lines ? n - max_lines : 0;
	char line[1024];

	if (n == 0) {		/* empty: just the cursor */
		starts[0] = ends[0] = 0;
		n = 1;
	}
	for (int i = first; i < n; i++) {
		int len = ends[i] - starts[i];
		memcpy(line, text + starts[i], len);
		line[len] = 0;
		ui_text(c, x, y + (i - first) * lh, UI_SANS, size, UI_FG, line);
	}
	/* The cursor after the text; the letter being chosen underlined.
	 * Trailing spaces are not in the last line's range, so add them. */
	int last = n - 1, len = strlen(text);
	int ly = y + (last - first) * lh;
	snprintf(line, sizeof(line), "%.*s", len - starts[last],
		 text + starts[last]);
	int lw = ui_text_width(UI_SANS, size, line);
	if (e->pend_key && len > 0) {
		char ch[2] = { text[len - 1], 0 };
		int cw = ui_text_width(UI_SANS, size, ch);
		ui_fill(c, x + lw - cw, ly + lh - 3, cw, 2, UI_ACCENT);
	} else {
		ui_fill(c, x + lw + 1, ly + 3, 2, lh - 4, UI_ACCENT);
	}
	return (n - first) * lh;
}

/*
 * The indicator lives in the status bar for every app (s22-statusbar, as
 * s22-t9d's does for foot): an entry drawn this frame writes its state to
 * $XDG_RUNTIME_DIR/s22-entry in s22-t9d's tap format, the mode and then
 * "KEY<TAB>INDEX<TAB>CHARACTERS" while a key is being tapped.
 */
static void entry_publish(const char *text)
{
	static char last[192] = "\1";
	const char *dir = getenv("XDG_RUNTIME_DIR");
	char path[256], tmp[264];
	FILE *f;

	entry_published = text[0] != 0;
	if (!dir || strcmp(text, last) == 0)
		return;
	snprintf(path, sizeof(path), "%s/s22-entry", dir);
	snprintf(tmp, sizeof(tmp), "%s.%d", path, (int)getpid());
	f = fopen(tmp, "we");
	if (!f)
		return;
	fputs(text, f);
	fclose(f);
	rename(tmp, path);
	snprintf(last, sizeof(last), "%s", text);
}

void ui_entry_indicator(struct ui_canvas *c, struct ui_entry *e, int x, int y,
			int w)
{
	int n = snprintf(entry_frame, sizeof(entry_frame), "%s\n",
			 e->digits ? "123" : mode_names[e->mode]);

	if (e->pend_key) {
		const char *list = tap_list(e->pend_key);
		n += snprintf(entry_frame + n, sizeof(entry_frame) - n, "%c\t%d\t",
			      e->pend_key == KEY_NUMERIC_STAR ? '*' :
			      '0' + key_digit(e->pend_key), e->pend_idx);
		for (int i = 0; list[i] && n < (int)sizeof(entry_frame) - 2; i++)
			entry_frame[n++] = cased(e, list[i]);
		entry_frame[n++] = '\n';
		entry_frame[n] = 0;
	}
}

/* ----------------------------------------------------- notifications --- */

void ui_notify_dismiss(const char *name)
{
	char path[512];

	if (name[0] == '.' || strchr(name, '/'))
		return;
	snprintf(path, sizeof(path), "%s/%s", UI_NOTIFY_DIR, name);
	if (unlink(path) == 0 || errno == ENOENT)
		return;
	/* Not ours: s22-notifyd removes it for group wheel (sticky dir) */
	snprintf(path, sizeof(path), "%s/.dismiss-%s", UI_NOTIFY_DIR, name);
	int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (fd >= 0)
		close(fd);
}

void ui_notify_dismiss_matching(const char *source, const char *prefix)
{
	DIR *d = opendir(UI_NOTIFY_DIR);
	struct dirent *e;
	char names[64][64];
	int n = 0;

	if (d == NULL)
		return;
	while ((e = readdir(d)) != NULL && n < 64) {
		char path[512], line[1024];
		bool src = false, txt = false;
		if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(names[0]))
			continue;
		snprintf(path, sizeof(path), "%s/%s", UI_NOTIFY_DIR, e->d_name);
		FILE *f = fopen(path, "re");
		if (f == NULL)
			continue;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = 0;
			if (strncmp(line, "source=", 7) == 0)
				src = strcmp(line + 7, source) == 0;
			else if (strncmp(line, "text=", 5) == 0)
				txt = strncmp(line + 5, prefix, strlen(prefix)) == 0;
		}
		fclose(f);
		if (src && txt)
			snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
	}
	closedir(d);
	for (int i = 0; i < n; i++)
		ui_notify_dismiss(names[i]);
}

static void entry_takeback(void)
{
	if (entry_published)
		entry_publish("");
}

int ui_main(struct ui_app *app)
{
	struct ui *ui = calloc(1, sizeof(*ui));

	atexit(entry_takeback);
	ui->app = app;
	ui->touch_id = -1;
	ui->w = 480;
	ui->h = 640;
	ui->display = wl_display_connect(NULL);
	if (ui->display == NULL) {
		fprintf(stderr, "%s: no Wayland display\n", app->app_id);
		return 1;
	}
	ui->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	ui->repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	ui->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);

	struct wl_registry *reg = wl_display_get_registry(ui->display);
	wl_registry_add_listener(reg, &registry_listener, ui);
	wl_display_roundtrip(ui->display);
	if (!ui->compositor || !ui->shm || !ui->wm_base ||
	    (app->layer && !ui->layer_shell)) {
		fprintf(stderr, "%s: compositor lacks wl_shm, xdg-shell or layer-shell\n",
			app->app_id);
		return 1;
	}
	if (app->lock) {
		if (!lock_start(ui)) {
			fprintf(stderr, "%s: the compositor cannot lock the session\n",
				app->app_id);
			return 1;
		}
		goto started;
	}
	if (app->layer)
		goto started;
	ui->surface = wl_compositor_create_surface(ui->compositor);
	ui->xdg_surface = xdg_wm_base_get_xdg_surface(ui->wm_base, ui->surface);
	xdg_surface_add_listener(ui->xdg_surface, &xdg_surface_listener, ui);
	ui->xdg_toplevel = xdg_surface_get_toplevel(ui->xdg_surface);
	xdg_toplevel_add_listener(ui->xdg_toplevel, &toplevel_listener, ui);
	xdg_toplevel_set_app_id(ui->xdg_toplevel, app->app_id);
	xdg_toplevel_set_title(ui->xdg_toplevel, app->title);
	wl_surface_commit(ui->surface);
started:
	if (app->start)
		app->start(ui);
	app_socket_listen(ui);
	if (app->uri && app->open && uri_ok(app->uri))
		app->open(ui, app->uri);

	while (!ui->quit) {
		if (ui->surface && ui->configured && ui->dirty && !ui->frame_pending)
			draw(ui);

		struct pollfd pfd[3 + MAX_WATCH];
		int nw = ui->nwatch;
		pfd[0] = (struct pollfd){ wl_display_get_fd(ui->display), POLLIN };
		pfd[1] = (struct pollfd){ ui->repeat_fd, POLLIN };
		pfd[2] = (struct pollfd){ ui->timer_fd, POLLIN };
		for (int i = 0; i < nw; i++)
			pfd[3 + i] = (struct pollfd){ ui->watch[i].fd, POLLIN };

		while (wl_display_prepare_read(ui->display) != 0)
			wl_display_dispatch_pending(ui->display);
		wl_display_flush(ui->display);
		if (poll(pfd, 3 + nw, -1) < 0) {
			wl_display_cancel_read(ui->display);
			if (errno == EINTR)
				continue;
			break;
		}
		if (pfd[0].revents & POLLIN) {
			if (wl_display_read_events(ui->display) < 0)
				break;
		} else {
			wl_display_cancel_read(ui->display);
		}
		if (pfd[0].revents & (POLLERR | POLLHUP))
			break;
		if (wl_display_dispatch_pending(ui->display) < 0)
			break;

		uint64_t n;
		if ((pfd[1].revents & POLLIN) &&
		    read(ui->repeat_fd, &n, sizeof(n)) == sizeof(n) && ui->repeat_key)
			key_deliver(ui, ui->repeat_key, UI_REPEATED);
		if ((pfd[2].revents & POLLIN) &&
		    read(ui->timer_fd, &n, sizeof(n)) == sizeof(n) && app->tick)
			app->tick(ui);
		/* A callback may unwatch; go by fd, not by slot */
		for (int i = 0; i < nw; i++) {
			if (!(pfd[3 + i].revents & (POLLIN | POLLHUP)))
				continue;
			for (int j = 0; j < ui->nwatch; j++)
				if (ui->watch[j].fd == pfd[3 + i].fd) {
					ui->watch[j].cb(ui, pfd[3 + i].fd);
					break;
				}
		}
	}
	wl_display_disconnect(ui->display);
	return 0;
}
