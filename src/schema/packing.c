/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "intpack.i"

/*
 * Throughout this code we have to be aware of default argument conversion.
 *
 * Refer to Chapter 8 of "Expert C Programming" by Peter van der Linden for the
 * gory details.  The short version is that we have less cases to deal with
 * because the compiler promotes shorter types to int or unsigned int.
 */

typedef struct {
	const char *cur, *end, *orig;
	unsigned long repeats;
} WT_PACK;

typedef struct {
	char type;
	union {
		int64_t i;
		uint64_t u;
		const char *s;
		WT_ITEM *item;
	} u;
	int havesize;
	size_t size;
} WT_PACK_VALUE;

static int
__pack_initn(WT_PACK *pack, const char *fmt, size_t len)
{
	if (*fmt == '@' || *fmt == '<' || *fmt == '>')
		return (EINVAL);
	if (*fmt == '.')
		++fmt;

	pack->cur = pack->orig = fmt;
	pack->end = fmt + len;
	pack->repeats = 0;
	return (0);
}

static int
__pack_init(WT_PACK *pack, const char *fmt)
{
	return (__pack_initn(pack, fmt, strlen(fmt)));
}

static int
__pack_next(WT_PACK *pack, WT_PACK_VALUE *pv)
{
	char *endsize;

	if (pack->repeats > 0) {
		--pack->repeats;
		return (0);
	}

next:	if (*pack->cur == '\0')
		return (WT_NOTFOUND);

	pv->size = strtoul(pack->cur, &endsize, 10);
	pv->havesize = (endsize > pack->cur);
	if (!pv->havesize)
		pv->size = 1;
	pack->cur = endsize;
	pv->type = *pack->cur++;

	switch (pv->type) {
	case 'u':
		/* Special case for items with a size prefix. */
		if (!pv->havesize && *pack->cur != '\0')
			pv->type = 'U';
		/* FALLTHROUGH */
	case 'x':
	case 's':
	case 'S':
		pack->repeats = 0;
		return (0);
	case 'b':
	case 'h':
	case 'i':
	case 'B':
	case 'H':
	case 'I':
	case 'l':
	case 'L':
	case 'q':
	case 'Q':
	case 'r':
		/* Integral types repeat <size> times. */
		if (pv->size == 0)
			goto next;
		pack->repeats = pv->size - 1;
		return (0);
	default:
		WT_ASSERT(NULL, pv->type != pv->type);
		return (EINVAL);
	}

}

/*
 * wiredtiger_struct_sizev --
 *	Calculate the size of a packed byte string (va_list version).
 */
size_t
wiredtiger_struct_sizev(const char *fmt, va_list ap)
{
	WT_ITEM *item;
	WT_PACK pack;
	WT_PACK_VALUE pv;
	const char *s;
	size_t len, padding, total;
	int ret;

	total = 0;

	if (__pack_init(&pack, fmt) != 0)
		return (size_t)-1;

	while ((ret = __pack_next(&pack, &pv)) == 0) {
		switch (pv.type) {
		case 'x':
			total += pv.size;
			break;
		case 's':
		case 'S':
			s = va_arg(ap, const char *);
			len = strlen(s);
			if ((pv.type == 's' || pv.havesize) && pv.size < len) {
				len = pv.size;
				padding = 0;
			} else if (pv.havesize)
				padding = pv.size - len;
			else
				padding = 1;
			total += len + padding;
			break;
		case 'U':
		case 'u':
			item = va_arg(ap, WT_ITEM *);
			len = item->size;
			padding = 0;
			if (pv.havesize && pv.size < len)
				len = pv.size;
			else if (pv.havesize)
				padding = pv.size - len;
			if (pv.type == 'U')
				total += __wt_vsize_uint(len + padding);
			total += len + padding;
			break;
		case 'b':
		case 'h':
		case 'i':
			total += __wt_vsize_int(va_arg(ap, int));
			break;
		case 'B':
		case 'H':
		case 'I':
			total += __wt_vsize_uint(va_arg(ap, unsigned int));
			break;
		case 'l':
			total += __wt_vsize_int(va_arg(ap, long));
			break;
		case 'L':
			total += __wt_vsize_uint(va_arg(ap, unsigned long));
			break;
		case 'q':
			total += __wt_vsize_int(va_arg(ap, int64_t));
			break;
		case 'Q':
		case 'r':
			total += __wt_vsize_uint(va_arg(ap, uint64_t));
			break;
		}
	}

	return (total);
}

/*
 * wiredtiger_struct_packv --
 *	Pack a byte string (va_list version).
 */
int
wiredtiger_struct_packv(
    void *buffer, size_t size, const char *fmt, va_list ap)
{
	WT_ITEM *item;
	WT_PACK pack;
	WT_PACK_VALUE pv;
	WT_SESSION_IMPL *session;
	const char *s;
	uint8_t *p, *end;
	size_t len, padding;
	int ret;

	session = NULL;	/* XXX */

	WT_RET(__pack_init(&pack, fmt));

	p = buffer;
	end = p + size;

	while ((ret = __pack_next(&pack, &pv)) == 0) {
		switch (pv.type) {
		case 'x':
			memset(p, 0, pv.size);
			p += pv.size;
			break;
		case 's':
		case 'S':
			s = va_arg(ap, const char *);
			len = strlen(s);
			if ((pv.type == 's' || pv.havesize) && pv.size < len) {
				len = pv.size;
				padding = 0;
			} else if (pv.havesize)
				padding = pv.size - len;
			else
				padding = 1;
			if (p + len + padding > end)
				return (ENOMEM);
			if (len > 0)
				memcpy(p, s, len);
			p += len;
			if (padding > 0)
				memset(p, 0, padding);
			p += padding;
			break;
		case 'U':
		case 'u':
			item = va_arg(ap, WT_ITEM *);
			len = item->size;
			padding = 0;
			if (pv.havesize && pv.size < len)
				len = pv.size;
			else if (pv.havesize)
				padding = pv.size - len;
			if (pv.type == 'U')
				WT_RET(__wt_vpack_uint(session,
				     &p, (size_t)(end - p), len + padding));
			if (p + len + padding > end)
				return (ENOMEM);
			if (len > 0)
				memcpy(p, item->data, len);
			p += len;
			if (padding > 0)
				memset(p, 0, padding);
			p += padding;
			break;
		case 'b':
		case 'h':
		case 'i':
			WT_RET(__wt_vpack_int(session,
			     &p, (size_t)(end - p), va_arg(ap, int)));
			break;
		case 'B':
		case 'H':
		case 'I':
			WT_RET(__wt_vpack_uint(session,
			     &p, (size_t)(end - p), va_arg(ap, unsigned int)));
			break;
		case 'l':
			WT_RET(__wt_vpack_int(session,
			     &p, (size_t)(end - p), va_arg(ap, long)));
			break;
		case 'L':
			WT_RET(__wt_vpack_uint(session,
			     &p, (size_t)(end - p), va_arg(ap, unsigned long)));
			break;
		case 'q':
			WT_RET(__wt_vpack_int(session,
			     &p, (size_t)(end - p), va_arg(ap, int64_t)));
			break;
		case 'Q':
		case 'r':
			WT_RET(__wt_vpack_uint(session,
			     &p, (size_t)(end - p), va_arg(ap, uint64_t)));
			break;
		}
	}

	if (ret != WT_NOTFOUND)
		return (ret);

	return (0);
}

/*
 * wiredtiger_struct_unpackv --
 *	Unpack a byte string (va_list version).
 */
int
wiredtiger_struct_unpackv(
    const void *buffer, size_t size, const char *fmt, va_list ap)
{
	WT_ITEM *item;
	WT_PACK pack;
	WT_PACK_VALUE pv;
	WT_SESSION_IMPL *session;
	const uint8_t *p, *end;
	const char **strp;
	size_t len;
	uint64_t u;
	int64_t i;
	int ret;

	session = NULL;	/* XXX */

	WT_RET(__pack_init(&pack, fmt));

	p = buffer;
	end = p + size;

	while ((ret = __pack_next(&pack, &pv)) == 0) {
		switch (pv.type) {
		case 'x':
			p += pv.size;
			break;
		case 's':
		case 'S':
			strp = va_arg(ap, const char **);
			if (pv.type == 's' || pv.havesize)
				len = pv.size;
			else
				len = strlen((const char *)p) + 1;
			if (len > 0)
				*strp = (const char *)p;
			p += len;
			break;
		case 'U':
			WT_RET(__wt_vunpack_uint(session,
			     &p, (size_t)(end - p), &u));
			len = (size_t)u;
			/* FALLTHROUGH */
		case 'u':
			item = va_arg(ap, WT_ITEM *);
			if (pv.havesize)
				len = pv.size;
			else if (pv.type != 'U')
				len = (size_t)(end - p);
			item->data = p;
			item->size = (uint32_t)len;
			p += len;
			break;
		case 'b':
		case 'h':
		case 'i':
		case 'l':
		case 'q':
			WT_RET(__wt_vunpack_int(session,
			     &p, (size_t)(end - p), &i));
			switch (pv.type) {
			case 'b':
				*va_arg(ap, int8_t *) = (int8_t)i;
				break;
			case 'h':
				*va_arg(ap, short *) = (short)i;
				break;
			case 'i':
				*va_arg(ap, int *) = (int)i;
				break;
			case 'l':
				*va_arg(ap, long *) = (long)i;
				break;
			case 'q':
				*va_arg(ap, int64_t *) = i;
				break;
			}
			break;
		case 'B':
		case 'H':
		case 'I':
		case 'L':
		case 'Q':
		case 'r':
			WT_RET(__wt_vunpack_uint(session,
			     &p, (size_t)(end - p), &u));
			switch (pv.type) {
			case 'B':
				*va_arg(ap, uint8_t *) = (uint8_t)u;
				break;
			case 'H':
				*va_arg(ap, unsigned short *) =
				    (unsigned short)u;
				break;
			case 'I':
				*va_arg(ap, unsigned int *) = (unsigned int)u;
				break;
			case 'L':
				*va_arg(ap, unsigned long *) = (unsigned long)u;
				break;
			case 'Q':
			case 'r':
				*va_arg(ap, uint64_t *) = u;
				break;
			}
			break;
		}
	}

	if (ret != WT_NOTFOUND)
		return (ret);

	return (0);
}

/*
 * wiredtiger_struct_size --
 *	Calculate the size of a packed byte string.
 */
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

/*
 * wiredtiger_struct_pack --
 *	Pack a byte string.
 */
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

/*
 * wiredtiger_struct_unpack --
 *	Unpack a byte string.
 */
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
