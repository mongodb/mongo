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

/* This file is copied from mongoc-array.c in libmongoc version 1.17.7 at commit
 * 200a01bb208633fe3cf395d81acc1e19492d9de4 */

#include "mc-array-private.h"

void _mc_array_init(mc_array_t *array, size_t element_size) {
    BSON_ASSERT_PARAM(array);
    BSON_ASSERT(element_size);

    array->len = 0;
    array->element_size = element_size;
    array->allocated = 128;
    array->data = (void *)bson_malloc0(array->allocated);
}

/*
 *--------------------------------------------------------------------------
 *
 * _mc_array_copy --
 *
 *       Destroy dst and copy src into it. Both arrays must be initialized.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void _mc_array_copy(mc_array_t *dst, const mc_array_t *src) {
    BSON_ASSERT_PARAM(dst);
    BSON_ASSERT_PARAM(src);

    _mc_array_destroy(dst);

    dst->len = src->len;
    dst->element_size = src->element_size;
    dst->allocated = src->allocated;
    dst->data = (void *)bson_malloc(dst->allocated);
    memcpy(dst->data, src->data, dst->allocated);
}

void _mc_array_destroy(mc_array_t *array) {
    if (array && array->data) {
        bson_free(array->data);
    }
}

void _mc_array_append_vals(mc_array_t *array, const void *data, uint32_t n_elements) {
    size_t len;
    size_t off;
    size_t next_size;

    BSON_ASSERT_PARAM(array);
    BSON_ASSERT_PARAM(data);

    BSON_ASSERT(array->len <= SIZE_MAX / array->element_size);
    off = array->element_size * array->len;
    BSON_ASSERT(n_elements <= SIZE_MAX / array->element_size);
    len = (size_t)n_elements * array->element_size;
    BSON_ASSERT(len <= SIZE_MAX - off);
    if ((off + len) > array->allocated) {
        next_size = bson_next_power_of_two(off + len);
        array->data = (void *)bson_realloc(array->data, next_size);
        array->allocated = next_size;
    }

    memcpy((uint8_t *)array->data + off, data, len);

    BSON_ASSERT(array->len <= SIZE_MAX - n_elements);
    array->len += n_elements;
}
