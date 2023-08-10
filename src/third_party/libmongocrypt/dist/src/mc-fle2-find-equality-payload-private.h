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

#ifndef MC_FLE2_FIND_EQUALITY_PAYLOAD_PRIVATE_H
#define MC_FLE2_FIND_EQUALITY_PAYLOAD_PRIVATE_H

#include "mongocrypt-buffer-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt.h"

typedef struct {
    _mongocrypt_buffer_t edcDerivedToken;       // d
    _mongocrypt_buffer_t escDerivedToken;       // s
    _mongocrypt_buffer_t eccDerivedToken;       // c
    _mongocrypt_buffer_t serverEncryptionToken; // e
    int64_t maxContentionCounter;               // cm
} mc_FLE2FindEqualityPayload_t;

void mc_FLE2FindEqualityPayload_init(mc_FLE2FindEqualityPayload_t *payload);

bool mc_FLE2FindEqualityPayload_parse(mc_FLE2FindEqualityPayload_t *out, const bson_t *in, mongocrypt_status_t *status);

bool mc_FLE2FindEqualityPayload_serialize(const mc_FLE2FindEqualityPayload_t *payload, bson_t *out);

void mc_FLE2FindEqualityPayload_cleanup(mc_FLE2FindEqualityPayload_t *payload);

#endif /* MC_FLE2_FIND_EQUALITY_PAYLOAD_PRIVATE_H */
