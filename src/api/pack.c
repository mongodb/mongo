/* Copyright (c) 2010 WiredTiger, Inc.  All rights reserved. */

#include "wiredtiger.h"

int wiredtiger_struct_sizev(const char *fmt, va_list ap)
{
	return 0;
}

int wiredtiger_struct_packv(void *buffer, int size, const char *fmt, va_list ap)
{
	return 0;
}

int wiredtiger_struct_unpackv(const void *buffer, int size, const char *fmt, va_list ap)
{
	return 0;
}

int wiredtiger_struct_size(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	return wiredtiger_struct_sizev(fmt, ap);
}

int wiredtiger_struct_pack(void *buffer, int size, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	return wiredtiger_struct_packv(buffer, size, fmt, ap);
}

int wiredtiger_struct_unpack(const void *buffer, int size, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	return wiredtiger_struct_unpackv(buffer, size, fmt, ap);
}
