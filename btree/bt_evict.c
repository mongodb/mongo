/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_db_fopen --
 *	Open an underlying file.
 */
int
__wt_db_fopen(WT_BTREE *bt)
{
	DB *db;
	IDB *idb;
	IENV *ienv;
	off_t size;
	int ret;

	db = bt->db;
	ienv = db->ienv;
	idb = db->idb;

	/* Try and open the fle. */
	if ((ret = __wt_open(ienv, idb->file_name, idb->mode,
	    F_ISSET(idb, WT_CREATE) ? WT_OPEN_CREATE : 0, &bt->fh)) != 0)
		return (ret);

	if ((ret = __wt_filesize(ienv, bt->fh, &size)) != 0)
		goto err;

	/*
	 * Convert the size in bytes to "fragments".  If part of the write
	 * of a fragment failed, pretend it all failed, and truncate the
	 * file.
	 */
	bt->frags = (u_int32_t)(size / db->fragsize);

	return (0);

err:	(void)__wt_close(ienv, bt->fh);
	return (ret);
}

/*
 * __wt_db_fclose --
 *	Close an underlying file.
 */
int
__wt_db_fclose(WT_BTREE *bt)
{
	IENV *ienv;

	ienv = bt->db->ienv;

	return (__wt_close(ienv, bt->fh));
}

/*
 * __wt_db_falloc --
 *	Allocate a chunk of a file.
 */
int
__wt_db_falloc(DB *db, u_int32_t frags, WT_PAGE **pagep)
{
	IENV *ienv;
	WT_BTREE *bt;
	WT_PAGE *page;
	int ret;

	ienv = db->ienv;
	bt = db->idb->btree;

	*pagep = NULL;

	if (UINT32_MAX - bt->frags < frags) {
		__wt_db_errx(db,
		    "Requested additional space is not available; the file"
		    " cannot grow that much");
		return (WT_ERROR);
	}

	/*
	 * Allocate memory for the in-memory page information.  It's a separate
	 * allocation call from the page because we hope to interact gracefully
	 * with the underlying heap memory allocator.
	 */
	if ((ret = __wt_calloc(ienv, 1, sizeof(WT_PAGE), &page)) != 0)
		return (ret);

	/*
	 * Allocate the memory to hold the page -- clear the memory, as code
	 * depends on values in the page being zero.
	 */
	if ((ret = __wt_calloc(
	    ienv, 1, (size_t)WT_FRAGS_TO_BYTES(db, frags), &page->hdr)) != 0)
		goto err;

	page->addr = bt->frags;
	bt->frags += frags;
	page->frags = frags;

	/* Initialize the rest of the in-memory page structure. */
	__wt_page_inmem(db, page, 1);

	*pagep = page;
	return (0);

err:	if (page->hdr != NULL)
		__wt_free(ienv, page->hdr);
	__wt_free(ienv, page);
	return (ret);
}

/*
 * __wt_db_fread --
 *	Read a chunk of a file.
 */
int
__wt_db_fread(DB *db,
    u_int32_t addr, u_int32_t frags, WT_PAGE **pagep, u_int32_t flags)
{
	IENV *ienv;
	WT_BTREE *bt;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	size_t bytes;
	int ret;

	DB_FLAG_CHK(db, "__wt_db_fread", flags, WT_APIMASK_DB_FREAD);

	ienv = db->ienv;
	bt = db->idb->btree;

	*pagep = page = NULL;

	/* Allocate the memory to hold it. */
	bytes = (size_t)WT_FRAGS_TO_BYTES(db, frags);
	if ((ret = __wt_malloc(ienv, bytes, &hdr)) != 0)
		return (ret);

	/* Read the page. */
	if ((ret = __wt_read(ienv,
	    bt->fh, WT_FRAGS_TO_BYTES(db, addr), bytes, hdr)) != 0)
		goto err;

	/* Verify the checksum. */
	if (!LF_ISSET(WT_NO_CHECKSUM)) {
		u_int32_t checksum = hdr->checksum;
		hdr->checksum = 0;
		if (checksum != __wt_cksum(hdr, bytes)) {
			ret = __wt_cksum_err(db, addr);
			goto err;
		}
	}

	/* Allocate an in-memory page structure. */
	if ((ret = __wt_calloc(ienv, 1, sizeof(WT_PAGE), &page)) != 0)
		goto err;

	page->addr = addr;
	page->frags = frags;
	page->hdr = hdr;
	__wt_page_inmem(db, page, 0);

#ifdef HAVE_DIAGNOSTIC
	/* Verify the page. */
	if ((ret = __wt_db_page_verify(db, page, NULL)) != 0)
		goto err;
#endif

	*pagep = page;
	return (0);

err:	if (page != NULL)
		__wt_free(ienv, page);
	__wt_free(ienv, hdr);
	return (ret);
}

/*
 * __wt_db_fwrite --
 *	Write a chunk of a file.
 */
int
__wt_db_fwrite(DB *db, WT_PAGE *page)
{
	IENV *ienv;
	WT_BTREE *bt;
	WT_PAGE_HDR *hdr;
	size_t bytes;
	int ret;

	ienv = db->ienv;
	bt = db->idb->btree;

#ifdef HAVE_DIAGNOSTIC
	/* Verify the page. */
	if ((ret = __wt_db_page_verify(db, page, NULL)) != 0)
		return (ret);
#endif

	bytes = (size_t)WT_FRAGS_TO_BYTES(db, page->frags);

	/* Update the checksum. */
	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, bytes);

	/* Write the page. */
	return (__wt_write(ienv, bt->fh,
	    (off_t)WT_FRAGS_TO_BYTES(db, page->addr), bytes, hdr));
}

/*
 * __wt_db_fdiscard --
 *	Discard a page of a file.
 */
int
__wt_db_fdiscard(DB *db, WT_PAGE *page)
{
	IENV *ienv;
	int ret;

	ienv = db->ienv;

#ifdef HAVE_DIAGNOSTIC
	/* Verify the page. */
	if ((ret = __wt_db_page_verify(db, page, NULL)) != 0)
		return (ret);
#endif

	__wt_free(ienv, page->hdr);
	__wt_free(ienv, page);
	return (0);
}
