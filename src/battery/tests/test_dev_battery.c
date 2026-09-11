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

// mock out externals defined in chargerlib.c

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
#define NYX_CONF_FILE "/nonexistent/test_dev_battery/nyx.conf"

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

// mock out calls to nyx-modules: utils.c
char *test_find_power_supply_sysfs_path_retval = TEST_BATT_NODE;

char *find_power_supply_sysfs_path(const char *device_type)
{
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

int FileGetString(const char *path, char *ret_string, size_t maxlen)
{
	if (ret_string && maxlen > 0)
	{
		ret_string[0] = '\0';
	}

	return -1;
}

// define paths used in detect_battery_sysfs_paths() in battery.c
static char *test_batt_sysfs_path = TEST_BATT_NODE;
static char *test_batt_capacity_path = "Battery/capacity";
static char *test_batt_energy_now_path = "Battery/energy_now";
static char *test_batt_energy_full_path = "Battery/energy_full";
static char *test_batt_energy_full_design_path = "Battery/energy_full_design";
static char *test_batt_charge_now_path = "Battery/charge_now";
static char *test_batt_charge_full_path = "Battery/charge_full";
static char *test_batt_charge_full_design_path = "Battery/charge_full_design";
static char *test_batt_temperature_path = "Battery/temp";
static char *test_batt_voltage_path = "Battery/voltage_now";
static char *test_batt_current_path = "Battery/current_now";
static char *test_batt_present_path = "Battery/present";
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
					else ifMatchReturnRetvalForTestPath(test_batt_charge_full_path)
						else ifMatchReturnRetvalForTestPath(test_batt_charge_full_design_path)
							else ifMatchReturnRetvalForTestPath(test_batt_temperature_path)
								else ifMatchReturnRetvalForTestPath(test_batt_voltage_path)
									else ifMatchReturnRetvalForTestPath(test_batt_current_path)
										else ifMatchReturnRetvalForTestPath(test_batt_present_path)
											else ifMatchReturnRetvalForTestPath(test_batt_fake_battery_path)

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
// which is a callback passed to g_io_add_watch() in _charger_init()...
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
int32_t test_batt_charge_full_path_exists = false;
int32_t test_batt_charge_full_design_path_exists = false;
int32_t test_batt_temperature_path_exists = false;
int32_t test_batt_voltage_path_exists = false;
int32_t test_batt_current_path_exists = false;
int32_t test_batt_present_path_exists = false;
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

	ifMatchReturnExistsForTestPath(test_batt_capacity_path)
	else ifMatchReturnExistsForTestPath(test_batt_energy_now_path)
		else ifMatchReturnExistsForTestPath(test_batt_energy_full_path)
			else ifMatchReturnExistsForTestPath(test_batt_energy_full_design_path)
				else ifMatchReturnExistsForTestPath(test_batt_charge_now_path)
					else ifMatchReturnExistsForTestPath(test_batt_charge_full_path)
						else ifMatchReturnExistsForTestPath(test_batt_charge_full_design_path)
							else ifMatchReturnExistsForTestPath(test_batt_temperature_path)
								else ifMatchReturnExistsForTestPath(test_batt_voltage_path)
									else ifMatchReturnExistsForTestPath(test_batt_current_path)
										else ifMatchReturnExistsForTestPath(test_batt_present_path)
											else ifMatchReturnExistsForTestPath(test_batt_fake_battery_path)

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
	// return value is "the event source id" which is not used by charger.c
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

#if 0
static int32_t init_charger_max_current = -1;
static int32_t init_connected = -1;
static int32_t init_powered = -1;
static bool init_is_charging = false;
static char *init_serial_number = "serialNumber";
static void resetTestChargerStatus(nyx_charger_status_t *chargerStatus)
{
	chargerStatus->charger_max_current = init_charger_max_current;
	chargerStatus->connected = init_connected;
	chargerStatus->powered = init_powered;
	chargerStatus->is_charging = init_is_charging;
	strncpy(chargerStatus->dock_serial_number, init_serial_number,
	        NYX_DOCK_SERIAL_NUMBER_LEN);
}
#endif

//
// Tests for the _charger_init API method
// nyx_error_t _charger_init(void)
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

#if 0
//
// Tests for the _charger_read_status API method
// nyx_error_t _charger_read_status(nyx_charger_status_t *status)
//
static void
test__charger_read_status(/*api_test_fixture *fixture, gconstpointer unused*/)
{
	nyx_charger_status_t testChargerStatus;
	resetTestChargerStatus(&testChargerStatus);

	// Check for no error
	g_assert_true(NYX_ERROR_NONE == _charger_read_status(&testChargerStatus));

	// For now, check to make sure values returned are different from our initialized test values
	g_assert_true(testChargerStatus.charger_max_current !=
	              init_charger_max_current);
	g_assert_true(testChargerStatus.connected != init_connected);
	g_assert_true(testChargerStatus.powered != init_powered);
	// can't check to see if is_charging changed since it's a "bool"
	//g_assert_true(testChargerStatus.is_charging != init_is_charging);
	g_assert_true(0 != strncmp(testChargerStatus.dock_serial_number,
	                           init_serial_number, NYX_DOCK_SERIAL_NUMBER_LEN));

	// Check to see if is_charging returns true when we claim to be connected to USB
	test_battery_sysfs_path_retval = 0;
	test_charger_usb_sysfs_path_retval = 1;
	test_charger_ac_sysfs_path_retval = 0;
	test_charger_touch_sysfs_path_retval = 0;
	test_charger_wireless_sysfs_path_retval = 0;
	resetTestChargerStatus(&testChargerStatus);
	// force is_charging status to false; make sure it returns true
	testChargerStatus.is_charging = false;
	g_assert_true(NYX_ERROR_NONE == _charger_read_status(&testChargerStatus));
	g_assert_true(true == testChargerStatus.is_charging);

	// Check to see if is_charging returns true when we claim to be connected to AC
	test_battery_sysfs_path_retval = 0;
	test_charger_usb_sysfs_path_retval = 0;
	test_charger_ac_sysfs_path_retval = 1;
	test_charger_touch_sysfs_path_retval = 0;
	test_charger_wireless_sysfs_path_retval = 0;
	resetTestChargerStatus(&testChargerStatus);
	// force is_charging status to false; make sure it returns true
	testChargerStatus.is_charging = false;
	g_assert_true(NYX_ERROR_NONE == _charger_read_status(&testChargerStatus));
	g_assert_true(true == testChargerStatus.is_charging);

	// Check to see if is_charging returns false when we claim to NOT be connected to AC or USB
	test_battery_sysfs_path_retval = 0;
	test_charger_usb_sysfs_path_retval = 0;
	test_charger_ac_sysfs_path_retval = 0;
	test_charger_touch_sysfs_path_retval = 0;
	test_charger_wireless_sysfs_path_retval = 0;
	// force is_charging status to true; make sure it returns false
	resetTestChargerStatus(&testChargerStatus);
	testChargerStatus.is_charging = 1;
	g_assert_true(NYX_ERROR_NONE == _charger_read_status(&testChargerStatus));
	g_assert_true(0 == testChargerStatus.is_charging);

	// NOTE: We don't bother passing NULL for status since status is checked in charger_read_status() in chargerlib.c
}

#endif

void reset_battery_path_retvals(void)
{
	test_batt_capacity_path_exists = false;
	test_batt_energy_now_path_exists = false;
	test_batt_energy_full_path_exists = false;
	test_batt_energy_full_design_path_exists = false;
	test_batt_charge_now_path_exists = false;
	test_batt_charge_full_path_exists = false;
	test_batt_charge_full_design_path_exists = false;
	test_batt_temperature_path_exists = false;
	test_batt_voltage_path_exists = false;
	test_batt_current_path_exists = false;
	test_batt_present_path_exists = false;
	test_batt_fake_battery_path_exists = false;

	test_batt_capacity_path_retval = -1;
	test_batt_energy_now_path_retval = -1;
	test_batt_energy_full_path_retval = -1;
	test_batt_energy_full_design_path_retval = -1;
	test_batt_charge_now_path_retval = -1;
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
	test_FileGetDouble_temp_result = -1;
	g_assert_true(-1 == battery_temperature(BATTERY_PRIMARY));

	//
	// temp is in tenths of a degree Celsius, and battery_temperature()
	// answers in whole degrees - which is what "temperature_C" and the
	// CTIA limits in battery.c have always meant by it.
	//
	reset_battery_path_retvals();
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = 333;
	g_assert_true(33 == battery_temperature(BATTERY_PRIMARY));

	// Rounded, not truncated
	reset_battery_path_retvals();
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = 296;
	g_assert_true(30 == battery_temperature(BATTERY_PRIMARY));

	// A battery at the CTIA shutdown limit, and one just under it
	reset_battery_path_retvals();
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = 600;
	g_assert_true(60 == battery_temperature(BATTERY_PRIMARY));

	reset_battery_path_retvals();
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
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = -50;
	g_assert_true(-5 == battery_temperature(BATTERY_PRIMARY));

	reset_battery_path_retvals();
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = -200;
	g_assert_true(-20 == battery_temperature(BATTERY_PRIMARY));

	// Rounding must not make a freezing battery look warmer than it is
	reset_battery_path_retvals();
	test_FileGetDouble_temp_result = 0;
	test_FileGetDouble_temp_retval = -55;
	g_assert_true(-6 == battery_temperature(BATTERY_PRIMARY));

	// Zero is a real reading, not an absent one
	reset_battery_path_retvals();
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
	test_FileGetDouble_result = -1;
	g_assert_true(-1 == battery_current(BATTERY_PRIMARY));

	// Check for correct return value while charging
	reset_battery_path_retvals();
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
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = -1543000;
	g_assert_true(-1543 == battery_current(BATTERY_PRIMARY));

	// Truncation toward zero on both sides: a current under a milliamp is
	// reported as none rather than rounding away from zero.
	reset_battery_path_retvals();
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = -600;
	g_assert_true(0 == battery_current(BATTERY_PRIMARY));

	// A parse failure must not be reported as a reading. FileGetDouble()
	// used to return success without storing anything, leaving the caller
	// to return whatever was on the stack.
	reset_battery_path_retvals();
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
	test_FileGetDouble_result = -1;
	g_assert_true(-1 == battery_avg_current(BATTERY_PRIMARY));

	reset_battery_path_retvals();
	test_FileGetDouble_result = 0;
	test_FileGetDouble_retval = 371870;
	// Milliamps, like battery_current() it delegates to
	g_assert_true(371 == battery_avg_current(BATTERY_PRIMARY));
	g_assert_true(battery_current(BATTERY_PRIMARY) == battery_avg_current(
	                  BATTERY_PRIMARY));

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
// Set-up GLib, then register and run the tests.
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/battery/device/battery_init", test_battery_init);
	// g_test_add_func("/battery/device/battery_deinit", test_battery_deinit);

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

	// not currently supported by device/battery.c or emulator/fake_battery.c (stub implementations)
	// g_test_add_func("/battery/device/battery_authenticate", test_battery_authenticate);
	// g_test_add_func("/battery/device/battery_set_wakeup_percent", test_battery_set_wakeup_percent);

	return g_test_run();
}
