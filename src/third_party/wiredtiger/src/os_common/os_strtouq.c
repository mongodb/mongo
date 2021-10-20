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
 *     Convert a string to an unsigned quad integer.
 */
uint64_t
__wt_strtouq(const char *nptr, char **endptr, int base)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
#if defined(HAVE_STRTOUQ)
    return (strtouq(nptr, endptr, base));
#else
    WT_STATIC_ASSERT(sizeof(uint64_t) == sizeof(unsigned long long));

    return (strtoull(nptr, endptr, base));
#endif
}
