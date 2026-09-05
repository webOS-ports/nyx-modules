// Copyright (c) 2010-2018 LG Electronics, Inc.
// Copyright (c) 2016 Nikolay Nizov <nizovn@gmail.com>
// Copyright (c) 2018-2026 Herman van Hazendonk <github.com@herrie.org>
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
* @file msm.c
*
* Mass Storage Mode module backed by MTP (umtprd).
*
* Two USB gadget stacks are supported:
*  - legacy Android kernels exposing /sys/class/android_usb/android0
*  - configfs/libcomposite kernels exposing UDC state in /sys/class/udc
************************************************
*/

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>
#include <nyx/module/nyx_log.h>
#include "msgid.h"

#include <glib.h>
#include <libudev.h>

GIOChannel *channel;

struct udev *udev;
struct udev_monitor *mon;
static guint event_watch;

nyx_device_t *nyxDev = NULL;
void *mtp_change_callback_context = NULL;
nyx_device_callback_function_t mtp_change_callback;

#define ANDROID_USB_SYSFS_PATH    "/sys/class/android_usb/android0"
#define UDC_CLASS_PATH            "/sys/class/udc"
#define MTP_DAEMON_PATH           "/usr/bin/umtprd"

void mtp_init(void);
void mtp_close(void);
nyx_error_t mtp_get_state(nyx_device_handle_t handle, nyx_mass_storage_mode_state_t *state);

NYX_DECLARE_MODULE(NYX_DEVICE_MASS_STORAGE_MODE, "Main");

nyx_error_t nyx_module_open(nyx_instance_t i, nyx_device_t **d)
{
	if (nyxDev)
	{
		nyx_info(MSGID_NYX_MOD_MSMMTP_OPEN_ERR, 0, "MassStorageMode module already open");
		return NYX_ERROR_NONE;
	}

	nyxDev = (nyx_device_t *)calloc(1, sizeof(nyx_device_t));

	if (NULL == nyxDev)
	{
		return NYX_ERROR_OUT_OF_MEMORY;
	}

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_MASS_STORAGE_MODE_SET_MODE_MODULE_METHOD,
	                           "mtp_set_mode");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_MASS_STORAGE_MODE_GET_STATE_MODULE_METHOD,
	                           "mtp_get_state");

	nyx_module_register_method(i, (nyx_device_t *)nyxDev,
	                           NYX_MASS_STORAGE_MODE_REGISTER_CHANGE_CALLBACK_MODULE_METHOD,
	                           "mtp_register_change_callback");

	*d = (nyx_device_t *)nyxDev;
	mtp_init();

	return NYX_ERROR_NONE;
}

nyx_error_t nyx_module_close(nyx_device_t *d)
{
	mtp_close();
	return NYX_ERROR_NONE;
}

nyx_error_t mtp_set_mode(nyx_device_handle_t handle,
        nyx_mass_storage_mode_action_t action, nyx_mass_storage_mode_return_code_t *ret)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (!ret)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	if ((action == NYX_MASS_STORAGE_MODE_DISABLE) ||
	    (action == NYX_MASS_STORAGE_MODE_DISABLE_AFTER_FSCK))
	{
		*ret = NYX_MASS_STORAGE_MODE_SUCCESS;
		return NYX_ERROR_NONE;
	}

	if (action == NYX_MASS_STORAGE_MODE_ENABLE) {

		nyx_mass_storage_mode_state_t state;
		if (mtp_get_state(nyxDev, &state) != NYX_ERROR_NONE)
			return NYX_ERROR_INVALID_OPERATION;

		if (!(state & NYX_MASS_STORAGE_MODE_DRIVER_AVAILABLE)) {
			*ret = NYX_MASS_STORAGE_MODE_DRIVER_UNAVAILABLE;
			return NYX_ERROR_INVALID_OPERATION;
		}

		if (!(state & NYX_MASS_STORAGE_MODE_HOST_CONNECTED)) {
			*ret = NYX_MASS_STORAGE_MODE_HOST_NOT_CONNECTED;
			return NYX_ERROR_INVALID_OPERATION;
		}

		if (state & NYX_MASS_STORAGE_MODE_MODE_ON) {
			*ret = NYX_MASS_STORAGE_MODE_SUCCESS;
			return NYX_ERROR_NONE;
		}

		if (system("systemctl start umtprd --no-block") == 0) {
			*ret = NYX_MASS_STORAGE_MODE_SUCCESS;
			return NYX_ERROR_NONE;
		}
		else {
			*ret = NYX_MASS_STORAGE_MODE_MOUNT_FAILURE;
			return NYX_ERROR_INVALID_OPERATION;
		}
	}

	return NYX_ERROR_INVALID_VALUE;
}

/* Read a sysfs attribute into a newly allocated, whitespace-trimmed string.
 * Returns NULL when the attribute does not exist. */
static gchar *read_sysfs_attr(const char *path)
{
	gchar *contents = NULL;

	if (!g_file_get_contents(path, &contents, NULL, NULL))
		return NULL;

	return g_strstrip(contents);
}

/* Legacy Android composite gadget: /sys/class/android_usb/android0 */
static void get_state_android_usb(nyx_mass_storage_mode_state_t *state)
{
	gchar *attr;
	gchar *functions;
	char *running;
	char *token;
	bool mtp_enabled = false;

	attr = read_sysfs_attr(ANDROID_USB_SYSFS_PATH "/enable");
	if (g_strcmp0(attr, "1") != 0) {
		g_free(attr);
		return;
	}
	g_free(attr);

	functions = read_sysfs_attr(ANDROID_USB_SYSFS_PATH "/functions");
	running = functions;
	while ((token = strsep(&running, ","))) {
		mtp_enabled |= (g_strcmp0(token, "mtp") == 0);
	}
	g_free(functions);

	if (!mtp_enabled)
		return;

	*state |= NYX_MASS_STORAGE_MODE_DRIVER_AVAILABLE;

	attr = read_sysfs_attr(ANDROID_USB_SYSFS_PATH "/state");
	if ((g_strcmp0(attr, "CONFIGURED") == 0) || (g_strcmp0(attr, "CONNECTED") == 0)) {
		*state |= NYX_MASS_STORAGE_MODE_HOST_CONNECTED;
	}
	g_free(attr);
}

/* configfs/libcomposite gadget: cable and enumeration state live in
 * /sys/class/udc/<name>/state; umtprd brings its own ffs.mtp function. */
static void get_state_udc(nyx_mass_storage_mode_state_t *state)
{
	GDir *dir;
	const gchar *entry;
	bool have_udc = false;

	dir = g_dir_open(UDC_CLASS_PATH, 0, NULL);
	if (!dir)
		return;

	while ((entry = g_dir_read_name(dir)) != NULL) {
		gchar *path;
		gchar *attr;

		have_udc = true;

		path = g_build_filename(UDC_CLASS_PATH, entry, "state", NULL);
		attr = read_sysfs_attr(path);
		if ((g_strcmp0(attr, "configured") == 0) ||
		    (g_strcmp0(attr, "suspended") == 0)) {
			*state |= NYX_MASS_STORAGE_MODE_HOST_CONNECTED;
		}
		g_free(attr);
		g_free(path);
	}
	g_dir_close(dir);

	/* The MTP function is created on demand when umtprd starts, so the
	 * "driver" is available as soon as there is a UDC to serve it. */
	if (have_udc && (access(MTP_DAEMON_PATH, X_OK) == 0))
		*state |= NYX_MASS_STORAGE_MODE_DRIVER_AVAILABLE;
}

nyx_error_t mtp_get_state(nyx_device_handle_t handle,
        nyx_mass_storage_mode_state_t *state)
{
	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (!state)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

	*state = 0;

	// when using mtp, internal partition is always mounted
	*state |= NYX_MASS_STORAGE_MODE_PARTITION_MOUNTED;

	if (g_file_test(ANDROID_USB_SYSFS_PATH, G_FILE_TEST_IS_DIR))
		get_state_android_usb(state);
	else
		get_state_udc(state);

	if (system("pidof umtprd > /dev/null") == 0)
		*state |= NYX_MASS_STORAGE_MODE_MODE_ON;

	return NYX_ERROR_NONE;
}

nyx_error_t mtp_register_change_callback(nyx_device_handle_t handle,
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

	mtp_change_callback = callback_func;
	mtp_change_callback_context = context;

	return NYX_ERROR_NONE;
}

gboolean _handle_event(GIOChannel *channel, GIOCondition condition, gpointer data)
{
	struct udev_device *dev;

	if ((condition  & G_IO_IN) == G_IO_IN) {
		dev = udev_monitor_receive_device(mon);
		if (dev) {
			/* USB gadget state changed; notify connected clients so
			 * they can query the new status */
			if (mtp_change_callback)
				mtp_change_callback(nyxDev, NYX_CALLBACK_STATUS_DONE, mtp_change_callback_context);
			udev_device_unref(dev);
		}
	}

	return TRUE;
}

void mtp_init(void)
{
	int fd;

	udev = udev_new();
	if (!udev) {
		nyx_error(MSGID_NYX_MOD_UDEV_ERR, 0, "Could not initialize udev component; mtp status updates will not be available");
		return;
	}

	mon = udev_monitor_new_from_netlink(udev, "udev");
	/* legacy Android gadget on old kernels, UDC state changes on
	 * configfs/libcomposite kernels */
	udev_monitor_filter_add_match_subsystem_devtype(mon, "android_usb", NULL);
	udev_monitor_filter_add_match_subsystem_devtype(mon, "udc", NULL);
	udev_monitor_enable_receiving(mon);
	fd = udev_monitor_get_fd(mon);

	channel = g_io_channel_unix_new(fd);
	event_watch = g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_NVAL, _handle_event, NULL);
}

void mtp_close(void)
{
	if (channel) {
		g_io_channel_shutdown(channel, FALSE, NULL);
		channel = NULL;

		g_source_remove(event_watch);
	}

	if (mon) {
		udev_monitor_unref(mon);
	}

	if (udev) {
		udev_unref(udev);
	}
}
