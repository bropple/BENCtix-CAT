// SPDX-License-Identifier: GPL-2.0-only
/*
 * s22-t9d: text input from the Cat S22 Flip's keypad.
 *
 * Grabs the keypad's input device and types through a uinput keyboard
 * (US layout), so the console, getty and anything later see plain typing.
 *
 *   2-9    multi-tap letters (2 = a b c 2); hold for the digit
 *   1      punctuation  . , ? ! - / _ : ; ' "
 *   0      space; hold for 0
 *   *      every symbol, the most used first for what is typed: in a
 *          shell  / - . _ | ~ * $ > & ; : " ' = < # ! ? , @ % ^ + ( ) [ ] { } \ `
 *          prose  . , ? ! ' " - : ; ( ) @ & * / # $ % + = _ ~ ^ | \ < > [ ] { } `
 *          a web address  . / : - _ ? = & % # @ ~ + , ; ! * ' " $ ( ) ...
 *   #      mode: abc -> Abc (next letter capital) -> ABC -> 123
 *   C      Backspace      OK, Call  Enter
 *   Back   Esc            Home      Tab
 *   Recents  Ctrl for the next character or key (Recents, Left: Ctrl+Left);
 *            held, Ctrl-C twice (programs that want it twice to quit)
 *   D-pad and other keys pass through.
 *
 * A multi-tap character is typed at once; tapping the same key again within
 * the timeout replaces it (Backspace, then the next character). With Ctrl
 * armed the character is held back and sent with Ctrl when it is final.
 * The current mode is written to OUTDIR/mode (default /run/s22-t9).
 *
 * Under the GUI (the console in front is in KD_GRAPHICS) the compositor
 * picks how the keypad behaves for the window in front by writing to
 * OUTDIR/gui (group wheel may write): "raw" passes every key through
 * unchanged, for apps that do their own text entry; "text" (the default)
 * types as above, for programs that only know a keyboard, except that Home
 * stays Home (the compositor goes home) and only held is it Tab; "game"
 * passes keys through too, but the keypad's own keys become the PC keys
 * games know (SDL has no scancodes for OK, Call, * or #): OK Enter, Back
 * Escape, Call Left Ctrl, * keypad *, # Tab, Speaker Left Alt. Recents
 * in a game switches the keypad to typing (text mode: a saved game's name,
 * chat) and back; it is the game's keys again when the window changes.
 *
 * On a text console an indicator in the top-right corner shows the mode and,
 * while a key is being tapped, its characters with the current one
 * highlighted. At a password prompt (no echo, line mode) it also
 * shows one '*' per character typed on the line. Only the character being
 * chosen is ever shown, and only until it is final. It clears itself after
 * a few seconds without typing. -q turns it off. Under the GUI's text mode
 * the same goes to OUTDIR/tap for the status bar (s22-statusbar): the mode
 * on the first line, and while a key is being tapped a second line
 * "KEY<TAB>INDEX<TAB>CHARACTERS"; empty when the GUI is not in text mode.
 * It follows OUTDIR/gui (s22-wm's, as the window in front changes), so the
 * mode shows as soon as a text window comes to the front, before a key.
 *
 * The keypad backlight (LED class device, default white:kbd_backlight)
 * comes on at a key press and goes off BACKLIGHT_MS after the last one
 * (default 5000; -b 0 leaves the LED alone).
 *
 *   s22-t9d [-v] [-q] [-d DEVICE] [-t TAP_MS] [-l HOLD_MS] [-o OUTDIR]
 *           [-b BACKLIGHT_MS] [-B LED]
 */
#include <dirent.h>
#include <grp.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define BIT(arr, b)	((arr)[(b) / 8] & (1 << ((b) % 8)))

static int tap_ms = 900;	/* window for the next tap on the same key */
static int hold_ms = 500;	/* a press this long types the digit */
static const char *outdir = "/run/s22-t9";
static int backlight_ms = 5000;	/* keypad light stays on this long */
static const char *backlight_led = "white:kbd_backlight";

enum mode { M_LOWER, M_SHIFT1, M_UPPER, M_DIGITS, M_COUNT };
static const char *mode_name[] = { "abc", "Abc", "ABC", "123" };
static enum mode mode = M_LOWER;

static const char *const taps[10] = {
	" 0", ".,?!-/_:;'\"1", "abc2", "def3", "ghi4",
	"jkl5", "mno6", "pqrs7", "tuv8", "wxyz9",
};
/*
 * Every ASCII symbol on *, the most used first for what is being typed (1
 * has the common punctuation too): the shell's on the console and in a
 * terminal, a web address's in the browser's address bar, else prose's.
 * Each order stays fixed, so a symbol is always the same number of taps
 * away in the same place. The GUI says which in OUTDIR/gui ("text shell",
 * "text url", "text prose").
 */
static const char sym_shell[] = "/-._|~*$>&;:\"'=<#!?,@%^+()[]{}\\`";
static const char sym_prose[] = ".,?!'\"-:;()@&*/#$%+=_~^|\\<>[]{}`";
static const char sym_url[] = "./:-_?=&%#@~+,;!*'\"$()[]{}<>|^\\`";
static const char *symbols = sym_shell;

static int ufd;
static int vt = -1;		/* /dev/tty0, the console in front, for the indicator */
static int kd = -1;		/* /dev/tty0 again, for its mode, even with -q */
static bool dirty;		/* the indicator needs redrawing */
static int typed_on_line;	/* characters typed since the last Enter */
static bool verbose;		/* -v: log every input event and action */

#define dbg(...) do { if (verbose) { fprintf(stderr, "%lld ", now_ms()); fprintf(stderr, __VA_ARGS__); } } while (0)
static long long now_ms(void);

/* -------------------------------------------------------- US keymap --- */

struct kc { unsigned short code; bool shift; };

static struct kc char_key(char c)
{
	static const unsigned short letters[26] = {
		KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
		KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
		KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	};
	static const struct { char c; unsigned short code; bool shift; } sym[] = {
		{ ' ', KEY_SPACE, 0 }, { '.', KEY_DOT, 0 }, { ',', KEY_COMMA, 0 },
		{ '?', KEY_SLASH, 1 }, { '!', KEY_1, 1 }, { '-', KEY_MINUS, 0 },
		{ '/', KEY_SLASH, 0 }, { '_', KEY_MINUS, 1 }, { ':', KEY_SEMICOLON, 1 },
		{ ';', KEY_SEMICOLON, 0 }, { '\'', KEY_APOSTROPHE, 0 },
		{ '"', KEY_APOSTROPHE, 1 }, { '*', KEY_8, 1 }, { '+', KEY_EQUAL, 1 },
		{ '=', KEY_EQUAL, 0 }, { '@', KEY_2, 1 }, { '#', KEY_3, 1 },
		{ '$', KEY_4, 1 }, { '%', KEY_5, 1 }, { '&', KEY_7, 1 },
		{ '|', KEY_BACKSLASH, 1 }, { '~', KEY_GRAVE, 1 }, { '`', KEY_GRAVE, 0 },
		{ '^', KEY_6, 1 }, { '\\', KEY_BACKSLASH, 0 }, { '(', KEY_9, 1 },
		{ ')', KEY_0, 1 }, { '[', KEY_LEFTBRACE, 0 }, { ']', KEY_RIGHTBRACE, 0 },
		{ '{', KEY_LEFTBRACE, 1 }, { '}', KEY_RIGHTBRACE, 1 },
		{ '<', KEY_COMMA, 1 }, { '>', KEY_DOT, 1 },
	};
	if (c >= 'a' && c <= 'z')
		return (struct kc){ letters[c - 'a'], 0 };
	if (c >= 'A' && c <= 'Z')
		return (struct kc){ letters[c - 'A'], 1 };
	if (c >= '1' && c <= '9')
		return (struct kc){ KEY_1 + (c - '1'), 0 };
	if (c == '0')
		return (struct kc){ KEY_0, 0 };
	for (size_t i = 0; i < sizeof(sym) / sizeof(sym[0]); i++)
		if (sym[i].c == c)
			return (struct kc){ sym[i].code, sym[i].shift };
	return (struct kc){ 0, 0 };
}

/* ----------------------------------------------------------- output --- */

static void emit(unsigned short type, unsigned short code, int value)
{
	struct input_event ev = { .type = type, .code = code, .value = value };

	if (write(ufd, &ev, sizeof(ev)) != sizeof(ev))
		perror("s22-t9d: uinput write");
}

static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }

static void tap_key(unsigned short code, bool shift, bool ctrl)
{
	if (ctrl) { emit(EV_KEY, KEY_LEFTCTRL, 1); syn(); }
	if (shift) { emit(EV_KEY, KEY_LEFTSHIFT, 1); syn(); }
	emit(EV_KEY, code, 1); syn();
	emit(EV_KEY, code, 0); syn();
	if (shift) { emit(EV_KEY, KEY_LEFTSHIFT, 0); syn(); }
	if (ctrl) { emit(EV_KEY, KEY_LEFTCTRL, 0); syn(); }
}

static void type_char(char c, bool ctrl)
{
	struct kc k = char_key(c);

	dbg("type '%c'%s\n", c, ctrl ? " +ctrl" : "");
	if (k.code)
		tap_key(k.code, k.shift, ctrl);
}

static void write_mode(void)
{
	char path[256], tmp[260];
	FILE *f;

	snprintf(path, sizeof(path), "%s/mode", outdir);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "%s\n", mode_name[mode]);
	fclose(f);
	rename(tmp, path);
}

/* --------------------------------------------------------- multi-tap --- */

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static struct {
	int key;		/* KEY_x of the pending character, 0 if none */
	int index;		/* position in its tap list */
	long long until;	/* the next tap replaces it until then */
	bool typed;		/* already on screen (not held back for Ctrl) */
} pend;

static bool ctrl_armed;
static unsigned short ctrl_chord;	/* a key sent as Ctrl+key, until released */

/* A digit typed by holding its key, shown highlighted like a tap */
static struct {
	char c;			/* 0 if nothing to show */
	long long until;
} flash;

static const char *tap_list(int key)
{
	if (key == KEY_NUMERIC_STAR)
		return symbols;
	if (key >= KEY_1 && key <= KEY_9)
		return taps[key - KEY_1 + 1];
	if (key == KEY_0)
		return taps[0];
	return NULL;
}

static char apply_case(char c)
{
	if ((mode == M_SHIFT1 || mode == M_UPPER) && c >= 'a' && c <= 'z')
		return c - 'a' + 'A';
	return c;
}

static char pending_char(void)
{
	return apply_case(tap_list(pend.key)[pend.index]);
}

/* The pending character is final: send it if it was held back for Ctrl */
static void commit(void)
{
	if (!pend.key)
		return;
	if (!pend.typed) {
		type_char(pending_char(), ctrl_armed);
		ctrl_armed = false;
	}
	if (mode == M_SHIFT1) {
		char c = tap_list(pend.key)[pend.index];
		if (c >= 'a' && c <= 'z') {
			mode = M_LOWER;
			write_mode();
		}
	}
	pend.key = 0;
}

static void start_char(int key, int index)
{
	pend.key = key;
	pend.index = index;
	pend.until = now_ms() + tap_ms;
	pend.typed = !ctrl_armed;
	if (pend.typed) {
		type_char(pending_char(), false);
		typed_on_line++;
	}
}

static void tap(int key)
{
	const char *list = tap_list(key);
	int n = strlen(list);

	if (pend.key == key && now_ms() < pend.until) {
		/* Same key again: replace the pending character */
		if (pend.typed) {
			dbg("replace\n");
			tap_key(KEY_BACKSPACE, false, false);
		}
		pend.index = (pend.index + 1) % n;
		pend.until = now_ms() + tap_ms;
		if (pend.typed)
			type_char(pending_char(), false);
		return;
	}
	commit();
	flash.c = 0;
	start_char(key, 0);
}

/* A held digit key types the digit itself */
static void hold(int key)
{
	char c = key == KEY_0 ? '0' : '1' + (key - KEY_1);

	commit();
	type_char(c, ctrl_armed);
	if (!ctrl_armed)
		typed_on_line++;
	ctrl_armed = false;
	flash.c = c;
	flash.until = now_ms() + tap_ms;
	dirty = true;
}

/* --------------------------------------------------------- indicator --- */

#define IND_IDLE_MS	3000

static int ind_len;		/* width of what is on screen now */
static long long ind_clear_at;

static bool text_console(void)
{
	int m;

	return vt >= 0 && ioctl(vt, KDGETMODE, &m) == 0 && m == KD_TEXT;
}

/*
 * A password prompt: the terminal does not echo but still reads whole lines
 * (login, sudo, passwd, read -s). Line editors and full-screen programs
 * (readline, zsh, nano) also turn echo off, but they leave canonical mode.
 */
static bool password_prompt(void)
{
	struct termios t;

	return vt >= 0 && tcgetattr(vt, &t) == 0 &&
	       !(t.c_lflag & ECHO) && (t.c_lflag & ICANON);
}

static int columns(void)
{
	struct winsize ws;

	if (ioctl(vt, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20)
		return ws.ws_col;
	return 60;
}

static void vt_write(const char *buf, size_t len)
{
	if (write(vt, buf, len) < 0)
		perror("s22-t9d: console");
}

/* Overwrite the indicator area with spaces */
static void indicator_clear(void)
{
	char buf[256];
	int n;

	if (!ind_len || !text_console())
		return;
	n = snprintf(buf, sizeof(buf), "\0337\033[1;%dH%*s\0338",
		     columns() - ind_len + 1, ind_len, "");
	vt_write(buf, n);
	ind_len = 0;
}

static void indicator(void)
{
	char text[160], buf[512];
	int w = 0, cols, pad, n, sel = -1;

	if (!text_console())
		return;
	/* The visible text; sel marks the highlighted character */
	w += snprintf(text + w, sizeof(text) - w, " %s", mode_name[mode]);
	if (ctrl_armed)
		w += snprintf(text + w, sizeof(text) - w, " ^");
	if (password_prompt()) {
		int k = typed_on_line > 16 ? 16 : typed_on_line;
		w += snprintf(text + w, sizeof(text) - w, " pw ");
		while (k--)
			text[w++] = '*';
		text[w] = 0;
	}
	if (pend.key) {
		const char *list = tap_list(pend.key);
		w += snprintf(text + w, sizeof(text) - w, " %c:",
			      pend.key == KEY_NUMERIC_STAR ? '*' :
			      pend.key == KEY_0 ? '0' : '1' + (pend.key - KEY_1));
		for (int i = 0; list[i] && w < (int)sizeof(text) - 2; i++) {
			char c = apply_case(list[i]);
			if (i == pend.index)
				sel = w;
			text[w++] = c == ' ' ? '_' : c;
		}
		text[w] = 0;
	} else if (flash.c) {
		w += snprintf(text + w, sizeof(text) - w, " %c:", flash.c);
		sel = w;
		text[w++] = flash.c;
		text[w] = 0;
	}
	text[w++] = ' ';
	text[w] = 0;

	cols = columns();
	if (w > cols)
		return;
	pad = ind_len > w ? ind_len - w : 0;
	n = snprintf(buf, sizeof(buf), "\0337\033[1;%dH%*s\033[7m",
		     cols - w - pad + 1, pad, "");
	for (int i = 0; i < w && n < (int)sizeof(buf) - 16; i++) {
		if (i == sel)
			n += snprintf(buf + n, sizeof(buf) - n, "\033[27m%c\033[7m", text[i]);
		else
			buf[n++] = text[i];
	}
	n += snprintf(buf + n, sizeof(buf) - n, "\033[0m\0338");
	vt_write(buf, n);
	ind_len = w;
	ind_clear_at = now_ms() + IND_IDLE_MS;
}

/* ------------------------------------------------- the GUI's indicator --- */

static enum front_kind { FK_CONSOLE, FK_GUI_TEXT, FK_GUI_RAW } front_kind(void);

/* OUTDIR/tap, rewritten only when it changes */
static void gui_indicator(void)
{
	static char last[192] = "\1";
	char text[192] = "", path[256], tmp[260];
	int w = 0;
	FILE *f;

	if (front_kind() == FK_GUI_TEXT) {
		w = snprintf(text, sizeof(text), "%s%s\n", mode_name[mode],
			     ctrl_armed ? " ^" : "");
		if (pend.key) {
			const char *list = tap_list(pend.key);
			w += snprintf(text + w, sizeof(text) - w, "%c\t%d\t",
				      pend.key == KEY_NUMERIC_STAR ? '*' :
				      pend.key == KEY_0 ? '0' : '1' + (pend.key - KEY_1),
				      pend.index);
			for (int i = 0; list[i] && w < (int)sizeof(text) - 2; i++)
				text[w++] = apply_case(list[i]);
			text[w++] = '\n';
			text[w] = 0;
		} else if (flash.c) {
			snprintf(text + w, sizeof(text) - w, "%c\t0\t%c\n", flash.c, flash.c);
		}
	}
	if (strcmp(text, last) == 0)
		return;
	snprintf(path, sizeof(path), "%s/tap", outdir);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fputs(text, f);
	fclose(f);
	rename(tmp, path);
	snprintf(last, sizeof(last), "%s", text);
}

/* ------------------------------------------------------ key handling --- */

static int held_key;		/* a tap key being held, for long press */
static long long held_since;
static bool held_done;		/* the long press already typed */

static unsigned short passthrough(unsigned short code)
{
	switch (code) {
	case KEY_PHONE:		return KEY_ENTER;
	case KEY_BACK:		return KEY_ESC;
	case KEY_HOMEPAGE:	return KEY_TAB;
	default:		return code;
	}
}

/* What is in front: the console, or the GUI wanting text, raw keys or a
 * game's */
enum front { F_CONSOLE, F_GUI_TEXT, F_GUI_RAW, F_GUI_GAME };

/* A game typing (Recents): text mode until Recents again, or until the
 * compositor rewrites OUTDIR/gui (another window, or the game's again) */
static bool game_text;
static struct timespec game_text_gui;

static enum front front(void)
{
	char path[256], buf[32] = "";
	int m, fd;

	if (kd < 0 || ioctl(kd, KDGETMODE, &m) != 0 || m != KD_GRAPHICS) {
		symbols = sym_shell;
		return F_CONSOLE;
	}
	snprintf(path, sizeof(path), "%s/gui", outdir);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return F_GUI_TEXT;
	if (read(fd, buf, sizeof(buf) - 1) < 0)
		buf[0] = 0;
	struct stat st = { 0 };
	bool same = fstat(fd, &st) == 0 &&
		    st.st_mtim.tv_sec == game_text_gui.tv_sec &&
		    st.st_mtim.tv_nsec == game_text_gui.tv_nsec;
	close(fd);
	if (game_text && (!same || strncmp(buf, "game", 4) != 0))
		game_text = false;
	if (game_text)
		return F_GUI_TEXT;
	game_text_gui = st.st_mtim;
	/* "text shell", "text url", else prose (a plain "text" too) */
	symbols = strstr(buf, " shell") ? sym_shell : strstr(buf, " url") ? sym_url :
		  sym_prose;
	return strncmp(buf, "raw", 3) == 0 ? F_GUI_RAW :
	       strncmp(buf, "game", 4) == 0 ? F_GUI_GAME : F_GUI_TEXT;
}

static enum front_kind front_kind(void)
{
	enum front f = front();

	return f == F_GUI_TEXT ? FK_GUI_TEXT :
	       f == F_GUI_RAW || f == F_GUI_GAME ? FK_GUI_RAW : FK_CONSOLE;
}

/* Home under the GUI's text mode: Home when tapped, Tab when held */
static bool home_held, home_done;
static long long home_since;

/* Recents held: Ctrl-C twice */
static bool recents_held, recents_done;
static long long recents_since;

static void gui_init(void)
{
	char path[256];
	struct group *g = getgrnam("wheel");
	FILE *f;

	snprintf(path, sizeof(path), "%s/gui", outdir);
	f = fopen(path, "w");
	if (!f)
		return;
	fputs("text\n", f);
	fclose(f);
	if (g && chown(path, 0, g->gr_gid) == 0)
		chmod(path, 0664);
}

static unsigned char raw_down[KEY_MAX / 8 + 1];	/* pressed in raw mode */
static unsigned char game_down[KEY_MAX / 8 + 1];	/* ... in game mode */

/* Game mode: the keypad's own keys as PC keys */
static unsigned short game_key(unsigned short code)
{
	switch (code) {
	case KEY_OK: return KEY_ENTER;
	case KEY_BACK: return KEY_ESC;
	case KEY_PHONE: return KEY_LEFTCTRL;
	case KEY_NUMERIC_STAR: return KEY_KPASTERISK;
	case KEY_NUMERIC_POUND: return KEY_TAB;
	case KEY_MICMUTE: return KEY_LEFTALT;
	}
	return code;
}

static bool is_tap_key(unsigned short code)
{
	return (code >= KEY_1 && code <= KEY_0) || code == KEY_NUMERIC_STAR;
}

static void key_event(unsigned short code, int value)
{
	dbg("key %u %d (held %d, pending %d)\n", code, value, held_key, pend.key);
	/* A key pressed in raw (game) mode is released in raw (game) mode */
	enum front f = value == 1 ? front() : F_CONSOLE;
	/* Recents in a game: typing, or the game's keys again (the release
	 * is Recents', below) */
	if (code == KEY_APPSELECT && value == 1 && (f == F_GUI_GAME || game_text)) {
		commit();
		game_text = !game_text;
		dirty = true;
		return;
	}
	if (f == F_GUI_RAW || f == F_GUI_GAME) {
		commit();
		held_key = 0;
		raw_down[code / 8] |= 1 << (code % 8);
		if (f == F_GUI_GAME)
			game_down[code / 8] |= 1 << (code % 8);
		else
			game_down[code / 8] &= ~(1 << (code % 8));
	}
	if (BIT(raw_down, code)) {
		unsigned short out = BIT(game_down, code) ? game_key(code) : code;

		if (value == 0)
			raw_down[code / 8] &= ~(1 << (code % 8));
		if (value != 2) {
			emit(EV_KEY, out, value);
			syn();
		}
		return;
	}
	if (value != 2)
		dirty = true;
	if (code == KEY_HOMEPAGE && (f == F_GUI_TEXT || home_held)) {
		if (value == 1) {
			commit();
			home_held = true;
			home_done = false;
			home_since = now_ms();
		} else if (value == 0) {
			home_held = false;
			if (!home_done) {
				emit(EV_KEY, KEY_HOMEPAGE, 1);
				syn();
				emit(EV_KEY, KEY_HOMEPAGE, 0);
				syn();
			}
		}
		return;
	}
	if (code == KEY_NUMERIC_POUND) {
		if (value == 1) {
			commit();
			mode = (mode + 1) % M_COUNT;
			write_mode();
		}
		return;
	}
	if (code == KEY_APPSELECT) {
		if (value == 1) {
			commit();
			ctrl_armed = !ctrl_armed;
			recents_held = true;
			recents_done = false;
			recents_since = now_ms();
		} else if (value == 0) {
			recents_held = false;
		}
		return;
	}
	if (is_tap_key(code)) {
		if (value == 1) {
			held_key = code;
			held_since = now_ms();
			held_done = false;
		} else if (value == 0 && code == held_key) {
			held_key = 0;
			if (held_done)
				return;
			if (mode == M_DIGITS && code != KEY_NUMERIC_STAR) {
				commit();
				hold(code);	/* digits mode: the digit itself */
			} else {
				tap(code);
			}
		}
		return;
	}
	/* Everything else: finish any pending character, then pass through;
	 * with Ctrl armed, as a Ctrl chord (its release is the chord's) */
	if (value == 1 && ctrl_armed) {
		commit();
		ctrl_armed = false;
		ctrl_chord = code;
		tap_key(passthrough(code), false, true);
		return;
	}
	if (code == ctrl_chord) {
		if (value == 0)
			ctrl_chord = 0;
		return;
	}
	if (value == 1) {
		commit();
		if (code == KEY_BACKSPACE && typed_on_line > 0)
			typed_on_line--;
		if (code == KEY_ENTER || code == KEY_PHONE)
			typed_on_line = 0;
	}
	if (value == 2)
		return;		/* the uinput device repeats by itself */
	emit(EV_KEY, passthrough(code), value);
	syn();
}

/* --------------------------------------------------------- backlight --- */
static int bl_fd = -1;
static char bl_max[16] = "1";
static bool bl_on;
static long long bl_until;

static void backlight_open(void)
{
	char p[256];
	int fd, n;

	if (!backlight_ms)
		return;
	snprintf(p, sizeof(p), "/sys/class/leds/%s/max_brightness", backlight_led);
	fd = open(p, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		n = read(fd, bl_max, sizeof(bl_max) - 1);
		bl_max[n > 0 ? n : 0] = 0;
		bl_max[strcspn(bl_max, "\n")] = 0;
		close(fd);
	}
	snprintf(p, sizeof(p), "/sys/class/leds/%s/brightness", backlight_led);
	bl_fd = open(p, O_WRONLY | O_CLOEXEC);
	if (bl_fd < 0)
		fprintf(stderr, "s22-t9d: %s: %s (no keypad backlight)\n", p, strerror(errno));
}

static void backlight(bool on)
{
	const char *v = on ? bl_max : "0";

	if (bl_fd < 0 || on == bl_on)
		return;
	if (pwrite(bl_fd, v, strlen(v), 0) < 0)
		dbg("backlight: %s\n", strerror(errno));
	bl_on = on;
}

/* A key press: light the keypad, or keep it lit */
static void backlight_poke(void)
{
	if (bl_fd < 0)
		return;
	backlight(true);
	bl_until = now_ms() + backlight_ms;
}

static void timers(void)
{
	long long t = now_ms();

	if (held_key && !held_done && held_key != KEY_NUMERIC_STAR &&
	    t - held_since >= hold_ms) {
		hold(held_key);
		held_done = true;
	}
	if (home_held && !home_done && t - home_since >= hold_ms) {
		tap_key(KEY_TAB, false, false);
		home_done = true;
	}
	if (recents_held && !recents_done && t - recents_since >= hold_ms) {
		/* Apart, so a program reading the terminal sees two presses */
		ctrl_armed = false;
		tap_key(KEY_C, false, true);
		usleep(150000);
		tap_key(KEY_C, false, true);
		typed_on_line = 0;
		recents_done = true;
		dirty = true;
	}
	if (pend.key && t >= pend.until) {
		commit();
		dirty = true;
	}
	if (flash.c && t >= flash.until) {
		flash.c = 0;
		dirty = true;
	}
	if (ind_len && !pend.key && !flash.c && t >= ind_clear_at)
		indicator_clear();
	if (bl_on && t >= bl_until)
		backlight(false);
}

static int next_timeout(void)
{
	long long t = now_ms(), next = -1;

	if (held_key && !held_done)
		next = held_since + hold_ms;
	if (home_held && !home_done && (next < 0 || home_since + hold_ms < next))
		next = home_since + hold_ms;
	if (recents_held && !recents_done && (next < 0 || recents_since + hold_ms < next))
		next = recents_since + hold_ms;
	if (pend.key && (next < 0 || pend.until < next))
		next = pend.until;
	if (ind_len && (next < 0 || ind_clear_at < next))
		next = ind_clear_at;
	if (flash.c && (next < 0 || flash.until < next))
		next = flash.until;
	if (bl_on && (next < 0 || bl_until < next))
		next = bl_until;
	if (next < 0)
		return -1;
	return next > t ? (int)(next - t) : 0;
}

/* ------------------------------------------------------------- setup --- */

/* The keypad: the device with both KEY_NUMERIC_POUND and KEY_2 */
static int open_keypad(const char *path)
{
	unsigned char keys[KEY_MAX / 8 + 1];
	char p[300];
	struct dirent *de;
	DIR *d;
	int fd;

	if (path)
		return open(path, O_RDONLY | O_CLOEXEC);
	d = opendir("/dev/input");
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(p, sizeof(p), "/dev/input/%s", de->d_name);
		fd = open(p, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(keys, 0, sizeof(keys));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 &&
		    BIT(keys, KEY_NUMERIC_POUND) && BIT(keys, KEY_2)) {
			closedir(d);
			return fd;
		}
		close(fd);
	}
	closedir(d);
	errno = ENODEV;
	return -1;
}

static int open_uinput(const unsigned char *srckeys)
{
	struct uinput_setup us = { .id = { .bustype = BUS_VIRTUAL, .vendor = 0x5332,
				   .product = 0x0009, .version = 1 } };
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		return -1;
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_REP);	/* kernel autorepeat */
	/* Everything a US keyboard can type, plus the keypad's own keys */
	for (int k = KEY_ESC; k <= KEY_KPDOT; k++)
		ioctl(fd, UI_SET_KEYBIT, k);
	for (int k = 0; k < KEY_MAX; k++)
		if (BIT(srckeys, k))
			ioctl(fd, UI_SET_KEYBIT, k);
	strcpy(us.name, "S22 Flip keypad (T9)");
	if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static volatile sig_atomic_t quit;
static void on_signal(int sig) { (void)sig; quit = 1; }

int main(int argc, char **argv)
{
	unsigned char keys[KEY_MAX / 8 + 1] = { 0 };
	const char *dev = NULL;
	struct input_event ev;
	bool quiet = false;
	int kfd, opt;

	while ((opt = getopt(argc, argv, "vqd:t:l:o:b:B:")) != -1) {
		switch (opt) {
		case 'v': verbose = true; break;
		case 'q': quiet = true; break;
		case 'd': dev = optarg; break;
		case 't': tap_ms = atoi(optarg); break;
		case 'l': hold_ms = atoi(optarg); break;
		case 'o': outdir = optarg; break;
		case 'b': backlight_ms = atoi(optarg); break;
		case 'B': backlight_led = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-v] [-q] [-d DEVICE] [-t TAP_MS] [-l HOLD_MS] [-o OUTDIR] [-b BACKLIGHT_MS] [-B LED]\n", argv[0]);
			return 2;
		}
	}

	kfd = open_keypad(dev);
	if (kfd < 0) {
		perror("s22-t9d: keypad");
		return 1;
	}
	ioctl(kfd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
	ufd = open_uinput(keys);
	if (ufd < 0) {
		perror("s22-t9d: uinput");
		return 1;
	}
	if (ioctl(kfd, EVIOCGRAB, 1) < 0) {
		perror("s22-t9d: grab");
		return 1;
	}
	if (!quiet)
		vt = open("/dev/tty0", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	kd = open("/dev/tty0", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	mkdir(outdir, 0755);
	write_mode();
	gui_init();
	backlight_open();
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	fprintf(stderr, "s22-t9d: keypad grabbed; typing as \"S22 Flip keypad (T9)\"\n");

	/* s22-wm rewrites OUTDIR/gui as the window in front changes: the
	 * status bar's mode follows at once, not at the next key */
	int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ifd >= 0 && inotify_add_watch(ifd, outdir, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
		close(ifd);
		ifd = -1;
	}

	while (!quit) {
		struct pollfd pfd[2] = { { .fd = kfd, .events = POLLIN },
					 { .fd = ifd, .events = POLLIN } };
		int r = poll(pfd, ifd >= 0 ? 2 : 1, next_timeout());

		if (r < 0 && errno != EINTR)
			break;
		if (r > 0 && ifd >= 0 && (pfd[1].revents & POLLIN)) {
			char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
			ssize_t len;
			bool gui = false;
			while ((len = read(ifd, buf, sizeof(buf))) > 0)
				for (char *p = buf; p < buf + len;) {
					struct inotify_event *e = (struct inotify_event *)p;
					if (e->len && strcmp(e->name, "gui") == 0)
						gui = true;
					p += sizeof(*e) + e->len;
				}
			if (gui)
				gui_indicator();
		}
		if (r > 0 && (pfd[0].revents & (POLLIN | POLLERR | POLLHUP))) {
			ssize_t n = read(kfd, &ev, sizeof(ev));
			if (n != sizeof(ev)) {
				if (n < 0 && errno == ENODEV)
					break;
				continue;
			}
			if (ev.type == EV_KEY) {
				if (ev.value)
					backlight_poke();
				key_event(ev.code, ev.value);
			}
		}
		timers();
		if (dirty) {
			indicator();
			gui_indicator();
			dirty = false;
		}
	}
	commit();
	indicator_clear();
	backlight(false);
	ioctl(kfd, EVIOCGRAB, 0);
	ioctl(ufd, UI_DEV_DESTROY);
	return 0;
}
