/*
 * Copyright 2009-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common-prelude.h"

#ifndef COMMON_B64_PRIVATE_H
#define COMMON_B64_PRIVATE_H

#include <bson/bson.h>

#define mcommon_b64_ntop_calculate_target_size COMMON_NAME (b64_ntop_calculate_target_size)
#define mcommon_b64_pton_calculate_target_size COMMON_NAME (b64_pton_calculate_target_size)
#define mcommon_b64_ntop COMMON_NAME (b64_ntop)
#define mcommon_b64_pton COMMON_NAME (b64_pton)

/**
 * When encoding from "network" (raw data) to "presentation" (base64 encoded).
 * Includes the trailing null byte. */
size_t
mcommon_b64_ntop_calculate_target_size (size_t raw_size);

/* When encoding from "presentation" (base64 encoded) to "network" (raw data).
 * This may be an overestimate if the base64 data includes spaces. For a more
 * accurate size, call b64_pton (src, NULL, 0), which will read the src
 * data and return an exact size. */
size_t
mcommon_b64_pton_calculate_target_size (size_t base64_encoded_size);

/* Returns the number of bytes written (excluding NULL byte) to target on
 * success or -1 on error. Adds a trailing NULL byte.
 * Encodes from "network" (raw data) to "presentation" (base64 encoded),
 * hence the obscure name "ntop".
 */
int
mcommon_b64_ntop (uint8_t const *src, size_t srclength, char *target, size_t targsize);

/** If target is not NULL, the number of bytes written to target on success or
 * -1 on error. If target is NULL, returns the exact number of bytes that would
 * be written to target on decoding. Encodes from "presentation" (base64
 * encoded) to "network" (raw data), hence the obscure name "pton".
 */
int
mcommon_b64_pton (char const *src, uint8_t *target, size_t targsize);

#endif /* COMMON_B64_PRIVATE_H */
