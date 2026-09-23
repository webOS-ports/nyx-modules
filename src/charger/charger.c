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
#include "nyx_conf.h"

#define STATUS_LEN 64
#define PATH_LEN 128

struct udev *udev = NULL;
struct udev_monitor *mon = NULL;
guint watch = 0;
static GIOChannel *channel = NULL;

/*
 * Where power_supply nodes live. Overridable so a host-side test can point
 * the USB-family scan at a fixture tree.
 */
#ifndef POWER_SUPPLY_SYSFS_ROOT
#define POWER_SUPPLY_SYSFS_ROOT "/sys/class/power_supply"
#endif

/*
 * How long a fresh sysfs read may lag the uevent that announced it, and how
 * long after that we look once more. See _charger_arm_settle().
 */
#define CHARGER_SETTLE_MS    200
#define CHARGER_RESETTLE_MS  1000

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
 * What the client was last told, as opposed to gChargerStatus, which is
 * what sysfs said the last time anyone looked.
 *
 * gChargerStatus is rewritten by every core_charger_read_status(), and
 * charger_query_charger_status() is one of its callers. The uevent handler
 * used to decide "did the charger change?" by comparing gChargerStatus
 * before and after its own read, so a client query landing between the
 * sysfs transition and the uevent (batteryd answers chargerStatusQuery for
 * sleepd, the display manager and the shell) consumed the edge: the handler
 * then saw "no change" and never fired the callback. The comparison is now
 * against this snapshot, which only moves when the callback actually fires.
 */
static bool notified_charging = false;
static bool notified_valid = false;

/*
 * Consecutive uevent wakeups that produced no device. See
 * _handle_power_supply_event(): a handful is normal (a filtered message),
 * a run of them means the socket is wedged and has to be rebuilt.
 */
static int empty_reads = 0;
#define CHARGER_MAX_EMPTY_READS 16

/* Pending settle re-reads: 0 = none, 1 = the CHARGER_SETTLE_MS one, 2 = the
 * CHARGER_RESETTLE_MS one. */
static guint settle_source = 0;
static int settle_stage = 0;
static int settle_ms = CHARGER_SETTLE_MS;
static int resettle_ms = CHARGER_RESETTLE_MS;

/*
 * "online" of every power_supply whose type starts with USB and which is not
 * already in a configured slot. On Qualcomm smb2/smb5 parts the charger
 * input is split over two nodes ("usb" and "pc_port") of which only one is
 * live for a given source; a nyx.conf that names one of them, or a fallback
 * walk that picked the wrong one, would otherwise miss the other entirely.
 */
static GPtrArray *usb_family_online_paths = NULL;

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

	/*
	 * "online" is not a boolean: the power_supply class documents 0 for
	 * offline, 1 for online (fixed) and 2 for online (programmable), and
	 * MediaTek's mt6375 charger on the MP01 reports 2 for a plain USB
	 * cable. Comparing against 1 read that as "no charger", so batteryd
	 * never announced the cable, the display manager never held the screen
	 * and sleepd suspended on the charger. Anything positive is online;
	 * nyx_utils_read_value() returns a negative number for an unreadable node.
	 */
	usb_online      = (nyx_utils_read_value(charger_usb_sysfs_online_path) > 0);
	ac_online       = (nyx_utils_read_value(charger_ac_sysfs_online_path) > 0);
	touch_online    = (nyx_utils_read_value(charger_touch_sysfs_online_path) > 0);
	wireless_online = (nyx_utils_read_value(charger_wireless_sysfs_online_path) > 0);

	/*
	 * A USB-family supply nobody configured that reports online is a wired
	 * charger all the same. It cannot be classified (no usb_type to read),
	 * so it lands in the wall/direct slot, which is what the AC path does
	 * for a Mains-class supply too. Only consulted when neither configured
	 * slot is live, so it never overrides a configured classification.
	 */
	if (!usb_online && !ac_online && usb_family_online_paths)
	{
		guint i;

		for (i = 0; i < usb_family_online_paths->len; i++)
		{
			if (nyx_utils_read_value(g_ptr_array_index(usb_family_online_paths, i)) > 0)
			{
				ac_online = true;
				break;
			}
		}
	}

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
			gChargerStatus.dock_serial_number[
			    sizeof(gChargerStatus.dock_serial_number) - 1] = '\0';
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
		memset(battery_status, 0, STATUS_LEN);
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

/*
 * Decide whether the charger's connected state differs from what the client
 * was last told, and tell it if so.
 *
 * A disconnect is reported at once: sleepd's charger veto and the display
 * manager's on-when-connected hold both key off it, and a stale "connected"
 * keeps the device awake on a dead battery. A connect is only reported from
 * a settle re-read (from_settle), i.e. once it has held for settle_ms. On
 * sargo the smb5 "usb" node was seen to report online for a moment two
 * seconds after the cable came out, and the immediate read turned that into
 * a connect broadcast with nothing to retract it; a connect that is real is
 * still there 200 ms later.
 *
 * Returns true when the client was notified.
 */
static bool _charger_evaluate(const char *why, bool from_settle)
{
	bool now;

	core_charger_read_status(NULL);
	now = gChargerStatus.is_charging;

	if (notified_valid && now == notified_charging)
	{
		return false;
	}

	if (now && !from_settle && notified_valid)
	{
		nyx_debug("charger: connect seen (%s), waiting %d ms for it to hold",
		          why, settle_ms);
		return false;
	}

	nyx_info(MSGID_NYX_MOD_CHARG_EDGE, 0, "charger %s (%s%s)",
	         now ? "connected" : "disconnected", why,
	         from_settle ? ", settle re-read" : "");

	_has_charger_connected_state_changed(notified_charging, now);
	notified_charging = now;
	notified_valid = true;

	if (charger_status_callback)
	{
		charger_status_callback(nyxDev, NYX_CALLBACK_STATUS_DONE,
		                        charger_status_callback_context);
	}

	if (state_change_callback)
	{
		state_change_callback(nyxDev, NYX_CALLBACK_STATUS_DONE,
		                      state_change_callback_context);
	}

	return true;
}

static gboolean _charger_settle_cb(gpointer data)
{
	int stage = settle_stage;

	settle_source = 0;
	settle_stage = 0;

	_charger_evaluate(stage == 1 ? "settle" : "resettle", true);

	if (stage == 1)
	{
		settle_stage = 2;
		settle_source = g_timeout_add(resettle_ms, _charger_settle_cb, NULL);
	}

	return G_SOURCE_REMOVE;
}

/*
 * Look at the charger again shortly, and once more after that.
 *
 * The kernel emits a power_supply uevent per supply that changed, and the
 * one we are configured to read is not always among them: on sargo the
 * charger input is "usb" (which emits uevents) mirrored into "pc_port"
 * (which never does), and pc_port/online was seen to still read 1 when the
 * uevents for usb, main and battery arrived, dropping to 0 within the
 * second. With nothing else scheduled to look, that disconnect was lost for
 * as long as nothing else on the bus changed. Every uevent therefore arms a
 * re-read at settle_ms, which arms another at resettle_ms. A new uevent
 * while either is pending restarts the sequence.
 */
static void _charger_arm_settle(void)
{
	if (settle_source)
	{
		g_source_remove(settle_source);
	}

	settle_stage = 1;
	settle_source = g_timeout_add(settle_ms, _charger_settle_cb, NULL);
}

static void _charger_cancel_settle(void)
{
	if (settle_source)
	{
		g_source_remove(settle_source);
		settle_source = 0;
	}

	settle_stage = 0;
}

static bool _path_is_under(const char *path, const char *dir)
{
	size_t n = strlen(dir);

	return n > 0 && strncmp(path, dir, n) == 0 && path[n] == '/';
}

/*
 * Collect "online" of every supply under root whose type starts with USB
 * (USB, USB_PD, USB_DCP, ...) and that is not one of the configured slots.
 * Rebuilt on power_supply add/remove.
 */
static void _scan_usb_family_supplies(const char *root)
{
	GDir *dir;
	const char *name;

	if (usb_family_online_paths)
	{
		g_ptr_array_free(usb_family_online_paths, TRUE);
	}

	usb_family_online_paths = g_ptr_array_new_with_free_func(g_free);

	dir = g_dir_open(root, 0, NULL);

	if (!dir)
	{
		return;
	}

	while ((name = g_dir_read_name(dir)) != NULL)
	{
		gchar *dir_path = g_build_filename(root, name, NULL);
		gchar *type_path = g_build_filename(dir_path, "type", NULL);
		char type[64] = "";

		if (g_file_test(type_path, G_FILE_TEST_IS_REGULAR) &&
		        FileGetString(type_path, type, sizeof(type)) == 0 &&
		        strncmp(type, "USB", 3) == 0 &&
		        !_path_is_under(charger_usb_sysfs_online_path, dir_path) &&
		        !_path_is_under(charger_ac_sysfs_online_path, dir_path))
		{
			g_ptr_array_add(usb_family_online_paths,
			                g_build_filename(dir_path, "online", NULL));
			nyx_debug("charger: also watching %s/online (type %s)", dir_path, type);
		}

		g_free(type_path);
		g_free(dir_path);
	}

	g_dir_close(dir);
}

static gboolean _charger_monitor_start(void);

gboolean _handle_power_supply_event(GIOChannel *ch, GIOCondition condition,
                                    gpointer data)
{
	struct udev_device *dev;
	bool fire_state_change_cb = false;

	if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
	{
		/*
		 * The netlink socket is gone. Returning TRUE here, as this used to,
		 * would have glib call back on the dead descriptor forever and the
		 * charger would never be heard from again; the settle timers are
		 * the only thing that would still notice a change. Rebuild it.
		 */
		nyx_error(MSGID_NYX_MOD_CHARG_MONITOR, 0,
		          "power_supply uevent socket lost (condition 0x%x), reopening",
		          condition);
		watch = 0;
		_charger_monitor_start();
		_charger_arm_settle();
		return G_SOURCE_REMOVE;
	}

	if ((condition & G_IO_IN) == G_IO_IN)
	{
		dev = udev_monitor_receive_device(mon);

		if (!dev)
		{
			/*
			 * Readable, but nothing came back. udev_monitor_receive_device()
			 * returns NULL for a message that fails its filter, for a recv
			 * error, and for a socket that has gone bad - and in that last
			 * case the descriptor stays readable for ever. Returning TRUE
			 * then has glib re-dispatch us immediately, on a descriptor that
			 * will never yield a device again: a tight loop that burns a
			 * whole core and starves every other source in the process.
			 *
			 * Measured on a PinePhone Pro 2026-09-23: batteryd pinned at
			 * 100% of a CPU with 21h32m of CPU time against 20 minutes for
			 * the next-busiest process on the device, and its luna methods
			 * answering nothing at all - com.webos.service.battery/status
			 * and /chargerStatusQuery both silent while
			 * com.palm.display/control/status answered in 3.00 ms. With
			 * batteryd unable to answer or broadcast, sleepd never learned
			 * the charger state and suspend broke in both directions: it
			 * refused to suspend on battery (chargerIsConnected stuck true)
			 * and suspended while charging (stuck false).
			 *
			 * A filtered message is normal and transient, so tolerate a few
			 * in a row; a descriptor that keeps claiming to be readable with
			 * nothing to give is broken, so rebuild the monitor exactly as
			 * the HUP path above does.
			 */
			if (++empty_reads < CHARGER_MAX_EMPTY_READS)
			{
				return TRUE;
			}

			nyx_error(MSGID_NYX_MOD_CHARG_MONITOR, 0,
			          "power_supply uevent socket readable but empty %d times, reopening",
			          empty_reads);
			empty_reads = 0;
			watch = 0;
			_charger_monitor_start();
			_charger_arm_settle();
			return G_SOURCE_REMOVE;
		}

		empty_reads = 0;

		{
			const char *action = udev_device_get_action(dev);
			const char *sysname = udev_device_get_sysname(dev);
			char sysname_copy[64];

			/*
			 * sysname points into dev and does not outlive the unref below,
			 * but it is still wanted afterwards as the reason string for the
			 * edge log. Reading it after the free put uninitialised bytes
			 * straight into the journal - "charger disconnected (\u042e..."
			 * was what gave this away on the PinePhone Pro. Take a copy.
			 */
			g_strlcpy(sysname_copy, sysname ? sysname : "uevent",
			          sizeof(sysname_copy));

			nyx_debug("charger: power_supply uevent %s %s",
			          action ? action : "?", sysname ? sysname : "?");

			if (action && (0 == g_strcmp0(action, "add") ||
			               0 == g_strcmp0(action, "remove")))
			{
				_scan_usb_family_supplies(POWER_SUPPLY_SYSFS_ROOT);
			}

			udev_device_unref(dev);

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
			_charger_evaluate(sysname_copy, false);

			/* The supply we read may not be the one that spoke; look again. */
			_charger_arm_settle();

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
	notified_charging = gChargerStatus.is_charging;
	notified_valid = true;
}

/*
 * Fill in the path of an attribute the supply may or may not have, leaving
 * it empty when it does not. Every read of the status used to try all of
 * them and log NYXUTIL_GET_STRING_ERR for each one missing - three lines
 * per uevent on a supply that only has "online" - and the status is now
 * re-read on a timer as well.
 */
static void _optional_attr_path(char *dst, const char *dir, const char *attr)
{
	snprintf(dst, PATH_LEN, "%s/%s", dir, attr);

	if (!g_file_test(dst, G_FILE_TEST_EXISTS))
	{
		dst[0] = '\0';
	}
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
	char *battery_sysfs_path = nyx_conf_get_path("module.battery", "sysfs_path");

	if (!battery_sysfs_path)
	{
#ifdef BATTERY_SYSFS_PATH
		battery_sysfs_path = g_strdup(BATTERY_SYSFS_PATH);
#else
		battery_sysfs_path = find_power_supply_sysfs_path("Battery");
#endif
	}
	char *charger_usb_sysfs_path = nyx_conf_get_path("module.charger", "usb_sysfs_path");

	if (!charger_usb_sysfs_path)
	{
#ifdef CHARGER_USB_SYSFS_PATH
		charger_usb_sysfs_path = g_strdup(CHARGER_USB_SYSFS_PATH);
#else
		charger_usb_sysfs_path = find_power_supply_sysfs_path("USB");
#endif
	}
	char *charger_ac_sysfs_path = nyx_conf_get_path("module.charger", "ac_sysfs_path");

	if (!charger_ac_sysfs_path)
	{
#ifdef CHARGER_AC_SYSFS_PATH
		charger_ac_sysfs_path = g_strdup(CHARGER_AC_SYSFS_PATH);
#else
		charger_ac_sysfs_path = find_power_supply_sysfs_path("Mains");
#endif
	}
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
		_optional_attr_path(charger_usb_sysfs_usb_type_path,
		                    charger_usb_sysfs_path, "usb_type");
		_optional_attr_path(charger_usb_sysfs_current_max_path,
		                    charger_usb_sysfs_path, "current_max");
		_optional_attr_path(charger_usb_sysfs_vendor_variant_path,
		                    charger_usb_sysfs_path, "vendor_charger_variant");
	}

	if (charger_ac_sysfs_path)
	{
		snprintf(charger_ac_sysfs_online_path, PATH_LEN, "%s/online",
		         charger_ac_sysfs_path);
		/* Mains chargers (e.g. max8903) also expose current_max. */
		_optional_attr_path(charger_ac_sysfs_current_max_path,
		                    charger_ac_sysfs_path, "current_max");
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

static void _charger_monitor_stop(void)
{
	if (0 != watch)
	{
		g_source_remove(watch);
		watch = 0;
	}

	/*
	 * The monitor owns the descriptor; the channel must not close it too,
	 * or it closes whatever number the kernel has since handed out.
	 */
	if (NULL != channel)
	{
		g_io_channel_set_close_on_unref(channel, FALSE);
		g_io_channel_unref(channel);
		channel = NULL;
	}

	if (NULL != mon)
	{
		udev_monitor_unref(mon);
		mon = NULL;
	}
}

/*
 * Open the kernel netlink monitor for power_supply and watch it from the
 * caller's main loop. Called at init and again if the socket dies.
 */
static gboolean _charger_monitor_start(void)
{
	int fd;

	_charger_monitor_stop();

	mon = udev_monitor_new_from_netlink(udev, "kernel");

	if (mon == NULL)
	{
		nyx_error(MSGID_NYX_MOD_NETLINK_ERR, 0,
		          "Failed to create udev monitor for kernel events");
		return FALSE;
	}

	if (udev_monitor_filter_add_match_subsystem_devtype(mon, "power_supply",
	        NULL) < 0)
	{
		nyx_error(MSGID_NYX_MOD_CHR_SUB_ERR, 0,
		          "Failed to setup udev filter for power_supply subsytem events");
		_charger_monitor_stop();
		return FALSE;
	}

	if (udev_monitor_enable_receiving(mon) < 0)
	{
		nyx_error(MSGID_NYX_MOD_ENABLE_REV_ERR, 0,
		          "Failed to enable receiving kernel events for power_supply subsytem\n");
		_charger_monitor_stop();
		return FALSE;
	}

	fd = udev_monitor_get_fd(mon);

	if (-1 == fd)
	{
		_charger_monitor_stop();
		return FALSE;
	}

	channel = g_io_channel_unix_new(fd);

	if (!channel)
	{
		_charger_monitor_stop();
		return FALSE;
	}

	g_io_channel_set_close_on_unref(channel, FALSE);
	watch = g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
	                       _handle_power_supply_event, NULL);

	if (0 == watch)
	{
		_charger_monitor_stop();
		return FALSE;
	}

	return TRUE;
}

static void _charger_cleanup(void)
{
	_charger_cancel_settle();
	_charger_monitor_stop();

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

	if (NULL != usb_family_online_paths)
	{
		g_ptr_array_free(usb_family_online_paths, TRUE);
		usb_family_online_paths = NULL;
	}

	if (NULL != udev)
	{
		udev_unref(udev);
		udev = NULL;
	}

	notified_valid = false;

	return;
}

/* [module.charger] settle_ms= / resettle_ms= in nyx.conf override the defaults. */
static void _charger_read_settle_conf(void)
{
	gchar *value;

	settle_ms = CHARGER_SETTLE_MS;
	resettle_ms = CHARGER_RESETTLE_MS;

	value = nyx_conf_get_path("module.charger", "settle_ms");

	if (value)
	{
		int v = atoi(value);

		if (v > 0)
		{
			settle_ms = v;
		}

		g_free(value);
	}

	value = nyx_conf_get_path("module.charger", "resettle_ms");

	if (value)
	{
		int v = atoi(value);

		if (v > 0)
		{
			resettle_ms = v;
		}

		g_free(value);
	}
}

nyx_error_t core_charger_init(void)
{
	udev = udev_new();

	if (!udev)
	{
		nyx_error(MSGID_NYX_MOD_CHARG_ERR, 0,
		          "Could not initialize udev component; charger status updates will not be available");
		return NYX_ERROR_GENERIC;
	}

	/* Initialize charger sysfs paths */
	_detect_charger_sysfs_paths();
	_charger_read_settle_conf();
	_scan_usb_family_supplies(POWER_SUPPLY_SYSFS_ROOT);
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
	if (!_charger_monitor_start())
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
