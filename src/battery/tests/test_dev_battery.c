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

#include <glib.h>
#include <stdio.h>

//
// Provide missing g_test macros if they are not defined in this version.
//
// We can't simply back-port the real definitions from glib as that would
// would change the license for this component.
//

#ifndef g_assert_true
#define g_assert_true(X) g_assert((X))
#endif

#ifndef g_assert_false
#define g_assert_false(X) g_assert(!(X))
#endif

#ifndef g_assert_nonnull
#define g_assert_nonnull(X) g_assert((X) != NULL)
#endif

#ifndef g_assert_null
#define g_assert_null(X) g_assert((X) == NULL)
#endif

//
// Pull in the relevant nyx headers. That way we can redefine macros
// if necessary (e.g. for logging) and the anti-recursion in the headers
// will let our redefinitions leak through into the UUT.
//
#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>

//
// Mock out all the calls to nyx-lib
//
#undef nyx_info
#define nyx_info(m, args...) {}
#undef nyx_debug
#define nyx_debug(m, args...) {}
#undef nyx_error
#define nyx_error(m, args...) {}
#undef nyx_warn
#define nyx_warn(m, args...) {}

// NOTE: define this nyx_debug to send TEST messages (from THIS file, e.g. refcounts) to stderr:
//#define nyx_debug(m, ...) {fprintf(stderr,"\n\t"); fprintf(stderr, m, ##__VA_ARGS__);}
// NOTE: define this nyx_error to send TARGET nyx_error messages (from the tested source) to stderr:
//#define nyx_error(m, ...) {fprintf(stderr,"\n\t"); fprintf(stderr, m, ##__VA_ARGS__);}

// mock out externals defined in batterylib.c

nyx_device_t *nyxDev = NULL;

//*****************************************************************************
//*****************************************************************************

// Define battery callback context used by _handle_event()
void *battery_callback_context = (void *)12345;

//typedef void (*nyx_device_callback_function_t)(nyx_device_handle_t, nyx_callback_status_t, void *);
void test_battery_callback(nyx_device_handle_t device,
                           nyx_callback_status_t status, void *context);
void test_battery_callback(nyx_device_handle_t device,
                           nyx_callback_status_t status, void *context)
{
	g_assert_true(nyxDev == device);
	g_assert_true(NYX_CALLBACK_STATUS_DONE == status);
	g_assert_true(battery_callback_context == context);
	return;
}

// Define battery callback called by _handle_event()
nyx_device_callback_function_t battery_callback = &test_battery_callback;


//*****************************************************************************
//*****************************************************************************

//
// Point the module's config lookup at a file that does not exist, so
// nyx_conf_get_path() returns NULL and detection follows the code path the
// tests mock, rather than whatever /etc/nyx.conf says on the build host.
//
// A test that needs the config to say something points it at a fixture with
// test_conf_set() and puts it back afterwards.
//
#define TEST_NO_CONF "/nonexistent/test_dev_battery/nyx.conf"
static const char *test_nyx_conf_file = TEST_NO_CONF;
#define NYX_CONF_FILE test_nyx_conf_file

// Pull in the unit under test
#include "../battery.c"

//*****************************************************************************
//*****************************************************************************

//
// Node names the mocks answer for. The module builds its attribute paths by
// appending to the sysfs path, so "Battery" here yields "Battery/capacity" and
// friends below.
//
#define TEST_BATT_NODE "Battery"
#define TEST_KBD_NODE "Keyboard"
// A fuel gauge sitting beside the battery as its own power_supply, which is
// where a Qualcomm board keeps charge_full_design.
#define TEST_BMS_NODE "BMS"

// mock out calls to nyx-modules: utils.c
char *test_find_power_supply_sysfs_path_retval = TEST_BATT_NODE;
// NULL means "this board has no BMS supply", which is the common case.
char *test_find_power_supply_bms_retval = NULL;

char *find_power_supply_sysfs_path_by_name(const char *name)
{
	// The gauge is looked up by node name now; the fixture calls its node "BMS".
	if (name && 0 == g_strcmp0(name, "bms"))
	{
		return test_find_power_supply_bms_retval
		       ? g_strdup(test_find_power_supply_bms_retval) : NULL;
	}

	return NULL;
}

char *find_power_supply_sysfs_path(const char *device_type)
{
	// Asked for by type, and the module asks for two different ones, so the
	// mock has to tell them apart rather than answering "Battery" to
	// everything.
	if (device_type && 0 == g_strcmp0(device_type, "BMS"))
	{
		return test_find_power_supply_bms_retval
		       ? g_strdup(test_find_power_supply_bms_retval) : NULL;
	}

	if (!test_find_power_supply_sysfs_path_retval)
	{
		return NULL;
	}

	// The caller frees this, so it has to be allocated - handing back the
	// argument (or a literal) means g_free() on memory glib never owned.
	return g_strdup(test_find_power_supply_sysfs_path_retval);
}

// NULL means "no extra batteries"; otherwise a NULL-terminated list of nodes.
char **test_find_power_supply_sysfs_paths_retval = NULL;

char **find_power_supply_sysfs_paths(const char *device_type)
{
	if (!test_find_power_supply_sysfs_paths_retval)
	{
		return NULL;
	}

	return g_strdupv(test_find_power_supply_sysfs_paths_retval);
}

// Only the battery's "health" attribute is mocked with contents; every other
// path still reads as absent, as it did before there was anything to return.
char test_FileGetString_health[64] = "";
int32_t test_FileGetString_health_retval = -1;
char test_FileGetString_status[64] = "";
int32_t test_FileGetString_status_retval = -1;
char test_FileGetString_level[64] = "";
int32_t test_FileGetString_level_retval = -1;

int FileGetString(const char *path, char *ret_string, size_t maxlen)
{
	if (ret_string && maxlen > 0)
	{
		ret_string[0] = '\0';
	}

	if (0 == g_strcmp0(path, "Battery/capacity_level"))
	{
		if (test_FileGetString_level_retval < 0)
		{
			return -1;
		}

		if (ret_string && maxlen > 0)
		{
			g_strlcpy(ret_string, test_FileGetString_level, maxlen);
		}

		return 0;
	}

	if (0 == g_strcmp0(path, "Battery/status"))
	{
		if (test_FileGetString_status_retval < 0)
		{
			return -1;
		}

		if (ret_string && maxlen > 0)
		{
			g_strlcpy(ret_string, test_FileGetString_status, maxlen);
		}

		return 0;
	}

	if (0 != g_strcmp0(path, "Battery/health") ||
	        test_FileGetString_health_retval < 0)
	{
		return -1;
	}

	if (ret_string && maxlen > 0)
	{
		g_strlcpy(ret_string, test_FileGetString_health, maxlen);
	}

	return 0;
}

// define paths used in detect_battery_sysfs_paths() in battery.c
static char *test_batt_sysfs_path = TEST_BATT_NODE;
static char *test_batt_capacity_path = "Battery/capacity";
static char *test_batt_energy_now_path = "Battery/energy_now";
static char *test_batt_energy_full_path = "Battery/energy_full";
static char *test_batt_energy_full_design_path = "Battery/energy_full_design";
static char *test_batt_charge_now_path = "Battery/charge_now";
static char *test_batt_charge_counter_path = "Battery/charge_counter";
static char *test_batt_health_path = "Battery/health";
// The keyboard battery is only ever counted and named, but every battery
// gets its paths resolved, and resolving them asks whether the two
// full-capacity attributes are there.
static char *test_kbd_charge_full_path = "Keyboard/charge_full";
static char *test_kbd_charge_full_design_path = "Keyboard/charge_full_design";
static char *test_bms_charge_full_path = "BMS/charge_full";
static char *test_bms_charge_full_design_path = "BMS/charge_full_design";
static char *test_batt_charge_full_path = "Battery/charge_full";
static char *test_batt_charge_full_design_path = "Battery/charge_full_design";
static char *test_batt_temperature_path = "Battery/temp";
static char *test_batt_voltage_path = "Battery/voltage_now";
static char *test_batt_current_path = "Battery/current_now";
static char *test_batt_current_avg_path = "Battery/current_avg";
static char *test_batt_present_path = "Battery/present";
static char *test_batt_status_path = "Battery/status";
static char *test_batt_capacity_level_path = "Battery/capacity_level";
static char *test_batt_fake_battery_path = "Battery/pseudo_batt";

//
// battery_current() and battery_temperature() read through FileGetDouble() so
// that a negative reading - discharging, or a battery below freezing - is not
// confused with a failed read. Both are mocked here, by path, because they are
// the two values that are legitimately signed.
//
double test_FileGetDouble_retval = 0;
int test_FileGetDouble_result = -1;
double test_FileGetDouble_temp_retval = 0;
int test_FileGetDouble_temp_result = -1;
double test_FileGetDouble_avg_retval = 0;
int test_FileGetDouble_avg_result = -1;

int FileGetDouble(const char *path, double *ret_data)
{
	int result = test_FileGetDouble_result;
	double value = test_FileGetDouble_retval;

	g_assert_nonnull(path);

	if (0 == strncmp(path, test_batt_temperature_path, PATH_LEN))
	{
		result = test_FileGetDouble_temp_result;
		value = test_FileGetDouble_temp_retval;
	}

	if (0 == strncmp(path, test_batt_current_avg_path, PATH_LEN))
	{
		result = test_FileGetDouble_avg_result;
		value = test_FileGetDouble_avg_retval;
	}

	if (0 != result)
	{
		return -1;
	}

	if (ret_data)
	{
		*ret_data = value;
	}

	return 0;
}

// define (mocked) values returns for above paths
int32_t test_batt_capacity_path_retval = 0;
int32_t test_batt_energy_now_path_retval = 0;
int32_t test_batt_energy_full_path_retval = 0;
int32_t test_batt_energy_full_design_path_retval = 0;
int32_t test_batt_charge_now_path_retval = 0;
int32_t test_batt_charge_counter_path_retval = 0;
int32_t test_kbd_charge_full_path_retval = 0;
int32_t test_kbd_charge_full_design_path_retval = 0;
int32_t test_bms_charge_full_path_retval = 0;
int32_t test_bms_charge_full_design_path_retval = 0;
int32_t test_batt_charge_full_path_retval = 0;
int32_t test_batt_charge_full_design_path_retval = 0;
int32_t test_batt_temperature_path_retval = 0;
int32_t test_batt_voltage_path_retval = 0;
int32_t test_batt_current_path_retval = 0;
int32_t test_batt_present_path_retval = 0;
int32_t test_batt_fake_battery_path_retval = 0;

#define ifMatchReturnRetvalForTestPath(value) if (0 == strncmp(path, value, PATH_LEN)) { return value##_retval; }
// ifMatchReturnRetvalForTestPath(batt_capacity) expands to:
// if (0 == strncmp(path, test_batt_capacity_path, PATH_LEN))
// {
//  return test_batt_capacity_path_retval;
// }

// return appropriate _retval value for calls to nyx_utils_read_value
int32_t nyx_utils_read_value(char *path)
{
	//fprintf(stderr,"path (%s) passed to nyx_utils_read_value\n", path);
	ifMatchReturnRetvalForTestPath(test_batt_capacity_path)
		else ifMatchReturnRetvalForTestPath(test_batt_energy_now_path)
			else ifMatchReturnRetvalForTestPath(test_batt_energy_full_path)
				else ifMatchReturnRetvalForTestPath(test_batt_energy_full_design_path)
					else ifMatchReturnRetvalForTestPath(test_batt_charge_now_path)
						else ifMatchReturnRetvalForTestPath(test_batt_charge_counter_path)
							else ifMatchReturnRetvalForTestPath(test_batt_charge_full_path)
								else ifMatchReturnRetvalForTestPath(test_batt_charge_full_design_path)
									else ifMatchReturnRetvalForTestPath(test_batt_temperature_path)
										else ifMatchReturnRetvalForTestPath(test_batt_voltage_path)
											else ifMatchReturnRetvalForTestPath(test_batt_current_path)
												else ifMatchReturnRetvalForTestPath(test_batt_present_path)
													else ifMatchReturnRetvalForTestPath(test_batt_fake_battery_path)
														else ifMatchReturnRetvalForTestPath(test_kbd_charge_full_path)
															else ifMatchReturnRetvalForTestPath(test_kbd_charge_full_design_path)
																else ifMatchReturnRetvalForTestPath(test_bms_charge_full_path)
																	else ifMatchReturnRetvalForTestPath(test_bms_charge_full_design_path)
			else ifMatchReturnRetvalForTestPath(test_batt_energy_full_path)
				else ifMatchReturnRetvalForTestPath(test_batt_energy_full_design_path)
					else ifMatchReturnRetvalForTestPath(test_batt_charge_now_path)
						else ifMatchReturnRetvalForTestPath(test_batt_charge_counter_path)
							else ifMatchReturnRetvalForTestPath(test_batt_charge_full_path)
								else ifMatchReturnRetvalForTestPath(test_batt_charge_full_design_path)
									else ifMatchReturnRetvalForTestPath(test_batt_temperature_path)
										else ifMatchReturnRetvalForTestPath(test_batt_voltage_path)
											else ifMatchReturnRetvalForTestPath(test_batt_current_path)
												else ifMatchReturnRetvalForTestPath(test_batt_present_path)
													else ifMatchReturnRetvalForTestPath(test_batt_fake_battery_path)
														else ifMatchReturnRetvalForTestPath(test_bms_charge_full_path)
															else ifMatchReturnRetvalForTestPath(test_bms_charge_full_design_path)
			else ifMatchReturnRetvalForTestPath(test_batt_energy_full_path)
				else ifMatchReturnRetvalForTestPath(test_batt_energy_full_design_path)
					else ifMatchReturnRetvalForTestPath(test_batt_charge_now_path)
						else ifMatchReturnRetvalForTestPath(test_batt_charge_counter_path)
							else ifMatchReturnRetvalForTestPath(test_batt_charge_full_path)
								else ifMatchReturnRetvalForTestPath(test_batt_charge_full_design_path)
									else ifMatchReturnRetvalForTestPath(test_batt_temperature_path)
										else ifMatchReturnRetvalForTestPath(test_batt_voltage_path)
											else ifMatchReturnRetvalForTestPath(test_batt_current_path)
												else ifMatchReturnRetvalForTestPath(test_batt_present_path)
													else ifMatchReturnRetvalForTestPath(test_batt_fake_battery_path)
														else ifMatchReturnRetvalForTestPath(test_bms_charge_full_design_path)

												// bad path: print error, force g_assert, and return -1
												fprintf(stderr, "Bad path (%s) passed to nyx_utils_read_value\n", path);

	g_assert_true(path == (const char *)"Bad path passed to nyx_utils_read_value");
	return -1;
}

//
// Fake ("pseudo") battery mode, which is how an emulated target with no
// battery of its own is given one. nyx_utils_read() answers -1 when it cannot
// even open the node, which is the usual case off-target, and the module has
// to treat that as an error rather than as a successful read.
//
int32_t test_nyx_utils_read_result = -1;
char test_nyx_utils_read_contents[64] = "";
char test_nyx_utils_write_buf[64] = "";
size_t test_nyx_utils_write_size = 0;

int32_t nyx_utils_read(char *path, char *buf, size_t size)
{
	size_t len;

	g_assert_nonnull(buf);
	g_assert_true(size > 0);

	if (test_nyx_utils_read_result < 0)
	{
		// Deliberately leaves buf untouched, exactly as nyx-lib does.
		return -1;
	}

	len = g_strlcpy(buf, test_nyx_utils_read_contents, size);

	if (len >= size)
	{
		len = size - 1;
	}

	return (int32_t) len;
}

void nyx_utils_write(char *path, char *buf, size_t size)
{
	g_assert_nonnull(buf);
	g_assert_true(size < sizeof(test_nyx_utils_write_buf));

	// Record exactly what the module asked to be written: passing sizeof the
	// caller's buffer rather than the string length used to push uninitialised
	// stack bytes past the terminator into the kernel.
	memcpy(test_nyx_utils_write_buf, buf, size);
	test_nyx_utils_write_buf[size] = '\0';
	test_nyx_utils_write_size = size;
}

//*****************************************************************************
//*****************************************************************************

// udev is an "Opaque object representing the library context"
struct udev
{
	int opaque;
};

// udev_device is an "Opaque object representing one kernel sys device."
struct udev_device
{
	int opaque;
};

// udev_monitor is an "Opaque object handling an event source"
struct udev_monitor
{
	int opaque;
};


// Mock the udev calls

// udev_monitor_receive_device() is called by _handle_power_supply_event()
// which is a callback passed to g_io_add_watch() in battery_init()...
struct udev_device testUdevDevice;
struct udev_device *testUdevDevice_retval = &testUdevDevice;
struct udev_device *udev_monitor_receive_device(struct udev_monitor
        *udev_monitor)
{
	return testUdevDevice_retval;
}

int32_t testUdevRefcount = 0;
struct udev testUdevStruct;
struct udev *testUdevStruct_retval = &testUdevStruct;
struct udev *udev_new(void)
{
	// are we returning our valid "test" udev (as opposed to NULL)?
	if (&testUdevStruct == testUdevStruct_retval)
	{
		// yes, so bump our testGIOChannel refcount
		testUdevRefcount++;
	}

	nyx_debug("In udev_new: testUdevRefcount = %d", testUdevRefcount);
	return testUdevStruct_retval;
}

/* kernel and udev generated events over netlink */
int32_t testUdevMonitorRefcount = 0;
struct udev_monitor testUdevMonitorStruct;
struct udev_monitor *testUdevMonitorStruct_retval = &testUdevMonitorStruct;
struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev,
        const char *name)
{
	// are we returning our valid "test" monitor (as opposed to NULL)?
	if (&testUdevMonitorStruct == testUdevMonitorStruct_retval)
	{
		testUdevMonitorRefcount++;
	}

	nyx_debug("In udev_monitor_new_from_netlink: testUdevMonitorRefcount = %d",
	          testUdevMonitorRefcount);
	return testUdevMonitorStruct_retval;
}

/* in-kernel socket filters to select messages that get delivered to a listener */
int testUdevMonitorFilterAddMatchResult_retval = 0;
int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor
        *udev_monitor,
        const char *subsystem, const char *devtype)
{
	return testUdevMonitorFilterAddMatchResult_retval;
}

/* bind socket */
int testUdevMonitorEnableReceiving_retval = 0;
int udev_monitor_enable_receiving(struct udev_monitor *udev_monitor)
{
	return testUdevMonitorEnableReceiving_retval;
}

int testUdevMonitorGetFd_retval = 0;
int udev_monitor_get_fd(struct udev_monitor *udev_monitor)
{
	return testUdevMonitorGetFd_retval;
}

int udev_monitor_filter_remove(struct udev_monitor *udev_monitor)
{
	return 0;
}

//
// The monitor owns the descriptor the GIOChannel is wrapped around, so it has
// to be unreffed on the way out. Dropping the pointer instead leaked it, along
// with its reference on the udev context.
//
struct udev_monitor *udev_monitor_unref(struct udev_monitor *udev_monitor)
{
	if (&testUdevMonitorStruct == udev_monitor)
	{
		testUdevMonitorRefcount--;
	}

	nyx_debug("In udev_monitor_unref: testUdevMonitorRefcount = %d",
	          testUdevMonitorRefcount);
	return NULL;
}

// _handle_event() rebuilds the battery list on add/remove
const char *testUdevDeviceAction_retval = NULL;
const char *udev_device_get_action(struct udev_device *udev_device)
{
	return testUdevDeviceAction_retval;
}

int32_t testUdevDeviceRefcount = 0;
struct udev_device *udev_device_ref(struct udev_device *udev_device)
{
	testUdevDeviceRefcount++;
	return udev_device;
}

struct udev_device *udev_device_unref(struct udev_device *udev_device)
{
	if (udev_device)
	{
		testUdevDeviceRefcount--;
	}

	return NULL;
}

struct udev *udev_unref(struct udev *udev)
{
	// is this a request for our valid "test" udev?
	if (&testUdevStruct == udev)
	{
		// yes, so decrement our testGIOChannel refcount
		testUdevRefcount--;
	}

	nyx_debug("In udev_unref: testUdevRefcount = %d", testUdevRefcount);
	return NULL;
}

//*****************************************************************************
//*****************************************************************************

// Mock the glib calls

// define (mocked) values returns for above paths
int32_t test_batt_capacity_path_exists = false;
int32_t test_batt_energy_now_path_exists = false;
int32_t test_batt_energy_full_path_exists = false;
int32_t test_batt_energy_full_design_path_exists = false;
int32_t test_batt_charge_now_path_exists = false;
int32_t test_batt_charge_counter_path_exists = false;
int32_t test_batt_health_path_exists = false;
int32_t test_kbd_charge_full_path_exists = false;
int32_t test_kbd_charge_full_design_path_exists = false;
int32_t test_bms_charge_full_path_exists = false;
int32_t test_bms_charge_full_design_path_exists = false;
int32_t test_batt_charge_full_path_exists = false;
int32_t test_batt_charge_full_design_path_exists = false;
int32_t test_batt_temperature_path_exists = false;
int32_t test_batt_voltage_path_exists = false;
int32_t test_batt_current_path_exists = false;
int32_t test_batt_current_avg_path_exists = false;
int32_t test_batt_present_path_exists = false;
int32_t test_batt_status_path_exists = false;
int32_t test_batt_capacity_level_path_exists = false;
int32_t test_batt_fake_battery_path_exists = false;

//
// Whether the power_supply directory itself is there. A configured battery
// whose node has gone - a keyboard unplugged from the phone - keeps its slot
// in the list and is reported as not present, so the module asks this before
// it reads any attribute.
//
int32_t test_batt_sysfs_path_is_dir = true;

#define ifMatchReturnExistsForTestPath(value) if (0 == strncmp(path, value, PATH_LEN)) { return value##_exists; }
// ifMatchReturnExistsForTestPath(batt_capacity) expands to:
// if (0 == strncmp(path, test_batt_capacity_path, PATH_LEN))
// {
//  return test_batt_capacity_path_exists;
// }

// return appropriate _exists value for calls to g_file_test
gboolean g_file_test(const gchar *path, GFileTest test)
{
	//fprintf(stderr,"path (%s) passed to g_file_test\n", path);
	if (G_FILE_TEST_IS_DIR == test)
	{
		// Only ever asked about a power_supply node itself, not an attribute.
		if (0 == strncmp(path, test_batt_sysfs_path, PATH_LEN))
		{
			return test_batt_sysfs_path_is_dir;
		}

		return false;
	}

	if (G_FILE_TEST_EXISTS != test)
	{
		fprintf(stderr, "Un-mocked test (%d) passed to g_file_test\n",
		        G_FILE_TEST_EXISTS);
		g_assert_true(path == (const char *)"Un-mocked test passed to g_file_test");
		return -1;
	}

	//
	// An attribute the supply does not have leaves its path empty, and
	// battery_full40() and battery_full_design() ask g_file_test() about
	// charge_full directly rather than through _read_optional_value(). "Not
	// there" is the truth for an empty path, and answering it here keeps the
	// strictness below for paths that are actually spelled out.
	//
	if (!path || !path[0])
	{
		return false;
	}

	ifMatchReturnExistsForTestPath(test_batt_capacity_path)
		else ifMatchReturnExistsForTestPath(test_batt_energy_now_path)
			else ifMatchReturnExistsForTestPath(test_batt_energy_full_path)
				else ifMatchReturnExistsForTestPath(test_batt_energy_full_design_path)
					else ifMatchReturnExistsForTestPath(test_batt_charge_now_path)
						else ifMatchReturnExistsForTestPath(test_batt_charge_counter_path)
							else ifMatchReturnExistsForTestPath(test_batt_charge_full_path)
								else ifMatchReturnExistsForTestPath(test_batt_charge_full_design_path)
									else ifMatchReturnExistsForTestPath(test_batt_temperature_path)
										else ifMatchReturnExistsForTestPath(test_batt_voltage_path)
											else ifMatchReturnExistsForTestPath(test_batt_current_path)
											else ifMatchReturnExistsForTestPath(test_batt_current_avg_path)
												else ifMatchReturnExistsForTestPath(test_batt_present_path)
												else ifMatchReturnExistsForTestPath(test_batt_status_path)
												else ifMatchReturnExistsForTestPath(test_batt_capacity_level_path)
													else ifMatchReturnExistsForTestPath(test_batt_fake_battery_path)
														else ifMatchReturnExistsForTestPath(test_batt_health_path)
															else ifMatchReturnExistsForTestPath(test_kbd_charge_full_path)
																else ifMatchReturnExistsForTestPath(test_kbd_charge_full_design_path)
																	else ifMatchReturnExistsForTestPath(test_bms_charge_full_path)
																		else ifMatchReturnExistsForTestPath(test_bms_charge_full_design_path)
			else ifMatchReturnExistsForTestPath(test_batt_energy_full_path)
				else ifMatchReturnExistsForTestPath(test_batt_energy_full_design_path)
					else ifMatchReturnExistsForTestPath(test_batt_charge_now_path)
						else ifMatchReturnExistsForTestPath(test_batt_charge_counter_path)
							else ifMatchReturnExistsForTestPath(test_batt_charge_full_path)
								else ifMatchReturnExistsForTestPath(test_batt_charge_full_design_path)
									else ifMatchReturnExistsForTestPath(test_batt_temperature_path)
										else ifMatchReturnExistsForTestPath(test_batt_voltage_path)
											else ifMatchReturnExistsForTestPath(test_batt_current_path)
											else ifMatchReturnExistsForTestPath(test_batt_current_avg_path)
												else ifMatchReturnExistsForTestPath(test_batt_present_path)
												else ifMatchReturnExistsForTestPath(test_batt_status_path)
												else ifMatchReturnExistsForTestPath(test_batt_capacity_level_path)
													else ifMatchReturnExistsForTestPath(test_batt_fake_battery_path)
														else ifMatchReturnExistsForTestPath(test_batt_health_path)
															else ifMatchReturnExistsForTestPath(test_bms_charge_full_path)
																else ifMatchReturnExistsForTestPath(test_bms_charge_full_design_path)
			else ifMatchReturnExistsForTestPath(test_batt_energy_full_path)
				else ifMatchReturnExistsForTestPath(test_batt_energy_full_design_path)
					else ifMatchReturnExistsForTestPath(test_batt_charge_now_path)
						else ifMatchReturnExistsForTestPath(test_batt_charge_counter_path)
							else ifMatchReturnExistsForTestPath(test_batt_charge_full_path)
								else ifMatchReturnExistsForTestPath(test_batt_charge_full_design_path)
									else ifMatchReturnExistsForTestPath(test_batt_temperature_path)
										else ifMatchReturnExistsForTestPath(test_batt_voltage_path)
											else ifMatchReturnExistsForTestPath(test_batt_current_path)
											else ifMatchReturnExistsForTestPath(test_batt_current_avg_path)
												else ifMatchReturnExistsForTestPath(test_batt_present_path)
												else ifMatchReturnExistsForTestPath(test_batt_status_path)
												else ifMatchReturnExistsForTestPath(test_batt_capacity_level_path)
													else ifMatchReturnExistsForTestPath(test_batt_fake_battery_path)
														else ifMatchReturnExistsForTestPath(test_batt_health_path)
															else ifMatchReturnExistsForTestPath(test_bms_charge_full_design_path)

												//
												// Resolving a battery's paths asks whether these two
												// attributes are on its node, and it does that for
												// every battery that turns up - the keyboard, the
												// hard-coded fallback node - not only the one a given
												// test set paths up for. Answering "not there" for an
												// unmocked node is the truth and keeps the strictness
												// below for everything else; the explicit entries
												// above still win where a test declared one.
												//
												if (g_str_has_suffix(path, "/charge_full") ||
												        g_str_has_suffix(path, "/charge_full_design"))
												{
													return false;
												}

												//
												// The keyboard's battery is probed for every attribute
												// the module resolves, and only the two capacities are
												// mocked for it. So is the node the module falls back to
												// when nothing is configured and nothing is found, which
												// no test mocks anything for. What no test gave them does
												// not exist - but only for the attributes the module is
												// known to probe, so a path it should not be asking
												// about is still caught below.
												//
												if (g_str_has_prefix(path, TEST_KBD_NODE "/") ||
												        g_str_has_prefix(path, "/sys/class/power_supply/battery/"))
												{
													static const char *probed[] =
													{
														"capacity", "energy_now", "energy_full",
														"energy_full_design", "charge_now",
														"charge_counter", "temp", "voltage_now",
														"current_now", "current_avg", "present",
														"status", "capacity_level", "pseudo_batt",
														"health", NULL
													};

													for (int k = 0; probed[k]; k++)
													{
														gchar *suffix = g_strdup_printf("/%s", probed[k]);
														bool match = g_str_has_suffix(path, suffix);

														g_free(suffix);

														if (match)
														{
															return false;
														}
													}
												}

												// bad path: print error, force g_assert, and return -1
												fprintf(stderr, "Bad path (%s) passed to g_file_test\n", path);

	g_assert_true(path == (const char *)"Bad path passed to g_file_test");
	return -1;
}



// code calls: channel = g_io_channel_unix_new(fd);
int32_t testGIOChannelRefcount = 0;
GIOChannel testGIOChannel;
GIOChannel *testGIOChannel_retval = &testGIOChannel;
GIOChannel *g_io_channel_unix_new(int fd)
{
	// are we returning our valid "test" channel (as opposed to NULL)?
	if (&testGIOChannel == testGIOChannel_retval)
	{
		// yes, so bump our testGIOChannel refcount
		testGIOChannelRefcount++;
	}

	nyx_debug("In g_io_channel_unix_new: testGIOChannelRefcount = %d",
	          testGIOChannelRefcount);
	return testGIOChannel_retval;
}

// code calls: g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_NVAL, _handle_power_supply_event, NULL);
static const guint testEventSourceIdGood = 1;
static const guint testEventSourceIdBad = 0;
guint     testEventSourceId_retVal = 0;
guint     g_io_add_watch(GIOChannel      *channel,
                         GIOCondition     condition,
                         GIOFunc          func,
                         gpointer         user_data)
{
	// is this a request for our valid "test" channel (with good retVal)?
	if ((&testGIOChannel == channel) &&
	        (testEventSourceIdGood == testEventSourceId_retVal))
	{
		// yes, so bump our testGIOChannel refcount
		testGIOChannelRefcount++;
	}

	nyx_debug("In g_io_add_watch: testGIOChannelRefcount = %d",
	          testGIOChannelRefcount);
	// return value is "the event source id" which is not used by battery.c
	return testEventSourceId_retVal;
}

// code calls: g_io_channel_set_close_on_unref(channel, TRUE);
void                g_io_channel_set_close_on_unref(GIOChannel *channel,
        gboolean do_close)
{
	return;
}

// code calls: g_io_channel_unref(channel);
void                g_io_channel_unref(GIOChannel *channel)
{
	// is this a request for our valid "test" channel?
	if (&testGIOChannel == channel)
	{
		// yes, so decrement our testGIOChannel refcount
		testGIOChannelRefcount--;
	}

	nyx_debug("In g_io_channel_unref: testGIOChannelRefcount = %d",
	          testGIOChannelRefcount);
	return;
}

// code calls: g_source_remove(watch);
gboolean g_source_remove(guint tag)
{
	// is this a request for our valid "test" watch?
	if (testEventSourceIdGood == tag)
	{
		// yes, so decrement our testGIOChannel refcount
		testGIOChannelRefcount--;
	}

	nyx_debug("In g_source_remove: testGIOChannelRefcount = %d",
	          testGIOChannelRefcount);
	return TRUE;
}

//*****************************************************************************
//*****************************************************************************

//
// Tests for the battery_init API method
// nyx_error_t battery_init(void)
//
static void
test_battery_init(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// NOTE: We always call battery_deinit() after calling battery_init() to prevent leaks

	// Initial setup of good return values
	testUdevStruct_retval = &testUdevStruct;
	testUdevMonitorStruct_retval = &testUdevMonitorStruct;
	testUdevMonitorFilterAddMatchResult_retval = 0;
	testUdevMonitorEnableReceiving_retval = 0;
	testUdevMonitorGetFd_retval = 0;
	testGIOChannel_retval = &testGIOChannel;
	testEventSourceId_retVal = testEventSourceIdGood;

	// setup BAD return value for udev_new and force expected event
	nyx_debug("\nIn test_battery_init: setup BAD return value for udev_new");
	testUdevStruct_retval = NULL;
	g_assert_false(NYX_ERROR_NONE == battery_init());
	g_assert_true(NYX_ERROR_NONE == battery_deinit());
	// make sure we didn't leak any GIOChannel or udev references
	g_assert_true(0 == testGIOChannelRefcount);
	g_assert_true(0 == testUdevRefcount);
	g_assert_true(0 == testUdevMonitorRefcount);

	// setup GOOD return value for udev_new
	testUdevStruct_retval = &testUdevStruct;


	// setup BAD return value for udev_monitor_new_from_netlink and force expected event
	nyx_debug("\nIn test_battery_init: setup BAD return value for udev_monitor_new_from_netlink");
	testUdevMonitorStruct_retval = NULL;
	g_assert_false(NYX_ERROR_NONE == battery_init());
	g_assert_true(NYX_ERROR_NONE == battery_deinit());
	// make sure we didn't leak any GIOChannel or udev references
	g_assert_true(0 == testGIOChannelRefcount);
	g_assert_true(0 == testUdevRefcount);
	g_assert_true(0 == testUdevMonitorRefcount);

	// setup GOOD return value for udev_monitor_new_from_netlink
	testUdevMonitorStruct_retval = &testUdevMonitorStruct;


	// setup BAD return value for udev_monitor_filter_add_match_subsystem_devtype and force expected event
	nyx_debug("\nIn test_battery_init: setup BAD return value for udev_monitor_filter_add_match_subsystem_devtype");
	testUdevMonitorFilterAddMatchResult_retval = -1;
	g_assert_false(NYX_ERROR_NONE == battery_init());
	g_assert_true(NYX_ERROR_NONE == battery_deinit());
	// make sure we didn't leak any GIOChannel or udev references
	g_assert_true(0 == testGIOChannelRefcount);
	g_assert_true(0 == testUdevRefcount);
	g_assert_true(0 == testUdevMonitorRefcount);

	// setup GOOD return value for udev_monitor_filter_add_match_subsystem_devtype
	testUdevMonitorFilterAddMatchResult_retval = 0;


	// setup BAD return value for udev_monitor_enable_receiving and force expected event
	nyx_debug("\nIn test_battery_init: setup BAD return value for udev_monitor_enable_receiving");
	testUdevMonitorEnableReceiving_retval = -1;
	g_assert_false(NYX_ERROR_NONE == battery_init());
	g_assert_true(NYX_ERROR_NONE == battery_deinit());
	// make sure we didn't leak any GIOChannel or udev references
	g_assert_true(0 == testGIOChannelRefcount);
	g_assert_true(0 == testUdevRefcount);
	g_assert_true(0 == testUdevMonitorRefcount);

	// setup GOOD return value for udev_monitor_enable_receiving
	testUdevMonitorEnableReceiving_retval = 0;


	// setup BAD return value for udev_monitor_get_fd and force expected event
	nyx_debug("\nIn test_battery_init: setup BAD return value for udev_monitor_get_fd");
	testUdevMonitorGetFd_retval = -1;
	g_assert_false(NYX_ERROR_NONE == battery_init());
	g_assert_true(NYX_ERROR_NONE == battery_deinit());
	// make sure we didn't leak any GIOChannel or udev references
	g_assert_true(0 == testGIOChannelRefcount);
	g_assert_true(0 == testUdevRefcount);
	g_assert_true(0 == testUdevMonitorRefcount);

	// setup GOOD return value for udev_monitor_get_fd
	testUdevMonitorGetFd_retval = 0;


	// setup BAD return value for g_io_channel_unix_new and force expected event
	nyx_debug("\nIn test_battery_init: setup BAD return value for g_io_channel_unix_new");
	testGIOChannel_retval = NULL;
	g_assert_false(NYX_ERROR_NONE == battery_init());
	g_assert_true(NYX_ERROR_NONE == battery_deinit());
	// make sure we didn't leak any GIOChannel or udev references
	g_assert_true(0 == testGIOChannelRefcount);
	g_assert_true(0 == testUdevRefcount);
	g_assert_true(0 == testUdevMonitorRefcount);

	// setup GOOD return value for g_io_channel_unix_new
	testGIOChannel_retval = &testGIOChannel;


	// setup BAD return value for g_io_add_watch and force expected event
	nyx_debug("\nIn test_battery_init: setup BAD return value for g_io_add_watch");
	testEventSourceId_retVal = testEventSourceIdBad;
	g_assert_false(NYX_ERROR_NONE == battery_init());
	g_assert_true(NYX_ERROR_NONE == battery_deinit());
	// make sure we didn't leak any GIOChannel or udev references
	g_assert_true(0 == testGIOChannelRefcount);
	g_assert_true(0 == testUdevRefcount);
	g_assert_true(0 == testUdevMonitorRefcount);

	// setup GOOD return value for g_io_add_watch and force expected event
	nyx_debug("\nIn test_battery_init: setup GOOD return value for g_io_add_watch");
	testEventSourceId_retVal = testEventSourceIdGood;
	g_assert_true(NYX_ERROR_NONE == battery_init());
	g_assert_true(NYX_ERROR_NONE == battery_deinit());
	// make sure we didn't leak any GIOChannel or udev references
	g_assert_true(0 == testGIOChannelRefcount);
	g_assert_true(0 == testUdevRefcount);
	g_assert_true(0 == testUdevMonitorRefcount);
	nyx_debug("\n");
}

void reset_battery_path_retvals(void)
{
	test_batt_capacity_path_exists = false;
	test_batt_energy_now_path_exists = false;
	test_batt_energy_full_path_exists = false;
	test_batt_energy_full_design_path_exists = false;
	test_batt_charge_now_path_exists = false;
	test_batt_charge_counter_path_exists = false;
	test_batt_health_path_exists = false;
	test_bms_charge_full_path_exists = false;
	test_bms_charge_full_design_path_exists = false;
	test_batt_charge_full_path_exists = false;
	test_batt_charge_full_design_path_exists = false;
	test_batt_temperature_path_exists = false;
	test_batt_voltage_path_exists = false;
	test_batt_current_path_exists = false;
	test_batt_current_avg_path_exists = false;
	test_batt_present_path_exists = false;
	test_batt_status_path_exists = false;
	test_batt_capacity_level_path_exists = false;
	test_batt_fake_battery_path_exists = false;

	test_batt_capacity_path_retval = -1;
	test_batt_energy_now_path_retval = -1;
	test_batt_energy_full_path_retval = -1;
	test_batt_energy_full_design_path_retval = -1;
	test_batt_charge_now_path_retval = -1;
	test_batt_charge_counter_path_retval = -1;
	test_bms_charge_full_path_retval = -1;
	test_bms_charge_full_design_path_retval = -1;
	test_batt_charge_full_path_retval = -1;
	test_batt_charge_full_design_path_retval = -1;
	test_batt_temperature_path_retval = -1;
	test_batt_voltage_path_retval = -1;
	test_batt_current_path_retval = -1;
	test_batt_present_path_retval = -1;
	test_batt_fake_battery_path_retval = -1;

	test_batt_sysfs_path_is_dir = true;
	test_FileGetDouble_result = -1;
	test_FileGetDouble_retval = 0;
	test_FileGetDouble_temp_result = -1;
	test_FileGetDouble_temp_retval = 0;
	test_nyx_utils_read_result = -1;
	test_nyx_utils_read_contents[0] = '\0';
	test_nyx_utils_write_buf[0] = '\0';
	test_nyx_utils_write_size = 0;

	test_find_power_supply_sysfs_path_retval = TEST_BATT_NODE;
	test_find_power_supply_sysfs_paths_retval = NULL;
	test_find_power_supply_bms_retval = NULL;
	test_FileGetString_health[0] = '\0';
	test_FileGetString_health_retval = -1;
	test_FileGetString_status[0] = '\0';
	test_FileGetString_status_retval = -1;
	test_FileGetString_level[0] = '\0';
	test_FileGetString_level_retval = -1;
	test_FileGetDouble_avg_retval = 0;
	test_FileGetDouble_avg_result = -1;

	//
	// Readings are taken per battery, so there has to be a battery in the
	// list before any of them mean anything. Rebuild it from the mocks above
	// rather than leaning on whatever a previous test left behind.
	//
	detect_battery_sysfs_paths();
	g_assert_true(1 == battery_count());
}

//
// Release the battery list. Every test that has built one should end with
// this, so a leak check sees a clean exit.
//
void forget_batteries(void)
{
	battery_forget_all();
	g_assert_true(0 == battery_count());
}

//
// The module resolves a battery's attribute paths when the battery is
// detected, and an attribute the supply does not export gets no path at all, so
// that nothing reads it and nothing logs its absence. Every case below states
// what the supply exports by setting the "exists" mocks and then calls the API,
// which is a supply that was detected after those mocks were set - what the
// hardware looks like at probe time. reset_battery_path_retvals() detects
// before any case has said anything, so without this each case would be
// reading from a list built for a supply that exports nothing.
//
// So the accessors that read an attribute detect again first, under the mocks
// as they stand. (battery_xxx) in parentheses is the function itself, not the
// macro.
//
static void test_redetect(void)
{
	battery_forget_all();
	detect_battery_sysfs_paths();
}

#define battery_percent(i)        (test_redetect(), (battery_percent)(i))
#define battery_temperature(i)    (test_redetect(), (battery_temperature)(i))
#define battery_voltage(i)        (test_redetect(), (battery_voltage)(i))
#define battery_current(i)        (test_redetect(), (battery_current)(i))
#define battery_avg_current(i)    (test_redetect(), (battery_avg_current)(i))
#define battery_charging_state(i) (test_redetect(), (battery_charging_state)(i))
#define battery_full40(i)         (test_redetect(), (battery_full40)(i))
#define battery_full_design(i)    (test_redetect(), (battery_full_design)(i))
#define battery_health(i)         (test_redetect(), (battery_health)(i))
#define battery_coulomb(i)        (test_redetect(), (battery_coulomb)(i))
#define battery_is_present(i)     (test_redetect(), (battery_is_present)(i))

//
// Tests for the battery_percent API method
// int battery_percent(int index)
//
static void
test_battery_percent(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// Check for failure returned from test_batt_capacity_path if NO paths available
	reset_battery_path_retvals();
	g_assert_true(-1 == battery_percent(BATTERY_PRIMARY));

	// Check for failure returned from test_batt_capacity_path if (only) invalid capacity
	reset_battery_path_retvals();
	test_batt_capacity_path_exists = true;
	test_batt_capacity_path_retval = -1;
	g_assert_true(-1 == battery_percent(BATTERY_PRIMARY));

	// Check for correct return value from test_batt_capacity_path using valid capacity
	reset_battery_path_retvals();
	test_batt_capacity_path_exists = true;
	test_batt_capacity_path_retval = 80;
	g_assert_true(80 == battery_percent(BATTERY_PRIMARY));


	// Check for failure returned from test_batt_capacity_path using invalid energy_now
	reset_battery_path_retvals();
	test_batt_energy_now_path_exists = true;
	test_batt_energy_full_path_exists = true;
	test_batt_energy_now_path_retval = -1;
	test_batt_energy_full_path_retval = 1000000;
	g_assert_true(-1 == battery_percent(BATTERY_PRIMARY));

	// Check for failure returned from test_batt_capacity_path using invalid energy_full
	reset_battery_path_retvals();
	test_batt_energy_now_path_exists = true;
	test_batt_energy_full_path_exists = true;
	test_batt_energy_now_path_retval = 800000;
	test_batt_energy_full_path_retval = -1;
	g_assert_true(-1 == battery_percent(BATTERY_PRIMARY));

	// Check for correct return value from test_batt_capacity_path using energy_now / energy_full
	reset_battery_path_retvals();
	test_batt_energy_now_path_exists = true;
	test_batt_energy_full_path_exists = true;
	test_batt_energy_now_path_retval = 800000;
	test_batt_energy_full_path_retval = 1000000;
	g_assert_true(80 == battery_percent(BATTERY_PRIMARY));


	// Check for failure returned from test_batt_capacity_path using invalid charge_now
	reset_battery_path_retvals();
	test_batt_charge_now_path_exists = true;
	test_batt_charge_full_path_exists = true;
	test_batt_charge_now_path_retval = -1;
	test_batt_charge_full_path_retval = 2300000;
	g_assert_true(-1 == battery_percent(BATTERY_PRIMARY));

	// Check for failure returned from test_batt_capacity_path using invalid charge_full
	reset_battery_path_retvals();
	test_batt_charge_now_path_exists = true;
	test_batt_charge_full_path_exists = true;
	test_batt_charge_now_path_retval = 1840000;
	test_batt_charge_full_path_retval = -1;
	g_assert_true(-1 == battery_percent(BATTERY_PRIMARY));

	// Check for correct return value from test_batt_capacity_path using charge_now / charge_full
	reset_battery_path_retvals();
	test_batt_charge_now_path_exists = true;
	test_batt_charge_full_path_exists = true;
	test_batt_charge_now_path_retval = 1840000;
	test_batt_charge_full_path_retval = 2300000;
	g_assert_true(80 == battery_percent(BATTERY_PRIMARY));

	// Out-of-range battery
	g_assert_true(-1 == battery_percent(battery_count()));

	forget_batteries();
}

//
// Tests for the battery_temperature API method
// int battery_temperature(int index)
//
static void
test_battery_temperature(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// Out-of-range battery
	reset_battery_path_retvals();
	g_assert_true(-1 == battery_temperature(battery_count()));

	// Check for failure returned from test_batt_temperature_path
	reset_battery_path_retvals();
	test_batt_temperature_path_exists = true;
	test_FileGetDouble_temp_result = -1;
	g_assert_true(-1 == battery_temperature(BATTERY_PRIMARY));

	//
	// temp is in tenths of a degree Celsius, and battery_temperature()
	// answers in whole degrees - which is what "temperature_C" and the
	// CTIA limits in battery.c have always meant by it.
	//
	reset_battery_path_retvals();
	test_batt_temperature_path_exists = true;
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = 333;
	g_assert_true(33 == battery_temperature(BATTERY_PRIMARY));

	// Rounded, not truncated
	reset_battery_path_retvals();
	test_batt_temperature_path_exists = true;
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = 296;
	g_assert_true(30 == battery_temperature(BATTERY_PRIMARY));

	// A battery at the CTIA shutdown limit, and one just under it
	reset_battery_path_retvals();
	test_batt_temperature_path_exists = true;
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = 600;
	g_assert_true(60 == battery_temperature(BATTERY_PRIMARY));

	reset_battery_path_retvals();
	test_batt_temperature_path_exists = true;
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = 594;
	g_assert_true(59 == battery_temperature(BATTERY_PRIMARY));

	//
	// A battery below freezing - a phone left in a car overnight. Reading
	// temp through nyx_utils_read_value() reported every negative as a
	// failed read, so the charging logic could not see the one condition
	// the CTIA minimum charge temperature exists to catch.
	//
	reset_battery_path_retvals();
	test_batt_temperature_path_exists = true;
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = -50;
	g_assert_true(-5 == battery_temperature(BATTERY_PRIMARY));

	reset_battery_path_retvals();
	test_batt_temperature_path_exists = true;
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = -200;
	g_assert_true(-20 == battery_temperature(BATTERY_PRIMARY));

	// Rounding must not make a freezing battery look warmer than it is
	reset_battery_path_retvals();
	test_batt_temperature_path_exists = true;
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = -55;
	g_assert_true(-6 == battery_temperature(BATTERY_PRIMARY));

	// Zero is a real reading, not an absent one
	reset_battery_path_retvals();
	test_batt_temperature_path_exists = true;
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = 0;
	g_assert_true(0 == battery_temperature(BATTERY_PRIMARY));

	forget_batteries();
}

//
// Tests for the battery_voltage API method
// int battery_voltage(int index)
//
static void
test_battery_voltage(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// Out-of-range battery
	reset_battery_path_retvals();
	g_assert_true(-1 == battery_voltage(battery_count()));

	// Check for failure returned from test_batt_voltage_path
	reset_battery_path_retvals();
	test_batt_voltage_path_exists = true;
	test_batt_voltage_path_retval = -1;
	g_assert_true(-1 == battery_voltage(BATTERY_PRIMARY));

	//
	// voltage_now is in microvolts and battery_voltage() answers in
	// millivolts, which is what batteryd publishes it as. The question the
	// TODO here used to ask - mV or uV, because "the emulator returns mV" -
	// is settled by the kernel's power_supply ABI: microvolts. A driver
	// that reports millivolts is out of spec.
	//
	reset_battery_path_retvals();
	test_batt_voltage_path_exists = true;
	test_batt_voltage_path_retval = 3995000;
	g_assert_true(3995 == battery_voltage(BATTERY_PRIMARY));

	// A cell at the low end of its range
	reset_battery_path_retvals();
	test_batt_voltage_path_exists = true;
	test_batt_voltage_path_retval = 3400000;
	g_assert_true(3400 == battery_voltage(BATTERY_PRIMARY));

	forget_batteries();
}

//
// Tests for the battery_current API method
// int battery_current(int index)
//
static void
test_battery_current(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// Out-of-range battery
	reset_battery_path_retvals();
	g_assert_true(-1 == battery_current(battery_count()));

	// Check for failure returned when the node cannot be read
	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_FileGetDouble_result = -1;
	g_assert_true(-1 == battery_current(BATTERY_PRIMARY));

	// Check for correct return value while charging
	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = 371870;
	//
	// current_now is in microamps and battery_current() answers in
	// milliamps, matching batteryd's "current_mA". Same ABI question as
	// the voltage above, same answer.
	//
	g_assert_true(371 == battery_current(BATTERY_PRIMARY));

	//
	// current_now is signed: negative means discharging. Reading it through
	// nyx_utils_read_value() collapsed that into "read failed" and reported
	// -1 the whole time the device was on battery.
	//
	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = -1543000;
	g_assert_true(-1543 == battery_current(BATTERY_PRIMARY));

	// Truncation toward zero on both sides: a current under a milliamp is
	// reported as none rather than rounding away from zero.
	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = -600;
	g_assert_true(0 == battery_current(BATTERY_PRIMARY));

	//
	// Which sign means charging is not fixed by the ABI, so the reading is
	// normalised against status: positive is into the pack, whichever way the
	// driver happened to report it. A Qualcomm gauge charging at 410 mA reads
	// current_now negative with status "Charging".
	//
	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_batt_status_path_exists = true;
	test_FileGetString_status_retval = 0;
	g_strlcpy(test_FileGetString_status, "Charging",
	          sizeof(test_FileGetString_status));
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = -410156;
	g_assert_true(410 == battery_current(BATTERY_PRIMARY));

	// A mainline gauge reports the same state as positive: unchanged.
	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_batt_status_path_exists = true;
	test_FileGetString_status_retval = 0;
	g_strlcpy(test_FileGetString_status, "Charging",
	          sizeof(test_FileGetString_status));
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = 410156;
	g_assert_true(410 == battery_current(BATTERY_PRIMARY));

	// "Discharging" is the mirror of the above: out of the pack is negative.
	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_batt_status_path_exists = true;
	test_FileGetString_status_retval = 0;
	g_strlcpy(test_FileGetString_status, "Discharging",
	          sizeof(test_FileGetString_status));
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = 352000;
	g_assert_true(-352 == battery_current(BATTERY_PRIMARY));

	//
	// "Not charging" and "Full" say nothing about which way a current flows,
	// so the reading is passed through rather than given an invented
	// direction. Same for a driver that exports no status at all, which the
	// cases above this block already cover.
	//
	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_batt_status_path_exists = true;
	test_FileGetString_status_retval = 0;
	g_strlcpy(test_FileGetString_status, "Not charging",
	          sizeof(test_FileGetString_status));
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = 2929;
	g_assert_true(2 == battery_current(BATTERY_PRIMARY));

	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_batt_status_path_exists = true;
	test_FileGetString_status_retval = 0;
	g_strlcpy(test_FileGetString_status, "Full",
	          sizeof(test_FileGetString_status));
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = -5000;
	g_assert_true(-5 == battery_current(BATTERY_PRIMARY));

	//
	// capacity_level "Full" has the last word over a status that still says
	// "Charging", so the reading keeps its sign here too - the MindPhone
	// reports exactly this pair.
	//
	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_batt_status_path_exists = true;
	test_FileGetString_status_retval = 0;
	g_strlcpy(test_FileGetString_status, "Charging",
	          sizeof(test_FileGetString_status));
	test_batt_capacity_level_path_exists = true;
	test_FileGetString_level_retval = 0;
	g_strlcpy(test_FileGetString_level, "Full",
	          sizeof(test_FileGetString_level));
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = -5000;
	g_assert_true(-5 == battery_current(BATTERY_PRIMARY));

	// any other capacity_level leaves status in charge of the direction
	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_batt_status_path_exists = true;
	test_FileGetString_status_retval = 0;
	g_strlcpy(test_FileGetString_status, "Charging",
	          sizeof(test_FileGetString_status));
	test_batt_capacity_level_path_exists = true;
	test_FileGetString_level_retval = 0;
	g_strlcpy(test_FileGetString_level, "Normal",
	          sizeof(test_FileGetString_level));
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = -410156;
	g_assert_true(410 == battery_current(BATTERY_PRIMARY));

	// A parse failure must not be reported as a reading. FileGetDouble()
	// used to return success without storing anything, leaving the caller
	// to return whatever was on the stack.
	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_FileGetDouble_result = -1;
	test_FileGetDouble_retval = 12345;
	g_assert_true(-1 == battery_current(BATTERY_PRIMARY));

	forget_batteries();
}

//
// Tests for the battery_avg_current API method
// int battery_avg_current(int index)
//
static void
test_battery_avg_current(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// There is no separate "average" node, so this tracks battery_current()
	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_FileGetDouble_result = -1;
	g_assert_true(-1 == battery_avg_current(BATTERY_PRIMARY));

	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = 371870;
	// Milliamps, like battery_current() it delegates to
	g_assert_true(371 == battery_avg_current(BATTERY_PRIMARY));
	g_assert_true(battery_current(BATTERY_PRIMARY) == battery_avg_current(
	                  BATTERY_PRIMARY));


	//
	// Where the driver does export current_avg, that is the node to read: the
	// MindPhone's MT6739 gauge pins current_now to 0 and carries the real
	// reading on current_avg only, so delegating to the instantaneous node
	// reported no current at all. The mock answers by path, so a differing
	// current_avg is what proves which one was read.
	//
	reset_battery_path_retvals();
	test_batt_current_path_exists = true;
	test_batt_current_avg_path_exists = true;
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = 0;
	test_FileGetDouble_avg_result = 0;
	test_FileGetDouble_avg_retval = 138000;
	g_assert_true(138 == battery_avg_current(BATTERY_PRIMARY));
	// and the instantaneous reading is still its own answer
	g_assert_true(0 == battery_current(BATTERY_PRIMARY));

	// current_avg is normalised against status just as current_now is
	reset_battery_path_retvals();
	test_batt_current_avg_path_exists = true;
	test_batt_status_path_exists = true;
	test_FileGetString_status_retval = 0;
	g_strlcpy(test_FileGetString_status, "Charging",
	          sizeof(test_FileGetString_status));
	test_FileGetDouble_avg_result = 0;
	test_FileGetDouble_avg_retval = -138000;
	g_assert_true(138 == battery_avg_current(BATTERY_PRIMARY));

	forget_batteries();
}

//
// Tests for the battery_full40 API method
// double battery_full40(int index)
//
static void
test_battery_full40(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// Check for failure returned from test_batt_charge_full_path
	reset_battery_path_retvals();
	test_batt_charge_full_path_exists = false;
	test_batt_charge_full_path_retval = 2300000;
	g_assert_true(-1 == battery_full40(BATTERY_PRIMARY));

	// Check for failure returned from test_batt_charge_full_path
	reset_battery_path_retvals();
	test_batt_charge_full_path_exists = true;
	test_batt_charge_full_path_retval = -1;
	g_assert_true(-1 == battery_full40(BATTERY_PRIMARY));

	// Check for correct return value from test_batt_charge_full_path
	reset_battery_path_retvals();
	test_batt_charge_full_path_exists = true;
	test_batt_charge_full_path_retval = 2300000;
	// TODO: Should this be in mA or uA?  Device returns uA but emulator returns mA!
	g_assert_true((2300000 / 1000) == battery_full40(BATTERY_PRIMARY));


	// charge_full_design is read without an existence check - it is the last
	// fallback, and a failed read is the answer either way
	// Check for failure returned from test_batt_charge_full_design_path
	reset_battery_path_retvals();
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = -1;
	g_assert_true(-1 == battery_full40(BATTERY_PRIMARY));

	// Check for correct return value from test_batt_charge_full_design_path
	reset_battery_path_retvals();
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = 3400000;
	// TODO: Should this be in mA or uA?  Device returns uA but emulator returns mA!
	g_assert_true((3400000 / 1000) == battery_full40(BATTERY_PRIMARY));

	// Out-of-range battery
	g_assert_true(-1 == battery_full40(battery_count()));

	forget_batteries();
}

//
// Tests for the battery_rawcoulomb API method
// double battery_rawcoulomb(int index)
//
static void
test_battery_rawcoulomb(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// Check for failure returned from battery_rawcoulomb (not implemented)
	g_assert_true(-1 == battery_rawcoulomb(BATTERY_PRIMARY));
}

//
// Tests for the battery_coulomb API method
// double battery_coulomb(int index)
//
static void
test_battery_coulomb(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// Out-of-range battery
	reset_battery_path_retvals();
	g_assert_true(-1 == battery_coulomb(battery_count()));

	// Check for failure returned from test_batt_charge_now_path
	reset_battery_path_retvals();
	test_batt_charge_now_path_exists = true;
	test_batt_charge_now_path_retval = -1;
	g_assert_true(-1 == battery_coulomb(BATTERY_PRIMARY));

	// Check for correct return value from test_batt_charge_now_path
	reset_battery_path_retvals();
	test_batt_charge_now_path_exists = true;
	test_batt_charge_now_path_retval = 1840000;
	g_assert_true((1840000 / 1000) == battery_coulomb(BATTERY_PRIMARY));

	// A driver with no charge_now at all, which is how Qualcomm's charger
	// driver exports the "battery" supply: charge_counter carries the charge
	// and nothing else does. 2634787 uAh is a tissot reading.
	reset_battery_path_retvals();
	test_batt_charge_counter_path_exists = true;
	test_batt_charge_counter_path_retval = 2634787;
	g_assert_true((2634787 / 1000.0) == battery_coulomb(BATTERY_PRIMARY));

	// A charge_now that is present and permanently zero, which is how the
	// same platform exports the "bms" supply. The counter beside it wins.
	reset_battery_path_retvals();
	test_batt_charge_now_path_exists = true;
	test_batt_charge_now_path_retval = 0;
	test_batt_charge_counter_path_exists = true;
	test_batt_charge_counter_path_retval = 2634787;
	g_assert_true((2634787 / 1000.0) == battery_coulomb(BATTERY_PRIMARY));

	// charge_now wins when it has something to say, so nothing changes on a
	// device that was already answering.
	reset_battery_path_retvals();
	test_batt_charge_now_path_exists = true;
	test_batt_charge_now_path_retval = 1840000;
	test_batt_charge_counter_path_exists = true;
	test_batt_charge_counter_path_retval = 2634787;
	g_assert_true((1840000 / 1000) == battery_coulomb(BATTERY_PRIMARY));

	// A flat zero with no counter to prefer is no answer, not a measurement:
	// a present pack does not hold nothing, so the node is simply not wired
	// up. A sargo whose gauge never loaded its profile reports 0 on every
	// charge attribute it has while sitting at 61%, and "0 mAh" beside that
	// percentage is a claim about the hardware that is not true.
	reset_battery_path_retvals();
	test_batt_charge_now_path_exists = true;
	test_batt_charge_now_path_retval = 0;
	g_assert_true(-1 == battery_coulomb(BATTERY_PRIMARY));

	// ... and the same when the counter is the one reading zero
	reset_battery_path_retvals();
	test_batt_charge_counter_path_exists = true;
	test_batt_charge_counter_path_retval = 0;
	g_assert_true(-1 == battery_coulomb(BATTERY_PRIMARY));

	// Samsung's sec-battery puts its charging mode in charge_now: 1 on a Galaxy
	// A3 (2015) at 23%. Under 1 mAh beside a percentage is no charge, and with
	// no capacity stated there is nothing to derive one from.
	reset_battery_path_retvals();
	test_batt_charge_now_path_exists = true;
	test_batt_charge_now_path_retval = 1;
	test_batt_capacity_path_exists = true;
	test_batt_capacity_path_retval = 23;
	g_assert_true(-1 == battery_coulomb(BATTERY_PRIMARY));

	forget_batteries();
}

//
// Tests for the battery_full_design API method
// double battery_full_design(int index)
//
static void
test_battery_full_design(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// Out-of-range battery
	reset_battery_path_retvals();
	g_assert_true(-1 == battery_full_design(battery_count()));

	// A driver that does not report a design capacity
	reset_battery_path_retvals();
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = -1;
	g_assert_true(-1 == battery_full_design(BATTERY_PRIMARY));

	// The ordinary case: the battery supply carries it
	reset_battery_path_retvals();
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = 3080000;
	g_assert_true((3080000 / 1000.0) == battery_full_design(BATTERY_PRIMARY));

	//
	// A Qualcomm board: charge_full_design is on the BMS supply and not on
	// the battery supply the module picked. Paths are resolved when the
	// battery is detected, so the mocks have to say so before the list is
	// rebuilt rather than afterwards.
	//
	reset_battery_path_retvals();
	test_batt_charge_full_design_path_exists = false;
	test_find_power_supply_bms_retval = TEST_BMS_NODE;
	test_bms_charge_full_design_path_exists = true;
	test_bms_charge_full_design_path_retval = 3080000;
	battery_forget_all();
	detect_battery_sysfs_paths();
	g_assert_true(1 == battery_count());
	g_assert_true((3080000 / 1000.0) == battery_full_design(BATTERY_PRIMARY));

	// The battery supply wins when it has the attribute, so a board with
	// both does not start reading the gauge instead.
	reset_battery_path_retvals();
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = 4000000;
	test_find_power_supply_bms_retval = TEST_BMS_NODE;
	test_bms_charge_full_design_path_exists = true;
	test_bms_charge_full_design_path_retval = 3080000;
	battery_forget_all();
	detect_battery_sysfs_paths();
	g_assert_true((4000000 / 1000.0) == battery_full_design(BATTERY_PRIMARY));

	// Neither supply has it: still -1, not a silent fall back to
	// charge_full, which would report every pack as factory fresh.
	reset_battery_path_retvals();
	test_batt_charge_full_design_path_exists = false;
	test_batt_charge_full_path_exists = true;
	test_batt_charge_full_path_retval = 2300000;
	battery_forget_all();
	detect_battery_sysfs_paths();
	g_assert_true(-1 == battery_full_design(BATTERY_PRIMARY));

	//
	// A driver reporting the two attributes in different units. Both of
	// these are real readings - the first from a MediaTek Halium port, the
	// second from a radon - where charge_full_design comes back a factor of
	// ten below charge_full. Taken at face value they say the pack is
	// holding ten times what it shipped with.
	//
	reset_battery_path_retvals();
	test_batt_charge_full_path_exists = true;
	test_batt_charge_full_path_retval = 2951000;
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = 295000;
	g_assert_true(-1 == battery_full_design(BATTERY_PRIMARY));

	reset_battery_path_retvals();
	test_batt_charge_full_path_exists = true;
	test_batt_charge_full_path_retval = 4370000;
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = 437000;
	g_assert_true(-1 == battery_full_design(BATTERY_PRIMARY));

	// The mismatch the other way round is just as impossible
	reset_battery_path_retvals();
	test_batt_charge_full_path_exists = true;
	test_batt_charge_full_path_retval = 295000;
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = 2951000;
	g_assert_true(-1 == battery_full_design(BATTERY_PRIMARY));

	// A genuinely worn pack is not a unit mismatch and still answers. 1500
	// of 3080 mAh is a pack at not quite half of what it shipped with.
	reset_battery_path_retvals();
	test_batt_charge_full_path_exists = true;
	test_batt_charge_full_path_retval = 1500000;
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = 3080000;
	g_assert_true((3080000 / 1000.0) == battery_full_design(BATTERY_PRIMARY));

	// Nor is a gauge that has learnt slightly above the design figure, which
	// is ordinary on a new pack.
	reset_battery_path_retvals();
	test_batt_charge_full_path_exists = true;
	test_batt_charge_full_path_retval = 3200000;
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = 3080000;
	g_assert_true((3080000 / 1000.0) == battery_full_design(BATTERY_PRIMARY));

	// With no charge_full to check against, the design figure is reported as
	// read - there is nothing to contradict it.
	reset_battery_path_retvals();
	test_batt_charge_full_path_exists = false;
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = 295000;
	battery_forget_all();
	detect_battery_sysfs_paths();
	g_assert_true((295000 / 1000.0) == battery_full_design(BATTERY_PRIMARY));

	forget_batteries();
}

//
// Point the config lookup at a fixture holding body, or back at the file that
// does not exist when body is NULL.
//
static gchar *test_conf_tmp = NULL;

static void test_conf_set(const char *body)
{
	if (test_conf_tmp)
	{
		unlink(test_conf_tmp);
		g_free(test_conf_tmp);
		test_conf_tmp = NULL;
	}

	test_nyx_conf_file = TEST_NO_CONF;

	if (body)
	{
		test_conf_tmp = g_strdup_printf("%s/test_dev_battery_%d.conf",
		                                g_get_tmp_dir(), (int) getpid());
		g_assert_true(g_file_set_contents(test_conf_tmp, body, -1, NULL));
		test_nyx_conf_file = test_conf_tmp;
	}
}

//
// The driver figures of a MediaTek gauge that carries the vendor's reference
// capacity table: charge_full in the table's unit, charge_counter that figure
// scaled by the percentage, and charge_full_design a factor of ten below it.
//
static void mock_mediatek_gauge(int charge_counter)
{
	reset_battery_path_retvals();
	test_batt_charge_full_path_exists = true;
	test_batt_charge_full_path_retval = 2946000;
	test_batt_charge_counter_path_exists = true;
	test_batt_charge_counter_path_retval = charge_counter;
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = 294000;
}

//
// Detect the batteries with body as the config. The generated nyx.conf always
// names the primary battery, so a fixture does too unless it says otherwise
// with a sysfs_path= of its own - an empty one being how a device whose
// primary was not named looks.
//
static void detect_with_conf(const char *body)
{
	gchar *full = NULL;

	if (body && !strstr(body, "sysfs_path="))
	{
		const char *header = "[module.battery]\n";

		g_assert_true(g_str_has_prefix(body, header));
		full = g_strdup_printf("%ssysfs_path=%s\n%s", header, TEST_BATT_NODE,
		                       body + strlen(header));
	}

	test_conf_set(full ? full : body);
	g_free(full);
	battery_forget_all();
	detect_battery_sysfs_paths();
	g_assert_true(battery_count() >= 1);
}

//
// Tests for the capacity keys in [module.battery]: full_capacity_mah and
// design_capacity_mah.
//
static void
test_battery_configured_capacity(void)
{
	// Neither key: the driver's figures are used exactly as before, and the
	// mismatched design figure is still refused.
	mock_mediatek_gauge(2946000);
	detect_with_conf("[module.battery]\nfull_capacity_mah=\ndesign_capacity_mah=\n");
	g_assert_cmpfloat_with_epsilon(battery_full40(BATTERY_PRIMARY), 2946.0, 0.001);
	g_assert_cmpfloat_with_epsilon(battery_coulomb(BATTERY_PRIMARY), 2946.0, 0.001);
	g_assert_true(-1 == battery_full_design(BATTERY_PRIMARY));

	// ... and the fall-back to the design figure where charge_full has nothing
	// to say is untouched.
	reset_battery_path_retvals();
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = 3080000;
	detect_with_conf(NULL);
	g_assert_cmpfloat_with_epsilon(battery_full40(BATTERY_PRIMARY), 3080.0, 0.001);

	// The full capacity alone: charge_full is replaced and the driver's charge
	// is corrected by the same factor, so a full pack stays full and a half
	// full pack stays half full.
	mock_mediatek_gauge(2946000);
	detect_with_conf("[module.battery]\nfull_capacity_mah=5100\n");
	g_assert_cmpfloat_with_epsilon(battery_full40(BATTERY_PRIMARY), 5100.0, 0.001);
	g_assert_cmpfloat_with_epsilon(battery_coulomb(BATTERY_PRIMARY), 5100.0, 0.001);
	g_assert_true(-1 == battery_full_design(BATTERY_PRIMARY));

	mock_mediatek_gauge(1473000);
	detect_with_conf("[module.battery]\nfull_capacity_mah=5100\n");
	g_assert_cmpfloat_with_epsilon(battery_coulomb(BATTERY_PRIMARY), 2550.0, 0.001);

	// No charge_full on the driver to take the factor from: the percentage is
	// the only other statement of how full the pack is.
	reset_battery_path_retvals();
	test_batt_charge_counter_path_exists = true;
	test_batt_charge_counter_path_retval = 1000000;
	test_batt_capacity_path_exists = true;
	test_batt_capacity_path_retval = 40;
	detect_with_conf("[module.battery]\nfull_capacity_mah=5000\n");
	g_assert_cmpfloat_with_epsilon(battery_coulomb(BATTERY_PRIMARY), 2000.0, 0.001);

	// A driver whose only capacity is the design figure: that is the attribute
	// that comes in a different unit, so it cannot supply the factor either -
	// a ratio against 294 mAh would be out by exactly that unit. The percentage
	// answers instead.
	reset_battery_path_retvals();
	test_batt_charge_counter_path_exists = true;
	test_batt_charge_counter_path_retval = 1000000;
	test_batt_charge_full_design_path_exists = true;
	test_batt_charge_full_design_path_retval = 294000;
	test_batt_capacity_path_exists = true;
	test_batt_capacity_path_retval = 40;
	detect_with_conf("[module.battery]\nfull_capacity_mah=5000\n");
	g_assert_cmpfloat_with_epsilon(battery_coulomb(BATTERY_PRIMARY), 2000.0, 0.001);

	// ... and with neither, no answer rather than a made-up one.
	reset_battery_path_retvals();
	test_batt_charge_counter_path_exists = true;
	test_batt_charge_counter_path_retval = 1000000;
	detect_with_conf("[module.battery]\nfull_capacity_mah=5000\n");
	g_assert_true(-1 == battery_coulomb(BATTERY_PRIMARY));

	// The sec-battery charge_now above, on a device that states its capacity:
	// the stated capacity times the percentage, 1900 mAh at 23%.
	reset_battery_path_retvals();
	test_batt_charge_now_path_exists = true;
	test_batt_charge_now_path_retval = 1;
	test_batt_capacity_path_exists = true;
	test_batt_capacity_path_retval = 23;
	detect_with_conf("[module.battery]\nfull_capacity_mah=1900\n");
	g_assert_cmpfloat_with_epsilon(battery_coulomb(BATTERY_PRIMARY), 437.0, 0.001);

	// A driver with no charge to report stays unanswered whatever the config
	// says: the key corrects a figure, it does not invent one.
	reset_battery_path_retvals();
	detect_with_conf("[module.battery]\nfull_capacity_mah=5100\n");
	g_assert_true(-1 == battery_coulomb(BATTERY_PRIMARY));
	g_assert_cmpfloat_with_epsilon(battery_full40(BATTERY_PRIMARY), 5100.0, 0.001);

	// The design capacity is only taken together with the full capacity: alone
	// it would pair the device's figure with the driver's wrong one and publish
	// a wear figure for a pack that has none. So it is ignored, and the driver's
	// own answer - here no answer, the units disagree - stands.
	mock_mediatek_gauge(2946000);
	detect_with_conf("[module.battery]\ndesign_capacity_mah=5100\n");
	g_assert_true(-1 == battery_full_design(BATTERY_PRIMARY));
	g_assert_cmpfloat_with_epsilon(battery_full40(BATTERY_PRIMARY), 2946.0, 0.001);

	// Both together: the device's word on both, and the unit cross-check that
	// refuses the driver's design figure does not apply to it.
	mock_mediatek_gauge(2946000);
	detect_with_conf("[module.battery]\nfull_capacity_mah=5100\ndesign_capacity_mah=5100\n");
	g_assert_cmpfloat_with_epsilon(battery_full40(BATTERY_PRIMARY), 5100.0, 0.001);
	g_assert_cmpfloat_with_epsilon(battery_full_design(BATTERY_PRIMARY), 5100.0, 0.001);

	// ... including on a driver with no design attribute at all.
	reset_battery_path_retvals();
	detect_with_conf("[module.battery]\nfull_capacity_mah=5100\ndesign_capacity_mah=5100\n");
	g_assert_cmpfloat_with_epsilon(battery_full_design(BATTERY_PRIMARY), 5100.0, 0.001);

	// A stated figure is no answer for a battery that is not there: the node
	// has gone, so there is no pack to have a capacity.
	mock_mediatek_gauge(2946000);
	detect_with_conf("[module.battery]\nfull_capacity_mah=5100\ndesign_capacity_mah=5100\n");
	test_batt_sysfs_path_is_dir = false;
	g_assert_true(-1 == battery_full40(BATTERY_PRIMARY));
	g_assert_true(-1 == battery_full_design(BATTERY_PRIMARY));
	test_batt_sysfs_path_is_dir = true;

	// A primary the config did not name was found by walking the class, and
	// may be a docked keyboard's cell: the figures are not applied to it.
	mock_mediatek_gauge(2946000);
	detect_with_conf("[module.battery]\nsysfs_path=\nfull_capacity_mah=5100\n"
	                 "design_capacity_mah=5100\n");
	g_assert_cmpfloat_with_epsilon(battery_full40(BATTERY_PRIMARY), 2946.0, 0.001);
	g_assert_cmpfloat_with_epsilon(battery_coulomb(BATTERY_PRIMARY), 2946.0, 0.001);
	g_assert_true(-1 == battery_full_design(BATTERY_PRIMARY));

	// A measured charge_now is the pack's, not a table's: it is reported as
	// the driver gives it, whatever full capacity the config states.
	mock_mediatek_gauge(2946000);
	test_batt_charge_now_path_exists = true;
	test_batt_charge_now_path_retval = 1000000;
	detect_with_conf("[module.battery]\nfull_capacity_mah=5100\n");
	g_assert_cmpfloat_with_epsilon(battery_coulomb(BATTERY_PRIMARY), 1000.0, 0.001);

	// A charge the pack cannot hold - the two reads behind the factor straddled
	// a profile reload, say - is capped at a full pack.
	mock_mediatek_gauge(5000000);
	detect_with_conf("[module.battery]\nfull_capacity_mah=5100\n");
	g_assert_cmpfloat_with_epsilon(battery_coulomb(BATTERY_PRIMARY), 5100.0, 0.001);

	// The percentage fallback: above a hundred is rounding, not charge, and an
	// empty pack is no answer rather than a measured zero.
	reset_battery_path_retvals();
	test_batt_charge_counter_path_exists = true;
	test_batt_charge_counter_path_retval = 1000000;
	test_batt_capacity_path_exists = true;
	test_batt_capacity_path_retval = 150;
	detect_with_conf("[module.battery]\nfull_capacity_mah=5000\n");
	g_assert_cmpfloat_with_epsilon(battery_coulomb(BATTERY_PRIMARY), 5000.0, 0.001);
	test_batt_capacity_path_retval = 0;
	g_assert_true(-1 == battery_coulomb(BATTERY_PRIMARY));

	// Values that are not a capacity in mAh are ignored, not half-parsed: a
	// trailing unit, a sign, zero, a fraction, an overflow, and a figure beyond
	// any pack there is.
	const char *bad[] = { "abc", "-5", "0", "5100mAh", "5100.5", "1000001",
	                      "99999999999999999999", NULL
	                    };

	for (int i = 0; bad[i]; i++)
	{
		gchar *body = g_strdup_printf(
		                  "[module.battery]\nfull_capacity_mah=%s\n"
		                  "design_capacity_mah=%s\n", bad[i], bad[i]);

		mock_mediatek_gauge(2946000);
		detect_with_conf(body);
		g_assert_cmpfloat_with_epsilon(battery_full40(BATTERY_PRIMARY), 2946.0, 0.001);
		g_assert_cmpfloat_with_epsilon(battery_coulomb(BATTERY_PRIMARY), 2946.0, 0.001);
		g_assert_true(-1 == battery_full_design(BATTERY_PRIMARY));
		g_free(body);
	}

	// The keys describe the device's own pack: an accessory battery in the same
	// list keeps what its own driver reports.
	mock_mediatek_gauge(2946000);
	test_kbd_charge_full_path_exists = true;
	test_kbd_charge_full_path_retval = 2000000;
	test_kbd_charge_full_design_path_exists = true;
	test_kbd_charge_full_design_path_retval = 2000000;
	char *extras[] = { TEST_BATT_NODE, TEST_KBD_NODE, NULL };
	test_find_power_supply_sysfs_paths_retval = extras;
	detect_with_conf("[module.battery]\n"
	                 "full_capacity_mah=5100\ndesign_capacity_mah=5100\n");
	g_assert_true(2 == battery_count());
	g_assert_cmpfloat_with_epsilon(battery_full40(BATTERY_PRIMARY), 5100.0, 0.001);
	g_assert_cmpfloat_with_epsilon(battery_full_design(BATTERY_PRIMARY), 5100.0, 0.001);
	g_assert_cmpfloat_with_epsilon(battery_full40(1), 2000.0, 0.001);
	g_assert_cmpfloat_with_epsilon(battery_full_design(1), 2000.0, 0.001);
	test_find_power_supply_sysfs_paths_retval = NULL;
	test_kbd_charge_full_path_exists = false;
	test_kbd_charge_full_design_path_exists = false;

	// Detection is re-run on a udev change: a key removed from the config
	// must not leave the old figure behind.
	mock_mediatek_gauge(2946000);
	detect_with_conf("[module.battery]\nfull_capacity_mah=5100\n");
	g_assert_cmpfloat_with_epsilon(battery_full40(BATTERY_PRIMARY), 5100.0, 0.001);
	detect_with_conf(NULL);
	g_assert_cmpfloat_with_epsilon(battery_full40(BATTERY_PRIMARY), 2946.0, 0.001);

	test_conf_set(NULL);
	forget_batteries();
}

//
// Tests for the battery_health API method
// int battery_health(int index)
//
static void
test_battery_health(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// Out-of-range battery
	reset_battery_path_retvals();
	g_assert_true(NYX_BATTERY_HEALTH_UNKNOWN == battery_health(battery_count()));

	// No health attribute at all
	reset_battery_path_retvals();
	g_assert_true(NYX_BATTERY_HEALTH_UNKNOWN == battery_health(BATTERY_PRIMARY));

	// Every value the kernel defines, spelled as the kernel spells it
	static const struct
	{
		const char *text;
		int value;
	} cases[] =
	{
		{ "Good",                  NYX_BATTERY_HEALTH_GOOD },
		{ "Overheat",              NYX_BATTERY_HEALTH_OVERHEAT },
		{ "Dead",                  NYX_BATTERY_HEALTH_DEAD },
		{ "Over voltage",          NYX_BATTERY_HEALTH_OVERVOLTAGE },
		{ "Unspecified failure",   NYX_BATTERY_HEALTH_UNSPEC_FAILURE },
		{ "Cold",                  NYX_BATTERY_HEALTH_COLD },
		{ "Watchdog timer expire", NYX_BATTERY_HEALTH_WATCHDOG_TIMER_EXPIRE },
		{ "Safety timer expire",   NYX_BATTERY_HEALTH_SAFETY_TIMER_EXPIRE },
		{ "Over current",          NYX_BATTERY_HEALTH_OVERCURRENT },
		{ "Calibration required",  NYX_BATTERY_HEALTH_CALIBRATION_REQUIRED },
		{ "Warm",                  NYX_BATTERY_HEALTH_WARM },
		{ "Cool",                  NYX_BATTERY_HEALTH_COOL },
		{ "Hot",                   NYX_BATTERY_HEALTH_HOT },
		{ "No battery",            NYX_BATTERY_HEALTH_NO_BATTERY },
	};
	size_t i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		reset_battery_path_retvals();
		test_batt_health_path_exists = true;
		test_FileGetString_health_retval = 0;
		g_strlcpy(test_FileGetString_health, cases[i].text,
		          sizeof(test_FileGetString_health));
		g_assert_true(cases[i].value == battery_health(BATTERY_PRIMARY));
	}

	// Drivers are not consistent about case
	reset_battery_path_retvals();
	test_batt_health_path_exists = true;
	test_FileGetString_health_retval = 0;
	g_strlcpy(test_FileGetString_health, "GOOD",
	          sizeof(test_FileGetString_health));
	g_assert_true(NYX_BATTERY_HEALTH_GOOD == battery_health(BATTERY_PRIMARY));

	// Something nobody has seen before is unknown, not a guess
	reset_battery_path_retvals();
	test_batt_health_path_exists = true;
	test_FileGetString_health_retval = 0;
	g_strlcpy(test_FileGetString_health, "Slightly damp",
	          sizeof(test_FileGetString_health));
	g_assert_true(NYX_BATTERY_HEALTH_UNKNOWN == battery_health(BATTERY_PRIMARY));

	forget_batteries();
}

//
// Tests for the battery_age API method
// double battery_age(int index)
//
static void
test_battery_age(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// Check for failure returned from battery_age (not implemented)
	g_assert_true(-1 == battery_age(BATTERY_PRIMARY));
}

//
// Tests for the battery_is_present API method
// bool battery_is_present(int index)
//
static void
test_battery_is_present(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// Out-of-range battery
	reset_battery_path_retvals();
	g_assert_true(false == battery_is_present(battery_count()));

	//
	// The node itself is gone: a detachable battery that is currently
	// detached. Answered before any attribute is read, since they would all
	// fail anyway.
	//
	reset_battery_path_retvals();
	test_batt_sysfs_path_is_dir = false;
	test_batt_present_path_exists = true;
	test_batt_present_path_retval = 1;
	g_assert_true(false == battery_is_present(BATTERY_PRIMARY));

	//
	// "present" is optional in the power_supply class and a soldered-in cell
	// has no reason to export it. Its absence must not read as "no battery",
	// or such a device reports none at all.
	//
	reset_battery_path_retvals();
	test_batt_present_path_exists = false;
	test_batt_present_path_retval = 0;
	g_assert_true(true == battery_is_present(BATTERY_PRIMARY));

	// Check for failure returned from test_batt_present_path
	reset_battery_path_retvals();
	test_batt_present_path_exists = true;
	test_batt_present_path_retval = -1;
	g_assert_true(false == battery_is_present(BATTERY_PRIMARY));

	// Check for correct return value from test_batt_present_path (battery not present)
	reset_battery_path_retvals();
	test_batt_present_path_exists = true;
	test_batt_present_path_retval = 0;
	g_assert_true(false == battery_is_present(BATTERY_PRIMARY));

	// Check for correct return value from test_batt_present_path (battery present)
	reset_battery_path_retvals();
	test_batt_present_path_exists = true;
	test_batt_present_path_retval = 1;
	g_assert_true(true == battery_is_present(BATTERY_PRIMARY));

	forget_batteries();
}

//
// Tests for the get_battery_ctia_params API method
// nyx_battery_ctia_t *get_battery_ctia_params(void)
//
static void
test_get_battery_ctia_params(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	// Check for correct return value when voltage is zero
	nyx_battery_ctia_t *battery_ctia_params;
	battery_ctia_params = get_battery_ctia_params();
	g_assert_true(NULL != battery_ctia_params);

	// Can't assume CHARGE_MIN_TEMPERATURE_C is zero, so skip that...
	//g_assert_true(0 == battery_ctia_params->charge_min_temp_c);

	g_assert_true(0 != battery_ctia_params->charge_max_temp_c);
	g_assert_true(0 != battery_ctia_params->battery_crit_max_temp);

	// Can't assume skip_battery_authentication is true, so skip that...
	//g_assert_true(battery_ctia_params->skip_battery_authentication);
}

//
// Tests for detect_battery_sysfs_paths()
//
// The primary battery keeps index 0, and anything else the kernel calls a
// battery follows it. Picking between several by directory order picks at
// random, which is what this replaced.
//
static void
test_detect_battery_sysfs_paths(void)
{
	char *extras[] = { TEST_BATT_NODE, TEST_KBD_NODE, NULL };

	// One battery, found by walking the power_supply class
	reset_battery_path_retvals();
	g_assert_true(1 == battery_count());
	g_assert_cmpstr(battery_name(BATTERY_PRIMARY), ==, TEST_BATT_NODE);
	g_assert_cmpstr(battery_role(BATTERY_PRIMARY), ==, "main");

	// A second battery: the phone's own plus the one in its keyboard
	reset_battery_path_retvals();
	test_find_power_supply_sysfs_paths_retval = extras;
	detect_battery_sysfs_paths();
	g_assert_true(2 == battery_count());
	g_assert_cmpstr(battery_name(BATTERY_PRIMARY), ==, TEST_BATT_NODE);
	g_assert_cmpstr(battery_role(BATTERY_PRIMARY), ==, "main");
	g_assert_cmpstr(battery_name(1), ==, TEST_KBD_NODE);
	g_assert_cmpstr(battery_role(1), ==, "aux");

	// The primary is not listed twice just because the walk finds it too
	g_assert_true(0 != g_strcmp0(battery_name(BATTERY_PRIMARY), battery_name(1)));

	// Indices outside the list are refused rather than read
	g_assert_cmpstr(battery_name(-1), ==, "");
	g_assert_cmpstr(battery_name(battery_count()), ==, "");
	g_assert_cmpstr(battery_role(-1), ==, "");
	g_assert_cmpstr(battery_role(battery_count()), ==, "");

	//
	// Nothing configured and nothing found still leaves one slot: callers
	// expect a primary to exist, and every read from it fails cleanly.
	//
	reset_battery_path_retvals();
	test_find_power_supply_sysfs_path_retval = NULL;
	test_find_power_supply_sysfs_paths_retval = NULL;
	detect_battery_sysfs_paths();
	g_assert_true(1 == battery_count());
	g_assert_cmpstr(battery_role(BATTERY_PRIMARY), ==, "main");

	forget_batteries();
}

//
// A large pack must not overflow the percentage calculation. energy_now is in
// microwatt hours, so 100 * now leaves the range of a signed int at about
// 21.5 Wh, and signed overflow is undefined rather than merely wrong.
//
static void
test_battery_percent_large_pack(void)
{
	// 50 Wh, half full: 100 * 25000000 is well past INT_MAX
	reset_battery_path_retvals();
	test_batt_energy_now_path_exists = true;
	test_batt_energy_full_path_exists = true;
	test_batt_energy_now_path_retval = 25000000;
	test_batt_energy_full_path_retval = 50000000;
	g_assert_true(50 == battery_percent(BATTERY_PRIMARY));

	// and at the top of the range
	reset_battery_path_retvals();
	test_batt_energy_now_path_exists = true;
	test_batt_energy_full_path_exists = true;
	test_batt_energy_now_path_retval = 2000000000;
	test_batt_energy_full_path_retval = 2000000000;
	g_assert_true(100 == battery_percent(BATTERY_PRIMARY));

	forget_batteries();
}

//
// Tests for the fake ("pseudo") battery mode, which is how an emulated target
// with no battery of its own is given one to report.
// void battery_set_fakemode(bool enable)
// nyx_error_t battery_get_fakemode(bool *enable)
//
static void
test_battery_fakemode(void)
{
	bool testEnable;

	//
	// nyx_utils_read() answers -1 when it cannot open the node, which is the
	// usual case: pseudo_batt only exists on targets that have it. Treating
	// that as a successful read ran strstr() over an uninitialised buffer.
	//
	reset_battery_path_retvals();
	test_nyx_utils_read_result = -1;
	testEnable = true;
	g_assert_true(NYX_ERROR_NONE != battery_get_fakemode(&testEnable));
	g_assert_true(true == testEnable);

	// A NULL out-parameter is refused
	reset_battery_path_retvals();
	test_nyx_utils_read_result = 0;
	g_assert_true(NYX_ERROR_NONE != battery_get_fakemode(NULL));

	// "NORMAL" means the real battery is being passed through
	reset_battery_path_retvals();
	test_nyx_utils_read_result = 0;
	g_strlcpy(test_nyx_utils_read_contents, "NORMAL",
	          sizeof(test_nyx_utils_read_contents));
	testEnable = true;
	g_assert_true(NYX_ERROR_NONE == battery_get_fakemode(&testEnable));
	g_assert_true(false == testEnable);

	// anything else means the pseudo battery is driving
	reset_battery_path_retvals();
	test_nyx_utils_read_result = 0;
	g_strlcpy(test_nyx_utils_read_contents, "PSEUDO 1 100 40 4100 80 1",
	          sizeof(test_nyx_utils_read_contents));
	testEnable = false;
	g_assert_true(NYX_ERROR_NONE == battery_get_fakemode(&testEnable));
	g_assert_true(true == testEnable);

	//
	// Only the string is written, not the whole buffer: passing sizeof used
	// to hand the kernel the uninitialised stack bytes past the terminator.
	//
	reset_battery_path_retvals();
	battery_set_fakemode(true);
	g_assert_true(strlen(test_nyx_utils_write_buf) == test_nyx_utils_write_size);
	g_assert_cmpstr(test_nyx_utils_write_buf, ==, "1 1 100 40 4100 80 1");

	reset_battery_path_retvals();
	battery_set_fakemode(false);
	g_assert_true(strlen(test_nyx_utils_write_buf) == test_nyx_utils_write_size);
	g_assert_cmpstr(test_nyx_utils_write_buf, ==, "0 1 100 40 4100 80 1");

	// With no battery in the list there is nothing to write to, and nothing
	// should be written
	forget_batteries();
	test_nyx_utils_write_size = 0;
	battery_set_fakemode(true);
	g_assert_true(0 == test_nyx_utils_write_size);
	g_assert_true(NYX_ERROR_NONE != battery_get_fakemode(&testEnable));
}

//
// Tests for the battery_deinit API method
// nyx_error_t battery_deinit(void)
//
static void
test_battery_deinit(void)
{
	// Everything the udev side can be asked for is available, as at the end of
	// test_battery_init.
	testUdevStruct_retval = &testUdevStruct;
	testUdevMonitorStruct_retval = &testUdevMonitorStruct;
	testUdevMonitorFilterAddMatchResult_retval = 0;
	testUdevMonitorEnableReceiving_retval = 0;
	testUdevMonitorGetFd_retval = 0;
	testGIOChannel_retval = &testGIOChannel;
	testEventSourceId_retVal = testEventSourceIdGood;

	// Nothing was initialised: deinit is harmless and leaves nothing behind.
	forget_batteries();
	g_assert_true(NYX_ERROR_NONE == battery_deinit());
	g_assert_true(0 == battery_count());
	g_assert_true(0 == testGIOChannelRefcount);
	g_assert_true(0 == testUdevRefcount);
	g_assert_true(0 == testUdevMonitorRefcount);

	// Initialised: the udev context, the monitor, the channel and the watch are
	// all held, and so is the battery list...
	reset_battery_path_retvals();
	g_assert_true(NYX_ERROR_NONE == battery_init());
	g_assert_cmpint(testUdevRefcount, >, 0);
	g_assert_cmpint(testUdevMonitorRefcount, >, 0);
	g_assert_cmpint(testGIOChannelRefcount, >, 0);
	g_assert_true(battery_count() >= 1);

	// ... and deinit gives every one of them back.
	g_assert_true(NYX_ERROR_NONE == battery_deinit());
	g_assert_true(0 == testGIOChannelRefcount);
	g_assert_true(0 == testUdevRefcount);
	g_assert_true(0 == testUdevMonitorRefcount);
	g_assert_true(0 == battery_count());

	// Deinit twice: there is nothing left to release, and nothing is released a
	// second time - a reference count below zero would be exactly that.
	g_assert_true(NYX_ERROR_NONE == battery_deinit());
	g_assert_true(0 == testGIOChannelRefcount);
	g_assert_true(0 == testUdevRefcount);
	g_assert_true(0 == testUdevMonitorRefcount);
	g_assert_true(0 == battery_count());

	// Deinit does not leave the module unable to start again.
	reset_battery_path_retvals();
	g_assert_true(NYX_ERROR_NONE == battery_init());
	g_assert_true(battery_count() >= 1);
	g_assert_true(NYX_ERROR_NONE == battery_deinit());
	g_assert_true(0 == testGIOChannelRefcount);
	g_assert_true(0 == testUdevRefcount);
	g_assert_true(0 == testUdevMonitorRefcount);
	g_assert_true(0 == battery_count());
}

//
// Tests for the battery_authenticate API method
// bool battery_authenticate(void)
//
// There is nothing to authenticate against - the module says so by answering
// true, as batterylib.c passes on to its caller - so what is worth pinning is
// that it answers that however the supply looks and does not touch it.
//
static void
test_battery_authenticate(void)
{
	// An ordinary battery
	reset_battery_path_retvals();
	test_batt_present_path_exists = true;
	test_batt_present_path_retval = 1;
	g_assert_true(true == battery_authenticate());
	g_assert_true(0 == test_nyx_utils_write_size);

	// A battery that reads as absent
	reset_battery_path_retvals();
	test_batt_present_path_exists = true;
	test_batt_present_path_retval = 0;
	g_assert_true(true == battery_authenticate());
	g_assert_true(0 == test_nyx_utils_write_size);

	// No battery at all
	forget_batteries();
	test_nyx_utils_write_size = 0;
	g_assert_true(true == battery_authenticate());
	g_assert_true(0 == test_nyx_utils_write_size);
	g_assert_true(0 == battery_count());
}

//
// Tests for the battery_set_wakeup_percent API method
// void battery_set_wakeup_percent(int percentage)
//
// Not supported, and says so by doing nothing: the point of the test is that it
// really does nothing, for any value and with or without a battery - not that
// a value is stored or that the kernel is written to - so that a caller cannot
// be misled into thinking a threshold has been armed.
//
static void
test_battery_set_wakeup_percent(void)
{
	const int values[] = { G_MININT, -1, 0, 1, 50, 99, 100, 101, G_MAXINT };
	int percent_before, count_before;
	bool present_before;

	reset_battery_path_retvals();
	test_batt_capacity_path_exists = true;
	test_batt_capacity_path_retval = 80;
	test_batt_present_path_exists = true;
	test_batt_present_path_retval = 1;
	percent_before = battery_percent(BATTERY_PRIMARY);
	present_before = battery_is_present(BATTERY_PRIMARY);
	count_before = battery_count();
	g_assert_true(80 == percent_before);
	g_assert_true(present_before);

	for (size_t i = 0; i < G_N_ELEMENTS(values); i++)
	{
		battery_set_wakeup_percent(values[i]);

		// Nothing written, the battery list is as it was, and the readings that
		// a threshold could have changed are unchanged
		g_assert_true(0 == test_nyx_utils_write_size);
		g_assert_true(count_before == battery_count());
		g_assert_true(percent_before == battery_percent(BATTERY_PRIMARY));
		g_assert_true(present_before == battery_is_present(BATTERY_PRIMARY));
	}

	// With no battery in the list there is nothing to arm, and no failure
	forget_batteries();
	test_nyx_utils_write_size = 0;
	battery_set_wakeup_percent(50);
	g_assert_true(0 == test_nyx_utils_write_size);
	g_assert_true(0 == battery_count());
}

//
// Set-up GLib, then register and run the tests.
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/battery/device/battery_init", test_battery_init);
	g_test_add_func("/battery/device/battery_deinit", test_battery_deinit);

	g_test_add_func("/battery/device/battery_percent", test_battery_percent);
	g_test_add_func("/battery/device/battery_temperature",
	                test_battery_temperature);
	g_test_add_func("/battery/device/battery_voltage", test_battery_voltage);
	g_test_add_func("/battery/device/battery_current", test_battery_current);
	g_test_add_func("/battery/device/battery_avg_current",
	                test_battery_avg_current);

	g_test_add_func("/battery/device/battery_full40", test_battery_full40);
	g_test_add_func("/battery/device/battery_rawcoulomb", test_battery_rawcoulomb);
	g_test_add_func("/battery/device/battery_coulomb", test_battery_coulomb);
	g_test_add_func("/battery/device/battery_full_design",
	                test_battery_full_design);
	g_test_add_func("/battery/device/battery_configured_capacity",
	                test_battery_configured_capacity);
	g_test_add_func("/battery/device/battery_health", test_battery_health);
	g_test_add_func("/battery/device/battery_age", test_battery_age);

	g_test_add_func("/battery/device/battery_is_present", test_battery_is_present);
	g_test_add_func("/battery/device/detect_battery_sysfs_paths",
	                test_detect_battery_sysfs_paths);
	g_test_add_func("/battery/device/battery_percent_large_pack",
	                test_battery_percent_large_pack);
	g_test_add_func("/battery/device/battery_fakemode", test_battery_fakemode);
	g_test_add_func("/battery/device/get_battery_ctia_params",
	                test_get_battery_ctia_params);

	// TODO: Add test for _handle_event() callback function?

	// Not supported by device/battery.c (stub implementations): these pin that
	// they stay inert, not that they do anything.
	g_test_add_func("/battery/device/battery_authenticate",
	                test_battery_authenticate);
	g_test_add_func("/battery/device/battery_set_wakeup_percent",
	                test_battery_set_wakeup_percent);

	return g_test_run();
}
