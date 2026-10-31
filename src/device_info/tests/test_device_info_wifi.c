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
// interface's address (SIOCGIFHWADDR) only without one.

#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>

#undef nyx_error
#define nyx_error(m, args...) {}

// The record directory, and the owner a test run can create files as
static char *test_dir;
#define WIFI_HWADDR_DIR   test_dir
#define WIFI_HWADDR_OWNER geteuid()

// What SIOCGIFHWADDR on wlan0 answers
static unsigned char live_mac[6];
static unsigned short live_family;
static int live_fail;
static int ioctl_calls;
int test_ioctl(int fd, unsigned long request, ...);
#define ioctl test_ioctl

#ifndef WEBOS_INSTALL_EXECSTATEDIR
#define WEBOS_INSTALL_EXECSTATEDIR "/var/lib"
#endif

// Pull in the units under test
#include "../wifi_hwaddr.c"
#include "../device_info_generic.c"

#undef ioctl

int test_ioctl(int fd, unsigned long request, ...)
{
	struct ifreq *ifr;
	va_list ap;

	(void) fd;
	va_start(ap, request);
	ifr = va_arg(ap, struct ifreq *);
	va_end(ap);

	g_assert_true(request == SIOCGIFHWADDR);
	g_assert_cmpstr(ifr->ifr_name, ==, "wlan0");
	ioctl_calls++;
	if (live_fail)
		return -1;
	ifr->ifr_hwaddr.sa_family = live_family;
	memcpy(ifr->ifr_hwaddr.sa_data, live_mac, sizeof(live_mac));
	return 0;
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
	static const unsigned char mac[6] = { 0x52, 0x54, 0x00, 0xab, 0xcd, 0xef };

	set_record(NULL, 0);
	memcpy(live_mac, mac, sizeof(mac));
	live_family = ARPHRD_ETHER;
	live_fail = 0;
	ioctl_calls = 0;
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
	g_assert_cmpint(ioctl_calls, ==, 0);
}

static void test_no_record_uses_interface(void)
{
	reset();
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "52:54:00:AB:CD:EF");
	g_assert_cmpint(ioctl_calls, ==, 1);
}

static void test_bad_record_uses_interface(void)
{
	reset();
	set_record("02:11:22:33:44:55 9\n", 0644);
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "52:54:00:AB:CD:EF");

	reset();
	set_record("02:11:22:33:44:55 1\n", 0666);
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "52:54:00:AB:CD:EF");
}

static void test_record_then_none(void)
{
	reset();
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "52:54:00:AB:CD:EF");
	set_record("02:11:22:33:44:55 0\n", 0644);
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "02:11:22:33:44:55");
	set_record(NULL, 0);
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "52:54:00:AB:CD:EF");
}

static void test_nothing_found(void)
{
	reset();
	live_fail = 1;
	g_assert_cmpstr(wifi_addr(NYX_ERROR_DEVICE_UNAVAILABLE), ==, "");
}

static void test_bad_interface_addresses(void)
{
	static const unsigned char zero[6] = { 0 };
	static const unsigned char multicast[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };

	reset();
	live_family = ARPHRD_IEEE80211;
	g_assert_cmpstr(wifi_addr(NYX_ERROR_DEVICE_UNAVAILABLE), ==, "");

	reset();
	memcpy(live_mac, zero, sizeof(zero));
	g_assert_cmpstr(wifi_addr(NYX_ERROR_DEVICE_UNAVAILABLE), ==, "");

	reset();
	memcpy(live_mac, multicast, sizeof(multicast));
	g_assert_cmpstr(wifi_addr(NYX_ERROR_DEVICE_UNAVAILABLE), ==, "");

	reset();
	live_mac[0] = 0x02;
	g_assert_cmpstr(wifi_addr(NYX_ERROR_NONE), ==, "02:54:00:AB:CD:EF");
}

int main(int argc, char **argv)
{
	int ret;

	g_test_init(&argc, &argv, NULL);
	test_dir = g_dir_make_tmp("test-device-info-XXXXXX", NULL);
	g_assert_nonnull(test_dir);
	g_assert_true(chmod(test_dir, 0755) == 0);

	g_test_add_func("/device_info/wifi_addr/record_used", test_record_used);
	g_test_add_func("/device_info/wifi_addr/no_record_uses_interface",
	                test_no_record_uses_interface);
	g_test_add_func("/device_info/wifi_addr/bad_record_uses_interface",
	                test_bad_record_uses_interface);
	g_test_add_func("/device_info/wifi_addr/record_then_none", test_record_then_none);
	g_test_add_func("/device_info/wifi_addr/nothing_found", test_nothing_found);
	g_test_add_func("/device_info/wifi_addr/bad_interface_addresses",
	                test_bad_interface_addresses);
	ret = g_test_run();

	set_record(NULL, 0);
	g_rmdir(test_dir);
	g_free(test_dir);
	free((void *) dev.wifi_mac);
	return ret;
}
