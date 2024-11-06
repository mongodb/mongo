/*
 * Copyright 2024-present MongoDB, Inc.
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

#ifndef MC_FLE2_TAG_AND_ENCRYPTED_METADATA_BLOCK_H
#define MC_FLE2_TAG_AND_ENCRYPTED_METADATA_BLOCK_H

#include "mc-reader-private.h"
#include "mc-writer-private.h"
#include "mongocrypt-private.h"

typedef struct {
    _mongocrypt_buffer_t encryptedCount;
    _mongocrypt_buffer_t tag;
    _mongocrypt_buffer_t encryptedZeros;
} mc_FLE2TagAndEncryptedMetadataBlock_t;

#define kFieldLen 32U

void mc_FLE2TagAndEncryptedMetadataBlock_init(mc_FLE2TagAndEncryptedMetadataBlock_t *metadata);

void mc_FLE2TagAndEncryptedMetadataBlock_cleanup(mc_FLE2TagAndEncryptedMetadataBlock_t *metadata);

bool mc_FLE2TagAndEncryptedMetadataBlock_parse(mc_FLE2TagAndEncryptedMetadataBlock_t *metadata,
                                               const _mongocrypt_buffer_t *buf,
                                               mongocrypt_status_t *status);

bool mc_FLE2TagAndEncryptedMetadataBlock_serialize(const mc_FLE2TagAndEncryptedMetadataBlock_t *metadata,
                                                   _mongocrypt_buffer_t *buf,
                                                   mongocrypt_status_t *status);

#endif /* MC_FLE2_TAG_AND_ENCRYPTED_METADATA_BLOCK_H */