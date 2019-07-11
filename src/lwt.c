#include <stdlib.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <vte/vte.h>

// Config path.
#define LWT_CONF ".config/lwt/lwt.conf"

// Fallback options if not present in config file.
#define LWT_FONT "Fixed 9"
#define LWT_SHELL "/bin/bash"
#define LWT_OPACITY 1.0
#define LWT_SCROLLBACK 1000000
#define LWT_SPAWN_TIMEOUT -1

// Theme configuration
struct theme {
	GdkRGBA fg, bg;
	GdkRGBA colors[256];
	gboolean bold_is_bright;
	size_t size;
};

void on_shell_spawn(VteTerminal *vte, GPid pid, GError *error, gpointer user_data);
gboolean on_button_press(GtkWidget *win, GdkEventButton *event, VteTerminal *vte);
gboolean on_key_press(GtkWidget *win, GdkEventKey *event, VteTerminal *vte);
void on_screen_change(GtkWidget *win, GdkScreen *prev, gpointer data);
void update_visuals(GtkWidget *win);
void clear_shell(VteTerminal *vte);
void on_bell(VteTerminal *vte, gpointer data);
void on_select_clipboard(VteTerminal *vte, gpointer data);
int keyfile_load_color(GdkRGBA *dest, GKeyFile *kf, char* group, char *key);
size_t conf_theme_set_size(struct theme *theme, GKeyFile *conf);
int conf_load_theme(struct theme *theme, GKeyFile *conf);

int main(int argc, char **argv) {
	gtk_init(&argc, &argv);

	struct theme *theme = NULL;
	char *font, *shell;
	double opacity;
	int scrollback, spawn_timeout;
	gboolean select_to_clipboard;
	//get default shell
	char *default_shell = vte_get_user_shell();
	if (!default_shell)
		default_shell = LWT_SHELL;

	// Parse config file.
	GError *err = NULL;
	char conf_path[1024];
	snprintf(conf_path, sizeof(conf_path), "%s/%s", getenv("HOME"), LWT_CONF);
	GKeyFile *conf = g_key_file_new();
	g_key_file_load_from_file(conf, conf_path, G_KEY_FILE_NONE, NULL);
	font = g_key_file_get_string(conf, "lwt", "font", &err);
	if (err)
		font = LWT_FONT;
	err = NULL;
	shell = g_key_file_get_string(conf, "lwt", "shell", &err);
	if (err)
		shell = default_shell;
	err = NULL;
	opacity = g_key_file_get_double(conf, "lwt", "opacity", &err);
	if (err)
		opacity = LWT_OPACITY;
	err = NULL;
	scrollback = g_key_file_get_integer(conf, "lwt", "scrollback", &err);
	if (err)
		scrollback = LWT_SCROLLBACK;
	err = NULL;
	spawn_timeout = g_key_file_get_integer(conf, "lwt", "spawn_timeout", &err);
	if (err)
		spawn_timeout = LWT_SPAWN_TIMEOUT;
	err = NULL;
	select_to_clipboard = g_key_file_get_boolean(conf, "lwt", "select_to_clipboard", &err);
	if (err)
		select_to_clipboard = FALSE;
	err = NULL;
	if (g_key_file_has_group(conf, "theme")) {
		theme = calloc(1, sizeof(struct theme));
		if (conf_load_theme(theme, conf)) {
			g_warning("Could not load complete theme; using default colors");
			free(theme);
			theme = NULL;
		}
	}

	g_key_file_free(conf);

	// Create window with a terminal emulator.
	GtkWindow *win = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
	VteTerminal *vte = VTE_TERMINAL(vte_terminal_new());
	gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(vte));
	vte_terminal_set_font(vte, pango_font_description_from_string(font));
	vte_terminal_set_scrollback_lines(vte, scrollback);

	// Connect signals.
	g_signal_connect(win, "delete_event", gtk_main_quit, NULL);
	g_signal_connect(win, "key-press-event", G_CALLBACK(on_key_press), vte);
	g_signal_connect(vte, "child-exited", gtk_main_quit, NULL);
	g_signal_connect(vte, "bell", G_CALLBACK(on_bell), win);
	g_signal_connect(vte, "button-press-event", G_CALLBACK(on_button_press), vte);
	if (select_to_clipboard)
		g_signal_connect(vte, "selection-changed", G_CALLBACK(on_select_clipboard), vte);
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
		vte_terminal_set_colors(vte, &(theme->fg), &(theme->bg), theme->colors, theme->size);
		vte_terminal_set_bold_is_bright(vte, theme->bold_is_bright);
	}

	//Allow links
	vte_terminal_set_allow_hyperlink(vte, TRUE);

	// Fork shell process.
	argv[0] = shell;
#if (VTE_MAJOR_VERSION < 1) && (VTE_MINOR_VERSION < 52)
	//FIX for https://gitlab.gnome.org/GNOME/vte/issues/7
	vte_terminal_spawn_sync(vte, 0, NULL, argv, NULL, 0, NULL, NULL, NULL, NULL, NULL);
#else
	//proper way.
	vte_terminal_spawn_async(vte, 0, NULL, argv, NULL, 0, NULL, NULL, NULL, spawn_timeout, NULL, &on_shell_spawn, (gpointer)win);
#endif
	// Show main window.
	gtk_widget_show_all(GTK_WIDGET(win));
	gtk_main();

	return 0;
}

// on_select_clipboard will copy the selected text to clipboard
void on_select_clipboard(VteTerminal *vte, gpointer data)
{
	if (vte_terminal_get_has_selection(vte)) {
		vte_terminal_copy_clipboard_format(vte, VTE_FORMAT_TEXT);
	}
}

// on_shell_spawn handles the spawn of the child shell process
void on_shell_spawn(VteTerminal *vte, GPid pid, GError *error, gpointer user_data)
{
	if (error) {
		g_error("error spawning shell: %s\n", error->message);
	}
}

//handle button presses
gboolean on_button_press(GtkWidget *win, GdkEventButton *event, VteTerminal *vte)
{
	GtkClipboard *cb;
	char *link;
	//right click -- copy marked URL
	if (event->button == 3) {
		if ((link = vte_terminal_hyperlink_check_event(vte, (GdkEvent*)event))) {
			cb = gtk_clipboard_get(GDK_NONE);
			gtk_clipboard_set_text(cb, link, -1);
		}
	}
	return FALSE;
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

// set urgent when bell rings.
void on_bell(VteTerminal *vte, gpointer data)
{
	if (!gtk_window_has_toplevel_focus(GTK_WINDOW(data)))
		gtk_window_set_urgency_hint(GTK_WINDOW(data), TRUE);
}

// Load single color from GKeyFile
int keyfile_load_color(GdkRGBA *dest, GKeyFile *kf, char* group, char *key)
{
	int ret = 1;
	char *val = g_key_file_get_string(kf, group, key, NULL);
	if (val && gdk_rgba_parse(dest,val)) {
		ret = 0;
	}
	g_free(val);
	return ret;
}

// Set size of a theme from GKeyFile configuration
size_t conf_theme_set_size(struct theme *theme, GKeyFile *conf)
{
	char *val, **size;
	char *sizes[] = {"0", "8", "16", "232", "256", NULL}; // all VTE supports.
	for (size = sizes; *size; ++size) {
		val = g_key_file_get_string(conf, "theme", *size, NULL);
		if (!val) {
			break;
		} else {
			g_free(val);
		}
	}
	theme->size = atol(*size);
	g_free(val);
	return theme->size;
}


// Load whole theme from GKeyFile configuration
int conf_load_theme(struct theme *theme, GKeyFile *conf)
{
	char key[4];
	int i, missing = 0;
	conf_theme_set_size(theme, conf);
	for (i = 0; i < theme->size; ++i) {
		snprintf(key, 4, "%d", i);
		missing += keyfile_load_color(theme->colors + i, conf, "theme", key);
	}
	missing += keyfile_load_color(&(theme->fg), conf, "theme", "fg");
	missing += keyfile_load_color(&(theme->bg), conf, "theme", "bg");
	theme->bold_is_bright = g_key_file_get_boolean(conf, "theme", "bold_is_bright", NULL);
	return missing;
}


