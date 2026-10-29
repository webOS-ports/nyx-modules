// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
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

// NYX_DEVICE_INFO_WIFI_ADDR: connman's hardware address record first, the
// ifconfig output only without one.

#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>

#undef nyx_error
#define nyx_error(m, args...) {}

// The record directory, and the owner a test run can create files as
static char *test_dir;
#define WIFI_HWADDR_DIR   test_dir
#define WIFI_HWADDR_OWNER geteuid()

// What "ifconfig wlan0 | awk" prints, or NULL when it cannot run
static const char *ifconfig_output;
static int popen_calls;
FILE *test_popen(const char *command, const char *mode);
int test_pclose(FILE *fp);
#define popen  test_popen
#define pclose test_pclose

#ifndef WEBOS_INSTALL_EXECSTATEDIR
#define WEBOS_INSTALL_EXECSTATEDIR "/var/lib"
#endif

// Pull in the units under test
#include "../wifi_hwaddr.c"
#include "../device_info_generic.c"

#undef popen
#undef pclose

static char ifconfig_buf[64];

FILE *test_popen(const char *command, const char *mode)
{
	(void) mode;
	popen_calls++;
	g_assert_nonnull(strstr(command, "wlan0"));
	if (ifconfig_output == NULL)
		return NULL;
	g_strlcpy(ifconfig_buf, ifconfig_output, sizeof(ifconfig_buf));
	return fmemopen(ifconfig_buf, strlen(ifconfig_buf), "r");
}

int test_pclose(FILE *fp)
{
	return fclose(fp);
}

nyx_error_t nyx_module_register_method(nyx_instance_t i, nyx_device_t *d,
                                       module_method_t m, const char *s)
{
	(void) i; (void) d; (void) m; (void) s;
	return NYX_ERROR_NONE;
}

static device_info_device_t dev;

static void set_record(const char *content, mode_t mode)
{
	char *path = g_build_filename(test_dir, "wifi-hwaddr-wlan0", NULL);

	g_unlink(path);
	if (content)
	{
		int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
		g_assert_true(fd >= 0);
		g_assert_true(write(fd, content, strlen(content)) == (ssize_t) strlen(content));
		close(fd);
		g_assert_true(chmod(path, mode) == 0);
	}
	g_free(path);
}

static void reset(void)
{
	set_record(NULL, 0);
	ifconfig_output = "52:54:00:12:34:56\n";
	popen_calls = 0;
}

static const char *wifi_addr(nyx_error_t expected)
{
	const char *dest = NULL;

	g_assert_true(device_info_query((nyx_device_handle_t) &dev,
	                                NYX_DEVICE_INFO_WIFI_ADDR, &dest) == expected);
	g_assert_nonnull(dest);
	return dest;
}

static void test_record_used(void)
{
	reset();
	set_record("02:1a:2b:3c:4d:5e 1\n", 0644);
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "02:1A:2B:3C:4D:5E");
	g_assert_cmpint(popen_calls, ==, 0);
}

static void test_no_record_uses_ifconfig(void)
{
	reset();
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "52:54:00:12:34:56");
	g_assert_cmpint(popen_calls, ==, 1);
}

static void test_bad_record_uses_ifconfig(void)
{
	reset();
	set_record("02:11:22:33:44:55 9\n", 0644);
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "52:54:00:12:34:56");

	reset();
	set_record("02:11:22:33:44:55 1\n", 0666);
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "52:54:00:12:34:56");
}

static void test_record_then_none(void)
{
	reset();
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "52:54:00:12:34:56");
	set_record("02:11:22:33:44:55 0\n", 0644);
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "02:11:22:33:44:55");
	set_record(NULL, 0);
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "52:54:00:12:34:56");
}

static void test_nothing_found(void)
{
	reset();
	ifconfig_output = NULL;
	g_assert_cmpstr(wifi_addr(NYX_ERROR_DEVICE_UNAVAILABLE), ==, "");
}

int main(int argc, char **argv)
{
	int ret;

	g_test_init(&argc, &argv, NULL);
	test_dir = g_dir_make_tmp("test-device-info-XXXXXX", NULL);
	g_assert_nonnull(test_dir);
	g_assert_true(chmod(test_dir, 0755) == 0);

	g_test_add_func("/device_info/wifi_addr/record_used", test_record_used);
	g_test_add_func("/device_info/wifi_addr/no_record_uses_ifconfig",
	                test_no_record_uses_ifconfig);
	g_test_add_func("/device_info/wifi_addr/bad_record_uses_ifconfig",
	                test_bad_record_uses_ifconfig);
	g_test_add_func("/device_info/wifi_addr/record_then_none", test_record_then_none);
	g_test_add_func("/device_info/wifi_addr/nothing_found", test_nothing_found);
	ret = g_test_run();

	set_record(NULL, 0);
	g_rmdir(test_dir);
	g_free(test_dir);
	free((void *) dev.wifi_mac);
	return ret;
}
