// SPDX-License-Identifier: GPL-2.0-only
/*
 * s22-outerd: a status screen on the Cat S22 Flip's outer 128x128 display.
 *
 * With the lid closed, the screen lights for SHOW_MS (default 10 s) when the
 * lid closes, when the side or a volume key is pressed, and when a
 * notification arrives. It shows the time and date, battery, WiFi and
 * Bluetooth, and a notification, over a wallpaper. Opening the lid turns it
 * off (backlight off, panel blanked).
 *
 * The keys reachable with the lid closed belong to this daemon (D024):
 *   side, short   wake; when lit, the next (older) notification
 *   side, twice   play/pause what plays (lid closed; the media hook)
 *   side, long    dismiss the notification shown
 *   side + volume down  nothing here: s22-wm's flashlight (D066)
 *   volume, short media volume up/down     (lid closed; open, s22-wm's)
 *   volume, long  next/previous track       (lid closed)
 * Media actions run HOOKDIR/media volume-up|volume-down|next|previous
 * (default /etc/s22-outerd.d/media), so a music player can take them over;
 * one at a time in the background, the screen going on meanwhile. The
 * volume shows as the GUI counts it, 0-15 ("Level: N 15" from the hook),
 * the new level at once (D068).
 *
 * Now playing (D054): while the GUI's media plays or is paused (MPRIS, kept
 * in /run/s22-gui/nowplaying by s22-audio's playerctl), its title and
 * artist show under the date, scrolling when long, after a note.
 *
 * Wallpaper: a binary PPM (P6) of any size, scaled to cover the screen, at
 * WALLPAPER (default /var/lib/s22/wallpaper-outer.ppm, which s22-wallpaper
 * makes from the picture chosen in Settings, D066). Several PPMs one after
 * another are an animation, ANIM_MS a frame (up to MAX_FRAMES), played from
 * the start while the screen is lit. It is re-read when the file changes,
 * and on SIGHUP. Without one the background is a dark gradient.
 *
 * Settings: SETTINGS (default /var/lib/s22/settings, shared with the GUI
 * and set in its Settings app), "key=value" lines, re-read whenever the
 * file changes:
 *   clock=12|24      12-hour clock with AM/PM (default), or 24-hour
 *   blink=yes|no     the colon blinks once a second (default yes)
 *   timeout=SECONDS  how long the outer screen stays lit (default: -t, 10 s)
 *   wallpaper_speed=X  a moving wallpaper's speed, 0.25-2 (default 1)
 *
 * Text: ASCII in the console's 8x16 font; anything else (accents, Japanese,
 * Chinese, Korean, ...) drawn by FreeType from the font fontconfig finds
 * for it (Noto's), at FT_PX on the same baseline (D068).
 *
 * Notifications: one file per notification in NOTIFYDIR (default
 * /run/s22-notify, world-writable and sticky); the newest is shown on one
 * line, as a ticker that scrolls when it does not fit. s22-notify writes and
 * clears them ("source=" and "text=" lines; a file without "text=" is shown
 * as it is).
 *
 *   s22-outerd [-v] [-f FBNAME] [-B BACKLIGHT] [-t SHOW_MS] [-w WALLPAPER]
 *              [-n NOTIFYDIR] [-s SETTINGS] [-k HOOKDIR]
 */
#define _GNU_SOURCE		/* pipe2 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include "fonts.h"

#define W	128
#define H	128
#define BIT(arr, b)	((arr)[(b) / 8] & (1 << ((b) % 8)))
#define RGB(r, g, b)	((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

#define WHITE	RGB(255, 255, 255)
#define GREY	RGB(110, 110, 110)
#define BLACK	RGB(0, 0, 0)
#define GREEN	RGB(80, 220, 100)
#define RED	RGB(240, 70, 60)
#define YELLOW	RGB(250, 210, 60)
#define BLUE	RGB(90, 160, 255)

static const char *fb_name = "panel-mipi-dbid";
static const char *bl_name = "ext-backlight";
static int show_ms = 10000;
static const char *wallpaper = "/var/lib/s22/wallpaper-outer.ppm";
static const char *notifydir = "/run/s22-notify";
static const char *settings = "/var/lib/s22/settings";
static const char *hookdir = "/etc/s22-outerd.d";
static bool verbose;

/* From the settings file */
static bool clock12 = true, blink = true;
static int timeout_ms;			/* 0: use show_ms */
static struct timespec settings_mtime = { -1, 0 };
static double wall_speed = 1;
static int settings_volume = -1;	/* the GUI's media volume, 0-15 */

#define dbg(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while (0)

#define ANIM_MS		50	/* 20 frames a second, as s22-wallpaper makes them */
#define MAX_FRAMES	400
static uint16_t frame[W * H], *wall;	/* nwall frames of W * H */
static int nwall;
static bool have_wall;
static struct timespec wall_mtime;
static ino_t wall_ino;
static char fbsys[300];
static int fbfd = -1;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void put(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0 || write(fd, val, strlen(val)) < 0)
		fprintf(stderr, "s22-outerd: %s: %s\n", path, strerror(errno));
	if (fd >= 0)
		close(fd);
}

/* First line of a small sysfs/proc file, newline stripped; "" if missing */
static char *get(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "re");

	buf[0] = 0;
	if (f) {
		if (!fgets(buf, len, f))
			buf[0] = 0;
		fclose(f);
	}
	buf[strcspn(buf, "\n")] = 0;
	return buf;
}

/* --------------------------------------------------------- the panel --- */
static int find_fb(void)
{
	char p[300], name[64];
	struct dirent *de;
	DIR *d = opendir("/sys/class/graphics");

	if (!d)
		return -1;
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "fb", 2) || !de->d_name[2])
			continue;
		snprintf(p, sizeof(p), "/sys/class/graphics/%s/name", de->d_name);
		if (strcmp(get(p, name, sizeof(name)), fb_name))
			continue;
		snprintf(fbsys, sizeof(fbsys), "/sys/class/graphics/%s", de->d_name);
		snprintf(p, sizeof(p), "/dev/%s", de->d_name);
		closedir(d);
		fbfd = open(p, O_WRONLY | O_CLOEXEC);
		return fbfd;
	}
	closedir(d);
	return -1;
}

static bool lit;
static long long lit_since;	/* when the screen last lit: ticker start */
static bool ticking;		/* a ticker is scrolling: redraw often */
static int vol_level = -1, vol_max = 15;	/* the media volume, as the GUI counts it */
static long long volume_until;	/* show it until then */

static void flush(void)
{
	if (pwrite(fbfd, frame, sizeof(frame), 0) < 0)
		fprintf(stderr, "s22-outerd: write: %s\n", strerror(errno));
}

static void backlight(bool on)
{
	char p[128];

	snprintf(p, sizeof(p), "/sys/class/backlight/%s/brightness", bl_name);
	put(p, on ? "1" : "0");
}

static void panel_on(void)
{
	char p[320];

	snprintf(p, sizeof(p), "%s/blank", fbsys);
	put(p, "0");
	lit = true;
	lit_since = now_ms();
}

static void panel_off(void)
{
	char p[320];

	backlight(false);
	snprintf(p, sizeof(p), "%s/blank", fbsys);
	put(p, "4");		/* FB_BLANK_POWERDOWN */
	lit = false;
}

/* --------------------------------------------------------- wallpaper --- */
static int ppm_token(FILE *f)
{
	int c, v = 0;

	do {
		c = fgetc(f);
		if (c == '#')
			while (c != '\n' && c != EOF)
				c = fgetc(f);
	} while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
	if (c < '0' || c > '9')
		return -1;
	while (c >= '0' && c <= '9') {
		v = v * 10 + (c - '0');
		c = fgetc(f);
	}
	return v;		/* the single whitespace after it is consumed */
}

static void load_wallpaper(void)
{
	int w, h, maxval, x, y;
	unsigned char *px = NULL;
	struct stat st;
	FILE *f;

	have_wall = false;
	free(wall);
	wall = NULL;
	nwall = 0;
	if (stat(wallpaper, &st))
		return;
	wall_mtime = st.st_mtim;
	wall_ino = st.st_ino;
	f = fopen(wallpaper, "rbe");
	if (!f)
		return;
	/* One frame after another, each scaled to cover, centred */
	while (nwall < MAX_FRAMES) {
		int c = fgetc(f);
		while (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			c = fgetc(f);
		if (c == EOF)
			break;
		if (c != 'P' || fgetc(f) != '6' ||
		    (w = ppm_token(f)) <= 0 || (h = ppm_token(f)) <= 0 ||
		    (maxval = ppm_token(f)) <= 0 || maxval > 255 || w > 4096 || h > 4096) {
			fprintf(stderr, "s22-outerd: %s: not a binary PPM\n", wallpaper);
			break;
		}
		unsigned char *npx = realloc(px, (size_t)w * h * 3);
		uint16_t *nwl = realloc(wall, (size_t)(nwall + 1) * W * H * sizeof(*wall));
		if (npx)
			px = npx;
		if (nwl)
			wall = nwl;
		if (!npx || !nwl || fread(px, 3, (size_t)w * h, f) != (size_t)w * h) {
			fprintf(stderr, "s22-outerd: %s: short read\n", wallpaper);
			break;
		}
		double s = (double)w / W < (double)h / H ? (double)w / W : (double)h / H;
		double ox = (w - W * s) / 2, oy = (h - H * s) / 2;
		uint16_t *out = wall + (size_t)nwall * W * H;
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++) {
				int sx = (int)(ox + (x + 0.5) * s), sy = (int)(oy + (y + 0.5) * s);
				unsigned char *p = px + ((size_t)sy * w + sx) * 3;

				out[y * W + x] = RGB(p[0] * 255 / maxval, p[1] * 255 / maxval,
						     p[2] * 255 / maxval);
			}
		nwall++;
	}
	free(px);
	fclose(f);
	have_wall = nwall > 0;
	dbg("wallpaper %s: %d frame(s)\n", wallpaper, nwall);
}

static void background(void)
{
	struct stat st;
	int y;

	if (stat(wallpaper, &st) == 0 ? st.st_mtim.tv_sec != wall_mtime.tv_sec ||
					st.st_mtim.tv_nsec != wall_mtime.tv_nsec ||
					st.st_ino != wall_ino || !have_wall
				      : have_wall)
		load_wallpaper();
	if (have_wall) {
		long long el = now_ms() - lit_since;
		int i = nwall > 1 && el > 0 ? (int)((long long)(el * wall_speed) / ANIM_MS % nwall) : 0;

		memcpy(frame, wall + (size_t)i * W * H, sizeof(frame));
		return;
	}
	for (y = 0; y < H; y++) {	/* dark blue to black */
		uint16_t c = RGB(12 * (H - y) / H, 18 * (H - y) / H, 40 * (H - y) / H);
		int x;

		for (x = 0; x < W; x++)
			frame[y * W + x] = c;
	}
}

/* ---------------------------------------------------------- settings --- */
static bool yes(const char *v)
{
	return !strcmp(v, "yes") || !strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "true");
}

/* Re-read the settings file if it changed (or appeared, or went away) */
static void load_settings(void)
{
	struct stat st;
	char line[128];
	FILE *f;

	if (stat(settings, &st))
		st.st_mtim = (struct timespec){ 0, 0 };
	/* To the nanosecond: Settings may write twice in a second */
	if (st.st_mtim.tv_sec == settings_mtime.tv_sec &&
	    st.st_mtim.tv_nsec == settings_mtime.tv_nsec)
		return;
	settings_mtime = st.st_mtim;
	clock12 = true;
	blink = true;
	timeout_ms = 0;
	wall_speed = 1;
	f = fopen(settings, "re");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *k = line, *v = strchr(line, '=');

		while (*k == ' ' || *k == '\t')
			k++;
		if (*k == '#' || !v)
			continue;
		*v++ = 0;
		k[strcspn(k, " \t")] = 0;
		while (*v == ' ' || *v == '\t')
			v++;
		v[strcspn(v, " \t\r\n#")] = 0;
		if (!strcmp(k, "clock"))
			clock12 = atoi(v) != 24;
		else if (!strcmp(k, "blink"))
			blink = yes(v);
		else if (!strcmp(k, "timeout") && atoi(v) > 0)
			timeout_ms = atoi(v) * 1000;
		else if (!strcmp(k, "wallpaper_speed") && atof(v) >= 0.1 && atof(v) <= 4)
			wall_speed = atof(v);
		else if (!strcmp(k, "volume") && atoi(v) >= 0 && atoi(v) <= 15)
			settings_volume = atoi(v);
	}
	fclose(f);
	dbg("settings: clock %d, blink %d, timeout %d ms\n", clock12 ? 12 : 24,
	    blink, timeout_ms);
}

static int lit_ms(void)
{
	load_settings();
	return timeout_ms ? timeout_ms : show_ms;
}

/* ----------------------------------------------------------- drawing --- */
static int clip_r = W;		/* pixels at x >= clip_r are not drawn */
static int clip_l;		/* ... nor at x < clip_l */

static void pixel(int x, int y, uint16_t c)
{
	if (x >= clip_l && x < clip_r && y >= 0 && y < H)
		frame[y * W + x] = c;
}

static void rect(int x, int y, int w, int h, uint16_t c)
{
	int i, j;

	for (j = y; j < y + h; j++)
		for (i = x; i < x + w; i++)
			pixel(i, j, c);
}

/* Halve the brightness of an area, so text over a wallpaper stays legible */
static void dim(int x, int y, int w, int h)
{
	int i, j;

	for (j = y; j < y + h && j < H; j++)
		for (i = x; i < x + w && i < W; i++)
			if (i >= 0 && j >= 0)
				frame[j * W + i] = (frame[j * W + i] >> 1) & 0x7bef;
}

/* 8x16 glyph, each pixel drawn as a k x k block */
static void glyph8(int x, int y, unsigned char ch, int k, uint16_t c)
{
	int r, b;

	for (r = 0; r < 16; r++)
		for (b = 0; b < 8; b++)
			if (font8x16[ch * 16 + r] & (0x80 >> b))
				rect(x + b * k, y + r * k, k, k, c);
}

/* --------------------------------------------- text beyond ASCII --- */
#define FT_PX		14	/* fits the 16-pixel line, CJK included */
#define FT_BASE		12	/* the 8x16 font's baseline row */

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
	else { *s += 1; return '?'; }
	for (int i = 1; i < n; i++) {
		if ((p[i] & 0xc0) != 0x80) { *s += i; return '?'; }
		cp = cp << 6 | (p[i] & 0x3f);
	}
	*s += n;
	return cp;
}

struct ftglyph {
	struct ftglyph *next;
	uint32_t cp;
	int w, h, left, top, advance;
	uint8_t *alpha;
};
static struct ftglyph *ftglyphs[256];
static FT_Library ftlib;

/* The face fontconfig finds for cp (Japanese first for shared Han), opened
 * once per file; NULL if none */
static FT_Face ft_face(uint32_t cp)
{
	static struct { char file[256]; int index; FT_Face face; } faces[16];
	static int nfaces;
	static bool tried, ok;
	FcChar8 *file;
	FcCharSet *has;
	int index = 0;
	FT_Face found = NULL;

	if (!tried) {
		tried = true;
		ok = FcInit() && FT_Init_FreeType(&ftlib) == 0;
	}
	if (!ok)
		return NULL;
	FcPattern *p = FcPatternCreate();
	FcCharSet *cs = FcCharSetCreate();
	FcLangSet *ls = FcLangSetCreate();
	FcCharSetAddChar(cs, cp);
	FcLangSetAdd(ls, (const FcChar8 *)"ja");
	FcPatternAddCharSet(p, FC_CHARSET, cs);
	FcPatternAddLangSet(p, FC_LANG, ls);
	/* DejaVu's look where it has it; the Japanese face of Noto CJK for
	 * Han, kana and Hangul (alone, lang=ja picked its Korean one) */
	FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)"DejaVu Sans");
	FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)"Noto Sans CJK JP");
	FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)"sans-serif");
	FcConfigSubstitute(NULL, p, FcMatchPattern);
	FcDefaultSubstitute(p);
	FcResult r;
	FcPattern *m = FcFontMatch(NULL, p, &r);
	if (m && FcPatternGetCharSet(m, FC_CHARSET, 0, &has) == FcResultMatch &&
	    FcCharSetHasChar(has, cp) &&
	    FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) {
		FcPatternGetInteger(m, FC_INDEX, 0, &index);
		for (int i = 0; i < nfaces && !found; i++)
			if (faces[i].index == index && !strcmp(faces[i].file, (const char *)file))
				found = faces[i].face;
		if (!found && nfaces < 16 &&
		    FT_New_Face(ftlib, (const char *)file, index, &found) == 0) {
			FT_Set_Pixel_Sizes(found, 0, FT_PX);
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

/* A character beyond ASCII, rendered once; a '?' box's width if none */
static struct ftglyph *ft_glyph(uint32_t cp)
{
	unsigned slot = cp % 256;
	struct ftglyph *g;

	for (g = ftglyphs[slot]; g; g = g->next)
		if (g->cp == cp)
			return g;
	g = calloc(1, sizeof(*g));
	if (!g)
		return NULL;
	g->cp = cp;
	g->advance = 8;
	FT_Face f = ft_face(cp);
	if (f && FT_Load_Char(f, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
		FT_GlyphSlot gs = f->glyph;
		g->w = gs->bitmap.width;
		g->h = gs->bitmap.rows;
		g->left = gs->bitmap_left;
		g->top = gs->bitmap_top;
		g->advance = (gs->advance.x + 32) >> 6;
		g->alpha = malloc((size_t)g->w * g->h + 1);
		for (int y = 0; g->alpha && y < g->h; y++)
			memcpy(g->alpha + y * g->w, gs->bitmap.buffer + y * gs->bitmap.pitch, g->w);
	}
	g->next = ftglyphs[slot];
	ftglyphs[slot] = g;
	return g;
}

/* c blended over the frame's pixel by a (of 255) */
static void blend(int x, int y, uint16_t c, int a)
{
	if (x < clip_l || x >= clip_r || y < 0 || y >= H || a <= 0)
		return;
	uint16_t d = frame[y * W + x];
	int r = ((c >> 11) * a + (d >> 11) * (255 - a)) / 255;
	int g = (((c >> 5) & 63) * a + ((d >> 5) & 63) * (255 - a)) / 255;
	int b = ((c & 31) * a + (d & 31) * (255 - a)) / 255;
	frame[y * W + x] = (uint16_t)(r << 11 | g << 5 | b);
}

static void ft_draw(int x, int y, const struct ftglyph *g, uint16_t c)
{
	if (!g->alpha) {			/* no font has it: a box */
		for (int i = 0; i < 6; i++) {
			blend(x + 1 + i, y + 3, c, 255);
			blend(x + 1 + i, y + 12, c, 255);
		}
		for (int i = 3; i <= 12; i++) {
			blend(x + 1, y + i, c, 255);
			blend(x + 6, y + i, c, 255);
		}
		return;
	}
	for (int r = 0; r < g->h; r++)
		for (int b = 0; b < g->w; b++)
			blend(x + g->left + b, y + FT_BASE - g->top + r, c, g->alpha[r * g->w + b]);
}

/* How wide text8 draws s */
static int text_width(const char *s)
{
	int w = 0;

	while (*s) {
		uint32_t cp = utf8_next(&s);
		if (cp < 0x80) {
			w += 8;
		} else {
			struct ftglyph *g = ft_glyph(cp);
			w += g ? g->advance : 8;
		}
	}
	return w;
}

/* Text with a one-pixel shadow: ASCII in the 8x16 font, the rest by
 * FreeType */
static int text8(int x, int y, const char *s, uint16_t c)
{
	for (int pass = 0; pass < 2; pass++) {
		const char *p = s;
		int cx = x + (pass ? 0 : 1), cy = y + (pass ? 0 : 1);
		uint16_t col = pass ? c : BLACK;

		while (*p) {
			uint32_t cp = utf8_next(&p);
			if (cp < 0x80) {
				glyph8(cx, cy, (unsigned char)cp, 1, col);
				cx += 8;
			} else {
				struct ftglyph *g = ft_glyph(cp);
				if (g) {
					ft_draw(cx, cy, g, col);
					cx += g->advance;
				}
			}
		}
		if (pass)
			x = cx;
	}
	return x;
}

/*
 * WiFi: a dot and three arcs, 15x12 px. The digit is the segment, 0 (dot) to
 * 3 (outer arc); the first "lit" segments are white, the rest grey.
 */
static const char *wifi_px[] = {
	"......333......",
	"...333333333...",
	".333.......333.",
	"33...........33",
	"3...2222222...3",
	"..222.....222..",
	"..2.........2..",
	".....11111.....",
	"....11...11....",
	"...............",
	".......0.......",
	"......000......",
};

static void wifi_icon(int x0, int y0, int lit)
{
	int x, y;

	for (y = 0; y < 12; y++)
		for (x = 0; x < 15; x++)
			if (wifi_px[y][x] != '.')
				pixel(x0 + x, y0 + y, wifi_px[y][x] - '0' < lit ? WHITE : GREY);
}

/* Cellular signal bars (0-4) */
static void signal_bars(int x0, int bars)
{
	int i;

	for (i = 0; i < 4; i++)
		rect(x0 + i * 4, 14 - 3 * (i + 1), 3, 3 * (i + 1), i < bars ? WHITE : GREY);
}

/*
 * Cellular bars from s22-celld's state: 0-4 when registered, 0 (all grey)
 * in limited or no service, -1 without s22-celld (no bars drawn).
 */
static int cell_bars(void)
{
	char line[64];
	int bars = 0;
	bool full = false;
	FILE *f = fopen("/run/s22-cell/state", "re");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (!strcmp(line, "service=full\n"))
			full = true;
		sscanf(line, "bars=%d", &bars);
	}
	fclose(f);
	return full ? bars : 0;
}

static const char *bt_rune[] = {
	"...#...", "...##..", "#..#.#.", ".#.#..#", "..##.#.", "...##..",
	"..##.#.", ".#.#..#", "#..#.#.", "...##..", "...#...",
};

/* ------------------------------------------------------------ status --- */
static int battery(bool *charging)
{
	char p[300], buf[32];
	struct dirent *de;
	DIR *d = opendir("/sys/class/power_supply");
	int cap = -1;

	*charging = false;
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p), "/sys/class/power_supply/%s/type", de->d_name);
		get(p, buf, sizeof(buf));
		if (!strcmp(buf, "Battery")) {
			snprintf(p, sizeof(p), "/sys/class/power_supply/%s/capacity", de->d_name);
			if (*get(p, buf, sizeof(buf)))
				cap = atoi(buf);
			snprintf(p, sizeof(p), "/sys/class/power_supply/%s/status", de->d_name);
			if (!strcmp(get(p, buf, sizeof(buf)), "Charging"))
				*charging = true;
		} else {
			snprintf(p, sizeof(p), "/sys/class/power_supply/%s/online", de->d_name);
			if (!strcmp(get(p, buf, sizeof(buf)), "1"))
				*charging = true;	/* plugged in */
		}
	}
	closedir(d);
	/* s22-batteryd's smoothed capacity, when it runs: the voltage-mode
	 * gauge's own value jumps with load and charging */
	if (*get("/run/s22-battery/capacity", buf, sizeof(buf)))
		cap = atoi(buf);
	return cap;
}

/*
 * WiFi bars 0-4, -1 when not associated, -2 without a WiFi interface. The
 * signal comes from nl80211 through "iw dev IF link" (there is no
 * /proc/net/wireless without wireless extensions), at most every 5 s.
 */
static int wifi_bars(void)
{
	static long long checked;
	static int bars = -2;
	char line[256], cmd[128], ifname[64] = "";
	struct dirent *de;
	DIR *d;
	FILE *f;

	if (checked && now_ms() - checked < 5000)
		return bars;
	checked = now_ms();
	d = opendir("/sys/class/net");
	if (d) {
		while ((de = readdir(d)))
			if (!strncmp(de->d_name, "wlan", 4)) {
				snprintf(ifname, sizeof(ifname), "%.32s", de->d_name);
				break;
			}
		closedir(d);
	}
	bars = -2;
	if (!ifname[0])
		return bars;
	bars = -1;
	snprintf(cmd, sizeof(cmd), "iw dev %s link 2>/dev/null", ifname);
	f = popen(cmd, "re");
	if (!f)
		return bars;
	while (fgets(line, sizeof(line), f)) {
		int level;

		if (sscanf(line, " signal: %d dBm", &level) == 1)
			bars = level >= -55 ? 4 : level >= -65 ? 3 : level >= -75 ? 2 :
			       level >= -85 ? 1 : 0;
	}
	pclose(f);
	return bars;
}

/* A Bluetooth ACL link shows up as hciN:HANDLE */
static bool bt_link(void)
{
	struct dirent *de;
	DIR *d = opendir("/sys/class/bluetooth");
	bool on = false;

	if (!d)
		return false;
	while ((de = readdir(d)))
		if (!strncmp(de->d_name, "hci", 3) && strchr(de->d_name, ':'))
			on = true;
	closedir(d);
	return on;
}

/* Digits in 3x5 pixels, drawn twice the size inside the battery */
static const char digits3x5[10][5][4] = {
	{ "###", "#.#", "#.#", "#.#", "###" }, { ".#.", "##.", ".#.", ".#.", "###" },
	{ "###", "..#", "###", "#..", "###" }, { "###", "..#", "###", "..#", "###" },
	{ "#.#", "#.#", "###", "..#", "..#" }, { "###", "#..", "###", "..#", "###" },
	{ "###", "#..", "###", "#.#", "###" }, { "###", "..#", "..#", "..#", "..#" },
	{ "###", "#.#", "###", "#.#", "###" }, { "###", "#.#", "###", "..#", "###" },
};

/* The battery, its charge as a fill and as a number inside it: black on
 * the fill, white beside it */
static void battery_icon(int cap, bool charging)
{
	const int bx = 96, by = 3, bw = 30, bh = 13;	/* the outline */
	char num[8];
	int fill = (cap * (bw - 2) + 50) / 100, n, x0, y0 = by + 2;

	rect(bx, by, bw, bh, WHITE);
	rect(bx + 1, by + 1, bw - 2, bh - 2, BLACK);
	rect(bx + bw, by + 3, 2, bh - 6, WHITE);		/* nub */
	rect(bx + 1, by + 1, fill, bh - 2, charging ? YELLOW : cap <= 20 ? RED : GREEN);
	n = snprintf(num, sizeof(num), "%d", cap);
	x0 = bx + 1 + (bw - 2 - (n * 8 - 2)) / 2;
	for (int i = 0; i < n; i++)
		for (int r = 0; r < 5; r++)
			for (int col = 0; col < 3; col++) {
				if (digits3x5[num[i] - '0'][r][col] != '#')
					continue;
				int px = x0 + i * 8 + col * 2, py = y0 + r * 2;
				uint16_t c = px < bx + 1 + fill ? BLACK : WHITE;
				rect(px, py, 2, 2, c);
			}
}

/* Vibrate only and silent (D041, D042): a phone buzzing, a speaker struck */
static const char *vib_icon[] = {
	"..#####..",
	"..#...#..",
	"#.#...#.#",
	".##...##.",
	"#.#...#.#",
	".##...##.",
	"#.#...#.#",
	"..#...#..",
	"..#####..",
};
static const char *silent_icon[] = {
	"....#.....",
	"...##.....",
	"####.#...#",
	"####..#.#.",
	"####...#..",
	"####..#.#.",
	"####.#...#",
	"...##.....",
	"....#.....",
};

/* 0 sounds on, 1 vibrate only, 2 silent: the phone's settings */
static int quiet_mode(void)
{
	char line[128];
	bool sounds = true, vib = true;
	FILE *f = fopen("/var/lib/s22/settings", "re");

	while (f && fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "sounds=off", 10))
			sounds = false;
		else if (!strncmp(line, "sounds=on", 9))
			sounds = true;
		else if (!strncmp(line, "ring_volume=", 12) && atoi(line + 12) == 0)
			sounds = false;
		else if (!strncmp(line, "vibrate=off", 11))
			vib = false;
		else if (!strncmp(line, "vibrate=on", 10))
			vib = true;
	}
	if (f)
		fclose(f);
	return sounds ? 0 : vib ? 1 : 2;
}

/* -1 off, 0 on, 1 a device connected: s22-btd's state, else the links */
static int bt_state(void)
{
	char line[64];
	int powered = -1, connected = 0;
	FILE *f = fopen("/run/s22-bt/state", "re");

	if (!f)
		return bt_link() ? 1 : -1;
	while (fgets(line, sizeof(line), f)) {
		sscanf(line, "powered=%d", &powered);
		sscanf(line, "connected=%d", &connected);
	}
	fclose(f);
	return powered != 1 ? -1 : connected > 0 ? 1 : 0;
}

/* Notifications, newest first; cursor = the one shown (0 = newest) */
#define MAX_NOTES 64
struct note { char name[256]; time_t mtime; };
static int cursor;
static bool shown_wrap;		/* the side key stepped past the oldest: wrap */
static char shown[256];		/* the file of the notification on screen */

static int by_age(const void *a, const void *b)
{
	const struct note *x = a, *y = b;

	if (x->mtime != y->mtime)
		return x->mtime < y->mtime ? 1 : -1;
	return strcmp(y->name, x->name);
}

static int list_notes(struct note *list)
{
	char p[512];
	struct dirent *de;
	struct stat st;
	DIR *d = opendir(notifydir);
	int n = 0;

	if (!d)
		return 0;
	while ((de = readdir(d)) && n < MAX_NOTES) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p), "%s/%s", notifydir, de->d_name);
		if (stat(p, &st) || !S_ISREG(st.st_mode))
			continue;
		snprintf(list[n].name, sizeof(list[n].name), "%s", de->d_name);
		list[n++].mtime = st.st_mtime;
	}
	closedir(d);
	qsort(list, n, sizeof(*list), by_age);
	return n;
}

/* The notification at the cursor: its text (one line); returns how many */
static int notification(char *text, size_t len)
{
	struct note list[MAX_NOTES];
	char p[512];
	FILE *f;
	size_t r = 0;
	int n = list_notes(list);

	text[0] = 0;
	shown[0] = 0;
	if (!n) {
		cursor = 0;
		return 0;
	}
	if (cursor >= n)
		cursor = shown_wrap ? 0 : n - 1;
	shown_wrap = false;
	memcpy(shown, list[cursor].name, sizeof(shown));	/* same size */
	snprintf(p, sizeof(p), "%s/%s", notifydir, shown);
	f = fopen(p, "re");
	if (f) {
		r = fread(text, 1, len - 1, f);
		fclose(f);
	}
	text[r] = 0;
	/* s22-notify's format: "key=value" lines, the text in "text=" */
	{
		char *t = !strncmp(text, "text=", 5) ? text : strstr(text, "\ntext=");

		if (t) {
			t += *t == '\n' ? 6 : 5;
			memmove(text, t, strlen(t) + 1);
			text[strcspn(text, "\n")] = 0;
		}
	}
	for (char *c = text; *c; c++)	/* one line of plain characters */
		if ((unsigned char)*c < ' ')
			*c = ' ';
	for (size_t l = strlen(text); l && text[l - 1] == ' '; l--)
		text[l - 1] = 0;
	return n;
}

static void dismiss_shown(void)
{
	char p[512];

	if (!shown[0])
		return;
	snprintf(p, sizeof(p), "%s/%s", notifydir, shown);
	if (unlink(p))
		fprintf(stderr, "s22-outerd: dismiss %s: %s\n", shown, strerror(errno));
	dbg("dismissed %s\n", shown);
	shown[0] = 0;		/* the cursor now points at the next one */
}

/* ------------------------------------------------------------ screen --- */
#define NOWPLAYING	"/run/s22-gui/nowplaying"

/*
 * What plays: NOWPLAYING, "STATUS<TAB>ARTIST<TAB>TITLE" (the GUI session's
 * playerctl, s22-audio). "Title - Artist" into out, in UTF-8 (text8 draws
 * it). False with nothing playing or paused.
 */
static bool now_playing(char *out, int len, bool *paused)
{
	char buf[512], t[512];
	FILE *f = fopen(NOWPLAYING, "re");

	if (f == NULL)
		return false;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return false;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	char *artist = strchr(buf, '\t');
	if (artist == NULL)
		return false;
	*artist++ = 0;
	char *title = strchr(artist, '\t');
	if (title == NULL || !title[1])
		return false;
	*title++ = 0;
	if (strcmp(buf, "Playing") && strcmp(buf, "Paused"))
		return false;
	*paused = strcmp(buf, "Paused") == 0;
	snprintf(t, sizeof(t), *artist ? "%s - %s" : "%s", title, artist);
	int o = 0;
	for (unsigned char *p = (unsigned char *)t; *p && o < len - 1; p++)
		if (*p >= 0x20 && *p != 0x7f)	/* UTF-8 as it is (text8) */
			out[o++] = (char)*p;
	while (o > 0 && ((unsigned char)out[o - 1] & 0xc0) == 0x80 &&
	       o == len - 1)		/* not a character cut in half */
		o--;
	out[o] = 0;
	return o > 0;
}

static void draw(void)
{
	char buf[64], note[512];
	time_t t = time(NULL);
	struct tm tm;
	bool charging;
	int cap, bars, i, n, x, w;

	localtime_r(&t, &tm);
	background();

	/* Status bar */
	dim(0, 0, W, 18);
	bars = wifi_bars();	/* all grey when not connected */
	if (bars >= -1)
		wifi_icon(2, 3, bars > 0 ? bars : 0);	/* bars 1-4: dot + arcs */
	bars = cell_bars();
	if (bars >= 0)
		signal_bars(20, bars);
	n = bt_state();		/* grey when on, blue with a device */
	if (n >= 0)
		for (i = 0; i < 11; i++)
			for (x = 0; x < 7; x++)
				if (bt_rune[i][x] == '#')
					pixel(40 + x, 3 + i, n ? BLUE : GREY);
	n = quiet_mode();
	if (n)
		for (i = 0; i < 9; i++)
			for (x = 0; x < 10; x++) {
				const char *row = n == 1 ? vib_icon[i] : silent_icon[i];
				if (x < (int)strlen(row) && row[x] == '#')
					pixel(52 + x, 4 + i, WHITE);
			}
	cap = battery(&charging);
	if (cap >= 0) {
		battery_icon(cap > 100 ? 100 : cap, charging);
	}

	/* Clock, the 8x16 font at 2x: 12-hour without a leading zero and a
	 * small AM/PM after it, or 24-hour; the colon blinks once a second */
	load_settings();
	if (clock12) {
		int h = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;

		snprintf(buf, sizeof(buf), "%d:%02d", h, tm.tm_min);
		w = (int)strlen(buf) * 16 + 4 + 16;
	} else {
		strftime(buf, sizeof(buf), "%H:%M", &tm);
		w = (int)strlen(buf) * 16;
	}
	x = (W - w) / 2;
	for (i = 0; buf[i]; i++)
		if (buf[i] != ':' || !blink || tm.tm_sec % 2 == 0) {
			glyph8(x + i * 16 + 1, 27, (unsigned char)buf[i], 2, BLACK);
			glyph8(x + i * 16, 26, (unsigned char)buf[i], 2, WHITE);
		}
	/* Same baseline: digits and capitals have ink in rows 2-11 of a cell,
	 * so the 2x clock's last ink row is 26 + 2 * 11 + 1 = 49 = 38 + 11 */
	if (clock12)
		text8(x + (int)strlen(buf) * 16 + 4, 38, tm.tm_hour < 12 ? "AM" : "PM", WHITE);
	/* The date under the clock, its ink (rows 2-11 of the cell, 52-61)
	 * 2 px below the clock's, shadowed like it */
	strftime(buf, sizeof(buf), "%a %d %b %Y", &tm);
	text8((W - text_width(buf)) / 2, 50, buf, WHITE);
	/* What plays (the GUI's media, through MPRIS) under it, 2 px down,
	 * scrolling when it does not fit */
	char np[400];
	bool paused, np_ticking = false;
	if (now_playing(np, sizeof(np), &paused)) {
		int tw = text_width(np);
		uint16_t c = paused ? GREY : WHITE;

		dim(0, 66, W, 18);
		glyph8(2, 67, 0x0e, 1, paused ? GREY : BLUE);	/* the console font's ♫ */
		clip_l = 14;
		if (tw <= W - 16) {
			text8(14, 67, np, c);
		} else {
			long long el = now_ms() - lit_since - 1000;
			int period = tw + 32, off = el > 0 ? (int)(el * 40 / 1000 % period) : 0;

			text8(14 - off, 67, np, c);
			text8(14 - off + period, 67, np, c);
			np_ticking = true;
		}
		clip_l = 0;
	}

	/* A volume change: a bar in the notification strip for 2 s */
	ticking = np_ticking;
	if (vol_level >= 0 && now_ms() < volume_until) {
		int bw = 72 * vol_level / (vol_max > 0 ? vol_max : 15);

		dim(0, 96, W, 20);
		text8(2, 98, "Vol", WHITE);
		rect(30, 101, 74, 10, GREY);
		rect(31, 102, 72, 8, BLACK);
		rect(31, 102, bw > 72 ? 72 : bw, 8, WHITE);
		snprintf(buf, sizeof(buf), "%d", vol_level);
		text8(W - 2 - (int)strlen(buf) * 8, 98, buf, WHITE);
		flush();
		return;
	}

	/* Notification at the cursor on one line, scrolling (ticker) when it
	 * does not fit; "2/3" at the right when there are several */
	n = notification(note, sizeof(note));
	if (n) {
		int right = W, tw = text_width(note);

		dim(0, 96, W, 20);
		if (n > 1) {
			snprintf(buf, sizeof(buf), "%d/%d", cursor + 1, n);
			right = W - (int)strlen(buf) * 8 - 2;
			text8(right + 1, 98, buf, YELLOW);
		}
		clip_r = right;
		if (tw <= right - 4) {
			text8(2, 98, note, WHITE);
		} else {
			/* 1 s still, then 40 px/s, looping with a 32 px gap */
			long long el = now_ms() - lit_since - 1000;
			int period = tw + 32, off = el > 0 ? (int)(el * 40 / 1000 % period) : 0;

			text8(2 - off, 98, note, WHITE);
			text8(2 - off + period, 98, note, WHITE);
			ticking = true;
		}
		clip_r = W;
	}
	flush();
}

/* ------------------------------------------------------------ inputs --- */
#define MAX_INPUTS 16
static struct { int fd; char path[300]; } inputs[MAX_INPUTS];
static int ninputs;
static bool lid_closed;

/* The keys reachable with the lid closed (power/End is on the keypad) */
static bool wake_key(int code)
{
	return code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN || code == KEY_NUMERIC_B;
}

/*
 * Run HOOKDIR/media ACTION. A hook that prints "Volume: 0.55" (wpctl
 * get-volume's format) gets that shown on the outer display.
 */

static const char *media_queue[8];	/* waiting their turn */
static int nmedia;
static int media_fd = -1;		/* the hook running: its output */
static pid_t media_pid;
static char media_out[256];
static size_t media_len;

static void media_next(void)
{
	char p[300];
	int fds[2];

	if (media_fd >= 0 || nmedia == 0)
		return;
	const char *action = media_queue[0];
	memmove(media_queue, media_queue + 1, --nmedia * sizeof(media_queue[0]));
	snprintf(p, sizeof(p), "%s/media", hookdir);
	if (access(p, X_OK) || pipe2(fds, O_CLOEXEC))
		return;
	media_pid = fork();
	if (media_pid == 0) {
		dup2(fds[1], 1);
		execl(p, p, action, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	if (media_pid < 0) {
		close(fds[0]);
		return;
	}
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	media_fd = fds[0];
	media_len = 0;
}

static void media(const char *action)
{
	dbg("media %s\n", action);
	if (nmedia < (int)(sizeof(media_queue) / sizeof(media_queue[0])))
		media_queue[nmedia++] = action;
	media_next();
}

/* The hook's output; at its end, what it said, and the next one */
static void media_read(void)
{
	ssize_t r;

	while ((r = read(media_fd, media_out + media_len,
			 sizeof(media_out) - 1 - media_len)) > 0)
		media_len += r;
	if (r < 0 && errno == EAGAIN && media_len < sizeof(media_out) - 1)
		return;
	media_out[media_len] = 0;
	close(media_fd);
	media_fd = -1;
	waitpid(media_pid, NULL, 0);
	for (char *line = strtok(media_out, "\n"); line; line = strtok(NULL, "\n")) {
		int a, b;
		float v;

		if (sscanf(line, "Level: %d %d", &a, &b) == 2 && b > 0) {
			if (nmedia == 0)	/* else more steps are on their way */
				vol_level = a;
			vol_max = b;
			volume_until = now_ms() + 2000;
		} else if (sscanf(line, "Volume: %f", &v) == 1) {
			vol_max = 15;
			vol_level = (int)(v * 15 + 0.5);
			volume_until = now_ms() + 2000;
		}
	}
	media_next();
}

/* A volume key: the new level shown at once, the hook setting it */
static void volume_step(int dir)
{
	if (vol_level < 0 || (media_fd < 0 && nmedia == 0))
		vol_level = settings_volume;	/* nothing on its way: the file's */
	if (vol_level >= 0) {
		vol_level += dir;
		vol_level = vol_level < 0 ? 0 : vol_level > vol_max ? vol_max : vol_level;
		volume_until = now_ms() + 2000;
	}
	media(dir > 0 ? "volume-up" : "volume-down");
}

#define LONG_MS 600
static int held;		/* key being held, 0 for none */
static bool side_down;		/* the side key is down */
static long long held_at;
static bool held_long;		/* its long action already ran */
/* With the lid closed, a side tap waits DOUBLE_MS for a second: two are
 * play/pause (the media hook), one does what a tap does */
#define DOUBLE_MS 350
static long long side_tap;	/* the first tap's time, 0 for none */
static bool side_tap_lit;	/* the screen was lit at that tap */

static void scan_inputs(void)
{
	unsigned char keys[KEY_MAX / 8 + 1], sw[SW_MAX / 8 + 1];
	char p[300];
	struct dirent *de;
	DIR *d = opendir("/dev/input");
	int i, fd;

	if (!d)
		return;
	while ((de = readdir(d)) && ninputs < MAX_INPUTS) {
		bool want = false, known = false;

		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(p, sizeof(p), "/dev/input/%s", de->d_name);
		for (i = 0; i < ninputs; i++)
			if (!strcmp(inputs[i].path, p))
				known = true;
		if (known)
			continue;
		fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(keys, 0, sizeof(keys));
		memset(sw, 0, sizeof(sw));
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
		ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sw)), sw);
		if (BIT(sw, SW_LID)) {
			unsigned char state[SW_MAX / 8 + 1] = { 0 };

			ioctl(fd, EVIOCGSW(sizeof(state)), state);
			lid_closed = BIT(state, SW_LID);
			want = true;
		}
		for (i = 0; i <= KEY_MAX && !want; i++)
			if (BIT(keys, i) && wake_key(i))
				want = true;
		if (!want) {
			close(fd);
			continue;
		}
		inputs[ninputs].fd = fd;
		snprintf(inputs[ninputs].path, sizeof(inputs[ninputs].path), "%s", p);
		ninputs++;
		dbg("input %s\n", p);
	}
	closedir(d);
}

static void drop_input(int i)
{
	close(inputs[i].fd);
	inputs[i] = inputs[--ninputs];
}

/* -------------------------------------------------------------- main --- */
static volatile sig_atomic_t quit, reload;
static void on_term(int sig) { (void)sig; quit = 1; }
static void on_hup(int sig) { (void)sig; reload = 1; }

int main(int argc, char **argv)
{
	struct pollfd pfd[MAX_INPUTS + 3];
	long long show_until = 0;
	int opt, i, in_notify, in_dev;

	while ((opt = getopt(argc, argv, "vf:B:t:w:n:s:k:")) != -1) {
		switch (opt) {
		case 'v': verbose = true; break;
		case 'f': fb_name = optarg; break;
		case 'B': bl_name = optarg; break;
		case 't': show_ms = atoi(optarg); break;
		case 'w': wallpaper = optarg; break;
		case 'n': notifydir = optarg; break;
		case 's': settings = optarg; break;
		case 'k': hookdir = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-v] [-f FBNAME] [-B BACKLIGHT] [-t SHOW_MS] "
				"[-w WALLPAPER] [-n NOTIFYDIR] [-s SETTINGS] [-k HOOKDIR]\n", argv[0]);
			return 2;
		}
	}
	for (i = 0; i < 40 && find_fb() < 0; i++)
		usleep(250000);
	if (fbfd < 0) {
		fprintf(stderr, "s22-outerd: no framebuffer named %s\n", fb_name);
		return 1;
	}
	mkdir(notifydir, 01777);
	chmod(notifydir, 01777);
	in_notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	inotify_add_watch(in_notify, notifydir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE);
	in_dev = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	inotify_add_watch(in_dev, "/dev/input", IN_CREATE | IN_ATTRIB);
	scan_inputs();
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGHUP, on_hup);
	load_wallpaper();
	fprintf(stderr, "s22-outerd: %s, %d inputs, lid %s\n", fbsys, ninputs,
		lid_closed ? "closed" : "open");

	panel_off();
	load_settings();
	if (lid_closed)
		show_until = now_ms() + lit_ms();

	while (!quit) {
		long long t = now_ms();
		int timeout = -1, n = 0, r;
		bool wake = false, redraw = false;

		/* Show or hide */
		if (show_until > t && lid_closed) {
			if (!lit) {
				/*
				 * The current frame first: the panel lights only once
				 * the frame the enable sends is in, and the enable
				 * sends the DRM buffer, which fbdev's damage worker
				 * fills from what we write. Unblanking at once would
				 * block that worker for the whole panel init (~0.4 s)
				 * and the old frame would go out; give it time first.
				 */
				draw();
				usleep(20000);
				panel_on();
				backlight(true);
			}
			timeout = (int)(show_until - t);
			/* redraw every second for the clock, 20 times a second for
			 * a scrolling ticker or a moving wallpaper */
			r = ticking || nwall > 1 ? ANIM_MS - (int)(t % ANIM_MS) :
				1000 - (int)(t % 1000);
			if (volume_until > t && volume_until - t < r)
				r = (int)(volume_until - t);
			if (r < timeout)
				timeout = r;
		} else if (lit) {
			panel_off();
		}
		if (held && !held_long) {	/* the long press fires while held */
			int d = (int)(held_at + LONG_MS - t);

			if (d < 0)
				d = 0;
			if (timeout < 0 || d < timeout)
				timeout = d;
		}
		if (side_tap) {			/* a single tap, once no second came */
			int d = (int)(side_tap + DOUBLE_MS - t);

			if (d < 0)
				d = 0;
			if (timeout < 0 || d < timeout)
				timeout = d;
		}

		for (i = 0; i < ninputs; i++)
			pfd[n++] = (struct pollfd){ .fd = inputs[i].fd, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = in_notify, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = in_dev, .events = POLLIN };
		/* The media hook last, if one runs (-1 is skipped by poll) */
		pfd[n++] = (struct pollfd){ .fd = media_fd, .events = POLLIN };
		r = poll(pfd, n, timeout);
		if (r < 0 && errno != EINTR)
			break;
		if (reload) {
			reload = 0;
			load_wallpaper();
			redraw = true;
		}
		/* A side tap with no second one: what a tap does */
		if (side_tap && now_ms() - side_tap >= DOUBLE_MS) {
			side_tap = 0;
			if (side_tap_lit && lid_closed) {
				cursor++;
				shown_wrap = true;	/* past the oldest: newest */
			} else {
				cursor = 0;
			}
			redraw = true;
		}
		/* Long presses */
		if (held && !held_long && now_ms() - held_at >= LONG_MS) {
			held_long = true;
			if (held == KEY_NUMERIC_B) {
				if (lid_closed && lit) {
					dismiss_shown();	/* inotify then redraws */
					show_until = now_ms() + lit_ms();
				}
			} else if (lid_closed) {	/* open: the GUI's keys */
				media(held == KEY_VOLUMEUP ? "next" : "previous");
			}
		}
		if (r <= 0) {
			if (lit && show_until > now_ms())
				draw();		/* the clock ticks */
			continue;
		}

		for (i = ninputs - 1; i >= 0; i--) {
			struct input_event ev;
			ssize_t len;

			if (!(pfd[i].revents & (POLLIN | POLLERR | POLLHUP)))
				continue;
			while ((len = read(inputs[i].fd, &ev, sizeof(ev))) == sizeof(ev)) {
				if (ev.type == EV_SW && ev.code == SW_LID) {
					lid_closed = ev.value;
					dbg("lid %s\n", ev.value ? "closed" : "open");
					if (ev.value)
						wake = true;
					else
						show_until = 0;
				} else if (ev.type == EV_KEY && wake_key(ev.code) &&
					   ev.value == 1 && ev.code == KEY_VOLUMEDOWN &&
					   side_down && lid_closed) {
					/* The flashlight's chord: neither key acts */
					dbg("side + volume down\n");
					held = 0;
					side_tap = 0;
				} else if (ev.type == EV_KEY && wake_key(ev.code) &&
					   ev.value == 1) {
					dbg("key %d down\n", ev.code);
					if (ev.code == KEY_NUMERIC_B)
						side_down = true;
					held = ev.code;
					held_at = now_ms();
					held_long = false;
				} else if (ev.type == EV_KEY && ev.code == KEY_NUMERIC_B &&
					   ev.value == 0 && held != KEY_NUMERIC_B) {
					side_down = false;	/* after a chord */
				} else if (ev.type == EV_KEY && ev.code == held &&
					   ev.value == 0) {
					if (ev.code == KEY_NUMERIC_B)
						side_down = false;
					/* A short press acts on release */
					dbg("key %d up%s\n", ev.code, held_long ? " (long)" : "");
					if (!held_long && held == KEY_NUMERIC_B) {
						if (!lid_closed) {
							cursor = 0;
						} else if (side_tap) {	/* the second tap */
							side_tap = 0;
							media("play-pause");
						} else {
							side_tap = now_ms();
							side_tap_lit = lit;
						}
					} else if (!held_long && lid_closed) {
						volume_step(held == KEY_VOLUMEUP ? 1 : -1);
						redraw = true;
					}
					held = 0;
					wake = true;
				}
			}
			if (len < 0 && errno == ENODEV)
				drop_input(i);
		}
		if (media_fd >= 0 && (pfd[n - 1].revents & (POLLIN | POLLHUP))) {
			media_read();
			redraw = true;
		}
		if (pfd[n - 3].revents & POLLIN) {
			char buf[4096];

			while (read(in_notify, buf, sizeof(buf)) > 0)
				;
			wake = true;	/* a new or cleared notification */
		}
		if (pfd[n - 2].revents & POLLIN) {
			char buf[4096];

			while (read(in_dev, buf, sizeof(buf)) > 0)
				;
			usleep(100000);	/* let udev set permissions */
			scan_inputs();
		}
		if (wake && lid_closed) {
			show_until = now_ms() + lit_ms();
			redraw = lit;
		}
		if (redraw && lit)
			draw();
	}
	panel_off();
	return 0;
}
