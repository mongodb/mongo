/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_strtouq --
 *     Convert a string to an unsigned long integer. Effectively uses `strtoull`.
 */
uint64_t
__wt_strtouq(const char *nptr, char **endptr, int base)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    static_assert(
      sizeof(uint64_t) == sizeof(unsigned long long), "unsigned long long is not 64 bytes");

    return (strtoull(nptr, endptr, base));
}
