/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int
__curbulk_insert(WT_CURSOR *cursor)
{
	ICURSOR_BULK *cbulk;

	cbulk = (ICURSOR_BULK *)cursor;

	/* TODO: check the state of the key/value pair. */

	WT_RET(__wt_bulk_var_insert(cbulk));

	return (0);

}

static int
__curbulk_close(WT_CURSOR *cursor, const char *config)
{
	ICURSOR_BULK *cbulk;
	int ret;

	WT_UNUSED(config);

	cbulk = (ICURSOR_BULK *)cursor;
	ret = 0;

	WT_TRET(__wt_bulk_end(cbulk));
	WT_TRET(__wt_curstd_close(cursor, config));

	return (ret);
}

int
__wt_curbulk_init(ICURSOR_BULK *cbulk)
{
	WT_CURSOR *c = &cbulk->ctable.cstd.iface;

	c->insert = __curbulk_insert;
	c->close = __curbulk_close;

	WT_RET(__wt_bulk_init(cbulk));

	return (0);
}
