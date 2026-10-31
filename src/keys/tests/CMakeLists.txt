# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
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

# Host-runnable as well, for both key maps: see run-host-tests.sh
webos_add_test(test_keys_lookup
		SOURCES test_keys_lookup.c
		LIBRARIES ${NYXLIB_LDFLAGS} ${GLIB2_LDFLAGS} ${PMLOG_LDFLAGS} -lrt -lpthread)
# webos_add_test() makes no target unless tests are built.
if(TARGET test_keys_lookup AND ${WEBOS_TARGET_MACHINE_IMPL} STREQUAL emulator)
	target_compile_definitions(test_keys_lookup PRIVATE KEYS_EMULATOR)
endif()
