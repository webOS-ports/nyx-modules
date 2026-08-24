// @@@LICENSE
//
//      Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
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
// LICENSE@@@

/*
 * Read a device path out of /etc/nyx.conf.
 *
 * These paths used to be compile-time -D defines set by a per-machine cmake
 * include, which meant a package built for one device and 57 machine files to
 * maintain. luneos-device-config now derives them at boot - or takes them from
 * the adaptation's deviceinfo when they cannot be derived - writes nyx.conf
 * and bind-mounts it over the shipped default, so the same binary suits any
 * device. The keys module has read [module.keys] paths= this way for a while;
 * this is the same mechanism for the rest.
 *
 * Returns NULL if the file, group or key is missing or the value is empty, so
 * callers fall through to exactly what they did before. Caller frees.
 */

#ifndef _NYX_CONF_H_
#define _NYX_CONF_H_

#include <glib.h>

#define NYX_CONF_FILE "/etc/nyx.conf"

static inline gchar *nyx_conf_get_path(const gchar *group, const gchar *key)
{
	GKeyFile *keyfile = g_key_file_new();
	gchar *value = NULL;

	if (g_key_file_load_from_file(keyfile, NYX_CONF_FILE, G_KEY_FILE_NONE, NULL))
	{
		value = g_key_file_get_string(keyfile, group, key, NULL);
	}

	g_key_file_free(keyfile);

	if (value && *value == '\0')
	{
		g_free(value);
		value = NULL;
	}

	return value;
}

#endif /* _NYX_CONF_H_ */
