# Copyright (c) 2010-2018 LG Electronics, Inc.
# Copyright (c) 2018 Herman van Hazendonk <github.com@herrie.org>
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

if(${WEBOS_TARGET_MACHINE_IMPL} STREQUAL emulator)
	webos_build_nyx_module(KeysMain 
						   SOURCES keys_common.c emulator/keys.c
						   LIBRARIES ${GLIB2_LDFLAGS} ${PMLOG_LDFLAGS} ${NYXLIB_LDFLAGS} -lrt -lpthread)
elseif(${WEBOS_TARGET_MACHINE_IMPL} STREQUAL device)
	webos_build_nyx_module(KeysMain 
						   SOURCES keys_common.c device/keys.c
						   LIBRARIES ${GLIB2_LDFLAGS} ${PMLOG_LDFLAGS} ${NYXLIB_LDFLAGS} -lrt -lpthread)
endif()
