# Copyright (c) 2010-2024 LG Electronics, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

include(FindPkgConfig)

pkg_check_modules(GLIB2 REQUIRED glib-2.0)
include_directories(${GLIB2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GLIB2_CFLAGS_OTHER})

pkg_check_modules(NYXLIB REQUIRED nyx>=7.3)
include_directories(${NYXLIB_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${NYXLIB_CFLAGS_OTHER})

pkg_check_modules(LIBCRYPTO REQUIRED libcrypto)
include_directories(${LIBCRYPTO_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${LIBCRYPTO_CFLAGS_OTHER})

pkg_check_modules(GIO REQUIRED gio-2.0)
include_directories(${GIO_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GIO_CFLAGS_OTHER})

if(NYXMOD_OW_BATTERY OR NYXMOD_OW_CHARGER OR NYXMOD_OW_MSMMTP OR NYXMOD_OW_LED OR NYXMOD_OW_HAPTICS)
    pkg_check_modules(UDEV REQUIRED libudev)
    include_directories(${UDEV_INCLUDE_DIRS})
    webos_add_compiler_flags(ALL ${UDEV_CFLAGS_OTHER})
endif()

webos_add_linker_options(ALL --no-undefined --as-needed)
webos_add_compiler_flags(ALL -Wall -g)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99")
webos_add_compiler_flags(ALL -D_GNU_SOURCE=1)
webos_add_compiler_flags(DEBUG -O0 -DDEBUG -D_DEBUG)
webos_add_compiler_flags(RELEASE -DNDEBUG)

if(NYXMOD_OW_BATTERY)
    add_subdirectory(battery)
endif()

if(NYXMOD_OW_CHARGER)
    add_subdirectory(charger)
endif()

if(NYXMOD_OW_DEVICEINFO)
    add_subdirectory(device_info)
endif()

if(NYXMOD_OW_DISPLAY)
    add_subdirectory(display)
endif()

if(NYXMOD_OW_SECURITY)
    add_subdirectory(security)
endif()

if(NYXMOD_OW_SECURITY2)
    add_subdirectory(security2)
endif()

if(NYXMOD_OW_SYSTEM)
    add_subdirectory(system)
endif()

if(NYXMOD_OW_OSINFO)
#    set(ENCRYPTION_KEY_TYPE "" CACHE STRING "Encryption key type")
    add_subdirectory(os_info)
endif()

if(NYXMOD_OW_GPS)
    add_subdirectory(gps)
endif()

if(NYXMOD_OW_CEC)
    add_subdirectory(cec)
endif()

# Modules added for LuneOS
if(NYXMOD_OW_TOUCHPANEL_MTDEV)
    pkg_check_modules(MTDEV REQUIRED mtdev)
    include_directories(${MTDEV_INCLUDE_DIRS})
    webos_add_compiler_flags(ALL ${MTDEV_CFLAGS_OTHER})
    add_subdirectory(touchpanel_mtdev)
endif()

if(NYXMOD_OW_TOUCHPANEL)
    add_subdirectory(touchpanel)
endif()

if(NYXMOD_OW_MSMMTP)
    add_subdirectory(msm_mtp)
endif()

if(NYXMOD_OW_ALS)
    add_subdirectory(als)
endif()

if(NYXMOD_OW_KEYS)
    add_subdirectory(keys)
endif()

if(NYXMOD_OW_LED)
    add_subdirectory(led_controller)
endif()

if(NYXMOD_OW_HAPTICS)
    add_subdirectory(haptics_timedoutput)
endif()

# Not gated on NYXMOD_OW_LED. That flag says which of nyx-modules and
# nyx-modules-hybris provides the LED module, and the tool is a client of
# whichever won - on a hybris machine the flag is FALSE and the LED module is
# built by the other recipe entirely, which is exactly where a way to poke the
# notification LED is most wanted.
add_subdirectory(led_controller_test)

# The torch has two backends, so like the LED controller it needs a per-machine
# owner. Here it is the sysfs one; NYXMOD_OW_LEDTORCH FALSE hands it to
# nyx-modules-hybris, which drives the Android camera service instead - the
# route that works where /sys/class/leds belongs to the Android side.
#
# Defaulted rather than required of all 57 machine files: sysfs is the right
# answer for every native device, and a halium machine that needs the other
# backend says so explicitly.
if(NOT DEFINED NYXMOD_OW_LEDTORCH)
    set(NYXMOD_OW_LEDTORCH TRUE)
endif()

if(NYXMOD_OW_LEDTORCH)
    add_subdirectory(led_torch)
endif()
add_subdirectory(led_test)
