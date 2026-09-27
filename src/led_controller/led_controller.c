// Copyright (c) 2013 Simon Busch <morphis@gravedo.de>
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
 * One LED-controller module for every machine, probed at runtime.
 *
 * This used to be two modules installed under the same filename - a sysfs one
 * here and an Android one in nyx-modules-hybris - with NYXMOD_OW_LED choosing
 * between them per machine. That split cost every device something. The Android
 * one was the only one that could reach a notification LED, so mainline devices
 * with a perfectly good multicolour LED in /sys/class/leds (PinePhone and
 * PinePhone Pro) had none. The sysfs one was the only one that could find a
 * backlight in the "backlight" udev class, so it was the only one that worked on
 * mainline. And on a device like the Zinwa Q25 the Android one won, could not
 * load a legacy lights HAL at all (Android 12+ ships the android.hardware.light
 * AIDL service and no lights.<board>.so), and left the RGB LED the kernel was
 * offering it untouched.
 *
 * So the backends live side by side here and are chosen per light, at runtime:
 *
 *   Android lights HAL   dlopen()ed, never linked - see lights_hal_load()
 *   sysfs LED class      /sys/class/leds/<name>/brightness
 *   sysfs backlight      the "backlight" udev class, for mainline panels
 *
 * A machine needs no configuration for any of this, which is what lets one
 * module serve all of them - the same reasoning as the touchpanel module, and
 * the reason nothing here may name a device.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <dlfcn.h>
#include <glib.h>
#include <libudev.h>

#include "lights_compat.h"

#include <nyx/nyx_module.h>
#include <nyx/module/nyx_utils.h>
#include <nyx/module/nyx_log.h>
#include "msgid.h"

NYX_DECLARE_MODULE(NYX_DEVICE_LED_CONTROLLER, "LedControllers");

#define LEDS_CLASS_DIR "/sys/class/leds"

/*
 * Display backlights that live in the LED class rather than the backlight one.
 *
 * MTK and most Android kernels put the panel backlight here; mainline puts it in
 * the "backlight" class, which find_backlight_udev() covers instead. Probed in
 * order, first match wins.
 */
static const char *const backlight_leds[] = {
	"lcd-backlight",
	"backlight",
};

/*
 * Keyboard backlights, for devices with a physical keyboard (BlackBerry KEY2 and
 * friends). This is a second light in the same effect - the effect struct
 * carries brightness_lcd and brightness_keypad side by side, and
 * luna-displaymanager fills both on every backlightOn()/backlightOff() - so it
 * is driven from here rather than from a device of its own.
 *
 * "keyboard-backlight" is the name AOSP's reference lights HAL uses for
 * LIGHT_ID_KEYBOARD, and what the athena (KEY2) device tree registers for both
 * LED drivers that can own that light. "kbd_backlight" is the upstream-Linux
 * spelling used by the laptop and Chromebook drivers.
 *
 * Deliberately absent: button-backlight. That is the capacitive navigation row,
 * a different light that happens to sit beside this one in athena's device tree,
 * and driving it from the keyboard brightness would light the wrong thing on
 * every device that has both.
 */
static const char *const keypad_leds[] = {
	"keyboard-backlight",
	"kbd_backlight",
};

/*
 * The notification LED, one list per channel.
 *
 * Bare "red"/"green"/"blue" is what Android device trees register. The
 * ":indicator" and ":status" spellings are what upstream Linux produces from a
 * DT led node's colour and function properties, which is how the PinePhone and
 * PinePhone Pro present theirs. A device with only some of these channels gets
 * those - a single-colour LED still lights, it just cannot change hue.
 */
static const char *const core_leds[3][3] = {
	{ "red",   "red:indicator",   "red:status"   },
	{ "green", "green:indicator", "green:status" },
	{ "blue",  "blue:indicator",  "blue:status"  },
};

enum { CHANNEL_RED = 0, CHANNEL_GREEN, CHANNEL_BLUE, CHANNEL_COUNT };

/* An LED-class or backlight-class node we drive by writing its brightness. */
typedef struct
{
	gchar *brightness;
	gchar *max_brightness;
	gchar *trigger;
	int max;
} sysfs_light_t;

static sysfs_light_t sysfs_backlight;
static sysfs_light_t sysfs_keypad;
static sysfs_light_t sysfs_core[CHANNEL_COUNT];
static bool have_sysfs_core = false;

static const struct hw_module_t *lights_module = NULL;
static struct light_device_t *backlight_device = NULL;
static struct light_device_t *notifications_device = NULL;
static struct light_device_t *keypad_device = NULL;

typedef int (*hw_get_module_fn)(const char *id, const struct hw_module_t **module);
static hw_get_module_fn hw_get_module_p = NULL;

/* ------------------------------------------------------------------ sysfs */

static int sysfs_read_int(const char *path, int fallback)
{
	char buf[32];
	char *end = NULL;
	long value;
	FILE *f;

	if (!path)
		return fallback;

	f = fopen(path, "r");

	if (!f)
		return fallback;

	if (fgets(buf, sizeof(buf), f) == NULL)
	{
		(void) fclose(f);
		return fallback;
	}

	(void) fclose(f);

	value = strtol(buf, &end, 10);

	if (end == buf || value < INT_MIN || value > INT_MAX)
		return fallback;

	return (int) value;
}

static bool sysfs_write_str(const char *path, const char *value)
{
	FILE *f;

	if (!path)
		return false;

	f = fopen(path, "w");

	if (!f)
	{
		nyx_error(MSGID_NYX_MOD_LED_OPENFILE_ERR, 0, "could not open %s for writing: %s",
		          path, strerror(errno));
		return false;
	}

	if (fprintf(f, "%s", value) < 0)
	{
		(void) fclose(f);
		nyx_error(MSGID_NYX_MOD_LED_OPENFILE_ERR, 0, "could not write \"%s\" to %s",
		          value, path);
		return false;
	}

	/*
	 * fclose() is where a sysfs store() actually reports failure - the write is
	 * buffered until then - so an unchecked close would hide, for example, the
	 * EACCES from a node whose permissions no udev rule has relaxed.
	 */
	if (fclose(f) != 0)
	{
		nyx_error(MSGID_NYX_MOD_LED_OPENFILE_ERR, 0, "writing \"%s\" to %s failed: %s",
		          value, path, strerror(errno));
		return false;
	}

	return true;
}

static bool sysfs_write_int(const char *path, int value)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%d", value);
	return sysfs_write_str(path, buf);
}

static bool sysfs_light_valid(const sysfs_light_t *light)
{
	return light->brightness != NULL;
}

/* Take a node if its brightness attribute is there and writable. */
static bool sysfs_light_take(sysfs_light_t *light, const gchar *dir)
{
	gchar *brightness = g_build_filename(dir, "brightness", NULL);
	FILE *f = fopen(brightness, "w");

	if (!f)
	{
		g_free(brightness);
		return false;
	}

	(void) fclose(f);

	light->brightness = brightness;
	light->max_brightness = g_build_filename(dir, "max_brightness", NULL);
	light->trigger = g_build_filename(dir, "trigger", NULL);
	light->max = sysfs_read_int(light->max_brightness, 255);

	if (light->max <= 0)
		light->max = 255;

	return true;
}

static void sysfs_light_release(sysfs_light_t *light)
{
	g_free(light->brightness);
	g_free(light->max_brightness);
	g_free(light->trigger);
	light->brightness = NULL;
	light->max_brightness = NULL;
	light->trigger = NULL;
}

/* First of names[] that this device has under /sys/class/leds. */
static bool sysfs_light_probe_names(sysfs_light_t *light,
                                    const char *const *names, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
	{
		gchar *dir = g_build_filename(LEDS_CLASS_DIR, names[i], NULL);
		bool ok = sysfs_light_take(light, dir);

		g_free(dir);

		if (ok)
			return true;
	}

	return false;
}

/*
 * A percentage, not a raw value: everything above this module speaks 0..100 (see
 * luna-sysmgr's NyxLedControl), and max_brightness differs wildly between nodes.
 */
static bool sysfs_light_set_percent(const sysfs_light_t *light, int percent)
{
	int pct = (percent < 0) ? 0 : (percent > 100) ? 100 : percent;

	return sysfs_write_int(light->brightness, (pct * light->max + 50) / 100);
}

/* A 0..255 channel value, for the notification LED. */
static bool sysfs_light_set_channel(const sysfs_light_t *light, int value)
{
	int v = (value < 0) ? 0 : (value > 255) ? 255 : value;

	return sysfs_write_int(light->brightness, (v * light->max + 127) / 255);
}

/* --------------------------------------------------- backlight, udev class */

static gchar *backlight_udev_by_type(struct udev *udev,
                                     struct udev_list_entry *devices,
                                     const char *type)
{
	gchar *path = NULL;
	struct udev_list_entry *l;

	for (l = devices; l != NULL; l = udev_list_entry_get_next(l))
	{
		struct udev_device *device =
		    udev_device_new_from_syspath(udev, udev_list_entry_get_name(l));

		if (device == NULL)
			continue;

		if (g_strcmp0(udev_device_get_sysattr_value(device, "type"), type) == 0)
		{
			path = g_strdup(udev_device_get_syspath(device));
			udev_device_unref(device);
			break;
		}

		udev_device_unref(device);
	}

	return path;
}

/*
 * The mainline panel backlight.
 *
 * Preferred by type in the order the kernel documents as most to least
 * authoritative, so a device exposing more than one (a firmware interface beside
 * a raw PWM, say) gets the one that actually drives the panel.
 */
static gchar *find_backlight_udev(void)
{
	struct udev *udev;
	struct udev_enumerate *enumerator;
	struct udev_list_entry *devices;
	gchar *path = NULL;
	size_t i;
	static const char *const types[] = { "firmware", "platform", "raw" };

	udev = udev_new();

	if (!udev)
	{
		nyx_error(MSGID_NYX_MOD_UDEV_ERR, 0, "Could not initialize udev component");
		return NULL;
	}

	enumerator = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(enumerator, "backlight");
	udev_enumerate_scan_devices(enumerator);

	devices = udev_enumerate_get_list_entry(enumerator);

	for (i = 0; devices != NULL && i < G_N_ELEMENTS(types) && path == NULL; i++)
		path = backlight_udev_by_type(udev, devices, types[i]);

	udev_enumerate_unref(enumerator);
	udev_unref(udev);

	return path;
}

static bool sysfs_backlight_probe(void)
{
	gchar *udev_path;

	if (sysfs_light_probe_names(&sysfs_backlight, backlight_leds,
	                            G_N_ELEMENTS(backlight_leds)))
		return true;

	udev_path = find_backlight_udev();

	if (udev_path)
	{
		bool ok = sysfs_light_take(&sysfs_backlight, udev_path);

		g_free(udev_path);

		if (ok)
			return true;
	}

	return false;
}

/*
 * The notification LED. Every channel is optional, and any one of them is enough
 * to have a light worth driving.
 */
static bool sysfs_core_probe(void)
{
	int c;
	bool any = false;

	for (c = 0; c < CHANNEL_COUNT; c++)
	{
		if (sysfs_light_probe_names(&sysfs_core[c], core_leds[c],
		                            G_N_ELEMENTS(core_leds[c])))
			any = true;
	}

	return any;
}

/* ----------------------------------------------------- Android lights HAL */

/*
 * hw_get_module() comes from libhybris' libhardware, which exists only on a
 * halium machine. Resolving it at runtime instead of linking it is what lets one
 * module serve every machine: a mainline build simply finds no library and uses
 * sysfs, with no build-time dependency on libhybris and nothing per-machine to
 * configure.
 *
 * The handle is deliberately never dlclose()d, and this module is linked
 * -Wl,-z,nodelete for the same reason: the blob this pulls in registers state
 * that outlives any close, and unloading afterwards segfaults. On tissot every
 * nyx_device_close() on the LED controller died that way, long after the light
 * had been set, reading as an unrelated crash.
 */
static bool lights_hal_load(void)
{
	static bool done = false;
	void *lib;

	if (done)
		return lights_module != NULL;

	done = true;

	lib = dlopen("libhardware.so", RTLD_LAZY);

	if (!lib)
	{
		nyx_debug("No libhardware (%s); this machine has no Android lights HAL",
		          dlerror());
		return false;
	}

	hw_get_module_p = (hw_get_module_fn) dlsym(lib, "hw_get_module");

	if (!hw_get_module_p)
	{
		nyx_debug("libhardware has no hw_get_module; not using the lights HAL");
		return false;
	}

	hw_get_module_p(LIGHTS_HARDWARE_MODULE_ID, &lights_module);

	if (!lights_module)
	{
		/*
		 * Not an error. Android 12 and later ship the android.hardware.light
		 * AIDL service and no legacy lights.<board>.so for hw_get_module() to
		 * find, so this is the normal answer on a recent base - and the sysfs
		 * backends below are what such a device wants anyway.
		 */
		nyx_debug("No legacy Android lights module; using sysfs lights");
	}

	return lights_module != NULL;
}

static struct light_device_t *lights_hal_open(const char *id, bool optional)
{
	struct light_device_t *device = NULL;

	if (!lights_module)
		return NULL;

	lights_module->methods->open(lights_module, id,
	                             (struct hw_device_t **) &device);

	if (!device)
	{
		if (optional)
			nyx_debug("No light device for id %s; this device has none", id);
		else
			nyx_error(MSGID_NYX_MOD_LED_NODEVICE_ERR, 0,
			          "Failed to open light device (id %s)", id);

		return NULL;
	}

	return device;
}

static void lights_hal_close(struct light_device_t *device)
{
	if (!device)
		return;

	/*
	 * common.close is supplied by the vendor blob, and plenty of lights HALs
	 * leave it NULL - the legacy HAL never required it. Calling it
	 * unconditionally segfaults.
	 */
	if (device->common.close)
		device->common.close((struct hw_device_t *) device);
}

static bool lights_hal_set(struct light_device_t *device, int r, int g, int b,
                           int ms_on, int ms_off)
{
	struct light_state_t state;

	if (!device)
		return false;

	memset(&state, 0, sizeof(state));
	state.color = (0xffu << 24) | ((unsigned) r << 16) | ((unsigned) g << 8) |
	              (unsigned) b;
	state.brightnessMode = BRIGHTNESS_MODE_USER;

	if (ms_on > 0 && ms_off > 0)
	{
		state.flashMode = LIGHT_FLASH_TIMED;
		state.flashOnMS = ms_on;
		state.flashOffMS = ms_off;
	}
	else
	{
		state.flashMode = LIGHT_FLASH_NONE;
	}

	if (device->set_light(device, &state) < 0)
	{
		nyx_error(MSGID_NYX_MOD_LED_OPENFILE_ERR, 0,
		          "Failed to set light (colour %d,%d,%d)", r, g, b);
		return false;
	}

	return true;
}

/* The HAL takes a greyscale colour for a brightness-only light. */
static bool lights_hal_set_brightness(struct light_device_t *device, int level)
{
	int v = (level < 0) ? 0 : (level > 255) ? 255 : level;

	return lights_hal_set(device, v, v, v, 0, 0);
}

/* ------------------------------------------------------ notification LED */

/*
 * Drive the notification LED from sysfs.
 *
 * ms_on/ms_off both positive asks for a blink. The LED class can do that in the
 * kernel through the "timer" trigger, which keeps blinking without this module
 * having to wake up; a node without that trigger falls back to a steady colour,
 * which is better than a light that never comes on. Switching a trigger resets
 * the node's brightness, so the trigger is chosen first and the colour written
 * after.
 */
static bool sysfs_core_set(int r, int g, int b, int ms_on, int ms_off)
{
	const int channel[CHANNEL_COUNT] = { r, g, b };
	bool blink = (ms_on > 0 && ms_off > 0);
	bool ok = true;
	int c;

	for (c = 0; c < CHANNEL_COUNT; c++)
	{
		if (!sysfs_light_valid(&sysfs_core[c]))
			continue;

		/*
		 * A channel that is off stays off rather than blinking at zero, so a
		 * red pulse does not also arm the green and blue timers.
		 */
		if (blink && channel[c] > 0)
		{
			if (sysfs_write_str(sysfs_core[c].trigger, "timer"))
			{
				gchar *dir = g_path_get_dirname(sysfs_core[c].brightness);
				gchar *on = g_build_filename(dir, "delay_on", NULL);
				gchar *off = g_build_filename(dir, "delay_off", NULL);

				(void) sysfs_write_int(on, ms_on);
				(void) sysfs_write_int(off, ms_off);

				g_free(on);
				g_free(off);
				g_free(dir);
			}
		}
		else
		{
			(void) sysfs_write_str(sysfs_core[c].trigger, "none");
		}

		if (!sysfs_light_set_channel(&sysfs_core[c], channel[c]))
			ok = false;
	}

	return ok;
}

/*
 * Resolve an effect's colour into three channels.
 *
 * A caller that set no colour gets greyscale, which is what this module produced
 * before colour existed, so CoreNaviLeds and every other brightness-only caller
 * is unaffected. A caller that set one gets it scaled by brightness, the
 * contract documented in nyx_led_controller_core_configuration.h: colour is the
 * hue, brightness stays the intensity control.
 */
static void resolve_colour(nyx_led_controller_core_configuration_handle_t config,
                           int brightness, int *r, int *g, int *b)
{
	static const nyx_led_controller_parameter_type_t param[CHANNEL_COUNT] = {
		NYX_LED_CONTROLLER_CORE_EFFECT_COLOUR_RED,
		NYX_LED_CONTROLLER_CORE_EFFECT_COLOUR_GREEN,
		NYX_LED_CONTROLLER_CORE_EFFECT_COLOUR_BLUE,
	};
	/*
	 * Seeded before the call, not after a failed one. nyx-lib's get_param ends
	 * its switch with "default: break;" and returns NYX_ERROR_NONE for a
	 * parameter it does not recognise, without touching *value - so against a
	 * nyx-lib older than the one that added these, the channels would keep
	 * whatever was on the stack, any_set would go true on garbage, and garbage
	 * would be scaled and handed out as a colour.
	 */
	int32_t channel[CHANNEL_COUNT] = {
		NYX_LED_CONTROLLER_CORE_COLOUR_UNSET,
		NYX_LED_CONTROLLER_CORE_COLOUR_UNSET,
		NYX_LED_CONTROLLER_CORE_COLOUR_UNSET,
	};
	bool any_set = false;
	int i;

	for (i = 0; i < CHANNEL_COUNT; i++)
	{
		if (nyx_led_controller_core_configuration_get_param(config, param[i],
		                                                    &channel[i]) != NYX_ERROR_NONE)
			channel[i] = NYX_LED_CONTROLLER_CORE_COLOUR_UNSET;

		if (channel[i] != NYX_LED_CONTROLLER_CORE_COLOUR_UNSET)
			any_set = true;
	}

	if (!any_set)
	{
		channel[0] = channel[1] = channel[2] = brightness;
	}
	else
	{
		for (i = 0; i < CHANNEL_COUNT; i++)
		{
			/* One channel named and the others silent means the others are off. */
			if (channel[i] == NYX_LED_CONTROLLER_CORE_COLOUR_UNSET)
				channel[i] = 0;

			channel[i] = channel[i] * brightness / NYX_LED_CONTROLLER_CORE_COLOUR_MAX;
		}
	}

	/*
	 * Clamp rather than trust the arithmetic: the HAL packs each of these into
	 * one byte beside the alpha byte, so an out-of-range channel would not just
	 * be too bright, it would corrupt the value.
	 */
	for (i = 0; i < CHANNEL_COUNT; i++)
	{
		if (channel[i] < 0)
			channel[i] = 0;
		else if (channel[i] > NYX_LED_CONTROLLER_CORE_COLOUR_MAX)
			channel[i] = NYX_LED_CONTROLLER_CORE_COLOUR_MAX;
	}

	*r = channel[0];
	*g = channel[1];
	*b = channel[2];
}

static bool core_set(int r, int g, int b, int ms_on, int ms_off)
{
	if (notifications_device)
		return lights_hal_set(notifications_device, r, g, b, ms_on, ms_off);

	if (have_sysfs_core)
		return sysfs_core_set(r, g, b, ms_on, ms_off);

	return false;
}

/* ------------------------------------------------------------- nyx module */

nyx_error_t nyx_module_open(nyx_instance_t i, nyx_device_t **d)
{
	nyx_device_t *nyxDev = (nyx_device_t *) calloc(1, sizeof(nyx_device_t));
	bool have_backlight;
	bool have_keypad;

	if (NULL == nyxDev)
		return NYX_ERROR_OUT_OF_MEMORY;

	nyx_module_register_method(i, nyxDev,
	                           NYX_LED_CONTROLLER_EXECUTE_EFFECT_MODULE_METHOD,
	                           "led_controller_execute_effect");
	nyx_module_register_method(i, nyxDev,
	                           NYX_LED_CONTROLLER_GET_STATE_MODULE_METHOD,
	                           "led_controller_get_state");

	*d = nyxDev;

	/*
	 * Every light is probed independently, and the HAL is preferred only where
	 * it actually answers. A device can have a HAL that lights the display while
	 * leaving the keyboard and the notification LED to the kernel, and one that
	 * has no HAL at all can still have all three in sysfs.
	 */
	if (lights_hal_load())
	{
		backlight_device = lights_hal_open(LIGHT_ID_BACKLIGHT, true);
		keypad_device = lights_hal_open(LIGHT_ID_KEYBOARD, true);
		notifications_device = lights_hal_open(LIGHT_ID_NOTIFICATIONS, true);
	}

	have_backlight = backlight_device != NULL;

	if (!have_backlight)
	{
		have_backlight = sysfs_backlight_probe();

		if (have_backlight)
			nyx_debug("Using sysfs backlight %s (max %d)",
			          sysfs_backlight.brightness, sysfs_backlight.max);
	}

	have_keypad = keypad_device != NULL;

	if (!have_keypad)
	{
		have_keypad = sysfs_light_probe_names(&sysfs_keypad, keypad_leds,
		                                      G_N_ELEMENTS(keypad_leds));

		if (have_keypad)
			nyx_debug("Using sysfs keypad backlight %s (max %d)",
			          sysfs_keypad.brightness, sysfs_keypad.max);
	}

	if (!notifications_device)
	{
		have_sysfs_core = sysfs_core_probe();

		if (have_sysfs_core)
			nyx_debug("Using sysfs notification LED (%s%s%s)",
			          sysfs_light_valid(&sysfs_core[CHANNEL_RED]) ? "r" : "",
			          sysfs_light_valid(&sysfs_core[CHANNEL_GREEN]) ? "g" : "",
			          sysfs_light_valid(&sysfs_core[CHANNEL_BLUE]) ? "b" : "");
	}

	if (!have_backlight && !have_keypad && !notifications_device && !have_sysfs_core)
	{
		nyx_error(MSGID_NYX_MOD_LED_NODEVICE_ERR, 0,
		          "Found no backlight, keypad light or notification LED");
		return NYX_ERROR_DEVICE_UNAVAILABLE;
	}

	return NYX_ERROR_NONE;
}

nyx_error_t nyx_module_close(nyx_device_t *d)
{
	int c;

	free(d);

	lights_hal_close(notifications_device);
	notifications_device = NULL;

	lights_hal_close(keypad_device);
	keypad_device = NULL;

	lights_hal_close(backlight_device);
	backlight_device = NULL;

	sysfs_light_release(&sysfs_backlight);
	sysfs_light_release(&sysfs_keypad);

	for (c = 0; c < CHANNEL_COUNT; c++)
		sysfs_light_release(&sysfs_core[c]);

	have_sysfs_core = false;

	return NYX_ERROR_NONE;
}

static nyx_error_t handle_backlight_effect(nyx_device_handle_t handle,
                                           nyx_led_controller_effect_t effect)
{
	nyx_callback_status_t status = NYX_CALLBACK_STATUS_DONE;

	switch (effect.required.effect)
	{
	case NYX_LED_CONTROLLER_EFFECT_LED_SET:
	{
		int32_t brightness = effect.backlight.brightness_lcd;
		int32_t keypad_brightness = effect.backlight.brightness_keypad;

		nyx_debug("Adjusting backlight: brightness %i, keypad %i", brightness,
		          keypad_brightness);

		if (backlight_device)
		{
			if (!lights_hal_set_brightness(backlight_device, brightness))
				status = NYX_CALLBACK_STATUS_FAILED;
		}
		else if (sysfs_light_valid(&sysfs_backlight))
		{
			if (!sysfs_light_set_percent(&sysfs_backlight, brightness))
				status = NYX_CALLBACK_STATUS_FAILED;
		}

		if (status == NYX_CALLBACK_STATUS_FAILED)
			break;

		/*
		 * A negative keypad value means "leave this light alone" - what
		 * nyx-test-ledcontroller documents for --keypad and sends by default, so
		 * honouring it keeps a display-brightness test from switching the
		 * keyboard off as a side effect. luna-displaymanager never sends one:
		 * backlightOn() computes a 0..100 percentage and backlightOff() asks for
		 * 0, which does turn the keyboard off with the screen.
		 *
		 * brightness_lcd above deliberately does NOT get this treatment:
		 * backlightOff() passes -1 for the display and relies on it reaching
		 * zero before the panel is powered down, so a negative there has always
		 * meant off, and changing that is a separate question.
		 */
		if (keypad_brightness < 0)
			break;

		if (keypad_device)
		{
			if (!lights_hal_set_brightness(keypad_device, keypad_brightness))
				status = NYX_CALLBACK_STATUS_FAILED;
		}
		else if (sysfs_light_valid(&sysfs_keypad))
		{
			if (!sysfs_light_set_percent(&sysfs_keypad, keypad_brightness))
				status = NYX_CALLBACK_STATUS_FAILED;
		}

		break;
	}
	default:
		break;
	}

	if (effect.backlight.callback)
		effect.backlight.callback(handle, status, effect.backlight.callback_context);

	return NYX_ERROR_NONE;
}

static nyx_error_t handle_notification_effect(nyx_device_handle_t handle,
                                              nyx_led_controller_effect_t effect)
{
	int32_t led_on = 0, led_off = 0, brightness = 0;
	int red = 0, green = 0, blue = 0;
	nyx_error_t err;

	if (handle == NULL)
	{
		nyx_error(MSGID_NYX_MOD_LED_NODEVICE_ERR, 0,
		          "Handle to LED device - for notification-effect = NULL");
		return NYX_ERROR_INVALID_HANDLE;
	}

	if (effect.core_configuration == NULL)
	{
		nyx_error(MSGID_NYX_MOD_LED_NODEVICE_ERR, 0,
		          "LED Core configuration argument = NULL");
		return NYX_ERROR_INVALID_VALUE;
	}

	if (!notifications_device && !have_sysfs_core)
		return NYX_ERROR_DEVICE_UNAVAILABLE;

	switch (effect.required.effect)
	{
	case NYX_LED_CONTROLLER_EFFECT_LED_SET:
		err = nyx_led_controller_core_configuration_get_param(
		          effect.core_configuration,
		          NYX_LED_CONTROLLER_CORE_EFFECT_BRIGHTNESS, &brightness);

		if (err != NYX_ERROR_NONE)
		{
			nyx_debug("Could not resolve brightness level");
			return err;
		}

		resolve_colour(effect.core_configuration, brightness, &red, &green, &blue);

		nyx_debug("setting LED colour = [%d,%d,%d] (brightness %d)", red, green,
		          blue, brightness);

		if (!core_set(red, green, blue, 0, 0))
			return NYX_ERROR_INVALID_OPERATION;

		break;

	case NYX_LED_CONTROLLER_EFFECT_LED_PULSATE:
		err = nyx_led_controller_core_configuration_get_param(
		          effect.core_configuration,
		          NYX_LED_CONTROLLER_CORE_EFFECT_FADE_IN, &led_on);

		if (err != NYX_ERROR_NONE)
		{
			nyx_debug("Could not resolve pulse fade-in time");
			return err;
		}

		err = nyx_led_controller_core_configuration_get_param(
		          effect.core_configuration,
		          NYX_LED_CONTROLLER_CORE_EFFECT_FADE_OUT, &led_off);

		if (err != NYX_ERROR_NONE)
		{
			nyx_debug("Could not resolve pulse fade-out time");
			return err;
		}

		err = nyx_led_controller_core_configuration_get_param(
		          effect.core_configuration,
		          NYX_LED_CONTROLLER_CORE_EFFECT_BRIGHTNESS, &brightness);

		if (err != NYX_ERROR_NONE)
		{
			nyx_debug("Could not resolve pulse brightness level");
			return err;
		}

		resolve_colour(effect.core_configuration, brightness, &red, &green, &blue);

		nyx_debug("setting LED colour [%d,%d,%d] (brightness %d) to pulse on=%dms off=%dms",
		          red, green, blue, brightness, led_on, led_off);

		if (!core_set(red, green, blue, led_on, led_off))
			return NYX_ERROR_INVALID_OPERATION;

		break;

	default:
		break;
	}

	return NYX_ERROR_NONE;
}

nyx_error_t led_controller_execute_effect(nyx_device_handle_t handle,
                                          nyx_led_controller_effect_t effect)
{
	if (effect.required.led == NYX_LED_CONTROLLER_BACKLIGHT_LEDS)
		return handle_backlight_effect(handle, effect);

	/*
	 * Matched as a mask, not compared: callers name the notification LED as
	 * CENTER_LED, and webOS's own core-navi callers as CORE_LEDS, which is
	 * LEFT|CENTER|RIGHT. A device with one LED lights it for either.
	 */
	if (effect.required.led & NYX_LED_CONTROLLER_CORE_LEDS)
		return handle_notification_effect(handle, effect);

	return NYX_ERROR_DEVICE_UNAVAILABLE;
}

nyx_error_t led_controller_get_state(nyx_device_handle_t handle,
                                     nyx_led_controller_led_t led,
                                     nyx_led_controller_state_t *state)
{
	int brightness;

	if (!state)
		return NYX_ERROR_INVALID_VALUE;

	/*
	 * Only answerable for a light this module reads back, which means a sysfs
	 * one: the legacy lights HAL is write-only, with no way to ask a vendor blob
	 * what it last set.
	 */
	if (led == NYX_LED_CONTROLLER_BACKLIGHT_LEDS &&
	        sysfs_light_valid(&sysfs_backlight))
	{
		brightness = sysfs_read_int(sysfs_backlight.brightness, -1);
	}
	else if ((led & NYX_LED_CONTROLLER_CORE_LEDS) && have_sysfs_core)
	{
		int c;

		brightness = 0;

		for (c = 0; c < CHANNEL_COUNT; c++)
		{
			if (!sysfs_light_valid(&sysfs_core[c]))
				continue;

			brightness += sysfs_read_int(sysfs_core[c].brightness, 0);
		}
	}
	else
	{
		return NYX_ERROR_DEVICE_UNAVAILABLE;
	}

	if (brightness < 0)
		return NYX_ERROR_DEVICE_UNAVAILABLE;

	*state = brightness > 0 ? NYX_LED_CONTROLLER_STATE_ON
	                        : NYX_LED_CONTROLLER_STATE_OFF;

	return NYX_ERROR_NONE;
}
