#include <libxfce4panel/libxfce4panel.h>
#include <gtk/gtk.h>
#include <curl/curl.h>
#include <libnotify/notify.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>

typedef struct {
    char *name;
    char *mission;
    int players;
    int port; // Port number
    int ping; // Ping time in milliseconds
} ServerInfo;

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget *box;
    GtkWidget *label;
    //int online;
    int in_game;
    int servers;
    int online_users;
    int refresh_interval;
    guint timeout_id;
    char *command;
    gboolean pipeline_notified;
    gboolean notify_enabled;
    int min_players;
    char *mission_name;
    gboolean ignore_mission_name;
    time_t last_notify_time;
    gboolean fetching;
} PluginData;

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void write_log(const char *fmt, ...) {
    pthread_mutex_lock(&log_mutex);
    FILE *f = fopen("/tmp/285AApanel.log", "a");
    if (!f) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    time_t t = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&t, &tm_buf);
    char timestr[64];
    if (tm) strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", tm);
    else snprintf(timestr, sizeof(timestr), "unknown-time");
    fprintf(f, "%s - ", timestr);
    vfprintf(f, fmt, ap);
    fprintf(f, "\n");
    va_end(ap);
    fclose(f);
    pthread_mutex_unlock(&log_mutex);
}

static char *url_decode(const char *str) {
    GString *decoded = g_string_new("");
    for (const char *p = str; *p; p++) {
        if (*p == '%' && isxdigit(p[1]) && isxdigit(p[2])) {
            char hex[3] = {p[1], p[2], 0};
            g_string_append_c(decoded, (char)strtol(hex, NULL, 16));
            p += 2;
        } else {
            g_string_append_c(decoded, *p);
        }
    }
    return g_string_free(decoded, FALSE);
}

static gint compare_players(gconstpointer a, gconstpointer b) {
    ServerInfo *sa = (ServerInfo *)a;
    ServerInfo *sb = (ServerInfo *)b;
    return sb->players - sa->players;
}

static void free_server(ServerInfo *si) {
    g_free(si->name);
    g_free(si->mission);
    g_free(si);
}

static void show_about(XfcePanelPlugin *plugin, PluginData *data);
static gboolean update_data(PluginData *data);
static void show_run_dialog(PluginData *data);
static gboolean on_click(GtkWidget *widget, GdkEventButton *event, PluginData *data);
static void free_data(XfcePanelPlugin *plugin, PluginData *data);
static void show_properties(XfcePanelPlugin *plugin, PluginData *data);

typedef struct {
    char *cmd;
} SpawnData;

static gboolean spawn_idle_func(gpointer user_data) {
    SpawnData *s = (SpawnData *)user_data;
    if (s && s->cmd) {
        write_log("spawn_idle: running '%s'", s->cmd);
        g_spawn_command_line_async(s->cmd, NULL);
    }
    if (s) {
        g_free(s->cmd);
        g_free(s);
    }
    return FALSE;
}

static void on_notify_toggled(GtkToggleButton *toggle, gpointer user_data) {
    GtkWidget *widget = GTK_WIDGET(user_data);
    gtk_widget_set_sensitive(widget, gtk_toggle_button_get_active(toggle));
}

typedef struct {
    GtkWidget *check_notify;
    GtkWidget *check_ignore;
    GtkWidget *target_widget;
} MissionFieldData;

static void on_mission_field_update(GtkToggleButton *toggle, gpointer user_data) {
    MissionFieldData *mfd = (MissionFieldData *)user_data;
    gboolean notify_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mfd->check_notify));
    gboolean ignore_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mfd->check_ignore));
    // Field is enabled only if notify is enabled AND ignore is NOT active
    gtk_widget_set_sensitive(mfd->target_widget, notify_active && !ignore_active);
}

static void show_properties(XfcePanelPlugin *plugin, PluginData *data) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons("285AApanel Properties", NULL, GTK_DIALOG_MODAL, "_OK", GTK_RESPONSE_OK, "_Cancel", GTK_RESPONSE_CANCEL, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 300, -1); // Minimum width 300px
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(content), grid);

    // Refresh Interval
    GtkWidget *label_refresh = gtk_label_new("Refresh Interval (seconds):");
    gtk_widget_set_halign(label_refresh, GTK_ALIGN_START);
    GtkWidget *spin_refresh = gtk_spin_button_new_with_range(60, 600, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_refresh), data->refresh_interval);
    gtk_grid_attach(GTK_GRID(grid), label_refresh, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), spin_refresh, 1, 0, 1, 1);

    // Command
    GtkWidget *label_command = gtk_label_new("Command:");
    gtk_widget_set_halign(label_command, GTK_ALIGN_START);
    GtkWidget *entry_command = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(entry_command), 40);
    gtk_entry_set_text(GTK_ENTRY(entry_command), data->command ? data->command : "");
    gtk_grid_attach(GTK_GRID(grid), label_command, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry_command, 1, 1, 1, 1);

    // Notify Enabled
    GtkWidget *check_notify = gtk_check_button_new_with_label("Enable Notifications");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_notify), data->notify_enabled);
    gtk_grid_attach(GTK_GRID(grid), check_notify, 0, 2, 2, 1);

    // Min Players
    GtkWidget *label_min_players = gtk_label_new("Minimum Players for Notification:");
    gtk_widget_set_halign(label_min_players, GTK_ALIGN_START);
    GtkWidget *spin_min_players = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_min_players), data->min_players);
    gtk_grid_attach(GTK_GRID(grid), label_min_players, 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), spin_min_players, 1, 3, 1, 1);

    // Ignore Mission Name checkbox
    GtkWidget *check_ignore_mission = gtk_check_button_new_with_label("Ignore Mission Name (notify on any mission)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_ignore_mission), data->ignore_mission_name);
    gtk_grid_attach(GTK_GRID(grid), check_ignore_mission, 0, 4, 2, 1);

    // Mission Name
    GtkWidget *label_mission = gtk_label_new("Mission Name for Notification:");
    gtk_widget_set_halign(label_mission, GTK_ALIGN_START);
    GtkWidget *entry_mission = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_mission), data->mission_name ? data->mission_name : "");
    gtk_grid_attach(GTK_GRID(grid), label_mission, 0, 5, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry_mission, 1, 5, 1, 1);

    // Set initial sensitivity
    gtk_widget_set_sensitive(label_min_players, data->notify_enabled);
    gtk_widget_set_sensitive(spin_min_players, data->notify_enabled);
    gtk_widget_set_sensitive(check_ignore_mission, data->notify_enabled);
    gtk_widget_set_sensitive(label_mission, data->notify_enabled && !data->ignore_mission_name);
    gtk_widget_set_sensitive(entry_mission, data->notify_enabled && !data->ignore_mission_name);

    // Create helper structures for mission field callbacks
    MissionFieldData *mfd_label = g_new(MissionFieldData, 1);
    mfd_label->check_notify = check_notify;
    mfd_label->check_ignore = check_ignore_mission;
    mfd_label->target_widget = label_mission;

    MissionFieldData *mfd_entry = g_new(MissionFieldData, 1);
    mfd_entry->check_notify = check_notify;
    mfd_entry->check_ignore = check_ignore_mission;
    mfd_entry->target_widget = entry_mission;

    // Connect signal to toggle sensitivity
    g_signal_connect(check_notify, "toggled", G_CALLBACK(on_notify_toggled), label_min_players);
    g_signal_connect(check_notify, "toggled", G_CALLBACK(on_notify_toggled), spin_min_players);
    g_signal_connect(check_notify, "toggled", G_CALLBACK(on_notify_toggled), check_ignore_mission);
    g_signal_connect(check_notify, "toggled", G_CALLBACK(on_mission_field_update), mfd_label);
    g_signal_connect(check_notify, "toggled", G_CALLBACK(on_mission_field_update), mfd_entry);
    g_signal_connect(check_ignore_mission, "toggled", G_CALLBACK(on_mission_field_update), mfd_label);
    g_signal_connect(check_ignore_mission, "toggled", G_CALLBACK(on_mission_field_update), mfd_entry);

    gtk_widget_show_all(dialog);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_OK) {
        // Save values
        data->refresh_interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_refresh));
        g_free(data->command);
        data->command = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry_command)));
        data->notify_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_notify));
        data->min_players = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_min_players));
        data->ignore_mission_name = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_ignore_mission));
        g_free(data->mission_name);
        data->mission_name = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry_mission)));
        if (!data->mission_name || !*data->mission_name) {
            g_free(data->mission_name);
            data->mission_name = g_strdup("Pipeline");
        }

        // Save to config
        GKeyFile *keyfile = g_key_file_new();
        gchar *config_path = g_build_filename(g_get_user_config_dir(), "285AApanel", "config", NULL);
        g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, NULL);
        g_key_file_set_integer(keyfile, "settings", "refresh_interval", data->refresh_interval);
        g_key_file_set_string(keyfile, "settings", "command", data->command);
        g_key_file_set_boolean(keyfile, "settings", "notify_enabled", data->notify_enabled);
        g_key_file_set_integer(keyfile, "settings", "min_players", data->min_players);
        g_key_file_set_boolean(keyfile, "settings", "ignore_mission_name", data->ignore_mission_name);
        g_key_file_set_string(keyfile, "settings", "mission_name", data->mission_name);
        gchar *config_dir = g_path_get_dirname(config_path);
        g_mkdir_with_parents(config_dir, 0755);
        g_free(config_dir);
        g_key_file_save_to_file(keyfile, config_path, NULL);
        g_free(config_path);
        g_key_file_free(keyfile);

        // Update timeout
        if (data->timeout_id) g_source_remove(data->timeout_id);
        data->timeout_id = g_timeout_add_seconds(data->refresh_interval, (GSourceFunc)update_data, data);
    }
    
    // Free helper structures
    g_free(mfd_label);
    g_free(mfd_entry);
    
    gtk_widget_destroy(dialog);
}

static void show_about(XfcePanelPlugin *plugin, PluginData *data) {
    const gchar *authors[] = {"Developer", NULL};
    gtk_show_about_dialog(NULL, "program-name", "285AApanel", "version", "1.0", "comments", "Displays game server statistics", "authors", authors, NULL);
}

static void show_run_dialog(PluginData *data) {
    // Launch application via idle handler to avoid reentrancy and
    // potential race conditions with the panel/plugin lifecycle.
    if (!data || !data->command || !*data->command) return;

    SpawnData *sd = g_new0(SpawnData, 1);
    sd->cmd = g_strdup(data->command);
    write_log("show_run_dialog: scheduling spawn cmd='%s'", sd->cmd);
    g_idle_add(spawn_idle_func, sd);
}

static gboolean on_click(GtkWidget *widget, GdkEventButton *event, PluginData *data) {
    if (event->button == 1) { // left click
        show_run_dialog(data);
        return TRUE;
    }
    return FALSE;
}
static void free_data(XfcePanelPlugin *plugin, PluginData *data);
static void update_labels(PluginData *data);
static int get_servers();
static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata);
static gboolean update_data(PluginData *data);
static gboolean on_visibility_notify(GtkWidget *widget, GdkEventVisibility *event, PluginData *data);
static gboolean on_map_event(GtkWidget *widget, GdkEvent *event, PluginData *data);
static gboolean on_unmap_event(GtkWidget *widget, GdkEvent *event, PluginData *data);
static void construct(XfcePanelPlugin *plugin) {
    GKeyFile *keyfile = g_key_file_new();
    gchar *config_path = g_build_filename(g_get_user_config_dir(), "285AApanel", "config", NULL);

    PluginData *data = g_new(PluginData, 1);
    data->plugin = plugin;
    data->in_game = 0;
    data->servers = 0;
    data->online_users = 0;
    data->refresh_interval = 60;
    data->fetching = FALSE;
    write_log("construct: allocated PluginData %p", data);
    g_object_set_data(G_OBJECT(plugin), "285AApanel-data", data);
    data->command = g_strdup("");
    data->pipeline_notified = FALSE;
    data->notify_enabled = TRUE;
    data->min_players = 3;
    data->ignore_mission_name = FALSE;
    data->mission_name = g_strdup("Pipeline");
    data->last_notify_time = 0;

    if (g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, NULL)) {
        data->refresh_interval = g_key_file_get_integer(keyfile, "settings", "refresh_interval", NULL);
        if (data->refresh_interval < 1) data->refresh_interval = 60;
        g_free(data->command);
        data->command = g_key_file_get_string(keyfile, "settings", "command", NULL);
        if (!data->command) data->command = g_strdup("");
        data->notify_enabled = g_key_file_get_boolean(keyfile, "settings", "notify_enabled", NULL);
        data->min_players = g_key_file_get_integer(keyfile, "settings", "min_players", NULL);
        if (data->min_players < 1) data->min_players = 3;
        data->ignore_mission_name = g_key_file_get_boolean(keyfile, "settings", "ignore_mission_name", NULL);
        g_free(data->mission_name);
        data->mission_name = g_key_file_get_string(keyfile, "settings", "mission_name", NULL);
        if (!data->mission_name) data->mission_name = g_strdup("Pipeline");
    }

    // Set defaults for missing keys
    if (!g_key_file_has_key(keyfile, "settings", "notify_enabled", NULL)) data->notify_enabled = TRUE;
    if (!g_key_file_has_key(keyfile, "settings", "min_players", NULL)) data->min_players = 3;
    if (!g_key_file_has_key(keyfile, "settings", "ignore_mission_name", NULL)) data->ignore_mission_name = FALSE;
    if (!g_key_file_has_key(keyfile, "settings", "mission_name", NULL)) data->mission_name = g_strdup("Pipeline");
    if (!g_key_file_has_key(keyfile, "settings", "refresh_interval", NULL)) data->refresh_interval = 60;
    if (!g_key_file_has_key(keyfile, "settings", "command", NULL)) data->command = g_strdup("");

    // Save defaults if not present
    if (g_key_file_has_key(keyfile, "settings", "notify_enabled", NULL) == FALSE) {
        g_key_file_set_boolean(keyfile, "settings", "notify_enabled", TRUE);
    }
    if (g_key_file_has_key(keyfile, "settings", "min_players", NULL) == FALSE) {
        g_key_file_set_integer(keyfile, "settings", "min_players", 3);
    }
    if (g_key_file_has_key(keyfile, "settings", "ignore_mission_name", NULL) == FALSE) {
        g_key_file_set_boolean(keyfile, "settings", "ignore_mission_name", FALSE);
    }
    if (g_key_file_has_key(keyfile, "settings", "mission_name", NULL) == FALSE) {
        g_key_file_set_string(keyfile, "settings", "mission_name", "Pipeline");
    }
    if (g_key_file_has_key(keyfile, "settings", "refresh_interval", NULL) == FALSE) {
        g_key_file_set_integer(keyfile, "settings", "refresh_interval", 60);
    }
    if (g_key_file_has_key(keyfile, "settings", "command", NULL) == FALSE) {
        g_key_file_set_string(keyfile, "settings", "command", "");
    }

    gchar *config_dir = g_path_get_dirname(config_path);
    g_mkdir_with_parents(config_dir, 0755);
    g_free(config_dir);

    g_key_file_save_to_file(keyfile, config_path, NULL);

    g_free(config_path);
    g_key_file_free(keyfile);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    notify_init("285AApanel");

    // Create a horizontal box
    data->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_add_events(data->box, GDK_BUTTON_PRESS_MASK);

    // Create label
    data->label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(data->label), "<span foreground='#008000'>0|0|0</span>");

    // Pack label into the box
    gtk_box_pack_start(GTK_BOX(data->box), data->label, TRUE, TRUE, 0);

    // Add the box to the plugin
    gtk_container_add(GTK_CONTAINER(plugin), data->box);

    // Add action widget
    xfce_panel_plugin_add_action_widget(plugin, data->box);

    // Connect click signal to plugin widget
    GtkWidget *plugin_widget = GTK_WIDGET(plugin);
    gtk_widget_add_events(plugin_widget, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(plugin_widget, "button-press-event", G_CALLBACK(on_click), data);

    // Add menu items
    xfce_panel_plugin_menu_show_configure(plugin);
    xfce_panel_plugin_menu_show_about(plugin);

    // Connect signals
    g_signal_connect(plugin, "configure-plugin", G_CALLBACK(show_properties), data);
    g_signal_connect(plugin, "about", G_CALLBACK(show_about), data);
    g_signal_connect(plugin, "free-data", G_CALLBACK(free_data), data);

    // Connect visibility notify event
    gtk_widget_add_events(plugin_widget, GDK_VISIBILITY_NOTIFY_MASK);
    g_signal_connect(plugin_widget, "visibility-notify-event", G_CALLBACK(on_visibility_notify), data);

    // Log map/unmap events for box to detect disappearing widget
    g_signal_connect(data->box, "map-event", G_CALLBACK(on_map_event), data);
    g_signal_connect(data->box, "unmap-event", G_CALLBACK(on_unmap_event), data);

    // curl_global_init was already called above

    // Initial update
    write_log("construct: calling initial update_data");
    update_data(data);

    // Update every refresh_interval seconds
    data->timeout_id = g_timeout_add_seconds(data->refresh_interval, (GSourceFunc)update_data, data);

    // Show all widgets
    gtk_widget_show_all(data->box);
    write_log("construct: shown box=%p label=%p plugin_widget=%p", data->box, data->label, plugin_widget);
}

static void free_data(XfcePanelPlugin *plugin, PluginData *data) {
    write_log("free_data: enter data=%p plugin=%p", data, plugin);
    if (!data) {
        write_log("free_data: data is NULL");
        return;
    }
    PluginData *stored = g_object_get_data(G_OBJECT(plugin), "285AApanel-data");
    if (stored != data) {
        write_log("free_data: stored pointer %p does not match provided %p, skipping free", stored, data);
        return;
    }
    if (data->timeout_id) {
        g_source_remove(data->timeout_id);
        data->timeout_id = 0;
    }

    /* Prevent other code from seeing this PluginData via the plugin
     * object while we wait for any background fetch to finish. */
    g_object_set_data(G_OBJECT(plugin), "285AApanel-data", NULL);

    /* Wait briefly for any active background fetch to complete to avoid
     * use-after-free in worker threads that hold a pointer to `data`.
     * If it doesn't finish within the timeout, proceed with freeing
     * to avoid hanging shutdown. */
    int waited_ms = 0;
    while (data->fetching && waited_ms < 5000) {
        write_log("free_data: waiting for fetch to finish (waited %d ms)", waited_ms);
        g_usleep(100 * 1000); /* 100 ms */
        waited_ms += 100;
    }
    if (data->fetching) {
        write_log("free_data: fetch still running after %d ms, proceeding to free", waited_ms);
    }

    if (data->command) {
        g_free(data->command);
        data->command = NULL;
    }
    if (data->mission_name) {
        g_free(data->mission_name);
        data->mission_name = NULL;
    }

    write_log("free_data: fetching=%d (skipping global cleanup)", data->fetching);
    g_free(data);
    write_log("free_data: freed data");
}

static void update_labels(PluginData *data) {
    char buf[128];
    if (!data) return;
    if (!data->label) return;
    sprintf(buf, "<span foreground='#008000'>%d|%d|%d</span>", data->online_users, data->in_game, data->servers);
    write_log("update_labels: setting markup='%s'", buf);
    gtk_label_set_markup(GTK_LABEL(data->label), buf);
}

static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t realsize = size * nmemb;
    GString *data = (GString *)userdata;
    g_string_append_len(data, (const gchar *)ptr, realsize);
    return realsize;
}

static int fetch_online_users(void) {
    /* Lightweight, thread-safe parser: avoid json-glib here to prevent
     * potential double-free issues inside the JSON library when called
     * from background threads. We'll fetch the payload and then scan
     * for the "online_users" object and count its top-level keys. */
    int online_count = 0;
    GString *auth_data = g_string_new(NULL);
    if (!auth_data) return 0;

    CURL *curl2 = curl_easy_init();
    if (!curl2) {
        g_warning("Failed to initialize curl for auth data");
        g_string_free(auth_data, TRUE);
        return 0;
    }

    curl_easy_setopt(curl2, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl2, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl2, CURLOPT_URL, "https://auth.aa2reborn.com/api/auth/ping/?format=json");
    curl_easy_setopt(curl2, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl2, CURLOPT_WRITEDATA, auth_data);
    curl_easy_setopt(curl2, CURLOPT_TIMEOUT, 10L);

    CURLcode res2 = curl_easy_perform(curl2);
    curl_easy_cleanup(curl2);

    if (res2 != CURLE_OK) {
        g_warning("Failed to fetch auth data: %s", curl_easy_strerror(res2));
        g_string_free(auth_data, TRUE);
        return 0;
    }

    const char *s = auth_data->str ? auth_data->str : "";
    /* Find "online_users" key */
    const char *p = strstr(s, "\"online_users\"");
    if (p) {
        /* move to the colon after the key */
        p = strchr(p, ':');
        if (p) {
            /* skip whitespace */
            p++;
            while (*p && g_ascii_isspace(*p)) p++;
            if (*p == '{') {
                /* we're in an object; count top-level string keys (like "29":) */
                p++; /* enter object */
                gboolean in_string = FALSE;
                gboolean esc = FALSE;
                GString *key = g_string_new(NULL);
                int depth = 1;
                while (*p && depth > 0) {
                    if (in_string) {
                        if (!esc && *p == '\\') esc = TRUE;
                        else if (!esc && *p == '"') {
                            in_string = FALSE;
                        } else {
                            if (esc) esc = FALSE;
                        }
                    } else {
                        if (*p == '"') {
                            /* start of a key or string */
                            const char *q = p + 1;
                            /* capture until next unescaped quote */
                            gboolean qesc = FALSE;
                            gboolean has_digits = FALSE;
                            while (*q) {
                                if (!qesc && *q == '\\') qesc = TRUE;
                                else if (!qesc && *q == '"') break;
                                else {
                                    if (g_ascii_isdigit(*q)) has_digits = TRUE;
                                    qesc = FALSE;
                                }
                                q++;
                            }
                            /* if the following non-space char after the closing quote is ':' then it's a key
                             * only count it when we're at the top-level (depth == 1) of the `online_users` object */
                            const char *after = q + 1;
                            while (*after && g_ascii_isspace(*after)) after++;
                            if (*after == ':' && depth == 1) {
                                /* consider it a top-level member key */
                                online_count++;
                            }
                            p = q; /* advance to closing quote */
                        } else if (*p == '{') {
                            depth++;
                        } else if (*p == '}') {
                            depth--;
                        }
                    }
                    p++;
                }
                g_string_free(key, TRUE);
            } else if (*p == '[') {
                /* array: count commas + 1 */
                p++;
                int items = 0;
                gboolean in_string = FALSE;
                gboolean esc = FALSE;
                int depth = 1;
                while (*p && depth > 0) {
                    if (in_string) {
                        if (!esc && *p == '\\') esc = TRUE;
                        else if (!esc && *p == '"') in_string = FALSE;
                        else esc = FALSE;
                    } else {
                        if (*p == '"') in_string = TRUE;
                        else if (*p == '[') depth++;
                        else if (*p == ']') depth--;
                        else if (*p == ',') items++;
                    }
                    p++;
                }
                if (items >= 0) online_count = items + 1;
            } else {
                /* try numeric */
                while (*p && !g_ascii_isdigit(*p) && *p != '-') p++;
                if (*p) online_count = atoi(p);
            }
        }
    }

    write_log("fetch_online_users: online_count=%d", online_count);
    g_string_free(auth_data, TRUE);
    return online_count;
}

typedef struct {
    int in_game;
    int servers;
    int online_users;
    char *tooltip_markup;
    gboolean network_error;
    gboolean success;
} FetchResult;

typedef struct {
    XfcePanelPlugin *plugin;
    PluginData *expected;
    FetchResult *result;
} IdleApplyData;

static gboolean apply_fetch_result(gpointer user_data) {
    IdleApplyData *iad = (IdleApplyData *)user_data;
    FetchResult *r = iad->result;
    XfcePanelPlugin *plugin = iad->plugin;
    PluginData *expected = iad->expected;
    /* Verify plugin still holds the same PluginData pointer before dereferencing */
    PluginData *stored = g_object_get_data(G_OBJECT(plugin), "285AApanel-data");
    write_log("apply_fetch_result: enter expected=%p plugin=%p stored=%p network_error=%d success=%d", expected, plugin, stored, r->network_error, r->success);
    if (stored != expected) {
        write_log("apply_fetch_result: plugin data mismatch or freed (stored=%p expected=%p)", stored, expected);
        if (r->tooltip_markup) g_free(r->tooltip_markup);
        g_free(r);
        g_free(iad);
        return FALSE;
    }
    PluginData *data = stored;

    if (r->network_error) {
        data->in_game = 0;
        data->servers = 0;
        data->online_users = 0;
        update_labels(data);
        gtk_widget_set_tooltip_text(data->box, "No server data (network error)");
    } else if (r->success) {
        data->in_game = r->in_game;
        data->servers = r->servers;
        data->online_users = r->online_users;
        if (r->tooltip_markup) gtk_widget_set_tooltip_markup(data->box, r->tooltip_markup);
        update_labels(data);
        write_log("apply_fetch_result: applied in_game=%d servers=%d online=%d", data->in_game, data->servers, data->online_users);
    } else {
        // parsing error: keep previous values
        write_log("apply_fetch_result: parse error, keeping previous values");
    }

    if (r->tooltip_markup) g_free(r->tooltip_markup);
    g_free(r);
    g_free(iad);
    data->fetching = FALSE;
    return FALSE; // remove source
}

static gpointer fetch_thread_func(gpointer user_data) {
    PluginData *data = (PluginData *)user_data;
    FetchResult *r = g_new0(FetchResult, 1);
    r->success = FALSE;
    r->network_error = FALSE;

    // Fetch server data (similar to previous synchronous code), but fill local vars
    GString *http_data = g_string_new(NULL);
    CURL *curl = curl_easy_init();
    if (!curl) {
        r->network_error = TRUE;
        goto done;
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_URL, "https://auth.aa2reborn.com/api/servers/query-all/?format=json");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, http_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        r->network_error = TRUE;
        g_string_free(http_data, TRUE);
        goto done;
    }

    GError *error = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, http_data->str, -1, &error)) {
        write_log("fetch_thread: failed parse: %s", error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_string_free(http_data, TRUE);
        g_object_unref(parser);
        goto done;
    }
    g_string_free(http_data, TRUE);

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        goto done;
    }
    JsonObject *root_obj = json_node_get_object(root);
    if (!json_object_has_member(root_obj, "servers")) {
        g_object_unref(parser);
        goto done;
    }

    JsonArray *servers_array = json_object_get_array_member(root_obj, "servers");
    if (!servers_array) {
        g_object_unref(parser);
        goto done;
    }

    int total_players = 0;
    int valid_servers = 0;
    GString *tooltip = g_string_new("<tt><span foreground='#008000'>");
    guint n_servers = json_array_get_length(servers_array);
    int cnt = 0;
    for (guint i = 0; i < n_servers; i++) {
        JsonObject *server_obj = json_array_get_object_element(servers_array, i);
        if (!json_object_has_member(server_obj, "query_result")) continue;
        JsonObject *query_result = json_object_get_object_member(server_obj, "query_result");
        if (!query_result || !json_object_has_member(query_result, "success")) continue;
        if (!json_object_get_boolean_member(query_result, "success")) continue;
        if (!json_object_has_member(query_result, "server_info")) continue;
        JsonObject *server_info = json_object_get_object_member(query_result, "server_info");
        if (!server_info) continue;
        int numplayers = json_object_has_member(server_info, "numplayers") ? json_object_get_int_member(server_info, "numplayers") : 0;
        total_players += numplayers;

        const gchar *hostname = json_object_has_member(server_info, "hostname") ? json_object_get_string_member(server_info, "hostname") : NULL;
        const gchar *mapname = json_object_has_member(server_info, "mapname") ? json_object_get_string_member(server_info, "mapname") : NULL;
        if (cnt < 3) {
            g_string_append_printf(tooltip, "%-35.35s | %-15.15s | %5d", hostname ? hostname : "Unknown", mapname ? mapname : "Unknown", numplayers);
            if (cnt < 2) g_string_append(tooltip, "\n");
        }
        cnt++;
        valid_servers++;
    }
    g_string_append(tooltip, "</span></tt>");

    r->in_game = total_players;
    r->servers = valid_servers;
    r->tooltip_markup = g_string_free(tooltip, FALSE);
    r->online_users = fetch_online_users();
    r->success = TRUE;

    g_object_unref(parser);
    write_log("fetch_thread: result success=%d network_error=%d in_game=%d servers=%d online=%d", r->success, r->network_error, r->in_game, r->servers, r->online_users);
done:
    // schedule apply on main thread
    IdleApplyData *iad = g_new0(IdleApplyData, 1);
    iad->plugin = data->plugin;
    iad->expected = data;
    iad->result = r;
    guint idle_id = g_idle_add(apply_fetch_result, iad);
    write_log("fetch_thread: g_idle_add returned id=%u for pdata=%p", idle_id, data);
    return NULL;
}

static gboolean update_data(PluginData *data) {
    if (!data) return TRUE;
    if (data->fetching) return TRUE; // already fetching
    data->fetching = TRUE;
    // Spawn background thread to fetch data without blocking UI
    g_thread_new("285AA-fetch", fetch_thread_func, data);
    return TRUE;
}

XFCE_PANEL_PLUGIN_REGISTER(construct);

static gboolean on_visibility_notify(GtkWidget *widget, GdkEventVisibility *event, PluginData *data) {
    // This function is called when the widget becomes visible
    // You can add code here to handle visibility changes
    return FALSE;
}

static gboolean on_map_event(GtkWidget *widget, GdkEvent *event, PluginData *data) {
    write_log("on_map_event: widget=%p data=%p", widget, data);
    return FALSE; // let other handlers run
}

static gboolean on_unmap_event(GtkWidget *widget, GdkEvent *event, PluginData *data) {
    write_log("on_unmap_event: widget=%p data=%p", widget, data);
    return FALSE;
}