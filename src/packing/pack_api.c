/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * wiredtiger_struct_pack --
 *	Pack a byte string (extension API).
 */
int
wiredtiger_struct_pack(WT_SESSION *wt_session,
    void *buffer, size_t size, const char *fmt, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	session = (WT_SESSION_IMPL *)wt_session;

	va_start(ap, fmt);
	ret = __wt_struct_packv(session, buffer, size, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * wiredtiger_struct_size --
 *	Calculate the size of a packed byte string (extension API).
 */
int
wiredtiger_struct_size(WT_SESSION *wt_session,
    size_t *sizep, const char *fmt, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	session = (WT_SESSION_IMPL *)wt_session;

	va_start(ap, fmt);
	ret = __wt_struct_sizev(session, sizep, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __wt_ext_struct_unpack --
 *	Unpack a byte string (extension API).
 */
int
wiredtiger_struct_unpack(WT_SESSION *wt_session,
    const void *buffer, size_t size, const char *fmt, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	session = (WT_SESSION_IMPL *)wt_session;

	va_start(ap, fmt);
	ret = __wt_struct_unpackv(session, buffer, size, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __wt_ext_struct_pack --
 *	Pack a byte string (extension API).
 */
int
__wt_ext_struct_pack(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
    void *buffer, size_t size, const char *fmt, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	session = (wt_session != NULL) ? (WT_SESSION_IMPL *)wt_session :
	    ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

	va_start(ap, fmt);
	ret = __wt_struct_packv(session, buffer, size, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __wt_ext_struct_size --
 *	Calculate the size of a packed byte string (extension API).
 */
int
__wt_ext_struct_size(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
    size_t *sizep, const char *fmt, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	session = (wt_session != NULL) ? (WT_SESSION_IMPL *)wt_session :
	    ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

	va_start(ap, fmt);
	ret = __wt_struct_sizev(session, sizep, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __wt_ext_struct_unpack --
 *	Unpack a byte string (extension API).
 */
int
__wt_ext_struct_unpack(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
    const void *buffer, size_t size, const char *fmt, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	session = (wt_session != NULL) ? (WT_SESSION_IMPL *)wt_session :
	    ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

	va_start(ap, fmt);
	ret = __wt_struct_unpackv(session, buffer, size, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * wiredtiger_pack_uint_raw --
 *	Pack a 8B value into a buffer, returning the length.
 */
int
wiredtiger_pack_uint_raw(void *buffer, uint64_t u, size_t *sizep)
{
	WT_DECL_RET;
	uint8_t *p;

	p = buffer;
	if ((ret = __wt_vpack_uint(&p, (size_t)0, u)) != 0)
		return (ret);

	*sizep = WT_PTRDIFF(p, buffer);
	return (0);
}

/*
 * wiredtiger_unpack_uint_raw --
 *	Unpack a buffer into an 8B value.
 */
int
wiredtiger_unpack_uint_raw(const void *buffer, uint64_t *up)
{
	const uint8_t *p;

	p = buffer;
	return (__wt_vunpack_uint(&p, (size_t)0, up));
}
