/* Copyright 2017-2018 All Rights Reserved.
 *  Gyeonghwan Hong (redcarrottt@gmail.com)
 *
 * [Contact]
 *  Gyeonghwan Hong (redcarrottt@gmail.com)
 *
 * Licensed under the Apache License, Version 2.0(the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __CONFIG_history_based_POLICY_H__
#define __CONFIG_history_based_POLICY_H__

#define INC_COUNT_THRESHOLD 10
#define DEC_COUNT_THRESHOLD 10

#define IDLE_THRESHOLD 5 * 1000 // 5KB/s
#define SWITCH_THRESHOLD 75 * 1000 // 75KB/s

#define HIGH_THRESHOLD 150 * 1000 // 150KB/s
#define LOW_THRESHOLD 50 * 1000 // 50KB/s

#define TRAFFIC_HISTORY_TABLE_FILENAME "traffic_history_table.txt"

#endif /* defined(__CONFIG_history_based_POLICY_H__) */