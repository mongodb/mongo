/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_bt_ovfl_in --
 *	Read an overflow item from the disk.
 */
int
__wt_bt_ovfl_in(WT_TOC *toc, WT_OVFL *ovfl, WT_PAGE **pagep, int dsk_verify)
{
	DB *db;
	WT_OFF off;
	WT_REF ref;

	db = toc->db;

	/*
	 * Read an overflow page, using an overflow structure on a page for
	 * which we (better) have a hazard reference.
	 */
	__wt_bt_gen_ref_pair(&ref, &off,
	    ovfl->addr, WT_HDR_BYTES_TO_ALLOC(db, ovfl->size));
	WT_RET(__wt_bt_page_in(toc, &ref, &off, dsk_verify));

	*pagep = ref.page;
	return (0);
}
