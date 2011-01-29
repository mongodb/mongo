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
 * __wt_page_read --
 *	Read a database page (same as read, but verify the checksum).
 */
int
__wt_page_read(DB *db, WT_PAGE *page)
{
	ENV *env;
	WT_FH *fh;
	WT_PAGE_HDR *hdr;
	off_t offset;
	uint32_t checksum;

	env = db->env;
	fh = db->idb->fh;
	hdr = page->hdr;

	offset = WT_ADDR_TO_OFF(db, page->addr);
	WT_RET(__wt_read(env, fh, offset, page->size, hdr));

	checksum = hdr->checksum;
	hdr->checksum = 0;
	if (checksum != __wt_cksum(hdr, page->size)) {
		__wt_api_env_errx(env,
		    "read checksum error: addr/size %lu/%lu at offset %llu",
		    (u_long)page->addr,
		    (u_long)page->size, (unsigned long long)offset);
		return (WT_ERROR);
	}

	return (0);
}

/*
 * __wt_page_write --
 *	Write a database page.
 */
int
__wt_page_write(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	ENV *env;
	WT_FH *fh;
	WT_PAGE_HDR *hdr;

	db = toc->db;
	env = toc->env;
	fh = db->idb->fh;

	WT_ASSERT(env, __wt_verify_dsk_page(toc, page) == 0);

	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, page->size);

	return (__wt_write(
	    env, fh, WT_ADDR_TO_OFF(db, page->addr), page->size, hdr));
}
