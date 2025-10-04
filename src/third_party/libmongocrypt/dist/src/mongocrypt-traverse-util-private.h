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

#ifndef MONGOCRYPT_TRAVERSE_UTIL_H
#define MONGOCRYPT_TRAVERSE_UTIL_H

#include "mongocrypt-buffer-private.h"
#include "mongocrypt-status-private.h"

typedef enum {
    TRAVERSE_MATCH_CIPHERTEXT,
    TRAVERSE_MATCH_MARKING,
    TRAVERSE_MATCH_SUBTYPE6,
} traversal_match_t;

typedef bool (*_mongocrypt_traverse_callback_t)(void *ctx, _mongocrypt_buffer_t *in, mongocrypt_status_t *status);

typedef bool (*_mongocrypt_transform_callback_t)(void *ctx,
                                                 _mongocrypt_buffer_t *in,
                                                 bson_value_t *out,
                                                 mongocrypt_status_t *status);

bool _mongocrypt_traverse_binary_in_bson(_mongocrypt_traverse_callback_t cb,
                                         void *ctx,
                                         traversal_match_t match,
                                         bson_iter_t *iter,
                                         mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _mongocrypt_transform_binary_in_bson(_mongocrypt_transform_callback_t cb,
                                          void *ctx,
                                          traversal_match_t match,
                                          bson_iter_t *iter,
                                          bson_t *out,
                                          mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

#endif /* MONGOCRYPT_TRAVERSE_UTIL_H */
