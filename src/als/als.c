// Copyright (c) 2013 Simon Busch <morphis@gravedo.de>
// Copyright (c) 2018 Herman van Hazendonk <github.com@herrie.org	>
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
#include <math.h>
#include <unistd.h>
#include <linux/input.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <glib.h>

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>
#include <nyx/module/nyx_log.h>
#include "msgid.h"
#include "nyx_conf.h"

#define MAX_EVENTS		64
#ifndef ALS_INPUT_DEVICE
#define ALS_INPUT_DEVICE		"/sys/class/input/event4/"
#endif

#define NYX_CONF_GROUP_ALS	"module.als"
#define NYX_CONF_KEY_PATH	"sysfs_path"

#define IIO_DEVICES_DIR		"/sys/bus/iio/devices"
#define IIO_ILLUMINANCE_RAW	"in_illuminance_raw"
#define IIO_ILLUMINANCE_SCALE	"in_illuminance_scale"

/*
 * Poll intervals behind nyx_report_rate_t. The consumer (luna-displaymanager)
 * asks for HIGH while the reading is moving between regions and drops back to
 * LOW once it has settled, so the fast rate only costs power while the light
 * is actually changing.
 */
#define ALS_INTERVAL_HIGHEST_MS	100
#define ALS_INTERVAL_HIGH_MS	200
#define ALS_INTERVAL_MEDIUM_MS	500
#define ALS_INTERVAL_LOW_MS	1000

typedef struct {
	nyx_device_t parent;
	nyx_event_sensor_als_t *current_event_ptr;
	int fd;
	struct input_event raw_events[MAX_EVENTS];
	int event_count;
	int event_iter;

	/*
	 * IIO backing. The legacy path above is an evdev node that pushes
	 * ABS_MISC events; an IIO ALS has no such stream, so the descriptor
	 * handed to the caller is a timerfd and each expiry is one sysfs read.
	 */
	gboolean iio_mode;
	gchar *iio_raw_path;
	double iio_scale;
	int interval_ms;
} als_device_t;

NYX_DECLARE_MODULE(NYX_DEVICE_SENSOR_ALS, "Default");

nyx_error_t als_release_event(nyx_device_t *device, nyx_event_t *event)
{
	if (device == NULL || event == NULL)
		return NYX_ERROR_INVALID_HANDLE;

	nyx_event_sensor_als_t *als_event = (nyx_event_sensor_als_t*) event;
	free(als_event);

	return NYX_ERROR_NONE;
}

static nyx_event_sensor_als_t *als_event_create(void)
{
	nyx_event_sensor_als_t* event = (nyx_event_sensor_als_t*)
		calloc(sizeof(nyx_event_sensor_als_t), 1);

	if (event == NULL)
		return NULL;

	((nyx_event_t*) event)->type = NYX_EVENT_SENSOR_ALS;

	return event;
}


/*
 * Read a whole sysfs attribute as a double. Returns FALSE when the attribute
 * is missing or unparseable, which is how a device without an ALS channel is
 * told apart from one whose reading happens to be zero.
 */
static gboolean als_read_double(const gchar *path, double *out)
{
	gchar *contents = NULL;
	gboolean ok = FALSE;

	if (path == NULL || out == NULL)
		return FALSE;

	if (!g_file_get_contents(path, &contents, NULL, NULL))
		return FALSE;

	errno = 0;
	gchar *end = NULL;
	double value = g_ascii_strtod(contents, &end);

	if (end != contents && errno == 0) {
		*out = value;
		ok = TRUE;
	}

	g_free(contents);

	return ok;
}

/*
 * Locate an IIO device exposing an illuminance channel.
 *
 * A runtime path from luneos-device-config wins, as it does for the keys and
 * battery modules; otherwise walk the class directory. The walk is sorted so
 * a board with more than one candidate picks the same one on every boot -
 * g_dir_read_name() order follows the filesystem and is not stable.
 */
static gchar *als_find_iio_path(void)
{
	gchar *configured = nyx_conf_get_path(NYX_CONF_GROUP_ALS, NYX_CONF_KEY_PATH);

	if (configured != NULL) {
		gchar *raw = g_build_filename(configured, IIO_ILLUMINANCE_RAW, NULL);

		if (g_file_test(raw, G_FILE_TEST_EXISTS)) {
			g_free(raw);
			return configured;
		}

		nyx_warn(MSGID_NYX_MOD_ALS_OPEN_ERR, 0,
		         "configured ALS path %s has no %s, falling back to detection",
		         configured, IIO_ILLUMINANCE_RAW);
		g_free(raw);
		g_free(configured);
	}

	GDir *dir = g_dir_open(IIO_DEVICES_DIR, 0, NULL);

	if (dir == NULL)
		return NULL;

	GList *names = NULL;
	const gchar *name;

	while ((name = g_dir_read_name(dir)) != NULL)
		names = g_list_prepend(names, g_strdup(name));

	g_dir_close(dir);

	names = g_list_sort(names, (GCompareFunc) g_strcmp0);

	gchar *found = NULL;

	for (GList *it = names; it != NULL; it = it->next) {
		gchar *candidate = g_build_filename(IIO_DEVICES_DIR, (const gchar *) it->data, NULL);
		gchar *raw = g_build_filename(candidate, IIO_ILLUMINANCE_RAW, NULL);

		if (found == NULL && g_file_test(raw, G_FILE_TEST_EXISTS))
			found = g_strdup(candidate);

		g_free(raw);
		g_free(candidate);
	}

	g_list_free_full(names, g_free);

	return found;
}

static int als_interval_for_rate(nyx_report_rate_t rate)
{
	switch (rate) {
		case NYX_REPORT_RATE_HIGHEST: return ALS_INTERVAL_HIGHEST_MS;
		case NYX_REPORT_RATE_HIGH:    return ALS_INTERVAL_HIGH_MS;
		case NYX_REPORT_RATE_MEDIUM:  return ALS_INTERVAL_MEDIUM_MS;
		case NYX_REPORT_RATE_LOW:     return ALS_INTERVAL_LOW_MS;
		default:                      return ALS_INTERVAL_LOW_MS;
	}
}

/* Arm the sampling timer, or disarm it entirely when interval_ms is 0. */
static nyx_error_t als_arm_timer(als_device_t *als_device, int interval_ms)
{
	struct itimerspec spec;

	memset(&spec, 0, sizeof(spec));

	if (interval_ms > 0) {
		spec.it_interval.tv_sec  = interval_ms / 1000;
		spec.it_interval.tv_nsec = (long) (interval_ms % 1000) * 1000000L;
		spec.it_value = spec.it_interval;
	}

	if (timerfd_settime(als_device->fd, 0, &spec, NULL) != 0) {
		nyx_error(MSGID_NYX_MOD_ALS_OPEN_ERR, 0,
		          "Failed to arm ALS sampling timer: %s", strerror(errno));
		return NYX_ERROR_GENERIC;
	}

	g_warning("ALSNYX: armed timer fd=%d interval=%dms", als_device->fd, interval_ms);

	return NYX_ERROR_NONE;
}

nyx_error_t als_set_report_rate(nyx_device_t *device, nyx_report_rate_t rate)
{
	als_device_t *als_device = (als_device_t *) device;

	if (device == NULL)
		return NYX_ERROR_INVALID_HANDLE;

	/* The evdev sensor pushes at whatever rate its driver chooses. */
	if (!als_device->iio_mode)
		return NYX_ERROR_NOT_IMPLEMENTED;

	int interval_ms = als_interval_for_rate(rate);

	if (interval_ms == als_device->interval_ms)
		return NYX_ERROR_NONE;

	als_device->interval_ms = interval_ms;

	return als_arm_timer(als_device, interval_ms);
}

nyx_error_t nyx_module_open(nyx_instance_t i, nyx_device_t** device)
{
	als_device_t *als_device = (als_device_t*) calloc(sizeof(als_device_t), 1);

	if (G_UNLIKELY(!als_device))
		return NYX_ERROR_OUT_OF_MEMORY;

	als_device->event_count = 0;
	als_device->event_iter = 0;

	/*
	 * Prefer an IIO ALS. The evdev path below only exists on the old
	 * Android-derived boards this module was written for; every device with
	 * a mainline light sensor exposes it through IIO instead, and on those
	 * the open below simply failed and the module never loaded.
	 */
	gchar *iio_path = als_find_iio_path();

	if (iio_path != NULL) {
		gchar *scale_path = g_build_filename(iio_path, IIO_ILLUMINANCE_SCALE, NULL);

		als_device->iio_raw_path = g_build_filename(iio_path, IIO_ILLUMINANCE_RAW, NULL);

		/* scale is optional: without it the raw count is already lux. */
		if (!als_read_double(scale_path, &als_device->iio_scale) ||
		    als_device->iio_scale <= 0.0) {
			als_device->iio_scale = 1.0;
		}

		g_free(scale_path);
		g_free(iio_path);

		/*
		 * CLOCK_BOOTTIME rather than CLOCK_MONOTONIC: it keeps counting
		 * across a suspend, so the first sample after a resume is not
		 * delayed by however long the device was down.
		 */
		als_device->fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK);

		if (als_device->fd < 0) {
			nyx_error(MSGID_NYX_MOD_ALS_OPEN_ERR, 0,
			          "Failed to create ALS sampling timer: %s", strerror(errno));
			g_free(als_device->iio_raw_path);
			free(als_device);
			return NYX_ERROR_GENERIC;
		}

		als_device->iio_mode = TRUE;
		als_device->interval_ms = ALS_INTERVAL_LOW_MS;

		g_warning("ALSNYX: IIO mode, raw=%s scale=%.4f timerfd=%d",
		          als_device->iio_raw_path, als_device->iio_scale, als_device->fd);

		/* Left disarmed until set_operating_mode turns the sensor on, so an
		 * opened-but-unused ALS costs nothing. */
	}
	else {
		/* now we can start to use the input device and listen for events */
		als_device->fd = open(ALS_INPUT_DEVICE, O_RDONLY);
		if (als_device->fd < 0) {
			free(als_device);
			return NYX_ERROR_INVALID_VALUE;
		}
	}

	nyx_module_register_method(i, (nyx_device_t*) als_device,
			NYX_GET_EVENT_SOURCE_MODULE_METHOD, "als_get_event_source");
	nyx_module_register_method(i, (nyx_device_t*) als_device,
			NYX_GET_EVENT_MODULE_METHOD, "als_get_event");
	nyx_module_register_method(i, (nyx_device_t*) als_device,
			NYX_RELEASE_EVENT_MODULE_METHOD, "als_release_event");
	nyx_module_register_method(i, (nyx_device_t*) als_device,
			NYX_SET_OPERATING_MODE_MODULE_METHOD, "als_set_operating_mode");
	nyx_module_register_method(i, (nyx_device_t*) als_device,
			NYX_SET_REPORT_RATE_MODULE_METHOD, "als_set_report_rate");

	*device = (nyx_device_t*) als_device;

	return NYX_ERROR_NONE;
}

nyx_error_t nyx_module_close(nyx_device_t* device)
{
	als_device_t *als_device = (als_device_t*) device;

	if (device == NULL)
		return NYX_ERROR_INVALID_HANDLE;

	if (als_device->fd > 0)
		close(als_device->fd);

	if (als_device->current_event_ptr)
		als_release_event((nyx_device_t*) als_device, (nyx_event_t*) als_device->current_event_ptr);

	g_free(als_device->iio_raw_path);

	free(als_device);

	return NYX_ERROR_NONE;
}

gboolean file_set_contents(const char *filename, const char *content, unsigned int length)
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

nyx_error_t als_set_operating_mode(nyx_device_t *device, nyx_operating_mode_t mode)
{
	als_device_t *als_device = (als_device_t *) device;

	if (device == NULL)
		return NYX_ERROR_INVALID_HANDLE;

	if (als_device->iio_mode) {
		/* No enable attribute to write: sampling simply stops. */
		switch (mode) {
			case NYX_OPERATING_MODE_OFF:
				return als_arm_timer(als_device, 0);
			case NYX_OPERATING_MODE_ON:
				return als_arm_timer(als_device, als_device->interval_ms);
			default:
				return NYX_ERROR_INVALID_VALUE;
		}
	}

	switch (mode) {
		case NYX_OPERATING_MODE_OFF:
			if (file_set_contents(ALS_INPUT_DEVICE"device/enable", "0", 2) == FALSE) {
				nyx_error(MSGID_NYX_MOD_ALS_DISABLE_ERR, 0, "Failed to disable ALS sensor device");
				return NYX_ERROR_INVALID_FILE_ACCESS;
			}
			break;
		case NYX_OPERATING_MODE_ON:
			if (file_set_contents(ALS_INPUT_DEVICE"device/enable", "1", 2) == FALSE) {
				nyx_error(MSGID_NYX_MOD_ALS_ENABLE_ERR, 0, "Failed to enable ALS sensor device");
				return NYX_ERROR_INVALID_FILE_ACCESS;
			}
			break;
		default:
			return NYX_ERROR_INVALID_VALUE;
	}

	return NYX_ERROR_NONE;
}

nyx_error_t als_get_event_source(nyx_device_t *device, int *fd)
{
	als_device_t *als_device = (als_device_t*) device;

	if (device == NULL || fd == NULL)
		return NYX_ERROR_INVALID_VALUE;

	*fd = als_device->fd;

	return NYX_ERROR_NONE;
}

struct pollfd fds[1];

static int read_input_event(int fd, struct input_event *events, int max_events)
{
	int num_events = 0;
	int rc = 0;
	int bytesread;

	if(events == NULL)
		return -1;

	fds[0].fd = fd;
	fds[0].events = POLLIN;

	rc = poll(fds, 1, 0);
	if (rc <= 0)
		return 0;

	if (fds[0].revents & POLLIN) {
		for (;;) {
			bytesread = read(fds[0].fd, events, sizeof(struct input_event) * max_events);
			if (bytesread > 0) {
				num_events += bytesread / sizeof(struct input_event);
				break;
			}
			else if (bytesread < 0 && errno != EINTR) {
				nyx_error(MSGID_NYX_MOD_ALS_READ_EVENT_ERR, 0, "Failed to read events from event file");
				return -1;
			}
		}
	}

	return num_events;
}


/*
 * One timer expiry, one sysfs read, one event. The descriptor has to be
 * drained or the caller's poll loop would spin on a permanently ready fd.
 */
static nyx_error_t als_get_event_iio(als_device_t *als_device, nyx_event_t **event)
{
	uint64_t expirations = 0;
	ssize_t rd;

	*event = NULL;

	do {
		rd = read(als_device->fd, &expirations, sizeof(expirations));
	} while (rd < 0 && errno == EINTR);

	if (rd != (ssize_t) sizeof(expirations)) {
		/* Nothing pending is not an error - the caller polls this fd. */
		if (rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			g_warning("ALSNYX: get_event - timerfd not ready (EAGAIN)");
			return NYX_ERROR_NONE;
		}

		g_warning("ALSNYX: get_event - timerfd read rd=%zd errno=%d", rd, errno);
		return NYX_ERROR_GENERIC;
	}

	g_warning("ALSNYX: timer fired, %llu expirations",
	          (unsigned long long) expirations);

	double raw = 0.0;

	g_warning("ALSNYX: reading %s (scale %.4f)", als_device->iio_raw_path, als_device->iio_scale);

	if (!als_read_double(als_device->iio_raw_path, &raw)) {
		nyx_warn(MSGID_NYX_MOD_ALS_READ_EVENT_ERR, 0,
		         "Failed to read %s", als_device->iio_raw_path);
		return NYX_ERROR_GENERIC;
	}

	if (als_device->current_event_ptr == NULL) {
		als_device->current_event_ptr = als_event_create();

		if (als_device->current_event_ptr == NULL)
			return NYX_ERROR_OUT_OF_MEMORY;
	}

	double lux = raw * als_device->iio_scale;
	int32_t lux_i = (int32_t) (lux + 0.5);

	if (lux_i == 0 && raw > 0.0)
		lux_i = 1;

	als_device->current_event_ptr->item.intensity_in_lux = lux_i;

	g_warning("ALSNYX: raw=%.1f x %.4f = %.3f -> %d lux", raw,
	          als_device->iio_scale, lux, lux_i);

	*event = (nyx_event_t *) als_device->current_event_ptr;
	als_device->current_event_ptr = NULL;

	g_warning("ALSNYX: handing back event %p", (void *) *event);

	return NYX_ERROR_NONE;
}

nyx_error_t als_get_event(nyx_device_t* device, nyx_event_t** event)
{
	als_device_t *als_device = (als_device_t*) device;

	if (device == NULL || event == NULL)
		return NYX_ERROR_INVALID_HANDLE;

	if (als_device->iio_mode)
		return als_get_event_iio(als_device, event);

	/* event bookkeeping... */
	if(!als_device->event_iter) {
		als_device->event_count = read_input_event(als_device->fd, als_device->raw_events,
											MAX_EVENTS);
		als_device->current_event_ptr = NULL;
	}

	if (als_device->current_event_ptr == NULL) {
		/* let's allocate new event and hold it here */
		als_device->current_event_ptr = als_event_create();

		if (als_device->current_event_ptr == NULL)
			return NYX_ERROR_OUT_OF_MEMORY;
	}

	for (; als_device->event_iter < als_device->event_count;) {
		struct input_event* current_event = &als_device->raw_events[als_device->event_iter];
		als_device->event_iter++;

		if (current_event->type == EV_ABS && current_event->code == ABS_MISC) {
			/**
			 * From AOSP device/samsung/tuna/libsensors/LightSensor.cpp:
			 * Convert adc value to lux assuming:
			 *  I = 10 * log(Ev) uA; R = 24kOhm
			 * Max adc value 1023 = 1.25V
			 *  1/4 of light reaches sensor
			 */
			als_device->current_event_ptr->item.intensity_in_lux =
				(int32_t) (powf(10, current_event->value * (125.0f / 1023.0f / 24.0f)) * 4);
		}
		else {
			continue;
		}

		*event = (nyx_event_t*) als_device->current_event_ptr;
		als_device->current_event_ptr = NULL;

		/* Generated event, bail out and let the caller know. */
		if (NULL != *event)
			break;
	}

	if(als_device->event_iter >= als_device->event_count)
		als_device->event_iter = 0;

	return NYX_ERROR_NONE;
}
