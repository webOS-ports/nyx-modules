// Copyright (c) 2016-2018 LG Electronics, Inc.
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

#ifndef __NYX__MODULES__MESSAGE__LOG_H__
#define __NYX__MODULES__MESSAGE__LOG_H__

/** Utils*/
#define MSGID_NYX_MOD_GET_STRING_ERR                                        "NYXUTIL_GET_STRING_ERR"
#define MSGID_NYX_MOD_GET_DOUBLE_ERR                                        "NYXUTIL_GET_DOUBLE_ERR"
#define MSGID_NYX_MOD_GET_STRTOD_ERR                                        "NYXUTIL_GET_STRTOD_ERR"
#define MSGID_NYX_MOD_SYSFS_ERR                                             "NYXUTIL_SYSFS_ERR"
#define MSGID_NYX_MOD_GET_DIR_ERR                                           "NYXUTIL_GET_DIR_ERR"

/** Battery*/
#define MSGID_NYX_MOD_UDEV_ERR                                              "NYXBAT_UDEV_ERR"
#define MSGID_NYX_MOD_UDEV_MONITOR_ERR                                      "NYXBAT_UDEV_MONITOR_ERR"
#define MSGID_NYX_MOD_UDEV_SUBSYSTEM_ERR                                    "NYXBAT_UDEV_SUBSYSTEM_ERR"
#define MSGID_NYX_MOD_UDEV_RECV_ERR                                         "NYXBAT_UDEV_RECV_ERR"
#define MSGID_NYX_MOD_BATT_OPEN_ALREADY_ERR                                 "NYXBAT_OPEN_ALREADY_ERR"
#define MSGID_NYX_MOD_BATT_OPEN_ERR                                         "NYXBAT_OPEN_ERR"
#define MSGID_NYX_MOD_BATT_OUT_OF_MEMORY                                    "NYXBAT_OUT_OF_MEM"

/** Charger*/
#define MSGID_NYX_MOD_CHARG_ERR                                             "NYXCHG_ERR"
#define MSGID_NYX_MOD_NETLINK_ERR                                           "NYXCHG_NETLINK_ERR"
#define MSGID_NYX_MOD_CHR_SUB_ERR                                           "NYXCHR_SUB_ERR"
#define MSGID_NYX_MOD_ENABLE_REV_ERR                                        "NYXCHG_ENABLE_REV_ERR"
#define MSGID_NYX_MOD_CHARG_OPEN_ERR                                        "NYXCHG_OPEN_ERR"
#define MSGID_NYX_MOD_CHARG_OUT_OF_MEMORY                                   "NYXCHG_OUT_OF_MEM"

/** Device info generic*/
#define MSGID_NYX_MOD_OPEN_NDUID_ERR                                        "NYXDEV_OPEN_NDUID_ERR"
#define MSGID_NYX_MOD_READ_NDUID_ERR                                        "NYXDEV_READ_NDUID_ERR"
#define MSGID_NYX_MOD_WRITE_NDUID_ERR                                       "NYXDEV_WRITE_NDUID_ERR"
#define MSGID_NYX_MOD_CHMOD_ERR                                             "NYXDEV_CHMOD_ERR"
#define MSGID_NYX_MOD_MALLOC_ERR1                                           "NYXDEV_OOM1_ERR"
#define MSGID_NYX_MOD_MALLOC_ERR2                                           "NYXDEV_OOM2_ERR"
#define MSGID_NYX_MOD_URANDOM_ERR                                           "NYXDEV_URANDOM_ERR"
#define MSGID_NYX_MOD_URANDOM_OPEN_ERR                                      "NYXDEV_URANDOM_OPEN_ERR"
#define MSGID_NYX_MOD_MEMINFO_OPEN_ERR                                      "NYXDEV_MEMINFO_OPEN_ERR"
#define MSGID_NYX_MOD_STORAGE_ERR                                           "NYXDEV_STORAGE_ERR"
#define MSGID_NYX_MOD_DEV_INFO_OPEN_ERR                                     "NYXDEV_INFO_OPEN_ERR"

/** Display lib open*/
#define MSGID_NYX_MOD_DISP_OPEN_ALREADY_ERR                                 "NYXDIS_OPEN_ALREADY_ERR"
#define MSGID_NYX_MOD_DISP_OPEN_ERR                                         "NYXDIS_OPEN_ERR"
#define MSGID_NYX_MOD_DISP_OUT_OF_MEMORY                                    "NYXDIS_OUT_OF_MEM"

/** Security lib open*/
#define MSGID_NYX_MOD_SECU_OPEN_ERR                                         "NYXSEC_OPEN_ERR"
#define MSGID_NYX_MOD_SECU_OUT_OF_MEMORY                                    "NYXSEC_OUT_OF_MEM"

/** System */
#define MSGID_NYX_MOD_SYSTEM_OUT_OF_MEMORY                                  "NYXSYS_OUT_OF_MEM"
#define MSGID_NYX_MOD_SYSTEM_OPEN_ERR                                       "NYXSYS_OPEN_ERR"

/** Mass Storage Mode - MTP */
#define MSGID_NYX_MOD_MSMMTP_OPEN_ERR                                       "NYXMSM_OPEN_ERR"

/** Ambient Light Sensor */
#define MSGID_NYX_MOD_ALS_ENABLE_ERR                                        "NYXALS_ENABLE_ERR"
#define MSGID_NYX_MOD_ALS_DISABLE_ERR                                       "NYXALS_DISABLE_ERR"
#define MSGID_NYX_MOD_ALS_READ_EVENT_ERR                                    "NYXALS_READ_EVENT_ERR"

/** LED Controller */
#define MSGID_NYX_MOD_LED_NODEVICE_ERR                                      "NYXLED_NODEVICE_ERR"
#define MSGID_NYX_MOD_LED_OPENFILE_ERR                                      "NYXLED_OPENFILE_ERR"
#define MSGID_NYX_MOD_LED_FILE_CONTENT_ERR                                  "NYXLED_FILECONTENT_ERR"

/** Haptics Controller */
#define MSGID_NYX_MOD_HAPTICS_ODEVICE_FOUND                                 "NYXHAPTICS_DEVICE_FOUND"
#define MSGID_NYX_MOD_HAPTICS_NODEVICE_ERR                                  "NYXHAPTICS_NODEVICE_ERR"
#define MSGID_NYX_MOD_HAPTICS_NOSPECIAL_EFF                                 "NYXHAPTICS_NOSPECIAL_EFF"
#define MSGID_NYX_MOD_HAPTICS_NOPULSES_ERR                                  "NYXHAPTICS_NOPULSES_ERR"
#define MSGID_NYX_MOD_HAPTICS_TOGGLE_TIMEOUT                                "NYXHAPTICS_TOGGLE_TIMEOUT"
#define MSGID_NYX_MOD_HAPTICS_VIBRATE_PATTERN                               "NYXHAPTICS_VIBRATE_PATTERN"
#define MSGID_NYX_MOD_HAPTICS_VIBRATE                                       "NYXHAPTICS_VIBRATE"

/** Keys */
#define MSGID_NYX_MOD_KEYS_CONF_FILE_ERR                                    "NYXKEYS_CONF_FILE_ERR"
#define MSGID_NYX_MOD_KEYS_CONF_FILE_PATH_ERR                               "NYXKEYS_CONF_FILE_PATH_ERR"
#define MSGID_NYX_MOD_KEYS_NEW_INPUT_DEV                                    "NYXKEYS_NEW_INPUT_DEV"
#define MSGID_NYX_MOD_KEY_EVENT_ERR                                         "NYXKEY_EVENT_ERR"
#define MSGID_NYX_MOD_KEY_EVENT_READ_ERR                                    "NYXKEY_EVENT_READ_ERR"
#define MSGID_NYX_MOD_KEYS_OPEN_ERR                                         "NYXKEY_OPEN_ERR"
#define MSGID_NYX_MOD_KEY_OUT_OF_MEM                                        "NYXKEY_OUT_OF_MEM_ERR"

/** Touchpanel & Touchpanel MTDEV */
#define MSGID_NYX_MOD_TP_COORDBUF_ERR                                       "NYXTP_COORDBUF_ERR"
#define MSGID_NYX_MOD_TP_COORDS_ERR                                         "NYXTP_COORDS_ERR"
#define MSGID_NYX_MOD_TP_NO_FINGER_BUFF                                     "NYXTP_NO_FINGER_BUFF"
#define MSGID_NYX_MOD_TP_NEW_FINGER                                         "NYXTP_NEW_FINGER"
#define MSGID_NYX_MOD_TP_FINGER_WT                                          "NYXTP_FINGER_WT"
#define MSGID_NYX_MOD_TP_FINGER_DOWN                                        "NYXTP_FINGER_DOWN"
#define MSGID_NYX_MOD_TP_FINGER_UP                                          "NYXTP_FINGER_UP"
#define MSGID_NYX_MOD_TP_FING_LOW_WT                                        "NYXTP_FING_LOW_WT"
#define MSGID_NYX_MOD_TP_NOTOUCH_ERR                                        "NYXTP_NOTOUCH_ERR"
#define MSGID_NYX_MOD_TP_VBOX_OPEN_ERR                                      "NYXTP_VBOX_OPEN_ERR"
#define MSGID_NYX_MOD_TP_IOCTL_ERR                                          "NYXTP_IOCTL_ERR"
#define MSGID_NYX_MOD_TP_IOCTL_READ_ERR                                     "NYXTP_IOCTL_READ_ERR"
#define MSGID_NYX_MOD_TP_IOCTL_REQUEST_ERR                                  "NYXTP_IOCTL_REQUEST_ERR"
#define MSGID_NYX_MOD_TP_SETPTR_ERR                                         "NYXTP_SETPTR_ERR"
#define MSGID_NYX_MOD_TP_OPEN_FB_ERR                                        "NYXTP_OPEN_FB_ERR"
#define MSGID_NYX_MOD_TP_VSCREEN_INFO_ERR                                   "NYXTP_FB_VSCREEN_INFO_ERR"
#define MSGID_NYX_MOD_TP_OPEN_ERR                                           "NYXTP_OPEN_ERR"
#define MSGID_NYX_MOD_TP_EVENT_HLIMIT_ERR                                   "NYXTP_EVENT_HLIMIT_ERR"
#define MSGID_NYX_MOD_TP_EVENT_VLIMIT_ERR                                   "NYXTP_EVENT_VLIMIT_ERR"
#define MSGID_NYX_MOD_TP_RES_ERR                                            "NYXTP_DP_RES_ERR"
#define MSGID_NYX_MOD_TP_EVT_READ_ERR                                       "NYXTP_INPUT_EVENT_READ_ERR"
#define MSGID_NYX_MOD_TP_ABS_ERR                                            "NYXTP_ABS_ERR"
#define MSGID_NYX_MOD_TP_EVENT_NULL_ERR                                     "NYXTP_EVENT_NULL_ERR"
#define MSGID_NYX_MOD_TP_INVALID_EVENT                                      "NYXTP_INVALID_EVENT"
#define MSGID_NYX_MOD_TP_TOOMANY_ITEMS_ERR                                  "NYXTP_TOOMANY_ITEMS_ERR"
#define MSGID_NYX_MOD_TP_OUT_OF_MEMORY                                      "NYXTP_OUT_OF_MEM_ERR"
#define MSGID_NYX_MOD_TP_IGNORING_COORD                                     "NYXTP_IGNORING_COORD"

#endif // __NYX__MODULES__MESSAGE__LOG_H__
