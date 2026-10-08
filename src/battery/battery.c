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

/*
 * How far charge_full may sit from charge_full_design before the two are
 * taken to be in different units rather than describing a worn pack. A
 * battery at a fifth of what it shipped with is scrap but physically real;
 * one holding twice what it shipped with is not, and a decimal mismatch
 * lands an order of magnitude outside either bound.
 */
#define BATTERY_DESIGN_RATIO_MIN 0.2
#define BATTERY_DESIGN_RATIO_MAX 2.0

#define PATH_LEN 256

/* Longest power_supply status string is "Not charging". */
#define STATUS_LEN 32

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
	char charge_counter_path[PATH_LEN];
	char health_path[PATH_LEN];
	char temperature_path[PATH_LEN];
	char voltage_path[PATH_LEN];
	char current_path[PATH_LEN];
	char current_avg_path[PATH_LEN];
	char present_path[PATH_LEN];
	char status_path[PATH_LEN];
	char capacity_level_path[PATH_LEN];
	char fake_battery_path[PATH_LEN];

	/*
	 * What the device configuration says the pack holds, in mAh, or 0 where it
	 * says nothing. Only the primary battery ever has either: they come from
	 * [module.battery] in nyx.conf, which describes the device's own pack. See
	 * battery_conf_capacity_mah().
	 */
	int full_capacity_mah;
	int design_capacity_mah;

	/* last values seen, so _handle_event only wakes callers on a change */
	int last_percentage;
	bool last_present;
} battery_device_t;

/*
 * Not locked, and it must stay that way only as long as the assumption below
 * holds: every entry point runs on the client's default GMainContext - the nyx
 * API calls because the client makes them from its main loop, and _handle_event
 * because that is where g_io_add_watch() attached the udev watch. A caller that
 * queried from a second thread could be inside battery_is_present() reading
 * b->sysfs_path while a udev add/remove event has detect_battery_sysfs_paths()
 * g_free() it - a use-after-free, not merely a stale reading. Add a mutex here
 * before adding any such caller.
 */
static battery_device_t batteries[MAX_BATTERIES];
static int batteries_count = 0;

nyx_battery_ctia_t battery_ctia_params;

struct udev *udev = NULL;
struct udev_monitor *mon = NULL;
guint watch = 0;

/*
 * Consecutive uevent wakeups that produced no device. See _handle_event(): a
 * handful is normal (a message that failed the filter), a run of them means the
 * socket is wedged and has to be rebuilt. The charger module carries the same
 * guard for the same reason.
 */
static int empty_reads = 0;
#define BATTERY_MAX_EMPTY_READS 16

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

/*
 * battery_name() and battery_role() hand out pointers into the battery table.
 * Copy what you need before returning to the main loop: a udev add/remove
 * event rebuilds the table in place, so a pointer held across one describes a
 * different battery afterwards.
 */
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

/*
 * Fill in the path of an attribute the supply may or may not have, leaving it
 * empty when it does not, so callers can skip it without a failed read. The
 * charger module carries the same helper for the same reason.
 */
static void _optional_attr_path(char *dst, const char *dir, const char *attr)
{
	snprintf(dst, PATH_LEN, "%s/%s", dir, attr);

	if (!g_file_test(dst, G_FILE_TEST_EXISTS))
	{
		dst[0] = '\0';
	}
}

/*
 * nyx_utils_read_value() for a path that may legitimately be empty, because
 * _optional_attr_path() found the attribute absent. Returns the same -1 a
 * failed read returns, without logging: "this driver does not implement this
 * property" is not an error, and on a lean mainline driver it would otherwise
 * be logged on every poll of every missing property.
 */
static int _read_optional_value(const char *path)
{
	if (!path || !path[0])
	{
		return -1;
	}

	return nyx_utils_read_value(path);
}

/* No pack a device carries is anywhere near a thousand amp-hours. */
#define BATTERY_CONF_CAPACITY_MAX_MAH 1000000

/*
 * A pack capacity the device configuration states, from [module.battery] in
 * nyx.conf, in mAh. 0 where the key is absent, empty or not a capacity, so
 * the driver's own figure is used exactly as it was before.
 *
 * Some fuel gauges cannot say what their pack holds. A MediaTek gauge whose
 * kernel carries the vendor's reference capacity table, not this pack's,
 * exports charge_full from that table - 2946 mAh on a pack the stock gauge
 * profile puts at about 5 Ah - and a charge_full_design in a different unit
 * again. No amount of cross-checking the two recovers a number the driver
 * never had, and patching every vendor driver is not something this module
 * can do. The device's own configuration knows the pack, so it can say.
 *
 * It is the device's word, not a guess, which is why a stated figure is
 * trusted over the driver's where the driver's is merely derived.
 */
static int battery_conf_capacity_mah(const char *key)
{
	gchar *value = nyx_conf_get_path("module.battery", key);
	gchar *end = NULL;
	gint64 mah;

	if (!value)
	{
		return 0;
	}

	mah = g_ascii_strtoll(value, &end, 10);

	if (end == value || *end != '\0' || mah <= 0
	        || mah > BATTERY_CONF_CAPACITY_MAX_MAH)
	{
		nyx_warn(MSGID_NYX_MOD_BATT_CONF_CAPACITY, 0,
		         "ignoring [module.battery] %s=%s: not a capacity in mAh",
		         key, value);
		mah = 0;
	}

	g_free(value);
	return (int) mah;
}

/*
 * The driver's own account of what the pack is doing, in the strings the
 * power_supply class defines: "Charging", "Discharging", "Not charging",
 * "Full" or "Unknown". false when the supply exports no status at all, which
 * is the one case where callers have to guess instead.
 */
static bool _battery_status(int index, char *out, size_t len)
{
	battery_device_t *b = battery_at(index);
	char level[STATUS_LEN];
	bool have_status;

	if (!b)
	{
		return false;
	}

	have_status = b->status_path[0]
	              && FileGetString(b->status_path, out, len) >= 0;

	/*
	 * capacity_level has the last word when it says "Full", because a driver
	 * that reports a full pack and a status of "Charging" at the same time is
	 * contradicting itself and the capacity is the half to believe. The
	 * MindPhone's MT6739 gauge does exactly that: it sits at capacity 100 with
	 * capacity_level "Full" and never moves status off "Charging", so taking
	 * status at its word reported a full battery as still charging for as long
	 * as it stayed on the cable.
	 *
	 * Reported as "Full" rather than as "not charging" so the one override
	 * answers both questions this is asked: the charging flag goes false, and
	 * the current keeps its sign unexamined, which is already what "Full"
	 * means to the normalisation below - a pack that is done charging is not
	 * obviously moving charge in either direction.
	 */
	if (b->capacity_level_path[0]
	        && FileGetString(b->capacity_level_path, level, sizeof(level)) >= 0
	        && 0 == g_ascii_strcasecmp(level, "Full"))
	{
		g_strlcpy(out, "Full", len);
		return true;
	}

	return have_status;
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
	if ((capacity = _read_optional_value(b->capacity_path)) < 0)
	{
		/* capacity node is not available so next try is energy_full path */
		if (g_file_test(b->energy_full_path, G_FILE_TEST_EXISTS))
		{
			if ((now = _read_optional_value(b->energy_now_path)) < 0)
			{
				return -1;
			}

			if ((full = _read_optional_value(b->energy_full_path)) <= 0)
			{
				return -1;
			}

			capacity = (int)((gint64) 100 * now / full);
		}
		/* as last try we can use charge_now path */
		else if (g_file_test(b->charge_now_path, G_FILE_TEST_EXISTS))
		{
			if ((full = _read_optional_value(b->charge_full_path)) <= 0)
			{
				return -1;
			}

			if ((now = _read_optional_value(b->charge_now_path)) < 0)
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
	if (!b || !b->temperature_path[0]
	        || FileGetDouble(b->temperature_path, &temp) < 0)
	{
		return -1;
	}

	/*
	 * The power_supply class exports temp in tenths of a degree Celsius,
	 * so 293 is 29.3 degrees. Everything that consumes this treats it as
	 * whole degrees: batteryd publishes it as "temperature_C", and the
	 * CTIA limits a few lines up in this file - 0, 57 and 60 - are plainly
	 * degrees. Left unconverted, an ordinary 30 degree battery read as 300
	 * and sat permanently above every one of them.
	 *
	 * Rounded rather than truncated so that a battery below freezing does
	 * not come out warmer than it is: -5.5 degrees is -6, not -5.
	 */
	return (int)((temp < 0) ? (temp / 10.0 - 0.5) : (temp / 10.0 + 0.5));
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

	if (!b || (voltage = _read_optional_value(b->voltage_path)) < 0)
	{
		return -1;
	}

	/*
	 * voltage_now is in microvolts. batteryd publishes this as
	 * "voltage_mV", and battery_coulomb() and battery_full40() below
	 * already divide their own microamp-hour readings down for exactly the
	 * same reason - these three simply never got the same treatment, so a
	 * 3.84 V cell was reported as 3843000 mV.
	 *
	 * The old test here asked "should this be in mV or uV? Device returns
	 * uV but emulator returns mV". The kernel's power_supply ABI settles
	 * it: voltage_now is microvolts. A driver reporting millivolts is out
	 * of spec, and is a bug in that driver rather than something to guess
	 * at here - magnitude cannot be used to tell them apart for current or
	 * temperature, so guessing for one of the three and not the others
	 * would be worse than being consistent.
	 */
	return voltage / 1000;
}

/**
 * @brief Read the amount of current being drawn by the battery (negative = charging!)
 *
 * @retval Current (integer)
 */
/*
 * Read one of the signed current attributes, in milliamps, with its sign
 * normalised so that positive is into the pack.
 *
 * current_now is signed, and nyx_utils_read_value() collapses "value is
 * negative" with "read failed", which reported -1 the whole time a device ran
 * on battery. FileGetDouble() signals errors through its return code instead
 * and stores the reading through the out-parameter, so a negative one
 * survives the trip.
 *
 * Which sign means charging is not fixed by the power_supply ABI, and drivers
 * split both ways: mainline gauges report charging as positive, Qualcomm's
 * downstream charger and fuel-gauge drivers report it as negative. A sargo
 * charging at 410 mA reads current_now = -410156.
 *
 * Publishing that raw just moves the question to every consumer, and they
 * answer it inconsistently - batteryd's own poll loop and the settings app
 * both take a negative current for discharging, so a charging sargo came out
 * as "Current: -410 mA (discharging)" beside "Status: Charging".
 *
 * So normalise it here, once, to the convention the consumers already assume.
 * status carries the direction and the attribute the magnitude. "Not
 * charging", "Full" and "Unknown" say nothing about which way a current is
 * flowing, so leave those alone rather than invent a direction for them - as
 * with a driver that exports no status at all.
 */
static int _read_current_mA(int index, const char *path)
{
	char status[STATUS_LEN];
	double current = 0;

	if (!path || !path[0] || FileGetDouble(path, &current) < 0)
	{
		return -1;
	}

	if (_battery_status(index, status, sizeof(status)))
	{
		double magnitude = (current < 0) ? -current : current;

		if (0 == g_ascii_strcasecmp(status, "Charging"))
		{
			current = magnitude;
		}
		else if (0 == g_ascii_strcasecmp(status, "Discharging"))
		{
			current = -magnitude;
		}
	}

	/* Microamps, for the same reason as the voltage above: batteryd
	 * publishes it as "current_mA". Signed, so this truncates toward zero
	 * on both sides, which is what dropping sub-milliamp precision should
	 * do. */
	return (int)(current / 1000.0);
}

int battery_current(int index)
{
	battery_device_t *b = battery_at(index);

	return b ? _read_current_mA(index, b->current_path) : -1;
}

/**
 * @brief Read average current being drawn by the battery.
 *
 * @retval Current (integer)
 */

int battery_avg_current(int index)
{
	battery_device_t *b = battery_at(index);

	if (!b)
	{
		return -1;
	}

	/*
	 * current_avg is the attribute the class defines for this, so where a
	 * driver exports it that is the answer - and on some it is the only
	 * reading that works. The MindPhone's MT6739 fuel gauge pins current_now
	 * to 0 while current_avg carries the 138 mA actually flowing, so taking
	 * the instantaneous node there reported no current at all.
	 *
	 * Fall back to current_now where there is no current_avg, which is what
	 * every caller got before and what a gauge with only the one node can
	 * answer.
	 */
	if (b->current_avg_path[0])
	{
		return _read_current_mA(index, b->current_avg_path);
	}

	return battery_current(index);
}

/**
 * @brief Whether the pack is taking charge, from the driver's own verdict.
 *
 * The sign of current_now cannot answer this. The power_supply ABI does not
 * fix which direction is positive and drivers split both ways: mainline
 * gauges report charging as positive, while Qualcomm's downstream charger
 * and fuel-gauge drivers report it as negative. A sargo charging at 410 mA
 * reads current_now = -410156 with status = "Charging", so a caller testing
 * the sign for "> 0" calls that discharging - and one testing for "< 0"
 * would get the PinePhone wrong the same way.
 *
 * The status attribute is the driver saying it outright, in the same strings
 * the charger module already matches on, so prefer it and leave the sign to
 * the caller as a fallback for a driver that does not export it.
 *
 * "Full" is deliberately not charging: the pack is connected but no longer
 * taking charge, and the charger module raises charge-complete separately.
 *
 * @retval 1 charging, 0 not charging, -1 no status attribute to ask.
 */
int battery_charging_state(int index)
{
	char status[STATUS_LEN];

	if (!_battery_status(index, status, sizeof(status)))
	{
		return -1;
	}

	return (0 == g_ascii_strcasecmp(status, "Charging")) ? 1 : 0;
}

/*
 * What the driver's charge_full says the pack holds when full, in mAh, whatever
 * the device configuration may say about it; -1 where there is no such
 * attribute or it reads zero. Zero is not a capacity a present pack can have:
 * a gauge that failed to load its profile reports it, and passing it on claims
 * the pack holds nothing.
 */
static double _driver_charge_full_mah(const battery_device_t *b)
{
	int charge_full = -1;

	if (g_file_test(b->charge_full_path, G_FILE_TEST_EXISTS))
	{
		charge_full = _read_optional_value(b->charge_full_path);
	}

	if (charge_full <= 0)
	{
		return -1;
	}

	/* Divide the value by 1000 to convert from uAh to mAh */
	return (double) charge_full / 1000;
}

/*
 * The same, falling back to the design figure where charge_full has nothing to
 * say - and if that is zero too, there is no answer to give. Same reasoning as
 * battery_coulomb().
 */
static double _driver_full_mah(const battery_device_t *b)
{
	double full = _driver_charge_full_mah(b);
	int charge_full_design;

	if (full > 0)
	{
		return full;
	}

	charge_full_design = _read_optional_value(b->charge_full_design_path);

	if (charge_full_design <= 0)
	{
		return -1;
	}

	return (double) charge_full_design / 1000;
}

/**
 * @brief Read battery full capacity
 *
 * What the device configuration states, if it states one
 * (see battery_conf_capacity_mah()), otherwise what the driver reports.
 *
 * A stated figure is still the answer for a battery that is there and no
 * answer for one that is not: a primary whose node has gone reports -1, as it
 * always did, not the capacity of a pack nobody can see.
 *
 * @retval Battery capacity (double), or -1 where there is none to give
 */
double battery_full40(int index)
{
	battery_device_t *b = battery_at(index);

	if (!b)
	{
		return -1;
	}

	if (b->full_capacity_mah > 0)
	{
		return battery_is_present(index) ? b->full_capacity_mah : -1;
	}

	return _driver_full_mah(b);
}

/**
 * @brief Read the capacity the pack shipped with, in mAh.
 *
 * The counterpart to battery_full40(): that is what the gauge believes the
 * pack holds now, this is what it held when it was made, and only the two
 * together say anything about wear. Deliberately no fallback to charge_full -
 * answering the design question with the present capacity would report every
 * pack as factory fresh, which is worse than not answering.
 *
 * What the device configuration states, if it states one - and it only does
 * so alongside a full capacity, see detect_battery_sysfs_paths() - otherwise
 * what the driver reports, after the unit cross-check below.
 *
 * @retval Design capacity in mAh, or -1 where there is none to give: the
 *         driver reports none, or reports one that is not in the same unit as
 *         charge_full, or the battery is not there.
 */
double battery_full_design(int index)
{
	battery_device_t *b = battery_at(index);
	int charge_full_design, charge_full;
	double design;

	/*
	 * The device configuration's own figure is not subject to the unit
	 * cross-check below: that exists to catch a driver attribute nobody has
	 * said anything about, and this one the device's configuration has. It
	 * does not make the figure right - nothing here can check it against the
	 * pack - which is why it is only ever taken together with the full
	 * capacity it is to be compared with. It is also what lets a device whose
	 * driver reports no usable design capacity say so anyway.
	 */
	if (b && b->design_capacity_mah > 0)
	{
		return battery_is_present(index) ? b->design_capacity_mah : -1;
	}

	if (!b ||
	        (charge_full_design =
	             _read_optional_value(b->charge_full_design_path)) < 0)
	{
		return -1;
	}

	/* Divide the value by 1000 to convert from uAh to mAh */
	design = (double) charge_full_design / 1000;

	/*
	 * Not every driver reports this attribute in the unit the ABI says. Two
	 * unrelated Halium ports - a MediaTek device and a radon - report
	 * charge_full in uAh and charge_full_design a factor of ten below it:
	 *
	 *     charge_full = 2951000   charge_full_design = 295000
	 *     charge_full = 4370000   charge_full_design = 437000
	 *
	 * charge_full is the one to believe on both; dividing charge_counter by
	 * it gives the percentage the "capacity" attribute reports. Taken at
	 * face value the design figure makes a phone look like it shipped with a
	 * 295 mAh pack and is now holding ten times that, which downstream comes
	 * out as a battery at 1000% of its original capacity.
	 *
	 * So cross-check the two before believing the design figure. A pack's
	 * present capacity is somewhere between a worn-out fraction of what it
	 * shipped with and a little over it - never several times it - so a
	 * ratio outside that band is two different units rather than a
	 * measurement, and there is no answer to give.
	 *
	 * Deliberately not rescaled by the factor of ten that would make these
	 * two devices line up: the attribute would still be one this driver
	 * cannot be trusted about, and a guessed correction that happens to look
	 * plausible is worse than saying nothing. Nothing is lost on either
	 * device, where the corrected figure would equal charge_full and so
	 * report a gauge that does not measure wear anyway.
	 */
	if (g_file_test(b->charge_full_path, G_FILE_TEST_EXISTS) &&
	        (charge_full = _read_optional_value(b->charge_full_path)) > 0)
	{
		double full = (double) charge_full / 1000;

		if (full > design * BATTERY_DESIGN_RATIO_MAX ||
		        full < design * BATTERY_DESIGN_RATIO_MIN)
		{
			nyx_warn(MSGID_NYX_MOD_BATT_DESIGN_UNIT, 0,
			         "battery %d: charge_full %.0f mAh and charge_full_design "
			         "%.0f mAh are not the same unit; reporting no design "
			         "capacity", index, full, design);
			return -1;
		}
	}

	return design;
}

/**
 * @brief Read the condition the driver reports the battery to be in.
 *
 * The kernel's own verdict, from the "health" attribute, mapped onto
 * nyx_battery_health_t. It is not worked out from any of the readings and is
 * a different question from wear: a pack reporting "Good" can still hold half
 * what it once did.
 *
 * @retval One of nyx_battery_health_t; NYX_BATTERY_HEALTH_UNKNOWN where there
 *         is no such attribute or it says something this does not recognise.
 */
int battery_health(int index)
{
	battery_device_t *b = battery_at(index);
	char health[64] = "";
	size_t i;

	static const struct
	{
		const char *name;
		int value;
	} known[] =
	{
		{ "Good",                    NYX_BATTERY_HEALTH_GOOD },
		{ "Overheat",                NYX_BATTERY_HEALTH_OVERHEAT },
		{ "Dead",                    NYX_BATTERY_HEALTH_DEAD },
		{ "Over voltage",            NYX_BATTERY_HEALTH_OVERVOLTAGE },
		{ "Unspecified failure",     NYX_BATTERY_HEALTH_UNSPEC_FAILURE },
		{ "Cold",                    NYX_BATTERY_HEALTH_COLD },
		{ "Watchdog timer expire",   NYX_BATTERY_HEALTH_WATCHDOG_TIMER_EXPIRE },
		{ "Safety timer expire",     NYX_BATTERY_HEALTH_SAFETY_TIMER_EXPIRE },
		{ "Over current",            NYX_BATTERY_HEALTH_OVERCURRENT },
		{ "Calibration required",    NYX_BATTERY_HEALTH_CALIBRATION_REQUIRED },
		{ "Warm",                    NYX_BATTERY_HEALTH_WARM },
		{ "Cool",                    NYX_BATTERY_HEALTH_COOL },
		{ "Hot",                     NYX_BATTERY_HEALTH_HOT },
		{ "No battery",              NYX_BATTERY_HEALTH_NO_BATTERY },
	};

	if (!b || !b->health_path[0]
	        || FileGetString(b->health_path, health, sizeof(health)) < 0)
	{
		return NYX_BATTERY_HEALTH_UNKNOWN;
	}

	for (i = 0; i < G_N_ELEMENTS(known); i++)
	{
		if (0 == g_ascii_strcasecmp(health, known[i].name))
		{
			return known[i].value;
		}
	}

	return NYX_BATTERY_HEALTH_UNKNOWN;
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
 * @brief Read how much charge is in the battery right now, in mAh.
 *
 * charge_now is the obvious node and the one to prefer, but it is not the
 * only node that carries this and on some drivers it is not the one that
 * works. Qualcomm's charger and fuel-gauge drivers export the charge on the
 * "battery" supply as charge_counter with no charge_now beside it at all,
 * and put a charge_now on the companion "bms" supply that is hardwired to
 * zero while the charge_now_raw next to it holds the real figure. Reading
 * only charge_now therefore answers -1 on a pack that is reporting perfectly
 * well.
 *
 * charge_counter is the same quantity in the same unit - accumulated charge
 * in uAh - and is what Android reads for BATTERY_PROPERTY_CHARGE_COUNTER, so
 * falling back to it costs nothing where charge_now works and is what makes
 * the question answerable at all where it does not.
 *
 * A charge_now of zero falls back for the same reason: a pack with no charge
 * left in it is a device that has switched off, so in practice a zero here
 * only ever means the node is not wired up.
 *
 * Which is why a zero still left after the fallback is reported as no answer
 * rather than as a measurement. A sargo whose fuel gauge never loaded its
 * battery profile exports charge_counter = 0 and charge_full = 0 on a pack
 * sitting at 61%, and bms answers 0 for every one of them too, so there is
 * nothing better to read - but "0 mAh" is a claim about the hardware that
 * contradicts the percentage beside it, where -1 is the truth that this gauge
 * cannot say. Callers already treat a negative as "do not show this".
 *
 * Where the device configuration states the pack's capacity
 * (see battery_conf_capacity_mah()) a charge taken from charge_counter is
 * brought into the same scale as that figure, and is never more than it. A
 * charge_now is reported as the driver gives it.
 *
 * @retval Battery capacity in mAh, or -1 where no node can answer
 */

double battery_coulomb(int index)
{
	battery_device_t *b = battery_at(index);
	int charge_now, charge_counter;
	bool from_counter = false;
	double charge;

	if (!b)
	{
		return -1;
	}

	if ((charge_now = _read_optional_value(b->charge_now_path)) <= 0)
	{
		if ((charge_counter =
		         _read_optional_value(b->charge_counter_path)) >= 0)
		{
			charge_now = charge_counter;
			from_counter = true;
		}
	}

	if (charge_now <= 0)
	{
		return -1;
	}

	/*
	 * A charge_now under 1 mAh beside a percentage above zero is not a
	 * charge. Samsung's sec-battery driver puts its charging mode there
	 * (sec_battery.c: case POWER_SUPPLY_PROP_CHARGE_NOW: val->intval =
	 * battery->charging_mode), so a Galaxy A3 (2015) at 23% reported
	 * 0.001 mAh. No pack that still runs a device holds less than 1 mAh, so
	 * read such a value as no charge_now at all: the stated capacity times the
	 * percentage where the device configuration states one, otherwise no
	 * answer.
	 */
	if (!from_counter && charge_now < 1000)
	{
		int percent = battery_percent(index);

		if (percent > 0)
		{
			if (b->full_capacity_mah > 0)
			{
				return (double) b->full_capacity_mah * percent / 100;
			}

			return -1;
		}
	}

	/* Divide the value by 1000 to convert from uAh to mAh */
	charge = (double) charge_now / 1000;

	/*
	 * Where the device configuration states the pack's capacity and the charge
	 * is the counter's, correct the counter. On the gauges the key exists for,
	 * charge_counter is not a measurement: the driver computes it as
	 * charge_full times the percentage, from the same wrong table, so it is in
	 * the same wrong scale as charge_full and both want the same factor.
	 * Reporting it against a corrected full capacity would put a pack the
	 * percentage says is full at well under its own capacity.
	 *
	 * A charge_now is left alone. A driver that measures it is reading the
	 * pack, not a table, and rescaling a correct reading by a factor taken from
	 * the attribute that is wrong would corrupt the one figure that was right.
	 *
	 * Only charge_full can supply the factor. The design figure is the one
	 * attribute known to come in a different unit (see battery_full_design()),
	 * so a ratio against it would be wrong by exactly that unit. With no
	 * charge_full to take the factor from, the percentage is the only other
	 * statement of how full the pack is.
	 */
	if (b->full_capacity_mah > 0 && from_counter)
	{
		double driver_full = _driver_charge_full_mah(b);

		if (driver_full > 0)
		{
			charge = charge * b->full_capacity_mah / driver_full;
		}
		else
		{
			int percent = battery_percent(index);

			if (percent < 0)
			{
				return -1;
			}

			charge = (double) b->full_capacity_mah * percent / 100;
		}
	}

	/*
	 * The two readings behind the factor are taken a moment apart, and a gauge
	 * that reloads its profile between them would hand back a charge the pack
	 * cannot hold - or a percentage above a hundred on the fallback above.
	 * Never report more than a full pack, and - as above - no charge at all as
	 * no answer, not as a measurement.
	 */
	if (b->full_capacity_mah > 0 && charge > b->full_capacity_mah)
	{
		charge = b->full_capacity_mah;
	}

	return (charge > 0) ? charge : -1;
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

	if ((present = _read_optional_value(b->present_path)) < 0)
	{
		return false;
	}

	return (1 == present);
}

/**
 * @brief Point a path at the BMS's copy of an attribute if the battery has none.
 *
 * The attribute is named rather than recovered from the path, because an
 * attribute the battery does not have leaves the path empty - that is what
 * _optional_attr_path() does and the whole reason to come looking here - so
 * there is no name left in it to read back.
 *
 * The gauge is found by node name, not by type: Qualcomm's kernels map
 * POWER_SUPPLY_TYPE_BMS onto the sysfs string "Mains", so asking for a supply
 * of type "BMS" never matches anything.
 *
 * @param path  rewritten in place when it is empty and a power_supply of type
 *              "BMS" carries the attribute. Left alone when it already points
 *              somewhere, so a battery that has the attribute keeps it.
 * @param attribute the power_supply attribute name to look for under "BMS".
 */
static void battery_prefer_bms_path(char *path, const char *attribute)
{
	char *bms_path;
	char candidate[PATH_LEN];

	if (!path || !attribute || path[0])
	{
		return;
	}

	bms_path = find_power_supply_sysfs_path_by_name("bms");

	if (!bms_path)
	{
		return;
	}

	snprintf(candidate, PATH_LEN, "%s/%s", bms_path, attribute);

	if (g_file_test(candidate, G_FILE_TEST_EXISTS))
	{
		g_strlcpy(path, candidate, PATH_LEN);
	}

	g_free(bms_path);
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

	/*
	 * Every one of these is optional. The power_supply class does not
	 * mandate any property, and mainline drivers are consistently leaner
	 * than the vendor ones they replace: across the 74 battery-type drivers
	 * in 7.3, status is present in 85%, voltage_now 76%, current_now and
	 * present 60%, capacity 58%, health and temp 50% each, and only 19%
	 * provide all of current_now + health + temp. A driver exposing six
	 * properties (rt5033, the Samsung A3/A5 2015 fuel gauge) is below the
	 * median of ten but entirely ordinary.
	 *
	 * Assembling the paths unconditionally made every read of an absent
	 * attribute an error: batteryd logged NYXUTIL_GET_DOUBLE_ERR /
	 * NYXUTIL_GET_STRING_ERR per property per poll - 68 lines in one boot
	 * on an A3 - while reporting capacity and status perfectly well. Probe
	 * once here instead and leave the path empty when the attribute is not
	 * there; the readers below treat an empty path as "not available" and
	 * return exactly what they used to return on a failed read, minus the
	 * logging. Same approach _optional_attr_path() already takes in the
	 * charger module.
	 */
	_optional_attr_path(b->capacity_path, sysfs_path, "capacity");
	_optional_attr_path(b->energy_now_path, sysfs_path, "energy_now");
	_optional_attr_path(b->energy_full_path, sysfs_path, "energy_full");
	_optional_attr_path(b->energy_full_design_path, sysfs_path,
	                    "energy_full_design");
	_optional_attr_path(b->charge_now_path, sysfs_path, "charge_now");
	_optional_attr_path(b->charge_full_path, sysfs_path, "charge_full");
	_optional_attr_path(b->charge_full_design_path, sysfs_path,
	                    "charge_full_design");
	_optional_attr_path(b->charge_counter_path, sysfs_path, "charge_counter");
	_optional_attr_path(b->temperature_path, sysfs_path, "temp");
	_optional_attr_path(b->voltage_path, sysfs_path, "voltage_now");
	_optional_attr_path(b->current_path, sysfs_path, "current_now");
	_optional_attr_path(b->current_avg_path, sysfs_path, "current_avg");
	_optional_attr_path(b->present_path, sysfs_path, "present");
	_optional_attr_path(b->status_path, sysfs_path, "status");
	_optional_attr_path(b->capacity_level_path, sysfs_path,
	                    "capacity_level");
	_optional_attr_path(b->fake_battery_path, sysfs_path, "pseudo_batt");
	_optional_attr_path(b->health_path, sysfs_path, "health");

	/*
	 * How big the pack is, and how big it used to be, are fuel-gauge
	 * questions, and the supply of type "Battery" is not always the fuel
	 * gauge. On Qualcomm platforms the charger driver owns that supply and
	 * a separate one of type "BMS" is the gauge: a tissot has charge_full
	 * on both but charge_full_design only on the BMS, so asking the battery
	 * supply alone answers "this pack has no factory capacity" about a pack
	 * that publishes one a directory away.
	 *
	 * Only these two fall back. The readings that describe the moment -
	 * voltage, current, temperature, charge - come from the supply the
	 * module picked and stay there, so nothing starts silently mixing two
	 * sources for the same instant.
	 */
	battery_prefer_bms_path(b->charge_full_path, "charge_full");
	battery_prefer_bms_path(b->charge_full_design_path,
	                        "charge_full_design");
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
	bool primary_pinned;
	int i;

	battery_forget_all();

	/* Runtime value from luneos-device-config wins over both. */
	primary_path = nyx_conf_get_path("module.battery", "sysfs_path");
	primary_pinned = (primary_path != NULL);

	if (!primary_path)
	{
#ifdef BATTERY_SYSFS_PATH
		primary_pinned = true;
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

	/*
	 * The capacities in [module.battery] describe the device's own pack, which
	 * is the primary. An accessory's cell - a keyboard's - is not covered by
	 * them and keeps whatever its own driver reports.
	 *
	 * That only holds if the primary is the device's pack, and only a primary
	 * the configuration or the build named is known to be: one found by the
	 * directory walk is whichever Battery supply the filesystem lists first,
	 * which with a keyboard docked may be the keyboard's. So the figures are
	 * taken for a named primary only, and left unused - with a word about why -
	 * where the primary was guessed.
	 *
	 * The design capacity is only ever taken together with the full capacity.
	 * Either one alone pairs a figure the device states with one the driver
	 * does, and where the driver is the wrong one - the reason for having the
	 * keys - the pair comes out as a wear figure for a pack that has none.
	 */
	if (batteries_count > 0)
	{
		int full = battery_conf_capacity_mah("full_capacity_mah");
		int design = battery_conf_capacity_mah("design_capacity_mah");

		if ((full > 0 || design > 0) && !primary_pinned)
		{
			nyx_warn(MSGID_NYX_MOD_BATT_CONF_CAPACITY, 0,
			         "ignoring the capacities in [module.battery]: the primary "
			         "battery %s was not named by sysfs_path, so it may not be "
			         "the device's own pack", batteries[BATTERY_PRIMARY].sysfs_path);
			full = design = 0;
		}
		else if (design > 0 && full <= 0)
		{
			nyx_warn(MSGID_NYX_MOD_BATT_CONF_CAPACITY, 0,
			         "ignoring [module.battery] design_capacity_mah: it is only "
			         "taken together with full_capacity_mah");
			design = 0;
		}

		batteries[BATTERY_PRIMARY].full_capacity_mah = full;
		batteries[BATTERY_PRIMARY].design_capacity_mah = design;
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

static gboolean _battery_monitor_start(void);
gboolean _handle_event(GIOChannel *channel, GIOCondition condition, gpointer data);

/*
 * Drop the watch and the monitor, keeping the udev context. battery_cleanup()
 * tears down everything including udev and the battery list, which is far more
 * than a wedged socket calls for.
 */
static void _battery_monitor_stop(void)
{
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
}

/*
 * Build the power_supply uevent monitor and put a watch on it. Called from
 * battery_init() and again from _handle_event() when the descriptor it is
 * watching turns out to be dead.
 */
static gboolean _battery_monitor_start(void)
{
	GIOChannel *channel;
	int fd;

	_battery_monitor_stop();

	mon = udev_monitor_new_from_netlink(udev, "kernel");

	if (mon == NULL)
	{
		nyx_error(MSGID_NYX_MOD_UDEV_MONITOR_ERR, 0,
		          "Failed to create udev monitor for kernel events");
		return FALSE;
	}

	if (udev_monitor_filter_add_match_subsystem_devtype(mon, "power_supply",
	        NULL) < 0)
	{
		nyx_error(MSGID_NYX_MOD_UDEV_SUBSYSTEM_ERR, 0,
		          "Failed to setup udev filter for power_supply subsytem events");
		_battery_monitor_stop();
		return FALSE;
	}

	if (udev_monitor_enable_receiving(mon) < 0)
	{
		nyx_error(MSGID_NYX_MOD_UDEV_RECV_ERR, 0,
		          "Failed to enable receiving kernel events for power_supply subsytem");
		_battery_monitor_stop();
		return FALSE;
	}

	fd = udev_monitor_get_fd(mon);

	if (-1 == fd)
	{
		_battery_monitor_stop();
		return FALSE;
	}

	channel = g_io_channel_unix_new(fd);

	if (!channel)
	{
		_battery_monitor_stop();
		return FALSE;
	}

	/* add the watch (which takes a ref) before dropping ours */
	watch = g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
	                       _handle_event, NULL);

	/*
	 * Deliberately not g_io_channel_set_close_on_unref(): the fd belongs to
	 * the udev monitor, which closes it in udev_monitor_unref(). Letting the
	 * channel close it too would close a descriptor number that libudev still
	 * believes it holds, and that the kernel may already have handed to
	 * something else.
	 */
	g_io_channel_unref(channel);

	if (0 == watch)
	{
		_battery_monitor_stop();
		return FALSE;
	}

	empty_reads = 0;
	return TRUE;
}

gboolean _handle_event(GIOChannel *channel, GIOCondition condition,
                       gpointer data)
{
	struct udev_device *dev;

	if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
	{
		/*
		 * The netlink socket is gone. glib reports these conditions whether or
		 * not they were asked for, and returning TRUE here - as this used to,
		 * unconditionally, for every condition - has glib re-dispatch us
		 * immediately on a dead descriptor, for ever: a tight loop that burns a
		 * whole core and starves every other source in the process, batteryd's
		 * luna methods included. Rebuild the monitor instead.
		 */
		nyx_error(MSGID_NYX_MOD_UDEV_MONITOR_ERR, 0,
		          "power_supply uevent socket lost (condition 0x%x), reopening",
		          condition);
		watch = 0;
		_battery_monitor_start();
		return G_SOURCE_REMOVE;
	}

	if ((condition  & G_IO_IN) == G_IO_IN)
	{
		dev = udev_monitor_receive_device(mon);

		if (dev)
		{
			/*Initiate callback only if battery percentage or present parameters change*/
			bool changed = false;
			int i;

			empty_reads = 0;

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
			/*
			 * Readable, but nothing came back. udev_monitor_receive_device()
			 * returns NULL for a message that failed its filter, for a recv
			 * error, and for a socket that has gone bad - and in that last case
			 * the descriptor stays readable for ever.
			 *
			 * This used to fire the battery callback here and return TRUE, so a
			 * wedged socket did not merely spin: every pass told batteryd the
			 * battery had changed, which had it re-read sysfs and re-broadcast
			 * as fast as the loop could turn. Tolerate a few filtered messages,
			 * which are normal and transient, then rebuild the monitor the same
			 * way the hangup path above does, and say so once rather than
			 * thousands of times.
			 */
			if (++empty_reads < BATTERY_MAX_EMPTY_READS)
			{
				return TRUE;
			}

			nyx_error(MSGID_NYX_MOD_UDEV_MONITOR_ERR, 0,
			          "power_supply uevent socket readable but empty %d times, reopening",
			          empty_reads);
			watch = 0;

			if (_battery_monitor_start() && battery_callback != NULL)
			{
				/*
				 * A real change may have arrived while the socket was wedged,
				 * so ask for one re-read now that there is a working monitor.
				 */
				battery_callback(nyxDev, NYX_CALLBACK_STATUS_DONE,
				                 battery_callback_context);
			}

			return G_SOURCE_REMOVE;
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
	udev = udev_new();

	if (!udev)
	{
		nyx_error(MSGID_NYX_MOD_UDEV_ERR, 0 ,
		          "Could not initialize udev component; battery status updates will not be available");
		return NYX_ERROR_GENERIC;
	}

	/*Initialize the sysfs paths, and with them the current present/percentage values*/
	detect_battery_sysfs_paths();

	/* Same code path as the rebuild in _handle_event(), so both stay correct. */
	if (!_battery_monitor_start())
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

/**
 * @brief Turn the pseudo battery on or off.
 *
 * Still needed: an emulated target (qemux86 and friends) has no battery at
 * all, and the pseudo_batt node is how it is given one to report.
 */
void battery_set_fakemode(bool enable)
{
	battery_device_t *b = battery_at(BATTERY_PRIMARY);
	char buf[32];

	if (!b)
	{
		return;
	}

	snprintf(buf, sizeof(buf), "%d %s", enable, "1 100 40 4100 80 1");

	/*
	 * strlen, not sizeof: nyx_utils_write() writes exactly the number of
	 * bytes it is given and snprintf() does not pad, so sizeof handed the
	 * kernel the ten uninitialised stack bytes past the terminator.
	 */
	nyx_utils_write(b->fake_battery_path, buf, strlen(buf));

	return;
}

nyx_error_t battery_get_fakemode(bool *enable)
{
	battery_device_t *b = battery_at(BATTERY_PRIMARY);
	char buf[32] = "";

	if (enable == NULL || b == NULL)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	/*
	 * nyx_utils_read() returns the byte count, or -1 if it could not even
	 * open the node - which is the normal case, since pseudo_batt only
	 * exists on targets that have it. Testing for truth took that -1 as
	 * success and ran strstr() over an uninitialised buffer.
	 */
	if (nyx_utils_read(b->fake_battery_path, buf, sizeof(buf)) <= 0)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	/* The node reports "NORMAL" when it is passing the real battery through. */
	*enable = (NULL == strstr(buf, "NORMAL"));

	return NYX_ERROR_NONE;
}
