/* @@@LICENSE
*
* Copyright (c) 2013 Simon Busch
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/input.h>
#include <fcntl.h>
#include <poll.h>
#include <glib.h>

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>

#define MAX_EVENTS		64

typedef struct {
	nyx_device_t parent;
	nyx_event_sensor_als_t *current_event_ptr;
	int fd;
	struct input_event raw_events[MAX_EVENTS];
	int event_count;
	int event_iter;
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

static nyx_event_keys_t *als_event_create()
{
	nyx_event_sensor_als_t* event = (nyx_event_sensor_als_t*)
		calloc(sizeof(nyx_event_sensor_als_t), 1);

	if (event == NULL)
		return NULL;

	((nyx_event_t*) event)->type = NYX_EVENT_SENSOR_ALS;

	return event;
}

nyx_error_t nyx_module_open(nyx_instance_t i, nyx_device_t** device)
{
	als_device_t *als_device = (als_device_t*) calloc(sizeof(als_device_t), 1);

	if (G_UNLIKELY(!als_device))
		return NYX_ERROR_OUT_OF_MEMORY;

	als_device->event_count = 0;
	als_device->event_iter = 0;

	/* now we can start to use the input device and listen for events */
	als_device->fd = open("/dev/input/event4", O_RDONLY);
	if (als_device->fd < 0) {
		free(als_device);
		return NYX_ERROR_INVALID_VALUE;
	}

	nyx_module_register_method(i, (nyx_device_t*) als_device,
			NYX_GET_EVENT_SOURCE_MODULE_METHOD, "als_get_event_source");
	nyx_module_register_method(i, (nyx_device_t*) als_device,
			NYX_GET_EVENT_MODULE_METHOD, "als_get_event");
	nyx_module_register_method(i, (nyx_device_t*) als_device,
			NYX_RELEASE_EVENT_MODULE_METHOD, "als_release_event");
	nyx_module_register_method(i, (nyx_device_t*) als_device,
			NYX_SET_OPERATING_MODE_MODULE_METHOD, "als_set_operating_mode");

	*device = (nyx_device_t*) als_device;

	return NYX_ERROR_NONE;
}

nyx_error_t nyx_module_close(nyx_device_t* device)
{
	als_device_t *als_device = (als_device_t*) device;

	if (als_device->fd > 0)
		close(als_device->fd);

	if (device == NULL)
		return NYX_ERROR_INVALID_HANDLE;

	if (als_device->current_event_ptr)
		als_release_event(als_device, (nyx_event_t*) als_device->current_event_ptr);

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
	GError *error = NULL;

	switch (mode) {
		case NYX_OPERATING_MODE_OFF:
			if (file_set_contents("/sys/class/input/event4/device/enable", "0", 2) == FALSE) {
				nyx_error("Failed to disable ALS sensor device");
				return NYX_ERROR_INVALID_FILE_ACCESS;
			}
			break;
		case NYX_OPERATING_MODE_ON:
			if (file_set_contents("/sys/class/input/event4/device/enable", "1", 2) == FALSE) {
				nyx_error("Failed to enable ALS sensor device");
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
				nyx_error("Failed to read events from keypad event file");
				return -1;
			}
		}
	}

	return num_events;
}


nyx_error_t als_get_event(nyx_device_t* device, nyx_event_t** event)
{
	int rd = 0;
	als_device_t *als_device = (als_device_t*) device;


	/* event bookkeeping... */
	if(!als_device->event_iter) {
		als_device->event_count = read_input_event(als_device->fd, als_device->raw_events,
											MAX_EVENTS);
		als_device->current_event_ptr = NULL;
	}

	if (als_device->current_event_ptr == NULL) {
		/* let's allocate new event and hold it here */
		als_device->current_event_ptr = als_event_create();
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
