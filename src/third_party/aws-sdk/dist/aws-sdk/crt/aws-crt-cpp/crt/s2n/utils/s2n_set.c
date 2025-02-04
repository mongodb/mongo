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
#include "utils/s2n_set.h"

#include "utils/s2n_array.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_result.h"
#include "utils/s2n_safety.h"

#define S2N_INITIAL_SET_SIZE 16

S2N_RESULT s2n_set_validate(const struct s2n_set *set)
{
    RESULT_ENSURE_REF(set);
    RESULT_GUARD(s2n_array_validate(set->data));
    return S2N_RESULT_OK;
}

/* Sets "out" to the index at which the element should be inserted.
 * Returns an error if the element already exists */
static S2N_RESULT s2n_set_binary_search(struct s2n_set *set, void *element, uint32_t *out)
{
    RESULT_GUARD(s2n_set_validate(set));
    RESULT_ENSURE(S2N_MEM_IS_READABLE(element, set->data->element_size), S2N_ERR_NULL);
    RESULT_ENSURE_REF(out);
    struct s2n_array *array = set->data;
    int (*comparator)(const void *, const void *) = set->comparator;

    uint32_t len = 0;
    RESULT_GUARD(s2n_array_num_elements(array, &len));

    if (len == 0) {
        *out = 0;
        return S2N_RESULT_OK;
    }

    /* Use 64 bit ints to avoid possibility of overflow */
    int64_t low = 0;
    int64_t top = len - 1;

    while (low <= top) {
        int64_t mid = low + ((top - low) / 2);
        void *array_element = NULL;
        RESULT_GUARD(s2n_array_get(array, mid, &array_element));
        int m = comparator(array_element, element);

        /* the element is already in the set */
        if (m == 0) {
            RESULT_BAIL(S2N_ERR_SET_DUPLICATE_VALUE);
        }

        if (m > 0) {
            top = mid - 1;
        } else {
            low = mid + 1;
        }
    }

    *out = low;
    return S2N_RESULT_OK;
}

struct s2n_set *s2n_set_new(uint32_t element_size, int (*comparator)(const void *, const void *))
{
    PTR_ENSURE_REF(comparator);
    struct s2n_blob mem = { 0 };
    PTR_GUARD_POSIX(s2n_alloc(&mem, sizeof(struct s2n_set)));
    struct s2n_set *set = (void *) mem.data;
    *set = (struct s2n_set){ .data = s2n_array_new(element_size), .comparator = comparator };
    if (set->data == NULL) {
        PTR_GUARD_POSIX(s2n_free(&mem));
        return NULL;
    }
    return set;
}

S2N_RESULT s2n_set_add(struct s2n_set *set, void *element)
{
    RESULT_GUARD(s2n_set_validate(set));

    uint32_t idx = 0;
    RESULT_GUARD(s2n_set_binary_search(set, element, &idx));
    RESULT_GUARD(s2n_array_insert_and_copy(set->data, idx, element));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_set_get(struct s2n_set *set, uint32_t idx, void **element)
{
    RESULT_GUARD(s2n_set_validate(set));
    RESULT_ENSURE_REF(element);

    RESULT_GUARD(s2n_array_get(set->data, idx, element));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_set_remove(struct s2n_set *set, uint32_t idx)
{
    RESULT_GUARD(s2n_set_validate(set));
    RESULT_GUARD(s2n_array_remove(set->data, idx));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_set_free_p(struct s2n_set **pset)
{
    RESULT_ENSURE_REF(pset);
    struct s2n_set *set = *pset;

    RESULT_ENSURE_REF(set);
    RESULT_GUARD(s2n_array_free(set->data));

    /* And finally the set object. */
    RESULT_GUARD_POSIX(s2n_free_object((uint8_t **) pset, sizeof(struct s2n_set)));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_set_free(struct s2n_set *set)
{
    RESULT_ENSURE_REF(set);
    return s2n_set_free_p(&set);
}

S2N_RESULT s2n_set_len(struct s2n_set *set, uint32_t *len)
{
    RESULT_GUARD(s2n_set_validate(set));

    RESULT_GUARD(s2n_array_num_elements(set->data, len));

    return S2N_RESULT_OK;
}
