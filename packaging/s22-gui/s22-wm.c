// SPDX-License-Identifier: MIT
/*
 * s22-wm: the Cat S22 Flip's Wayland compositor. A cut-down tinywl
 * (wlroots 0.20, MIT) made for a phone with one screen and a keypad:
 *
 *   - every window is fullscreen on the main display; the newest one is on
 *     top and has the keyboard
 *   - Home goes home: raise the launcher, starting it if needed, and close
 *     the apps (all but the Shell, KEEP_ID), so each starts on its first
 *     page next time; Recents switches to the window before
 *   - End (KEY_POWER) belongs to calls: s22-calld hangs up with it, and
 *     here it wakes the display; held POWER_HOLD_MS (lid open), it asks
 *     s22-shell for the power menu ("power")
 *   - the keypad (s22-t9d) types multi-tap text for most windows, and
 *     passes keys unchanged to our own apps (app_id "s22-..."), which do
 *     their own text entry, and as PC keys to games (app_id "game-...")
 *   - the lid switches the display off and on; it also goes off after
 *     screen_timeout seconds (from /var/lib/s22/settings; else -t, 30)
 *     without a key press, and the key that wakes it is not passed on;
 *     with the lid closed no keys reach the windows
 *   - layer-shell surfaces (s22-shell's notification banner and list) go
 *     above or below the windows; one that wants the keyboard gets it
 *     until it unmaps or a window is raised
 *   - during a call (s22-calld's state, /run/s22-call/state, watched) with
 *     the phone at the ear (its near=1, from the proximity sensor) the
 *     display goes off and touches, the side key and other keys are
 *     ignored; End still hangs up, and the volume keys still work
 *   - Volume up/down: while a call rings, silence it (s22-calld); the call
 *     volume (call_volume, 1-7: the earpiece, or the loudspeaker on
 *     speakerphone, amixer) during a call or with the Phone app in front;
 *     else the media volume (volume, 0-15: PipeWire's default output,
 *     "Media", wpctl). Saved in the settings and shown by s22-shell
 *     ("volume KIND LEVEL MAX")
 *   - the session lock (ext-session-lock-v1, D044): with a PIN lock set
 *     (screen_lock=pin), the display going dark (lid, timeout; not during
 *     a call) starts s22-lock after lock_after seconds, and so does the
 *     start; while locked only its surface shows and takes keys and touch,
 *     but a call's Phone window shows over it; a locker that dies leaves
 *     the session locked and is started again; Home and Recents do
 *     nothing, End held is the locker's KEY_POWER; the side key still
 *     opens s22-shell's notifications and quick settings, which show
 *     above the lock (over_lock) and take the keys while open (opening
 *     what a notification links to waits for the unlock: s22-shell hears
 *     "locked" and "unlocked")
 *   - End + Volume Down takes a screenshot (s22-screenshot, D052)
 *   - the side key (KEY_NUMERIC_B) held SIDE_HOLD_MS switches the
 *     flashlight (the camera flash in torch mode), with the display off
 *     too; with the lid closed a long press is the outer screen's (it
 *     dismisses a notification) and side + Volume Down is the flashlight
 *     (D066); pressed, it
 *     asks s22-shell for the notification
 *     list ("list" on $XDG_RUNTIME_DIR/s22-shell.sock); s22-wm starts
 *     s22-shell with the launcher, and again if it cannot be reached
 *   - the clipboard outlives the app that copied (D052): its text is read
 *     as soon as it is copied (up to CLIP_MAX), and when that app closes
 *     (Home closes apps) s22-wm offers the text itself; wl-copy and
 *     wl-paste work through the data-control protocols
 *   - a control socket ($XDG_RUNTIME_DIR/s22-wm.sock, one command per
 *     connection, one line of reply):
 *         home            raise the launcher
 *         raise APP_ID    raise that app's window: "ok", or "none"
 *         wake            switch the display on
 *         list            the app_ids, top first, one per line
 *         key CODE...     press and release keys (evdev codes) as if typed
 *                         on the keypad, in turn; keydown CODE and keyup
 *                         CODE for one held. For scripts and a headless
 *                         session (WLR_BACKENDS=headless: the display
 *                         without a mode of its own gets S22_WM_SIZE,
 *                         480x640 by default), as for screenshots.
 *
 * Touch goes to the surface under the finger; a touch that wakes the
 * display is not passed on. touch_calibration (settings: "a b c d e f",
 * libinput's calibration matrix on 0..1 coordinates; s22-touchtest
 * measures it) applies to every touchscreen. No pointer, no cursor, no move or resize.
 *
 *   s22-wm [-s LAUNCHER_CMD] [-S SHELL_CMD] [-t IDLE_SECONDS]
 *          (defaults s22-launcher, s22-shell, 30; 0 never blanks)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/dmabuf.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_switch.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <libinput.h>

#define LAUNCHER_ID	"s22-launcher"
#define KEEP_ID		"foot"		/* the Shell keeps its session */
#define T9_GUI		"/run/s22-t9/gui"
#define SETTINGS	"/var/lib/s22/settings"
#define CALL_DIR	"/run/s22-call"
#define CALL_STATE	CALL_DIR "/state"
#define PHONE_ID	"s22-phone"
#define CALL_VOL_MAX	7
#define CALL_VOL_DEF	4
#define VOL_MAX		15
#define VOL_DEF		15
#define SIDE_HOLD_MS	500
#define POWER_HOLD_MS	1000
#define FLASH		"/sys/class/leds/white:flash/brightness"

struct server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wl_listener renderer_lost;	/* a GPU reset: renderer_lost() */
	struct wl_event_source *renew;		/* the new renderer, when idle */
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *output_layout;
	struct wlr_scene_tree *apps;	/* windows, top one last */
	struct wlr_scene_tree *layers[4];	/* by zwlr_layer_shell_v1_layer */

	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_layer_surface;
	struct wl_list layer_surfaces;
	struct layer *focused_layer;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list toplevels;	/* mapped, top first */

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_set_selection;
	/* The clipboard's text, kept for when the app that copied it closes */
	struct wl_listener set_selection;
	char *clip;			/* the last text copied */
	size_t clip_len;
	char *clip_buf;			/* ... being read from its app */
	size_t clip_buf_len;
	int clip_fd;
	struct wl_event_source *clip_read;
	struct wlr_data_source *clip_from;	/* the source being read */
	struct wlr_data_source *keeper;		/* ours, offering clip */
	struct wl_list keyboards;
	int touch_devices;
	struct wl_list touches;
	float calib[6];			/* touch_calibration, identity if unset */
	uint32_t touch_swallowed;	/* touch ids that woke the display */
	double touch_off[16][2];	/* layout minus surface coordinates */
	struct xkb_context *xkb;

	struct output *output;		/* the one display we use */
	struct wl_listener new_output;
	int width, height;
	struct wlr_box usable;		/* what the bars leave for windows */

	bool lid_closed, blanked, display_on;
	bool in_call, near;		/* s22-calld: a call; the phone at the ear */
	bool speaker;			/* ... the call is on the loudspeaker */
	bool ringing;			/* ... a call rings */
	bool side_down, side_used;	/* the side key held; its hold acted */
	bool side_woke;			/* ... its press woke the display */
	struct wl_event_source *side_timer;
	bool power_down;		/* End held */

	struct wl_listener new_surface;		/* GPU buffers: gpu_wait */

	/* The session lock (D044) */
	struct wlr_session_lock_manager_v1 *lock_manager;
	struct wl_listener new_lock;
	struct wlr_session_lock_v1 *lock;	/* the locker's; NULL between */
	struct wl_listener lock_new_surface, lock_unlock, lock_destroy;
	struct wlr_scene_tree *lock_tree;	/* above everything */
	struct wlr_scene_tree *over_lock;	/* s22-shell's, while locked */
	struct wlr_surface *lock_surface;
	struct wl_listener lock_surface_destroy;
	bool locked;
	struct wl_event_source *lock_timer;	/* the display went dark */
	struct wl_event_source *locker_timer;	/* start s22-lock (again) */
	time_t locker_started;
	struct wl_event_source *power_timer;
	int call_watch;			/* inotify on CALL_DIR, or -1 */
	struct wl_event_source *call_retry;
	bool waking;			/* clients redrawing before display on */
	struct wl_event_source *wake_timer;
	int idle_ms, default_idle_ms;
	struct timespec settings_mtime;	/* to the nanosecond: Settings may write
					 * twice in a second */
	struct wl_event_source *idle_timer;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
	struct wl_listener new_inhibitor;
	int inhibitors;			/* clients keeping the screen on (video) */
	uint32_t swallowed;		/* keycode whose release is eaten */

	const char *launcher_cmd, *shell_cmd, *statusbar_cmd;
	time_t shell_started;
	time_t launched;		/* CLOCK_MONOTONIC s of the last start */
	bool quitting;
	char ctl_path[108];
	int ctl_fd;
};

struct output {
	struct server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct toplevel {
	struct wl_list link;
	struct server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
};

struct layer {
	struct wl_list link;
	struct server *server;
	struct wlr_layer_surface_v1 *surface;
	struct wlr_scene_layer_surface_v1 *scene;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	bool mapped;
};

struct popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct keyboard {
	struct wl_list link;
	struct server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct touch {
	struct wl_list link;
	struct server *server;
	struct libinput_device *li;	/* NULL if not libinput's */
	struct wl_listener down;
	struct wl_listener up;
	struct wl_listener motion;
	struct wl_listener cancel;
	struct wl_listener frame;
	struct wl_listener destroy;
};

struct lid {
	struct server *server;
	struct wl_listener toggle;
	struct wl_listener destroy;
};

/* ----------------------------------------------------------- display --- */

static void display_commit(struct server *server, bool on)
{
	struct output *output = server->output;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, on);
	if (on) {
		struct wlr_output_mode *mode =
			wlr_output_preferred_mode(output->wlr_output);
		if (mode != NULL)
			wlr_output_state_set_mode(&state, mode);
	}
	if (wlr_output_commit_state(output->wlr_output, &state))
		server->display_on = on;
	else
		wlr_log(WLR_ERROR, "could not switch the display %s",
			on ? "on" : "off");
	wlr_output_state_finish(&state);
	wlr_log(WLR_INFO, "display %s", server->display_on ? "on" : "off");
}

static void send_frame_done(struct wlr_surface *surface, int sx, int sy, void *data)
{
	wlr_surface_send_frame_done(surface, data);
}

/*
 * While the display is off no frame callbacks go out, so clients stop
 * drawing (s22ui waits for one), and switching on would show their last
 * frame (the launcher's clock from when the screen went dark) until they
 * catch up. So ask every client for a new frame first (a configure, which
 * s22ui redraws on, and the pending frame callbacks), and switch on once
 * they have had WAKE_MS to commit it.
 */
#define WAKE_MS 80

static void clients_refresh(struct server *server)
{
	struct timespec now;
	struct toplevel *t;
	struct layer *l;

	clock_gettime(CLOCK_MONOTONIC, &now);
	wl_list_for_each(t, &server->toplevels, link) {
		wlr_xdg_surface_schedule_configure(t->xdg_toplevel->base);
		wlr_xdg_surface_for_each_surface(t->xdg_toplevel->base,
						 send_frame_done, &now);
	}
	wl_list_for_each(l, &server->layer_surfaces, link)
		if (l->mapped)
			wlr_layer_surface_v1_for_each_surface(l->surface,
							      send_frame_done, &now);
}

static int wake_done(void *data)
{
	struct server *server = data;

	server->waking = false;
	if (!server->lid_closed && !server->blanked && !server->display_on)
		display_commit(server, true);
	return 0;
}

static void lock_check(struct server *server);

static void display_apply(struct server *server)
{
	bool on = !server->lid_closed && !server->blanked &&
		  !(server->in_call && server->near);

	lock_check(server);

	if (server->output == NULL)
		return;
	if (!on) {
		if (server->waking) {
			server->waking = false;
			wl_event_source_timer_update(server->wake_timer, 0);
		}
		if (server->display_on)
			display_commit(server, false);
		return;
	}
	if (server->display_on || server->waking)
		return;
	server->waking = true;
	clients_refresh(server);
	wl_event_source_timer_update(server->wake_timer, WAKE_MS);
}

static void touch_calibrate(struct server *server, struct touch *t)
{
	if (t->li == NULL || !libinput_device_config_calibration_has_matrix(t->li))
		return;
	if (libinput_device_config_calibration_set_matrix(t->li, server->calib) !=
	    LIBINPUT_CONFIG_STATUS_SUCCESS)
		wlr_log(WLR_ERROR, "touch calibration not accepted");
}

static void touch_calibrate_all(struct server *server)
{
	struct touch *t;

	wl_list_for_each(t, &server->touches, link)
		touch_calibrate(server, t);
	wlr_log(WLR_INFO, "touch calibration %.4f %.4f %.4f %.4f %.4f %.4f",
		server->calib[0], server->calib[1], server->calib[2],
		server->calib[3], server->calib[4], server->calib[5]);
}

/* screen_timeout from the settings file, when it has changed */
static void load_settings(struct server *server)
{
	struct stat st;
	char line[128];
	FILE *f;

	if (stat(SETTINGS, &st) != 0)
		st.st_mtim = (struct timespec){ 0 };
	if (st.st_mtim.tv_sec == server->settings_mtime.tv_sec &&
	    st.st_mtim.tv_nsec == server->settings_mtime.tv_nsec)
		return;
	server->settings_mtime = st.st_mtim;
	server->idle_ms = server->default_idle_ms;
	float calib[6] = { 1, 0, 0, 0, 1, 0 };
	f = fopen(SETTINGS, "re");
	if (f != NULL) {
		while (fgets(line, sizeof(line), f)) {
			float m[6];
			if (strncmp(line, "screen_timeout=", 15) == 0)
				server->idle_ms = atoi(line + 15) * 1000;
			else if (sscanf(line, "touch_calibration=%f %f %f %f %f %f",
					&m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6)
				memcpy(calib, m, sizeof(m));
		}
		fclose(f);
	}
	wlr_log(WLR_INFO, "screen timeout %d s", server->idle_ms / 1000);
	if (memcmp(calib, server->calib, sizeof(calib)) != 0) {
		memcpy(server->calib, calib, sizeof(calib));
		touch_calibrate_all(server);
	}
}

static void idle_reset(struct server *server)
{
	load_settings(server);
	if (server->idle_ms <= 0 || server->inhibitors > 0)
		wl_event_source_timer_update(server->idle_timer, 0);
	else
		wl_event_source_timer_update(server->idle_timer, server->idle_ms);
}

static int idle_expired(void *data)
{
	struct server *server = data;

	server->blanked = true;
	display_apply(server);
	return 0;
}

/* idle-inhibit: a video player keeps the screen on while it plays */
struct inhibitor {
	struct server *server;
	struct wl_listener destroy;
};

static void inhibitor_destroy(struct wl_listener *listener, void *data)
{
	struct inhibitor *in = wl_container_of(listener, in, destroy);
	struct server *server = in->server;

	wl_list_remove(&in->destroy.link);
	free(in);
	server->inhibitors--;
	idle_reset(server);
}

static void new_inhibitor(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_inhibitor);
	struct wlr_idle_inhibitor_v1 *inhibitor = data;
	struct inhibitor *in = calloc(1, sizeof(*in));

	if (in == NULL)
		return;
	in->server = server;
	in->destroy.notify = inhibitor_destroy;
	wl_signal_add(&inhibitor->events.destroy, &in->destroy);
	server->inhibitors++;
	idle_reset(server);
}

/* Returns true if the display was off (so the waking key is eaten) */
static bool wake(struct server *server)
{
	if (server->in_call && server->near)
		return true;		/* at the ear: stays off, eats it */
	bool was_off = server->output != NULL && !server->display_on;

	server->blanked = false;
	display_apply(server);
	idle_reset(server);
	return was_off;
}

/* ----------------------------------------------------------- windows --- */

/* Tell s22-t9d how the keypad should behave for this window */
static void keypad_mode(const char *app_id)
{
	bool raw = app_id != NULL && strncmp(app_id, "s22-", 4) == 0;
	bool game = app_id != NULL && strncmp(app_id, "game-", 5) == 0;
	/* What the text is, for s22-t9d's order of symbols: the Shell's, or
	 * prose for everything else (a browser may say "url" itself) */
	const char *mode = raw ? "raw\n" : game ? "game\n" :
		app_id && !strcmp(app_id, KEEP_ID) ? "text shell\n" : "text prose\n";
	int fd = open(T9_GUI, O_WRONLY | O_TRUNC | O_CLOEXEC);

	if (fd < 0)
		return;
	if (write(fd, mode, strlen(mode)) < 0)
		wlr_log_errno(WLR_DEBUG, T9_GUI);
	close(fd);
}

/* ------------------------------------------------------ the lock --- */

static void spawn(const char *cmd);
static struct toplevel *find_app(struct server *server, const char *app_id);
static void shell_say(const char *msg);
static void runtime_say(const char *name, const char *msg);
static void layer_focus(struct server *server);
static void layer_place(struct server *server, struct layer *layer);

static bool lock_enabled(void)
{
	char line[300];
	bool pin = false, hash = false;
	FILE *f = fopen(SETTINGS, "re");

	while (f && fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "screen_lock=", 12))
			pin = !strncmp(line + 12, "pin", 3);
		else if (!strncmp(line, "lock_pin=$", 10))
			hash = true;
	}
	if (f)
		fclose(f);
	return pin && hash;
}

/* A call goes on over the lock: its Phone window shows and takes keys */
static bool call_over_lock(struct server *server)
{
	return server->locked && server->in_call && find_app(server, PHONE_ID);
}

static void lock_focus(struct server *server)
{
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);

	if (server->lock_surface == NULL) {
		wlr_seat_keyboard_clear_focus(server->seat);
		return;
	}
	keypad_mode("s22-lock");		/* digits, not letters */
	if (keyboard != NULL)
		wlr_seat_keyboard_notify_enter(server->seat, server->lock_surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	else
		wlr_seat_keyboard_notify_enter(server->seat, server->lock_surface,
			NULL, 0, NULL);
}

static void focus_toplevel(struct toplevel *toplevel);

/* What shows while locked: the lock, or a call's Phone window */
static void lock_apply(struct server *server)
{
	bool over = call_over_lock(server);
	struct toplevel *t;

	for (int i = 0; i < 4; i++)
		wlr_scene_node_set_enabled(&server->layers[i]->node, !server->locked);
	wlr_scene_node_set_enabled(&server->apps->node, !server->locked || over);
	wl_list_for_each(t, &server->toplevels, link) {
		const char *id = t->xdg_toplevel->app_id;
		wlr_scene_node_set_enabled(&t->scene_tree->node, !server->locked ||
			(over && id && !strcmp(id, PHONE_ID)));
	}
	wlr_scene_node_set_enabled(&server->lock_tree->node, server->locked && !over);
	struct layer *l;
	wl_list_for_each(l, &server->layer_surfaces, link)
		layer_place(server, l);
	if (over) {
		focus_toplevel(find_app(server, PHONE_ID));
	} else if (server->locked) {
		server->focused_layer = NULL;
		layer_focus(server);	/* the lock, or s22-shell's list */
	}
}

static void start_locker(struct server *server)
{
	time_t now = time(NULL);

	if (server->lock || server->quitting || now - server->locker_started < 3)
		return;
	server->locker_started = now;
	wlr_log(WLR_INFO, "locking");
	spawn("s22-lock");
}

static int locker_due(void *data)
{
	start_locker(data);
	return 0;
}

/* The display is dark: lock after lock_after seconds (not in a call) */
static void lock_check(struct server *server)
{
	bool dark = server->lid_closed || server->blanked;

	if (!dark || server->locked || server->in_call || !lock_enabled()) {
		wl_event_source_timer_update(server->lock_timer, 0);
		return;
	}
	char line[128];
	int after = 0;
	FILE *f = fopen(SETTINGS, "re");
	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, "lock_after=", 11))
			after = atoi(line + 11);
	if (f)
		fclose(f);
	wl_event_source_timer_update(server->lock_timer, after > 0 ? after * 1000 : 1);
}

static void lock_surface_destroyed(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, lock_surface_destroy);

	wl_list_remove(&server->lock_surface_destroy.link);
	server->lock_surface = NULL;
	if (server->locked)
		wlr_seat_keyboard_clear_focus(server->seat);
}

static void lock_new_surface(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, lock_new_surface);
	struct wlr_session_lock_surface_v1 *ls = data;

	if (server->lock_surface)
		return;
	server->lock_surface = ls->surface;
	wlr_scene_subsurface_tree_create(server->lock_tree, ls->surface);
	wlr_session_lock_surface_v1_configure(ls, server->width, server->height);
	server->lock_surface_destroy.notify = lock_surface_destroyed;
	wl_signal_add(&ls->surface->events.destroy, &server->lock_surface_destroy);
	lock_apply(server);
}

static void lock_forget(struct server *server)
{
	wl_list_remove(&server->lock_new_surface.link);
	wl_list_remove(&server->lock_unlock.link);
	wl_list_remove(&server->lock_destroy.link);
	server->lock = NULL;
}

static void lock_unlocked(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, lock_unlock);
	struct toplevel *top;

	lock_forget(server);
	server->locked = false;
	wlr_log(WLR_INFO, "unlocked");
	shell_say("unlocked\n");	/* opens what was chosen while locked */
	lock_apply(server);
	wlr_seat_keyboard_clear_focus(server->seat);
	server->focused_layer = NULL;
	if (!wl_list_empty(&server->toplevels)) {
		top = wl_container_of(server->toplevels.next, top, link);
		focus_toplevel(top);
	}
}

/* The locker went away without unlocking: stay locked, start it again */
static void lock_destroyed(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, lock_destroy);

	lock_forget(server);
	wlr_log(WLR_ERROR, "the locker died; still locked");
	wl_event_source_timer_update(server->locker_timer, 1000);
}

static void new_lock(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_lock);
	struct wlr_session_lock_v1 *lock = data;

	if (server->lock) {			/* one locker at a time */
		wlr_session_lock_v1_destroy(lock);
		return;
	}
	server->lock = lock;
	server->locked = true;
	server->lock_new_surface.notify = lock_new_surface;
	wl_signal_add(&lock->events.new_surface, &server->lock_new_surface);
	server->lock_unlock.notify = lock_unlocked;
	wl_signal_add(&lock->events.unlock, &server->lock_unlock);
	server->lock_destroy.notify = lock_destroyed;
	wl_signal_add(&lock->events.destroy, &server->lock_destroy);
	wl_event_source_timer_update(server->lock_timer, 0);
	lock_apply(server);
	wlr_session_lock_v1_send_locked(lock);
	shell_say("locked\n");		/* its list closes */
}

static void focus_toplevel(struct toplevel *toplevel)
{
	if (toplevel == NULL)
		return;
	struct server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev = seat->keyboard_state.focused_surface;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;

	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	server->focused_layer = NULL;	/* a raised window takes the keys */
	if (server->locked) {			/* only a call's Phone window */
		const char *id = toplevel->xdg_toplevel->app_id;
		if (!call_over_lock(server) || !id || strcmp(id, PHONE_ID)) {
			lock_apply(server);
			return;
		}
	}
	if (prev == surface)
		return;
	if (prev != NULL) {
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev);
		if (prev_toplevel != NULL)
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
	}
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	keypad_mode(toplevel->xdg_toplevel->app_id);
	/* Focus even with no keyboard yet: one may come */
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard != NULL)
		wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes,
			keyboard->num_keycodes, &keyboard->modifiers);
	else
		wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
}

static struct toplevel *find_app(struct server *server, const char *app_id)
{
	struct toplevel *toplevel;

	wl_list_for_each(toplevel, &server->toplevels, link) {
		const char *id = toplevel->xdg_toplevel->app_id;
		if (id != NULL && strcmp(id, app_id) == 0)
			return toplevel;
	}
	return NULL;
}

static void spawn(const char *cmd)
{
	pid_t pid = fork();

	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	if (pid < 0)
		wlr_log_errno(WLR_ERROR, "fork");
}

/* Ask every app window but the launcher's and the Shell's to close */
static void close_apps(struct server *server)
{
	struct toplevel *toplevel;

	wl_list_for_each(toplevel, &server->toplevels, link) {
		const char *id = toplevel->xdg_toplevel->app_id;
		if (id != NULL && (strcmp(id, LAUNCHER_ID) == 0 || strcmp(id, KEEP_ID) == 0))
			continue;
		wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
	}
}

static void go_home(struct server *server)
{
	struct toplevel *launcher = find_app(server, LAUNCHER_ID);

	struct timespec now;

	if (server->quitting)
		return;
	close_apps(server);
	if (launcher != NULL) {
		/* The main grid, an open folder closed: in front already, it
		 * would not hear of it otherwise (its focus does not change) */
		runtime_say("s22-app-" LAUNCHER_ID ".sock", "open home:\n");
		focus_toplevel(launcher);
		return;
	}
	/* Not again within 3 s, in case it dies at once */
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (server->launched && now.tv_sec - server->launched < 3)
		return;
	server->launched = now.tv_sec;
	spawn(server->launcher_cmd);
}

static void toplevel_map(struct wl_listener *listener, void *data)
{
	struct toplevel *toplevel = wl_container_of(listener, toplevel, map);

	wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
	focus_toplevel(toplevel);
}

static void toplevel_unmap(struct wl_listener *listener, void *data)
{
	struct toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	struct server *server = toplevel->server;

	wl_list_remove(&toplevel->link);
	if (wl_list_empty(&server->toplevels)) {
		go_home(server);
		return;
	}
	if (server->locked) {			/* a call's window went: the lock */
		lock_apply(server);
		return;
	}
	struct toplevel *next = wl_container_of(server->toplevels.next, next, link);
	focus_toplevel(next);
}

/* Every window fills what the bars (s22-statusbar) leave */
static void toplevel_configure(struct toplevel *toplevel)
{
	struct server *server = toplevel->server;
	struct wlr_box *u = &server->usable;

	if (u->width <= 0 || u->height <= 0)
		*u = (struct wlr_box){ 0, 0, server->width, server->height };
	wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, true);
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, u->width, u->height);
	wlr_scene_node_set_position(&toplevel->scene_tree->node, u->x, u->y);
}

static void toplevel_commit(struct wl_listener *listener, void *data)
{
	struct toplevel *toplevel = wl_container_of(listener, toplevel, commit);

	if (toplevel->xdg_toplevel->base->initial_commit)
		toplevel_configure(toplevel);
}

static void toplevel_request_state(struct wl_listener *listener, void *data)
{
	/* Always fullscreen, whatever is asked; the protocol wants a reply */
	struct toplevel *toplevel = wl_container_of(listener, toplevel,
		request_fullscreen);

	if (toplevel->xdg_toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

static void toplevel_request_maximize(struct wl_listener *listener, void *data)
{
	struct toplevel *toplevel = wl_container_of(listener, toplevel,
		request_maximize);

	if (toplevel->xdg_toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

static void toplevel_destroy(struct wl_listener *listener, void *data)
{
	struct toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	free(toplevel);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;
	struct toplevel *toplevel = calloc(1, sizeof(*toplevel));

	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->scene_tree = wlr_scene_xdg_surface_create(server->apps,
		xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

	toplevel->map.notify = toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
	toplevel->destroy.notify = toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
	toplevel->request_maximize.notify = toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize,
		&toplevel->request_maximize);
	toplevel->request_fullscreen.notify = toplevel_request_state;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen,
		&toplevel->request_fullscreen);
}

/* ------------------------------------------------------------ layers --- */

static void arrange_layers(struct server *server)
{
	struct wlr_box full = { 0, 0, server->width, server->height };
	struct wlr_box usable = full;
	struct layer *layer;
	struct toplevel *t;

	wl_list_for_each(layer, &server->layer_surfaces, link)
		if (layer->surface->initialized)
			wlr_scene_layer_surface_v1_configure(layer->scene, &full, &usable);
	if (memcmp(&usable, &server->usable, sizeof(usable)) == 0)
		return;
	server->usable = usable;
	wlr_log(WLR_INFO, "windows get %dx%d at %d,%d", usable.width,
		usable.height, usable.x, usable.y);
	wl_list_for_each(t, &server->toplevels, link)
		toplevel_configure(t);
}

static struct toplevel *top_toplevel(struct server *server)
{
	struct toplevel *toplevel;

	if (wl_list_empty(&server->toplevels))
		return NULL;
	return wl_container_of(server->toplevels.next, toplevel, link);
}

/* The top layer surface that wants the keyboard gets it; else the window */
/* s22-shell's surfaces (notifications, quick settings) may show over the
 * lock; nothing else's */
static bool shell_layer(struct layer *layer)
{
	const char *ns = layer->surface->namespace;

	return ns && !strcmp(ns, "s22-shell");
}

static void layer_place(struct server *server, struct layer *layer)
{
	struct wlr_scene_tree *parent = server->locked && shell_layer(layer) ?
		server->over_lock : server->layers[layer->surface->current.layer];

	if (layer->scene->tree->node.parent != parent)
		wlr_scene_node_reparent(&layer->scene->tree->node, parent);
}

static void layer_focus(struct server *server)
{
	struct layer *layer, *best = NULL;

	if (server->locked) {
		/* The lock has the keys, but s22-shell's list while it is open
		 * (a call over the lock keeps them) */
		if (call_over_lock(server))
			return;
		wl_list_for_each(layer, &server->layer_surfaces, link)
			if (layer->mapped && shell_layer(layer) &&
			    layer->surface->current.keyboard_interactive !=
			    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
				best = layer;
		if (best == NULL) {
			server->focused_layer = NULL;
			lock_focus(server);
			return;
		}
		if (best == server->focused_layer)
			return;
	}

	wl_list_for_each(layer, &server->layer_surfaces, link) {
		struct wlr_layer_surface_v1 *ls = layer->surface;
		if (!layer->mapped || ls->current.keyboard_interactive ==
		    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE ||
		    ls->current.layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP)
			continue;
		if (best == NULL || ls->current.layer > best->surface->current.layer)
			best = layer;
	}
	if (best == server->focused_layer && !server->locked)
		return;
	if (best == NULL) {
		server->focused_layer = NULL;
		struct toplevel *top = top_toplevel(server);
		if (top != NULL) {
			wlr_seat_keyboard_clear_focus(server->seat);
			focus_toplevel(top);
		}
		return;
	}
	server->focused_layer = best;
	keypad_mode(best->surface->namespace);
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard != NULL)
		wlr_seat_keyboard_notify_enter(server->seat, best->surface->surface,
			keyboard->keycodes, keyboard->num_keycodes,
			&keyboard->modifiers);
	else
		wlr_seat_keyboard_notify_enter(server->seat, best->surface->surface,
			NULL, 0, NULL);
}

static void layer_map(struct wl_listener *listener, void *data)
{
	struct layer *layer = wl_container_of(listener, layer, map);

	layer->mapped = true;
	arrange_layers(layer->server);
	layer_focus(layer->server);
}

static void layer_unmap(struct wl_listener *listener, void *data)
{
	struct layer *layer = wl_container_of(listener, layer, unmap);
	struct server *server = layer->server;

	layer->mapped = false;
	if (server->focused_layer == layer) {
		server->focused_layer = NULL;
		struct toplevel *top = top_toplevel(server);
		wlr_seat_keyboard_clear_focus(server->seat);
		if (top != NULL)
			focus_toplevel(top);
	}
	layer_focus(server);
}

static void layer_commit(struct wl_listener *listener, void *data)
{
	struct layer *layer = wl_container_of(listener, layer, commit);
	struct wlr_layer_surface_v1 *ls = layer->surface;
	struct server *server = layer->server;

	if (!ls->initial_commit && ls->current.committed == 0)
		return;
	layer_place(server, layer);
	arrange_layers(server);
	if (layer->mapped)
		layer_focus(server);
}

static void layer_destroy(struct wl_listener *listener, void *data)
{
	struct layer *layer = wl_container_of(listener, layer, destroy);

	if (layer->server->focused_layer == layer)
		layer->server->focused_layer = NULL;
	wl_list_remove(&layer->link);
	wl_list_remove(&layer->map.link);
	wl_list_remove(&layer->unmap.link);
	wl_list_remove(&layer->commit.link);
	wl_list_remove(&layer->destroy.link);
	free(layer);
}

static void server_new_layer_surface(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		new_layer_surface);
	struct wlr_layer_surface_v1 *ls = data;

	if (server->output == NULL) {
		wlr_layer_surface_v1_destroy(ls);
		return;
	}
	ls->output = server->output->wlr_output;
	struct layer *layer = calloc(1, sizeof(*layer));
	layer->server = server;
	layer->surface = ls;
	layer->scene = wlr_scene_layer_surface_v1_create(
		server->layers[ls->pending.layer], ls);
	ls->data = layer;
	layer->map.notify = layer_map;
	wl_signal_add(&ls->surface->events.map, &layer->map);
	layer->unmap.notify = layer_unmap;
	wl_signal_add(&ls->surface->events.unmap, &layer->unmap);
	layer->commit.notify = layer_commit;
	wl_signal_add(&ls->surface->events.commit, &layer->commit);
	layer->destroy.notify = layer_destroy;
	wl_signal_add(&ls->events.destroy, &layer->destroy);
	wl_list_insert(&server->layer_surfaces, &layer->link);
}

/* "list" to s22-shell; start it if it does not answer */
static void shell_list(struct server *server)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	struct timespec now;
	int fd;

	if (dir == NULL)
		return;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/s22-shell.sock", dir);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
	    send(fd, "list\n", 5, MSG_NOSIGNAL) == 5) {
		close(fd);
		return;
	}
	if (fd >= 0)
		close(fd);
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (server->shell_started && now.tv_sec - server->shell_started < 3)
		return;
	server->shell_started = now.tv_sec;
	wlr_log(WLR_INFO, "starting the shell");
	spawn(server->shell_cmd);
}

static void popup_commit(struct wl_listener *listener, void *data)
{
	struct popup *popup = wl_container_of(listener, popup, commit);

	if (popup->xdg_popup->base->initial_commit)
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
}

static void popup_destroy(struct wl_listener *listener, void *data)
{
	struct popup *popup = wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_popup *xdg_popup = data;
	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);

	if (parent == NULL || parent->data == NULL)
		return;
	struct popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent->data,
		xdg_popup->base);
	popup->commit.notify = popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ------------------------------------------------------------- input --- */

static void keyboard_modifiers(struct wl_listener *listener, void *data)
{
	struct keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);

	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

/* ------------------------------------------------- calls and volume --- */

static void call_state(struct server *server)
{
	char line[160];
	bool in_call = false, near = false;
	FILE *f = fopen(CALL_STATE, "re");

	server->speaker = server->ringing = false;
	while (f && fgets(line, sizeof(line), f)) {
		if (strncmp(line, "call ", 5) == 0)
			in_call = true;
		if (strncmp(line, "call ", 5) == 0 && strstr(line, " incoming "))
			server->ringing = true;
		else if (strncmp(line, "near=1", 6) == 0)
			near = true;
		else if (strncmp(line, "speaker=1", 9) == 0)
			server->speaker = true;
	}
	if (f)
		fclose(f);
	if (in_call == server->in_call && near == server->near)
		return;
	bool was_dark = server->in_call && server->near;
	server->in_call = in_call;
	server->near = near;
	wlr_log(WLR_INFO, "call %s, %s", in_call ? "up" : "none", near ? "near" : "far");
	if (was_dark && !(in_call && near))
		wake(server);		/* away from the ear */
	else
		display_apply(server);
	if (server->locked)
		lock_apply(server);	/* a call shows over the lock, or ends */
	lock_check(server);
}

static int on_call_dir(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

	while (read(fd, buf, sizeof(buf)) > 0)
		;
	call_state(data);
	return 0;
}

/* Watch CALL_DIR; s22-calld makes it, so try again until it is there */
static int call_watch_open(void *data)
{
	struct server *server = data;

	if (server->call_watch < 0)
		server->call_watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (server->call_watch >= 0 &&
	    inotify_add_watch(server->call_watch, CALL_DIR, IN_MOVED_TO | IN_CLOSE_WRITE) >= 0) {
		wl_event_loop_add_fd(wl_display_get_event_loop(server->display),
				     server->call_watch, WL_EVENT_READABLE, on_call_dir, server);
		call_state(server);
		return 0;
	}
	wl_event_source_timer_update(server->call_retry, 10000);
	return 0;
}

static int setting_int(const char *key, int def)
{
	char line[128];
	size_t kl = strlen(key);
	int v = def;
	FILE *f = fopen(SETTINGS, "re");

	while (f && fgets(line, sizeof(line), f))
		if (strncmp(line, key, kl) == 0 && line[kl] == '=')
			v = atoi(line + kl + 1);
	if (f)
		fclose(f);
	return v;
}

/* key=value in the settings file, the other lines kept */
static void setting_set_int(const char *key, int value)
{
	char line[256], tmp[128];
	size_t kl = strlen(key);
	FILE *in = fopen(SETTINGS, "re"), *out;

	snprintf(tmp, sizeof(tmp), "%s.%d", SETTINGS, (int)getpid());
	out = fopen(tmp, "we");
	if (out == NULL) {
		if (in)
			fclose(in);
		return;
	}
	while (in && fgets(line, sizeof(line), in))
		if (!(strncmp(line, key, kl) == 0 && line[kl] == '='))
			fputs(line, out);
	fprintf(out, "%s=%d\n", key, value);
	if (in)
		fclose(in);
	fchmod(fileno(out), 0664);
	if (fclose(out) == 0)
		rename(tmp, SETTINGS);
	else
		unlink(tmp);
}

static void mixer(const char *control, int value)
{
	char cmd[160];

	snprintf(cmd, sizeof(cmd), "amixer -q -c 0 cset name='%s' %d", control, value);
	spawn(cmd);
}

/* What s22-calld puts on the earpiece for a call volume level */
static int call_volume_raw(int level)
{
	return 84 + (level - CALL_VOL_DEF) * 4;	/* 0 dB at the default, 4 dB a step */
}

/* A line to a socket in the runtime directory, if anyone listens */
static void runtime_say(const char *name, const char *msg)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd;

	if (dir == NULL)
		return;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/%s", dir, name);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
		send(fd, msg, strlen(msg), MSG_NOSIGNAL);
	if (fd >= 0)
		close(fd);
}

static void shell_say(const char *msg)
{
	runtime_say("s22-shell.sock", msg);
}

/* "silence" to s22-calld: the ringing call stops ringing */
static void silence_call(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	snprintf(sa.sun_path, sizeof(sa.sun_path), "/run/s22-call.sock");
	if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
		send(fd, "silence\n", 8, MSG_NOSIGNAL);
	if (fd >= 0)
		close(fd);
}

static void media_volume(int level)
{
	char cmd[96];

	snprintf(cmd, sizeof(cmd), "wpctl set-volume @DEFAULT_AUDIO_SINK@ %.3f",
		 level / (double)VOL_MAX);
	spawn(cmd);
}

static void volume_key(struct server *server, int dir)
{
	struct toplevel *front = wl_list_empty(&server->toplevels) ? NULL :
		wl_container_of(server->toplevels.next, front, link);
	const char *id = front ? front->xdg_toplevel->app_id : NULL;
	bool call = server->in_call || (id && strcmp(id, PHONE_ID) == 0);
	int max = call ? CALL_VOL_MAX : VOL_MAX, lo = call ? 1 : 0;
	int v = setting_int(call ? "call_volume" : "volume", call ? CALL_VOL_DEF : VOL_DEF);
	char msg[64];

	if (server->ringing) {
		silence_call();
		return;
	}
	v += dir;
	v = v < lo ? lo : v > max ? max : v;
	setting_set_int(call ? "call_volume" : "volume", v);
	if (call && server->in_call && server->speaker)	/* as s22-calld has it */
		mixer("Speaker Volume", 184 - (CALL_VOL_MAX - v) * 4);
	else if (call && server->in_call)
		mixer("RX1 Digital Volume", call_volume_raw(v));
	else if (!call)
		media_volume(v);
	snprintf(msg, sizeof(msg), "volume %s %d %d\n", call ? "call" : "media", v, max);
	shell_say(msg);
}

/* The flashlight on or off */
static void flashlight(void)
{
	char v[4] = "0";
	int fd;

	fd = open(FLASH, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (read(fd, v, sizeof(v) - 1) < 0)
			v[0] = '0';
		close(fd);
	}
	fd = open(FLASH, O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (write(fd, v[0] != '0' ? "0" : "1", 1) != 1)
			wlr_log_errno(WLR_ERROR, FLASH);
		close(fd);
	}
	wlr_log(WLR_INFO, "flashlight %s", v[0] != '0' ? "off" : "on");
}

/* The side key held, the lid open: the flashlight */
static int side_held(void *data)
{
	struct server *server = data;

	if (!server->side_down || server->lid_closed)
		return 0;
	server->side_used = true;
	flashlight();
	return 0;
}

/* End held: the power menu */
static int power_held(void *data)
{
	struct server *server = data;

	if (!server->power_down || server->lid_closed)
		return 0;
	wake(server);
	wlr_log(WLR_INFO, "power menu");
	if (server->locked && !call_over_lock(server)) {
		/* The locker's own menu: End reaches it as KEY_POWER */
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		uint32_t ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
		/* From s22-shell's list too: it closes when the keys go */
		server->focused_layer = NULL;
		lock_focus(server);
		if (server->lock_surface) {
			wlr_seat_keyboard_notify_key(server->seat, ms, KEY_POWER,
				WL_KEYBOARD_KEY_STATE_PRESSED);
			wlr_seat_keyboard_notify_key(server->seat, ms + 1, KEY_POWER,
				WL_KEYBOARD_KEY_STATE_RELEASED);
		}
		return 0;
	}
	shell_say("power\n");
	return 0;
}

/* Pressed: the notifications (s22-shell); held: the flashlight */
static void side_key(struct server *server, bool pressed)
{
	wlr_log(WLR_DEBUG, "side key %s", pressed ? "down" : "up");
	if (pressed) {
		server->side_down = true;
		server->side_used = false;
		server->side_woke = !server->lid_closed && wake(server);
		wl_event_source_timer_update(server->side_timer, SIDE_HOLD_MS);
		return;
	}
	if (!server->side_down)
		return;
	server->side_down = false;
	wl_event_source_timer_update(server->side_timer, 0);
	/* A press that only woke the display, or with the lid closed (the
	 * outer screen's), opens nothing */
	if (!server->side_used && !server->side_woke && !server->lid_closed &&
	    !(server->in_call && server->near))
		shell_list(server);
}

static void keyboard_key(struct wl_listener *listener, void *data)
{
	struct keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;

	if (event->keycode == KEY_NUMERIC_B) {	/* the lid closed too */
		side_key(server, pressed);
		return;
	}
	/* The lid closed: side + Volume Down, the flashlight (the volume keys
	 * are otherwise s22-outerd's, which leaves this chord alone) */
	if (server->lid_closed && event->keycode == KEY_VOLUMEDOWN && pressed &&
	    server->side_down) {
		server->side_used = true;
		flashlight();
		return;
	}
	if (server->lid_closed)
		return;
	if (event->keycode == KEY_VOLUMEUP || event->keycode == KEY_VOLUMEDOWN) {
		/* End + Volume Down: a screenshot (never during a call), and no
		 * power menu from that End press */
		if (pressed && event->keycode == KEY_VOLUMEDOWN && server->power_down &&
		    !server->in_call) {
			server->power_down = false;
			wl_event_source_timer_update(server->power_timer, 0);
			spawn("s22-screenshot");
			return;
		}
		if (pressed)
			volume_key(server, event->keycode == KEY_VOLUMEUP ? 1 : -1);
		return;
	}
	if (server->in_call && server->near)	/* at the ear: nothing */
		return;
	if (server->locked && !call_over_lock(server) &&
	    (event->keycode == KEY_HOMEPAGE || event->keycode == KEY_APPSELECT)) {
		if (pressed) {
			wake(server);
			server->focused_layer = NULL;
			lock_focus(server);	/* s22-shell's list closes */
		}
		return;
	}
	if (event->keycode == KEY_POWER) {	/* wakes; held, the power menu */
		server->power_down = pressed;
		wl_event_source_timer_update(server->power_timer, pressed ? POWER_HOLD_MS : 0);
		if (pressed)
			wake(server);
		return;
	}
	if (pressed && wake(server)) {
		server->swallowed = event->keycode;
		return;
	}
	if (!pressed && event->keycode == server->swallowed) {
		server->swallowed = 0;
		return;
	}
	if (event->keycode == KEY_HOMEPAGE) {
		if (pressed)
			go_home(server);
		return;
	}

	if (event->keycode == KEY_APPSELECT) {
		if (pressed && server->toplevels.next->next != &server->toplevels) {
			struct toplevel *prev = wl_container_of(
				server->toplevels.next->next, prev, link);
			focus_toplevel(prev);
		}
		return;
	}
	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
		event->keycode, event->state);
}

static void keyboard_destroy(struct wl_listener *listener, void *data)
{
	struct keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	struct server *server = keyboard->server;

	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	/* The seat's keyboard went away: another one takes over, or the
	 * focused window gets no keymap and no keys */
	if (wlr_seat_get_keyboard(server->seat) == keyboard->wlr_keyboard ||
	    wlr_seat_get_keyboard(server->seat) == NULL) {
		struct keyboard *next = wl_list_empty(&server->keyboards) ? NULL :
			wl_container_of(server->keyboards.next, next, link);
		wlr_seat_set_keyboard(server->seat, next ? next->wlr_keyboard : NULL);
	}
	free(keyboard);
}

static void new_keyboard(struct server *server, struct wlr_input_device *device)
{
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct keyboard *keyboard = calloc(1, sizeof(*keyboard));
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(server->xkb, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 500);

	keyboard->modifiers.notify = keyboard_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void lid_toggle(struct wl_listener *listener, void *data)
{
	struct lid *lid = wl_container_of(listener, lid, toggle);
	struct server *server = lid->server;
	struct wlr_switch_toggle_event *event = data;

	if (event->switch_type != WLR_SWITCH_TYPE_LID)
		return;
	server->lid_closed = event->switch_state == WLR_SWITCH_STATE_ON;
	wlr_log(WLR_INFO, "lid %s", server->lid_closed ? "closed" : "open");
	if (server->lid_closed)
		display_apply(server);
	else
		wake(server);
}

static void lid_destroy(struct wl_listener *listener, void *data)
{
	struct lid *lid = wl_container_of(listener, lid, destroy);

	wl_list_remove(&lid->toggle.link);
	wl_list_remove(&lid->destroy.link);
	free(lid);
}

static void new_switch(struct server *server, struct wlr_input_device *device)
{
	struct wlr_switch *sw = wlr_switch_from_input_device(device);
	struct lid *lid = calloc(1, sizeof(*lid));

	lid->server = server;
	lid->toggle.notify = lid_toggle;
	wl_signal_add(&sw->events.toggle, &lid->toggle);
	lid->destroy.notify = lid_destroy;
	wl_signal_add(&device->events.destroy, &lid->destroy);
}

/* The surface under layout point lx, ly, and the point in its coordinates */
static struct wlr_surface *surface_at(struct server *server, double lx,
				      double ly, double *sx, double *sy)
{
	struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node,
		lx, ly, sx, sy);

	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER)
		return NULL;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(
		wlr_scene_buffer_from_node(node));
	return ss ? ss->surface : NULL;
}

static void touch_down(struct wl_listener *listener, void *data)
{
	struct touch *t = wl_container_of(listener, t, down);
	struct server *server = t->server;
	struct wlr_touch_down_event *ev = data;
	double lx = ev->x * server->width, ly = ev->y * server->height, sx, sy;
	uint32_t bit = 1u << (ev->touch_id & 31);

	if (server->lid_closed)
		return;
	if (wake(server)) {		/* also at the ear: ignored */
		server->touch_swallowed |= bit;
		return;
	}
	struct wlr_surface *surface = surface_at(server, lx, ly, &sx, &sy);
	if (surface == NULL)
		return;
	server->touch_off[ev->touch_id & 15][0] = lx - sx;
	server->touch_off[ev->touch_id & 15][1] = ly - sy;
	wlr_seat_touch_notify_down(server->seat, surface, ev->time_msec,
		ev->touch_id, sx, sy);
}

static void touch_motion(struct wl_listener *listener, void *data)
{
	struct touch *t = wl_container_of(listener, t, motion);
	struct server *server = t->server;
	struct wlr_touch_motion_event *ev = data;
	double *off = server->touch_off[ev->touch_id & 15];

	if (server->touch_swallowed & (1u << (ev->touch_id & 31)) ||
	    wlr_seat_touch_get_point(server->seat, ev->touch_id) == NULL)
		return;
	idle_reset(server);
	wlr_seat_touch_notify_motion(server->seat, ev->time_msec, ev->touch_id,
		ev->x * server->width - off[0], ev->y * server->height - off[1]);
}

static void touch_up(struct wl_listener *listener, void *data)
{
	struct touch *t = wl_container_of(listener, t, up);
	struct server *server = t->server;
	struct wlr_touch_up_event *ev = data;

	server->touch_swallowed &= ~(1u << (ev->touch_id & 31));
	if (wlr_seat_touch_get_point(server->seat, ev->touch_id) != NULL)
		wlr_seat_touch_notify_up(server->seat, ev->time_msec, ev->touch_id);
}

static void touch_cancel(struct wl_listener *listener, void *data)
{
	struct touch *t = wl_container_of(listener, t, cancel);
	struct wlr_touch_cancel_event *ev = data;
	struct wlr_touch_up_event up = { .touch = ev->touch,
		.time_msec = ev->time_msec, .touch_id = ev->touch_id };

	touch_up(&t->up, &up);
}

static void touch_frame(struct wl_listener *listener, void *data)
{
	struct touch *t = wl_container_of(listener, t, frame);

	wlr_seat_touch_notify_frame(t->server->seat);
}

static void touch_destroy(struct wl_listener *listener, void *data)
{
	struct touch *t = wl_container_of(listener, t, destroy);

	t->server->touch_devices--;
	wl_list_remove(&t->link);
	wl_list_remove(&t->down.link);
	wl_list_remove(&t->up.link);
	wl_list_remove(&t->motion.link);
	wl_list_remove(&t->cancel.link);
	wl_list_remove(&t->frame.link);
	wl_list_remove(&t->destroy.link);
	free(t);
}

static void new_touch(struct server *server, struct wlr_input_device *device)
{
	struct wlr_touch *wt = wlr_touch_from_input_device(device);
	struct touch *t = calloc(1, sizeof(*t));

	t->server = server;
	t->down.notify = touch_down;
	wl_signal_add(&wt->events.down, &t->down);
	t->up.notify = touch_up;
	wl_signal_add(&wt->events.up, &t->up);
	t->motion.notify = touch_motion;
	wl_signal_add(&wt->events.motion, &t->motion);
	t->cancel.notify = touch_cancel;
	wl_signal_add(&wt->events.cancel, &t->cancel);
	t->frame.notify = touch_frame;
	wl_signal_add(&wt->events.frame, &t->frame);
	t->destroy.notify = touch_destroy;
	wl_signal_add(&device->events.destroy, &t->destroy);
	server->touch_devices++;
	if (wlr_input_device_is_libinput(device))
		t->li = wlr_libinput_get_device_handle(device);
	wl_list_insert(&server->touches, &t->link);
	touch_calibrate(server, t);
}

static void server_new_input(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	wlr_log(WLR_INFO, "input: %s (type %d)", device->name, device->type);
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_TOUCH:
		new_touch(server, device);
		break;
	case WLR_INPUT_DEVICE_SWITCH:
		new_switch(server, device);
		break;
	default:
		break;
	}
	wlr_seat_set_capabilities(server->seat,
		(wl_list_empty(&server->keyboards) ? 0 : WL_SEAT_CAPABILITY_KEYBOARD) |
		(server->touch_devices ? WL_SEAT_CAPABILITY_TOUCH : 0));
}

static void seat_request_set_selection(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;

	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* ------------------------------------------------------- the clipboard --- */

#define CLIP_MAX	(1 << 20)	/* text kept, at most */

static const char *const clip_types[] = {
	"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING",
};

struct keeper {
	struct wlr_data_source base;
	struct server *server;
};

/* A paste of the kept text: written by a child, so a slow reader never
 * holds up the compositor (children are not waited for: SA_NOCLDWAIT) */
static void keeper_send(struct wlr_data_source *source, const char *mime, int32_t fd)
{
	struct keeper *k = wl_container_of(source, k, base);
	struct server *server = k->server;

	if (fork() == 0) {
		const char *p = server->clip;
		size_t left = server->clip_len;
		int flags = fcntl(fd, F_GETFL);
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
		while (left > 0) {
			ssize_t n = write(fd, p, left);
			if (n <= 0)
				break;
			p += n;
			left -= n;
		}
		_exit(0);
	}
	close(fd);
}

static void keeper_destroy(struct wlr_data_source *source)
{
	struct keeper *k = wl_container_of(source, k, base);

	if (k->server->keeper == source)
		k->server->keeper = NULL;
	free(k);
}

static const struct wlr_data_source_impl keeper_impl = {
	.send = keeper_send,
	.destroy = keeper_destroy,
};

static void clip_offer(struct server *server)
{
	struct keeper *k = calloc(1, sizeof(*k));

	if (k == NULL)
		return;
	k->server = server;
	wlr_data_source_init(&k->base, &keeper_impl);
	for (size_t i = 0; i < sizeof(clip_types) / sizeof(clip_types[0]); i++) {
		char **slot = wl_array_add(&k->base.mime_types, sizeof(char *));
		if (slot)
			*slot = strdup(clip_types[i]);
	}
	server->keeper = &k->base;
	wlr_seat_set_selection(server->seat, &k->base,
			       wl_display_next_serial(server->display));
}

static void clip_read_end(struct server *server, bool keep)
{
	if (server->clip_read) {
		wl_event_source_remove(server->clip_read);
		server->clip_read = NULL;
	}
	if (server->clip_fd >= 0) {
		close(server->clip_fd);
		server->clip_fd = -1;
	}
	if (keep && server->clip_buf_len > 0) {
		free(server->clip);
		server->clip = server->clip_buf;
		server->clip_len = server->clip_buf_len;
	} else {
		free(server->clip_buf);
	}
	server->clip_buf = NULL;
	server->clip_buf_len = 0;
}

static int clip_readable(int fd, uint32_t mask, void *data)
{
	struct server *server = data;
	char buf[16384];
	ssize_t n;

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		if (server->clip_buf_len + n > CLIP_MAX) {
			clip_read_end(server, false);	/* too big to keep */
			return 0;
		}
		char *grown = realloc(server->clip_buf, server->clip_buf_len + n);
		if (grown == NULL) {
			clip_read_end(server, false);
			return 0;
		}
		server->clip_buf = grown;
		memcpy(server->clip_buf + server->clip_buf_len, buf, n);
		server->clip_buf_len += n;
	}
	if (n == 0 || (n < 0 && errno != EAGAIN) || (mask & WL_EVENT_HANGUP))
		clip_read_end(server, true);
	return 0;
}

/* The selection changed: read a new copy's text; when an app's copy goes
 * away with the app, put the kept text back */
static void seat_set_selection(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, set_selection);
	struct wlr_data_source *src = server->seat->selection_source;

	if (src == NULL) {
		if (server->clip_from != NULL) {
			server->clip_from = NULL;
			if (server->clip_len > 0)
				clip_offer(server);
		}
		return;
	}
	if (src == server->keeper)
		return;
	clip_read_end(server, false);
	server->clip_from = src;
	const char *type = NULL;
	char **t;
	for (size_t i = 0; type == NULL && i < sizeof(clip_types) / sizeof(clip_types[0]); i++)
		wl_array_for_each(t, &src->mime_types)
			if (strcmp(*t, clip_types[i]) == 0) {
				type = clip_types[i];
				break;
			}
	if (type == NULL) {		/* not text: nothing to keep */
		free(server->clip);
		server->clip = NULL;
		server->clip_len = 0;
		return;
	}
	int fds[2];
	if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
		return;
	server->clip_fd = fds[0];
	server->clip_read = wl_event_loop_add_fd(wl_display_get_event_loop(server->display),
		fds[0], WL_EVENT_READABLE, clip_readable, server);
	wlr_data_source_send(src, type, fds[1]);	/* closes fds[1] */
}

/* ------------------------------------------------------- GPU buffers --- */

/*
 * A commit with a buffer the client's GPU is still drawing waits until it
 * is done (D061), as GNOME (Mutter) and KDE (KWin) do; wlroots relies on
 * implicit sync instead. Without it Doom showed runs of black frames here
 * (about 37 of 85 sampled), cured in the game only by glFinish at a third
 * of its frame rate; with it, none. The kernel hands out the buffer's
 * pending writes as a sync_file (DMA_BUF_IOCTL_EXPORT_SYNC_FILE); the
 * commit is held, as wlroots does for explicit sync, until that signals,
 * and the client draws its next frame meanwhile.
 */
struct gpu_surface {
	struct wlr_surface *surface;
	struct wl_listener client_commit, destroy;
};

struct gpu_wait {
	struct wlr_surface *surface;
	uint32_t seq;
	int fd;
	struct wl_event_source *source;
	struct wl_listener destroy;
};

static void gpu_wait_done(struct gpu_wait *w)
{
	wlr_surface_unlock_cached(w->surface, w->seq);
	wl_list_remove(&w->destroy.link);
	wl_event_source_remove(w->source);
	close(w->fd);
	free(w);
}

static int gpu_wait_ready(int fd, uint32_t mask, void *data)
{
	gpu_wait_done(data);
	return 0;
}

static void gpu_wait_surface_destroy(struct wl_listener *listener, void *data)
{
	struct gpu_wait *w = wl_container_of(listener, w, destroy);

	gpu_wait_done(w);
}

static void gpu_client_commit(struct wl_listener *listener, void *data)
{
	struct gpu_surface *gs = wl_container_of(listener, gs, client_commit);
	struct wlr_surface *surface = gs->surface;
	struct wlr_dmabuf_attributes dmabuf;
	struct dma_buf_export_sync_file sync = { .flags = DMA_BUF_SYNC_READ, .fd = -1 };

	if (!(surface->pending.committed & WLR_SURFACE_STATE_BUFFER) ||
	    surface->pending.buffer == NULL ||
	    !wlr_buffer_get_dmabuf(surface->pending.buffer, &dmabuf))
		return;
	if (ioctl(dmabuf.fd[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &sync) != 0)
		return;
	struct pollfd p = { .fd = sync.fd, .events = POLLIN };
	if (poll(&p, 1, 0) == 1) {	/* drawn already */
		close(sync.fd);
		return;
	}
	struct gpu_wait *w = calloc(1, sizeof(*w));
	if (w == NULL) {
		close(sync.fd);
		return;
	}
	w->surface = surface;
	w->fd = sync.fd;
	w->source = wl_event_loop_add_fd(
		wl_display_get_event_loop(wl_client_get_display(
			wl_resource_get_client(surface->resource))),
		sync.fd, WL_EVENT_READABLE, gpu_wait_ready, w);
	if (w->source == NULL) {
		close(sync.fd);
		free(w);
		return;
	}
	w->seq = wlr_surface_lock_pending(surface);
	w->destroy.notify = gpu_wait_surface_destroy;
	wl_signal_add(&surface->events.destroy, &w->destroy);
}

static void gpu_surface_destroy(struct wl_listener *listener, void *data)
{
	struct gpu_surface *gs = wl_container_of(listener, gs, destroy);

	wl_list_remove(&gs->client_commit.link);
	wl_list_remove(&gs->destroy.link);
	free(gs);
}

static void new_surface(struct wl_listener *listener, void *data)
{
	struct wlr_surface *surface = data;
	struct gpu_surface *gs = calloc(1, sizeof(*gs));

	if (gs == NULL)
		return;
	gs->surface = surface;
	gs->client_commit.notify = gpu_client_commit;
	wl_signal_add(&surface->events.client_commit, &gs->client_commit);
	gs->destroy.notify = gpu_surface_destroy;
	wl_signal_add(&surface->events.destroy, &gs->destroy);
}

/* ------------------------------------------------------------ output --- */

static void output_frame(struct wl_listener *listener, void *data)
{
	struct output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		output->server->scene, output->wlr_output);
	struct timespec now;

	wlr_scene_output_commit(scene_output, NULL);
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data)
{
	struct output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;

	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data)
{
	struct output *output = wl_container_of(listener, output, destroy);

	if (output->server->output == output)
		output->server->output = NULL;
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	free(output);
}

/*
 * The GPU was reset (the kernel's hangcheck, after a client's job hung it:
 * D077). The GL context went with it, and nothing draws again until there
 * is a new one: a new renderer and allocator, the display rendering with
 * them, the compositor importing client buffers with them; then the old
 * ones go. Not in the "lost" signal itself: the old renderer is in the
 * middle of a frame then, and the signal's own markers sit in its listener
 * list, which wlr_renderer_destroy() requires empty.
 */
static void renderer_renew(void *data)
{
	struct server *server = data;
	struct wlr_renderer *renderer, *old_renderer = server->renderer;
	struct wlr_allocator *allocator, *old_allocator = server->allocator;

	server->renew = NULL;
	wlr_log(WLR_INFO, "GPU reset: making a new renderer");
	renderer = wlr_renderer_autocreate(server->backend);
	if (renderer == NULL) {
		wlr_log(WLR_ERROR, "GPU reset: no new renderer");
		return;
	}
	allocator = wlr_allocator_autocreate(server->backend, renderer);
	if (allocator == NULL) {
		wlr_log(WLR_ERROR, "GPU reset: no new allocator");
		wlr_renderer_destroy(renderer);
		return;
	}
	server->renderer = renderer;
	server->allocator = allocator;
	wl_list_remove(&server->renderer_lost.link);
	wl_signal_add(&renderer->events.lost, &server->renderer_lost);
	wlr_compositor_set_renderer(server->compositor, renderer);
	if (server->output != NULL)
		wlr_output_init_render(server->output->wlr_output, allocator, renderer);
	wlr_allocator_destroy(old_allocator);
	wlr_renderer_destroy(old_renderer);
	if (server->output != NULL)
		wlr_output_schedule_frame(server->output->wlr_output);
}

static void renderer_lost(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, renderer_lost);

	if (server->renew == NULL)
		server->renew = wl_event_loop_add_idle(
			wl_display_get_event_loop(server->display), renderer_renew, server);
}

static void server_new_output(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	if (server->output != NULL) {
		wlr_log(WLR_INFO, "ignoring a second display, %s", wlr_output->name);
		return;
	}
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	} else {
		/* No modes (a headless session): the phone's panel */
		int w = 480, h = 640;
		const char *size = getenv("S22_WM_SIZE");
		if (size && sscanf(size, "%dx%d", &w, &h) != 2)
			w = 480, h = 640;
		wlr_output_state_set_custom_mode(&state, w, h, 60000);
	}
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	server->output = output;
	server->display_on = wlr_output->enabled;
	display_apply(server);		/* the lid may be closed already */
	server->width = wlr_output->width;
	server->height = wlr_output->height;
	wlr_log(WLR_INFO, "display %s, %dx%d", wlr_output->name,
		server->width, server->height);

	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output,
		scene_output);

	/* Windows that came before the display get its size now */
	struct toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link)
		toplevel_configure(toplevel);
}

/* ----------------------------------------------------- control socket --- */

/* The control socket's keyboard: keys sent with "key", taken like the
 * keypad's (made at the first, a keyboard among the others) */
static const struct wlr_keyboard_impl ctl_keyboard_impl = { .name = "s22-wm-ctl" };
static struct wlr_keyboard ctl_keyboard;
static bool ctl_keyboard_made;

static void ctl_key(struct server *server, uint32_t code, bool down)
{
	struct timespec now;

	if (!ctl_keyboard_made) {
		wlr_keyboard_init(&ctl_keyboard, &ctl_keyboard_impl, "s22-wm-ctl");
		new_keyboard(server, &ctl_keyboard.base);
		wlr_seat_set_capabilities(server->seat, WL_SEAT_CAPABILITY_KEYBOARD |
			(server->touch_devices ? WL_SEAT_CAPABILITY_TOUCH : 0));
		ctl_keyboard_made = true;
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	struct wlr_keyboard_key_event event = {
		.time_msec = now.tv_sec * 1000 + now.tv_nsec / 1000000,
		.keycode = code,
		.update_state = true,
		.state = down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
	};
	wlr_keyboard_notify_key(&ctl_keyboard, &event);
}

static int ctl_client(int fd, uint32_t mask, void *data)
{
	struct server *server = data;
	char buf[256], out[4096];
	ssize_t n = 0;

	/* Commands are tiny and sent at once; wait briefly for the line */
	struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0)
		goto done;
	buf[n] = 0;
	buf[strcspn(buf, "\r\n")] = 0;

	if (strcmp(buf, "home") == 0) {
		wake(server);
		go_home(server);
		snprintf(out, sizeof(out), "ok\n");
	} else if (strncmp(buf, "raise ", 6) == 0) {
		struct toplevel *toplevel = find_app(server, buf + 6);
		if (toplevel != NULL) {
			wake(server);
			focus_toplevel(toplevel);
		}
		snprintf(out, sizeof(out), "%s\n", toplevel ? "ok" : "none");
	} else if (strcmp(buf, "locked") == 0) {
		snprintf(out, sizeof(out), "%s\n", server->locked ? "yes" : "no");
	} else if (strcmp(buf, "wake") == 0) {
		wake(server);
		snprintf(out, sizeof(out), "ok\n");
	} else if (strncmp(buf, "key ", 4) == 0 || strncmp(buf, "keydown ", 8) == 0 ||
		   strncmp(buf, "keyup ", 6) == 0) {
		bool down = buf[3] == 'd', up = buf[3] == 'u';
		char *p = strchr(buf, ' '), *end;
		int n = 0;
		for (long code; (code = strtol(p, &end, 10)), end != p; p = end, n++) {
			if (code <= 0 || code > 767)
				break;
			if (!up)
				ctl_key(server, code, true);
			if (!down)
				ctl_key(server, code, false);
		}
		snprintf(out, sizeof(out), n ? "ok\n" : "error: no key codes\n");
	} else if (strcmp(buf, "list") == 0) {
		struct toplevel *toplevel;
		size_t len = 0;
		out[0] = 0;
		wl_list_for_each(toplevel, &server->toplevels, link) {
			const char *id = toplevel->xdg_toplevel->app_id;
			if (len < sizeof(out))
				len += snprintf(out + len, sizeof(out) - len, "%s\n",
					id ? id : "?");
		}
	} else {
		snprintf(out, sizeof(out), "error: unknown command\n");
	}
	/* The asker may not wait for the reply */
	if (send(fd, out, strlen(out), MSG_NOSIGNAL) < 0)
		wlr_log_errno(WLR_DEBUG, "ctl reply");
done:
	close(fd);
	return 0;
}

static int ctl_accept(int fd, uint32_t mask, void *data)
{
	int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);

	if (c >= 0)
		ctl_client(c, 0, data);
	return 0;
}

static bool ctl_open(struct server *server)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un sa = { .sun_family = AF_UNIX };

	if (dir == NULL)
		return false;
	snprintf(server->ctl_path, sizeof(server->ctl_path), "%s/s22-wm.sock", dir);
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", server->ctl_path);
	unlink(server->ctl_path);
	server->ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (server->ctl_fd < 0 ||
	    bind(server->ctl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    listen(server->ctl_fd, 4) < 0) {
		wlr_log_errno(WLR_ERROR, "%s", server->ctl_path);
		return false;
	}
	wl_event_loop_add_fd(wl_display_get_event_loop(server->display),
		server->ctl_fd, WL_EVENT_READABLE, ctl_accept, server);
	return true;
}

/* -------------------------------------------------------------- main --- */

static int on_signal(int sig, void *data)
{
	wl_display_terminate(data);
	return 0;
}

int main(int argc, char *argv[])
{
	struct server server = { .launcher_cmd = LAUNCHER_ID, .shell_cmd = "s22-shell",
		.statusbar_cmd = "s22-statusbar",
		.default_idle_ms = 30000, .settings_mtime = { -1, 0 },
		.ctl_fd = -1 };
	int c;

	while ((c = getopt(argc, argv, "s:S:t:h")) != -1) {
		switch (c) {
		case 's':
			server.launcher_cmd = optarg;
			break;
		case 'S':
			server.shell_cmd = optarg;
			break;
		case 't':
			server.default_idle_ms = atoi(optarg) * 1000;
			break;
		default:
			fprintf(stderr, "usage: %s [-s LAUNCHER_CMD] [-S SHELL_CMD] [-t IDLE_SECONDS]\n",
				argv[0]);
			return 2;
		}
	}
	wlr_log_init(getenv("S22_WM_DEBUG") ? WLR_DEBUG : WLR_INFO, NULL);

	/* Children are not waited for; they get the default back in spawn() */
	struct sigaction sa = { .sa_handler = SIG_DFL, .sa_flags = SA_NOCLDWAIT };
	sigaction(SIGCHLD, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);	/* a client gone mid-write is no reason to die */

	server.display = wl_display_create();
	struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
	server.backend = wlr_backend_autocreate(loop, NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.display);
	server.renderer_lost.notify = renderer_lost;
	wl_signal_add(&server.renderer->events.lost, &server.renderer_lost);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	struct wlr_compositor *compositor =
		wlr_compositor_create(server.display, 5, server.renderer);
	server.compositor = compositor;
	server.new_surface.notify = new_surface;
	wl_signal_add(&compositor->events.new_surface, &server.new_surface);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);
	wlr_screencopy_manager_v1_create(server.display);	/* screenshots (grim) */

	server.output_layout = wlr_output_layout_create(server.display);
	wlr_xdg_output_manager_v1_create(server.display, server.output_layout);
	wlr_viewporter_create(server.display);	/* video players scale through it (mpv) */
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene,
		server.output_layout);
	/* Bottom to top: background, bottom, windows, top, overlay */
	server.layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] =
		wlr_scene_tree_create(&server.scene->tree);
	server.layers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] =
		wlr_scene_tree_create(&server.scene->tree);
	server.apps = wlr_scene_tree_create(&server.scene->tree);
	server.layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP] =
		wlr_scene_tree_create(&server.scene->tree);
	server.layers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] =
		wlr_scene_tree_create(&server.scene->tree);
	server.lock_tree = wlr_scene_tree_create(&server.scene->tree);
	wlr_scene_node_set_enabled(&server.lock_tree->node, false);
	server.over_lock = wlr_scene_tree_create(&server.scene->tree);
	server.lock_manager = wlr_session_lock_manager_v1_create(server.display);
	server.new_lock.notify = new_lock;
	wl_signal_add(&server.lock_manager->events.new_lock, &server.new_lock);
	wl_list_init(&server.layer_surfaces);
	server.layer_shell = wlr_layer_shell_v1_create(server.display, 4);
	server.new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server.layer_shell->events.new_surface,
		&server.new_layer_surface);

	wl_list_init(&server.toplevels);
	server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel,
		&server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	server.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	wl_list_init(&server.touches);
	memcpy(server.calib, (float[6]){ 1, 0, 0, 0, 1, 0 }, sizeof(server.calib));
	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.display, "seat0");
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
		&server.request_set_selection);
	server.clip_fd = -1;
	server.set_selection.notify = seat_set_selection;
	wl_signal_add(&server.seat->events.set_selection, &server.set_selection);
	/* wl-copy, wl-paste and clipboard tools */
	wlr_data_control_manager_v1_create(server.display);
	wlr_ext_data_control_manager_v1_create(server.display, 1);

	server.idle_timer = wl_event_loop_add_timer(loop, idle_expired, &server);
	server.wake_timer = wl_event_loop_add_timer(loop, wake_done, &server);
	server.call_watch = -1;
	server.side_timer = wl_event_loop_add_timer(loop, side_held, &server);
	server.power_timer = wl_event_loop_add_timer(loop, power_held, &server);
	server.lock_timer = wl_event_loop_add_timer(loop, locker_due, &server);
	server.locker_timer = wl_event_loop_add_timer(loop, locker_due, &server);
	server.call_retry = wl_event_loop_add_timer(loop, call_watch_open, &server);
	server.idle_inhibit = wlr_idle_inhibit_v1_create(server.display);
	server.new_inhibitor.notify = new_inhibitor;
	wl_signal_add(&server.idle_inhibit->events.new_inhibitor, &server.new_inhibitor);
	wl_event_loop_add_signal(loop, SIGTERM, on_signal, server.display);
	wl_event_loop_add_signal(loop, SIGINT, on_signal, server.display);

	/* On failure just exit: tearing down with our listeners still on the
	 * backend trips wlroots' assertions, and the exit code is what the
	 * supervisor needs */
	const char *socket = wl_display_add_socket_auto(server.display);
	if (socket == NULL) {
		wlr_log(WLR_ERROR, "no Wayland socket in %s",
			getenv("XDG_RUNTIME_DIR") ?: "(XDG_RUNTIME_DIR unset)");
		return 1;
	}
	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "the backend did not start");
		return 1;
	}
	setenv("WAYLAND_DISPLAY", socket, true);
	if (!ctl_open(&server))
		wlr_log(WLR_ERROR, "no control socket");
	idle_reset(&server);
	call_watch_open(&server);
	if (lock_enabled())		/* a PIN lock starts locked */
		start_locker(&server);
	go_home(&server);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	server.shell_started = now.tv_sec;
	spawn(server.shell_cmd);
	spawn(server.statusbar_cmd);

	wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(server.display);

	server.quitting = true;
	if (server.ctl_fd >= 0)
		unlink(server.ctl_path);
	wl_display_destroy_clients(server.display);
	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_layer_surface.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.new_output.link);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wl_list_remove(&server.renderer_lost.link);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.display);
	xkb_context_unref(server.xkb);
	return 0;
}
