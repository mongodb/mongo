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

#include <stdbool.h>
#include <stdint.h>

#include "utils/s2n_result.h"

struct s2n_blob {
    /* The data for the s2n_blob */
    uint8_t *data;

    /* The size of the data */
    uint32_t size;

    /* The amount of memory allocated for this blob (i.e. the amount of memory
     * which needs to be freed when the blob is cleaned up). If this blob was
     * created with s2n_blob_init(), this value is 0. If s2n_alloc() was called,
     * this value will be greater than or equal to size.
     * 
     * size < allocated implies that an allocated blob is being reused to store 
     * a smaller amount of data.
     */
    uint32_t allocated;

    /* An allocated blob (e.g.`s2n_alloc`) is always growable. A "reference"
     * blob (from `s2n_blob_init`) is never growable.
     * 
     * This field is necessary to distinguish zero-sized allocated blobs from 
     * zero-sized "reference" blobs. Zero-sized allocated blobs can not be
     * constructed with s2n_alloc or s2n_realloc, but they are directly initialized
     * in s2n_free_object.
     */
    unsigned growable : 1;
};

bool s2n_blob_is_growable(const struct s2n_blob *b);
S2N_RESULT s2n_blob_validate(const struct s2n_blob *b);
int S2N_RESULT_MUST_USE s2n_blob_init(struct s2n_blob *b, uint8_t *data, uint32_t size);
int s2n_blob_zero(struct s2n_blob *b);
int S2N_RESULT_MUST_USE s2n_blob_char_to_lower(struct s2n_blob *b);
int S2N_RESULT_MUST_USE s2n_blob_slice(const struct s2n_blob *b, struct s2n_blob *slice, uint32_t offset, uint32_t size);

#define s2n_stack_blob(name, requested_size, maximum)   \
    size_t name##_requested_size = (requested_size);    \
    uint8_t name##_buf[(maximum)] = { 0 };              \
    POSIX_ENSURE_LTE(name##_requested_size, (maximum)); \
    struct s2n_blob name = { 0 };                       \
    POSIX_GUARD(s2n_blob_init(&name, name##_buf, name##_requested_size))

#define RESULT_STACK_BLOB(name, requested_size, maximum) \
    size_t name##_requested_size = (requested_size);     \
    uint8_t name##_buf[(maximum)] = { 0 };               \
    RESULT_ENSURE_LTE(name##_requested_size, (maximum)); \
    struct s2n_blob name = { 0 };                        \
    RESULT_GUARD_POSIX(s2n_blob_init(&name, name##_buf, name##_requested_size))

#define S2N_BLOB_LABEL(name, str)       \
    static uint8_t name##_data[] = str; \
    const struct s2n_blob name = { .data = name##_data, .size = sizeof(name##_data) - 1 };
