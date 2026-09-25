// Copyright (c) 2026 LuneOS
// SPDX-License-Identifier: Apache-2.0

/*
 * nyx-test-system - drive the NYX_DEVICE_SYSTEM instance's RTC and alarm calls.
 *
 * Companion to the host-side tests in src/system/tests, which cover the alarm
 * layer and the RTC watch in isolation. This exercises the same paths through
 * the real module on a real device: the RTC clock, the alarm the module arms
 * (RTC alarm plus a CLOCK_REALTIME_ALARM timer), and the callback that fires
 * when it expires.
 *
 * Note it competes with sleepd, which owns alarms in a running system: both set
 * the same RTC alarm, so the last writer wins. Stop sleepd before drawing
 * conclusions from an alarm that did not fire, or accept that sleepd may have
 * moved it.
 */

#include <errno.h>
#include <glib.h>
#include <inttypes.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nyx/nyx_client.h>

static GMainLoop *loop;
static int alarm_fired;

static void usage(const char *prog)
{
	printf(
	    "Usage: %s <command> [args]\n"
	    "\n"
	    "Commands:\n"
	    "  rtc-time                 report the RTC's current time\n"
	    "  next-alarm               report the alarm the RTC currently holds\n"
	    "  set-alarm <+secs|epoch>  arm an alarm, '+30' for 30 seconds out\n"
	    "  clear-alarm              clear any alarm the RTC holds\n"
	    "  wait <secs>              arm nothing, wait for a callback that long\n"
	    "  watch <+secs>            arm an alarm and wait for it to fire\n"
	    "\n"
	    "Exit status is 0 on success, 1 on a nyx error, 2 on misuse, and for\n"
	    "'watch' 3 when the alarm did not fire within its window.\n",
	    prog);
}

static void print_time(const char *what, time_t t)
{
	struct tm tm_utc;
	char buf[64] = "";

	if (gmtime_r(&t, &tm_utc))
		strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm_utc);

	printf("%-12s %" PRId64 "  %s\n", what, (int64_t) t, buf);
}

/* Called from the module's glib context when the RTC alarm expires. */
static void alarm_callback(nyx_device_handle_t handle, nyx_callback_status_t status,
                           void *context)
{
	(void) handle;
	(void) context;

	alarm_fired++;
	printf("alarm fired (status %d) at %" PRId64 "\n", status,
	       (int64_t) time(NULL));

	if (loop)
		g_main_loop_quit(loop);
}

static gboolean on_timeout(gpointer data)
{
	(void) data;

	if (loop)
		g_main_loop_quit(loop);

	return G_SOURCE_REMOVE;
}

/* Run the caller's glib context so the module can deliver its callback. */
static void wait_for_alarm(int seconds)
{
	printf("waiting up to %ds for the alarm callback...\n", seconds);
	loop = g_main_loop_new(NULL, FALSE);
	g_timeout_add_seconds(seconds, on_timeout, NULL);
	g_main_loop_run(loop);
	g_main_loop_unref(loop);
	loop = NULL;
}

/* "+30" is 30 seconds from now; anything else is an absolute epoch. */
static int parse_when(const char *arg, time_t *out)
{
	char *end = NULL;
	long long v;

	if (!arg || !*arg)
		return -1;

	v = strtoll(arg[0] == '+' ? arg + 1 : arg, &end, 10);

	if (!end || *end)
		return -1;

	*out = (arg[0] == '+') ? time(NULL) + (time_t) v : (time_t) v;
	return 0;
}

int main(int argc, char **argv)
{
	nyx_device_handle_t dev = NULL;
	nyx_error_t err;
	const char *cmd;
	int rc = 0;

	if (argc < 2)
	{
		usage(basename(argv[0]));
		return 2;
	}

	cmd = argv[1];

	err = nyx_init();

	if (err != NYX_ERROR_NONE)
	{
		fprintf(stderr, "nyx_init: %d\n", err);
		return 1;
	}

	err = nyx_device_open(NYX_DEVICE_SYSTEM, "Main", &dev);

	if (err != NYX_ERROR_NONE || !dev)
	{
		fprintf(stderr, "nyx_device_open(NYX_DEVICE_SYSTEM, \"Main\"): %d\n", err);
		nyx_deinit();
		return 1;
	}

	if (!strcmp(cmd, "rtc-time"))
	{
		time_t t = 0;
		err = nyx_system_query_rtc_time(dev, &t);

		if (err == NYX_ERROR_NONE)
		{
			print_time("rtc-time", t);
			print_time("system", time(NULL));
		}
	}
	else if (!strcmp(cmd, "next-alarm"))
	{
		time_t t = 0;
		err = nyx_system_query_next_alarm(dev, &t);

		if (err == NYX_ERROR_NONE)
		{
			if (t == 0)
				printf("next-alarm   none\n");
			else
				print_time("next-alarm", t);
		}
	}
	else if (!strcmp(cmd, "set-alarm") || !strcmp(cmd, "watch"))
	{
		time_t when = 0;

		if (argc < 3 || parse_when(argv[2], &when) != 0)
		{
			usage(basename(argv[0]));
			rc = 2;
			goto out;
		}

		print_time("arming", when);
		err = nyx_system_set_alarm(dev, when, alarm_callback, NULL);

		if (err == NYX_ERROR_NONE && !strcmp(cmd, "watch"))
		{
			time_t now = time(NULL);
			int window = (int) (when > now ? when - now : 0) + 5;

			wait_for_alarm(window);

			if (!alarm_fired)
			{
				fprintf(stderr, "alarm did not fire within %ds\n", window);
				rc = 3;
			}
		}
	}
	else if (!strcmp(cmd, "clear-alarm"))
	{
		/* 0 is the documented way to clear whatever the RTC holds. */
		err = nyx_system_set_alarm(dev, 0, NULL, NULL);

		if (err == NYX_ERROR_NONE)
			printf("alarm cleared\n");
	}
	else if (!strcmp(cmd, "wait"))
	{
		if (argc < 3)
		{
			usage(basename(argv[0]));
			rc = 2;
			goto out;
		}

		wait_for_alarm(atoi(argv[2]));
		err = NYX_ERROR_NONE;
	}
	else
	{
		usage(basename(argv[0]));
		rc = 2;
		goto out;
	}

	if (err != NYX_ERROR_NONE)
	{
		fprintf(stderr, "%s: nyx error %d\n", cmd, err);
		rc = 1;
	}

out:
	nyx_device_close(dev);
	nyx_deinit();
	return rc;
}
