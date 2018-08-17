#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <vte/vte.h>
#include "iniparser.h"

// Config path.
#define LWT_CONF ".config/lwt/lwt.conf"

// Fallback options if not present in config file.
#define LWT_FONT "Fixed 9"
#define LWT_SHELL "/bin/bash"
#define LWT_OPACITY 1.0

// Theme configuration
struct theme {
	GdkRGBA fg, bg;
	GdkRGBA colors[16];
};

// Convert theme from iniparser dictionary
int ini_load_theme(struct theme *theme, dictionary *dict)
{
	char key[48];
	const char *val = NULL;
	int i;
	for (i = 0; i < 16; ++i) {
		snprintf(key, 48, "color:%d", i);
		if ((val = iniparser_getstring(dict, key, NULL))) {
			gdk_rgba_parse(theme->colors + i, val);
		} else {
			return 1;
		}
	}
	if ((val = iniparser_getstring(dict, "color:fg", NULL))) {
		gdk_rgba_parse(&(theme->fg), val);
	} else {
		return 1;
	}
	if ((val = iniparser_getstring(dict, "color:bg", NULL))) {
		gdk_rgba_parse(&(theme->bg), val);
	} else {
		return 1;
	}
	return 0;
}

gboolean on_key_press(GtkWidget *win, GdkEventKey *event, VteTerminal *vte);
void on_screen_change(GtkWidget *win, GdkScreen *prev, gpointer data);
void update_visuals(GtkWidget *win);
void clear_shell(VteTerminal *vte);

int main(int argc, char **argv) {
	gtk_init(&argc, &argv);

	struct theme *theme = NULL;
	// Parse config file.
	char conf_path[1024];
	snprintf(conf_path, sizeof(conf_path), "%s/%s", getenv("HOME"), LWT_CONF);
	dictionary *dict = iniparser_load(conf_path);
	char *font = strdup(iniparser_getstring(dict, "lwt:font", LWT_FONT));
	char *shell = strdup(iniparser_getstring(dict, "lwt:shell", LWT_SHELL));
	double opacity = iniparser_getdouble(dict, "lwt:opacity", LWT_OPACITY);
	if (iniparser_find_entry(dict, "color")) {
		theme = calloc(1, sizeof(struct theme));
		if (ini_load_theme(theme, dict)) {
			g_printerr("Could not load complete theme; using default colors");
			free(theme);
			theme = NULL;
		}
	}

	iniparser_freedict(dict);

	// Create window with a terminal emulator.
	GtkWindow *win = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
	VteTerminal *vte = VTE_TERMINAL(vte_terminal_new());
	gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(vte));
	vte_terminal_set_font(vte, pango_font_description_from_string(font));
	vte_terminal_set_scrollback_lines(vte, 1000000);

	// Connect signals.
	g_signal_connect(win, "delete_event", gtk_main_quit, NULL);
	g_signal_connect(win, "key-press-event", G_CALLBACK(on_key_press), vte);
	g_signal_connect(vte, "child-exited", gtk_main_quit, NULL);

	// Enable transparency.
	if (opacity < 1) {
		GdkScreen *screen = gdk_screen_get_default();
		if (!gdk_screen_is_composited(screen)) {
			fprintf(stderr, "unable to enable transparency; no compositing manager running (e.g. xcompmgr)\n.");
		} else {
			g_signal_connect(win, "screen-changed", G_CALLBACK(on_screen_change), NULL);
			update_visuals(GTK_WIDGET(win));
			gtk_widget_set_app_paintable(GTK_WIDGET(win), TRUE);
			gtk_widget_set_opacity(GTK_WIDGET(win), opacity);
		}
	}

	//Set theme.
	if (theme) {
		vte_terminal_set_colors(vte, &(theme->fg), &(theme->bg), theme->colors, 16);
	}


	// Fork shell process.
	argv[0] = shell;
	vte_terminal_spawn_sync(vte, 0, NULL, argv, NULL, 0, NULL, NULL, NULL, NULL, NULL);

	// Show window.
	gtk_widget_show_all(GTK_WIDGET(win));
	gtk_main();

	return 0;
}

// on_key_press handles key-press events for the GTK window.
gboolean on_key_press(GtkWidget *win, GdkEventKey *event, VteTerminal *vte) {
	// [ctrl] + [shift]
	int mod = event->state & gtk_accelerator_get_default_mod_mask();
	if (mod == (GDK_CONTROL_MASK|GDK_SHIFT_MASK)) {
		switch (event->keyval) {
		// [ctrl] + [shift] + 'c'
		case GDK_KEY_C:
		case GDK_KEY_c:
			vte_terminal_copy_clipboard_format(vte, VTE_FORMAT_TEXT);
			return TRUE;
		// [ctrl] + [shift] + 'v'
		case GDK_KEY_V:
		case GDK_KEY_v:
			vte_terminal_paste_clipboard(vte);
			return TRUE;
		// [ctrl] + [shift] + 'l'
		case GDK_KEY_L:
		case GDK_KEY_l:
			clear_shell(vte);
			return TRUE;
		}
	}

	// [ctrl]
	double font_scale;
	if ((mod&GDK_CONTROL_MASK) != 0) {
		switch (event->keyval) {
		// [ctrl] + '+'
		case GDK_KEY_plus:
			font_scale = vte_terminal_get_font_scale(vte);
			font_scale *= 1.1;
			vte_terminal_set_font_scale(vte, font_scale);
			return TRUE;
		// [ctrl] + '-'
		case GDK_KEY_minus:
			font_scale = vte_terminal_get_font_scale(vte);
			font_scale /= 1.1;
			vte_terminal_set_font_scale(vte, font_scale);
			return TRUE;
		}
	}
	return FALSE;
}

// on_screen_change handles screen-changed events for the GTK window.
void on_screen_change(GtkWidget *win, GdkScreen *prev, gpointer data) {
	update_visuals(win);
}

// update_visuals updates the video display format of the screen for the given
// window.
void update_visuals(GtkWidget *win) {
	GdkScreen *screen = gtk_widget_get_screen(win);
	GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
	gtk_widget_set_visual(win, visual);
}

// clear_shell clears the given shell window.
void clear_shell(VteTerminal *vte) {
	vte_terminal_reset(vte, TRUE, TRUE);
	VtePty *pty = vte_terminal_get_pty(vte);
	write(vte_pty_get_fd(pty), "\x0C", 1);
}
