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
#include <nyx/module/nyx_device_haptics_internal.h>
#include "msgid.h"

NYX_DECLARE_MODULE(NYX_DEVICE_HAPTICS, "Main");

#ifndef HAPTICS_VIBRATOR_GPIO_PATH
#define HAPTICS_VIBRATOR_GPIO_PATH "/dev/input/by-path/platform-vibrator-event"
#endif

/* Durations (ms) used for the named effects requested by LunaSysMgr */
#define HAPTICS_EFFECT_TAP_DURATION            50
#define HAPTICS_EFFECT_NOTIFICATION_DURATION   300
#define HAPTICS_EFFECT_ALERT_PULSES            3
#define HAPTICS_EFFECT_ALERT_ON                500
#define HAPTICS_EFFECT_ALERT_OFF               500
#define HAPTICS_EFFECT_RINGTONE_PULSES         4
#define HAPTICS_EFFECT_RINGTONE_ON             800
#define HAPTICS_EFFECT_RINGTONE_OFF            400

typedef struct {
	/* nyx-lib casts haptics devices to nyx_haptics_device_t to service
	 * nyx_haptics_get_effect_id()/dampening factor accessors, so the
	 * parent must be the full haptics device struct, not nyx_device_t. */
	nyx_haptics_device_t _parent;
	const char *path;             /* path for duration */
	const char *path_activation;  /* path for activating the vibrator */
	int fd_ioctl;
	struct ff_effect ff;
	guint fulltimeoutwatch;
	guint smalltimeoutwatch;
	guint don;
	guint doff;
	gboolean on;
	guint pulses;
	int32_t effect_counter;
} haptics_device_t;

static void clean_timeouts(haptics_device_t *device);


static const char* find_haptics_device_timed_output(void)
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

			nyx_debug("Found possible vibrator device: %s", udev_list_entry_get_name(l));
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

static const char* find_haptics_device_leds(void)
{
	struct udev *udev;
	const char *path = NULL;
	struct udev_device *device;

	udev = udev_new();
	if (!udev) {
		nyx_error(MSGID_NYX_MOD_UDEV_ERR, 0, "Could not initialize udev component");
		return NULL;
	}

	device = udev_device_new_from_syspath(udev, "/sys/class/leds/vibrator");
	if (!device) {
		nyx_error(MSGID_NYX_MOD_HAPTICS_NODEVICE_ERR, 0, "Did not find any devices matching the vibrator subsystem");
	}
	else {
		nyx_debug("Found possible vibrator device: %s", "/sys/class/leds/vibrator");
		path = g_strdup(udev_device_get_syspath(device));
		udev_device_unref(device);
	}
	udev_unref(udev);

	return path;
}

nyx_error_t nyx_module_open(nyx_instance_t instance, nyx_device_t** device)
{
	const char *vibrator_path = NULL;
	const char *path_activation = NULL;
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

	vibrator_path = find_haptics_device_timed_output();
	if (vibrator_path) {
		/* found it ! */
		haptics_device->path = g_build_filename(vibrator_path, "enable", NULL);
		haptics_device->path_activation = NULL; /* for clarity's sake */
	}
	else {
		/* try the leds class vibrator */
		vibrator_path = find_haptics_device_leds();
		
		if (vibrator_path) {
			haptics_device->path = g_build_filename(vibrator_path, "duration", NULL);
			haptics_device->path_activation = g_build_filename(vibrator_path, "activate", NULL);
		}
		else {
			haptics_device->fd_ioctl = open(HAPTICS_VIBRATOR_GPIO_PATH, O_RDWR | O_CLOEXEC);
		}
	}
	if (!vibrator_path && haptics_device->fd_ioctl<=0) {
		nyx_error(MSGID_NYX_MOD_HAPTICS_NODEVICE_ERR, 0, "Could not find a vibrator device");
		return NYX_ERROR_DEVICE_UNAVAILABLE;
	}

	g_free(vibrator_path);

	*device = (nyx_device_t*) haptics_device;

	return NYX_ERROR_NONE;
}

nyx_error_t nyx_module_close(nyx_device_t* device)
{
	haptics_device_t *haptics_device = (haptics_device_t*) device;

	if (device == NULL)
		return NYX_ERROR_INVALID_HANDLE;

	/* A ringtone/alert pattern may still have timers queued; they would
	 * fire on freed memory. */
	clean_timeouts(haptics_device);

	if(haptics_device->fd_ioctl>0) {
		close(haptics_device->fd_ioctl); 
		haptics_device->fd_ioctl = 0; 
	}

	g_free(haptics_device->path);
	if(haptics_device->path_activation) g_free(haptics_device->path_activation);
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

	/* On the leds-class interface a stop is a write of "0" to activate;
	 * activating with a zero duration is driver-defined and may vibrate
	 * indefinitely. */
	if (duration == 0 && device->path_activation) {
		device->on = FALSE;
		return file_set_contents(device->path_activation, "0", 1);
	}

	gchar *content = g_strdup_printf("%d", duration);
	ret = file_set_contents(device->path, content, strlen(content));

	if(TRUE==ret && device->path_activation) {
		ret = file_set_contents(device->path_activation, "1", 1);
	}

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
	
	if (device->fd_ioctl>0) {
		/* stop the vibration */
		struct input_event ff_event;
		memset(&ff_event, 0, sizeof(ff_event));
		ff_event.type = EV_FF;
		ff_event.code = device->ff.id;
		ff_event.value = 0;
		write(device->fd_ioctl, &ff_event, sizeof ff_event);

		/* clear rumble effect */
		ioctl(device->fd_ioctl, EVIOCRMFF, device->ff.id);
	}

	device->pulses = 0;
	device->on = FALSE;
}

static gboolean vibrate_timeout_cb(gpointer data)
{
	haptics_device_t *device = data;

	/* This source is removed by returning FALSE; forget the id so
	 * clean_timeouts() does not remove the currently-dispatching source. */
	device->fulltimeoutwatch = 0;
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

static gboolean vibrate_oneshot(haptics_device_t *device, int milliseconds)
{
	if (milliseconds < 50)
		milliseconds = 50;

	/* A pattern is playing; treat the one-shot as merged into it rather
	 * than erroring out (tap feedback during a ringtone/alert). */
	if (device->pulses > 0)
		return TRUE;

	clean_timeouts(device);
	return enable_vibrator(device, milliseconds);
}

static gboolean toggle_timeout(gpointer data)
{
	haptics_device_t *device = data;

	nyx_debug("on = %d, pulses = %d", (guint) device->on, (guint) device->pulses);

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
	nyx_debug("%s pulses=%i delay_on=%i delay_off=%i", __PRETTY_FUNCTION__, pulses, delay_on, delay_off);

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
	haptics_device_t *haptics_device = (haptics_device_t*) device;
	guint pulses = 0;
	guint delay_on = 0;
	guint delay_off = 0;
	gint one_shot = 0;

	nyx_debug("%s", __PRETTY_FUNCTION__);

	switch (configuration.type) {
	case NYX_HAPTICS_EFFECT_UNDEFINED:
		if (configuration.period > 0) {
			pulses = configuration.duration / configuration.period;
			delay_off = configuration.period / 2;
			delay_on = configuration.period / 2;
			if (pulses < 1) {
				/* duration shorter than one period (the Phone
				 * dialpad sends period 100, duration 10): one
				 * blip of the requested duration, not an error */
				one_shot = configuration.duration;
			}
		}
		else {
			/* no period given: single vibration for the full duration */
			one_shot = configuration.duration;
		}
		break;
	case NYX_HAPTICS_EFFECT_TAPUP:
	case NYX_HAPTICS_EFFECT_TAPDOWN:
		one_shot = HAPTICS_EFFECT_TAP_DURATION;
		break;
	case NYX_HAPTICS_EFFECT_NOTIFICATION:
		one_shot = HAPTICS_EFFECT_NOTIFICATION_DURATION;
		break;
	case NYX_HAPTICS_EFFECT_ALERT:
		pulses = HAPTICS_EFFECT_ALERT_PULSES;
		delay_on = HAPTICS_EFFECT_ALERT_ON;
		delay_off = HAPTICS_EFFECT_ALERT_OFF;
		break;
	case NYX_HAPTICS_EFFECT_RINGTONE:
		pulses = HAPTICS_EFFECT_RINGTONE_PULSES;
		delay_on = HAPTICS_EFFECT_RINGTONE_ON;
		delay_off = HAPTICS_EFFECT_RINGTONE_OFF;
		break;
	}

	if (one_shot <= 0 && pulses < 1) {
		nyx_debug("No pulses!");
		return NYX_ERROR_INVALID_VALUE;
	}

	if (haptics_device->fd_ioctl>0) {
		int ff_duration = one_shot > 0 ? one_shot : (gint) (pulses * (delay_on + delay_off));

		/* upload rumble effect */
		memset(&haptics_device->ff, 0, sizeof(haptics_device->ff));
		haptics_device->ff.type = FF_RUMBLE,
		haptics_device->ff.id = -1;
		haptics_device->ff.replay.length = ff_duration;
		haptics_device->ff.replay.delay = 0;
		haptics_device->ff.u.rumble.strong_magnitude = 0xc000;
		haptics_device->ff.u.rumble.weak_magnitude = 0;
		ioctl(haptics_device->fd_ioctl, EVIOCSFF, &(haptics_device->ff));

		/* play vibration pattern */
		struct input_event ff_event;
		memset(&ff_event, 0, sizeof(ff_event));
		ff_event.type = EV_FF;
		ff_event.code = haptics_device->ff.id;
		ff_event.value = 1;
		write(haptics_device->fd_ioctl, &ff_event, sizeof ff_event);

		/* timeout to stop vibration - tracked so cancel/close can
		 * remove it; an untracked watch would fire after the device
		 * is freed */
		clean_timeouts(haptics_device);
		haptics_device->fulltimeoutwatch = g_timeout_add(ff_duration, vibrate_timeout_cb, device);
    }
	else if (one_shot > 0) {
		if (vibrate_oneshot(haptics_device, one_shot) == FALSE)
			return NYX_ERROR_DEVICE_UNAVAILABLE;
	}
	else {
		if (vibrate_pattern(haptics_device, pulses, delay_on, delay_off) == FALSE)
			return NYX_ERROR_INVALID_VALUE;
	}

	haptics_device->effect_counter++;

	if (haptics_device->effect_counter <= 0)
		haptics_device->effect_counter = 1;

	haptics_device->_parent.haptic_effect_id = haptics_device->effect_counter;

	return NYX_ERROR_NONE;
}

nyx_error_t haptics_cancel(nyx_device_t *device, int32_t id)
{
	haptics_device_t *haptics_device = (haptics_device_t*) device;

	if (device == NULL)
		return NYX_ERROR_INVALID_HANDLE;

	clean_timeouts(haptics_device);
	enable_vibrator(haptics_device, 0);

	return NYX_ERROR_NONE;
}

nyx_error_t haptics_cancel_all(nyx_device_t *device)
{
	return haptics_cancel(device, 0);
}
