// SPDX-License-Identifier: MIT
/*
 * s22-camera: the camera app. Raw frames straight from the sensor (GC5035,
 * through CAMSS's RDI path) and a small ISP of our own, so every part of
 * the picture is ours to control:
 *
 *   viewfinder  the sensor's 1296x972 binned mode at 30 fps; each 2x2 Bayer
 *               block makes one pixel, turned upright for the screen
 *   photo       the 2592x1944 mode, demosaiced (bilinear) into a 1944x2592
 *               JPEG in ~/Pictures/Camera
 *   exposure    automatic (centre-weighted, with compensation) or manual
 *               shutter and gain, in any mix
 *   white       grey world on the mid-tones, or held
 *   focus       contrast-detect autofocus with the DW9714 lens (a coarse
 *               then a fine sweep of the centre's sharpness), or manual
 *   flash       off, on for the photo (as a torch), or a steady torch
 *
 * Keys: OK focuses (when automatic) and takes the photo, * focuses only,
 * # switches between the rear and front cameras (the front: fixed focus,
 * no flash, a mirror in the viewfinder), Left/Right choose a setting in
 * the bar, Up/Down change it, Back closes.
 * The camera runs only while the app is in front.
 *
 * Calibration so far: black level 64 (of 1023) and the analogue gain
 * levels were measured on this phone (see the gc5035 driver); line time
 * 16.67 us in both modes, measured from frame timestamps. Lens shading:
 * per-channel gains on a grid over the sensor, from SHADING (measured on
 * the phone from a raw flat field; none if the file is missing).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <jpeglib.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "s22ui.h"

#define FLASH_LED	"/sys/class/leds/white:flash/brightness"
#define SHADING		"/var/lib/s22/camera-shading"	/* rear; front: -front */
#define BLACK		64		/* of 1023 */
#define NBUF		6
#define GAIN_ONE	16		/* sensor gain code for 1x */
#define GAIN_MAX	182
#define AE_TARGET	0.14		/* mean linear level of the metered area */

/* The cameras: what differs between them */
struct camdef {
	const char *sensor;	/* subdev name prefix */
	const char *name;
	int path;		/* CSIPHY, CSID, ISPIF and RDI number */
	int view_w, view_h, still_w, still_h;
	double line_us;
	int vts;		/* lines per frame at the full frame rate */
	int gain_max;		/* sensor gain code, 16 = 1x */
	const char *lens;	/* focus motor subdev, or NULL */
	bool flash, shading;
	bool cw;		/* turned clockwise to stand up, else counter */
	bool mirror;		/* the viewfinder as a mirror (selfies) */
};

static const struct camdef cams[] = {
	{ "gc5035", "Rear", 0, 1296, 972, 2592, 1944, 16.667, 2008, 182,
	  "dw9714", true, true, false, false },
	/* Line time from the frame rate: 1268 lines in 33.09 ms */
	{ "gc02m2", "Front", 1, 1600, 1200, 1600, 1200, 26.09, 1268, 160,
	  NULL, false, true, true, false },
};
static int cam_idx;
#define CAM		(&cams[cam_idx])
#define LINE_US		(CAM->line_us)
#define FRAME_US	((CAM->vts - 16) * LINE_US) /* the longest at full rate */

/* ------------------------------------------------------------ settings --- */

enum { S_EV, S_SHUTTER, S_GAIN, S_FOCUS, S_WB, S_FLASH, S_COUNT };

static const int shutters_us[] = { 0, 1000, 2000, 4000, 8000, 16667, 33333,
	66667, 125000, 250000 };
static const char *const shutter_names[] = { "Auto", "1/1000", "1/500",
	"1/250", "1/125", "1/60", "1/30", "1/15", "1/8", "1/4" };
static const int gains[] = { 0, 16, 32, 64, 128, 182 };
static const char *const gain_names[] = { "Auto", "1x", "2x", "4x", "8x",
	"11x" };
static const char *const flash_names[] = { "Off", "On", "Torch" };

static struct {
	int ev;			/* thirds of a stop, -6..6 */
	int shutter;		/* index into shutters_us */
	int gain;		/* index into gains */
	int focus;		/* -1 auto, else the lens position */
	bool wb_hold;
	int flash;
} set = { .ev = 0, .focus = -1 };
static int sel = S_EV;

/* -------------------------------------------------------------- devices --- */

static int sensor_fd = -1, lens_fd = -1, video_fd = -1;
static char video_dev[64], sensor_name[64];
static struct {
	void *p;
	size_t len;
} bufs[NBUF];
static int frame_w, frame_h, frame_stride;
static bool streaming;
static int vts;				/* lines per frame */

#define dbg(...) do { if (getenv("S22_CAMERA_DEBUG")) fprintf(stderr, __VA_ARGS__); } while (0)

static int subdev_open(const char *prefix)
{
	DIR *d = opendir("/sys/class/video4linux");
	struct dirent *e;
	int fd = -1;

	while (d && fd < 0 && (e = readdir(d)) != NULL) {
		char path[512], name[64] = "";
		snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name",
			 e->d_name);
		FILE *f = fopen(path, "re");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f) &&
		    strncmp(name, prefix, strlen(prefix)) == 0) {
			snprintf(path, sizeof(path), "/dev/%s", e->d_name);
			fd = open(path, O_RDWR | O_CLOEXEC);
			if (strcmp(prefix, CAM->sensor) == 0) {
				name[strcspn(name, "\n")] = 0;
				snprintf(sensor_name, sizeof(sensor_name), "%s", name);
			}
		}
		fclose(f);
	}
	if (d)
		closedir(d);
	return fd;
}

static int ctrl_set(int fd, unsigned id, int val)
{
	struct v4l2_control c = { id, val };

	return fd >= 0 ? ioctl(fd, VIDIOC_S_CTRL, &c) : -1;
}

static void flash(bool on)
{
	int fd = open(FLASH_LED, O_WRONLY | O_CLOEXEC);

	if (fd >= 0) {
		if (write(fd, on ? "1" : "0", 1) < 0)
			;
		close(fd);
	}
}

/* media-ctl does the links and formats; it is there with v4l-utils */
static bool pipeline(int w, int h)
{
	char cmd[1024], fmt[64];
	FILE *p;

	int n = CAM->path;

	snprintf(fmt, sizeof(fmt), "SRGGB10_1X10/%dx%d", w, h);
	snprintf(cmd, sizeof(cmd),
		 "media-ctl -d /dev/media0 -r && "
		 "media-ctl -d /dev/media0 -l '\"msm_csiphy%d\":1->\"msm_csid%d\":0[1],"
		 "\"msm_csid%d\":1->\"msm_ispif%d\":0[1],\"msm_ispif%d\":1->\"msm_vfe0_rdi%d\":0[1]' && "
		 "media-ctl -d /dev/media0 -V '\"%s\":0[fmt:%s],"
		 "\"msm_csiphy%d\":0[fmt:%s],\"msm_csid%d\":0[fmt:%s],"
		 "\"msm_ispif%d\":0[fmt:%s],\"msm_vfe0_rdi%d\":0[fmt:%s]'",
		 n, n, n, n, n, n, sensor_name, fmt, n, fmt, n, fmt, n, fmt, n, fmt);
	if (system(cmd) != 0)
		return false;
	if (!video_dev[0]) {
		snprintf(cmd, sizeof(cmd), "media-ctl -d /dev/media0 -e msm_vfe0_video%d", n);
		p = popen(cmd, "r");
		if (p == NULL || !fgets(video_dev, sizeof(video_dev), p)) {
			if (p)
				pclose(p);
			return false;
		}
		pclose(p);
		video_dev[strcspn(video_dev, "\n")] = 0;
	}
	return true;
}

static void stream_stop(void)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	struct v4l2_requestbuffers rb = { .count = 0,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP };

	if (video_fd < 0)
		return;
	if (streaming)
		ioctl(video_fd, VIDIOC_STREAMOFF, &type);
	streaming = false;
	for (int i = 0; i < NBUF; i++)
		if (bufs[i].p) {
			munmap(bufs[i].p, bufs[i].len);
			bufs[i].p = NULL;
		}
	ioctl(video_fd, VIDIOC_REQBUFS, &rb);
	close(video_fd);
	video_fd = -1;
}

static bool stream_start(int w, int h)
{
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	struct v4l2_requestbuffers rb = { .count = NBUF,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP };
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	stream_stop();
	if (!pipeline(w, h)) {
		fprintf(stderr, "s22-camera: media-ctl setup failed\n");
		return false;
	}
	video_fd = open(video_dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (video_fd < 0) {
		fprintf(stderr, "s22-camera: %s: %s\n", video_dev, strerror(errno));
		return false;
	}
	f.fmt.pix_mp.width = w;
	f.fmt.pix_mp.height = h;
	f.fmt.pix_mp.pixelformat = v4l2_fourcc('p', 'R', 'A', 'A');
	f.fmt.pix_mp.num_planes = 1;
	if (ioctl(video_fd, VIDIOC_S_FMT, &f) || ioctl(video_fd, VIDIOC_REQBUFS, &rb)) {
		fprintf(stderr, "s22-camera: format or buffers: %s\n", strerror(errno));
		stream_stop();
		return false;
	}
	dbg("stream %dx%d on %s, %u buffers, stride %u\n", w, h, video_dev,
	    rb.count, f.fmt.pix_mp.plane_fmt[0].bytesperline);
	frame_w = f.fmt.pix_mp.width;
	frame_h = f.fmt.pix_mp.height;
	frame_stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;
	for (unsigned i = 0; i < rb.count && i < NBUF; i++) {
		struct v4l2_plane plane = { 0 };
		struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP, .index = i, .length = 1,
			.m.planes = &plane };
		if (ioctl(video_fd, VIDIOC_QUERYBUF, &b)) {
			stream_stop();
			return false;
		}
		bufs[i].len = plane.length;
		bufs[i].p = mmap(NULL, plane.length, PROT_READ, MAP_SHARED, video_fd,
				 plane.m.mem_offset);
		if (bufs[i].p == MAP_FAILED) {
			bufs[i].p = NULL;
			stream_stop();
			return false;
		}
		ioctl(video_fd, VIDIOC_QBUF, &b);
	}
	if (ioctl(video_fd, VIDIOC_STREAMON, &type)) {
		fprintf(stderr, "s22-camera: stream on: %s\n", strerror(errno));
		stream_stop();
		return false;
	}
	streaming = true;
	return true;
}

/* ------------------------------------------------------------ exposure --- */

static double exp_us = 20000, gain_x = 1;	/* what the sensor has now */
static double wb_r = 1.6, wb_b = 1.6;		/* white balance, red and blue */

/* Program the sensor: frame length so the exposure fits, then exposure and
 * gain. The frame length grows for long exposures (the frame rate drops). */
static void exposure_apply(void)
{
	int h = frame_h ? frame_h : CAM->view_h;
	int lines = (int)(exp_us / LINE_US + 0.5);
	int vblank_min = CAM->vts - h;	/* the full frame rate */
	int vblank = lines + 16 > h + vblank_min ? lines + 16 - h : vblank_min;
	int code = (int)(gain_x * GAIN_ONE + 0.5);

	if (lines < 4)
		lines = 4;
	/*
	 * Only what changed, and never an exposure longer than the frame, not
	 * even between two writes: the sensor stalls for a second if so. A
	 * longer frame goes first; a shorter one after the exposure.
	 */
	static int last_vblank = -1, last_lines = -1, last_code = -1;
	if (!streaming)
		last_vblank = last_lines = last_code = -1;
	if (vblank > last_vblank)
		ctrl_set(sensor_fd, V4L2_CID_VBLANK, vblank);
	if (lines != last_lines) {
		/* The driver writes the high byte, then the low one: going up
		 * across a high byte would show (new high, old low) for a
		 * moment, longer than either. Change the low byte first. */
		if (last_lines >= 0 && (lines >> 8) > (last_lines >> 8))
			ctrl_set(sensor_fd, V4L2_CID_EXPOSURE,
				 (last_lines & ~0xff) | (lines & 0xff));
		ctrl_set(sensor_fd, V4L2_CID_EXPOSURE, lines);
	}
	if (vblank < last_vblank)
		ctrl_set(sensor_fd, V4L2_CID_VBLANK, vblank);
	last_vblank = vblank;
	last_lines = lines;
	code = code < GAIN_ONE ? GAIN_ONE : code > CAM->gain_max ? CAM->gain_max : code;
	if (code != last_code)
		ctrl_set(sensor_fd, V4L2_CID_ANALOGUE_GAIN, code);
	last_code = code;
	vts = h + vblank;
}

/* Auto exposure: the metered mean towards the target; frames are judged
 * only every third one, the sensor taking two to show a change */
static int ae_wait;

static void auto_exposure(double mean, double clipped)
{
	double target = AE_TARGET * pow(2, set.ev / 3.0);
	double total, ratio;

	if (ae_wait > 0) {
		ae_wait--;
		return;
	}
	if (mean < 1e-4)
		mean = 1e-4;
	ratio = target / mean;
	if (clipped > 0.04 && ratio > 0.8)
		ratio = 0.8;		/* highlights first */
	if (ratio > 0.92 && ratio < 1.08)
		return;			/* close enough: no hunting */
	ratio = pow(ratio, 0.7);	/* not all the way at once */
	if (ratio > 4)
		ratio = 4;
	if (ratio < 0.25)
		ratio = 0.25;
	total = exp_us * gain_x * ratio;

	bool auto_shutter = shutters_us[set.shutter] == 0;
	bool auto_gain = gains[set.gain] == 0;
	double fixed_exp = auto_shutter ? 0 : shutters_us[set.shutter];
	double fixed_gain = auto_gain ? 0 : gains[set.gain] / (double)GAIN_ONE;

	if (!auto_shutter && !auto_gain)
		return;
	if (auto_shutter && auto_gain) {
		/* Shutter up to 1/30 s first (the full frame rate), then gain,
		 * then longer shutter up to 1/15 s */
		double e = total, g = 1;
		if (e > FRAME_US) {
			e = FRAME_US;
			g = total / e;
		}
		if (g > CAM->gain_max / (double)GAIN_ONE) {
			g = CAM->gain_max / (double)GAIN_ONE;
			e = total / g;
			if (e > 66667)		/* 1/15 s: 15 fps at least */
				e = 66667;
		}
		exp_us = e;
		gain_x = g;
	} else if (auto_shutter) {
		gain_x = fixed_gain;
		exp_us = total / gain_x;
		if (exp_us > 250000)
			exp_us = 250000;
	} else {
		exp_us = fixed_exp;
		gain_x = total / exp_us;
		if (gain_x > CAM->gain_max / (double)GAIN_ONE)
			gain_x = CAM->gain_max / (double)GAIN_ONE;
	}
	if (exp_us < 60)
		exp_us = 60;
	if (gain_x < 1)
		gain_x = 1;
	exposure_apply();
	ae_wait = 2;
}

static void manual_exposure(void)
{
	if (shutters_us[set.shutter])
		exp_us = shutters_us[set.shutter];
	if (gains[set.gain])
		gain_x = gains[set.gain] / (double)GAIN_ONE;
	exposure_apply();
	ae_wait = 2;
}

static void auto_white(double r, double g, double b)
{
	if (set.wb_hold || r <= 0 || b <= 0)
		return;
	double tr = g / r, tb = g / b;
	if (tr < 0.5) tr = 0.5;
	if (tr > 4) tr = 4;
	if (tb < 0.5) tb = 0.5;
	if (tb > 4) tb = 4;
	wb_r += (tr - wb_r) * 0.2;
	wb_b += (tb - wb_b) * 0.2;
}

/* ------------------------------------------------------ lens shading --- */

#define SH_X	17
#define SH_Y	13
static float shades[2][3][SH_Y][SH_X];
static bool have_shadings[2];
#define shade		shades[cam_idx]
#define have_shading	have_shadings[cam_idx]

/* The current camera's map: SHADING, or SHADING-front for the front */
static void shading_load(void)
{
	char path[128];

	snprintf(path, sizeof(path), "%s%s", SHADING, cam_idx ? "-front" : "");
	FILE *f = fopen(path, "re");
	char word[16];
	int c = -1, n = 0, gx, gy;

	if (f == NULL)
		return;
	while (fscanf(f, "%15s", word) == 1) {
		if (word[0] == '#') {
			int ch;
			while ((ch = fgetc(f)) != '\n' && ch != EOF)
				;
		} else if (strcmp(word, "grid") == 0) {
			if (fscanf(f, "%d %d", &gx, &gy) != 2 || gx != SH_X || gy != SH_Y)
				break;
		} else if (strcmp(word, "R") == 0 || strcmp(word, "G") == 0 ||
			   strcmp(word, "B") == 0) {
			c = word[0] == 'R' ? 0 : word[0] == 'G' ? 1 : 2;
			n = 0;
		} else if (c >= 0 && n < SH_X * SH_Y) {
			shade[c][n / SH_X][n % SH_X] = atof(word);
			if (++n == SH_X * SH_Y && c == 2)
				have_shading = true;
		}
	}
	fclose(f);
}

/* Channel c's gain at (u, v), 0..1 across and down the sensor */
static float shading_gain(int c, double u, double v)
{
	if (!have_shading || !CAM->shading)
		return 1;
	double fx = u * (SH_X - 1), fy = v * (SH_Y - 1);
	int ix = fx, iy = fy;
	if (ix >= SH_X - 1) ix = SH_X - 2;
	if (iy >= SH_Y - 1) iy = SH_Y - 2;
	double tx = fx - ix, ty = fy - iy;
	return (shade[c][iy][ix] * (1 - tx) + shade[c][iy][ix + 1] * tx) * (1 - ty) +
	       (shade[c][iy + 1][ix] * (1 - tx) + shade[c][iy + 1][ix + 1] * tx) * ty;
}

/* -------------------------------------------------------- tone curve --- */

/* 10-bit linear above black (0..959) to 8-bit display, per channel gain
 * folded in: a gamma curve with a gentle toe */
static uint8_t lut[3][1024];
static double lut_r, lut_b;

static void lut_build(void)
{
	double gain[3] = { wb_r, 1, wb_b };

	for (int c = 0; c < 3; c++)
		for (int v = 0; v < 1024; v++) {
			double x = v / (double)(1023 - BLACK) * gain[c];
			if (x > 1)
				x = 1;
			double y = x < 0.0031308 ? 12.92 * x :
				1.055 * pow(x, 1 / 2.4) - 0.055;
			lut[c][v] = (uint8_t)(y * 255 + 0.5);
		}
	lut_r = wb_r;
	lut_b = wb_b;
}

/* ------------------------------------------------------------ autofocus --- */

enum af_state { AF_IDLE, AF_COARSE, AF_FINE };
static enum af_state af = AF_IDLE;
static int af_pos, af_end, af_step, af_best, af_wait;
static double af_best_score, af_min_score;
static bool shoot_after_af;
static int lens_pos = 512, lens_good = -1;	/* the last position AF found */
static double af_saved_exp, af_saved_gain;

static void lens_move(int pos)
{
	lens_pos = pos < 0 ? 0 : pos > 1023 ? 1023 : pos;
	ctrl_set(lens_fd, V4L2_CID_FOCUS_ABSOLUTE, lens_pos);
}

static void exposure_apply(void);

static void af_begin(bool shoot)
{
	shoot_after_af = shoot;
	if (lens_fd < 0) {
		af = AF_IDLE;
		return;
	}
	af = AF_COARSE;
	af_pos = 0;
	af_end = 1023;
	af_step = 64;
	af_best_score = -1;
	af_min_score = 1e30;
	/* In the dark, sweep at the full frame rate: shutter down to 1/30 s,
	 * the rest made up with gain (noisier, but the sweep is 3x quicker) */
	af_saved_exp = exp_us;
	af_saved_gain = gain_x;
	if (exp_us > FRAME_US) {
		gain_x *= exp_us / FRAME_US;
		if (gain_x > CAM->gain_max / (double)GAIN_ONE)
			gain_x = CAM->gain_max / (double)GAIN_ONE;
		exp_us = FRAME_US;
		exposure_apply();
	}
	lens_move(af_pos);
	af_wait = 2;
}

static void af_end_sweep(void)
{
	af = AF_IDLE;
	if (exp_us != af_saved_exp || gain_x != af_saved_gain) {
		exp_us = af_saved_exp;
		gain_x = af_saved_gain;
		exposure_apply();
	}
}

static char af_note[64];		/* for the screen, after a sweep */
static bool af_note_shown;		/* the UI arms a timer to clear it */

/* One sharpness reading per lens position; returns true when it is done */
static bool af_step_frame(double score)
{
	if (af_wait > 0) {
		af_wait--;
		return false;
	}
	if (score > af_best_score) {
		af_best_score = score;
		af_best = af_pos;
	}
	if (score < af_min_score)
		af_min_score = score;
	af_pos += af_step;
	if (af_pos > af_end) {
		if (af == AF_COARSE) {
			/* No clear peak (flat, dark or noisy): keep the last good
			 * focus rather than a random one */
			if (af_best_score <= 0 ||
			    (af_best_score - af_min_score) / af_best_score < 0.15) {
				af_end_sweep();
				lens_move(lens_good >= 0 ? lens_good : 300);
				snprintf(af_note, sizeof(af_note), "Could not focus");
				af_note_shown = true;
				return true;
			}
			af = AF_FINE;
			af_pos = af_best - 56 < 0 ? 0 : af_best - 56;
			af_end = af_best + 56 > 1023 ? 1023 : af_best + 56;
			af_step = 16;
			af_best_score = -1;
		} else {
			af_end_sweep();
			lens_move(af_best);
			lens_good = af_best;
			return true;
		}
	}
	lens_move(af_pos);
	af_wait = 1;
	return false;
}

/* ---------------------------------------------------------- the frames --- */

static uint32_t *view;			/* screen-sized, upright */
static int view_w, view_h;
static char message[128];
static bool taking;			/* a photo is being taken */
static int still_skip;

/*
 * Packed 10-bit: 4 pixels in 5 bytes, the first four the high 8 bits.
 * Viewfinder: one pixel per 2x2 block (R G / G B), 8-bit precision, turned
 * 90 degrees counter-clockwise (the sensor is mounted at 270). Gathers the
 * metering, white balance and sharpness statistics on the way.
 */
/* The 2x2 block shown at screen column x and screen row y */
static int block_row(int x, int dw, int bh, double s)
{
	int xx = CAM->mirror ? dw - 1 - x : x;
	int r = (int)(xx * s);

	return CAM->cw ? bh - 1 - r : r;
}

static int block_col(int y, int bw, double s)
{
	int c = (int)(y * s);

	return CAM->cw ? c : bw - 1 - c;
}

static void view_frame(const uint8_t *raw, double *mean, double *clipped,
		       double rgb[3], double *sharp)
{
	int bw = frame_w / 2, bh = frame_h / 2;	/* blocks */
	int ow = bh, oh = bw;			/* upright: turned */
	double s = (double)ow / view_w > (double)oh / view_h ?
		(double)ow / view_w : (double)oh / view_h;
	int dw = ow / s, dh = oh / s, x0 = (view_w - dw) / 2, y0 = (view_h - dh) / 2;
	static int *rowoff, *coloff, tab_w, tab_h, tab_stride, tab_cam = -1;
	static uint16_t *gain3;		/* per screen pixel: R, G, B, 8.8 */

	if (lut_r != wb_r || lut_b != wb_b)
		lut_build();
	/* Where each screen column and row read from, once per size */
	if (tab_w != dw || tab_h != dh || tab_stride != frame_stride ||
	    tab_cam != cam_idx) {
		tab_cam = cam_idx;
		free(rowoff);
		free(coloff);
		rowoff = malloc(dw * sizeof(int));
		coloff = malloc(dh * sizeof(int));
		/* Counter-clockwise: screen x is the block row, screen y the block
		 * column from the right; clockwise the other way round */
		for (int x = 0; x < dw; x++)
			rowoff[x] = 2 * block_row(x, dw, bh, s) * frame_stride;
		for (int y = 0; y < dh; y++) {
			int px = 2 * block_col(y, bw, s);
			coloff[y] = (px / 4) * 5 + (px % 4);
		}
		free(gain3);
		gain3 = malloc((size_t)dw * dh * 3 * sizeof(*gain3));
		for (int y = 0; y < dh; y++)
			for (int x = 0; x < dw; x++) {
				double u = (block_col(y, bw, s) + 0.5) / bw;
				double v = (block_row(x, dw, bh, s) + 0.5) / bh;
				uint16_t *g = gain3 + ((size_t)y * dw + x) * 3;
				for (int c = 0; c < 3; c++)
					g[c] = shading_gain(c, u, v) * 256 + 0.5;
			}
		tab_w = dw;
		tab_h = dh;
		tab_stride = frame_stride;
		for (int i = 0; i < view_w * view_h; i++)
			view[i] = 0xff000000;
	}

	/*
	 * Turned 90 degrees: walking a screen row walks down the frame. In
	 * 32x32 tiles both stay in the cache. Each 2x2 block (R G / G B) is
	 * one pixel; px is even, so R and G sit in the same 5-byte group.
	 */
	for (int ty = 0; ty < dh; ty += 32)
		for (int tx = 0; tx < dw; tx += 32) {
			int ey = ty + 32 < dh ? ty + 32 : dh;
			int ex = tx + 32 < dw ? tx + 32 : dw;
			for (int y = ty; y < ey; y++) {
				const uint8_t *col = raw + coloff[y];
				uint32_t *out = view + (size_t)(y0 + y) * view_w + x0;
				const uint16_t *g = gain3 + (size_t)y * dw * 3;
				for (int x = tx; x < ex; x++) {
					const uint8_t *p = col + rowoff[x];
					const uint16_t *gx = g + x * 3;
					/* 10-bit linear above black, shading corrected */
					int r = (p[0] * 4 - BLACK) * gx[0] >> 8;
					int gr = ((p[1] + p[frame_stride]) * 2 - BLACK) * gx[1] >> 8;
					int b = (p[frame_stride + 1] * 4 - BLACK) * gx[2] >> 8;
					r = r < 0 ? 0 : r > 1023 ? 1023 : r;
					gr = gr < 0 ? 0 : gr > 1023 ? 1023 : gr;
					b = b < 0 ? 0 : b > 1023 ? 1023 : b;
					out[x] = 0xff000000 | lut[0][r] << 16 |
						lut[1][gr] << 8 | lut[2][b];
				}
			}
		}

	/* Statistics on every 4th block: centre-weighted level, clipping,
	 * colour of the mid-tones, and the centre's sharpness */
	double sum = 0, wsum = 0, clip = 0, n = 0, sr = 0, sg = 0, sb = 0, sh = 0;
	for (int by = 0; by < bh; by += 4) {
		const uint8_t *r0 = raw + (size_t)2 * by * frame_stride;
		const uint8_t *r1 = r0 + frame_stride;
		double fy = (by - bh / 2.0) / bh;
		for (int bx = 0; bx < bw; bx += 4) {
			int px = 2 * bx, o = (px / 4) * 5 + (px % 4);
			int R = r0[o] * 4, G1 = r0[o + 1] * 4, G2 = r1[o] * 4, B = r1[o + 1] * 4;
			int r = R - BLACK, g = (G1 + G2) / 2 - BLACK, b = B - BLACK;
			if (r < 0) r = 0;
			if (g < 0) g = 0;
			if (b < 0) b = 0;
			double fx = (bx - bw / 2.0) / bw;
			double w = fx * fx + fy * fy < 0.04 ? 4 : 1;
			double l = (0.3 * r + 0.55 * g + 0.15 * b) / (1023 - BLACK);
			sum += l * w;
			wsum += w;
			if (R > 1000 || G1 > 1000 || B > 1000)
				clip++;
			n++;
			if (l > 0.05 && l < 0.8) {
				sr += r;
				sg += g;
				sb += b;
			}
		}
	}
	/*
	 * Sharpness in the middle third: both greens of each block, pairs of
	 * blocks summed (less noise) and squared differences of the pairs
	 * (edges count far more than grain).
	 */
	for (int by = bh / 3; by < 2 * bh / 3; by++) {
		const uint8_t *r0 = raw + (size_t)2 * by * frame_stride;
		const uint8_t *r1 = r0 + frame_stride;
		int g[4] = { 0 }, k = 0;
		for (int bx = bw / 3; bx < 2 * bw / 3; bx++, k++) {
			int px = 2 * bx, o = (px / 4) * 5 + (px % 4);
			g[k & 3] = r0[o + 1] + r1[o];
			if (k >= 3) {
				int d = g[k & 3] + g[(k - 1) & 3] - g[(k - 2) & 3] -
					g[(k - 3) & 3];
				sh += (double)d * d;
			}
		}
	}
	*mean = wsum ? sum / wsum : 0;
	*clipped = n ? clip / n : 0;
	rgb[0] = sr;
	rgb[1] = sg;
	rgb[2] = sb;
	*sharp = sh / ((1 + *mean * 1000) * (1 + *mean * 1000));	/* not just brighter */
}

static char last_photo[700];		/* its path, for Photos */
#define THUMB_W		72
#define THUMB_H		96
static uint32_t last_thumb[THUMB_W * THUMB_H];
static bool have_thumb;

/* The viewfinder as it was at the photo, small, for the corner */
static void thumb_from_view(void)
{
	if (view == NULL)
		return;
	for (int y = 0; y < THUMB_H; y++)
		for (int x = 0; x < THUMB_W; x++)
			last_thumb[y * THUMB_W + x] =
				view[(size_t)(y * view_h / THUMB_H) * view_w +
				     x * view_w / THUMB_W];
	have_thumb = true;
}

static bool save_photo(const uint8_t *raw, char *name, size_t len)
{
	const char *home = getenv("HOME");
	char dir[512], path[700], stamp[40];
	time_t t = time(NULL);
	struct tm tm;
	int W = frame_w, H = frame_h;

	/* Unpack to 10-bit above black */
	uint16_t *px = malloc((size_t)W * H * sizeof(*px));
	if (!px)
		return false;
	float rowg[2][SH_X];		/* this row's gains at the grid columns */
	for (int y = 0; y < H; y++) {
		const uint8_t *row = raw + (size_t)y * frame_stride;
		uint16_t *o = px + (size_t)y * W;
		/* RGGB: even rows R G, odd rows G B */
		int c0 = y & 1 ? 1 : 0, c1 = y & 1 ? 2 : 1;
		for (int i = 0; i < SH_X; i++) {
			rowg[0][i] = shading_gain(c0, i / (double)(SH_X - 1), (y + 0.5) / H);
			rowg[1][i] = shading_gain(c1, i / (double)(SH_X - 1), (y + 0.5) / H);
		}
		for (int x = 0; x < W; x += 4, row += 5)
			for (int k = 0; k < 4; k++) {
				int v = (row[k] << 2 | (row[4] >> (2 * k) & 3)) - BLACK;
				double fx = (x + k) * (SH_X - 1.0) / (W - 1);
				int i = fx >= SH_X - 1 ? SH_X - 2 : (int)fx;
				const float *g = rowg[(x + k) & 1];
				double gain = g[i] + (g[i + 1] - g[i]) * (fx - i);
				v = v < 0 ? 0 : (int)(v * gain);
				o[x + k] = v > 1023 ? 1023 : v;
			}
	}

	localtime_r(&t, &tm);
	strftime(stamp, sizeof(stamp), "IMG_%Y%m%d_%H%M%S.jpg", &tm);
	snprintf(dir, sizeof(dir), "%s/Pictures", home ? home : "/tmp");
	mkdir(dir, 0755);
	strncat(dir, "/Camera", sizeof(dir) - strlen(dir) - 1);
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/%s", dir, stamp);
	FILE *f = fopen(path, "wb");
	if (!f) {
		free(px);
		return false;
	}

	if (lut_r != wb_r || lut_b != wb_b)
		lut_build();
	struct jpeg_compress_struct ci;
	struct jpeg_error_mgr jerr;
	ci.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&ci);
	jpeg_stdio_dest(&ci, f);
	ci.image_width = H;		/* upright: turned */
	ci.image_height = W;
	ci.input_components = 3;
	ci.in_color_space = JCS_RGB;
	jpeg_set_defaults(&ci);
	jpeg_set_quality(&ci, 92, TRUE);
	jpeg_start_compress(&ci, TRUE);
	uint8_t *line = malloc((size_t)H * 3);
	/* Output row j is sensor column W-1-j; output column i is sensor row i */
	while (ci.next_scanline < ci.image_height) {
		int sx = CAM->cw ? (int)ci.next_scanline : W - 1 - (int)ci.next_scanline;
		for (int i = 0; i < H; i++) {
			int sy = CAM->cw ? H - 1 - i : i;
			/* Bilinear demosaic of RGGB at (sx, sy), edges clamped */
			#define P(dx, dy) px[(size_t)(sy + (dy) < 0 ? 0 : sy + (dy) >= H ? H - 1 : sy + (dy)) * W + \
				(sx + (dx) < 0 ? 0 : sx + (dx) >= W ? W - 1 : sx + (dx))]
			int r, g, b, ex = sx & 1, ey = sy & 1;
			if (!ex && !ey) {		/* R */
				r = P(0, 0);
				g = (P(-1, 0) + P(1, 0) + P(0, -1) + P(0, 1)) / 4;
				b = (P(-1, -1) + P(1, -1) + P(-1, 1) + P(1, 1)) / 4;
			} else if (ex && ey) {		/* B */
				b = P(0, 0);
				g = (P(-1, 0) + P(1, 0) + P(0, -1) + P(0, 1)) / 4;
				r = (P(-1, -1) + P(1, -1) + P(-1, 1) + P(1, 1)) / 4;
			} else if (ex) {		/* G on a red row */
				g = P(0, 0);
				r = (P(-1, 0) + P(1, 0)) / 2;
				b = (P(0, -1) + P(0, 1)) / 2;
			} else {			/* G on a blue row */
				g = P(0, 0);
				b = (P(-1, 0) + P(1, 0)) / 2;
				r = (P(0, -1) + P(0, 1)) / 2;
			}
			#undef P
			line[i * 3] = lut[0][r > 1023 ? 1023 : r];
			line[i * 3 + 1] = lut[1][g > 1023 ? 1023 : g];
			line[i * 3 + 2] = lut[2][b > 1023 ? 1023 : b];
		}
		JSAMPROW rp = line;
		jpeg_write_scanlines(&ci, &rp, 1);
	}
	jpeg_finish_compress(&ci);
	jpeg_destroy_compress(&ci);
	fclose(f);
	free(line);
	free(px);
	snprintf(name, len, "%s", stamp);
	snprintf(last_photo, sizeof(last_photo), "%s", path);
	return true;
}

/* The stream's fd changes with every restart: keep the watch on it */
static int watched_fd = -1;
static void on_frame(struct ui *ui, int fd);

static void rewatch(struct ui *ui)
{
	if (video_fd == watched_fd)
		return;
	if (watched_fd >= 0)
		ui_unwatch(ui, watched_fd);
	if (video_fd >= 0)
		ui_watch(ui, video_fd, on_frame);
	watched_fd = video_fd;
}

static void view_start(struct ui *ui)
{
	bool ok = stream_start(CAM->view_w, CAM->view_h);
	rewatch(ui);
	if (!ok) {
		snprintf(message, sizeof(message), "The camera did not start");
		return;
	}
	exposure_apply();
	lens_move(set.focus >= 0 ? set.focus : lens_pos);
	flash(set.flash == 2 && CAM->flash);
}

static int preflash;		/* frames of light before focus and photo */
static double still_exp, still_gain;

static void take_photo(struct ui *ui)
{
	taking = true;
	thumb_from_view();
	/* In the dark, less gain and a longer shutter for the photo (less
	 * noise; held still, 1/8 s is fine): gain down to 4x where it can */
	still_exp = exp_us;
	still_gain = gain_x;
	if (shutters_us[set.shutter] == 0 && gain_x > 4) {
		double room = 125000 / exp_us, want = gain_x / 4;
		double k = want < room ? want : room;
		if (k > 1) {
			still_exp = exp_us * k;
			still_gain = gain_x / k;
		}
	}
	/* Same exposure in the full mode (same line time) */
	bool ok = stream_start(CAM->still_w, CAM->still_h);
	rewatch(ui);
	if (!ok) {
		snprintf(message, sizeof(message), "The camera did not start");
		taking = false;
		view_start(ui);
		return;
	}
	double ve = exp_us, vg = gain_x;
	exp_us = still_exp;
	gain_x = still_gain;
	exposure_apply();
	exp_us = ve;		/* the viewfinder carries on from its own */
	gain_x = vg;
	still_skip = 2;		/* the new exposure arrives */
}

/* OK: light first if the flash is on, then focus, then the photo */
static void shoot(struct ui *ui)
{
	message[0] = 0;
	af_note[0] = 0;
	if (set.flash == 1 && CAM->flash) {
		flash(true);
		preflash = 10;	/* exposure and white balance settle under it */
		return;
	}
	if (set.focus < 0 && lens_fd >= 0)
		af_begin(true);
	else
		take_photo(ui);
}

static void on_frame(struct ui *ui, int fd)
{
	struct v4l2_plane plane = { 0 }, plane2 = { 0 };
	struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &plane };

	if (ioctl(video_fd, VIDIOC_DQBUF, &b)) {
		if (errno != EAGAIN)
			fprintf(stderr, "s22-camera: dequeue: %s\n", strerror(errno));
		return;
	}
	/* Only the newest matters: drain any later ones */
	struct v4l2_buffer n = b;
	n.m.planes = &plane2;
	while (ioctl(video_fd, VIDIOC_DQBUF, &n) == 0) {
		ioctl(video_fd, VIDIOC_QBUF, &b);
		b.index = n.index;
		n.m.planes = &plane2;
	}
	/* Copy the frame out and give the buffer straight back: CAMSS stops
	 * capturing for a second or so if it runs short of buffers */
	static uint8_t *copy;
	static size_t copy_len;
	size_t need = (size_t)frame_stride * frame_h;
	if (copy_len < need) {
		free(copy);
		copy = malloc(need);
		copy_len = copy ? need : 0;
	}
	if (copy == NULL) {
		ioctl(video_fd, VIDIOC_QBUF, &b);
		return;
	}
	memcpy(copy, bufs[b.index].p, need);
	unsigned seq = b.sequence;
	ioctl(video_fd, VIDIOC_QBUF, &b);
	const uint8_t *raw = copy;

	if (taking) {
		if (still_skip-- > 0)
			return;
		char name[64];
		snprintf(message, sizeof(message), "Saving…");
		bool ok = save_photo(raw, name, sizeof(name));
		taking = false;
		if (set.flash == 1)
			flash(false);
		snprintf(message, sizeof(message), ok ? "Saved %s" :
			 "Could not save the photo%s", ok ? name : "");
		ui_timer(ui, 3000);
		view_start(ui);		/* the buffer goes with the old stream */
		ui_redraw(ui);
		return;
	}

	double mean, clipped, rgb[3], sharp;
	struct timespec t0, t1;
	static struct timespec last;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	view_frame(raw, &mean, &clipped, rgb, &sharp);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	dbg("frame %u: gap %.1f ms, view %.1f ms%s\n", seq,
	    (t0.tv_sec - last.tv_sec) * 1e3 + (t0.tv_nsec - last.tv_nsec) / 1e6,
	    (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6,
	    af != AF_IDLE ? " AF" : "");
	last = t0;

	if (af != AF_IDLE) {
		if (af_step_frame(sharp) && shoot_after_af)
			take_photo(ui);
		if (af_note_shown) {
			af_note_shown = false;
			ui_timer(ui, 3000);
		}
	} else {
		auto_exposure(mean, clipped);
		auto_white(rgb[0], rgb[1], rgb[2]);
		if (preflash > 0 && --preflash == 0) {
			if (set.focus < 0 && lens_fd >= 0)
				af_begin(true);
			else
				take_photo(ui);
		}
	}
	ui_redraw(ui);
}

/* ------------------------------------------------------------------ UI --- */

/* Focus and flash only where the camera has them */
static bool setting_shown(int s)
{
	return !((s == S_FOCUS && lens_fd < 0) || (s == S_FLASH && !CAM->flash));
}

static void setting_text(int s, char *buf, size_t len)
{
	switch (s) {
	case S_EV:
		snprintf(buf, len, "EV %+.1f", set.ev / 3.0);
		break;
	case S_SHUTTER:
		snprintf(buf, len, "%s", set.shutter ? shutter_names[set.shutter] :
			 "Shutter A");
		break;
	case S_GAIN:
		snprintf(buf, len, "%s", set.gain ? gain_names[set.gain] : "Gain A");
		break;
	case S_FOCUS:
		if (set.focus < 0)
			snprintf(buf, len, "AF");
		else
			snprintf(buf, len, "MF %d", set.focus);
		break;
	case S_WB:
		snprintf(buf, len, "WB %s", set.wb_hold ? "hold" : "auto");
		break;
	case S_FLASH:
		snprintf(buf, len, "⚡ %s", flash_names[set.flash]);
		break;
	}
}

static void draw(struct ui *ui, struct ui_canvas *c)
{
	/* The window's size comes after start() (the status bar takes its
	 * share): the viewfinder follows it, filled again by the next frame */
	if (view_w != c->w || view_h != c->h) {
		free(view);
		view_w = c->w;
		view_h = c->h;
		view = calloc((size_t)view_w * view_h, 4);
	}
	if (view)
		for (int y = 0; y < c->h; y++)
			memcpy(c->px + (size_t)y * c->stride,
			       view + (size_t)y * view_w, c->w * 4);

	/* The settings bar: Left/Right choose, Up/Down change */
	int bar_y = c->h - UI_HINTS_H - 40, x = 6;
	ui_fill(c, 0, bar_y, c->w, 40, UI_BAR);
	for (int s = 0; s < S_COUNT; s++) {
		char t[32];
		if (!setting_shown(s))
			continue;
		setting_text(s, t, sizeof(t));
		int w = ui_text_width(UI_SANS, 15, t) + 12;
		if (s == sel)
			ui_round(c, x - 2, bar_y + 6, w + 4, 28, 8, UI_ACCENT);
		ui_text(c, x + 6, bar_y + 11, UI_SANS, 15, s == sel ? UI_BG : UI_FG, t);
		x += w + 6;
	}

	/* What the exposure is now */
	char info[96];
	snprintf(info, sizeof(info), "%s  1/%.0f s  %.1fx", CAM->name,
		 1e6 / exp_us, gain_x);
	ui_text(c, 10, 8, UI_SANS, 15, 0xffffffff, info);

	const char *status = af != AF_IDLE ? "Focusing…" : preflash ? "Flash…" :
		taking ? "Taking…" : message[0] ? message : af_note;
	if (status[0]) {
		ui_round(c, 16, 40, c->w - 32, 38, 10, UI_BAR);
		ui_text_box(c, 26, 48, c->w - 52, UI_SANS, 17, UI_FG, UI_CENTER,
			    status);
	}
	/* The last photo, small: 0 opens it in Photos */
	if (have_thumb) {
		int tx = c->w - THUMB_W - 10, ty = bar_y - THUMB_H - 10;
		ui_fill(c, tx - 2, ty - 2, THUMB_W + 4, THUMB_H + 4, 0xffffffff);
		for (int y = 0; y < THUMB_H; y++)
			memcpy(c->px + (size_t)(ty + y) * c->stride + tx,
			       last_thumb + y * THUMB_W, THUMB_W * 4);
		ui_round(c, tx - 8, ty - 8, 22, 22, 11, UI_ACCENT);
		ui_text_box(c, tx - 8, ty - 6, 22, UI_BOLD, 14, UI_BG, UI_CENTER, "0");
	}
	ui_hints(c, "Close", "Shoot", lens_fd >= 0 ? "* focus  # flip" : "# flip");
}

static void change(int s, int d)
{
	switch (s) {
	case S_EV:
		set.ev += d;
		if (set.ev < -6) set.ev = -6;
		if (set.ev > 6) set.ev = 6;
		break;
	case S_SHUTTER:
		set.shutter = (set.shutter + d + 10) % 10;
		manual_exposure();
		break;
	case S_GAIN:
		set.gain = (set.gain + d + 6) % 6;
		manual_exposure();
		break;
	case S_FOCUS:
		/* Down from the nearest end goes back to auto */
		if (set.focus < 0)
			set.focus = lens_pos;
		set.focus += d * 32;
		if (set.focus < 0 || set.focus > 1023)
			set.focus = -1;
		else
			lens_move(set.focus);
		break;
	case S_WB:
		set.wb_hold = !set.wb_hold;
		break;
	case S_FLASH:
		set.flash = (set.flash + d + 3) % 3;
		flash(set.flash == 2);
		break;
	}
}

static void camera_switch(struct ui *ui);

static void key(struct ui *ui, uint32_t code, enum ui_key_state st,
		const char *text)
{
	if (st == UI_RELEASED || taking)
		return;
	switch (code) {
	case KEY_LEFT:
	case KEY_RIGHT:
		do
			sel = (sel + (code == KEY_LEFT ? S_COUNT - 1 : 1)) % S_COUNT;
		while (!setting_shown(sel));
		break;
	case KEY_UP:
	case KEY_DOWN:
		change(sel, code == KEY_UP ? 1 : -1);
		break;
	case KEY_ENTER:
	case KEY_OK:
	case KEY_CAMERA:
		if (st != UI_PRESSED || af != AF_IDLE || preflash)
			return;
		shoot(ui);
		break;
	case KEY_NUMERIC_POUND:
		if (st == UI_PRESSED && af == AF_IDLE && !preflash && !taking)
			camera_switch(ui);
		break;
	case KEY_NUMERIC_STAR:
		if (st == UI_PRESSED && af == AF_IDLE && lens_fd >= 0 && !preflash) {
			set.focus = -1;
			af_note[0] = 0;
			af_begin(false);
		}
		break;
	case KEY_0:
		/* Photos, at the one just taken */
		if (st == UI_PRESSED) {
			char uri[720];
			snprintf(uri, sizeof(uri), "file:%s", last_photo);
			if (!last_photo[0] || !ui_open_uri(uri)) {
				struct ui_appinfo apps[64];
				int n = ui_apps(apps, 64);
				for (int i = 0; i < n; i++)
					if (strcmp(apps[i].id, "s22-photos") == 0)
						ui_open_app(&apps[i]);
			}
		}
		return;
	case KEY_BACK:
	case KEY_ESC:
		if (st == UI_PRESSED)
			ui_quit(ui);
		return;
	default:
		return;
	}
	ui_redraw(ui);
}

static void camera_off(struct ui *ui)
{
	stream_stop();
	rewatch(ui);
	flash(false);
	af = AF_IDLE;
	taking = false;
}

static void camera_on(struct ui *ui)
{
	view_start(ui);
}

/* # : the other camera */
static void camera_switch(struct ui *ui)
{
	camera_off(ui);
	if (sensor_fd >= 0)
		close(sensor_fd);
	if (lens_fd >= 0)
		close(lens_fd);
	cam_idx = (cam_idx + 1) % (int)(sizeof(cams) / sizeof(cams[0]));
	video_dev[0] = 0;
	sensor_fd = subdev_open(CAM->sensor);
	lens_fd = CAM->lens ? subdev_open(CAM->lens) : -1;
	exp_us = 20000;
	gain_x = 1;
	preflash = 0;
	af_note[0] = 0;
	if (sensor_fd < 0) {
		snprintf(message, sizeof(message), "No %s camera", CAM->name);
		ui_timer(ui, 3000);
		return;
	}
	camera_on(ui);
	if (lens_fd >= 0 && set.focus < 0)
		af_begin(false);
	if (!setting_shown(sel))
		sel = S_EV;
}

static void focus_changed(struct ui *ui, bool focused)
{
	dbg("focus %d\n", focused);
	/* The camera runs only while we are in front */
	if (focused && video_fd < 0) {
		camera_on(ui);
		if (set.focus < 0)
			af_begin(false);
	} else if (!focused) {
		camera_off(ui);
	}
	ui_redraw(ui);
}

static void tick(struct ui *ui)
{
	message[0] = 0;
	af_note[0] = 0;
	ui_redraw(ui);
}

static void start(struct ui *ui)
{
	sensor_fd = subdev_open(CAM->sensor);
	lens_fd = CAM->lens ? subdev_open(CAM->lens) : -1;
	if (sensor_fd < 0)
		snprintf(message, sizeof(message), "No camera found");
	lut_build();
	for (cam_idx = 1; cam_idx >= 0; cam_idx--)	/* both, ending on the rear */
		shading_load();
	cam_idx = 0;
	view_w = ui_width(ui);
	view_h = ui_height(ui);
	view = calloc((size_t)view_w * view_h, 4);
	/* Started in front; focus events pause and resume it */
	if (sensor_fd >= 0) {
		camera_on(ui);
		if (set.focus < 0)
			af_begin(false);
	}
}

int main(void)
{
	struct ui_app app = {
		.app_id = "s22-camera",
		.title = "Camera",
		.draw = draw,
		.key = key,
		.tick = tick,
		.start = start,
		.focus = focus_changed,
	};
	int ret = ui_main(&app);

	stream_stop();
	flash(false);
	return ret;
}
