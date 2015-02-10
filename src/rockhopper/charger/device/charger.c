/* @@@LICENSE
*
*      Copyright (c) 2010-2012 Hewlett-Packard Development Company, L.P.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

/**
 * @file charger.c
 */

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>

#include <glib.h>
#include <libudev.h>

GIOChannel *channel;

struct udev *udev;
struct udev_monitor *mon;

extern nyx_device_t *nyxDev;
extern void *charger_status_callback_context;
extern nyx_device_callback_function_t charger_status_callback;

#ifndef CHARGER_USB_SYSFS_PATH
#define CHARGER_USB_SYSFS_PATH		"/sys/class/power_supply/usb/"
#endif

#ifndef CHARGER_AC_SYSFS_PATH
#define CHARGER_AC_SYSFS_PATH		"/sys/class/power_supply/ac/"
#endif

nyx_charger_status_t gChargerStatus =
{
	.charger_max_current = 0,
	.connected = 0,
	.powered = 0,
	.dock_serial_number = {0},
};

static nyx_charger_event_t current_event = NYX_NO_NEW_EVENT;
static guint event_watch;

gboolean _handle_power_supply_event(GIOChannel *channel, GIOCondition condition, gpointer data)
{
	struct udev_device *dev;

	if ((condition  & G_IO_IN) == G_IO_IN) {
		dev = udev_monitor_receive_device(mon);
		if (dev) {
			/* something related to power supply has changed; notify connected clients so
			 * they can query the new status */
			charger_status_callback(nyxDev, NYX_CALLBACK_STATUS_DONE, charger_status_callback_context);
		}
	}

	return TRUE;
}

nyx_error_t _charger_init(void)
{
	int fd;

	udev = udev_new();
	if (!udev) {
		nyx_error("Could not initialize udev component; battery status updates will not be available");
		return;
	}

	mon = udev_monitor_new_from_netlink(udev, "kernel");
	udev_monitor_filter_add_match_subsystem_devtype(mon, "power_supply", NULL);
	udev_monitor_enable_receiving(mon);
	fd = udev_monitor_get_fd(mon);

	channel = g_io_channel_unix_new(fd);
	event_watch = g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_NVAL, _handle_power_supply_event, NULL);

	return NYX_ERROR_NONE;
}

void _charger_close(void)
{
	if (channel) {
		g_io_channel_shutdown(channel, NULL, NULL);
		channel = NULL;

		g_source_remove(event_watch);
	}

	if (mon) {
		udev_monitor_unref(mon);
	}

	if (udev) {
		udev_unref(udev);
	}
}

nyx_error_t _charger_read_status(nyx_charger_status_t *status)
{
	int32_t online;

	/* before we start to update the charger status we reset it completely */
	memset(&gChargerStatus, 0, sizeof(nyx_charger_status_t));

	if (nyx_utils_read_value(CHARGER_USB_SYSFS_PATH "online")) {
		gChargerStatus.connected |= NYX_CHARGER_PC_CONNECTED;
		gChargerStatus.powered |= NYX_CHARGER_USB_POWERED;
		gChargerStatus.is_charging = 1;
	}

	if (nyx_utils_read_value(CHARGER_AC_SYSFS_PATH "online")) {
		gChargerStatus.connected |= NYX_CHARGER_WALL_CONNECTED;
		gChargerStatus.powered |= NYX_CHARGER_DIRECT_POWERED;
		gChargerStatus.is_charging = 1;
	}

	memcpy(status, &gChargerStatus, sizeof(nyx_charger_status_t));

	return NYX_ERROR_NONE;
}

nyx_error_t _charger_enable_charging(nyx_charger_status_t *status)
{
	memcpy(status, &gChargerStatus, sizeof(nyx_charger_status_t));

	return NYX_ERROR_NONE;
}

nyx_error_t _charger_disable_charging(nyx_charger_status_t *status)
{
	memcpy(status, &gChargerStatus, sizeof(nyx_charger_status_t));

	return NYX_ERROR_NONE;
}

nyx_error_t _charger_query_charger_event(nyx_charger_event_t *event)
{
	*event = current_event;

	current_event = NYX_NO_NEW_EVENT;

	return NYX_ERROR_NONE;
}
