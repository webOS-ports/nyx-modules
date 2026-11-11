// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
// SPDX-License-Identifier: Apache-2.0

/*
 * Host-side test for led_controller's notification LED: which nodes become the
 * red, green and blue channels ([module.led_controller] red_path, green_path,
 * blue_path in nyx.conf, else the names in core_leds[], with the lights HAL
 * kept only when nothing is configured), and what gets written to them -
 * the steady trigger a node needs ("cc_mode" where offered, else "none"), the
 * timer trigger for a blink, a dark channel of a showing colour held in that
 * mode, and "none" everywhere once the light is off. Drives the
 * module against a temporary /sys/class/leds lookalike and nyx.conf, so nothing
 * here needs a device or root.
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

/* Effects are driven through sysfs_core_set() directly here. */
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

static gchar *node_file(const char *name, const char *file)
{
	return g_build_filename(test_leds, name, file, NULL);
}

static void put(const char *name, const char *file, const char *text)
{
	gchar *p = node_file(name, file);

	g_assert_true(g_file_set_contents(p, text, -1, NULL));
	g_free(p);
}

static gchar *get(const char *name, const char *file)
{
	gchar *p = node_file(name, file);
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(p, &text, NULL, NULL));
	g_free(p);
	return g_strstrip(text);
}

static void assert_file(const char *name, const char *file, const char *want)
{
	gchar *text = get(name, file);

	g_assert_cmpstr(text, ==, want);
	g_free(text);
}

/* An LED node with the given max_brightness and trigger list. */
static void led(const char *name, int max, const char *triggers)
{
	gchar *dir = g_build_filename(test_leds, name, NULL);
	gchar *v = g_strdup_printf("%d\n", max);

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	put(name, "brightness", "0\n");
	put(name, "max_brightness", v);
	put(name, "trigger", triggers);
	g_free(v);
	g_free(dir);
}

/* nyx.conf naming the given channels; NULL leaves a key out. */
static void conf(const char *red, const char *green, const char *blue)
{
	GString *text = g_string_new("[module.led_controller]\n");
	const char *v[CHANNEL_COUNT] = { red, green, blue };
	int c;

	for (c = 0; c < CHANNEL_COUNT; c++)
	{
		if (v[c])
		{
			gchar *dir = g_build_filename(test_leds, v[c], NULL);

			g_string_append_printf(text, "%s=%s\n", core_conf_keys[c], dir);
			g_free(dir);
		}
	}

	g_assert_true(g_file_set_contents(test_conf, text->str, -1, NULL));
	g_string_free(text, TRUE);
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
	tmp_root = g_dir_make_tmp("nyx-core-XXXXXX", NULL);
	g_assert_nonnull(tmp_root);
	test_leds = g_build_filename(tmp_root, "leds", NULL);
	test_conf = g_build_filename(tmp_root, "nyx.conf", NULL);
	g_assert_cmpint(g_mkdir_with_parents(test_leds, 0755), ==, 0);
	conf(NULL, NULL, NULL);
	notifications_device = NULL;
	fake_close_calls = 0;
}

static void teardown(void)
{
	gchar *argv[] = { "rm", "-rf", tmp_root, NULL };
	int c;

	for (c = 0; c < CHANNEL_COUNT; c++)
		sysfs_light_release(&sysfs_core[c]);

	g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
	                           NULL, NULL, NULL, NULL));
	g_free(test_conf);
	g_free(test_leds);
	g_free(tmp_root);
	test_conf = test_leds = tmp_root = NULL;
}

static void assert_channel(int c, const char *name)
{
	gchar *want = node_file(name, "brightness");

	g_assert_cmpstr(sysfs_core[c].brightness, ==, want);
	g_free(want);
}

/* The MT6370 layout: four numbered nodes, red on the charge indicator. */
#define MTK_TRIGGERS "[none] timer cc_mode pwm_mode breath_mode\n"

static void mtk_leds(void)
{
	led("mt6370_pmu_led1", 6, MTK_TRIGGERS);
	led("mt6370_pmu_led2", 6, MTK_TRIGGERS);
	led("mt6370_pmu_led3", 6, MTK_TRIGGERS);
	led("mt6370_pmu_led4", 3, MTK_TRIGGERS);
}

/* No configuration: the known names, as before. */
static void test_names(void)
{
	setup();
	led("red", 255, "[none] timer\n");
	led("green:indicator", 1, "[none] timer\n");
	g_assert_true(core_probe());
	assert_channel(CHANNEL_RED, "red");
	assert_channel(CHANNEL_GREEN, "green:indicator");
	g_assert_null(sysfs_core[CHANNEL_BLUE].brightness);
	g_assert_cmpstr(sysfs_core[CHANNEL_RED].steady, ==, "none");
	teardown();
}

/* Numbered MediaTek nodes are not guessed. */
static void test_numbered_not_guessed(void)
{
	setup();
	mtk_leds();
	g_assert_false(core_probe());
	teardown();
}

/* Configured: those nodes, each with cc_mode as its steady trigger. */
static void test_configured(void)
{
	setup();
	mtk_leds();
	conf("mt6370_pmu_led4", "mt6370_pmu_led2", "mt6370_pmu_led3");
	g_assert_true(core_probe());
	assert_channel(CHANNEL_RED, "mt6370_pmu_led4");
	assert_channel(CHANNEL_GREEN, "mt6370_pmu_led2");
	assert_channel(CHANNEL_BLUE, "mt6370_pmu_led3");
	g_assert_cmpint(sysfs_core[CHANNEL_RED].max, ==, 3);
	g_assert_cmpstr(sysfs_core[CHANNEL_RED].steady, ==, "cc_mode");
	teardown();
}

/* A configured channel beside a named one: each found its own way. */
static void test_configured_and_names(void)
{
	setup();
	mtk_leds();
	led("green", 255, "[none]\n");
	conf("mt6370_pmu_led4", NULL, NULL);
	g_assert_true(core_probe());
	assert_channel(CHANNEL_RED, "mt6370_pmu_led4");
	assert_channel(CHANNEL_GREEN, "green");
	g_assert_null(sysfs_core[CHANNEL_BLUE].brightness);
	teardown();
}

/* A configured path that is not there: the names, as without one. */
static void test_configured_missing_falls_back(void)
{
	setup();
	led("red", 255, "[none]\n");
	conf("no-such-led", NULL, NULL);
	g_assert_true(core_probe());
	assert_channel(CHANNEL_RED, "red");
	teardown();
}

/* Configured: wins over the HAL's notification light, which is let go. */
static void test_configured_wins_over_hal(void)
{
	struct light_device_t *hal = g_new0(struct light_device_t, 1);

	setup();
	hal->common.close = fake_close;
	notifications_device = hal;
	mtk_leds();
	conf("mt6370_pmu_led4", NULL, NULL);
	g_assert_true(core_probe());
	g_assert_null(notifications_device);
	g_assert_cmpint(fake_close_calls, ==, 1);
	g_free(hal);
	teardown();
}

/* Not configured: the HAL keeps the light, and found names are let go. */
static void test_hal_kept_without_config(void)
{
	struct light_device_t *hal = g_new0(struct light_device_t, 1);

	setup();
	hal->common.close = fake_close;
	notifications_device = hal;
	led("red", 255, "[none]\n");
	g_assert_false(core_probe());
	g_assert_true(notifications_device == hal);
	g_assert_cmpint(fake_close_calls, ==, 0);
	g_assert_null(sysfs_core[CHANNEL_RED].brightness);
	notifications_device = NULL;
	g_free(hal);
	teardown();
}

/*
 * Steady colour: cc_mode on the lit channels, and on the dark one too, held
 * there with its brightness left unwritten (0 would drop the trigger).
 */
static void test_steady_uses_cc_mode(void)
{
	setup();
	mtk_leds();
	conf("mt6370_pmu_led4", "mt6370_pmu_led2", "mt6370_pmu_led3");
	g_assert_true(core_probe());
	put("mt6370_pmu_led3", "brightness", "untouched\n");
	g_assert_true(sysfs_core_set(255, 128, 0, 0, 0));
	assert_file("mt6370_pmu_led4", "trigger", "cc_mode");
	assert_file("mt6370_pmu_led4", "brightness", "3");
	assert_file("mt6370_pmu_led2", "trigger", "cc_mode");
	assert_file("mt6370_pmu_led2", "brightness", "3");
	assert_file("mt6370_pmu_led3", "trigger", "cc_mode");
	assert_file("mt6370_pmu_led3", "brightness", "untouched");
	teardown();
}

/* Green alone: red, the charge indicator, is held dark rather than released. */
static void test_green_holds_red(void)
{
	setup();
	mtk_leds();
	conf("mt6370_pmu_led4", "mt6370_pmu_led2", "mt6370_pmu_led3");
	g_assert_true(core_probe());
	put("mt6370_pmu_led4", "brightness", "untouched\n");
	g_assert_true(sysfs_core_set(0, 255, 0, 0, 0));
	assert_file("mt6370_pmu_led4", "trigger", "cc_mode");
	assert_file("mt6370_pmu_led4", "brightness", "untouched");
	assert_file("mt6370_pmu_led2", "brightness", "6");
	teardown();
}

/* A dark node without a mode trigger is still switched off. */
static void test_dark_plain_node(void)
{
	setup();
	led("red", 255, "[none] timer\n");
	led("green", 255, "[none] timer\n");
	g_assert_true(core_probe());
	put("red", "brightness", "255\n");
	g_assert_true(sysfs_core_set(0, 255, 0, 0, 0));
	assert_file("red", "trigger", "none");
	assert_file("red", "brightness", "0");
	assert_file("green", "brightness", "255");
	teardown();
}

/* Off: every channel back to no trigger and 0, red included. */
static void test_off_releases(void)
{
	setup();
	mtk_leds();
	conf("mt6370_pmu_led4", "mt6370_pmu_led2", "mt6370_pmu_led3");
	g_assert_true(core_probe());
	g_assert_true(sysfs_core_set(255, 255, 255, 0, 0));
	g_assert_true(sysfs_core_set(0, 0, 0, 0, 0));
	assert_file("mt6370_pmu_led4", "trigger", "none");
	assert_file("mt6370_pmu_led4", "brightness", "0");
	assert_file("mt6370_pmu_led2", "trigger", "none");
	assert_file("mt6370_pmu_led3", "trigger", "none");
	teardown();
}

/* A node without cc_mode keeps "none" for a steady colour. */
static void test_steady_plain_node(void)
{
	setup();
	led("red", 255, "[none] timer heartbeat\n");
	g_assert_true(core_probe());
	g_assert_true(sysfs_core_set(255, 0, 0, 0, 0));
	assert_file("red", "trigger", "none");
	assert_file("red", "brightness", "255");
	teardown();
}

/* cc_mode is matched as a word, not as part of another trigger's name. */
static void test_cc_mode_whole_word(void)
{
	setup();
	led("red", 255, "[none] notcc_mode cc_mode2\n");
	g_assert_true(core_probe());
	g_assert_cmpstr(sysfs_core[CHANNEL_RED].steady, ==, "none");
	teardown();
}

/* The active trigger is bracketed; still found. */
static void test_cc_mode_active(void)
{
	setup();
	led("red", 255, "none timer [cc_mode]\n");
	g_assert_true(core_probe());
	g_assert_cmpstr(sysfs_core[CHANNEL_RED].steady, ==, "cc_mode");
	teardown();
}

/* Blink: the timer on the lit channel, the dark ones held. */
static void test_blink(void)
{
	setup();
	mtk_leds();
	conf("mt6370_pmu_led4", "mt6370_pmu_led2", "mt6370_pmu_led3");
	g_assert_true(core_probe());
	g_assert_true(sysfs_core_set(0, 255, 0, 500, 1500));
	assert_file("mt6370_pmu_led2", "trigger", "timer");
	assert_file("mt6370_pmu_led2", "delay_on", "500");
	assert_file("mt6370_pmu_led2", "delay_off", "1500");
	assert_file("mt6370_pmu_led2", "brightness", "6");
	assert_file("mt6370_pmu_led4", "trigger", "cc_mode");
	teardown();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/core/names", test_names);
	g_test_add_func("/core/numbered-not-guessed", test_numbered_not_guessed);
	g_test_add_func("/core/configured", test_configured);
	g_test_add_func("/core/configured-and-names", test_configured_and_names);
	g_test_add_func("/core/configured-missing-falls-back", test_configured_missing_falls_back);
	g_test_add_func("/core/configured-wins-over-hal", test_configured_wins_over_hal);
	g_test_add_func("/core/hal-kept-without-config", test_hal_kept_without_config);
	g_test_add_func("/core/steady-uses-cc-mode", test_steady_uses_cc_mode);
	g_test_add_func("/core/green-holds-red", test_green_holds_red);
	g_test_add_func("/core/dark-plain-node", test_dark_plain_node);
	g_test_add_func("/core/off-releases", test_off_releases);
	g_test_add_func("/core/steady-plain-node", test_steady_plain_node);
	g_test_add_func("/core/cc-mode-whole-word", test_cc_mode_whole_word);
	g_test_add_func("/core/cc-mode-active", test_cc_mode_active);
	g_test_add_func("/core/blink", test_blink);
	return g_test_run();
}
