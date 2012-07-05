/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curconfig_close --
 *	WT_CURSOR->close method for the config cursor type.
 */
static int
__curconfig_close(WT_CURSOR *cursor)
{
	return (__wt_cursor_close(cursor));
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
		NULL,
		NULL,			/* equals */
		__wt_cursor_notsup,	/* next */
		__wt_cursor_notsup,	/* prev */
		__wt_cursor_notsup,	/* reset */
		__wt_cursor_notsup,	/* search */
					/* search-near */
		(int (*)(WT_CURSOR *, int *))__wt_cursor_notsup,
		__wt_cursor_notsup,	/* insert */
		__wt_cursor_notsup,	/* update */
		__wt_cursor_notsup,	/* remove */
		__curconfig_close,
		(int (*)		/* reconfigure */
		    (WT_CURSOR *, const char *))__wt_cursor_notsup,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ 0 },                  /* recno raw buffer */
		{ NULL, 0, 0, NULL, 0 },/* WT_ITEM key */
		{ NULL, 0, 0, NULL, 0 },/* WT_ITEM value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	WT_CURSOR_CONFIG *cconfig;
	WT_CURSOR *cursor;
	WT_DECL_RET;

	WT_UNUSED(uri);

	WT_RET(__wt_calloc_def(session, 1, &cconfig));

	cursor = &cconfig->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->key_format = cursor->value_format = "S";

	/* __wt_cursor_init is last so we don't have to clean up on error. */
	STATIC_ASSERT(offsetof(WT_CURSOR_CONFIG, iface) == 0);
	WT_ERR(__wt_cursor_init(cursor, uri, 0, cfg, cursorp));

	if (0) {
err:		__wt_free(session, cconfig);
	}
	return (ret);
}
