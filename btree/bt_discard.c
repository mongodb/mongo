/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_page_inmem_col_leaf(DB *, WT_PAGE *);
static int __wt_bt_page_inmem_fixed_int(DB *, WT_PAGE *);
static int __wt_bt_page_inmem_item_int(DB *, WT_PAGE *);
static int __wt_bt_page_inmem_row_leaf(DB *, WT_PAGE *);

/*
 * __wt_bt_page_alloc --
 *	Allocate a new btree page from the cache.
 */
int
__wt_bt_page_alloc(WT_TOC *toc, int isleaf, WT_PAGE **pagep)
{
	DB *db;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;

	db = toc->db;

	WT_RET((__wt_cache_alloc(
	    toc, isleaf ? db->leafsize : db->intlsize, &page)));

	/*
	 * Generally, the defaults values of 0 on page are correct; set
	 * the fragment addresses to the "unset" value.
	 */
	hdr = page->hdr;
	hdr->prntaddr = hdr->prevaddr = hdr->nextaddr = WT_ADDR_INVALID;

	/* Set the space-available and first-free byte. */
	__wt_set_ff_and_sa_from_addr(page, WT_PAGE_BYTE(page));

	/* If we're allocating page 0, initialize the WT_PAGE_DESC structure. */
	if (page->addr == 0)
		__wt_bt_desc_init(db, page);

	*pagep = page;
	return (0);
}

/*
 * __wt_bt_page_in --
 *	Get a btree page from the cache.
 */
int
__wt_bt_page_in(
    WT_TOC *toc, u_int32_t addr, int isleaf, int inmem, WT_PAGE **pagep)
{
	DB *db;
	WT_PAGE *page;

	db = toc->db;

	WT_RET((__wt_cache_in(
	    toc, addr, isleaf ? db->leafsize : db->intlsize, 0, &page)));

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
int
__wt_bt_page_out(WT_TOC *toc, WT_PAGE *page, u_int32_t flags)
{
	WT_ASSERT(toc->env, __wt_bt_verify_page(toc, page, NULL) == 0);

	return (__wt_cache_out(toc, page, flags));
}

/*
 * __wt_bt_page_recycle --
 *	Discard any in-memory allocated memory and reset the counters.
 */
void
__wt_bt_page_recycle(ENV *env, WT_PAGE *page)
{
	WT_INDX *indx;
	u_int32_t i;

	WT_ASSERT(env, !F_ISSET(page, WT_MODIFIED));

	if (F_ISSET(page, WT_ALLOCATED)) {
		F_CLR(page, WT_ALLOCATED);
		WT_INDX_FOREACH(page, indx, i)
			if (F_ISSET(indx, WT_ALLOCATED)) {
				F_CLR(indx, WT_ALLOCATED);
				__wt_free(env, indx->data, 0);
			}
	}
	if (page->indx != NULL)
		__wt_free(env, page->indx, 0);

	__wt_free(env, page->hdr, page->bytes);
	__wt_free(env, page, sizeof(WT_PAGE));
}

/*
 * __wt_bt_page_inmem --
 *	Build in-memory page information.
 */
int
__wt_bt_page_inmem(DB *db, WT_PAGE *page)
{
	ENV *env;
	IDB *idb;
	WT_PAGE_HDR *hdr;
	u_int32_t nindx;
	int ret;

	env = db->env;
	idb = db->idb;
	hdr = page->hdr;
	ret = 0;

	/*
	 * Figure out how many indexes we'll need for this page.
	 *
	 * Track the most indexes we find on internal pages, used if we
	 * allocate new internal pages.
	 */
	switch (hdr->type) {
	case WT_PAGE_COL_INT:
		nindx = hdr->u.entries;
		if (idb->indx_size_hint < nindx)
			idb->indx_size_hint = nindx;
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		nindx = hdr->u.entries / 2;
		if (idb->indx_size_hint < nindx)
			idb->indx_size_hint = nindx;
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
		nindx = hdr->u.entries;
		break;
	case WT_PAGE_ROW_LEAF:
		nindx = hdr->u.entries / 2;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * We may be passed a page with a too-small indx array -- in that case,
	 * allocate one of the appropriate size.   We can simply free the indx
	 * array, as any allocated memory in the array  will have have already
	 * been freed.
	 */
	if (page->indx != NULL && page->indx_size < nindx)
		__wt_free(env, page->indx, 0);

	if (page->indx == NULL) {
		WT_RET((__wt_calloc(env, nindx, sizeof(WT_INDX), &page->indx)));
		page->indx_size = nindx;
	}

	switch (hdr->type) {
	case WT_PAGE_COL_INT:
		ret = __wt_bt_page_inmem_fixed_int(db, page);
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		ret = __wt_bt_page_inmem_item_int(db, page);
		break;
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
		ret = __wt_bt_page_inmem_col_leaf(db, page);
		break;
	case WT_PAGE_ROW_LEAF:
		ret = __wt_bt_page_inmem_row_leaf(db, page);
		break;
	WT_ILLEGAL_FORMAT(db);
	}
	return (ret);
}

/*
 * __wt_bt_page_inmem_append --
 *	Append a new WT_ITEM_KEY/WT_OFF pair to an internal page's in-memory
 *	information.
 */
int
__wt_bt_page_inmem_append(DB *db, WT_PAGE *page, WT_ITEM *key, void *page_data)
{
	ENV *env;
	IDB *idb;
	WT_INDX *indx;
	WT_OVFL *ovfl;
	u_int32_t bytes, n;

	env = db->env;
	idb = db->idb;

	/*
	 * Make sure there's enough room in the in-memory index.  We track how
	 * many keys fit on internal pages in this database, use it as a hint.
	 */
	if (page->indx_count == page->indx_size) {
		n = page->indx_size + 50;
		if (idb->indx_size_hint < n)
			idb->indx_size_hint = n;
		else
			n = idb->indx_size_hint;
		bytes = page->indx_size * sizeof(page->indx[0]);
		WT_RET((__wt_realloc(env,
		    &bytes, n * sizeof(page->indx[0]), &page->indx)));
		page->indx_size = n;
	}

	/* Add in the new index entry. */
	indx = page->indx + page->indx_count;
	++page->indx_count;

	/*
	 * If there's a key, fill it in.  On-page keys are directly referenced.
	 * Overflow keys, we grab the size but otherwise leave them alone.
	 */
	if (key != NULL) {
		switch (WT_ITEM_TYPE(key)) {
		case WT_ITEM_KEY:
			indx->data = WT_ITEM_BYTE(key);
			indx->size = WT_ITEM_LEN(key);
			break;
		case WT_ITEM_KEY_OVFL:
			ovfl = (WT_OVFL *)WT_ITEM_BYTE(key);
			indx->size = ovfl->len;
			break;
		WT_ILLEGAL_FORMAT(db);
		}

		if (idb->huffman_key != NULL)
			F_SET(indx, WT_HUFFMAN);
	}

	/* Fill in the on-page data. */
	indx->page_data = page_data;

	return (0);
}

/*
 * __wt_bt_page_inmem_item_int --
 *	Build in-memory index for row store and off-page duplicate tree
 *	internal pages.
 */
static int
__wt_bt_page_inmem_item_int(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_INDX *indx;
	WT_ITEM *item;
	WT_OFF *offp;
	WT_OVFL *ovfl;
	WT_PAGE_HDR *hdr;
	u_int64_t records;
	u_int32_t i;

	idb = db->idb;
	hdr = page->hdr;
	indx = page->indx;
	records = 0;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 *
	 *	The page contains sorted key/offpage-reference pairs.  Keys
	 *	are on-page (WT_ITEM_KEY) or overflow (WT_ITEM_KEY_OVFL) items.
	 *	Offpage references are WT_ITEM_OFFPAGE items.
	 */
	WT_ITEM_FOREACH(page, item, i)
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
			indx->data = WT_ITEM_BYTE(item);
			indx->size = WT_ITEM_LEN(item);
			if (idb->huffman_key != NULL)
				F_SET(indx, WT_HUFFMAN);
			break;
		case WT_ITEM_KEY_OVFL:
			ovfl = (WT_OVFL *)WT_ITEM_BYTE(item);
			indx->size = ovfl->len;
			if (idb->huffman_key != NULL)
				F_SET(indx, WT_HUFFMAN);
			break;
		case WT_ITEM_OFF_INT:
		case WT_ITEM_OFF_LEAF:
			offp = (WT_OFF *)WT_ITEM_BYTE(item);
			records += WT_RECORDS(offp);
			indx->page_data = item;
			++indx;
			break;
		WT_ILLEGAL_FORMAT(db);
		}

	page->indx_count = hdr->u.entries / 2;
	page->records = records;

	__wt_set_ff_and_sa_from_addr(page, (u_int8_t *)item);
	return (0);
}

/*
 * __wt_bt_page_inmem_fixed_int --
 *	Build in-memory index for column store internal pages.
 */
static int
__wt_bt_page_inmem_fixed_int(DB *db, WT_PAGE *page)
{
	WT_INDX *indx;
	WT_OFF *offp;
	WT_PAGE_HDR *hdr;
	u_int64_t records;
	u_int32_t i;

	hdr = page->hdr;
	indx = page->indx;
	records = 0;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 * The page contains WT_OFF structures.
	 */
	WT_OFF_FOREACH(page, offp, i) {
		indx->page_data = offp;
		++indx;
		records += WT_RECORDS(offp);
	}

	page->indx_count = hdr->u.entries;
	page->records = records;

	__wt_set_ff_and_sa_from_addr(page, (u_int8_t *)offp);
	return (0);
}

/*
 * __wt_bt_page_inmem_row_leaf --
 *	Build in-memory index for row store leaf pages.
 */
static int
__wt_bt_page_inmem_row_leaf(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_INDX *indx;
	WT_ITEM *item;
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
	 * reference (WT_ITEM_OFF_LEAF or WT_ITEM_OFF_INT).
	 */
	indx = NULL;
	indx_count = 0;
	WT_ITEM_FOREACH(page, item, i)
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
			if (indx == NULL)
				indx = page->indx;
			else
				++indx;
			indx->data = WT_ITEM_BYTE(item);
			indx->size = WT_ITEM_LEN(item);
			if (idb->huffman_key != NULL)
				F_SET(indx, WT_HUFFMAN);

			++indx_count;
			break;
		case WT_ITEM_KEY_OVFL:
			if (indx == NULL)
				indx = page->indx;
			else
				++indx;
			indx->size = ((WT_OVFL *)WT_ITEM_BYTE(item))->len;
			if (idb->huffman_key != NULL)
				F_SET(indx, WT_HUFFMAN);

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
			if (indx->page_data == NULL)
				indx->page_data = item;
			++records;
			break;
		case WT_ITEM_OFF_INT:
		case WT_ITEM_OFF_LEAF:
			indx->page_data = item;
			records += WT_INDX_ITEM_OFF_RECORDS(indx);
			break;
		WT_ILLEGAL_FORMAT(db);
		}

	page->indx_count = indx_count;
	page->records = records;

	__wt_set_ff_and_sa_from_addr(page, (u_int8_t *)item);
	return (0);
}

/*
 * __wt_bt_page_inmem_col_leaf --
 *	Build in-memory index for variable-length, data-only leaf pages.
 */
static int
__wt_bt_page_inmem_col_leaf(DB *db, WT_PAGE *page)
{
	WT_INDX *indx;
	WT_ITEM *item;
	WT_OVFL *ovfl;
	WT_PAGE_HDR *hdr;
	u_int32_t i;

	hdr = page->hdr;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 *
	 *	The page contains sorted data items.  The data items are
	 *	on-page (WT_ITEM_DUP) or overflow (WT_ITEM_DUP_OVFL).
	 */
	indx = page->indx;
	WT_ITEM_FOREACH(page, item, i) {
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_DATA:
		case WT_ITEM_DUP:
			indx->data = WT_ITEM_BYTE(item);
			indx->size = WT_ITEM_LEN(item);
			break;
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DUP_OVFL:
			ovfl = (WT_OVFL *)WT_ITEM_BYTE(item);
			indx->size = ovfl->len;
			break;
		WT_ILLEGAL_FORMAT(db);
		}
		indx->page_data = item;
		++indx;
	}

	page->indx_count = hdr->u.entries;
	page->records = hdr->u.entries;

	__wt_set_ff_and_sa_from_addr(page, (u_int8_t *)item);
	return (0);
}

/*
 * __wt_bt_key_to_indx --
 *	Overflow and/or compressed on-page items need processing before
 *	we look at them.   Copy such items into allocated memory in a
 *	WT_INDX.
 */
int
__wt_bt_key_to_indx(WT_TOC *toc, WT_PAGE *page, WT_INDX *ip)
{
	ENV *env;
	IDB *idb;
	WT_PAGE *ovfl_page;
	int is_overflow;
	u_int8_t *p, *dest;
	int ret;

	env = toc->env;
	idb = toc->db->idb;
	ovfl_page = NULL;

	/*
	 * 3 cases:
	 * (1) Uncompressed overflow item
	 * (2) Compressed overflow item
	 * (3) Compressed on-page item
	 *
	 * If the item is an overflow item, bring it into memory.
	 */
	is_overflow = ip->data == NULL ? 1 : 0;
	if (is_overflow) {
		WT_RET(__wt_bt_ovfl_in(
		    toc, WT_INDX_ITEM_OVFL_ADDR(ip), ip->size, &ovfl_page));
		p = WT_PAGE_BYTE(ovfl_page);
	} else
		p = ip->data;

	/*
	 * If the item is compressed, decode it, otherwise just copy it into
	 * place.
	 */
	if (F_ISSET(ip, WT_HUFFMAN))
		WT_ERR(__wt_huffman_decode(idb->huffman_key,
		    p, ip->size, &dest, NULL, &ip->size));
	else {
		WT_ERR(__wt_calloc(env, ip->size, 1, &dest));
		memcpy(dest, p, ip->size);
	}

	F_SET(ip, WT_ALLOCATED);
	F_CLR(ip, WT_HUFFMAN);

	ip->data = dest;
	F_SET(page, WT_ALLOCATED);

err:	if (is_overflow)
		WT_TRET(__wt_bt_page_out(toc, ovfl_page, 0));

	return (ret);
}
