// SPDX-License-Identifier: MIT
/*
 * s22-emu: Game Boy (and Color) and NES games (D048), a small libretro
 * frontend: the core (gambatte, nestopia) is loaded with dlopen and run
 * one frame per tick of a timer at the game's rate.
 *
 *   s22-emu gb|nes [ROM]
 *
 * Games are the user's own, in ~/Games/gb (.gb, .gbc) and ~/Games/nes
 * (.nes, .fds); a list picks one. Saves go to ~/Games/saves: the
 * cartridge's battery memory (NAME.srm, and NAME.rtc for its clock) every
 * few seconds while it changes, one save state from the menu (NAME.state),
 * and where the game was when the app closed (NAME.resume: Home closes the
 * apps), which it continues from the next time. A core's own files (boot
 * ROMs, the FDS BIOS, NstDatabase.xml) go in ~/Games/system.
 *
 * The picture is as large as the window allows at the core's aspect ratio,
 * pixel aspect ratio included (the NES' 8:7): a whole multiple of the
 * game's pixels is plain (the Game Boy is 3x, 480x432); otherwise each
 * screen pixel is the game's pixels it covers, by area, so pixels stay
 * sharp and even, with soft edges one screen pixel wide.
 *
 * While a game plays the display stays on (idle-inhibit). It pauses, into
 * the menu, when the window is not in front or not shown (the display off,
 * the lid shut, the lock screen). Sound goes to the media output (ALSA's
 * default, PipeWire), and the frame rate follows it: the timer runs a
 * little slower or faster to keep about AUDIO_TARGET_MS queued.
 *
 * The game runs in a thread of its own, on another core than the drawing
 * (SMB3 takes 10.5 ms a frame and the drawing 4.5, of the 16.7 there
 * are). Frames pass between them triple-buffered; the core is only ever
 * touched by one thread at a time (core_lock: the menu's work happens
 * with the game thread stopped).
 *
 * Keys, by default: the D-pad on the arrows or 2 4 6 8; A on 3, 9 or OK;
 * B on 1, 7 or C; Start #, Select *. The menu's Controls change them, per
 * system, and add turbo A and B. Back always opens the menu.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include "libretro.h"
#include "s22ui.h"

#define AUDIO_TARGET_MS	40	/* queued sound the pacing aims for */
#define AUDIO_PERIOD_US	10000	/* PipeWire's share: its quantum follows */
#define SRAM_EVERY	(5 * 60) /* frames between battery memory checks */
#define MAX_GAMES	512
#define BLACK		0xff000000

struct system {
	const char *key, *app_id, *title, *core, *dir;
	const char *exts[4];
	const char *where;	/* for the empty list */
};

static const struct system systems[] = {
	{ "gb", "s22-gb", "Game Boy", "/usr/lib/libretro/gambatte_libretro.so", "gb",
	  { ".gb", ".gbc", ".dmg", NULL }, ".gb and .gbc files" },
	{ "nes", "s22-nes", "NES", "/usr/lib/libretro/nestopia_libretro.so", "nes",
	  { ".nes", ".fds", ".unf", ".unif" }, ".nes files" },
};
static const struct system *sys;

/* ---- the core ---- */

static struct {
	void *dl;
	void (*init)(void);
	void (*deinit)(void);
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*get_system_info)(struct retro_system_info *);
	void (*get_system_av_info)(struct retro_system_av_info *);
	void (*set_controller_port_device)(unsigned, unsigned);
	void (*reset)(void);
	void (*run)(void);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *, size_t);
	bool (*unserialize)(const void *, size_t);
	bool (*load_game)(const struct retro_game_info *);
	void (*unload_game)(void);
	void *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
} core;

static struct retro_system_av_info av;
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static char games_dir[256], saves_dir[256], system_dir[256], roms_dir[256];

/* Core options: what the core declared, at their defaults */
#define MAX_VARS 96
static struct { char key[64], value[64]; } vars[MAX_VARS];
static int nvars;

/* ---- the game ---- */

enum view { V_PICK, V_PLAY, V_MENU, V_KEYS };
static enum view view = V_PICK;
static char game_path[512], game_name[256];
static void *game_data;
static bool loaded;
static char flash[96];		/* a message under the menu */

/* Frames, XRGB: the game thread fills back, and swaps it with ready;
 * drawing swaps ready with front, and shows front */
#define FB_MAX	(512 * 512)
struct frame {
	uint32_t *px;
	int w, h;
};
static struct frame frames[3];
static struct frame *back = &frames[0], *ready = &frames[1], *front = &frames[2];
static bool back_new, ready_new;
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;

/* The game thread runs frames while running is set; it holds core_lock
 * while in the core */
static pthread_mutex_t core_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t run_cond = PTHREAD_COND_INITIALIZER;
static bool running;
static int ready_fd = -1;	/* eventfd: a frame is ready to show */
static uint32_t *conv565, *conv1555;
static uint16_t pad;		/* buttons held (RETRO_DEVICE_ID_JOYPAD_*) */
static bool keydown[KEY_MAX + 1];
static void pad_update(void);
static uint16_t turbo;		/* buttons held by turbo keys */
static unsigned frame_no;	/* the game thread's, for turbo */

/* The battery memory as last written, to write only what changed */
static void *sram_copy, *rtc_copy;
static size_t sram_len, rtc_len;
static int sram_count;

static snd_pcm_t *pcm;
static int16_t abuf[2 * 8192];	/* the frame's sound, stereo */
static size_t aframes;

/* S22_EMU_STATS set: every 5 s of play, what the frames cost (stderr) */
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	bool on;
	long long since, run_ns, run_max, draw_ns, draw_max;
	int runs, draws, late;
	long delay;
} st;

static int timer_fd = -1;
static long long next_ns, frame_ns;
static double pace = 1.0;
static bool focused = true;

/* Picker */
static char (*games)[256];
static int ngames;
static struct ui_list pick = { .row_h = 64 };
/* Menu */
enum { M_RESUME, M_SAVE, M_LOAD, M_RESET, M_KEYS, M_PICK, M_QUIT, M_COUNT };
static const char *menu_items[M_COUNT] = {
	"Resume", "Save state", "Load state", "Restart game", "Controls",
	"Choose another game", "Quit",
};
static int menu_sel;

static long long mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- files ---- */

static void *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rbe");
	void *buf = NULL;
	long n;

	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && n < (64 << 20) &&
	    fseek(f, 0, SEEK_SET) == 0 && (buf = malloc(n)) != NULL) {
		if (fread(buf, 1, n, f) == (size_t)n) {
			*len = n;
		} else {
			free(buf);
			buf = NULL;
		}
	}
	fclose(f);
	return buf;
}

/* Whole or not at all: a temporary file, renamed over */
static bool write_file(const char *path, const void *data, size_t len)
{
	char tmp[600];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.new", path);
	f = fopen(tmp, "wbe");
	if (f == NULL)
		return false;
	bool ok = fwrite(data, 1, len, f) == len;
	if (fclose(f) != 0)
		ok = false;
	if (ok && rename(tmp, path) == 0)
		return true;
	unlink(tmp);
	return false;
}

static void save_path(char *buf, int len, const char *ext)
{
	snprintf(buf, len, "%s/%s.%s", saves_dir, game_name, ext);
}

/* ---- battery memory ---- */

static void mem_load(unsigned id, const char *ext, void **copy, size_t *copy_len)
{
	void *mem = core.get_memory_data(id);
	size_t n = core.get_memory_size(id), got = 0;
	char path[600];

	free(*copy);
	*copy = NULL;
	*copy_len = 0;
	if (mem == NULL || n == 0)
		return;
	save_path(path, sizeof(path), ext);
	void *data = read_file(path, &got);
	if (data) {
		memcpy(mem, data, got < n ? got : n);
		free(data);
	}
	*copy = malloc(n);
	if (*copy) {
		memcpy(*copy, mem, n);
		*copy_len = n;
	}
}

static void mem_flush(unsigned id, const char *ext, void **copy, size_t *copy_len)
{
	void *mem = core.get_memory_data(id);
	size_t n = core.get_memory_size(id);
	char path[600];

	if (mem == NULL || n == 0 || (*copy && *copy_len == n && !memcmp(mem, *copy, n)))
		return;
	save_path(path, sizeof(path), ext);
	if (!write_file(path, mem, n)) {
		fprintf(stderr, "s22-emu: %s: %s\n", path, strerror(errno));
		return;
	}
	if (*copy_len != n) {
		free(*copy);
		*copy = malloc(n);
		*copy_len = *copy ? n : 0;
	}
	if (*copy)
		memcpy(*copy, mem, n);
}

static void sram_flush(void)
{
	if (!loaded)
		return;
	mem_flush(RETRO_MEMORY_SAVE_RAM, "srm", &sram_copy, &sram_len);
	mem_flush(RETRO_MEMORY_RTC, "rtc", &rtc_copy, &rtc_len);
}

/* ---- states ---- */

static bool state_save(const char *ext)
{
	size_t n = core.serialize_size();
	void *buf = n ? malloc(n) : NULL;
	char path[600];
	bool ok = false;

	if (buf && core.serialize(buf, n)) {
		save_path(path, sizeof(path), ext);
		ok = write_file(path, buf, n);
	}
	free(buf);
	return ok;
}

static bool state_load(const char *ext)
{
	char path[600];
	size_t n = 0;

	save_path(path, sizeof(path), ext);
	void *buf = read_file(path, &n);
	bool ok = buf && core.unserialize(buf, n);
	free(buf);
	return ok;
}

static bool state_exists(const char *ext)
{
	char path[600];

	save_path(path, sizeof(path), ext);
	return access(path, R_OK) == 0;
}

/* ---- callbacks from the core ---- */

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	va_list ap;

	if (level < RETRO_LOG_WARN)
		return;
	va_start(ap, fmt);
	fprintf(stderr, "s22-emu (core): ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* "Description; default|other|..." */
static void vars_declare(const struct retro_variable *v)
{
	for (; v->key && nvars < MAX_VARS; v++) {
		const char *semi = v->value ? strstr(v->value, "; ") : NULL;
		if (semi == NULL)
			continue;
		snprintf(vars[nvars].key, sizeof(vars[nvars].key), "%s", v->key);
		snprintf(vars[nvars].value, sizeof(vars[nvars].value), "%.*s",
			 (int)strcspn(semi + 2, "|"), semi + 2);
		nvars++;
	}
}

static bool core_env(unsigned cmd, void *data)
{
	switch (cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char **)data = system_dir;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = saves_dir;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format f = *(enum retro_pixel_format *)data;
		if (f != RETRO_PIXEL_FORMAT_0RGB1555 && f != RETRO_PIXEL_FORMAT_RGB565 &&
		    f != RETRO_PIXEL_FORMAT_XRGB8888)
			return false;
		pixfmt = f;
		return true;
	}
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		av.geometry = *(const struct retro_game_geometry *)data;
		return true;
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		av = *(const struct retro_system_av_info *)data;
		frame_ns = (long long)(1e9 / (av.timing.fps > 1 ? av.timing.fps : 60));
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		nvars = 0;
		vars_declare(data);
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *v = data;
		for (int i = 0; i < nvars; i++)
			if (!strcmp(vars[i].key, v->key)) {
				v->value = vars[i].value;
				return true;
			}
		v->value = NULL;
		return false;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		if (data)
			*(int *)data = 3;
		return true;
	case RETRO_ENVIRONMENT_GET_LANGUAGE:
		*(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE: {
		const struct retro_message *m = data;
		snprintf(flash, sizeof(flash), "%s", m->msg);
		return true;
	}
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
		return true;
	default:
		return false;
	}
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (data == NULL || w == 0 || h == 0)
		return;			/* the same frame again */
	if (w * h > FB_MAX)
		return;
	back->w = w;
	back->h = h;
	back_new = true;
	for (unsigned y = 0; y < h; y++) {
		const uint8_t *row = (const uint8_t *)data + y * pitch;
		uint32_t *out = back->px + y * w;
		if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
			const uint32_t *in = (const uint32_t *)row;
			for (unsigned x = 0; x < w; x++)
				out[x] = in[x] | BLACK;
		} else {
			const uint16_t *in = (const uint16_t *)row;
			const uint32_t *t = pixfmt == RETRO_PIXEL_FORMAT_RGB565 ? conv565 : conv1555;
			for (unsigned x = 0; x < w; x++)
				out[x] = t[in[x]];
		}
	}
}

static void audio_add(const int16_t *data, size_t frames)
{
	size_t room = sizeof(abuf) / sizeof(abuf[0]) / 2 - aframes;

	if (frames > room)
		frames = room;
	memcpy(abuf + 2 * aframes, data, frames * 2 * sizeof(int16_t));
	aframes += frames;
}

static void audio_sample(int16_t l, int16_t r)
{
	int16_t s[2] = { l, r };

	audio_add(s, 1);
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
	audio_add(data, frames);
	return frames;
}

static void input_poll(void) {}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	if (port != 0 || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD)
		return 0;
	uint16_t p = __atomic_load_n(&pad, __ATOMIC_RELAXED);

	if (frame_no & 2)
		p |= __atomic_load_n(&turbo, __ATOMIC_RELAXED);

	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return p;
	return id < 16 ? (p >> id) & 1 : 0;
}

/* ---- sound ---- */

static void audio_close(void)
{
	if (pcm) {
		snd_pcm_drop(pcm);
		snd_pcm_close(pcm);
		pcm = NULL;
	}
}

static void audio_open(void)
{
	unsigned rate = (unsigned)(av.timing.sample_rate + 0.5);
	int err;

	if (pcm || rate == 0)
		return;
	err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (err < 0) {
		fprintf(stderr, "s22-emu: no sound: %s\n", snd_strerror(err));
		pcm = NULL;
		return;
	}
	/* Short periods: PipeWire sizes its graph's quantum by them, and the
	 * whole graph's latency with it; the buffer holds twice the target */
	snd_pcm_hw_params_t *hw;
	unsigned period = AUDIO_PERIOD_US, buffer = 2 * AUDIO_TARGET_MS * 1000;
	snd_pcm_hw_params_alloca(&hw);
	if ((err = snd_pcm_hw_params_any(pcm, hw)) >= 0 &&
	    (err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) >= 0 &&
	    (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16)) >= 0 &&
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) >= 0 &&
	    (err = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0)) >= 0 &&
	    (err = snd_pcm_hw_params_set_period_time_near(pcm, hw, &period, NULL)) >= 0 &&
	    (err = snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &buffer, NULL)) >= 0)
		err = snd_pcm_hw_params(pcm, hw);
	if (err < 0) {
		fprintf(stderr, "s22-emu: sound at %u Hz: %s\n", rate, snd_strerror(err));
		snd_pcm_close(pcm);
		pcm = NULL;
		return;
	}
	/* Start with the queue where the pacing wants it */
	static const int16_t quiet[2 * 512];
	long want = (long)rate * AUDIO_TARGET_MS / 1000;
	while (want > 0) {
		snd_pcm_sframes_t r = snd_pcm_writei(pcm, quiet, want < 512 ? want : 512);
		if (r <= 0)
			break;
		want -= r;
	}
}

/* The frame's sound, and how far the pacing is off: > 1 when too much is
 * queued (run slower) */
static void audio_write(void)
{
	const int16_t *p = abuf;
	size_t left = aframes;

	aframes = 0;
	if (pcm == NULL)
		return;
	while (left > 0) {
		snd_pcm_sframes_t r = snd_pcm_writei(pcm, p, left);
		if (r == -EAGAIN)
			break;		/* full: this much is dropped */
		if (r < 0) {
			if (snd_pcm_recover(pcm, r, 1) < 0)
				break;
			continue;
		}
		p += 2 * r;
		left -= r;
	}
	snd_pcm_sframes_t delay;
	if (snd_pcm_delay(pcm, &delay) == 0) {
		double target = av.timing.sample_rate * AUDIO_TARGET_MS / 1000;
		double off = (delay - target) / target;
		off = off < -1 ? -1 : off > 1 ? 1 : off;
		pace = 1.0 + off * 0.005;
		st.delay = delay;
	}
}

/* ---- running ---- */

static void timer_arm(long long at)
{
	struct itimerspec it = { .it_value = { at / 1000000000LL, at % 1000000000LL } };

	timerfd_settime(timer_fd, TFD_TIMER_ABSTIME, &it, NULL);
}

static void timer_stop(void)
{
	struct itimerspec it = { 0 };

	timerfd_settime(timer_fd, 0, &it, NULL);
}

/* Out of the core, and staying out */
static void worker_stop(void)
{
	pthread_mutex_lock(&core_lock);
	running = false;
	pthread_mutex_unlock(&core_lock);
	timer_stop();
}

static void pause_game(struct ui *ui)
{
	if (view != V_PLAY)
		return;
	worker_stop();
	audio_close();
	sram_flush();
	ui_inhibit_idle(ui, false);
	memset(keydown, 0, sizeof(keydown));
	pad_update();
	view = V_MENU;
	menu_sel = M_RESUME;
	ui_redraw(ui);
}

static void resume_game(struct ui *ui)
{
	if (!loaded)
		return;
	view = V_PLAY;
	flash[0] = 0;
	memset(keydown, 0, sizeof(keydown));
	pad_update();
	audio_open();
	ui_inhibit_idle(ui, true);
	next_ns = mono_ns() + frame_ns;
	pthread_mutex_lock(&st_lock);
	bool stats = st.on;
	memset(&st, 0, sizeof(st));
	st.on = stats;
	st.since = next_ns;
	pthread_mutex_unlock(&st_lock);
	pthread_mutex_lock(&core_lock);
	running = true;
	pthread_cond_signal(&run_cond);
	pthread_mutex_unlock(&core_lock);
	timer_arm(next_ns);
	ui_redraw(ui);
}

/* The game thread, holding core_lock: the frame (or frames) due */
static void run_frames(void)
{
	/* Behind (a slow frame, a busy moment): catch up a frame or two
	 * without showing them, then give up and go on from now */
	long long now = mono_ns();
	for (int i = 0; i < 3 && next_ns <= now; i++) {
		long long t0 = mono_ns();
		frame_no++;
		core.run();
		audio_write();
		next_ns += (long long)(frame_ns * pace);
		t0 = mono_ns() - t0;
		pthread_mutex_lock(&st_lock);
		st.runs++;
		st.run_ns += t0;
		if (t0 > st.run_max)
			st.run_max = t0;
		pthread_mutex_unlock(&st_lock);
	}
	pthread_mutex_lock(&st_lock);
	if (next_ns <= now) {
		next_ns = now + frame_ns;
		st.late++;
	}
	if (st.on && now - st.since >= 5000000000LL) {
		double s = (now - st.since) / 1e9;
		fprintf(stderr, "s22-emu: %.1f fps run (%.2f ms, max %.2f), %.1f fps drawn "
			"(%.2f ms, max %.2f), %d late, sound queued %.0f ms, pace %.4f\n",
			st.runs / s, st.runs ? st.run_ns / 1e6 / st.runs : 0, st.run_max / 1e6,
			st.draws / s, st.draws ? st.draw_ns / 1e6 / st.draws : 0,
			st.draw_max / 1e6, st.late,
			st.delay * 1000.0 / (av.timing.sample_rate > 0 ? av.timing.sample_rate : 1),
			pace);
		memset(&st, 0, sizeof(st));
		st.on = true;
		st.since = now;
	}
	pthread_mutex_unlock(&st_lock);
	timer_arm(next_ns);
	if (back_new) {
		pthread_mutex_lock(&frame_lock);
		struct frame *t = ready;
		ready = back;
		back = t;
		ready_new = true;
		pthread_mutex_unlock(&frame_lock);
		back_new = false;
	}
	uint64_t one = 1;
	if (write(ready_fd, &one, sizeof(one)) < 0) {
		/* the counter is full: the drawing is behind anyway */
	}
	if (++sram_count >= SRAM_EVERY) {
		sram_count = 0;
		sram_flush();
	}
}

static void *game_thread(void *arg)
{
	uint64_t n;

	for (;;) {
		pthread_mutex_lock(&core_lock);
		while (!running)
			pthread_cond_wait(&run_cond, &core_lock);
		pthread_mutex_unlock(&core_lock);
		/* Stopped meanwhile, the timer is too: the next resume arms it */
		if (read(timer_fd, &n, sizeof(n)) != sizeof(n))
			continue;
		pthread_mutex_lock(&core_lock);
		if (running)
			run_frames();
		pthread_mutex_unlock(&core_lock);
	}
	return NULL;
}

/* A frame is ready: show it, unless the game should not be running */
static void on_ready(struct ui *ui, int fd)
{
	uint64_t n;

	if (read(fd, &n, sizeof(n)) != sizeof(n) || view != V_PLAY)
		return;
	if (!focused || !ui_visible(ui)) {
		pause_game(ui);
		return;
	}
	ui_redraw(ui);
}

static void game_close(void)
{
	if (!loaded)
		return;
	sram_flush();
	state_save("resume");
	core.unload_game();
	loaded = false;
	free(game_data);
	game_data = NULL;
	for (int i = 0; i < 3; i++)
		frames[i].w = frames[i].h = 0;
	back_new = ready_new = false;
}

static bool game_open(struct ui *ui, const char *path)
{
	struct retro_system_info info = { 0 };
	struct retro_game_info gi = { .path = path };
	size_t len = 0;

	game_close();
	core.get_system_info(&info);
	game_data = read_file(path, &len);
	if (game_data == NULL && !info.need_fullpath) {
		snprintf(flash, sizeof(flash), "Cannot read the game");
		return false;
	}
	gi.data = game_data;
	gi.size = len;
	if (!core.load_game(&gi)) {
		free(game_data);
		game_data = NULL;
		snprintf(flash, sizeof(flash), "Not a game this can play");
		return false;
	}
	loaded = true;
	snprintf(game_path, sizeof(game_path), "%s", path);
	const char *base = strrchr(path, '/');
	snprintf(game_name, sizeof(game_name), "%s", base ? base + 1 : path);
	char *dot = strrchr(game_name, '.');
	if (dot && dot != game_name)
		*dot = 0;
	core.get_system_av_info(&av);
	frame_ns = (long long)(1e9 / (av.timing.fps > 1 ? av.timing.fps : 60));
	core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	mem_load(RETRO_MEMORY_SAVE_RAM, "srm", &sram_copy, &sram_len);
	mem_load(RETRO_MEMORY_RTC, "rtc", &rtc_copy, &rtc_len);
	/* Where it was when the app last closed */
	state_load("resume");
	sram_count = 0;
	resume_game(ui);
	return true;
}

/* ---- the picture ---- */

/*
 * Scaling tables: for each screen column (row), the game column (row) it
 * starts in, and how much of it, out of 256, lies in the next one.
 */
static struct {
	int src_w, src_h, dst_w, dst_h;
	int *x0, *y0;
	uint8_t *xw, *yw;
	uint32_t *rows;		/* the game's rows, scaled across */
} sc;

static void scale_axis(int src, int dst, int *i0, uint8_t *w)
{
	for (int d = 0; d < dst; d++) {
		/* The screen pixel covers [a, b) of the game's pixels */
		long long a = (long long)d * src, b = (long long)(d + 1) * src;
		int i = a / dst;
		long long edge = (long long)(i + 1) * dst;
		i0[d] = i;
		w[d] = b > edge && i + 1 < src ? (uint8_t)((b - edge) * 256 / (b - a)) : 0;
	}
}

static bool scale_setup(int sw, int sh, int dw, int dh)
{
	if (sc.src_w == sw && sc.src_h == sh && sc.dst_w == dw && sc.dst_h == dh)
		return true;
	free(sc.x0);
	free(sc.y0);
	free(sc.xw);
	free(sc.yw);
	free(sc.rows);
	sc.x0 = malloc(dw * sizeof(int));
	sc.y0 = malloc(dh * sizeof(int));
	sc.xw = malloc(dw);
	sc.yw = malloc(dh);
	sc.rows = malloc((size_t)sh * dw * sizeof(uint32_t));
	if (!sc.x0 || !sc.y0 || !sc.xw || !sc.yw || !sc.rows) {
		sc.src_w = 0;
		return false;
	}
	scale_axis(sw, dw, sc.x0, sc.xw);
	scale_axis(sh, dh, sc.y0, sc.yw);
	sc.src_w = sw;
	sc.src_h = sh;
	sc.dst_w = dw;
	sc.dst_h = dh;
	return true;
}

static inline uint32_t mix(uint32_t a, uint32_t b, unsigned w)
{
	uint32_t rb = (((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8) & 0xff00ff;
	uint32_t g = (((a & 0xff00) * (256 - w) + (b & 0xff00) * w) >> 8) & 0xff00;

	return BLACK | rb | g;
}

/* The largest rectangle of the game's display aspect ratio in the window */
static void fit(struct ui_canvas *c, int *x, int *y, int *w, int *h)
{
	double base = av.geometry.base_width && av.geometry.base_height ?
		(double)av.geometry.base_width / av.geometry.base_height :
		(double)front->w / front->h;
	double par = av.geometry.aspect_ratio > 0 ? av.geometry.aspect_ratio / base : 1;
	double dar = (double)front->w / front->h * par;

	*w = c->w;
	*h = (int)(c->w / dar + 0.5);
	if (*h > c->h) {
		*h = c->h;
		*w = (int)(c->h * dar + 0.5);
	}
	*x = (c->w - *w) / 2;
	*y = (c->h - *h) / 2;
}

static void blit(struct ui_canvas *c)
{
	int dx, dy, dw, dh;

	int fb_w = front->w, fb_h = front->h;
	const uint32_t *fb = front->px;

	if (fb_w == 0 || fb_h == 0)
		return;
	fit(c, &dx, &dy, &dw, &dh);
	if (dw <= 0 || dh <= 0 || !scale_setup(fb_w, fb_h, dw, dh))
		return;
	/* Across: each of the game's rows to the screen's width */
	for (int y = 0; y < fb_h; y++) {
		const uint32_t *in = fb + y * fb_w;
		uint32_t *out = sc.rows + y * dw;
		for (int x = 0; x < dw; x++) {
			unsigned w = sc.xw[x];
			const uint32_t *p = in + sc.x0[x];
			out[x] = w ? mix(p[0], p[1], w) : p[0];
		}
	}
	/* Down */
	for (int y = 0; y < dh; y++) {
		const uint32_t *a = sc.rows + sc.y0[y] * dw;
		uint32_t *out = c->px + (dy + y) * c->stride + dx;
		unsigned w = sc.yw[y];
		if (w == 0) {
			memcpy(out, a, dw * sizeof(uint32_t));
		} else {
			const uint32_t *b = a + dw;
			for (int x = 0; x < dw; x++)
				out[x] = mix(a[x], b[x], w);
		}
	}
}

static void tables_init(void)
{
	conv565 = malloc(65536 * sizeof(uint32_t));
	conv1555 = malloc(32768 * sizeof(uint32_t));
	if (!conv565 || !conv1555) {
		fprintf(stderr, "s22-emu: out of memory\n");
		exit(1);
	}
	for (int i = 0; i < 65536; i++) {
		unsigned r = i >> 11, g = (i >> 5) & 63, b = i & 31;
		conv565[i] = BLACK | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 |
			     (b << 3 | b >> 2);
	}
	for (int i = 0; i < 32768; i++) {
		unsigned r = (i >> 10) & 31, g = (i >> 5) & 31, b = i & 31;
		conv1555[i] = BLACK | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 |
			      (b << 3 | b >> 2);
	}
}

/* ---- the game list ---- */

static bool is_game(const char *name)
{
	const char *dot = strrchr(name, '.');

	if (dot == NULL)
		return false;
	for (int i = 0; i < 4 && sys->exts[i]; i++)
		if (!strcasecmp(dot, sys->exts[i]))
			return true;
	return false;
}

static int by_name(const void *a, const void *b)
{
	return strcasecmp(a, b);
}

static void games_load(void)
{
	DIR *d = opendir(roms_dir);
	struct dirent *de;

	ngames = 0;
	if (games == NULL)
		games = calloc(MAX_GAMES, sizeof(*games));
	while (d && games && (de = readdir(d)) && ngames < MAX_GAMES)
		if (de->d_name[0] != '.' && is_game(de->d_name))
			snprintf(games[ngames++], sizeof(*games), "%s", de->d_name);
	if (d)
		closedir(d);
	if (ngames)
		qsort(games, ngames, sizeof(*games), by_name);
	pick.count = ngames;
	if (pick.sel >= ngames)
		pick.sel = ngames ? ngames - 1 : 0;
}

static void pick_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
		     int w, int h, bool sel)
{
	char name[256], tag[16] = "";
	const char *dot = strrchr(games[i], '.');

	snprintf(name, sizeof(name), "%.*s", dot ? (int)(dot - games[i]) : 255, games[i]);
	if (dot && !strcasecmp(dot, ".gbc"))
		snprintf(tag, sizeof(tag), "Color");
	else if (dot && !strcasecmp(dot, ".fds"))
		snprintf(tag, sizeof(tag), "Disk");
	int tw = tag[0] ? ui_text_width(UI_SANS, 15, tag) + 16 : 0;
	ui_text_box(c, x + 16, y + 20, w - 32 - tw, UI_SANS, 20, UI_FG, UI_LEFT, name);
	if (tag[0])
		ui_text_box(c, x + w - 16 - tw, y + 24, tw, UI_SANS, 15, UI_DIM, UI_RIGHT, tag);
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

/* ---- the controls ---- */

/*
 * Each button takes up to KEYS_PER keys, one system's layout kept in the
 * setting emu_keys_SYSTEM ("code,code;code;..." in buttons[] order). Back
 * stays the menu's. Turbo A and B press their button every other pair of
 * frames while held (15 times a second).
 */
enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_A, B_B, B_START, B_SELECT, B_TURBO_A,
       B_TURBO_B, NBUTTONS };
#define KEYS_PER 3
static const struct { const char *name; int id; } buttons[NBUTTONS] = {
	{ "Up", RETRO_DEVICE_ID_JOYPAD_UP },
	{ "Down", RETRO_DEVICE_ID_JOYPAD_DOWN },
	{ "Left", RETRO_DEVICE_ID_JOYPAD_LEFT },
	{ "Right", RETRO_DEVICE_ID_JOYPAD_RIGHT },
	{ "A", RETRO_DEVICE_ID_JOYPAD_A },
	{ "B", RETRO_DEVICE_ID_JOYPAD_B },
	{ "Start", RETRO_DEVICE_ID_JOYPAD_START },
	{ "Select", RETRO_DEVICE_ID_JOYPAD_SELECT },
	{ "Turbo A", RETRO_DEVICE_ID_JOYPAD_A },
	{ "Turbo B", RETRO_DEVICE_ID_JOYPAD_B },
};
static const uint16_t default_keys[NBUTTONS][KEYS_PER] = {
	[B_UP] = { KEY_UP, KEY_2 },
	[B_DOWN] = { KEY_DOWN, KEY_8 },
	[B_LEFT] = { KEY_LEFT, KEY_4 },
	[B_RIGHT] = { KEY_RIGHT, KEY_6 },
	[B_A] = { KEY_3, KEY_9, KEY_OK },
	[B_B] = { KEY_1, KEY_7, KEY_BACKSPACE },
	[B_START] = { KEY_NUMERIC_POUND },
	[B_SELECT] = { KEY_NUMERIC_STAR },
};
static uint16_t keys[NBUTTONS][KEYS_PER];

static struct ui_list ctl = { .row_h = 46, .count = NBUTTONS + 1 };
static int capture = -1, captured;	/* the button taking keys, and how many */

static const char *key_name(uint16_t code, char *buf, int len)
{
	switch (code) {
	case KEY_UP: return "Up";
	case KEY_DOWN: return "Down";
	case KEY_LEFT: return "Left";
	case KEY_RIGHT: return "Right";
	case KEY_OK: case KEY_ENTER: return "OK";
	case KEY_BACKSPACE: return "C";
	case KEY_NUMERIC_STAR: return "*";
	case KEY_NUMERIC_POUND: return "#";
	case KEY_PHONE: return "Call";
	case KEY_MICMUTE: return "Speaker";
	case KEY_0: return "0";
	}
	if (code >= KEY_1 && code <= KEY_9)
		snprintf(buf, len, "%d", code - KEY_1 + 1);
	else
		snprintf(buf, len, "Key %u", code);
	return buf;
}

static void keys_setting(char *name, int len)
{
	snprintf(name, len, "emu_keys_%s", sys->key);
}

static void keys_load(void)
{
	char name[32], v[512];
	const char *p;

	memcpy(keys, default_keys, sizeof(keys));
	keys_setting(name, sizeof(name));
	p = ui_setting(name, "", v, sizeof(v));
	if (!p[0])
		return;
	memset(keys, 0, sizeof(keys));
	for (int b = 0; b < NBUTTONS && *p; b++) {
		for (int k = 0; *p && *p != ';'; ) {
			long code = strtol(p, (char **)&p, 10);
			if (code > 0 && code <= KEY_MAX && k < KEYS_PER)
				keys[b][k++] = code;
			if (*p == ',')
				p++;
			else if (*p && *p != ';')
				p++;		/* not a number: skip it */
		}
		if (*p == ';')
			p++;
	}
}

static void keys_save(void)
{
	char name[32], v[512];
	int o = 0;

	for (int b = 0; b < NBUTTONS; b++) {
		for (int k = 0; k < KEYS_PER && keys[b][k]; k++)
			o += snprintf(v + o, sizeof(v) - o, "%s%u", k ? "," : "", keys[b][k]);
		o += snprintf(v + o, sizeof(v) - o, ";");
	}
	keys_setting(name, sizeof(name));
	if (!memcmp(keys, default_keys, sizeof(keys)))
		ui_setting_set(name, NULL);
	else if (!ui_setting_set(name, v))
		snprintf(flash, sizeof(flash), "Could not save the controls");
}

/* Take a key off every button, keeping each list packed */
static void key_unassign(uint16_t code)
{
	for (int b = 0; b < NBUTTONS; b++)
		for (int k = 0; k < KEYS_PER; k++)
			if (keys[b][k] == code) {
				memmove(&keys[b][k], &keys[b][k + 1],
					(KEYS_PER - k - 1) * sizeof(keys[b][0]));
				keys[b][KEYS_PER - 1] = 0;
				k--;
			}
}

static void pad_update(void)
{
	uint16_t p = 0, t = 0;

	for (int b = 0; b < NBUTTONS; b++)
		for (int k = 0; k < KEYS_PER && keys[b][k]; k++)
			if (keydown[keys[b][k]]) {
				if (b >= B_TURBO_A)
					t |= 1 << buttons[b].id;
				else
					p |= 1 << buttons[b].id;
			}
	__atomic_store_n(&turbo, t, __ATOMIC_RELAXED);
	__atomic_store_n(&pad, p, __ATOMIC_RELAXED);
}

static void ctl_row(struct ui *ui, struct ui_canvas *c, int i, int x, int y,
		    int w, int h, bool sel)
{
	char line[64] = "", nb[16];
	int o = 0;

	if (i == NBUTTONS) {
		ui_text_box(c, x + 16, y + 12, w - 32, UI_SANS, 19, UI_ACCENT, UI_LEFT,
			    "Reset to defaults");
		return;
	}
	for (int k = 0; k < KEYS_PER && keys[i][k]; k++)
		o += snprintf(line + o, sizeof(line) - o, "%s%s", k ? "  " : "",
			      key_name(keys[i][k], nb, sizeof(nb)));
	bool taking = i == capture;
	ui_text_box(c, x + 16, y + 12, 130, UI_BOLD, 19, UI_FG, UI_LEFT, buttons[i].name);
	ui_text_box(c, x + 150, y + 12, w - 166, UI_SANS, 19,
		    taking ? UI_ACCENT : line[0] ? UI_DIM : UI_WARN, UI_LEFT,
		    taking && !captured ? "Press a key…" : line[0] ? line : "none");
	ui_fill(c, x, y + h - 1, w, 1, UI_BAR);
}

/* ---- drawing ---- */

static void draw_pick(struct ui *ui, struct ui_canvas *c)
{
	int h = c->h - UI_HEADER_H - UI_HINTS_H;

	ui_header(ui, c, sys->title);
	if (ngames == 0) {
		char t[256];
		snprintf(t, sizeof(t), "No games yet. Put %s in ~/Games/%s on the SD card.",
			 sys->where, sys->dir);
		ui_text_wrap(c, 24, UI_HEADER_H + 40, c->w - 48, UI_SANS, 20, UI_DIM, t);
	} else {
		ui_list_draw(ui, c, &pick, UI_HEADER_H, h, pick_row);
	}
	if (flash[0])
		ui_text_box(c, 16, c->h - UI_HINTS_H - 34, c->w - 32, UI_SANS, 17, UI_WARN,
			    UI_CENTER, flash);
	ui_hints(c, "Quit", ngames ? "Play" : NULL, NULL);
}

static void dim(struct ui_canvas *c)
{
	for (int y = 0; y < c->h; y++) {
		uint32_t *p = c->px + y * c->stride;
		for (int x = 0; x < c->w; x++)
			p[x] = BLACK | ((p[x] >> 2) & 0x3f3f3f);
	}
}

static void draw_keys(struct ui *ui, struct ui_canvas *c)
{
	char title[64];
	int note = 52, h = c->h - UI_HEADER_H - UI_HINTS_H - note;

	snprintf(title, sizeof(title), "%s controls", sys->title);
	ui_header(ui, c, title);
	ui_list_draw(ui, c, &ctl, UI_HEADER_H, h, ctl_row);
	ui_text_wrap(c, 16, UI_HEADER_H + h + 6, c->w - 32, UI_SANS, 15, UI_DIM,
		     capture >= 0 ? "Up to 3 keys; one taken from another button "
				    "leaves it. Back when done." :
		     flash[0] ? flash : "Back is always the menu, the volume keys the volume.");
	if (capture >= 0)
		ui_hints(c, "Done", NULL, NULL);
	else
		ui_hints(c, "Back", ctl.sel == NBUTTONS ? "Reset" : "Change", NULL);
}

static void draw_menu(struct ui *ui, struct ui_canvas *c)
{
	int rh = 52, y = 70;
	bool have_state = state_exists("state");

	dim(c);
	ui_text_box(c, 16, 20, c->w - 32, UI_BOLD, 22, UI_FG, UI_CENTER, game_name);
	for (int i = 0; i < M_COUNT; i++, y += rh) {
		bool off = i == M_LOAD && !have_state;
		if (i == menu_sel)
			ui_round(c, 40, y, c->w - 80, rh - 6, 12, UI_SEL);
		ui_text_box(c, 56, y + 12, c->w - 112, UI_SANS, 21, off ? UI_DIM : UI_FG,
			    UI_LEFT, menu_items[i]);
	}
	if (flash[0])
		ui_text_box(c, 16, y + 10, c->w - 32, UI_SANS, 18, UI_ACCENT, UI_CENTER, flash);
	ui_hints(c, "Resume", "Choose", NULL);
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	if (view == V_PICK || view == V_KEYS) {
		ui_fill(c, 0, 0, c->w, c->h, UI_BG);	/* the app is opaque */
		if (view == V_PICK)
			draw_pick(ui, c);
		else
			draw_keys(ui, c);
		return;
	}
	long long t0 = mono_ns();
	int x, y, w, h;
	pthread_mutex_lock(&frame_lock);
	if (ready_new) {
		struct frame *t = front;
		front = ready;
		ready = t;
		ready_new = false;
	}
	pthread_mutex_unlock(&frame_lock);
	if (front->w && front->h) {
		/* Black around the picture only */
		fit(c, &x, &y, &w, &h);
		ui_fill(c, 0, 0, c->w, y, BLACK);
		ui_fill(c, 0, y + h, c->w, c->h - y - h, BLACK);
		ui_fill(c, 0, y, x, h, BLACK);
		ui_fill(c, x + w, y, c->w - x - w, h, BLACK);
		blit(c);
	} else {
		ui_fill(c, 0, 0, c->w, c->h, BLACK);
	}
	if (view == V_MENU) {
		draw_menu(ui, c);
		return;
	}
	t0 = mono_ns() - t0;
	pthread_mutex_lock(&st_lock);
	st.draws++;
	st.draw_ns += t0;
	if (t0 > st.draw_max)
		st.draw_max = t0;
	pthread_mutex_unlock(&st_lock);
}

/* ---- keys ---- */

static void menu_choose(struct ui *ui)
{
	switch (menu_sel) {
	case M_RESUME:
		resume_game(ui);
		return;
	case M_SAVE:
		snprintf(flash, sizeof(flash), state_save("state") ? "Saved" : "Could not save");
		break;
	case M_LOAD:
		if (!state_exists("state"))
			return;
		if (state_load("state")) {
			resume_game(ui);
			return;
		}
		snprintf(flash, sizeof(flash), "Could not load the state");
		break;
	case M_RESET:
		core.reset();
		resume_game(ui);
		return;
	case M_KEYS:
		flash[0] = 0;
		capture = -1;
		view = V_KEYS;
		break;
	case M_PICK:
		game_close();
		flash[0] = 0;
		games_load();
		view = V_PICK;
		break;
	case M_QUIT:
		ui_quit(ui);
		return;
	}
	ui_redraw(ui);
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st, const char *text)
{
	if (st == UI_REPEATED && view == V_PLAY)
		return;
	if (view == V_PLAY) {
		if (code == KEY_BACK || code == KEY_ESC) {
			if (st == UI_PRESSED)
				pause_game(ui);
			return;
		}
		if (code <= KEY_MAX) {
			keydown[code] = st == UI_PRESSED;
			pad_update();
		}
		return;
	}
	if (st == UI_RELEASED)
		return;
	switch (view) {
	case V_PICK:
		if (code == KEY_BACK || code == KEY_ESC) {
			ui_quit(ui);
		} else if ((code == KEY_ENTER || code == KEY_OK) && ngames) {
			char path[600];
			flash[0] = 0;
			snprintf(path, sizeof(path), "%s/%s", roms_dir, games[pick.sel]);
			game_open(ui, path);
		} else {
			ui_list_key(&pick, code);
		}
		break;
	case V_MENU:
		if (code == KEY_BACK || code == KEY_ESC) {
			resume_game(ui);
			return;
		}
		if (code == KEY_UP || code == KEY_2)
			menu_sel = (menu_sel + M_COUNT - 1) % M_COUNT;
		else if (code == KEY_DOWN || code == KEY_8)
			menu_sel = (menu_sel + 1) % M_COUNT;
		else if (code == KEY_ENTER || code == KEY_OK || code == KEY_5)
			menu_choose(ui);
		break;
	case V_KEYS:
		if (capture >= 0) {
			if (st != UI_PRESSED)
				return;
			if (code == KEY_BACK || code == KEY_ESC) {
				capture = -1;
				keys_save();
			} else if (code > 0 && code <= KEY_MAX) {
				if (captured == 0)
					memset(keys[capture], 0, sizeof(keys[capture]));
				key_unassign(code);
				keys[capture][captured++] = code;
				if (captured == KEYS_PER) {
					capture = -1;
					keys_save();
				}
			}
		} else if (code == KEY_BACK || code == KEY_ESC) {
			flash[0] = 0;
			view = V_MENU;
		} else if (code == KEY_ENTER || code == KEY_OK) {
			flash[0] = 0;
			if (ctl.sel == NBUTTONS) {
				memcpy(keys, default_keys, sizeof(keys));
				keys_save();
			} else if (st == UI_PRESSED) {
				capture = ctl.sel;
				captured = 0;
			}
		} else {
			ui_list_key(&ctl, code);
		}
		break;
	default:
		break;
	}
	ui_redraw(ui);
}

static void focus(struct ui *ui, bool in)
{
	focused = in;
	if (!in)
		pause_game(ui);
}

static void tap(struct ui *ui, int x, int y)
{
	if (view == V_PICK) {
		int i = ui_list_at(&pick, UI_HEADER_H, y);
		if (i >= 0) {
			pick.sel = i;
			key(ui, KEY_OK, UI_PRESSED, "");
		}
	}
}

static void drag(struct ui *ui, int dx, int dy, bool done)
{
	if (view == V_PICK) {
		ui_list_drag(&pick, dy, ui_height(ui) - UI_HEADER_H - UI_HINTS_H);
		ui_redraw(ui);
	}
}

/* SIGTERM, SIGINT, SIGHUP: save and go */
static void on_signal(struct ui *ui, int fd)
{
	struct signalfd_siginfo si;

	if (read(fd, &si, sizeof(si)) == sizeof(si))
		ui_quit(ui);
}

static const char *start_rom;

static void start(struct ui *ui)
{
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGHUP);
	sigprocmask(SIG_BLOCK, &set, NULL);
	int sfd = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
	if (sfd >= 0)
		ui_watch(ui, sfd, on_signal);
	pthread_t t;
	timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	ready_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	for (int i = 0; i < 3; i++)
		frames[i].px = malloc(FB_MAX * sizeof(uint32_t));
	if (timer_fd < 0 || ready_fd < 0 || !frames[0].px || !frames[1].px || !frames[2].px ||
	    pthread_create(&t, NULL, game_thread, NULL) != 0) {
		fprintf(stderr, "s22-emu: cannot start the game thread\n");
		exit(1);
	}
	ui_watch(ui, ready_fd, on_ready);
	keys_load();
	games_load();
	if (start_rom)
		game_open(ui, start_rom);
	ui_redraw(ui);
}

static bool core_load(const char *path)
{
	core.dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (core.dl == NULL) {
		fprintf(stderr, "s22-emu: %s\n", dlerror());
		return false;
	}
#define SYM(field, name) \
	if ((*(void **)&core.field = dlsym(core.dl, name)) == NULL) { \
		fprintf(stderr, "s22-emu: %s lacks %s\n", path, name); \
		return false; \
	}
	SYM(init, "retro_init");
	SYM(deinit, "retro_deinit");
	SYM(set_environment, "retro_set_environment");
	SYM(set_video_refresh, "retro_set_video_refresh");
	SYM(set_audio_sample, "retro_set_audio_sample");
	SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
	SYM(set_input_poll, "retro_set_input_poll");
	SYM(set_input_state, "retro_set_input_state");
	SYM(get_system_info, "retro_get_system_info");
	SYM(get_system_av_info, "retro_get_system_av_info");
	SYM(set_controller_port_device, "retro_set_controller_port_device");
	SYM(reset, "retro_reset");
	SYM(run, "retro_run");
	SYM(serialize_size, "retro_serialize_size");
	SYM(serialize, "retro_serialize");
	SYM(unserialize, "retro_unserialize");
	SYM(load_game, "retro_load_game");
	SYM(unload_game, "retro_unload_game");
	SYM(get_memory_data, "retro_get_memory_data");
	SYM(get_memory_size, "retro_get_memory_size");
#undef SYM
	core.set_environment(core_env);
	core.set_video_refresh(video_refresh);
	core.set_audio_sample(audio_sample);
	core.set_audio_sample_batch(audio_batch);
	core.set_input_poll(input_poll);
	core.set_input_state(input_state);
	core.init();
	return true;
}

int main(int argc, char **argv)
{
	const char *home = getenv("HOME");

	for (size_t i = 0; argc >= 2 && i < sizeof(systems) / sizeof(systems[0]); i++)
		if (!strcmp(argv[1], systems[i].key))
			sys = &systems[i];
	if (sys == NULL || argc > 3) {
		fprintf(stderr, "usage: s22-emu gb|nes [ROM]\n");
		return 2;
	}
	start_rom = argc == 3 ? argv[2] : NULL;
	snprintf(games_dir, sizeof(games_dir), "%s/Games", home ? home : ".");
	snprintf(roms_dir, sizeof(roms_dir), "%s/%s", games_dir, sys->dir);
	snprintf(saves_dir, sizeof(saves_dir), "%s/saves", games_dir);
	snprintf(system_dir, sizeof(system_dir), "%s/system", games_dir);
	mkdir(games_dir, 0755);
	mkdir(roms_dir, 0755);
	mkdir(saves_dir, 0755);
	mkdir(system_dir, 0755);

	st.on = getenv("S22_EMU_STATS") != NULL;
	tables_init();
	if (!core_load(sys->core))
		return 1;

	struct ui_app app = {
		.app_id = sys->app_id,
		.title = sys->title,
		.draw = draw,
		.key = key,
		.start = start,
		.focus = focus,
		.tap = tap,
		.drag = drag,
		.opaque = true,
	};
	int r = ui_main(&app);

	worker_stop();
	game_close();
	audio_close();
	core.deinit();
	return r;
}
