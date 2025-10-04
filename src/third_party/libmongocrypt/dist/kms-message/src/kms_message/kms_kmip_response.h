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

#ifndef KMS_KMIP_RESPONSE_H
#define KMS_KMIP_RESPONSE_H

#include "kms_message_defines.h"

#include <stdint.h>

#include "kms_response.h"

/* kms_kmip_response_get_unique_identifier returns the UniqueIdentifier in the
 * first BatchItem in a ResponseMessage.
 * - Returns a NULL terminated string that the caller must free.
 * - Returns NULL on error and sets an error on kms_response_t. */
KMS_MSG_EXPORT (char *)
kms_kmip_response_get_unique_identifier (kms_response_t *res);

/* kms_kmip_response_get_secretdata returns the KeyMaterial in the
 * first BatchItem in a ResponseMessage.
 * - Caller must free returned data.
 * - Returns NULL on error and sets an error on kms_response_t. */
KMS_MSG_EXPORT (uint8_t *)
kms_kmip_response_get_secretdata (kms_response_t *res, size_t *secretdatalen);

KMS_MSG_EXPORT (uint8_t *)
kms_kmip_response_get_data (kms_response_t *res, size_t *datalen);

KMS_MSG_EXPORT (uint8_t *)
kms_kmip_response_get_iv (kms_response_t *res, size_t *datalen);

#endif /* KMS_KMIP_RESPONSE_H */
