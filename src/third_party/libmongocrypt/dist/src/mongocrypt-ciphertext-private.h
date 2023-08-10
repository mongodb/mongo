/*
 * Copyright 2019-present MongoDB, Inc.
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

#ifndef MONGOCRYPT_CIPHERTEXT_PRIVATE_H
#define MONGOCRYPT_CIPHERTEXT_PRIVATE_H

#include "mc-fle-blob-subtype-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt.h"

/**
 * Produced by mongocrypt-marking.c from _mongocrypt_marking_t
 * as encrypted payloads for blob_subtypes:
 *   FLE1DeterministicEncryptedValue(1)
 *   FLE1RandomEncryptedValue(2)
 *   FLE2InsertUpdatePayload(4)
 */
typedef struct {
    _mongocrypt_buffer_t key_id;
    mc_fle_blob_subtype_t blob_subtype;
    uint8_t original_bson_type;
    _mongocrypt_buffer_t data;
} _mongocrypt_ciphertext_t;

void _mongocrypt_ciphertext_init(_mongocrypt_ciphertext_t *ciphertext);

void _mongocrypt_ciphertext_cleanup(_mongocrypt_ciphertext_t *ciphertext);

bool _mongocrypt_ciphertext_parse_unowned(_mongocrypt_buffer_t *in,
                                          _mongocrypt_ciphertext_t *ciphertext,
                                          mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _mongocrypt_serialize_ciphertext(_mongocrypt_ciphertext_t *ciphertext,
                                      _mongocrypt_buffer_t *out) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _mongocrypt_ciphertext_serialize_associated_data(_mongocrypt_ciphertext_t *ciphertext,
                                                      _mongocrypt_buffer_t *out) MONGOCRYPT_WARN_UNUSED_RESULT;

#endif /* MONGOCRYPT_CIPHERTEXT_PRIVATE_H */
