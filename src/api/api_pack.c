/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "intpack.i"

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
	session.event_handler = __wt_event_handler_default;

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
	WT_SESSION_IMPL session;
	va_list ap;
	int ret;

	WT_CLEAR(session);
	session.event_handler = __wt_event_handler_default;

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
	WT_SESSION_IMPL session;
	va_list ap;
	int ret;

	WT_CLEAR(session);
	session.event_handler = __wt_event_handler_default;

	va_start(ap, fmt);
	ret = __wt_struct_unpackv(&session, buffer, size, fmt, ap);
	va_end(ap);

	return (ret);
}
