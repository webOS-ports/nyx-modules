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
 * @file battery.c
 *
 * @brief Interface for reading all the battery values from sysfs node.
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <glib.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>
#include <nyx/module/nyx_log.h>
#include "msgid.h"
#include "nyx_conf.h"

#include <glib.h>
#include <libudev.h>

#include "battery.h"
#include "utils.h"

#define CHARGE_MIN_TEMPERATURE_C 0
#define CHARGE_MAX_TEMPERATURE_C 57
#define BATTERY_MAX_TEMPERATURE_C  60

#define PATH_LEN 256

/*
 * Two is what the hardware this was written for has - a phone and the battery
 * in its keyboard - and the cap only bounds how much of a misconfigured
 * extra_sysfs_paths list is honoured, so leave room without inviting a config
 * that nothing can display sensibly.
 */
#define MAX_BATTERIES 8

/**
 * One battery: where it lives in sysfs, what to call it, and the attribute
 * paths derived from it.
 *
 * @c configured marks a battery named in nyx.conf. Those keep their slot even
 * while their sysfs node is gone, so a detached keyboard is reported as a
 * battery that is not present rather than vanishing from the list - a
 * disappearing entry looks to the shell like a battery it never knew about.
 * Batteries found by walking the power_supply class come and go with the walk.
 */
typedef struct
{
	gchar *sysfs_path;
	bool configured;
	char name[NYX_BATTERY_NAME_MAX];
	char role[NYX_BATTERY_NAME_MAX];

	char capacity_path[PATH_LEN];
	char energy_now_path[PATH_LEN];
	char energy_full_path[PATH_LEN];
	char energy_full_design_path[PATH_LEN];
	char charge_now_path[PATH_LEN];
	char charge_full_path[PATH_LEN];
	char charge_full_design_path[PATH_LEN];
	char temperature_path[PATH_LEN];
	char voltage_path[PATH_LEN];
	char current_path[PATH_LEN];
	char present_path[PATH_LEN];
	char fake_battery_path[PATH_LEN];

	/* last values seen, so _handle_event only wakes callers on a change */
	int last_percentage;
	bool last_present;
} battery_device_t;

static battery_device_t batteries[MAX_BATTERIES];
static int batteries_count = 0;

nyx_battery_ctia_t battery_ctia_params;

struct udev *udev = NULL;
struct udev_monitor *mon = NULL;
guint watch = 0;

extern nyx_device_t *nyxDev;
extern void *battery_callback_context;
extern nyx_device_callback_function_t battery_callback;

nyx_battery_ctia_t *get_battery_ctia_params(void)
{
	battery_ctia_params.charge_min_temp_c = CHARGE_MIN_TEMPERATURE_C;
	battery_ctia_params.charge_max_temp_c = CHARGE_MAX_TEMPERATURE_C;
	battery_ctia_params.battery_crit_max_temp = BATTERY_MAX_TEMPERATURE_C;
	battery_ctia_params.skip_battery_authentication = true;

	return &battery_ctia_params;
}

/**
 * @brief The battery at an index, or NULL if there is none there.
 */
static battery_device_t *battery_at(int index)
{
	if (index < 0 || index >= batteries_count)
	{
		return NULL;
	}

	return &batteries[index];
}

int battery_count(void)
{
	return batteries_count;
}

const char *battery_name(int index)
{
	battery_device_t *b = battery_at(index);

	return b ? b->name : "";
}

const char *battery_role(int index)
{
	battery_device_t *b = battery_at(index);

	return b ? b->role : "";
}

/**
 * @brief Read battery percentage
 *
 * @retval Battery percentage (integer)
 */
int battery_percent(int index)
{
	battery_device_t *b = battery_at(index);
	int now, full;
	int capacity;

	if (!b)
	{
		return -1;
	}

	// TODO: Might first confirm that battery is present?

	/*
	 * The ratios below are computed in 64 bits. energy_now is in microwatt
	 * hours, so 100 * now overflows a signed int for any pack above roughly
	 * 21.5 Wh - which is most of them once this runs on anything larger than
	 * a phone - and signed overflow is undefined, not merely wrong.
	 */

	/* try capacity node first but keep in mind it's not supported by all power class devices */
	if ((capacity = nyx_utils_read_value(b->capacity_path)) < 0)
	{
		/* capacity node is not available so next try is energy_full path */
		if (g_file_test(b->energy_full_path, G_FILE_TEST_EXISTS))
		{
			if ((now = nyx_utils_read_value(b->energy_now_path)) < 0)
			{
				return -1;
			}

			if ((full = nyx_utils_read_value(b->energy_full_path)) <= 0)
			{
				return -1;
			}

			capacity = (int)((gint64) 100 * now / full);
		}
		/* as last try we can use charge_now path */
		else if (g_file_test(b->charge_now_path, G_FILE_TEST_EXISTS))
		{
			if ((full = nyx_utils_read_value(b->charge_full_path)) <= 0)
			{
				return -1;
			}

			if ((now = nyx_utils_read_value(b->charge_now_path)) < 0)
			{
				return -1;
			}

			capacity = (int)((gint64) 100 * now / full);
		}
		else
		{
			return -1;
		}
	}

	return capacity;
}

/**
 * @brief Read battery temperature
 *
 * @retval Battery temperature (integer)
 */
int battery_temperature(int index)
{
	battery_device_t *b = battery_at(index);
	double temp = 0;

	/*
	 * temp is signed, and a battery really can be below freezing - a phone
	 * left in a car overnight reports a negative temperature, and the CTIA
	 * limits this module publishes exist precisely to stop it charging
	 * there. nyx_utils_read_value() reports every negative reading as a
	 * failed read, so the one case the charging logic most needs to see was
	 * the one it could not. Read it through FileGetDouble() for the same
	 * reason battery_current() does.
	 */
	if (!b || FileGetDouble(b->temperature_path, &temp) < 0)
	{
		return -1;
	}

	return (int)temp;
}

/**
 * @brief Read battery voltage
 *
 * @retval Battery voltage (integer)
 */

int battery_voltage(int index)
{
	battery_device_t *b = battery_at(index);
	int voltage;

	if (!b || (voltage = nyx_utils_read_value(b->voltage_path)) < 0)
	{
		return -1;
	}

	return voltage;
}

/**
 * @brief Read the amount of current being drawn by the battery (negative = charging!)
 *
 * @retval Current (integer)
 */
int battery_current(int index)
{
	battery_device_t *b = battery_at(index);
	double current = 0;

	/*
	 * The Linux power_supply class exports current_now as a *signed*
	 * value in microamps: positive while the battery is charging,
	 * negative while it is discharging. nyx_utils_read_value() collapses
	 * "value is negative" with "read failed", which means battery_current
	 * returns -1 on every Linux-mainline target the moment the device
	 * runs on battery — masking the real value and confusing
	 * batteryStatusQuery consumers in cardshell / powerd.
	 *
	 * Use FileGetDouble() instead, which signals errors via its return
	 * code and stores the parsed value through the out-parameter, so
	 * negative readings are passed through cleanly.
	 */
	if (!b || FileGetDouble(b->current_path, &current) < 0)
	{
		return -1;
	}

	return (int)current;
}

/**
 * @brief Read average current being drawn by the battery.
 *
 * @retval Current (integer)
 */

int battery_avg_current(int index)
{
	// return battery_current for this device unless we have a way to separately read "average" current
	return battery_current(index);
}

/**
 * @brief Read battery full capacity
 *
 * @retval Battery capacity (double)
 */
double battery_full40(int index)
{
	battery_device_t *b = battery_at(index);
	int charge_full;

	if (!b)
	{
		return -1;
	}

	if (!g_file_test(b->charge_full_path, G_FILE_TEST_EXISTS) ||
	        ((charge_full = nyx_utils_read_value(b->charge_full_path)) < 0))
	{
		if ((charge_full = nyx_utils_read_value(b->charge_full_design_path)) < 0)
		{
			return -1;
		}
	}

	/* Divide the value by 1000 to convert from uAh to mAh */
	return (double) charge_full / 1000;
}

/**
 * @brief Read battery current raw capacity
 *
 * @retval Battery capacity (double)
 */

double battery_rawcoulomb(int index)
{
	return -1;
}

/**
 * @brief Read battery current capacity
 *
 * @retval Battery capacity (double)
 */

double battery_coulomb(int index)
{
	battery_device_t *b = battery_at(index);
	int charge_now;

	if (!b || (charge_now = nyx_utils_read_value(b->charge_now_path)) < 0)
	{
		return -1;
	}

	/* Divide the value by 1000 to convert from uAh to mAh */
	return (double) charge_now / 1000;
}

/**
 * @brief Read battery age
 *
 * @retval Battery age (double)
 */
double battery_age(int index)
{
	return -1;
}

bool battery_is_present(int index)
{
	battery_device_t *b = battery_at(index);
	int present;

	if (!b)
	{
		return false;
	}

	/*
	 * A battery named in nyx.conf whose node is not there at all is a
	 * detachable one that is currently detached - the keyboard is off the
	 * phone. Answer before touching the attributes, which would all fail
	 * to read anyway.
	 */
	if (!g_file_test(b->sysfs_path, G_FILE_TEST_IS_DIR))
	{
		return false;
	}

	/*
	 * "present" is optional in the power_supply class and a fixed internal
	 * cell has no reason to export it. Reading it as absent-means-missing
	 * would report no battery at all on such a device, so fall back to the
	 * node's own existence, which we have just established.
	 */
	if (!g_file_test(b->present_path, G_FILE_TEST_EXISTS))
	{
		return true;
	}

	if ((present = nyx_utils_read_value(b->present_path)) < 0)
	{
		return false;
	}

	return (1 == present);
}

/**
 * @brief Fill in a battery's attribute paths and its identity.
 *
 * @param role what the battery powers, for a caller that wants to label it.
 *             NULL or empty means "work it out from the node name".
 */
static void battery_set_paths(battery_device_t *b, const char *sysfs_path,
                              const char *role, bool configured)
{
	const char *node_name;

	memset(b, 0, sizeof(*b));

	b->sysfs_path = g_strdup(sysfs_path);
	b->configured = configured;

	node_name = strrchr(sysfs_path, '/');
	node_name = node_name ? node_name + 1 : sysfs_path;
	g_strlcpy(b->name, node_name, sizeof(b->name));

	if (role && *role)
	{
		g_strlcpy(b->role, role, sizeof(b->role));
	}

	snprintf(b->capacity_path, PATH_LEN, "%s/capacity", sysfs_path);
	snprintf(b->energy_now_path, PATH_LEN, "%s/energy_now", sysfs_path);
	snprintf(b->energy_full_path, PATH_LEN, "%s/energy_full", sysfs_path);
	snprintf(b->energy_full_design_path, PATH_LEN, "%s/energy_full_design",
	         sysfs_path);
	snprintf(b->charge_now_path, PATH_LEN, "%s/charge_now", sysfs_path);
	snprintf(b->charge_full_path, PATH_LEN, "%s/charge_full", sysfs_path);
	snprintf(b->charge_full_design_path, PATH_LEN, "%s/charge_full_design",
	         sysfs_path);
	snprintf(b->temperature_path, PATH_LEN, "%s/temp", sysfs_path);
	snprintf(b->voltage_path, PATH_LEN, "%s/voltage_now", sysfs_path);
	snprintf(b->current_path, PATH_LEN, "%s/current_now", sysfs_path);
	snprintf(b->present_path, PATH_LEN, "%s/present", sysfs_path);
	snprintf(b->fake_battery_path, PATH_LEN, "%s/pseudo_batt", sysfs_path);
}

static bool battery_already_known(const char *sysfs_path)
{
	int i;

	for (i = 0; i < batteries_count; i++)
	{
		if (0 == g_strcmp0(batteries[i].sysfs_path, sysfs_path))
		{
			return true;
		}
	}

	return false;
}

static void battery_add(const char *sysfs_path, const char *role,
                        bool configured)
{
	if (!sysfs_path || !*sysfs_path)
	{
		return;
	}

	if (batteries_count >= MAX_BATTERIES)
	{
		nyx_warn(MSGID_NYX_MOD_BATT_TOO_MANY, 0,
		         "more than %d batteries configured, ignoring %s", MAX_BATTERIES,
		         sysfs_path);
		return;
	}

	if (battery_already_known(sysfs_path))
	{
		return;
	}

	battery_set_paths(&batteries[batteries_count], sysfs_path, role, configured);
	batteries_count++;
}

/**
 * @brief Add the batteries listed in [module.battery] extra_sysfs_paths.
 *
 * The value is a list of entries separated by ';', each either a bare sysfs
 * path or "role:path" - the role being what the battery powers, which the
 * shell uses to label it:
 *
 *     extra_sysfs_paths=keyboard:/sys/class/power_supply/ip5xxx-battery
 *
 * luneos-device-config writes this; nothing here needs to know what a
 * PinePhone keyboard is.
 */
static void battery_add_configured_extras(void)
{
	gchar *value = nyx_conf_get_path("module.battery", "extra_sysfs_paths");
	gchar **entries;
	int i;

	if (!value)
	{
		return;
	}

	entries = g_strsplit(value, ";", -1);

	for (i = 0; entries && entries[i]; i++)
	{
		gchar *entry = g_strstrip(entries[i]);
		gchar *sep;

		if (!*entry)
		{
			continue;
		}

		/* "role:path"; a bare path starts with '/' and has no role */
		sep = ('/' == entry[0]) ? NULL : strchr(entry, ':');

		if (sep)
		{
			gchar *role = g_strndup(entry, sep - entry);
			gchar *path = g_strdup(sep + 1);
			battery_add(g_strstrip(path), g_strstrip(role), true);
			g_free(path);
			g_free(role);
		}
		else
		{
			battery_add(entry, NULL, true);
		}
	}

	g_strfreev(entries);
	g_free(value);
}

static void battery_forget_all(void)
{
	int i;

	for (i = 0; i < batteries_count; i++)
	{
		g_free(batteries[i].sysfs_path);
	}

	memset(batteries, 0, sizeof(batteries));
	batteries_count = 0;
}

/**
 * @brief Work out which batteries this device has, primary first.
 *
 * Precedence for the primary is unchanged: the runtime value from
 * luneos-device-config, then the compile-time define, then detection. What is
 * new is that detection no longer stops at the first hit - a device with a
 * keyboard battery has two supplies of type "Battery" and picking between them
 * by directory order picks at random.
 */
static void detect_battery_sysfs_paths(void)
{
	gchar *primary_path;
	char **all_batteries;
	int i;

	battery_forget_all();

	/* Runtime value from luneos-device-config wins over both. */
	primary_path = nyx_conf_get_path("module.battery", "sysfs_path");

	if (!primary_path)
	{
#ifdef BATTERY_SYSFS_PATH
		/*
		 * Honour the BATTERY_SYSFS_PATH define from the machine-specific
		 * cmake include (e.g. meta-luneos's tenderloin.cmake). Bypasses
		 * the directory walk in find_power_supply_sysfs_path(), which on
		 * boards that expose more than one type=Battery power_supply
		 * picks whichever one g_dir_read_name() returns first — order
		 * depends on the underlying filesystem and is not deterministic.
		 *
		 * The HP TouchPad has two A6 microcontrollers (a6-0, a6-1) both
		 * registered by the kernel as power_supply type=Battery; only
		 * a6-0 actually has a battery wired to it.
		 */
		primary_path = g_strdup(BATTERY_SYSFS_PATH);
#else
		primary_path = find_power_supply_sysfs_path("Battery");
#endif
	}

	/* Index 0 is the primary battery; everything else follows it. */
	if (primary_path)
	{
		battery_add(primary_path, "main", true);
		g_free(primary_path);
	}

	battery_add_configured_extras();

	/*
	 * Anything else the kernel calls a battery. Sorted, so the order is the
	 * same on every boot, and skipped if it is already in the list.
	 */
	all_batteries = find_power_supply_sysfs_paths("Battery");

	for (i = 0; all_batteries && all_batteries[i]; i++)
	{
		battery_add(all_batteries[i], NULL, false);
	}

	g_strfreev(all_batteries);

	/*
	 * With nothing configured and nothing found there is still one battery
	 * slot, pointing at nothing: callers expect a primary to exist and
	 * every read from it fails cleanly, which is what happened before.
	 */
	if (0 == batteries_count)
	{
		battery_add("/sys/class/power_supply/battery", "main", true);
	}

	for (i = 0; i < batteries_count; i++)
	{
		if (!*batteries[i].role)
		{
			g_strlcpy(batteries[i].role, (BATTERY_PRIMARY == i) ? "main" : "aux",
			          sizeof(batteries[i].role));
		}

		batteries[i].last_present = battery_is_present(i);
		batteries[i].last_percentage = batteries[i].last_present ? battery_percent(i) : 0;

		nyx_info(MSGID_NYX_MOD_BATT_DETECTED, 0, "battery %d: %s (%s) at %s%s", i,
		         batteries[i].name, batteries[i].role, batteries[i].sysfs_path,
		         batteries[i].last_present ? "" : ", not present");
	}
}

gboolean _handle_event(GIOChannel *channel, GIOCondition condition,
                       gpointer data)
{
	struct udev_device *dev;

	if ((condition  & G_IO_IN) == G_IO_IN)
	{
		dev = udev_monitor_receive_device(mon);

		if (dev)
		{
			/*Initiate callback only if battery percentage or present parameters change*/
			bool changed = false;
			int i;

			/*
			 * A battery appearing or disappearing is a power_supply add or
			 * remove, not a change on a node we already watch, so the list
			 * itself has to be rebuilt: docking a PinePhone in its keyboard
			 * adds a second battery that was not there at boot.
			 */
			const char *action = udev_device_get_action(dev);

			if (action && (0 == g_strcmp0(action, "add") ||
			               0 == g_strcmp0(action, "remove")))
			{
				detect_battery_sysfs_paths();
				changed = true;
			}

			for (i = 0; i < batteries_count; i++)
			{
				bool present = battery_is_present(i);
				int percentage = present ? battery_percent(i) : 0;

				if (present != batteries[i].last_present ||
				        percentage != batteries[i].last_percentage)
				{
					changed = true;
				}

				batteries[i].last_present = present;
				batteries[i].last_percentage = percentage;
			}

			udev_device_unref(dev);

			if (changed && battery_callback != NULL)
			{
				battery_callback(nyxDev, NYX_CALLBACK_STATUS_DONE, battery_callback_context);
			}
		}
		else
		{
			if (battery_callback != NULL)
			{
				battery_callback(nyxDev, NYX_CALLBACK_STATUS_DONE, battery_callback_context);
			}
		}
	}

	return TRUE;
}

static void battery_cleanup(void)
{
	/*
	 * The udev monitor owns its file descriptor, so the GIOChannel wrapped
	 * around it must not close it - see battery_init(). Drop the watch first
	 * so glib stops polling the fd, then let udev_monitor_unref() close it.
	 *
	 * This used to drop the monitor pointer without unreffing it, leaking the
	 * monitor and its ref on the udev context on every deinit, while the
	 * channel closed a descriptor it did not own.
	 */
	if (0 != watch)
	{
		g_source_remove(watch);
		watch = 0;
	}

	if (NULL != mon)
	{
		udev_monitor_unref(mon);
		mon = NULL;
	}

	if (NULL != udev)
	{
		udev_unref(udev);
		udev = NULL;
	}

	battery_forget_all();

	return;
}

nyx_error_t battery_init(void)
{
	int fd;
	GIOChannel *channel = NULL;

	udev = udev_new();

	if (!udev)
	{
		nyx_error(MSGID_NYX_MOD_UDEV_ERR, 0 ,
		          "Could not initialize udev component; battery status updates will not be available");
		return NYX_ERROR_GENERIC;
	}

	/*Initialize the sysfs paths, and with them the current present/percentage values*/
	detect_battery_sysfs_paths();

	mon = udev_monitor_new_from_netlink(udev, "kernel");

	if (mon == NULL)
	{
		nyx_error(MSGID_NYX_MOD_UDEV_MONITOR_ERR, 0,
		          "Failed to create udev monitor for kernel events");
		battery_cleanup();
		return NYX_ERROR_GENERIC;
	}

	if (udev_monitor_filter_add_match_subsystem_devtype(mon, "power_supply",
	        NULL) < 0)
	{
		nyx_error(MSGID_NYX_MOD_UDEV_SUBSYSTEM_ERR, 0,
		          "Failed to setup udev filter for power_supply subsytem events");
		battery_cleanup();
		return NYX_ERROR_GENERIC;
	}

	if (udev_monitor_enable_receiving(mon) < 0)
	{
		nyx_error(MSGID_NYX_MOD_UDEV_RECV_ERR, 0,
		          "Failed to enable receiving kernel events for power_supply subsytem\n");
		battery_cleanup();
		return NYX_ERROR_GENERIC;
	}

	/* Setup io watch for uevents */
	fd = udev_monitor_get_fd(mon);

	if (-1 == fd)
	{
		battery_cleanup();
		return NYX_ERROR_GENERIC;
	}

	channel = g_io_channel_unix_new(fd);

	if (!channel)
	{
		battery_cleanup();
		return NYX_ERROR_GENERIC;
	}

	/* add watch event (which adds a ref) before calling g_io_channel_unref */
	watch = g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_NVAL, _handle_event,
	                       NULL);

	/* Remove the ref from g_io_channel_unix_new so we won't leak the channel if g_io_add_watch failed */
	/* watch holds another ref which is removed in battery_cleanup */
	/*
	 * Deliberately not g_io_channel_set_close_on_unref(): the fd belongs to
	 * the udev monitor, which closes it in battery_cleanup(). Letting the
	 * channel close it too would close a descriptor number that libudev still
	 * believes it holds, and that the kernel may already have handed to
	 * something else.
	 */
	g_io_channel_unref(channel);

	if (0 == watch)
	{
		battery_cleanup();
		return NYX_ERROR_GENERIC;
	}

	return NYX_ERROR_NONE;
}

nyx_error_t battery_deinit(void)
{
	battery_cleanup();
	return NYX_ERROR_NONE;
}

#if 0
// not currently called by batterylib.c
bool battery_is_authenticated(const char *pair_challenge,
                              const char *pair_response)
{
	/* not supported */
	return true;
}
#endif

bool battery_authenticate(void)
{
	/* not supported */
	return true;
}

void battery_set_wakeup_percent(int percentage)
{
	/* not supported */
	return;
}

void battery_set_fakemode(bool enable)
{
	battery_device_t *b = battery_at(BATTERY_PRIMARY);
	char buf[32];

	if (!b)
	{
		return;
	}

	snprintf(buf, sizeof(buf), "%d %s", enable, "1 100 40 4100 80 1");
	nyx_utils_write(b->fake_battery_path, buf, sizeof(buf));

	return;
}

nyx_error_t battery_get_fakemode(bool *enable)
{
	battery_device_t *b = battery_at(BATTERY_PRIMARY);
	char buf[32];

	if (enable == NULL || b == NULL)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	if (nyx_utils_read(b->fake_battery_path, buf, sizeof(buf)))
	{
		*enable = strstr(buf, "NORMAL") == 0;
	}
	else
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	return NYX_ERROR_NONE;
}
