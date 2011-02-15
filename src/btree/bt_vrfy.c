/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * There's a bunch of stuff we pass around during verification, group it
 * together to make the code prettier.
 */
typedef struct {
	uint32_t frags;				/* Total frags */
	bitstr_t *fragbits;			/* Frag tracking bit list */

	FILE	*stream;			/* Dump file stream */

	void (*f)(const char *, uint64_t);	/* Progress callback */
	uint64_t fcnt;				/* Progress counter */

	uint64_t duptree;			/* Dup tree record count */

	WT_PAGE *leaf;				/* Last leaf-page */
} WT_VSTUFF;

static int __wt_verify_addfrag(WT_TOC *, uint32_t, uint32_t, WT_VSTUFF *);
static int __wt_verify_checkfrag(DB *, WT_VSTUFF *);
static int __wt_verify_delfmt(DB *, uint32_t, uint32_t);
static int __wt_verify_dsk_chunk(WT_TOC *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_col_fix(DB *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_col_int(DB *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_col_rle(DB *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_item(WT_TOC *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_eof(DB *, uint32_t, uint32_t);
static int __wt_verify_eop(DB *, uint32_t, uint32_t);
static int __wt_verify_freelist(WT_TOC *, WT_VSTUFF *);
static int __wt_verify_key_order(WT_TOC *, WT_PAGE *);
static int __wt_verify_overflow_col(WT_TOC *, WT_PAGE *, WT_VSTUFF *);
static int __wt_verify_overflow_common(
		WT_TOC *, WT_OVFL *, uint32_t, uint32_t, WT_VSTUFF *);
static int __wt_verify_overflow_row(WT_TOC *, WT_PAGE *, WT_VSTUFF *);
static int __wt_verify_pc(WT_TOC *, WT_ROW *, WT_PAGE *, int);
static int __wt_verify_tree(WT_TOC *,
		WT_ROW *, uint64_t, uint64_t, uint32_t, WT_REF *, WT_VSTUFF *);

/*
 * __wt_db_verify --
 *	Verify a Btree.
 */
int
__wt_db_verify(WT_TOC *toc, void (*f)(const char *, uint64_t))
{
	return (__wt_verify(toc, f, NULL));
}

/*
 * __wt_verify --
 *	Verify a Btree, optionally dumping each page in debugging mode.
 */
int
__wt_verify(
    WT_TOC *toc, void (*f)(const char *, uint64_t), FILE *stream)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_VSTUFF vstuff;
	int ret;

	env = toc->env;
	db = toc->db;
	idb = db->idb;
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
	 * The first sector of the file is the description record -- ignore
	 * it for now.
	 */
	bit_nset(vstuff.fragbits, 0, 0);

	/* Verify the tree, starting at the root. */
	WT_ERR(__wt_verify_tree(toc, NULL, WT_RECORDS(&idb->root_off),
	    (uint64_t)1, WT_NOLEVEL, &idb->root_page, &vstuff));

	WT_ERR(__wt_verify_freelist(toc, &vstuff));

	WT_ERR(__wt_verify_checkfrag(db, &vstuff));

err:	/* Wrap up reporting and free allocated memory. */
	if (vstuff.f != NULL)
		vstuff.f(toc->name, vstuff.fcnt);
	if (vstuff.fragbits != NULL)
		__wt_free(env, vstuff.fragbits, 0);

	return (ret);
}

/*
 * __wt_verify_tree --
 *	Verify a tree, recursively descending through it in depth-first fashion.
 * The page argument was physically verified (so we know it's correctly formed),
 * and the in-memory version built.  Our job is to check logical relationships
 * in the page and in the tree.
 */
static int
__wt_verify_tree(
    WT_TOC *toc,		/* Thread of control */
    WT_ROW *parent_rip,		/* Internal key referencing this page, if any */
    uint64_t parent_records,	/* Parent's count of records in this tree */
    uint64_t start_recno,	/* First record on this page */
    uint32_t level,		/* Page's tree level */
    WT_REF *ref,		/* Already verified page reference */
    WT_VSTUFF *vs)		/* The verify package */
{
	DB *db;
	WT_COL *cip;
	WT_ITEM *item;
	WT_OFF *off;
	WT_OFF_RECORD *off_record;
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	WT_REPL *repl;
	WT_ROW *rip;
	uint64_t records;
	uint32_t i, item_num;
	int is_root, ret;

	db = toc->db;
	page = ref->page;
	dsk = page->dsk;
	ret = 0;

	/* Report progress every 10 pages. */
	if (vs->f != NULL && ++vs->fcnt % 10 == 0)
		vs->f(toc->name, vs->fcnt);

	/* Update frags list. */
	WT_ERR(__wt_verify_addfrag(toc, page->addr, page->size, vs));

#ifdef DIAGNOSTIC
	/* Optionally dump the page in debugging mode. */
	if (vs->stream != NULL)
		return (__wt_debug_page(toc, page, NULL, vs->stream));
#endif

	/*
	 * The page's physical structure was verified when it was read into
	 * memory by the read server thread, and then the in-memory version
	 * of the page was built.   Now we make sure the page and tree are
	 * logically consistent.
	 *
	 * !!!
	 * The problem: (1) the read server has to build the in-memory version
	 * of the page because the read server is the thread that flags when
	 * any thread can access the page in the tree; (2) we can't build the
	 * in-memory version of the page until the physical structure is known
	 * to be OK, so the read server has to verify at least the physical
	 * structure of the page; (3) doing complete page verification requires
	 * reading additional pages (for example, overflow keys imply reading
	 * overflow pages in order to test the key's order in the page); (4)
	 * the read server cannot read additional pages because it will hang
	 * waiting on itself.  For this reason, we split page verification
	 * into a physical verification, which allows the in-memory version
	 * of the page to be built, and then a subsequent logical verification
	 * which happens here.
	 */

	/*
	 * If passed a level of WT_NOLEVEL, that is, the only level that can't
	 * possibly be a valid database page level, this is the root page of
	 * the tree.
	 *
	 * If it's the root, use this page's level to initialize expected the
	 * values for the rest of the tree.
	 */
	is_root = level == WT_NOLEVEL ? 1 : 0;
	if (is_root)
		level = dsk->level;

	/* Check that tree levels and record counts match up. */
	if (dsk->level != level) {
		__wt_api_db_errx(db,
		    "page at addr %lu has a tree level of %lu where the "
		    "expected level was %lu",
		    (u_long)page->addr, (u_long)dsk->level, (u_long)level);
		goto err;
	}

	/*
	 * Check the starting record number and record counts.
	 *
	 * Confirm the number of records found on this page (by summing the
	 * WT_OFF_RECORD structure record counts) matches the WT_OFF_RECORD
	 * structure record count in our parent.  Use the in-memory record
	 * count for internal pages -- we could sum the record counts as we
	 * walk the page below, but we did that when building the in-memory
	 * version of the page, there's no reason to do it again.
	 */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		if (dsk->start_recno != start_recno) {
			__wt_api_db_errx(db,
			    "page at addr %lu has a starting record of %llu "
			    "where the expected starting record was %llu",
			    (u_long)page->addr,
			    (unsigned long long)dsk->start_recno,
			    (unsigned long long)start_recno);
			goto err;
		}
		if (page->records != parent_records) {
			__wt_api_db_errx(db,
			    "page at addr %lu has a record count of %llu where "
			    "the expected record count was %llu",
			    (u_long)page->addr, page->records,
			    (unsigned long long)parent_records);
			goto err;
		}
		break;
	case WT_PAGE_DUP_LEAF:
		/*
		 * The only row-store record count we maintain is a total count
		 * of the data items held in each off-page duplicate tree.
		 * Keep track of how many records on each duplicate tree leaf
		 * page and check it against the parent row-store leaf page
		 * stored count when the traversal is complete.
		 */
		vs->duptree += dsk->u.entries;
		/* FALLTHROUGH */
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		if (page->records != 0) {
			__wt_api_db_errx(db,
			    "page at addr %lu has a record count of %llu where "
			    "no record count was expected",
			    (u_long)page->addr, page->records);
			goto err;
		}
		break;
	default:
		break;
	}

	/*
	 * Check on-page overflow page references.
	 *
	 * There's a potential performance problem here: we read key overflow
	 * pages twice, once when checking the overflow page itself, and again
	 * when checking the key ordering.   It's a pain to combine the two
	 * tests (the page types with overflow items aren't exactly the same
	 * as the page types with ordered keys, and the underlying functions
	 * that instantiate (and decompress) overflow pages don't want to know
	 * anything about verification), and I don't want to keep the overflow
	 * keys in the cache, it's likely to be wasted space.  Until it's a
	 * problem, I'm going to assume the second read of the overflow key is
	 * satisfied in the operating system buffer cache, and not worry about
	 * it.  Table verify isn't likely to be a performance path anyway.
	 */
	switch (dsk->type) {
	case WT_PAGE_COL_VAR:
		WT_RET(__wt_verify_overflow_col(toc, page, vs));
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_verify_overflow_row(toc, page, vs));
		break;
	default:
		break;
	}

	/* Check on-page key ordering. */
	switch (dsk->type) {
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_verify_key_order(toc, page));
		break;
	default:
		break;
	}

	/* Check tree connections and recursively descend the tree. */
	switch (dsk->type) {
	case WT_PAGE_COL_INT:
		/* For each entry in an internal page, verify the subtree. */
		start_recno = dsk->start_recno;
		WT_INDX_FOREACH(page, cip, i) {
			records = WT_COL_OFF_RECORDS(cip);

			/* cip references the subtree containing the record */
			ref = WT_COL_REF(page, cip);
			off_record = WT_COL_OFF(cip);
			switch (ret =
			    __wt_page_in(toc, page, ref, off_record, 1)) {
			case 0:				/* Valid page */
				ret = __wt_verify_tree(toc, NULL,
				    records, start_recno, level - 1, ref, vs);
				__wt_hazard_clear(toc, ref->page);
				break;
			case WT_PAGE_DELETED:
				ret = 0;		/* Skip deleted pages */
				break;
			}
			if (ret != 0)
				goto err;

			start_recno += records;
		}
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		/*
		 * There are two row-store, logical connection checks:
		 *
		 * First, compare the internal node key leading to the current
		 * page against the first entry on the current page.  The
		 * internal node key must compare less than or equal to the
		 * first entry on the current page.
		 *
		 * Second, compare the largest key we've seen on any leaf page
		 * against the next internal node key we find.  This check is
		 * a little tricky: every time we find a leaf page, we save a
		 * reference in the vs->leaf field.  The next time we're about
		 * to indirect through an entry on an internal node, we compare
		 * the last entry on that saved page against the internal node
		 * entry's key.  In that comparison, the leaf page's key must
		 * be less than the internal node entry's key.
		 */
		if (parent_rip != NULL)
			WT_ERR(__wt_verify_pc(toc, parent_rip, page, 1));

		/* For each entry in an internal page, verify the subtree. */
		WT_INDX_FOREACH(page, rip, i) {
			/*
			 * At each off-page entry, we compare the current entry
			 * against the largest key in the subtree rooted to the
			 * immediate left of the current item; this key must
			 * compare less than or equal to the current item.  The
			 * trick here is we need the last leaf key, not the last
			 * internal node key.  It's returned to us in the leaf
			 * field of the vs structure, whenever we verify a leaf
			 * page.  Discard the leaf node as soon as we've used it
			 * in a comparison.
			 */
			if (vs->leaf != NULL) {
				WT_ERR(
				    __wt_verify_pc(toc, rip, vs->leaf, 0));
				__wt_hazard_clear(toc, vs->leaf);
				vs->leaf = NULL;
			}

			/* rip references the subtree containing the record */
			ref = WT_ROW_REF(page, rip);
			off = WT_ROW_OFF(rip);
			switch (ret = __wt_page_in(toc, page, ref, off, 1)) {
			case 0:				/* Valid page */
				ret = __wt_verify_tree(
				    toc, rip, (uint64_t)0,
				    (uint64_t)0, level - 1, ref, vs);
				/*
				 * Remaining special handling of the last
				 * verified leaf page: if we kept a reference
				 * to that page, don't release the hazard
				 * reference until after comparing the last key
				 * on that page against the next key in the
				 * tree.
				 */
				if (vs->leaf != ref->page)
					__wt_hazard_clear(toc, ref->page);
				break;
			case WT_PAGE_DELETED:
				ret = 0;		/* Skip deleted pages */
				break;
			}
			if (ret != 0)
				goto err;
		}
		break;
	case WT_PAGE_ROW_LEAF:
		/*
		 * For each entry in a row-store leaf page, verify any off-page
		 * duplicates tree.
		 */
		item_num = 0;
		WT_INDX_FOREACH(page, rip, i) {
			++item_num;

			/* Ignore anything except off-page duplicate trees. */
			if ((repl = WT_ROW_REPL(
			    page, rip)) != NULL && WT_REPL_DELETED_ISSET(repl))
				continue;
			item = rip->data;
			if (WT_ITEM_TYPE(item) != WT_ITEM_OFF_RECORD)
				continue;

			/* Verify the off-page duplicate tree. */
			vs->duptree = 0;

			ref = WT_ROW_DUP(page, rip);
			off_record = WT_ROW_OFF_RECORD(rip);
			switch (ret =
			    __wt_page_in(toc, page, ref, off_record, 1)) {
			case 0:				/* Valid page */
				ret = __wt_verify_tree(
				    toc, NULL, (uint64_t)0,
				    (uint64_t)0, WT_NOLEVEL, ref, vs);
				__wt_hazard_clear(toc, ref->page);
				break;
			case WT_PAGE_DELETED:
				ret = 0;		/* Skip deleted pages */
				break;
			}
			if (ret != 0)
				goto err;

			if (vs->duptree != WT_RECORDS(off_record)) {
				__wt_api_db_errx(db,
				    "off-page duplicate tree referenced from "
				    "item %lu of page %lu has a record count "
				    "of %llu when a record count of %llu was "
				    "expected",
				    (u_long)item_num, (u_long)page->addr,
				    (unsigned long long)vs->duptree,
				    (unsigned long long)WT_RECORDS(off_record));
				goto err;
			}
		}
		/* FALLTHROUGH */
	case WT_PAGE_DUP_LEAF:
		/*
		 * Retain a reference to all row-store leaf pages, we need them
		 * to check their last entry against the next internal key in
		 * the tree.
		 */
		vs->leaf = page;
		return (0);
	default:
		break;
	}

	/*
	 * The largest key on the last leaf page in the tree is never needed,
	 * there aren't any internal pages after it.  So, we get here with
	 * vs->leaf needing to be released.
	 */
err:	if (vs->leaf != NULL) {
		__wt_hazard_clear(toc, vs->leaf);
		vs->leaf = NULL;
	}

	return (ret);
}

/*
 * __wt_verify_pc --
 *	Compare a key on a parent page to a designated entry on a child page.
 */
static int
__wt_verify_pc(WT_TOC *toc, WT_ROW *parent_rip, WT_PAGE *child, int first_entry)
{
	DB *db;
	DBT *cd_ref, *pd_ref, *scratch1, *scratch2;
	WT_ROW *child_rip;
	int cmp, ret, (*func)(DB *, const DBT *, const DBT *);

	db = toc->db;
	scratch1 = scratch2 = NULL;
	ret = 0;

	/* Set the comparison function. */
	switch (child->dsk->type) {
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
	if (__wt_key_process(child_rip)) {
		WT_ERR(__wt_scr_alloc(toc, 0, &scratch1));
		WT_ERR(__wt_item_process(toc, child_rip->key, scratch1));
		cd_ref = scratch1;
	} else
		cd_ref = (DBT *)child_rip;
	if (__wt_key_process(parent_rip)) {
		WT_ERR(__wt_scr_alloc(toc, 0, &scratch2));
		WT_RET(__wt_item_process(toc, parent_rip->key, scratch2));
		pd_ref = scratch2;
	} else
		pd_ref = (DBT *)parent_rip;

	/* Compare the parent's key against the child's key. */
	cmp = func(db, cd_ref, pd_ref);

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

err:	if (scratch1 != NULL)
		__wt_scr_release(&scratch1);
	if (scratch2 != NULL)
		__wt_scr_release(&scratch2);

	return (ret);
}

/*
 * __wt_verify_key_order --
 *	Check on-page key ordering.
 */
static int
__wt_verify_key_order(WT_TOC *toc, WT_PAGE *page)
{
	struct {
		DBT	*dbt;			/* DBT to compare */
		DBT	*scratch;		/* scratch buffer */
	} *current, *last, _a, _b;
	DB *db;
	WT_PAGE_DISK *dsk;
	WT_ROW *rip;
	uint32_t i;
	int (*func)(DB *, const DBT *, const DBT *), ret;

	db = toc->db;
	dsk = page->dsk;
	ret = 0;

	WT_CLEAR(_a);
	WT_CLEAR(_b);
	current = &_a;
	WT_ERR(__wt_scr_alloc(toc, 0, &current->scratch));
	last = &_b;
	WT_ERR(__wt_scr_alloc(toc, 0, &last->scratch));

	/* Set the comparison function. */
	switch (dsk->type) {
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

	/* Walk the page, comparing keys. */
	WT_INDX_FOREACH(page, rip, i) {
		/* Skip duplicates */
		if (WT_ROW_INDX_IS_DUPLICATE(page, rip))
			continue;

		/*
		 * The two keys we're going to compare may be overflow keys --
		 * don't bother instantiating the keys in the tree, there's no
		 * reason to believe we're going to be working in this database.
		 */
		if (__wt_key_process(rip)) {
			WT_RET(__wt_item_process(
			    toc, rip->key, current->scratch));
			current->dbt = current->scratch;
		} else
			current->dbt = (DBT *)rip;

		/* Compare the current key against the last key. */
		if (last->dbt != NULL &&
		    func(db, last->dbt, current->dbt) >= 0) {
			__wt_api_db_errx(db,
			    "the %lu and %lu keys on page at addr %lu are "
			    "incorrectly sorted",
			    (u_long)WT_ROW_SLOT(page, rip) - 1,
			    (u_long)WT_ROW_SLOT(page, rip),
			    (u_long)page->addr);
			ret = WT_ERROR;
			goto err;
		}
	}

err:	if (_a.scratch != NULL)
		__wt_scr_release(&_a.scratch);
	if (_b.scratch != NULL)
		__wt_scr_release(&_b.scratch);

	return (ret);
}

/*
 * __wt_verify_dsk_page --
 *	Verify a single Btree page as read from disk.
 */
int
__wt_verify_dsk_page(
    WT_TOC *toc, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	DB *db;

	db = toc->db;

	/* Check the page type. */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_FREELIST:
	case WT_PAGE_OVFL:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		break;
	case WT_PAGE_INVALID:
	default:
		__wt_api_db_errx(db,
		    "page at addr %lu has an invalid type of %lu",
		    (u_long)addr, (u_long)dsk->type);
		return (WT_ERROR);
	}

	/*
	 * FUTURE:
	 * Check the LSN against the existing log files.
	 */

	/* Ignore the checksum -- it verified when we first read the page. */

	/* Check the page level. */
	switch (dsk->type) {
	case WT_PAGE_FREELIST:
	case WT_PAGE_OVFL:
		if (dsk->level != WT_NOLEVEL)
			goto err_level;
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		if (dsk->level != WT_LLEAF)
			goto err_level;
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		if (dsk->level <= WT_LLEAF) {
err_level:		__wt_api_db_errx(db,
			    "%s page at addr %lu has incorrect tree level "
			    "of %lu",
			    __wt_page_type_string(dsk),
			    (u_long)addr, (u_long)dsk->level);
			return (WT_ERROR);
		}
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	if (dsk->unused[0] != '\0' || dsk->unused[1] != '\0') {
		__wt_api_db_errx(db,
		    "page at addr %lu has non-zero unused header fields",
		    (u_long)addr);
		return (WT_ERROR);
	}

	/* Verify the items on the page. */
	switch (dsk->type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_verify_dsk_item(toc, dsk, addr, size));
		break;
	case WT_PAGE_COL_INT:
		WT_RET(__wt_verify_dsk_col_int(db, dsk, addr, size));
		break;
	case WT_PAGE_COL_FIX:
		WT_RET(__wt_verify_dsk_col_fix(db, dsk, addr, size));
		break;
	case WT_PAGE_COL_RLE:
		WT_RET(__wt_verify_dsk_col_rle(db, dsk, addr, size));
		break;
	case WT_PAGE_FREELIST:
	case WT_PAGE_OVFL:
		WT_RET(__wt_verify_dsk_chunk(toc, dsk, addr, size));
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	return (0);
}

/*
 * __wt_verify_dsk_item --
 *	Walk a disk page of WT_ITEMs, and verify them.
 */
static int
__wt_verify_dsk_item(
    WT_TOC *toc, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	enum { IS_FIRST, WAS_KEY, WAS_DATA, WAS_DUP_DATA } last_item_type;
	DB *db;
	WT_ITEM *item;
	WT_OVFL *ovfl;
	WT_OFF *off;
	WT_OFF_RECORD *off_record;
	off_t file_size;
	uint8_t *end;
	uint32_t i, item_num, item_len, item_type;

	db = toc->db;
	file_size = db->idb->fh->file_size;

	end = (uint8_t *)dsk + size;

	last_item_type = IS_FIRST;
	item_num = 0;
	WT_ITEM_FOREACH(dsk, item, i) {
		++item_num;

		/* Check if this item is entirely on the page. */
		if ((uint8_t *)item + sizeof(WT_ITEM) > end)
			goto eop;

		item_type = WT_ITEM_TYPE(item);
		item_len = WT_ITEM_LEN(item);

		/* Check the item's type. */
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (dsk->type != WT_PAGE_ROW_INT &&
			    dsk->type != WT_PAGE_ROW_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_KEY_DUP:
		case WT_ITEM_KEY_DUP_OVFL:
			if (dsk->type != WT_PAGE_DUP_INT)
				goto item_vs_page;
			break;
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
			if (dsk->type != WT_PAGE_COL_VAR &&
			    dsk->type != WT_PAGE_ROW_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_DATA_DUP:
		case WT_ITEM_DATA_DUP_OVFL:
			if (dsk->type != WT_PAGE_DUP_LEAF &&
			    dsk->type != WT_PAGE_ROW_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_DEL:
			/* Deleted items only appear on column-store pages. */
			if (dsk->type != WT_PAGE_COL_VAR)
				goto item_vs_page;
			break;
		case WT_ITEM_OFF:
			if (dsk->type != WT_PAGE_DUP_INT &&
			    dsk->type != WT_PAGE_ROW_INT &&
			    dsk->type != WT_PAGE_ROW_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_OFF_RECORD:
			if (dsk->type != WT_PAGE_ROW_LEAF) {
item_vs_page:			__wt_api_db_errx(db,
				    "illegal item and page type combination "
				    "(item %lu on page at addr %lu is a %s "
				    "item on a %s page)",
				    (u_long)item_num, (u_long)addr,
				    __wt_item_type_string(item),
				    __wt_page_type_string(dsk));
				return (WT_ERROR);
			}
			break;
		default:
			__wt_api_db_errx(db,
			    "item %lu on page at addr %lu has an illegal type "
			    "of %lu",
			    (u_long)item_num, (u_long)addr, (u_long)item_type);
			return (WT_ERROR);
		}

		/*
		 * Only row-store leaf pages require item type ordering checks,
		 * other page types don't have ordering relationships between
		 * their WT_ITEM entries, so we can skip those tests.
		 */
		if (dsk->type != WT_PAGE_ROW_LEAF)
			goto skip_order_check;

		/*
		 * For row-store leaf pages, check for:
		 *	two keys in a row,
		 *	two non-dup data items in a row,
		 *	inter-mixed dup and non-dup data items,
		 *	a data item as the first item on a page.
		 */
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_DUP:
		case WT_ITEM_KEY_DUP_OVFL:
		case WT_ITEM_KEY_OVFL:
			if (last_item_type == WAS_KEY) {
				__wt_api_db_errx(db,
				    "item %lu on page at addr %lu is first of "
				    "two adjacent keys",
				    (u_long)item_num - 1, (u_long)addr);
				return (WT_ERROR);
			} else
				last_item_type = WAS_KEY;
			break;
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DEL:
		case WT_ITEM_OFF_RECORD:
			switch (last_item_type) {
			case IS_FIRST:
				goto first_data;
			case WAS_DATA:
				__wt_api_db_errx(db,
				    "item %lu on page at addr %lu is "
				    "the first of two adjacent data "
				    "items",
				    (u_long)item_num - 1, (u_long)addr);
				return (WT_ERROR);
			case WAS_DUP_DATA:
				goto mixed_order;
			case WAS_KEY:
				last_item_type = WAS_DATA;
				break;
			}
			break;
		case WT_ITEM_DATA_DUP:
		case WT_ITEM_DATA_DUP_OVFL:
			switch (last_item_type) {
			case IS_FIRST:
first_data:				__wt_api_db_errx(db,
				    "page at addr %lu begins with a "
				    "data item",
				    (u_long)addr);
				return (WT_ERROR);
			case WAS_DATA:
mixed_order:				__wt_api_db_errx(db,
				    "item %lu on page at addr %lu is "
				    "the first of mixed duplicate and "
				    "non-duplicate data items",
				    (u_long)item_num - 1, (u_long)addr);
				return (WT_ERROR);
			case WAS_DUP_DATA:
			case WAS_KEY:
				last_item_type = WAS_DUP_DATA;
				break;
			}
			break;
		}

skip_order_check:
		/* Check the item's length. */
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_DUP:
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_DUP:
			/* The length is variable, we can't check it. */
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_KEY_DUP_OVFL:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DATA_DUP_OVFL:
			if (item_len != sizeof(WT_OVFL))
				goto item_len;
			break;
		case WT_ITEM_DEL:
			if (item_len != 0)
				goto item_len;
			break;
		case WT_ITEM_OFF:
			if (item_len != sizeof(WT_OFF))
				goto item_len;
			break;
		case WT_ITEM_OFF_RECORD:
			if (item_len != sizeof(WT_OFF_RECORD)) {
item_len:			__wt_api_db_errx(db,
				    "item %lu on page at addr %lu has an "
				    "incorrect length",
				    (u_long)item_num, (u_long)addr);
				return (WT_ERROR);
			}
			break;
		default:
			break;
		}

		/* Check if the item is entirely on the page. */
		if ((uint8_t *)WT_ITEM_NEXT(item) > end)
			goto eop;

		/* Check if the referenced item is entirely in the file. */
		switch (item_type) {
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_KEY_DUP_OVFL:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DATA_DUP_OVFL:
			ovfl = WT_ITEM_BYTE_OVFL(item);
			if (WT_ADDR_TO_OFF(db, ovfl->addr) +
			    WT_HDR_BYTES_TO_ALLOC(db, ovfl->size) > file_size)
				goto eof;
			break;
		case WT_ITEM_OFF:
			off = WT_ITEM_BYTE_OFF(item);
			if (WT_ADDR_TO_OFF(db,
			    off->addr) + off->size > file_size)
				goto eof;
			break;
		case WT_ITEM_OFF_RECORD:
			off_record = WT_ITEM_BYTE_OFF_RECORD(item);
			if (WT_ADDR_TO_OFF(db,
			    off_record->addr) + off_record->size > file_size)
				goto eof;
			break;
		default:
			break;
		}
	}
	return (0);

eof:	return (__wt_verify_eof(db, item_num, addr));
eop:	return (__wt_verify_eop(db, item_num, addr));
}

/*
 * __wt_verify_dsk_col_int --
 *	Walk a WT_PAGE_COL_INT disk page and verify it.
 */
static int
__wt_verify_dsk_col_int(DB *db, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	IDB *idb;
	WT_OFF_RECORD *off_record;
	uint8_t *end;
	uint32_t i, entry_num;

	idb = db->idb;
	end = (uint8_t *)dsk + size;

	entry_num = 0;
	WT_OFF_FOREACH(dsk, off_record, i) {
		++entry_num;

		/* Check if this entry is entirely on the page. */
		if ((uint8_t *)off_record + sizeof(WT_OFF_RECORD) > end)
			return (__wt_verify_eop(db, entry_num, addr));

		/* Check if the reference is past the end-of-file. */
		if (WT_ADDR_TO_OFF(db,
		    off_record->addr) + off_record->size > idb->fh->file_size)
			return (__wt_verify_eof(db, entry_num, addr));
	}

	return (0);
}

/*
 * __wt_verify_dsk_col_fix --
 *	Walk a WT_PAGE_COL_FIX disk page and verify it.
 */
static int
__wt_verify_dsk_col_fix(DB *db, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	u_int len;
	uint32_t i, j, entry_num;
	uint8_t *data, *end, *p;

	len = db->fixed_len;
	end = (uint8_t *)dsk + size;

	entry_num = 0;
	WT_FIX_FOREACH(db, dsk, data, i) {
		++entry_num;

		/* Check if this entry is entirely on the page. */
		if (data + len > end)
			return (__wt_verify_eop(db, entry_num, addr));

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

	return (0);

delfmt:	return (__wt_verify_delfmt(db, entry_num, addr));
}

/*
 * __wt_verify_dsk_col_rle --
 *	Walk a WT_PAGE_COL_RLE disk page and verify it.
 */
static int
__wt_verify_dsk_col_rle(DB *db, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	u_int len;
	uint32_t i, j, entry_num;
	uint8_t *data, *end, *last_data, *p;

	end = (uint8_t *)dsk + size;

	last_data = NULL;
	len = db->fixed_len + sizeof(uint16_t);

	entry_num = 0;
	WT_RLE_REPEAT_FOREACH(db, dsk, data, i) {
		++entry_num;

		/* Check if this entry is entirely on the page. */
		if (data + len > end)
			return (__wt_verify_eop(db, entry_num, addr));

		/* Count must be non-zero. */
		if (WT_RLE_REPEAT_COUNT(data) == 0) {
			__wt_api_db_errx(db,
			    "fixed-length entry %lu on page at addr "
			    "%lu has a repeat count of 0",
			    (u_long)entry_num, (u_long)addr);
			return (WT_ERROR);
		}

		/* Deleted items are entirely nul bytes. */
		p = WT_RLE_REPEAT_DATA(data);
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
		    memcmp(WT_RLE_REPEAT_DATA(last_data),
		    WT_RLE_REPEAT_DATA(data), db->fixed_len) == 0 &&
		    WT_RLE_REPEAT_COUNT(last_data) < UINT16_MAX) {
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

	return (0);

delfmt:	return (__wt_verify_delfmt(db, entry_num, addr));
}

/*
 * __wt_verify_overflow_col --
 *	Check on-page column-store overflow references.
 */
static int
__wt_verify_overflow_col(WT_TOC *toc, WT_PAGE *page, WT_VSTUFF *vs)
{
	WT_COL *cip;
	WT_ITEM *item;
	uint32_t i;

	/* Walk the in-memory page, verifying overflow items. */
	WT_INDX_FOREACH(page, cip, i) {
		item = cip->data;
		if (WT_ITEM_TYPE(item) == WT_ITEM_DATA_OVFL)
			WT_RET(__wt_verify_overflow_common(
			    toc, WT_ITEM_BYTE_OVFL(item),
			    WT_COL_SLOT(page, cip) + 1, page->addr, vs));
	}
	return (0);
}

/*
 * __wt_verify_overflow_row --
 *	Check on-page row-store overflow references.
 */
static int
__wt_verify_overflow_row(WT_TOC *toc, WT_PAGE *page, WT_VSTUFF *vs)
{
	WT_ITEM *data_item, *key_item;
	WT_ROW *rip;
	uint32_t i;
	int check_data;

	/*
	 * Walk the in-memory page, verifying overflow items.   We service 4
	 * page types here: DUP_INT, DUP_LEAF, ROW_INT and ROW_LEAF.  In the
	 * case of DUP_INT, DUP_LEAF and ROW_INT, we only check the key, as
	 * there is either no data item, or the data item is known to not be
	 * an overflow page.   In the case of ROW_LEAF, we have to check both
	 * the key and the data item.
	 */
	check_data = page->dsk->type == WT_PAGE_ROW_LEAF ? 1 : 0;

	/*
	 * Walk the in-memory page, verifying overflow items.
	 *
	 * We have to walk the original disk page as well as the current page:
	 * see the comment at WT_INDX_AND_KEY_FOREACH for details.
	 */
	WT_INDX_AND_KEY_FOREACH(page, rip, key_item, i) {
		if (!WT_ROW_INDX_IS_DUPLICATE(page, rip)) {
			/*
			 * WT_ITEM_DATA_XXX types are listed because the data
			 * items for off-page duplicate trees are sorted, and
			 * the WT_ROW "key" item actually reference data items.
			 */
			switch (WT_ITEM_TYPE(key_item)) {
			case WT_ITEM_DATA_DUP_OVFL:
			case WT_ITEM_DATA_OVFL:
			case WT_ITEM_KEY_DUP_OVFL:
			case WT_ITEM_KEY_OVFL:
				WT_RET(__wt_verify_overflow_common(
				    toc, WT_ITEM_BYTE_OVFL(key_item),
				    WT_ROW_SLOT(page, rip) + 1, page->addr,
				    vs));
				break;
			default:
				break;
			}
		}

		if (check_data) {
			data_item = rip->data;
			switch (WT_ITEM_TYPE(data_item)) {
			case WT_ITEM_DATA_OVFL:
			case WT_ITEM_DATA_DUP_OVFL:
				WT_RET(__wt_verify_overflow_common(
				    toc, WT_ITEM_BYTE_OVFL(data_item),
				    WT_ROW_SLOT(page, rip) + 1, page->addr,
				    vs));
				break;
			default:
				break;
			}
		}
	}
	return (0);
}

/*
 * __wt_verify_overflow_common --
 *	Common code that reads in an overflow page and checks it.
 */
static int
__wt_verify_overflow_common(WT_TOC *toc,
    WT_OVFL *ovfl, uint32_t entry_num, uint32_t page_ref_addr, WT_VSTUFF *vs)
{
	DB *db;
	DBT *scratch1;
	WT_PAGE_DISK *dsk;
	uint32_t addr, size;
	int ret;

	db = toc->db;
	scratch1 = NULL;
	ret = 0;

	addr = ovfl->addr;
	size = WT_HDR_BYTES_TO_ALLOC(db, ovfl->size);

	/* Allocate enough memory to hold the overflow pages. */
	WT_RET(__wt_scr_alloc(toc, size, &scratch1));

	/* Read the page. */
	dsk = scratch1->data;
	WT_ERR(__wt_page_disk_read(toc, dsk, addr, size));

	/*
	 * Verify the disk image -- this function would normally be called
	 * from the asynchronous read server, but overflow pages are read
	 * synchronously. Regardless, we break the overflow verification code
	 * into two parts, on-disk format checking and internal checking,
	 * just so it looks like all of the other page type checking.
	 */
	WT_ERR(__wt_verify_dsk_chunk(toc, dsk, addr, size));

	/* Add the fragments. */
	WT_ERR(__wt_verify_addfrag(toc, addr, size, vs));

	/*
	 * The only other thing to check is that the size we have in the page
	 * matches the size on the underlying overflow page.
	 */
	if (ovfl->size != dsk->u.datalen) {
		__wt_api_db_errx(db,
		    "overflow page reference in item %lu on page at addr %lu "
		    "does not match the data size on the overflow page",
		    (u_long)entry_num, (u_long)page_ref_addr);
		ret = WT_ERROR;
	}

err:	__wt_scr_release(&scratch1);

	return (ret);
}

/*
 * __wt_verify_dsk_chunk --
 *	Verify the WT_PAGE_FREELIST and WT_PAGE_OVFL disk pages.
 */
static int
__wt_verify_dsk_chunk(
    WT_TOC *toc, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	DB *db;
	uint32_t len;
	uint8_t *p;

	db = toc->db;

	/*
	 * Overflow and freelist pages are roughly identical, both are simply
	 * chunks of data.   This routine should also be used for any chunks
	 * of data we store in the file in the future.
	 */
	if (dsk->u.datalen == 0) {
		__wt_api_db_errx(db,
		    "%s page at addr %lu has no data",
		    __wt_page_type_string(dsk), (u_long)addr);
		return (WT_ERROR);
	}

	/* Any data after the data chunk should be nul bytes. */
	p = (uint8_t *)dsk + (WT_PAGE_DISK_SIZE + dsk->u.datalen);
	len = size - (WT_PAGE_DISK_SIZE + dsk->u.datalen);
	for (; len > 0; ++p, --len)
		if (*p != '\0') {
			__wt_api_db_errx(db,
			    "%s page at addr %lu has non-zero trailing bytes",
			    __wt_page_type_string(dsk), (u_long)addr);
			return (WT_ERROR);
		}

	return (0);
}

/*
 * __wt_verify_eop --
 *	Generic item extends past the end-of-page error.
 */
static int
__wt_verify_eop(DB *db, uint32_t entry_num, uint32_t addr)
{
	__wt_api_db_errx(db,
	    "item %lu on page at addr %lu extends past the end of the page",
	    (u_long)entry_num, (u_long)addr);
	return (WT_ERROR);
}

/*
 * __wt_verify_eof --
 *	Generic item references non-existent file pages error.
 */
static int
__wt_verify_eof(DB *db, uint32_t entry_num, uint32_t addr)
{
	__wt_api_db_errx(db,
	    "off-page item %lu on page at addr %lu references non-existent "
	    "file pages",
	    (u_long)entry_num, (u_long)addr);
	return (WT_ERROR);
}

/*
 * __wt_verify_delfmt --
 *	WT_PAGE_COL_FIX and WT_PAGE_COL_RLE error where a deleted item has
 *	non-nul bytes.
 */
static int
__wt_verify_delfmt(DB *db, uint32_t entry_num, uint32_t addr)
{
	__wt_api_db_errx(db,
	    "deleted fixed-length entry %lu on page at addr %lu has non-nul "
	    "bytes",
	    (u_long)entry_num, (u_long)addr);
	return (WT_ERROR);
}

/*
 * __wt_verify_freelist --
 *	Add the freelist fragments to the list of verified fragments.
 */
static int
__wt_verify_freelist(WT_TOC *toc, WT_VSTUFF *vs)
{
	IDB *idb;
	WT_FREE_ENTRY *fe;

	idb = toc->db->idb;

	TAILQ_FOREACH(fe, &idb->freeqa, qa)
		WT_RET(__wt_verify_addfrag(toc, fe->addr, fe->size, vs));
	return (0);
}

/*
 * __wt_verify_addfrag --
 *	Add the WT_PAGE's fragments to the list, and complain if we've already
 *	verified this chunk of the file.
 */
static int
__wt_verify_addfrag(WT_TOC *toc, uint32_t addr, uint32_t size, WT_VSTUFF *vs)
{
	DB *db;
	uint32_t frags, i;

	db = toc->db;

	frags = WT_OFF_TO_ADDR(db, size);
	for (i = 0; i < frags; ++i)
		if (bit_test(vs->fragbits, addr + i)) {
			__wt_api_db_errx(db,
			    "page fragment at addr %lu already verified",
			    (u_long)addr);
			return (0);
		}
	bit_nset(vs->fragbits, addr, addr + (frags - 1));
	return (0);
}

/*
 * __wt_verify_checkfrag --
 *	Verify we've checked all the fragments in the file.
 */
static int
__wt_verify_checkfrag(DB *db, WT_VSTUFF *vs)
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
			if (ffc_start == ffc_end)
				__wt_api_db_errx(db,
				    "fragment %d was never verified",
				    ffc_start);
			else
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
