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

/* src and target may be the same string. Assumes no whitespace in src. */
KMS_MSG_EXPORT (int)
kms_message_b64_to_b64url (const char *src,
                           size_t srclength,
                           char *target,
                           size_t targsize);
KMS_MSG_EXPORT (int)
kms_message_b64url_to_b64 (const char *src,
                           size_t srclength,
                           char *target,
                           size_t targsize);

/* Convenience conversions which return copies. */
char *
kms_message_raw_to_b64 (const uint8_t *raw, size_t raw_len);

uint8_t *
kms_message_b64_to_raw (const char *b64, size_t *out);

char *
kms_message_raw_to_b64url (const uint8_t *raw, size_t raw_len);

uint8_t *
kms_message_b64url_to_raw (const char *b64url, size_t *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KMS_MESSAGE_B64_H */
