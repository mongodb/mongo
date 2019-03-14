/*
 * Copyright 2018-present MongoDB, Inc.
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

#ifndef KMS_RESPONSE_H
#define KMS_RESPONSE_H

#include "kms_message.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _kms_response_t kms_response_t;

KMS_MSG_EXPORT (const char *) kms_response_get_body (kms_response_t *reply);
KMS_MSG_EXPORT (void) kms_response_destroy (kms_response_t *reply);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KMS_RESPONSE_H */
