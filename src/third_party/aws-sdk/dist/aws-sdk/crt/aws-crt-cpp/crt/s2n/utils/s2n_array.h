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

#include "api/s2n.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_result.h"

#define S2N_INITIAL_ARRAY_SIZE 16

struct s2n_array {
    /* Pointer to elements in array */
    struct s2n_blob mem;

    /* The total number of elements currently in the array. */
    uint32_t len;

    /* The size of each element in the array */
    uint32_t element_size;
};

S2N_RESULT s2n_array_validate(const struct s2n_array *array);
struct s2n_array *s2n_array_new(uint32_t element_size);
struct s2n_array *s2n_array_new_with_capacity(uint32_t element_size, uint32_t capacity);
S2N_RESULT s2n_array_init(struct s2n_array *array, uint32_t element_size);
S2N_RESULT s2n_array_init_with_capacity(struct s2n_array *array, uint32_t element_size, uint32_t capacity);
S2N_RESULT s2n_array_pushback(struct s2n_array *array, void **element);
S2N_RESULT s2n_array_get(struct s2n_array *array, uint32_t idx, void **element);
S2N_RESULT s2n_array_insert(struct s2n_array *array, uint32_t idx, void **element);
S2N_RESULT s2n_array_insert_and_copy(struct s2n_array *array, uint32_t idx, void *element);
S2N_RESULT s2n_array_num_elements(struct s2n_array *array, uint32_t *len);
S2N_RESULT s2n_array_capacity(struct s2n_array *array, uint32_t *capacity);
S2N_RESULT s2n_array_remove(struct s2n_array *array, uint32_t idx);
S2N_CLEANUP_RESULT s2n_array_free_p(struct s2n_array **parray);
S2N_RESULT s2n_array_free(struct s2n_array *array);
