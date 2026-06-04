// Copyright (c) 2014-2018 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

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
#include <time.h>
#include <libudev.h>
#include <utils.h>

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>
#include "msgid.h"

#define STATUS_LEN 64
#define PATH_LEN 128

struct udev *udev = NULL;
struct udev_monitor *mon = NULL;
guint watch = 0;

extern nyx_device_t *nyxDev;
extern void *charger_status_callback_context;
extern void *state_change_callback_context;
extern nyx_device_callback_function_t charger_status_callback;
extern nyx_device_callback_function_t state_change_callback;

nyx_battery_status_t *curr_battery_state = NULL;
char *battery_status = NULL;

char batt_present_path[PATH_LEN] = {0,};
char batt_status_path[PATH_LEN] = {0,};
char charger_usb_sysfs_online_path[PATH_LEN] = {0,};
char charger_usb_sysfs_usb_type_path[PATH_LEN] = {0,};
char charger_usb_sysfs_current_max_path[PATH_LEN] = {0,};
char charger_usb_sysfs_vendor_variant_path[PATH_LEN] = {0,};
char charger_ac_sysfs_online_path[PATH_LEN] = {0,};
char charger_ac_sysfs_current_max_path[PATH_LEN] = {0,};
char charger_touch_sysfs_online_path[PATH_LEN] = {0,};
char charger_wireless_sysfs_online_path[PATH_LEN] = {0,};

/* Parsed value from /sys/class/power_supply/<usb>/vendor_charger_variant.
 * Empty string when no HP/Palm vendor variant is reported.  Stored in
 * gChargerStatus.dock_serial_number so existing batteryd payloads (which
 * already echo this field) carry the variant identity through to luna
 * clients without needing a new struct field.
 */

static nyx_charger_event_t current_event = NYX_NO_NEW_EVENT;
nyx_charger_status_t gChargerStatus =
{
	.charger_max_current = 0,
	.connected = 0,
	.powered = 0,
	.dock_serial_number = {0},
	.is_charging = false,
};

/*
 * _parse_usb_type_bracketed - extract the bracketed token from
 * power_supply usb_type sysfs output (e.g. "Unknown SDP [DCP] CDP").
 *
 * Returns a pointer into the supplied buffer (NUL-terminated, with the
 * closing bracket overwritten with '\0') or NULL if no bracketed token
 * was found.  The buffer is mutated in place.
 */
static char *_parse_usb_type_bracketed(char *buf)
{
	char *start = strchr(buf, '[');
	char *end;

	if (!start)
	{
		return NULL;
	}

	end = strchr(start, ']');

	if (!end)
	{
		return NULL;
	}

	*end = '\0';
	return start + 1;
}

nyx_error_t core_charger_read_status(nyx_charger_status_t *status)
{
	bool usb_online;
	bool ac_online;
	bool touch_online;
	bool wireless_online;
	int  current_max_ua = 0;
	char usb_type_buf[64] = {0};
	char variant_buf[NYX_DOCK_SERIAL_NUMBER_LEN] = {0};
	char num_buf[32] = {0};
	char *active_type;

	/* before we start to update the charger status we reset it completely */
	memset(&gChargerStatus, 0, sizeof(nyx_charger_status_t));

	usb_online      = (nyx_utils_read_value(charger_usb_sysfs_online_path) == 1);
	ac_online       = (nyx_utils_read_value(charger_ac_sysfs_online_path) == 1);
	touch_online    = (nyx_utils_read_value(charger_touch_sysfs_online_path) == 1);
	wireless_online = (nyx_utils_read_value(charger_wireless_sysfs_online_path) == 1);

	if (usb_online)
	{
		/*
		 * If the USB power_supply driver classifies the source via
		 * BC 1.2 (SDP/CDP/DCP), use that to drive the Nyx connection
		 * type and pull the negotiated current limit from current_max.
		 * Fall back to the legacy "any USB power_supply online =
		 * PC_CONNECTED" behaviour when usb_type / current_max are
		 * absent (matches older drivers that only expose /online).
		 */
		gChargerStatus.powered |= NYX_CHARGER_USB_POWERED;

		if (charger_usb_sysfs_usb_type_path[0] &&
		    FileGetString(charger_usb_sysfs_usb_type_path, usb_type_buf,
		                  sizeof(usb_type_buf)) != -1 &&
		    (active_type = _parse_usb_type_bracketed(usb_type_buf)) != NULL)
		{
			if (strcmp(active_type, "DCP") == 0)
			{
				/* Dedicated wall charger via USB cable */
				gChargerStatus.connected |= NYX_CHARGER_WALL_CONNECTED;
			}
			else /* SDP, CDP, Unknown -- treat as host-class */
			{
				gChargerStatus.connected |= NYX_CHARGER_PC_CONNECTED;
			}
		}
		else
		{
			gChargerStatus.connected |= NYX_CHARGER_PC_CONNECTED;
		}

		/*
		 * Pull the negotiated max current (microamps in sysfs, mA in
		 * the Nyx struct).  Wall chargers via USB cable report up to
		 * 2000 mA via HP-variant detection; standard SDP reports 500.
		 */
		if (charger_usb_sysfs_current_max_path[0] &&
		    FileGetString(charger_usb_sysfs_current_max_path, num_buf,
		                  sizeof(num_buf)) != -1)
		{
			current_max_ua = atoi(num_buf);
			if (current_max_ua > 0)
			{
				gChargerStatus.charger_max_current = current_max_ua / 1000;
			}
		}

		/*
		 * Optional HP/Palm vendor variant string -- carried through to
		 * userspace as the chargerStatus payload's "USBName" field via
		 * the existing dock_serial_number plumbing in batteryd.
		 */
		if (charger_usb_sysfs_vendor_variant_path[0] &&
		    FileGetString(charger_usb_sysfs_vendor_variant_path, variant_buf,
		                  sizeof(variant_buf)) != -1 &&
		    variant_buf[0] != '\0')
		{
			strncpy(gChargerStatus.dock_serial_number, variant_buf,
			        sizeof(gChargerStatus.dock_serial_number) - 1);
		}
	}
	else if (ac_online)
	{
		/*
		 * Wired Mains-class supply (e.g. max8903 via Touchstone
		 * inductive input).  Drop NYX_CHARGER_DIRECT_POWERED here as a
		 * best-effort indicator; userspace separately reads the
		 * "Touch" power_supply (if present) for the wireless flag.
		 */
		gChargerStatus.connected |= NYX_CHARGER_WALL_CONNECTED;
		gChargerStatus.powered   |= NYX_CHARGER_DIRECT_POWERED;

		if (charger_ac_sysfs_current_max_path[0] &&
		    FileGetString(charger_ac_sysfs_current_max_path, num_buf,
		                  sizeof(num_buf)) != -1)
		{
			current_max_ua = atoi(num_buf);
			if (current_max_ua > 0)
			{
				gChargerStatus.charger_max_current = current_max_ua / 1000;
			}
		}
	}

	gChargerStatus.is_charging = usb_online || ac_online ||
	                             touch_online || wireless_online;

	if (status)
	{
		memcpy(status, &gChargerStatus, sizeof(nyx_charger_status_t));
	}

	return NYX_ERROR_NONE;
}

void _battery_read_status()
{
	if (curr_battery_state && battery_status)
	{
		memset(curr_battery_state, 0, sizeof(nyx_battery_status_t));
		memset(battery_status, 0, sizeof(battery_status));
		char status[STATUS_LEN];

		curr_battery_state->present = ((nyx_utils_read_value(batt_present_path)) == 1) ?
		                              true : false;

		if (FileGetString(batt_status_path, status, STATUS_LEN) != -1)
		{
			strcpy(battery_status, status);
		}
	}
}

bool _has_charger_state_changed(char *old_state, char *new_state)
{
	if (new_state && !old_state && (strcmp(new_state, "Full") == 0))
	{
		current_event &= ~NYX_CHARGE_RESTART;
		current_event |= NYX_CHARGE_COMPLETE;
		return true;
	}

	if (old_state && new_state && (strcmp(old_state, new_state) != 0))
	{
		if ((strcmp(old_state, "Charging") == 0) && (strcmp(new_state, "Full") == 0))
		{
			current_event &= ~NYX_CHARGE_RESTART;
			current_event |= NYX_CHARGE_COMPLETE;
		}
		else if ((strcmp(old_state, "Full") == 0) &&
		         (strcmp(new_state, "Charging") == 0))
		{
			current_event &= ~NYX_CHARGE_COMPLETE;
			current_event |= NYX_CHARGE_RESTART;
		}
		else
		{
			return false;
		}

		return true;
	}

	return false;
}

bool _has_battery_state_changed(int old_state, int new_state)
{
	if (old_state != new_state)
	{
		if (new_state)
		{
			current_event &= ~NYX_BATTERY_ABSENT;
			current_event |= NYX_BATTERY_PRESENT;
		}
		else
		{
			current_event &= ~NYX_BATTERY_PRESENT;
			current_event |= NYX_BATTERY_ABSENT;
		}

		return true;
	}

	return false;
}

bool _has_charger_connected_state_changed(bool old_state, bool new_state)
{
	if (old_state != new_state)
	{
		if (new_state)
		{
			current_event &= ~NYX_CHARGER_DISCONNECTED;
			current_event |= NYX_CHARGER_CONNECTED;
		}
		else
		{
			current_event &= ~NYX_CHARGER_CONNECTED;
			current_event |= NYX_CHARGER_DISCONNECTED;
		}

		return true;
	}

	return false;
}

gboolean _handle_power_supply_event(GIOChannel *channel, GIOCondition condition,
                                    gpointer data)
{
	struct udev_device *dev;
	bool fire_charger_status_cb = false;
	bool fire_state_change_cb = false;

	if ((condition & G_IO_IN) == G_IO_IN)
	{
		dev = udev_monitor_receive_device(mon);

		if (dev)
		{
			/* something related to power supply has changed; set the modified event and notify connected clients so
			 * they can query the new status */

			/* Check for event changes and initiate state callback for particular events as below:
			 * NYX_CHARGE_COMPLETE if battery/status from NULL/Charging to Full, NYX_CHARGE_RESTART if battery/status from Full to Charging,
			 * NYX_CHARGER_CONNECTED if USB,AC or any other charger online is from 0 to 1,
			 * NYX_CHARGER_DISCONNECTED if any charger online from 1 to 0,
			 * NYX_CHARGER_FAULT if online=1 and battery/status=Not Charging/Discharging? - TODO: not implemented since we are not sure of the state change for this event
			 * NYX_BATTERY_PRESENT if battery is present (0-1)
			 * NYX_BATTERY_ABSENT if battery is absent (1-0)
			 * NYX_BATTERY_CRITICAL_VOLTAGE if Battery voltage below threshold - TODO: not implemented since we do not get kobject for voltage changes
			 * NYX_BATTERY_TEMPERATURE_LIMIT if Battery temperature below/above limits - TODO: not implemented since we do not get kobject for temperature changes
			 */

			bool prev_charging = gChargerStatus.is_charging;
			core_charger_read_status(NULL);

			if (_has_charger_connected_state_changed(prev_charging,
			        gChargerStatus.is_charging))
			{
				fire_charger_status_cb = true;
				fire_state_change_cb = true;
			}

			if (fire_charger_status_cb && charger_status_callback)
			{
				charger_status_callback(nyxDev, NYX_CALLBACK_STATUS_DONE,
				                        charger_status_callback_context);
				fire_charger_status_cb = false;
			}

			/* Keep a note of previous values */
			char *prev_batt_status = g_strdup(battery_status);
			int prev_batt_present = curr_battery_state->present;

			_battery_read_status();

			if ((_has_charger_state_changed(prev_batt_status, battery_status)) ||
			        (_has_battery_state_changed(prev_batt_present, curr_battery_state->present)))
			{
				fire_state_change_cb = true;
			}

			g_free(prev_batt_status);

			if (fire_state_change_cb && state_change_callback)
			{
				state_change_callback(nyxDev, NYX_CALLBACK_STATUS_DONE,
				                      state_change_callback_context);
				fire_state_change_cb = false;
			}
		}
	}

	return TRUE;
}

void _charger_init_events()
{
	_has_charger_state_changed(NULL, battery_status);
	_has_battery_state_changed(0, curr_battery_state->present);
	_has_charger_connected_state_changed(0, gChargerStatus.is_charging);
}

void _detect_charger_sysfs_paths()
{
	/*
	 * Each path honours an optional compile-time override from the
	 * machine-specific cmake include (e.g. meta-luneos's tenderloin.cmake),
	 * falling back to a directory walk of /sys/class/power_supply when the
	 * override is not set. Pinning the path avoids the auto-detect picking
	 * the wrong device when a board exposes more than one power_supply of
	 * the same type (e.g. two type=Battery entries, or a "Mains" charger
	 * sharing the namespace with a separately-named USB BC1.2 detector).
	 */
#ifdef BATTERY_SYSFS_PATH
	char *battery_sysfs_path = g_strdup(BATTERY_SYSFS_PATH);
#else
	char *battery_sysfs_path = find_power_supply_sysfs_path("Battery");
#endif
#ifdef CHARGER_USB_SYSFS_PATH
	char *charger_usb_sysfs_path = g_strdup(CHARGER_USB_SYSFS_PATH);
#else
	char *charger_usb_sysfs_path = find_power_supply_sysfs_path("USB");
#endif
#ifdef CHARGER_AC_SYSFS_PATH
	char *charger_ac_sysfs_path = g_strdup(CHARGER_AC_SYSFS_PATH);
#else
	char *charger_ac_sysfs_path = find_power_supply_sysfs_path("Mains");
#endif
#ifdef CHARGER_TOUCH_SYSFS_PATH
	char *charger_touch_sysfs_path = g_strdup(CHARGER_TOUCH_SYSFS_PATH);
#else
	char *charger_touch_sysfs_path = find_power_supply_sysfs_path("Touch");
#endif
#ifdef CHARGER_WIRELESS_SYSFS_PATH
	char *charger_wireless_sysfs_path = g_strdup(CHARGER_WIRELESS_SYSFS_PATH);
#else
	char *charger_wireless_sysfs_path = find_power_supply_sysfs_path("Wireless");
#endif

	if (charger_usb_sysfs_path)
	{
		snprintf(charger_usb_sysfs_online_path, PATH_LEN, "%s/online",
		         charger_usb_sysfs_path);
		/*
		 * Additional sysfs attributes published by BC 1.2-capable USB
		 * type detectors (e.g. drivers/usb/chipidea/ci_hdrc_msm.c on
		 * MSM8660).  All are optional -- a power_supply that exposes
		 * only "online" still works at the legacy classification.
		 *
		 * usb_type              : kernel-formatted enum line e.g.
		 *                         "Unknown SDP [DCP] CDP" -- the
		 *                         bracketed token is the active value.
		 * current_max           : maximum charge current in microamps.
		 * vendor_charger_variant: optional vendor variant string e.g.
		 *                         "hp-touchstone-10w" / "hp-phone-900ma"
		 *                         / "omtp-900ma".  Empty when the
		 *                         platform driver did not detect a
		 *                         vendor-specific variant.
		 */
		snprintf(charger_usb_sysfs_usb_type_path, PATH_LEN, "%s/usb_type",
		         charger_usb_sysfs_path);
		snprintf(charger_usb_sysfs_current_max_path, PATH_LEN, "%s/current_max",
		         charger_usb_sysfs_path);
		snprintf(charger_usb_sysfs_vendor_variant_path, PATH_LEN,
		         "%s/vendor_charger_variant", charger_usb_sysfs_path);
	}

	if (charger_ac_sysfs_path)
	{
		snprintf(charger_ac_sysfs_online_path, PATH_LEN, "%s/online",
		         charger_ac_sysfs_path);
		/* Mains chargers (e.g. max8903) also expose current_max. */
		snprintf(charger_ac_sysfs_current_max_path, PATH_LEN, "%s/current_max",
		         charger_ac_sysfs_path);
	}

	if (charger_touch_sysfs_path)
	{
		snprintf(charger_touch_sysfs_online_path, PATH_LEN, "%s/online",
		         charger_touch_sysfs_path);
	}

	if (charger_wireless_sysfs_path)
	{
		snprintf(charger_wireless_sysfs_online_path, PATH_LEN, "%s/online",
		         charger_wireless_sysfs_path);
	}

	if (battery_sysfs_path)
	{
		snprintf(batt_present_path, PATH_LEN, "%s/present", battery_sysfs_path);
		snprintf(batt_status_path, PATH_LEN, "%s/status", battery_sysfs_path);
	}

	/* Free allocated paths after use */
	g_free(battery_sysfs_path);
	g_free(charger_usb_sysfs_path);
	g_free(charger_ac_sysfs_path);
	g_free(charger_touch_sysfs_path);
	g_free(charger_wireless_sysfs_path);
}

static void _charger_cleanup(void)
{
	// _charger_init sets g_io_channel_set_close_on_unref, and calls g_io_channel_unref.
	// This leaves one ref associated with the watch, so removing the watch should close the channel.
	if (0 != watch)
	{
		g_source_remove(watch);
		watch = 0;
	}

	if (NULL != curr_battery_state)
	{
		free(curr_battery_state);
		curr_battery_state = NULL;
	}

	if (NULL != battery_status)
	{
		free(battery_status);
		battery_status = NULL;
	}

	if (NULL != mon)
	{
		udev_monitor_filter_remove(mon);
		mon = NULL;
	}

	if (NULL != udev)
	{
		udev_unref(udev);
		udev = NULL;
	}

	return;
}

nyx_error_t core_charger_init(void)
{
	int fd;
	GIOChannel *channel = NULL;

	udev = udev_new();

	if (!udev)
	{
		nyx_error(MSGID_NYX_MOD_CHARG_ERR, 0,
		          "Could not initialize udev component; charger status updates will not be available");
		return NYX_ERROR_GENERIC;
	}

	mon = udev_monitor_new_from_netlink(udev, "kernel");

	if (mon == NULL)
	{
		nyx_error(MSGID_NYX_MOD_NETLINK_ERR, 0,
		          "Failed to create udev monitor for kernel events");
		_charger_cleanup();
		return NYX_ERROR_GENERIC;
	}

	if (udev_monitor_filter_add_match_subsystem_devtype(mon, "power_supply",
	        NULL) < 0)
	{
		nyx_error(MSGID_NYX_MOD_CHR_SUB_ERR, 0,
		          "Failed to setup udev filter for power_supply subsytem events");
		_charger_cleanup();
		return NYX_ERROR_GENERIC;
	}

	if (udev_monitor_enable_receiving(mon) < 0)
	{
		nyx_error(MSGID_NYX_MOD_ENABLE_REV_ERR, 0,
		          "Failed to enable receiving kernel events for power_supply subsytem\n");
		_charger_cleanup();
		return NYX_ERROR_GENERIC;
	}

	/* Initialize charger sysfs paths */
	_detect_charger_sysfs_paths();
	/* Initialize battery and charger status */
	core_charger_read_status(NULL);
	curr_battery_state = (nyx_battery_status_t *) malloc(sizeof(
	                         nyx_battery_status_t));

	if (NULL == curr_battery_state)
	{
		_charger_cleanup();
		return NYX_ERROR_OUT_OF_MEMORY;
	}

	battery_status = (char *)malloc(STATUS_LEN);

	if (NULL == battery_status)
	{
		_charger_cleanup();
		return NYX_ERROR_OUT_OF_MEMORY;
	}

	_battery_read_status();

	/* Initialize events */
	_charger_init_events();

	/* Setup io watch for uevents */
	fd = udev_monitor_get_fd(mon);

	if (-1 == fd)
	{
		_charger_cleanup();
		return NYX_ERROR_GENERIC;
	}

	channel = g_io_channel_unix_new(fd);

	if (!channel)
	{
		_charger_cleanup();
		return NYX_ERROR_GENERIC;
	}

	/* add watch event (which adds a ref) before calling g_io_channel_unref */
	watch = g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_NVAL,
	                       _handle_power_supply_event, NULL);

	/* Remove the ref from g_io_channel_unix_new so we won't leak the channel if g_io_add_watch failed */
	/* watch holds another ref which is removed in _charger_cleanup */
	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_unref(channel);

	if (0 == watch)
	{
		_charger_cleanup();
		return NYX_ERROR_GENERIC;
	}

	return NYX_ERROR_NONE;
}

nyx_error_t core_charger_deinit(void)
{
	_charger_cleanup();
	return NYX_ERROR_NONE;
}

nyx_error_t core_charger_enable_charging(nyx_charger_status_t *status)
{
	memcpy(status, &gChargerStatus, sizeof(nyx_charger_status_t));

	return NYX_ERROR_NONE;
}

nyx_error_t core_charger_disable_charging(nyx_charger_status_t *status)
{
	memcpy(status, &gChargerStatus, sizeof(nyx_charger_status_t));

	return NYX_ERROR_NONE;
}

nyx_error_t core_charger_query_charger_event(nyx_charger_event_t *event)
{
	*event = current_event;

	return NYX_ERROR_NONE;
}
