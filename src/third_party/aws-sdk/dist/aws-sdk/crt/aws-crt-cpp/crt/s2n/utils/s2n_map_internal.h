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

#include "utils/s2n_map.h"

struct s2n_map_entry {
    struct s2n_blob key;
    struct s2n_blob value;
};

struct s2n_map {
    /* The total capacity of the table, in number of elements. */
    uint32_t capacity;

    /* The total number of elements currently in the table. Used for measuring the load factor */
    uint32_t size;

    /* Once a map has been looked up, it is considered immutable */
    int immutable;

    /* Pointer to the hash-table, should be capacity * sizeof(struct s2n_map_entry) */
    struct s2n_map_entry *table;
};
