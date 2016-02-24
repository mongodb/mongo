/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
    const char *fmt, size_t len, bool *fixedp, uint32_t *fixed_lenp)
{
	WT_DECL_PACK_VALUE(pv);
	WT_DECL_RET;
	WT_PACK pack;
	int fields;

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
 * __wt_struct_confchk --
 *	Check that the specified packing format is valid, configuration version.
 */
int
__wt_struct_confchk(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *v)
{
	return (__wt_struct_check(session, v->str, v->len, NULL, NULL));
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

/*
 * __wt_struct_repack --
 *	Return the subset of the packed buffer that represents part of
 *	the format.  If the result is not contiguous in the existing
 *	buffer, a buffer is reallocated and filled.
 */
int
__wt_struct_repack(WT_SESSION_IMPL *session, const char *infmt,
    const char *outfmt, const WT_ITEM *inbuf, WT_ITEM *outbuf)
{
	WT_DECL_PACK_VALUE(pvin);
	WT_DECL_PACK_VALUE(pvout);
	WT_DECL_RET;
	WT_PACK packin, packout;
	const uint8_t *before, *end, *p;
	const void *start;

	start = NULL;
	p = inbuf->data;
	end = p + inbuf->size;

	WT_RET(__pack_init(session, &packout, outfmt));
	WT_RET(__pack_init(session, &packin, infmt));

	/* Outfmt should complete before infmt */
	while ((ret = __pack_next(&packout, &pvout)) == 0) {
		if (p >= end)
			WT_RET(EINVAL);
		if (pvout.type == 'x' && pvout.size == 0 && pvout.havesize)
			continue;
		WT_RET(__pack_next(&packin, &pvin));
		before = p;
		WT_RET(__unpack_read(session, &pvin, &p, (size_t)(end - p)));
		if (pvout.type != pvin.type)
			WT_RET(ENOTSUP);
		if (start == NULL)
			start = before;
	}
	WT_RET_NOTFOUND_OK(ret);

	/* Be paranoid - __pack_write should never overflow. */
	WT_ASSERT(session, p <= end);

	outbuf->data = start;
	outbuf->size = WT_PTRDIFF(p, start);

	return (0);
}
