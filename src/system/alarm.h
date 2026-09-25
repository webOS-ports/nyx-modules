/* @@@LICENSE
*
* Copyright (c) 2014 Simon Busch <morphis@gravedo.de>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

/*
*******************************************
* @file alarm.h
*
* @brief Wakeup alarms backed by the kernel's alarmtimer framework.
*
* Moved here from nyx-modules-hybris once it stopped depending on Android at
* all - see alarm.c. rtc.c arms these alongside the RTC alarm.
*******************************************
*/

#ifndef _ALARM_H_
#define _ALARM_H_

#include <stdbool.h>
#include <time.h>

bool wakeup_alarm_open(void);
void wakeup_alarm_close(void);
bool wakeup_alarm_set(time_t expiry);
bool wakeup_alarm_clear(void);
bool wakeup_alarm_read(struct tm *tm_time);
time_t wakeup_alarm_time(time_t *time);

#endif
