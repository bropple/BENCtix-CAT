// SPDX-License-Identifier: MIT
/*
 * s22-browser: the web browser (D050). WebKitGTK 6.0 (the engine GNOME Web
 * uses) in a GTK4 window, made for a 480x640 screen, a keypad and a finger.
 *
 * Touch is WebKit's own: drag to scroll with inertia, pinch to zoom, tap
 * to follow a link. Pages get a phone's user agent, so sites send their
 * mobile layouts, which fit the 480 px width as it is.
 *
 * Keys, while browsing (the keypad comes raw: app_id "s22-..."):
 *   arrows   move between links and fields (WebKit's spatial navigation),
 *            or scroll where there are none
 *   OK       follow the link chosen (or press the button)
 *   2 / 8    a screen up / down        0   the top
 *   4 / 6    back / forward            5   reload
 *   1 / 3    smaller / larger          C   stop loading
 *   7        bookmarks                 9   bookmark this page
 *   4, 6 held  the tab before / after  5 held  a new tab
 *   C held   close this tab (the last one closes the browser)
 *   #        type an address or a search
 *   *        the menu
 *   Back     back a page, or close the browser at the first
 * In a text field (the address, or one on a page) the keypad types as
 * s22-t9d does on the console: the browser asks for "text" mode (writing
 * /run/s22-t9/gui) while one has the focus, and Back (Esc) leaves it.
 *
 * Tabs (D057): up to MAX_TABS, numbered at the bar's right while there are
 * two or more (a tap switches, + opens one), all in one web process
 * A link that wants a new window gets a new tab while there is room. The
 * tabs come back next time; those not in front load when first shown.
 * Each site gets a web process of its own (WebKit's), so when the phone's
 * available memory drops below LOW_MEM_KB, the tab longest out of view has
 * its process ended and reloads when it comes back to the front.
 *
 * Ad blocking: EasyList and uBlock Origin's filters, fetched and converted
 * to WebKit's content blocker rules by s22-browser-filters once a week,
 * compiled into a WebKitUserContentFilterStore; they block requests and
 * hide ad elements. YouTube's ads come inside its video data, so a page
 * script for youtube.com (youtube.js) takes them out (D054).
 *
 * Kept in ~/.local/share/s22-browser: cookies and site data, the compiled
 * filters, bookmarks ("TITLE<TAB>URL" lines), prefs (zoom=, adblock=,
 * desktop=) and the page open when it closed, which opens next time.
 * Downloads go to ~/Downloads, with a notification when done. Pages may
 * not use the camera, microphone, location or notifications.
 *
 * Links: "open URL" on $XDG_RUNTIME_DIR/s22-app-s22-browser.sock (s22ui's
 * ui_open_uri, for http: and https: links), or a URL as the argument.
 *
 *   s22-browser [URL]
 */
#include <errno.h>
#include <glib/gstdio.h>
#include <gio/gunixsocketaddress.h>
#include <gtk/gtk.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <glib-unix.h>
#include <webkit/webkit.h>

#define APP_ID		"s22-browser"
#define HOME_URL	"https://lite.duckduckgo.com/lite/"
#define SEARCH_URL	"https://lite.duckduckgo.com/lite/?q="
#define T9_GUI		"/run/s22-t9/gui"
#define SETTINGS	"/var/lib/s22/settings"
#define THEMES		"/usr/share/s22/themes"
#define FILTERS_BIN	"/usr/lib/s22-browser/s22-browser-filters"
#define FILTER_ID	"adblock"
#define YOUTUBE_JS	"/usr/lib/s22-browser/youtube.js"
#define FILTER_AGE_S	(7 * 24 * 3600)
#define MOBILE_UA	"Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) " \
			"AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 " \
			"Mobile/15E148 Safari/604.1"

/* The keypad's codes, as evdev has them (GDK's keycode is 8 more) */
#ifndef KEY_NUMERIC_STAR
#define KEY_NUMERIC_STAR	0x20a
#define KEY_NUMERIC_POUND	0x20b
#endif

static GMainLoop *loop;
static GtkWidget *win, *bar, *host_label, *title_label, *entry, *progress;
static GtkWidget *panel, *panel_title, *panel_list, *panel_scroll, *toast;
static WebKitWebView *view;		/* the tab in front */
#define MAX_TABS	5
static WebKitWebView *tabs[MAX_TABS];
static char *tab_pending[MAX_TABS];	/* restored, not loaded until shown */
static int ntabs, cur_tab;
static gint64 tab_seen[MAX_TABS];	/* when each was last in front */
#define LOW_MEM_KB	(400 * 1024)	/* below this, background tabs are put away */
static GtkWidget *stack, *tab_box;
static WebKitSettings *wk_settings;
static WebKitWebsitePolicies *wk_policies;
static WebKitNetworkSession *session;
static WebKitUserContentManager *ucm;
static WebKitUserContentFilterStore *store;
static char *data_dir, *last_file, *prefs_file, *marks_file;

static bool web_editing;	/* a text field on the page has the focus */
static bool typing;		/* the address entry is out */
static int zoom_pct = 100;
static bool adblock = true, desktop;
static guint toast_id;

enum panel_kind { P_NONE, P_MENU, P_MARKS };
static enum panel_kind panel_kind;
static int panel_sel, panel_count;
static int confirm_delete = -1;	/* bookmark C was pressed on once */

struct mark { char *title, *url; };
static GPtrArray *marks;

/* ------------------------------------------------------------ helpers */

static void toast_show(const char *text);

static char *read_text(const char *path)
{
	char *s = NULL;

	return g_file_get_contents(path, &s, NULL, NULL) ? s : NULL;
}

/* A key's value in a "key=value" file, or NULL (free it) */
static char *kv_get(const char *path, const char *key)
{
	char *text = read_text(path), *val = NULL;
	size_t n = strlen(key);

	for (char *l = text ? strtok(text, "\n") : NULL; l; l = strtok(NULL, "\n"))
		if (!strncmp(l, key, n) && l[n] == '=') {
			g_free(val);
			val = g_strdup(l + n + 1);
		}
	g_free(text);
	return val;
}

static void prefs_save(void)
{
	char *s = g_strdup_printf("zoom=%d\nadblock=%s\ndesktop=%s\n", zoom_pct,
				  adblock ? "on" : "off", desktop ? "on" : "off");
	g_file_set_contents(prefs_file, s, -1, NULL);
	g_free(s);
}

static void prefs_load(void)
{
	char *v;

	if ((v = kv_get(prefs_file, "zoom")))
		zoom_pct = CLAMP(atoi(v), 50, 300);
	g_free(v);
	if ((v = kv_get(prefs_file, "adblock")))
		adblock = strcmp(v, "off") != 0;
	g_free(v);
	if ((v = kv_get(prefs_file, "desktop")))
		desktop = strcmp(v, "on") == 0;
	g_free(v);
}

/* The keypad: text while a field has the focus, else raw keys */
static void keypad_update(void)
{
	if (!gtk_window_is_active(GTK_WINDOW(win)))
		return;
	FILE *f = fopen(T9_GUI, "we");
	if (f) {
		/* The address bar types a web address, a page's field prose */
		fputs(typing ? "text url\n" : web_editing ? "text prose\n" : "raw\n", f);
		fclose(f);
	}
}

static void js(const char *code)
{
	webkit_web_view_evaluate_javascript(view, code, -1, NULL, NULL, NULL, NULL, NULL);
}

static char *host_of(const char *uri)
{
	GUri *u = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
	char *h = u && g_uri_get_host(u) ? g_strdup(g_uri_get_host(u)) : NULL;

	if (u)
		g_uri_unref(u);
	if (h && g_str_has_prefix(h, "www."))
		memmove(h, h + 4, strlen(h + 4) + 1);
	return h;
}

/* What was typed: an address, or words to search for */
static char *typed_to_uri(const char *t)
{
	while (*t == ' ')
		t++;
	if (!*t)
		return NULL;
	if (strstr(t, "://") || g_str_has_prefix(t, "about:"))
		return g_strdup(t);
	if (!strchr(t, ' ') && strchr(t, '.') && t[strlen(t) - 1] != '.')
		return g_strconcat("https://", t, NULL);
	char *q = g_uri_escape_string(t, NULL, TRUE);
	char *u = g_strconcat(SEARCH_URL, q, NULL);
	g_free(q);
	return u;
}

static void load(const char *uri)
{
	if (uri && *uri)
		webkit_web_view_load_uri(view, uri);
}

/* ---------------------------------------------------------- the bar */

static void bar_update(void)
{
	const char *uri = webkit_web_view_get_uri(view);
	const char *title = webkit_web_view_get_title(view);
	char *host = host_of(uri);

	gtk_label_set_text(GTK_LABEL(host_label), host ? host : "");
	gtk_label_set_text(GTK_LABEL(title_label), title && *title ? title : "");
	g_free(host);
}

static void typing_start(const char *text)
{
	typing = true;
	gtk_widget_set_visible(tab_box, false);
	gtk_widget_set_visible(host_label, false);
	gtk_widget_set_visible(title_label, false);
	gtk_widget_set_visible(entry, true);
	gtk_editable_set_text(GTK_EDITABLE(entry), text ? text : "");
	gtk_widget_grab_focus(entry);
	gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
	keypad_update();
}

static void typing_end(void)
{
	if (!typing)
		return;
	typing = false;
	gtk_widget_set_visible(tab_box, ntabs > 1);
	gtk_widget_set_visible(entry, false);
	gtk_widget_set_visible(host_label, true);
	gtk_widget_set_visible(title_label, true);
	gtk_widget_grab_focus(GTK_WIDGET(view));
	keypad_update();
}

static void on_entry_activate(GtkEntry *e, gpointer d)
{
	char *uri = typed_to_uri(gtk_editable_get_text(GTK_EDITABLE(e)));

	typing_end();
	load(uri);
	g_free(uri);
}

/* A tap on the page while typing: the address is given up */
static void on_entry_leave(GtkEventControllerFocus *c, gpointer d)
{
	if (typing)
		typing_end();
}

/* -------------------------------------------------------- bookmarks */

static void marks_load(void)
{
	char *text = read_text(marks_file);

	g_ptr_array_set_size(marks, 0);
	for (char *l = text ? strtok(text, "\n") : NULL; l; l = strtok(NULL, "\n")) {
		char *tab = strchr(l, '\t');
		if (!tab)
			continue;
		struct mark *m = g_new(struct mark, 1);
		m->title = g_strndup(l, tab - l);
		m->url = g_strdup(tab + 1);
		g_ptr_array_add(marks, m);
	}
	g_free(text);
}

static void marks_save(void)
{
	GString *s = g_string_new(NULL);

	for (guint i = 0; i < marks->len; i++) {
		struct mark *m = marks->pdata[i];
		g_string_append_printf(s, "%s\t%s\n", m->title, m->url);
	}
	g_file_set_contents(marks_file, s->str, -1, NULL);
	g_string_free(s, TRUE);
}

static void mark_free(gpointer p)
{
	struct mark *m = p;

	g_free(m->title);
	g_free(m->url);
	g_free(m);
}

static void mark_add(void)
{
	const char *uri = webkit_web_view_get_uri(view);
	const char *title = webkit_web_view_get_title(view);

	if (!uri || !*uri)
		return;
	for (guint i = 0; i < marks->len; i++)
		if (!strcmp(((struct mark *)marks->pdata[i])->url, uri)) {
			toast_show("Already bookmarked");
			return;
		}
	struct mark *m = g_new(struct mark, 1);
	char *t = g_strdup(title && *title ? title : uri);
	for (char *p = t; *p; p++)
		if (*p == '\t' || *p == '\n')
			*p = ' ';
	m->title = t;
	m->url = g_strdup(uri);
	g_ptr_array_add(marks, m);
	marks_save();
	toast_show("Bookmarked");
}

/* ------------------------------------------------------------ panel */

enum { M_ADDRESS, M_MARKS, M_MARK, M_RELOAD, M_ZOOM, M_ADBLOCK, M_DESKTOP,
       M_HOME, M_CLEAR, M_CLOSE, M_NEW_TAB, M_CLOSE_TAB, M_TAB0 };
static int menu_act[M_TAB0 + MAX_TABS];	/* each row's action */

static void panel_row(const char *text, const char *hint)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_set_hexpand(l, true);
	gtk_box_append(GTK_BOX(box), l);
	if (hint) {
		GtkWidget *h = gtk_label_new(hint);
		gtk_widget_add_css_class(h, "hint");
		gtk_box_append(GTK_BOX(box), h);
	}
	gtk_list_box_append(GTK_LIST_BOX(panel_list), box);
	panel_count++;
}

static void panel_select(void)
{
	GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(panel_list), panel_sel);

	if (r) {
		gtk_list_box_select_row(GTK_LIST_BOX(panel_list), r);
		gtk_widget_grab_focus(GTK_WIDGET(r));
	}
}

static void panel_fill(void)
{
	char z[48];

	gtk_list_box_remove_all(GTK_LIST_BOX(panel_list));
	panel_count = 0;
	if (panel_kind == P_MENU) {
		gtk_label_set_text(GTK_LABEL(panel_title), "Browser");
		snprintf(z, sizeof(z), "Text size  %d%%", zoom_pct);
		/* The open tabs first, when there are several */
		for (int i = 0; ntabs > 1 && i < ntabs; i++) {
			const char *t = webkit_web_view_get_title(tabs[i]);
			char *h = host_of(tab_pending[i] ? tab_pending[i] :
					  webkit_web_view_get_uri(tabs[i]));
			char line[160];
			snprintf(line, sizeof(line), "%s%d  %s", i == cur_tab ? "▸ " : "", i + 1,
				 t && *t ? t : h ? h : "New tab");
			menu_act[panel_count] = M_TAB0 + i;
			panel_row(line, h);
			g_free(h);
		}
#define MROW(act, text, hint) (menu_act[panel_count] = (act), panel_row(text, hint))
		MROW(M_ADDRESS, "Address or search", "#");
		if (ntabs < MAX_TABS)
			MROW(M_NEW_TAB, "New tab", "5 held");
		if (ntabs > 1)
			MROW(M_CLOSE_TAB, "Close this tab", "C held");
		MROW(M_MARKS, "Bookmarks", "7");
		MROW(M_MARK, "Bookmark this page", "9");
		MROW(M_RELOAD, "Reload", "5");
		MROW(M_ZOOM, z, "◂ ▸");
		MROW(M_ADBLOCK, adblock ? "Ad blocking: on" : "Ad blocking: off", NULL);
		MROW(M_DESKTOP, desktop ? "Desktop site: on" : "Desktop site: off", NULL);
		MROW(M_HOME, "Home page", NULL);
		MROW(M_CLEAR, "Clear cookies and site data", NULL);
		MROW(M_CLOSE, "Close the browser", NULL);
#undef MROW
	} else {
		gtk_label_set_text(GTK_LABEL(panel_title),
				   marks->len ? "Bookmarks" : "No bookmarks yet (9 adds one)");
		for (guint i = 0; i < marks->len; i++) {
			struct mark *m = marks->pdata[i];
			char *host = host_of(m->url);
			panel_row(m->title, (int)i == confirm_delete ? "C again: delete" : host);
			g_free(host);
		}
	}
	panel_sel = CLAMP(panel_sel, 0, MAX(panel_count - 1, 0));
	panel_select();
}

static void panel_open(enum panel_kind k)
{
	panel_kind = k;
	panel_sel = 0;
	confirm_delete = -1;
	gtk_widget_set_visible(panel, true);
	panel_fill();
}

static void panel_close(void)
{
	panel_kind = P_NONE;
	gtk_widget_set_visible(panel, false);
	gtk_widget_grab_focus(GTK_WIDGET(view));
}

static void zoom_set(int pct)
{
	zoom_pct = CLAMP(pct, 50, 300);
	for (int i = 0; i < ntabs; i++)
		webkit_web_view_set_zoom_level(tabs[i], zoom_pct / 100.0);
	prefs_save();
}

static void filters_apply(void);

static void on_cleared(GObject *o, GAsyncResult *res, gpointer d)
{
	webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(o), res, NULL);
	toast_show("Cookies and site data cleared");
}

static void ua_apply(void)
{
	webkit_settings_set_user_agent(wk_settings, desktop ? NULL : MOBILE_UA);
}

static void tab_new(const char *uri, bool show, bool lazy);
static void tab_show(int i);
static void tab_close(int i);
static void on_progress(GObject *o, GParamSpec *p, gpointer d);

static void menu_choose(void)
{
	int act = menu_act[panel_sel];

	if (act >= M_TAB0) {
		panel_close();
		tab_show(act - M_TAB0);
		return;
	}
	switch (act) {
	case M_NEW_TAB:
		panel_close();
		tab_new(HOME_URL, true, false);
		typing_start("");
		return;
	case M_CLOSE_TAB:
		panel_close();
		tab_close(cur_tab);
		return;
	case M_ADDRESS:
		panel_close();
		typing_start(webkit_web_view_get_uri(view));
		return;
	case M_MARKS:
		panel_open(P_MARKS);
		return;
	case M_MARK:
		panel_close();
		mark_add();
		return;
	case M_RELOAD:
		panel_close();
		webkit_web_view_reload(view);
		return;
	case M_ZOOM:
		return;			/* Left/Right change it */
	case M_ADBLOCK:
		adblock = !adblock;
		prefs_save();
		filters_apply();
		webkit_web_view_reload(view);
		break;
	case M_DESKTOP:
		desktop = !desktop;
		prefs_save();
		ua_apply();
		webkit_web_view_reload(view);
		break;
	case M_HOME:
		panel_close();
		load(HOME_URL);
		return;
	case M_CLEAR:
		webkit_website_data_manager_clear(
			webkit_network_session_get_website_data_manager(session),
			WEBKIT_WEBSITE_DATA_ALL, 0, NULL, on_cleared, NULL);
		panel_close();
		return;
	case M_CLOSE:
		g_main_loop_quit(loop);
		return;
	}
	panel_fill();
}

/* Keys while the panel is out; true if used */
static bool panel_key(unsigned code)
{
	switch (code) {
	case KEY_BACK:
	case KEY_ESC:
		if (panel_kind == P_MARKS)
			panel_open(P_MENU);
		else
			panel_close();
		return true;
	case KEY_NUMERIC_STAR:
		panel_close();
		return true;
	case KEY_UP:
	case KEY_2:
		if (panel_sel > 0)
			panel_sel--;
		confirm_delete = -1;
		panel_fill();
		return true;
	case KEY_DOWN:
	case KEY_8:
		if (panel_sel < panel_count - 1)
			panel_sel++;
		confirm_delete = -1;
		panel_fill();
		return true;
	case KEY_LEFT:
	case KEY_RIGHT:
		if (panel_kind == P_MENU && menu_act[panel_sel] == M_ZOOM) {
			zoom_set(zoom_pct + (code == KEY_RIGHT ? 10 : -10));
			panel_fill();
		}
		return true;
	case KEY_OK:
	case KEY_ENTER:
	case KEY_5:
		if (panel_kind == P_MENU) {
			menu_choose();
		} else if (panel_sel < (int)marks->len) {
			char *u = g_strdup(((struct mark *)marks->pdata[panel_sel])->url);
			panel_close();
			load(u);
			g_free(u);
		}
		return true;
	case KEY_BACKSPACE:
		if (panel_kind == P_MARKS && panel_sel < (int)marks->len) {
			if (confirm_delete == panel_sel) {
				g_ptr_array_remove_index(marks, panel_sel);
				marks_save();
				confirm_delete = -1;
			} else {
				confirm_delete = panel_sel;
			}
			panel_fill();
		}
		return true;
	}
	return true;			/* nothing else reaches the page meanwhile */
}

/* ------------------------------------------------------------- keys */

/* OK on the page: follow the link (or press the button) that has the
 * focus; a text field takes the focus, which asks for text mode */
static const char OK_JS[] =
	"(function(){var e=document.activeElement;"
	"if(!e||e===document.body||e===document.documentElement)return;"
	"var t=e.tagName,y=(e.type||'').toLowerCase();"
	"if(e.isContentEditable||t==='TEXTAREA'||(t==='INPUT'&&['text','search',"
	"'email','url','tel','password','number',''].indexOf(y)>=0)){e.focus();return;}"
	"e.click();})()";

/*
 * 4, 6, 5 and C: a tap is back, forward, reload, stop; held HOLD_MS, the
 * tab before or after, a new tab, closing this one. They act on release,
 * or when the hold is reached.
 */
#define HOLD_MS 500
static unsigned hold_code;		/* the key down, 0 for none */
static bool hold_done;			/* its hold already acted */
static guint hold_timer;

static void tab_step(int d)
{
	char t[32];

	if (ntabs < 2) {
		toast_show("Only one tab (5 held opens another)");
		return;
	}
	tab_show((cur_tab + d + ntabs) % ntabs);
	snprintf(t, sizeof(t), "Tab %d of %d", cur_tab + 1, ntabs);
	toast_show(t);
}

static gboolean hold_fire(gpointer d)
{
	hold_timer = 0;
	hold_done = true;
	switch (hold_code) {
	case KEY_4: tab_step(-1); break;
	case KEY_6: tab_step(1); break;
	case KEY_5:
		if (ntabs >= MAX_TABS) {
			toast_show("5 tabs at most: C held closes one");
		} else {
			tab_new(HOME_URL, true, false);
			typing_start("");
		}
		break;
	case KEY_BACKSPACE: tab_close(cur_tab); break;
	}
	return G_SOURCE_REMOVE;
}

static void tap_act(unsigned code)
{
	switch (code) {
	case KEY_4: webkit_web_view_go_back(view); break;
	case KEY_6: webkit_web_view_go_forward(view); break;
	case KEY_5: webkit_web_view_reload(view); break;
	case KEY_BACKSPACE:
		/* Stopped: no bar, even while WebKit still winds the load down */
		g_object_set_data(G_OBJECT(view), "stopped", GINT_TO_POINTER(1));
		webkit_web_view_stop_loading(view);
		on_progress(NULL, NULL, NULL);
		break;
	}
}

static gboolean on_key_up(GtkEventControllerKey *c, guint keyval, guint keycode,
			  GdkModifierType state, gpointer d)
{
	unsigned code = keycode - 8;

	if (!hold_code || code != hold_code)
		return FALSE;
	if (hold_timer) {
		g_source_remove(hold_timer);
		hold_timer = 0;
	}
	if (!hold_done)
		tap_act(code);
	hold_code = 0;
	return TRUE;
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint keycode,
		       GdkModifierType state, gpointer d)
{
	unsigned code = keycode - 8;

	if (hold_code && code == hold_code)
		return TRUE;		/* the key's repeats, while held */

	/* Typing: everything is the field's, but Back (Esc) leaves it */
	if (typing) {
		if (keyval == GDK_KEY_Escape) {
			typing_end();
			return TRUE;
		}
		return FALSE;
	}
	if (panel_kind != P_NONE)
		return panel_key(code);
	if (web_editing) {
		if (keyval == GDK_KEY_Escape) {
			js("document.activeElement&&document.activeElement.blur()");
			web_editing = false;
			keypad_update();
			return TRUE;
		}
		return FALSE;
	}
	switch (code) {
	case KEY_BACK:
	case KEY_ESC:
		if (webkit_web_view_can_go_back(view))
			webkit_web_view_go_back(view);
		else
			g_main_loop_quit(loop);
		return TRUE;
	case KEY_OK:
	case KEY_ENTER:
		js(OK_JS);
		return TRUE;
	case KEY_2:
	case KEY_8:
		js(code == KEY_2 ?
		   "window.scrollBy({top:-0.85*innerHeight,behavior:'smooth'})" :
		   "window.scrollBy({top:0.85*innerHeight,behavior:'smooth'})");
		return TRUE;
	case KEY_0:
		js("window.scrollTo({top:0,behavior:'smooth'})");
		return TRUE;
	case KEY_4:
	case KEY_6:
	case KEY_5:
	case KEY_BACKSPACE:
		hold_code = code;
		hold_done = false;
		if (hold_timer)
			g_source_remove(hold_timer);
		hold_timer = g_timeout_add(HOLD_MS, hold_fire, NULL);
		return TRUE;
	case KEY_1:
	case KEY_3:
		zoom_set(zoom_pct + (code == KEY_3 ? 10 : -10));
		{
			char t[32];
			snprintf(t, sizeof(t), "Text size %d%%", zoom_pct);
			toast_show(t);
		}
		return TRUE;
	case KEY_7:
		panel_open(P_MARKS);
		return TRUE;
	case KEY_9:
		mark_add();
		return TRUE;
	case KEY_NUMERIC_POUND:
		typing_start(webkit_web_view_get_uri(view));
		return TRUE;
	case KEY_NUMERIC_STAR:
		panel_open(P_MENU);
		return TRUE;
	case KEY_PHONE:			/* the call keys are for calls */
		return TRUE;
	}
	return FALSE;			/* arrows and the rest: the page's */
}

/* The page says whether a text field has the focus */
static void on_script_message(WebKitUserContentManager *m, JSCValue *v, gpointer d)
{
	char *s = jsc_value_to_string(v);
	bool editing = s && !strcmp(s, "edit");

	g_free(s);
	if (editing != web_editing) {
		web_editing = editing;
		keypad_update();
	}
}

static const char FOCUS_JS[] =
	"(function(){function ed(e){if(!e)return false;if(e.isContentEditable)return true;"
	"var t=e.tagName;if(t==='TEXTAREA')return true;if(t==='INPUT'){var y=(e.type||'text')"
	".toLowerCase();return['text','search','email','url','tel','password','number','']"
	".indexOf(y)>=0;}return false;}"
	"function post(v){try{window.webkit.messageHandlers.s22.postMessage(v?'edit':'browse')}"
	"catch(x){}}"
	"document.addEventListener('focusin',function(e){post(ed(e.target))},true);"
	"document.addEventListener('focusout',function(){setTimeout(function(){"
	"post(ed(document.activeElement))},0)},true);"
	"window.addEventListener('pagehide',function(){post(false)});})()";

/* ------------------------------------------------------------ toast */

static gboolean toast_hide(gpointer d)
{
	gtk_widget_set_visible(toast, false);
	toast_id = 0;
	return G_SOURCE_REMOVE;
}

static void toast_show(const char *text)
{
	gtk_label_set_text(GTK_LABEL(toast), text);
	gtk_widget_set_visible(toast, true);
	if (toast_id)
		g_source_remove(toast_id);
	toast_id = g_timeout_add(2000, toast_hide, NULL);
}

/* ----------------------------------------------------------- filters */

static WebKitUserContentFilter *filter;

/* YouTube's ads come with its videos: a page script takes them out (D054) */
static WebKitUserScript *youtube;
static bool youtube_added;

static void youtube_load(void)
{
	static const char *const pages[] = { "*://*.youtube.com/*", "*://youtube.com/*", NULL };
	char *js = read_text(YOUTUBE_JS);

	if (js) {
		youtube = webkit_user_script_new(js, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
			WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, pages, NULL);
		g_free(js);
	}
}

static void filters_apply(void)
{
	webkit_user_content_manager_remove_all_filters(ucm);
	if (adblock && filter)
		webkit_user_content_manager_add_filter(ucm, filter);
	if (youtube && adblock != youtube_added) {
		if (adblock)
			webkit_user_content_manager_add_script(ucm, youtube);
		else
			webkit_user_content_manager_remove_script(ucm, youtube);
		youtube_added = adblock;
	}
}

static void on_filter_saved(GObject *o, GAsyncResult *res, gpointer d)
{
	GError *err = NULL;
	WebKitUserContentFilter *f = webkit_user_content_filter_store_save_finish(
		WEBKIT_USER_CONTENT_FILTER_STORE(o), res, &err);

	if (!f) {
		g_printerr("s22-browser: the ad list did not compile: %s\n", err->message);
		g_error_free(err);
		return;
	}
	if (filter)
		webkit_user_content_filter_unref(filter);
	filter = f;
	filters_apply();
	toast_show("Ad blocking list updated");
}

static void on_filters_fetched(GObject *o, GAsyncResult *res, gpointer d)
{
	GSubprocess *p = G_SUBPROCESS(o);
	char *json_path = g_build_filename(data_dir, "adblock.json", NULL);
	GError *err = NULL;
	GMappedFile *mf;

	if (!g_subprocess_wait_check_finish(p, res, &err)) {
		g_printerr("s22-browser: s22-browser-filters: %s\n", err->message);
		g_error_free(err);
	} else if ((mf = g_mapped_file_new(json_path, FALSE, NULL))) {
		GBytes *b = g_mapped_file_get_bytes(mf);
		webkit_user_content_filter_store_save(store, FILTER_ID, b, NULL,
						      on_filter_saved, NULL);
		g_bytes_unref(b);
		g_mapped_file_unref(mf);
	}
	g_free(json_path);
	g_object_unref(p);
}

/* The list is a week old (or missing): fetch and convert it again */
static void filters_refresh(void)
{
	char *json_path = g_build_filename(data_dir, "adblock.json", NULL);
	GStatBuf st;
	bool fresh = g_stat(json_path, &st) == 0 &&
		     time(NULL) - st.st_mtime < FILTER_AGE_S;

	g_free(json_path);
	if (fresh && filter)
		return;
	GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE, NULL,
					  FILTERS_BIN, data_dir, NULL);
	if (p)
		g_subprocess_wait_check_async(p, NULL, on_filters_fetched, NULL);
}

static void on_filter_loaded(GObject *o, GAsyncResult *res, gpointer d)
{
	filter = webkit_user_content_filter_store_load_finish(
		WEBKIT_USER_CONTENT_FILTER_STORE(o), res, NULL);
	filters_apply();
	filters_refresh();
}

/* -------------------------------------------------------- web view */

/* The open tabs, for next time: the one in front first ("@N"), then a
 * line a tab (an unloaded one keeps the address it came back with) */
static void session_save(void)
{
	GString *s = g_string_new(NULL);

	g_string_append_printf(s, "@%d\n", cur_tab);
	for (int i = 0; i < ntabs; i++) {
		const char *u = tab_pending[i] ? tab_pending[i] : webkit_web_view_get_uri(tabs[i]);
		g_string_append_printf(s, "%s\n", u && g_str_has_prefix(u, "http") ? u : HOME_URL);
	}
	g_file_set_contents(last_file, s->str, -1, NULL);
	g_string_free(s, TRUE);
}

static void on_load_changed(WebKitWebView *v, WebKitLoadEvent ev, gpointer d)
{
	if (ev == WEBKIT_LOAD_STARTED)
		g_object_set_data(G_OBJECT(v), "stopped", NULL);
	if (ev == WEBKIT_LOAD_STARTED && v == view) {
		web_editing = false;
		keypad_update();
	}
	if (ev == WEBKIT_LOAD_FINISHED)
		session_save();
	if (v == view)
		bar_update();
}

static void on_progress(GObject *o, GParamSpec *p, gpointer d)
{
	if (o && o != G_OBJECT(view))
		return;
	double f = webkit_web_view_get_estimated_load_progress(view);
	bool loading = webkit_web_view_is_loading(view) &&
		       !g_object_get_data(G_OBJECT(view), "stopped");

	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), f);
	gtk_widget_set_opacity(progress, loading && f < 1.0 ? 1.0 : 0.0);
}

static void on_title(GObject *o, GParamSpec *p, gpointer d)
{
	if (o == G_OBJECT(view))
		bar_update();
}

/* Links that want a new window: a new tab while there is room, else here */
static gboolean on_decide_policy(WebKitWebView *v, WebKitPolicyDecision *dec,
				 WebKitPolicyDecisionType type, gpointer d)
{
	if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
		WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(
			WEBKIT_NAVIGATION_POLICY_DECISION(dec));
		const char *u = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
		if (ntabs < MAX_TABS)
			tab_new(u, true, false);
		else
			webkit_web_view_load_uri(v, u);
		webkit_policy_decision_ignore(dec);
		return TRUE;
	}
	if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE &&
	    !webkit_response_policy_decision_is_mime_type_supported(
		    WEBKIT_RESPONSE_POLICY_DECISION(dec))) {
		webkit_policy_decision_download(dec);
		return TRUE;
	}
	return FALSE;
}

static gboolean on_permission(WebKitWebView *v, WebKitPermissionRequest *r, gpointer d)
{
	webkit_permission_request_deny(r);
	return TRUE;
}

/* --------------------------------------------------------- downloads */

static gboolean on_decide_destination(WebKitDownload *dl, char *name, gpointer d)
{
	char *dir = g_build_filename(g_get_home_dir(), "Downloads", NULL);
	char *base = g_path_get_basename(name && *name ? name : "download");
	char *path = g_build_filename(dir, base, NULL);

	g_mkdir_with_parents(dir, 0755);
	for (int i = 1; g_file_test(path, G_FILE_TEST_EXISTS) && i < 100; i++) {
		g_free(path);
		char *b = g_strdup_printf("%d-%s", i, base);
		path = g_build_filename(dir, b, NULL);
		g_free(b);
	}
	webkit_download_set_destination(dl, path);
	toast_show("Downloading…");
	g_free(path);
	g_free(base);
	g_free(dir);
	return TRUE;
}

static void on_download_finished(WebKitDownload *dl, gpointer d)
{
	const char *path = webkit_download_get_destination(dl);
	char *base = path ? g_path_get_basename(path) : g_strdup("a file");
	char *text = g_strdup_printf("Downloaded %s", base);
	char *link = path ? g_strconcat("file:", path, NULL) : NULL;
	const char *argv[] = { "s22-notify", "-s", "browser", "-o", link, text, NULL };

	if (!link) {
		argv[3] = text;
		argv[4] = NULL;
	}
	g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
	g_free(link);
	g_free(text);
	g_free(base);
}

static void on_download_failed(WebKitDownload *dl, GError *err, gpointer d)
{
	toast_show("The download failed");
}

static void on_download_started(WebKitNetworkSession *s, WebKitDownload *dl, gpointer d)
{
	g_signal_connect(dl, "decide-destination", G_CALLBACK(on_decide_destination), NULL);
	g_signal_connect(dl, "finished", G_CALLBACK(on_download_finished), NULL);
	g_signal_connect(dl, "failed", G_CALLBACK(on_download_failed), NULL);
}

/* ------------------------------------------------------------- links */

static gboolean on_link(GSocketService *svc, GSocketConnection *conn,
			GObject *src, gpointer d)
{
	GDataInputStream *in = g_data_input_stream_new(
		g_io_stream_get_input_stream(G_IO_STREAM(conn)));
	char *line = g_data_input_stream_read_line(in, NULL, NULL, NULL);

	if (line && g_str_has_prefix(line, "open ") &&
	    (g_str_has_prefix(line + 5, "http://") || g_str_has_prefix(line + 5, "https://"))) {
		panel_close();
		typing_end();
		if (ntabs < MAX_TABS)
			tab_new(line + 5, true, false);
		else
			load(line + 5);
		gtk_window_present(GTK_WINDOW(win));
	}
	g_free(line);
	g_object_unref(in);
	return TRUE;
}

static void links_listen(void)
{
	const char *dir = g_getenv("XDG_RUNTIME_DIR");
	GError *err = NULL;

	if (!dir)
		return;
	char *path = g_strdup_printf("%s/s22-app-%s.sock", dir, APP_ID);
	g_unlink(path);
	GSocketService *svc = g_socket_service_new();
	GSocketAddress *a = g_unix_socket_address_new(path);
	if (!g_socket_listener_add_address(G_SOCKET_LISTENER(svc), a, G_SOCKET_TYPE_STREAM,
					   G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &err)) {
		g_printerr("s22-browser: %s: %s\n", path, err->message);
		g_error_free(err);
	} else {
		g_signal_connect(svc, "incoming", G_CALLBACK(on_link), NULL);
		g_socket_service_start(svc);
	}
	g_object_unref(a);
	g_free(path);
}

/* ------------------------------------------------------------ style */

/* The phone's theme (s22-gui's colours), for the bar and the menu */
static void style_load(void)
{
	char *theme = kv_get(SETTINGS, "theme");
	char *path = g_strdup_printf("%s/%s.theme", THEMES, theme ? theme : "dark");
	static const char *keys[] = { "bg", "fg", "dim", "accent", "bar", "sel" };
	static const char *dark[] = { "#101418", "#e8eaed", "#9aa0a6", "#4fc3f7",
				      "#1c2228", "#2a3f52" };
	char *c[6];

	for (int i = 0; i < 6; i++) {
		c[i] = kv_get(path, keys[i]);
		if (!c[i])
			c[i] = g_strdup(dark[i]);
	}
	char *css = g_strdup_printf(
		"window { background: %1$s; }"
		".bar { background: %5$s; color: %2$s; padding: 3px 8px; min-height: 28px; }"
		".bar label { font: 15px \"DejaVu Sans\"; }"
		".bar label.host { font-weight: bold; color: %2$s; }"
		".bar label.title { color: %3$s; }"
		".bar entry { font: 16px \"DejaVu Sans\"; min-height: 26px; }"
		".bar button.tab { min-width: 24px; min-height: 22px; padding: 0 2px; margin: 0;"
		"  border: none; border-radius: 6px; box-shadow: none; background: %6$s;"
		"  background-image: none; color: %3$s; font: bold 13px \"DejaVu Sans\"; }"
		".bar button.tab.current { background: %4$s; color: %1$s; }"
		"progressbar trough, progressbar progress { min-height: 3px; border-radius: 0; }"
		"progressbar progress { background: %4$s; }"
		"progressbar trough { background: transparent; }"
		".panel { background: %1$s; color: %2$s; }"
		".panel .ptitle { font: bold 18px \"DejaVu Sans\"; padding: 10px 14px; background: %5$s; }"
		".panel list { background: %1$s; }"
		".panel row { padding: 11px 14px; font: 18px \"DejaVu Sans\"; color: %2$s;"
		"  border-bottom: 1px solid %5$s; }"
		".panel row:selected { background: %6$s; }"
		".panel .hint { color: %3$s; font-size: 14px; }"
		".toast { background: %5$s; color: %2$s; border-radius: 12px; padding: 8px 16px;"
		"  margin-bottom: 18px; font: 15px \"DejaVu Sans\"; }",
		c[0], c[1], c[2], c[3], c[4], c[5]);
	GtkCssProvider *p = gtk_css_provider_new();
	gtk_css_provider_load_from_string(p, css);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
		GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme",
		     !(theme && !strcmp(theme, "light")), NULL);
	for (int i = 0; i < 6; i++)
		g_free(c[i]);
	g_free(css);
	g_free(path);
	g_free(theme);
}

/* -------------------------------------------------------------- main */

static void on_active(GObject *o, GParamSpec *p, gpointer d)
{
	keypad_update();		/* after s22-wm's own choice for the app */
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
	g_main_loop_quit(loop);
	return TRUE;
}

static gboolean on_signal(gpointer d)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------- tabs */

static void on_tab_button(GtkButton *b, gpointer d)
{
	int i = GPOINTER_TO_INT(d);

	if (i < 0) {
		if (ntabs < MAX_TABS) {
			tab_new(HOME_URL, true, false);
			typing_start("");
		}
	} else if (i < ntabs) {
		tab_show(i);
	}
}

/* The numbered buttons at the bar's right, with two tabs or more */
static void tabs_update(void)
{
	GtkWidget *child;

	while ((child = gtk_widget_get_first_child(tab_box)))
		gtk_box_remove(GTK_BOX(tab_box), child);
	gtk_widget_set_visible(tab_box, ntabs > 1 && !typing);
	if (ntabs < 2)
		return;
	for (int i = 0; i <= ntabs && i <= MAX_TABS; i++) {
		if (i == ntabs && ntabs >= MAX_TABS)
			break;
		char n[8] = "+";
		if (i < ntabs)
			snprintf(n, sizeof(n), "%d", i + 1);
		GtkWidget *b = gtk_button_new_with_label(n);
		gtk_widget_set_focusable(b, false);
		gtk_widget_add_css_class(b, "tab");
		if (i == cur_tab)
			gtk_widget_add_css_class(b, "current");
		g_signal_connect(b, "clicked", G_CALLBACK(on_tab_button),
				 GINT_TO_POINTER(i < ntabs ? i : -1));
		gtk_box_append(GTK_BOX(tab_box), b);
	}
}

/* The phone's available memory, kB */
static long mem_available(void)
{
	char *text = read_text("/proc/meminfo"), *p;
	long kb = -1;

	if (text && (p = strstr(text, "MemAvailable:")))
		kb = strtol(p + 13, NULL, 10);
	g_free(text);
	return kb;
}

/* Memory short: end the process of the tab out of view longest, keeping
 * its address to load again when it is shown (never the tab in front) */
static void tabs_trim(void)
{
	long kb = mem_available();

	while (kb >= 0 && kb < LOW_MEM_KB) {
		int lru = -1;
		char *front = host_of(webkit_web_view_get_uri(view));
		for (int i = 0; i < ntabs; i++) {
			if (i == cur_tab || tab_pending[i])
				continue;
			/* the same site may share the front tab's process */
			char *h = host_of(webkit_web_view_get_uri(tabs[i]));
			bool same = h && front && !strcmp(h, front);
			g_free(h);
			if (!same && (lru < 0 || tab_seen[i] < tab_seen[lru]))
				lru = i;
		}
		g_free(front);
		if (lru < 0)
			return;
		const char *u = webkit_web_view_get_uri(tabs[lru]);
		tab_pending[lru] = g_strdup(u && *u ? u : HOME_URL);
		webkit_web_view_terminate_web_process(tabs[lru]);
		g_usleep(300000);		/* for the memory to come back */
		kb = mem_available();
	}
}

static void tab_show(int i)
{
	if (i < 0 || i >= ntabs)
		return;
	cur_tab = i;
	view = tabs[i];
	tab_seen[i] = g_get_monotonic_time();
	gtk_stack_set_visible_child(GTK_STACK(stack), GTK_WIDGET(view));
	if (tab_pending[i]) {
		char *u = tab_pending[i];
		tab_pending[i] = NULL;
		webkit_web_view_load_uri(view, u);
		g_free(u);
	}
	web_editing = false;
	keypad_update();
	bar_update();
	on_progress(NULL, NULL, NULL);
	tabs_update();
	if (panel_kind == P_NONE && !typing)
		gtk_widget_grab_focus(GTK_WIDGET(view));
	session_save();
	tabs_trim();
}

/* A tab's web process ended (put away above, or ended by WebKit for
 * memory, or crashed): the tab in front loads its page again at once,
 * another when it is next shown */
static void on_web_process_gone(WebKitWebView *v, WebKitWebProcessTerminationReason why,
				gpointer d)
{
	for (int i = 0; i < ntabs; i++) {
		if (tabs[i] != v)
			continue;
		if (v == view) {
			webkit_web_view_reload(v);
		} else if (!tab_pending[i]) {
			const char *u = webkit_web_view_get_uri(v);
			tab_pending[i] = g_strdup(u && *u ? u : HOME_URL);
		}
	}
}

/* A tab for uri (shown at once or not; lazy: loaded when first shown) */
static void tab_new(const char *uri, bool show, bool lazy)
{
	if (ntabs >= MAX_TABS) {
		toast_show("5 tabs at most");
		return;
	}
	/* The first tab gets the network session; the others take it from it
	 * (a related view may not be given one) */
	WebKitWebView *v = ntabs ?
		WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", tabs[0],
			"user-content-manager", ucm, "settings", wk_settings,
			"website-policies", wk_policies, NULL)) :
		WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session,
			"user-content-manager", ucm, "settings", wk_settings,
			"website-policies", wk_policies, NULL));
	webkit_web_view_set_zoom_level(v, zoom_pct / 100.0);
	g_signal_connect(v, "load-changed", G_CALLBACK(on_load_changed), NULL);
	g_signal_connect(v, "notify::estimated-load-progress", G_CALLBACK(on_progress), NULL);
	g_signal_connect(v, "notify::is-loading", G_CALLBACK(on_progress), NULL);
	g_signal_connect(v, "notify::title", G_CALLBACK(on_title), NULL);
	g_signal_connect(v, "notify::uri", G_CALLBACK(on_title), NULL);
	g_signal_connect(v, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
	g_signal_connect(v, "permission-request", G_CALLBACK(on_permission), NULL);
	g_signal_connect(v, "web-process-terminated", G_CALLBACK(on_web_process_gone), NULL);
	gtk_stack_add_child(GTK_STACK(stack), GTK_WIDGET(v));
	tabs[ntabs] = v;
	tab_pending[ntabs] = NULL;
	if (lazy)
		tab_pending[ntabs] = g_strdup(uri && *uri ? uri : HOME_URL);
	else
		webkit_web_view_load_uri(v, uri && *uri ? uri : HOME_URL);
	ntabs++;
	if (show)
		tab_show(ntabs - 1);
	else
		tabs_update();	/* restored ones: session_load shows one at the end */
}

/* Close a tab; the last one closes the browser */
static void tab_close(int i)
{
	if (i < 0 || i >= ntabs)
		return;
	if (ntabs == 1) {
		g_main_loop_quit(loop);
		return;
	}
	gtk_stack_remove(GTK_STACK(stack), GTK_WIDGET(tabs[i]));
	g_free(tab_pending[i]);
	memmove(&tabs[i], &tabs[i + 1], (ntabs - i - 1) * sizeof(tabs[0]));
	memmove(&tab_pending[i], &tab_pending[i + 1], (ntabs - i - 1) * sizeof(tab_pending[0]));
	memmove(&tab_seen[i], &tab_seen[i + 1], (ntabs - i - 1) * sizeof(tab_seen[0]));
	ntabs--;
	tabs[ntabs] = NULL;
	tab_pending[ntabs] = NULL;
	tab_show(cur_tab > i || cur_tab >= ntabs ? (cur_tab > 0 ? cur_tab - 1 : 0) : cur_tab);
	toast_show("Tab closed");
}

/* The tabs from last time ("@N" first, then a line a tab; an older file
 * has just one address) */
static bool session_load(void)
{
	char *text = read_text(last_file);
	int front = 0, n = 0;

	if (!text)
		return false;
	for (char *l = strtok(text, "\n"); l; l = strtok(NULL, "\n")) {
		g_strstrip(l);
		if (l[0] == '@') {
			front = atoi(l + 1);
		} else if (g_str_has_prefix(l, "http") && n < MAX_TABS) {
			tab_new(l, false, true);
			n++;
		}
	}
	g_free(text);
	if (n)
		tab_show(CLAMP(front, 0, n - 1));
	return n > 0;
}

static void build_ui(void)
{
	win = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(win), "Browser");
	gtk_window_set_default_size(GTK_WINDOW(win), 480, 614);

	GtkWidget *overlay = gtk_overlay_new();
	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_add_css_class(bar, "bar");
	host_label = gtk_label_new("");
	gtk_widget_add_css_class(host_label, "host");
	title_label = gtk_label_new("");
	gtk_widget_add_css_class(title_label, "title");
	gtk_label_set_ellipsize(GTK_LABEL(title_label), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(title_label), 0);
	gtk_widget_set_hexpand(title_label, true);
	entry = gtk_entry_new();
	gtk_widget_set_hexpand(entry, true);
	gtk_entry_set_input_hints(GTK_ENTRY(entry), GTK_INPUT_HINT_NO_SPELLCHECK);
	gtk_widget_set_visible(entry, false);
	g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), NULL);
	GtkEventController *fc = gtk_event_controller_focus_new();
	g_signal_connect(fc, "leave", G_CALLBACK(on_entry_leave), NULL);
	gtk_widget_add_controller(entry, fc);
	tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
	gtk_widget_set_visible(tab_box, false);
	gtk_box_append(GTK_BOX(bar), host_label);
	gtk_box_append(GTK_BOX(bar), title_label);
	gtk_box_append(GTK_BOX(bar), entry);
	gtk_box_append(GTK_BOX(bar), tab_box);

	progress = gtk_progress_bar_new();
	gtk_widget_set_opacity(progress, 0.0);

	gtk_box_append(GTK_BOX(vbox), bar);
	gtk_box_append(GTK_BOX(vbox), progress);
	stack = gtk_stack_new();
	gtk_widget_set_vexpand(stack, true);
	gtk_box_append(GTK_BOX(vbox), stack);
	gtk_overlay_set_child(GTK_OVERLAY(overlay), vbox);

	panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(panel, "panel");
	panel_title = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(panel_title), 0);
	gtk_widget_add_css_class(panel_title, "ptitle");
	panel_list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(panel_list), GTK_SELECTION_SINGLE);
	panel_scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(panel_scroll), panel_list);
	gtk_widget_set_vexpand(panel_scroll, true);
	gtk_box_append(GTK_BOX(panel), panel_title);
	gtk_box_append(GTK_BOX(panel), panel_scroll);
	gtk_widget_set_visible(panel, false);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), panel);

	toast = gtk_label_new("");
	gtk_widget_add_css_class(toast, "toast");
	gtk_widget_set_halign(toast, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(toast, GTK_ALIGN_END);
	gtk_widget_set_can_target(toast, false);
	gtk_widget_set_visible(toast, false);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), toast);

	gtk_window_set_child(GTK_WINDOW(win), overlay);

	GtkEventController *k = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(k, GTK_PHASE_CAPTURE);
	g_signal_connect(k, "key-pressed", G_CALLBACK(on_key), NULL);
	g_signal_connect(k, "key-released", G_CALLBACK(on_key_up), NULL);
	gtk_widget_add_controller(win, k);
	g_signal_connect(win, "notify::is-active", G_CALLBACK(on_active), NULL);
	g_signal_connect(win, "close-request", G_CALLBACK(on_close), NULL);
}

static void build_view(void)
{
	char *cache = g_build_filename(g_get_user_cache_dir(), "s22-browser", NULL);
	char *filters = g_build_filename(data_dir, "filters", NULL);

	session = webkit_network_session_new(data_dir, cache);
	g_signal_connect(session, "download-started", G_CALLBACK(on_download_started), NULL);
	webkit_web_context_set_cache_model(webkit_web_context_get_default(),
					   WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);

	ucm = webkit_user_content_manager_new();
	webkit_user_content_manager_register_script_message_handler(ucm, "s22", NULL);
	g_signal_connect(ucm, "script-message-received::s22", G_CALLBACK(on_script_message), NULL);
	WebKitUserScript *us = webkit_user_script_new(FOCUS_JS,
		WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
		NULL, NULL);
	webkit_user_content_manager_add_script(ucm, us);
	webkit_user_script_unref(us);

	WebKitSettings *s = webkit_settings_new();
	webkit_settings_set_enable_spatial_navigation(s, TRUE);
	webkit_settings_set_enable_developer_extras(s, FALSE);
	webkit_settings_set_default_font_family(s, "DejaVu Sans");
	webkit_settings_set_enable_back_forward_navigation_gestures(s, FALSE);

	wk_settings = s;
	wk_policies = webkit_website_policies_new_with_policies(
		"autoplay", WEBKIT_AUTOPLAY_DENY, NULL);
	ua_apply();

	g_mkdir_with_parents(filters, 0700);
	store = webkit_user_content_filter_store_new(filters);
	youtube_load();
	filters_apply();		/* the YouTube script, until the lists load */
	webkit_user_content_filter_store_load(store, FILTER_ID, NULL, on_filter_loaded, NULL);
	g_free(filters);
	g_free(cache);
}

int main(int argc, char **argv)
{
	g_set_prgname(APP_ID);		/* the Wayland app_id: raw keypad */
	gtk_init();

	data_dir = g_build_filename(g_get_user_data_dir(), "s22-browser", NULL);
	g_mkdir_with_parents(data_dir, 0700);
	last_file = g_build_filename(data_dir, "last", NULL);
	prefs_file = g_build_filename(data_dir, "prefs", NULL);
	marks_file = g_build_filename(data_dir, "bookmarks", NULL);
	marks = g_ptr_array_new_with_free_func(mark_free);
	prefs_load();
	marks_load();

	style_load();
	build_ui();
	build_view();
	links_listen();

	/* Last time's tabs, and a link given as the argument in a new one */
	char *first = argc > 1 ? typed_to_uri(argv[1]) : NULL;
	bool restored = session_load();
	if (first || !restored)
		tab_new(first ? first : HOME_URL, true, false);
	g_free(first);

	gtk_window_present(GTK_WINDOW(win));
	gtk_widget_grab_focus(GTK_WIDGET(view));

	loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGTERM, on_signal, NULL);
	g_unix_signal_add(SIGINT, on_signal, NULL);
	g_main_loop_run(loop);

	session_save();
	return 0;
}
