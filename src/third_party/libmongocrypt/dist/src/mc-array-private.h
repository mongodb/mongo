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

#ifndef MC_ARRAY_PRIVATE_H
#define MC_ARRAY_PRIVATE_H

/* This file is copied from mongoc-array-private.h in libmongoc version 1.17.7
 * at commit 200a01bb208633fe3cf395d81acc1e19492d9de4 */

#include <bson/bson.h>

typedef struct _mc_array_t mc_array_t;

struct _mc_array_t {
    size_t len;
    size_t element_size;
    size_t allocated;
    void *data;
};

#define _mc_array_append_val(a, v) _mc_array_append_vals(a, &v, 1)
#define _mc_array_index(a, t, i) (((t *)(a)->data)[i])
#define _mc_array_clear(a) (a)->len = 0

void _mc_array_init(mc_array_t *array, size_t element_size);
void _mc_array_copy(mc_array_t *dst, const mc_array_t *src);
void _mc_array_append_vals(mc_array_t *array, const void *data, uint32_t n_elements);
void _mc_array_destroy(mc_array_t *array);

#endif /* MC_ARRAY_PRIVATE_H */
