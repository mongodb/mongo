/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
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
	WT_DECL_RET;
	WT_PACK pack;
	WT_PACK_VALUE pv;
	int fields;

	WT_CLEAR(pv);		/* -Wuninitialized. */

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
int
__wt_struct_sizev(
    WT_SESSION_IMPL *session, size_t *sizep, const char *fmt, va_list ap)
{
	WT_PACK pack;
	WT_PACK_VALUE pv;
	size_t total;

	WT_CLEAR(pv);		/* -Wuninitialized */

	WT_RET(__pack_init(session, &pack, fmt));

	for (total = 0; __pack_next(&pack, &pv) == 0;) {
		WT_PACK_GET(session, pv, ap);
		total += __pack_size(session, &pv);
	}
	*sizep = total;
	return (0);
}

/*
 * __wt_struct_size --
 *	Calculate the size of a packed byte string.
 */
int
__wt_struct_size(WT_SESSION_IMPL *session, size_t *sizep, const char *fmt, ...)
{
	WT_DECL_RET;
	va_list ap;

	va_start(ap, fmt);
	ret = __wt_struct_sizev(session, sizep, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __wt_struct_packv --
 *	Pack a byte string (va_list version).
 */
int
__wt_struct_packv(WT_SESSION_IMPL *session,
    void *buffer, size_t size, const char *fmt, va_list ap)
{
	WT_DECL_RET;
	WT_PACK pack;
	WT_PACK_VALUE pv;
	uint8_t *p, *end;

	WT_CLEAR(pv);		/* -Wuninitialized */

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
 * __wt_struct_pack --
 *	Pack a byte string.
 */
int
__wt_struct_pack(WT_SESSION_IMPL *session,
    void *buffer, size_t size, const char *fmt, ...)
{
	WT_DECL_RET;
	va_list ap;

	va_start(ap, fmt);
	ret = __wt_struct_packv(session, buffer, size, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __wt_struct_unpackv --
 *	Unpack a byte string (va_list version).
 */
int
__wt_struct_unpackv(WT_SESSION_IMPL *session,
    const void *buffer, size_t size, const char *fmt, va_list ap)
{
	WT_DECL_RET;
	WT_PACK pack;
	WT_PACK_VALUE pv;
	const uint8_t *p, *end;

	WT_RET(__pack_init(session, &pack, fmt));

	p = buffer;
	end = p + size;
	WT_CLEAR(pv.u.item);			/* GCC 4.6 lint */

	while ((ret = __pack_next(&pack, &pv)) == 0) {
		WT_RET(__unpack_read(session, &pv, &p, (size_t)(end - p)));
		WT_UNPACK_PUT(session, pv, ap);
	}

	WT_ASSERT(session, p <= end);

	if (ret != WT_NOTFOUND)
		return (ret);

	return (0);
}

/*
 * __wt_struct_unpack --
 *	Unpack a byte string.
 */
int
__wt_struct_unpack(WT_SESSION_IMPL *session,
    const void *buffer, size_t size, const char *fmt, ...)
{
	WT_DECL_RET;
	va_list ap;

	va_start(ap, fmt);
	ret = __wt_struct_unpackv(session, buffer, size, fmt, ap);
	va_end(ap);

	return (ret);
}
