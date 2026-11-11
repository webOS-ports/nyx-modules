// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
// SPDX-License-Identifier: Apache-2.0

/*
 * Host-side test for led_torch's MediaTek /dev/flashlight backend. The ioctl
 * is replaced by a model of the flashlight core and a driver behind it:
 *
 *   dual     flashlights-mt6370.c (Titan Pocket): SET_ONOFF only records a
 *            channel's value; once every channel has one, the driver acts on
 *            them all and resets them to NONE. A lone channel returns 0, does
 *            nothing, and stays pending for the next caller.
 *   single   a one-channel driver: acts at once; the core rejects channel 2
 *            and up before the driver sees them.
 *
 * Build and run: src/led_torch/tests/run-host-tests.sh
 */

#include <errno.h>
#include <glib.h>
#include <stdbool.h>
#include <string.h>

#include <nyx/nyx_module.h>

struct mtk_flashlight_user_arg;
static int fake_ioctl(int fd, unsigned long cmd, struct mtk_flashlight_user_arg *arg);
#define TORCH_IOCTL fake_ioctl

#include "../led_torch.c"

PmLogContext getNyxContext(void) { return NULL; }

nyx_error_t nyx_module_register_method(nyx_instance_t i, nyx_device_t *d,
                                       module_method_t method, const char *name)
{
	(void) i;
	(void) d;
	(void) method;
	(void) name;
	return NYX_ERROR_NONE;
}

enum { NONE = -1, OFF = 0, ON = 1 };

static int channels;           /* how many the driver has: 1 or 2 */
static int pending[MTK_CT_MAX + 1];
static bool led_on;            /* what the hardware is doing */
static int driver_set;
static int refused;            /* calls the core turned away */
static GString *calls;         /* "ct:nr:arg " for every accepted call */

static int fake_ioctl(int fd, unsigned long cmd, struct mtk_flashlight_user_arg *a)
{
	(void) fd;

	if (a->type_id != 1 || a->ct_id < 1 || a->ct_id > channels)
	{
		refused++;
		errno = EINVAL;
		return -1;
	}

	g_string_append_printf(calls, "%d:%lu:%d ", a->ct_id, (unsigned long)_IOC_NR(cmd), a->arg);

	if (cmd == MTK_IOC_X_SET_DRIVER)
	{
		driver_set = a->arg;
	}
	else if (cmd == MTK_IOC_SET_ONOFF)
	{
		pending[a->ct_id] = a->arg ? ON : OFF;

		for (int ct = 1; ct <= channels; ct++)
			if (pending[ct] == NONE)
				return 0;      /* half a pair: recorded, nothing done */

		led_on = pending[MTK_CT_TORCH] == ON;
		for (int ct = 1; ct <= channels; ct++)
			pending[ct] = NONE;
	}

	return 0;
}

static void setup(int nchannels)
{
	channels = nchannels;
	for (int ct = 0; ct <= MTK_CT_MAX; ct++)
		pending[ct] = NONE;
	led_on = false;
	driver_set = 0;
	refused = 0;
	calls = g_string_new(NULL);
	mtk_fd = 3;            /* never used for real: the ioctl is faked */
	mtk_brightness = 0;
	mtk_channels = 0;
}

static void teardown(void)
{
	g_string_free(calls, TRUE);
	calls = NULL;
	mtk_fd = -1;
}

/* The Titan Pocket's MT6370, from power-up: on lights it. */
static void test_dual_on(void)
{
	setup(2);
	g_assert_true(mtk_set(100));
	g_assert_true(led_on);
	g_assert_cmpint(mtk_brightness, ==, 100);
	teardown();
}

/* And off puts it out - a lone channel-1 "off" used to stay pending. */
static void test_dual_off(void)
{
	setup(2);
	g_assert_true(mtk_set(100));
	g_assert_true(mtk_set(0));
	g_assert_false(led_on);
	g_assert_cmpint(driver_set, ==, 0);
	g_assert_cmpint(mtk_brightness, ==, 0);
	teardown();
}

/* Half a pair left by someone else, either channel: on still lights. */
static void test_stale_half_pair(void)
{
	for (int stale = 1; stale <= 2; stale++)
	{
		setup(2);
		pending[stale] = OFF;
		g_assert_true(mtk_set(100));
		g_assert_true(led_on);
		teardown();
	}
}

/* Over and over, the hardware follows every switch. */
static void test_dual_cycles(void)
{
	setup(2);
	for (int i = 0; i < 4; i++)
	{
		g_assert_true(mtk_set(100));
		g_assert_true(led_on);
		g_assert_true(mtk_set(0));
		g_assert_false(led_on);
	}
	teardown();
}

/* Ours goes first in every switch. */
static void test_ours_first(void)
{
	setup(2);
	g_assert_true(mtk_set(50));
	g_assert_nonnull(strstr(calls->str, "1:115:1 2:115:0 "));
	g_assert_true(mtk_set(0));
	g_assert_nonnull(strstr(calls->str, "1:115:0 2:115:0 "));
	teardown();
}

/* A one-channel driver: on and off, the core asked about channel 2 once. */
static void test_single_channel(void)
{
	setup(1);
	for (int i = 0; i < 3; i++)
	{
		g_assert_true(mtk_set(100));
		g_assert_true(led_on);
		g_assert_true(mtk_set(0));
		g_assert_false(led_on);
	}
	g_assert_cmpint(refused, ==, 1);
	g_assert_cmpint(mtk_channels, ==, 1);
	teardown();
}

/* The MT6370's two channels are learned once too: channel 3 asked once. */
static void test_dual_channels_learned(void)
{
	setup(2);
	for (int i = 0; i < 3; i++)
	{
		g_assert_true(mtk_set(100));
		g_assert_true(mtk_set(0));
	}
	g_assert_cmpint(refused, ==, 1);
	g_assert_cmpint(mtk_channels, ==, 2);
	teardown();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/torch/mtk/dual-on", test_dual_on);
	g_test_add_func("/torch/mtk/dual-off", test_dual_off);
	g_test_add_func("/torch/mtk/stale-half-pair", test_stale_half_pair);
	g_test_add_func("/torch/mtk/dual-cycles", test_dual_cycles);
	g_test_add_func("/torch/mtk/ours-first", test_ours_first);
	g_test_add_func("/torch/mtk/single-channel", test_single_channel);
	g_test_add_func("/torch/mtk/dual-channels-learned", test_dual_channels_learned);
	return g_test_run();
}
