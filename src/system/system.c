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
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <glib.h>
#include <gio/gio.h>
#include "rtc.h"
#include "nyx_conf.h"

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


/*
 * Suspend: one-shot, blocking, with the wakeup_count handshake.
 *
 * sleepd calls nyx_system_suspend_async() from MachineSleep() on its own
 * suspend thread and treats the call as the whole sleep: when it returns the
 * state machine goes straight to kernel-resume (resume signal, MachineWakeup,
 * idle check rescheduled). Both suspend entry points therefore share this one
 * body and block until the kernel has resumed or refused to enter suspend.
 *
 * The handshake mirrors what Android's SystemSuspend does and works the same
 * on kernels with and without PM_AUTOSLEEP:
 *
 *   1. read /sys/power/wakeup_count. The read blocks while any wakeup source
 *      is active - that includes every kernel wakelock in /sys/power/wake_lock,
 *      which is how sleepd activities, IPC clients and the display manager veto
 *      the suspend between the userspace vote and the kernel write. Unlike
 *      Android we do not wait indefinitely: nothing in LuneOS holds a wakelock
 *      for "the screen is on", so a suspend that parked here for as long as,
 *      say, the USB controller's wakeup source stays active (the whole time a
 *      cable is plugged in) would fire the instant the cable is pulled, with
 *      the user looking at the screen. The read is bounded to
 *      WAKEUP_COUNT_WAIT_MS - long enough to absorb the short wakelocks an
 *      interrupt handler holds, and on the scale of sleepd's own retry
 *      interval (after_resume_idle_ms, 1 s by default). Past that we report
 *      "not suspended" naming the sources still active and let sleepd re-run
 *      its policy before trying again.
 *   2. write the value back. EBUSY (or EINVAL on older kernels) means a wakeup
 *      event raced with us: report "not suspended" and let sleepd retry after
 *      after_resume_idle_ms. That is the retry loop; there is none here.
 *   3. write "mem" to /sys/power/state. Returns 0 once the kernel has resumed;
 *      -EBUSY if a wakeup arrived during entry (also "not suspended").
 *
 * /sys/power/autosleep is never armed: an opportunistic re-suspend loop the
 * kernel runs on its own leaves sleepd unable to tell wake from sleep and,
 * measured on a PinePhone Pro, turns the device into a zombie that re-suspends
 * before userspace can take a wakelock. system_resume() only disarms it, in
 * case something else did.
 *
 * *success = false with NYX_ERROR_NONE means "retry later"; an NYX error is
 * reserved for a bad handle.
 */

#define SYSFS_POWER_STATE   "/sys/power/state"
#define SYSFS_WAKEUP_COUNT  "/sys/power/wakeup_count"
#define SYSFS_AUTOSLEEP     "/sys/power/autosleep"

/* Returns 0, or -errno. */
static int write_sysfs_string(const char *path, const char *value)
{
	ssize_t written;
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
	{
		return -errno;
	}

	written = write(fd, value, strlen(value));

	if (written < 0)
	{
		int err = errno;
		close(fd);
		return -err;
	}

	close(fd);
	return 0;
}

/* How long to wait for the kernel's wakeup sources to go quiet. */
#define WAKEUP_COUNT_WAIT_MS 1000

struct wakeup_count_read
{
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool done;
	int result;     /* 0, or -errno */
	char buf[32];   /* the count, newline stripped */
};

static void close_fd_cleanup(void *arg)
{
	close(*(int *)arg);
}

/*
 * Helper thread: the sysfs read blocks (interruptibly) while a wakeup source
 * is active, so it runs here and the caller times it out with pthread_cancel.
 * read() is the cancellation point; the fd is closed by the cleanup handler
 * if the thread is unwound there.
 */
static void *wakeup_count_reader(void *arg)
{
	struct wakeup_count_read *r = arg;
	int ret;
	int fd = open(SYSFS_WAKEUP_COUNT, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
	{
		ret = -errno;
	}
	else
	{
		ssize_t n;
		int old_state;

		pthread_cleanup_push(close_fd_cleanup, &fd);
		n = read(fd, r->buf, sizeof(r->buf) - 1);
		ret = (n < 0) ? -errno : 0;
		/* Past the blocking read: finish and report even if a cancel is pending. */
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state);
		pthread_cleanup_pop(1);

		if (ret == 0)
		{
			while (n > 0 && (r->buf[n - 1] == '\n' || r->buf[n - 1] == ' '))
			{
				n--;
			}

			r->buf[n] = '\0';
			ret = (n > 0) ? 0 : -EIO;
		}
	}

	pthread_mutex_lock(&r->lock);
	r->result = ret;
	r->done = true;
	pthread_cond_signal(&r->cond);
	pthread_mutex_unlock(&r->lock);
	return NULL;
}

/*
 * Reads the current wakeup_count into buf, waiting at most wait_ms for the
 * kernel's wakeup sources to go quiet. Returns 0 on success, -ETIMEDOUT if a
 * source was still active when the wait ran out, or -errno.
 */
static int read_wakeup_count(char *buf, size_t len, unsigned int wait_ms)
{
	struct wakeup_count_read r = { .done = false, .result = -EIO, .buf = "" };
	pthread_condattr_t cattr;
	pthread_t tid;
	struct timespec deadline;
	int ret;

	pthread_mutex_init(&r.lock, NULL);
	pthread_condattr_init(&cattr);
	pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
	pthread_cond_init(&r.cond, &cattr);
	pthread_condattr_destroy(&cattr);

	ret = pthread_create(&tid, NULL, wakeup_count_reader, &r);

	if (ret != 0)
	{
		pthread_cond_destroy(&r.cond);
		pthread_mutex_destroy(&r.lock);
		return -ret;
	}

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += wait_ms / 1000;
	deadline.tv_nsec += (long)(wait_ms % 1000) * 1000000L;

	if (deadline.tv_nsec >= 1000000000L)
	{
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&r.lock);

	while (!r.done)
	{
		if (pthread_cond_timedwait(&r.cond, &r.lock, &deadline) == ETIMEDOUT)
		{
			break;
		}
	}

	pthread_mutex_unlock(&r.lock);

	if (!r.done)
	{
		pthread_cancel(tid);
	}

	pthread_join(tid, NULL);
	pthread_cond_destroy(&r.cond);
	pthread_mutex_destroy(&r.lock);

	/* r.done may have flipped between the timeout and the cancel. */
	if (!r.done)
	{
		return -ETIMEDOUT;
	}

	if (r.result == 0)
	{
		g_strlcpy(buf, r.buf, len);
	}

	return r.result;
}

/*
 * Names the wakeup sources that are active right now, comma separated, for
 * the log. Best effort: /sys/class/wakeup (5.4+) first, then the debugfs
 * table older kernels have; an empty string if neither is readable.
 */
static void active_wakeup_sources(char *out, size_t len)
{
	const char *dir_path = "/sys/class/wakeup";
	GDir *dir;
	FILE *f;
	char line[512];

	out[0] = '\0';
	dir = g_dir_open(dir_path, 0, NULL);

	if (dir)
	{
		const char *entry;

		while ((entry = g_dir_read_name(dir)) != NULL)
		{
			char *path = g_build_filename(dir_path, entry, "active_time_ms", NULL);
			char *contents = NULL;
			char *name = NULL;

			if (g_file_get_contents(path, &contents, NULL, NULL) &&
			        g_ascii_strtoll(contents, NULL, 10) > 0)
			{
				char *name_path = g_build_filename(dir_path, entry, "name", NULL);
				g_file_get_contents(name_path, &name, NULL, NULL);
				g_free(name_path);
			}

			if (name)
			{
				g_strchomp(name);
				g_strlcat(out, out[0] ? "," : "", len);
				g_strlcat(out, name, len);
			}

			g_free(name);
			g_free(contents);
			g_free(path);
		}

		g_dir_close(dir);
		return;
	}

	f = fopen("/sys/kernel/debug/wakeup_sources", "r");

	if (!f)
	{
		return;
	}

	/* name active_count event_count wakeup_count expire_count active_since ... */
	while (fgets(line, sizeof(line), f))
	{
		char name[128];
		unsigned long long active_since;

		if (sscanf(line, "%127s %*u %*u %*u %*u %llu", name, &active_since) == 2 &&
		        active_since != 0)
		{
			g_strlcat(out, out[0] ? "," : "", len);
			g_strlcat(out, name, len);
		}
	}

	fclose(f);
}


/*
 * What ended the last sleep, for the resume log line: the IRQ the kernel
 * recorded in /sys/power/pm_wakeup_irq (cleared on each suspend; ENODATA when
 * the wake was not an IRQ the core saw, e.g. an alarm through the RTC's own
 * path) named through /proc/interrupts, plus whatever wakeup sources are
 * still active. Best effort, empty when nothing is readable. Measured need:
 * on tissot the kernel names only the fuel gauge's wakes itself, and 170 of
 * 203 resumes in a four-hour run went unexplained.
 */
static void describe_wake(char *out, size_t len)
{
	char *irq = NULL;
	char *table = NULL;
	char active[256];

	out[0] = '\0';

	if (g_file_get_contents("/sys/power/pm_wakeup_irq", &irq, NULL, NULL) && irq[0])
	{
		const char *name = NULL;
		char **lines = NULL;
		gint i;

		g_strchomp(irq);
		if (g_file_get_contents("/proc/interrupts", &table, NULL, NULL))
		{
			lines = g_strsplit(table, "\n", -1);
			for (i = 0; lines && lines[i]; i++)
			{
				char *line = g_strchug(lines[i]);
				size_t n = strlen(irq);

				if (strncmp(line, irq, n) == 0 && line[n] == ':')
				{
					/* the action name is the last field on the line */
					char *last = strrchr(g_strchomp(line), ' ');
					name = last ? last + 1 : line;
					break;
				}
			}
		}
		g_snprintf(out, len, "wake irq %s (%s)", irq, name ? name : "?");
		g_strfreev(lines);
	}
	g_free(table);
	g_free(irq);

	active_wakeup_sources(active, sizeof(active));
	if (active[0])
	{
		g_strlcat(out, out[0] ? "; active: " : "active: ", len);
		g_strlcat(out, active, len);
	}
}

static double boottime_now(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
	{
		return 0.0;
	}

	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * Which component performs the final kernel write.
 *
 * On a mainline phone the daemons that must prepare hardware for sleep only
 * hear about a suspend from systemd-logind: eg25-manager (Quectel modem) holds
 * a logind "delay" inhibitor and runs its AT sequence on PrepareForSleep, and
 * ModemManager, NetworkManager and the system-sleep hooks work the same way.
 * A direct write to /sys/power/state is invisible to all of them; measured on
 * a PinePhone Pro that meant the modem re-enumerated on USB after every
 * suspend and woke the phone again three seconds later, 1027 times in an
 * hour, where one suspend through logind slept 52 minutes untouched.
 *
 * sleepd keeps the policy, the RTC alarm and the wakeup_count handshake; only
 * the write moves. The handshake still protects the logind path: the count
 * written back is what the kernel checks when logind's helper enters suspend,
 * so an event that races the inhibitor phase still aborts the attempt.
 *
 * [module.system] suspend_backend = auto | kernel | logind (nyx.conf).
 * "auto" (the default) uses logind when the machine has no Android container
 * (Halium prepares its own side) and at least one logind sleep-delay
 * inhibitor is registered, i.e. someone is actually listening.
 */
#define LOGIND_NAME   "org.freedesktop.login1"
#define LOGIND_PATH   "/org/freedesktop/login1"
#define LOGIND_IFACE  "org.freedesktop.login1.Manager"
#define HALIUM_CONTAINER_CONFIG "/var/lib/lxc/android/config"
/* logind has InhibitDelayMaxSec (5 s by default) to run the inhibitors. */
#define LOGIND_ENTRY_TIMEOUT_MS 20000

enum suspend_backend { BACKEND_KERNEL, BACKEND_LOGIND };

static bool logind_has_sleep_delay_inhibitor(GDBusConnection *bus)
{
	GError *err = NULL;
	GVariant *reply;
	GVariantIter *iter;
	const char *what, *who, *why, *mode;
	guint32 uid, pid;
	bool found = false;

	reply = g_dbus_connection_call_sync(bus, LOGIND_NAME, LOGIND_PATH, LOGIND_IFACE,
	                                    "ListInhibitors", NULL,
	                                    G_VARIANT_TYPE("(a(ssssuu))"),
	                                    G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
	if (!reply)
	{
		nyx_debug("system: logind ListInhibitors failed: %s", err ? err->message : "?");
		g_clear_error(&err);
		return false;
	}

	g_variant_get(reply, "(a(ssssuu))", &iter);
	while (g_variant_iter_loop(iter, "(&s&s&s&suu)", &what, &who, &why, &mode, &uid, &pid))
	{
		if (strstr(what, "sleep") && g_strcmp0(mode, "delay") == 0)
		{
			nyx_debug("system: logind sleep-delay inhibitor held by %s (%s)", who, why);
			found = true;
		}
	}
	g_variant_iter_free(iter);
	g_variant_unref(reply);
	return found;
}

static enum suspend_backend choose_suspend_backend(GDBusConnection **bus_out)
{
	gchar *conf = nyx_conf_get_path("module.system", "suspend_backend");
	enum suspend_backend backend = BACKEND_KERNEL;
	GDBusConnection *bus = NULL;
	GError *err = NULL;

	*bus_out = NULL;

	if (conf && g_strcmp0(conf, "kernel") == 0)
	{
		g_free(conf);
		return BACKEND_KERNEL;
	}

	if (!conf || g_strcmp0(conf, "auto") == 0)
	{
		if (access(HALIUM_CONTAINER_CONFIG, F_OK) == 0)
		{
			g_free(conf);
			return BACKEND_KERNEL;
		}
	}
	else if (g_strcmp0(conf, "logind") != 0)
	{
		nyx_info(MSGID_NYX_MOD_SYSTEM_SUSPEND, 0,
		         "unknown suspend_backend '%s', using the kernel directly", conf);
		g_free(conf);
		return BACKEND_KERNEL;
	}

	bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!bus)
	{
		nyx_info(MSGID_NYX_MOD_SYSTEM_SUSPEND, 0,
		         "system bus unavailable (%s), suspending through the kernel",
		         err ? err->message : "?");
		g_clear_error(&err);
		g_free(conf);
		return BACKEND_KERNEL;
	}

	/* "logind" forces it; "auto" wants a listener to justify the detour. */
	if ((conf && g_strcmp0(conf, "logind") == 0) || logind_has_sleep_delay_inhibitor(bus))
	{
		backend = BACKEND_LOGIND;
		*bus_out = bus;
	}
	else
	{
		g_object_unref(bus);
	}
	g_free(conf);
	return backend;
}

static long read_suspend_success_count(void)
{
	char *contents = NULL;
	long n = -1;

	if (g_file_get_contents("/sys/power/suspend_stats/success", &contents, NULL, NULL))
	{
		n = strtol(contents, NULL, 10);
	}
	else if (g_file_get_contents("/sys/kernel/debug/suspend_stats", &contents, NULL, NULL))
	{
		char *p = strstr(contents, "success:");
		if (p)
		{
			n = strtol(p + strlen("success:"), NULL, 10);
		}
	}
	g_free(contents);
	return n;
}

struct logind_wait
{
	GMainLoop *loop;
	int phase;        /* 0: waiting for PrepareForSleep(true), 1: asleep, 2: resumed */
	bool timed_out;
};

static void on_prepare_for_sleep(GDBusConnection *bus, const gchar *sender,
                                 const gchar *path, const gchar *iface,
                                 const gchar *signal, GVariant *params, gpointer data)
{
	struct logind_wait *w = data;
	gboolean start = FALSE;

	g_variant_get(params, "(b)", &start);
	if (start)
	{
		w->phase = 1;
	}
	else if (w->phase == 1)
	{
		w->phase = 2;
		g_main_loop_quit(w->loop);
	}
}

static gboolean on_logind_entry_timeout(gpointer data)
{
	struct logind_wait *w = data;

	if (w->phase == 0)
	{
		w->timed_out = true;
		g_main_loop_quit(w->loop);
	}
	return G_SOURCE_REMOVE;
}

/*
 * Ask logind to suspend and block until it reports the resume. Returns 0 when
 * the kernel counted a successful suspend, -EAGAIN when logind went through
 * the motions but the kernel aborted (a wakeup raced the inhibitor phase), or
 * -EIO when logind could not be asked at all (the caller then falls back to
 * the kernel write; the handshake is still valid).
 */
static int suspend_via_logind(GDBusConnection *bus, double *asleep)
{
	GMainContext *ctx = g_main_context_new();
	struct logind_wait w = { .phase = 0, .timed_out = false };
	GError *err = NULL;
	GVariant *reply;
	GSource *timeout;
	guint sub;
	long before, after;
	double t0;
	int ret;

	g_main_context_push_thread_default(ctx);
	w.loop = g_main_loop_new(ctx, FALSE);

	/* Subscribed on this thread's context, so the loop below delivers it. */
	sub = g_dbus_connection_signal_subscribe(bus, LOGIND_NAME, LOGIND_IFACE,
	                                         "PrepareForSleep", LOGIND_PATH, NULL,
	                                         G_DBUS_SIGNAL_FLAGS_NONE,
	                                         on_prepare_for_sleep, &w, NULL);

	before = read_suspend_success_count();
	t0 = boottime_now();

	reply = g_dbus_connection_call_sync(bus, LOGIND_NAME, LOGIND_PATH, LOGIND_IFACE,
	                                    "Suspend", g_variant_new("(b)", FALSE), NULL,
	                                    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
	if (!reply)
	{
		nyx_info(MSGID_NYX_MOD_SYSTEM_SUSPEND, 0,
		         "logind Suspend refused: %s", err ? err->message : "?");
		g_clear_error(&err);
		ret = -EIO;
		goto out;
	}
	g_variant_unref(reply);

	timeout = g_timeout_source_new(LOGIND_ENTRY_TIMEOUT_MS);
	g_source_set_callback(timeout, on_logind_entry_timeout, &w, NULL);
	g_source_attach(timeout, ctx);
	g_source_unref(timeout);

	/* Runs through the inhibitor phase, the sleep itself, and the resume. */
	g_main_loop_run(w.loop);

	if (w.timed_out)
	{
		nyx_info(MSGID_NYX_MOD_SYSTEM_SUSPEND, 0,
		         "not suspended: logind did not enter sleep within %d ms",
		         LOGIND_ENTRY_TIMEOUT_MS);
		ret = -EAGAIN;
		goto out;
	}

	after = read_suspend_success_count();
	*asleep = boottime_now() - t0;
	if (before >= 0 && after >= 0 && after <= before)
	{
		nyx_info(MSGID_NYX_MOD_SYSTEM_SUSPEND, 0,
		         "not suspended: logind ran the sleep but the kernel aborted it");
		ret = -EAGAIN;
		goto out;
	}
	ret = 0;

out:
	g_dbus_connection_signal_unsubscribe(bus, sub);
	g_main_loop_unref(w.loop);
	g_main_context_pop_thread_default(ctx);
	g_main_context_unref(ctx);
	return ret;
}

static nyx_error_t suspend_blocking(nyx_device_handle_t handle, bool *success)
{
	char count[32];
	double t0;
	int ret;
	enum suspend_backend backend;
	GDBusConnection *bus = NULL;

	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (success)
	{
		*success = false;
	}

	ret = read_wakeup_count(count, sizeof(count), WAKEUP_COUNT_WAIT_MS);

	if (ret == -ETIMEDOUT)
	{
		char active[256];

		active_wakeup_sources(active, sizeof(active));
		nyx_info(MSGID_NYX_MOD_SYSTEM_SUSPEND, 0,
		         "not suspended: wakeup source still active after %u ms: %s",
		         WAKEUP_COUNT_WAIT_MS, active[0] ? active : "(unknown)");
		return NYX_ERROR_NONE;
	}
	else if (ret == -ENOENT)
	{
		/* No wakeup_count on this kernel: no handshake possible, suspend blind. */
		nyx_info(MSGID_NYX_MOD_SYSTEM_SUSPEND, 0,
		         "no " SYSFS_WAKEUP_COUNT ", suspending without the handshake");
	}
	else if (ret < 0)
	{
		nyx_info(MSGID_NYX_MOD_SYSTEM_SUSPEND, 0,
		         "not suspended: reading " SYSFS_WAKEUP_COUNT " failed: %s (%d)",
		         strerror(-ret), -ret);
		return NYX_ERROR_NONE;
	}
	else
	{
		ret = write_sysfs_string(SYSFS_WAKEUP_COUNT, count);

		if (ret < 0)
		{
			/* EBUSY (EINVAL on old kernels): a wakeup event raced us. */
			nyx_info(MSGID_NYX_MOD_SYSTEM_SUSPEND, 0,
			         "not suspended: wakeup_count %s changed under us: %s (%d)",
			         count, strerror(-ret), -ret);
			return NYX_ERROR_NONE;
		}
	}

	backend = choose_suspend_backend(&bus);
	if (backend == BACKEND_LOGIND)
	{
		double asleep = 0.0;

		ret = suspend_via_logind(bus, &asleep);
		g_object_unref(bus);
		if (ret == -EAGAIN)
		{
			return NYX_ERROR_NONE;
		}
		if (ret == 0)
		{
			char wake[320];

			describe_wake(wake, sizeof(wake));
			nyx_info(MSGID_NYX_MOD_SYSTEM_SUSPEND, 0,
			         "suspended through logind and resumed after %.1f s%s%s", asleep,
			         wake[0] ? ": " : "", wake);
			if (success)
			{
				*success = true;
			}
			return NYX_ERROR_NONE;
		}
		/* logind could not be asked: the handshake still holds, write ourselves. */
	}

	t0 = boottime_now();
	ret = write_sysfs_string(SYSFS_POWER_STATE, "mem");

	if (ret < 0)
	{
		nyx_info(MSGID_NYX_MOD_SYSTEM_SUSPEND, 0,
		         "not suspended: writing mem to " SYSFS_POWER_STATE " failed: %s (%d)",
		         strerror(-ret), -ret);
		return NYX_ERROR_NONE;
	}

	{
		char wake[320];
		double asleep = boottime_now() - t0;

		describe_wake(wake, sizeof(wake));
		nyx_info(MSGID_NYX_MOD_SYSTEM_SUSPEND, 0,
		         "suspended and resumed after %.1f s%s%s", asleep,
		         wake[0] ? ": " : "", wake);
	}

	if (success)
	{
		*success = true;
	}

	return NYX_ERROR_NONE;
}

nyx_error_t system_suspend(nyx_device_handle_t handle, bool *success)
{
	return suspend_blocking(handle, success);
}

nyx_error_t system_suspend_async(nyx_device_handle_t handle, bool *success)
{
	return suspend_blocking(handle, success);
}

/*
 * Nothing to undo after a one-shot suspend. Disarm autosleep defensively, only
 * where the node exists: an image whose previous nyx build armed it, or anything
 * else that did, would otherwise leave the device unable to stay awake.
 */
nyx_error_t system_resume(nyx_device_handle_t handle, bool *success)
{
	int ret;

	if (handle != nyxDev)
	{
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (access(SYSFS_AUTOSLEEP, W_OK) == 0)
	{
		ret = write_sysfs_string(SYSFS_AUTOSLEEP, "off");

		if (ret < 0)
		{
			nyx_info(MSGID_NYX_MOD_SYSTEM_SUSPEND, 0,
			         "disarming " SYSFS_AUTOSLEEP " failed: %s (%d)",
			         strerror(-ret), -ret);
		}
	}

	if (success)
	{
		*success = true;
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
