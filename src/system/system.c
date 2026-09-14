// Copyright (c) 2010-2021 LG Electronics, Inc.
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
************************************************
* @file system.c
************************************************
*/

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <glib.h>
#include "rtc.h"

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>
#include "msgid.h"

nyx_device_t *nyxDev;
nyx_device_callback_function_t alarm_fired_callback = NULL;
bool reformatted = false;

NYX_DECLARE_MODULE(NYX_DEVICE_SYSTEM, "System");

void AlarmFiredCB(void)
{
	if (alarm_fired_callback)
	{
		alarm_fired_callback(nyxDev, NYX_CALLBACK_STATUS_DONE, NULL);
	}
}

nyx_error_t nyx_module_open(nyx_instance_t i, nyx_device_t **d)
{

	if (NULL == d)
	{
		nyx_error(MSGID_NYX_MOD_SYSTEM_OPEN_ERR, 0, "System module already open.");
		return NYX_ERROR_INVALID_VALUE;
	}

	if (nyxDev)
	{
		return NYX_ERROR_NONE;
	}

	nyxDev = (nyx_device_t *)calloc(sizeof(nyx_device_t), 1);

	if (NULL == nyxDev)
	{
		nyx_error(MSGID_NYX_MOD_SYSTEM_OUT_OF_MEMORY, 0, "Out of memory");
		return NYX_ERROR_OUT_OF_MEMORY;
	}

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_SYSTEM_SET_ALARM_MODULE_METHOD,
	                           "system_set_alarm");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_SYSTEM_QUERY_NEXT_ALARM_MODULE_METHOD,
	                           "system_query_next_alarm");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_SYSTEM_QUERY_RTC_TIME_MODULE_METHOD,
	                           "system_query_rtc_time");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_SYSTEM_SUSPEND_MODULE_METHOD,
	                           "system_suspend");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_SYSTEM_SUSPEND_ASYNC_MODULE_METHOD,
	                           "system_suspend_async");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_SYSTEM_RESUME_MODULE_METHOD,
	                           "system_resume");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_SYSTEM_SHUTDOWN_MODULE_METHOD,
	                           "system_shutdown");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_SYSTEM_REBOOT_MODULE_METHOD,
	                           "system_reboot");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_SYSTEM_ERASE_PARTITION_MODULE_METHOD,
	                           "system_erase_partition");

	*d = (nyx_device_t *)nyxDev;
	return NYX_ERROR_NONE;
}

nyx_error_t nyx_module_close(nyx_device_t *d)
{
	rtc_close();
	if (nyxDev)
	{
		free(nyxDev);
		nyxDev = NULL;
	}
	return NYX_ERROR_NONE;
}

nyx_error_t system_set_alarm(nyx_device_handle_t handle, time_t time,
                             nyx_device_callback_function_t callback_func, void *context)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (rtc_open() == 0)
	{
		return NYX_ERROR_INVALID_OPERATION;
	}

	if (!time)
	{
		rtc_clear_alarm();
	}
	else
	{
		if (rtc_set_alarm_time(time) == 0)
		{
			return NYX_ERROR_INVALID_OPERATION;
		}

		if (callback_func)
		{
			alarm_fired_callback = callback_func;
			rtc_add_watch(AlarmFiredCB);
		}
		else
		{
			alarm_fired_callback = NULL;
			rtc_clear_watch();
		}
	}

	return NYX_ERROR_NONE;
}

nyx_error_t system_query_next_alarm(nyx_device_handle_t handle, time_t *time)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (rtc_open() == 0)
	{
		return NYX_ERROR_INVALID_OPERATION;
	}

	if (rtc_read_alarm_time(time) == false)
	{
		return NYX_ERROR_INVALID_OPERATION;
	}

	return NYX_ERROR_NONE;
}

nyx_error_t system_query_rtc_time(nyx_device_handle_t handle, time_t *time)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (rtc_open() == 0)
	{
		return NYX_ERROR_INVALID_OPERATION;
	}

	if (rtc_time(time) < 0)
	{
		return NYX_ERROR_INVALID_OPERATION;
	}

	return NYX_ERROR_NONE;
}


nyx_error_t system_suspend(nyx_device_handle_t handle, bool *success)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	int32_t ret = access("/usr/sbin/suspend_action", R_OK | X_OK);

	if (ret || (success == NULL))
	{
		/* dummy sleep function */
		sleep(5);
		ret = 0;
	}
	else
	{
		ret = system("/usr/sbin/suspend_action");
	}

	if (success)
	{
		*success = (ret == 0);
	}

	return NYX_ERROR_NONE;
}


static int write_sysfs_string(const char *path, const char *value)
{
	ssize_t written;
	int fd = open(path, O_WRONLY);

	if (fd < 0)
	{
		return -1;
	}

	written = write(fd, value, strlen(value));
	close(fd);

	return (written < 0) ? -1 : 0;
}

/*
 * sleepd only ever calls the async entry point - MachineSleep() in
 * src/pwrevents/machine.c goes straight to nyx_system_suspend_async() - so a
 * module that registers just NYX_SYSTEM_SUSPEND_MODULE_METHOD can never
 * suspend: the call returns NYX_ERROR_NOT_IMPLEMENTED (9), MachineSleep()
 * reports failure and the state machine aborts. Measured on a PinePhone Pro
 * before this method existed: sleepd ran a complete suspend cycle twice a
 * second forever, /sys/power/suspend_stats/success stayed at 0 across 25 hours
 * of uptime, and the battery drained at 33%/hour.
 *
 * Doing the work here rather than deferring to system_suspend() matters: that
 * path depends on an /usr/sbin/suspend_action script no image ships, and its
 * fallback is a sleep(5) that reports success without suspending anything -
 * which is exactly what a PinePhone Pro measured: sleepd believed it was
 * sleeping while /sys/power/suspend_stats/success stayed at 0.
 *
 * Writing "mem" to /sys/power/autosleep arms opportunistic suspend and
 * returns immediately - the kernel suspends as soon as no wakeup source is
 * held, retries on its own after every wake, and that is the asynchronous
 * contract sleepd's reworked state machine expects (the same semantics the
 * hybris module gets from libsuspend). system_resume() writes "off" so a
 * device sleepd wants awake stays awake; without it the kernel re-enters
 * suspend the moment the wakeup source that woke it is released. Kernels
 * without CONFIG_PM_AUTOSLEEP fall back to a blocking write of
 * /sys/power/state, which returns on resume; sleepd runs MachineSleep() on
 * its suspend thread, so blocking there is tolerable.
 */
nyx_error_t system_suspend_async(nyx_device_handle_t handle, bool *success)
{
	int ret;

	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	ret = write_sysfs_string("/sys/power/autosleep", "mem");

	if (ret < 0)
	{
		ret = write_sysfs_string("/sys/power/state", "mem");
	}

	if (success)
	{
		*success = (ret == 0);
	}

	return NYX_ERROR_NONE;
}

nyx_error_t system_resume(nyx_device_handle_t handle, bool *success)
{
	int ret;

	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	ret = write_sysfs_string("/sys/power/autosleep", "off");

	if (success)
	{
		*success = (ret == 0);
	}

	return NYX_ERROR_NONE;
}


nyx_error_t system_shutdown(nyx_device_handle_t handle ,
                            nyx_system_shutdown_type_t type, const char *reason)
{
	int ret = 0;

	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	switch (type)
	{
		case NYX_SYSTEM_EMERG_SHUTDOWN:
			ret = system("halt -f");
			break;

		case NYX_SYSTEM_NORMAL_SHUTDOWN:
		case NYX_SYSTEM_TEST_SHUTDOWN:
		default:
			ret = system("shutdown -h now");
			break;
	}

	if (ret != 0)
	{
		return NYX_ERROR_GENERIC;
	}

	return NYX_ERROR_NONE;
}


nyx_error_t system_reboot(nyx_device_handle_t handle ,
                          nyx_system_shutdown_type_t type, const char *reason)
{
	int ret = 0;

	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	switch (type)
	{
		case NYX_SYSTEM_EMERG_SHUTDOWN:
			ret = system("reboot -f");
			break;

		case NYX_SYSTEM_NORMAL_SHUTDOWN:
		case NYX_SYSTEM_TEST_SHUTDOWN:
		default:
			ret = system("reboot");
			break;
	}

	if (ret != 0)
	{
		return NYX_ERROR_GENERIC;
	}

	return NYX_ERROR_NONE;
}


nyx_error_t system_erase_partition(nyx_device_handle_t handle,
                                   nyx_system_erase_type_t type)
{
	return NYX_ERROR_NOT_IMPLEMENTED;
}
