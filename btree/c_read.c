/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_db_page_discard(DB *, WT_PAGE *);

/*
 * __wt_db_page_open --
 *	Open an underlying file.
 */
int
__wt_db_page_open(WT_BTREE *bt)
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
 * __wt_db_page_close --
 *	Close an underlying file.
 */
int
__wt_db_page_close(DB *db)
{
	WT_BTREE *bt;
	IENV *ienv;
	WT_PAGE *page;
	int ret, tret;

	ienv = db->ienv;
	bt = db->idb->btree;
	ret = 0;

	while ((page = TAILQ_FIRST(&bt->hlru)) != NULL) {
		if (F_ISSET(page, WT_MODIFIED) &&
		    (tret = __wt_db_page_out(db, page, WT_MODIFIED)) != 0 &&
		    ret == 0)
			ret = tret;
		if ((tret = __wt_db_page_discard(db, page)) != 0 && ret == 0)
			ret = tret;
	}

	return (__wt_close(ienv, bt->fh));
}

/*
 * __wt_db_page_alloc --
 *	Allocate fragments from a file.
 */
int
__wt_db_page_alloc(DB *db, u_int32_t frags, WT_PAGE **pagep)
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

	TAILQ_INSERT_HEAD(&bt->hhq[WT_HASH(page->addr)], page, hq);
	TAILQ_INSERT_TAIL(&bt->hlru, page, lq);

	*pagep = page;
	return (0);

err:	if (page->hdr != NULL)
		__wt_free(ienv, page->hdr);
	__wt_free(ienv, page);
	return (ret);
}

/*
 * __wt_db_page_in --
 *	Read fragments from a file.
 */
int
__wt_db_page_in(DB *db,
    u_int32_t addr, u_int32_t frags, WT_PAGE **pagep, u_int32_t flags)
{
	IENV *ienv;
	WT_BTREE *bt;
	WT_PAGE_HQH *hashq;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	size_t bytes;
	int ret;

	DB_FLAG_CHK(db, "__wt_db_page_in", flags, WT_APIMASK_DB_FILE_READ);

	ienv = db->ienv;
	bt = db->idb->btree;

	*pagep = NULL;

	/* Check for the page in the cache. */
	hashq = &bt->hhq[WT_HASH(addr)];
	TAILQ_FOREACH(page, hashq, hq)
		if (page->addr == addr)
			break;
	if (page != NULL) {
		TAILQ_REMOVE(hashq, page, hq);
		TAILQ_REMOVE(&bt->hlru, page, lq);
		TAILQ_INSERT_HEAD(hashq, page, hq);
		TAILQ_INSERT_TAIL(&bt->hlru, page, lq);
		*pagep = page;
		return (0);
	}

	/* Allocate the memory to hold a new page. */
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
			__wt_db_errx(db,
			    "fragment %lu was read and had a checksum error",
			    (u_long)addr);
			ret = WT_ERROR;
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

	/* Insert at the head of the hash queue and the tail of LRU queue. */
	TAILQ_INSERT_HEAD(hashq, page, hq);
	TAILQ_INSERT_TAIL(&bt->hlru, page, lq);

#ifdef HAVE_DIAGNOSTIC
	/* Verify the page. */
	if ((ret = __wt_db_verify_page(db, page, NULL)) != 0)
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
 * __wt_db_page_out --
 *	Write a chunk of a file.
 */
int
__wt_db_page_out(DB *db, WT_PAGE *page, u_int32_t flags)
{
	IENV *ienv;
	WT_BTREE *bt;
	WT_PAGE_HDR *hdr;
	WT_PAGE_HQH *hashq;
	size_t bytes;
	int ret;

	ienv = db->ienv;
	bt = db->idb->btree;

	DB_FLAG_CHK(db, "__wt_db_page_out", flags, WT_APIMASK_DB_FILE_WRITE);

#ifdef HAVE_DIAGNOSTIC
	/* Verify the page. */
	if ((ret = __wt_db_verify_page(db, page, NULL)) != 0)
		return (ret);
#endif

	/*
	 * If the page is dirty, set the modified flag, unless we're going to
	 * write it now.  If we're writing it now, clear the modified flag.
	 * In any case, re-insert the page at the head of the hash queue and
	 * at the tail of the LRU queue.
	 */
	if (LF_ISSET(WT_MODIFIED))
		F_SET(page, WT_MODIFIED);
	if (LF_ISSET(WT_MODIFIED_FLUSH))
		F_CLR(page, WT_MODIFIED);

	hashq = &bt->hhq[WT_HASH(page->addr)];
	TAILQ_REMOVE(hashq, page, hq);
	TAILQ_INSERT_HEAD(hashq, page, hq);
	TAILQ_REMOVE(&bt->hlru, page, lq);
	TAILQ_INSERT_TAIL(&bt->hlru, page, lq);

	if (!LF_ISSET(WT_MODIFIED_FLUSH))
		return (0);

	/*
	 * If the page is dirty, but we don't need to flush it, mark it dirty
	 * and continue.
	 */
	if (!LF_ISSET(WT_MODIFIED_FLUSH)) {
		F_SET(page, WT_MODIFIED);
		return (0);
	}

	/* Otherwise, update the checksum and write the page now. */
	bytes = (size_t)WT_FRAGS_TO_BYTES(db, page->frags);

	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, bytes);

	return (__wt_write(ienv, bt->fh,
	    (off_t)WT_FRAGS_TO_BYTES(db, page->addr), bytes, hdr));
}

/*
 * __wt_db_page_discard --
 *	Discard a page of a file.
 */
static int
__wt_db_page_discard(DB *db, WT_PAGE *page)
{
	IENV *ienv;
	WT_BTREE *bt;
	int ret;

	ienv = db->ienv;
	bt = db->idb->btree;

#ifdef HAVE_DIAGNOSTIC
	/* Verify the page. */
	if ((ret = __wt_db_verify_page(db, page, NULL)) != 0)
		return (ret);
#endif
	TAILQ_REMOVE(&bt->hhq[WT_HASH(page->addr)], page, hq);
	TAILQ_REMOVE(&bt->hlru, page, lq);

	__wt_free(ienv, page->hdr);
	__wt_free(ienv, page);
	return (0);
}
