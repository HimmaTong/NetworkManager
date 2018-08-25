/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */

/* NetworkManager system settings service (ifupdown)
 *
 * Alexander Sack <asac@ubuntu.com>
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
 * (C) Copyright 2007,2008 Canonical Ltd.
 * (C) Copyright 2009 - 2011 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nms-ifupdown-plugin.h"

#include <string.h>
#include <arpa/inet.h>
#include <gmodule.h>
#include <libudev.h>

#include "nm-setting-connection.h"
#include "nm-dbus-interface.h"
#include "settings/nm-settings-plugin.h"
#include "nm-setting-ip4-config.h"
#include "nm-setting-wireless.h"
#include "nm-setting-wired.h"
#include "nm-setting-ppp.h"
#include "nm-utils.h"
#include "nm-core-internal.h"
#include "NetworkManagerUtils.h"
#include "nm-config.h"
#include "nm-utils/nm-udev-utils.h"

#include "nms-ifupdown-interface-parser.h"
#include "nms-ifupdown-connection.h"
#include "nms-ifupdown-parser.h"

#define ENI_INTERFACES_FILE "/etc/network/interfaces"

#define IFUPDOWN_UNMANAGE_WELL_KNOWN_DEFAULT TRUE

/*****************************************************************************/

typedef struct {
	NMUdevClient *udev_client;

	GHashTable *connections;  /* /e/n/i block name :: NMIfupdownConnection */

	/* Stores all blocks/interfaces read from /e/n/i regardless of whether
	 * there is an NMIfupdownConnection for block.
	 */
	GHashTable *eni_ifaces;

	/* Stores any network interfaces the kernel knows about */
	GHashTable *kernel_ifaces;

	bool ifupdown_managed;
} SettingsPluginIfupdownPrivate;

struct _SettingsPluginIfupdown {
	NMSettingsPlugin parent;
	SettingsPluginIfupdownPrivate _priv;
};

struct _SettingsPluginIfupdownClass {
	NMSettingsPluginClass parent;
};

G_DEFINE_TYPE (SettingsPluginIfupdown, settings_plugin_ifupdown, NM_TYPE_SETTINGS_PLUGIN)

#define SETTINGS_PLUGIN_IFUPDOWN_GET_PRIVATE(self) _NM_GET_PRIVATE (self, SettingsPluginIfupdown, SETTINGS_IS_PLUGIN_IFUPDOWN)

/*****************************************************************************/

static SettingsPluginIfupdown *settings_plugin_ifupdown_get (void);
NM_DEFINE_SINGLETON_GETTER (SettingsPluginIfupdown, settings_plugin_ifupdown_get, SETTINGS_TYPE_PLUGIN_IFUPDOWN);

/*****************************************************************************/

#define _NMLOG_PREFIX_NAME      "ifupdown"
#define _NMLOG_DOMAIN           LOGD_SETTINGS
#define _NMLOG(level, ...) \
    nm_log ((level), _NMLOG_DOMAIN, NULL, NULL, \
            "%s" _NM_UTILS_MACRO_FIRST (__VA_ARGS__), \
            _NMLOG_PREFIX_NAME": " \
            _NM_UTILS_MACRO_REST (__VA_ARGS__))

/*****************************************************************************/

static void
bind_device_to_connection (SettingsPluginIfupdown *self,
                           struct udev_device *device,
                           NMIfupdownConnection *exported)
{
	NMSettingWired *s_wired;
	NMSettingWireless *s_wifi;
	const char *iface, *address;

	iface = udev_device_get_sysname (device);
	if (!iface) {
		_LOGD ("bind-to-connection: failed to get ifname for device.");
		return;
	}

	address = udev_device_get_sysattr_value (device, "address");
	if (!address || !address[0]) {
		_LOGD ("bind-to-connection: failed to get MAC address for %s", iface);
		return;
	}

	if (!nm_utils_hwaddr_valid (address, ETH_ALEN)) {
		_LOGD ("bind-to-connection: failed to parse MAC address '%s' for %s",
		       address, iface);
		return;
	}

	s_wired = nm_connection_get_setting_wired (nm_settings_connection_get_connection (NM_SETTINGS_CONNECTION (exported)));
	s_wifi = nm_connection_get_setting_wireless (nm_settings_connection_get_connection (NM_SETTINGS_CONNECTION (exported)));
	if (s_wired) {
		_LOGD ("bind-to-connection: locking wired connection setting");
		g_object_set (s_wired, NM_SETTING_WIRED_MAC_ADDRESS, address, NULL);
	} else if (s_wifi) {
		_LOGD ("bind-to-connection: locking wireless connection setting");
		g_object_set (s_wifi, NM_SETTING_WIRELESS_MAC_ADDRESS, address, NULL);
	}

	nm_settings_connection_update (NM_SETTINGS_CONNECTION (exported),
	                               NULL,
	                               NM_SETTINGS_CONNECTION_PERSIST_MODE_DISK,
	                               NM_SETTINGS_CONNECTION_COMMIT_REASON_NONE,
	                               "ifupdown-new",
	                               NULL);
}

static void
udev_device_added (SettingsPluginIfupdown *self, struct udev_device *device)
{
	SettingsPluginIfupdownPrivate *priv = SETTINGS_PLUGIN_IFUPDOWN_GET_PRIVATE (self);
	const char *iface, *path;
	NMIfupdownConnection *exported;

	iface = udev_device_get_sysname (device);
	path = udev_device_get_syspath (device);
	if (!iface || !path)
		return;

	_LOGD ("udev: devices added (path: %s, iface: %s)", path, iface);

	/* if we have a configured connection for this particular iface
	 * we want to either unmanage the device or lock it
	 */
	exported = g_hash_table_lookup (priv->connections, iface);
	if (   !exported
	    && !g_hash_table_contains (priv->eni_ifaces, iface)) {
		_LOGD ("udev: device added (path: %s, iface: %s): no ifupdown configuration found.",
		       path, iface);
		return;
	}

	g_hash_table_insert (priv->kernel_ifaces, g_strdup (iface), udev_device_ref (device));

	if (exported)
		bind_device_to_connection (self, device, exported);

	if (!priv->ifupdown_managed)
		_nm_settings_plugin_emit_signal_unmanaged_specs_changed (NM_SETTINGS_PLUGIN (self));
}

static void
udev_device_removed (SettingsPluginIfupdown *self, struct udev_device *device)
{
	SettingsPluginIfupdownPrivate *priv = SETTINGS_PLUGIN_IFUPDOWN_GET_PRIVATE (self);
	const char *iface, *path;

	iface = udev_device_get_sysname (device);
	path = udev_device_get_syspath (device);
	if (!iface || !path)
		return;

	_LOGD ("udev: devices removed (path: %s, iface: %s)", path, iface);

	if (!g_hash_table_remove (priv->kernel_ifaces, iface))
		return;

	if (!priv->ifupdown_managed)
		_nm_settings_plugin_emit_signal_unmanaged_specs_changed (NM_SETTINGS_PLUGIN (self));
}

static void
udev_device_changed (SettingsPluginIfupdown *self, struct udev_device *device)
{
	SettingsPluginIfupdownPrivate *priv = SETTINGS_PLUGIN_IFUPDOWN_GET_PRIVATE (self);
	const char *iface, *path;

	iface = udev_device_get_sysname (device);
	path = udev_device_get_syspath (device);
	if (!iface || !path)
		return;

	_LOGD ("udev: device changed (path: %s, iface: %s)", path, iface);

	if (!g_hash_table_lookup (priv->kernel_ifaces, iface))
		return;

	if (!priv->ifupdown_managed)
		_nm_settings_plugin_emit_signal_unmanaged_specs_changed (NM_SETTINGS_PLUGIN (self));
}

static void
handle_uevent (NMUdevClient *client,
               struct udev_device *device,
               gpointer user_data)
{
	SettingsPluginIfupdown *self = SETTINGS_PLUGIN_IFUPDOWN (user_data);
	const char *subsys;
	const char *action;

	action = udev_device_get_action (device);

	g_return_if_fail (action != NULL);

	/* A bit paranoid */
	subsys = udev_device_get_subsystem (device);
	g_return_if_fail (nm_streq0 (subsys, "net"));

	if (nm_streq (action, "add"))
		udev_device_added (self, device);
	else if (nm_streq (action, "remove"))
		udev_device_removed (self, device);
	else if (nm_streq (action, "change"))
		udev_device_changed (self, device);
}

/* Returns the plugins currently known list of connections.  The returned
 * list is freed by the system settings service.
 */
static GSList*
get_connections (NMSettingsPlugin *plugin)
{
	SettingsPluginIfupdown *self = SETTINGS_PLUGIN_IFUPDOWN (plugin);
	SettingsPluginIfupdownPrivate *priv = SETTINGS_PLUGIN_IFUPDOWN_GET_PRIVATE (self);

	if (!priv->ifupdown_managed) {
		_LOGD ("get_connections: not connections due to managed=false");
		return NULL;
	}

	_LOGD ("get_connections: %u connections", g_hash_table_size (priv->connections));
	return _nm_utils_hash_values_to_slist (priv->connections);
}

/*
 * Return a list of device specifications which NetworkManager should not
 * manage.  Returned list will be freed by the system settings service, and
 * each element must be allocated using g_malloc() or its variants.
 */
static GSList*
get_unmanaged_specs (NMSettingsPlugin *plugin)
{
	SettingsPluginIfupdown *self = SETTINGS_PLUGIN_IFUPDOWN (plugin);
	SettingsPluginIfupdownPrivate *priv = SETTINGS_PLUGIN_IFUPDOWN_GET_PRIVATE (self);
	GSList *specs = NULL;
	GHashTableIter iter;
	struct udev_device *device;
	const char *iface;

	if (priv->ifupdown_managed)
		return NULL;

	_LOGD ("unmanaged-specs: unmanaged devices count %u",
	       g_hash_table_size (priv->kernel_ifaces));

	g_hash_table_iter_init (&iter, priv->kernel_ifaces);
	while (g_hash_table_iter_next (&iter, (gpointer) &iface, (gpointer) &device)) {
		const char *address;

		address = udev_device_get_sysattr_value (device, "address");
		if (address)
			specs = g_slist_append (specs, g_strdup_printf ("mac:%s", address));
		else
			specs = g_slist_append (specs, g_strdup_printf ("interface-name:%s", iface));
	}
	return specs;
}

/*****************************************************************************/

static void
_udev_device_unref (gpointer ptr)
{
	udev_device_unref (ptr);
}

static void
initialize (NMSettingsPlugin *plugin)
{
	SettingsPluginIfupdown *self = SETTINGS_PLUGIN_IFUPDOWN (plugin);
	SettingsPluginIfupdownPrivate *priv = SETTINGS_PLUGIN_IFUPDOWN_GET_PRIVATE (self);
	gs_unref_hashtable GHashTable *auto_ifaces = NULL;
	if_block *block = NULL;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *keys;
	GHashTableIter con_iter;
	const char *block_name;
	NMIfupdownConnection *connection;

	priv->udev_client = nm_udev_client_new ((const char *[]) { "net", NULL },
	                                        handle_uevent, self);

	/* Read in all the interfaces */
	ifparser_init (ENI_INTERFACES_FILE, 0);
	for (block = ifparser_getfirst (); block; block = block->next) {

		if (NM_IN_STRSET (block->type, "auto", "allow-hotplug")) {
			if (!auto_ifaces)
				auto_ifaces = g_hash_table_new_full (nm_str_hash, g_str_equal, g_free, NULL);
			g_hash_table_add (auto_ifaces, g_strdup (block->name));
			continue;
		}

		if (nm_streq (block->type, "iface")) {
			NMIfupdownConnection *exported;

			/* Bridge configuration */
			if (g_str_has_prefix (block_name, "br")) {
				/* Try to find bridge ports */
				const char *ports = ifparser_getkey (block, "bridge-ports");

				if (ports) {
					guint i;
					int state = 0;
					gs_strfreev char **port_ifaces = NULL;

					_LOGD ("found bridge ports %s for %s", ports, block->name);

					port_ifaces = g_strsplit_set (ports, " \t", -1);
					for (i = 0; port_ifaces[i]; i++) {
						const char *token = port_ifaces[i];

						/* Skip crazy stuff like regex or all */
						if (nm_streq (token, "all"))
							continue;

						/* Small SM to skip everything inside regex */
						if (nm_streq (token, "regex")) {
							state++;
							continue;
						}
						if (nm_streq (token, "noregex")) {
							state--;
							continue;
						}
						if (nm_streq (token, "none"))
							continue;
						if (state == 0 && strlen (token) > 0) {
							_LOGD ("adding bridge port %s to eni_ifaces", token);
							g_hash_table_add (priv->eni_ifaces, g_strdup (token));
						}
					}
				}
				continue;
			}

			/* Skip loopback configuration */
			if (nm_streq (block->name, "lo"))
				continue;

			/* Remove any connection for this block that was previously found */
			exported = g_hash_table_lookup (priv->connections, block->name);
			if (exported) {
				_LOGD ("deleting %s from connections", block->name);
				nm_settings_connection_delete (NM_SETTINGS_CONNECTION (exported), NULL);
				g_hash_table_remove (priv->connections, block->name);
			}

			/* add the new connection */
			exported = nm_ifupdown_connection_new (block);
			if (exported) {
				_LOGD ("adding %s to connections", block->name);
				g_hash_table_insert (priv->connections, g_strdup (block->name), exported);
			}
			_LOGD ("adding iface %s to eni_ifaces", block->name);
			g_hash_table_add (priv->eni_ifaces, g_strdup (block->name));
			continue;
		}

		if (nm_streq (block->type, "mapping")) {
			g_hash_table_add (priv->eni_ifaces, g_strdup (block->name));
			_LOGD ("adding mapping %s to eni_ifaces", block->name);
			continue;
		}

		/* unhandled. */
	}

	/* Make 'auto' interfaces autoconnect=TRUE */
	g_hash_table_iter_init (&con_iter, priv->connections);
	while (g_hash_table_iter_next (&con_iter, (gpointer) &block_name, (gpointer) &connection)) {
		NMSettingConnection *setting;

		if (   !auto_ifaces
		    || !g_hash_table_contains (auto_ifaces, block_name))
			continue;

		/* FIXME(copy-on-write-connection): avoid modifying NMConnection instances and share them via copy-on-write. */
		setting = nm_connection_get_setting_connection (nm_settings_connection_get_connection (NM_SETTINGS_CONNECTION (connection)));
		g_object_set (setting, NM_SETTING_CONNECTION_AUTOCONNECT, TRUE, NULL);
	}

	/* Check the config file to find out whether to manage interfaces */
	priv->ifupdown_managed = nm_config_data_get_value_boolean (NM_CONFIG_GET_DATA_ORIG,
	                                                           NM_CONFIG_KEYFILE_GROUP_IFUPDOWN,
	                                                           NM_CONFIG_KEYFILE_KEY_IFUPDOWN_MANAGED,
	                                                           !IFUPDOWN_UNMANAGE_WELL_KNOWN_DEFAULT);
	_LOGI ("management mode: %s", priv->ifupdown_managed ? "managed" : "unmanaged");

	/* Add well-known interfaces */
	enumerate = nm_udev_client_enumerate_new (priv->udev_client);
	udev_enumerate_scan_devices (enumerate);
	keys = udev_enumerate_get_list_entry (enumerate);
	for (; keys; keys = udev_list_entry_get_next (keys)) {
		struct udev_device *udevice;

		udevice = udev_device_new_from_syspath (udev_enumerate_get_udev (enumerate),
		                                        udev_list_entry_get_name (keys));
		if (udevice) {
			udev_device_added (self, udevice);
			udev_device_unref (udevice);
		}
	}
	udev_enumerate_unref (enumerate);

	/* Now if we're running in managed mode, let NM know there are new connections */
	if (priv->ifupdown_managed) {
		GList *con_list = g_hash_table_get_values (priv->connections);
		GList *cl_iter;

		for (cl_iter = con_list; cl_iter; cl_iter = g_list_next (cl_iter)) {
			_nm_settings_plugin_emit_signal_connection_added (NM_SETTINGS_PLUGIN (self),
			                                                  NM_SETTINGS_CONNECTION (cl_iter->data));
		}
		g_list_free (con_list);
	}
}

/*****************************************************************************/

static void
settings_plugin_ifupdown_init (SettingsPluginIfupdown *self)
{
	SettingsPluginIfupdownPrivate *priv = SETTINGS_PLUGIN_IFUPDOWN_GET_PRIVATE (self);

	priv->connections = g_hash_table_new_full (nm_str_hash, g_str_equal, g_free, g_object_unref);
	priv->kernel_ifaces = g_hash_table_new_full (nm_str_hash, g_str_equal, g_free, _udev_device_unref);
	priv->eni_ifaces = g_hash_table_new_full (nm_str_hash, g_str_equal, g_free, NULL);
}

static void
dispose (GObject *object)
{
	SettingsPluginIfupdown *plugin = SETTINGS_PLUGIN_IFUPDOWN (object);
	SettingsPluginIfupdownPrivate *priv = SETTINGS_PLUGIN_IFUPDOWN_GET_PRIVATE (plugin);

	g_clear_pointer (&priv->connections, g_hash_table_destroy);
	g_clear_pointer (&priv->kernel_ifaces, g_hash_table_destroy);
	g_clear_pointer (&priv->eni_ifaces, g_hash_table_destroy);

	priv->udev_client = nm_udev_client_unref (priv->udev_client);

	G_OBJECT_CLASS (settings_plugin_ifupdown_parent_class)->dispose (object);
}

static void
settings_plugin_ifupdown_class_init (SettingsPluginIfupdownClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMSettingsPluginClass *plugin_class = NM_SETTINGS_PLUGIN_CLASS (klass);

	object_class->dispose = dispose;

	plugin_class->initialize          = initialize;
	plugin_class->get_connections     = get_connections;
	plugin_class->get_unmanaged_specs = get_unmanaged_specs;
}

/*****************************************************************************/

G_MODULE_EXPORT NMSettingsPlugin *
nm_settings_plugin_factory (void)
{
	return NM_SETTINGS_PLUGIN (g_object_ref (settings_plugin_ifupdown_get ()));
}
