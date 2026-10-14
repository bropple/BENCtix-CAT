// SPDX-License-Identifier: MIT
/*
 * s22-touchtest: where the touchscreen thinks the finger is.
 *
 * Five targets (the corners and the middle); every finger down gets a
 * crosshair with its coordinates, and its path is traced. A target turns
 * green when a touch lands within 20 px of it. C clears, Back closes.
 *
 * OK calibrates: five targets one at a time, each touched once. The fit
 * (least squares, affine: offset, scale and skew) maps where the touches
 * landed onto the targets; composed with the calibration in force it
 * becomes the new touch_calibration setting, libinput's matrix on 0..1
 * coordinates, which s22-wm applies. 0 goes back to none.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s22ui.h"

#define MAX_FINGERS	10
#define TRAIL		2000
#define HIT_PX		20

static struct { bool down; int x, y; } fingers[MAX_FINGERS];
static struct { short x, y; } trail[TRAIL];
static int ntrail;
static bool hit[5];
static int last_x = -1, last_y = -1;
static char message[96];

/* Calibration: which target is up (-1: not calibrating), and the mean of
 * every sample of the touch on each */
#define CAL_N		5
#define CAL_INSET	40
#define CAL_MAX_ERR	30	/* px a touch may miss the fit by */
static int cal = -1;
static double cal_x[CAL_N], cal_y[CAL_N], sum_x, sum_y;
static int samples;

static void cal_target(int w, int h, int i, int *x, int *y)
{
	static const int fx[CAL_N] = { 0, 1, 1, 0, 0 }, fy[CAL_N] = { 0, 0, 1, 1, 0 };

	if (i == CAL_N - 1) {
		*x = w / 2;
		*y = h / 2;
		return;
	}
	*x = fx[i] ? w - 1 - CAL_INSET : CAL_INSET;
	*y = fy[i] ? h - 1 - CAL_INSET : CAL_INSET;
}

/* Solve the 3x3 system a.x = b (Gaussian elimination); false if singular */
static bool solve3(double a[3][3], double b[3], double x[3])
{
	for (int c = 0; c < 3; c++) {
		int p = c;
		for (int r = c + 1; r < 3; r++)
			if (fabs(a[r][c]) > fabs(a[p][c]))
				p = r;
		if (fabs(a[p][c]) < 1e-12)
			return false;
		for (int k = 0; k < 3; k++) {
			double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t;
		}
		double t = b[c]; b[c] = b[p]; b[p] = t;
		for (int r = 0; r < 3; r++) {
			if (r == c)
				continue;
			double f = a[r][c] / a[c][c];
			for (int k = 0; k < 3; k++)
				a[r][k] -= f * a[c][k];
			b[r] -= f * b[c];
		}
	}
	for (int c = 0; c < 3; c++)
		x[c] = b[c] / a[c][c];
	return true;
}

static void cal_finish(struct ui *ui)
{
	int w = ui_width(ui), h = ui_height(ui);
	double ata[3][3] = { 0 }, atx[3] = { 0 }, aty[3] = { 0 }, ax[3], ay[3];
	double m[6] = { 1, 0, 0, 0, 1, 0 };
	char buf[128];

	/* Least squares on 0..1 coordinates: observed -> target */
	for (int i = 0; i < CAL_N; i++) {
		int tx, ty;
		cal_target(w, h, i, &tx, &ty);
		double v[3] = { cal_x[i] / w, cal_y[i] / h, 1 };
		for (int r = 0; r < 3; r++) {
			for (int k = 0; k < 3; k++)
				ata[r][k] += v[r] * v[k];
			atx[r] += v[r] * tx / w;
			aty[r] += v[r] * ty / h;
		}
	}
	double ata2[3][3];
	memcpy(ata2, ata, sizeof(ata));
	if (!solve3(ata, atx, ax) || !solve3(ata2, aty, ay)) {
		snprintf(message, sizeof(message), "Could not fit: try again (OK)");
		return;
	}
	/* Every touch must sit near its target after the fit */
	for (int i = 0; i < CAL_N; i++) {
		int tx, ty;
		cal_target(w, h, i, &tx, &ty);
		double fx = (ax[0] * cal_x[i] / w + ax[1] * cal_y[i] / h + ax[2]) * w;
		double fy = (ay[0] * cal_x[i] / w + ay[1] * cal_y[i] / h + ay[2]) * h;
		if (hypot(fx - tx, fy - ty) > CAL_MAX_ERR) {
			snprintf(message, sizeof(message),
				 "Touch %d was off by %.0f px: try again (OK)", i + 1,
				 hypot(fx - tx, fy - ty));
			return;
		}
	}
	/* Compose with the calibration in force: new = fit . current */
	if (sscanf(ui_setting("touch_calibration", "", buf, sizeof(buf)),
		   "%lf %lf %lf %lf %lf %lf", &m[0], &m[1], &m[2], &m[3], &m[4],
		   &m[5]) != 6)
		memcpy(m, (double[6]){ 1, 0, 0, 0, 1, 0 }, sizeof(m));
	double n[6] = {
		ax[0] * m[0] + ax[1] * m[3], ax[0] * m[1] + ax[1] * m[4],
		ax[0] * m[2] + ax[1] * m[5] + ax[2],
		ay[0] * m[0] + ay[1] * m[3], ay[0] * m[1] + ay[1] * m[4],
		ay[0] * m[2] + ay[1] * m[5] + ay[2],
	};
	snprintf(buf, sizeof(buf), "%.5f %.5f %.5f %.5f %.5f %.5f",
		 n[0], n[1], n[2], n[3], n[4], n[5]);
	if (ui_setting_set("touch_calibration", buf)) {
		ui_wm("wake", NULL, 0);		/* s22-wm re-reads the settings */
		snprintf(message, sizeof(message), "Calibrated and saved");
	} else {
		snprintf(message, sizeof(message), "Could not save the setting");
	}
}

static void target(int w, int h, int i, int *x, int *y)
{
	static const int fx[5] = { 0, 1, 0, 1 }, fy[5] = { 0, 0, 1, 1 };

	if (i == 4) {
		*x = w / 2;
		*y = h / 2;
		return;
	}
	*x = fx[i] ? w - 1 - 24 : 24;
	*y = fy[i] ? h - 1 - 24 : 24;
}

static void cross(struct ui_canvas *c, int x, int y, int r, uint32_t color)
{
	ui_fill(c, x - r, y, 2 * r + 1, 1, color);
	ui_fill(c, x, y - r, 1, 2 * r + 1, color);
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	char buf[64];

	if (cal >= 0) {
		int x, y;
		cal_target(c->w, c->h, cal, &x, &y);
		cross(c, x, y, 24, UI_ACCENT);
		ui_round(c, x - 5, y - 5, 11, 11, 5, UI_ACCENT);
		snprintf(buf, sizeof(buf), "Calibrating: touch the cross (%d of %d)",
			 cal + 1, CAL_N);
		ui_text_box(c, 0, c->h / 2 + 40, c->w, UI_SANS, 18, UI_FG,
			    UI_CENTER, buf);
		ui_text_box(c, 0, c->h / 2 + 66, c->w, UI_SANS, 15, UI_DIM,
			    UI_CENTER, "Back stops");
		return;
	}

	for (int i = 0; i < ntrail; i++)
		ui_fill(c, trail[i].x - 1, trail[i].y - 1, 3, 3, UI_DIM);
	for (int i = 0; i < 5; i++) {
		int x, y;
		target(c->w, c->h, i, &x, &y);
		cross(c, x, y, 16, hit[i] ? UI_OK : UI_FG);
		ui_fill(c, x - 2, y - 2, 5, 5, hit[i] ? UI_OK : UI_FG);
	}
	for (int i = 0; i < MAX_FINGERS; i++) {
		if (!fingers[i].down)
			continue;
		/* Full-screen lines through the finger */
		ui_fill(c, 0, fingers[i].y, c->w, 1, UI_ACCENT);
		ui_fill(c, fingers[i].x, 0, 1, c->h, UI_ACCENT);
		snprintf(buf, sizeof(buf), "%d: %d, %d", i, fingers[i].x, fingers[i].y);
		int tx = fingers[i].x + 8, ty = fingers[i].y - 26;
		if (tx > c->w - 130)
			tx = fingers[i].x - 130;
		if (ty < 4)
			ty = fingers[i].y + 8;
		ui_text(c, tx, ty, UI_BOLD, 17, UI_ACCENT, buf);
	}
	if (last_x >= 0)
		snprintf(buf, sizeof(buf), "last: %d, %d", last_x, last_y);
	else
		snprintf(buf, sizeof(buf), "Touch the crosses");
	ui_text_box(c, 0, c->h / 2 + 40, c->w, UI_SANS, 18, UI_DIM, UI_CENTER, buf);
	ui_text_box(c, 0, c->h / 2 + 66, c->w, UI_SANS, 15, UI_DIM, UI_CENTER,
		    "OK calibrates, 0 resets, C clears, Back closes");
	if (message[0])
		ui_text_box(c, 0, c->h / 2 + 92, c->w, UI_BOLD, 17, UI_ACCENT,
			    UI_CENTER, message);
}

static void touch(struct ui *ui, int id, enum ui_key_state st, int x, int y)
{
	if (id < 0 || id >= MAX_FINGERS)
		return;
	if (cal >= 0) {
		if (id != 0 && st != UI_RELEASED)
			return;		/* the first finger only */
		if (st != UI_RELEASED) {
			sum_x += x;
			sum_y += y;
			samples++;
		} else if (samples > 0) {
			cal_x[cal] = sum_x / samples;
			cal_y[cal] = sum_y / samples;
			sum_x = sum_y = 0;
			samples = 0;
			if (++cal == CAL_N) {
				cal = -1;
				cal_finish(ui);
			}
		}
		ui_redraw(ui);
		return;
	}
	if (st == UI_RELEASED) {
		fingers[id].down = false;
	} else {
		fingers[id].down = true;
		fingers[id].x = x;
		fingers[id].y = y;
		last_x = x;
		last_y = y;
		if (ntrail < TRAIL)
			trail[ntrail++].x = x, trail[ntrail - 1].y = y;
		for (int i = 0; i < 5; i++) {
			int tx, ty;
			target(ui_width(ui), ui_height(ui), i, &tx, &ty);
			if ((x - tx) * (x - tx) + (y - ty) * (y - ty) <= HIT_PX * HIT_PX)
				hit[i] = true;
		}
	}
	ui_redraw(ui);
}

static void key(struct ui *ui, uint32_t code, enum ui_key_state st,
		const char *text)
{
	if (st != UI_PRESSED)
		return;
	if (code == KEY_BACK || code == KEY_ESC) {
		if (cal >= 0) {
			cal = -1;
			snprintf(message, sizeof(message), "Calibration stopped");
			ui_redraw(ui);
			return;
		}
		ui_quit(ui);
	}
	if (code == KEY_ENTER || code == KEY_OK) {
		cal = 0;
		sum_x = sum_y = 0;
		samples = 0;
		message[0] = 0;
		memset(fingers, 0, sizeof(fingers));
		ui_redraw(ui);
	}
	if (code == KEY_0) {
		ui_setting_set("touch_calibration", NULL);
		ui_wm("wake", NULL, 0);
		snprintf(message, sizeof(message), "Calibration reset");
		ui_redraw(ui);
	}
	if (code == KEY_BACKSPACE) {
		ntrail = 0;
		memset(hit, 0, sizeof(hit));
		last_x = last_y = -1;
		ui_redraw(ui);
	}
}

int main(void)
{
	struct ui_app app = {
		.app_id = "s22-touchtest",
		.title = "Touch test",
		.draw = draw,
		.key = key,
		.touch = touch,
	};

	return ui_main(&app);
}
