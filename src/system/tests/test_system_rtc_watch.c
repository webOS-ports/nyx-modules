// Copyright (c) 2026 LuneOS
// SPDX-License-Identifier: Apache-2.0

/*
 * Host-side tests for the RTC alarm watch in rtc.c - specifically that it gives
 * up rather than spinning.
 *
 * rtc_event() used to return TRUE for every dispatch. glib reports G_IO_HUP,
 * G_IO_ERR and G_IO_NVAL whether or not a watch asked for them, and a read that
 * consumes nothing leaves the descriptor ready, so both cases meant being
 * re-dispatched immediately on a descriptor that would never yield anything
 * again: one core gone and every other glib source in the process starved.
 *
 * The descriptor under test is a pipe, which is enough to reproduce all of it:
 * empty and non-blocking it fails every read, and a short write gives a partial
 * data word. Nothing here needs a device, an RTC, or root.
 *
 * Build and run: src/system/tests/run-host-tests.sh
 */

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <string.h>
#include <unistd.h>

#include <nyx/nyx_module.h>

PmLogContext getNyxContext(void) { return NULL; }

#include "../alarm.c"
#include "../rtc.c"

static int alarm_fired;

static void alarm_cb(void) { alarm_fired++; }

/* An empty non-blocking pipe stands in for an RTC that will not read. */
static int pipe_fds[2] = { -1, -1 };

static void fake_rtc_open(void)
{
	g_assert_cmpint(pipe(pipe_fds), ==, 0);
	g_assert_cmpint(fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK), ==, 0);
	rtc_fd = pipe_fds[0];
	rtc_read_errors = 0;
	rtc_channel = NULL;
	alarm_fired = 0;
}

static void fake_rtc_close(void)
{
	if (pipe_fds[0] >= 0) close(pipe_fds[0]);
	if (pipe_fds[1] >= 0) close(pipe_fds[1]);
	pipe_fds[0] = pipe_fds[1] = -1;
	rtc_fd = -1;
}

static void test_hangup_drops_the_watch(void)
{
	fake_rtc_open();

	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "rtc descriptor lost*");
	g_assert_cmpint(rtc_event(NULL, G_IO_HUP, (gpointer) alarm_cb), ==,
	                G_SOURCE_REMOVE);
	g_test_assert_expected_messages();

	/* Cleared so rtc_add_watch() can attach a fresh channel later. */
	g_assert_null(rtc_channel);
	g_assert_cmpint(alarm_fired, ==, 0);

	fake_rtc_close();
}

static void test_nval_and_err_drop_the_watch_too(void)
{
	GIOCondition conds[] = { G_IO_ERR, G_IO_NVAL, G_IO_IN | G_IO_HUP };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(conds); i++)
	{
		fake_rtc_open();
		g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "rtc descriptor lost*");
		g_assert_cmpint(rtc_event(NULL, conds[i], (gpointer) alarm_cb), ==,
		                G_SOURCE_REMOVE);
		g_test_assert_expected_messages();
		fake_rtc_close();
	}
}

static void test_unreadable_descriptor_gives_up(void)
{
	int i;

	fake_rtc_open();

	/*
	 * Readable as far as glib is concerned, unreadable in practice. Staying
	 * subscribed is tolerated for a while - a transient EAGAIN is not a reason
	 * to tear the watch down - and then given up.
	 */
	for (i = 1; i < RTC_MAX_READ_ERRORS; i++)
	{
		g_assert_cmpint(rtc_event(NULL, G_IO_IN, (gpointer) alarm_cb), ==,
		                G_SOURCE_CONTINUE);
		g_assert_cmpint(rtc_read_errors, ==, i);
	}

	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "rtc read failed*");
	g_assert_cmpint(rtc_event(NULL, G_IO_IN, (gpointer) alarm_cb), ==,
	                G_SOURCE_REMOVE);
	g_test_assert_expected_messages();

	/* The counter is reset with the watch, so a later watch starts clean. */
	g_assert_cmpint(rtc_read_errors, ==, 0);
	g_assert_null(rtc_channel);
	g_assert_cmpint(alarm_fired, ==, 0);

	fake_rtc_close();
}

static void test_short_read_counts_as_nothing_consumed(void)
{
	char partial[sizeof(unsigned long) - 1];

	fake_rtc_open();
	memset(partial, 0xff, sizeof(partial));

	/*
	 * A partial data word must not be acted on: the alarm flag would be read
	 * out of bytes the kernel never wrote. 0xff is deliberate - it has RTC_AF
	 * set, so a copy that trusted a short read would report an alarm here.
	 */
	g_assert_cmpint(write(pipe_fds[1], partial, sizeof(partial)), ==,
	                (gssize) sizeof(partial));
	g_assert_cmpint(rtc_event(NULL, G_IO_IN, (gpointer) alarm_cb), ==,
	                G_SOURCE_CONTINUE);
	g_assert_cmpint(rtc_read_errors, ==, 1);
	g_assert_cmpint(alarm_fired, ==, 0);

	fake_rtc_close();
}

static void test_full_read_without_the_alarm_flag_resets_the_counter(void)
{
	unsigned long data = 0;

	fake_rtc_open();
	rtc_read_errors = 5;

	/* A complete word with RTC_AF clear: a legitimate read, no alarm. */
	g_assert_cmpint(write(pipe_fds[1], &data, sizeof(data)), ==,
	                (gssize) sizeof(data));
	g_assert_cmpint(rtc_event(NULL, G_IO_IN, (gpointer) alarm_cb), ==,
	                G_SOURCE_CONTINUE);
	g_assert_cmpint(rtc_read_errors, ==, 0);
	g_assert_cmpint(alarm_fired, ==, 0);

	fake_rtc_close();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/system/rtc-watch/hangup-drops-the-watch",
	                test_hangup_drops_the_watch);
	g_test_add_func("/system/rtc-watch/err-and-nval-drop-it-too",
	                test_nval_and_err_drop_the_watch_too);
	g_test_add_func("/system/rtc-watch/unreadable-descriptor-gives-up",
	                test_unreadable_descriptor_gives_up);
	g_test_add_func("/system/rtc-watch/short-read-counts-as-nothing",
	                test_short_read_counts_as_nothing_consumed);
	g_test_add_func("/system/rtc-watch/full-read-resets-the-counter",
	                test_full_read_without_the_alarm_flag_resets_the_counter);
	return g_test_run();
}
