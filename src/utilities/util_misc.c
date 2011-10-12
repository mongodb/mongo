/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
#include "util.h"

int
util_syserr(void)
{
	fprintf(stderr, "%s: %s\n", progname, wiredtiger_strerror(errno));
	return (1);
}

