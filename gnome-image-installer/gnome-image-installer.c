/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2012 Red Hat
 *               2016 Endless Mobile, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Original code written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 */

#include "config.h"

#include "gnome-image-installer.h"

#include <pwd.h>
#include <unistd.h>
#include <stdlib.h>
#include <glib/gi18n.h>
#include <udisks/udisks.h>

#ifdef HAVE_CLUTTER
#include <clutter-gtk/clutter-gtk.h>
#endif

#ifdef HAVE_CHEESE
#include <cheese-gtk.h>
#endif

#include "pages/language/cc-common-language.h"
#include "pages/language/gis-language-page.h"
#include "pages/keyboard/gis-keyboard-page.h"
#include "pages/display/gis-display-page.h"
#include "pages/endless-eula/gis-endless-eula-page.h"
#include "pages/eulas/gis-eula-pages.h"
#include "pages/network/gis-network-page.h"
#include "pages/account/gis-account-page.h"
#include "pages/location/gis-location-page.h"
#include "pages/goa/gis-goa-page.h"
#include "pages/diskimage/gis-diskimage-page.h"
#include "pages/disktarget/gis-disktarget-page.h"
#include "pages/finished/gis-finished-page.h"
#include "pages/install/gis-install-page.h"

/* main {{{1 */

static gboolean force_new_user_mode;
static const gchar *system_setup_pages[] = {
    "account",
    "display",
    "endless_eula",
    "location"
};

typedef void (*PreparePage) (GisDriver *driver);

typedef struct {
    const gchar *page_id;
    PreparePage prepare_page_func;
} PageData;

#define PAGE(name) { #name, gis_prepare_ ## name ## _page }

static PageData page_table[] = {
  PAGE (language),
  PAGE (diskimage),
  PAGE (disktarget),
  PAGE (install),
  PAGE (finished),
  { NULL },
};

#undef PAGE

#define EOS_GROUP "EndlessOS"
#define UNATTENDED_GROUP "Unattended"
#define LOCALE_KEY "locale"
#define VENDOR_KEY "vendor"
#define PRODUCT_KEY "product"

static void
destroy_pages_after (GisAssistant *assistant,
                     GisPage      *page)
{
  GList *pages, *l, *next;

  pages = gis_assistant_get_all_pages (assistant);

  for (l = pages; l != NULL; l = l->next)
    if (l->data == page)
      break;

  l = l->next;
  for (; l != NULL; l = next) {
    next = l->next;
    gtk_widget_destroy (GTK_WIDGET (l->data));
  }
}

static gchar*
sanitize_string (gchar *string)
{
  gchar *r = string;
  gchar *w = string;

  if (string == NULL)
    return NULL;

  for (;*r != '\0'; r++)
    {
      if (*r < 32 || *r > 126)
        continue;
      *w = *r;
      w++;
    }
  *w = '\0';
  return g_ascii_strdown(string, -1);
}

static void
read_keys (const gchar *path)
{
  gchar *ini = g_build_path ("/", path, "install.ini", NULL);
  GKeyFile *keys = g_key_file_new();

  if (g_key_file_load_from_file (keys, ini, G_KEY_FILE_NONE, NULL))
    {
      gchar *locale = g_key_file_get_string (keys, EOS_GROUP, LOCALE_KEY, NULL);
      if (locale != NULL)
        {
          gis_language_page_preselect_language (locale);
          g_free (locale);
        }

      gis_store_set_key_file (keys);

      if (g_key_file_has_group (keys, UNATTENDED_GROUP))
        {
          gchar *contents = NULL;
          gchar *vendor = NULL;
          gchar *product = NULL;
          GError *error = NULL;

          if (g_file_get_contents ("/sys/class/dmi/id/sys_vendor", &contents, NULL, &error))
            {
              vendor = sanitize_string (contents);
              g_free (contents);
            }
          else
            {
              g_warning ("%s", error->message);
              g_error_free (error);
            }

          if (g_file_get_contents ("/sys/class/dmi/id/product_name", &contents, NULL, &error))
            {
              product = sanitize_string (contents);
              g_free (contents);
            }
          else
            {
              g_warning ("%s", error->message);
              g_error_free (error);
            }

          if (vendor != NULL && product != NULL)
            {
              gchar *target_vendor = g_key_file_get_string (keys, UNATTENDED_GROUP, VENDOR_KEY, NULL);
              gchar *target_product = g_key_file_get_string (keys, UNATTENDED_GROUP, PRODUCT_KEY, NULL);
              gchar *lowcase_vendor = g_ascii_strdown (target_vendor, -1);
              gchar *lowcase_product = g_ascii_strdown (target_product, -1);

              if (g_str_equal (vendor, lowcase_vendor)
               && g_str_equal (product, lowcase_product))
                {
                  /* We just set the flag here, rest of the magic happens as we go */
                  gis_store_enter_unattended();
                }
              else
                {
                  g_error ("Unattended mode requested but target device is wrong: expected '%s' from '%s' but system reports '%s' from '%s'",
                           lowcase_product, lowcase_vendor, product, vendor);
                }
              g_free (lowcase_vendor);
              g_free (lowcase_product);
              g_free (target_vendor);
              g_free (target_product);
            }

            g_free (vendor);
            g_free (product);
        }
    }
}

static void
mount_and_read_keys ()
{
  GError *error = NULL;
  UDisksClient *client = udisks_client_new_sync(NULL, &error);
  GDBusObjectManager *manager = udisks_client_get_object_manager(client);
  GList *objects = g_dbus_object_manager_get_objects(manager);
  GList *l;

  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlock *block = udisks_object_peek_block (object);
      UDisksFilesystem *fs = NULL;
      const gchar *const*mounts = NULL;
      gchar *path = NULL;

      if (block == NULL)
        continue;

      if (!g_str_equal ("eosimages", udisks_block_get_id_label (block)))
        continue;

      fs = udisks_object_peek_filesystem (object);

      if (fs == NULL)
        {
          continue;
        }

      mounts = udisks_filesystem_get_mount_points (fs);

      if (mounts != NULL && mounts[0] != NULL)
        {
          read_keys (mounts[0]);
          return;
        }

      if (udisks_filesystem_call_mount_sync (fs, g_variant_new ("a{sv}", NULL),
                                             &path, NULL, NULL))
        {
          read_keys (path);
        }
    }
}

static void
check_for_live_image ()
{
  GError *error = NULL;
  UDisksClient *client = udisks_client_new_sync(NULL, &error);
  GDBusObjectManager *manager = udisks_client_get_object_manager(client);
  GList *objects = g_dbus_object_manager_get_objects(manager);
  GList *l;
  gboolean image_partition = FALSE;
  gboolean root_is_ro_loop = FALSE;

  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlock *block = udisks_object_peek_block (object);
      UDisksFilesystem *fs = NULL;
      const gchar *const*mounts = NULL;
      gchar *path = NULL;

      if (block == NULL)
        continue;

      fs = udisks_object_peek_filesystem (object);

      if (fs == NULL)
        continue;

      mounts = udisks_filesystem_get_mount_points (fs);

      if (mounts == NULL)
        continue;

      if (mounts[0] != NULL && g_str_equal ("/", mounts[0]))
        {
          if (udisks_object_peek_loop (object) == NULL)
            continue;

          /* Since we know this is a loop device, we can trust that the
             readonly property reflects the mount ro-status. For real devices
             it reflects a physical switch rather than mounting options */
          if (udisks_block_get_read_only (block))
            root_is_ro_loop = TRUE;

          continue;
        }

      if (g_str_equal ("eoslive", udisks_block_get_id_label (block)))
        {
          image_partition = TRUE;
          continue;
        }
    }

  g_object_unref (client);
  if (image_partition && root_is_ro_loop)
    gis_store_enter_live_install ();
}

static void
rebuild_pages_cb (GisDriver *driver)
{
  PageData *page_data;
  GisAssistant *assistant;
  GisPage *current_page;

  assistant = gis_driver_get_assistant (driver);
  current_page = gis_assistant_get_current_page (assistant);

  page_data = page_table;

  if (current_page != NULL) {
    destroy_pages_after (assistant, current_page);

    for (page_data = page_table; page_data->page_id != NULL; ++page_data)
      if (g_str_equal (page_data->page_id, GIS_PAGE_GET_CLASS (current_page)->page_id))
        break;

    ++page_data;
  }

  for (; page_data->page_id != NULL; ++page_data)
      page_data->prepare_page_func (driver);

  gis_assistant_locale_changed (assistant);

  /* Skip welcome page in unattended and live install mode */
  if (gis_store_is_unattended () || gis_store_is_live_install ())
    gis_assistant_next_page (assistant);
}

static gboolean
is_running_as_user (const gchar *username)
{
  struct passwd pw, *pwp;
  char buf[4096];

  getpwnam_r (username, &pw, buf, sizeof (buf), &pwp);
  if (pwp == NULL)
    return FALSE;

  return pw.pw_uid == getuid ();
}

static GisDriverMode
get_mode (void)
{
  if (force_new_user_mode)
    return GIS_DRIVER_MODE_NEW_USER;
  else if (is_running_as_user ("gnome-initial-setup"))
    return GIS_DRIVER_MODE_NEW_USER;
  else
    return GIS_DRIVER_MODE_EXISTING_USER;
}

int
main (int argc, char *argv[])
{
  GisDriver *driver;
  int status;
  GOptionContext *context;

gis_page_get_type();

  GOptionEntry entries[] = {
    { "force-new-user", 0, 0, G_OPTION_ARG_NONE, &force_new_user_mode,
      _("Force new user mode"), NULL },
    { NULL }
  };

  context = g_option_context_new (_("- GNOME initial setup"));
  g_option_context_add_main_entries (context, entries, NULL);

  g_option_context_parse (context, &argc, &argv, NULL);

  bindtextdomain (GETTEXT_PACKAGE, GNOMELOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

#ifdef HAVE_CHEESE
  cheese_gtk_init (NULL, NULL);
#endif

  gtk_init (&argc, &argv);
  ev_init ();

#if HAVE_CLUTTER
  if (gtk_clutter_init (NULL, NULL) != CLUTTER_INIT_SUCCESS) {
    g_critical ("Clutter-GTK init failed");
    exit (1);
  }
#endif

  gis_ensure_login_keyring ("gis");

  check_for_live_image ();
  mount_and_read_keys ();

  driver = gis_driver_new (get_mode ());
  g_signal_connect (driver, "rebuild-pages", G_CALLBACK (rebuild_pages_cb), NULL);
  status = g_application_run (G_APPLICATION (driver), argc, argv);

  g_object_unref (driver);
  g_option_context_free (context);
  ev_shutdown ();

  return status;
}

/* Epilogue {{{1 */
/* vim: set foldmethod=marker: */
