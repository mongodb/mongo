/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

void
__wt_progress(const char *s, u_int64_t v)
{
	printf("\r\t%s: %llu", s, (u_quad)v);
	(void)fflush(stdout);
}
