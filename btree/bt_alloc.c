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

#ifdef HAVE_DIAGNOSTIC
static int __wt_bt_table_free_write(WT_TOC *, uint32_t, uint32_t);
#endif

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

/*
 * __wt_bt_table_free --
 *	Free a chunk of space to the underlying file.
 */
int
__wt_bt_table_free(WT_TOC *toc, uint32_t addr, uint32_t size)
{
	WT_STATS *stats;

	stats = toc->db->idb->stats;

#ifdef HAVE_DIAGNOSTIC
	WT_RET(__wt_bt_table_free_write(toc, addr, size));
#endif

	WT_STAT_INCR(stats, DB_FREE);

	return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_bt_table_free_write --
 *	Overwrite the space in the file so future reads don't get fooled.
 *	DIAGNOSTIC only.
 */
static int
__wt_bt_table_free_write(WT_TOC *toc, uint32_t addr, uint32_t size)
{
	DBT *tmp;
	WT_PAGE *page, _page;
	uint32_t allocsize;
	int ret;

	allocsize = toc->db->allocsize;
	ret = 0;

	WT_RET(__wt_scr_alloc(toc, allocsize, &tmp));
	memset(tmp->data, 0, allocsize);

	WT_CLEAR(_page);
	page = &_page;
	page->size = allocsize;
	page->hdr = tmp->data;
	page->hdr->type = WT_PAGE_FREE;
	for (; size >= allocsize; size -= allocsize) {
		page->addr = addr++;
		WT_ERR(__wt_page_write(toc, page));
	}

err:	__wt_scr_release(&tmp);
	return (ret);
}
#endif
