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

#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/input.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>
#include <glib.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>
#include <nyx/module/nyx_log.h>
#include "msgid.h"

#include "keys_common.h"

NYX_DECLARE_MODULE(NYX_DEVICE_KEYS, "Keys");

#define NYX_CONF_FILE           "/etc/nyx.conf"
#define NYX_CONF_GROUP_KEYS     "module.keys"
#define NYX_CONF_KEY_PATHS      "paths"

#define MAX_INPUT_NODES         5

int keypad_event_fd[MAX_INPUT_NODES];
int num_keypad_event_fd = 0;
int keypad_notifier_pipe_fds[2];

/* How often to retry a missing node while one is outstanding.  IN_CREATE can
 * arrive before udev has finished setting the permissions, so the open that
 * succeeds is often the one a moment later rather than the one on the
 * notification. */
#define INPUT_RESCAN_INTERVAL_MS 1000

/* The configured path for each slot, kept for the lifetime of the module so a
 * node that disappears can be reopened when it comes back. */
static gchar *keypad_event_path[MAX_INPUT_NODES];
static int input_watch_fd = -1;

/*
 * The notifier thread signals the main loop that an input event is waiting,
 * but it never reads the evdev descriptors itself - read_input_event() on the
 * main thread does that. poll() is level-triggered, so between the signal and
 * that read the same event stays readable and the thread would otherwise wake,
 * write to the pipe and poll again in a tight loop. One keypress produced tens
 * of pipe writes, and QSocketNotifier delivered one main-loop wakeup for each.
 *
 * notify_pending closes that gap: the thread signals once, then blocks until
 * the main thread reports the descriptors drained.
 */
static pthread_mutex_t notify_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  notify_cond  = PTHREAD_COND_INITIALIZER;
static bool            notify_pending = false;

static void notifier_wait_until_drained(void)
{
	pthread_mutex_lock(&notify_mutex);

	while (notify_pending)
	{
		pthread_cond_wait(&notify_cond, &notify_mutex);
	}

	pthread_mutex_unlock(&notify_mutex);
}

static void notifier_mark_pending(void)
{
	pthread_mutex_lock(&notify_mutex);
	notify_pending = true;
	pthread_mutex_unlock(&notify_mutex);
}

static void notifier_mark_drained(void)
{
	pthread_mutex_lock(&notify_mutex);
	notify_pending = false;
	pthread_cond_signal(&notify_cond);
	pthread_mutex_unlock(&notify_mutex);
}
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
    gsize path_count = 0;

    keyfile = g_key_file_new();
    g_key_file_set_list_separator(keyfile, ';');

    if (!g_key_file_load_from_file(keyfile, NYX_CONF_FILE, G_KEY_FILE_NONE, &error)) {
        nyx_error(MSGID_NYX_MOD_KEYS_CONF_FILE_ERR, 0, "Failed to load conf file");
        g_error_free(error);
        goto cleanup;
    }

    if (!g_key_file_has_key(keyfile, NYX_CONF_GROUP_KEYS, NYX_CONF_KEY_PATHS, &error)) {
        nyx_error(MSGID_NYX_MOD_KEYS_CONF_FILE_PATH_ERR, 0, "Failed to read input paths from conf file");
        g_error_free(error);
        goto cleanup;
    }

    result = g_key_file_get_string_list(keyfile, NYX_CONF_GROUP_KEYS, NYX_CONF_KEY_PATHS, &path_count, NULL);
    *num_paths = (guint) path_count;

cleanup:
    g_key_file_free(keyfile);
    return result;
}

/*
 * Watch the directories the configured nodes live in, so a device that comes
 * back is picked up without restarting the process.
 */
static void setup_input_watch(void)
{
    int n;

    input_watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (input_watch_fd < 0) {
        nyx_error(MSGID_NYX_MOD_KEYS_OPEN_ERR, 0,
                  "Failed to create inotify instance (%s); a node that "
                  "disappears will stay gone until restart", strerror(errno));
        return;
    }

    for (n = 0; n < num_keypad_event_fd; n++) {
        gchar *dir;

        if (keypad_event_path[n] == NULL)
            continue;

        dir = g_path_get_dirname(keypad_event_path[n]);

        /* Watching a directory twice returns the descriptor already held for
         * it, so the duplicates the paths share cost nothing.  IN_ATTRIB is
         * wanted as well as IN_CREATE: udev sets the permissions after the
         * node exists, and that is often when it first becomes openable. */
        if (inotify_add_watch(input_watch_fd, dir, IN_CREATE | IN_ATTRIB) < 0)
            nyx_error(MSGID_NYX_MOD_KEYS_OPEN_ERR, 0,
                      "Failed to watch %s (%s)", dir, strerror(errno));

        g_free(dir);
    }
}

static void drain_input_watch(void)
{
    char buf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));

    /* Which node changed does not matter: any change in a watched directory
     * is reason enough to retry everything currently missing. */
    while (read(input_watch_fd, buf, sizeof(buf)) > 0)
        ;
}

static int count_missing_event_fds(void)
{
    int n, missing = 0;

    for (n = 0; n < num_keypad_event_fd; n++) {
        if (keypad_event_fd[n] < 0 && keypad_event_path[n] != NULL)
            missing++;
    }

    return missing;
}

/*
 * Reopen every slot whose node is absent.  Returns how many are still
 * missing afterwards.
 */
static int reopen_missing_event_fds(void)
{
    int n, missing = 0;

    for (n = 0; n < num_keypad_event_fd; n++) {
        int fd;

        if (keypad_event_fd[n] >= 0 || keypad_event_path[n] == NULL)
            continue;

        fd = open(keypad_event_path[n], O_RDONLY);
        if (fd < 0) {
            missing++;
            continue;
        }

        nyx_warn(MSGID_NYX_MOD_KEYS_NEW_INPUT_DEV, 0,
                 "Reopened input node %s", keypad_event_path[n]);
        keypad_event_fd[n] = fd;
    }

    return missing;
}

/*
 * Close descriptors the kernel has hung up on.
 *
 * When an input device disappears -- the detachable keyboard re-enumerating
 * after the USB controller is reinitialised on resume, for instance -- its
 * open descriptor stays valid but reports POLLERR|POLLHUP forever.  Those bits
 * are not cleared by polling and there is nothing left to read, so a poll()
 * that only tests the return value never blocks again: the notifier thread
 * wakes, signals the main loop, the main loop finds no POLLIN, reports itself
 * drained, and the cycle repeats for as long as the process lives.  Measured
 * at ~2500 rounds a second, better than a full CPU core, on a PineTab2 whose
 * keyboard had re-enumerated once.
 *
 * The slot is kept and its descriptor set to -1 rather than removed, because
 * poll() ignores a negative fd and the configured path stays on record for
 * whoever wants to reopen it.
 */
static void reap_dead_event_fds(struct pollfd *fds)
{
    int n;

    for (n = 0; n < num_keypad_event_fd; n++) {
        if (keypad_event_fd[n] < 0)
            continue;
        if (!(fds[n].revents & (POLLERR | POLLHUP | POLLNVAL)))
            continue;

        nyx_error(MSGID_NYX_MOD_KEY_EVENT_ERR, 0,
                  "input node %d hung up (revents 0x%x), closing it",
                  keypad_event_fd[n], fds[n].revents);
        close(keypad_event_fd[n]);
        keypad_event_fd[n] = -1;
    }
}

void *notifier_thread_func(void *user_data)
{
    struct pollfd fds[MAX_INPUT_NODES + 1];
    int event = 1, n, have_input, nfds, timeout;
    int missing = count_missing_event_fds();

    while (1) {
        /* Do not poll again until the main thread has read what the last
         * notification was about, or this spins on the same pending event. */
        notifier_wait_until_drained();

        /* Rebuilt every pass: reap_dead_event_fds() can retire a descriptor
         * and reopen_missing_event_fds() can restore one, and a retired slot
         * polls as -1, which poll() skips. */
        for (n = 0; n < num_keypad_event_fd; n++) {
            fds[n].fd = keypad_event_fd[n];
            fds[n].events = POLLIN;
            fds[n].revents = 0;
        }
        nfds = num_keypad_event_fd;

        if (input_watch_fd >= 0) {
            fds[nfds].fd = input_watch_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        timeout = missing ? INPUT_RESCAN_INTERVAL_MS : -1;

        int ret_val = poll(fds, nfds, timeout);
        if (ret_val < 0)
            continue;

        /* Timed out with something still missing: retry it. */
        if (ret_val == 0) {
            missing = reopen_missing_event_fds();
            continue;
        }

        if (input_watch_fd >= 0 &&
            (fds[num_keypad_event_fd].revents & POLLIN)) {
            drain_input_watch();
            missing = reopen_missing_event_fds();
        }

        have_input = 0;
        for (n = 0; n < num_keypad_event_fd; n++) {
            if (fds[n].revents & POLLIN)
                have_input = 1;
        }

        reap_dead_event_fds(fds);
        missing = count_missing_event_fds();

        /* A descriptor hung up and has now been retired.  Waking the main
         * thread would only buy it an empty read, so go straight back to
         * poll(), which can block properly on what is left. */
        if (!have_input)
            continue;

        nyx_debug("Got new input event; waking up main thread ..");

        notifier_mark_pending();

        /* wakeup main thread */
        if (write(keypad_notifier_pipe_fds[1], &event, sizeof(int)) < 0)
        {
            nyx_debug("Failed to wake main thread: %s", strerror(errno));
            /* Nothing will drain on our behalf if the write failed. */
            notifier_mark_drained();
        }
    }

    return NULL;
}


static nyx_event_keys_t *keys_event_create()
{
	nyx_event_keys_t *event_ptr = (nyx_event_keys_t *) calloc(
	                                  sizeof(nyx_event_keys_t), 1);

	if (NULL == event_ptr)
	{
		return event_ptr;
	}

	((nyx_event_t *) event_ptr)->type = NYX_EVENT_KEYS;

	return event_ptr;
}

nyx_error_t keys_release_event(nyx_device_t *d, nyx_event_t *e)
{
	if (NULL == d)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (NULL == e)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	nyx_event_keys_t *a = (nyx_event_keys_t *) e;

	free(a);
	return NYX_ERROR_NONE;
}

nyx_error_t nyx_module_open(nyx_instance_t i, nyx_device_t **d)
{
    guint num_paths;
    gchar **input_paths;
    gchar *path;
    int fd, n;

	if (NULL == d)
	{
	    nyx_error(MSGID_NYX_MOD_KEYS_OPEN_ERR, 0,"Keys device open error.");
	    return NYX_ERROR_INVALID_VALUE;
	}

    input_paths = read_input_paths(&num_paths);

    if (input_paths == NULL)
    {
        return NYX_ERROR_NOT_FOUND;
    }

    for (n = 0; n < num_paths; n++) {
        path = input_paths[n];

        /* The configured list is separator terminated, which leaves a
         * trailing empty entry. */
        if (path == NULL || *path == '\0')
            continue;

        if (num_keypad_event_fd == MAX_INPUT_NODES) {
            nyx_warn(MSGID_NYX_MOD_KEYS_OPEN_ERR, 0, "Reached maximum number of input nodes. Skipping others.");
            break;
        }

        nyx_debug("Initializing input device %s", path);

        fd = open(path, O_RDONLY);
        if (fd < 0)
            nyx_error(MSGID_NYX_MOD_KEYS_OPEN_ERR, 0,
                      "Could not open keypad event file at %s; will retry "
                      "when it appears", path);

        /* Keep the slot whether or not the open worked: the recorded path is
         * what lets the notifier thread pick the node up once it appears, and
         * a slot holding -1 is simply skipped by poll(). */
        keypad_event_path[num_keypad_event_fd] = g_strdup(path);
        keypad_event_fd[num_keypad_event_fd] = fd;
        num_keypad_event_fd++;
    }

    if (num_keypad_event_fd == 0)
    {
        return NYX_ERROR_NOT_FOUND;
    }

	keys_device_t *keys_device = (keys_device_t *) calloc(sizeof(keys_device_t),
	                             1);

	if (G_UNLIKELY(!keys_device))
	{
		nyx_error(MSGID_NYX_MOD_KEY_OUT_OF_MEM, 0, "Out of memory");
		return NYX_ERROR_OUT_OF_MEMORY;
	}

	nyx_module_register_method(i, (nyx_device_t *) keys_device,
	                           NYX_GET_EVENT_SOURCE_MODULE_METHOD, "keys_get_event_source");
	nyx_module_register_method(i, (nyx_device_t *) keys_device,
	                           NYX_GET_EVENT_MODULE_METHOD, "keys_get_event");
	nyx_module_register_method(i, (nyx_device_t *) keys_device,
	                           NYX_RELEASE_EVENT_MODULE_METHOD, "keys_release_event");

	*d = (nyx_device_t *) keys_device;

    if (pipe2(keypad_notifier_pipe_fds, O_NONBLOCK) < 0)
    {
        nyx_error(MSGID_NYX_MOD_KEYS_OPEN_ERR, 0, "Failed to create notifier pipe");
        return NYX_ERROR_GENERIC;
    }

    setup_input_watch();

    if (pthread_create(&notifier_thread, NULL, notifier_thread_func, NULL) != 0)
    {
        nyx_error(MSGID_NYX_MOD_KEYS_OPEN_ERR, 0, "Failed to create notifier thread");
        return NYX_ERROR_GENERIC;
    }

    return NYX_ERROR_NONE;
}

nyx_error_t nyx_module_close(nyx_device_t *d)
{
	keys_device_t *keys_device = (keys_device_t *) d;

	if (NULL == d)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (keys_device->current_event_ptr)
	{
		keys_release_event(d, (nyx_event_t *) keys_device->current_event_ptr);
	}

	nyx_debug("Freeing keys %p", d);
	free(d);

	return NYX_ERROR_NONE;
}

nyx_error_t keys_get_event_source(nyx_device_t *d, int *f)
{
	if (NULL == d)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (NULL == f)
	{
		return NYX_ERROR_INVALID_VALUE;
	}

    *f = keypad_notifier_pipe_fds[0];

    return NYX_ERROR_NONE;
}

int read_input_event(InputEvent_t* pEvents, int maxEvents)
{
    int numEvents = 0;
    int rd = 0, n, event;
    struct pollfd fds[MAX_INPUT_NODES];

	if (pEvents == NULL)
	{
		return -1;
	}

	/* Past this point every return has to go through notifier_mark_drained():
	 * the notifier thread is blocked until it is told the descriptors were
	 * read, so an early return that skips it stops all key input for good. */

    /* Clear the notifier pipe completely: a burst can leave more than one
     * token queued, and each leftover token is another main-loop wakeup that
     * finds nothing to read. */
    while (read(keypad_notifier_pipe_fds[0], &event, sizeof(int)) > 0)
        ;

    for (n = 0; n < num_keypad_event_fd; n++) {
        fds[n].fd = keypad_event_fd[n];
        fds[n].events = POLLIN;
    }

    int ret_val = poll(fds, num_keypad_event_fd, 0);
    if (ret_val <= 0)
    {
        notifier_mark_drained();
        return 0;
    }

    for (n = 0; n < num_keypad_event_fd; n++)
    {
        if (fds[n].revents & POLLIN) 
        {
            /* keep looping if get EINTR */
            for (;;) 
            {
                rd = read(fds[n].fd, pEvents, sizeof(InputEvent_t) * maxEvents);

                if (rd > 0) 
                {
                    numEvents += rd / sizeof(InputEvent_t);
                    break;
                }
                else if (rd == 0)
                {
                    /*
                     * EOF on an input node. A zero return matched neither of the
                     * other two arms, so this read a dead descriptor in a tight
                     * loop for ever. reap_dead_event_fds() below retires the
                     * node; just stop reading it.
                     */
                    nyx_error(MSGID_NYX_MOD_KEY_EVENT_READ_ERR, 0, "Keypad event file returned EOF");
                    break;
                }
                else if (rd < 0 && errno != EINTR) 
                {
    				nyx_error(MSGID_NYX_MOD_KEY_EVENT_READ_ERR, 0, "Failed to read events from keypad event file");
                    break;
                }
            }
        }
    }

	/* Same hangup sweep as the notifier thread: whichever side sees a dead
	 * descriptor first is the one that retires it. */
	reap_dead_event_fds(fds);

	/* Descriptors are drained now, so the notifier thread may poll again. */
	notifier_mark_drained();

	return numEvents;
}

#define MAX_EVENTS      64

nyx_error_t keys_get_event(nyx_device_t *d, nyx_event_t **e)
{
	static InputEvent_t raw_events[MAX_EVENTS];

	static int event_count = 0;
	static int event_iter = 0;

	keys_device_t *keys_device = (keys_device_t *) d;

	/*
	 * Event bookkeeping...
	 */
	if (!event_iter)
	{
		event_count = read_input_event(raw_events, MAX_EVENTS);
		keys_device->current_event_ptr = NULL;
	}

	if (keys_device->current_event_ptr == NULL)
	{
		/*
		 * let's allocate new event and hold it here.
		 */
		keys_device->current_event_ptr = keys_event_create();

		if (keys_device->current_event_ptr == NULL)
		{
			return NYX_ERROR_OUT_OF_MEMORY;
		}
	}

	for (; event_iter < event_count;)
	{
		InputEvent_t *input_event_ptr;
		input_event_ptr = &raw_events[event_iter];
		event_iter++;

		if (input_event_ptr->type == EV_KEY)
		{
			keys_device->current_event_ptr->key_type = NYX_KEY_TYPE_STANDARD;
			keys_device->current_event_ptr->key = lookup_key(keys_device,
			                                      input_event_ptr->code, input_event_ptr->value,
			                                      &keys_device->current_event_ptr->key_type);
		}
		else
		{
			continue;
		}

		keys_device->current_event_ptr->key_is_press
		    = (input_event_ptr->value) ? true : false;
		keys_device->current_event_ptr->key_is_auto_repeat
		    = (input_event_ptr->value > 1) ? true : false;

		*e = (nyx_event_t *) keys_device->current_event_ptr;
		keys_device->current_event_ptr = NULL;

		/*
		 * Generated event, bail out and let the caller know.
		 */
		if (NULL != *e)
		{
			break;
		}
	}

	if (event_iter >= event_count)
	{
		event_iter = 0;
	}

	return NYX_ERROR_NONE;
}
