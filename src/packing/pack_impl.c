/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
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
 * __wt_struct_unpack_size --
 *	Determine the packed size of a buffer matching the format.
 */
int
__wt_struct_unpack_size(WT_SESSION_IMPL *session,
    const void *buffer, size_t size, const char *fmt, size_t *resultp)
{
	WT_DECL_PACK_VALUE(pv);
	WT_DECL_RET;
	WT_PACK pack;
	const uint8_t *p, *end;

	p = buffer;
	end = p + size;

	WT_RET(__pack_init(session, &pack, fmt));
	while ((ret = __pack_next(&pack, &pv)) == 0)
		WT_RET(__unpack_read(session, &pv, &p, (size_t)(end - p)));

	/* Be paranoid - __pack_write should never overflow. */
	WT_ASSERT(session, p <= end);

	if (ret != WT_NOTFOUND)
		return (ret);

	*resultp = WT_PTRDIFF(p, buffer);
	return (0);
}

/*
 * __wt_struct_repack --
 *	Return the subset of the packed buffer that represents part of
 *	the format.  If the result is not contiguous in the existing
 *	buffer, a buffer is reallocated and filled.
 */
int
__wt_struct_repack(WT_SESSION_IMPL *session, const char *infmt,
    const char *outfmt, const WT_ITEM *inbuf, WT_ITEM *outbuf,
    void **reallocp)
{
	WT_DECL_PACK_VALUE(pvin);
	WT_DECL_PACK_VALUE(pvout);
	WT_DECL_RET;
	WT_PACK packin, packout;
	const uint8_t *before, *end, *p;
	uint8_t *newbuf, *pout;
	size_t len;
	const void *start;

	start = newbuf = NULL;
	p = inbuf->data;
	end = p + inbuf->size;

	/*
	 * Handle this non-contiguous case: 'U' -> 'u' at the end of the buf.
	 * The former case has the size embedded before the item, the latter
	 * does not.
	 */
	if ((len = strlen(outfmt)) > 1 && outfmt[len - 1] == 'u' &&
	    strlen(infmt) > len && infmt[len - 1] == 'U') {
		WT_ERR(__wt_realloc(session, NULL, inbuf->size, reallocp));
		pout = *reallocp;
	} else
		pout = NULL;

	WT_ERR(__pack_init(session, &packout, outfmt));
	WT_ERR(__pack_init(session, &packin, infmt));

	/* Outfmt should complete before infmt */
	while ((ret = __pack_next(&packout, &pvout)) == 0) {
		WT_ERR(__pack_next(&packin, &pvin));
		before = p;
		WT_ERR(__unpack_read(session, &pvin, &p, (size_t)(end - p)));
		if (pvout.type != pvin.type) {
			if (pvout.type == 'u' && pvin.type == 'U') {
				/* Skip the prefixed size, we don't need it */
				WT_ERR(__wt_struct_unpack_size(session, before,
				    (size_t)(end - before), "I", &len));
				before += len;
			} else
				WT_ERR(ENOTSUP);
		}
		if (pout != NULL) {
			memcpy(pout, before, WT_PTRDIFF(p, before));
			pout += p - before;
		} else if (start == NULL)
			start = before;
	}
	WT_ERR_NOTFOUND_OK(ret);

	/* Be paranoid - __pack_write should never overflow. */
	WT_ASSERT(session, p <= end);

	if (pout != NULL) {
		outbuf->data = *reallocp;
		outbuf->size = WT_PTRDIFF(pout, *reallocp);
	} else {
		outbuf->data = start;
		outbuf->size = WT_PTRDIFF(p, start);
	}

err:	return (ret);
}
