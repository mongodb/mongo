/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

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
		__wt_cursor_notsup,	/* first */
		__wt_cursor_notsup,	/* last */
		__wt_cursor_notsup,	/* next */
		__wt_cursor_notsup,	/* prev */
		__wt_cursor_notsup,	/* search */
					/* search-near */
		(int (*)(WT_CURSOR *, int *))__wt_cursor_notsup,
		__wt_cursor_notsup,	/* insert */
		__wt_cursor_notsup,	/* update */
		__wt_cursor_notsup,	/* remove */
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
