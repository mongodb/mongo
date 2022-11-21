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

#ifndef KMS_KMIP_RESULT_REASON_PRIVATE_H
#define KMS_KMIP_RESULT_REASON_PRIVATE_H

#include "kms_message/kms_message_defines.h"

/* clang-format off */
#define KMS_XMACRO                                                                             \
   KMS_X (ItemNotFound, "Item Not Found", 0x00000001)                                          \
   KMS_X (ResponseTooLarge, "Response Too Large", 0x00000002)                                  \
   KMS_X (AuthenticationNotSuccessful, "Authentication Not Successful", 0x00000003)            \
   KMS_X (InvalidMessage, "Invalid Message", 0x00000004)                                       \
   KMS_X (OperationNotSupported, "Operation Not Supported", 0x00000005)                        \
   KMS_X (MissingData, "Missing Data", 0x00000006)                                             \
   KMS_X (InvalidField, "Invalid Field", 0x00000007)                                           \
   KMS_X (FeatureNotSupported, "Feature Not Supported", 0x00000008)                            \
   KMS_X (OperationCanceledByRequester, "Operation Canceled By Requester", 0x00000009)         \
   KMS_X (CryptographicFailure, "Cryptographic Failure", 0x0000000A)                           \
   KMS_X (IllegalOperation, "Illegal Operation", 0x0000000B)                                   \
   KMS_X (PermissionDenied, "Permission Denied", 0x0000000C)                                   \
   KMS_X (Objectarchived, "Object archived", 0x0000000D)                                       \
   KMS_X (IndexOutofBounds, "Index Out of Bounds", 0x0000000E)                                 \
   KMS_X (ApplicationNamespaceNotSupported, "Application Namespace Not Supported", 0x0000000F) \
   KMS_X (KeyFormatTypeNotSupported, "Key Format Type Not Supported", 0x00000010)              \
   KMS_X (KeyCompressionTypeNotSupported, "Key Compression Type Not Supported", 0x00000011)    \
   KMS_X (EncodingOptionError, "Encoding Option Error", 0x00000012)                            \
   KMS_X (KeyValueNotPresent, "Key Value Not Present", 0x00000013)                             \
   KMS_X (AttestationRequired, "Attestation Required", 0x00000014)                             \
   KMS_X (AttestationFailed, "Attestation Failed", 0x00000015)                                 \
   KMS_X (Sensitive, "Sensitive", 0x00000016)                                                  \
   KMS_X (NotExtractable, "Not Extractable", 0x00000017)                                       \
   KMS_X (ObjectAlreadyExists, "Object Already Exists", 0x00000018)                            \
   KMS_X_LAST (GeneralFailure, "General Failure", 0x00000100)
/* clang-format on */

/* Generate an enum with each result_reason value. */
#define KMS_X(RESULT_REASON, STR, VAL) KMIP_RESULT_REASON_##RESULT_REASON = VAL,
#define KMS_X_LAST(RESULT_REASON, STR, VAL) \
   KMIP_RESULT_REASON_##RESULT_REASON = VAL
typedef enum { KMS_XMACRO } kmip_result_reason_t;
#undef KMS_X
#undef KMS_X_LAST

#define KMS_X(RESULT_REASON, STR, VAL)      \
   case KMIP_RESULT_REASON_##RESULT_REASON: \
      return STR;
#define KMS_X_LAST KMS_X
static KMS_MSG_INLINE const char *
kmip_result_reason_to_string (kmip_result_reason_t result_reason)
{
   switch (result_reason) {
   default:
      return "Unknown KMIP result reason";
      KMS_XMACRO
   }
}
#undef KMS_X
#undef KMS_X_LAST

#undef KMS_XMACRO

#endif /* KMS_KMIP_RESULT_REASON_PRIVATE_H */
