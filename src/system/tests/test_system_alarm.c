// Copyright (c) 2026 LuneOS
// SPDX-License-Identifier: Apache-2.0

/*
 * Host-side tests for the wakeup alarm layer (alarm.c), which moved here from
 * nyx-modules-hybris once timerfd replaced /dev/alarm and nothing Android was
 * left in it.
 *
 * Everything here runs against a real timerfd in this process - no device, no
 * root. CLOCK_REALTIME_ALARM needs CAP_WAKE_ALARM, so on an unprivileged host
 * alarm.c falls back to CLOCK_REALTIME and warns; the tests detect which case
 * they are in rather than assuming, so they pass either way and still assert
 * that the distinction is reported.
 *
 * Build and run: src/system/tests/run-host-tests.sh
 */

#include <errno.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <nyx/nyx_module.h>

PmLogContext getNyxContext(void) { return NULL; }

#include "../alarm.c"

/* Can this process get a wakeup-capable timer at all? */
static bool host_has_alarm_clock(void)
{
	int fd = timerfd_create(CLOCK_REALTIME_ALARM, TFD_CLOEXEC);

	if (fd < 0)
		return false;

	close(fd);
	return true;
}

/*
 * Open, tolerating the fallback warning on a host without CAP_WAKE_ALARM. The
 * warning is asserted rather than silenced: degrading a wakeup alarm into a
 * plain one has to be visible.
 */
static void open_alarm(void)
{
	bool privileged = host_has_alarm_clock();

	if (!privileged)
		g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
		                      "CLOCK_REALTIME_ALARM unavailable*");

	g_assert_true(wakeup_alarm_open());

	if (!privileged)
		g_test_assert_expected_messages();

	g_assert_cmpint(alarm_wakes_from_suspend, ==, privileged);
}

/* Seconds until the armed timer fires, or -1 when it is disarmed. */
static long armed_in(void)
{
	struct itimerspec its;

	g_assert_cmpint(timerfd_gettime(alarm_fd, &its), ==, 0);

	if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0)
		return -1;

	return (long) its.it_value.tv_sec;
}

static void test_set_and_clear_without_open(void)
{
	wakeup_alarm_close();

	/* No timer, so nothing to arm - and no crash on the -1 descriptor. */
	g_assert_false(wakeup_alarm_set(time(NULL) + 60));
	g_assert_false(wakeup_alarm_clear());
}

static void test_open_is_idempotent(void)
{
	int first;

	wakeup_alarm_close();
	open_alarm();
	first = alarm_fd;
	g_assert_cmpint(first, >=, 0);

	/* A second open must not leak a descriptor or replace the timer. */
	g_assert_true(wakeup_alarm_open());
	g_assert_cmpint(alarm_fd, ==, first);

	wakeup_alarm_close();
	g_assert_cmpint(alarm_fd, ==, -1);
	g_assert_false(alarm_wakes_from_suspend);
}

static void test_arms_at_the_absolute_expiry(void)
{
	time_t now, expiry;
	long remaining;

	wakeup_alarm_close();
	open_alarm();

	now = time(NULL);
	expiry = now + 120;
	g_assert_true(wakeup_alarm_set(expiry));

	/*
	 * TFD_TIMER_ABSTIME was passed, so the timer is due at expiry; what
	 * timerfd_gettime reports is the remaining time, which is what makes the
	 * absolute/relative mix-up visible if it ever comes back.
	 */
	remaining = armed_in();
	g_assert_cmpint(remaining, >, 110);
	g_assert_cmpint(remaining, <=, 120);

	wakeup_alarm_close();
}

static void test_expiry_is_floored_two_seconds_out(void)
{
	time_t now;

	wakeup_alarm_close();
	open_alarm();

	now = time(NULL);

	/* In the past: still armed, floored to now + 2, never left unarmed. */
	g_assert_true(wakeup_alarm_set(now - 3600));
	g_assert_cmpint(armed_in(), >, 0);
	g_assert_cmpint(armed_in(), <=, 2);

	wakeup_alarm_close();
}

static void test_same_expiry_twice_is_a_no_op(void)
{
	time_t expiry;
	long first;

	wakeup_alarm_close();
	open_alarm();

	expiry = time(NULL) + 300;
	g_assert_true(wakeup_alarm_set(expiry));
	first = armed_in();

	/* Reports success without re-arming: the caller cannot tell, and the
	 * kernel is not asked twice for the same thing. */
	g_assert_true(wakeup_alarm_set(expiry));
	g_assert_cmpint(armed_in(), <=, first);
	g_assert_cmpint(armed_in(), >, first - 2);
	g_assert_cmpint(curr_expiry, ==, expiry);

	wakeup_alarm_close();
}

static void test_clear_disarms(void)
{
	wakeup_alarm_close();
	open_alarm();

	g_assert_true(wakeup_alarm_set(time(NULL) + 600));
	g_assert_cmpint(armed_in(), >, 0);

	g_assert_true(wakeup_alarm_clear());
	g_assert_cmpint(armed_in(), ==, -1);
	g_assert_cmpint(curr_expiry, ==, 0);

	/* Cleared, so the same expiry is a fresh request rather than a no-op. */
	g_assert_true(wakeup_alarm_set(time(NULL) + 600));
	g_assert_cmpint(armed_in(), >, 0);

	wakeup_alarm_close();
}

/*
 * The regression this file exists for as much as any: wakeup_alarm_read() hands
 * back a UTC breakdown because its callers pair it with timegm(). When it used
 * localtime_r the round trip came out one UTC offset in the future, and
 * wakeup_alarm_set() floored every expiry against that - so in a UTC+2 zone an
 * alarm due within the next two hours was pushed out to roughly two hours away.
 */
static void test_read_round_trips_through_timegm_in_any_zone(void)
{
	static const char * const zones[] = { "UTC", "Europe/Amsterdam",
	                                      "America/Los_Angeles",
	                                      "Australia/Sydney" };
	char *saved = g_strdup(g_getenv("TZ"));
	guint i;

	for (i = 0; i < G_N_ELEMENTS(zones); i++)
	{
		struct tm tm_time;
		time_t now, round;

		g_setenv("TZ", zones[i], TRUE);
		tzset();

		now = wakeup_alarm_time(NULL);
		g_assert_cmpint(now, >, 0);

		memset(&tm_time, 0, sizeof(tm_time));
		g_assert_true(wakeup_alarm_read(&tm_time));

		round = timegm(&tm_time);
		g_assert_cmpint(labs((long) (round - now)), <=, 1);
	}

	if (saved)
		g_setenv("TZ", saved, TRUE);
	else
		g_unsetenv("TZ");

	tzset();
	g_free(saved);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/system/alarm/set-and-clear-without-open",
	                test_set_and_clear_without_open);
	g_test_add_func("/system/alarm/open-is-idempotent",
	                test_open_is_idempotent);
	g_test_add_func("/system/alarm/arms-at-the-absolute-expiry",
	                test_arms_at_the_absolute_expiry);
	g_test_add_func("/system/alarm/expiry-is-floored-two-seconds-out",
	                test_expiry_is_floored_two_seconds_out);
	g_test_add_func("/system/alarm/same-expiry-twice-is-a-no-op",
	                test_same_expiry_twice_is_a_no_op);
	g_test_add_func("/system/alarm/clear-disarms", test_clear_disarms);
	g_test_add_func("/system/alarm/read-round-trips-in-any-zone",
	                test_read_round_trips_through_timegm_in_any_zone);
	return g_test_run();
}
