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

//
// Tests for src/utils/utils.c, the file readers and the power_supply walk that
// every battery and charger reading goes through. It lives here rather than
// under src/utils because this is where the test build is already wired up.
//
// The walk is pointed at a fixture directory built in a temporary path, so
// these run the same on a build host with no batteries as on a device.
//

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

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

#include <nyx/nyx_module.h>

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

//
// The fixture directory is only known at run time, so the unit under test has
// to be told where to look. Both walks read POWER_SUPPLY_SYSFS_DIR, so give it
// something that expands to a variable rather than a literal.
//
static const char *test_power_supply_dir = "/nonexistent/power_supply/";
#define POWER_SUPPLY_SYSFS_DIR test_power_supply_dir

// Pull in the unit under test
#include "../../utils/utils.c"

//*****************************************************************************
//*****************************************************************************

static gchar *fixture_root = NULL;

static void write_file(const char *dir, const char *name, const char *contents)
{
	gchar *path = g_build_filename(dir, name, NULL);
	g_assert_true(g_file_set_contents(path, contents, -1, NULL));
	g_free(path);
}

static gchar *make_supply(const char *name, const char *type,
                          const char *online)
{
	gchar *dir = g_build_filename(fixture_root, name, NULL);

	g_assert_true(0 == g_mkdir_with_parents(dir, 0755));

	if (type)
	{
		write_file(dir, "type", type);
	}

	if (online)
	{
		write_file(dir, "online", online);
	}

	return dir;
}

//
// A power_supply whose "type" cannot be read. A supply being unbound answers
// -ENODEV, and the walk used to compare an uninitialised stack buffer against
// the wanted type - reading past it when it held no terminator.
//
static void make_unreadable_supply(const char *name)
{
	gchar *dir = make_supply(name, "Battery\n", NULL);
	gchar *path = g_build_filename(dir, "type", NULL);

	g_assert_true(0 == g_chmod(path, 0));

	g_free(path);
	g_free(dir);
}

static void fixture_setup(void)
{
	GError *error = NULL;

	fixture_root = g_dir_make_tmp("nyx-utils-test-XXXXXX", &error);
	g_assert_no_error(error);
	g_assert_nonnull(fixture_root);

	test_power_supply_dir = fixture_root;
}

static void rm_rf(const char *path)
{
	GDir *dir = g_dir_open(path, 0, NULL);
	const char *name;

	if (dir)
	{
		while ((name = g_dir_read_name(dir)) != NULL)
		{
			gchar *child = g_build_filename(path, name, NULL);

			if (g_file_test(child, G_FILE_TEST_IS_DIR))
			{
				g_chmod(child, 0755);
				rm_rf(child);
			}
			else
			{
				g_chmod(child, 0644);
				g_remove(child);
			}

			g_free(child);
		}

		g_dir_close(dir);
	}

	g_rmdir(path);
}

static void fixture_teardown(void)
{
	rm_rf(fixture_root);
	g_free(fixture_root);
	fixture_root = NULL;
	test_power_supply_dir = "/nonexistent/power_supply/";
}

//*****************************************************************************
//*****************************************************************************

//
// Tests for FileGetString()
// int FileGetString(const char *path, char *ret_string, size_t maxlen)
//
static void test_FileGetString(void)
{
	char buf[64];
	gchar *path;

	fixture_setup();

	//
	// A failed read must leave the caller with an empty string, not with
	// whatever was on the stack: callers pass uninitialised buffers and not
	// all of them check the return code.
	//
	memset(buf, 'A', sizeof(buf));
	g_assert_true(-1 == FileGetString("/nonexistent/type", buf, sizeof(buf)));
	g_assert_cmpstr(buf, ==, "");

	memset(buf, 'A', sizeof(buf));
	g_assert_true(-1 == FileGetString(NULL, buf, sizeof(buf)));
	g_assert_cmpstr(buf, ==, "");

	// Degenerate arguments are refused rather than written through
	g_assert_true(-1 == FileGetString("/nonexistent/type", NULL, sizeof(buf)));
	g_assert_true(-1 == FileGetString("/nonexistent/type", buf, 0));

	// A good read is stripped of its trailing newline
	path = g_build_filename(fixture_root, "value", NULL);
	g_assert_true(g_file_set_contents(path, "  Battery \n", -1, NULL));
	g_assert_true(0 == FileGetString(path, buf, sizeof(buf)));
	g_assert_cmpstr(buf, ==, "Battery");

	// A value longer than the buffer is truncated, and still terminated
	g_assert_true(g_file_set_contents(path,
	                                  "0123456789012345678901234567890123456789", -1, NULL));
	g_assert_true(0 == FileGetString(path, buf, 8));
	g_assert_cmpstr(buf, ==, "0123456");

	g_free(path);
	fixture_teardown();
}

//
// Tests for FileGetDouble()
// int FileGetDouble(const char *path, double *ret_data)
//
static void test_FileGetDouble(void)
{
	double value;
	gchar *path;

	fixture_setup();
	path = g_build_filename(fixture_root, "current_now", NULL);

	// A file that is not there is an error
	value = 4242;
	g_assert_true(-1 == FileGetDouble("/nonexistent/current_now", &value));
	g_assert_true(4242 == value);

	g_assert_true(-1 == FileGetDouble(NULL, &value));

	//
	// An unparseable file is an error too, and the out-parameter is left
	// alone. Reporting success without storing anything left callers
	// returning whatever their uninitialised local happened to hold - and a
	// positive value there reads as "charging".
	//
	value = 4242;
	g_assert_true(g_file_set_contents(path, "", -1, NULL));
	g_assert_true(-1 == FileGetDouble(path, &value));
	g_assert_true(4242 == value);

	value = 4242;
	g_assert_true(g_file_set_contents(path, "n/a\n", -1, NULL));
	g_assert_true(-1 == FileGetDouble(path, &value));
	g_assert_true(4242 == value);

	// current_now is signed: discharging must survive the trip
	value = 0;
	g_assert_true(g_file_set_contents(path, "-1543000\n", -1, NULL));
	g_assert_true(0 == FileGetDouble(path, &value));
	g_assert_true(-1543000 == value);

	value = 0;
	g_assert_true(g_file_set_contents(path, "371870\n", -1, NULL));
	g_assert_true(0 == FileGetDouble(path, &value));
	g_assert_true(371870 == value);

	// A NULL out-parameter is allowed, and must not be written through
	g_assert_true(0 == FileGetDouble(path, NULL));

	g_free(path);
	fixture_teardown();
}

//
// Tests for find_power_supply_sysfs_path()
// char *find_power_supply_sysfs_path(const char *device_type)
//
static void test_find_power_supply_sysfs_path(void)
{
	gchar *dir;
	char *found;

	fixture_setup();

	// Nothing there at all
	g_assert_null(find_power_supply_sysfs_path("Battery"));
	g_assert_null(find_power_supply_sysfs_path(NULL));

	//
	// One supply whose type cannot be read, which must be skipped rather
	// than ending the walk - and must not be compared against uninitialised
	// memory on the way past.
	//
	make_unreadable_supply("aa_broken");

	dir = make_supply("bb_battery", "Battery\n", NULL);
	found = find_power_supply_sysfs_path("Battery");
	g_assert_cmpstr(found, ==, dir);
	g_free(found);
	g_free(dir);

	// A supply of another type is not confused for the wanted one
	dir = make_supply("cc_mains", "Mains\n", "1\n");
	g_free(dir);
	found = find_power_supply_sysfs_path("Wireless");
	g_assert_null(found);

	//
	// USB matches the whole USB family, and an online supply is preferred
	// over an offline one: a device can expose an idle "USB" node alongside
	// the "USB_PD" node that is actually carrying the charge.
	//
	dir = make_supply("dd_usb", "USB\n", "0\n");
	g_free(dir);
	dir = make_supply("ee_usb_pd", "USB_PD\n", "1\n");
	found = find_power_supply_sysfs_path("USB");
	g_assert_cmpstr(found, ==, dir);
	g_free(found);
	g_free(dir);

	fixture_teardown();
}

//
// Tests for find_power_supply_sysfs_paths()
// char **find_power_supply_sysfs_paths(const char *device_type)
//
static void test_find_power_supply_sysfs_paths(void)
{
	gchar *first;
	gchar *second;
	char **found;

	fixture_setup();

	// Nothing there at all
	g_assert_null(find_power_supply_sysfs_paths("Battery"));
	g_assert_null(find_power_supply_sysfs_paths(NULL));

	make_unreadable_supply("aa_broken");
	g_assert_null(find_power_supply_sysfs_paths("Battery"));

	//
	// Every battery, in a stable order. Returning whichever one the
	// filesystem yielded first picked at random between the phone's own
	// battery and the one in its keyboard.
	//
	second = make_supply("zz_keyboard", "Battery\n", NULL);
	first = make_supply("bb_battery", "Battery\n", NULL);

	found = find_power_supply_sysfs_paths("Battery");
	g_assert_nonnull(found);
	g_assert_cmpstr(found[0], ==, first);
	g_assert_cmpstr(found[1], ==, second);
	g_assert_null(found[2]);
	g_strfreev(found);

	// Types are matched exactly here, unlike the USB-family match above
	g_assert_null(find_power_supply_sysfs_paths("Batt"));

	g_free(first);
	g_free(second);
	fixture_teardown();
}

//
// Set-up GLib, then register and run the tests.
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/utils/FileGetString", test_FileGetString);
	g_test_add_func("/utils/FileGetDouble", test_FileGetDouble);
	g_test_add_func("/utils/find_power_supply_sysfs_path",
	                test_find_power_supply_sysfs_path);
	g_test_add_func("/utils/find_power_supply_sysfs_paths",
	                test_find_power_supply_sysfs_paths);

	return g_test_run();
}
