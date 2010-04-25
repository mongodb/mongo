/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int  __wt_bt_page_inmem_col_fix(DB *, WT_PAGE *);
static int  __wt_bt_page_inmem_col_int(WT_PAGE *);
static int  __wt_bt_page_inmem_col_leaf(WT_PAGE *);
static int  __wt_bt_page_inmem_dup_leaf(DB *, WT_PAGE *);
static int  __wt_bt_page_inmem_item_int(DB *, WT_PAGE *);
static int  __wt_bt_page_inmem_row_leaf(DB *, WT_PAGE *);

/*
 * __wt_bt_page_alloc --
 *	Allocate a new btree page from the cache.
 */
int
__wt_bt_page_alloc(
    WT_TOC *toc, u_int type, u_int level, u_int32_t size, WT_PAGE **pagep)
{
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;

	WT_RET((__wt_page_alloc(toc, size, &page)));

	/* In-memory page structure: the space-available and first-free byte. */
	__wt_bt_set_ff_and_sa_from_addr(page, WT_PAGE_BYTE(page));

	/*
	 * Generally, the default values of 0 on page are correct; set the type
	 * and the level, and set the related page addresses to the "unset"
	 * value.
	 */
	hdr = page->hdr;
	hdr->type = (u_int8_t)type;
	hdr->level = (u_int8_t)level;

	*pagep = page;
	return (0);
}

/*
 * __wt_bt_page_in --
 *	Get a btree page from the cache.
 */
int
__wt_bt_page_in(
    WT_TOC *toc, u_int32_t addr, u_int32_t size, int inmem, WT_PAGE **pagep)
{
	DB *db;
	WT_PAGE *page;

	db = toc->db;

	WT_RET((__wt_page_in(toc, addr, size, &page)));

	/* Verify the page. */
	WT_ASSERT(toc->env, __wt_bt_verify_page(toc, page, NULL) == 0);

	/* Optionally build the in-memory version of the page. */
	if (inmem && page->indx_count == 0)
		WT_RET((__wt_bt_page_inmem(db, page)));

	*pagep = page;
	return (0);
}

/*
 * __wt_bt_page_out --
 *	Return a btree page to the cache.
 */
void
__wt_bt_page_out(WT_TOC *toc, WT_PAGE **pagep, u_int32_t flags)
{
	WT_PAGE *page;

	WT_ENV_FCHK_ASSERT(
	    toc->env, "__wt_bt_page_out", flags, WT_APIMASK_BT_PAGE_OUT);

	/*
	 * This function exists to help in debugging: First, clear the caller's
	 * reference so we don't accidentally use a page after discarding our
	 * reference.  Second, verify the page if we're running in diagnostic
	 * mode.
	 */
	page = *pagep;
	*pagep = NULL;

	WT_ASSERT(toc->env, __wt_bt_verify_page(toc, page, NULL) == 0);

	/* The caller may have decided the page isn't worth keeping around. */
	if (LF_ISSET(WT_DISCARD))
		F_SET(page, WT_DISCARD);

	/*
	 * Clearing the hazard reference technically belongs in the cache layer,
	 * but this is a trivial function already.
	 */
	__wt_hazard_clear(toc, page);
}

/*
 * __wt_bt_page_inmem --
 *	Build in-memory page information.
 */
int
__wt_bt_page_inmem(DB *db, WT_PAGE *page)
{
	ENV *env;
	WT_PAGE_HDR *hdr;
	u_int32_t nindx;
	int ret;

	env = db->env;
	hdr = page->hdr;
	ret = 0;

	WT_ASSERT(env, page->u.indx == NULL);

	/*
	 * Determine the maximum number of indexes we'll need for this page.
	 * The value is correct for page types other than WT_PAGE_ROW_LEAF --
	 * entries on that page includes the number of duplicate items, so the
	 * number of entries in the header is potentially more than actually
	 * needed.
	 */
	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
		nindx = hdr->u.entries;
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		nindx = hdr->u.entries / 2;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/* Allocate an array of WT_{ROW,COL}_INDX structures for the page. */
	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		WT_RET((__wt_calloc(env,
		    nindx, sizeof(WT_COL_INDX), &page->u.c_indx)));
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET((__wt_calloc(env,
		    nindx, sizeof(WT_ROW_INDX), &page->u.r_indx)));
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
		ret = __wt_bt_page_inmem_col_fix(db, page);
		break;
	case WT_PAGE_COL_INT:
		ret = __wt_bt_page_inmem_col_int(page);
		break;
	case WT_PAGE_COL_VAR:
		ret = __wt_bt_page_inmem_col_leaf(page);
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		ret = __wt_bt_page_inmem_item_int(db, page);
		break;
	case WT_PAGE_DUP_LEAF:
		ret = __wt_bt_page_inmem_dup_leaf(db, page);
		break;
	case WT_PAGE_ROW_LEAF:
		ret = __wt_bt_page_inmem_row_leaf(db, page);
		break;
	WT_ILLEGAL_FORMAT(db);
	}
	return (ret);
}

/*
 * __wt_bt_page_inmem_item_int --
 *	Build in-memory index for row-store and off-page duplicate tree
 *	internal pages.
 */
static int
__wt_bt_page_inmem_item_int(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_ITEM *item;
	WT_OFF *off;
	WT_PAGE_HDR *hdr;
	WT_ROW_INDX *ip;
	u_int64_t records;
	u_int32_t i;

	idb = db->idb;
	hdr = page->hdr;
	ip = page->u.r_indx;
	records = 0;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 *
	 * The page contains sorted key/offpage-reference pairs.  Keys are
	 * on-page (WT_ITEM_KEY) or overflow (WT_ITEM_KEY_OVFL) items.
	 * Offpage references are WT_ITEM_OFF items.
	 */
	WT_ITEM_FOREACH(page, item, i)
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
			if (idb->huffman_key == NULL) {
				ip->data = WT_ITEM_BYTE(item);
				ip->size = WT_ITEM_LEN(item);
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_KEY_OVFL:
			ip->data = item;
			ip->size = 0;
			break;
		case WT_ITEM_OFF:
			off = WT_ITEM_BYTE_OFF(item);
			records += WT_RECORDS(off);
			ip->page_data = item;
			++ip;
			break;
		WT_ILLEGAL_FORMAT(db);
		}

	page->indx_count = hdr->u.entries / 2;
	page->records = records;

	__wt_bt_set_ff_and_sa_from_addr(page, (u_int8_t *)item);
	return (0);
}

/*
 * __wt_bt_page_inmem_row_leaf --
 *	Build in-memory index for row-store leaf pages.
 */
static int
__wt_bt_page_inmem_row_leaf(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_ITEM *item;
	WT_ROW_INDX *ip;
	u_int32_t i, indx_count;
	u_int64_t records;

	idb = db->idb;
	records = 0;

	/*
	 * Walk a row-store page of WT_ITEMs, building indices and finding the
	 * end of the page.
	 *
	 * The page contains key/data pairs.  Keys are on-page (WT_ITEM_KEY) or
	 * overflow (WT_ITEM_KEY_OVFL) items.  The data sets are either: a
	 * single on-page (WT_ITEM_DATA) or overflow (WT_ITEM_DATA_OVFL) item;
	 * a group of duplicate data items where each duplicate is an on-page
	 * (WT_ITEM_DUP) or overflow (WT_ITEM_DUP_OVFL) item; or an offpage
	 * reference (WT_ITEM_OFF).
	 */
	ip = NULL;
	indx_count = 0;
	WT_ITEM_FOREACH(page, item, i)
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (ip == NULL)
				ip = page->u.r_indx;
			else
				++ip;
			if (idb->huffman_key != NULL ||
			    WT_ITEM_TYPE(item) == WT_ITEM_KEY_OVFL) {
				ip->data = item;
				ip->size = 0;
			} else {
				ip->data = WT_ITEM_BYTE(item);
				ip->size = WT_ITEM_LEN(item);
			}
			++indx_count;
			break;
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DUP:
		case WT_ITEM_DUP_OVFL:
			/*
			 * The page_data field references the first of the
			 * duplicate data sets, only set it if it hasn't yet
			 * been set.
			 */
			if (ip->page_data == NULL)
				ip->page_data = item;
			++records;
			break;
		case WT_ITEM_OFF:
			ip->page_data = item;
			records += WT_ROW_OFF_RECORDS(ip);
			break;
		WT_ILLEGAL_FORMAT(db);
		}

	page->indx_count = indx_count;
	page->records = records;

	__wt_bt_set_ff_and_sa_from_addr(page, (u_int8_t *)item);
	return (0);
}

/*
 * __wt_bt_page_inmem_col_int --
 *	Build in-memory index for column-store internal pages.
 */
static int
__wt_bt_page_inmem_col_int(WT_PAGE *page)
{
	WT_COL_INDX *ip;
	WT_OFF *off;
	WT_PAGE_HDR *hdr;
	u_int64_t records;
	u_int32_t i;

	hdr = page->hdr;
	ip = page->u.c_indx;
	records = 0;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 * The page contains WT_OFF structures.
	 */
	WT_OFF_FOREACH(page, off, i) {
		ip->page_data = off;
		++ip;
		records += WT_RECORDS(off);
	}

	page->indx_count = hdr->u.entries;
	page->records = records;

	__wt_bt_set_ff_and_sa_from_addr(page, (u_int8_t *)off);
	return (0);
}

/*
 * __wt_bt_page_inmem_col_leaf --
 *	Build in-memory index for variable-length, data-only leaf pages in
 *	column-store trees.
 */
static int
__wt_bt_page_inmem_col_leaf(WT_PAGE *page)
{
	WT_COL_INDX *ip;
	WT_ITEM *item;
	WT_PAGE_HDR *hdr;
	u_int32_t i;

	hdr = page->hdr;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 * The page contains unsorted data items.  The data items are on-page
	 * (WT_ITEM_DATA) or overflow (WT_ITEM_DATA_OVFL) items.
	 */
	ip = page->u.c_indx;
	WT_ITEM_FOREACH(page, item, i) {
		ip->page_data = item;
		++ip;
	}

	page->indx_count = hdr->u.entries;
	page->records = hdr->u.entries;

	__wt_bt_set_ff_and_sa_from_addr(page, (u_int8_t *)item);
	return (0);
}

/*
 * __wt_bt_page_inmem_dup_leaf --
 *	Build in-memory index for variable-length, data-only leaf pages in
 *	duplicate trees.
 */
static int
__wt_bt_page_inmem_dup_leaf(DB *db, WT_PAGE *page)
{
	WT_ROW_INDX *ip;
	WT_ITEM *item;
	WT_PAGE_HDR *hdr;
	u_int32_t i;

	hdr = page->hdr;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 * The page contains sorted data items.  The data items are on-page
	 * (WT_ITEM_DUP) or overflow (WT_ITEM_DUP_OVFL) items.
	 */
	ip = page->u.r_indx;
	WT_ITEM_FOREACH(page, item, i) {
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_DUP:
			ip->data = WT_ITEM_BYTE(item);
			ip->size = WT_ITEM_LEN(item);
			break;
		case WT_ITEM_DUP_OVFL:
			ip->data = WT_ITEM_BYTE(item);
			ip->size = 0;
			break;
		WT_ILLEGAL_FORMAT(db);
		}
		ip->page_data = item;
		++ip;
	}

	page->indx_count = hdr->u.entries;
	page->records = hdr->u.entries;

	__wt_bt_set_ff_and_sa_from_addr(page, (u_int8_t *)item);
	return (0);
}

/*
 * __wt_bt_page_inmem_col_fix --
 *	Build in-memory index for column-store fixed-length leaf pages.
 */
static int
__wt_bt_page_inmem_col_fix(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_COL_INDX *ip;
	WT_PAGE_HDR *hdr;
	u_int64_t records;
	u_int32_t i, j;
	u_int8_t *p;

	idb = db->idb;
	hdr = page->hdr;
	ip = page->u.c_indx;
	records = 0;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 * The page contains fixed-length objects.
	 */
	if (F_ISSET(idb, WT_REPEAT_COMP))
		WT_FIX_REPEAT_ITERATE(db, page, p, i, j) {
			ip->page_data = p;
			++ip;
			++records;
		}
	else
		WT_FIX_FOREACH(db, page, p, i) {
			ip->page_data = p;
			++ip;
			++records;
		}

	page->indx_count = hdr->u.entries;
	page->records = records;

	__wt_bt_set_ff_and_sa_from_addr(page, (u_int8_t *)p);
	return (0);
}

/*
 * __wt_bt_key_process --
 *	Overflow and/or compressed on-page items need processing before
 *	we look at them.
 */
int
__wt_bt_key_process(WT_TOC *toc, WT_ROW_INDX *ip, DBT *dbt)
{
	DBT local_dbt;
	ENV *env;
	IDB *idb;
	WT_ITEM *item;
	WT_OVFL *ovfl;
	WT_PAGE *ovfl_page;
	u_int32_t size;
	int ret;
	void *orig;

	env = toc->env;
	idb = toc->db->idb;
	ovfl_page = NULL;
	ret = 0;

	WT_ASSERT(env, ip->size == 0);

	/*
	 * 3 cases:
	 * (1) Uncompressed overflow item
	 * (2) Compressed overflow item
	 * (3) Compressed on-page item
	 *
	 * In these cases, the WT_ROW_INDX data field points to the on-page
	 * item.   We're going to process that item to create an in-memory
	 * key.
	 *
	 * If the item is an overflow item, bring it into memory.
	 */
	item = ip->data;
	if (WT_ITEM_TYPE(item) == WT_ITEM_KEY_OVFL) {
		ovfl = WT_ITEM_BYTE_OVFL(item);
		WT_RET(
		    __wt_bt_ovfl_in(toc, ovfl->addr, ovfl->size, &ovfl_page));
		orig = WT_PAGE_BYTE(ovfl_page);
		size = ovfl->size;
	} else {
		orig = WT_ITEM_BYTE(item);
		size = WT_ITEM_LEN(item);
	}

	/*
	 * When returning keys to the application, this function is called with
	 * a DBT into which to copy the key; if that isn't given, copy the key
	 * into allocated memory in the WT_ROW_INDX structure.
	 */
	if (dbt == NULL) {
		WT_CLEAR(local_dbt);
		dbt = &local_dbt;
	}

	/* Copy the item into place; if the item is compressed, decode it. */
	if (idb->huffman_key == NULL) {
		if (size > dbt->mem_size)
			WT_ERR(__wt_realloc(
			    env, &dbt->mem_size, size, &dbt->data));
		dbt->size = size;
		memcpy(dbt->data, orig, size);
	} else
		WT_ERR(__wt_huffman_decode(idb->huffman_key,
		    orig, size, &dbt->data, &dbt->mem_size, &dbt->size));

	/*
	 * If no target DBT specified (that is, we're intending to persist this
	 * conversion in our in-memory tree), update the WT_ROW_INDX reference
	 * with the processed key.
	 */
	if (dbt == &local_dbt) {
		ip->data = dbt->data;
		ip->size = dbt->size;
	}

err:	if (ovfl_page != NULL)
		__wt_bt_page_out(toc, &ovfl_page, 0);

	return (ret);
}
