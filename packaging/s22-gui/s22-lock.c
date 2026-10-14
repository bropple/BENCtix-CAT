// SPDX-License-Identifier: MIT
/*
 * s22-lock: the lock screen (D044). s22-wm starts it when the display goes
 * dark (after lock_after seconds) with a PIN lock set, and at start; it
 * locks the session (ext-session-lock-v1), so only it shows and takes keys
 * and touch until the PIN is right. If it dies, the session stays locked
 * and s22-wm starts it again.
 *
 * The keypad's digits make the PIN (shown as dots), C takes one back, OK
 * tries it; from the fourth digit it is also tried as it is typed, and
 * unlocks once right (D066; only OK counts a wrong one). After LOCKOUT_AFTER wrong ones in a row it waits LOCKOUT_S
 * seconds, twice as long each time after. End held (s22-wm passes it as
 * KEY_POWER) opens a power menu: power off and reboot, never lk2nd's
 * fastboot. Without a PIN set it unlocks at once.
 *
 * While music plays (D059) its cover fills the background and its card
 * shows under the date: Left/Right the song before or after (held,
 * scrubbing), OK play or pause until a digit is typed (then it unlocks, as
 * before), and the card's buttons and bar by touch.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "s22ui.h"

#define MAX_PIN		12
#define LOCKOUT_AFTER	5
#define LOCKOUT_S	30

static char pin[MAX_PIN + 1];
static int fails;
static time_t locked_until;
static char flash[96];
static bool powermenu;
static int power_sel;
static const char *power_doing;

static void wipe(void)
{
	explicit_bzero(pin, sizeof(pin));
}

/* A digit typed, four or more: tried once its dot is drawn (a check takes
 * about 120 ms, yescrypt) */
static bool auto_try;

static void try_pin(struct ui *ui)
{
	if (!pin[0])
		return;
	if (ui_pin_check(pin)) {
		wipe();
		ui_unlock(ui);
		return;
	}
	wipe();
	fails++;
	if (fails >= LOCKOUT_AFTER) {
		int wait = LOCKOUT_S << (fails - LOCKOUT_AFTER < 6 ? fails - LOCKOUT_AFTER : 6);
		locked_until = time(NULL) + wait;
		snprintf(flash, sizeof(flash), "Wrong PIN %d times: wait %d s", fails, wait);
	} else {
		snprintf(flash, sizeof(flash), "Wrong PIN");
	}
}

static void draw_power(struct ui *ui, struct ui_canvas *c)
{
	static const char *items[] = { "Power off", "Reboot" };
	int y = UI_STATUS_H + 60;

	ui_text_box(c, 0, UI_STATUS_H + 16, c->w, UI_BOLD, 22, UI_FG, UI_CENTER, "Power");
	if (power_doing) {
		ui_text_box(c, 16, c->h / 2 - 20, c->w - 32, UI_BOLD, 24, UI_FG,
			    UI_CENTER, power_doing);
		return;
	}
	for (int i = 0; i < 2; i++) {
		int ry = y + i * 84;
		if (i == power_sel)
			ui_round(c, 8, ry - 4, c->w - 16, 80, 14, UI_ACCENT);
		ui_round(c, 12, ry, c->w - 24, 72, 12, UI_BAR);
		ui_text_box(c, 28, ry + 22, c->w - 56, UI_BOLD, 22, UI_FG, UI_LEFT, items[i]);
	}
	ui_hints(c, "Cancel", "Choose", NULL);
}

/* Where the PIN pad starts, and the player's card above it */
static int pin_y(int h)
{
	return h - UI_HINTS_H - 200;
}

static int card_y(int h)
{
	int y = UI_STATUS_H + 200;

	return y + UI_NP_CARD_H + 16 > pin_y(h) ? pin_y(h) - UI_NP_CARD_H - 16 : y;
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	char clock[16], date[48], dots[2 * MAX_PIN + 2];
	time_t now = time(NULL);
	struct tm tm;

	/* The cover behind it all: blended with the background behind the
	 * clock, lighter around the card, mostly background behind the PIN;
	 * else the lock screen's wallpaper, if one is chosen (D066); the
	 * power menu on the plain background */
	if (!ui_np.on && !powermenu)
		ui_wallpaper(c, UI_WALL_LOCK);
	if (ui_np.on && !powermenu) {
		struct ui_fade fade = {
			.solid = UI_STATUS_H + 190, .ease = 60, .top_a = 160, .mid_a = 90,
			.bottom = pin_y(c->h) - 30, .bottom_a = 215,
		};
		const uint32_t *back = ui_art_backdrop(ui, ui_np.path, c->w, c->h, &fade);
		if (back)
			ui_blit(c, 0, 0, back, c->w, c->h);
	}
	ui_status_bar(c);
	if (powermenu) {
		draw_power(ui, c);
		return;
	}
	ui_clock(clock, sizeof(clock));
	localtime_r(&now, &tm);
	strftime(date, sizeof(date), "%A, %B %-d", &tm);
	/* Over a cover: haloed, and the date in the text colour */
	ui_text_box_halo(c, 40, UI_STATUS_H + 70, c->w - 80, UI_SANS, 64, UI_FG, UI_CENTER, clock);
	ui_text_box_halo(c, 40, UI_STATUS_H + 160, c->w - 80, UI_SANS, 20,
			 ui_np.on ? UI_FG : UI_DIM, UI_CENTER, date);

	if (ui_np.on)
		ui_np_card(ui, c, 12, card_y(c->h), c->w - 24, false, 200);

	int y = pin_y(c->h);
	if (locked_until > now) {
		char w[64];
		snprintf(w, sizeof(w), "Try again in %d s", (int)(locked_until - now));
		ui_text_box_halo(c, 40, y, c->w - 80, UI_SANS, 20, UI_WARN, UI_CENTER, w);
	} else {
		ui_text_box_halo(c, 40, y, c->w - 80, UI_SANS, 20, ui_np.on ? UI_FG : UI_DIM,
				 UI_CENTER, "Enter PIN");
		int n = strlen(pin), o = 0;
		for (int i = 0; i < n; i++)
			o += snprintf(dots + o, sizeof(dots) - o, "%s●", i ? " " : "");
		ui_round(c, 40, y + 40, c->w - 80, 60, 12, UI_BAR);
		ui_text_box(c, 40, y + 50, c->w - 80, UI_SANS, 28, UI_FG, UI_CENTER,
			    n ? dots : "");
	}
	if (flash[0])
		ui_text_box_halo(c, 16, y + 120, c->w - 32, UI_SANS, 18, UI_WARN, UI_CENTER, flash);
	ui_hints(c, pin[0] ? "C deletes" : NULL,
		 pin[0] ? "Unlock" : ui_np.on ? (ui_np.paused ? "Play" : "Pause") : NULL,
		 "End held: power");
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st, const char *text)
{
	/* The player's Left and Right, presses and releases */
	if (!powermenu && ui_np_key(ui, code, st))
		return;
	if (st != UI_PRESSED)
		return;
	if (code == KEY_POWER) {		/* End held (s22-wm) */
		powermenu = true;
		power_sel = 0;
		ui_redraw(ui);
		return;
	}
	if (powermenu) {
		if (power_doing)
			return;
		if (code == KEY_BACK || code == KEY_ESC)
			powermenu = false;
		else if (code == KEY_UP || code == KEY_DOWN)
			power_sel ^= 1;
		else if (code == KEY_ENTER || code == KEY_OK) {
			power_doing = power_sel ? "Rebooting…" : "Powering off…";
			ui_spawn(power_sel ? "sudo -n /usr/local/bin/s22-power reboot" :
				 "sudo -n /usr/local/bin/s22-power off");
		}
		ui_redraw(ui);
		return;
	}
	if (locked_until > time(NULL))
		return;
	flash[0] = 0;
	size_t n = strlen(pin);
	if (code >= KEY_1 && code <= KEY_0) {
		if (n < MAX_PIN) {
			pin[n] = code == KEY_0 ? '0' : '1' + (code - KEY_1);
			pin[n + 1] = 0;
			if (n + 1 >= 4) {
				auto_try = true;
				ui_timer(ui, 30);
			}
		}
	} else if (code == KEY_BACKSPACE && n) {
		pin[n - 1] = 0;
	} else if ((code == KEY_ENTER || code == KEY_OK) && !n && ui_np.on) {
		ui_np_toggle();		/* no PIN typed yet: the music */
	} else if (code == KEY_ENTER || code == KEY_OK || code == KEY_PHONE) {
		try_pin(ui);
	}
	ui_redraw(ui);
}

static void tick(struct ui *ui)
{
	if (auto_try) {
		auto_try = false;
		if (pin[0] && ui_pin_check(pin)) {
			wipe();
			ui_unlock(ui);
			return;
		}
	}
	if (locked_until && locked_until <= time(NULL)) {
		locked_until = 0;
		flash[0] = 0;
	}
	ui_redraw(ui);
	ui_timer(ui, locked_until ? 1000 : 60000 - (int)(time(NULL) % 60) * 1000);
}

static void tap(struct ui *ui, int x, int y)
{
	if (!powermenu && ui_np.on)
		ui_np_card_tap(ui, 12, card_y(ui_height(ui)), ui_width(ui) - 24, x, y);
}

static void focus(struct ui *ui, bool focused)
{
	if (!focused)
		ui_np_key_cancel();
}

static void start(struct ui *ui)
{
	ui_np_follow(ui);
	ui_np_live(ui, true);
	tick(ui);
}

int main(void)
{
	struct ui_app app = {
		.app_id = "s22-lock",
		.title = "Lock",
		.draw = draw,
		.key = key,
		.tick = tick,
		.start = start,
		.tap = tap,
		.focus = focus,
		.lock = true,
	};

	/* No PIN, nothing to lock with (and an unlock may only follow the
	 * compositor's "locked") */
	if (!ui_pin_set_up())
		return 0;
	int r = ui_main(&app);

	wipe();
	return r;
}
