// Copyright (c) 2026 LuneOS
// SPDX-License-Identifier: Apache-2.0

/*
 * Host-side test for the charger module's edge detection: the "last
 * notified" snapshot, the settle re-reads, connect debouncing and the
 * USB-family supply scan. Drives the module against a temporary sysfs
 * lookalike, so nothing here needs a device or root.
 *
 * Build and run: src/charger/tests/run-host-tests.sh
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>

PmLogContext getNyxContext(void) { return NULL; }

/* The module reads these through nyx_conf.h / charger.c macros. */
static char *test_root = NULL;
static char *test_conf = NULL;
#define NYX_CONF_FILE test_conf
#define POWER_SUPPLY_SYSFS_ROOT test_root

nyx_device_t *nyxDev = NULL;
void *charger_status_callback_context = NULL;
void *state_change_callback_context = NULL;
nyx_device_callback_function_t charger_status_callback = NULL;
nyx_device_callback_function_t state_change_callback = NULL;

#include "../charger.c"

/* nyx-lib's reader, reimplemented so the test links without nyx-lib. */
int32_t nyx_utils_read_value(char *path)
{
	char buf[32];
	int fd, r;

	if (!path || !*path)
		return -1;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -1;

	buf[r] = '\0';
	return (int32_t) strtol(buf, NULL, 10);
}

/* Fixture helpers */
static void supply_write(const char *supply, const char *attr, const char *value)
{
	gchar *dir = g_build_filename(test_root, supply, NULL);
	gchar *path = g_build_filename(dir, attr, NULL);

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_assert_true(g_file_set_contents(path, value, -1, NULL));
	g_free(path);
	g_free(dir);
}

static int status_cb_count = 0;
static int state_cb_count = 0;
static bool last_notified_connected = false;

static void on_status(nyx_device_handle_t h, nyx_callback_status_t s, void *ctx)
{
	status_cb_count++;
	last_notified_connected = gChargerStatus.is_charging;
}

static void on_state(nyx_device_handle_t h, nyx_callback_status_t s, void *ctx)
{
	state_cb_count++;
}

/* What the uevent handler does for one event, minus libudev. */
static void simulate_uevent(const char *sysname)
{
	_charger_evaluate(sysname, false);
	_charger_arm_settle();
}

static gboolean quit_loop(gpointer data)
{
	g_main_loop_quit((GMainLoop *) data);
	return G_SOURCE_REMOVE;
}

static void run_for(int ms)
{
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);

	g_timeout_add(ms, quit_loop, loop);
	g_main_loop_run(loop);
	g_main_loop_unref(loop);
}

static void fixture_setup(void)
{
	GError *err = NULL;

	test_root = g_dir_make_tmp("nyx-charger-XXXXXX", &err);
	g_assert_no_error(err);
	test_conf = g_build_filename(test_root, "nyx.conf", NULL);

	/* sargo-shaped: pc_port never emits uevents, usb does, both are USB-family */
	supply_write("pc_port", "type", "USB\n");
	supply_write("pc_port", "online", "1\n");
	supply_write("usb", "type", "USB_PD\n");
	supply_write("usb", "online", "0\n");
	supply_write("main", "type", "Main\n");
	supply_write("main", "online", "1\n");
	supply_write("battery", "type", "Battery\n");
	supply_write("battery", "present", "1\n");
	supply_write("battery", "status", "Charging\n");

	gchar *conf = g_strdup_printf(
	                  "[module.battery]\nsysfs_path=%s/battery\n"
	                  "[module.charger]\nusb_sysfs_path=%s/pc_port\nac_sysfs_path=%s/usb\n"
	                  "settle_ms=50\nresettle_ms=120\n",
	                  test_root, test_root, test_root);
	g_assert_true(g_file_set_contents(test_conf, conf, -1, NULL));
	g_free(conf);

	_detect_charger_sysfs_paths();
	_charger_read_settle_conf();
	_scan_usb_family_supplies(POWER_SUPPLY_SYSFS_ROOT);
	core_charger_read_status(NULL);
	curr_battery_state = calloc(1, sizeof(nyx_battery_status_t));
	battery_status = calloc(1, STATUS_LEN);
	_battery_read_status();
	_charger_init_events();

	charger_status_callback = on_status;
	state_change_callback = on_state;
	status_cb_count = 0;
	state_cb_count = 0;
	last_notified_connected = gChargerStatus.is_charging;
	g_assert_true(notified_charging);
	g_assert_cmpint(settle_ms, ==, 50);
	g_assert_cmpint(resettle_ms, ==, 120);
}

static void fixture_teardown(void)
{
	_charger_cancel_settle();
	free(curr_battery_state);
	curr_battery_state = NULL;
	free(battery_status);
	battery_status = NULL;
	g_ptr_array_free(usb_family_online_paths, TRUE);
	usb_family_online_paths = NULL;
	charger_status_callback = NULL;
	state_change_callback = NULL;
	notified_valid = false;

	gchar *cmd = g_strdup_printf("rm -rf '%s'", test_root);
	g_assert_cmpint(system(cmd), ==, 0);
	g_free(cmd);
	g_free(test_root);
	g_free(test_conf);
}

/*
 * A client query between the sysfs transition and the uevent must not
 * consume the edge. This is the case that lost disconnects on sargo when
 * anything polled chargerStatusQuery.
 */
static void test_query_between_transition_and_uevent(void)
{
	nyx_charger_status_t st;

	fixture_setup();

	supply_write("pc_port", "online", "0\n");
	g_assert_cmpint(core_charger_read_status(&st), ==, NYX_ERROR_NONE);   /* the query */
	g_assert_false(st.is_charging);
	g_assert_cmpint(status_cb_count, ==, 0);

	simulate_uevent("usb");
	g_assert_cmpint(status_cb_count, ==, 1);
	g_assert_false(last_notified_connected);
	g_assert_true(current_event & NYX_CHARGER_DISCONNECTED);
	g_assert_false(current_event & NYX_CHARGER_CONNECTED);

	run_for(300);
	g_assert_cmpint(status_cb_count, ==, 1);   /* settle re-reads add nothing */

	fixture_teardown();
}

/*
 * The uevent comes from a supply we do not read, and the one we do read
 * has not caught up yet. Nothing else will ever announce it; the settle
 * re-read has to.
 */
static void test_disconnect_after_uevent_settles(void)
{
	fixture_setup();

	simulate_uevent("usb");                 /* pc_port still 1 */
	g_assert_cmpint(status_cb_count, ==, 0);

	supply_write("pc_port", "online", "0\n");   /* settles, no further uevent */
	run_for(80);
	g_assert_cmpint(status_cb_count, ==, 1);
	g_assert_false(last_notified_connected);

	run_for(200);
	g_assert_cmpint(status_cb_count, ==, 1);

	fixture_teardown();
}

/* A connect that does not hold for settle_ms is not reported at all. */
static void test_transient_connect_is_ignored(void)
{
	fixture_setup();

	supply_write("pc_port", "online", "0\n");
	simulate_uevent("usb");
	g_assert_cmpint(status_cb_count, ==, 1);
	run_for(300);
	g_assert_cmpint(status_cb_count, ==, 1);

	/* the smb5 "usb" node blips online after the cable is out */
	supply_write("usb", "online", "1\n");
	simulate_uevent("usb");
	g_assert_cmpint(status_cb_count, ==, 1);
	supply_write("usb", "online", "0\n");
	run_for(300);
	g_assert_cmpint(status_cb_count, ==, 1);
	g_assert_false(last_notified_connected);

	/* a real connect is reported once it has held */
	supply_write("usb", "online", "1\n");
	simulate_uevent("usb");
	g_assert_cmpint(status_cb_count, ==, 1);
	run_for(80);
	g_assert_cmpint(status_cb_count, ==, 2);
	g_assert_true(last_notified_connected);
	g_assert_true(current_event & NYX_CHARGER_CONNECTED);
	g_assert_true(gChargerStatus.connected & NYX_CHARGER_WALL_CONNECTED);

	fixture_teardown();
}

/* A USB-family supply nobody configured still counts as a wired charger. */
static void test_usb_family_supply_counts(void)
{
	nyx_charger_status_t st;

	fixture_setup();

	/* drop the ac slot so "usb" is only found by the scan */
	charger_ac_sysfs_online_path[0] = '\0';
	charger_ac_sysfs_current_max_path[0] = '\0';
	_scan_usb_family_supplies(POWER_SUPPLY_SYSFS_ROOT);
	g_assert_cmpint(usb_family_online_paths->len, ==, 1);

	supply_write("pc_port", "online", "0\n");
	supply_write("usb", "online", "1\n");
	core_charger_read_status(&st);
	g_assert_true(st.is_charging);
	g_assert_true(st.connected & NYX_CHARGER_WALL_CONNECTED);

	supply_write("usb", "online", "0\n");
	core_charger_read_status(&st);
	g_assert_false(st.is_charging);
	g_assert_cmpint(st.connected, ==, 0);

	fixture_teardown();
}

/* The configured USB slot still classifies by usb_type when it is live. */
static void test_configured_slot_wins(void)
{
	nyx_charger_status_t st;

	fixture_setup();
	supply_write("pc_port", "usb_type", "Unknown [SDP] CDP DCP\n");
	supply_write("pc_port", "current_max", "500000\n");
	_detect_charger_sysfs_paths();   /* optional attributes are resolved here */
	core_charger_read_status(&st);
	g_assert_true(st.powered & NYX_CHARGER_USB_POWERED);
	g_assert_true(st.connected & NYX_CHARGER_PC_CONNECTED);
	g_assert_cmpint(st.charger_max_current, ==, 500);
	fixture_teardown();
}

/*
 * "online" is a small integer, not a flag: MediaTek's mt6375 charger on the
 * MP01 reports 2 (online, programmable) for a plain USB cable, and that
 * used to read as "no charger".
 */
static void test_online_two_is_connected(void)
{
	nyx_charger_status_t st;

	fixture_setup();

	supply_write("pc_port", "online", "2\n");
	g_assert_cmpint(core_charger_read_status(&st), ==, NYX_ERROR_NONE);
	g_assert_true(st.is_charging);
	g_assert_true(st.connected & NYX_CHARGER_PC_CONNECTED ||
	              st.connected & NYX_CHARGER_WALL_CONNECTED);

	supply_write("pc_port", "online", "0\n");
	g_assert_cmpint(core_charger_read_status(&st), ==, NYX_ERROR_NONE);
	g_assert_false(st.is_charging);

	fixture_teardown();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/charger/query-between-transition-and-uevent",
	                test_query_between_transition_and_uevent);
	g_test_add_func("/charger/disconnect-after-uevent-settles",
	                test_disconnect_after_uevent_settles);
	g_test_add_func("/charger/transient-connect-is-ignored",
	                test_transient_connect_is_ignored);
	g_test_add_func("/charger/usb-family-supply-counts",
	                test_usb_family_supply_counts);
	g_test_add_func("/charger/online-two-is-connected",
	                test_online_two_is_connected);
	g_test_add_func("/charger/configured-slot-wins",
	                test_configured_slot_wins);
	return g_test_run();
}
