/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __curconfig_first --
 *	WT_CURSOR->first method for the config cursor type.
 */
static int
__curconfig_first(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curconfig_last --
 *	WT_CURSOR->last method for the config cursor type.
 */
static int
__curconfig_last(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curconfig_next --
 *	WT_CURSOR->next method for the config cursor type.
 */
static int
__curconfig_next(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curconfig_prev --
 *	WT_CURSOR->prev method for the config cursor type.
 */
static int
__curconfig_prev(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curconfig_search_near --
 *	WT_CURSOR->search_near method for the config cursor type.
 */
static int
__curconfig_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_UNUSED(cursor);
	WT_UNUSED(exact);

	return (ENOTSUP);
}

/*
 * __curconfig_insert --
 *	WT_CURSOR->insert method for the config cursor type.
 */
static int
__curconfig_insert(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curconfig_update --
 *	WT_CURSOR->update method for the config cursor type.
 */
static int
__curconfig_update(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curconfig_remove --
 *	WT_CURSOR->remove method for the config cursor type.
 */
static int
__curconfig_remove(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curconfig_close --
 *	WT_CURSOR->close method for the config cursor type.
 */
static int
__curconfig_close(WT_CURSOR *cursor, const char *config)
{
	return (__wt_cursor_close(cursor, config));
}

/*
 * __wt_curconfig_open --
 *	WT_SESSION->open_cursor method for config cursors.
 */
int
__wt_curconfig_open(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR **cursorp)
{
	static WT_CURSOR iface = {
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		__curconfig_first,
		__curconfig_last,
		__curconfig_next,
		__curconfig_prev,
		NULL,
		__curconfig_search_near,
		__curconfig_insert,
		__curconfig_update,
		__curconfig_remove,
		__curconfig_close,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ 0 },                  /* recno raw buffer */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	WT_CURSOR_CONFIG *cconfig;
	WT_CURSOR *cursor;

	WT_UNUSED(uri);

	WT_RET(__wt_calloc_def(session, 1, &cconfig));

	cursor = &cconfig->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->key_format = cursor->value_format = "S";

	STATIC_ASSERT(offsetof(WT_CURSOR_CONFIG, iface) == 0);
	__wt_cursor_init(cursor, 0, 1, cfg);
	*cursorp = cursor;

	return (0);
}
