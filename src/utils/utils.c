// Copyright (c) 2014-2018 LG Electronics, Inc.
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
* @file utils.c
*
* @brief Common methods to read values from a file, evaluate sysfs path, etc
*
*/

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <glib.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <nyx/module/nyx_log.h>
#include "msgid.h"

/* Overridable so a host-side test can point at a fixture instead. */
#ifndef POWER_SUPPLY_SYSFS_DIR
#define POWER_SUPPLY_SYSFS_DIR "/sys/class/power_supply/"
#endif

/**
 * Returns string in pre-allocated buffer.
 *
 * On failure the buffer is set to the empty string rather than left as it was.
 * Callers pass an uninitialised stack buffer and not all of them check the
 * return code, and a sysfs read can fail for reasons that have nothing to do
 * with the caller - a power_supply being unbound answers -ENODEV. Terminating
 * the buffer means the worst such a caller can do is compare against "".
 */

int FileGetString(const char *path, char *ret_string, size_t maxlen)
{
	GError *gerror = NULL;
	char *contents = NULL;
	gsize len;

	if (ret_string && maxlen > 0)
	{
		ret_string[0] = '\0';
	}

	if (!path || !ret_string || 0 == maxlen ||
	        !g_file_get_contents(path, &contents, &len, &gerror))
	{
		if (gerror)
		{
			nyx_error(MSGID_NYX_MOD_GET_STRING_ERR, 0, "error: %s", gerror->message);
			g_error_free(gerror);
		}

		return -1;
	}

	g_strstrip(contents);
	g_strlcpy(ret_string, contents, maxlen);
	g_free(contents);

	return 0;
}

int FileGetDouble(const char *path, double *ret_data)
{
	GError *gerror = NULL;
	char *contents = NULL;
	char *endptr;
	gsize len;
	float val;

	if (!path || !g_file_get_contents(path, &contents, &len, &gerror))
	{
		if (gerror)
		{
			nyx_error(MSGID_NYX_MOD_GET_DOUBLE_ERR, 0, "error: %s", gerror->message);
			g_error_free(gerror);
		}

		return -1;
	}

	val = strtod(contents, &endptr);

	if (endptr == contents)
	{
		nyx_error(MSGID_NYX_MOD_GET_STRTOD_ERR, 0, "Invalid input in %s.", path);
		goto end;
	}

	if (ret_data)
	{
		*ret_data = val;
	}

end:
	g_free(contents);
	return 0;
}

char *find_power_supply_sysfs_path(const char *device_type)
{
	GError *gerror = NULL;
	GDir *dir = NULL;
	GDir *subdir = NULL;
	gchar *dir_path = NULL;
	gchar *full_path = NULL;
	gchar *result = NULL;
	gchar *fallback = NULL;
	const char *sub_dir_name;
	const char *file_name;
	char file_contents[64];
	char base_dir[64] = POWER_SUPPLY_SYSFS_DIR;

	dir = g_dir_open(base_dir, 0, &gerror);

	if (gerror)
	{
		nyx_error(MSGID_NYX_MOD_SYSFS_ERR, 0, "error: %s", gerror->message);
		g_error_free(gerror);
		return NULL;
	}

	while ((sub_dir_name = g_dir_read_name(dir)) != 0)
	{
		// ignore hidden files
		if ('.' == sub_dir_name[0])
		{
			continue;
		}

		dir_path = g_build_filename(base_dir, sub_dir_name, NULL);

		if (g_file_test(dir_path, G_FILE_TEST_IS_DIR))
		{
			subdir = g_dir_open(dir_path, 0, &gerror);

			if (gerror)
			{
				nyx_error(MSGID_NYX_MOD_GET_DIR_ERR, 0, "error: %s", gerror->message);
				g_error_free(gerror);
				g_free(dir_path);
				g_dir_close(dir);
				return NULL;
			}

			while ((file_name = g_dir_read_name(subdir)) != 0)
			{
				if (strcmp(file_name, "type") == 0)
				{
					full_path = g_build_filename(dir_path, file_name, NULL);
					FileGetString(full_path, file_contents, 64);

					g_free(full_path);
					full_path = NULL;

					/* Exact match, or any USB-family type (USB, USB_PD,
					 * USB_DCP, USB_CDP, ...) when looking for "USB". Some
					 * devices (e.g. Pixel 3a) expose the live charger as
					 * "USB_PD" rather than plain "USB". */
					if ((strcmp(file_contents, device_type) == 0) ||
					    (strcmp(device_type, "USB") == 0 &&
					     strncmp(file_contents, "USB", 3) == 0))
					{
						/* Several matching supplies can coexist (e.g. an
						 * offline "USB" node alongside an online "USB_PD"
						 * node); only the online one is the live charger.
						 * Prefer it, keeping the first match as a fallback. */
						gchar *online_path = g_build_filename(dir_path, "online", NULL);
						char online[8] = "";
						int online_ok = FileGetString(online_path, online, sizeof(online));
						g_free(online_path);

						if (online_ok == 0 && online[0] == '1')
						{
							result = dir_path;
							dir_path = NULL;
							g_free(fallback);
							g_dir_close(subdir);
							g_dir_close(dir);
							return result;
						}

						if (!fallback)
						{
							fallback = dir_path;
							dir_path = NULL;  /* keep as fallback; don't free below */
						}
						break;
					}
				}
			}

			g_dir_close(subdir);
			subdir = NULL;
		}

		g_free(dir_path);
		dir_path = NULL;
	}

	g_dir_close(dir);
	return fallback;
}

static gint compare_path_names(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(*(const char *const *) a, *(const char *const *) b);
}

/**
 * Collect every power_supply of a given type rather than the first one found.
 *
 * find_power_supply_sysfs_path() returns whichever entry g_dir_read_name()
 * happens to yield first, which is filesystem order and so is neither stable
 * across boots nor meaningful. That is tolerable while a device has exactly
 * one supply of each type and wrong as soon as it does not: a PinePhone (Pro)
 * in its keyboard has two of type "Battery", the phone's own and the
 * keyboard's, and picking between them by directory order picks at random.
 *
 * The result is sorted by node name so callers get the same list in the same
 * order every time. NULL when nothing matches; free with g_strfreev().
 */
char **find_power_supply_sysfs_paths(const char *device_type)
{
	GError *gerror = NULL;
	GDir *dir;
	const char *sub_dir_name;
	char file_contents[64];
	const char *base_dir = POWER_SUPPLY_SYSFS_DIR;
	GPtrArray *found;

	dir = g_dir_open(base_dir, 0, &gerror);

	if (gerror)
	{
		nyx_error(MSGID_NYX_MOD_SYSFS_ERR, 0, "error: %s", gerror->message);
		g_error_free(gerror);
		return NULL;
	}

	found = g_ptr_array_new();

	while ((sub_dir_name = g_dir_read_name(dir)) != NULL)
	{
		gchar *dir_path;
		gchar *type_path;

		// ignore hidden files
		if ('.' == sub_dir_name[0])
		{
			continue;
		}

		dir_path = g_build_filename(base_dir, sub_dir_name, NULL);

		if (!g_file_test(dir_path, G_FILE_TEST_IS_DIR))
		{
			g_free(dir_path);
			continue;
		}

		type_path = g_build_filename(dir_path, "type", NULL);
		file_contents[0] = '\0';

		if (0 == FileGetString(type_path, file_contents, sizeof(file_contents)) &&
		        0 == strcmp(file_contents, device_type))
		{
			g_ptr_array_add(found, dir_path);
		}
		else
		{
			g_free(dir_path);
		}

		g_free(type_path);
	}

	g_dir_close(dir);

	if (0 == found->len)
	{
		g_ptr_array_free(found, TRUE);
		return NULL;
	}

	g_ptr_array_sort(found, compare_path_names);
	g_ptr_array_add(found, NULL);

	return (char **) g_ptr_array_free(found, FALSE);
}
