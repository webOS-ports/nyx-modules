// Copyright (c) 2010-2018 LG Electronics, Inc.
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
****************************************************************
* @file rtc.c
*
* @brief Convenience functions to interact with the rtc driver.
***************************************************************
*/

#include <linux/rtc.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>
#include "wait.h"
//#include "debug.h"
#include "rtc.h"
#include "alarm.h"

#ifdef BUILD_FOR_DESKTOP
#define DEV_RTC_IMPLEMENTED 0
#else
#define DEV_RTC_IMPLEMENTED 1
#endif

/**
 * @addtogroup RTCAlarms
 * @{
 */

static int32_t rtc_fd = -1;

#define STD_ASCTIME_BUF_SIZE    26

#if DEV_RTC_IMPLEMENTED

/**
* @brief Convert struct rtc_time to struct tm
*/

static void
rtc_time_to_tm(struct rtc_time *rtc_time, struct tm *tm_time)
{
	g_return_if_fail(rtc_time && tm_time);

	memset(tm_time, 0, sizeof(struct tm));
	tm_time->tm_mon = rtc_time->tm_mon;
	tm_time->tm_mday = rtc_time->tm_mday;
	tm_time->tm_year = rtc_time->tm_year;
	tm_time->tm_hour = rtc_time->tm_hour;
	tm_time->tm_min  = rtc_time->tm_min;
	tm_time->tm_sec  = rtc_time->tm_sec;
	tm_time->tm_isdst = -1; /* not known */
}
#endif

/**
* @brief Convert struct tm to struct rtc_time
*/

static void
tm_to_rtc_time(struct tm *tm_time, struct rtc_time *rtc_time)
{
	g_return_if_fail(tm_time && rtc_time);

	memset(rtc_time, 0, sizeof(struct rtc_time));
	rtc_time->tm_mon = tm_time->tm_mon;
	rtc_time->tm_mday = tm_time->tm_mday;
	rtc_time->tm_year = tm_time->tm_year;
	rtc_time->tm_hour = tm_time->tm_hour;
	rtc_time->tm_min  = tm_time->tm_min;
	rtc_time->tm_sec  = tm_time->tm_sec;
	rtc_time->tm_isdst = -1;
}

/**
* @brief Convert struct tm to struct rtc_wkalrm
*/

static void
tm_to_rtc_wkalrm(struct tm *tm_time, struct rtc_wkalrm *alarm)
{
	g_return_if_fail(tm_time && alarm);
	tm_to_rtc_time(tm_time, &alarm->time);
	alarm->enabled = 1;
	alarm->pending = 0;
}

/**
 * @brief Open rtc device.
 *
 */
bool
rtc_open(void)
{
#if DEV_RTC_IMPLEMENTED

	if (rtc_fd >= 0)
	{
		return true;
	}

	rtc_fd = open("/dev/rtc", O_RDONLY);

	if (rtc_fd < 0)
	{
		int32_t err1 = errno;
		rtc_fd = open("/dev/rtc0", O_RDONLY);

		if (rtc_fd < 0)
		{
			g_critical("Could not open rtc driver. %d %d", err1, errno);
			//BUG();
			return false;
		}
	}

	/*
	 * A CLOCK_REALTIME_ALARM timer, armed alongside the RTC alarm by
	 * rtc_set_alarm_time(). Failing to get one is not fatal - the RTC alarm
	 * still works - so the return value is deliberately not checked here;
	 * alarm.c logs which clock it ended up with.
	 */
	wakeup_alarm_open();

	return true;
#else
	g_debug("Powerd RTC code disabled");
	return false;
#endif
}

#if DEV_RTC_IMPLEMENTED

static GIOChannel *rtc_channel = NULL;

/*
 * Consecutive failed reads of the RTC descriptor. A read that fails does not
 * drain it, so it stays ready and glib re-dispatches immediately: the same
 * spin the charger, battery and keys modules each had to be taught to avoid.
 * rtc_check_alarm() maintains this.
 */
static int rtc_read_errors = 0;
#define RTC_MAX_READ_ERRORS 16

/**
* @brief The callback function called when the rtc driver notifies an alarm expiry.
*/

static gboolean
rtc_event(GIOChannel *source, GIOCondition condition, gpointer ctx)
{
	RtcAlarmFunc func = (RtcAlarmFunc)ctx;

	if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
	{
		/*
		 * glib reports these whether or not they were asked for, and this
		 * watch only asked for G_IO_IN. Returning TRUE on a descriptor the
		 * kernel has hung up means being called again immediately, for ever.
		 */
		g_warning("rtc descriptor lost (condition 0x%x), dropping the alarm watch",
		          condition);
		rtc_channel = NULL;
		return G_SOURCE_REMOVE;
	}

	if (rtc_check_alarm())
	{
		func();
	}

	if (rtc_read_errors >= RTC_MAX_READ_ERRORS)
	{
		/*
		 * Ready to read and failing to read, over and over: the descriptor is
		 * never drained, so staying subscribed is a busy loop. Give it up and
		 * say so once; rtc_add_watch() can attach a fresh channel later.
		 */
		g_warning("rtc read failed %d times in a row, dropping the alarm watch",
		          rtc_read_errors);
		rtc_read_errors = 0;
		rtc_channel = NULL;
		return G_SOURCE_REMOVE;
	}

	return TRUE;
}
#endif

/**
* @brief Attach a watching.
*
*/

bool
rtc_add_watch(RtcAlarmFunc func)
{
#if DEV_RTC_IMPLEMENTED

	/*
	 * The wakeup timer reports expiries to the same callback. On a device whose
	 * RTC cannot be set - a Pixel 3a, say - it is the only one of the two that
	 * can fire at the right time.
	 */
	wakeup_alarm_set_callback((WakeupAlarmFunc) func);

	if (rtc_channel == NULL)
	{
		rtc_channel = g_io_channel_unix_new(rtc_fd);
		g_io_add_watch(rtc_channel, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
		               rtc_event, func);
		g_io_channel_unref(rtc_channel);
	}

	return true;
#else
	return false;
#endif
}

bool
rtc_clear_watch(void)
{
#if DEV_RTC_IMPLEMENTED

	wakeup_alarm_set_callback(NULL);

	if (rtc_channel)
	{
		g_io_channel_shutdown(rtc_channel, true, NULL);
		rtc_channel = NULL;
		rtc_close();
	}

	return true;
#else
	return false;
#endif
}

/**
* @brief Close rtc device.
*/
void
rtc_close(void)
{
	if (rtc_fd >= 0)
	{
		close(rtc_fd);
		rtc_fd = -1;
	}

	wakeup_alarm_close();
}

/**
* @brief Obtain an fd for use in poll() or select().
*/
int32_t
rtc_getfd(void)
{
	return rtc_fd;
}

/**
* @brief Read the RTC time from the rtc driver.
*/

bool
rtc_read(struct tm *tm_time)
{
#if DEV_RTC_IMPLEMENTED

	if (!tm_time)
	{
		return false;
	}

	struct rtc_time rtc_time;

	int32_t ret = ioctl(rtc_fd, RTC_RD_TIME, &rtc_time);

	if (ret < 0)
	{
		g_critical("RTC_RD_TIME ioctl %d", errno);
		//      BUG();
		return false;
	}

	rtc_time_to_tm(&rtc_time, tm_time);

	return true;
#else
	return false;
#endif
}

/**
* @brief Read the RTC time and convert it in time_t.
*/

time_t rtc_time(time_t *time)
{
	struct tm tm;
	time_t t;

	if (!rtc_read(&tm))
	{
#if DEV_RTC_IMPLEMENTED
		g_warning("%s: warning could not read time.\n", __FUNCTION__);
		//     BUG();
#endif
		return -1;
	}

	t = timegm(&tm);

	if (time)
	{
		*time = t;
	}

	return t;
}

/**
* @brief Sets an rtc alarm to fire.
*
* Alarm expiry will be floored at 2 seconds in the future
* (i.e. if expiry = now + 1, alarm will fire at now + 2).
*
* @param  expiry
*
* @retval
i*/


bool
rtc_set_alarm_time(time_t expiry)
{
	time_t now = 0;
	struct tm tm_time;
	struct rtc_wkalrm alarm;
	static time_t curr_expiry = 0;

	if (expiry == curr_expiry)
	{
		return true;
	}
	else
	{
		curr_expiry = expiry;
	}

	rtc_time(&now);

	if (expiry < now + 2)
	{
		g_debug("%s: expiry = now + 2", __FUNCTION__);
		expiry = now + 2;
	}

	/*
	 * Make sure we really wake up when in deep sleep. An RTC alarm alone is at
	 * the mercy of what the platform does with the RTC across suspend; a
	 * CLOCK_REALTIME_ALARM timer is the interface the kernel's alarmtimer
	 * framework guarantees for it. Arm both and let whichever fires first do
	 * the waking.
	 */
	wakeup_alarm_set(expiry);

	gmtime_r(&expiry, &tm_time);
	tm_to_rtc_wkalrm(&tm_time, &alarm);
	return rtc_set_alarm(&alarm);
}

/**
* @brief Set the next alarm in the rtc driver
*
* @retval TRUE is alarm was set successfully
*/


bool
rtc_set_alarm(struct rtc_wkalrm *alarm)
{
#if DEV_RTC_IMPLEMENTED

	if (!alarm)
	{
		return false;
	}

	int32_t ret = ioctl(rtc_fd, RTC_WKALM_SET, alarm);

	if (ret < 0)
	{
		g_critical("Could not set RTC_WKALM_SET %d", errno);

		if (errno == ENOTTY)
		{
			g_critical("Alarm IRQs not supported.");
		}

		return false;
	}

	return true;
#else
	return false;
#endif
}

/**
* @brief Read the current alarm expiry time.
*/

bool
rtc_read_alarm(struct rtc_wkalrm *alarm)
{
#if DEV_RTC_IMPLEMENTED

	if (!alarm)
	{
		return false;
	}

	int32_t ret = ioctl(rtc_fd, RTC_WKALM_RD, alarm);

	if (ret < 0)
	{
		g_critical("RTC_WKALM_RD ioctl %d", errno);

		alarm->enabled = false;
		//        BUG();
		return false;
	}

	return true;
#else
	return false;
#endif
}


/**
* @brief Read the current alarm expiry time in time_t.
*/

bool
rtc_read_alarm_time(time_t *time)
{
	struct rtc_wkalrm alarm;
	struct tm alarm_tm;

	if (!rtc_read_alarm(&alarm))
	{
		return false;
	}

	rtc_time_to_tm(&alarm.time, &alarm_tm);
	*time = timegm(&alarm_tm);
	return true;
}



/**
* @brief Clear the RTC alarm, if its set.
*/

bool
rtc_clear_alarm(void)
{
#if DEV_RTC_IMPLEMENTED
	int32_t ret;
	struct rtc_wkalrm alarm;

	rtc_read_alarm(&alarm);

	if (alarm.enabled)
	{
		g_debug("%s: clearing...", __FUNCTION__);

		alarm.enabled = false;
		ret = ioctl(rtc_fd, RTC_WKALM_SET, &alarm);

		if (ret < 0)
		{
			g_critical("Could not clear alarm %d", errno);

			/*
			 * NOV-92165: we can get an invalid value in the above rtc read so
			 * we specifically set a valid time here when disabling the alarm
			 */
			alarm.time.tm_sec = 0;
			alarm.time.tm_min = 29;
			alarm.time.tm_hour = 23;
			alarm.time.tm_mday = 14;
			alarm.time.tm_mon = 2;
			alarm.time.tm_year = 85;
			alarm.time.tm_wday = 4;
			alarm.time.tm_yday = 72;
			alarm.time.tm_isdst = 0;

			ret = ioctl(rtc_fd, RTC_WKALM_SET, &alarm);

			if (ret < 0)
			{
				g_critical("Could not clear alarm after explicit reset: %d", errno);
				goto error;
			}
		}
	}

	wakeup_alarm_clear();

	return true;
error:
	return false;
#else
	return false;
#endif
}

/**
* @brief returns true if alarm event was triggered.
*
* @retval
*/
bool
rtc_check_alarm(void)
{
#if DEV_RTC_IMPLEMENTED
	unsigned long data;
	ssize_t ret;

	ret = read(rtc_fd, &data, sizeof(data));

	if (ret < (ssize_t) sizeof(data))
	{
		/*
		 * Nothing was consumed, so the descriptor stays ready. rtc_event()
		 * gives the watch up once this has happened often enough in a row,
		 * rather than being re-dispatched on it for ever. A short read is
		 * counted the same way: acting on a partly-filled data word would be
		 * reading the alarm flag out of uninitialised bytes.
		 */
		rtc_read_errors++;
		return false;
	}

	rtc_read_errors = 0;

	if (data & RTC_AF)
	{
		rtc_clear_alarm();
		return true;
	}
	else
	{
		return false;
	}

#else
	return false;
#endif
}


/* @} END OF RTCAlarms */
