// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
// SPDX-License-Identifier: Apache-2.0

/*
 * Host-side tests for the keys module's key map (lookup_key), in the variant
 * this build uses: device/keys.c on hardware, emulator/keys.c on an emulator
 * or VM (KEYS_EMULATOR).
 *
 * What both variants owe their one reader that matters, LunaDisplayManager:
 * KEY_POWER is the custom power key, and no ordinary key of a keyboard is.
 * The emulator variant passes a keyboard's keys through as standard keys, so
 * it also has to keep their own codes - KEY_2 is 3, the same number as
 * NYX_KEYS_CUSTOM_KEY_POWER_ON, and only the type tells them apart.
 *
 * Build and run: src/keys/tests/run-host-tests.sh
 */

#include <glib.h>
#include <stdint.h>

#include <nyx/nyx_module.h>

#if defined(KEYS_EMULATOR)
#include "../emulator/keys.c"
#else
#include "../device/keys.c"
#endif

typedef struct {
	int key;
	nyx_key_type_t type;
} mapped_t;

/* As keys_get_event() calls it: the type starts out standard. */
static mapped_t map(uint16_t code)
{
	keys_device_t device = { 0 };
	mapped_t m;

	m.type = NYX_KEY_TYPE_STANDARD;
	m.key = lookup_key(&device, code, 1, &m.type);
	return m;
}

static gboolean is_power(mapped_t m)
{
	return m.type == NYX_KEY_TYPE_CUSTOM && m.key == NYX_KEYS_CUSTOM_KEY_POWER_ON;
}

static void test_power_key_is_power(void)
{
	g_assert_true(is_power(map(KEY_POWER)));
}

/* Every key a PC keyboard has, bar KEY_POWER itself: none of them may switch
 * the display off. KEY_MAX bounds what evdev can deliver. */
static void test_no_other_key_is_power(void)
{
	unsigned code;

	for (code = 0; code <= KEY_MAX; code++) {
		if (code == KEY_POWER)
			continue;
		gboolean power = is_power(map((uint16_t) code));

		if (power)
			g_test_message("key code %u maps to the power key", code);
		g_assert_false(power);
	}
}

static void test_volume_keys(void)
{
	mapped_t up = map(KEY_VOLUMEUP), down = map(KEY_VOLUMEDOWN);

	g_assert_cmpint(up.type, ==, NYX_KEY_TYPE_CUSTOM);
	g_assert_cmpint(up.key, ==, NYX_KEYS_CUSTOM_KEY_VOL_UP);
	g_assert_cmpint(down.type, ==, NYX_KEY_TYPE_CUSTOM);
	g_assert_cmpint(down.key, ==, NYX_KEYS_CUSTOM_KEY_VOL_DOWN);
}

#if defined(KEYS_EMULATOR)
/* A keyboard's own keys come through as themselves, as standard keys. */
static void test_keyboard_keys_pass_through(void)
{
	mapped_t two = map(KEY_2), end = map(KEY_END);

	g_assert_cmpint(two.type, ==, NYX_KEY_TYPE_STANDARD);
	g_assert_cmpint(two.key, ==, KEY_2);
	g_assert_cmpint(end.type, ==, NYX_KEY_TYPE_STANDARD);
	g_assert_cmpint(end.key, ==, KEY_END);
}
#endif

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/keys/power-key-is-power", test_power_key_is_power);
	g_test_add_func("/keys/no-other-key-is-power", test_no_other_key_is_power);
	g_test_add_func("/keys/volume-keys", test_volume_keys);
#if defined(KEYS_EMULATOR)
	g_test_add_func("/keys/keyboard-keys-pass-through", test_keyboard_keys_pass_through);
#endif

	return g_test_run();
}
