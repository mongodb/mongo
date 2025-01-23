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

#include <stdint.h>

#include "utils/s2n_blob.h"

int s2n_mem_init(void);
bool s2n_mem_is_init(void);
uint32_t s2n_mem_get_page_size(void);
int s2n_mem_cleanup(void);

/*
 * Generally, s2n_realloc is preferred over s2n_alloc. This is because calling
 * s2n_alloc on a blob that already has memory allocated will leak memory.
*/
int S2N_RESULT_MUST_USE s2n_alloc(struct s2n_blob *b, uint32_t size);
int S2N_RESULT_MUST_USE s2n_realloc(struct s2n_blob *b, uint32_t size);
int s2n_free(struct s2n_blob *b);
int s2n_free_without_wipe(struct s2n_blob *b);
int s2n_free_object(uint8_t **p_data, uint32_t size);
int S2N_RESULT_MUST_USE s2n_dup(struct s2n_blob *from, struct s2n_blob *to);

/* Unlike free, s2n_free_or_wipe accepts static blobs.
 * It frees allocated blobs and wipes static blobs.
 *
 * This is mostly useful for some of the s2n-tls shared secret generation logic,
 * which allocates memory for some algorithms and uses static memory for others.
 *
 * Prefer s2n_free. Only use this method if completely necessary.
 */
int s2n_free_or_wipe(struct s2n_blob *b);

S2N_RESULT s2n_mem_override_callbacks(s2n_mem_init_callback mem_init_callback, s2n_mem_cleanup_callback mem_cleanup_callback,
        s2n_mem_malloc_callback mem_malloc_callback, s2n_mem_free_callback mem_free_callback);
S2N_RESULT s2n_mem_get_callbacks(s2n_mem_init_callback *mem_init_callback, s2n_mem_cleanup_callback *mem_cleanup_callback,
        s2n_mem_malloc_callback *mem_malloc_callback, s2n_mem_free_callback *mem_free_callback);
