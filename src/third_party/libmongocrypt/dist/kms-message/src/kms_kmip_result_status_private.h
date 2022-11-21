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

#ifndef KMS_KMIP_RESULT_STATUS_PRIVATE_H
#define KMS_KMIP_RESULT_STATUS_PRIVATE_H

#include "kms_message/kms_message_defines.h"

#define KMS_XMACRO                                           \
   KMS_X (OperationSuccess, "Success", 0x00000000)           \
   KMS_X (OperationFailed, "Operation Failed", 0x00000001)   \
   KMS_X (OperationPending, "Operation Pending", 0x00000002) \
   KMS_X_LAST (OperationUndone, "Operation Undone", 0x00000003)

/* Generate an enum with each result_status value. */
#define KMS_X(RESULT_STATUS, STR, VAL) KMIP_RESULT_STATUS_##RESULT_STATUS = VAL,
#define KMS_X_LAST(RESULT_STATUS, STR, VAL) \
   KMIP_RESULT_STATUS_##RESULT_STATUS = VAL
typedef enum { KMS_XMACRO } kmip_result_status_t;
#undef KMS_X
#undef KMS_X_LAST

#define KMS_X(RESULT_STATUS, STR, VAL)      \
   case KMIP_RESULT_STATUS_##RESULT_STATUS: \
      return STR;
#define KMS_X_LAST KMS_X
static KMS_MSG_INLINE const char *
kmip_result_status_to_string (kmip_result_status_t result_status)
{
   switch (result_status) {
   default:
      return "Unknown KMIP result status";
      KMS_XMACRO
   }
}
#undef KMS_X
#undef KMS_X_LAST

#undef KMS_XMACRO

#endif /* KMS_KMIP_RESULT_STATUS_PRIVATE_H */
