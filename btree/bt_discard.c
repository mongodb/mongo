/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_page_inmem_dup_leaf(DB *, WT_PAGE *);
static int __wt_bt_page_inmem_intl(DB *, WT_PAGE *);
static int __wt_bt_page_inmem_leaf(DB *, WT_PAGE *);

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
	__wt_set_ff_and_sa_from_addr(db, page, WT_PAGE_BYTE(page));

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
	WT_PAGE **hp;

	WT_ASSERT(toc->env, __wt_bt_verify_page(toc, page, NULL) == 0);

	/* Discard the caller's hazard pointer. */
	for (hp = toc->hazard; hp < toc->hazard_next; ++hp)
		if (*hp == page)
			break;
	WT_ASSERT(toc->env, hp < toc->hazard_next);
	--toc->hazard_next;
	*hp = *toc->hazard_next;
	*toc->hazard_next = NULL;

	/*
	 * We don't have to flush memory here for correctness, but doing so
	 * gives the cache drain code immediate access to the buffer.
	 */
	WT_MEMORY_FLUSH;

	/* We don't have to call the cache code unless the page was modified. */
	return (flags == 0 ? 0 : __wt_cache_out(toc, page, flags));
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

	/* Figure out how many indexes we'll need for this page. */
	switch (hdr->type) {
	case WT_PAGE_INT:
	case WT_PAGE_DUP_INT:
		/*
		 * Track the most indexes we find on an internal page in this
		 * database, used if we allocate new internal pages.
		 */
		nindx = hdr->u.entries / 2;
		if (idb->indx_size_hint < nindx)
			idb->indx_size_hint = nindx;
		break;
	case WT_PAGE_LEAF:
		nindx = hdr->u.entries / 2;
		break;
	case WT_PAGE_DUP_LEAF:
		nindx = hdr->u.entries;
		break;
	WT_DEFAULT_FORMAT(db);
	}

	/*
	 * We may be passed a page with a too-small indx array -- in that case,
	 * build one of the appropriate size.   We can simply free the indx
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
	case WT_PAGE_INT:
	case WT_PAGE_DUP_INT:
		ret = __wt_bt_page_inmem_intl(db, page);
		break;
	case WT_PAGE_LEAF:
		ret = __wt_bt_page_inmem_leaf(db, page);
		break;
	case WT_PAGE_DUP_LEAF:
		ret = __wt_bt_page_inmem_dup_leaf(db, page);
		break;
	WT_DEFAULT_FORMAT(db);
	}
	return (ret);
}

/*
 * __wt_bt_page_inmem_append --
 *	Append a new WT_ITEM_KEY/WT_ITEM_OFFP pair to an internal page's
 *	in-memory information.
 */
int
__wt_bt_page_inmem_append(DB *db,
    WT_PAGE *page, WT_ITEM *key_item, WT_ITEM *data_item)
{
	ENV *env;
	IDB *idb;
	WT_INDX *indx;
	WT_ITEM_OVFL *ovfl;
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
	indx = page->indx + page->indx_count;
	++page->indx_count;

	switch (WT_ITEM_TYPE(key_item)) {
	case WT_ITEM_KEY:
		indx->data = WT_ITEM_BYTE(key_item);
		indx->size = WT_ITEM_LEN(key_item);
		break;
	case WT_ITEM_KEY_OVFL:
		ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(key_item);
		indx->size = ovfl->len;
		break;
	WT_DEFAULT_FORMAT(db);
	}

	if (idb->huffman_key != NULL)
		F_SET(indx, WT_HUFFMAN);

	indx->ditem = data_item;

	return (0);
}

/*
 * __wt_bt_page_inmem_intl --
 *	Build in-memory index for primary and off-page duplicate tree internal
 *	pages.
 */
static int
__wt_bt_page_inmem_intl(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_INDX *indx;
	WT_ITEM *item;
	WT_ITEM_OFFP *offp;
	WT_ITEM_OVFL *ovfl;
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
			ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
			indx->size = ovfl->len;
			if (idb->huffman_key != NULL)
				F_SET(indx, WT_HUFFMAN);
			break;
		case WT_ITEM_OFFP_INTL:
		case WT_ITEM_OFFP_LEAF:
			offp = (WT_ITEM_OFFP *)WT_ITEM_BYTE(item);
			records += WT_64_CAST(offp->records);
			indx->ditem = item;
			++indx;
			break;
		WT_DEFAULT_FORMAT(db);
		}

	page->indx_count = hdr->u.entries / 2;
	page->records = records;

	__wt_set_ff_and_sa_from_addr(db, page, (u_int8_t *)item);
	return (0);
}

/*
 * __wt_bt_page_inmem_leaf --
 *	Build in-memory index for primary leaf pages.
 */
static int
__wt_bt_page_inmem_leaf(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_INDX *indx;
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
	u_int32_t i, indx_count;

	idb = db->idb;

	/*
	 * Walk the page, building indices and finding the end of the page.
	 *
	 *	The page contains sorted key/data sets.  Keys are on-page
	 *	(WT_ITEM_KEY) or overflow (WT_ITEM_KEY_OVFL) items.  The data
	 *	sets are either: a single on-page (WT_ITEM_DATA) or overflow
	 *	(WT_ITEM_DATA_OVFL) item; a group of duplicate data items
	 *	where each duplicate is an on-page (WT_ITEM_DUP) or overflow
	 *	(WT_ITEM_DUP_OVFL) item; an offpage reference (WT_ITEM_OFFPAGE).
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

			ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
			indx->size = ovfl->len;
			if (idb->huffman_key != NULL)
				F_SET(indx, WT_HUFFMAN);

			++indx_count;
			break;
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DUP:
		case WT_ITEM_DUP_OVFL:
		case WT_ITEM_OFFP_INTL:
		case WT_ITEM_OFFP_LEAF:
			/*
			 * BUG!!!
			 * Can indx->ditem ever be NULL here?
			 */
			if (indx->ditem == NULL)
				indx->ditem = item;
			break;
		WT_DEFAULT_FORMAT(db);
		}

	page->indx_count = indx_count;
	page->records = indx_count;

	__wt_set_ff_and_sa_from_addr(db, page, (u_int8_t *)item);
	return (0);
}

/*
 * __wt_bt_page_inmem_dup_leaf --
 *	Build in-memory index for off-page duplicate tree leaf pages.
 */
static int
__wt_bt_page_inmem_dup_leaf(DB *db, WT_PAGE *page)
{
	WT_INDX *indx;
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
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
		case WT_ITEM_DUP:
			indx->data = WT_ITEM_BYTE(item);
			indx->size = WT_ITEM_LEN(item);
			break;
		case WT_ITEM_DATA_OVFL:
			ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
			indx->size = ovfl->len;
			break;
		WT_DEFAULT_FORMAT(db);
		}
		indx->ditem = item;
		++indx;
	}

	page->indx_count = hdr->u.entries;
	page->records = hdr->u.entries;

	__wt_set_ff_and_sa_from_addr(db, page, (u_int8_t *)item);
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
		    toc, WT_INDX_OVFL_ADDR(ip), ip->size, &ovfl_page));
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
