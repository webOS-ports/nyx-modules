// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
// SPDX-License-Identifier: Apache-2.0

/*
 * Host-side tests for the pulse patterns of the timed_output / leds-class
 * vibrator: that a cancel stops them, whenever it comes, and that each one
 * plays the pulses it was asked for.
 *
 * The vibrator's sysfs file is a FIFO here. Each write the module makes to it
 * - the pulse length in milliseconds to switch on, "0" to switch off - lands
 * in the pipe, where the test reads it back. The pulse length is 60 ms, so
 * every '6' in what the module wrote is one switch-on. Nothing here needs a
 * device or root; the patterns run on a real GLib main loop, in real time.
 *
 * Build and run: src/haptics_timedoutput/tests/run-host-tests.sh
 */

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nyx/nyx_module.h>

PmLogContext getNyxContext(void) { return NULL; }

/* nyx-lib's, for nyx_module_open(); these tests do not open the module. */
nyx_error_t nyx_module_register_method(nyx_instance_t instance, nyx_device_t *device,
                                       module_method_t method, const char *symbol)
{
	return NYX_ERROR_NONE;
}

#include "../haptics.c"

/* The module splits a period into equal halves on and off. */
#define ON_MS   60
#define PERIOD  (2 * ON_MS)

static gchar *dir;
static int reader = -1;
static haptics_device_t *device;

static void setup(void)
{
	GError *error = NULL;

	dir = g_dir_make_tmp("haptics-test-XXXXXX", &error);
	g_assert_no_error(error);

	gchar *fifo = g_build_filename(dir, "enable", NULL);
	g_assert_cmpint(mkfifo(fifo, 0600), ==, 0);
	/* Held open for the whole test, so the module's open(O_WRONLY) never
	 * blocks waiting for a reader. */
	reader = open(fifo, O_RDONLY | O_NONBLOCK);
	g_assert_cmpint(reader, >=, 0);

	/* As nyx_module_open() leaves a timed_output vibrator. */
	device = g_new0(haptics_device_t, 1);
	device->path = fifo;
}

static void teardown(void)
{
	if (device) {
		clean_timeouts(device);
		g_free(device->path);
		g_free(device);
		device = NULL;
	}
	close(reader);
	reader = -1;

	gchar *fifo = g_build_filename(dir, "enable", NULL);
	g_unlink(fifo);
	g_free(fifo);
	g_rmdir(dir);
	g_free(dir);
	dir = NULL;
}

/* Everything the module wrote to the vibrator since the last call. */
static GString *drain(void)
{
	GString *s = g_string_new(NULL);
	char buf[256];
	ssize_t n;

	while ((n = read(reader, buf, sizeof(buf))) > 0)
		g_string_append_len(s, buf, n);
	g_assert_true(n == 0 || errno == EAGAIN);
	return s;
}

/* How many times the module switched the motor on since the last call. */
static guint switch_ons(void)
{
	GString *s = drain();
	guint n = 0;
	gsize i;

	for (i = 0; i < s->len; i++)
		n += s->str[i] == '6';
	g_string_free(s, TRUE);
	return n;
}

static gboolean quit(gpointer loop)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

/* Runs the default main context for ms milliseconds. */
static void run_for(guint ms)
{
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	g_timeout_add(ms, quit, loop);
	g_main_loop_run(loop);
	g_main_loop_unref(loop);
}

static nyx_haptics_configuration_t pulses_of_60(int pulses)
{
	nyx_haptics_configuration_t c;
	memset(&c, 0, sizeof(c));
	c.type = NYX_HAPTICS_EFFECT_UNDEFINED;
	c.period = PERIOD;
	c.duration = pulses * PERIOD;
	return c;
}

static guint pattern_ms(int pulses)
{
	return pulses * PERIOD;
}

static void assert_idle(void)
{
	g_assert_cmpuint(device->pulses, ==, 0);
	g_assert_cmpuint(device->smalltimeoutwatch, ==, 0);
	g_assert_cmpuint(device->fulltimeoutwatch, ==, 0);
}

static void test_pattern_plays_its_pulses(void)
{
	setup();
	g_assert_cmpint(haptics_vibrate((nyx_device_t *) device, pulses_of_60(3)), ==,
	                NYX_ERROR_NONE);
	run_for(pattern_ms(3) + 200);
	g_assert_cmpuint(switch_ons(), ==, 3);
	assert_idle();
	teardown();
}

/*
 * A pattern used to leave "on" set when it finished, as does a one-shot: the
 * next pattern then started in its off branch, dropped its first pulse and
 * opened with a gap.
 */
static void test_next_pattern_plays_all_of_its_pulses(void)
{
	nyx_haptics_configuration_t tap;

	setup();
	g_assert_cmpint(haptics_vibrate((nyx_device_t *) device, pulses_of_60(2)), ==,
	                NYX_ERROR_NONE);
	run_for(pattern_ms(2) + 200);
	g_assert_cmpuint(switch_ons(), ==, 2);

	g_assert_cmpint(haptics_vibrate((nyx_device_t *) device, pulses_of_60(3)), ==,
	                NYX_ERROR_NONE);
	run_for(pattern_ms(3) + 200);
	g_assert_cmpuint(switch_ons(), ==, 3);

	/* After a one-shot too. */
	memset(&tap, 0, sizeof(tap));
	tap.type = NYX_HAPTICS_EFFECT_TAPDOWN;
	g_assert_cmpint(haptics_vibrate((nyx_device_t *) device, tap), ==, NYX_ERROR_NONE);
	run_for(100);
	g_assert_cmpuint(switch_ons(), ==, 0);      /* tap: 50 ms, no '6' */
	g_assert_cmpint(haptics_vibrate((nyx_device_t *) device, pulses_of_60(2)), ==,
	                NYX_ERROR_NONE);
	run_for(pattern_ms(2) + 200);
	g_assert_cmpuint(switch_ons(), ==, 2);
	teardown();
}

/*
 * The pulse-on timer was never kept, so a cancel during a pulse could not
 * remove it. It fired after the cancel had zeroed the pulse count, switched
 * the motor on again, and the count wrapped from 0 to 4294967295 on the way
 * out: the pattern ran on, and nothing could stop it.
 */
static void cancel_after(guint ms)
{
	setup();
	g_assert_cmpint(haptics_vibrate((nyx_device_t *) device, pulses_of_60(1000)), ==,
	                NYX_ERROR_NONE);
	run_for(ms);
	g_assert_cmpint(haptics_cancel((nyx_device_t *) device, 1), ==, NYX_ERROR_NONE);
	assert_idle();
	g_string_free(drain(), TRUE);
	run_for(4 * PERIOD);
	g_assert_cmpuint(switch_ons(), ==, 0);
	assert_idle();
	teardown();
}

static void test_cancel_during_a_pulse_stops_the_pattern(void)
{
	cancel_after(ON_MS / 2);                    /* in the first pulse */
	cancel_after(2 * ON_MS + ON_MS / 2);        /* in the second */
}

static void test_cancel_between_pulses_stops_the_pattern(void)
{
	cancel_after(ON_MS + ON_MS / 2);            /* in the first gap */
}

static void test_cancel_all_during_a_pulse_stops_the_pattern(void)
{
	setup();
	g_assert_cmpint(haptics_vibrate((nyx_device_t *) device, pulses_of_60(1000)), ==,
	                NYX_ERROR_NONE);
	run_for(ON_MS / 2);
	g_assert_cmpint(haptics_cancel_all((nyx_device_t *) device), ==, NYX_ERROR_NONE);
	g_string_free(drain(), TRUE);
	run_for(4 * PERIOD);
	g_assert_cmpuint(switch_ons(), ==, 0);
	assert_idle();
	teardown();
}

/*
 * Each finished phase left its timer id behind, and the next cancel removed
 * that id again: "Source ID n was not found" from GLib, which a test makes
 * fatal.
 */
static void test_cancel_after_the_pattern_ended_is_quiet(void)
{
	setup();
	g_assert_cmpint(haptics_vibrate((nyx_device_t *) device, pulses_of_60(2)), ==,
	                NYX_ERROR_NONE);
	run_for(pattern_ms(2) + 200);
	g_assert_cmpint(haptics_cancel((nyx_device_t *) device, 1), ==, NYX_ERROR_NONE);
	assert_idle();
	teardown();
}

/*
 * A new pattern may start once a cancelled one is gone; it used to be refused
 * while the leaked count claimed one was still playing.
 */
static void test_pattern_after_a_cancel(void)
{
	setup();
	g_assert_cmpint(haptics_vibrate((nyx_device_t *) device, pulses_of_60(1000)), ==,
	                NYX_ERROR_NONE);
	run_for(ON_MS / 2);
	g_assert_cmpint(haptics_cancel((nyx_device_t *) device, 1), ==, NYX_ERROR_NONE);
	g_string_free(drain(), TRUE);
	g_assert_cmpint(haptics_vibrate((nyx_device_t *) device, pulses_of_60(2)), ==,
	                NYX_ERROR_NONE);
	run_for(pattern_ms(2) + 200);
	g_assert_cmpuint(switch_ons(), ==, 2);
	assert_idle();
	teardown();
}

/*
 * Closing frees the device. The pulse-on timer that clean_timeouts() could
 * not see outlived it and fired on freed memory, which ASan reports.
 */
static void test_close_during_a_pulse(void)
{
	setup();
	g_assert_cmpint(haptics_vibrate((nyx_device_t *) device, pulses_of_60(1000)), ==,
	                NYX_ERROR_NONE);
	run_for(ON_MS / 2);
	g_assert_cmpint(nyx_module_close((nyx_device_t *) device), ==, NYX_ERROR_NONE);
	device = NULL;
	g_string_free(drain(), TRUE);
	run_for(4 * PERIOD);
	g_assert_cmpuint(switch_ons(), ==, 0);
	teardown();
}

/*
 * Negative values: a negative period used to play as one buzz of the
 * duration, and a negative duration over a positive period went into the
 * unsigned pulse count. Refused, as the hybris module refuses them.
 */
static void test_negative_values_are_refused(void)
{
	nyx_haptics_configuration_t c;
	int bad[][2] = { { 200, -1000 }, { -1, 500 }, { -200, -200 }, { 200, -1 } };
	guint i;

	setup();
	for (i = 0; i < G_N_ELEMENTS(bad); i++)
	{
		memset(&c, 0, sizeof(c));
		c.type = NYX_HAPTICS_EFFECT_UNDEFINED;
		c.period = bad[i][0];
		c.duration = bad[i][1];
		g_assert_cmpint(haptics_vibrate((nyx_device_t *) device, c), ==,
		                NYX_ERROR_INVALID_VALUE);
	}
	run_for(100);
	GString *written = drain();
	g_assert_cmpuint(written->len, ==, 0);
	g_string_free(written, TRUE);
	assert_idle();
	teardown();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/haptics/pattern/plays-its-pulses", test_pattern_plays_its_pulses);
	g_test_add_func("/haptics/pattern/next-pattern-plays-all-of-its-pulses",
	                test_next_pattern_plays_all_of_its_pulses);
	g_test_add_func("/haptics/cancel/during-a-pulse", test_cancel_during_a_pulse_stops_the_pattern);
	g_test_add_func("/haptics/cancel/between-pulses", test_cancel_between_pulses_stops_the_pattern);
	g_test_add_func("/haptics/cancel/all-during-a-pulse",
	                test_cancel_all_during_a_pulse_stops_the_pattern);
	g_test_add_func("/haptics/cancel/after-the-pattern-ended",
	                test_cancel_after_the_pattern_ended_is_quiet);
	g_test_add_func("/haptics/cancel/then-a-new-pattern", test_pattern_after_a_cancel);
	g_test_add_func("/haptics/close/during-a-pulse", test_close_during_a_pulse);
	g_test_add_func("/haptics/vibrate/negative-values-refused", test_negative_values_are_refused);
	return g_test_run();
}
