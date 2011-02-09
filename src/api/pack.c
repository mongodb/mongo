/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

size_t
wiredtiger_struct_sizev(const char *fmt, va_list ap)
{
	size_t size;

	size = 0;

	switch (fmt[0]) {
	case 'S':
		size += strlen(va_arg(ap, const char *)) + 1;
		break;

	case 'u':
		size += va_arg(ap, WT_DATAITEM *)->size;
		break;
	}

	return (size);
}

int
wiredtiger_struct_packv(void *buffer, size_t size, const char *fmt, va_list ap)
{
	WT_DATAITEM *item;
	uint8_t *p;
	const void *src;
	const char *str;
	size_t sz;

	p = buffer;
	switch (fmt[0]) {
	case 'S':
		str = va_arg(ap, const char *);
		src = str;
		sz = strlen(str) + 1;
		break;

	case 'u':
		item = va_arg(ap, WT_DATAITEM *);
		src = item->data;
		sz = item->size;
		break;

	default:
		return (EINVAL);
	}

	if (sz > size)
		return (ENOMEM);
	memcpy(p, src, sz);
	p += sz;
	size -= sz;

	if (fmt[1] != '\0')
		return (EINVAL);

	return (0);
}

int
wiredtiger_struct_unpackv(
    const void *buffer, size_t size, const char *fmt, va_list ap)
{
	WT_DATAITEM *item;
	const uint8_t *p;
	const char **strp;

	p = buffer;
	switch (fmt[0]) {
	case 'S':
		strp = va_arg(ap, const char **);
		*strp = (const char *)p;
		break;
	case 'u':
		item = va_arg(ap, WT_DATAITEM *);
		item->data = p;
		item->size = (uint32_t)size;
		break;

	default:
		return (EINVAL);
	}

	if (fmt[1] != '\0')
		return (EINVAL);

	return (0);
}

size_t
wiredtiger_struct_size(const char *fmt, ...)
{
	va_list ap;
	size_t size;

	va_start(ap, fmt);
	size = wiredtiger_struct_sizev(fmt, ap);
	va_end(ap);

	return (size);
}

int
wiredtiger_struct_pack(void *buffer, size_t size, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = wiredtiger_struct_packv(buffer, size, fmt, ap);
	va_end(ap);

	return (ret);
}

int
wiredtiger_struct_unpack(const void *buffer, size_t size, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = wiredtiger_struct_unpackv(buffer, size, fmt, ap);
	va_end(ap);

	return (ret);
}
