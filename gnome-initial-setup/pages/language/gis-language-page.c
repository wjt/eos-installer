/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2012 Red Hat
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
 * Written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 *     Michael Wood <michael.g.wood@intel.com>
 *
 * Based on gnome-control-center cc-region-panel.c
 */

/* Language page {{{1 */

#define PAGE_ID "language"

#include "config.h"
#include "language-resources.h"
#include "cc-language-chooser.h"
#include "gis-language-page.h"

#include <act/act-user-manager.h>
#include <polkit/polkit.h>
#include <locale.h>
#include <gtk/gtk.h>

G_DEFINE_TYPE (GisLanguagePage, gis_language_page, GIS_TYPE_PAGE);

#define GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GIS_TYPE_LANGUAGE_PAGE, GisLanguagePagePrivate))

struct _GisLanguagePagePrivate
{
  GtkWidget *language_chooser;
  GDBusProxy *localed;
  GPermission *permission;
  const gchar *new_locale_id;

  GCancellable *cancellable;
};

#define OBJ(type,name) ((type)gtk_builder_get_object(GIS_PAGE (page)->builder,(name)))
#define WID(name) OBJ(GtkWidget*,name)

static void
set_localed_locale (GisLanguagePage *self)
{
  GisLanguagePagePrivate *priv = self->priv;
  GVariantBuilder *b;
  gchar *s;

  b = g_variant_builder_new (G_VARIANT_TYPE ("as"));
  s = g_strconcat ("LANG=", priv->new_locale_id, NULL);
  g_variant_builder_add (b, "s", s);
  g_free (s);

  g_dbus_proxy_call (priv->localed,
                     "SetLocale",
                     g_variant_new ("(asb)", b, TRUE),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1, NULL, NULL, NULL);
  g_variant_builder_unref (b);
}

static void
change_locale_permission_acquired (GObject      *source,
                                   GAsyncResult *res,
                                   gpointer      data)
{
  GisLanguagePagePrivate *priv;
  GError *error = NULL;
  gboolean allowed;

  priv = GIS_LANGUAGE_PAGE (data)->priv;

  allowed = g_permission_acquire_finish (priv->permission, res, &error);
  if (error) {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to acquire permission: %s\n", error->message);
      g_error_free (error);
      return;
  }

  if (allowed)
    set_localed_locale (GIS_LANGUAGE_PAGE (data));
}

static void
user_loaded (GObject    *object,
             GParamSpec *pspec,
             gpointer    user_data)
{
  gchar *new_locale_id = user_data;

  act_user_set_language (ACT_USER (object), new_locale_id);

  g_free (new_locale_id);
}

static void
language_changed (CcLanguageChooser  *chooser,
                  GParamSpec         *pspec,
                  GisLanguagePage    *page)
{
  GisLanguagePagePrivate *priv = page->priv;
  ActUser *user;
  GisDriver *driver;

  priv->new_locale_id = cc_language_chooser_get_language (chooser);
  driver = GIS_PAGE (page)->driver;

  setlocale (LC_MESSAGES, priv->new_locale_id);
  gis_driver_locale_changed (driver);

  if (gis_driver_get_mode (driver) == GIS_DRIVER_MODE_NEW_USER) {
      if (g_permission_get_allowed (priv->permission)) {
          set_localed_locale (page);
      }
      else if (g_permission_get_can_acquire (priv->permission)) {
          g_permission_acquire_async (priv->permission,
                                      NULL,
                                      change_locale_permission_acquired,
                                      page);
      }
  }
  user = act_user_manager_get_user (act_user_manager_get_default (),
                                    g_get_user_name ());
  if (act_user_is_loaded (user))
    act_user_set_language (user, priv->new_locale_id);
  else
    g_signal_connect (user,
                      "notify::is-loaded",
                      G_CALLBACK (user_loaded),
                      g_strdup (priv->new_locale_id));

  gis_driver_set_user_language (driver, priv->new_locale_id);
}

static void
localed_proxy_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      data)
{
  GisLanguagePage *self = data;
  GisLanguagePagePrivate *priv;
  GDBusProxy *proxy;
  GError *error = NULL;

  proxy = g_dbus_proxy_new_finish (res, &error);

  if (!proxy) {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to contact localed: %s\n", error->message);
      g_error_free (error);
      return;
  }

  priv = self->priv;
  priv->localed = proxy;
}

static void
gis_language_page_constructed (GObject *object)
{
  GisLanguagePage *page = GIS_LANGUAGE_PAGE (object);
  GisLanguagePagePrivate *priv = page->priv;
  GDBusConnection *bus;

  g_type_ensure (CC_TYPE_LANGUAGE_CHOOSER);

  G_OBJECT_CLASS (gis_language_page_parent_class)->constructed (object);

  gtk_container_add (GTK_CONTAINER (page), WID ("language-page"));

  priv->language_chooser = WID ("language-chooser");
  g_signal_connect (priv->language_chooser, "notify::language",
                    G_CALLBACK (language_changed), page);


  /* If we're in new user mode then we're manipulating system settings */
  if (gis_driver_get_mode (GIS_PAGE (page)->driver) == GIS_DRIVER_MODE_NEW_USER)
    {
      priv->permission = polkit_permission_new_sync ("org.freedesktop.locale1.set-locale", NULL, NULL, NULL);

      bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
      g_dbus_proxy_new (bus,
                        G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES,
                        NULL,
                        "org.freedesktop.locale1",
                        "/org/freedesktop/locale1",
                        "org.freedesktop.locale1",
                        priv->cancellable,
                        (GAsyncReadyCallback) localed_proxy_ready,
                        object);
      g_object_unref (bus);
  }

  gis_page_set_complete (GIS_PAGE (page), TRUE);
  gtk_widget_show (GTK_WIDGET (page));
}

static void
gis_language_page_locale_changed (GisPage *page)
{
  gis_page_set_title (GIS_PAGE (page), _("Welcome"));
}

static void
gis_language_page_dispose (GObject *object)
{
  GisLanguagePage *page = GIS_LANGUAGE_PAGE (object);
  GisLanguagePagePrivate *priv = page->priv;

  g_clear_object (&priv->permission);
  g_clear_object (&priv->localed);
  g_clear_object (&priv->cancellable);
}

static void
gis_language_page_class_init (GisLanguagePageClass *klass)
{
  GisPageClass *page_class = GIS_PAGE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  page_class->page_id = PAGE_ID;
  page_class->locale_changed = gis_language_page_locale_changed;
  object_class->constructed = gis_language_page_constructed;
  object_class->dispose = gis_language_page_dispose;

  g_type_class_add_private (object_class, sizeof(GisLanguagePagePrivate));
}

static void
gis_language_page_init (GisLanguagePage *page)
{
  g_resources_register (language_get_resource ());
  page->priv = GET_PRIVATE (page);
}

void
gis_prepare_language_page (GisDriver *driver)
{
  gis_driver_add_page (driver,
                       g_object_new (GIS_TYPE_LANGUAGE_PAGE,
                                     "driver", driver,
                                     NULL));
}
