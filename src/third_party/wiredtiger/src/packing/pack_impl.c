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
