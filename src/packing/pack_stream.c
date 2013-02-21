/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Streaming interface to packing.
 *
 * This allows applications to pack or unpack records one field at a time.
 */
struct __wt_pack_stream {
	WT_PACK pack;
	uint8_t *end, *p;
};

int
wiredtiger_pack_start(WT_SESSION *wt_session, const char *format, void *buffer, size_t len, WT_PACK_STREAM **psp)
{
	WT_DECL_RET;
	WT_PACK_STREAM *ps;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	WT_RET(__wt_calloc_def(session, 1, &ps));
	WT_ERR(__pack_init(session, &ps->pack, format));
	ps->p = buffer;
	ps->end = ps->p + len;
	*psp = ps;

	if (0) {
err:		(void)wiredtiger_pack_close(ps);
	}
	return (ret);
}

int
wiredtiger_unpack_start(WT_SESSION *wt_session, const char *format, const void *buffer, size_t size, WT_PACK_STREAM **psp)
{
	return (wiredtiger_pack_start(wt_session, format, (void *)buffer, size, psp));
}

int
wiredtiger_pack_close(WT_PACK_STREAM *ps)
{
	if (ps != NULL)
		__wt_free(ps->pack.session, ps);

	return (0);
}

int
wiredtiger_pack_item(WT_PACK_STREAM *ps, WT_ITEM *item)
{
	WT_PACK_VALUE pv;
	WT_SESSION_IMPL *session;

	session = ps->pack.session;
	WT_RET(__pack_next(&ps->pack, &pv));
	pv.u.item.data = item->data;
	pv.u.item.size = item->size;
	WT_RET(__pack_write(session, &pv, &ps->p, (size_t)(ps->end - ps->p)));

	return (0);
}

int
wiredtiger_pack_int(WT_PACK_STREAM *ps, int64_t i)
{
	WT_PACK_VALUE pv;
	WT_SESSION_IMPL *session;

	session = ps->pack.session;
	WT_RET(__pack_next(&ps->pack, &pv));
	pv.u.i = i;
	WT_RET(__pack_write(session, &pv, &ps->p, (size_t)(ps->end - ps->p)));

	return (0);
}

int
wiredtiger_pack_str(WT_PACK_STREAM *ps, const char *s)
{
	WT_PACK_VALUE pv;
	WT_SESSION_IMPL *session;

	session = ps->pack.session;
	WT_RET(__pack_next(&ps->pack, &pv));
	pv.u.s = s;
	WT_RET(__pack_write(session, &pv, &ps->p, (size_t)(ps->end - ps->p)));

	return (0);
}

int
wiredtiger_pack_uint(WT_PACK_STREAM *ps, uint64_t u)
{
	WT_PACK_VALUE pv;
	WT_SESSION_IMPL *session;

	session = ps->pack.session;
	WT_RET(__pack_next(&ps->pack, &pv));
	pv.u.u = u;
	WT_RET(__pack_write(session, &pv, &ps->p, (size_t)(ps->end - ps->p)));

	return (0);
}

int
wiredtiger_unpack_item(WT_PACK_STREAM *ps, WT_ITEM *item)
{
	WT_PACK_VALUE pv;
	WT_SESSION_IMPL *session;

	session = ps->pack.session;
	WT_RET(__pack_next(&ps->pack, &pv));
	WT_RET(__unpack_read(session,
	    &pv, (const uint8_t **)&ps->p, (size_t)(ps->end - ps->p)));
	item->data = pv.u.item.data;
	item->size = pv.u.item.size;
	return (0);
}

int
wiredtiger_unpack_int(WT_PACK_STREAM *ps, int64_t *ip)
{
	WT_PACK_VALUE pv;
	WT_SESSION_IMPL *session;

	session = ps->pack.session;
	WT_RET(__pack_next(&ps->pack, &pv));
	WT_RET(__unpack_read(session,
	    &pv, (const uint8_t **)&ps->p, (size_t)(ps->end - ps->p)));
	*ip = pv.u.i;
	return (0);
}

int
wiredtiger_unpack_str(WT_PACK_STREAM *ps, const char **sp)
{
	WT_PACK_VALUE pv;
	WT_SESSION_IMPL *session;

	session = ps->pack.session;
	WT_RET(__pack_next(&ps->pack, &pv));
	WT_RET(__unpack_read(session,
	    &pv, (const uint8_t **)&ps->p, (size_t)(ps->end - ps->p)));
	*sp = pv.u.s;
	return (0);
}

int
wiredtiger_unpack_uint(WT_PACK_STREAM *ps, uint64_t *up)
{
	WT_PACK_VALUE pv;
	WT_SESSION_IMPL *session;

	session = ps->pack.session;
	WT_RET(__pack_next(&ps->pack, &pv));
	WT_RET(__unpack_read(session,
	    &pv, (const uint8_t **)&ps->p, (size_t)(ps->end - ps->p)));
	*up = pv.u.u;
	return (0);
}
