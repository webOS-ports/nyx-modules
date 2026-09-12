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

/**
 * @file battery.h
 */

#ifndef BATTERY_H_
#define BATTERY_H_

#include <nyx/common/nyx_error.h>
#include <nyx/common/nyx_battery_common.h>

/*
 * A device can carry more than one battery - a PinePhone (Pro) docked in its
 * keyboard has the phone's own and the keyboard's - so every reading is taken
 * for a given battery, identified by its index. Index 0 is the primary
 * battery: the one the charging logic and the low-battery shutdown follow, and
 * the one the single-battery part of the nyx API reports.
 */
#define BATTERY_PRIMARY 0

// These functions are implemented in device/battery.c or emulator/fake_battery.c

// called by batterylib.c
nyx_error_t battery_init(void);
nyx_error_t battery_deinit(void);
nyx_battery_ctia_t *get_battery_ctia_params(void);

// how many batteries were found, and who they are
int battery_count(void);
const char *battery_name(int index);
const char *battery_role(int index);

// called by battery_read_status() in batterylib.c
int battery_percent(int index);
int battery_temperature(int index);
int battery_voltage(int index);
int battery_current(int index);
int battery_avg_current(int index);
double battery_full40(int index);
double battery_full_design(int index);
int battery_health(int index);
double battery_rawcoulomb(int index);
double battery_coulomb(int index);
double battery_age(int index);
bool battery_is_present(int index);

// not currently supported by device/battery.c or emulator/fake_battery.c (stub implementations)
bool battery_authenticate(void);
void battery_set_wakeup_percent(int);

void battery_set_fakemode(bool);
nyx_error_t battery_get_fakemode(bool *);

// not currently called by batterylib.c
// bool battery_is_authenticated(const char *pair_challenge, const char *pair_response);

#endif /* BATTERY_H_ */
