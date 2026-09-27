// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
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
 * Stand-in for <android/hardware/lights.h> on header sets that no longer ship
 * it.
 *
 * Android 14 replaced the legacy lights HAL with the android.hardware.light
 * AIDL interface and stopped installing the header; android-headers-halium 14.0
 * has 43 hardware headers and this is not one of them. 9.0, 11.0 and 16.0 all
 * still ship it, and their copies are byte-identical, so the interface itself
 * has not moved - only its availability at build time.
 *
 * What matters here is the ABI of the vendor blob we dlopen, not which header
 * AOSP chose to install, so declaring it ourselves is safe. The declarations
 * below are copied from that header and must stay layout-compatible with it:
 * struct light_state_t is passed straight into the blob's set_light().
 *
 * If a device really has no legacy lights HAL, hw_get_module() returns NULL at
 * runtime and led_controller.c takes its existing sysfs path - the same thing
 * already happens on any device whose HAL cannot be loaded, so nothing new is
 * required of the caller.
 *
 * Deliberately carries the upstream include guard: if some other header pulls
 * in the real lights.h, whichever arrives second is skipped instead of
 * redefining these types.
 */

#ifndef ANDROID_LIGHTS_INTERFACE_H
#define ANDROID_LIGHTS_INTERFACE_H

#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/types.h>

/*
 * hw_module_t and friends, which the real lights.h takes from
 * <hardware/hardware.h>.
 *
 * nyx-modules deliberately has no virtual/android-headers dependency: it builds
 * for machines that have no Android side at all (rpi, qemu, PinePhone), and the
 * lights HAL here is loaded at runtime or not at all. So take the header when a
 * build happens to have it, and otherwise declare the three structs ourselves.
 *
 * These must stay layout-compatible with AOSP's, not merely contain the fields
 * used: we only ever touch module->methods->open, device->common.close and
 * device->set_light, but the vendor blob writes the whole of its own structs
 * through these pointers, so every preceding field - the reserved arrays
 * included, which are pointer-sized and so differ between 32- and 64-bit -
 * has to sit at the same offset it does there.
 */
#if defined(__has_include)
#  if __has_include(<hardware/hardware.h>)
#    include <hardware/hardware.h>
#    define NYX_LIGHTS_HAVE_HARDWARE_H 1
#  endif
#endif

#ifndef NYX_LIGHTS_HAVE_HARDWARE_H

struct hw_module_t;
struct hw_module_methods_t;
struct hw_device_t;

typedef struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct hw_module_methods_t *methods;
    void *dso;
#ifdef __LP64__
    uint64_t reserved[7];
#else
    uint32_t reserved[7];
#endif
} hw_module_t;

typedef struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *module, const char *id,
                struct hw_device_t **device);
} hw_module_methods_t;

typedef struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
#ifdef __LP64__
    uint64_t reserved[12];
#else
    uint32_t reserved[12];
#endif
    int (*close)(struct hw_device_t *device);
} hw_device_t;

#endif /* NYX_LIGHTS_HAVE_HARDWARE_H */

__BEGIN_DECLS

#define LIGHTS_HARDWARE_MODULE_ID "lights"

/* Logical, not physical, lights. */
#define LIGHT_ID_BACKLIGHT          "backlight"
#define LIGHT_ID_KEYBOARD           "keyboard"
#define LIGHT_ID_BUTTONS            "buttons"
#define LIGHT_ID_BATTERY            "battery"
#define LIGHT_ID_NOTIFICATIONS      "notifications"
#define LIGHT_ID_ATTENTION          "attention"

/* Flash modes for light_state_t.flashMode. */
#define LIGHT_FLASH_NONE            0
#define LIGHT_FLASH_TIMED           1
#define LIGHT_FLASH_HARDWARE        2

/* Policies for light_state_t.brightnessMode. */
#define BRIGHTNESS_MODE_USER        0
#define BRIGHTNESS_MODE_SENSOR      1

struct light_state_t {
    /* Colour of the LED in ARGB. The high byte is ignored; callers set 0xff. */
    unsigned int color;

    int flashMode;      /* see the LIGHT_FLASH_* constants */
    int flashOnMS;
    int flashOffMS;

    int brightnessMode; /* see the BRIGHTNESS_MODE_* constants */
};

struct light_device_t {
    struct hw_device_t common;

    /* Returns 0 on success, an error code on failure. */
    int (*set_light)(struct light_device_t *dev,
                     struct light_state_t const *state);
};

__END_DECLS

#endif  // ANDROID_LIGHTS_INTERFACE_H
