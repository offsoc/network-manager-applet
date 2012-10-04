/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager Connection editor -- Connection editor for NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright 2012 Red Hat, Inc.
 */

#include "config.h"

#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include <nm-setting-connection.h>

#include "page-general.h"

G_DEFINE_TYPE (CEPageGeneral, ce_page_general, CE_TYPE_PAGE)

#define CE_PAGE_GENERAL_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CE_TYPE_PAGE_GENERAL, CEPageGeneralPrivate))

typedef struct {
	NMRemoteSettings *remote_settings;
	NMSettingConnection *setting;

#if GTK_CHECK_VERSION(2,24,0)
	GtkComboBoxText *firewall_zone;
#else
	GtkComboBox *firewall_zone;
#endif

	GtkToggleButton *dependent_vpn_checkbox;
	GtkComboBox *dependent_vpn;
	GtkListStore *dependent_vpn_store;
} CEPageGeneralPrivate;

/* TRANSLATORS: Default zone set for firewall, when no zone is selected */
#define FIREWALL_ZONE_DEFAULT _("Default")
#define FIREWALL_ZONE_TOOLTIP_AVAILBALE _("The zone defines the trust level of the connection. Default is not a regular zone, selecting it results in the use of the default zone set in the firewall. Only usable if firewalld is active.")
#define FIREWALL_ZONE_TOOLTIP_UNAVAILBALE _("FirewallD is not running.")

enum {
	COL_ID,
	COL_UUID,
	N_COLUMNS
};

static void
general_private_init (CEPageGeneral *self)
{
	CEPageGeneralPrivate *priv = CE_PAGE_GENERAL_GET_PRIVATE (self);
	GtkBuilder *builder;
	GtkWidget *align;
	GtkLabel *label;

	builder = CE_PAGE (self)->builder;

	/*-- Firewall zone --*/
#if GTK_CHECK_VERSION(2,24,0)
	priv->firewall_zone = GTK_COMBO_BOX_TEXT (gtk_combo_box_text_new ());
#else
	priv->firewall_zone = GTK_COMBO_BOX (gtk_combo_box_new_text ());
#endif

	align = GTK_WIDGET (gtk_builder_get_object (builder, "firewall_zone_alignment"));
	gtk_container_add (GTK_CONTAINER (align), GTK_WIDGET (priv->firewall_zone));
	gtk_widget_show_all (GTK_WIDGET (priv->firewall_zone));

	/* Set mnemonic widget for device Firewall zone label */
	label = GTK_LABEL (GTK_WIDGET (gtk_builder_get_object (builder, "firewall_zone_label")));
	gtk_label_set_mnemonic_widget (label, GTK_WIDGET (priv->firewall_zone));

	/*-- Dependent VPN connection --*/
	priv->dependent_vpn_checkbox = GTK_TOGGLE_BUTTON (gtk_builder_get_object (builder, "dependent_vpn_checkbox"));
	priv->dependent_vpn = GTK_COMBO_BOX (gtk_builder_get_object (builder, "dependent_vpn_combo"));
	priv->dependent_vpn_store = GTK_LIST_STORE (gtk_builder_get_object (builder, "dependent_vpn_model"));
}

static void
dispose (GObject *object)
{
	CEPageGeneral *self = CE_PAGE_GENERAL (object);
	CEPageGeneralPrivate *priv = CE_PAGE_GENERAL_GET_PRIVATE (self);

	if (priv->remote_settings) {
		g_object_unref (priv->remote_settings);
		priv->remote_settings = NULL;
	}

	G_OBJECT_CLASS (ce_page_general_parent_class)->dispose (object);
}

static void
stuff_changed (GtkWidget *w, gpointer user_data)
{
	ce_page_changed (CE_PAGE (user_data));
}

static void
vpn_checkbox_toggled (GtkToggleButton *button, gpointer user_data)
{
	CEPageGeneralPrivate *priv = CE_PAGE_GENERAL_GET_PRIVATE (user_data);

	gtk_widget_set_sensitive (GTK_WIDGET (priv->dependent_vpn), gtk_toggle_button_get_active (priv->dependent_vpn_checkbox));
	ce_page_changed (CE_PAGE (user_data));
}

/* Get zones from firewalld */
static char **
get_zones_from_firewall (void)
{
	DBusGConnection *bus;
	GError *error = NULL;
	DBusGProxy *proxy;
	char **zones;

	bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (error || !bus) {
		g_message ("Getting zones from FirewallD not possible (failed to connect to D-Bus: %s).",
		           (error && error->message) ? error->message : "unknown");
		g_error_free (error);
	} else {
		proxy = dbus_g_proxy_new_for_name (bus,
		                                   "org.fedoraproject.FirewallD1",
		                                   "/org/fedoraproject/FirewallD1",
		                                   "org.fedoraproject.FirewallD1.zone");
		if (proxy) {
			/* get zones */
			if (!dbus_g_proxy_call (proxy, "getZones", &error,
			                        G_TYPE_INVALID,
			                        G_TYPE_STRV, &zones, G_TYPE_INVALID)) {
				g_warning ("Could not get zones from FirewallD: %s", error->message);
				g_error_free (error);
			} else
				return zones;

		} else {
			g_message ("FirewallD not available.");
		}
	}

	return NULL;
}

static void
populate_ui (CEPageGeneral *self)
{
	CEPageGeneralPrivate *priv = CE_PAGE_GENERAL_GET_PRIVATE (self);
	NMSettingConnection *setting = priv->setting;
	char **zones;
	char **zone_ptr;
	const char *s_zone, *vpn_uuid;
	guint32 combo_idx = 0, idx;
	GSList *con_list, *l;
	GtkTreeIter iter;

	s_zone = nm_setting_connection_get_zone (setting);
	
	/* Always add "fake" 'Default' zone for default firewall settings */
#if GTK_CHECK_VERSION (2,24,0)
	gtk_combo_box_text_append_text (priv->firewall_zone, FIREWALL_ZONE_DEFAULT);
#else
	gtk_combo_box_append_text (priv->firewall_zone, FIREWALL_ZONE_DEFAULT);
#endif

	/* Get zones from FirewallD and list them in the combo */
	zones = get_zones_from_firewall ();

	for (zone_ptr = zones, idx = 0; zone_ptr && *zone_ptr; zone_ptr++, idx++) {
#if GTK_CHECK_VERSION (2,24,0)
		gtk_combo_box_text_append_text (priv->firewall_zone, *zone_ptr);
#else
		gtk_combo_box_append_text (priv->firewall_zone, *zone_ptr);
#endif
		if (g_strcmp0 (s_zone, *zone_ptr) == 0)
			combo_idx = idx + 1;
	}

	if (s_zone && combo_idx == 0) {
		/* Unknown zone in connection setting - add it to combobox */
#if GTK_CHECK_VERSION (2,24,0)
		gtk_combo_box_text_append_text (priv->firewall_zone, s_zone);
#else
		gtk_combo_box_append_text (priv->firewall_zone, s_zone);
#endif
		combo_idx = idx + 1;
	}
	gtk_combo_box_set_active (GTK_COMBO_BOX (priv->firewall_zone), combo_idx);

	/* Zone tooltip and availability */
	if (zones) {
		gtk_widget_set_tooltip_text (GTK_WIDGET (priv->firewall_zone), FIREWALL_ZONE_TOOLTIP_AVAILBALE);
		gtk_widget_set_sensitive (GTK_WIDGET (priv->firewall_zone), TRUE);
	} else {
		gtk_widget_set_tooltip_text (GTK_WIDGET (priv->firewall_zone), FIREWALL_ZONE_TOOLTIP_UNAVAILBALE);
		gtk_widget_set_sensitive (GTK_WIDGET (priv->firewall_zone), FALSE);
	}
	g_strfreev (zones);

	/* Secondary UUID (VPN) */
	vpn_uuid = nm_setting_connection_get_secondary (setting, 0);
	con_list = nm_remote_settings_list_connections (priv->remote_settings);
	for (l = con_list, idx = 0, combo_idx = 0; l; l = l->next) {
		const char *uuid = nm_connection_get_uuid (l->data);
		const char *id = nm_connection_get_id (l->data);

		if (!nm_connection_is_type (l->data, NM_SETTING_VPN_SETTING_NAME))
			continue;

		gtk_list_store_append (priv->dependent_vpn_store, &iter);
		gtk_list_store_set (priv->dependent_vpn_store, &iter, COL_ID, id, COL_UUID, uuid, -1);
		if (g_strcmp0 (vpn_uuid, uuid) == 0)
			combo_idx = idx;
		idx++;
	}
	g_slist_free (con_list);
	gtk_combo_box_set_active (GTK_COMBO_BOX (priv->dependent_vpn), combo_idx);

	stuff_changed (NULL, self);
}

static void
finish_setup (CEPageGeneral *self, gpointer unused, GError *error, gpointer user_data)
{
	CEPageGeneralPrivate *priv = CE_PAGE_GENERAL_GET_PRIVATE (self);
	gboolean any_dependent_vpn;

	if (error)
		return;

	populate_ui (self);

	g_signal_connect (priv->firewall_zone, "changed", G_CALLBACK (stuff_changed), self);

	any_dependent_vpn = !!nm_setting_connection_get_num_secondaries (priv->setting);
	gtk_toggle_button_set_active (priv->dependent_vpn_checkbox, any_dependent_vpn);
	g_signal_connect (priv->dependent_vpn_checkbox, "toggled", G_CALLBACK (vpn_checkbox_toggled), self);
	gtk_widget_set_sensitive (GTK_WIDGET (priv->dependent_vpn), any_dependent_vpn);
	g_signal_connect (priv->dependent_vpn, "changed", G_CALLBACK (stuff_changed), self);
}

CEPage *
ce_page_general_new (NMConnection *connection,
                     GtkWindow *parent_window,
                     NMClient *client,
                     NMRemoteSettings *settings,
                     const char **out_secrets_setting_name,
                     GError **error)
{
	CEPageGeneral *self;
	CEPageGeneralPrivate *priv;

	self = CE_PAGE_GENERAL (ce_page_new (CE_TYPE_PAGE_GENERAL,
	                                     connection,
	                                     parent_window,
	                                     client,
	                                     settings,
	                                     UIDIR "/ce-page-general.ui",
	                                     "GeneralPage",
	                                     _("General")));
	if (!self) {
		g_set_error_literal (error, NMA_ERROR, NMA_ERROR_GENERIC,
		                     _("Could not load General user interface."));
		return NULL;
	}

	general_private_init (self);
	priv = CE_PAGE_GENERAL_GET_PRIVATE (self);

	priv->remote_settings = g_object_ref (settings);

	priv->setting = nm_connection_get_setting_connection (connection);
	if (!priv->setting) {
		priv->setting = NM_SETTING_CONNECTION (nm_setting_connection_new ());
		nm_connection_add_setting (connection, NM_SETTING (priv->setting));
	}

	g_signal_connect (self, "initialized", G_CALLBACK (finish_setup), NULL);

	return CE_PAGE (self);
}

static void
ui_to_setting (CEPageGeneral *self)
{
	CEPageGeneralPrivate *priv = CE_PAGE_GENERAL_GET_PRIVATE (self);
	char *zone;
	char *uuid = NULL;
	GtkTreeIter iter;

#if GTK_CHECK_VERSION (2,24,0)
	zone = gtk_combo_box_text_get_active_text (priv->firewall_zone);
#else
	zone = gtk_combo_box_get_active_text (priv->firewall_zone);
#endif

	if (g_strcmp0 (zone, FIREWALL_ZONE_DEFAULT) == 0)
		zone = NULL;
	g_object_set (priv->setting, NM_SETTING_CONNECTION_ZONE, zone, NULL);

	g_free (zone);

	if (   gtk_toggle_button_get_active (priv->dependent_vpn_checkbox)
	    && gtk_combo_box_get_active_iter (priv->dependent_vpn, &iter))
		gtk_tree_model_get (GTK_TREE_MODEL (priv->dependent_vpn_store), &iter,
		                                    COL_UUID, &uuid, -1);

	g_object_set (G_OBJECT (priv->setting), NM_SETTING_CONNECTION_SECONDARIES, NULL, NULL);
	if (uuid)
		nm_setting_connection_add_secondary (priv->setting, uuid);
	g_free (uuid);
}

static gboolean
validate (CEPage *page, NMConnection *connection, GError **error)
{
	CEPageGeneral *self = CE_PAGE_GENERAL (page);
	CEPageGeneralPrivate *priv = CE_PAGE_GENERAL_GET_PRIVATE (self);

	ui_to_setting (self);
	return nm_setting_verify (NM_SETTING (priv->setting), NULL, error);
}

static void
ce_page_general_init (CEPageGeneral *self)
{
}

static void
ce_page_general_class_init (CEPageGeneralClass *connection_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (connection_class);
	CEPageClass *parent_class = CE_PAGE_CLASS (connection_class);

	g_type_class_add_private (object_class, sizeof (CEPageGeneralPrivate));

	/* virtual methods */
	object_class->dispose = dispose;

	parent_class->validate = validate;
}

