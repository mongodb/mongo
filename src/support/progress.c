/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_progress --
 *	Send a progress message to stdout.
 */
void
__wt_progress(const char *s, uint64_t v)
{
	(void)printf("\r\t%s: %llu", s, (unsigned long long)v);
	(void)fflush(stdout);
}
