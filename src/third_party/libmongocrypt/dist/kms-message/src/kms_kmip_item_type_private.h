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

#ifndef KMS_KMIP_ITEM_TYPE_PRIVATE_H
#define KMS_KMIP_ITEM_TYPE_PRIVATE_H

#include "kms_message/kms_message_defines.h"

#define KMS_XMACRO           \
   KMS_X (Structure, 0x01)   \
   KMS_X (Integer, 0x02)     \
   KMS_X (LongInteger, 0x03) \
   KMS_X (BigInteger, 0x04)  \
   KMS_X (Enumeration, 0x05) \
   KMS_X (Boolean, 0x06)     \
   KMS_X (TextString, 0x07)  \
   KMS_X (ByteString, 0x08)  \
   KMS_X (DateTime, 0x09)    \
   KMS_X_LAST (Interval, 0x0A)

/* Generate an enum with each item_type value. */
#define KMS_X(ITEM_TYPE, VAL) KMIP_ITEM_TYPE_##ITEM_TYPE = VAL,
#define KMS_X_LAST(ITEM_TYPE, VAL) KMIP_ITEM_TYPE_##ITEM_TYPE = VAL
typedef enum { KMS_XMACRO } kmip_item_type_t;
#undef KMS_X
#undef KMS_X_LAST

#define KMS_X(ITEM_TYPE, VAL)       \
   case KMIP_ITEM_TYPE_##ITEM_TYPE: \
      return #ITEM_TYPE;
#define KMS_X_LAST KMS_X
static KMS_MSG_INLINE const char *
kmip_item_type_to_string (kmip_item_type_t item_type)
{
   switch (item_type) {
   default:
      return "Unknown KMIP item type";
      KMS_XMACRO
   }
}
#undef KMS_X
#undef KMS_X_LAST

#undef KMS_XMACRO

#endif /* KMS_KMIP_ITEM_TYPE_PRIVATE_H */
