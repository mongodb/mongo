/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

/*
 * There's a bunch of stuff we pass around during verification, group it
 * together to make the code prettier.
 */
typedef struct {
	uint32_t  frags;			/* Total frags */
	bitstr_t *fragbits;			/* Frag tracking bit list */

	/*
	 * When tree verification starts, copy the eviction generation number,
	 * updated by the eviction thread before it writes pages.  The problem
	 * is we have to turn off our checks that every fragment in a file is
	 * verified once (and only once) if the eviction thread writes pages,
	 * because we can race with the eviction thread.
	 */
#define	WT_EVICT_CURRENT(toc, vs)					\
	((vs)->evict_rec_gen == (toc)->env->ienv->cache->evict_rec_gen)
	uint64_t  evict_rec_gen;		/* Eviction generation */

	FILE	*stream;			/* Dump file stream */

	void (*f)(const char *, uint64_t);	/* Progress callback */
	uint64_t fcnt;				/* Progress counter */

	WT_PAGE *leaf;				/* Last leaf-page */
} WT_VSTUFF;

static int __wt_verify_addfrag(WT_TOC *, uint32_t, uint32_t, WT_VSTUFF *);
static int __wt_verify_checkfrag(WT_TOC *, WT_VSTUFF *);
static int __wt_verify_freelist(WT_TOC *, WT_VSTUFF *);
static int __wt_verify_overflow_page(WT_TOC *, WT_PAGE *, WT_VSTUFF *);
static int __wt_verify_overflow(
		WT_TOC *, WT_OVFL *, uint32_t, uint32_t, WT_VSTUFF *);
static int __wt_verify_pc(WT_TOC *, WT_ROW_REF *, WT_PAGE *, int);
static int __wt_verify_tree(
		WT_TOC *, WT_ROW_REF *, WT_PAGE *, uint64_t, WT_VSTUFF *);

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
	BTREE *btree;
	WT_VSTUFF vstuff;
	int ret;

	env = toc->env;
	db = toc->db;
	btree = db->btree;
	ret = 0;

	WT_CLEAR(vstuff);
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
	vstuff.frags = WT_OFF_TO_ADDR(db, btree->fh->file_size);
	if (vstuff.frags > INT_MAX) {
		__wt_api_db_errx(db, "file is too large to verify");
		goto err;
	}
	WT_ERR(bit_alloc(env, vstuff.frags, &vstuff.fragbits));
	vstuff.evict_rec_gen = toc->env->ienv->cache->evict_rec_gen;
	vstuff.stream = stream;
	vstuff.f = f;

	/*
	 * The first sector of the file is the description record -- ignore
	 * it for now.
	 */
	bit_nset(vstuff.fragbits, 0, 0);

	/* Verify the tree, starting at the root. */
	WT_ERR(__wt_verify_tree(toc, NULL,
	    btree->root_page.page, (uint64_t)1,&vstuff));

	WT_ERR(__wt_verify_freelist(toc, &vstuff));

	WT_ERR(__wt_verify_checkfrag(toc, &vstuff));

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
	WT_ROW_REF *parent_rref,/* Internal key referencing this page, if any */
	WT_PAGE *page,		/* Page to verify */
	uint64_t parent_recno,	/* First record in this subtree */
	WT_VSTUFF *vs)		/* The verify package */
{
	DB *db;
	WT_COL_REF *cref;
	WT_PAGE_DISK *dsk;
	WT_REF *ref;
	WT_ROW_REF *rref;
	uint32_t i;
	int ret;

	db = toc->db;
	dsk = page->dsk;
	ret = 0;

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

	/* Report progress every 10 pages. */
	if (vs->f != NULL && ++vs->fcnt % 10 == 0)
		vs->f(toc->name, vs->fcnt);

	/* Update frags list. */
	WT_ERR(__wt_verify_addfrag(toc, page->addr, page->size, vs));

#ifdef HAVE_DIAGNOSTIC
	/* Optionally dump the page in debugging mode. */
	if (vs->stream != NULL)
		WT_ERR(__wt_debug_page(toc, page, NULL, vs->stream));
#endif

	/* Check the starting record number. */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		if (parent_recno != dsk->recno) {
			__wt_api_db_errx(db,
			    "page at addr %lu has a starting record of %llu "
			    "where the expected starting record was %llu",
			    (u_long)page->addr,
			    (unsigned long long)dsk->recno,
			    (unsigned long long)parent_recno);
			goto err;
		}
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
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_verify_overflow_page(toc, page, vs));
		break;
	}

	/* Check tree connections and recursively descend the tree. */
	switch (dsk->type) {
	case WT_PAGE_COL_INT:
		/* For each entry in an internal page, verify the subtree. */
		WT_COL_REF_FOREACH(page, cref, i) {
			/* cref references the subtree containing the record */
			ref = &cref->ref;
			switch (ret = __wt_page_in(toc, page, ref, 1)) {
			case 0:				/* Valid page */
				ret = __wt_verify_tree(
				    toc, NULL, ref->page, cref->recno, vs);
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
		if (parent_rref != NULL)
			WT_ERR(__wt_verify_pc(toc, parent_rref, page, 1));

		/* For each entry in an internal page, verify the subtree. */
		WT_ROW_REF_FOREACH(page, rref, i) {
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
				WT_ERR(__wt_verify_pc(toc, rref, vs->leaf, 0));
				__wt_hazard_clear(toc, vs->leaf);
				vs->leaf = NULL;
			}

			/* rref references the subtree containing the record */
			ref = &rref->ref;
			switch (ret = __wt_page_in(toc, page, ref, 1)) {
			case 0:				/* Valid page */
				ret = __wt_verify_tree(
				    toc, rref, ref->page, (uint64_t)0, vs);
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
__wt_verify_pc(WT_TOC *toc,
    WT_ROW_REF *parent_rref, WT_PAGE *child, int first_entry)
{
	DB *db;
	DBT *cd_ref, *pd_ref, *scratch1, *scratch2;
	WT_ROW *child_key;
	int cmp, ret, (*func)(DB *, const DBT *, const DBT *);

	db = toc->db;
	scratch1 = scratch2 = NULL;
	func = db->btree_compare;
	ret = 0;

	/*
	 * We're passed both row-store internal and leaf pages -- find the
	 * right WT_ROW_REF or WT_ROW structure; the first two fields of the
	 * structures are a void *data/uint32_t size pair.
	 */
	switch (child->dsk->type) {
	case WT_PAGE_ROW_INT:
		child_key = (WT_ROW *)(first_entry ? child->u.row_int.t :
		    child->u.row_int.t + (child->indx_count - 1));
		break;
	case WT_PAGE_ROW_LEAF:
		child_key = first_entry ? child->u.row_leaf.d :
		    child->u.row_leaf.d + (child->indx_count - 1);
		break;
	}

	/*
	 * The two keys we're going to compare may be overflow keys -- don't
	 * bother instantiating the keys in the tree, there's no reason to
	 * believe we're going to be doing real operations in this file.
	 */
	if (__wt_key_process(child_key)) {
		WT_ERR(__wt_scr_alloc(toc, 0, &scratch1));
		WT_ERR(__wt_item_process(toc, child_key->key, scratch1));
		cd_ref = scratch1;
	} else
		cd_ref = (DBT *)child_key;
	if (__wt_key_process(parent_rref)) {
		WT_ERR(__wt_scr_alloc(toc, 0, &scratch2));
		WT_RET(__wt_item_process(toc, parent_rref->key, scratch2));
		pd_ref = scratch2;
	} else
		pd_ref = (DBT *)parent_rref;

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
 * __wt_verify_overflow_page --
 *	Verify overflow items.
 */
static int
__wt_verify_overflow_page(WT_TOC *toc, WT_PAGE *page, WT_VSTUFF *vs)
{
	WT_ITEM *item;
	WT_PAGE_DISK *dsk;
	uint32_t entry_num, i;

	dsk = page->dsk;

	/*
	 * Overflow items aren't "in-memory", they're on-disk.  Ignore the fact
	 * they might have been updated, that doesn't mean anything until page
	 * reconciliation writes them to disk.
	 *
	 * Walk the disk page, verifying overflow items.
	 */
	entry_num = 0;
	WT_ITEM_FOREACH(dsk, item, i) {
		++entry_num;
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DATA_OVFL:
			WT_RET(__wt_verify_overflow(toc,
			    WT_ITEM_BYTE_OVFL(item),
			    entry_num, page->addr, vs));
		}
	}
	return (0);
}

/*
 * __wt_verify_overflow --
 *	Read in an overflow page and check it.
 */
static int
__wt_verify_overflow(WT_TOC *toc,
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
	WT_ERR(__wt_disk_read(toc, dsk, addr, size));

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
 * __wt_verify_freelist --
 *	Add the freelist fragments to the list of verified fragments.
 */
static int
__wt_verify_freelist(WT_TOC *toc, WT_VSTUFF *vs)
{
	BTREE *btree;
	ENV *env;
	WT_CACHE *cache;
	WT_FREE_ENTRY *fe;
	int ret;

	env = toc->env;
	btree = toc->db->btree;
	cache = env->ienv->cache;
	ret = 0;

	/*
	 * If the eviction thread ran during verification, these checks aren't
	 * possible.
	 */
	if (!WT_EVICT_CURRENT(toc, vs))
		return (0);

	/*
	 * Lock out the eviction thread -- we're going to read through the
	 * freelist, and that's owned and operated by the eviction thread.
	 */
	__wt_lock(env, cache->mtx_reconcile);
	TAILQ_FOREACH(fe, &btree->freeqa, qa)
		WT_TRET(__wt_verify_addfrag(toc, fe->addr, fe->size, vs));
	__wt_unlock(env, cache->mtx_reconcile);

	return (ret);
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

	/*
	 * If the eviction thread ran during verification, these checks aren't
	 * possible.
	 */
	if (!WT_EVICT_CURRENT(toc, vs))
		return (0);

	frags = WT_OFF_TO_ADDR(db, size);
	for (i = 0; i < frags; ++i)
		if (bit_test(vs->fragbits, addr + i)) {
			__wt_api_db_errx(db,
			    "file fragment at addr %lu already verified",
			    (u_long)addr);
			return (WT_ERROR);
		}
	bit_nset(vs->fragbits, addr, addr + (frags - 1));
	return (0);
}

/*
 * __wt_verify_checkfrag --
 *	Verify we've checked all the fragments in the file.
 */
static int
__wt_verify_checkfrag(WT_TOC *toc, WT_VSTUFF *vs)
{
	DB *db;
	int ffc, ffc_start, ffc_end, frags, ret;

	db = toc->db;
	frags = (int)vs->frags;		/* XXX: bitstring.h wants "ints" */
	ret = 0;

	/*
	 * If the eviction thread ran during verification, these checks aren't
	 * possible.
	 */
	if (!WT_EVICT_CURRENT(toc, vs))
		return (0);

	/* Check for file fragments we haven't verified. */
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
				    "file fragment %d was never verified",
				    ffc_start);
			else
				__wt_api_db_errx(db,
				    "file fragments %d-%d were never verified",
				    ffc_start, ffc_end);
			ret = WT_ERROR;
		}
		ffc_start = ffc_end = ffc;
		if (ffc == -1)
			break;
	}
	return (ret);
}
