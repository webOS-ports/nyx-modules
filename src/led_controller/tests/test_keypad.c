// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
// SPDX-License-Identifier: Apache-2.0

/*
 * Host-side test for led_controller's choice of keyboard backlight: the light
 * named by [module.led_controller] keypad_path in nyx.conf, else the lights
 * HAL's, else the first of keypad_leds[] in the LED class. Drives the module
 * against a temporary /sys/class/leds lookalike and nyx.conf, so nothing here
 * needs a device or root.
 *
 * Build and run: src/led_controller/tests/run-host-tests.sh
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>

PmLogContext getNyxContext(void) { return NULL; }

/* The module reads these through nyx_conf.h and its own macro. */
static char *test_leds = NULL;
static char *test_conf = NULL;
#define NYX_CONF_FILE test_conf
#define LEDS_CLASS_DIR test_leds

#include "../led_controller.c"

/* nyx-lib's reader, reimplemented so the test links without nyx-lib. */
int32_t nyx_utils_read_value(char *path)
{
	gchar *buf = NULL;
	int32_t v = -1;

	if (path && g_file_get_contents(path, &buf, NULL, NULL))
		v = (int32_t) strtol(buf, NULL, 10);

	g_free(buf);
	return v;
}

nyx_error_t nyx_module_register_method(nyx_instance_t i, nyx_device_t *d,
                                       module_method_t method, const char *name)
{
	(void) i;
	(void) d;
	(void) method;
	(void) name;
	return NYX_ERROR_NONE;
}

/* Effects are not exercised here; the module only needs to link. */
nyx_error_t nyx_led_controller_core_configuration_get_param(
    nyx_led_controller_core_configuration_handle_t handle,
    nyx_led_controller_parameter_type_t param, int32_t *value_out_ptr)
{
	(void) handle;
	(void) param;
	*value_out_ptr = 0;
	return NYX_ERROR_NONE;
}

static gchar *tmp_root;

static void led(const char *name, int max)
{
	gchar *dir = g_build_filename(test_leds, name, NULL);
	gchar *b = g_build_filename(dir, "brightness", NULL);
	gchar *m = g_build_filename(dir, "max_brightness", NULL);
	gchar *v = g_strdup_printf("%d\n", max);

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_assert_true(g_file_set_contents(b, "0\n", -1, NULL));
	g_assert_true(g_file_set_contents(m, v, -1, NULL));
	g_free(v);
	g_free(m);
	g_free(b);
	g_free(dir);
}

static gchar *led_dir(const char *name)
{
	return g_build_filename(test_leds, name, NULL);
}

static void conf(const char *keypad_path)
{
	gchar *text = g_strdup_printf("[module.led_controller]\nkeypad_path=%s\n",
	                              keypad_path ? keypad_path : "");

	g_assert_true(g_file_set_contents(test_conf, text, -1, NULL));
	g_free(text);
}

static int fake_close_calls;

static int fake_close(struct hw_device_t *dev)
{
	(void) dev;
	fake_close_calls++;
	return 0;
}

static void setup(void)
{

	tmp_root = g_dir_make_tmp("nyx-keypad-XXXXXX", NULL);
	g_assert_nonnull(tmp_root);
	test_leds = g_build_filename(tmp_root, "leds", NULL);
	test_conf = g_build_filename(tmp_root, "nyx.conf", NULL);
	g_assert_cmpint(g_mkdir_with_parents(test_leds, 0755), ==, 0);
	conf(NULL);
	keypad_device = NULL;
	fake_close_calls = 0;
}

static void teardown(void)
{
	gchar *argv[] = { "rm", "-rf", tmp_root, NULL };

	sysfs_light_release(&sysfs_keypad);
	g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
	                           NULL, NULL, NULL, NULL));
	g_free(test_conf);
	g_free(test_leds);
	g_free(tmp_root);
	test_conf = test_leds = tmp_root = NULL;
}

/* No configuration: the first of the known names. */
static void test_known_name(void)
{
	gchar *want;

	setup();
	led("button-backlight", 255);
	led("kbd_backlight", 3);
	g_assert_true(keypad_probe());
	want = g_build_filename(test_leds, "kbd_backlight", "brightness", NULL);
	g_assert_cmpstr(sysfs_keypad.brightness, ==, want);
	g_assert_cmpint(sysfs_keypad.max, ==, 3);
	g_free(want);
	teardown();
}

/* button-backlight alone is not taken for the keyboard unless configured. */
static void test_button_backlight_not_guessed(void)
{
	setup();
	led("button-backlight", 255);
	g_assert_false(keypad_probe());
	g_assert_null(sysfs_keypad.brightness);
	teardown();
}

/* Configured: that light, even beside a known name. */
static void test_configured_wins(void)
{
	gchar *dir, *want;

	setup();
	led("button-backlight", 255);
	led("keyboard-backlight", 10);
	dir = led_dir("button-backlight");
	conf(dir);
	g_assert_true(keypad_probe());
	want = g_build_filename(dir, "brightness", NULL);
	g_assert_cmpstr(sysfs_keypad.brightness, ==, want);
	g_assert_cmpint(sysfs_keypad.max, ==, 255);
	g_free(want);
	g_free(dir);
	teardown();
}

/* Configured: wins over the HAL's keyboard light, which is let go. */
static void test_configured_wins_over_hal(void)
{
	struct light_device_t *hal = g_new0(struct light_device_t, 1);
	gchar *dir;

	setup();
	hal->common.close = fake_close;
	keypad_device = hal;
	led("button-backlight", 255);
	dir = led_dir("button-backlight");
	conf(dir);
	g_assert_true(keypad_probe());
	g_assert_null(keypad_device);
	g_assert_cmpint(fake_close_calls, ==, 1);
	g_assert_nonnull(sysfs_keypad.brightness);
	g_free(dir);
	g_free(hal);
	teardown();
}

/* Not configured: the HAL keeps the keyboard light when it has one. */
static void test_hal_kept_without_config(void)
{
	struct light_device_t *hal = g_new0(struct light_device_t, 1);

	setup();
	hal->common.close = fake_close;
	keypad_device = hal;
	led("keyboard-backlight", 10);
	g_assert_true(keypad_probe());
	g_assert_true(keypad_device == hal);
	g_assert_cmpint(fake_close_calls, ==, 0);
	g_assert_null(sysfs_keypad.brightness);
	keypad_device = NULL;
	g_free(hal);
	teardown();
}

/* A configured path that is not there: fall back to the usual search. */
static void test_configured_missing_falls_back(void)
{
	gchar *dir, *want;

	setup();
	led("keyboard-backlight", 10);
	dir = led_dir("no-such-led");
	conf(dir);
	g_assert_true(keypad_probe());
	want = g_build_filename(test_leds, "keyboard-backlight", "brightness", NULL);
	g_assert_cmpstr(sysfs_keypad.brightness, ==, want);
	g_free(want);
	g_free(dir);
	teardown();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/keypad/known-name", test_known_name);
	g_test_add_func("/keypad/button-backlight-not-guessed", test_button_backlight_not_guessed);
	g_test_add_func("/keypad/configured-wins", test_configured_wins);
	g_test_add_func("/keypad/configured-wins-over-hal", test_configured_wins_over_hal);
	g_test_add_func("/keypad/hal-kept-without-config", test_hal_kept_without_config);
	g_test_add_func("/keypad/configured-missing-falls-back", test_configured_missing_falls_back);
	return g_test_run();
}
