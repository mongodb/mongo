/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "intpack.i"
#include "packing.i"

/*
 * __wt_struct_check --
 *	Check that the specified packing format is valid, and whether it will
 *	be encoded into a fixed size.
 */
int
__wt_struct_check(WT_SESSION_IMPL *session,
    const char *fmt, size_t len, int *fixedp, uint32_t *fixed_lenp)
{
	WT_PACK pack;
	WT_PACK_VALUE pv;
	char *endp, t;
	int ret;

	WT_RET(__pack_initn(session, &pack, fmt, len));

	while ((ret = __pack_next(&pack, &pv)) == 0)
		;

	if (ret != WT_NOTFOUND)
		return (ret);

	if (fixedp != NULL && fixed_lenp != NULL) {
		if (len > 1 && ((t = fmt[len - 1]) == 'u' || t == 'S')) {
			*fixed_lenp = (uint32_t)strtol(fmt, &endp, 10);
			*fixedp = (endp == fmt + len - 1);
		} else
			*fixedp = 0;
	}

	return (0);
}

/*
 * __wt_struct_sizev --
 *	Calculate the size of a packed byte string (va_list version).
 */
size_t
__wt_struct_sizev(WT_SESSION_IMPL *session, const char *fmt, va_list ap)
{
	WT_ITEM *item;
	WT_PACK pack;
	WT_PACK_VALUE pv;
	const char *s;
	size_t len, padding, total;
	int ret;

	total = 0;

	if (__pack_init(session, &pack, fmt) != 0)
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
 * __wt_struct_packv --
 *	Pack a byte string (va_list version).
 */
int
__wt_struct_packv(WT_SESSION_IMPL *session,
    void *buffer, size_t size, const char *fmt, va_list ap)
{
	WT_ITEM *item;
	WT_PACK pack;
	WT_PACK_VALUE pv;
	const char *s;
	uint8_t *p, *end;
	size_t len, padding;
	int ret;

	WT_RET(__pack_init(session, &pack, fmt));

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
 * __wt_struct_unpackv --
 *	Unpack a byte string (va_list version).
 */
int
__wt_struct_unpackv(WT_SESSION_IMPL *session,
    const void *buffer, size_t size, const char *fmt, va_list ap)
{
	WT_ITEM *item;
	WT_PACK pack;
	WT_PACK_VALUE pv;
	const uint8_t *p, *end;
	const char **strp;
	size_t len;
	uint64_t u;
	int64_t i;
	int ret;

	WT_RET(__pack_init(session, &pack, fmt));

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
