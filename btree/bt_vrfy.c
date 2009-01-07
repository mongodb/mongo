/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_item_walk(DB *, WT_PAGE *, bitstr_t *, FILE *);
static int __wt_bt_verify_checkfrag(DB *, bitstr_t *);
static int __wt_bt_verify_connections(DB *, WT_PAGE *, bitstr_t *, FILE *);
static int __wt_bt_verify_level(DB *, u_int32_t, bitstr_t *, FILE *);
static int __wt_bt_verify_ovfl(DB *, WT_ITEM_OVFL *, bitstr_t *, FILE *);

/*
 * __wt_db_verify --
 *	Db.verify method.
 */
int
__wt_db_verify(DB *db, u_int32_t flags)
{
	DB_FLAG_CHK(db, "Db.verify", flags, WT_APIMASK_DB_VERIFY);

	return (__wt_bt_verify_int(db, NULL));
}

/*
 * __wt_bt_verify_int --
 *	Verify a Btree, internal version, optionally dumping each page in
 *	debugging mode.
 */
int
__wt_bt_verify_int(DB *db, FILE *fp)
{
	IDB *idb;
	IENV *ienv;
	WT_PAGE *page;
	WT_PAGE_DESC desc;
	bitstr_t *fragbits;
	int ret;

	ienv = db->ienv;
	idb = db->idb;
	ret = 0;

	/*
	 * Allocate a bit array to represent the fragments in the file --
	 * it's how we keep track of the fragments we've visited.  Storing
	 * this on the heap seems reasonable: a 16GB file of 512B frags,
	 * where we track 8 frags per allocated byte, means we allocate 4MB
	 * for the bit array.   If we have to verify larger files than we
	 * can track this way, we'd have to write parts of the bit array
	 * into a file somewhere.
	 *
	 * !!!
	 * There's one portability issue -- the bitstring package uses "ints",
	 * not unsigned ints, or any fixed size.   If an "int" can't hold a
	 * big enough value, then we can lose.   There's a check here just
	 * to make sure we don't overflow.   I don't ever expect to see this
	 * error message, but better safe than sorry.
	 */
	if (idb->frags > INT_MAX) {
		__wt_db_errx(db, "file has too many fragments to verify");
		return (WT_ERROR);
	}
	if ((ret = bit_alloc(ienv, idb->frags, &fragbits)) != 0)
		return (ret);

	/* Get the root address. */
	if ((ret = __wt_bt_desc_read(db, &desc)) != 0)
		goto err;

	/* If no root address has been set, it's a one-leaf-page database. */
	if (desc.root_addr == WT_ADDR_INVALID) {
		if ((ret = __wt_cache_db_in(db,
		    WT_ADDR_FIRST_PAGE, WT_FRAGS_PER_PAGE(db), &page, 0)) != 0)
			goto err;
		if ((ret = __wt_bt_verify_page(db, page, fragbits, fp)) != 0)
			goto err;
		if ((ret = __wt_cache_db_out(db, page, 0)) != 0)
			goto err;
	} else
		if ((ret = __wt_bt_verify_level(
		    db, desc.root_addr, fragbits, fp)) != 0)
			goto err;

	ret = __wt_bt_verify_checkfrag(db, fragbits);

err:	__wt_free(ienv, fragbits);
	return (ret);
}

/*
 * __wt_bt_verify_level --
 *	Verify a level of a tree.
 */
static int
__wt_bt_verify_level(DB *db, u_int32_t addr, bitstr_t *fragbits, FILE *fp)
{
	WT_PAGE *page, *prev;
	WT_PAGE_HDR *hdr;
	WT_INDX *page_indx, *prev_indx;
	u_int32_t descend_addr;
	int (*func)(DB *, const DBT *, const DBT *);
	int first, ret, tret;

	descend_addr = WT_ADDR_INVALID;

	/*
	 * The plan is pretty simple.  We read through the levels of the tree,
	 * from top to bottom (root level to leaf level), and from left to
	 * right (smallest to greatest), verifying each page as we go.  After
	 * a page is verified, we know that it is correctly formed, and any
	 * keys it contains are correctly ordered.  After we verify a page, we
	 * check its connections.
	 *
	 * Most connection checks are done in the __wt_bt_verify_connections
	 * function, but one of them is done here.  The following comment
	 * describes the entire process of connection checking.  Imagine the
	 * following tree:
	 *
	 *	P1 - I1
	 *	P2
	 *	P3
	 *	|    |
	 *	P4 - I2
	 *	P5
	 *	P6
	 *	|    |
	 *	P7 - I3
	 *	P8
	 *	P9
	 *
	 * After page verification, we know the page is OK, and now all we
	 * need to do is confirm that the tree itself is well formed.
	 *
	 * When walking each internal page level (I 1-3), we confirm the first
	 * key on each page is greater than the last key on the previous page.
	 * For example, we check that I2/K1 is greater than I1/K*, and I3/K1 is
	 * greater than I2/K*.  This check is safe as we verify the pages in
	 * list order.  That is the only check done in this function, the other
	 * connection checks are done in __wt_bt_verify_connections.
	 *
	 * When walking internal or leaf page levels (I 1-3, and later P 1-9),
	 * we confirm the first key on each page is greater than or equal its
	 * the referencing key on the parent page.  In other words, somewhere
	 * on I1 is a key referencing P 1-3.  When verifying P 1-3, we check
	 * that internal key against the first key on P 1-3.  We also check
	 * the subsequent key in the internal level is greater than the last
	 * key on the page.   So, in the example, key 2 in I1 references P2.
	 * The check is that I1/K2 is less than the P2/K1, and I1/K3 is greater
	 * than P2/K*.
	 *
	 * The boundary cases are where the internal key is the first or last
	 * key on the page.
	 *
	 * If the key is the first key on the internal page, there are two
	 * cases: First, the key may be the first key in the level (I1/K1 in
	 * the example).  In this case, we confirm the referenced key in P1
	 * is the first key in its level.  Second, if the key is not the first
	 * key in the level (I2/K1, or I3/K1 in the example).  In this case,
	 * there is again, no work to be done -- the above check that the first
	 * key in each internal page sorts after the last key in the previous
	 * internal page guarantees the referenced key in P1 is correct with
	 * respect to the previous page on the internal level.
	 *
	 * If the key is the last key on the internal page, there are two
	 * cases: First, the key may be the last key in the level (I3/K* in
	 * the example).  In this case, we confirm the referenced key in P1
	 * is the last key in its level.  Second, if the key is not the first
	 * key in the level (I1/K*, or I2/K* in the example).   In this case,
	 * we check the referenced key in the page against the first key in
	 * the subsequent page.  For example, P6/KN would be compared against
	 * I3/K1.
	 *
	 * All of the connection checks are safe because we only look at the
	 * previous pages on the current level or pages in higher levels of
	 * the tree.
	 *
	 * We do it this way because, while I don't mind random access in the
	 * tree for the internal pages, I want to read the tree's leaf pages
	 * contiguously.  As long as the internal nodes for any single level
	 * fit into the cache, we'll not have to move the disk heads except to
	 * get the next page we're verifying.
	 */
	for (first = 1, page = prev = NULL;
	    addr != WT_ADDR_INVALID;
	    addr = hdr->nextaddr, prev = page, page = NULL) {
		/* Get the next page. */
		if ((ret = __wt_cache_db_in(db, addr,
		    WT_FRAGS_PER_PAGE(db), &page, WT_NO_INMEM_PAGE)) != 0)
			return (ret);

		/* Verify the page. */
		if ((ret = __wt_bt_verify_page(db, page, fragbits, fp)) != 0)
			goto err;

		/*
		 * If we're walking an internal page, we'll want to descend
		 * to the first offpage in this level, save the address for
		 * the next iteration.
		 */
		hdr = page->hdr;
		if (first) {
			first = 0;
			 if (hdr->type == WT_PAGE_INT ||
			     hdr->type == WT_PAGE_DUP_INT)
				 __wt_bt_first_offp_addr(page, &descend_addr);

			/*
			 * Set the comparison function -- tucked away here
			 * because we can't set it without knowing what the
			 * page looks like, and we don't want to set it every
			 * time through the loop.
			 */
			if (hdr->type == WT_PAGE_DUP_INT ||
			    hdr->type == WT_PAGE_DUP_LEAF)
				func = db->dup_compare;
			else
				func = db->btree_compare;
		}

		/*
		 * The page is OK, instantiate its in-memory information, if
		 * we don't have it already.
		 */
		if (page->indx == NULL &&
		    (ret = __wt_bt_page_inmem(db, page)) != 0)
			goto err;

		/* Verify its connections. */
		if ((ret =
		    __wt_bt_verify_connections(db, page, fragbits, fp)) != 0)
			goto err;

		if (prev == NULL)
			continue;

		/*
		 * If we have a previous page, there's one more check, the last
		 * key of the previous page against the first key of this page.
		 *
		 * The two keys we're going to compare may be overflow keys.
		 */
		prev_indx = prev->indx + (prev->indx_count - 1);
		if (prev_indx->data == NULL &&
		    (ret = __wt_bt_ovfl_copy_to_indx(db, prev, prev_indx)) != 0)
			goto err;
		page_indx = page->indx;
		if (page_indx->data == NULL &&
		    (ret = __wt_bt_ovfl_copy_to_indx(db, page, page_indx)) != 0)
			goto err;
		if (func(db, (DBT *)prev_indx, (DBT *)page_indx) >= 0) {
			__wt_db_errx(db,
			    "the first key on page at addr %lu does not sort "
			    "after the last key on the previous page", 
			    (u_long)addr);
			goto err;
		}

		/* We're done with the previous page. */
		if ((ret = __wt_cache_db_out(db, prev, 0)) != 0)
			goto err;
	}

err:	if (prev != NULL &&
	    (tret = __wt_cache_db_out(db, prev, 0)) != 0 && ret == 0)
		ret = tret;
	if (page != NULL &&
	    (tret = __wt_cache_db_out(db, page, 0)) != 0 && ret == 0)
		ret = tret;

	if (ret == 0 && descend_addr != WT_ADDR_INVALID)
		ret = __wt_bt_verify_level(db, descend_addr, fragbits, fp);

	return (ret);
}

/*
 * __wt_bt_verify_connections --
 *	Verify that the page is in the right place in the tree.
 */	
static int
__wt_bt_verify_connections(DB *db, WT_PAGE *child, bitstr_t *fragbits, FILE *fp)
{
	WT_INDX *child_indx, *parent_indx;
	WT_PAGE *parent;
	WT_PAGE_DESC desc;
	WT_PAGE_HDR *hdr;
	u_int32_t addr, frags, i, nextaddr;
	int (*func)(DB *, const DBT *, const DBT *);
	int ret, tret;

	parent = NULL;
	hdr = child->hdr;
	addr = child->addr;
	frags = WT_FRAGS_PER_PAGE(db);
	ret = 0;

	/*
	 * This function implements most of the connection checking in the
	 * tree, but not all of it -- see the comment at the beginning of
	 * the __wt_bt_verify_level function for details.
	 */

	/* Root pages are special cases, they shouldn't point to anything. */
	if (hdr->prntaddr == WT_ADDR_INVALID) {
		if (hdr->prevaddr != WT_ADDR_INVALID ||
		    hdr->nextaddr != WT_ADDR_INVALID) {
			__wt_db_errx(db,
			    "page at addr %lu has siblings, but no parent "
			    "address",
			    (u_long)addr);
			return (WT_ERROR);
		}

		/*
		 * If this is a primary root page, confirm the description
		 * record points to the right place.
		 */
		if (hdr->type == WT_PAGE_INT) {
			if ((ret = __wt_bt_desc_read(db, &desc)) != 0)
				return (ret);
			if (desc.root_addr != addr) {
				__wt_db_errx(db,
				    "page at addr %lu has no parent",
				    (u_long)addr);
				return (WT_ERROR);
			}
		}
		return (0);
	}

	/*
	 * If it's not the root page, we need a copy of its parent page.
	 *
	 * First, check to make sure we've verified the parent page -- if we
	 * haven't, there's a problem because we verified levels down the tree,
	 * starting at the top.
	 */
	for (i = 0; i < frags; ++i)
		if (!bit_test(fragbits, hdr->prntaddr + i)) {
			__wt_db_errx(db,
			    "parent of page at addr %lu not found on internal "
			    "page links",
			    (u_long)addr);
			return (WT_ERROR);
		}
	if ((ret = __wt_cache_db_in(db, hdr->prntaddr, frags, &parent, 0)) != 0)
		return (ret);

	/*
	 * Search the parent for the reference to this page -- because we've
	 * already verified this page, we can build the in-memory page info,
	 * and use it in the search.
	 */
	WT_INDX_FOREACH(parent, parent_indx, i)
		if (parent_indx->addr == addr)
			break;
	if (parent_indx == NULL) {
		__wt_db_errx(db,
		    "parent of page at addr %lu doesn't reference it",
		    (u_long)addr);
		goto err;
	}

	/* Set the comparison function. */
	if (hdr->type == WT_PAGE_DUP_INT ||
	    hdr->type == WT_PAGE_DUP_LEAF)
		func = db->dup_compare;
	else
		func = db->btree_compare;

	/*
	 * Confirm the parent's key is less than or equal to the first key
	 * on the child.
	 *
	 * If the parent's key is the smallest key on the page, check the
	 * parent's previous page addr.  If the previous page addr is not
	 * set (in other words, the parent is the smallest page on its level),
	 * confirm that's also the case for the child.
	 */
	if (parent_indx == parent->indx) {
		if (((hdr->prevaddr == WT_ADDR_INVALID &&
		    parent->hdr->prevaddr != WT_ADDR_INVALID) ||
		    (hdr->prevaddr != WT_ADDR_INVALID &&
		    parent->hdr->prevaddr == WT_ADDR_INVALID))) {
			__wt_db_errx(db,
			    "parent of page at addr %lu is the smallest in its "
			    "level, but the page is not the smallest in its "
			    "level",
			    (u_long)addr);
			goto err;
		}
	} else {
		/* The two keys we're going to compare may be overflow keys. */
		child_indx = child->indx;
		if (child_indx->data == NULL && (ret =
		    __wt_bt_ovfl_copy_to_indx(db, child, child_indx)) != 0)
			goto err;
		if (parent_indx->data == NULL && (ret =
		    __wt_bt_ovfl_copy_to_indx(db, parent, parent_indx)) != 0)
			goto err;

		/* Compare the parent's key against the child's key. */
		if (func(db, (DBT *)child_indx, (DBT *)parent_indx) < 0) {
			__wt_db_errx(db,
			    "the first key on page at addr %lu sorts before "
			    "its reference key on its parent's page",
			    (u_long)addr);
			goto err;
		}
	}

	/*
	 * Confirm the parent's following key is greater than the last key
	 * on the child.
	 *
	 * If the parent's key is the largest key on the page, look at the
	 * parent's next page addr.  If the parent's next page addr is set,
	 * confirm the first key on the page following the parent is greater
	 * than the last key on the child.  If the parent's next page addr
	 * is not set (in other words, the parent is the largest page on its
	 * level), confirm that's also the case for the child.
	 */
	if (parent_indx == (parent->indx + (parent->indx_count - 1))) {
		nextaddr = parent->hdr->nextaddr;
		if ((hdr->nextaddr == WT_ADDR_INVALID &&
		    nextaddr != WT_ADDR_INVALID) ||
		    (hdr->nextaddr != WT_ADDR_INVALID &&
		    nextaddr == WT_ADDR_INVALID)) {
			__wt_db_errx(db,
			    "parent of page at addr %lu is the largest in its "
			    "level, but the page is not the largest in its "
			    "level",
			    (u_long)addr);
			goto err;
		}

		/* Switch for the subsequent page at the parent level. */
		if ((ret = __wt_cache_db_out(db, parent, 0)) != 0)
			return (ret);
		if (nextaddr == WT_ADDR_INVALID)
			parent = NULL;
		else {
			if ((ret = __wt_cache_db_in(
			    db, nextaddr, frags, &parent, 0)) != 0)
				return (ret);
			parent_indx = parent->indx;
		}
	} else
		++parent_indx;

	if (parent != NULL) {
		/* The two keys we're going to compare may be overflow keys. */
		child_indx = child->indx + (child->indx_count - 1);
		if (child_indx->data == NULL && (ret =
		    __wt_bt_ovfl_copy_to_indx(db, child, child_indx)) != 0)
			goto err;
		if (parent_indx->data == NULL && (ret =
		    __wt_bt_ovfl_copy_to_indx(db, parent, parent_indx)) != 0)
			goto err;
		/* Compare the parent's key against the child's key. */
		if (func(db, (DBT *)child_indx, (DBT *)parent_indx) >= 0) {
			__wt_db_errx(db,
			    "the last key on page at addr %lu sorts after the "
			    "first key on a parent page",
			    (u_long)addr);
			goto err;
		}
	}

	if (0) {
err:		ret = WT_ERROR;
	}

	if (parent != NULL &&
	    (tret = __wt_cache_db_out(db, parent, 0)) != 0 && ret == 0)
		ret = tret;
	return (ret);
}

/*
 * __wt_bt_verify_page --
 *	Verify a single Btree page.
 */
int
__wt_bt_verify_page(DB *db, WT_PAGE *page, bitstr_t *fragbits, FILE *fp)
{
	WT_PAGE_HDR *hdr;
	u_int32_t addr, i;
	int ret;

	hdr = page->hdr;
	addr = page->addr;

	/*
	 * If we're verifying the whole tree, complain if there's a page
	 * we've already verified.
	 */
	if (fragbits != NULL) {
		for (i = 0; i < page->frags; ++i)
			if (bit_test(fragbits, addr + i)) {
				__wt_db_errx(db,
				    "page at addr %lu already verified",
				    (u_long)addr);
				return (WT_ERROR);
			}
		bit_nset(fragbits, addr, addr + (page->frags - 1));
	}

	/*
	 * FUTURE:
	 * Check the LSN against the existing log files.
	 */

	switch (hdr->type) {
	case WT_PAGE_OVFL:
		if (hdr->u.entries == 0) {
			__wt_db_errx(db,
			    "page at addr %lu has no entries", (u_long)addr);
			ret = WT_ERROR;
		}
		/* FALLTHROUGH */
	case WT_PAGE_INT:
	case WT_PAGE_LEAF:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
		break;
	default:
		__wt_db_errx(db,
		    "page at addr %lu has an invalid type of %lu",
		    (u_long)addr, (u_long)hdr->type);
		return (WT_ERROR);
	}

	if (hdr->unused[0] != '\0' ||
	    hdr->unused[1] != '\0' || hdr->unused[2] != '\0') {
		__wt_db_errx(db,
		    "page at addr %lu has non-zero unused header fields",
		    (u_long)addr);
		return (WT_ERROR);
	}

	/*
	 * Don't verify the checksum -- it verified when we first read the
	 * page.
	 */

	/* Page 0 has a descriptor record. */
	if (addr == WT_ADDR_FIRST_PAGE &&
	    (ret = __wt_bt_desc_verify(db, page)) != 0)
		return (ret);

	/* Verify the items on the page. */
	if (hdr->type != WT_PAGE_OVFL &&
	    (ret = __wt_bt_item_walk(db, page, fragbits, fp)) != 0)
		return (ret);

	return (0);
}

/*
 * __wt_bt_item_walk --
 *	Walk the items on a page and verify them.
 */
static int
__wt_bt_item_walk(DB *db, WT_PAGE *page, bitstr_t *fragbits, FILE *fp)
{
	struct {
		u_int32_t indx;			/* Item number */

		DBT *item;			/* Item to compare */
		DBT item_ovfl;			/* Overflow holder */
		DBT item_std;			/* On-page reference */
	} *current, *last_data, *last_key, *swap_tmp, _a, _b, _c;
	IENV *ienv;
	WT_ITEM *item;
	WT_PAGE_HDR *hdr;
	u_int8_t *end;
	u_int32_t addr, i, item_no;
	int (*func)(DB *, const DBT *, const DBT *), ret;

	ienv = db->ienv;
	hdr = page->hdr;
	addr = page->addr;
	ret = 0;

	/*
	 * We have 3 key/data items we track -- the last key, the last data
	 * item, and the current item.   They're stored in the _a, _b, and
	 * _c structures (it doesn't matter which) -- what matters is which
	 * item is referenced by current, last_data or last_key.
	 */
	_a.item = _b.item = _c.item = NULL;
	WT_CLEAR(_a.item_ovfl);
	WT_CLEAR(_a.item_std);
	WT_CLEAR(_b.item_ovfl);
	WT_CLEAR(_b.item_std);
	WT_CLEAR(_c.item_ovfl);
	WT_CLEAR(_c.item_std);
	current = &_a;
	last_data = &_b;
	last_key = &_c;

	/* Set the comparison function. */
	if (hdr->type == WT_PAGE_DUP_INT ||
	    hdr->type == WT_PAGE_DUP_LEAF)
		func = db->dup_compare;
	else
		func = db->btree_compare;

	end = (u_int8_t *)hdr + db->pagesize;
	item_no = 0;
	WT_ITEM_FOREACH(page, item, i) {
		++item_no;

		/* Check if this item is entirely on the page. */
		if ((u_int8_t *)item + sizeof(WT_ITEM) > end)
			goto eop;

		/* Check the item's type. */
		switch (item->type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (hdr->type != WT_PAGE_INT &&
			    hdr->type != WT_PAGE_LEAF &&
			    hdr->type != WT_PAGE_DUP_INT)
				goto item_vs_page;
			break;
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
			if (hdr->type != WT_PAGE_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_DUP:
		case WT_ITEM_DUP_OVFL:
			if (hdr->type != WT_PAGE_LEAF &&
			    hdr->type != WT_PAGE_DUP_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_OFFPAGE:
			if (hdr->type != WT_PAGE_INT &&
			    hdr->type != WT_PAGE_LEAF &&
			    hdr->type != WT_PAGE_DUP_INT) {
item_vs_page:			__wt_db_errx(db,
				    "item %lu on page at addr %lu is a %s "
				    "type on a %s page",
				    (u_long)item_no, (u_long)addr,
				    __wt_bt_item_type(item->type),
				    __wt_bt_hdr_type(hdr->type));
				goto err;
			}
			break;
		default:
			goto item_type;
		}

		/* Check the item's length. */
		switch (item->type) {
		case WT_ITEM_KEY:
		case WT_ITEM_DATA:
		case WT_ITEM_DUP:
			/* The length is variable, so we can't check it. */
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DUP_OVFL:
			if (item->len != sizeof(WT_ITEM_OVFL))
				goto item_len;
			break;
		case WT_ITEM_OFFPAGE:
			if (item->len != sizeof(WT_ITEM_OFFP)) {
item_len:			__wt_db_errx(db,
				    "item %lu on page at addr %lu has an "
				    "incorrect length",
				    (u_long)item_no, (u_long)addr);
				goto err;
			}
			break;
		default:
			goto item_type;
		}

		/* Check the unused fields. */
		if (item->unused[0] != 0 || item->unused[1] != 0) {
			__wt_db_errx(db,
			    "item %lu on page at addr %lu has non-zero "
			    "unused item fields", 
			    (u_long)item_no, (u_long)addr);
			goto err;
		}

		/* Check if the item's data is entirely on the page. */
		if ((u_int8_t *)WT_ITEM_NEXT(item) > end) {
eop:			__wt_db_errx(db,
			    "item %lu on page at addr %lu extends past the end "
			    " of the page",
			    (u_long)item_no, (u_long)addr);
			goto err;
		}

		/*
		 * When walking the whole file, verify off-page duplicate sets
		 * and overflow page references.
		 */
		if (fragbits != NULL) {
			if (item->type == WT_ITEM_OFFPAGE &&
			    page->hdr->type == WT_PAGE_LEAF &&
			    (ret = __wt_bt_verify_level(db,
			    ((WT_ITEM_OFFP *)WT_ITEM_BYTE(item))->addr,
			    fragbits, fp)) != 0)
				goto err;

			if ((item->type == WT_ITEM_KEY_OVFL ||
			    item->type == WT_ITEM_DATA_OVFL ||
			    item->type == WT_ITEM_DUP_OVFL) &&
			    (ret = __wt_bt_verify_ovfl(db,
			    (WT_ITEM_OVFL *)WT_ITEM_BYTE(item),
			    fragbits, fp)) != 0)
				goto err;
		}

		/* Some items aren't sorted on the page, so we're done. */
		if (item->type == WT_ITEM_DATA ||
		    item->type == WT_ITEM_DATA_OVFL ||
		    item->type == WT_ITEM_OFFPAGE)
			continue;

		/* Get a DBT that represents this item. */
		switch (item->type) {
		case WT_ITEM_KEY:
		case WT_ITEM_DUP:
			current->indx = item_no;
			current->item = &current->item_std;
			current->item_std.data = WT_ITEM_BYTE(item);
			current->item_std.size = item->len;
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DUP_OVFL:
			current->indx = item_no;
			current->item = &current->item_ovfl;
			if ((ret = __wt_bt_ovfl_copy_to_dbt(db, (WT_ITEM_OVFL *)
			    WT_ITEM_BYTE(item), current->item)) != 0)
				goto err;
			break;
		default:
			goto item_type;
		}

		/* Check the sort order. */
		switch (item->type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (last_key->item != NULL &&
			    func(db, last_key->item, current->item) >= 0) {
				__wt_db_errx(db,
				    "item %lu and item %lu on page at addr %lu "
				    "are incorrectly sorted", 
				    last_key->indx, current->indx,
				    (u_long)addr);
				goto err;
			}
			swap_tmp = last_key;
			last_key = current;
			current = swap_tmp;
			break;
		case WT_ITEM_DUP:
		case WT_ITEM_DUP_OVFL:
			if (last_data->item != NULL &&
			    func(db, last_data->item, current->item) >= 0) {
				__wt_db_errx(db,
				    "item %lu and item %lu on page at addr %lu "
				    "are incorrectly sorted", 
				    last_data->indx, current->indx,
				    (u_long)addr);
				goto err;
			}
			swap_tmp = last_data;
			last_data = current;
			current = swap_tmp;
			break;
		default:
			goto item_type;
		}
	}

	if (0) {
item_type:	__wt_db_errx(db,
		    "item %lu on page at addr %lu has an illegal type of %lu", 
		    (u_long)item_no, (u_long)addr, (u_long)item->type);
err:		ret = WT_ERROR;
	}

	if (_a.item_ovfl.data != NULL)
		__wt_free(ienv, _a.item_ovfl.data);
	if (_b.item_ovfl.data != NULL)
		__wt_free(ienv, _b.item_ovfl.data);
	if (_c.item_ovfl.data != NULL)
		__wt_free(ienv, _c.item_ovfl.data);

#ifdef HAVE_DIAGNOSTIC
	/* Optionally dump the page in debugging mode. */
	if (ret == 0 && fp != NULL)
		ret = __wt_bt_dump_page(db, page, NULL, fp);
#endif

	return (ret);
}

/*
 * __wt_bt_verify_ovfl --
 *	Verify an overflow item.
 */
static int
__wt_bt_verify_ovfl(DB *db, WT_ITEM_OVFL *ovfl, bitstr_t *fragbits, FILE *fp)
{
	WT_PAGE *ovfl_page;
	int ret, tret;

	if ((ret = __wt_cache_db_in(db, ovfl->addr,
	    WT_OVFL_BYTES_TO_FRAGS(db, ovfl->len), &ovfl_page, 0)) != 0)
		return (ret);

	ret = __wt_bt_verify_page(db, ovfl_page, fragbits, fp);

	if ((tret = __wt_cache_db_out(db, ovfl_page, 0)) != 0 && ret == 0)
		ret = tret;

	return (ret);
}

/*
 * __wt_bt_verify_checkfrag --
 *	Verify we've checked all the fragments in the file.
 */
static int
__wt_bt_verify_checkfrag(DB *db, bitstr_t *fragbits)
{
	IDB *idb;
	int ffc, ffc_start, ffc_end, ret;

	idb = db->idb;
	ret = 0;

	/* Check for page fragments we haven't verified. */
	for (ffc_start = ffc_end = -1;;) {
		bit_ffc(fragbits, idb->frags, &ffc);
		if (ffc != -1) {
			bit_set(fragbits, ffc);
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
			__wt_db_errx(db,
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
