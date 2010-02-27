/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static WT_BIN_INDX *
	    __wt_bt_bin_init(WT_BIN_INDX *, u_int32_t);
static int  __wt_bt_page_inmem_col_fix(DB *, WT_PAGE *);
static int  __wt_bt_page_inmem_col_int(WT_PAGE *);
static int  __wt_bt_page_inmem_col_leaf(WT_PAGE *);
static int  __wt_bt_page_inmem_dup_leaf(DB *, WT_PAGE *);
static int  __wt_bt_page_inmem_item_int(DB *, WT_PAGE *);
static int  __wt_bt_page_inmem_row_leaf(DB *, WT_PAGE *);
static void __wt_bt_page_recycle_repl(ENV *, WT_REPL *);

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
	__wt_bt_set_ff_and_sa_from_addr(page, WT_PAGE_BYTE(page));

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
	WT_COL_INDX *cip;
	WT_ROW_INDX *rip;
	u_int32_t i;
	void *bp, *ep;

	WT_ASSERT(env, !F_ISSET(page, WT_MODIFIED));

	switch (page->hdr->type) {
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		bp = (u_int8_t *)page->hdr;
		ep = (u_int8_t *)bp + page->bytes;
		WT_INDX_FOREACH(page, rip, i) {
			/*
			 * For each entry, see if the data was an allocation,
			 * that is, if it points somewhere other than the
			 * original page.  If it's an allocation, free it.
			 *
			 * For each entry, see if replacements were made -- if
			 * so, free them.
			 */
			if (rip->data < bp || rip->data >= ep)
				__wt_free(env, rip->data, rip->size);
			if (rip->repl != NULL)
				__wt_bt_page_recycle_repl(env, rip->repl);
		}
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		WT_INDX_FOREACH(page, cip, i)
			if (cip->repl != NULL)
				__wt_bt_page_recycle_repl(env, cip->repl);
		break;
	case WT_PAGE_OVFL:
	default:
		break;
	}

	if (page->u.indx != NULL)
		__wt_free(env, page->u.indx, 0);

	__wt_free(env, page->hdr, page->bytes);

#ifdef DIAGNOSTIC
	if (F_ISSET(page, ~WT_APIMASK_WT_PAGE))
		(void)__wt_api_args(env, "Page.recycle");
#endif
	__wt_free(env, page, sizeof(WT_PAGE));
}

/*
 * __wt_bt_page_recycle_repl --
 *	Recycle the replacement array.
 */
static void
__wt_bt_page_recycle_repl(ENV *env, WT_REPL *repl)
{
	WT_REPL *next;
	u_int16_t i;

	/*
	 * We only free the data pointers in the top-level WT_REPL structure,
	 * all data pointers in previous WT_REPL structures were copied into
	 * the top-level structure as it was upgraded.
	 */
	for (i = 0; i < repl->repl_next; ++i)
		if (repl->data[i].data != WT_DATA_DELETED)
			__wt_free(env, repl->data[i].data, repl->data[i].size);

	/* Walk the WT_REPL list, freeing the replacement structures. */
	do {
		next = repl->next;
		__wt_free(env, repl, sizeof(WT_REPL));
	} while ((repl = next) != NULL);
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
	WT_OFF *offp;
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
	 *	The page contains sorted key/offpage-reference pairs.  Keys
	 *	are on-page (WT_ITEM_KEY) or overflow (WT_ITEM_KEY_OVFL) items.
	 *	Offpage references are WT_ITEM_OFFPAGE items.
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
		case WT_ITEM_OFF_INT:
		case WT_ITEM_OFF_LEAF:
			offp = WT_ITEM_BYTE_OFF(item);
			records += WT_RECORDS(offp);
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
	 * reference (WT_ITEM_OFF_LEAF or WT_ITEM_OFF_INT).
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
		case WT_ITEM_OFF_INT:
		case WT_ITEM_OFF_LEAF:
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
	WT_OFF *offp;
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
	WT_OFF_FOREACH(page, offp, i) {
		ip->page_data = offp;
		++ip;
		records += WT_RECORDS(offp);
	}

	page->indx_count = hdr->u.entries;
	page->records = records;

	__wt_bt_set_ff_and_sa_from_addr(page, (u_int8_t *)offp);
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
			ip->size = WT_ITEM_BYTE_OVFL(item)->len;
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
 * __wt_bt_bin_create --
 *	Create the in-memory binary tree for a page.
 */
int
__wt_bt_bin_create(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	ENV *env;
	WT_BIN_INDX *bp;
	WT_COL_INDX *cip;
	WT_ROW_INDX *rip;
	u_int32_t i;

	db = toc->db;
	env = toc->env;

	/* Allocate the initial binary tree in one chunk of memory. */
	WT_RET(__wt_calloc(
	    env, page->indx_count, sizeof(WT_BIN_INDX), &page->bin));

	/*
	 * First, fill in the references to the underlying index array.   We
	 * could do this at the same time we build the binary tree, but it's
	 * a lot simpler to do in two passes than to complicate the recursive
	 * function that builds the tree.
	 */
	bp = page->bin;
	switch (page->hdr->type) {
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_INDX_FOREACH(page, rip, i) {
			bp->indx = rip;
			++bp;
		}
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		WT_INDX_FOREACH(page, cip, i) {
			bp->indx = cip;
			++bp;
		}
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/* Second, build the balanced binary tree. */
	page->bin = __wt_bt_bin_init(page->bin, page->indx_count);

	return (0);
}

/*
 * __wt_bt_bin_init --
 *	Routine to fill in the initial binary tree.
 */
static WT_BIN_INDX *
__wt_bt_bin_init(WT_BIN_INDX *base, u_int32_t n)
{
	WT_BIN_INDX *p;
	u_int32_t c, l, r;

	if (n == 1)
		return (base);

	c = (n + 1) / 2;			/* Center element */
	p = &base[c - 1];			/* Middle pointer */

	r = n - c;				/* Right count */
	if (r != 0)
		p->right = __wt_bt_bin_init(base + c, r);

	l = (n - 1) - r;			/* Left count */
	if (l != 0)
		p->left = __wt_bt_bin_init(base, l);

	return (p);
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
		WT_RET(__wt_bt_ovfl_in(toc, ovfl->addr, ovfl->len, &ovfl_page));
		orig = WT_PAGE_BYTE(ovfl_page);
		size = ovfl->len;
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
		WT_ERR(__wt_realloc(env, &dbt->data_len, size, &dbt->data));
		dbt->size = size;
		memcpy(dbt->data, orig, size);
	} else
		WT_ERR(__wt_huffman_decode(idb->huffman_key,
		    orig, size, &dbt->data, &dbt->data_len, &dbt->size));

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
		WT_TRET(__wt_bt_page_out(toc, ovfl_page, 0));

	return (ret);
}
