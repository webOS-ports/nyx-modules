// Copyright (c) 2014-2018 LG Electronics, Inc.
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
 * @file utils.h
 */

#ifndef UTILS_H_
#define UTILS_H_

int FileGetString(const char *path, char *ret_string, size_t maxlen);
int FileGetDouble(const char *path, double *ret_data);
/*
 * Locate a power_supply by its node name rather than by the type it reports.
 *
 * Needed because the type is not dependable for every supply: Qualcomm's
 * power_supply_sysfs.c maps POWER_SUPPLY_TYPE_BMS, _MAIN and _PARALLEL all onto
 * the string "Mains", so a fuel gauge registered as .type = POWER_SUPPLY_TYPE_BMS
 * reads back as "Mains" and cannot be told apart from an AC supply. Its node
 * name is fixed by the driver and is what the kernel actually guarantees.
 *
 * Returns a newly allocated path, or NULL when no such node exists. Free with
 * g_free().
 */
char *find_power_supply_sysfs_path_by_name(const char *name);

char *find_power_supply_sysfs_path(const char *device_type);

/**
 * Every power_supply of the given type, sorted by node name so the answer does
 * not depend on the order the filesystem hands entries back. Returns a
 * NULL-terminated vector, or NULL when there are none; free with g_strfreev().
 */
char **find_power_supply_sysfs_paths(const char *device_type);

#endif // UTILS_H_
