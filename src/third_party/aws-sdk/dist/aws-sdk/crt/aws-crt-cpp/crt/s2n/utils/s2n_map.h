/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
#pragma once

#include <string.h>

#include "crypto/s2n_hash.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_result.h"

struct s2n_map;
struct s2n_map_iterator {
    const struct s2n_map *map;
    /* Index of the entry to be returned on the next `s2n_map_iterator_next()` call. */
    uint32_t current_index;
    bool consumed;
};

struct s2n_map *s2n_map_new();
struct s2n_map *s2n_map_new_with_initial_capacity(uint32_t capacity);
S2N_RESULT s2n_map_add(struct s2n_map *map, struct s2n_blob *key, struct s2n_blob *value);
S2N_RESULT s2n_map_put(struct s2n_map *map, struct s2n_blob *key, struct s2n_blob *value);
S2N_RESULT s2n_map_complete(struct s2n_map *map);
S2N_RESULT s2n_map_unlock(struct s2n_map *map);
S2N_RESULT s2n_map_lookup(const struct s2n_map *map, struct s2n_blob *key, struct s2n_blob *value, bool *key_found);
S2N_RESULT s2n_map_free(struct s2n_map *map);
S2N_RESULT s2n_map_size(struct s2n_map *map, uint32_t *size);

S2N_RESULT s2n_map_iterator_init(struct s2n_map_iterator *iter, const struct s2n_map *map);
S2N_RESULT s2n_map_iterator_next(struct s2n_map_iterator *iter, struct s2n_blob *value);
bool s2n_map_iterator_has_next(const struct s2n_map_iterator *iter);
