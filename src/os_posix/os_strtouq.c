/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Convert a string to an unsigned quad integer.
 */
uint64_t
__wt_strtouq(const char *nptr, char **endptr, int base)
{
#if defined(HAVE_STRTOUQ)
	return (strtouq(nptr, endptr, base));
#else
	STATIC_ASSERT(sizeof(uint64_t) == sizeof(unsigned long long));

	return (strtoull(nptr, endptr, base));
#endif
}
