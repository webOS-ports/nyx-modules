/* @@@LICENSE
*
*      Copyright (c) 2010-2012 Hewlett-Packard Development Company, L.P.
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

#include <stdbool.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/input.h>
#include <errno.h>
#include <poll.h>
#include <glib.h>
#include <stdio.h>
#include <pthread.h>

#include <nyx/nyx_module.h>

#include "keys_common.h"

NYX_DECLARE_MODULE(NYX_DEVICE_KEYS, "Keys");

#define NYX_CONF_FILE           "/etc/nyx.conf"
#define NYX_CONF_GROUP_KEYS     "module.keys"
#define NYX_CONF_KEY_PATHS      "paths"

#define MAX_INPUT_NODES         5

int keypad_event_fd[MAX_INPUT_NODES];
int num_keypad_event_fd = 0;
int keypad_notifier_pipe_fds[2];
pthread_t notifier_thread;

/**
 * This is modeled after the linux input event interface events.
 * See linux/input.h for the original definition.
 */
typedef struct InputEvent
{
    struct timeval time;  /**< time event was generated */
    uint16_t type;        /**< type of event, EV_ABS, EV_MSC, etc. */
    uint16_t code;        /**< event code, ABS_X, ABS_Y, etc. */
    int32_t value;        /**< event value: coordinate, intensity,etc. */
} InputEvent_t;

static gchar** read_input_paths(guint *num_paths)
{
    GError *error = NULL;
    GKeyFile *keyfile = NULL;
    gchar **result = NULL;

    keyfile = g_key_file_new();
    g_key_file_set_list_separator(keyfile, ';');

    if (!g_key_file_load_from_file(keyfile, NYX_CONF_FILE, G_KEY_FILE_NONE, &error)) {
        nyx_error("Failed to load conf file from %s: %s", NYX_CONF_FILE, error->message);
        g_error_free(error);
        goto cleanup;
    }

    if (!g_key_file_has_key(keyfile, NYX_CONF_GROUP_KEYS, NYX_CONF_KEY_PATHS, &error)) {
        nyx_error("Failed to read input paths from conf file: %s", error->message);
        g_error_free(error);
        goto cleanup;
    }

    result = g_key_file_get_string_list(keyfile, NYX_CONF_GROUP_KEYS, NYX_CONF_KEY_PATHS, num_paths, NULL);

cleanup:
    g_key_file_free(keyfile);
    return result;
}

void *notifier_thread_func(void *user_data)
{
    struct pollfd fds[MAX_INPUT_NODES];
    int event = 1, n;

    for (n = 0; n < num_keypad_event_fd; n++) {
        fds[n].fd = keypad_event_fd[n];
        fds[n].events = POLLIN;
    }

    while (1) {
        int ret_val = poll(fds, num_keypad_event_fd, -1);
        if (ret_val <= 0)
            continue;

        nyx_debug("Got new input event; waking up main thread ..");

        /* wakeup main thread */
        (void) write(keypad_notifier_pipe_fds[1], &event, sizeof(int));
    }

    return NULL;
}


static nyx_event_keys_t* keys_event_create()
{
    nyx_event_keys_t* event_ptr = (nyx_event_keys_t*) calloc(
            sizeof(nyx_event_keys_t), 1);

    if (NULL == event_ptr)
        return event_ptr;

    ((nyx_event_t*) event_ptr)->type = NYX_EVENT_KEYS;

    return event_ptr;
}

nyx_error_t keys_release_event(nyx_device_t* d, nyx_event_t* e)
{
    if (NULL == d)
        return NYX_ERROR_INVALID_HANDLE;
    if (NULL == e)
        return NYX_ERROR_INVALID_HANDLE;

    nyx_event_keys_t* a = (nyx_event_keys_t*) e;

    free(a);
    return NYX_ERROR_NONE;
}

nyx_error_t nyx_module_open(nyx_instance_t i, nyx_device_t** d)
{
    guint num_paths;
    gchar **input_paths;
    gchar *path;
    int fd, n;

    input_paths = read_input_paths(&num_paths);

    if (input_paths == NULL)
        return NYX_ERROR_NOT_FOUND;

    for (n = 0; n < num_paths; n++) {
        path = input_paths[n];

        if (num_keypad_event_fd == MAX_INPUT_NODES) {
            nyx_warn("Reached maximum number of input nodes. Skipping others.");
            break;
        }

        nyx_debug("Initializing input device %s", path);

        fd = open(path, O_RDONLY);
        if (fd < 0) {
            nyx_error("Could not open keypad event file at %s", path);
            continue;
        }

        keypad_event_fd[num_keypad_event_fd] = fd;
        num_keypad_event_fd++;
    }

    if (num_keypad_event_fd == 0)
        return NYX_ERROR_NOT_FOUND;

    keys_device_t* keys_device = (keys_device_t*) calloc(sizeof(keys_device_t), 1);

    if (G_UNLIKELY(!keys_device))
        return NYX_ERROR_OUT_OF_MEMORY;

    nyx_module_register_method(i, (nyx_device_t*) keys_device,
            NYX_GET_EVENT_SOURCE_MODULE_METHOD, "keys_get_event_source");
    nyx_module_register_method(i, (nyx_device_t*) keys_device,
            NYX_GET_EVENT_MODULE_METHOD, "keys_get_event");
    nyx_module_register_method(i, (nyx_device_t*) keys_device,
            NYX_RELEASE_EVENT_MODULE_METHOD, "keys_release_event");

    *d = (nyx_device_t*) keys_device;

    notifier_thread = pthread_create(&notifier_thread, NULL, notifier_thread_func, NULL);
    pipe2(&keypad_notifier_pipe_fds, 0);

    return NYX_ERROR_NONE;

fail_unlock_settings:

    return NYX_ERROR_GENERIC;
}

nyx_error_t nyx_module_close(nyx_device_t* d)
{
    keys_device_t* keys_device = (keys_device_t*) d;
    if (NULL == d)
        return NYX_ERROR_INVALID_HANDLE;

    if (keys_device->current_event_ptr)
        keys_release_event(d, (nyx_event_t*) keys_device->current_event_ptr);

    nyx_debug("Freeing keys %p", d);
    free(d);

    return NYX_ERROR_NONE;
}

nyx_error_t keys_get_event_source(nyx_device_t* d, int* f)
{
    if (NULL == d)
        return NYX_ERROR_INVALID_HANDLE;
    if (NULL == f)
        return NYX_ERROR_INVALID_VALUE;

    *f = keypad_notifier_pipe_fds[0];

    return NYX_ERROR_NONE;
}

int read_input_event(InputEvent_t* pEvents, int maxEvents)
{
    int numEvents = 0;
    int rd = 0, n, event;
    struct pollfd fds[MAX_INPUT_NODES];

    if (pEvents == NULL)
        return -1;

    /* clear notifier pipe */
    (void) read(keypad_notifier_pipe_fds[0], &event, sizeof(int));

    for (n = 0; n < num_keypad_event_fd; n++) {
        fds[n].fd = keypad_event_fd[n];
        fds[n].events = POLLIN;
    }

    int ret_val = poll(fds, num_keypad_event_fd, 0);
    if (ret_val <= 0)
        return 0;

    for (n = 0; n < num_keypad_event_fd; n++) {
        if (fds[n].revents & POLLIN) {
            /* keep looping if get EINTR */
            for (;;) {
                rd = read(fds[n].fd, pEvents, sizeof(InputEvent_t) * maxEvents);
                if (rd > 0) {
                    numEvents += rd / sizeof(InputEvent_t);
                    break;
                }
                else if (rd < 0 && errno!=EINTR) {
                    nyx_error("Failed to read events from keypad event file");
                    break;
                }
            }
        }
    }

    return numEvents;
}

#define MAX_EVENTS      64

nyx_error_t keys_get_event(nyx_device_t* d, nyx_event_t** e)
{
    static InputEvent_t raw_events[MAX_EVENTS];

    static int event_count = 0;
    static int event_iter = 0;

    int rd = 0;

    keys_device_t* keys_device = (keys_device_t*) d;

    if (!event_iter) {
        event_count = read_input_event(raw_events, MAX_EVENTS);
        keys_device->current_event_ptr = NULL;
    }

    if (keys_device->current_event_ptr == NULL)
        keys_device->current_event_ptr = keys_event_create();

    for (; event_iter < event_count;) {
        InputEvent_t* input_event_ptr;
        input_event_ptr = &raw_events[event_iter];
        event_iter++;
        if (input_event_ptr->type == EV_KEY) {
            keys_device->current_event_ptr->key_type = NYX_KEY_TYPE_STANDARD;
            keys_device->current_event_ptr->key = lookup_key(keys_device,
                    input_event_ptr->code, input_event_ptr->value,
                    &keys_device->current_event_ptr->key_type);
        }
        else {
            continue;
        }

        keys_device->current_event_ptr->key_is_press
                = (input_event_ptr->value) ? true : false;
        keys_device->current_event_ptr->key_is_auto_repeat
                = (input_event_ptr->value > 1) ? true : false;

        *e = (nyx_event_t*) keys_device->current_event_ptr;
        keys_device->current_event_ptr = NULL;

        if (NULL != *e)
            break;
    }

    if (event_iter >= event_count)
        event_iter = 0;

    return NYX_ERROR_NONE;
}
