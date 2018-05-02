// Copyright (c) 2010-2018 LG Electronics, Inc.
// Copyright (c) 2013 Simon Busch <morphis@gravedo.de>
// Copyright (c) 2009-2013 Michael 'Mickey' Lauer <mlauer@vanille-media.de>
// Copyright (c) 2018 Herman van Hazendonk <github.com@herrie.org>
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

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/input.h>
#include <fcntl.h>
#include <glib.h>
#include <libudev.h>

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>
#include <nyx/module/nyx_log.h>
#include "msgid.h"

NYX_DECLARE_MODULE(NYX_DEVICE_HAPTICS, "Main");

typedef struct {
	nyx_haptics_device_t *parent;
	const char *path;
	guint fulltimeoutwatch;
	guint smalltimeoutwatch;
	guint don;
	guint doff;
	gboolean on;
	guint pulses;
} haptics_device_t;


static const char* find_haptics_device(void)
{
	struct udev *udev;
	struct udev_enumerate *enumerator;
	struct udev_list_entry *devices;
	const char *path = NULL;
	struct udev_device *device;
	struct udev_list_entry *l;
	const char *device_type = NULL;

	udev = udev_new();
	if (!udev) {
		nyx_error(MSGID_NYX_MOD_UDEV_ERR, 0, "Could not initialize udev component");
		return NULL;
	}

	enumerator = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(enumerator, "timed_output");
	udev_enumerate_scan_devices(enumerator);

	devices = udev_enumerate_get_list_entry(enumerator);
	if (devices != NULL) {
		for (l = devices; l != NULL; l = udev_list_entry_get_next(l)) {
			device = udev_device_new_from_syspath(udev, udev_list_entry_get_name(l));
			if (device == NULL)
				continue;

			nyx_debug(MSGID_NYX_MOD_HAPTICS_ODEVICE_FOUND, 0, "Found possible vibrator device: %s", udev_list_entry_get_name(l));
			path = g_strdup(udev_device_get_syspath(device));
			udev_device_unref(device);
			break;
		}
	}
	else {
		nyx_error(MSGID_NYX_MOD_HAPTICS_NODEVICE_ERR, 0, "Did not find any devices matching the vibrator subsystem");
	}

	udev_enumerate_unref(enumerator);
	udev_unref(udev);

	return path;
}

nyx_error_t nyx_module_open(nyx_instance_t instance, nyx_device_t** device)
{
	const char *vibrator_path = NULL;
	haptics_device_t *haptics_device;

	haptics_device = g_new0(haptics_device_t, 1);

	if (G_UNLIKELY(!haptics_device))
		return NYX_ERROR_OUT_OF_MEMORY;

	nyx_module_register_method(instance, (nyx_device_t*) haptics_device,
			NYX_HAPTICS_VIBRATE_MODULE_METHOD, "haptics_vibrate");
	nyx_module_register_method(instance, (nyx_device_t*) haptics_device,
			NYX_HAPTICS_CANCEL_MODULE_METHOD, "haptics_cancel");
	nyx_module_register_method(instance, (nyx_device_t*) haptics_device,
			NYX_HAPTICS_CANCEL_ALL_MODULE_METHOD, "haptics_cancel_all");
	nyx_module_register_method(instance, (nyx_device_t*) haptics_device,
			NYX_HAPTICS_GET_EFFECT_ID_MODULE_METHOD, "haptics_get_effect_id");

	vibrator_path = find_haptics_device();
	if (!vibrator_path) {
		nyx_error(MSGID_NYX_MOD_HAPTICS_NODEVICE_ERR, 0, "Could not find a vibrator device");
		return NYX_ERROR_DEVICE_UNAVAILABLE;
	}

	haptics_device->path = g_build_filename(vibrator_path, "enable", NULL);
	g_free(vibrator_path);

	*device = (nyx_device_t*) haptics_device;

	return NYX_ERROR_NONE;
}

nyx_error_t nyx_module_close(nyx_device_t* device)
{
	haptics_device_t *haptics_device = (haptics_device_t*) device;

	if (device == NULL)
		return NYX_ERROR_INVALID_HANDLE;

	g_free(haptics_device->path);
	g_free(haptics_device);

	return NYX_ERROR_NONE;
}

static gboolean file_set_contents(const char *filename, const char *content, unsigned int length)
{
	int fd;

	fd = open(filename, O_WRONLY);
	if (fd < 0)
		return FALSE;

	if (write(fd, content, length) < 0) {
		close(fd);
		return FALSE;
	}

	close(fd);

	return TRUE;
}

static gboolean enable_vibrator(haptics_device_t *device, int duration)
{
	gboolean ret;

	if (duration < 0)
		duration = 0;

	gchar *content = g_strdup_printf("%d", duration);
	ret = file_set_contents(device->path, content, strlen(content));

	device->on = (duration > 0);

	g_free(content);
	return ret;
}

static void clean_timeouts(haptics_device_t *device)
{
	if (device->smalltimeoutwatch > 0) {
		g_source_remove(device->smalltimeoutwatch);
		device->smalltimeoutwatch = 0;
	}

	if (device->fulltimeoutwatch > 0) {
		g_source_remove(device->fulltimeoutwatch);
		device->fulltimeoutwatch = 0;
	}

	device->pulses = 0;
	device->on = FALSE;
}

static gboolean vibrate_timeout_cb(gpointer data)
{
	haptics_device_t *device = data;
	clean_timeouts(device);
	return FALSE;
}

static gboolean vibrate(haptics_device_t *device, int milliseconds)
{
	if (device->pulses > 0 || device->fulltimeoutwatch > 0)
		return FALSE;

	if (milliseconds < 50)
		return FALSE;

	clean_timeouts(device);
	enable_vibrator(device, milliseconds);

	g_timeout_add(milliseconds, vibrate_timeout_cb, device);

	return TRUE;
}

static gboolean toggle_timeout(gpointer data)
{
	haptics_device_t *device = data;

	nyx_debug(MSGID_NYX_MOD_HAPTICS_TOGGLE_TIMEOUT, 0, "on = %d, pulses = %d", (guint) device->on, (guint) device->pulses);

	if (device->on == FALSE) {
		enable_vibrator(device, device->don);
		g_timeout_add(device->don, toggle_timeout, device);
	}
	else {
		device->pulses = device->pulses - 1;
		if (device->pulses > 0) {
			device->on = FALSE;
			device->smalltimeoutwatch = g_timeout_add(device->doff, toggle_timeout, device);
		}
	}

	return FALSE;
}

static gboolean vibrate_pattern(haptics_device_t *device, int pulses, int delay_on, int delay_off)
{
	nyx_debug(MSGID_NYX_MOD_HAPTICS_VIBRATE_PATTERN, 0, "%s pulses=%i delay_on=%i delay_off=%i", __PRETTY_FUNCTION__, pulses, delay_on, delay_off);

	if (device->pulses > 0 || device->fulltimeoutwatch > 0)
		return FALSE;

	if (pulses < 1 || delay_on < 50 || delay_off < 50)
		return FALSE;

	device->don = delay_on;
	device->doff = delay_off;
	device->pulses = pulses;

	toggle_timeout(device);

	return TRUE;
}

nyx_error_t haptics_vibrate(nyx_device_t *device, nyx_haptics_configuration_t configuration)
{
	haptics_device_t *hdevice = (haptics_device_t*) device;
	guint pulses = 0;
	guint delay_on = 0;
	guint delay_off = 0;

	nyx_debug(MSGID_NYX_MOD_HAPTICS_VIBRATE, 0, "%s", __PRETTY_FUNCTION__);

	switch (configuration.type) {
	case NYX_HAPTICS_EFFECT_UNDEFINED:
		pulses = configuration.duration / configuration.period;
		delay_off = configuration.period / 2;
		delay_on = configuration.period / 2;
		break;
	case NYX_HAPTICS_EFFECT_RINGTONE:
	case NYX_HAPTICS_EFFECT_ALERT:
	case NYX_HAPTICS_EFFECT_NOTIFICATION:
	case NYX_HAPTICS_EFFECT_TAPUP:
	case NYX_HAPTICS_EFFECT_TAPDOWN:
		nyx_error(MSGID_NYX_MOD_HAPTICS_NOSPECIAL_EFF, 0, "We're not supporting special effects at the moment!");
		return NYX_ERROR_INVALID_VALUE;
	}

	if (pulses < 0) {
		nyx_debug(MSGID_NYX_MOD_HAPTICS_NOPULSES_ERR, 0, "No pulses!");
		return NYX_ERROR_INVALID_VALUE;
	}

	if (vibrate_pattern(hdevice, pulses, delay_on, delay_off) == FALSE)
		return NYX_ERROR_INVALID_VALUE;

	return NYX_ERROR_NONE;
}

nyx_error_t haptics_cancel(nyx_device_t *device, int32_t id)
{
	return NYX_ERROR_NONE;
}

nyx_error_t haptics_cancel_all(nyx_device_t *device)
{
	return NYX_ERROR_NONE;
}
