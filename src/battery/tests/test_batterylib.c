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

// As we never actually call into nyx-lib, our nyx-lib instance
// can be anything we want it to be.
static nyx_instance_t the_instance = "an instance";

//
// Mock out the nyx call to register device methods
//
nyx_error_t nyx_module_register_method(nyx_instance_t instance,
                                       nyx_device_t *device_in_ptr,
                                       module_method_t method,
                                       const char *symbol_str)
{
	// Make sure the correct instance value gets passed through to nyx
	//
	// Could also add some tests to ensure only 'known' methods are
	// registered etc.
	g_assert_true(instance == the_instance);
	return NYX_ERROR_NONE;
}

//*****************************************************************************
//*****************************************************************************

// Pull in the unit under test
#include "../batterylib.c"

//*****************************************************************************
//*****************************************************************************

// default test return values
int test_battery_percent_retval = 66;
// What the battery.c readers below stand in for hand back: degrees, mV and
// mA, not the raw microvolts and microamps their sysfs nodes carry.
int test_battery_temperature_retval = 38;
int test_battery_voltage_retval = 3928;
int test_battery_current_retval = 85;
int test_battery_avg_current_retval = 85;
double test_battery_full40_retval = 1150.000;
double test_battery_rawcoulomb_retval = 761.250;
double test_battery_coulomb_retval = 748.800;
double test_battery_age_retval = 99.21875;
bool test_battery_is_present_retval = true;

//
// The readings are taken per battery now, so every mock takes the index and
// the fixture pretends to have two: index 0 the built-in one and index 1 a
// detached keyboard, which is how a caller sees a battery that is in the list
// but not present.
//
#define TEST_BATTERY_COUNT 2
#define TEST_BATTERY_ABSENT 1

int test_battery_count_retval = TEST_BATTERY_COUNT;

// Mock the battery.c functions (from battery.h)
int battery_count(void)
{
	return test_battery_count_retval;
}

const char *battery_name(int index)
{
	return (0 == index) ? "battery" : "keyboard-battery";
}

const char *battery_role(int index)
{
	return (0 == index) ? "main" : "keyboard";
}

int battery_percent(int index)
{
	return test_battery_percent_retval;
}

int battery_temperature(int index)
{
	return test_battery_temperature_retval;
}

int battery_voltage(int index)
{
	return test_battery_voltage_retval;
}

int battery_current(int index)
{
	return test_battery_current_retval;
}

int battery_avg_current(int index)
{
	return test_battery_avg_current_retval;
}

double battery_full40(int index)
{
	return test_battery_full40_retval;
}

double battery_rawcoulomb(int index)
{
	return test_battery_rawcoulomb_retval;
}

double battery_coulomb(int index)
{
	return test_battery_coulomb_retval;
}

double battery_age(int index)
{
	return test_battery_age_retval;
}

bool battery_is_present(int index)
{
	if (TEST_BATTERY_ABSENT == index)
	{
		return false;
	}

	return test_battery_is_present_retval;
}

bool battery_is_authenticated(const char *pair_challenge,
                              const char *pair_response)
{
	/* not supported */
	return true;
}

bool battery_authenticate(void)
{
	return true;
}

void battery_set_wakeup_percent(int percentage)
{
	return;
}

//
// Fake ("pseudo") battery mode. Emulated targets have no battery at all and
// this is how they are given one, so it is a supported path, not dead code.
//
bool test_battery_fakemode = false;
nyx_error_t test_battery_get_fakemode_retval = NYX_ERROR_NONE;

void battery_set_fakemode(bool enable)
{
	test_battery_fakemode = enable;
}

nyx_error_t battery_get_fakemode(bool *enable)
{
	if (NYX_ERROR_NONE != test_battery_get_fakemode_retval)
	{
		return test_battery_get_fakemode_retval;
	}

	if (NULL == enable)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	*enable = test_battery_fakemode;
	return NYX_ERROR_NONE;
}

#define CHARGE_MIN_TEMPERATURE_C 0
#define CHARGE_MAX_TEMPERATURE_C 57
#define BATTERY_MAX_TEMPERATURE_C  60

nyx_battery_ctia_t battery_ctia_params;

nyx_battery_ctia_t *test_get_battery_ctia_params_retval = &battery_ctia_params;

nyx_battery_ctia_t *get_battery_ctia_params(void)
{
	// TODO: DON'T FORGET TO REMOVE THIS LEAK WHEN TESTING VALGRIND!!!
	//  nyx_battery_ctia_t *leak = malloc(sizeof(nyx_battery_ctia_t));

	battery_ctia_params.charge_min_temp_c = CHARGE_MIN_TEMPERATURE_C;
	battery_ctia_params.charge_max_temp_c = CHARGE_MAX_TEMPERATURE_C;
	battery_ctia_params.battery_crit_max_temp = BATTERY_MAX_TEMPERATURE_C;
	battery_ctia_params.skip_battery_authentication = true;

	return test_get_battery_ctia_params_retval;
}

nyx_error_t testBatteryInit_retval = NYX_ERROR_NONE;
nyx_error_t battery_init(void)
{
	return testBatteryInit_retval;
}


nyx_error_t battery_deinit(void)
{
	return NYX_ERROR_NONE;
}

// Mock the battery.c functions (from battery.h)

static bool init_present = false;
static bool init_charging = false;
static int32_t init_percentage = -1;
static int32_t init_temperature = -1;
static int32_t init_current = -1;
static int32_t init_voltage = -1;
static float init_capacity = -1.0;
static int32_t init_avg_current = -1;
static float init_capacity_raw = -1.0;
static float init_capacity_full40 = -1.0;
static int32_t init_age = -1;

static void resetTestBatteryStatus(nyx_battery_status_t *batteryStatus)
{
	g_assert_nonnull(batteryStatus);
	batteryStatus->present = init_present;
	batteryStatus->charging = init_charging;
	batteryStatus->percentage = init_percentage;
	batteryStatus->temperature = init_temperature;
	batteryStatus->current = init_current;
	batteryStatus->voltage = init_voltage;
	batteryStatus->capacity = init_capacity;
	batteryStatus->avg_current = init_avg_current;
	batteryStatus->capacity_raw = init_capacity_raw;
	batteryStatus->capacity_full40 = init_capacity_full40;
	batteryStatus->age = init_age;
}

//*****************************************************************************
//*****************************************************************************


//
// All tests for API methods need an opened device (and need to close
// it afterwards), so they should use the following fixture, along with the
// setup and teardown functions.
//
// For ease, these tests can be added using the ADD_APITEST macro
//

typedef struct
{
	nyx_device_t *fixture_device;
} api_test_fixture;

//
// Setup for an API test by opening the module and storing the returned device
// in the fixture.
//
static void api_test_setup(api_test_fixture *fixture, gconstpointer unused)
{
	nyxDev = (nyx_device_t *) NULL;
	testBatteryInit_retval = NYX_ERROR_NONE;
	fixture->fixture_device = NULL;
	g_assert_true(nyx_module_open(the_instance,
	                              &fixture->fixture_device) == NYX_ERROR_NONE);
	g_assert_nonnull(fixture->fixture_device);
}

//
//  Tear down an API test
//
static void api_test_teardown(api_test_fixture *fixture, gconstpointer unused)
{
	// Closing the module should never fail
	g_assert_true(nyx_module_close(fixture->fixture_device) == NYX_ERROR_NONE);
	fixture->fixture_device = NULL;
	nyxDev = (nyx_device_t *) NULL;
}

//
//  Add an API test using the fixture
//
#define ADD_APITEST(path, func) g_test_add(path, api_test_fixture, NULL, api_test_setup, func, api_test_teardown)

//*****************************************************************************
//*****************************************************************************

//
// Test the module-open method. Many of the tests are duplicated in the
// fixture setup and tear-down, but it's worth having a standalone test for
// the method.
//
static void test_module_open()
{
	nyx_device_t *test_device = NULL;

	// Force a failed open
	nyxDev = (nyx_device_t *) NULL;
	testBatteryInit_retval = NYX_ERROR_GENERIC;
	test_device = NULL;
	g_assert_true(nyx_module_open(the_instance, &test_device) != NYX_ERROR_NONE);
	g_assert_null(test_device);

	// Check what should be a successful open
	nyxDev = (nyx_device_t *) NULL;
	testBatteryInit_retval = NYX_ERROR_NONE;
	test_device = NULL;
	g_assert_true(nyx_module_open(the_instance, &test_device) == NYX_ERROR_NONE);
	g_assert_nonnull(test_device);

	// Check re-opening which should return an error
	nyx_device_t *dupe_device = NULL;
	g_assert_true(nyx_module_open(the_instance, &dupe_device) != NYX_ERROR_NONE);
	g_assert_null(dupe_device);

	// Release the device datastructure (and zero our pointer and theirs)
	free(test_device);
	test_device = NULL;
	nyxDev = NULL;
}


//
// Test for the battery_query_battery_status API
// nyx_error_t battery_query_battery_status(nyx_device_handle_t handle, nyx_battery_status_t *status)
//
static void test_battery_query_battery_status(api_test_fixture *fixture,
        gconstpointer unused)
{
	nyx_battery_status_t testBatteryStatus;

	// Force a failed call
	resetTestBatteryStatus(&testBatteryStatus);
	g_assert_true(NYX_ERROR_INVALID_HANDLE == battery_query_battery_status(NULL,
	              &testBatteryStatus));

	// Force another failed call
	resetTestBatteryStatus(&testBatteryStatus);
	g_assert_true(NYX_ERROR_INVALID_VALUE == battery_query_battery_status(
	                  fixture->fixture_device, NULL));

	// Check for no error
	resetTestBatteryStatus(&testBatteryStatus);
	g_assert_true(NYX_ERROR_NONE == battery_query_battery_status(
	                  fixture->fixture_device, &testBatteryStatus));

	// Check to make sure values returned have changed:
	g_assert_true(testBatteryStatus.present != init_present);
	g_assert_true(testBatteryStatus.charging != init_charging);
	g_assert_true(testBatteryStatus.percentage != init_percentage);
	g_assert_true(testBatteryStatus.temperature != init_temperature);
	g_assert_true(testBatteryStatus.current != init_current);
	g_assert_true(testBatteryStatus.voltage != init_voltage);
	g_assert_true(testBatteryStatus.capacity != init_capacity);
	g_assert_true(testBatteryStatus.avg_current != init_avg_current);
	g_assert_true(testBatteryStatus.capacity_raw != init_capacity_raw);
	g_assert_true(testBatteryStatus.capacity_full40 != init_capacity_full40);
	g_assert_true(testBatteryStatus.age != init_age);
}


//typedef void (*nyx_device_callback_function_t)(nyx_device_handle_t, nyx_callback_status_t, void *);
void test_nyx_device_callback_function(nyx_device_handle_t device,
                                       nyx_callback_status_t status, void *context)
{
	return;
}

//
// Test for the battery_register_battery_status_callback API
// nyx_error_t battery_register_battery_status_callback (nyx_device_handle_t handle, nyx_device_callback_function_t callback_func, void *context)
//
static void test_battery_register_battery_status_callback(
    api_test_fixture *fixture, gconstpointer unused)
{
	// Force a failed call
	g_assert_true(NYX_ERROR_INVALID_HANDLE ==
	              battery_register_battery_status_callback(NULL,
	                      test_nyx_device_callback_function, (void *)NULL));

	// Force another failed call
	g_assert_true(NYX_ERROR_INVALID_VALUE ==
	              battery_register_battery_status_callback(fixture->fixture_device, NULL,
	                      (void *)NULL));

	// Check for no error
	g_assert_true(NYX_ERROR_NONE == battery_register_battery_status_callback(
	                  fixture->fixture_device, test_nyx_device_callback_function, (void *)NULL));
}

//
// Test for the battery_authenticate_battery API
// nyx_error_t battery_authenticate_battery(nyx_device_handle_t batt_device, bool *result)
//
static void test_battery_authenticate_battery(api_test_fixture *fixture,
        gconstpointer unused)
{
	bool testResult;

	// Force a failed call
	testResult = false;
	g_assert_true(NYX_ERROR_INVALID_HANDLE == battery_authenticate_battery(NULL,
	              &testResult));

	// Force another failed call
	g_assert_true(NYX_ERROR_INVALID_VALUE == battery_authenticate_battery(
	                  fixture->fixture_device, NULL));

	// Check for no error
	testResult = false;
	g_assert_true(NYX_ERROR_NONE == battery_authenticate_battery(
	                  fixture->fixture_device, &testResult));
	g_assert_true(true == testResult);
}

//
// Test for the battery_get_ctia_parameters API
// nyx_error_t battery_get_ctia_parameters(nyx_device_handle_t handle, nyx_battery_ctia_t *param)
//
static void test_battery_get_ctia_parameters(api_test_fixture *fixture,
        gconstpointer unused)
{
	nyx_battery_ctia_t testParam;

	// Force a failed call
	memset(&testParam, 0, sizeof(nyx_battery_ctia_t));
	g_assert_true(NYX_ERROR_INVALID_HANDLE == battery_get_ctia_parameters(NULL,
	              &testParam));

	// Force another failed call
	g_assert_true(NYX_ERROR_INVALID_VALUE == battery_get_ctia_parameters(
	                  fixture->fixture_device, NULL));

	// Force another failed call
	memset(&testParam, 0, sizeof(nyx_battery_ctia_t));
	test_get_battery_ctia_params_retval = NULL;
	g_assert_true(NYX_ERROR_NONE != battery_get_ctia_parameters(
	                  fixture->fixture_device, &testParam));

	// Check for no error
	memset(&testParam, 0, sizeof(nyx_battery_ctia_t));
	test_get_battery_ctia_params_retval = &battery_ctia_params;
	g_assert_true(NYX_ERROR_NONE == battery_get_ctia_parameters(
	                  fixture->fixture_device, &testParam));
	g_assert_true(CHARGE_MIN_TEMPERATURE_C == testParam.charge_min_temp_c);
	g_assert_true(CHARGE_MAX_TEMPERATURE_C == testParam.charge_max_temp_c);
	g_assert_true(BATTERY_MAX_TEMPERATURE_C == testParam.battery_crit_max_temp);
	g_assert_true(true == testParam.skip_battery_authentication);
}

//
// Test for the battery_set_wakeup_percentage API
// nyx_error_t battery_set_wakeup_percentage(nyx_device_handle_t handle, int percentage)
//
static void test_battery_set_wakeup_percentage(api_test_fixture *fixture,
        gconstpointer unused)
{
	int testPercentage;

	// Force a failed call
	testPercentage = 0;
	g_assert_true(NYX_ERROR_INVALID_HANDLE == battery_set_wakeup_percentage(NULL,
	              testPercentage));

	// Force another failed call
	testPercentage = 101;
	g_assert_true(NYX_ERROR_INVALID_VALUE == battery_set_wakeup_percentage(
	                  fixture->fixture_device, testPercentage));

	// Force another failed call
	testPercentage = -1;
	g_assert_true(NYX_ERROR_INVALID_VALUE == battery_set_wakeup_percentage(
	                  fixture->fixture_device, testPercentage));

	// Check for no error
	testPercentage = 20;
	g_assert_true(NYX_ERROR_NONE == battery_set_wakeup_percentage(
	                  fixture->fixture_device, testPercentage));
}


//
// Test for the battery_query_battery_count API
// nyx_error_t battery_query_battery_count(nyx_device_handle_t handle, int32_t *count)
//
static void test_battery_query_battery_count(api_test_fixture *fixture,
        gconstpointer unused)
{
	int32_t testCount;

	// Force a failed call
	testCount = -1;
	g_assert_true(NYX_ERROR_INVALID_HANDLE == battery_query_battery_count(NULL,
	              &testCount));

	// Force another failed call
	g_assert_true(NYX_ERROR_INVALID_VALUE == battery_query_battery_count(
	                  fixture->fixture_device, NULL));

	// Check for no error
	testCount = -1;
	g_assert_true(NYX_ERROR_NONE == battery_query_battery_count(
	                  fixture->fixture_device, &testCount));
	g_assert_true(TEST_BATTERY_COUNT == testCount);

	// A detached battery keeps its slot, so the count does not shrink when one
	// goes away - that is the whole point of reporting it as not present.
	g_assert_true(false == battery_is_present(TEST_BATTERY_ABSENT));
	g_assert_true(NYX_ERROR_NONE == battery_query_battery_count(
	                  fixture->fixture_device, &testCount));
	g_assert_true(TEST_BATTERY_COUNT == testCount);
}

//
// Test for the battery_query_battery_info API
// nyx_error_t battery_query_battery_info(nyx_device_handle_t handle, int32_t index, nyx_battery_info_t *info)
//
static void test_battery_query_battery_info(api_test_fixture *fixture,
        gconstpointer unused)
{
	nyx_battery_info_t testInfo;

	// Force a failed call
	g_assert_true(NYX_ERROR_INVALID_HANDLE == battery_query_battery_info(NULL, 0,
	              &testInfo));

	// Force another failed call
	g_assert_true(NYX_ERROR_INVALID_VALUE == battery_query_battery_info(
	                  fixture->fixture_device, 0, NULL));

	// Indices outside the list are rejected rather than read
	g_assert_true(NYX_ERROR_VALUE_OUT_OF_RANGE == battery_query_battery_info(
	                  fixture->fixture_device, -1, &testInfo));
	g_assert_true(NYX_ERROR_VALUE_OUT_OF_RANGE == battery_query_battery_info(
	                  fixture->fixture_device, TEST_BATTERY_COUNT, &testInfo));
	g_assert_true(NYX_ERROR_VALUE_OUT_OF_RANGE == battery_query_battery_info(
	                  fixture->fixture_device, G_MAXINT32, &testInfo));

	// The primary battery, which reports what battery_query_battery_status does
	memset(&testInfo, 0xff, sizeof(testInfo));
	g_assert_true(NYX_ERROR_NONE == battery_query_battery_info(
	                  fixture->fixture_device, BATTERY_PRIMARY, &testInfo));
	g_assert_cmpstr(testInfo.name, ==, battery_name(BATTERY_PRIMARY));
	g_assert_cmpstr(testInfo.role, ==, battery_role(BATTERY_PRIMARY));
	g_assert_true(true == testInfo.primary);
	g_assert_true(true == testInfo.status.present);
	g_assert_true(test_battery_percent_retval == testInfo.status.percentage);

	// The detached one: in the list, reported as not present, and with no
	// stale readings left in the struct behind that flag.
	memset(&testInfo, 0xff, sizeof(testInfo));
	g_assert_true(NYX_ERROR_NONE == battery_query_battery_info(
	                  fixture->fixture_device, TEST_BATTERY_ABSENT, &testInfo));
	g_assert_cmpstr(testInfo.name, ==, battery_name(TEST_BATTERY_ABSENT));
	g_assert_cmpstr(testInfo.role, ==, battery_role(TEST_BATTERY_ABSENT));
	g_assert_false(testInfo.primary);
	g_assert_false(testInfo.status.present);
	g_assert_false(testInfo.status.charging);
	g_assert_true(0 == testInfo.status.percentage);
	g_assert_true(0 == testInfo.status.voltage);
}

//
// Test for the battery fake-mode APIs, which is how an emulated target with no
// battery at all is given one.
// nyx_error_t battery_set_fake_mode(nyx_device_handle_t handle, bool enable)
// nyx_error_t battery_get_fake_mode(nyx_device_handle_t handle, bool *enable)
//
static void test_battery_fake_mode(api_test_fixture *fixture,
                                   gconstpointer unused)
{
	bool testEnable;

	// Force failed calls
	g_assert_true(NYX_ERROR_INVALID_HANDLE == battery_set_fake_mode(NULL, true));
	g_assert_true(NYX_ERROR_INVALID_HANDLE == battery_get_fake_mode(NULL,
	              &testEnable));

	test_battery_get_fakemode_retval = NYX_ERROR_NONE;
	g_assert_true(NYX_ERROR_INVALID_VALUE == battery_get_fake_mode(
	                  fixture->fixture_device, NULL));

	// A node that cannot be read is an error, not a silent "not faking": the
	// out-parameter must not be touched on that path.
	testEnable = true;
	test_battery_get_fakemode_retval = NYX_ERROR_INVALID_VALUE;
	g_assert_true(NYX_ERROR_NONE != battery_get_fake_mode(fixture->fixture_device,
	              &testEnable));
	g_assert_true(true == testEnable);
	test_battery_get_fakemode_retval = NYX_ERROR_NONE;

	// Round-trip both ways
	g_assert_true(NYX_ERROR_NONE == battery_set_fake_mode(fixture->fixture_device,
	              true));
	testEnable = false;
	g_assert_true(NYX_ERROR_NONE == battery_get_fake_mode(fixture->fixture_device,
	              &testEnable));
	g_assert_true(true == testEnable);

	g_assert_true(NYX_ERROR_NONE == battery_set_fake_mode(fixture->fixture_device,
	              false));
	testEnable = true;
	g_assert_true(NYX_ERROR_NONE == battery_get_fake_mode(fixture->fixture_device,
	              &testEnable));
	g_assert_true(false == testEnable);
}

//
// Closing the device must forget the client's callback along with its context.
// Keeping them would hand a freed context to a client that has gone away the
// next time a udev event arrives.
//
static void test_callback_cleared_on_close()
{
	nyx_device_t *test_device = NULL;
	int context = 0;

	nyxDev = (nyx_device_t *) NULL;
	testBatteryInit_retval = NYX_ERROR_NONE;
	g_assert_true(nyx_module_open(the_instance, &test_device) == NYX_ERROR_NONE);

	g_assert_true(NYX_ERROR_NONE == battery_register_battery_status_callback(
	                  test_device, test_nyx_device_callback_function, &context));
	g_assert_nonnull(battery_callback);
	g_assert_true(&context == battery_callback_context);

	g_assert_true(nyx_module_close(test_device) == NYX_ERROR_NONE);
	g_assert_null(battery_callback);
	g_assert_null(battery_callback_context);

	// ... and a fresh open does not resurrect them either
	test_device = NULL;
	g_assert_true(nyx_module_open(the_instance, &test_device) == NYX_ERROR_NONE);
	g_assert_null(battery_callback);
	g_assert_null(battery_callback_context);
	g_assert_true(nyx_module_close(test_device) == NYX_ERROR_NONE);
}

//
// Set-up GLib, then register and run the tests.
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/battery/api/module_open", test_module_open);
	ADD_APITEST("/battery/api/battery_query_battery_status",
	            test_battery_query_battery_status);
	ADD_APITEST("/battery/api/battery_register_battery_status_callback",
	            test_battery_register_battery_status_callback);
	ADD_APITEST("/battery/api/battery_authenticate_battery",
	            test_battery_authenticate_battery);
	ADD_APITEST("/battery/api/battery_get_ctia_parameters",
	            test_battery_get_ctia_parameters);
	ADD_APITEST("/battery/api/battery_set_wakeup_percentage",
	            test_battery_set_wakeup_percentage);
	ADD_APITEST("/battery/api/battery_query_battery_count",
	            test_battery_query_battery_count);
	ADD_APITEST("/battery/api/battery_query_battery_info",
	            test_battery_query_battery_info);
	ADD_APITEST("/battery/api/battery_fake_mode", test_battery_fake_mode);
	g_test_add_func("/battery/api/callback_cleared_on_close",
	                test_callback_cleared_on_close);

	return g_test_run();
}
