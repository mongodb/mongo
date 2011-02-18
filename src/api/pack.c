/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

size_t wiredtiger_struct_sizev(const char *fmt, va_list ap)
{
	WT_UNUSED(fmt);
	WT_UNUSED(ap);

	return 0;
}

int wiredtiger_struct_packv(void *buffer, size_t size, const char *fmt, va_list ap)
{
	WT_UNUSED(buffer);
	WT_UNUSED(size);
	WT_UNUSED(fmt);
	WT_UNUSED(ap);

	return 0;
}

int wiredtiger_struct_unpackv(const void *buffer, size_t size, const char *fmt, va_list ap)
{
	WT_UNUSED(buffer);
	WT_UNUSED(size);
	WT_UNUSED(fmt);
	WT_UNUSED(ap);

	return 0;
}

size_t wiredtiger_struct_size(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	return wiredtiger_struct_sizev(fmt, ap);
}

int wiredtiger_struct_pack(void *buffer, size_t size, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	return wiredtiger_struct_packv(buffer, size, fmt, ap);
}

int wiredtiger_struct_unpack(const void *buffer, size_t size, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	return wiredtiger_struct_unpackv(buffer, size, fmt, ap);
}
