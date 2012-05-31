#include <config.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixoutputstream.h>

#include <stdlib.h>
#include <gtk/gtk.h>

#include <nm-client.h>
#include <nm-device-wifi.h>
#include <nm-access-point.h>
#include <nm-utils.h>
#include <nm-remote-settings.h>

#include "panel-cell-renderer-signal.h"
#include "panel-cell-renderer-mode.h"
#include "panel-cell-renderer-security.h"

#include <act/act-user-manager.h>

#include "cc-timezone-map.h"
#include "timedated.h"
#include "um-utils.h"
#include "um-photo-dialog.h"
#include "pw-utils.h"
#include "gdm-greeter-client.h"

#define GWEATHER_I_KNOW_THIS_IS_UNSTABLE
#include <libgweather/location-entry.h>

#define GOA_API_IS_SUBJECT_TO_CHANGE
#include <goa/goa.h>
#define GOA_BACKEND_API_IS_SUBJECT_TO_CHANGE
#include <goabackend/goabackend.h>

#ifdef HAVE_CHEESE
#include <cheese-gtk.h>
#endif

#include <geoclue/geoclue-master.h>
#include <geoclue/geoclue-position.h>

#include <gnome-keyring.h>

#define DEFAULT_TZ "Europe/London"

/* Setup data {{{1 */
typedef struct {
        GtkBuilder *builder;
        GKeyFile *overrides;
        GtkAssistant *assistant;
        GdmGreeterClient *greeter_client;

        /* network data */
        NMClient *nm_client;
        NMRemoteSettings *nm_settings;
        NMDevice *nm_device;
        GtkListStore *ap_list;
        gboolean refreshing;

        GtkTreeRowReference *row;
        guint pulse;

        /* account data */
        ActUserManager *act_client;
        ActUser *act_user;

        gboolean valid_name;
        gboolean valid_username;
        gboolean valid_password;
        const gchar *password_reason;
        ActUserPasswordMode password_mode;
        ActUserAccountType account_type;

        gboolean user_data_unsaved;

        GtkWidget *photo_dialog;
        GdkPixbuf *avatar_pixbuf;
        gchar *avatar_filename;

        /* location data */
        CcTimezoneMap *map;
        TzLocation *current_location;
        Timedate1 *dtm;

        /* online data */
        GoaClient *goa_client;
} SetupData;

#define OBJ(type,name) ((type)gtk_builder_get_object(setup->builder,(name)))
#define WID(name) OBJ(GtkWidget*,name)

/* Welcome page {{{1 */

static void
prepare_welcome_page (SetupData *setup)
{
        gchar *s;

        s = g_key_file_get_locale_string (setup->overrides,
                                          "Welcome", "welcome-image",
                                          NULL, NULL);

        if (s && g_file_test (s, G_FILE_TEST_EXISTS))
                gtk_image_set_from_file (GTK_IMAGE (WID ("welcome-image")), s);

        g_free (s);

        s = g_key_file_get_locale_string (setup->overrides,
                                          "Welcome", "welcome-title",
                                          NULL, NULL);
        if (s)
                gtk_label_set_text (GTK_LABEL (WID ("welcome-title")), s);
        g_free (s);

        s = g_key_file_get_locale_string (setup->overrides,
                                          "Welcome", "welcome-subtitle",
                                          NULL, NULL);
        if (s)
                gtk_label_set_text (GTK_LABEL (WID ("welcome-subtitle")), s);
        g_free (s);
}

/* heavily lifted from g_output_stream_splice */
static void
splice_buffer (GInputStream  *stream,
               GtkTextBuffer *buffer,
               GError       **error)
{
        char contents[8192];
        gssize n_read;
        GtkTextIter iter;

        while (TRUE) {
                n_read = g_input_stream_read (stream, contents, sizeof (contents), NULL, error);

                /* error or eof */
                if (n_read <= 0)
                        break;

                gtk_text_buffer_get_end_iter (buffer, &iter);
                gtk_text_buffer_insert (buffer, &iter, contents, n_read);
        }
}

static GtkWidget *
build_eula_text_view (GFile *eula,
                      char **title_out)
{
        GInputStream *input_stream = NULL;
        GDataInputStream *data_input_stream = NULL;
        GError *error = NULL;
        GtkWidget *widget = NULL;
        GtkTextBuffer *buffer;

        input_stream = G_INPUT_STREAM (g_file_read (eula, NULL, &error));
        if (error != NULL) {
                g_printerr (error->message);
                goto out;
        }

        data_input_stream = g_data_input_stream_new (input_stream);
        g_object_unref (input_stream);

        *title_out = g_data_input_stream_read_line (data_input_stream,
                                                    NULL, NULL, &error);
        if (error != NULL) {
                g_printerr (error->message);
                goto out;
        }

        buffer = gtk_text_buffer_new (NULL);
        splice_buffer (G_INPUT_STREAM (data_input_stream), buffer, &error);
        if (error != NULL) {
                g_printerr (error->message);
                goto out;
        }

        widget = gtk_text_view_new_with_buffer (buffer);

 out:
        g_clear_object (&data_input_stream);
        g_clear_error (&error);
        return widget;
}

static void
build_eula_page (SetupData *setup,
                 GFile     *eula)
{
        GtkWidget *text_view;
        GtkWidget *vbox;
        GtkWidget *scrolled_window;
        GtkWidget *checkbox;
        char *title;

        text_view = build_eula_text_view (eula, &title);
        if (text_view == NULL)
                return;

        scrolled_window = gtk_scrolled_window_new (NULL, NULL);
        gtk_container_add (GTK_CONTAINER (scrolled_window), text_view);

        checkbox = gtk_check_button_new_with_mnemonic (_("I have _agreed to the "
                                                         "terms and conditions in "
                                                         "this end user license "
                                                         "agreement."));

        vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_add (GTK_CONTAINER (vbox), scrolled_window);
        gtk_container_add (GTK_CONTAINER (vbox), checkbox);

        /* XXX: 1 is the location after the welcome page.
         * Remove this hardcoded thing. */
        gtk_assistant_insert_page (setup->assistant, vbox, 1);
        gtk_assistant_set_page_title (setup->assistant, vbox, title);

        gtk_widget_show_all (GTK_WIDGET (vbox));
}

static void
prepare_eula_pages (SetupData *setup)
{
        gchar *eulas_dir_path;
        GFile *eulas_dir;
        GError *error = NULL;
        GFileEnumerator *enumerator = NULL;
        GFileInfo *info;

        eulas_dir_path = g_build_filename (UIDIR, "eulas", NULL);
        eulas_dir = g_file_new_for_path (eulas_dir_path);
        g_free (eulas_dir_path);

        if (!g_file_query_exists (eulas_dir, NULL)) {
                goto out;
        }

        enumerator = g_file_enumerate_children (eulas_dir,
                                                G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                G_FILE_QUERY_INFO_NONE,
                                                NULL,
                                                &error);

        if (error != NULL) {
                g_printerr (error->message);
                goto out;
        }

        while ((info = g_file_enumerator_next_file (enumerator, NULL, &error)) != NULL) {
                GFile *eula = g_file_get_child (eulas_dir, g_file_info_get_name (info));
                build_eula_page (setup, eula);
        }

        if (error != NULL) {
                g_printerr (error->message);
                goto out;
        }

 out:
        g_clear_error (&error);
        g_object_unref (eulas_dir);
        g_clear_object (&enumerator);
}

/* Network page {{{1 */

enum {
        PANEL_WIRELESS_COLUMN_ID,
        PANEL_WIRELESS_COLUMN_TITLE,
        PANEL_WIRELESS_COLUMN_SORT,
        PANEL_WIRELESS_COLUMN_STRENGTH,
        PANEL_WIRELESS_COLUMN_MODE,
        PANEL_WIRELESS_COLUMN_SECURITY,
        PANEL_WIRELESS_COLUMN_ACTIVATING,
        PANEL_WIRELESS_COLUMN_ACTIVE,
        PANEL_WIRELESS_COLUMN_PULSE
};

static gint
wireless_sort_cb (GtkTreeModel *model,
                  GtkTreeIter  *a,
                  GtkTreeIter  *b,
                  gpointer      user_data)
{
        gchar *str_a;
        gchar *str_b;
        gint retval;

        gtk_tree_model_get (model, a, PANEL_WIRELESS_COLUMN_SORT, &str_a, -1);
        gtk_tree_model_get (model, b, PANEL_WIRELESS_COLUMN_SORT, &str_b, -1);

        /* special case blank entries to the bottom */
        if (g_strcmp0 (str_a, "") == 0) {
                retval = 1;
                goto out;
        }
        if (g_strcmp0 (str_b, "") == 0) {
                retval = -1;
                goto out;
        }

        retval = g_strcmp0 (str_a, str_b);
out:
        g_free (str_a);
        g_free (str_b);

        return retval;
}

static GPtrArray *
get_strongest_unique_aps (const GPtrArray *aps)
{
        const GByteArray *ssid;
        const GByteArray *ssid_tmp;
        GPtrArray *unique = NULL;
        gboolean add_ap;
        guint i;
        guint j;
        NMAccessPoint *ap;
        NMAccessPoint *ap_tmp;

        /* we will have multiple entries for typical hotspots,
        * just keep the one with the strongest signal
        */
        unique = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
        if (aps == NULL)
                goto out;

        for (i = 0; i < aps->len; i++) {
                ap = NM_ACCESS_POINT (g_ptr_array_index (aps, i));
                ssid = nm_access_point_get_ssid (ap);
                add_ap = TRUE;

                /* get already added list */
                for (j = 0; j < unique->len; j++) {
                        ap_tmp = NM_ACCESS_POINT (g_ptr_array_index (unique, j));
                        ssid_tmp = nm_access_point_get_ssid (ap_tmp);

                        /* is this the same type and data? */
                        if (nm_utils_same_ssid (ssid, ssid_tmp, TRUE)) {
                                /* the new access point is stronger */
                                if (nm_access_point_get_strength (ap) >
                                    nm_access_point_get_strength (ap_tmp)) {
                                        g_ptr_array_remove (unique, ap_tmp);
                                        add_ap = TRUE;
                                } else {
                                        add_ap = FALSE;
                                }
                                break;
                        }
                }
                if (add_ap) {
                        g_ptr_array_add (unique, g_object_ref (ap));
                }
        }

out:
        return unique;
}

static guint
get_access_point_security (NMAccessPoint *ap)
{
        NM80211ApFlags flags;
        NM80211ApSecurityFlags wpa_flags;
        NM80211ApSecurityFlags rsn_flags;
        guint type;

        flags = nm_access_point_get_flags (ap);
        wpa_flags = nm_access_point_get_wpa_flags (ap);
        rsn_flags = nm_access_point_get_rsn_flags (ap);

        if (!(flags & NM_802_11_AP_FLAGS_PRIVACY) &&
            wpa_flags == NM_802_11_AP_SEC_NONE &&
            rsn_flags == NM_802_11_AP_SEC_NONE)
                type = NM_AP_SEC_NONE;
        else if ((flags & NM_802_11_AP_FLAGS_PRIVACY) &&
                 wpa_flags == NM_802_11_AP_SEC_NONE &&
                 rsn_flags == NM_802_11_AP_SEC_NONE)
                type = NM_AP_SEC_WEP;
        else if (!(flags & NM_802_11_AP_FLAGS_PRIVACY) &&
                 wpa_flags != NM_802_11_AP_SEC_NONE &&
                 rsn_flags != NM_802_11_AP_SEC_NONE)
                type = NM_AP_SEC_WPA;
        else
                type = NM_AP_SEC_WPA2;

        return type;
}

static void
add_access_point (SetupData *setup, NMAccessPoint *ap, NMAccessPoint *active)
{
        const GByteArray *ssid;
        const gchar *ssid_text;
        const gchar *object_path;
        GtkTreeIter iter;
        gboolean activated, activating;

        ssid = nm_access_point_get_ssid (ap);
        object_path = nm_object_get_path (NM_OBJECT (ap));

        if (ssid == NULL)
                return;
        ssid_text = nm_utils_escape_ssid (ssid->data, ssid->len);

        if (active &&
            nm_utils_same_ssid (ssid, nm_access_point_get_ssid (active), TRUE)) {
                switch (nm_device_get_state (setup->nm_device))
                {
                case NM_DEVICE_STATE_PREPARE:
                case NM_DEVICE_STATE_CONFIG:
                case NM_DEVICE_STATE_NEED_AUTH:
                case NM_DEVICE_STATE_IP_CONFIG:
                case NM_DEVICE_STATE_SECONDARIES:
                        activated = FALSE;
                        activating = TRUE;
                        break;
                case NM_DEVICE_STATE_ACTIVATED:
                        activated = TRUE;
                        activating = FALSE;
                        break;
                default:
                        activated = FALSE;
                        activating = FALSE;
                        break;
                }
        } else {
                activated = FALSE;
                activating = FALSE;
        }

        gtk_list_store_append (setup->ap_list, &iter);
        gtk_list_store_set (setup->ap_list, &iter,
                            PANEL_WIRELESS_COLUMN_ID, object_path,
                            PANEL_WIRELESS_COLUMN_TITLE, ssid_text,
                            PANEL_WIRELESS_COLUMN_SORT, ssid_text,
                            PANEL_WIRELESS_COLUMN_STRENGTH, nm_access_point_get_strength (ap),
                            PANEL_WIRELESS_COLUMN_MODE, nm_access_point_get_mode (ap),
                            PANEL_WIRELESS_COLUMN_SECURITY, get_access_point_security (ap),
                            PANEL_WIRELESS_COLUMN_ACTIVATING, activating,
                            PANEL_WIRELESS_COLUMN_ACTIVE, activated,
                            PANEL_WIRELESS_COLUMN_PULSE, setup->pulse,
                            -1);
        if (activating) {
                GtkTreePath *path;
                GtkTreeModel *model;

                model = (GtkTreeModel*)setup->ap_list;
                path = gtk_tree_model_get_path (model, &iter);
                setup->row = gtk_tree_row_reference_new (model, path);
                gtk_tree_path_free (path);
        }

}

static void
add_access_point_other (SetupData *setup)
{
        GtkTreeIter iter;

        gtk_list_store_append (setup->ap_list, &iter);
        gtk_list_store_set (setup->ap_list, &iter,
                            PANEL_WIRELESS_COLUMN_ID, "ap-other...",
                            /* TRANSLATORS: this is when the access point is not listed
 *                           * in the dropdown (or hidden) and the user has to select
 *                           * another entry manually */

                            PANEL_WIRELESS_COLUMN_TITLE, C_("Wireless access point", "Other..."),
                            /* always last */
                            PANEL_WIRELESS_COLUMN_SORT, "",
                            PANEL_WIRELESS_COLUMN_STRENGTH, 0,
                            PANEL_WIRELESS_COLUMN_MODE, NM_802_11_MODE_UNKNOWN,
                            PANEL_WIRELESS_COLUMN_SECURITY, NM_AP_SEC_UNKNOWN,
                            PANEL_WIRELESS_COLUMN_ACTIVATING, FALSE,
                            PANEL_WIRELESS_COLUMN_ACTIVE, FALSE,
                            PANEL_WIRELESS_COLUMN_PULSE, setup->pulse,
                            -1);
}

static void
select_and_scroll_to_ap (SetupData *setup, NMAccessPoint *ap)
{
        GtkTreeModel *model;
        GtkTreeView *tv;
        GtkTreeViewColumn *col;
        GtkTreeSelection *selection;
        GtkTreeIter iter;
        GtkTreePath *path;
        gchar *ssid_target;
        const GByteArray *ssid;
        const gchar *ssid_text;

        model = (GtkTreeModel *)setup->ap_list;

        if (!gtk_tree_model_get_iter_first (model, &iter))
                return;

        tv = OBJ(GtkTreeView*, "network-list");
        col = OBJ(GtkTreeViewColumn*, "network-list-column");
        selection = OBJ(GtkTreeSelection*, "network-list-selection");

        ssid = nm_access_point_get_ssid (ap);
        ssid_text = nm_utils_escape_ssid (ssid->data, ssid->len);

        do {
                gtk_tree_model_get (model, &iter,
                                    PANEL_WIRELESS_COLUMN_TITLE, &ssid_target,
                                    -1);
                if (g_strcmp0 (ssid_target, ssid_text) == 0) {
                        g_free (ssid_target);
                        gtk_tree_selection_select_iter (selection, &iter);
                        path = gtk_tree_model_get_path (model, &iter);
                        gtk_tree_view_scroll_to_cell (tv, path, col, FALSE, 0, 0);
                        gtk_tree_path_free (path);
                        break;
                }
                g_free (ssid_target);

        } while (gtk_tree_model_iter_next (model, &iter));
}

static void refresh_wireless_list (SetupData *setup);

static gboolean
refresh_again (gpointer data)
{
        SetupData *setup = data;

        refresh_wireless_list (setup);

        return FALSE;
}

static void
refresh_wireless_list (SetupData *setup)
{
        NMDeviceState state;
        NMAccessPoint *active_ap;
        NMAccessPoint *ap;
        const GPtrArray *aps;
        GPtrArray *unique_aps;
        guint i;
        GtkWidget *label;
        GtkWidget *spinner;
        GtkWidget *swin;

        setup->refreshing = TRUE;

        state = nm_device_get_state (setup->nm_device);

        active_ap = nm_device_wifi_get_active_access_point (NM_DEVICE_WIFI (setup->nm_device));

        g_object_ref (setup->ap_list);
        gtk_tree_view_set_model (OBJ(GtkTreeView*, "network-list"), NULL);
        gtk_list_store_clear (setup->ap_list);
        if (setup->row) {
                gtk_tree_row_reference_free (setup->row);
                setup->row = NULL;
        }

        aps = nm_device_wifi_get_access_points (NM_DEVICE_WIFI (setup->nm_device));

        swin = WID("network-scrolledwindow");
        label = WID("no-network-label");
        spinner = WID("no-network-spinner");

        if (state == NM_DEVICE_STATE_UNMANAGED ||
            state == NM_DEVICE_STATE_UNAVAILABLE) {
                if (nm_client_get_state (setup->nm_client) == NM_STATE_CONNECTED_GLOBAL)
                        gtk_label_set_text (GTK_LABEL (label), _("Wireless network is not available, but we are connected anyway."));
                else
                        gtk_label_set_text (GTK_LABEL (label), _("Network is not available, make sure to turn airplane mode off."));
                gtk_widget_hide (swin);
                gtk_widget_hide (spinner);
                gtk_widget_show (label);
                goto out;
        }
        else if (aps == NULL || aps->len == 0) {
                gtk_label_set_text (GTK_LABEL (label), _("Checking for available wireless networks"));
                gtk_widget_hide (swin);
                gtk_widget_show (spinner);
                gtk_widget_show (label);
                g_timeout_add_seconds (1, refresh_again, setup);

                goto out;
        }
        else {
                gtk_widget_show (swin);
                gtk_widget_hide (spinner);
                gtk_widget_hide (label);
        }

        unique_aps = get_strongest_unique_aps (aps);
        for (i = 0; i < unique_aps->len; i++) {
                ap = NM_ACCESS_POINT (g_ptr_array_index (unique_aps, i));
                add_access_point (setup, ap, active_ap);
        }
        g_ptr_array_unref (unique_aps);
        add_access_point_other (setup);

out:
        gtk_tree_view_set_model (OBJ(GtkTreeView*, "network-list"), (GtkTreeModel*)setup->ap_list);
        g_object_unref (setup->ap_list);

        if (active_ap)
                select_and_scroll_to_ap (setup, active_ap);

        setup->refreshing = FALSE;
}

static void
device_state_changed (NMDevice *device, GParamSpec *pspec, SetupData *setup)
{
        refresh_wireless_list (setup);
}

static void
connection_activate_cb (NMClient *client,
                        NMActiveConnection *connection,
                        GError *error,
                        gpointer user_data)
{
        SetupData *setup = user_data;

        if (connection == NULL) {
                /* failed to activate */
                refresh_wireless_list (setup);
        }
}

static void
connection_add_activate_cb (NMClient *client,
                            NMActiveConnection *connection,
                            const char *path,
                            GError *error,
                            gpointer user_data)
{
        connection_activate_cb (client, connection, error, user_data);
}

static void
connect_to_hidden_network_cb (GObject *source_object, GAsyncResult *res, gpointer data)
{
        SetupData *setup = data;
        GError *error = NULL;
        GVariant *result = NULL;

        result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &error);
        if (result == NULL) {
                g_warning ("failed to connect to hidden network: %s",
                           error->message);
                g_error_free (error);
        }

        refresh_wireless_list (setup);
}

static void
connect_to_hidden_network (SetupData *setup)
{
        GDBusProxy *proxy;
        GVariant *res = NULL;
        GError *error = NULL;

        /* connect to NM applet */
        proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                               G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                                               G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                               NULL,
                                               "org.gnome.network_manager_applet",
                                               "/org/gnome/network_manager_applet",
                                               "org.gnome.network_manager_applet",
                                               NULL,
                                               &error);
        if (proxy == NULL) {
                g_warning ("failed to connect to NM applet: %s",
                           error->message);
                g_error_free (error);
                goto out;
        }

        /* try to show the hidden network UI */
        g_dbus_proxy_call (proxy,
                           "ConnectToHiddenNetwork",
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           5000, /* don't wait forever */
                           NULL,
                           connect_to_hidden_network_cb,
                           setup);

out:
        if (proxy != NULL)
                g_object_unref (proxy);
        if (res != NULL)
                g_variant_unref (res);
}

static void
wireless_selection_changed (GtkTreeSelection *selection, SetupData *setup)
{
        GtkTreeModel *model;
        GtkTreeIter iter;
        gchar *object_path;
        gchar *ssid_target;
        GSList *list, *filtered, *l;
        NMConnection *connection;
        NMConnection *connection_to_activate;
        NMSettingWireless *setting;
        const GByteArray *ssid;
        const gchar *ssid_tmp;

        if (setup->refreshing)
                return;

        if (!gtk_tree_selection_get_selected (selection, &model, &iter))
                return;

        gtk_tree_model_get (model, &iter,
                            PANEL_WIRELESS_COLUMN_ID, &object_path,
                            PANEL_WIRELESS_COLUMN_TITLE, &ssid_target,
                            -1);

        gtk_list_store_set (setup->ap_list, &iter,
                            PANEL_WIRELESS_COLUMN_ACTIVATING, TRUE,
                            -1);

        if (g_strcmp0 (object_path, "ap-other...") == 0) {
                connect_to_hidden_network (setup);
                goto out;
        }

        list = nm_remote_settings_list_connections (setup->nm_settings);
        filtered = nm_device_filter_connections (setup->nm_device, list);

        connection_to_activate = NULL;

        for (l = filtered; l; l = l->next) {
                connection = NM_CONNECTION (l->data);
                setting = nm_connection_get_setting_wireless (connection);
                if (!NM_IS_SETTING_WIRELESS (setting))
                        continue;

                ssid = nm_setting_wireless_get_ssid (setting);
                if (ssid == NULL)
                        continue;
                ssid_tmp = nm_utils_escape_ssid (ssid->data, ssid->len);
                if (g_strcmp0 (ssid_target, ssid_tmp) == 0) {
                        connection_to_activate = connection;
                        break;
                }
        }
        g_slist_free (list);
        g_slist_free (filtered);

        if (connection_to_activate != NULL) {
                nm_client_activate_connection (setup->nm_client,
                                               connection_to_activate,
                                               setup->nm_device, NULL,
                                               connection_activate_cb, setup);
                goto out;
        }

        nm_client_add_and_activate_connection (setup->nm_client,
                                               NULL,
                                               setup->nm_device, object_path,
                                               connection_add_activate_cb, setup);

out:
        g_free (object_path);
        g_free (ssid_target);

        refresh_wireless_list (setup);
}

static void
connection_state_changed (NMActiveConnection *c, GParamSpec *pspec, SetupData *setup)
{
        refresh_wireless_list (setup);
}

static void
active_connections_changed (NMClient *client, GParamSpec *pspec, SetupData *setup)
{
        const GPtrArray *connections;
        guint i;

        connections = nm_client_get_active_connections (client);
        for (i = 0; connections && (i < connections->len); i++) {
                NMActiveConnection *connection;

                connection = g_ptr_array_index (connections, i);
                if (!g_object_get_data (G_OBJECT (connection), "has-state-changed-handler")) {
                        g_signal_connect (connection, "notify::state",
                                          G_CALLBACK (connection_state_changed), setup);
                        g_object_set_data (G_OBJECT (connection), "has-state-changed-handler", GINT_TO_POINTER (1));
                }
        }

        refresh_wireless_list (setup);
}

static gboolean
bump_pulse (gpointer data)
{
        SetupData *setup = data;
        GtkTreeIter iter;
        GtkTreePath *path;
        GtkTreeModel *model;

        setup->pulse++;

        if (!setup->refreshing &&
            gtk_tree_row_reference_valid (setup->row)) {
                model = (GtkTreeModel *)setup->ap_list;
                path = gtk_tree_row_reference_get_path (setup->row);
                gtk_tree_model_get_iter (model, &iter, path);
                gtk_tree_path_free (path);
                gtk_list_store_set (setup->ap_list, &iter,
                                    PANEL_WIRELESS_COLUMN_PULSE, setup->pulse,
                                    -1);
        }

        return TRUE;
}

static void
prepare_network_page (SetupData *setup)
{
        GtkTreeViewColumn *col;
        GtkCellRenderer *cell;
        GtkTreeSortable *sortable;
        GtkTreeSelection *selection;
        const GPtrArray *devices;
        NMDevice *device;
        guint i;
        DBusGConnection *bus;
        GError *error;

        col = OBJ(GtkTreeViewColumn*, "network-list-column");

        cell = gtk_cell_renderer_text_new ();
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (col), cell, FALSE);
        /* black small diamond */
        g_object_set (cell, "text", "\342\254\251", NULL);
        gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (col), cell,
                                        "visible", PANEL_WIRELESS_COLUMN_ACTIVE,
                                        NULL);
        cell = gtk_cell_renderer_spinner_new ();
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (col), cell, FALSE);
        gtk_cell_area_cell_set (gtk_cell_layout_get_area (GTK_CELL_LAYOUT (col)), cell, "align", FALSE, NULL);
        gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (col), cell,
                                        "active", PANEL_WIRELESS_COLUMN_ACTIVATING,
                                        "visible", PANEL_WIRELESS_COLUMN_ACTIVATING,
                                        "pulse", PANEL_WIRELESS_COLUMN_PULSE,
                                        NULL);
        g_timeout_add (80, bump_pulse, setup);

        cell = gtk_cell_renderer_text_new ();
        g_object_set (cell, "width", 400, "width-chars", 45, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (col), cell, TRUE);
        gtk_cell_area_cell_set (gtk_cell_layout_get_area (GTK_CELL_LAYOUT (col)), cell, "align", TRUE, NULL);
        gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (col), cell,
                                        "text", PANEL_WIRELESS_COLUMN_TITLE,
                                        NULL);

        cell = panel_cell_renderer_mode_new ();
        gtk_cell_renderer_set_padding (cell, 4, 0);
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (col), cell, FALSE);
        gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (col), cell,
                                        "mode", PANEL_WIRELESS_COLUMN_MODE,
                                        NULL);

        cell = panel_cell_renderer_security_new ();
        gtk_cell_renderer_set_padding (cell, 4, 0);
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (col), cell, FALSE);
        gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (col), cell,
                                        "security", PANEL_WIRELESS_COLUMN_SECURITY,
                                        NULL);

        cell = panel_cell_renderer_signal_new ();
        gtk_cell_renderer_set_padding (cell, 4, 0);
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (col), cell, FALSE);
        gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (col), cell,
                                        "signal", PANEL_WIRELESS_COLUMN_STRENGTH,
                                        NULL);

        setup->ap_list = OBJ(GtkListStore *, "liststore-wireless");
        sortable = GTK_TREE_SORTABLE (setup->ap_list);
        gtk_tree_sortable_set_sort_column_id (sortable,
                                              PANEL_WIRELESS_COLUMN_SORT,
                                              GTK_SORT_ASCENDING);
        gtk_tree_sortable_set_sort_func (sortable,
                                         PANEL_WIRELESS_COLUMN_SORT,
                                         wireless_sort_cb,
                                         sortable,
                                         NULL);

        setup->nm_client = nm_client_new ();

        g_signal_connect (setup->nm_client, "notify::active-connections",
                          G_CALLBACK (active_connections_changed), setup);

        devices = nm_client_get_devices (setup->nm_client);
        if (devices) {
                for (i = 0; i < devices->len; i++) {
                        device = g_ptr_array_index (devices, i);

                        if (!nm_device_get_managed (device))
                                continue;

                        if (nm_device_get_device_type (device) == NM_DEVICE_TYPE_WIFI) {
                                /* FIXME deal with multiple, dynamic devices */
                                setup->nm_device = device;
                                g_signal_connect (G_OBJECT (device), "notify::state",
                                                  G_CALLBACK (device_state_changed), setup);
                                break;
                        }
                }
        }

        if (setup->nm_device == NULL) {
                GtkWidget *swin;
                GtkWidget *label;
                GtkWidget *spinner;

                swin = WID("network-scrolledwindow");
                label = WID("no-network-label");
                spinner = WID("no-network-spinner");

                gtk_widget_hide (swin);
                gtk_widget_hide (spinner);

                gtk_label_set_text (GTK_LABEL (label), _("No network devices found"));
                gtk_widget_show (label);

                goto out;
        }

        error = NULL;
        bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
        if (!bus) {
                g_warning ("Error connecting to system D-Bus: %s",
                           error->message);
                g_error_free (error);
        }
        setup->nm_settings = nm_remote_settings_new (bus);

        selection = OBJ(GtkTreeSelection*, "network-list-selection");

        g_signal_connect (selection, "changed",
                          G_CALLBACK (wireless_selection_changed), setup);

        refresh_wireless_list (setup);

out: ;
}

/* Account page {{{1 */

enum {
        PANEL_ACCOUNT_COLUMN_ACTIVE,
        PANEL_ACCOUNT_COLUMN_TITLE,
        PANEL_ACCOUNT_COLUMN_NAME
};

enum {
        PANEL_ACCOUNT_ROW_LOCAL,
        PANEL_ACCOUNT_ROW_REMOTE
};

static gboolean skip_account = FALSE;

static void
update_account_page_status (SetupData *setup)
{
        gboolean complete;

        complete = setup->valid_name && setup->valid_username &&
                   (setup->valid_password ||
                    setup->password_mode == ACT_USER_PASSWORD_MODE_NONE);

        gtk_assistant_set_page_complete (setup->assistant, WID("account-page"), complete);
        gtk_widget_set_sensitive (WID("local-account-done-button"), complete);
}

static void
fullname_changed (GtkWidget *w, GParamSpec *pspec, SetupData *setup)
{
        GtkWidget *combo;
        GtkWidget *entry;
        GtkTreeModel *model;
        const char *name;

        name = gtk_entry_get_text (GTK_ENTRY (w));

        combo = WID("account-username-combo");
        entry = gtk_bin_get_child (GTK_BIN (combo));
        model = gtk_combo_box_get_model (GTK_COMBO_BOX (combo));

        gtk_list_store_clear (GTK_LIST_STORE (model));

        setup->valid_name = is_valid_name (name);
        setup->user_data_unsaved = TRUE;

        if (!setup->valid_name) {
                gtk_entry_set_text (GTK_ENTRY (entry), "");
                return;
        }

        generate_username_choices (name, GTK_LIST_STORE (model));

        gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);

        update_account_page_status (setup);
}

static void
username_changed (GtkComboBoxText *combo, SetupData *setup)
{
        const gchar *username;
        gchar *tip;
        GtkWidget *entry;

        username = gtk_combo_box_text_get_active_text (combo);

        setup->valid_username = is_valid_username (username, &tip);
        setup->user_data_unsaved = TRUE;

        entry = gtk_bin_get_child (GTK_BIN (combo));

        if (tip) {
                set_entry_validation_error (GTK_ENTRY (entry), tip);
                g_free (tip);
        }
        else {
                clear_entry_validation_error (GTK_ENTRY (entry));
        }

        update_account_page_status (setup);
}

static void
password_check_changed (GtkWidget *w, GParamSpec *pspec, SetupData *setup)
{
        GtkWidget *password_entry;
        GtkWidget *confirm_entry;
        gboolean need_password;

        need_password = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w));

        if (need_password) {
                setup->password_mode = ACT_USER_PASSWORD_MODE_REGULAR;
                setup->valid_password = FALSE;
        }
        else {
                setup->password_mode = ACT_USER_PASSWORD_MODE_NONE;
                setup->valid_password = TRUE;
        }

        password_entry = WID("account-password-entry");
        confirm_entry = WID("account-confirm-entry");

        gtk_entry_set_text (GTK_ENTRY (password_entry), "");
        gtk_entry_set_text (GTK_ENTRY (confirm_entry), "");
        gtk_widget_set_sensitive (password_entry, need_password);
        gtk_widget_set_sensitive (confirm_entry, need_password);

        setup->user_data_unsaved = TRUE;
        update_account_page_status (setup);
}

static void
admin_check_changed (GtkWidget *w, GParamSpec *pspec, SetupData *setup)
{
        if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w))) {
                setup->account_type = ACT_USER_ACCOUNT_TYPE_ADMINISTRATOR;
        }
        else {
                setup->account_type = ACT_USER_ACCOUNT_TYPE_STANDARD;
        }

        setup->user_data_unsaved = TRUE;
        update_account_page_status (setup);
}

#define MIN_PASSWORD_LEN 6

static void
update_password_entries (SetupData *setup)
{
        const gchar *password;
        const gchar *verify;
        const gchar *username;
        GtkWidget *password_entry;
        GtkWidget *confirm_entry;
        GtkWidget *username_combo;
        gdouble strength;
        const gchar *hint;
        const gchar *long_hint = NULL;

        password_entry = WID("account-password-entry");
        confirm_entry = WID("account-confirm-entry");
        username_combo = WID("account-username-combo");

        password = gtk_entry_get_text (GTK_ENTRY (password_entry));
        verify = gtk_entry_get_text (GTK_ENTRY (confirm_entry));
        username = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (username_combo));

        strength = pw_strength (password, NULL, username, &hint, &long_hint);

        if (strength == 0.0) {
                setup->valid_password = FALSE;
                setup->password_reason = long_hint ? long_hint : hint;
        }
        else if (strcmp (password, verify) != 0) {
                setup->valid_password = FALSE;
                setup->password_reason = _("Passwords do not match");
        }
        else {
                setup->valid_password = TRUE;
        }
}

static void
password_changed (GtkWidget *w, GParamSpec *pspec, SetupData *setup)
{
        update_password_entries (setup);

        setup->user_data_unsaved = TRUE;
        update_account_page_status (setup);
}

static void
confirm_changed (GtkWidget *w, GParamSpec *pspec, SetupData *setup)
{
        clear_entry_validation_error (GTK_ENTRY (w));
        update_password_entries (setup);

        setup->user_data_unsaved = TRUE;
        update_account_page_status (setup);
}

static gboolean
confirm_entry_focus_out (GtkWidget     *entry,
                         GdkEventFocus *event,
                         SetupData     *setup)
{
        const gchar *password;
        const gchar *verify;
        GtkEntry *password_entry;
        GtkEntry *confirm_entry;

        password_entry = OBJ(GtkEntry*, "account-password-entry");
        confirm_entry = OBJ(GtkEntry*, "account-confirm-entry");
        password = gtk_entry_get_text (password_entry);
        verify = gtk_entry_get_text (confirm_entry);

        if (strlen (password) > 0 && strlen (verify) > 0) {
                if (!setup->valid_password) {
                        set_entry_validation_error (confirm_entry,
                                                    setup->password_reason);
                }
                else {
                        clear_entry_validation_error (confirm_entry);
                }
        }

        return FALSE;
}

static void
set_user_avatar (SetupData *setup)
{
        gchar *path;
        gint fd;
        GOutputStream *stream;
        GError *error;

        if (setup->avatar_filename != NULL) {
                act_user_set_icon_file (setup->act_user, setup->avatar_filename);
                return;
        }

        if (setup->avatar_pixbuf == NULL) {
                return;
        }

        path = g_build_filename (g_get_tmp_dir (), "usericonXXXXXX", NULL);
        fd = g_mkstemp (path);
        if (fd == -1) {
                g_warning ("failed to create temporary file for image data");
                g_free (path);
                return;
        }

        stream = g_unix_output_stream_new (fd, TRUE);

        error = NULL;
        if (!gdk_pixbuf_save_to_stream (setup->avatar_pixbuf, stream, "png", NULL, &error, NULL)) {
                g_warning ("failed to save image: %s", error->message);
                g_error_free (error);
                g_object_unref (stream);
                return;
        }

        g_object_unref (stream);

        act_user_set_icon_file (setup->act_user, path); 
}

static void
create_user (SetupData *setup)
{
        const gchar *username;
        const gchar *fullname;
        GError *error;

        username = gtk_combo_box_text_get_active_text (OBJ(GtkComboBoxText*, "account-username-combo"));
        fullname = gtk_entry_get_text (OBJ(GtkEntry*, "account-fullname-entry"));

        error = NULL;
        setup->act_user = act_user_manager_create_user (setup->act_client, username, fullname, setup->account_type, &error);
        if (error != NULL) {
                g_warning ("Failed to create user: %s", error->message);
                g_error_free (error);
        }

        set_user_avatar (setup);
}

static void save_account_data (SetupData *setup);

gulong when_loaded;

static void
save_when_loaded (ActUser *user, GParamSpec *pspec, SetupData *setup)
{
        g_signal_handler_disconnect (user, when_loaded);
        when_loaded = 0;

        save_account_data (setup);
}

static void
set_account_model_row (SetupData *setup, gint row, gboolean active, const gchar *name)
{
        GtkTreeModel *model;
        GtkTreeIter iter;
        gchar *n = NULL;

        model = gtk_tree_view_get_model (GTK_TREE_VIEW (WID("account-list")));

        gtk_tree_model_get_iter_first (model, &iter);
        if (row == PANEL_ACCOUNT_ROW_REMOTE)
                gtk_tree_model_iter_next (model, &iter);

        if (name == NULL) {
                gtk_tree_model_get (model, &iter, PANEL_ACCOUNT_COLUMN_NAME, &n, -1);
                name = (const gchar *)n;
        }

        gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                            PANEL_ACCOUNT_COLUMN_ACTIVE, active,
                            PANEL_ACCOUNT_COLUMN_NAME, name,
                            -1);

        g_free (n);
}

static void
clear_account_page (SetupData *setup)
{
        GtkWidget *fullname_entry;
        GtkWidget *username_combo;
        GtkWidget *password_check;
        GtkWidget *admin_check;
        GtkWidget *password_entry;
        GtkWidget *confirm_entry;
        gboolean need_password;

        fullname_entry = WID("account-fullname-entry");
        username_combo = WID("account-username-combo");
        password_check = WID("account-password-check");
        admin_check = WID("account-admin-check");
        password_entry = WID("account-password-entry");
        confirm_entry = WID("account-confirm-entry");

        setup->valid_name = FALSE;
        setup->valid_username = FALSE;
        setup->valid_password = TRUE;
        setup->password_mode = ACT_USER_PASSWORD_MODE_NONE;
        setup->account_type = ACT_USER_ACCOUNT_TYPE_ADMINISTRATOR;
        setup->user_data_unsaved = FALSE;

        need_password = setup->password_mode != ACT_USER_PASSWORD_MODE_NONE;
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (password_check), need_password);
        gtk_widget_set_sensitive (password_entry, need_password);
        gtk_widget_set_sensitive (confirm_entry, need_password);

        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (admin_check), setup->account_type == ACT_USER_ACCOUNT_TYPE_ADMINISTRATOR);

        gtk_entry_set_text (GTK_ENTRY (fullname_entry), "");
        gtk_list_store_clear (GTK_LIST_STORE (gtk_combo_box_get_model (GTK_COMBO_BOX (username_combo))));
        gtk_entry_set_text (GTK_ENTRY (password_entry), "");
        gtk_entry_set_text (GTK_ENTRY (confirm_entry), "");

        set_account_model_row (setup, PANEL_ACCOUNT_ROW_LOCAL, FALSE, "");
}

static void
save_account_data (SetupData *setup)
{
        const gchar *realname;
        const gchar *username;
        const gchar *password;

        if (!setup->user_data_unsaved) {
                return;
        }

        /* this can happen when going back */
        if (!setup->valid_name ||
            !setup->valid_username ||
            !setup->valid_password) {
                return;
        }

        if (setup->act_user == NULL) {
                create_user (setup);
        }

        if (setup->act_user == NULL) {
                g_warning ("User creation failed");
                clear_account_page (setup);
                return;
        }

        if (!act_user_is_loaded (setup->act_user)) {
                if (when_loaded == 0)
                        when_loaded = g_signal_connect (setup->act_user, "notify::is-loaded",
                                                        G_CALLBACK (save_when_loaded), setup);
                return;
        }

        realname = gtk_entry_get_text (OBJ (GtkEntry*, "account-fullname-entry"));
        username = gtk_combo_box_text_get_active_text (OBJ (GtkComboBoxText*, "account-username-combo"));
        password = gtk_entry_get_text (OBJ (GtkEntry*, "account-password-entry"));

        act_user_set_real_name (setup->act_user, realname);
        act_user_set_user_name (setup->act_user, username);
        act_user_set_account_type (setup->act_user, setup->account_type);
        if (setup->password_mode == ACT_USER_PASSWORD_MODE_REGULAR) {
                act_user_set_password (setup->act_user, password, NULL);
        }
        else {
                act_user_set_password_mode (setup->act_user, setup->password_mode);
        }

        gnome_keyring_create_sync ("Default", password ? password : "");
        gnome_keyring_set_default_keyring_sync ("Default");

        setup->user_data_unsaved = FALSE;
}

static void
show_local_account_dialog (SetupData *setup)
{
        GtkWidget *dialog;

        dialog = WID("local-account-dialog");

        gtk_window_present (GTK_WINDOW (dialog));
}

static void
hide_local_account_dialog (GtkButton *button, gpointer data)
{
        SetupData *setup = data;
        GtkWidget *dialog;

        dialog = WID("local-account-dialog");

        gtk_widget_hide (dialog);
        clear_account_page (setup);
}

static void
create_local_account (GtkButton *button, gpointer data)
{
        SetupData *setup = data;
        GtkWidget *dialog;
        const gchar *realname;

        dialog = WID("local-account-dialog");

        realname = gtk_entry_get_text (OBJ (GtkEntry*, "account-fullname-entry"));
        set_account_model_row (setup, PANEL_ACCOUNT_ROW_LOCAL, TRUE, realname);

        gtk_widget_hide (dialog);
}

static void
account_set_active_data (GtkCellLayout   *layout,
                         GtkCellRenderer *cell,
                         GtkTreeModel    *model,
                         GtkTreeIter     *iter,
                         gpointer         data)
{
        gboolean active;

        gtk_tree_model_get (model, iter,
                            PANEL_ACCOUNT_COLUMN_ACTIVE, &active,
                            -1);

        g_object_set (cell, "text", active ? "\342\254\251" : " ", NULL);
}

static void
account_row_activated (GtkTreeView       *tv,
                       GtkTreePath       *path,
                       GtkTreeViewColumn *column,
                       gpointer           data)
{
        SetupData *setup = data;
        gint type;

        type = gtk_tree_path_get_indices (path)[0];

        if (type == PANEL_ACCOUNT_ROW_LOCAL) {
                set_account_model_row (setup, PANEL_ACCOUNT_ROW_LOCAL, TRUE, NULL);
                set_account_model_row (setup, PANEL_ACCOUNT_ROW_REMOTE, FALSE, "");
                show_local_account_dialog (setup);
        }
        else {
                set_account_model_row (setup, PANEL_ACCOUNT_ROW_LOCAL, FALSE, "");
                set_account_model_row (setup, PANEL_ACCOUNT_ROW_REMOTE, TRUE, NULL);
                clear_account_page (setup);
        }
}

static void
avatar_callback (GdkPixbuf   *pixbuf,
                 const gchar *filename,
                 gpointer     data)
{
        SetupData *setup = data;
        GtkWidget *image;
        GdkPixbuf *tmp;

        g_clear_object (&setup->avatar_pixbuf);
        g_free (setup->avatar_filename);
        setup->avatar_filename = NULL;

        image = WID("local-account-avatar-image");

        if (pixbuf) {
                setup->avatar_pixbuf = g_object_ref (pixbuf);
                tmp = gdk_pixbuf_scale_simple (pixbuf, 64, 64, GDK_INTERP_BILINEAR);
                gtk_image_set_from_pixbuf (GTK_IMAGE (image), tmp);
                g_object_unref (tmp);
        }
        else if (filename) {
                setup->avatar_filename = g_strdup (filename);
                tmp = gdk_pixbuf_new_from_file_at_size (filename, 64, 64, NULL);
                gtk_image_set_from_pixbuf (GTK_IMAGE (image), tmp);
                g_object_unref (tmp);
        }
        else {
                gtk_image_set_from_icon_name (GTK_IMAGE (image), "avatar-default",
                                                                GTK_ICON_SIZE_DIALOG);
        }
}

static void
prepare_account_page (SetupData *setup)
{
        GtkWidget *fullname_entry;
        GtkWidget *username_combo;
        GtkWidget *password_check;
        GtkWidget *admin_check;
        GtkWidget *password_entry;
        GtkWidget *confirm_entry;
        GtkWidget *local_account_cancel_button;
        GtkWidget *local_account_done_button;
        GtkWidget *local_account_avatar_button;
        GtkTreeViewColumn *col;
        GtkCellRenderer *cell;

        if (!skip_account)
                gtk_widget_show (WID("account-page"));

        col = OBJ(GtkTreeViewColumn*, "account-list-column");
        cell = gtk_cell_renderer_text_new ();
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (col), cell, FALSE);
        gtk_cell_layout_set_cell_data_func (GTK_CELL_LAYOUT (col), cell,
                                            account_set_active_data, NULL, NULL);
        cell = gtk_cell_renderer_text_new ();
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (col), cell, TRUE);
        gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (col), cell, "text", PANEL_ACCOUNT_COLUMN_TITLE);
        g_object_set (cell, "width", 400, NULL);

        cell = gtk_cell_renderer_text_new ();
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (col), cell, FALSE);
        gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (col), cell, "text", PANEL_ACCOUNT_COLUMN_NAME);

        g_signal_connect (WID("account-list"), "row-activated",
                          G_CALLBACK (account_row_activated), setup);

        fullname_entry = WID("account-fullname-entry");
        username_combo = WID("account-username-combo");
        password_check = WID("account-password-check");
        admin_check = WID("account-admin-check");
        password_entry = WID("account-password-entry");
        confirm_entry = WID("account-confirm-entry");
        local_account_cancel_button = WID("local-account-cancel-button");
        local_account_done_button = WID("local-account-done-button");
        local_account_avatar_button = WID("local-account-avatar-button");
        setup->photo_dialog = (GtkWidget *)um_photo_dialog_new (local_account_avatar_button,
                                                                avatar_callback,
                                                                setup);

        g_signal_connect (fullname_entry, "notify::text",
                          G_CALLBACK (fullname_changed), setup);
        g_signal_connect (username_combo, "changed",
                          G_CALLBACK (username_changed), setup);
        g_signal_connect (password_check, "notify::active",
                           G_CALLBACK (password_check_changed), setup);
        g_signal_connect (admin_check, "notify::active",
                          G_CALLBACK (admin_check_changed), setup);
        g_signal_connect (password_entry, "notify::text",
                          G_CALLBACK (password_changed), setup);
        g_signal_connect (confirm_entry, "notify::text",
                          G_CALLBACK (confirm_changed), setup);
        g_signal_connect_after (confirm_entry, "focus-out-event",
                                G_CALLBACK (confirm_entry_focus_out), setup);
        g_signal_connect (local_account_cancel_button, "clicked",
                          G_CALLBACK (hide_local_account_dialog), setup);
        g_signal_connect (local_account_done_button, "clicked",
                          G_CALLBACK (create_local_account), setup);

        setup->act_client = act_user_manager_get_default ();

        clear_account_page (setup);
        update_account_page_status (setup);
}

/* Location page {{{1 */

static void
set_timezone_cb (GObject      *source,
                 GAsyncResult *res,
                 gpointer      user_data)
{
        SetupData *setup = user_data;
        GError *error;

        error = NULL;
        if (!timedate1_call_set_timezone_finish (setup->dtm,
                                                 res,
                                                 &error)) {
                /* TODO: display any error in a user friendly way */
                g_warning ("Could not set system timezone: %s", error->message);
                g_error_free (error);
        }
}


static void
queue_set_timezone (SetupData *setup)
{
        /* for now just do it */
        if (setup->current_location) {
                timedate1_call_set_timezone (setup->dtm,
                                             setup->current_location->zone,
                                             TRUE,
                                             NULL,
                                             set_timezone_cb,
                                             setup);
        }
}

static void
update_timezone (SetupData *setup)
{
        GString *str;
        gchar *location;
        gchar *timezone;
        gchar *c;

        str = g_string_new ("");
        for (c = setup->current_location->zone; *c; c++) {
                switch (*c) {
                case '_':
                        g_string_append_c (str, ' ');
                        break;
                case '/':
                        g_string_append (str, " / ");
                        break;
                default:
                        g_string_append_c (str, *c);
                }
        }

        c = strstr (str->str, " / ");
        location = g_strdup (c + 3);
        timezone = g_strdup (str->str);

        gtk_label_set_label (OBJ(GtkLabel*,"current-location-label"), location);
        gtk_label_set_label (OBJ(GtkLabel*,"current-timezone-label"), timezone);

        g_free (location);
        g_free (timezone);

        g_string_free (str, TRUE);
}

static void
location_changed_cb (CcTimezoneMap *map,
                     TzLocation    *location,
                     SetupData     *setup)
{
  g_debug ("location changed to %s/%s", location->country, location->zone);

  setup->current_location = location;

  update_timezone (setup);

  queue_set_timezone (setup);
}

static void
set_location_from_gweather_location (SetupData        *setup,
                                     GWeatherLocation *gloc)
{
        GWeatherTimezone *zone = gweather_location_get_timezone (gloc);
        gchar *city = gweather_location_get_city_name (gloc);

        if (zone != NULL) {
                const gchar *name;
                const gchar *id;
                GtkLabel *label;

                label = OBJ(GtkLabel*, "current-timezone-label");

                name = gweather_timezone_get_name (zone);
                id = gweather_timezone_get_tzid (zone);
                if (name == NULL) {
                        /* Why does this happen ? */
                        name = id;
                }
                gtk_label_set_label (label, name);
                cc_timezone_map_set_timezone (setup->map, id);
        }

        if (city != NULL) {
                GtkLabel *label;

                label = OBJ(GtkLabel*, "current-location-label");
                gtk_label_set_label (label, city);
        }

        g_free (city);
}

static void
location_changed (GObject *object, GParamSpec *param, SetupData *setup)
{
        GWeatherLocationEntry *entry = GWEATHER_LOCATION_ENTRY (object);
        GWeatherLocation *gloc;

        gloc = gweather_location_entry_get_location (entry);
        if (gloc == NULL)
                return;

        set_location_from_gweather_location (setup, gloc);

        gweather_location_unref (gloc);
}

static void
position_callback (GeocluePosition      *pos,
		   GeocluePositionFields fields,
		   int                   timestamp,
		   double                latitude,
		   double                longitude,
		   double                altitude,
		   GeoclueAccuracy      *accuracy,
		   GError               *error,
		   SetupData            *setup)
{
	if (error) {
		g_printerr ("Error getting position: %s\n", error->message);
		g_error_free (error);
	} else {
		if (fields & GEOCLUE_POSITION_FIELDS_LATITUDE &&
		    fields & GEOCLUE_POSITION_FIELDS_LONGITUDE) {
                        GWeatherLocation *city = gweather_location_find_nearest_city (latitude, longitude);
                        set_location_from_gweather_location (setup, city);
		} else {
			g_print ("Position not available.\n");
		}
	}
}

static void
determine_location (GtkWidget *widget,
                    SetupData *setup)
{
	GeoclueMaster *master;
	GeoclueMasterClient *client;
	GeocluePosition *position = NULL;
        GError *error = NULL;

	master = geoclue_master_get_default ();
	client = geoclue_master_create_client (master, NULL, NULL);
	g_object_unref (master);

	if (!geoclue_master_client_set_requirements (client, 
	                                             GEOCLUE_ACCURACY_LEVEL_LOCALITY,
	                                             0, TRUE,
	                                             GEOCLUE_RESOURCE_ALL,
	                                             NULL)){
		g_printerr ("Setting requirements failed");
                goto out;
	}

	position = geoclue_master_client_create_position (client, &error);
	if (position == NULL) {
		g_warning ("Creating GeocluePosition failed: %s", error->message);
                goto out;
	}

	geoclue_position_get_position_async (position,
	                                     (GeocluePositionCallback) position_callback,
	                                     setup);

 out:
        g_clear_error (&error);
        g_object_unref (client);
        g_object_unref (position);
}

static void
prepare_location_page (SetupData *setup)
{
        GtkWidget *frame, *map, *entry;
        GWeatherLocation *world;
        GError *error;
        const gchar *timezone;

        frame = WID("location-map-frame");

        error = NULL;
        setup->dtm = timedate1_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                       G_DBUS_PROXY_FLAGS_NONE,
                                                       "org.freedesktop.timedate1",
                                                       "/org/freedesktop/timedate1",
                                                       NULL,
                                                       &error);
        if (setup->dtm == NULL) {
                g_error ("Failed to create proxy for timedated: %s", error->message);
                exit (1);
        }

        setup->map = cc_timezone_map_new ();
        map = (GtkWidget *)setup->map;
        gtk_widget_set_hexpand (map, TRUE);
        gtk_widget_set_vexpand (map, TRUE);
        gtk_widget_set_halign (map, GTK_ALIGN_FILL);
        gtk_widget_set_valign (map, GTK_ALIGN_FILL);
        gtk_widget_show (map);

        gtk_container_add (GTK_CONTAINER (frame), map);

        world = gweather_location_new_world (FALSE);
        entry = gweather_location_entry_new (world);
        gtk_entry_set_placeholder_text (GTK_ENTRY (entry), _("Search for a location"));
        gtk_widget_set_halign (entry, GTK_ALIGN_END);
        gtk_widget_show (entry);

        frame = WID("location-page");
        gtk_grid_attach (GTK_GRID (frame), entry, 1, 1, 1, 1);

        timezone = timedate1_get_timezone (setup->dtm);

        if (!cc_timezone_map_set_timezone (setup->map, timezone)) {
                g_warning ("Timezone '%s' is unhandled, setting %s as default", timezone, DEFAULT_TZ);
                cc_timezone_map_set_timezone (setup->map, DEFAULT_TZ);
        }
        else {
                g_debug ("System timezone is '%s'", timezone);
        }

        setup->current_location = cc_timezone_map_get_location (setup->map);
        update_timezone (setup);

        g_signal_connect (G_OBJECT (entry), "notify::location",
                          G_CALLBACK (location_changed), setup);

        g_signal_connect (setup->map, "location-changed",
                          G_CALLBACK (location_changed_cb), setup);

        g_signal_connect (WID ("location-auto-button"), "clicked",
                          G_CALLBACK (determine_location), setup);
}

/* Online accounts page {{{1 */

static GtkWidget *
create_provider_button (const gchar *type, const gchar *name, GIcon *icon)
{
  GtkWidget *button;
  GtkWidget *box;
  GtkWidget *image;
  GtkWidget *label;

  button = gtk_button_new ();

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (box, GTK_ALIGN_START);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_container_add (GTK_CONTAINER (button), box);

  image = gtk_image_new_from_gicon (icon, GTK_ICON_SIZE_BUTTON);
  gtk_box_pack_start (GTK_BOX (box), image, FALSE, FALSE, 0);

  label = gtk_label_new (name);
  gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

  gtk_widget_show (box);
  gtk_widget_show (image);
  gtk_widget_show (label);

  g_object_set_data (G_OBJECT (button), "provider-type", (gpointer)type);

  return button;
}

static void
add_account (GtkButton *button, gpointer data)
{
  SetupData *setup = data;
  GtkWidget *dialog;
  GtkWidget *goa_dialog;
  GtkWidget *vbox;
  const gchar *provider_type;
  GoaProvider *provider;
  GError *error;

  dialog = WID("online-accounts-dialog");
  gtk_widget_hide (dialog);

  provider_type = g_object_get_data (G_OBJECT (button), "provider-type");

  g_debug ("Adding online account: %s", provider_type);

  provider = goa_provider_get_for_provider_type (provider_type);

  goa_dialog = gtk_dialog_new ();

  gtk_container_set_border_width (GTK_CONTAINER (goa_dialog), 12);
  gtk_window_set_modal (GTK_WINDOW (goa_dialog), TRUE);
  gtk_window_set_resizable (GTK_WINDOW (goa_dialog), TRUE);
  gtk_window_set_transient_for (GTK_WINDOW (goa_dialog), GTK_WINDOW (setup->assistant));
  /* translators: This is the title of the "Add Account" dialogue.
   * The title is not visible when using GNOME Shell
   */
  gtk_window_set_title (GTK_WINDOW (goa_dialog), _("Add Account"));
  gtk_dialog_add_button (GTK_DIALOG (goa_dialog),
                         GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL);

  vbox = gtk_dialog_get_content_area (GTK_DIALOG (goa_dialog));
  gtk_widget_set_vexpand (vbox, TRUE);

  gtk_widget_show_all (goa_dialog);
  gtk_window_present (GTK_WINDOW (goa_dialog));

  error = NULL;
  goa_provider_add_account (provider,
                            setup->goa_client,
                            GTK_DIALOG (goa_dialog),
                            GTK_BOX (vbox),
                            &error);
  gtk_widget_destroy (goa_dialog);

  if (error &&
      !(error->domain == GOA_ERROR && error->code == GOA_ERROR_DIALOG_DISMISSED))
        {
          dialog = gtk_message_dialog_new (GTK_WINDOW (setup->assistant),
                                           GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                           GTK_MESSAGE_ERROR,
                                           GTK_BUTTONS_CLOSE,
                                           _("Error creating account"));
          gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                                    "%s",
                                                    error->message);
          gtk_widget_show (dialog);
          gtk_dialog_run (GTK_DIALOG (dialog));
          gtk_widget_destroy (dialog);
    }
}

static void
populate_online_account_dialog (SetupData *setup)
{
  GtkWidget *dialog;
  GtkWidget *content_area;
  GList *providers, *l;
  GoaProvider *provider;
  gchar *provider_name;
  const gchar *provider_type;
  GIcon *provider_icon;
  GtkWidget *button;


  dialog = WID("online-accounts-dialog");
  content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));

  providers = goa_provider_get_all ();
  for (l = providers; l; l = l->next)
    {
      provider = GOA_PROVIDER (l->data);
      provider_type = goa_provider_get_provider_type (provider);
      provider_name = goa_provider_get_provider_name (provider, NULL);
      provider_icon = goa_provider_get_provider_icon (provider, NULL);
      button = create_provider_button (provider_type, provider_name, provider_icon);
      gtk_container_add (GTK_CONTAINER (content_area), button);
      gtk_widget_show (button);
      g_free (provider_name);

      g_signal_connect (button, "clicked", G_CALLBACK (add_account), setup);
    }

  g_signal_connect (dialog, "delete-event",
                    G_CALLBACK (gtk_widget_hide_on_delete), NULL);
}

static void
show_online_account_dialog (GtkButton *button, gpointer data)
{
  SetupData *setup = data;
  GtkWidget *dialog;

  dialog = WID("online-accounts-dialog");

  gtk_window_present (GTK_WINDOW (dialog));
}

static void
remove_account_cb (GoaAccount   *account,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  SetupData *setup = user_data;
  GError *error;

  error = NULL;
  if (!goa_account_call_remove_finish (account, res, &error))
    {
      GtkWidget *dialog;
      dialog = gtk_message_dialog_new (GTK_WINDOW (setup->assistant),
                                       GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                       GTK_MESSAGE_ERROR,
                                       GTK_BUTTONS_CLOSE,
                                       _("Error removing account"));
      gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                                "%s",
                                                error->message);
      gtk_widget_show_all (dialog);
      gtk_dialog_run (GTK_DIALOG (dialog));
      gtk_widget_destroy (dialog);
      g_error_free (error);
    }
}


static void
confirm_remove_account (GtkButton *button, gpointer data)
{
  SetupData *setup = data;
  GtkWidget *dialog;
  GoaObject *object;
  gint response;

  object = g_object_get_data (G_OBJECT (button), "goa-object");

  dialog = gtk_message_dialog_new (GTK_WINDOW (setup->assistant),
                                   GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                   GTK_MESSAGE_QUESTION,
                                   GTK_BUTTONS_CANCEL,
                                   _("Are you sure you want to remove the account?"));
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                            _("This will not remove the account on the server."));
  gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Remove"), GTK_RESPONSE_OK);
  gtk_widget_show_all (dialog);
  response = gtk_dialog_run (GTK_DIALOG (dialog));
  gtk_widget_destroy (dialog);

  if (response == GTK_RESPONSE_OK)
    {
      goa_account_call_remove (goa_object_peek_account (object),
                               NULL, /* GCancellable */
                               (GAsyncReadyCallback) remove_account_cb,
                               setup);
    }
}


static void
add_account_to_list (SetupData *setup, GoaObject *object)
{
  GtkWidget *list;
  GtkWidget *box;
  GtkWidget *image;
  GtkWidget *label;
  GtkWidget *button;
  GoaAccount *account;
  GIcon *icon;
  gchar *markup;

  account = goa_object_peek_account (object);

  icon = g_icon_new_for_string (goa_account_get_provider_icon (account), NULL);
  markup = g_strdup_printf ("<b>%s</b>\n"
                            "<small><span foreground=\"#555555\">%s</span></small>",
                            goa_account_get_provider_name (account),
                            goa_account_get_presentation_identity (account));

  list = WID ("online-accounts-list");

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand (box, TRUE);

  g_object_set_data (G_OBJECT (box), "account-id",
                     (gpointer)goa_account_get_id (account));

  image = gtk_image_new_from_gicon (icon, GTK_ICON_SIZE_DIALOG);
  label = gtk_label_new (markup);
  gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
  button = gtk_button_new_with_label (_("Remove"));
  gtk_widget_set_halign (button, GTK_ALIGN_END);
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);

  g_object_set_data_full (G_OBJECT (button), "goa-object",
                          g_object_ref (object), g_object_unref);

  g_signal_connect (button, "clicked",
                    G_CALLBACK (confirm_remove_account), setup);

  gtk_box_pack_start (GTK_BOX (box), image, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), button, TRUE, TRUE, 0);

  gtk_widget_show_all (box);

  gtk_container_add (GTK_CONTAINER (list), box);
}

static void
remove_account_from_list (SetupData *setup, GoaObject *object)
{
  GtkWidget *list;
  GList *children, *l;
  GtkWidget *child;
  GoaAccount *account;
  const gchar *account_id, *id;

  account = goa_object_peek_account (object);

  account_id = goa_account_get_id (account);

  list = WID ("online-accounts-list");

  children = gtk_container_get_children (GTK_CONTAINER (list));
  for (l = children; l; l = l->next)
    {
      child = GTK_WIDGET (l->data);

      id = (const gchar *)g_object_get_data (G_OBJECT (child), "account-id");

      if (g_strcmp0 (id, account_id) == 0)
        {
          gtk_widget_destroy (child);
          break;
        }
    }
  g_list_free (children);
}

static void
populate_account_list (SetupData *setup)
{
  GList *accounts, *l;
  GoaObject *object;

  accounts = goa_client_get_accounts (setup->goa_client);
  for (l = accounts; l; l = l->next)
    {
      object = GOA_OBJECT (l->data);
      add_account_to_list (setup, object);
    }

  g_list_free_full (accounts, (GDestroyNotify) g_object_unref);
}

static void
goa_account_added (GoaClient *client, GoaObject *object, gpointer data)
{
  SetupData *setup = data;

  g_debug ("Online account added");

  add_account_to_list (setup, object);
}

static void
goa_account_removed (GoaClient *client, GoaObject *object, gpointer data)
{
  SetupData *setup = data;

  g_debug ("Online account removed");

  remove_account_from_list (setup, object);
}

static void
prepare_online_page (SetupData *setup)
{
  GtkWidget *button;
  GError *error = NULL;

  setup->goa_client = goa_client_new_sync (NULL, &error);
  if (setup->goa_client == NULL)
    {
       g_error ("Failed to get a GoaClient: %s", error->message);
       g_error_free (error);
       return;
    }

  populate_online_account_dialog (setup);
  populate_account_list (setup);

  button = WID("online-add-button");
  g_signal_connect (button, "clicked",
                    G_CALLBACK (show_online_account_dialog), setup);

  g_signal_connect (setup->goa_client, "account-added",
                    G_CALLBACK (goa_account_added), setup);
  g_signal_connect (setup->goa_client, "account-removed",
                    G_CALLBACK (goa_account_removed), setup);
}

/* Other setup {{{1 */

static void
copy_account_file (SetupData   *setup,
                   const gchar *relative_path)
{
        const gchar *username;
        const gchar *homedir;
        GSList *dirs = NULL, *l;
        gchar *p, *tmp;
        gchar *argv[20];
        gint i;
        gchar *from;
        gchar *to;
        GError *error = NULL;

        username = act_user_get_user_name (setup->act_user);
        homedir = act_user_get_home_dir (setup->act_user);

        from = g_build_filename (g_get_home_dir (), relative_path, NULL);
        to = g_build_filename (homedir, relative_path, NULL);

        p = g_path_get_dirname (relative_path);
        while (strcmp (p, ".") != 0) {
                dirs = g_slist_prepend (dirs, g_build_filename (homedir, p, NULL));
                tmp = g_path_get_dirname (p);
                g_free (p);
                p = tmp;
        }

        i = 0;
        argv[i++] = "/usr/bin/pkexec";
        argv[i++] = "install";
        argv[i++] = "--owner";
        argv[i++] = (gchar *)username;
        argv[i++] = "--group";
        argv[i++] = (gchar *)username;
        argv[i++] = "--mode";
        argv[i++] = "755";
        argv[i++] = "--directory";
        for (l = dirs; l; l = l->next) {
                argv[i++] = l->data;
                if (i == 20) {
                        g_warning ("Too many subdirectories");
                        goto out;
                }
        }
        argv[i++] = NULL;

        if (!g_spawn_sync (NULL, argv, NULL, 0, NULL, NULL, NULL, NULL, NULL, &error)) {
                g_warning ("Failed to copy account data: %s", error->message);
                g_error_free (error);
                goto out;
        }

        i = 0;
        argv[i++] = "/usr/bin/pkexec";
        argv[i++] = "install";
        argv[i++] = "--owner";
        argv[i++] = (gchar *)username;
        argv[i++] = "--group";
        argv[i++] = (gchar *)username;
        argv[i++] = "--mode";
        argv[i++] = "755";
        argv[i++] = from;
        argv[i++] = to;
        argv[i++] = NULL;

        if (!g_spawn_sync (NULL, argv, NULL, 0, NULL, NULL, NULL, NULL, NULL, &error)) {
                g_warning ("Failed to copy account data: %s", error->message);
                g_error_free (error);
                goto out;
        }

out:
        g_slist_free_full (dirs, g_free);
        g_free (to);
        g_free (from);
}

static void
copy_account_data (SetupData *setup)
{
        /* here is where we copy all the things we just
         * configured, from the current users home dir to the
         * account that was created in the first step
         */
        g_debug ("Copying account data");
        g_settings_sync ();

        copy_account_file (setup, ".config/dconf/user");
        copy_account_file (setup, ".config/goa-1.0/accounts.conf");
        copy_account_file (setup, ".gnome2/keyrings/Default.keyring");
}

static void
connect_to_slave (SetupData *setup)
{
        GError *error = NULL;
        gboolean res;

        setup->greeter_client = gdm_greeter_client_new ();

        res = gdm_greeter_client_open_connection (setup->greeter_client, &error);

        if (!res) {
                g_warning ("Failed to open connection to slave: %s", error->message);
                g_error_free (error);
                g_clear_object (&setup->greeter_client);
                return;
        }
}

static void
on_ready_for_auto_login (GdmGreeterClient *client,
                         const char       *service_name,
                         SetupData        *setup)
{
        const gchar *username;

        username = act_user_get_user_name (setup->act_user);

        g_debug ("Initiating autologin for %s", username);
        gdm_greeter_client_call_begin_auto_login (client, username);
        gdm_greeter_client_call_start_session_when_ready (client,
                                                          service_name,
                                                          TRUE);
}

static void
begin_autologin (SetupData *setup)
{
        if (setup->greeter_client == NULL) {
                g_warning ("No slave connection; not initiating autologin");
                return;
        }

        if (setup->act_user == NULL) {
                g_warning ("No username; not initiating autologin");
                return;
        }

        g_debug ("Preparing to autologin");

        g_signal_connect (setup->greeter_client,
                          "ready",
                          G_CALLBACK (on_ready_for_auto_login),
                          setup);
        gdm_greeter_client_call_start_conversation (setup->greeter_client, "gdm-autologin");
}

static void
byebye_cb (GtkButton *button, SetupData *setup)
{
        begin_autologin (setup);
}

static void
tour_cb (GtkButton *button, SetupData *setup)
{
        gchar *filename;

        /* the tour is triggered by ~/.config/run-welcome-tour */
        filename = g_build_filename (g_get_home_dir (), ".config", "run-welcome-tour", NULL);
        g_file_set_contents (filename, "yes", -1, NULL);
        copy_account_file (setup, ".config/run-welcome-tour");
        g_free (filename);

        begin_autologin (setup);
}

static void
prepare_summary_page (SetupData *setup)
{
        GtkWidget *button;
        gchar *s;

        s = g_key_file_get_locale_string (setup->overrides,
                                          "Summary", "summary-title",
                                          NULL, NULL);
        if (s)
                gtk_label_set_text (GTK_LABEL (WID ("summary-title")), s);
        g_free (s);

        s = g_key_file_get_locale_string (setup->overrides,
                                          "Summary", "summary-details",
                                          NULL, NULL);
        if (s) {
                gtk_label_set_text (GTK_LABEL (WID ("summary-details")), s);
        }
        g_free (s);

        s = g_key_file_get_locale_string (setup->overrides,
                                          "Summary", "summary-details2",
                                          NULL, NULL);
        if (s)
                gtk_label_set_text (GTK_LABEL (WID ("summary-details2")), s);
        g_free (s);

        s = g_key_file_get_locale_string (setup->overrides,
                                          "Summary", "summary-start-button",
                                          NULL, NULL);
        if (s)
                gtk_button_set_label (GTK_BUTTON (WID ("summary-start-button")), s);
        g_free (s);

        s = g_key_file_get_locale_string (setup->overrides,
                                          "Summary", "summary-tour-details",
                                          NULL, NULL);
        if (s)
                gtk_label_set_text (GTK_LABEL (WID ("summary-tour-details")), s);
        g_free (s);

        s = g_key_file_get_locale_string (setup->overrides,
                                          "Summary", "summary-tour-button",
                                          NULL, NULL);
        if (s)
                gtk_button_set_label (GTK_BUTTON (WID ("summary-tour-button")), s);
        g_free (s);

        button = WID("summary-start-button");
        g_signal_connect (button, "clicked",
                          G_CALLBACK (byebye_cb), setup);
        button = WID("summary-tour-button");
        g_signal_connect (button, "clicked",
                          G_CALLBACK (tour_cb), setup);
}

static void
prepare_cb (GtkAssistant *assi, GtkWidget *page, SetupData *setup)
{
        g_debug ("Preparing page %s", gtk_widget_get_name (page));

        if (page != WID("account-page"))
                gtk_assistant_set_page_complete (assi, page, TRUE);

        save_account_data (setup);

        if (page == WID("summary-page"))
                copy_account_data (setup);
}

static void
prepare_assistant (SetupData *setup)
{
        GList *list;

        setup->assistant = OBJ(GtkAssistant*, "gnome-setup-assistant");

        /* small hack to get rid of cancel button */
        gtk_assistant_commit (setup->assistant);

        /* another small hack to hide the sidebar */
        list = gtk_container_get_children (GTK_CONTAINER (gtk_bin_get_child (GTK_BIN (setup->assistant))));
        gtk_widget_hide (GTK_WIDGET (list->data));
        g_list_free (list);

        g_signal_connect (G_OBJECT (setup->assistant), "prepare",
                          G_CALLBACK (prepare_cb), setup);

        /* connect to gdm slave */
        connect_to_slave (setup);

        prepare_welcome_page (setup);
        prepare_eula_pages (setup);
        prepare_network_page (setup);
        prepare_account_page (setup);
        prepare_location_page (setup);
        prepare_online_page (setup);
        prepare_summary_page (setup);
}

/* main {{{1 */

int
main (int argc, char *argv[])
{
        SetupData *setup;
        gchar *filename;
        GError *error;
        GOptionEntry entries[] = {
                { "skip-account", 0, 0, G_OPTION_ARG_NONE, &skip_account, "Skip account creation", NULL },
                { NULL, 0 }
        };

        bindtextdomain (GETTEXT_PACKAGE, GNOMELOCALEDIR);
        bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

#ifdef HAVE_CHEESE
        cheese_gtk_init (NULL, NULL);
#endif

        setup = g_new0 (SetupData, 1);

        gtk_init_with_args (&argc, &argv, "", entries, GETTEXT_PACKAGE, NULL);

        error = NULL;
        if (g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error) == NULL) {
                g_error ("Couldn't get on session bus: %s", error->message);
                exit (1);
        };

        setup->builder = gtk_builder_new ();
        if (g_file_test ("setup.ui", G_FILE_TEST_EXISTS)) {
                gtk_builder_add_from_file (setup->builder, "setup.ui", &error);
        }
        else if (!gtk_builder_add_from_resource (setup->builder, "/ui/setup.ui", &error)) {
                g_error ("%s", error->message);
                exit (1);
        }

        setup->overrides = g_key_file_new ();
        filename = g_build_filename (UIDIR, "overrides.ini", NULL);
        if (!g_key_file_load_from_file (setup->overrides, filename, 0, &error)) {
                if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
                        g_error ("%s", error->message);
                        exit (1);
                }
                g_error_free (error);
        }
        g_free (filename);

        prepare_assistant (setup);

        gtk_window_present (GTK_WINDOW (setup->assistant));

        gtk_main ();

        return 0;
}

/* Epilogue {{{1 */
/* vim: set foldmethod=marker: */
