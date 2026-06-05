#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget *box;
    GtkWidget *label1;
    GtkWidget *label2;
} MyPlugin;

static void
my_plugin_construct (XfcePanelPlugin *plugin)
{
    MyPlugin *my_plugin = g_new (MyPlugin, 1);
    my_plugin->plugin = plugin;

    my_plugin->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_add (GTK_CONTAINER (plugin), my_plugin->box);

    my_plugin->label1 = gtk_label_new ("123");
    my_plugin->label2 = gtk_label_new ("456");

    gtk_box_pack_start (GTK_BOX (my_plugin->box), my_plugin->label1, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (my_plugin->box), my_plugin->label2, TRUE, TRUE, 0);

    gtk_widget_show_all (my_plugin->box);

    xfce_panel_plugin_set_tooltip (plugin, "This plugin displays two numerical values");

    g_object_set_data (G_OBJECT (plugin), "my-plugin", my_plugin);
}

XFCE_PANEL_PLUGIN_REGISTER (my_plugin_construct);