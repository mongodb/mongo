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
	int ret;

	ret = 0;
	WT_TRET(__wt_cursor_close(cursor, config));
	return (ret);
}

/*
 * __wt_curconfig_open --
 *	WT_SESSION->open_cursor method for config cursors.
 */
int
__wt_curconfig_open(SESSION *session,
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
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	const char *configitem;
	CURSOR_CONFIG *cconfig;
	WT_CURSOR *cursor;
	int ret;

	WT_UNUSED(config);
	configitem = uri + 7;
	ret = 0;

	WT_RET(__wt_calloc(session, 1, sizeof(CURSOR_CONFIG), &cconfig));

	cursor = &cconfig->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->key_format = cursor->value_format = "S";
	__wt_cursor_init(cursor, config);

	STATIC_ASSERT(offsetof(CURSOR_CONFIG, iface) == 0);
	TAILQ_INSERT_HEAD(&session->cursors, cursor, q);
	*cursorp = cursor;

	return (ret);
}

