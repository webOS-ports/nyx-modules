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

/**
 * @file batterylib.c
 *
 * @brief Interface for components talking to the battery module using NYX api.
 */

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>
#include <nyx/module/nyx_log.h>
#include "msgid.h"

#include "battery.h"

nyx_device_t *nyxDev = NULL;

void *battery_callback_context = NULL;
nyx_device_callback_function_t battery_callback = NULL;

NYX_DECLARE_MODULE(NYX_DEVICE_BATTERY, "Main");

nyx_error_t nyx_module_open(nyx_instance_t i, nyx_device_t **d)
{
	if (NULL == d)
	{
		nyx_error(MSGID_NYX_MOD_BATT_OPEN_ERR, 0, "Battery device  open error.");
		return NYX_ERROR_INVALID_VALUE;
	}

	*d = NULL;

	if (nyxDev)
	{
		nyx_error(MSGID_NYX_MOD_BATT_OPEN_ALREADY_ERR, 0,
		          "Battery device already open.");
		return NYX_ERROR_TOO_MANY_OPENS;
	}

	nyxDev = (nyx_device_t *)calloc(sizeof(nyx_device_t), 1);

	if (NULL == nyxDev)
	{
		nyx_error(MSGID_NYX_MOD_BATT_OUT_OF_MEMORY, 0, "Out of memory");
		return NYX_ERROR_OUT_OF_MEMORY;
	}

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_BATTERY_QUERY_BATTERY_STATUS_MODULE_METHOD,
	                           "battery_query_battery_status");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_BATTERY_REGISTER_BATTERY_STATUS_CALLBACK_MODULE_METHOD,
	                           "battery_register_battery_status_callback");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_BATTERY_AUTHENTICATE_BATTERY_MODULE_METHOD,
	                           "battery_authenticate_battery");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_BATTERY_GET_CTIA_PARAMETERS_MODULE_METHOD,
	                           "battery_get_ctia_parameters");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_BATTERY_SET_WAKEUP_PARAMETERS_MODULE_METHOD,
	                           "battery_set_wakeup_percentage");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_BATTERY_SET_FAKE_MODE_MODULE_METHOD,
	                           "battery_set_fake_mode");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_BATTERY_GET_FAKE_MODE_MODULE_METHOD,
	                           "battery_get_fake_mode");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_BATTERY_QUERY_BATTERY_COUNT_MODULE_METHOD,
	                           "battery_query_battery_count");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_BATTERY_QUERY_BATTERY_INFO_MODULE_METHOD,
	                           "battery_query_battery_info");

	nyx_error_t result = battery_init();

	if (NYX_ERROR_NONE != result)
	{
		free(nyxDev);
		nyxDev = NULL;
	}

	*d = (nyx_device_t *)nyxDev;
	return result;
}

nyx_error_t nyx_module_close(nyx_device_t *d)
{
	nyx_error_t result = NYX_ERROR_NONE;

	if (d == NULL)
	{
		result = NYX_ERROR_INVALID_VALUE;
	}

	if (NULL != nyxDev)
	{
		result = battery_deinit();
		free(nyxDev);
		nyxDev = NULL;
	}

	return result;
}

void battery_read_status_at(int index, nyx_battery_status_t *state)
{
	if (state)
	{
		memset(state, 0, sizeof(nyx_battery_status_t));

		state->present = battery_is_present(index);

		if (state->present)
		{
			state->percentage = battery_percent(index);
			state->temperature = battery_temperature(index);
			state->voltage = battery_voltage(index);
			state->current = battery_current(index);
			state->avg_current = battery_avg_current(index);
			state->capacity = battery_coulomb(index);
			state->capacity_raw = battery_rawcoulomb(index);
			state->capacity_full40 = battery_full40(index);
			state->age = battery_age(index);

			if (state->avg_current >  0)
			{
				state->charging = true;
			}
		}
		else
		{
			state->charging = false;
		}
	}
}

void battery_read_status(nyx_battery_status_t *state)
{
	battery_read_status_at(BATTERY_PRIMARY, state);
}

nyx_error_t battery_query_battery_status(nyx_device_handle_t handle,
        nyx_battery_status_t *status)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (!status)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	battery_read_status(status);

	return NYX_ERROR_NONE;
}

/**
 * @brief How many batteries this device has.
 *
 * Present or not: a detachable battery keeps its place in the list while it is
 * detached, reporting present = false, so that its absence is something the
 * caller is told about rather than something it has to infer from the list
 * getting shorter.
 */
nyx_error_t battery_query_battery_count(nyx_device_handle_t handle,
                                        int32_t *count)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (!count)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	*count = battery_count();

	return NYX_ERROR_NONE;
}

/**
 * @brief Identity and readings for one battery.
 *
 * Index 0 is the primary battery and reports exactly what
 * battery_query_battery_status() reports.
 */
nyx_error_t battery_query_battery_info(nyx_device_handle_t handle,
                                       int32_t index, nyx_battery_info_t *info)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (!info)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	if (index < 0 || index >= battery_count())
	{
		return NYX_ERROR_VALUE_OUT_OF_RANGE;
	}

	memset(info, 0, sizeof(nyx_battery_info_t));

	g_strlcpy(info->name, battery_name(index), sizeof(info->name));
	g_strlcpy(info->role, battery_role(index), sizeof(info->role));
	info->primary = (BATTERY_PRIMARY == index);

	battery_read_status_at(index, &info->status);

	return NYX_ERROR_NONE;
}

nyx_error_t battery_register_battery_status_callback(nyx_device_handle_t handle,
        nyx_device_callback_function_t callback_func, void *context)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (!callback_func)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	battery_callback_context = context;
	battery_callback = callback_func;

	return NYX_ERROR_NONE;
}

nyx_error_t battery_authenticate_battery(nyx_device_handle_t handle,
        bool *result)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (!result)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	*result = battery_authenticate();
	return NYX_ERROR_NONE;
}


nyx_error_t battery_get_ctia_parameters(nyx_device_handle_t handle,
                                        nyx_battery_ctia_t *param)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (!param)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	nyx_battery_ctia_t *battery_ctia = get_battery_ctia_params();

	if (!battery_ctia)
	{
		return NYX_ERROR_INVALID_OPERATION;
	}

	memcpy(param, battery_ctia, sizeof(nyx_battery_ctia_t));
	return NYX_ERROR_NONE;
}

nyx_error_t battery_set_wakeup_percentage(nyx_device_handle_t handle,
        int percentage)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (percentage < 0 || percentage > 100)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	battery_set_wakeup_percent(percentage);
	return NYX_ERROR_NONE;
}

nyx_error_t battery_set_fake_mode(nyx_device_handle_t handle,
                                  bool enable)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	battery_set_fakemode(enable);
	return NYX_ERROR_NONE;
}

nyx_error_t battery_get_fake_mode(nyx_device_handle_t handle,
                                  bool *enable)
{
	nyx_error_t err;

	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	err = battery_get_fakemode(enable);

	return err;
}
