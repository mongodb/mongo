/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __curindex_first --
 *	WT_CURSOR->first method for the index cursor type.
 */
static int
__curindex_first(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curindex_last --
 *	WT_CURSOR->last method for the index cursor type.
 */
static int
__curindex_last(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curindex_next --
 *	WT_CURSOR->next method for the index cursor type.
 */
static int
__curindex_next(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curindex_prev --
 *	WT_CURSOR->prev method for the index cursor type.
 */
static int
__curindex_prev(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curindex_search_near --
 *	WT_CURSOR->search_near method for the index cursor type.
 */
static int
__curindex_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_UNUSED(cursor);
	WT_UNUSED(exact);

	return (ENOTSUP);
}

/*
 * __curindex_insert --
 *	WT_CURSOR->insert method for the index cursor type.
 */
static int
__curindex_insert(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curindex_update --
 *	WT_CURSOR->update method for the index cursor type.
 */
static int
__curindex_update(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curindex_remove --
 *	WT_CURSOR->remove method for the index cursor type.
 */
static int
__curindex_remove(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curindex_close --
 *	WT_CURSOR->close method for the index cursor type.
 */
static int
__curindex_close(WT_CURSOR *cursor, const char *config)
{
	return (__wt_cursor_close(cursor, config));
}

/*
 * __wt_curindex_open --
 *	WT_SESSION->open_cursor method for index cursors.
 */
int
__wt_curindex_open(WT_SESSION_IMPL *session,
    const char *uri, const char *config, WT_CURSOR **cursorp)
{
	static WT_CURSOR iface = {
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		__curindex_first,
		__curindex_last,
		__curindex_next,
		__curindex_prev,
		NULL,
		__curindex_search_near,
		__curindex_insert,
		__curindex_update,
		__curindex_remove,
		__curindex_close,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	WT_CURSOR_INDEX *cindex;
	WT_CURSOR *cursor;

	WT_UNUSED(uri);

	WT_RET(__wt_calloc_def(session, 1, &cindex));

	cursor = &cindex->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->key_format = cursor->value_format = "S";

	STATIC_ASSERT(offsetof(WT_CURSOR_INDEX, iface) == 0);
	__wt_cursor_init(cursor, 1, config);
	*cursorp = cursor;

	return (0);
}
