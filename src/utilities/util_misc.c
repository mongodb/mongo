/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
#include "util.h"

/*
 * util_err --
 * 	Report an error.
 */
int
util_err(int e, const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "%s: ", progname);
	if (fmt != NULL) {
		va_start(ap, fmt);
		(void)vfprintf(stderr, fmt, ap);
		va_end(ap);
		if (e != 0)
			(void)fprintf(stderr, ": ");
	}
	if (e != 0)
		(void)fprintf(stderr, "%s", wiredtiger_strerror(e));
	(void)fprintf(stderr, "\n");
	return (1);
}
