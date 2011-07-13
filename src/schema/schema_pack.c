/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_struct_check --
 *	Check that the specified packing format is valid, and whether it fits
 *	into a fixed-sized bitfield.
 */
int
__wt_struct_check(WT_SESSION_IMPL *session,
    const char *fmt, size_t len, int *fixedp, uint32_t *fixed_lenp)
{
	WT_PACK pack;
	WT_PACK_VALUE pv;
	int fields, ret;

	WT_RET(__pack_initn(session, &pack, fmt, len));

	for (fields = 0; (ret = __pack_next(&pack, &pv)) == 0; fields++)
		;

	if (ret != WT_NOTFOUND)
		return (ret);

	if (fixedp != NULL && fixed_lenp != NULL) {
		if (fields == 0) {
			*fixedp = 1;
			*fixed_lenp = 0;
		} else if (fields == 1 && pv.type == 't') {
			*fixedp = 1;
			*fixed_lenp = pv.size;
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
	WT_PACK pack;
	WT_PACK_VALUE pv;
	size_t total;
	int ret;

	total = 0;

	if (__pack_init(session, &pack, fmt) != 0)
		return (size_t)-1;

	while ((ret = __pack_next(&pack, &pv)) == 0) {
		WT_PACK_GET(session, pv, ap);
		total += __pack_size(session, &pv);
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
	WT_PACK pack;
	WT_PACK_VALUE pv;
	uint8_t *p, *end;
	int ret;

	WT_RET(__pack_init(session, &pack, fmt));

	p = buffer;
	end = p + size;

	while ((ret = __pack_next(&pack, &pv)) == 0) {
		WT_PACK_GET(session, pv, ap);
		WT_RET(__pack_write(session, &pv, &p, (size_t)(end - p)));
	}

	WT_ASSERT(session, p <= end);

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
	WT_PACK pack;
	WT_PACK_VALUE pv;
	const uint8_t *p, *end;
	int ret;

	WT_RET(__pack_init(session, &pack, fmt));

	p = buffer;
	end = p + size;

	while ((ret = __pack_next(&pack, &pv)) == 0) {
		WT_RET(__unpack_read(session, &pv, &p, (size_t)(end - p)));
		WT_UNPACK_PUT(session, pv, ap);
	}

	WT_ASSERT(session, p <= end);

	if (ret != WT_NOTFOUND)
		return (ret);

	return (0);
}
