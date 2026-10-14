/* SPDX-License-Identifier: MIT */
/*
 * s22ui: the small UI library for the Cat S22 Flip's own apps.
 *
 * One fullscreen window per app (s22-wm sizes it), drawn in software into
 * shared memory, 32-bit XRGB. Keys arrive as evdev codes (KEY_UP, KEY_1,
 * KEY_BACK ... from <linux/input-event-codes.h>): s22-wm puts the keypad in
 * raw mode for app_ids starting "s22-", so digit keys are digits and the
 * app does its own text entry.
 *
 * An app fills in struct ui_app and calls ui_main(). Everything runs in
 * one thread: draw() whenever the window needs it (after ui_redraw()),
 * key() for each press, release and repeat, tick() when the timer set with
 * ui_timer() fires, and a callback per watched file descriptor.
 */
#ifndef S22UI_H
#define S22UI_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <linux/input-event-codes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ui;

/* A surface to draw on; px[y * stride + x], 0xAARRGGBB */
struct ui_canvas {
	uint32_t *px;
	int w, h, stride;
};

enum ui_key_state { UI_RELEASED, UI_PRESSED, UI_REPEATED };

struct ui_app {
	const char *app_id;	/* "s22-..." */
	const char *title;
	void (*draw)(struct ui *ui, struct ui_canvas *c);
	/* text: what the key types under the keymap, "" if nothing */
	void (*key)(struct ui *ui, uint32_t code, enum ui_key_state state,
		    const char *text);
	void (*tick)(struct ui *ui);
	void (*start)(struct ui *ui);	/* connected, before the first draw */
	void (*focus)(struct ui *ui, bool focused);	/* keyboard came or went */
	/*
	 * Touch, as gestures of the first finger: a tap where it lifted
	 * without moving, or a drag by (dx, dy) since the last call, with
	 * done set when the finger lifts. Navigation stays on the keys
	 * (Back, Home, Recents): touch only picks and scrolls.
	 */
	void (*tap)(struct ui *ui, int x, int y);
	void (*drag)(struct ui *ui, int dx, int dy, bool done);
	/* Every finger as it is, before any gesture: down, moved, up */
	void (*touch)(struct ui *ui, int id, enum ui_key_state st, int x, int y);
	void *data;
	/*
	 * An overlay instead of a window (s22-shell): nothing shows until
	 * ui_layer_show(), at the top of the screen, above the windows.
	 */
	bool layer;
	/*
	 * The session lock (ext-session-lock-v1, s22-lock): the compositor
	 * shows only this until ui_unlock(); it stays locked if the program
	 * dies instead.
	 */
	bool lock;
	/* draw() paints every pixel itself: no background fill before it
	 * (s22-emu, a full frame 60 times a second) */
	bool opaque;
	/*
	 * Links: "scheme:rest" strings one app hands another (sms:NUMBER opens
	 * that conversation in Messages). The .app file's Handles= lists the
	 * schemes; open() gets each link, the one the app was started with
	 * (set uri from argv[1]) after start(), later ones as they come.
	 */
	void (*open)(struct ui *ui, const char *uri);
	const char *uri;
};

int ui_main(struct ui_app *app);	/* returns when ui_quit() is called */
void ui_quit(struct ui *ui);
/* For a layer app: show at the top, height px tall (0: the whole screen),
 * with the keyboard or without; again to change; hide to remove */
void ui_layer_show(struct ui *ui, int height, bool keyboard);
/* A bar at the top that windows keep clear of (s22-statusbar): below the
 * overlays, height px reserved, never the keyboard */
void ui_layer_bar(struct ui *ui, int height);
void ui_layer_hide(struct ui *ui);
/* A lock app: unlock the session and quit */
void ui_unlock(struct ui *ui);
/* Keep the display on while this window is up (idle-inhibit: a game, a
 * video), or let it go */
void ui_inhibit_idle(struct ui *ui, bool on);
/* The window is being shown: the compositor answers its frames (not with
 * the display off, the lid closed, or another window over it) */
bool ui_visible(struct ui *ui);
bool ui_layer_shown(struct ui *ui);
void ui_redraw(struct ui *ui);
void *ui_data(struct ui *ui);
int ui_width(struct ui *ui);
int ui_height(struct ui *ui);

/* tick() after ms milliseconds, once; 0 cancels */
void ui_timer(struct ui *ui, int ms);
/* cb(ui, fd) when fd is readable; up to 8 */
bool ui_watch(struct ui *ui, int fd, void (*cb)(struct ui *ui, int fd));
void ui_unwatch(struct ui *ui, int fd);

/* Start a program, detached; cmd goes to /bin/sh -c */
void ui_spawn(const char *cmd);
/* A command to s22-wm's control socket; the reply in buf (may be NULL) */
bool ui_wm(const char *cmd, char *buf, int len);

/*
 * Installed apps: .app files in /usr/share/s22/apps, /home/.pacman-sd/apps
 * (pacman-sd's, for the SD card's programs) and ~/.local/share/s22/apps
 * (a later one wins for the same file name):
 *     Name=Messages       Exec=s22-messages   Id=s22-messages (its app_id)
 *     Icon=✉ (a character)  Color=#1565c0     Order=20 (grid place)
 *     Notify=sms          notifications from this source open it
 *     Badge=messages      the tile shows what waits (ui_badge)
 *     Hidden=yes          not on the home grid (opened from elsewhere)
 *     Folder=Games        in that folder on the home grid, not on it
 */
struct ui_appinfo {
	char file[64], name[64], exec[256], id[64], icon[16], notify[64];
	char handles[64];	/* link schemes, comma separated */
	char folder[32];	/* Folder=: in a folder of the home grid */
	char badge[16];		/* Badge=: its count on the tile (ui_badge) */
	bool hidden;		/* Hidden=yes: not on the home grid */
	uint32_t color;
	int order;
};
/* Up to max apps, by Order then name; returns how many */
int ui_apps(struct ui_appinfo *apps, int max);
/* Raise it if it runs, else start it */
void ui_open_app(const struct ui_appinfo *app);
/* The app for a notification source; false if none */
bool ui_app_for(const char *source, struct ui_appinfo *app);
/* Hand a link to the app that handles its scheme, starting or raising it;
 * false if no app does or the link has characters links may not have */
bool ui_open_uri(const char *uri);

/*
 * Notifications (s22-notify): files in UI_NOTIFY_DIR with source= and
 * text= lines. Dismissing removes the alert only, never the message.
 */
#define UI_NOTIFY_DIR	"/run/s22-notify"
void ui_notify_dismiss(const char *name);
/* Every notification from source whose text starts with prefix */
void ui_notify_dismiss_matching(const char *source, const char *prefix);

/*
 * Badges (s22badges.c, D075): what waits in an app, kept after its
 * notification is dismissed. "messages": unread texts (s22-messages' read
 * marks); "calls": missed calls since Phone's recent calls were last shown.
 * Counted again only when their files change; ui_badge_watch() is an
 * inotify descriptor that is readable when one may have.
 */
int ui_badge(const char *kind);
int ui_badge_watch(void);
void ui_calls_seen(void);		/* Phone showed its recent calls */
/* s22-smsd's numbers and times, as Messages and the badge read them: the
 * same number with or without the country code; "2026-10-05T15:19:59+00:00" */
bool ui_sms_same_number(const char *a, const char *b);
time_t ui_sms_time(const char *s);

/*
 * Settings shared by the phone's programs: UI_SETTINGS_FILE, "key=value"
 * lines (group wheel may write). ui_setting() copies the value into buf
 * (def if unset) and returns buf; ui_setting_set() rewrites the file with
 * the one key changed, or removed if value is NULL.
 */
#define UI_SETTINGS_FILE	"/var/lib/s22/settings"
const char *ui_setting(const char *key, const char *def, char *buf, int len);
int ui_setting_int(const char *key, int def);
bool ui_setting_set(const char *key, const char *value);
/* The time as the user likes it: "3:07 PM" or "15:07" */
void ui_clock(char *buf, int len);

/* ---- drawing ---- */

/*
 * Colors come from the theme named by the "theme" setting (default dark):
 * /usr/share/s22/themes/NAME.theme, "key=#rrggbb" lines with the keys
 * below. Draw with these, never with literal colors. The theme is re-read
 * when the settings file changes, before a draw.
 */
struct ui_theme {
	uint32_t bg, fg, dim, accent, bar, sel, ok, warn;
};
extern struct ui_theme ui_theme;
#define UI_THEMES_DIR	"/usr/share/s22/themes"

#define UI_BG		(ui_theme.bg)	/* the background */
#define UI_FG		(ui_theme.fg)	/* text */
#define UI_DIM		(ui_theme.dim)	/* secondary text */
#define UI_ACCENT	(ui_theme.accent) /* selection ring, highlights */
#define UI_BAR		(ui_theme.bar)	/* header and hints bars, lines */
#define UI_SEL		(ui_theme.sel)	/* the selected row */
#define UI_OK		(ui_theme.ok)	/* calls, good news */
#define UI_WARN		(ui_theme.warn)	/* errors, bad news */

void ui_fill(struct ui_canvas *c, int x, int y, int w, int h, uint32_t color);
void ui_round(struct ui_canvas *c, int x, int y, int w, int h, int r,
	      uint32_t color);

enum ui_font { UI_SANS, UI_BOLD, UI_MONO };
enum ui_align { UI_LEFT, UI_CENTER, UI_RIGHT };

/* Text with its top left at x, y; returns the width drawn */
int ui_text(struct ui_canvas *c, int x, int y, enum ui_font font, int size,
	    uint32_t color, const char *utf8);
int ui_text_width(enum ui_font font, int size, const char *utf8);
/* One line in a box of width w, aligned, cut with "…" if too long */
void ui_text_box(struct ui_canvas *c, int x, int y, int w, enum ui_font font,
		 int size, uint32_t color, enum ui_align align, const char *utf8);
/* Wrapped at spaces to width w; returns the height used */
int ui_text_wrap(struct ui_canvas *c, int x, int y, int w, enum ui_font font,
		 int size, uint32_t color, const char *utf8);
int ui_line_height(int size);

/*
 * Text entry from the keypad, the way s22-t9d types on the console:
 *   2-9 multi-tap letters (hold: the digit), 1 punctuation, 0 space (hold:
 *   0), * symbols, # mode (abc, Abc: next letter capital, ABC, 123),
 *   C deletes. A tap on the same key within a second replaces the letter.
 * With digits set (phone numbers) every key is its digit, * held is +.
 * With secret set (passwords) the text shows as asterisks, the newest
 * character in the clear until its key's second is over.
 * ui_entry_key() returns true if it used the key.
 */
struct ui_entry {
	char text[1024];
	int mode;
	bool digits;
	bool secret;
	bool reveal;				/* secret: the newest one shows */
	int pend_key, pend_idx, pend_len;	/* the letter being chosen */
	int held_key;
	long long held_since;
	int timer_fd;
	struct ui *ui;
};
void ui_entry_init(struct ui *ui, struct ui_entry *e, bool digits);
bool ui_entry_key(struct ui_entry *e, uint32_t code, enum ui_key_state st);
void ui_entry_set(struct ui_entry *e, const char *text);
/* The text wrapped in width w, the letter being chosen underlined, and a
 * cursor; returns the height. max_lines keeps the last lines only. */
int ui_entry_draw(struct ui_canvas *c, struct ui_entry *e, int x, int y, int w,
		  int size, int max_lines);
/* The mode, and the key's letters with the current one marked: shown in
 * the status bar (s22-statusbar) while this entry is on screen in the
 * window in front. Call it when drawing the entry; it draws nothing. */
void ui_entry_indicator(struct ui_canvas *c, struct ui_entry *e, int x, int y,
			int w);

/*
 * Contacts (s22contacts.c), shared by Phone, Messages and Contacts: a vCard
 * file, read again whenever it changes, sorted by name.
 */
#define UI_CONTACTS	"/var/lib/s22/contacts.vcf"
#define UI_MAX_NUMS	4
struct ui_contact {
	char name[64];
	int nnums;
	char num[UI_MAX_NUMS][32];
	char kind[UI_MAX_NUMS][12];	/* mobile, home, work, other */
	char ringtone[128];		/* a sound file, "silent" or "" */
	char texttone[128];		/* (X-S22-RINGTONE, X-S22-TEXTTONE) */
	char callvib[96];		/* a vibration pattern, "none" or "" */
	char textvib[96];		/* (X-S22-CALLVIB, X-S22-TEXTVIB) */
};
/* The contacts (*list, valid until the next call); their count */
int ui_contacts(struct ui_contact **list);
bool ui_contacts_save(const struct ui_contact *list, int n);
/* Add a vCard file's contacts whose numbers are new; how many, or -1 */
int ui_contacts_import(const char *path);
/* The same line: the last ten digits agree (short codes: all of them) */
bool ui_same_number(const char *a, const char *b);
/* The contact with that number (and which of its numbers), or NULL */
const struct ui_contact *ui_contact_for(const char *number, int *which);
const char *ui_contact_name(const char *number);
/* Smart dial: a word of the name starts with the keypad letters of digits */
bool ui_t9_match(const char *name, const char *digits);
/*
 * Sounds (s22sounds.c): the ringtones, notification sounds and alarms
 * (UI_SOUNDS/KIND, the AOSP set), and previews through s22-play at the
 * ring and notification volume.
 */
/*
 * Wallpapers (D066): binary PPMs that s22-wallpaper makes from a picture
 * chosen in Settings (480x640 for the inner screen). ui_wallpaper() fills
 * the canvas with one, by width, its bottom at the canvas's bottom (so a
 * window under the status bar shows what the whole screen would there),
 * re-read when the file changes; false, drawing nothing, without one.
 */
#define UI_WALL_HOME	"/var/lib/s22/wallpaper-home.ppm"
#define UI_WALL_LOCK	"/var/lib/s22/wallpaper-lock.ppm"
bool ui_wallpaper(struct ui_canvas *c, const char *path);

#define UI_SOUNDS	"/usr/share/sounds/aosp"
/* The files of a kind ("ringtones", "notifications"), sorted by name;
 * *list is valid until the next call for any kind */
int ui_sounds(const char *kind, char (**list)[160]);
/* A sound's name to show: "Ring Synth 04" for .../Ring_Synth_04.ogg;
 * "Silent" and "Default" for those */
const char *ui_sound_name(const char *path);
void ui_sound_preview(const char *path);	/* stops the one before */
void ui_sound_stop(void);

/*
 * Vibration patterns (D042): "on,off,on,..." in milliseconds, "none", or
 * "default" (the next setting decides). Presets for notifications and for
 * ringing (repeated while a call rings), and up to UI_VIB_CUSTOMS the user
 * recorded (vib_custom_N in the settings). Previews through s22-vibrate.
 */
struct ui_vib {
	const char *name, *pattern;
};
#define UI_VIB_CUSTOMS	4
const struct ui_vib *ui_vib_presets(bool ring, int *n);
/* The recorded patterns, "" where there is none */
int ui_vib_customs(char (*out)[96]);
/* "Heartbeat", "Custom 2", "None", "Default" or "Custom" */
const char *ui_vib_name(const char *pattern, bool ring);
void ui_vibrate(const char *pattern);	/* stops the one before */
void ui_vibrate_stop(void);

/*
 * The screen lock's PIN (s22pin.c, link -lcrypt): only its salted hash is
 * kept (lock_pin); screen_lock is "pin" or "none".
 */
bool ui_pin_set_up(void);		/* a PIN lock is on */
bool ui_pin_check(const char *pin);
bool ui_pin_set(const char *pin);	/* NULL: no lock */

/* Speed dial: keys 2-9 held on Phone's empty keypad call a number (1 is
 * voicemail); Contacts sets them */
#define UI_SPEED_DIAL	"/var/lib/s22/speeddial"
bool ui_speed_dial(int key, char *number, int len);
bool ui_speed_dial_set(int key, const char *number);	/* NULL: clear */

/*
 * Now playing (s22np.c, link -ljpeg; D059): the Music app's player
 * (s22-musicd) for the notification list and the lock screen, and covers,
 * the record drawn without one, and the player card for them and the app.
 */
struct ui_np {
	bool on;		/* a song loaded, the player running */
	bool paused;
	char title[256], artist[256], album[256], path[1024];
	double time, duration;	/* seconds */
};
extern struct ui_np ui_np;
/* Follow the player from now on: ui_np kept, a redraw at each change */
void ui_np_follow(struct ui *ui);
/* Shown: the time kept to the second while it plays */
void ui_np_live(struct ui *ui, bool live);
void ui_np_toggle(void);		/* play or pause */
void ui_np_stop(void);			/* stop; the queue is kept */
void ui_np_skip(int dir);		/* after (1); before, or the start (-1) */
void ui_np_seek_to(double t);
double ui_np_shown_time(void);		/* the time, or where a scrub is */
/* Left/Right on a player: a tap the song before or after, held scrubbing
 * (10 s a second, 30 after two). Pass releases too; true if it was one. */
bool ui_np_key(struct ui *ui, uint32_t code, enum ui_key_state st);
void ui_np_key_cancel(void);		/* the keyboard went: stop scrubbing */
/* The card: cover, title, artist, before / play-pause / after, and where
 * the song is; alpha 255 opaque, less to show what is under it */
#define UI_NP_CARD_H	148
void ui_np_card(struct ui *ui, struct ui_canvas *c, int x, int y, int w, bool sel, int alpha);
/* A tap at tx, ty on the card drawn at x, y, w: true if on it */
bool ui_np_card_tap(struct ui *ui, int x, int y, int w, int tx, int ty);
enum { UI_NP_PLAY, UI_NP_PAUSE, UI_NP_PREV, UI_NP_NEXT };
void ui_np_icon(struct ui_canvas *c, int cx, int cy, int size, int which, uint32_t color);

/* A song's cover (the folder's picture, else its own, read by ffmpeg in
 * the background, with a redraw when it comes), or NULL: size x size */
const uint32_t *ui_art_thumb(struct ui *ui, const char *song, int size);
/* The cover filling w x h from the top (cropped at the sides or the
 * bottom), blended into the theme's background, by row: top_a for the
 * first solid rows, easing to mid_a over ease rows, and from row bottom
 * easing to bottom_a at the end (amounts of 255) */
struct ui_fade {
	int solid, ease, bottom;
	int top_a, mid_a, bottom_a;
};
const uint32_t *ui_art_backdrop(struct ui *ui, const char *song, int w, int h,
				const struct ui_fade *fade);
void ui_blit(struct ui_canvas *c, int x, int y, const uint32_t *px, int w, int h);
/* Pixels blended toward color by alpha (of 255) */
void ui_dim(struct ui_canvas *c, int x, int y, int w, int h, uint32_t color, int alpha);
/* No cover: a record, radius r */
void ui_vinyl(struct ui_canvas *c, int cx, int cy, int r);
/* A rounded rectangle blended over what is there, alpha of 255 */
void ui_round_alpha(struct ui_canvas *c, int x, int y, int w, int h, int r, uint32_t color,
		    int alpha);
/* Text over a cover: a soft halo of the theme's background under it */
void ui_text_box_halo(struct ui_canvas *c, int x, int y, int w, enum ui_font font, int size,
		      uint32_t color, enum ui_align align, const char *text);
int ui_text_wrap_halo(struct ui_canvas *c, int x, int y, int w, enum ui_font font, int size,
		      uint32_t color, const char *text);

/* ---- pieces every app uses ---- */

#define UI_STATUS_H	26	/* system status bar (s22-statusbar) */
#define UI_TITLE_H	34	/* the app's title, centred */
#define UI_HEADER_H	UI_TITLE_H
#define UI_HINTS_H	36

/* The system status bar, UI_STATUS_H tall at the top of c: WiFi and
 * cellular signal on the left, clock and battery on the right. s22-statusbar
 * keeps it above every window */
void ui_status_bar(struct ui_canvas *c);
/* Signal icons: the WiFi fan (size x size, 0-4 arcs lit) and the cellular
 * bars (w x h, 0-4 lit) */
void ui_wifi_icon(struct ui_canvas *c, int x, int y, int size, int level);
/* The Bluetooth rune (status bars) */
void ui_bt_icon(struct ui_canvas *c, int x0, int y0, int w, int h, uint32_t color);
/* The location arrow, while GPS runs (status bar) */
void ui_location_icon(struct ui_canvas *c, int x0, int y0, int w, int h, uint32_t color);
/* Vibrate only, and silent (the status bars) */
void ui_vibrate_icon(struct ui_canvas *c, int x0, int y0, int w, int h, uint32_t color);
void ui_silent_icon(struct ui_canvas *c, int x0, int y0, int w, int h, uint32_t color);
void ui_cell_icon(struct ui_canvas *c, int x, int y, int w, int h, int bars);
/* Top bar: the app's title, centred (the status bar is s22-statusbar's) */
void ui_header(struct ui *ui, struct ui_canvas *c, const char *title);
/* Bottom bar: what Back and OK do, and a hint on the right (NULL: none) */
void ui_hints(struct ui_canvas *c, const char *back, const char *ok,
	      const char *right);

/* A scrolling list: rows of fixed height, one selected */
struct ui_list {
	int count, sel, top;
	int row_h;
	int drag;		/* pixels dragged, not yet a whole row */
};
/* Up/Down move the selection; true if the key was used */
bool ui_list_key(struct ui_list *l, uint32_t code);
/* The row at y for a list drawn from list_y, or -1 */
int ui_list_at(struct ui_list *l, int list_y, int y);
/* Scroll by a finger's drag of dy pixels in a list h pixels tall */
void ui_list_drag(struct ui_list *l, int dy, int h);
void ui_list_draw(struct ui *ui, struct ui_canvas *c, struct ui_list *l,
		  int y, int h,
		  void (*row)(struct ui *ui, struct ui_canvas *c, int i,
			      int x, int y, int w, int h, bool sel));

#ifdef __cplusplus
}
#endif

#endif
