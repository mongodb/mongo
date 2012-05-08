/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * wiredtiger_struct_size --
 *	Calculate the size of a packed byte string.
 */
size_t
wiredtiger_struct_size(const char *fmt, ...)
{
	WT_SESSION_IMPL session;
	va_list ap;
	size_t size;

	WT_CLEAR(session);

	va_start(ap, fmt);
	size = __wt_struct_sizev(&session, fmt, ap);
	va_end(ap);

	return (size);
}

/*
 * wiredtiger_struct_pack --
 *	Pack a byte string.
 */
int
wiredtiger_struct_pack(void *buffer, size_t size, const char *fmt, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL session;
	va_list ap;

	WT_CLEAR(session);

	va_start(ap, fmt);
	ret = __wt_struct_packv(&session, buffer, size, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * wiredtiger_struct_unpack --
 *	Unpack a byte string.
 */
int
wiredtiger_struct_unpack(const void *buffer, size_t size, const char *fmt, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL session;
	va_list ap;

	WT_CLEAR(session);

	va_start(ap, fmt);
	ret = __wt_struct_unpackv(&session, buffer, size, fmt, ap);
	va_end(ap);

	return (ret);
}
