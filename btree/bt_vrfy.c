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
 * There's a bunch of stuff we pass around during verification, group it
 * together to make the code prettier.
 */
typedef struct {
	u_int32_t frags;			/* Total frags */
	bitstr_t *fragbits;			/* Frag tracking bit list */

	FILE	*stream;			/* Dump file stream */

	void (*f)(const char *, u_int64_t);	/* Progress callback */
	u_int64_t fcnt;				/* Progress counter */

	WT_PAGE *leaf;				/* Child page */
} WT_VSTUFF;

static int __wt_bt_verify_addfrag(DB *, WT_PAGE *, WT_VSTUFF *);
static int __wt_bt_verify_checkfrag(DB *, WT_VSTUFF *);
static int __wt_bt_verify_cmp(WT_TOC *, WT_ROW *, WT_PAGE *, int);
static int __wt_bt_verify_ovfl(WT_TOC *, WT_OVFL *, WT_VSTUFF *);
static int __wt_bt_verify_page_col_fix(DB *, WT_PAGE *);
static int __wt_bt_verify_page_col_int(DB *, WT_PAGE *);
static int __wt_bt_verify_page_desc(DB *, WT_PAGE *);
static int __wt_bt_verify_page_item(WT_TOC *, WT_PAGE *, WT_VSTUFF *);
static int __wt_bt_verify_tree(
		WT_TOC *, WT_ROW *, u_int32_t, WT_OFF *, WT_VSTUFF *);

/*
 * __wt_db_verify --
 *	Verify a Btree.
 */
int
__wt_db_verify(WT_TOC *toc, void (*f)(const char *, u_int64_t))
{
	return (__wt_bt_verify(toc, f, NULL));
}

/*
 * __wt_bt_verify --
 *	Verify a Btree, optionally dumping each page in debugging mode.
 */
int
__wt_bt_verify(
    WT_TOC *toc, void (*f)(const char *, u_int64_t), FILE *stream)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_OFF off;
	WT_PAGE *page;
	WT_VSTUFF vstuff;
	int ret;

	env = toc->env;
	db = toc->db;
	idb = db->idb;
	page = NULL;
	ret = 0;

	memset(&vstuff, 0, sizeof(vstuff));
	vstuff.stream = stream;
	vstuff.f = f;

	/*
	 * Allocate a bit array, where each bit represents a single allocation
	 * size piece of the file.   This is how we track the parts of the file
	 * we've verified.  Storing this on the heap seems reasonable: with a
	 * minimum allocation size of 512B, we would allocate 4MB to verify a
	 * 16GB file.  To verify larger files than we can handle this way, we'd
	 * have to write parts of the bit array into a disk file.
	 *
	 * !!!
	 * There's one portability issue -- the bitstring package uses "ints",
	 * not unsigned ints, or any fixed size.   If an "int" can't hold a
	 * big enough value, we could lose.   There's a check here to make we
	 * don't overflow.   I don't ever expect to see this error message, but
	 * better safe than sorry.
	 */
	vstuff.frags = WT_OFF_TO_ADDR(db, idb->fh->file_size);
	if (vstuff.frags > INT_MAX) {
		__wt_api_db_errx(db, "file is too large to verify");
		goto err;
	}
	WT_ERR(bit_alloc(env, vstuff.frags, &vstuff.fragbits));

	/*
	 * Verify the descriptor page; the descriptor page can't move, so simply
	 * retry any WT_RESTART returns.
	 *
	 * We have to keep our hazard reference on the descriptor page while we
	 * walk the tree.  The problem we're solving is if the root page
	 * were to be re-written between the time we read the descriptor page
	 * and when we read the root page, we'd read an out-of-date root page.
	 * (Other methods don't have to worry about this because they only work
	 * when the database is opened and the root page is pinned into memory.
	 * Db.verify works on both opened and unopened databases, so it has to
	 * ensure the root page doesn't move.   This is a wildly unlikely race,
	 * of course, but it's easy to handle.)
	 */
	WT_ERR_RESTART(__wt_bt_page_in(toc, 0, 512, 0, &page));
	WT_ERR(__wt_bt_verify_page(toc, page, &vstuff));

	/* Verify the tree, starting at the root from the descriptor page. */
	WT_RECORDS(&off) = 0;
	off.addr = idb->root_addr;
	off.size = idb->root_size;
	WT_ERR(__wt_bt_verify_tree(toc, NULL, WT_LDESC, &off, &vstuff));

	WT_ERR(__wt_bt_verify_checkfrag(db, &vstuff));

err:	if (page != NULL)
		__wt_bt_page_out(toc, &page, 0);
	if (vstuff.leaf != NULL)
		__wt_bt_page_out(toc, &vstuff.leaf, 0);

	/* Wrap up reporting. */
	if (vstuff.f != NULL)
		vstuff.f(toc->name, vstuff.fcnt);
	if (vstuff.fragbits != NULL)
		__wt_free(env, vstuff.fragbits, 0);

	return (ret);
}

/*
 * Callers pass us a WT_OFF structure, and a reference to the internal node
 * key that referenced that page (if any -- the root node doesn't have one).
 *
 * The plan is simple.  We recursively descend the tree, in depth-first fashion.
 * First we verify each page, so we know it is correctly formed, and any keys
 * it contains are correctly ordered.  After page verification, we check the
 * connections within the tree.
 *
 * There are two connection checks: First, we compare the internal node key that
 * lead to the current page against the first entry on the current page.  The
 * internal node key must compare less than or equal to the first entry on the
 * current page.  Second, we compare the largest key we've seen on any leaf page
 * against the next internal node key we find.  This check is a little tricky:
 * Every time we find a leaf page, we save it in the vs->leaf structure.  The
 * next time we are about to indirect through an entry on an internal node, we
 * compare the last entry on that saved page against the internal node entry's
 * key.  In that comparison, the leaf page's key must be less than the internal
 * node entry's key.
 *
 * Off-page duplicate trees are handled the same way (this function is called
 * from the page verification routine when an off-page duplicate tree is found).
 *
 * __wt_bt_verify_tree --
 *	Verify a subtree of the tree, recursively descending through the tree.
 */
static int
__wt_bt_verify_tree(WT_TOC *toc,
    WT_ROW *parent_rip, u_int32_t level, WT_OFF *off, WT_VSTUFF *vs)
{
	DB *db;
	WT_COL *cip;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	WT_ROW *rip;
	int ret;

	db = toc->db;
	page = NULL;
	ret = 0;

	/*
	 * Read and verify the page.
	 *
	 * If the page were to be rewritten/discarded from the cache while
	 * we're getting it, we can re-try -- re-trying is safe because our
	 * addr/size information is from a page which can't be discarded
	 * because of our hazard reference.  If the page was re-written, our
	 * on-page overflow information will have been updated to the overflow
	 * page's new address.
	 */
	WT_RET_RESTART(__wt_bt_page_in(toc, off->addr, off->size, 0, &page));
	WT_ERR(__wt_bt_verify_page(toc, page, vs));

	/*
	 * The page is OK, instantiate its in-memory information if we don't
	 * already have it.
	 */
	if (page->u.indx == NULL)
		WT_ERR(__wt_bt_page_inmem(db, page));

	hdr = page->hdr;

	/*
	 * Check the tree levels and records counts match up.
	 *
	 * If passed a level of WT_LDESC, that is, the only level that can't
	 * possibly be a valid database page level, this is the root page of
	 * the tree, so we use this page's level to initialize expected values
	 * for the rest of the tree, and there's no record count to check.
	 */
	if (level == WT_LDESC)
		level = hdr->level;
	else {
		if (hdr->level != level) {
			__wt_api_db_errx(db,
			    "page at addr %lu has a tree level of %lu where "
			    "the expected level was %lu",
			    (u_long)off->addr,
			    (u_long)hdr->level, (u_long)level);
			goto err;
		}
		if (page->records != WT_RECORDS(off)) {
			__wt_api_db_errx(db,
			    "page at addr %lu has a record count of %llu where "
			    "the expected record count was %llu",
			    (u_long)off->addr, page->records, WT_RECORDS(off));
			goto err;
		}
	}

	/*
	 * In row stores we're passed the parent page's key that references this
	 * page: it must sort less than or equal to the first key on this page.
	 */
	if (parent_rip != NULL)
		WT_ERR(__wt_bt_verify_cmp(toc, parent_rip, page, 1));

	/*
	 * Leaf pages need no further processing; in the case of row-store leaf
	 * pages, we'll need them to check their last entry against the next
	 * internal key in the tree; save a reference and return.
	 */
	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		__wt_bt_page_out(toc, &page, 0);
		return (0);
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		vs->leaf = page;
		return (0);
	default:
		break;
	}

	/* For each entry in the internal page, verify the subtree. */
	switch (hdr->type) {
	u_int32_t i;
	case WT_PAGE_COL_INT:
		WT_INDX_FOREACH(page, cip, i)
			WT_ERR(__wt_bt_verify_tree(
			    toc, NULL, level - 1, cip->data, vs));
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_INDX_FOREACH(page, rip, i) {
			/*
			 * At each off-page entry, we compare the current entry
			 * against the largest key in the subtree rooted to the
			 * immediate left of the current item; this key must
			 * compare less than or equal to the current item.  The
			 * trick here is we need the last leaf key, not the last
			 * internal node key.  Discard the leaf node as soon as
			 * we've used it in a comparison.
			 */
			if (vs->leaf != NULL) {
				WT_ERR(
				    __wt_bt_verify_cmp(toc, rip, vs->leaf, 0));
				__wt_bt_page_out(toc, &vs->leaf, 0);
			}

			WT_ERR(__wt_bt_verify_tree(toc, rip,
			    level - 1, WT_ITEM_BYTE_OFF(rip->data), vs));
		}
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

	if (0) {
err:		if (vs->leaf != NULL)
			__wt_bt_page_out(toc, &vs->leaf, 0);
	}
	if (page != NULL)
		__wt_bt_page_out(toc, &page, 0);

	return (ret);
}

/*
 * __wt_bt_verify_cmp --
 *	Compare a key on a parent page to a designated entry on a child page.
 */
static int
__wt_bt_verify_cmp(
    WT_TOC *toc, WT_ROW *parent_rip, WT_PAGE *child, int first_entry)
{
	DB *db;
	DBT *cd_ref, *pd_ref;
	WT_ROW *child_rip;
	int cmp, ret, (*func)(DB *, const DBT *, const DBT *);

	db = toc->db;

	/* Set the comparison function. */
	switch (child->hdr->type) {
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
		func = db->btree_compare_dup;
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		func = db->btree_compare;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * The two keys we're going to compare may be overflow keys -- don't
	 * bother instantiating the keys in the tree, there's no reason to
	 * believe we're going to be working in this database.
	 */
	child_rip = first_entry ?
	    child->u.irow : child->u.irow + (child->indx_count - 1);
	if (WT_KEY_PROCESS(child_rip)) {
		cd_ref = &toc->tmp1;
		WT_RET(__wt_bt_key_process(toc, NULL, child_rip, cd_ref));
	} else
		cd_ref = (DBT *)child_rip;
	if (WT_KEY_PROCESS(parent_rip)) {
		pd_ref = &toc->tmp2;
		WT_RET(__wt_bt_key_process(toc, NULL, parent_rip, pd_ref));
	} else
		pd_ref = (DBT *)parent_rip;

	/* Compare the parent's key against the child's key. */
	cmp = func(db, cd_ref, pd_ref);

	ret = 0;
	if (first_entry && cmp < 0) {
		__wt_api_db_errx(db,
		    "the first key on page at addr %lu sorts before its "
		    "reference key on its parent's page",
		    (u_long)child->addr);
		ret = WT_ERROR;
	}
	if (!first_entry && cmp >= 0) {
		__wt_api_db_errx(db,
		    "the last key on the page at addr %lu sorts after a parent "
		    "page's key for the subsequent page",
		    (u_long)child->addr);
		ret = WT_ERROR;
	}

	return (ret);
}

/*
 * __wt_bt_verify_page --
 *	Verify a single Btree page.
 */
int
__wt_bt_verify_page(WT_TOC *toc, WT_PAGE *page, void *vs_arg)
{
	DB *db;
	WT_PAGE_HDR *hdr;
	WT_VSTUFF *vs;
	u_int32_t addr;

	vs = vs_arg;
	db = toc->db;

	hdr = page->hdr;
	addr = page->addr;

	/* Report progress every 10 pages. */
	if (vs != NULL && vs->f != NULL && ++vs->fcnt % 10 == 0)
		vs->f(toc->name, vs->fcnt);

	/* Update frags list. */
	if (vs != NULL && vs->fragbits != NULL)
		WT_RET(__wt_bt_verify_addfrag(db, page, vs));

	/*
	 * FUTURE:
	 * Check the LSN against the existing log files.
	 */
	if (hdr->lsn[0] != 0 || hdr->lsn[1] != 0) {
		__wt_api_db_errx(db,
		    "page at addr %lu has non-zero lsn header fields",
		    (u_long)addr);
		return (WT_ERROR);
	}

	/*
	 * Don't verify the checksum -- it verified when we first read the
	 * page.
	 */

	/* Check the page type. */
	switch (hdr->type) {
	case WT_PAGE_DESCRIPT:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		break;
	case WT_PAGE_OVFL:
		if (hdr->u.entries == 0) {
			__wt_api_db_errx(db,
			    "overflow page at addr %lu has no entries",
			    (u_long)addr);
			return (WT_ERROR);
		}
		break;
	case WT_PAGE_INVALID:
	default:
		__wt_api_db_errx(db,
		    "page at addr %lu has an invalid type of %lu",
		    (u_long)addr, (u_long)hdr->type);
		return (WT_ERROR);
	}

	/* Check the page level. */
	switch (hdr->type) {
	case WT_PAGE_DESCRIPT:
		if (hdr->level != WT_LDESC)
			goto err_level;
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_OVFL:
	case WT_PAGE_ROW_LEAF:
		if (hdr->level != WT_LLEAF)
			goto err_level;
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		if (hdr->level <= WT_LLEAF) {
err_level:		__wt_api_db_errx(db,
			    "%s page at addr %lu has incorrect tree level "
			    "of %lu",
			    __wt_bt_hdr_type(hdr),
			    (u_long)addr, (u_long)hdr->level);
			return (WT_ERROR);
		}
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	if (hdr->unused[0] != '\0' || hdr->unused[1] != '\0') {
		__wt_api_db_errx(db,
		    "page at addr %lu has non-zero unused header fields",
		    (u_long)addr);
		return (WT_ERROR);
	}

	/* Verify the items on the page. */
	switch (hdr->type) {
	case WT_PAGE_DESCRIPT:
		WT_RET(__wt_bt_verify_page_desc(db, page));
		break;
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_bt_verify_page_item(toc, page, vs));
		break;
	case WT_PAGE_COL_INT:
		WT_RET(__wt_bt_verify_page_col_int(db, page));
		break;
	case WT_PAGE_COL_FIX:
		WT_RET(__wt_bt_verify_page_col_fix(db, page));
		break;
	case WT_PAGE_OVFL:
		break;
	WT_ILLEGAL_FORMAT(db);
	}

#ifdef HAVE_DIAGNOSTIC
	/* Optionally dump the page in debugging mode. */
	if (vs != NULL && vs->stream != NULL)
		return (__wt_bt_debug_page(toc, page, NULL, vs->stream));
#endif
	return (0);
}

/*
 * __wt_bt_verify_page_item --
 *	Walk a page of WT_ITEMs, and verify them.
 */
static int
__wt_bt_verify_page_item(WT_TOC *toc, WT_PAGE *page, WT_VSTUFF *vs)
{
	struct {
		u_int32_t indx;			/* Item number */

		DBT *item;			/* Item to compare */
		DBT item_std;			/* On-page reference */
		DBT item_ovfl;			/* Overflow holder */
		DBT item_comp;			/* Uncompressed holder */
	} *current, *last_data, *last_key, *swap_tmp, _a, _b, _c;
	DB *db;
	ENV *env;
	IDB *idb;
	WT_ITEM *item;
	WT_OVFL *ovflp;
	WT_OFF *off;
	WT_PAGE_HDR *hdr;
	u_int8_t *end;
	u_int32_t addr, i, item_num, item_len, item_type;
	int (*func)(DB *, const DBT *, const DBT *), ret;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ret = 0;

	hdr = page->hdr;
	end = (u_int8_t *)hdr + page->size;
	addr = page->addr;

	/*
	 * We have a maximum of 3 key/data items we track -- the last key, the
	 * last data item, and the current item.   They're stored in the _a,
	 * _b, and _c structures (it doesn't matter which) -- what matters is
	 * which item is referenced by current, last_data or last_key.
	 */
	WT_CLEAR(_a);
	current = &_a;
	WT_CLEAR(_b);
	last_data = &_b;
	WT_CLEAR(_c);
	last_key = &_c;

	/* Set the comparison function. */
	switch (hdr->type) {
	case WT_PAGE_COL_VAR:
		func = NULL;
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
		func = db->btree_compare_dup;
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		func = db->btree_compare;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	item_num = 0;
	WT_ITEM_FOREACH(page, item, i) {
		++item_num;

		/* Check if this item is entirely on the page. */
		if ((u_int8_t *)item + sizeof(WT_ITEM) > end)
			goto eop;

		item_type = WT_ITEM_TYPE(item);
		item_len = WT_ITEM_LEN(item);

		/* Check the item's type. */
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (hdr->type != WT_PAGE_DUP_INT &&
			    hdr->type != WT_PAGE_ROW_INT &&
			    hdr->type != WT_PAGE_ROW_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_DEL:
			if (hdr->type != WT_PAGE_COL_VAR)
				goto item_vs_page;
			break;
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
			if (hdr->type != WT_PAGE_COL_VAR &&
			    hdr->type != WT_PAGE_ROW_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_DUP:
		case WT_ITEM_DUP_OVFL:
			if (hdr->type != WT_PAGE_DUP_LEAF &&
			    hdr->type != WT_PAGE_ROW_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_OFF:
			if (hdr->type != WT_PAGE_DUP_INT &&
			    hdr->type != WT_PAGE_ROW_INT &&
			    hdr->type != WT_PAGE_ROW_LEAF) {
item_vs_page:			__wt_api_db_errx(db,
				    "illegal item and page type combination "
				    "(item %lu on page at addr %lu is a %s "
				    "item on a %s page)",
				    (u_long)item_num, (u_long)addr,
				    __wt_bt_item_type(item),
				    __wt_bt_hdr_type(hdr));
				goto err_set;
			}
			break;
		default:
			__wt_api_db_errx(db,
			    "item %lu on page at addr %lu has an illegal type "
			    "of %lu",
			    (u_long)item_num, (u_long)addr, (u_long)item_type);
			goto err_set;
		}

		/* Check the item's length. */
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_DATA:
		case WT_ITEM_DUP:
			/* The length is variable, we can't check it. */
			break;
		case WT_ITEM_DEL:
			if (item_len != 0)
				goto item_len;
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DUP_OVFL:
			if (item_len != sizeof(WT_OVFL))
				goto item_len;
			break;
		case WT_ITEM_OFF:
			if (item_len != sizeof(WT_OFF)) {
item_len:			__wt_api_db_errx(db,
				    "item %lu on page at addr %lu has an "
				    "incorrect length",
				    (u_long)item_num, (u_long)addr);
				goto err_set;
			}
			break;
		default:
			break;
		}

		/* Check if the item is entirely on the page. */
		if ((u_int8_t *)WT_ITEM_NEXT(item) > end) {
eop:			__wt_api_db_errx(db,
			    "item %lu on page at addr %lu extends past the end "
			    " of the page",
			    (u_long)item_num, (u_long)addr);
			goto err_set;
		}
		switch (item_type) {
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DUP_OVFL:
			ovflp = WT_ITEM_BYTE_OVFL(item);
			if (WT_ADDR_TO_OFF(db, ovflp->addr) +
			    WT_HDR_BYTES_TO_ALLOC(db, ovflp->size) >
			    idb->fh->file_size)
				goto eof;
			break;
		case WT_ITEM_OFF:
			off = WT_ITEM_BYTE_OFF(item);
			if (WT_ADDR_TO_OFF(db, off->addr) +
			    off->size > idb->fh->file_size) {
eof:				__wt_api_db_errx(db,
				    "off-page reference in item %lu on "
				    "page at addr %lu extends past the "
				    "end of the file",
				    (u_long)item_num, (u_long)addr);
				goto err_set;
			}
			break;
		default:
			break;
		}

		/* Verify overflow references. */
		switch (item_type) {
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DUP_OVFL:
			ovflp = WT_ITEM_BYTE_OVFL(item);
			WT_ERR(__wt_bt_verify_ovfl(toc, ovflp, vs));
			break;
		default:
			break;
		}

		/*
		 * If we're verifying the entire tree, verify any off-page
		 * duplicate trees (that's any off-page references found on
		 * a row-store leaf page).
		 */
		if (vs != NULL && hdr->type == WT_PAGE_ROW_LEAF)
			switch (item_type) {
			case WT_ITEM_OFF:
				off = WT_ITEM_BYTE_OFF(item);
				WT_ERR(__wt_bt_verify_tree(
				    toc, NULL, WT_LDESC, off, vs));
				break;
			default:
				break;
			}

		/*
		 * Check the page item sort order.  If the page doesn't contain
		 * sorted items (or, if the item is an off-page item and we're
		 * not verifying the entire tree), continue walking the page
		 * items.   Otherwise, get a DBT that represents the item and
		 * compare it with the last item.
		 */
		switch (item_type) {
		case WT_ITEM_DEL:
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_OFF:
			/*
			 * These items aren't sorted on the page-- we're done.
			 */
			continue;
		case WT_ITEM_KEY:
		case WT_ITEM_DUP:
			current->indx = item_num;
			current->item = &current->item_std;
			current->item->data = WT_ITEM_BYTE(item);
			current->item->size = item_len;
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DUP_OVFL:
			current->indx = item_num;
			current->item = &current->item_ovfl;
			WT_ERR(__wt_bt_ovfl_to_dbt(toc, (WT_OVFL *)
			    WT_ITEM_BYTE(item), current->item));
			break;
		default:
			break;
		}

		/* If the key is compressed, get an uncompressed version. */
		if (idb->huffman_key != NULL) {
			WT_ERR(__wt_huffman_decode(idb->huffman_key,
			    current->item->data, current->item->size,
			    &current->item_comp.data,
			    &current->item_comp.mem_size,
			    &current->item_comp.size));
			current->item = &current->item_comp;
		}

		/* Check the sort order. */
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (last_key->item != NULL &&
			    func(db, last_key->item, current->item) >= 0) {
				__wt_api_db_errx(db,
				    "item %lu and item %lu on page at addr %lu "
				    "are incorrectly sorted",
				    last_key->indx, current->indx,
				    (u_long)addr);
				goto err_set;
			}
			swap_tmp = last_key;
			last_key = current;
			current = swap_tmp;
			break;
		case WT_ITEM_DUP:
		case WT_ITEM_DUP_OVFL:
			if (last_data->item != NULL &&
			    func(db, last_data->item, current->item) >= 0) {
				__wt_api_db_errx(db,
				    "item %lu and item %lu on page at addr %lu "
				    "are incorrectly sorted",
				    last_data->indx, current->indx,
				    (u_long)addr);
				goto err_set;
			}
			swap_tmp = last_data;
			last_data = current;
			current = swap_tmp;
			break;
		default:	/* No other values are possible. */
			break;
		}
	}

	if (0) {
err_set:	ret = WT_ERROR;
	}

err:	__wt_free(env, _a.item_ovfl.data, _a.item_ovfl.mem_size);
	__wt_free(env, _b.item_ovfl.data, _b.item_ovfl.mem_size);
	__wt_free(env, _c.item_ovfl.data, _c.item_ovfl.mem_size);
	__wt_free(env, _a.item_comp.data, _a.item_comp.mem_size);
	__wt_free(env, _b.item_comp.data, _b.item_comp.mem_size);
	__wt_free(env, _c.item_comp.data, _c.item_comp.mem_size);

	return (ret);
}

/*
 * __wt_bt_verify_page_col_int --
 *	Walk a WT_PAGE_COL_INT page and verify it.
 */
static int
__wt_bt_verify_page_col_int(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_OFF *off;
	WT_PAGE_HDR *hdr;
	u_int8_t *end;
	u_int32_t addr, i, entry_num;

	idb = db->idb;
	hdr = page->hdr;
	end = (u_int8_t *)hdr + page->size;
	addr = page->addr;

	entry_num = 0;
	WT_OFF_FOREACH(page, off, i) {
		++entry_num;

		/* Check if this entry is entirely on the page. */
		if ((u_int8_t *)off + sizeof(WT_OFF) > end) {
			__wt_api_db_errx(db,
			    "offpage reference %lu on page at addr %lu extends "
			    "past the end of the page",
			    (u_long)entry_num, (u_long)addr);
			return (WT_ERROR);
		}

		/* Check if the reference is past the end-of-file. */
		if (WT_ADDR_TO_OFF(
		    db, off->addr) + off->size > idb->fh->file_size) {
			__wt_api_db_errx(db,
			    "off-page reference in object %lu on page at "
			    "addr %lu extends past the end of the file",
			    (u_long)entry_num, (u_long)addr);
			return (WT_ERROR);
		}
	}

	return (0);
}

/*
 * __wt_bt_verify_page_col_fix --
 *	Walk a WT_PAGE_COL_FIX page and verify it.
 */
static int
__wt_bt_verify_page_col_fix(DB *db, WT_PAGE *page)
{
	IDB *idb;
	u_int len;
	u_int32_t addr, i, j, entry_num;
	u_int8_t *data, *end, *last_data, *p;

	idb = db->idb;
	end = (u_int8_t *)page->hdr + page->size;
	addr = page->addr;

	if (F_ISSET(idb, WT_REPEAT_COMP)) {
		last_data = NULL;
		len = db->fixed_len + sizeof(u_int16_t);

		entry_num = 0;
		WT_FIX_REPEAT_FOREACH(db, page, data, i) {
			++entry_num;

			/* Check if this entry is entirely on the page. */
			if (data + len > end)
				goto eop;

			/* Count must be non-zero. */
			if (WT_FIX_REPEAT_COUNT(data) == 0) {
				__wt_api_db_errx(db,
				    "fixed-length entry %lu on page at addr "
				    "%lu has a repeat count of 0",
				    (u_long)entry_num, (u_long)addr);
				return (WT_ERROR);
			}

			/* Deleted items are entirely nul bytes. */
			p = WT_FIX_REPEAT_DATA(data);
			if (WT_FIX_DELETE_ISSET(p)) {
				if (*p != WT_FIX_DELETE_BYTE)
					goto delfmt;
				for (j = 1; j < db->fixed_len; ++j)
					if (*++p != '\0')
						goto delfmt;
			}

			/*
			 * If the previous data is the same as this data, we
			 * missed an opportunity for compression -- complain.
			 */
			if (last_data != NULL &&
			    memcmp(WT_FIX_REPEAT_DATA(last_data),
			    WT_FIX_REPEAT_DATA(data), db->fixed_len) == 0 &&
			    WT_FIX_REPEAT_COUNT(last_data) < UINT16_MAX) {
				__wt_api_db_errx(db,
				    "fixed-length entries %lu and %lu on page "
				    "at addr %lu are identical and should have "
				    "been compressed",
				    (u_long)entry_num,
				    (u_long)entry_num - 1, (u_long)addr);
				return (WT_ERROR);
			}
			last_data = data;
		}
	} else {
		len = db->fixed_len;

		entry_num = 0;
		WT_FIX_FOREACH(db, page, data, i) {
			++entry_num;

			/* Check if this entry is entirely on the page. */
			if (data + len > end)
				goto eop;

			/* Deleted items are entirely nul bytes. */
			p = data;
			if (WT_FIX_DELETE_ISSET(data)) {
				if (*p != WT_FIX_DELETE_BYTE)
					goto delfmt;
				for (j = 1; j < db->fixed_len; ++j)
					if (*++p != '\0')
						goto delfmt;
			}
		}
	}

	return (0);

eop:	__wt_api_db_errx(db,
	    "fixed-length entry %lu on page at addr %lu extends past the end "
	    "of the page",
	    (u_long)entry_num, (u_long)addr);
	return (WT_ERROR);

delfmt:	__wt_api_db_errx(db,
	    "deleted fixed-length entry %lu on page at addr %lu has non-nul "
	    "bytes",
	    (u_long)entry_num, (u_long)addr);
	return (WT_ERROR);
}

/*
 * __wt_bt_verify_page_desc --
 *	Verify the database description on page 0.
 */
static int
__wt_bt_verify_page_desc(DB *db, WT_PAGE *page)
{
	WT_PAGE_DESC *desc;
	u_int i;
	u_int8_t *p;
	int ret;

	ret = 0;

	desc = (WT_PAGE_DESC *)WT_PAGE_BYTE(page);
	if (desc->magic != WT_BTREE_MAGIC) {
		__wt_api_db_errx(db, "magic number %#lx, expected %#lx",
		    (u_long)desc->magic, WT_BTREE_MAGIC);
		ret = WT_ERROR;
	}
	if (desc->majorv != WT_BTREE_MAJOR_VERSION) {
		__wt_api_db_errx(db, "major version %d, expected %d",
		    (int)desc->majorv, WT_BTREE_MAJOR_VERSION);
		ret = WT_ERROR;
	}
	if (desc->minorv != WT_BTREE_MINOR_VERSION) {
		__wt_api_db_errx(db, "minor version %d, expected %d",
		    (int)desc->minorv, WT_BTREE_MINOR_VERSION);
		ret = WT_ERROR;
	}
	if (desc->intlmin != db->intlmin) {
		__wt_api_db_errx(db,
		    "minimum internal page size %lu, expected %lu",
		    (u_long)db->intlmin, (u_long)desc->intlmin);
		ret = WT_ERROR;
	}
	if (desc->intlmax != db->intlmax) {
		__wt_api_db_errx(db,
		    "maximum internal page size %lu, expected %lu",
		    (u_long)db->intlmax, (u_long)desc->intlmax);
		ret = WT_ERROR;
	}
	if (desc->leafmin != db->leafmin) {
		__wt_api_db_errx(db, "minimum leaf page size %lu, expected %lu",
		    (u_long)db->leafmin, (u_long)desc->leafmin);
		ret = WT_ERROR;
	}
	if (desc->leafmax != db->leafmax) {
		__wt_api_db_errx(db, "maximum leaf page size %lu, expected %lu",
		    (u_long)db->leafmax, (u_long)desc->leafmax);
		ret = WT_ERROR;
	}
	if (desc->base_recno != 0) {
		__wt_api_db_errx(db, "base recno %llu, expected 0",
		    (u_quad)desc->base_recno);
		ret = WT_ERROR;
	}
	if (F_ISSET(desc, ~WT_PAGE_DESC_MASK)) {
		__wt_api_db_errx(db,
		    "unexpected flags found in description record");
		ret = WT_ERROR;
	}
	if (desc->fixed_len == 0 && F_ISSET(desc, WT_PAGE_DESC_REPEAT)) {
		__wt_api_db_errx(db,
		    "repeat counts configured but no fixed length record "
		    "size specified");
		ret = WT_ERROR;
	}

	for (p = (u_int8_t *)desc->unused1,
	    i = sizeof(desc->unused1); i > 0; --i)
		if (*p != '\0')
			goto unused_not_clear;
	for (p = (u_int8_t *)desc->unused2,
	    i = sizeof(desc->unused2); i > 0; --i)
		if (*p != '\0') {
unused_not_clear:	__wt_api_db_errx(db,
			    "unexpected values found in description record's "
			    "unused fields");
			ret = WT_ERROR;
		}

	return (ret);
}

/*
 * __wt_bt_verify_ovfl --
 *	Verify an overflow item.
 */
static int
__wt_bt_verify_ovfl(WT_TOC *toc, WT_OVFL *ovfl, WT_VSTUFF *vs)
{
	WT_PAGE *page;
	int ret;

	WT_RET(__wt_bt_ovfl_in(toc, ovfl, &page));

	ret = __wt_bt_verify_page(toc, page, vs);

	__wt_bt_page_out(toc, &page, 0);

	return (ret);
}

/*
 * __wt_bt_verify_addfrag --
 *	Add a new set of fragments to the list, and complain if we've already
 *	verified this chunk of the file.
 */
static int
__wt_bt_verify_addfrag(DB *db, WT_PAGE *page, WT_VSTUFF *vs)
{
	u_int32_t addr, frags, i;

	addr = page->addr;
	frags = WT_OFF_TO_ADDR(db, page->size);
	for (i = 0; i < frags; ++i)
		if (bit_test(vs->fragbits, addr + i)) {
			__wt_api_db_errx(db,
			    "page fragment at addr %lu already verified",
			    (u_long)addr);
			return (WT_ERROR);
		}
	bit_nset(vs->fragbits, addr, addr + (frags - 1));
	return (0);
}

/*
 * __wt_bt_verify_checkfrag --
 *	Verify we've checked all the fragments in the file.
 */
static int
__wt_bt_verify_checkfrag(DB *db, WT_VSTUFF *vs)
{
	int ffc, ffc_start, ffc_end, frags, ret;

	frags = (int)vs->frags;		/* XXX: bitstring.h wants "ints" */
	ret = 0;

	/* Check for page fragments we haven't verified. */
	for (ffc_start = ffc_end = -1;;) {
		bit_ffc(vs->fragbits, frags, &ffc);
		if (ffc != -1) {
			bit_set(vs->fragbits, ffc);
			if (ffc_start == -1) {
				ffc_start = ffc_end = ffc;
				continue;
			}
			if (ffc_end == ffc - 1) {
				ffc_end = ffc;
				continue;
			}
		}
		if (ffc_start != -1) {
			__wt_api_db_errx(db,
			    "fragments %d to %d were never verified",
			    ffc_start, ffc_end);
			ret = WT_ERROR;
		}
		ffc_start = ffc_end = ffc;
		if (ffc == -1)
			break;
	}
	return (ret);
}
