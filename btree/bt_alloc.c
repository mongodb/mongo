/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_bt_table_extend(WT_TOC *, uint32_t *, uint32_t);

/*
 * __wt_bt_table_alloc --
 *	Alloc a chunk of space from the underlying file.
 */
int
__wt_bt_table_alloc(WT_TOC *toc, uint32_t *addrp, uint32_t size)
{
	IDB *idb;

	idb = toc->db->idb;

	__wt_bt_table_extend(toc, addrp, size);

	WT_STAT_INCR(idb->stats, DB_ALLOC);

	return (0);
}

/*
 * __wt_bt_table_free --
 *	Free a chunk of space to the underlying file.
 */
int
__wt_bt_table_free(WT_TOC *toc, uint32_t addr, uint32_t size)
{
	IDB *idb;

	idb = toc->db->idb;

	WT_STAT_INCR(idb->stats, DB_FREE);
	return (0);
}

/*
 * __wt_bt_table_extend --
 *	Extend the file to allocate space.
 */
static void
__wt_bt_table_extend(WT_TOC *toc, uint32_t *addrp, uint32_t size)
{
	DB *db;
	IDB *idb;
	WT_FH *fh;

	db = toc->db;
	idb = db->idb;
	fh = idb->fh;

	/* Extend the file. */
	*addrp = WT_OFF_TO_ADDR(db, fh->file_size);
	fh->file_size += size;

	WT_STAT_INCR(idb->stats, DB_ALLOC_FILE);
}
