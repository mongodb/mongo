/*
 * Copyright 2021-present MongoDB, Inc.
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

#ifndef KMS_KMIP_REQUEST_H
#define KMS_KMIP_REQUEST_H

#include "kms_message_defines.h"
#include "kms_request.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KMS_KMIP_REQUEST_SECRETDATA_LENGTH 96

/* kms_kmip_request_register_secretdata_new creates a KMIP Register request with
 * a SecretData payload of length KMS_KMIP_REQUEST_SECRETDATA_LENGTH.
 * - len must be KMS_KMIP_REQUEST_SECRETDATA_LENGTH.
 * - Callers must check for an error by calling kms_request_get_error. */
KMS_MSG_EXPORT (kms_request_t *)
kms_kmip_request_register_secretdata_new (void *reserved,
                                          const uint8_t *data,
                                          size_t len);

/* kms_kmip_request_activate_new creates a KMIP Activate request with the
 * provided unique identifer.
 * - unique_identifier must be a NULL terminated string.
 * - Callers must check for an error by calling kms_request_get_error. */
KMS_MSG_EXPORT (kms_request_t *)
kms_kmip_request_activate_new (void *reserved, const char *unique_identifier);

/* kms_kmip_request_get_new creates a KMIP Get request with the provided unique
 * identifer.
 * - unique_identifier must be a NULL terminated string.
 * - Callers must check for an error by calling kms_request_get_error. */
KMS_MSG_EXPORT (kms_request_t *)
kms_kmip_request_get_new (void *reserved, const char *unique_identifier);

#ifdef __cplusplus
}
#endif

#endif /* KMS_KMIP_REQUEST_H */
