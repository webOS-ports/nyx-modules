// Copyright (c) 2010-2018 LG Electronics, Inc.
// Copyright (c) 2012 Simon Busch <morphis@gravedo.de>
// Copyright (c) 2018 Christophe Chapuis <chris.chapuis@gmail.com>
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

#ifndef __TOUCHPANEL_COMMON_H
#define __TOUCHPANEL_COMMON_H

void set_event_params(input_event_t *pEvent, time_stamp_t *pTime, uint16_t type,
                      uint16_t code, int32_t value);

#endif  /* __TOUCHPANEL_COMMON_PRV_H */
