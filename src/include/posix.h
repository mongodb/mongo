/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/* Some systems don't configure 64-bit MIN/MAX by default. */
#ifndef	ULLONG_MAX
#define	ULLONG_MAX	0xffffffffffffffffULL
#endif
#ifndef	LLONG_MAX
#define	LLONG_MAX	0x7fffffffffffffffLL
#endif
#ifndef	LLONG_MIN
#define	LLONG_MIN	(-0x7fffffffffffffffLL - 1)
#endif
