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
 * There's a bunch of stuff we pass around during verification, group it
 * together to make the code prettier.
 */
typedef struct {
	u_int32_t frags;			/* Total frags */
	bitstr_t *fragbits;			/* Frag tracking bit list */

	FILE	*stream;			/* Dump file stream */

	void (*f)(const char *s, u_int64_t);	/* Progress callback */
	const char *ptag;			/* Progress tag */
	u_int64_t fcnt;				/* Progress counter */
} VSTUFF;

static int __wt_bt_verify_checkfrag(DB *, VSTUFF *);
static int __wt_bt_verify_connections(WT_TOC *, WT_PAGE *, VSTUFF *);
static int __wt_bt_verify_level(WT_TOC *, u_int32_t, int, VSTUFF *);
static int __wt_bt_verify_ovfl(WT_TOC *, WT_OVFL *, VSTUFF *);
static int __wt_bt_verify_page_col_fix(WT_TOC *, WT_PAGE *);
static int __wt_bt_verify_page_col_int(WT_TOC *, WT_PAGE *);
static int __wt_bt_verify_page_item(WT_TOC *, WT_PAGE *, VSTUFF *);

/*
 * __wt_db_verify --
 *	Verify a Btree.
 */
int
__wt_db_verify(DB *db, void (*f)(const char *s, u_int64_t))
{
	ENV *env;
	WT_TOC *toc;
	int ret;

	env = db->env;

	WT_RET(env->toc(env, 0, &toc));
	WT_TOC_DB_INIT(toc, db, "Db.verify");

	ret = __wt_bt_verify_int(toc, f, "Db.verify", NULL);

	WT_TRET(toc->close(toc, 0));

	return (ret);
}

/*
 * __wt_bt_verify_int --
 *	Verify a Btree, optionally dumping each page in debugging mode.
 */
int
__wt_bt_verify_int(WT_TOC *toc,
    void (*f)(const char *s, u_int64_t), const char *ptag, FILE *stream)
{
	DB *db;
	ENV *env;
	IDB *idb;
	VSTUFF vstuff;
	WT_PAGE *page;
	int ret;

	env = toc->env;
	db = toc->db;
	idb = db->idb;
	ret = 0;

	memset(&vstuff, 0, sizeof(vstuff));
	vstuff.stream = stream;
	vstuff.f = f;
	vstuff.ptag = ptag;

	/*
	 * If we don't have a root page yet, read the database description
	 * to get the allocation and page sizes.   If we still don't have
	 * a root page, we're done, the file is empty.
	 */
	if (idb->root_page == NULL) {
		WT_ERR(__wt_bt_root_page(toc));
		if (idb->root_page == NULL)
			return (0);
	}

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
		__wt_db_errx(db, "file is too large to verify");
		goto err;
	}
	WT_ERR(bit_alloc(env, vstuff.frags, &vstuff.fragbits));

	/* Check for one-page databases. */
	switch (idb->root_page->hdr->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		WT_TRET(__wt_bt_verify_level(
		    toc, idb->root_page->addr, 0, &vstuff));
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__wt_bt_page_in(toc, WT_ADDR_FIRST_PAGE, 1, 0, &page));
		ret = __wt_bt_verify_page(toc, page, &vstuff);
		WT_TRET(__wt_bt_page_out(toc, page, 0));
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

	WT_TRET(__wt_bt_verify_checkfrag(db, &vstuff));

err:	if (vstuff.fragbits != NULL)
		__wt_free(env, vstuff.fragbits, 0);

	/* Wrap up reporting. */
	if (vstuff.f != NULL)
		vstuff.f(vstuff.ptag, vstuff.fcnt);

	return (ret);
}

/*
 * __wt_bt_verify_level --
 *	Verify a level of a tree.
 */
static int
__wt_bt_verify_level(WT_TOC *toc, u_int32_t addr, int isleaf, VSTUFF *vs)
{
	DB *db;
	WT_PAGE *page, *prev;
	WT_PAGE_HDR *hdr;
	WT_ROW_INDX *page_ip, *prev_ip;
	u_int32_t addr_arg;
	int first, isleaf_arg, ret;
	int (*func)(DB *, const DBT *, const DBT *);

	db = toc->db;
	addr_arg = WT_ADDR_INVALID;
	ret = 0;

	/*
	 * Callers pass us a reference to an on-page WT_ITEM_OFF_INT/LEAF.
	 *
	 * The plan is pretty simple.  We read through the levels of the tree,
	 * from top to bottom (root level to leaf level), and from left to
	 * right (smallest to greatest), verifying each page as we go.  First
	 * we verify each page, so we know it is correctly formed, and any
	 * keys it contains are correctly ordered.  After page verification,
	 * we check its connections within the tree.
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
	for (first = 1, page = prev = NULL;
	    addr != WT_ADDR_INVALID; addr = hdr->nextaddr) {
		/* Discard any previous page and get the next page. */
		if (prev != NULL)
			WT_ERR(__wt_bt_page_out(toc, prev, 0));
		prev = page;
		page = NULL;
		WT_ERR(__wt_bt_page_in(toc, addr, isleaf, 0, &page));

		/* Verify the page. */
		WT_ERR(__wt_bt_verify_page(toc, page, vs));

		hdr = page->hdr;
		if (first) {
			first = 0;

			/*
			 * If we're walking an internal level, we'll want to
			 * descend to the first offpage in this level.  Save
			 * away the address and level information for our next
			 * iteration.
			 */
			switch (hdr->type) {
			case WT_PAGE_COL_INT:
			case WT_PAGE_DUP_INT:
			case WT_PAGE_ROW_INT:
				__wt_bt_first_offp(
				    page, &addr_arg, &isleaf_arg);
				break;
			case WT_PAGE_COL_FIX:
			case WT_PAGE_COL_VAR:
			case WT_PAGE_DUP_LEAF:
			case WT_PAGE_ROW_LEAF:
				break;
			WT_ILLEGAL_FORMAT(db);
			}

			/*
			 * Set the comparison function -- tucked away here
			 * because we can't set it without knowing what the
			 * page looks like, and we don't want to set it every
			 * time through the loop.
			 */
			switch (hdr->type) {
			case WT_PAGE_DUP_INT:
			case WT_PAGE_DUP_LEAF:
				func = db->btree_compare_dup;
				break;
			case WT_PAGE_ROW_INT:
			case WT_PAGE_ROW_LEAF:
				func = db->btree_compare;
				break;
			case WT_PAGE_COL_FIX:
			case WT_PAGE_COL_INT:
			case WT_PAGE_COL_VAR:
				func = NULL;
				break;
			WT_ILLEGAL_FORMAT(db);
			}
		}

		/*
		 * The page is OK, instantiate its in-memory information, if
		 * we don't have it already.
		 */
		if (page->indx_count == 0)
			WT_ERR(__wt_bt_page_inmem(db, page));

		/* Verify its connections. */
		WT_ERR(__wt_bt_verify_connections(toc, page, vs));

		/*
		 * If we have a previous page, and the tree has sorted items,
		 * there's one more check, the last key of the previous page
		 * against the first key of this page.
		 *
		 * The keys we're going to compare may need to be instantiated.
		 */
		if (func == NULL || prev == NULL)
			continue;

		prev_ip = prev->u.r_indx + (prev->indx_count - 1);
		if (WT_ROW_INDX_PROCESS(prev_ip))
			WT_ERR(__wt_bt_key_to_indx(toc, prev, prev_ip));
		page_ip = page->u.r_indx;
		if (WT_ROW_INDX_PROCESS(page_ip))
			WT_ERR(__wt_bt_key_to_indx(toc, page, page_ip));
		if (func(db, (DBT *)prev_ip, (DBT *)page_ip) >= 0) {
			__wt_db_errx(db,
			    "the first key on page at addr %lu does not sort "
			    "after the last key on the previous page",
			    (u_long)addr);
			ret = WT_ERROR;
			goto err;
		}
	}

err:	if (prev != NULL)
		WT_TRET(__wt_bt_page_out(toc, prev, 0));
	if (page != NULL)
		WT_TRET(__wt_bt_page_out(toc, page, 0));

	if (ret == 0 && addr_arg != WT_ADDR_INVALID)
		ret = __wt_bt_verify_level(toc, addr_arg, isleaf_arg, vs);

	return (ret);
}

/*
 * __wt_bt_verify_connections --
 *	Verify the page is in the right place in the tree.
 */
static int
__wt_bt_verify_connections(WT_TOC *toc, WT_PAGE *child, VSTUFF *vs)
{
	DB *db;
	WT_OFF *offp;
	WT_PAGE *parent;
	WT_PAGE_HDR *hdr;
	WT_ROW_INDX *child_ip, *parent_ip;
	u_int32_t addr, frags, i, nextaddr, root_addr;
	int (*func)(DB *, const DBT *, const DBT *);
	int ret;

	db = toc->db;
	parent = NULL;
	hdr = child->hdr;
	addr = child->addr;
	ret = 0;

	/*
	 * This function implements most of the connection checking in the
	 * tree, but not all of it -- see the comment at the beginning of
	 * the __wt_bt_verify_level function for details.
	 *
	 * Root pages are special cases, they shouldn't point to anything.
	 */
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
		 * If this is the primary root page, confirm the description
		 * record (which we've already read in) points to the right
		 * place.
		 */
		if (hdr->type == WT_PAGE_COL_INT ||
		    hdr->type == WT_PAGE_ROW_INT) {
			WT_RET(__wt_bt_desc_read(toc, &root_addr));
			if (root_addr != addr) {
				__wt_db_errx(db,
				    "page at addr %lu appears to be a root "
				    "page which doesn't match the database "
				    "descriptor record", (u_long)addr);
				return (WT_ERROR);
			}
			return (0);
		}
	}

	/*
	 * If it's not the root page, we need a copy of its parent page.
	 *
	 * First, check to make sure we've verified the parent page -- if we
	 * haven't, there's a problem because we verified levels down the tree,
	 * starting at the top.   Then, read the page in.  Since we've already
	 * verified it, we can build the in-memory information.
	 */
	frags = WT_OFF_TO_ADDR(db, db->intlsize);
	for (i = 0; i < frags; ++i)
		if (!bit_test(vs->fragbits, hdr->prntaddr + i)) {
			__wt_db_errx(db,
			    "parent of page at addr %lu not found on internal "
			    "page links",
			    (u_long)addr);
			return (WT_ERROR);
		}
	WT_RET(__wt_bt_page_in(toc, hdr->prntaddr, 0, 1, &parent));

	/* Search the parent for the reference to the child page. */
	switch (parent->hdr->type) {
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_INDX_FOREACH(parent, parent_ip, i)
			if (WT_ROW_OFF_ADDR(parent_ip) == addr)
				break;
		break;
	case WT_PAGE_COL_INT:
		WT_INDX_FOREACH(parent, parent_ip, i)
			if (WT_COL_OFF_ADDR(parent_ip) == addr)
				break;
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}
	if (parent_ip == NULL) {
		__wt_db_errx(db,
		    "parent of page at addr %lu doesn't reference it",
		    (u_long)addr);
		goto err_set;
	}

	/* Check that the record counts are correct. */
	switch (parent->hdr->type) {
	case WT_PAGE_COL_INT:
		offp = (WT_OFF *)parent_ip->page_data;
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		offp = WT_ITEM_BYTE_OFF(parent_ip->page_data);
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}
	if (child->records != WT_RECORDS(offp)) {
		__wt_db_errx(db,
		    "parent of page at addr %lu has incorrect record count "
		    "(parent: %llu, child: %llu)",
		    (u_long)addr, WT_RECORDS(offp), child->records);
		goto err_set;
	}

	/*
	 * The only remaining work is to compare the sort order of the keys on
	 * the parent and child pages.  If the pages aren't sorted (that is, if
	 * it's a column store), we're done.  Otherwise, choose a comparison
	 * function.
	 */
	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		goto done;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
		func = db->btree_compare_dup;
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		func = db->btree_compare;
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

	/*
	 * Confirm the parent's key is less than or equal to the first key
	 * on the child.
	 *
	 * If the parent's key is the smallest key on the page, check the
	 * parent's previous page addr.  If the previous page addr is not
	 * set (in other words, the parent is the smallest page on its level),
	 * confirm that's also the case for the child.
	 */
	if (parent_ip == parent->u.r_indx) {
		if (((hdr->prevaddr == WT_ADDR_INVALID &&
		    parent->hdr->prevaddr != WT_ADDR_INVALID) ||
		    (hdr->prevaddr != WT_ADDR_INVALID &&
		    parent->hdr->prevaddr == WT_ADDR_INVALID))) {
			__wt_db_errx(db,
			    "parent key of page at addr %lu is the smallest "
			    "key in its level of the tree, but the child key "
			    "is not the smallest key in its level",
			    (u_long)addr);
			goto err_set;
		}
	} else {
		/* The two keys we're going to compare may be overflow keys. */
		child_ip = child->u.r_indx;
		if (WT_ROW_INDX_PROCESS(child_ip))
			WT_ERR(__wt_bt_key_to_indx(toc, child, child_ip));
		if (WT_ROW_INDX_PROCESS(parent_ip))
			WT_ERR(__wt_bt_key_to_indx(toc, parent, parent_ip));

		/* Compare the parent's key against the child's key. */
		if (func(db, (DBT *)child_ip, (DBT *)parent_ip) < 0) {
			__wt_db_errx(db,
			    "the first key on page at addr %lu sorts before "
			    "its reference key on its parent's page",
			    (u_long)addr);
			goto err_set;
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
	if (parent_ip == (parent->u.r_indx + (parent->indx_count - 1))) {
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
			goto err_set;
		}

		/* Switch for the subsequent page at the parent level. */
		WT_RET(__wt_bt_page_out(toc, parent, 0));
		if (nextaddr == WT_ADDR_INVALID)
			parent = NULL;
		else {
			WT_RET(__wt_bt_page_in(toc, nextaddr, 0, 1, &parent));
			parent_ip = parent->u.r_indx;
		}
	} else
		++parent_ip;

	if (parent != NULL) {
		/* The two keys we're going to compare may be overflow keys. */
		child_ip = child->u.r_indx + (child->indx_count - 1);
		if (WT_ROW_INDX_PROCESS(child_ip))
			WT_ERR(__wt_bt_key_to_indx(toc, child, child_ip));
		if (WT_ROW_INDX_PROCESS(parent_ip))
			WT_ERR(__wt_bt_key_to_indx(toc, parent, parent_ip));
		/* Compare the parent's key against the child's key. */
		if (func(db, (DBT *)child_ip, (DBT *)parent_ip) >= 0) {
			__wt_db_errx(db,
			    "the last key on page at addr %lu sorts after the "
			    "first key on a parent page",
			    (u_long)addr);
			goto err_set;
		}
	}

	if (0) {
err_set:	ret = WT_ERROR;
	}

done:
err:	if (parent != NULL)
		WT_TRET(__wt_bt_page_out(toc, parent, 0));

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
	VSTUFF *vs;
	u_int32_t addr, frags, i;

	vs = vs_arg;

	db = toc->db;
	hdr = page->hdr;
	addr = page->addr;

	/* Report progress every 100 pages. */
	if (vs != NULL && vs->f != NULL && ++vs->fcnt % 100 == 0)
		vs->f(vs->ptag, vs->fcnt);

	/*
	 * If we're verifying the whole tree, complain if there's a page
	 * we've already verified.
	 */
	if (vs != NULL && vs->fragbits != NULL) {
		frags = WT_OFF_TO_ADDR(db, page->bytes);
		for (i = 0; i < frags; ++i)
			if (bit_test(vs->fragbits, addr + i)) {
				__wt_db_errx(db,
				    "page at addr %lu already verified",
				    (u_long)addr);
				return (WT_ERROR);
			}
		bit_nset(vs->fragbits, addr, addr + (frags - 1));
	}

	/*
	 * FUTURE:
	 * Check the LSN against the existing log files.
	 */

	/* Check the page type. */
	switch (hdr->type) {
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
			__wt_db_errx(db,
			    "overflow page at addr %lu has no entries",
			    (u_long)addr);
			return (WT_ERROR);
		}
		break;
	case WT_PAGE_INVALID:
	default:
		__wt_db_errx(db,
		    "page at addr %lu has an invalid type of %lu",
		    (u_long)addr, (u_long)hdr->type);
		return (WT_ERROR);
	}

	if (hdr->unused[0] != '\0' || hdr->unused[1] != '\0') {
		__wt_db_errx(db,
		    "page at addr %lu has non-zero unused header fields",
		    (u_long)addr);
		return (WT_ERROR);
	}

	if (hdr->flags != 0)
		if (hdr->type != WT_PAGE_COL_INT ||
		    hdr->flags != WT_OFFPAGE_REF_LEAF) {
			__wt_db_errx(db,
			    "page at addr %lu has an invalid flags field "
			    "of %#lx",
			    (u_long)addr, (u_long)hdr->flags);
			return (WT_ERROR);
		}

	/*
	 * Don't verify the checksum -- it verified when we first read the
	 * page.
	 */

	/* Page 0 has the descriptor record. */
	if (addr == WT_ADDR_FIRST_PAGE)
		WT_RET(__wt_bt_desc_verify(db, page));

	/* Verify the items on the page. */
	switch (hdr->type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_bt_verify_page_item(toc, page, vs));
		break;
	case WT_PAGE_COL_INT:
		WT_RET(__wt_bt_verify_page_col_int(toc, page));
		break;
	case WT_PAGE_COL_FIX:
		WT_RET(__wt_bt_verify_page_col_fix(toc, page));
		break;
	case WT_PAGE_OVFL:
		break;
	WT_ILLEGAL_FORMAT(db);
	}

#ifdef HAVE_DIAGNOSTIC
	/* Optionally dump the page in debugging mode. */
	if (vs != NULL && vs->stream != NULL)
		return (__wt_bt_debug_page(toc, page, NULL, vs->stream, 0));
#endif
	return (0);
}

/*
 * __wt_bt_verify_page_item --
 *	Walk a page of WT_ITEMs, and verify them.
 */
static int
__wt_bt_verify_page_item(WT_TOC *toc, WT_PAGE *page, VSTUFF *vs)
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
	WT_OFF *offp;
	WT_PAGE_HDR *hdr;
	u_int8_t *end;
	u_int32_t addr, i, item_num, item_len, item_type;
	int (*func)(DB *, const DBT *, const DBT *), ret;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ret = 0;

	hdr = page->hdr;
	end = (u_int8_t *)hdr + page->bytes;
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
		case WT_ITEM_OFF_INT:
		case WT_ITEM_OFF_LEAF:
			if (hdr->type != WT_PAGE_DUP_INT &&
			    hdr->type != WT_PAGE_ROW_INT &&
			    hdr->type != WT_PAGE_ROW_LEAF) {
item_vs_page:			__wt_db_errx(db,
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
			__wt_db_errx(db,
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
			/* The length is variable, so we can't check it. */
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DUP_OVFL:
			if (item_len != sizeof(WT_OVFL))
				goto item_len;
			break;
		case WT_ITEM_OFF_INT:
		case WT_ITEM_OFF_LEAF:
			if (item_len != sizeof(WT_OFF)) {
item_len:			__wt_db_errx(db,
				    "item %lu on page at addr %lu has an "
				    "incorrect length",
				    (u_long)item_num, (u_long)addr);
				goto err_set;
			}
			break;
		default:
			break;
		}

		/* Check if the item's data is entirely on the page. */
		if ((u_int8_t *)WT_ITEM_NEXT(item) > end) {
eop:			__wt_db_errx(db,
			    "item %lu on page at addr %lu extends past the end "
			    " of the page",
			    (u_long)item_num, (u_long)addr);
			goto err_set;
		}

		/*
		 * When walking the whole file, verify off-page duplicate trees
		 * (any off-page reference on a row store leaf page) as well as
		 * overflow references.
		 *
		 * Check to see if addresses are past EOF; the check is simple
		 * and won't catch edge cases where the page starts before the
		 * EOF, but extends past EOF.  That's OK, those are not likely
		 * cases, and we'll fail when we try and read the page.
		 */
		if (vs != NULL && vs->fragbits != NULL)
			switch (item_type) {
			case WT_ITEM_KEY_OVFL:
			case WT_ITEM_DATA_OVFL:
			case WT_ITEM_DUP_OVFL:
				ovflp = WT_ITEM_BYTE_OVFL(item);
				if (WT_ADDR_TO_OFF(db, ovflp->addr) +
				    WT_OVFL_BYTES(db, ovflp->len) >
				    idb->fh->file_size)
					goto eof;
				WT_ERR(__wt_bt_verify_ovfl(toc, ovflp, vs));
				break;
			case WT_ITEM_OFF_INT:
				if (hdr->type != WT_PAGE_ROW_LEAF)
					break;
				offp = WT_ITEM_BYTE_OFF(item);
				if (WT_ADDR_TO_OFF(db, offp->addr) +
				    db->intlsize > idb->fh->file_size)
					goto eof;
				WT_ERR(__wt_bt_verify_level(
				    toc, offp->addr, 0, vs));
				break;
			case WT_ITEM_OFF_LEAF:
				if (hdr->type != WT_PAGE_ROW_LEAF)
					break;
				offp = WT_ITEM_BYTE_OFF(item);
				if (WT_ADDR_TO_OFF(db, offp->addr) +
				    db->leafsize > idb->fh->file_size) {
eof:					__wt_db_errx(db,
					    "off-page reference in item %lu on "
					    "page at addr %lu extends past the "
					    "end of the file",
					    (u_long)item_num, (u_long)addr);
					goto err_set;
				}
				WT_ERR(__wt_bt_verify_level(
				    toc, offp->addr, 1, vs));
				break;
			default:
				break;
			}

		/* Some items aren't sorted on the page, so we're done. */
		if (item_type == WT_ITEM_DATA ||
		    item_type == WT_ITEM_DATA_OVFL ||
		    item_type == WT_ITEM_OFF_INT ||
		    item_type == WT_ITEM_OFF_LEAF)
			continue;

		/* Get a DBT that represents this item. */
		current->indx = item_num;
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_DUP:
			current->item = &current->item_std;
			current->item->data = WT_ITEM_BYTE(item);
			current->item->size = item_len;
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DUP_OVFL:
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
			    &current->item_comp.data_len,
			    &current->item_comp.size));
			current->item = &current->item_comp;
		}

		/* Check the sort order. */
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (last_key->item != NULL &&
			    func(db, last_key->item, current->item) >= 0) {
				__wt_db_errx(db,
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
				__wt_db_errx(db,
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
		default:
			break;
		}
	}

	if (0) {
err_set:	ret = WT_ERROR;
	}

err:	__wt_free(env, _a.item_ovfl.data, _a.item_ovfl.data_len);
	__wt_free(env, _b.item_ovfl.data, _b.item_ovfl.data_len);
	__wt_free(env, _c.item_ovfl.data, _c.item_ovfl.data_len);
	__wt_free(env, _a.item_comp.data, _a.item_comp.data_len);
	__wt_free(env, _b.item_comp.data, _b.item_comp.data_len);
	__wt_free(env, _c.item_comp.data, _c.item_comp.data_len);

	return (ret);
}

/*
 * __wt_bt_verify_page_col_int --
 *	Walk a WT_PAGE_COL_INT page and verify it.
 */
static int
__wt_bt_verify_page_col_int(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	IDB *idb;
	WT_OFF *offp;
	WT_PAGE_HDR *hdr;
	u_int8_t *end;
	u_int32_t addr, i, entry_num;

	db = toc->db;
	idb = db->idb;
	hdr = page->hdr;
	end = (u_int8_t *)hdr + page->bytes;
	addr = page->addr;

	entry_num = 0;
	WT_OFF_FOREACH(page, offp, i) {
		++entry_num;

		/* Check if this entry is entirely on the page. */
		if ((u_int8_t *)offp + sizeof(WT_OFF) > end) {
			__wt_db_errx(db,
			    "offpage reference %lu on page at addr %lu extends "
			    "past the end of the page",
			    (u_long)entry_num, (u_long)addr);
			return (WT_ERROR);
		}

		/* Check if the reference is past the end-of-file. */
		if (WT_ADDR_TO_OFF(db, offp->addr) +
		    (F_ISSET(hdr, WT_OFFPAGE_REF_LEAF) ?
		    db->leafsize : db->intlsize) > idb->fh->file_size) {
			__wt_db_errx(db,
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
__wt_bt_verify_page_col_fix(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	IDB *idb;
	u_int len;
	u_int32_t addr, i, entry_num;
	u_int8_t *end, *p;

	db = toc->db;
	idb = db->idb;
	end = (u_int8_t *)page->hdr + page->bytes;
	addr = page->addr;

	if (F_ISSET(idb, WT_REPEAT_COMP)) {
		len = db->fixed_len + sizeof(u_int16_t);

		entry_num = 0;
		WT_FIX_REPEAT_FOREACH(db, page, p, i) {
			++entry_num;

			/* Check if this entry is entirely on the page. */
			if (p + len > end)
				goto eop;

			/* Count must be non-zero. */
			if (*(u_int16_t *)p == 0) {
				__wt_db_errx(db,
				    "fixed-length entry %lu on page at addr "
				    "%lu has a repeat count of 0",
				    (u_long)entry_num, (u_long)addr);
				return (WT_ERROR);
			}
		}
	} else {
		len = db->fixed_len;

		entry_num = 0;
		WT_FIX_FOREACH(db, page, p, i) {
			++entry_num;

			/* Check if this entry is entirely on the page. */
			if (p + len > end) {
eop:				__wt_db_errx(db,
				    "fixed-length entry %lu on page at addr "
				    "%lu extends past the end of the page",
				    (u_long)entry_num, (u_long)addr);
				return (WT_ERROR);
			}
		}
	}

	return (0);
}

/*
 * __wt_bt_verify_ovfl --
 *	Verify an overflow item.
 */
static int
__wt_bt_verify_ovfl(WT_TOC *toc, WT_OVFL *ovflp, VSTUFF *vs)
{
	WT_PAGE *pagep;
	int ret;

	WT_RET(__wt_bt_ovfl_in(toc, ovflp->addr, ovflp->len, &pagep));

	ret = __wt_bt_verify_page(toc, pagep, vs);

	WT_TRET(__wt_bt_page_out(toc, pagep, 0));

	return (ret);
}

/*
 * __wt_bt_verify_checkfrag --
 *	Verify we've checked all the fragments in the file.
 */
static int
__wt_bt_verify_checkfrag(DB *db, VSTUFF *vs)
{
	int ffc, ffc_start, ffc_end, ret;

	ret = 0;

	/* Check for page fragments we haven't verified. */
	for (ffc_start = ffc_end = -1;;) {
		bit_ffc(vs->fragbits, vs->frags, &ffc);
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
