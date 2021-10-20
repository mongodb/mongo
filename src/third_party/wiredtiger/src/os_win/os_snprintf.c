/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_vsnprintf_len_incr --
 *     POSIX vsnprintf convenience function, incrementing the returned size.
 */
int
__wt_vsnprintf_len_incr(char *buf, size_t size, size_t *retsizep, const char *fmt, va_list ap)
{
    int len;

    /*
     * WiredTiger calls with length 0 to get the needed buffer size. Call the count only version in
     * this case, _vsnprintf_s will invoke the invalid parameter handler if count is less than or
     * equal to zero.
     */
    if (size == 0) {
        *retsizep += (size_t)_vscprintf(fmt, ap);
        return (0);
    }

    /*
     * Additionally, the invalid parameter handler is invoked if buffer or format is a NULL pointer.
     */
    if (buf == NULL || fmt == NULL)
        return (EINVAL);

    /*
     * If the storage required to store the data and a terminating null exceeds size, the invalid
     * parameter handler is invoked, unless count is _TRUNCATE, in which case as much of the string
     * as will fit in the buffer is written and -1 returned.
     */
    if ((len = _vsnprintf_s(buf, size, _TRUNCATE, fmt, ap)) >= 0) {
        *retsizep += (size_t)len;
        return (0);
    }

    /* Return the buffer size required. */
    if (len == -1)
        *retsizep += (size_t)_vscprintf(fmt, ap);

    return (0);
}
