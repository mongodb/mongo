/*
 * Copyright 2022-present MongoDB, Inc.
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

#ifndef MC_FLE_BLOB_SUBTYPE_PRIVATE_H
#define MC_FLE_BLOB_SUBTYPE_PRIVATE_H

/* FLE Blob Subtype is the first byte of a BSON Binary Subtype 6.
 * FLE1 Blob Subtypes are defined in:
 * https://github.com/mongodb/specifications/blob/master/source/client-side-encryption/subtype6.rst
 * FLE2 Blob Subtypes are currently defined in:
 * https://github.com/markbenvenuto/mongo-enterprise-modules/blob/fle2/fle_protocol.md#reference-bindata-6-subtypes.
 */

typedef enum {
    MC_SUBTYPE_FLE1EncryptionPlaceholder = 0,
    MC_SUBTYPE_FLE1DeterministicEncryptedValue = 1,
    MC_SUBTYPE_FLE1RandomEncryptedValue = 2,
    MC_SUBTYPE_FLE2EncryptionPlaceholder = 3,
    MC_SUBTYPE_FLE2InsertUpdatePayload = 4,
    MC_SUBTYPE_FLE2FindEqualityPayload = 5,
    MC_SUBTYPE_FLE2UnindexedEncryptedValue = 6,
    MC_SUBTYPE_FLE2IndexedEqualityEncryptedValue = 7,
    MC_SUBTYPE_FLE2IndexedRangeEncryptedValue = 9,
    MC_SUBTYPE_FLE2FindRangePayload = 10,

    /* Queryable Encryption Version 2 Subtypes */
    MC_SUBTYPE_FLE2InsertUpdatePayloadV2 = 11,
    MC_SUBTYPE_FLE2FindEqualityPayloadV2 = 12,
    MC_SUBTYPE_FLE2FindRangePayloadV2 = 13,
    MC_SUBTYPE_FLE2IndexedEqualityEncryptedValueV2 = 14,
    MC_SUBTYPE_FLE2IndexedRangeEncryptedValueV2 = 15,
    MC_SUBTYPE_FLE2UnindexedEncryptedValueV2 = 16
} mc_fle_blob_subtype_t;

#endif /* MC_FLE_BLOB_SUBTYPE_PRIVATE_H */
