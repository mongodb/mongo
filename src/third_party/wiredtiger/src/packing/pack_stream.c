/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
	uint8_t *end, *p, *start;
};

/*
 * wiredtiger_pack_start --
 *	Open a stream for packing.
 */
int
wiredtiger_pack_start(WT_SESSION *wt_session,
	const char *format, void *buffer, size_t len, WT_PACK_STREAM **psp)
{
	WT_DECL_RET;
	WT_PACK_STREAM *ps;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	WT_RET(__wt_calloc_one(session, &ps));
	WT_ERR(__pack_init(session, &ps->pack, format));
	ps->p = ps->start = buffer;
	ps->end = ps->p + len;
	*psp = ps;

	if (0) {
err:		(void)wiredtiger_pack_close(ps, NULL);
	}
	return (ret);
}

/*
 * wiredtiger_unpack_start --
 *	Open a stream for unpacking.
 */
int
wiredtiger_unpack_start(WT_SESSION *wt_session, const char *format,
	const void *buffer, size_t size, WT_PACK_STREAM **psp)
{
	return (wiredtiger_pack_start(
	    wt_session, format, (void *)buffer, size, psp));
}

/*
 * wiredtiger_pack_close --
 *	Close a packing stream.
 */
int
wiredtiger_pack_close(WT_PACK_STREAM *ps, size_t *usedp)
{
	if (usedp != NULL)
		*usedp = WT_PTRDIFF(ps->p, ps->start);

	if (ps != NULL)
		__wt_free(ps->pack.session, ps);

	return (0);
}

/*
 * wiredtiger_pack_item --
 *	Pack an item.
 */
int
wiredtiger_pack_item(WT_PACK_STREAM *ps, WT_ITEM *item)
{
	WT_DECL_PACK_VALUE(pv);
	WT_SESSION_IMPL *session;

	session = ps->pack.session;

	/* Lower-level packing routines treat a length of zero as unchecked. */
	if (ps->p >= ps->end)
		return (ENOMEM);

	WT_RET(__pack_next(&ps->pack, &pv));
	switch (pv.type) {
	case 'U':
	case 'u':
		pv.u.item.data = item->data;
		pv.u.item.size = item->size;
		WT_RET(__pack_write(
		    session, &pv, &ps->p, (size_t)(ps->end - ps->p)));
		break;
	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}

/*
 * wiredtiger_pack_int --
 *	Pack a signed integer.
 */
int
wiredtiger_pack_int(WT_PACK_STREAM *ps, int64_t i)
{
	WT_DECL_PACK_VALUE(pv);
	WT_SESSION_IMPL *session;

	session = ps->pack.session;

	/* Lower-level packing routines treat a length of zero as unchecked. */
	if (ps->p >= ps->end)
		return (ENOMEM);

	WT_RET(__pack_next(&ps->pack, &pv));
	switch (pv.type) {
	case 'b':
	case 'h':
	case 'i':
	case 'l':
	case 'q':
		pv.u.i = i;
		WT_RET(__pack_write(
		    session, &pv, &ps->p, (size_t)(ps->end - ps->p)));
		break;
	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}

/*
 * wiredtiger_pack_str --
 *	Pack a string.
 */
int
wiredtiger_pack_str(WT_PACK_STREAM *ps, const char *s)
{
	WT_DECL_PACK_VALUE(pv);
	WT_SESSION_IMPL *session;

	session = ps->pack.session;

	/* Lower-level packing routines treat a length of zero as unchecked. */
	if (ps->p >= ps->end)
		return (ENOMEM);

	WT_RET(__pack_next(&ps->pack, &pv));
	switch (pv.type) {
	case 'S':
	case 's':
		pv.u.s = s;
		WT_RET(__pack_write(
		    session, &pv, &ps->p, (size_t)(ps->end - ps->p)));
		break;
	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}

/*
 * wiredtiger_pack_uint --
 *	Pack an unsigned int.
 */
int
wiredtiger_pack_uint(WT_PACK_STREAM *ps, uint64_t u)
{
	WT_DECL_PACK_VALUE(pv);
	WT_SESSION_IMPL *session;

	session = ps->pack.session;

	/* Lower-level packing routines treat a length of zero as unchecked. */
	if (ps->p >= ps->end)
		return (ENOMEM);

	WT_RET(__pack_next(&ps->pack, &pv));
	switch (pv.type) {
	case 'B':
	case 'H':
	case 'I':
	case 'L':
	case 'Q':
	case 'R':
	case 'r':
	case 't':
		pv.u.u = u;
		WT_RET(__pack_write(
		    session, &pv, &ps->p, (size_t)(ps->end - ps->p)));
		break;
	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}

/*
 * wiredtiger_unpack_item --
 *	Unpack an item.
 */
int
wiredtiger_unpack_item(WT_PACK_STREAM *ps, WT_ITEM *item)
{
	WT_DECL_PACK_VALUE(pv);
	WT_SESSION_IMPL *session;

	session = ps->pack.session;

	/* Lower-level packing routines treat a length of zero as unchecked. */
	if (ps->p >= ps->end)
		return (ENOMEM);

	WT_RET(__pack_next(&ps->pack, &pv));
	switch (pv.type) {
	case 'U':
	case 'u':
		WT_RET(__unpack_read(session,
		    &pv, (const uint8_t **)&ps->p, (size_t)(ps->end - ps->p)));
		item->data = pv.u.item.data;
		item->size = pv.u.item.size;
		break;
	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}

/*
 * wiredtiger_unpack_int --
 *	Unpack a signed integer.
 */
int
wiredtiger_unpack_int(WT_PACK_STREAM *ps, int64_t *ip)
{
	WT_DECL_PACK_VALUE(pv);
	WT_SESSION_IMPL *session;

	session = ps->pack.session;

	/* Lower-level packing routines treat a length of zero as unchecked. */
	if (ps->p >= ps->end)
		return (ENOMEM);

	WT_RET(__pack_next(&ps->pack, &pv));
	switch (pv.type) {
	case 'b':
	case 'h':
	case 'i':
	case 'l':
	case 'q':
		WT_RET(__unpack_read(session,
		    &pv, (const uint8_t **)&ps->p, (size_t)(ps->end - ps->p)));
		*ip = pv.u.i;
		break;
	WT_ILLEGAL_VALUE(session);
	}
	return (0);
}

/*
 * wiredtiger_unpack_str --
 *	Unpack a string.
 */
int
wiredtiger_unpack_str(WT_PACK_STREAM *ps, const char **sp)
{
	WT_DECL_PACK_VALUE(pv);
	WT_SESSION_IMPL *session;

	session = ps->pack.session;

	/* Lower-level packing routines treat a length of zero as unchecked. */
	if (ps->p >= ps->end)
		return (ENOMEM);

	WT_RET(__pack_next(&ps->pack, &pv));
	switch (pv.type) {
	case 'S':
	case 's':
		WT_RET(__unpack_read(session,
		    &pv, (const uint8_t **)&ps->p, (size_t)(ps->end - ps->p)));
		*sp = pv.u.s;
		break;
	WT_ILLEGAL_VALUE(session);
	}
	return (0);
}

/*
 * wiredtiger_unpack_uint --
 *	Unpack an unsigned integer.
 */
int
wiredtiger_unpack_uint(WT_PACK_STREAM *ps, uint64_t *up)
{
	WT_DECL_PACK_VALUE(pv);
	WT_SESSION_IMPL *session;

	session = ps->pack.session;

	/* Lower-level packing routines treat a length of zero as unchecked. */
	if (ps->p >= ps->end)
		return (ENOMEM);

	WT_RET(__pack_next(&ps->pack, &pv));
	switch (pv.type) {
	case 'B':
	case 'H':
	case 'I':
	case 'L':
	case 'Q':
	case 'R':
	case 'r':
	case 't':
		WT_RET(__unpack_read(session,
		    &pv, (const uint8_t **)&ps->p, (size_t)(ps->end - ps->p)));
		*up = pv.u.u;
		break;
	WT_ILLEGAL_VALUE(session);
	}
	return (0);
}
