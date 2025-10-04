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
#include "utils/s2n_array.h"
#include "utils/s2n_result.h"

struct s2n_set {
    struct s2n_array *data;
    int (*comparator)(const void *, const void *);
};

S2N_RESULT s2n_set_validate(const struct s2n_set *set);
struct s2n_set *s2n_set_new(uint32_t element_size, int (*comparator)(const void *, const void *));
S2N_RESULT s2n_set_add(struct s2n_set *set, void *element);
S2N_RESULT s2n_set_get(struct s2n_set *set, uint32_t idx, void **element);
S2N_RESULT s2n_set_remove(struct s2n_set *set, uint32_t idx);
S2N_RESULT s2n_set_free_p(struct s2n_set **pset);
S2N_RESULT s2n_set_free(struct s2n_set *set);
S2N_RESULT s2n_set_len(struct s2n_set *set, uint32_t *len);
