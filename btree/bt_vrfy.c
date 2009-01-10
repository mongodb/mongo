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
static int __wt_bt_verify_connections(DB *, WT_PAGE *, bitstr_t *);
static int __wt_bt_verify_level(DB *, WT_ITEM_OFFP *, bitstr_t *, FILE *);
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
	ENV *env;
	IDB *idb;
	WT_PAGE *page;
	WT_ITEM_OFFP offp;
	bitstr_t *fragbits;
	int ret, tret;

	env = db->env;
	idb = db->idb;
	page = NULL;
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
	if ((ret = bit_alloc(env, idb->frags, &fragbits)) != 0)
		return (ret);

	/* Read the database description chunk. */
	if ((ret = __wt_bt_desc_read(db)) != 0)
		goto err;

	/* If no root address has been set, it's a one-leaf-page database. */
	if (idb->root_addr == WT_ADDR_INVALID) {
		if ((ret =
		    __wt_bt_page_in(db, WT_ADDR_FIRST_PAGE, 1, page)) != 0)
			goto err;
		if ((ret = __wt_bt_verify_page(db, page, fragbits, fp)) != 0)
			goto err;
		if ((ret = __wt_cache_db_out(db, page, 0)) != 0)
			goto err;
		page = NULL;
	} else {
		/*
		 * Construct an OFFP for __wt_bt_verify_level -- the addr
		 * is correct, but the level is not.   We don't store the
		 * level in the DESC structure, so there's no way to know
		 * what the correct level is yet.
		 */
		offp.addr = idb->root_addr;
		offp.level = WT_FIRST_INTERNAL_LEVEL;
		if ((ret = __wt_bt_verify_level(db, &offp, fragbits, fp)) != 0)
			goto err;
	}

	ret = __wt_bt_verify_checkfrag(db, fragbits);

err:	if (page != NULL &&
	    (tret = __wt_cache_db_out(db, page, 0)) != 0 && ret == 0)
		ret = tret;
	__wt_free(env, fragbits);
	return (ret);
}

/*
 * __wt_bt_verify_level --
 *	Verify a level of a tree.
 */
static int
__wt_bt_verify_level(DB *db, WT_ITEM_OFFP *offp, bitstr_t *fragbits, FILE *fp)
{
	WT_PAGE *page, *prev;
	WT_PAGE_HDR *hdr;
	WT_INDX *page_indx, *prev_indx;
	u_int32_t addr, frags;
	int first, ret, tret;
	int (*func)(DB *, const DBT *, const DBT *);

	ret = 0;

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
	 * describes the entire process of connection checking.  Consider the
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
	 * After page verification, we know the pages are OK, and all we need
	 * to do is confirm the tree itself is well formed.
	 *
	 * When walking each internal page level (I 1-3), we confirm the first
	 * key on each page is greater than the last key on the previous page.
	 * For example, we check that I2/K1 is greater than I1/K*, and I3/K1 is
	 * greater than I2/K*.  This check is safe as we verify the pages in
	 * list order, so before we check I2/K1 against I1/K*, we've verified
	 * both I2 and I1.
	 *
	 * This check is the check done in this function, all other connection
	 * checks are in __wt_bt_verify_connections().  The remainder of this
	 * comment describes those checks.
	 *
	 * When walking internal or leaf page levels (I 1-3, and later P 1-9),
	 * we confirm the first key on each page is greater than or equal to
	 * its referencing key on the parent page.  In other words, somewhere
	 * on I1 are keys referencing P 1-3.  When verifying P 1-3, we check
	 * their parent key against the first key on P 1-3.  We also check that
	 * the subsequent key in the parent level is greater than the last
	 * key on the page.   So, in the example, key 2 in I1 references P2.
	 * The check is that I1/K2 is less than the P2/K1, and I1/K3 is greater
	 * than P2/K*.
	 *
	 * The boundary cases are where the parent key is the first or last
	 * key on the page.
	 *
	 * If the key is the first key on the parent page, there are two cases:
	 * First, the key may be the first key in the level (I1/K1 in the
	 * example).  In this case, we confirm the page key is the first key
	 * in its level (P1/K1 in the example).  Second, if the key is not the
	 * first key in the level (I2/K1, or I3/K1 in the example).  In this
	 * case, there is no work to be done -- the check we already did, that
	 * the first key in each internal page sorts after the last key in the
	 * previous internal page guarantees the referenced key in the page is
	 * correct with respect to the previous page on the internal level.
	 *
	 * If the key is the last key on the parent page, there are two cases:
	 * First, the key may be the last key in the level (I3/K* in the
	 * example).  In this case, we confirm the page key is the last key
	 * in its level (P9/K* in the example).  Second, if the key is not the
	 * last key in the level (I1/K*, or I2/K* in the example).   In this
	 * case, we check the referenced key in the page against the first key
	 * in the subsequent page.  For example, P6/KN is compared against
	 * I3/K1.
	 *
	 * All of the connection checks are safe because we only look at the
	 * previous pages on the current level or pages in higher levels of
	 * the tree.
	 *
	 * We do it this way because, while I don't mind random access in the
	 * tree for the internal pages, I want to read the tree's leaf pages
	 * contiguously.  As long as all of the internal pages for any single
	 * level fit into the cache, we'll not move the disk heads except to
	 * get the next page we're verifying.
	 */
	frags = offp->level == WT_LEAF_LEVEL ?
	    WT_FRAGS_PER_LEAF(db) : WT_FRAGS_PER_INTL(db);
	for (addr = offp->addr, page = prev = NULL, first = 1;
	    addr != WT_ADDR_INVALID;
	    addr = hdr->nextaddr, prev = page, page = NULL) {
		/* Get the next page. */
		if ((ret = __wt_cache_db_in(db, addr, frags, &page, 0)) != 0)
			return (ret);

		/* Verify the page. */
		if ((ret = __wt_bt_verify_page(db, page, fragbits, fp)) != 0)
			goto err;

		/*
		 * If we're walking an internal page, we'll want to descend
		 * to the first offpage in this level, save the address and
		 * level information for the next iteration.
		 */
		hdr = page->hdr;
		if (first) {
			first = 0;
			 if (hdr->type == WT_PAGE_INT ||
			     hdr->type == WT_PAGE_DUP_INT)
				__wt_bt_first_offp(page, offp);
			else
				offp = NULL;

			/*
			 * Set the comparison function -- tucked away here
			 * because we can't set it without knowing what the
			 * page looks like, and we don't want to set it every
			 * time through the loop.
			 */
			if (hdr->type == WT_PAGE_DUP_INT ||
			    hdr->type == WT_PAGE_DUP_LEAF)
				func = db->btree_dup_compare;
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
		if ((ret = __wt_bt_verify_connections(db, page, fragbits)) != 0)
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

	if (ret == 0 && offp != NULL)
		ret = __wt_bt_verify_level(db, offp, fragbits, fp);

	return (ret);
}

/*
 * __wt_bt_verify_connections --
 *	Verify that the page is in the right place in the tree.
 */
static int
__wt_bt_verify_connections(DB *db, WT_PAGE *child, bitstr_t *fragbits)
{
	IDB *idb;
	WT_INDX *child_indx, *parent_indx;
	WT_PAGE *parent;
	WT_PAGE_DESC desc;
	WT_PAGE_HDR *hdr;
	u_int32_t addr, frags, i, nextaddr;
	int (*func)(DB *, const DBT *, const DBT *);
	int ret, tret;

	idb = db->idb;
	parent = NULL;
	hdr = child->hdr;
	addr = child->addr;
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
		 * record (which we've already read in) points to the right
		 * place.
		 */
		if (hdr->type == WT_PAGE_INT && idb->root_addr != addr) {
			__wt_db_errx(db,
			    "page at addr %lu doesn't match the database "
			    "description information",
			    (u_long)addr);
			return (WT_ERROR);
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
	frags = WT_FRAGS_PER_INTL(db);
	for (i = 0; i < frags; ++i)
		if (!bit_test(fragbits, hdr->prntaddr + i)) {
			__wt_db_errx(db,
			    "parent of page at addr %lu not found on internal "
			    "page links",
			    (u_long)addr);
			return (WT_ERROR);
		}
	if ((ret = __wt_bt_page_in(db, hdr->prntaddr, 0, parent)) != 0)
		return (ret);

	/* Check the levels match up. */
	if (hdr->level + 1 != parent->hdr->level) {
		__wt_db_errx(db,
		    "parent's level of page at addr %lu is not one more than "
		    "the page's level",
		    (u_long)addr);
		return (WT_ERROR);
	}

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
		func = db->btree_dup_compare;
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
			    "parent key of page at addr %lu is the smallest "
			    "in its level, but the page is not the smallest "
			    "in its level",
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
			    "parent key of page at addr %lu is the largest in "
			    "its level, but the page is not the largest in its "
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
			if ((ret =
			    __wt_bt_page_in(db, nextaddr, 0, parent)) != 0)
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

	/* Check the page type. */
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

	/* Check the page level. */
	switch (hdr->type) {
	case WT_PAGE_OVFL:
	case WT_PAGE_LEAF:
	case WT_PAGE_DUP_LEAF:
		if (hdr->level != WT_LEAF_LEVEL) {
			__wt_db_errx(db,
			    "leaf page at addr %lu is marked internal level",
			    (u_long)addr);
			return (WT_ERROR);
		}
		break;
	case WT_PAGE_INT:
	case WT_PAGE_DUP_INT:
		if (hdr->level == WT_LEAF_LEVEL) {
			__wt_db_errx(db,
			    "internal page at addr %lu is marked leaf level",
			    (u_long)addr);
			return (WT_ERROR);
		}
		break;
	}

	if (hdr->unused[0] != '\0' || hdr->unused[1] != '\0') {
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
	ENV *env;
	WT_ITEM *item;
	WT_ITEM_OFFP offp;
	WT_PAGE_HDR *hdr;
	u_int8_t *end;
	u_int32_t addr, i, item_no;
	int (*func)(DB *, const DBT *, const DBT *), ret;

	env = db->env;
	ret = 0;

	addr = page->addr;
	hdr = page->hdr;
	end = (u_int8_t *)hdr + WT_FRAGS_TO_BYTES(db, page->frags);

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
		func = db->btree_dup_compare;
	else
		func = db->btree_compare;

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
			    page->hdr->type == WT_PAGE_LEAF) {
				/*
				 * !!!
				 * Don't pass __wt_bt_verify_level a pointer
				 * to the on-page OFFP structure, it writes
				 * the offp passed in.
				 */
				offp = *(WT_ITEM_OFFP *)WT_ITEM_BYTE(item);
				if ((ret = __wt_bt_verify_level(
				    db, &offp, fragbits, fp)) != 0)
					goto err;
			}

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
		__wt_free(env, _a.item_ovfl.data);
	if (_b.item_ovfl.data != NULL)
		__wt_free(env, _b.item_ovfl.data);
	if (_c.item_ovfl.data != NULL)
		__wt_free(env, _c.item_ovfl.data);

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

	if ((ret =
	    __wt_bt_ovfl_page_in(db, ovfl->addr, ovfl->len, ovfl_page)) != 0)
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
