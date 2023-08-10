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

#ifndef MONGOCRYPT_MARKING_PRIVATE_H
#define MONGOCRYPT_MARKING_PRIVATE_H

#include "mc-fle2-encryption-placeholder-private.h"
#include "mc-range-mincover-private.h"
#include "mongocrypt-ciphertext-private.h"
#include "mongocrypt-private.h"

typedef enum {
    MONGOCRYPT_MARKING_FLE1_BY_ID,
    MONGOCRYPT_MARKING_FLE1_BY_ALTNAME,
    MONGOCRYPT_MARKING_FLE2_ENCRYPTION,
} mongocrypt_marking_type_t;

typedef struct {
    mongocrypt_marking_type_t type;

    union {
        struct {
            // Markings used by FLE1
            mongocrypt_encryption_algorithm_t algorithm;
            bson_iter_t v_iter;

            _mongocrypt_buffer_t key_id;
            bson_value_t key_alt_name;
        };

        mc_FLE2EncryptionPlaceholder_t fle2;
    };
} _mongocrypt_marking_t;

void _mongocrypt_marking_init(_mongocrypt_marking_t *marking);

void _mongocrypt_marking_cleanup(_mongocrypt_marking_t *marking);

bool _mongocrypt_marking_parse_unowned(const _mongocrypt_buffer_t *in,
                                       _mongocrypt_marking_t *out,
                                       mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

// Callers are expected to initialize `ciphertext` with
// `_mongocrypt_ciphertext_init before calling,
// and eventually free it using `_mongocrypt_ciphertext_cleanup`.
bool _mongocrypt_marking_to_ciphertext(void *ctx,
                                       _mongocrypt_marking_t *marking,
                                       _mongocrypt_ciphertext_t *ciphertext,
                                       mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

mc_mincover_t *mc_get_mincover_from_FLE2RangeFindSpec(mc_FLE2RangeFindSpec_t *findSpec,
                                                      size_t sparsity,
                                                      mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

#endif /* MONGOCRYPT_MARKING_PRIVATE_H */
