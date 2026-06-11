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

#include "utils/s2n_blob.h"

#include <ctype.h>
#include <string.h>
#include <sys/param.h>

#include "api/s2n.h"
#include "error/s2n_errno.h"
#include "utils/s2n_safety.h"

S2N_RESULT s2n_blob_validate(const struct s2n_blob *b)
{
    RESULT_ENSURE_REF(b);
    RESULT_DEBUG_ENSURE(S2N_IMPLIES(b->data == NULL, b->size == 0), S2N_ERR_SAFETY);
    RESULT_DEBUG_ENSURE(S2N_IMPLIES(b->data == NULL, b->allocated == 0), S2N_ERR_SAFETY);
    RESULT_DEBUG_ENSURE(S2N_IMPLIES(b->growable == 0, b->allocated == 0), S2N_ERR_SAFETY);
    RESULT_DEBUG_ENSURE(S2N_IMPLIES(b->growable == 1, b->allocated > 0 || b->size == 0), S2N_ERR_SAFETY);
    RESULT_DEBUG_ENSURE(S2N_IMPLIES(b->growable != 0, b->size <= b->allocated), S2N_ERR_SAFETY);
    RESULT_DEBUG_ENSURE(S2N_MEM_IS_READABLE(b->data, b->allocated), S2N_ERR_SAFETY);
    RESULT_DEBUG_ENSURE(S2N_MEM_IS_READABLE(b->data, b->size), S2N_ERR_SAFETY);
    return S2N_RESULT_OK;
}

/**
 * Initialize a blob to reference some data.
 *  
 * `b` will not free `data`. The caller is responsible for making sure that
 * `data` outlives `b`.
 */
int s2n_blob_init(struct s2n_blob *b, uint8_t *data, uint32_t size)
{
    POSIX_ENSURE_REF(b);
    POSIX_ENSURE(S2N_MEM_IS_READABLE(data, size), S2N_ERR_SAFETY);
    *b = (struct s2n_blob){ .data = data, .size = size, .allocated = 0, .growable = 0 };
    POSIX_POSTCONDITION(s2n_blob_validate(b));
    return S2N_SUCCESS;
}

int s2n_blob_zero(struct s2n_blob *b)
{
    POSIX_PRECONDITION(s2n_blob_validate(b));
    POSIX_CHECKED_MEMSET(b->data, 0, MAX(b->allocated, b->size));
    POSIX_POSTCONDITION(s2n_blob_validate(b));
    return S2N_SUCCESS;
}

/**
 * Set `slice` to reference some portion of `b`.
 * 
 * The caller is responsible for ensuring that the data pointed to by `b` outlives
 * `slice`.
 */
int s2n_blob_slice(const struct s2n_blob *b, struct s2n_blob *slice, uint32_t offset, uint32_t size)
{
    POSIX_PRECONDITION(s2n_blob_validate(b));
    POSIX_PRECONDITION(s2n_blob_validate(slice));

    uint32_t slice_size = 0;
    POSIX_GUARD(s2n_add_overflow(offset, size, &slice_size));
    POSIX_ENSURE(b->size >= slice_size, S2N_ERR_SIZE_MISMATCH);
    slice->data = (b->data) ? (b->data + offset) : NULL;
    slice->size = size;
    slice->growable = 0;
    slice->allocated = 0;

    POSIX_POSTCONDITION(s2n_blob_validate(slice));
    return S2N_SUCCESS;
}

int s2n_blob_char_to_lower(struct s2n_blob *b)
{
    POSIX_PRECONDITION(s2n_blob_validate(b));
    for (size_t i = 0; i < b->size; i++) {
        b->data[i] = tolower(b->data[i]);
    }
    POSIX_POSTCONDITION(s2n_blob_validate(b));
    return S2N_SUCCESS;
}
