/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * wiredtiger_struct_pack --
 *     Pack a byte string (extension API).
 */
int
wiredtiger_struct_pack(WT_SESSION *wt_session, void *buffer, size_t len, const char *format, ...)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    session = (WT_SESSION_IMPL *)wt_session;

    va_start(ap, format);
    ret = __wt_struct_packv(session, buffer, len, format, ap);
    va_end(ap);

    return (ret);
}

/*
 * wiredtiger_struct_size --
 *     Calculate the size of a packed byte string (extension API).
 */
int
wiredtiger_struct_size(WT_SESSION *wt_session, size_t *lenp, const char *format, ...)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    session = (WT_SESSION_IMPL *)wt_session;

    va_start(ap, format);
    ret = __wt_struct_sizev(session, lenp, format, ap);
    va_end(ap);

    return (ret);
}

/*
 * wiredtiger_struct_unpack --
 *     Unpack a byte string (extension API).
 */
int
wiredtiger_struct_unpack(
  WT_SESSION *wt_session, const void *buffer, size_t len, const char *format, ...)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    session = (WT_SESSION_IMPL *)wt_session;

    va_start(ap, format);
    ret = __wt_struct_unpackv(session, buffer, len, format, ap);
    va_end(ap);

    return (ret);
}

/*
 * __wt_ext_struct_pack --
 *     Pack a byte string (extension API).
 */
int
__wt_ext_struct_pack(
  WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, void *buffer, size_t len, const char *fmt, ...)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    session = (wt_session != NULL) ? (WT_SESSION_IMPL *)wt_session :
                                     ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

    va_start(ap, fmt);
    ret = __wt_struct_packv(session, buffer, len, fmt, ap);
    va_end(ap);

    return (ret);
}

/*
 * __wt_ext_struct_size --
 *     Calculate the size of a packed byte string (extension API).
 */
int
__wt_ext_struct_size(
  WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, size_t *lenp, const char *fmt, ...)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    session = (wt_session != NULL) ? (WT_SESSION_IMPL *)wt_session :
                                     ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

    va_start(ap, fmt);
    ret = __wt_struct_sizev(session, lenp, fmt, ap);
    va_end(ap);

    return (ret);
}

/*
 * __wt_ext_struct_unpack --
 *     Unpack a byte string (extension API).
 */
int
__wt_ext_struct_unpack(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const void *buffer,
  size_t len, const char *fmt, ...)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    session = (wt_session != NULL) ? (WT_SESSION_IMPL *)wt_session :
                                     ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

    va_start(ap, fmt);
    ret = __wt_struct_unpackv(session, buffer, len, fmt, ap);
    va_end(ap);

    return (ret);
}
