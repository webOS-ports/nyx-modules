// Copyright (c) 2014 Simon Busch <morphis@gravedo.de>
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

/*
*************************************************************************
* @file alarm.c
*
* @brief Wakeup alarms backed by the kernel's alarmtimer framework.
*
* This used to drive the Android alarm-dev character device (/dev/alarm).
* That driver was removed from upstream Linux in 3.10 and vendor trees
* diverge even at the same version - a Pixel 3a 4.9 still ships /dev/alarm,
* a Mi A1 4.9 does not - so on half our devices every call here failed with
* ENOENT and then EBADF on an fd that stayed -1.
*
* timerfd_create(CLOCK_REALTIME_ALARM) is the in-kernel replacement, and the
* one AOSP itself moved to. It has been available since Linux 3.11, so it is
* a single path that works on both the old and the new devices.
*
* With /dev/alarm gone from the picture nothing here is Android-specific any
* more, which is why this file now lives in nyx-modules rather than in
* nyx-modules-hybris: one copy, built for every machine, instead of a fork per
* platform. The functions kept their shape but lost the android_ prefix, which
* only ever described the driver they used to talk to.
*
* Two things come out of that timer. Waking the system is the one it was added
* for: an expiring CLOCK_REALTIME_ALARM brings the device out of suspend, which
* is what the old ANDROID_ALARM_SET(RTC_WAKEUP) ioctl did, and rtc.c arms the
* RTC alongside it to "make sure we really wake up when in deep sleep".
*
* The second is the expiry itself. This descriptor used to be armed and never
* read, so the only thing that could tell a caller its alarm had fired was the
* RTC's own interrupt - and on a device whose RTC cannot be set, that never
* happens. A Pixel 3a is exactly that device: pm660.dtsi carries
* qcom,qpnp-rtc-write = <0>, so the driver registers read-only ops, RTC_SET_TIME
* returns EINVAL even for root, and the RTC sits at 1970 while the system clock
* is correct. qpnp_rtc_set_alarm() then compares the requested alarm against the
* RTC's own clock, finds 2026 comfortably in its future, and arms it for about
* fifty-six years' time.
*
* So the timer is watched as well as armed. CLOCK_REALTIME is right on such a
* device even when the RTC is not, which makes this the only path that can fire
* at the intended moment, and the callback it delivers is the same one rtc.c
* hands to rtc_add_watch().
*************************************************************************
*/

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>
#include <glib.h>
#include <nyx/nyx_module.h>
#include <nyx/module/nyx_log.h>
#include "msgid.h"
#include "alarm.h"

/*
 * Both are old enough to be everywhere we build, but a sufficiently ancient
 * libc header set can still be missing them while the kernel supports them.
 */
#ifndef CLOCK_REALTIME_ALARM
#define CLOCK_REALTIME_ALARM 8
#endif

#ifndef TFD_CLOEXEC
#define TFD_CLOEXEC O_CLOEXEC
#endif

/**
 * @addtogroup RTCAlarms
 * @{
 */

static int32_t alarm_fd = -1;
static guint alarm_watch = 0;
static WakeupAlarmFunc alarm_fired_func = NULL;

/*
 * Reads of the timer that produced nothing. A ready timerfd always has eight
 * bytes to give, so this should never move; it is here because returning TRUE
 * from a glib watch on a descriptor that cannot be drained is the spin this tree
 * has already had to fix in the charger, battery, keys and RTC paths.
 */
static int empty_reads = 0;
#define ALARM_MAX_EMPTY_READS 16

/*
 * False when we had to fall back to a plain CLOCK_REALTIME timer, which still
 * expires correctly while the device is awake but cannot pull it out of
 * suspend. Kept so the distinction is visible in the log rather than silently
 * degrading a wakeup alarm into a normal one.
 */
static bool alarm_wakes_from_suspend = false;

static time_t curr_expiry = 0;

static bool wakeup_alarm_available(void)
{
	return alarm_fd >= 0;
}

void wakeup_alarm_set_callback(WakeupAlarmFunc func)
{
	alarm_fired_func = func;
}

/*
 * The timer expired. Drain it - a timerfd stays readable until it is read - and
 * hand the expiry on.
 *
 * Where the RTC works, rtc_clear_alarm() disarms this timer before delivering
 * its own callback, so only one of the two is reported. The other order can
 * still deliver twice, here and from the RTC interrupt a moment later; consumers
 * re-read alarm state when called, so that is harmless.
 */
static gboolean _alarm_expired(GIOChannel *channel, GIOCondition condition,
                               gpointer data)
{
	uint64_t ticks = 0;
	ssize_t rd;

	if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
	{
		g_warning("wakeup alarm timer lost (condition 0x%x), dropping the watch",
		          condition);
		alarm_watch = 0;
		return G_SOURCE_REMOVE;
	}

	rd = read(alarm_fd, &ticks, sizeof(ticks));

	if (rd < (ssize_t) sizeof(ticks))
	{
		if (++empty_reads < ALARM_MAX_EMPTY_READS)
			return TRUE;

		g_warning("wakeup alarm timer ready but unreadable %d times, dropping the watch",
		          empty_reads);
		empty_reads = 0;
		alarm_watch = 0;
		return G_SOURCE_REMOVE;
	}

	empty_reads = 0;
	curr_expiry = 0;

	nyx_debug("%s: alarm expired (%llu tick(s))", __FUNCTION__,
	          (unsigned long long) ticks);

	if (alarm_fired_func)
		alarm_fired_func();

	return TRUE;
}

/* Watch the timer, so an expiry is delivered and not merely slept through. */
static void _wakeup_alarm_watch(void)
{
	GIOChannel *channel;

	if (alarm_fd < 0 || alarm_watch != 0)
		return;

	channel = g_io_channel_unix_new(alarm_fd);

	if (!channel)
		return;

	/*
	 * The descriptor belongs to this file, not to the channel, so the channel
	 * must not close it when the watch drops its reference.
	 */
	g_io_channel_set_close_on_unref(channel, FALSE);
	alarm_watch = g_io_add_watch(channel,
	                             G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
	                             _alarm_expired, NULL);
	g_io_channel_unref(channel);
	empty_reads = 0;
}

/**
 * @brief Create the wakeup alarm timer.
 *
 */
bool wakeup_alarm_open(void)
{
	int alarm_errno;

	if (alarm_fd >= 0)
		return true;

	alarm_fd = timerfd_create(CLOCK_REALTIME_ALARM, TFD_CLOEXEC | TFD_NONBLOCK);
	if (alarm_fd >= 0) {
		alarm_wakes_from_suspend = true;
		_wakeup_alarm_watch();
		return true;
	}

	alarm_errno = errno;

	/*
	 * CLOCK_REALTIME_ALARM needs CAP_WAKE_ALARM and a kernel with the
	 * alarmtimer framework. Falling back keeps timed alarms working on a
	 * device that has neither; it just cannot wake it from suspend.
	 */
	alarm_fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
	if (alarm_fd < 0) {
		g_critical("Could not create alarm timer. %d (CLOCK_REALTIME_ALARM: %d)",
		           errno, alarm_errno);
		return false;
	}

	alarm_wakes_from_suspend = false;
	g_warning("CLOCK_REALTIME_ALARM unavailable (%d) - alarms will not wake the device from suspend",
	          alarm_errno);

	/* It cannot wake the device, but it does still expire, so it is still worth
	 * watching: the caller gets its callback at the right moment. */
	_wakeup_alarm_watch();

	return true;
}

/**
* @brief Destroy the wakeup alarm timer.
*/
void wakeup_alarm_close(void)
{
	if (alarm_watch != 0)
	{
		g_source_remove(alarm_watch);
		alarm_watch = 0;
	}

	if (alarm_fd >= 0)
	{
		close(alarm_fd);
		alarm_fd = -1;
	}

	alarm_wakes_from_suspend = false;
	curr_expiry = 0;
	empty_reads = 0;
}

/**
* @brief Read the current wall-clock time, broken down.
*
* The alarm timer runs on CLOCK_REALTIME, so that is the clock an expiry has
* to be expressed against.
*/

bool wakeup_alarm_read(struct tm *tm_time)
{
	struct timespec now;

	nyx_debug("%s", __FUNCTION__);

	if (!tm_time)
		return false;

	if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
		nyx_warn(MSGID_NYX_MOD_SYSTEM_ALARM_TIME_ERR, 0,
		         "clock_gettime(CLOCK_REALTIME) %d", errno);
		return false;
	}

	/*
	 * gmtime_r, not localtime_r: callers pair this with timegm(), so handing
	 * back a local-time breakdown made the round trip come out one UTC offset
	 * in the future. wakeup_alarm_set() then floored every expiry against
	 * that, so in a UTC+2 zone an alarm due within the next two hours was
	 * pushed out to roughly two hours away.
	 */
	if (gmtime_r(&now.tv_sec, tm_time) == NULL)
		return false;

	return true;
}

/**
* @brief Read the current wall-clock time as a time_t.
*/

time_t wakeup_alarm_time(time_t *time)
{
	struct timespec now;

	nyx_debug("%s", __FUNCTION__);

	if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
		nyx_warn(MSGID_NYX_MOD_SYSTEM_ALARM_TIME_ERR, 0,
		         "clock_gettime(CLOCK_REALTIME) %d", errno);
		return -1;
	}

	if (time)
		*time = now.tv_sec;

	return now.tv_sec;
}

/**
* @brief Arm the wakeup alarm.
*
* Alarm expiry will be floored at 2 seconds in the future
* (i.e. if expiry = now + 1, alarm will fire at now + 2).
*
* @param  expiry
*
* @retval
*/

bool wakeup_alarm_set(time_t expiry)
{
	struct itimerspec wakeup_time;
	time_t now = 0;

	nyx_debug("%s", __FUNCTION__);

	if (!wakeup_alarm_available())
		return false;

	if (expiry == curr_expiry)
		return true;

	if (wakeup_alarm_time(&now) < 0)
		return false;

	if (expiry < now + 2) {
		g_debug("%s: expiry = now + 2", __FUNCTION__);
		expiry = now + 2;
	}

	/* One-shot: it_interval stays zero. */
	memset(&wakeup_time, 0, sizeof(wakeup_time));
	wakeup_time.it_value.tv_sec = expiry;

	if (timerfd_settime(alarm_fd, TFD_TIMER_ABSTIME, &wakeup_time, NULL) != 0) {
		g_warning("Failed to set wakeup alarm at %ld (err %d)", (long) expiry, errno);
		return false;
	}

	curr_expiry = expiry;

	return true;
}

/**
* @brief Disarm the wakeup alarm, if it is set.
*/

bool wakeup_alarm_clear(void)
{
	struct itimerspec disarm;

	g_debug("%s: clearing...", __FUNCTION__);

	if (!wakeup_alarm_available())
		return false;

	/* An all-zero it_value disarms the timer. */
	memset(&disarm, 0, sizeof(disarm));

	if (timerfd_settime(alarm_fd, 0, &disarm, NULL) != 0) {
		g_warning("Failed to clear alarm (err %d)", errno);
		return false;
	}

	curr_expiry = 0;

	return true;
}

/* @} END OF RTCAlarms */
