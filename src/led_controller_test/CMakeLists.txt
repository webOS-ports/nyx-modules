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

# A program rather than a webos_add_test(): this is driven by hand during
# bring-up, and nyx-modules.bb packages it as ${bindir}/nyx-test-ledcontroller.
add_executable(nyx-test-ledcontroller nyx_test_ledcontroller.c)
target_link_libraries(nyx-test-ledcontroller ${NYXLIB_LDFLAGS} ${GLIB2_LDFLAGS} ${PMLOG_LDFLAGS})
webos_build_program(NAME nyx-test-ledcontroller)
