/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

void
__wt_progress(const char *s, uint64_t v)
{
	(void)printf("\r\t%s: %llu", s, (unsigned long long)v);
	(void)fflush(stdout);
}
