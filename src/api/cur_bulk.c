/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __curbulk_insert --
 *	WT_CURSOR->insert for the bulk cursor type.
 */
static int
__curbulk_insert(WT_CURSOR *cursor)
{
	BTREE *btree;
	CURSOR_BULK *cbulk;

	cbulk = (CURSOR_BULK *)cursor;
	btree = cbulk->cbt.btree;

	/* TODO: check the state of the key/value pair. */
#if 0
	These are errors set in the original bulk-load code that need
	to be reported somewhere.

	/* if key value specified to a column-store */
	__wt_errx(session,
	    "column-store keys are implied and should not be returned by "
	    "the bulk load input routine"

	/* if 0-length key to row-store */
	__wt_errx(session, "zero-length keys are not supported");

	/* the high-bit is set on fixed-length keys to signify deletion. */
	__wt_errx(session,
	    "the first bit may not be stored in fixed-length column-store "
	    "file items");
#endif

	WT_RET(__wt_bulk_insert(cbulk));

	return (0);
}

/*
 * __curbulk_close --
 *	WT_CURSOR->close for the bulk cursor type.
 */
static int
__curbulk_close(WT_CURSOR *cursor, const char *config)
{
	CURSOR_BULK *cbulk;
	int ret;

	WT_UNUSED(config);

	cbulk = (CURSOR_BULK *)cursor;
	ret = 0;

	WT_TRET(__wt_bulk_end(cbulk));
	WT_TRET(__wt_cursor_close(cursor, config));

	return (ret);
}

/*
 * __wt_curbulk_init --
 *	initialize a bulk cursor.
 */
int
__wt_curbulk_init(CURSOR_BULK *cbulk)
{
	WT_CURSOR *c = &cbulk->cbt.iface;

	c->insert = __curbulk_insert;
	c->close = __curbulk_close;

	WT_RET(__wt_bulk_init(cbulk));

	return (0);
}
