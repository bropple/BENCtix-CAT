// SPDX-License-Identifier: MIT
/*
 * s22-match: BENCO: MATCH FILED (D074), the bubble shooter from BENCsite
 * (benco-match-filed.html), on the keypad. The same game: its rules and
 * numbers, its 260 x 310 playfield scaled to the screen's width and drawn
 * the way the page draws it (each piece drawn once per size, then copied),
 * and its square-wave and noise sounds, made here.
 *
 *   Left/Right, 4/6   aim (faster while held)    1/3  aim a hair
 *   OK, 5, Up, 2      fire                       *    laser sight
 *   Back              pause; Back again quits
 *
 * A finger on the screen aims, and fires as it lifts (as on the page); a tap
 * on "laser" in the bar above switches the sight. A high score takes three
 * letters: 2-9 as on a keypad, Up/Down through the alphabet, Left/Right
 * between them, OK to file. The best score, the leaderboard (ten) and the
 * sight are kept in ~/.local/share/s22/match-filed.
 *
 * It ticks only while something moves: aiming with a key held, a shot, a
 * fall, an explosion. Waiting on a shot costs nothing.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "s22ui.h"

/* ---- the page's numbers ---- */

#define GW		260.0
#define GH		310.0
#define OFF_X		10.0
#define OFF_Y		10.0
#define SHOOTER_X	(GW / 2)
#define SHOOTER_Y	(GH - 30)
#define PROJECTILE_SPEED 6.2
#define AIM_CLAMP	(75 * M_PI / 180)
#define ADD_ROW_INTERVAL 8
#define PIECE_GRAVITY	0.32
#define DANGER_MARGIN	3
#define LEADERBOARD_MAX	10
#define STEP_MS		(1000.0 / 60)

#define MAX_ROWS	18
#define MAX_COLS	22
#define MAX_FALLING	512
#define MAX_EXPLOSIONS	16

/* Aiming from the keys: a step at the press, then turning while held */
#define AIM_TAP		0.02
#define AIM_NUDGE	0.006
#define AIM_HOLD_MS	150
#define AIM_SLOW	0.015	/* a step (1/60 s): 0.9 rad/s */
#define AIM_FAST	0.03	/* after AIM_FAST_MS held: 1.8 rad/s */
#define AIM_FAST_MS	650

#define HUD_MIN		40	/* the bar above the playfield, at least */

enum { EMPTY, T_TRIY, T_OVAL, T_HEX, T_GON, T_GLOB, T_CROSS, T_STAR, WALL, INERT, BOMB };
#define NTYPES	7

/* The roster */
static const struct {
	const char *name;
	uint32_t fill, edge;
} types[NTYPES + 1] = {
	[T_TRIY]  = { "R. Triy", 0x78b946, 0x3f5c28 },
	[T_OVAL]  = { "O. Val",  0xc1443a, 0x7a281f },
	[T_HEX]   = { "H. Hex",  0xd97a2b, 0x8a4d18 },
	[T_GON]   = { "P. Gon",  0x3d7dbf, 0x254d75 },
	[T_GLOB]  = { "G. Lobe", 0x2c4a7c, 0x16283f },
	[T_CROSS] = { "C. Ross", 0x7a4fb5, 0x452766 },
	[T_STAR]  = { "S. Tarr", 0xeecb2e, 0xa3861a },
};

/* The page's colours */
#define C_GREEN		0x78b946
#define C_PHOSPHOR	0xcdeab0
#define C_DIM		0x6f8a5c
#define C_RED		0xd84a3a
#define C_AMBER		0xe8b23d
#define C_PANEL		0x11170f
#define C_BORDER	0x2a3620
#define C_FIELD		0x0c1420
#define C_FLOOR		0x182034
#define C_SHADE		0x060a05

static const struct {
	double cell;
	int even_cols, rows;
} presets[] = {
	{ 24, 10, 12 }, { 20, 12, 13 }, { 17.142857, 14, 14 }, { 15, 16, 15 },
	{ 13.333333, 18, 16 }, { 12, 20, 17 }, { 10.909091, 22, 18 },
};
#define NPRESETS	(int)(sizeof(presets) / sizeof(presets[0]))

/* ---- the game's state ---- */

static double CELL = 24, ROWH = 24 * 0.87;
static int EVEN_COLS = 10, ODD_COLS = 9, ROWS = 12, DANGER_ROW = 11;
static int stagger;		/* keeps the hex offset with the rows as they move */
static unsigned char grid[MAX_ROWS][MAX_COLS];

enum { READY, AIMING, GAMEOVER, LEVELCLEAR };
static int state = READY;
static int score, best, level = 1, shots_fired;
static double aim;
static int current, next;
static struct {
	bool on;
	double x, y, vx, vy;
	int type;
} shot;
static int levelclear_timer;
static bool bomb_used;
static double row_slide;
static bool laser;
static bool paused;

static struct falling {
	double x, y, vx, vy, angle, spin;
	int type;
} falling[MAX_FALLING];
static int nfalling;
static struct {
	double x, y;
	int timer, max;
} explosions[MAX_EXPLOSIONS];
static int nexplosions;

static struct entry {
	char name[4];
	int score, level;
} board[LEADERBOARD_MAX];
static int nboard;
static bool entering;
static char name_chars[4] = "AAA";
static int name_slot;
static const char *reject_msg;
static int reject_timer;

static const char *blocked_names[] = { "FUK", "FUC", "SEX", "NIG", "KKK", "CUM" };
#define EASTER_EGG_NAME	"ASS"

/* Keys held for aiming: when they went down (ms), 0 if up */
static long long held_left, held_right;
static long long last_tick;
static double accum;
static bool ticking;
static char save_path[512];

static double rnd(void)
{
	return random() / (RAND_MAX + 1.0);
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* ---- sound: the page's Web Audio beeps, mixed here ---- */

#define RATE		48000
#define BLOCK		256
#define MAX_VOICES	64

static struct voice {
	bool noise, filt;
	double freq, gain, fall;	/* gain times fall each sample */
	long wait, t, len, end;		/* in samples */
	double phase;
	double b0, b1, b2, a1, a2, x1, x2, y1, y2;
} voices[MAX_VOICES];
static int nvoices;
static bool sound_on;
static pthread_mutex_t a_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t a_cond = PTHREAD_COND_INITIALIZER;

/* An oscillator or noise at vol, falling exponentially to 0.0001 over dur
 * seconds, as the page's gain ramps do; delay seconds from now */
static void voice_add(bool noise, double freq, double dur, double vol, double delay,
		      double filter)
{
	pthread_mutex_lock(&a_lock);
	if (sound_on && nvoices < MAX_VOICES) {
		struct voice *v = &voices[nvoices++];
		memset(v, 0, sizeof(*v));
		v->noise = noise;
		v->freq = freq;
		v->gain = vol;
		v->wait = (long)(delay * RATE);
		v->len = (long)(dur * RATE);
		v->end = (long)((dur + 0.02) * RATE);
		v->fall = pow(0.0001 / vol, 1.0 / (v->len > 0 ? v->len : 1));
		if (filter > 0) {
			/* A biquad low-pass, Q 0.707 */
			double w0 = 2 * M_PI * filter / RATE, cw = cos(w0);
			double alpha = sin(w0) / (2 * M_SQRT1_2), a0 = 1 + alpha;
			v->filt = true;
			v->b0 = (1 - cw) / 2 / a0;
			v->b1 = (1 - cw) / a0;
			v->b2 = v->b0;
			v->a1 = -2 * cw / a0;
			v->a2 = (1 - alpha) / a0;
		}
		pthread_cond_signal(&a_cond);
	}
	pthread_mutex_unlock(&a_lock);
}

static void beep(double freq, double dur, double vol, double delay)
{
	voice_add(false, freq, dur, vol, delay, 0);
}

static void noise_burst(double dur, double vol, double delay, double filter)
{
	voice_add(true, 0, dur, vol, delay, filter);
}

/* Sounds on (the game shows and runs) or off (and the queue forgotten) */
static void sound_enable(bool on)
{
	pthread_mutex_lock(&a_lock);
	sound_on = on;
	if (!on)
		nvoices = 0;
	pthread_mutex_unlock(&a_lock);
}

/* One voice into mix; false when it is over */
static bool voice_mix(struct voice *v, double *mix, unsigned *seed)
{
	for (int i = 0; i < BLOCK; i++) {
		if (v->wait > 0) {
			v->wait--;
			continue;
		}
		if (v->t >= v->end)
			return false;
		double s;
		if (v->noise) {
			*seed = *seed * 1664525u + 1013904223u;
			s = (*seed >> 8) / 8388608.0 - 1;
			if (v->filt) {
				double y = v->b0 * s + v->b1 * v->x1 + v->b2 * v->x2 -
					   v->a1 * v->y1 - v->a2 * v->y2;
				v->x2 = v->x1;
				v->x1 = s;
				v->y2 = v->y1;
				v->y1 = y;
				s = y;
			}
		} else {
			v->phase += v->freq / RATE;
			v->phase -= floor(v->phase);
			s = v->phase < 0.5 ? 1 : -1;
		}
		mix[i] += s * v->gain;
		if (v->t < v->len)
			v->gain *= v->fall;
		v->t++;
	}
	return true;
}

/* Plays what is queued; the device is closed after two quiet seconds */
static void *audio_thread(void *arg)
{
	snd_pcm_t *pcm = NULL;
	int16_t out[BLOCK];
	double mix[BLOCK];
	unsigned seed = 12345;
	int quiet = 0;

	for (;;) {
		pthread_mutex_lock(&a_lock);
		while (nvoices == 0 && pcm == NULL)
			pthread_cond_wait(&a_cond, &a_lock);
		memset(mix, 0, sizeof(mix));
		for (int i = 0; i < nvoices; )
			if (voice_mix(&voices[i], mix, &seed))
				i++;
			else
				voices[i] = voices[--nvoices];
		bool any = nvoices > 0;
		pthread_mutex_unlock(&a_lock);

		quiet = any ? 0 : quiet + 1;
		if (pcm && quiet > 2 * RATE / BLOCK) {
			snd_pcm_close(pcm);
			pcm = NULL;
			continue;
		}
		if (pcm == NULL) {
			if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0 ||
			    snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
					       1, RATE, 1, 40000) < 0) {
				if (pcm)
					snd_pcm_close(pcm);
				pcm = NULL;
				sound_enable(false);	/* no sound this time */
				sound_enable(true);
				usleep(500000);
				continue;
			}
		}
		for (int i = 0; i < BLOCK; i++) {
			double s = mix[i] * 32767;
			out[i] = s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)s;
		}
		snd_pcm_sframes_t r = snd_pcm_writei(pcm, out, BLOCK);
		if (r < 0)
			snd_pcm_recover(pcm, r, 1);
	}
	return NULL;
}

static void sfx_shoot(void) { beep(500, 0.05, 0.06, 0); }
static void sfx_land(void) { beep(300, 0.04, 0.04, 0); }
static void sfx_wall_thud(void) { beep(160, 0.05, 0.05, 0); beep(100, 0.06, 0.04, 0.01); }
static void sfx_match(int n)
{
	for (int i = 0; i < n && i < 6; i++)
		beep(520 + i * 90, 0.07, 0.06, i * 0.045);
}
static void sfx_drop(int n)
{
	for (int i = 0; i < n && i < 8; i++)
		beep(300 - i * 18, 0.05, 0.05, i * 0.03);
}
static void sfx_row_add(void) { beep(90, 0.18, 0.08, 0); }
static void sfx_start(void) { beep(660, 0.07, 0.06, 0); }
static void sfx_level_clear(void)
{
	beep(520, 0.07, 0.07, 0);
	beep(660, 0.07, 0.07, 0.08);
	beep(880, 0.10, 0.07, 0.16);
}
static void sfx_game_over(void)
{
	beep(220, 0.14, 0.08, 0);
	beep(160, 0.16, 0.08, 0.13);
	beep(110, 0.20, 0.08, 0.28);
}
static void sfx_bomb(void)
{
	noise_burst(0.35, 0.24, 0, 900);
	noise_burst(0.18, 0.14, 0.02, 350);
	beep(140, 0.12, 0.12, 0);
	beep(90, 0.16, 0.11, 0.03);
	beep(55, 0.22, 0.10, 0.07);
	beep(35, 0.28, 0.09, 0.12);
}
static void sfx_reject(void)
{
	beep(180, 0.09, 0.08, 0);
	beep(140, 0.12, 0.08, 0.08);
}

/* ---- kept between runs ---- */

static void save_write(void)
{
	char tmp[600];
	FILE *f;

	if (!save_path[0])
		return;
	snprintf(tmp, sizeof(tmp), "%s.new", save_path);
	if (!(f = fopen(tmp, "w")))
		return;
	fprintf(f, "best %d\nlaser %d\n", best, laser);
	for (int i = 0; i < nboard; i++)
		fprintf(f, "entry %s %d %d\n", board[i].name, board[i].score, board[i].level);
	if (fclose(f) == 0)
		rename(tmp, save_path);
}

static void save_load(void)
{
	const char *home = getenv("HOME");
	char line[128], name[8];
	int a, b;
	FILE *f;

	if (!home)
		return;
	snprintf(save_path, sizeof(save_path), "%s/.local/share/s22", home);
	mkdir(save_path, 0755);
	snprintf(save_path, sizeof(save_path), "%s/.local/share/s22/match-filed", home);
	if (!(f = fopen(save_path, "r")))
		return;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "best %d", &a) == 1)
			best = a;
		else if (sscanf(line, "laser %d", &a) == 1)
			laser = a != 0;
		else if (nboard < LEADERBOARD_MAX &&
			 sscanf(line, "entry %3s %d %d", name, &a, &b) == 3) {
			snprintf(board[nboard].name, sizeof(board[0].name), "%s", name);
			board[nboard].score = a;
			board[nboard].level = b;
			nboard++;
		}
	}
	fclose(f);
}

static bool leaderboard_qualifies(int s)
{
	if (s <= 0)
		return false;
	return nboard < LEADERBOARD_MAX || s > board[nboard - 1].score;
}

static void leaderboard_insert(const char *name, int s, int lvl)
{
	int i = nboard < LEADERBOARD_MAX ? nboard++ : LEADERBOARD_MAX - 1;

	/* After the ones that score as much: the page's stable sort */
	while (i > 0 && board[i - 1].score < s) {
		board[i] = board[i - 1];
		i--;
	}
	snprintf(board[i].name, sizeof(board[i].name), "%s", name);
	board[i].score = s;
	board[i].level = lvl;
	save_write();
}

/* ---- the name for the leaderboard ---- */

static void name_entry_cycle(int dir)
{
	int ch = name_chars[name_slot] + dir;

	if (ch > 'Z')
		ch = 'A';
	if (ch < 'A')
		ch = 'Z';
	name_chars[name_slot] = ch;
}

static void name_reject(const char *msg, int timer)
{
	reject_msg = msg;
	reject_timer = timer;
	memcpy(name_chars, "AAA", 4);
	name_slot = 0;
	sfx_reject();
}

static void name_entry_advance(void)
{
	if (name_slot < 2) {
		name_slot++;
		return;
	}
	if (!strcmp(name_chars, EASTER_EGG_NAME)) {
		name_reject("REJECTED. THE FLOOR IS NOT THAT CLEAR.", 100);
		return;
	}
	for (size_t i = 0; i < sizeof(blocked_names) / sizeof(blocked_names[0]); i++)
		if (!strcmp(name_chars, blocked_names[i])) {
			name_reject("NAME NOT FILED. TRY AGAIN.", 60);
			return;
		}
	leaderboard_insert(name_chars, score, level);
	entering = false;
}

/* A keypad key's letters: the first, then the next on each press */
static void name_entry_key_letters(int digit)
{
	static const char *letters[10] = {
		[2] = "ABC", [3] = "DEF", [4] = "GHI", [5] = "JKL",
		[6] = "MNO", [7] = "PQRS", [8] = "TUV", [9] = "WXYZ",
	};
	const char *l = letters[digit], *at = strchr(l, name_chars[name_slot]);

	name_chars[name_slot] = at && at[1] ? at[1] : l[0];
}

static void start_name_entry(void)
{
	entering = true;
	memcpy(name_chars, "AAA", 4);
	name_slot = 0;
	reject_msg = NULL;
	reject_timer = 0;
}

/* ---- the grid ---- */

static void configure_level_grid(int lvl)
{
	int step = (lvl - 1) / 2;

	if (step > NPRESETS - 1)
		step = NPRESETS - 1;
	CELL = presets[step].cell;
	EVEN_COLS = presets[step].even_cols;
	ODD_COLS = EVEN_COLS - 1;
	ROWS = presets[step].rows;
	ROWH = CELL * 0.87;
	DANGER_ROW = ROWS - 1;
	stagger = 0;
}

static int parity(int row)
{
	return (row + stagger) % 2;
}

static void cell_px(int row, int col, double *x, double *y)
{
	*x = parity(row) == 0 ? OFF_X + col * CELL + CELL / 2 : OFF_X + col * CELL + CELL;
	*y = OFF_Y + row * ROWH + CELL / 2;
}

static int cols_in_row(int row)
{
	return parity(row) == 0 ? EVEN_COLS : ODD_COLS;
}

static int neighbors_of(int row, int col, int out[6][2])
{
	static const int even[6][2] = { {0,-1}, {0,1}, {-1,-1}, {-1,0}, {1,-1}, {1,0} };
	static const int odd[6][2] = { {0,-1}, {0,1}, {-1,0}, {-1,1}, {1,0}, {1,1} };
	const int (*d)[2] = parity(row) == 0 ? even : odd;
	int n = 0;

	for (int i = 0; i < 6; i++) {
		int r = row + d[i][0], c = col + d[i][1];
		if (r >= 0 && r < ROWS && c >= 0 && c < cols_in_row(r)) {
			out[n][0] = r;
			out[n][1] = c;
			n++;
		}
	}
	return n;
}

static bool is_type(int v)
{
	return v >= T_TRIY && v <= T_STAR;
}

static int pick_active_type(void)
{
	bool present[NTYPES + 1] = { false };
	int pool[NTYPES], n = 0;

	for (int r = 0; r < ROWS; r++)
		for (int c = 0; c < cols_in_row(r); c++)
			if (is_type(grid[r][c]))
				present[grid[r][c]] = true;
	for (int t = T_TRIY; t <= T_STAR; t++)
		if (present[t])
			pool[n++] = t;
	if (n == 0)
		for (int t = T_TRIY; t <= T_STAR; t++)
			pool[n++] = t;
	return pool[(int)(rnd() * n)];
}

static int active_roster_count(int lvl)
{
	int n = 4 + (lvl - 1) / 2;
	return n < NTYPES ? n : NTYPES;
}

static void make_random_row_fill(int row, bool allow_inert, unsigned char *out)
{
	int cols = cols_in_row(row), roster = active_roster_count(level);
	double inert_chance = allow_inert ? fmin(0.22, 0.04 * (level - 1)) : 0;

	memset(out, EMPTY, MAX_COLS);
	for (int c = 0; c < cols; c++) {
		if (rnd() < inert_chance) {
			out[c] = INERT;
			continue;
		}
		int pick, tries = 0;
		do {
			pick = T_TRIY + (int)(rnd() * roster);
			tries++;
		} while (tries < 4 && c >= 2 && out[c - 1] == pick && out[c - 2] == pick);
		out[c] = pick;
	}
}

static bool is_in_danger(void)
{
	for (int r = ROWS - DANGER_MARGIN > 0 ? ROWS - DANGER_MARGIN : 0; r < ROWS; r++)
		for (int c = 0; c < cols_in_row(r); c++)
			if (grid[r][c] != EMPTY && grid[r][c] != WALL)
				return true;
	return false;
}

/* The wall comes in at the top and pushes everything down; false if the
 * bottom row is taken (the run is over) */
static bool pressure_tick(void)
{
	for (int c = 0; c < MAX_COLS; c++)
		if (grid[ROWS - 1][c] != EMPTY)
			return false;
	for (int r = ROWS - 1; r > 0; r--)
		memcpy(grid[r], grid[r - 1], MAX_COLS);
	stagger = 1 - stagger;
	memset(grid[0], WALL, EVEN_COLS);
	memset(grid[0] + EVEN_COLS, EMPTY, MAX_COLS - EVEN_COLS);
	return true;
}

static bool find_ceiling_landing(double x, double y, int *row, int *col)
{
	double best_d = INFINITY;
	bool found = false;

	for (int c = 0; c < cols_in_row(0); c++) {
		if (grid[0][c] != EMPTY)
			continue;
		double cx, cy;
		cell_px(0, c, &cx, &cy);
		double d = hypot(cx - x, cy - y);
		if (d < best_d) {
			best_d = d;
			*row = 0;
			*col = c;
			found = true;
		}
	}
	return found;
}

static bool find_adjacent_landing(int hr, int hc, double x, double y, int *row, int *col)
{
	int nb[6][2], n = neighbors_of(hr, hc, nb);
	double best_d = INFINITY;
	bool found = false;

	for (int i = 0; i < n; i++) {
		if (grid[nb[i][0]][nb[i][1]] != EMPTY)
			continue;
		double cx, cy;
		cell_px(nb[i][0], nb[i][1], &cx, &cy);
		double d = hypot(cx - x, cy - y);
		if (d < best_d) {
			best_d = d;
			*row = nb[i][0];
			*col = nb[i][1];
			found = true;
		}
	}
	if (found)
		return true;
	/* No empty neighbour (a packed pocket): along the grid's edges, never
	 * by distance alone */
	static int queue[MAX_ROWS * MAX_COLS][2];
	bool seen[MAX_ROWS][MAX_COLS] = { { false } };
	int head = 0, tail = 0, steps = 0;
	seen[hr][hc] = true;
	for (int i = 0; i < n; i++) {
		seen[nb[i][0]][nb[i][1]] = true;
		queue[tail][0] = nb[i][0];
		queue[tail++][1] = nb[i][1];
	}
	while (head < tail && steps < 60) {
		steps++;
		int r = queue[head][0], c = queue[head++][1];
		if (grid[r][c] == EMPTY) {
			double cx, cy;
			cell_px(r, c, &cx, &cy);
			double d = hypot(cx - x, cy - y);
			if (d < best_d) {
				best_d = d;
				*row = r;
				*col = c;
				found = true;
			}
		}
		int nn[6][2], m = neighbors_of(r, c, nn);
		for (int i = 0; i < m; i++)
			if (!seen[nn[i][0]][nn[i][1]]) {
				seen[nn[i][0]][nn[i][1]] = true;
				queue[tail][0] = nn[i][0];
				queue[tail++][1] = nn[i][1];
			}
	}
	return found;
}

/* The group of type at row, col, in group; its size */
static int flood_match(int row, int col, int group[][2])
{
	int type = grid[row][col], n = 0, sp = 0;
	static int stack[MAX_ROWS * MAX_COLS][2];
	bool seen[MAX_ROWS][MAX_COLS] = { { false } };

	seen[row][col] = true;
	stack[sp][0] = group[n][0] = row;
	stack[sp++][1] = group[n++][1] = col;
	while (sp > 0) {
		sp--;
		int r = stack[sp][0], c = stack[sp][1], nb[6][2], m = neighbors_of(r, c, nb);
		for (int i = 0; i < m; i++) {
			int nr = nb[i][0], nc = nb[i][1];
			if (!seen[nr][nc] && grid[nr][nc] == type) {
				seen[nr][nc] = true;
				stack[sp][0] = group[n][0] = nr;
				stack[sp++][1] = group[n++][1] = nc;
			}
		}
	}
	return n;
}

/* What hangs on nothing, in out; how many */
static int find_floating(int out[][2])
{
	static int stack[MAX_ROWS * MAX_COLS][2];
	bool anchored[MAX_ROWS][MAX_COLS] = { { false } };
	int sp = 0, n = 0;

#define SEED(r, c) do { \
	if (grid[r][c] != EMPTY && !anchored[r][c]) { \
		anchored[r][c] = true; \
		stack[sp][0] = (r); \
		stack[sp++][1] = (c); \
	} } while (0)
	/* The wall, wherever it reaches, is the ceiling; before the first
	 * push, row 0 is */
	if (grid[0][0] == WALL) {
		for (int r = 0; r < ROWS; r++)
			for (int c = 0; c < cols_in_row(r); c++)
				if (grid[r][c] == WALL)
					SEED(r, c);
	} else {
		for (int c = 0; c < cols_in_row(0); c++)
			SEED(0, c);
	}
#undef SEED
	while (sp > 0) {
		sp--;
		int r = stack[sp][0], c = stack[sp][1], nb[6][2], m = neighbors_of(r, c, nb);
		for (int i = 0; i < m; i++) {
			int nr = nb[i][0], nc = nb[i][1];
			if (!anchored[nr][nc] && grid[nr][nc] != EMPTY) {
				anchored[nr][nc] = true;
				stack[sp][0] = nr;
				stack[sp++][1] = nc;
			}
		}
	}
	for (int r = 0; r < ROWS; r++)
		for (int c = 0; c < cols_in_row(r); c++)
			if (grid[r][c] != EMPTY && grid[r][c] != WALL && !anchored[r][c]) {
				out[n][0] = r;
				out[n++][1] = c;
			}
	return n;
}

static bool grid_is_empty(void)
{
	for (int r = 0; r < ROWS; r++)
		for (int c = 0; c < cols_in_row(r); c++)
			if (grid[r][c] != EMPTY && grid[r][c] != WALL)
				return false;
	return true;
}

/* ---- the game ---- */

static int new_piece(void)
{
	if (!bomb_used && is_in_danger()) {
		bomb_used = true;
		return BOMB;
	}
	return pick_active_type();
}

static void start_level(bool fresh)
{
	configure_level_grid(level);
	memset(grid, EMPTY, sizeof(grid));
	int start_rows = 4 + level - 1 < 8 ? 4 + level - 1 : 8;
	for (int r = 0; r < start_rows && r < ROWS; r++)
		make_random_row_fill(r, r > 0, grid[r]);	/* never inert at the top */
	shots_fired = 0;
	current = new_piece();
	next = new_piece();
	shot.on = false;
	nfalling = 0;
	row_slide = 0;
	bomb_used = false;
	state = fresh ? READY : AIMING;
}

static void reset_game(void)
{
	score = 0;
	level = 1;
	start_level(true);
}

static void launch_falling(double x, double y, int type, double vx, double vy, double spin)
{
	if (nfalling < MAX_FALLING)
		falling[nfalling++] = (struct falling){ x, y, vx, vy, 0, spin, type };
}

static double sign(void)
{
	return rnd() < 0.5 ? -1 : 1;
}

/* Everything left hanging falls, worth 20 each */
static void drop_floating(void)
{
	static int fl[MAX_ROWS * MAX_COLS][2];
	int n = find_floating(fl);

	if (n == 0)
		return;
	for (int i = 0; i < n; i++) {
		int r = fl[i][0], c = fl[i][1];
		double x, y;
		cell_px(r, c, &x, &y);
		launch_falling(x, y, grid[r][c], (rnd() - 0.5) * 1.4, -0.4 - rnd() * 0.6,
			       sign() * (0.08 + rnd() * 0.12));
		grid[r][c] = EMPTY;
	}
	score += n * 20;
	sfx_drop(n);
}

static void resolve_bomb(int row, int col)
{
	double bx, by;
	int nb[6][2], n = neighbors_of(row, col, nb), cleared = 0;

	cell_px(row, col, &bx, &by);
	if (nexplosions < MAX_EXPLOSIONS)
		explosions[nexplosions++] = (typeof(explosions[0])){ bx, by, 18, 18 };
	nb[n][0] = row;		/* the bomb's own cell, then its neighbours */
	nb[n][1] = col;
	for (int i = n; i >= 0; i--) {
		int r = nb[i][0], c = nb[i][1], v = grid[r][c];
		if (v != EMPTY && v != WALL) {
			double x, y;
			cell_px(r, c, &x, &y);
			grid[r][c] = EMPTY;
			launch_falling(x, y, v, (rnd() - 0.5) * 3, -2 - rnd() * 2,
				       sign() * (0.15 + rnd() * 0.2));
			cleared++;
		}
	}
	score += cleared * 25;
	sfx_bomb();
	drop_floating();
}

static void trigger_game_over(void)
{
	state = GAMEOVER;
	if (score > best) {
		best = score;
		save_write();
	}
	sfx_game_over();
	if (leaderboard_qualifies(score))
		start_name_entry();
}

static void resolve_landing(int row, int col, int type)
{
	sfx_land();
	if (type == BOMB) {
		resolve_bomb(row, col);
	} else {
		static int group[MAX_ROWS * MAX_COLS][2];
		grid[row][col] = type;
		int n = flood_match(row, col, group);
		if (n >= 3) {
			double sx = 0, sy = 0, px[MAX_ROWS * MAX_COLS], py[MAX_ROWS * MAX_COLS];
			for (int i = 0; i < n; i++) {
				cell_px(group[i][0], group[i][1], &px[i], &py[i]);
				sx += px[i];
				sy += py[i];
			}
			double cx = sx / n, cy = sy / n;
			for (int i = 0; i < n; i++) {
				int t = grid[group[i][0]][group[i][1]];
				grid[group[i][0]][group[i][1]] = EMPTY;
				double dx = px[i] - cx, dy = py[i] - cy;
				double dist = hypot(dx, dy);
				if (dist == 0)
					dist = 1;
				double speed = 2 + rnd() * 1.6;
				launch_falling(px[i], py[i], t,
					       dx / dist * speed + (rnd() - 0.5) * 0.8,
					       dy / dist * speed - 2 - rnd() * 1.2,
					       sign() * (0.10 + rnd() * 0.15));
			}
			score += n * 15;
			sfx_match(n);
			drop_floating();
		}
	}
	if (grid_is_empty()) {
		state = LEVELCLEAR;
		levelclear_timer = 70;
		sfx_level_clear();
		return;
	}
	if (++shots_fired >= ADD_ROW_INTERVAL) {
		shots_fired = 0;
		if (!pressure_tick()) {
			trigger_game_over();
			return;
		}
		sfx_row_add();
		row_slide = -ROWH;
	}
}

/* The shot's next spot touches a piece */
static bool hits_piece(double x, double y, int *hr, int *hc)
{
	for (int r = 0; r < ROWS; r++)
		for (int c = 0; c < cols_in_row(r); c++) {
			if (grid[r][c] == EMPTY)
				continue;
			double cx, cy;
			cell_px(r, c, &cx, &cy);
			if (hypot(cx - x, cy - y) < CELL * 0.75) {
				*hr = r;
				*hc = c;
				return true;
			}
		}
	return false;
}

static void aim_by(double d)
{
	aim += d;
	if (aim < -AIM_CLAMP)
		aim = -AIM_CLAMP;
	if (aim > AIM_CLAMP)
		aim = AIM_CLAMP;
}

/* Aiming from the keys held, a step's worth */
static void aim_held(long long now)
{
	double d = 0;

	if (held_left && now - held_left >= AIM_HOLD_MS)
		d -= now - held_left >= AIM_FAST_MS ? AIM_FAST : AIM_SLOW;
	if (held_right && now - held_right >= AIM_HOLD_MS)
		d += now - held_right >= AIM_FAST_MS ? AIM_FAST : AIM_SLOW;
	if (d != 0 && state == AIMING)
		aim_by(d);
}

static void update(long long now)
{
	for (int i = 0; i < nfalling; ) {
		struct falling *p = &falling[i];
		p->vy += PIECE_GRAVITY;
		p->x += p->vx;
		p->y += p->vy;
		p->angle += p->spin;
		if (p->y < GH + 30 && p->x > -30 && p->x < GW + 30)
			i++;
		else
			*p = falling[--nfalling];
	}
	for (int i = 0; i < nexplosions; )
		if (--explosions[i].timer > 0)
			i++;
		else
			explosions[i] = explosions[--nexplosions];
	if (reject_timer > 0 && --reject_timer == 0)
		reject_msg = NULL;
	if (row_slide != 0) {
		row_slide *= 0.72;
		if (fabs(row_slide) < 0.4)
			row_slide = 0;
	}
	aim_held(now);

	if (state == LEVELCLEAR) {
		if (--levelclear_timer <= 0) {
			level++;
			start_level(false);
		}
		return;
	}
	if (!shot.on)
		return;

	shot.x += shot.vx;
	shot.y += shot.vy;
	if (shot.x < OFF_X + CELL / 2) {
		shot.x = OFF_X + CELL / 2;
		shot.vx = -shot.vx;
		sfx_wall_thud();
	} else if (shot.x > GW - OFF_X - CELL / 2) {
		shot.x = GW - OFF_X - CELL / 2;
		shot.vx = -shot.vx;
		sfx_wall_thud();
	}
	bool ceiling = shot.y <= OFF_Y + CELL / 2;
	int hr = 0, hc = 0;
	bool hit = !ceiling && hits_piece(shot.x, shot.y, &hr, &hc);
	if (ceiling || hit) {
		int lr, lc;
		bool ok = ceiling ? find_ceiling_landing(shot.x, shot.y, &lr, &lc)
				  : find_adjacent_landing(hr, hc, shot.x, shot.y, &lr, &lc);
		shot.on = false;
		if (ok)
			resolve_landing(lr, lc, shot.type);
		else
			trigger_game_over();	/* nowhere left to put it */
	}
}

/* Something moves, so the clock must run */
static bool animating(void)
{
	return !paused && (shot.on || nfalling || nexplosions || row_slide != 0 ||
			   reject_timer || state == LEVELCLEAR ||
			   ((held_left || held_right) && state == AIMING));
}

static void run_clock(struct ui *ui)
{
	if (!ticking && animating()) {
		ticking = true;
		last_tick = now_ms();
		accum = 0;
		ui_timer(ui, 16);
	}
	ui_redraw(ui);
}

static void playing(struct ui *ui)
{
	ui_inhibit_idle(ui, !paused && (state == AIMING || state == LEVELCLEAR));
}

static void fire(struct ui *ui)
{
	if (state == READY) {
		state = AIMING;
		sfx_start();
	} else if (state == GAMEOVER) {
		if (entering)
			return;
		reset_game();
		sfx_start();
	} else if (state == AIMING && !shot.on) {
		shot.on = true;
		shot.x = SHOOTER_X;
		shot.y = SHOOTER_Y;
		shot.vx = sin(aim) * PROJECTILE_SPEED;
		shot.vy = -cos(aim) * PROJECTILE_SPEED;
		shot.type = current;
		current = next;
		next = new_piece();
		sfx_shoot();
	}
	playing(ui);
	run_clock(ui);
}

static void pause_game(struct ui *ui, bool on)
{
	if (paused == on)
		return;
	paused = on;
	held_left = held_right = 0;
	sound_enable(!on);
	playing(ui);
	run_clock(ui);
}

/* ---- drawing ---- */

/* Where the playfield is on the screen: x = ox + logical x * S */
static double S = 480 / GW, OX, OY;
static int hud_h;

/* A surface: the screen (opaque) or a piece's picture (premultiplied
 * ARGB), clipped */
struct target {
	uint32_t *px;
	int w, h, stride;
	bool alpha;
	int x0, y0, x1, y1;
};

static inline void put(struct target *t, int x, int y, uint32_t rgb, double a)
{
	if (a <= 0)
		return;
	uint32_t *p = &t->px[y * t->stride + x];
	int ia = a >= 1 ? 256 : (int)(a * 256);
	uint32_t d = *p;
	int r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;

	if (t->alpha) {		/* premultiplied over */
		int da = d >> 24, dr = d >> 16 & 255, dg = d >> 8 & 255, db = d & 255;
		int inv = 256 - ia;
		int oa = (255 * ia + da * inv) >> 8;
		int or_ = (r * ia + dr * inv) >> 8;
		int og = (g * ia + dg * inv) >> 8;
		int ob = (b * ia + db * inv) >> 8;
		*p = (uint32_t)oa << 24 | or_ << 16 | og << 8 | ob;
	} else {
		int dr = d >> 16 & 255, dg = d >> 8 & 255, db = d & 255;
		r = (r * ia + dr * (256 - ia)) >> 8;
		g = (g * ia + dg * (256 - ia)) >> 8;
		b = (b * ia + db * (256 - ia)) >> 8;
		*p = 0xff000000 | r << 16 | g << 8 | b;
	}
}

/* Shapes by their signed distance (negative inside), for edges smoothed
 * over a pixel */
enum { SH_POLY, SH_CIRCLE, SH_ELLIPSE, SH_RECT, SH_SEG };
struct shape {
	int kind, n;
	double v[12][2];	/* POLY: corners; SEG: its ends */
	double cx, cy, rx, ry;	/* CIRCLE (rx), ELLIPSE; RECT: centre, half sizes */
};

static double sdf(const struct shape *s, double x, double y)
{
	switch (s->kind) {
	case SH_CIRCLE:
		return hypot(x - s->cx, y - s->cy) - s->rx;
	case SH_ELLIPSE: {
		double px = (x - s->cx) / s->rx, py = (y - s->cy) / s->ry;
		double k0 = hypot(px, py);
		double k1 = hypot(px / s->rx, py / s->ry);
		return k1 > 0 ? k0 * (k0 - 1) / k1 : -fmin(s->rx, s->ry);
	}
	case SH_RECT: {
		double qx = fabs(x - s->cx) - s->rx, qy = fabs(y - s->cy) - s->ry;
		return hypot(fmax(qx, 0), fmax(qy, 0)) + fmin(fmax(qx, qy), 0);
	}
	case SH_SEG: {
		double ex = s->v[1][0] - s->v[0][0], ey = s->v[1][1] - s->v[0][1];
		double wx = x - s->v[0][0], wy = y - s->v[0][1];
		double l2 = ex * ex + ey * ey;
		double h = l2 > 0 ? fmin(fmax((wx * ex + wy * ey) / l2, 0), 1) : 0;
		return hypot(wx - ex * h, wy - ey * h);
	}
	default: {
		double d = INFINITY, sgn = 1;
		for (int i = 0, j = s->n - 1; i < s->n; j = i++) {
			double ex = s->v[j][0] - s->v[i][0], ey = s->v[j][1] - s->v[i][1];
			double wx = x - s->v[i][0], wy = y - s->v[i][1];
			double h = fmin(fmax((wx * ex + wy * ey) / (ex * ex + ey * ey), 0), 1);
			double bx = wx - ex * h, by = wy - ey * h;
			d = fmin(d, bx * bx + by * by);
			bool c1 = y >= s->v[i][1], c2 = y < s->v[j][1], c3 = ex * wy > ey * wx;
			if ((c1 && c2 && c3) || (!c1 && !c2 && !c3))
				sgn = -sgn;
		}
		return sgn * sqrt(d);
	}
	}
}

static void shape_bounds(const struct shape *s, double m, double *x0, double *y0,
			 double *x1, double *y1)
{
	if (s->kind == SH_POLY || s->kind == SH_SEG) {
		int n = s->kind == SH_SEG ? 2 : s->n;
		*x0 = *y0 = INFINITY;
		*x1 = *y1 = -INFINITY;
		for (int i = 0; i < n; i++) {
			*x0 = fmin(*x0, s->v[i][0]);
			*x1 = fmax(*x1, s->v[i][0]);
			*y0 = fmin(*y0, s->v[i][1]);
			*y1 = fmax(*y1, s->v[i][1]);
		}
	} else {
		double ry = s->kind == SH_CIRCLE ? s->rx : s->ry;
		*x0 = s->cx - s->rx;
		*x1 = s->cx + s->rx;
		*y0 = s->cy - ry;
		*y1 = s->cy + ry;
	}
	*x0 -= m;
	*y0 -= m;
	*x1 += m;
	*y1 += m;
}

/* Fill the shape (stroke 0) or stroke its outline that wide, in pixels */
static void draw_shape(struct target *t, const struct shape *s, uint32_t rgb, double alpha,
		       double stroke)
{
	double bx0, by0, bx1, by1;
	shape_bounds(s, stroke / 2 + 1, &bx0, &by0, &bx1, &by1);
	int x0 = (int)floor(bx0), y0 = (int)floor(by0), x1 = (int)ceil(bx1), y1 = (int)ceil(by1);

	if (x0 < t->x0) x0 = t->x0;
	if (y0 < t->y0) y0 = t->y0;
	if (x1 > t->x1) x1 = t->x1;
	if (y1 > t->y1) y1 = t->y1;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++) {
			double d = sdf(s, x + 0.5, y + 0.5), cov;
			if (stroke > 0)
				cov = stroke / 2 + 0.5 - fabs(d);
			else
				cov = 0.5 - d;
			if (cov > 0)
				put(t, x, y, rgb, alpha * (cov > 1 ? 1 : cov));
		}
}

static void fill_rect(struct target *t, double x, double y, double w, double h, uint32_t rgb,
		      double alpha)
{
	int x0 = (int)lround(x), y0 = (int)lround(y);
	int x1 = (int)lround(x + w), y1 = (int)lround(y + h);

	if (x0 < t->x0) x0 = t->x0;
	if (y0 < t->y0) y0 = t->y0;
	if (x1 > t->x1) x1 = t->x1;
	if (y1 > t->y1) y1 = t->y1;
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++)
			put(t, xx, yy, rgb, alpha);
}

static struct shape seg(double ax, double ay, double bx, double by)
{
	struct shape s = { .kind = SH_SEG };
	s.v[0][0] = ax;
	s.v[0][1] = ay;
	s.v[1][0] = bx;
	s.v[1][1] = by;
	return s;
}

static struct shape circle(double cx, double cy, double r)
{
	return (struct shape){ .kind = SH_CIRCLE, .cx = cx, .cy = cy, .rx = r };
}

static struct shape rect(double x, double y, double w, double h)
{
	return (struct shape){ .kind = SH_RECT, .cx = x + w / 2, .cy = y + h / 2,
			       .rx = w / 2, .ry = h / 2 };
}

/* A dashed line, the dashes in logical pixels as the page's setLineDash */
static void dashed(struct target *t, double ax, double ay, double bx, double by, double on,
		   double off, uint32_t rgb, double alpha, double width)
{
	double len = hypot(bx - ax, by - ay), ux = (bx - ax) / len, uy = (by - ay) / len;

	for (double p = 0; p < len; p += on + off) {
		double q = fmin(p + on, len);
		struct shape s = seg(ax + ux * p, ay + uy * p, ax + ux * q, ay + uy * q);
		draw_shape(t, &s, rgb, alpha, width);
	}
}

/* ---- the pieces, drawn as the page draws them, in logical pixels ---- */

#define MAX_LAYERS	32
struct layer {
	struct shape s;
	int fill, stroke;	/* colours, -1 for none */
	double width;
};

static int lay(struct layer *l, int n, struct shape s, int fill, int stroke)
{
	if (n < MAX_LAYERS)
		l[n] = (struct layer){ s, fill, stroke, 1 };
	return n + 1;
}

static struct shape poly(int n)
{
	return (struct shape){ .kind = SH_POLY, .n = n };
}

static int visor(struct layer *l, int n, double cx, double cy, double r)
{
	double vy = cy + r * 0.12;
	n = lay(l, n, rect(round(cx - r * 0.55), round(vy - r * 0.17), round(r * 1.1),
			   round(r * 0.34)), 0x9a9d94, -1);
	return lay(l, n, rect(round(cx - r * 0.35), round(vy - r * 0.08), round(r * 0.7),
			      round(r * 0.16)), 0xd84a3a, -1);
}

static int piece_layers(struct layer *l, int id, double cx, double cy, double r)
{
	int n = 0;

	if (id == WALL) {
		double w = r * 1.9, h = r * 1.3, xl = cx - w / 2, top = cy - h / 2;
		n = lay(l, n, rect(xl, top, w, h), 0x2c2c28, -1);
		for (double i = 0; i < w; i += 5)
			n = lay(l, n, rect(xl + i, top, 2, h), 0xe8b23d, -1);
		return lay(l, n, rect(xl, top, w, h), -1, 0x111111);
	}
	if (id == INERT) {
		double s = r * 0.85;
		n = lay(l, n, rect(cx - s, cy - s, s * 2, s * 2), 0x6b6b62, 0x3a3a34);
		n = lay(l, n, seg(cx - s * 0.6, cy - s * 0.6, cx + s * 0.5, cy + s * 0.7), -1, 0x3a3a34);
		return lay(l, n, seg(cx + s * 0.6, cy - s * 0.5, cx - s * 0.5, cy + s * 0.6), -1, 0x3a3a34);
	}
	if (id == BOMB) {
		n = lay(l, n, circle(cx, cy, r * 0.85), 0x1a1a1a, 0x000000);
		n = lay(l, n, seg(cx, cy - r * 0.85, cx + r * 0.3, cy - r * 1.3), -1, 0x8a6a1a);
		n = lay(l, n, circle(cx + r * 0.3, cy - r * 1.3, r * 0.18), 0xf4cf4a, -1);
		return visor(l, n, cx, cy, r);
	}
	if (!is_type(id))
		return 0;

	struct shape s;
	switch (id) {
	case T_TRIY:
		s = poly(3);
		s.v[0][0] = cx;            s.v[0][1] = cy - r;
		s.v[1][0] = cx + r * 0.95; s.v[1][1] = cy + r * 0.8;
		s.v[2][0] = cx - r * 0.95; s.v[2][1] = cy + r * 0.8;
		break;
	case T_OVAL:
		s = (struct shape){ .kind = SH_ELLIPSE, .cx = cx, .cy = cy, .rx = r * 0.95,
				    .ry = r * 0.72 };
		break;
	case T_HEX:
		s = rect(cx - r * 0.82, cy - r * 0.82, r * 1.64, r * 1.64);
		break;
	case T_GON:
		s = poly(5);
		for (int i = 0; i < 5; i++) {
			double a = -M_PI / 2 + i * (2 * M_PI / 5);
			s.v[i][0] = cx + r * cos(a);
			s.v[i][1] = cy + r * sin(a);
		}
		break;
	case T_GLOB:
		s = circle(cx, cy, r * 0.88);
		break;
	case T_CROSS: {
		double a = r * 0.38, e = r * 0.92;
		static const int pts[12][2] = {
			{-1,-2}, {1,-2}, {1,-1}, {2,-1}, {2,1}, {1,1},
			{1,2}, {-1,2}, {-1,1}, {-2,1}, {-2,-1}, {-1,-1},
		};
		s = poly(12);
		for (int i = 0; i < 12; i++) {
			s.v[i][0] = cx + (abs(pts[i][0]) == 2 ? e : a) * (pts[i][0] < 0 ? -1 : 1);
			s.v[i][1] = cy + (abs(pts[i][1]) == 2 ? e : a) * (pts[i][1] < 0 ? -1 : 1);
		}
		break;
	}
	default:	/* T_STAR */
		s = poly(10);
		for (int i = 0; i < 10; i++) {
			double a = -M_PI / 2 + i * (M_PI / 5), rad = i % 2 == 0 ? r * 0.95 : r * 0.4;
			s.v[i][0] = cx + rad * cos(a);
			s.v[i][1] = cy + rad * sin(a);
		}
		break;
	}
	n = lay(l, n, s, types[id].fill, types[id].edge);
	return visor(l, n, cx, cy, r);
}

static void scale_shape(struct shape *s, double k)
{
	for (int i = 0; i < 12; i++) {
		s->v[i][0] *= k;
		s->v[i][1] *= k;
	}
	s->cx *= k;
	s->cy *= k;
	s->rx *= k;
	s->ry *= k;
}

/* A piece's picture: as the page's sprite cache, a square ceil(r * 2.3)
 * logical pixels wide (what pokes out, the bomb's spark, is cut, as there),
 * at the screen's scale */
struct sprite {
	int id, rkey, n;
	uint32_t *px;
};
#define MAX_SPRITES	64
static struct sprite sprites[MAX_SPRITES];
static int nsprites;
static double sprite_scale;

static struct sprite *sprite_get(int id, double r)
{
	int rkey = (int)lround(r * 10);

	for (int i = 0; i < nsprites; i++)
		if (sprites[i].id == id && sprites[i].rkey == rkey)
			return &sprites[i];
	if (nsprites == MAX_SPRITES) {		/* old sizes: start again */
		for (int i = 0; i < nsprites; i++)
			free(sprites[i].px);
		nsprites = 0;
	}
	double size = ceil(r * 2.3);
	int n = (int)lround(size * S);
	struct sprite *sp = &sprites[nsprites];
	sp->px = calloc((size_t)n * n, sizeof(uint32_t));
	if (!sp->px)
		return NULL;
	sp->id = id;
	sp->rkey = rkey;
	sp->n = n;
	nsprites++;

	struct layer l[MAX_LAYERS];
	int nl = piece_layers(l, id, size / 2, size / 2, r);
	struct target t = { sp->px, n, n, n, true, 0, 0, n, n };
	for (int i = 0; i < nl && i < MAX_LAYERS; i++) {
		scale_shape(&l[i].s, S);
		if (l[i].fill >= 0 && l[i].s.kind != SH_SEG)
			draw_shape(&t, &l[i].s, l[i].fill, 1, 0);
		if (l[i].stroke >= 0)
			draw_shape(&t, &l[i].s, l[i].stroke, 1, l[i].width * S);
	}
	return sp;
}

/* Premultiplied over */
static inline void put_pm(struct target *t, int x, int y, uint32_t s)
{
	uint32_t *p = &t->px[y * t->stride + x];
	int a = s >> 24;

	if (a == 0)
		return;
	if (a == 255) {
		*p = s | 0xff000000;
		return;
	}
	uint32_t d = *p;
	int inv = 255 - a;
	int r = (s >> 16 & 255) + (((d >> 16 & 255) * inv) >> 8);
	int g = (s >> 8 & 255) + (((d >> 8 & 255) * inv) >> 8);
	int b = (s & 255) + (((d & 255) * inv) >> 8);
	*p = 0xff000000 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 |
	     (b > 255 ? 255 : b);
}

/* A piece centred at logical x, y, turned by angle */
static void draw_piece(struct target *t, int id, double x, double y, double r, double angle)
{
	struct sprite *sp = sprite_get(id, r);
	if (!sp)
		return;
	double cx = OX + x * S, cy = OY + y * S;
	int n = sp->n;

	if (angle == 0) {
		int x0 = (int)lround(cx - n / 2.0), y0 = (int)lround(cy - n / 2.0);
		for (int j = 0; j < n; j++) {
			int yy = y0 + j;
			if (yy < t->y0 || yy >= t->y1)
				continue;
			for (int i = 0; i < n; i++) {
				int xx = x0 + i;
				if (xx >= t->x0 && xx < t->x1)
					put_pm(t, xx, yy, sp->px[j * n + i]);
			}
		}
		return;
	}
	double ca = cos(angle), sa = sin(angle), half = n / 2.0, reach = half * 1.42;
	int x0 = (int)floor(cx - reach), x1 = (int)ceil(cx + reach);
	int y0 = (int)floor(cy - reach), y1 = (int)ceil(cy + reach);
	if (x0 < t->x0) x0 = t->x0;
	if (y0 < t->y0) y0 = t->y0;
	if (x1 > t->x1) x1 = t->x1;
	if (y1 > t->y1) y1 = t->y1;
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++) {
			double dx = xx + 0.5 - cx, dy = yy + 0.5 - cy;
			int sx = (int)floor(dx * ca + dy * sa + half);
			int sy = (int)floor(-dx * sa + dy * ca + half);
			if (sx >= 0 && sx < n && sy >= 0 && sy < n)
				put_pm(t, xx, yy, sp->px[sy * n + sx]);
		}
}

/* Text at logical x and baseline y, the page's sizes */
static void text(struct ui_canvas *c, double x, double y, double size, uint32_t rgb,
		 enum ui_align align, const char *s)
{
	int px = (int)lround(size * S), w = ui_text_width(UI_MONO, px, s);
	int sx = (int)lround(OX + x * S), sy = (int)lround(OY + y * S - px * 0.86);

	if (align == UI_CENTER)
		sx -= w / 2;
	else if (align == UI_RIGHT)
		sx -= w;
	ui_text(c, sx, sy, UI_MONO, px, 0xff000000 | rgb, s);
}

static uint32_t mix_rgb(uint32_t a, uint32_t b, double t)
{
	int r = (a >> 16 & 255) * t + (b >> 16 & 255) * (1 - t);
	int g = (a >> 8 & 255) * t + (b >> 8 & 255) * (1 - t);
	int bl = (a & 255) * t + (b & 255) * (1 - t);
	return r << 16 | g << 8 | bl;
}

static void shade(struct target *t, double alpha)
{
	fill_rect(t, OX, OY, GW * S, GH * S, C_SHADE, alpha);
}

static void simulate_trajectory(double angle, double pts[][2], int *n)
{
	double x = SHOOTER_X, y = SHOOTER_Y;
	double vx = sin(angle) * PROJECTILE_SPEED, vy = -cos(angle) * PROJECTILE_SPEED;
	int hr, hc;

	*n = 0;
	for (int i = 0; i < 220; i++) {
		x += vx;
		y += vy;
		if (x < OFF_X + CELL / 2) {
			x = OFF_X + CELL / 2;
			vx = -vx;
		} else if (x > GW - OFF_X - CELL / 2) {
			x = GW - OFF_X - CELL / 2;
			vx = -vx;
		}
		pts[*n][0] = x;
		pts[(*n)++][1] = y;
		if (y <= OFF_Y + CELL / 2 || hits_piece(x, y, &hr, &hc))
			break;
	}
}

static void draw_overlay_text(struct ui_canvas *c, struct target *t, const char **lines, int n)
{
	shade(t, 0.6);
	for (int i = 0; i < n; i++)
		text(c, GW / 2, GH / 2 - (n - 1) * 7 + i * 14, 10, C_PHOSPHOR, UI_CENTER, lines[i]);
}

/* The page's start screen, with its side panel's roster and how to play */
static void draw_ready(struct ui_canvas *c, struct target *t)
{
	shade(t, 0.72);
	text(c, GW / 2, 40, 12, C_GREEN, UI_CENTER, "BENCO: MATCH FILED");
	text(c, GW / 2, 56, 8, C_DIM, UI_CENTER, "Sort three or more. The floor is");
	text(c, GW / 2, 66, 8, C_DIM, UI_CENTER, "not clear until you do.");
	text(c, GW / 2, 86, 7, C_DIM, UI_CENTER, "ROSTER");
	for (int i = 0; i < NTYPES; i++) {
		double x = i < 4 ? 52 : 150, y = 100 + (i % 4) * 16;
		draw_piece(t, T_TRIY + i, x, y, 6, 0);
		text(c, x + 12, y + 3.5, 8, C_PHOSPHOR, UI_LEFT, types[T_TRIY + i].name);
	}
	static const char *how[] = {
		"4/6: aim   1/3: a hair   OK: fire",
		"*: laser sight   Back: pause",
		"Match 3+ of a kind. What hangs",
		"on nothing falls, gray tiles too.",
		"The striped wall pushes down every",
		"8 shots; reach the bottom and it's",
		"filed. Clear all but the wall.",
	};
	for (int i = 0; i < 7; i++)
		text(c, GW / 2, 178 + i * 11, 7, i < 2 ? C_AMBER : C_DIM, UI_CENTER, how[i]);
	text(c, GW / 2, 268, 10, C_PHOSPHOR, UI_CENTER, "PRESS OK TO START");
}

static void draw_name_entry(struct ui_canvas *c, struct target *t)
{
	char s[32];

	shade(t, 0.72);
	text(c, GW / 2, GH / 2 - 42, 10, C_AMBER, UI_CENTER, "NEW HIGH SCORE");
	snprintf(s, sizeof(s), "score: %d", score);
	text(c, GW / 2, GH / 2 - 26, 10, C_PHOSPHOR, UI_CENTER, s);
	for (int i = 0; i < 3; i++) {
		double x = GW / 2 - 26 + i * 26;
		char ch[2] = { name_chars[i], 0 };
		uint32_t col = i == name_slot ? C_RED : C_PHOSPHOR;
		text(c, x, GH / 2 + 8, 16, col, UI_CENTER, ch);
		if (i == name_slot)
			text(c, x, GH / 2 + 22, 10, col, UI_CENTER, "^");
	}
	if (reject_msg) {
		text(c, GW / 2, GH / 2 + 42, 8, C_RED, UI_CENTER, reject_msg);
	} else {
		text(c, GW / 2, GH / 2 + 42, 8, C_DIM, UI_CENTER, "2-9: letters   up/down: cycle");
		text(c, GW / 2, GH / 2 + 54, 8, C_DIM, UI_CENTER, "left/right: slot   OK: file");
		text(c, GW / 2, GH / 2 + 66, 8, C_DIM, UI_CENTER, "Back: skip");
	}
}

static void draw_game_over(struct ui_canvas *c, struct target *t)
{
	char s[48];

	shade(t, 0.6);
	text(c, GW / 2, 48, 10, C_PHOSPHOR, UI_CENTER, "FILED. PRESS OK TO RESTART");
	snprintf(s, sizeof(s), "score: %d", score);
	text(c, GW / 2, 64, 10, C_PHOSPHOR, UI_CENTER, s);
	text(c, GW / 2, 88, 9, C_AMBER, UI_CENTER, "LEADERBOARD");
	if (nboard == 0)
		text(c, GW / 2, 104, 8, C_DIM, UI_CENTER, "no entries filed yet");
	for (int i = 0; i < nboard; i++) {
		double y = 104 + i * 13;
		snprintf(s, sizeof(s), "%d. %s", i + 1, board[i].name);
		text(c, GW / 2 - 60, y, 9, C_PHOSPHOR, UI_LEFT, s);
		snprintf(s, sizeof(s), "%d", board[i].score);
		text(c, GW / 2 + 60, y, 9, C_PHOSPHOR, UI_RIGHT, s);
	}
}

/* The page's side panel, as a bar above the playfield */
static void draw_hud(struct ui_canvas *c)
{
	char v[5][16];
	static const char *labels[5] = { "score", "level", "best", "next row", "laser" };
	int w = c->w / 5;

	ui_fill(c, 0, 0, c->w, hud_h, 0xff000000 | C_PANEL);
	ui_fill(c, 0, hud_h - 1, c->w, 1, 0xff000000 | C_BORDER);
	snprintf(v[0], sizeof(v[0]), "%d", score);
	snprintf(v[1], sizeof(v[1]), "%d", level);
	snprintf(v[2], sizeof(v[2]), "%d", best > score ? best : score);
	snprintf(v[3], sizeof(v[3]), "%d", ADD_ROW_INTERVAL - shots_fired);
	snprintf(v[4], sizeof(v[4]), "%s", laser ? "on" : "off");
	int ls = 13, vs = 19, top = (hud_h - ui_line_height(ls) - ui_line_height(vs)) / 2;
	for (int i = 0; i < 5; i++) {
		ui_text_box(c, i * w, top, w, UI_MONO, ls, 0xff000000 | C_DIM, UI_CENTER, labels[i]);
		ui_text_box(c, i * w, top + ui_line_height(ls), w, UI_MONO, vs,
			    0xff000000 | (i == 4 && laser ? C_RED : C_PHOSPHOR), UI_CENTER, v[i]);
	}
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	/* The playfield as wide as the screen, under the bar */
	S = fmin(c->w / GW, (c->h - HUD_MIN) / GH);
	hud_h = c->h - (int)lround(GH * S);
	OX = (c->w - GW * S) / 2;
	OY = hud_h;
	if (S != sprite_scale) {	/* the pictures are at the old scale */
		for (int i = 0; i < nsprites; i++)
			free(sprites[i].px);
		nsprites = 0;
		sprite_scale = S;
	}

	ui_fill(c, 0, 0, c->w, c->h, 0xff000000 | C_SHADE);
	draw_hud(c);
	int fx0 = (int)lround(OX), fy0 = (int)lround(OY);
	int fx1 = (int)lround(OX + GW * S), fy1 = c->h;
	struct target t = { c->px, c->w, c->h, c->stride, false, fx0, fy0, fx1, fy1 };

	ui_fill(c, fx0, fy0, fx1 - fx0, fy1 - fy0, 0xff000000 | C_FIELD);
	double floor_y = OFF_Y + ROWS * ROWH;
	fill_rect(&t, OX, OY + floor_y * S, GW * S, (GH - floor_y) * S, C_FLOOR, 1);

	/* The danger line */
	double ly, lx;
	cell_px(DANGER_ROW, 0, &lx, &ly);
	ly += CELL * 0.42 + 2;
	dashed(&t, OX, OY + ly * S, OX + GW * S, OY + ly * S, 4 * S, 3 * S, C_RED, 0.5, S);

	double r = CELL * 0.42;
	for (int row = 0; row < ROWS; row++)
		for (int col = 0; col < cols_in_row(row); col++)
			if (grid[row][col] != EMPTY) {
				double x, y;
				cell_px(row, col, &x, &y);
				draw_piece(&t, grid[row][col], x, y + row_slide, r, 0);
			}
	for (int i = 0; i < nfalling; i++)
		draw_piece(&t, falling[i].type, falling[i].x, falling[i].y, r, falling[i].angle);
	for (int i = 0; i < nexplosions; i++) {
		double progress = 1 - (double)explosions[i].timer / explosions[i].max;
		double rad = (4 + progress * 22) * S, alpha = 1 - progress;
		double ex = OX + explosions[i].x * S, ey = OY + explosions[i].y * S;
		struct shape s = circle(ex, ey, rad * 0.4);
		draw_shape(&t, &s, 0xf4cf4a, alpha * 0.9, 0);
		for (int k = 0; k < 8; k++) {
			double a = k / 8.0 * M_PI * 2;
			s = seg(ex + cos(a) * rad * 0.5, ey + sin(a) * rad * 0.5,
				ex + cos(a) * rad, ey + sin(a) * rad);
			draw_shape(&t, &s, C_RED, alpha, 2 * S);
		}
	}
	if (shot.on)
		draw_piece(&t, shot.type, shot.x, shot.y, r, 0);

	if (laser && state == AIMING && !paused) {
		static double pts[220][2];
		int n;
		simulate_trajectory(aim, pts, &n);
		for (int i = 0; i < n; i += 3)
			fill_rect(&t, OX + (pts[i][0] - 1) * S, OY + (pts[i][1] - 1) * S, 2 * S, 2 * S,
				  C_RED, 0.55);
	}

	/* The shooter */
	fill_rect(&t, OX + (SHOOTER_X - 14) * S, OY + (SHOOTER_Y + 6) * S, 28 * S, 8 * S, 0x2c3a1c, 1);
	if (state == AIMING)
		dashed(&t, OX + SHOOTER_X * S, OY + SHOOTER_Y * S,
		       OX + (SHOOTER_X + sin(aim) * 26) * S, OY + (SHOOTER_Y - cos(aim) * 26) * S,
		       3 * S, 4 * S, C_PHOSPHOR, 0.4, S);
	if (current)
		draw_piece(&t, current, SHOOTER_X, SHOOTER_Y, r, 0);
	if (next) {
		text(c, GW - 24, SHOOTER_Y - 14, 9, mix_rgb(C_PHOSPHOR, C_FIELD, 0.5), UI_CENTER, "next");
		draw_piece(&t, next, GW - 24, SHOOTER_Y + 2, CELL * 0.32, 0);
	}

	if (state == READY) {
		draw_ready(c, &t);
	} else if (state == GAMEOVER) {
		if (entering)
			draw_name_entry(c, &t);
		else
			draw_game_over(c, &t);
	} else if (state == LEVELCLEAR) {
		char s[32];
		snprintf(s, sizeof(s), "level %d filed", level + 1);
		const char *lines[] = { "LEVEL CLEAR", s };
		draw_overlay_text(c, &t, lines, 2);
	}
	if (paused && state != READY && state != GAMEOVER) {
		const char *lines[] = { "PAUSED", "OK: resume   Back: quit" };
		draw_overlay_text(c, &t, lines, 2);
	}

	/* The page's scanlines: a faint dark line every third pixel */
	for (int y = fy0; y < fy1; y += 3) {
		uint32_t *p = c->px + y * c->stride;
		for (int x = fx0; x < fx1; x++) {
			uint32_t d = p[x];
			p[x] = 0xff000000 | ((d >> 16 & 255) * 240 >> 8) << 16 |
			       ((d >> 8 & 255) * 240 >> 8) << 8 | ((d & 255) * 240 >> 8);
		}
	}
}

/* ---- input ---- */

static void tick(struct ui *ui)
{
	long long now = now_ms();

	if (!ui_visible(ui) && (state == AIMING || state == LEVELCLEAR))
		pause_game(ui, true);
	if (paused) {
		ticking = false;
		return;
	}
	double delta = now - last_tick;
	last_tick = now;
	if (delta > 250)
		delta = 250;
	accum += delta;
	for (int steps = 0; accum >= STEP_MS && steps < 5; steps++) {
		update(now);
		accum -= STEP_MS;
	}
	playing(ui);
	ui_redraw(ui);
	if (animating())
		ui_timer(ui, 16);
	else
		ticking = false;
}

static void aim_at(double px, double py)
{
	double a = atan2(px - SHOOTER_X, -(py - SHOOTER_Y));
	aim = fmax(-AIM_CLAMP, fmin(AIM_CLAMP, a));
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st, const char *txt)
{
	bool left = code == KEY_LEFT || code == KEY_4, right = code == KEY_RIGHT || code == KEY_6;

	if (st == UI_RELEASED) {
		if (left)
			held_left = 0;
		if (right)
			held_right = 0;
		return;
	}
	if (entering) {
		if (st == UI_REPEATED && !(code == KEY_UP || code == KEY_DOWN))
			return;
		if (code == KEY_UP)
			name_entry_cycle(1);
		else if (code == KEY_DOWN)
			name_entry_cycle(-1);
		else if (code == KEY_LEFT || code == KEY_BACKSPACE)
			name_slot = (name_slot + 2) % 3;
		else if (code == KEY_RIGHT)
			name_slot = (name_slot + 1) % 3;
		else if (code == KEY_OK || code == KEY_ENTER)
			name_entry_advance();
		else if (code >= KEY_2 && code <= KEY_9)
			name_entry_key_letters(code - KEY_1 + 1);
		else if (code == KEY_BACK || code == KEY_ESC)
			entering = false;	/* not filed */
		run_clock(ui);
		return;
	}
	if (code == KEY_BACK || code == KEY_ESC) {
		if (st != UI_PRESSED)
			return;
		if (paused || state == READY || state == GAMEOVER)
			ui_quit(ui);
		else
			pause_game(ui, true);
		return;
	}
	if (paused) {
		if (st == UI_PRESSED && (code == KEY_OK || code == KEY_ENTER || code == KEY_5))
			pause_game(ui, false);
		return;
	}
	if (st == UI_REPEATED)
		return;		/* held keys are timed here */
	if (left || right) {
		if (left)
			held_left = now_ms();
		else
			held_right = now_ms();
		if (state == AIMING)
			aim_by(left ? -AIM_TAP : AIM_TAP);
	} else if (code == KEY_1 || code == KEY_3) {
		if (state == AIMING)
			aim_by(code == KEY_1 ? -AIM_NUDGE : AIM_NUDGE);
	} else if (code == KEY_OK || code == KEY_ENTER || code == KEY_5 || code == KEY_UP ||
		   code == KEY_2) {
		fire(ui);
	} else if (code == KEY_NUMERIC_STAR || code == KEY_KPASTERISK) {
		laser = !laser;
		save_write();
	}
	run_clock(ui);
}

/* A finger aims, and fires as it lifts; a tap on "laser" in the bar
 * switches the sight */
static void touch(struct ui *ui, int id, enum ui_key_state st, int x, int y)
{
	static int finger = -1;
	static bool in_bar;
	static int bar_x;

	if (st == UI_PRESSED) {
		if (finger >= 0)
			return;
		finger = id;
		in_bar = y < hud_h;
		bar_x = x;
	}
	if (id != finger)
		return;
	if (st == UI_RELEASED) {
		finger = -1;
		if (in_bar) {
			if (bar_x >= ui_width(ui) * 4 / 5) {
				laser = !laser;
				save_write();
			}
		} else if (paused) {
			pause_game(ui, false);
		} else if (entering) {
			name_entry_cycle(1);
		} else {
			fire(ui);
		}
		run_clock(ui);
		return;
	}
	if (!in_bar && !paused && !entering && state == AIMING) {
		aim_at((x - OX) / S, (y - OY) / S);
		ui_redraw(ui);
	}
}

static void focus(struct ui *ui, bool focused)
{
	if (!focused && (state == AIMING || state == LEVELCLEAR))
		pause_game(ui, true);
	if (!focused) {
		held_left = held_right = 0;
		sound_enable(false);
	} else if (!paused) {
		sound_enable(true);
	}
}

static void start(struct ui *ui)
{
	pthread_t th;

	sound_enable(true);
	if (pthread_create(&th, NULL, audio_thread, NULL) == 0)
		pthread_detach(th);
}

int main(void)
{
	srandom((unsigned)time(NULL) ^ (unsigned)getpid());
	save_load();
	reset_game();

	struct ui_app app = {
		.app_id = "s22-match",
		.title = "Match Filed",
		.draw = draw,
		.key = key,
		.tick = tick,
		.start = start,
		.focus = focus,
		.touch = touch,
		.opaque = true,
	};
	return ui_main(&app);
}
