/*
 * Copyright 2018-present MongoDB Inc.
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

#ifndef KMS_MESSAGE_B64_H
#define KMS_MESSAGE_B64_H

#include "kms_message.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

KMS_MSG_EXPORT (void)
kms_message_b64_initialize_rmap (void);

KMS_MSG_EXPORT (int)
kms_message_b64_ntop (uint8_t const *src,
                      size_t srclength,
                      char *target,
                      size_t targsize);

KMS_MSG_EXPORT (int)
kms_message_b64_pton (char const *src, uint8_t *target, size_t targsize);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KMS_MESSAGE_B64_H */
