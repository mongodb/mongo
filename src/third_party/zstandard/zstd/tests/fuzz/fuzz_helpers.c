/*
 * Copyright (c) Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */
#include "fuzz_helpers.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void* FUZZ_malloc(size_t size)
{
    if (size > 0) {
        void* const mem = malloc(size);
        FUZZ_ASSERT(mem);
        return mem;
    }
    return NULL;
}

int FUZZ_memcmp(void const* lhs, void const* rhs, size_t size)
{
    if (size == 0) {
        return 0;
    }
    return memcmp(lhs, rhs, size);
}
