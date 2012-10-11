/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

struct __rec_boundary;		typedef struct __rec_boundary WT_BOUNDARY;
struct __rec_dictionary;	typedef struct __rec_dictionary WT_DICTIONARY;
struct __rec_kv;		typedef struct __rec_kv WT_KV;

/*
 * Reconciliation is the process of taking an in-memory page, walking each entry
 * in the page, building a backing disk image in a temporary buffer representing
 * that information, and writing that buffer to disk.  What could be simpler?
 *
 * WT_RECONCILE --
 *	Information tracking a single page reconciliation.
 */
typedef struct {
	WT_PAGE *page;			/* Page being reconciled */

	WT_ITEM	 dsk;			/* Temporary disk-image buffer */

	/* Track whether all changes to the page are written. */
	uint32_t orig_write_gen;
	int upd_skipped;
	int upd_skip_fail;

	/*
	 * Track if reconciliation has seen any overflow items.  Leaf pages with
	 * no overflow items are special because we can delete them without
	 * reading them.  If a leaf page is reconciled and no overflow items are
	 * included, we set the parent page's address cell to a special type,
	 * leaf-no-overflow.  The code works on a per-page reconciliation basis,
	 * that is, once we see an overflow item, all subsequent leaf pages will
	 * not get the special cell type.  It would be possible to do better by
	 * tracking overflow items on split boundaries, but this is simply a
	 * a performance optimization for range deletes, I don't see an argument
	 * for optimizing for pages that split and contain chunks both with and
	 * without overflow items.
	 */
	int	ovfl_items;

	/*
	 * Reconciliation gets tricky if we have to split a page, that is, if
	 * the disk image we create exceeds the maximum size of disk images for
	 * this page type.  First, the split sizes: reconciliation splits to a
	 * smaller-than-maximum page size when a split is required so we don't
	 * repeatedly split a packed page.
	 */
	uint32_t page_size;		/* Maximum page size */
	uint32_t split_size;		/* Split page size */

	/*
	 * The problem with splits is we've done a lot of work by the time we
	 * realize we're going to have to split, we don't want to start over.
	 *
	 * To keep from having to start over when we hit the maximum page size,
	 * we track the page information when we approach a split boundary.
	 * If we eventually have to split, we walk this structure and pretend
	 * we were splitting all along.  After that, we continue to append to
	 * this structure, and eventually walk it to create a new internal page
	 * that references all of our split pages.
	 */
	struct __rec_boundary {
		/*
		 * The start field records location in the initial split buffer,
		 * that is, the first byte of the split chunk recorded before we
		 * decide to split a page; the offset between the first byte of
		 * chunk[0] and the first byte of chunk[1] is chunk[0]'s length.
		 *
		 * Once we split a page, we stop filling in the start field, as
		 * we're writing the split chunks as we find them.
		 */
		uint8_t *start;		/* Split's first byte */

		/*
		 * The recno and entries fields are the starting record number
		 * of the split chunk (for column-store splits), and the number
		 * of entries in the split chunk.  These fields are used both
		 * to write the split chunk, and to create a new internal page
		 * to reference the split pages.
		 */
		uint64_t recno;		/* Split's starting record */
		uint32_t entries;	/* Split's entries */

		WT_ADDR addr;		/* Split's written location */

		/*
		 * The key for a row-store page; no column-store key is needed
		 * because the page's recno, stored in the recno field, is the
		 * column-store key.
		 */
		WT_ITEM key;		/* Promoted row-store key */
	} *bnd;				/* Saved boundaries */
	uint32_t bnd_next;		/* Next boundary slot */
	uint32_t bnd_entries;		/* Total boundary slots */
	size_t   bnd_allocated;		/* Bytes allocated */

	/*
	 * We track the total number of page entries copied into split chunks
	 * so we can easily figure out how many entries in the current split
	 * chunk.
	 */
	uint32_t total_entries;		/* Total entries in splits */

	/*
	 * And there's state information as to where in this process we are:
	 * (1) tracking split boundaries because we can still fit more split
	 * chunks into the maximum page size, (2) tracking the maximum page
	 * size boundary because we can't fit any more split chunks into the
	 * maximum page size, (3) not performing boundary checks because it's
	 * either not useful with the current page size configuration, or
	 * because we've already been forced to split.
	 */
	enum {	SPLIT_BOUNDARY=0,	/* Next: a split page boundary */
		SPLIT_MAX=1,		/* Next: the maximum page boundary */
		SPLIT_TRACKING_OFF=2 }	/* No boundary checks */
	bnd_state;

	/*
	 * We track current information about the current record number, the
	 * number of entries copied into the temporary buffer, where we are
	 * in the temporary buffer, and how much memory remains.  Those items
	 * are packaged here rather than passing pointers to stack locations
	 * around the code.
	 */
	uint64_t recno;			/* Current record number */
	uint32_t entries;		/* Current number of entries */
	uint8_t *first_free;		/* Current first free byte */
	uint32_t space_avail;		/* Remaining space in this chunk */

	/*
	 * We don't need to keep the 0th key around on internal pages, the
	 * search code ignores them as nothing can sort less by definition.
	 * There's some trickiness here, see the code for comments on how
	 * these fields work.
	 */
	int	 cell_zero;		/* Row-store internal page 0th key */
	WT_REF	*merge_ref;		/* Row-store merge correction key */

	/*
	 * WT_DICTIONARY --
	 *	We optionally build a dictionary of row-store values for leaf
	 * pages.  Where two value cells are identical, only write the value
	 * once, the second and subsequent copies point to the original cell.
	 * The dictionary is fixed size, but organized in a skip-list to make
	 * searches faster.
	 */
	struct __rec_dictionary {
		uint64_t hash;				/* Hash value */
		void	*cell;				/* Matching cell */

		u_int depth;				/* Skiplist */
		WT_DICTIONARY *next[0];
	} **dictionary;					/* Dictionary */
	u_int dictionary_next, dictionary_slots;	/* Next, max entries */
							/* Skiplist head. */
	WT_DICTIONARY *dictionary_head[WT_SKIP_MAXDEPTH];

	/*
	 * WT_KV--
	 *	An on-page key/value item we're building.
	 */
	struct __rec_kv {
		WT_ITEM	 buf;		/* Data */
		WT_CELL	 cell;		/* Cell and cell's length */
		uint32_t cell_len;
		uint32_t len;		/* Total length of cell + data */
	} k, v;				/* Key/Value being built */

	WT_ITEM *cur, _cur;		/* Key/Value being built */
	WT_ITEM *last, _last;		/* Last key/value built */

	int key_pfx_compress;		/* If can prefix-compress next key */
	int key_pfx_compress_conf;	/* If prefix compression configured */
	int key_sfx_compress;		/* If can suffix-compress next key */
	int key_sfx_compress_conf;	/* If suffix compression configured */
} WT_RECONCILE;

static void __rec_cell_build_addr(
		WT_RECONCILE *, const void *, uint32_t, u_int, uint64_t);
static int  __rec_cell_build_key(WT_SESSION_IMPL *,
		WT_RECONCILE *, const void *, uint32_t, int, int *);
static int  __rec_cell_build_ovfl(WT_SESSION_IMPL *,
		WT_RECONCILE *, WT_KV *, uint8_t, uint64_t);
static int  __rec_cell_build_val(WT_SESSION_IMPL *,
		WT_RECONCILE *, const void *, uint32_t, uint64_t);
static int  __rec_col_fix(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_col_fix_slvg(WT_SESSION_IMPL *,
		WT_RECONCILE *, WT_PAGE *, WT_SALVAGE_COOKIE *);
static int  __rec_col_int(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_col_merge(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_col_var(WT_SESSION_IMPL *,
		WT_RECONCILE *, WT_PAGE *, WT_SALVAGE_COOKIE *);
static int  __rec_col_var_helper(WT_SESSION_IMPL *, WT_RECONCILE *,
		WT_SALVAGE_COOKIE *, WT_ITEM *, int, int, uint64_t);
static int  __rec_page_deleted(
		WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *, WT_REF *, int *);
static int  __rec_page_modified(
		WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *, WT_REF *, int *);
static int  __rec_row_int(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_row_leaf(WT_SESSION_IMPL *,
		WT_RECONCILE *, WT_PAGE *, WT_SALVAGE_COOKIE *);
static int  __rec_row_leaf_insert(
		WT_SESSION_IMPL *, WT_RECONCILE *, WT_INSERT *);
static int  __rec_row_merge(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_split(WT_SESSION_IMPL *session, WT_RECONCILE *);
static int  __rec_split_col(
		WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *, WT_PAGE **);
static int  __rec_split_discard(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_split_finish(WT_SESSION_IMPL *, WT_RECONCILE *);
static int  __rec_split_fixup(WT_SESSION_IMPL *, WT_RECONCILE *);
static int  __rec_split_init(WT_SESSION_IMPL *,
		WT_RECONCILE *, WT_PAGE *, uint64_t, uint32_t);
static int  __rec_split_row(
		WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *, WT_PAGE **);
static int  __rec_split_row_promote(
		WT_SESSION_IMPL *, WT_RECONCILE *, uint8_t);
static int  __rec_split_write(WT_SESSION_IMPL *,
		WT_RECONCILE *, WT_BOUNDARY *, WT_ITEM *, int);
static int  __rec_write_init(WT_SESSION_IMPL *, WT_PAGE *, uint32_t, void *);
static int  __rec_write_wrapup(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_write_wrapup_err(
		WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);

static void __rec_dictionary_free(WT_SESSION_IMPL *, WT_RECONCILE *);
static int  __rec_dictionary_init(WT_SESSION_IMPL *, WT_RECONCILE *);
static int  __rec_dictionary_lookup(
		WT_SESSION_IMPL *, WT_RECONCILE *, WT_KV *, WT_DICTIONARY **);
static void __rec_dictionary_reset(WT_RECONCILE *);

/*
 * __rec_page_modified --
 *	Return if the given WT_REF references any modifications.
 *
 * The reconciliation code is used in the following situations:
 *
 * (1) by the eviction server during sync;
 * (2) by the eviction server during forced eviction of a page; and
 * (3) by any thread during LRU eviction.
 *
 * The complexity is checking the page state of child pages when looking for
 * pages to merge.
 *
 * We clearly want to consider all normal, in-memory pages (WT_REF_MEM).
 *
 * While we are processing a sync request in case (1), pages in the file may be
 * awaiting forced eviction (WT_REF_EVICT_FORCE).  Those pages must be included
 * in the sync, otherwise it will be incomplete.
 *
 * During LRU eviction in case (3), the eviction code has already locked the
 * subtree, so locked pages should be included in the merge (WT_REF_LOCKED).
 *
 * To make this tractable, the eviction server guarantees that no thread is
 * doing LRU eviction in the tree when cases (1) and (2) occur.  That is, the
 * only state change that can occur during a sync or forced eviction is for a
 * reference to a page on disk to cause a page to be read (WT_REF_READING).
 * In the case of a read, we could safely ignore those pages because they are
 * unmodified by definition -- they are being read from disk, however, in the
 * current system, that state also includes fast-delete pages that are being
 * instantiated.  Those pages cannot be ignored, as they have been modified.
 * For this reason, we have to wait for the WT_REF_READING state to be resolved
 * to another state before we proceed.
 */
static int
__rec_page_modified(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, WT_PAGE *page, WT_REF *ref, int *modifyp)
{
	WT_DECL_RET;

	*modifyp = 0;
	for (;; __wt_yield())
		switch (ref->state) {
		case WT_REF_DISK:
			/* On disk, not modified by definition. */
			return (0);
		case WT_REF_DELETED:
			/*
			 * The WT_REF entry is in a deleted state.
			 *
			 * It's possible the state is changing underneath us and
			 * we can race between checking for a deleted state and
			 * looking at the stored transaction ID to see if the
			 * delete is visible to us.  Lock down the structure.
			 */
			if (!WT_ATOMIC_CAS(
			    ref->state, WT_REF_DELETED, WT_REF_READING))
				break;
			ret =
			    __rec_page_deleted(session, r, page, ref, modifyp);
			WT_PUBLISH(ref->state, WT_REF_DELETED);
			return (ret);
		case WT_REF_EVICT_FORCE:
		case WT_REF_EVICT_WALK:
		case WT_REF_LOCKED:
		case WT_REF_MEM:
			/*
			 * In-memory states: set modify based on the existence
			 * of the page's modify structure.
			 */
			if (ref->page->modify != NULL)
				*modifyp = 1;
			return (0);
		case WT_REF_READING:
			/*
			 * Being read or in fast-delete, wait for the page's
			 * state to settle.
			 */
			 break;
		WT_ILLEGAL_VALUE(session);
		}
	/* NOTREACHED */
}

/*
 * __rec_page_deleted --
 *	Handle pages with leaf pages in the WT_REF_DELETED state.
 */
static int
__rec_page_deleted(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, WT_PAGE *page, WT_REF *ref, int *modifyp)
{
	*modifyp = 0;

	/*
	 * Internal pages with child leaf pages in the WT_REF_DELETED state are
	 * a special case during reconciliation.  First, if the deletion isn't
	 * visible, we proceed as with any change that's not visible: set the
	 * skipped flag and ignore the change for the purposes of writing the
	 * internal page.
	 */
	if (!__wt_txn_visible(session, ref->txnid)) {
		r->upd_skipped = 1;
		return (0);
	}

	/* The deletion is visible, set the modified return. */
	*modifyp = 1;

	/*
	 * If the deletion is visible, check for any transactions in the system
	 * that might want to see the page's state before the deletion.
	 *
	 * If any such transactions exist, we cannot discard the underlying leaf
	 * page to the block manager because the transaction may eventually read
	 * it.  However, this write might be part of a checkpoint, and should we
	 * recover to that checkpoint, we'll need to delete the leaf page, else
	 * we'd leak it.  The solution is to write a proxy cell on the internal
	 * page ensuring the leaf page is eventually discarded.
	 *
	 * If no such transactions exist, we can discard the leaf page to the
	 * block manager, and no cell needs to be written at all.  We set the
	 * WT_REF.addr field to NULL for a few reasons: (1) we can avoid doing
	 * the free on the next reconciliation (that's only performance, as the
	 * underlying tracking routines won't free the same block twice), (2)
	 * our caller knows a WT_REF.addr of NULL means we skip the cell when
	 * writing the page, and (3) the cache read routine knows a WT_REF.addr
	 * of NULL means the underlying page is gone and it has to instantiate
	 * a new page.  Note #2 and #3 are safe: the WT_REF.addr field is never
	 * reset once cleared, so it's safe to test it outside of the WT_REF
	 * structure lock.
	 *
	 * One final note: if the WT_REF transaction ID is set to WT_TXN_NONE,
	 * it means this WT_REF is the re-creation of a deleted node (we wrote
	 * out the deleted node after the deletion became visible, but before
	 * we could delete the leaf page, and subsequently crashed, then read
	 * the page and re-created the WT_REF_DELETED state).   In other words,
	 * the delete is visible to all (it became visible), and by definition
	 * there are no older transactions needing to see previous versions of
	 * the page.
	 */
	if (ref->addr != NULL &&
	    (ref->txnid == WT_TXN_NONE ||
	    __wt_txn_visible_all(session, ref->txnid))) {
		/*
		 * Free the page when reconciliation completes and ensure we
		 * only free the page once.
		 */
		WT_RET(__wt_rec_track_onpage_ref(session, page, page, ref));
		ref->addr = NULL;
	}

	return (0);
}

/*
 * __rec_txn_read --
 *	Helper for transactional reads: fail fast if skipping updates.
 */
static inline int
__rec_txn_read(
    WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_UPDATE *upd, WT_UPDATE **updp)
{
	*updp = __wt_txn_read_skip(session, upd, &r->upd_skipped);
	return ((r->upd_skip_fail && r->upd_skipped) ? EBUSY : 0);
}

/*
 * __wt_rec_write --
 *	Reconcile an in-memory page into its on-disk format, and write it.
 */
int
__wt_rec_write(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_SALVAGE_COOKIE *salvage, uint32_t flags)
{
	WT_RECONCILE *r;
	WT_DECL_RET;

	WT_VERBOSE_RET(session, reconcile,
	    "page %p %s", page, __wt_page_type_string(page->type));

	WT_BSTAT_INCR(session, rec_written);

	/* We're shouldn't get called with a clean page, that's an error. */
	WT_ASSERT(session, __wt_page_is_modified(page));

	/*
	 * We can't do anything with a split-merge page, it must be merged into
	 * its parent.
	 */
	if (F_ISSET(page->modify, WT_PM_REC_SPLIT_MERGE))
		return (0);

	/* Initialize the reconciliation structure for each new run. */
	WT_RET(__rec_write_init(session, page, flags, &session->reconcile));
	r = session->reconcile;

	/* Initialize the tracking subsystem for each new run. */
	WT_RET(__wt_rec_track_init(session, page));

	/* Reconcile the page. */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		if (salvage != NULL)
			ret = __rec_col_fix_slvg(session, r, page, salvage);
		else
			ret = __rec_col_fix(session, r, page);
		break;
	case WT_PAGE_COL_INT:
		ret = __rec_col_int(session, r, page);
		break;
	case WT_PAGE_COL_VAR:
		ret = __rec_col_var(session, r, page, salvage);
		break;
	case WT_PAGE_ROW_INT:
		ret = __rec_row_int(session, r, page);
		break;
	case WT_PAGE_ROW_LEAF:
		ret = __rec_row_leaf(session, r, page, salvage);
		break;
	WT_ILLEGAL_VALUE(session);
	}
	if (ret != 0) {
		/*
		 * The underlying wrapup-on-error functions can fail, and they
		 * are written to return an error value, but now we discard it,
		 * we already have one.
		 */
		(void)__rec_write_wrapup_err(session, r, page);
		return (ret);
	}

	/* Wrap up the page's reconciliation. */
	WT_RET(__rec_write_wrapup(session, r, page));

	/*
	 * If this page has a parent, mark the parent dirty.  Split-merge pages
	 * are a special case: they are always dirty and never reconciled, they
	 * are always merged into their parent.  For that reason, we mark the
	 * first non-split-merge parent we find dirty, not the split-merge page
	 * itself, ensuring the chain of dirty pages up the tree isn't broken.
	 */
	if (!WT_PAGE_IS_ROOT(page)) {
		for (;;) {
			page = page->parent;
			if (page->modify == NULL ||
			    !F_ISSET(page->modify, WT_PM_REC_SPLIT_MERGE))
				break;
		}
		WT_RET(__wt_page_modify_init(session, page));
		__wt_page_modify_set(page);

		return (0);
	}

	/*
	 * Root pages are trickier.  First, if the page is empty or we performed
	 * a 1-for-1 page swap, we're done, we've written the root (and done the
	 * checkpoint).
	 */
	switch (F_ISSET(page->modify, WT_PM_REC_MASK)) {
	case WT_PM_REC_EMPTY:				/* Page is empty */
	case WT_PM_REC_REPLACE: 			/* 1-for-1 page swap */
		return (0);
	case WT_PM_REC_SPLIT:				/* Page split */
		break;
	WT_ILLEGAL_VALUE(session);
	}

	/*
	 * Newly created internal pages are normally merged into their parent
	 * when the parent is evicted.  Newly split root pages can't be merged,
	 * they have no parent and the new root page must be written.  We also
	 * have to write the root page immediately; the alternative would be to
	 * split the page in memory and continue, but that won't work because
	 * (1) we'd have to require incoming threads use hazard references to
	 * read the root page, and (2) the sync or close triggering the split
	 * won't see the new root page during the current traversal.
	 *
	 * Make the new split page look like a normal page that's been modified,
	 * and write it out.  Keep doing that and eventually we'll perform a
	 * simple replacement (as opposed to another level of split), and then
	 * we're done.  Given our support of big pages, the only time we see
	 * multiple splits is when we've bulk-loaded something huge, and we're
	 * evicting the index page referencing all of those leaf pages.
	 *
	 * This creates a new kind of data structure in the system: an in-memory
	 * root page, pointing to a chain of pages, each of which are flagged as
	 * "split" pages, up to a final replacement page.  We don't use those
	 * pages again, they are discarded in the next root page reconciliation.
	 * We could discard them immediately (as the checkpoint is complete, any
	 * pages we discard go on the next checkpoint's free list, it's safe to
	 * do), but the code is simpler this way, and this operation should not
	 * be common.
	 */
	WT_VERBOSE_RET(session, reconcile,
	    "root page split %p -> %p", page, page->modify->u.split);
	page = page->modify->u.split;
	__wt_page_modify_set(page);
	F_CLR(page->modify, WT_PM_REC_SPLIT_MERGE);

	WT_RET(__wt_rec_write(session, page, NULL, flags));

	return (0);
}

/*
 * __rec_write_init --
 *	Initialize the reconciliation structure.
 */
static int
__rec_write_init(
    WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags, void *retp)
{
	WT_BTREE *btree;
	WT_RECONCILE *r;

	btree = session->btree;

	/* Allocate a reconciliation structure if we don't already have one. */
	if ((r = *(WT_RECONCILE **)retp) == NULL) {
		WT_RET(__wt_calloc_def(session, 1, &r));
		*(WT_RECONCILE **)retp = r;

		/* Connect prefix compression pointers/buffers. */
		r->cur = &r->_cur;
		r->last = &r->_last;

		/* Disk buffers may need to be aligned. */
		F_SET(&r->dsk, WT_ITEM_ALIGNED);

	}

	/*
	 * Suffix compression is a hack to shorten internal page keys
	 * by discarding trailing bytes that aren't necessary for tree
	 * navigation.  We don't do suffix compression if there is a
	 * custom collator because we don't know what bytes a custom
	 * collator might use.  Some custom collators (for example, a
	 * collator implementing reverse ordering of strings), won't
	 * have any problem with suffix compression: if there's ever a
	 * reason to implement suffix compression for custom collators,
	 * we can add a setting to the collator, configured when the
	 * collator is added, that turns on suffix compression.
	 */
	r->key_sfx_compress_conf = 0;
	if (btree->collator == NULL && btree->internal_key_truncate)
		r->key_sfx_compress_conf = 1;

	/* Prefix compression discards key's repeated prefix bytes. */
	r->key_pfx_compress_conf = 0;
	if (btree->prefix_compression)
		r->key_pfx_compress_conf = 1;

	/*
	 * Dictionary compression only writes repeated values once.  We grow
	 * the dictionary as necessary, always using the largest size we've
	 * seen.
	 *
	 * Per-page reconciliation: reset the dictionary.
	 */
	if (btree->dictionary != 0 && btree->dictionary > r->dictionary_slots) {
		/*
		 * Sanity check the size: 100 slots is the smallest dictionary
		 * we use.
		 */
		r->dictionary_slots =
		    btree->dictionary < 100 ? 100 : btree->dictionary;

		WT_RET(__rec_dictionary_init(session, r));
	}
	if (btree->dictionary)
		__rec_dictionary_reset(r);

	/* Per-page reconciliation: track skipped updates. */
	r->upd_skipped = 0;
	r->upd_skip_fail = LF_ISSET(WT_REC_SINGLE) ? 0 : 1;

	/* Per-page reconciliation: track overflow items. */
	r->ovfl_items = 0;

	/* Read the disk generation before we read anything from the page. */
	r->page = page;
	WT_ORDERED_READ(r->orig_write_gen, page->modify->write_gen);

	return (0);
}

/*
 * __rec_destroy --
 *	Clean up the reconciliation structure.
 */
void
__wt_rec_destroy(WT_SESSION_IMPL *session, void *retp)
{
	WT_BOUNDARY *bnd;
	WT_RECONCILE *r;
	uint32_t i;

	if ((r = *(WT_RECONCILE **)retp) == NULL)
		return;

	__wt_buf_free(session, &r->dsk);

	if (r->bnd != NULL) {
		for (bnd = r->bnd, i = 0; i < r->bnd_entries; ++bnd, ++i) {
			__wt_free(session, bnd->addr.addr);
			__wt_buf_free(session, &bnd->key);
		}
		__wt_free(session, r->bnd);
	}

	__wt_buf_free(session, &r->k.buf);
	__wt_buf_free(session, &r->v.buf);
	__wt_buf_free(session, &r->_cur);
	__wt_buf_free(session, &r->_last);

	__rec_dictionary_free(session, r);

	__wt_free(session, r);
	*(WT_RECONCILE **)retp = NULL;
}

/*
 * __rec_incr --
 *	Update the memory tracking structure for a set of new entries.
 */
static inline void
__rec_incr(WT_SESSION_IMPL *session, WT_RECONCILE *r, uint32_t v, uint32_t size)
{
	/*
	 * The buffer code is fragile and prone to off-by-one errors -- check
	 * for overflow in diagnostic mode.
	 */
	WT_ASSERT(session, r->space_avail >= size);
	WT_ASSERT(session,
	    WT_BLOCK_FITS(r->first_free, size, r->dsk.mem, r->page_size));

	r->entries += v;
	r->space_avail -= size;
	r->first_free += size;
}

/*
 * __rec_copy_incr --
 *	Copy a key/value cell and buffer pair into the new image.
 */
static inline void
__rec_copy_incr(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_KV *kv)
{
	uint32_t len;
	uint8_t *p, *t;

	/*
	 * If there's only one chunk of data to copy (because the cell and data
	 * are being copied from the original disk page), the cell length won't
	 * be set, the WT_ITEM data/length will reference the data to be copied.
	 *
	 * WT_CELLs are typically small, 1 or 2 bytes -- don't call memcpy, do
	 * the copy in-line.
	 */
	for (p = (uint8_t *)r->first_free,
	    t = (uint8_t *)&kv->cell, len = kv->cell_len; len > 0; --len)
		*p++ = *t++;

	/* The data can be quite large -- call memcpy. */
	if (kv->buf.size != 0)
		memcpy(p, kv->buf.data, kv->buf.size);

	WT_ASSERT(session, kv->len == kv->cell_len + kv->buf.size);
	__rec_incr(session, r, 1, kv->len);
}

/*
 * __rec_dict_replace --
 *	Check for a dictionary match.
 */
static int
__rec_dict_replace(
    WT_SESSION_IMPL *session, WT_RECONCILE *r, uint64_t rle, WT_KV *kv)
{
	WT_DICTIONARY *dp;
	uint64_t offset;

	/*
	 * We optionally create a dictionary of values and only write a unique
	 * value once per page, using a special "copy" cell for all subsequent
	 * copies of the value.  We have to do the cell build and resolution at
	 * this low level because we need physical cell offsets for the page.
	 *
	 * Sanity check: short-data cells can be smaller than dictionary-copy
	 * cells.  If the data is already small, don't bother doing the work.
	 * This isn't just work avoidance: on-page cells can't grow as a result
	 * of writing a dictionary-copy cell, the reconciliation functions do a
	 * split-boundary test based on the size required by the value's cell;
	 * if we grow the cell after that test we'll potentially write off the
	 * end of the buffer's memory.
	 */
	if (kv->buf.size <= WT_INTPACK32_MAXSIZE)
		return (0);
	WT_RET(__rec_dictionary_lookup(session, r, kv, &dp));
	if (dp == NULL)
		return (0);

	/*
	 * If the dictionary cell reference is not set, we're creating a new
	 * entry in the dictionary, update it's location.
	 *
	 * If the dictionary cell reference is set, we have a matching value.
	 * Create a copy cell instead.
	 */
	if (dp->cell == NULL)
		dp->cell = r->first_free;
	else {
		offset = WT_PTRDIFF32(r->first_free, dp->cell);
		kv->len = kv->cell_len =
		   __wt_cell_pack_copy(&kv->cell, rle, offset);
		kv->buf.data = NULL;
		kv->buf.size = 0;
	}
	return (0);
}

/*
 * __rec_key_state_update --
 *	Update prefix and suffix compression based on the last key.
 */
static inline void
__rec_key_state_update(WT_RECONCILE *r, int ovfl_key)
{
	WT_ITEM *a;

	/*
	 * If writing an overflow key onto the page, don't update the "last key"
	 * value, and leave the state of prefix compression alone.  (If we are
	 * currently doing prefix compression, we have a key state which will
	 * continue to work, we're just skipping the key just created because
	 * it's an overflow key and doesn't participate in prefix compression.
	 * If we are not currently doing prefix compression, we can't start, an
	 * overflow key doesn't give us any state.)
	 *
	 * Additionally, if we wrote an overflow key onto the page, turn off the
	 * suffix compression of row-store internal node keys.  (When we split,
	 * "last key" is the largest key on the previous page, and "cur key" is
	 * the first key on the next page, which is being promoted.  In some
	 * cases we can discard bytes from the "cur key" that are not needed to
	 * distinguish between the "last key" and "cur key", compressing the
	 * size of keys on internal nodes.  If we just built an overflow key,
	 * we're not going to update the "last key", making suffix compression
	 * impossible for the next key.   Alternatively, we could remember where
	 * the last key was on the page, detect it's an overflow key, read it
	 * from disk and do suffix compression, but that's too much work for an
	 * unlikely event.)
	 *
	 * If we're not writing an overflow key on the page, update the last-key
	 * value and turn on both prefix and suffix compression.
	 */
	if (ovfl_key)
		r->key_sfx_compress = 0;
	else {
		a = r->cur;
		r->cur = r->last;
		r->last = a;

		r->key_pfx_compress = r->key_pfx_compress_conf;
		r->key_sfx_compress = r->key_sfx_compress_conf;
	}
}

/*
 * __rec_split_bnd_grow --
 *	Grow the boundary array as necessary.
 */
static int
__rec_split_bnd_grow(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
	/*
	 * Make sure there's enough room in which to save another boundary.
	 *
	 * The calculation is actually +1, because we save the start point one
	 * past the current entry -- make it +20 so we don't grow slot-by-slot.
	 */
	if (r->bnd_next + 1 >= r->bnd_entries) {
		WT_RET(__wt_realloc(session, &r->bnd_allocated,
		    (r->bnd_entries + 20) * sizeof(*r->bnd), &r->bnd));
		r->bnd_entries += 20;
	}
	return (0);
}

/*
 * __rec_split_init --
 *	Initialization for the reconciliation split functions.
 */
static int
__rec_split_init(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, WT_PAGE *page, uint64_t recno, uint32_t max)
{
	WT_BTREE *btree;
	WT_PAGE_HEADER *dsk;

	btree = session->btree;

	/* Ensure the scratch buffer is large enough. */
	WT_RET(__wt_bm_write_size(session, &max));
	WT_RET(__wt_buf_initsize(session, &r->dsk, (size_t)max));

	/*
	 * Clear the header and set the page type (the type doesn't change, and
	 * setting it later requires additional code in a few different places).
	 */
	dsk = r->dsk.mem;
	memset(dsk, 0, WT_PAGE_HEADER_SIZE);
	dsk->type = page->type;

	/*
	 * If we have to split, we want to choose a smaller page size for the
	 * split pages, because otherwise we could end up splitting one large
	 * packed page over and over.   We don't want to pick the minimum size
	 * either, because that penalizes an application that did a bulk load
	 * and subsequently inserted a few items into packed pages.  Currently,
	 * I'm using 75%, but I have no empirical evidence that's a good value.
	 * We should leave this as a tuning variable, but probably undocumented.
	 *
	 * The maximum page size may be a multiple of the split page size (for
	 * example, there's a maximum page size of 128KB, but because the table
	 * is active and we don't want to split a lot, the split size is 20KB).
	 * The maximum page size may NOT be an exact multiple of the split page
	 * size.
	 *
	 * It's lots of work to build these pages and don't want to start over
	 * when we reach the maximum page size (it's painful to restart after
	 * creating overflow items and compacted data, for example, as those
	 * items have already been written to disk).  So, the loop calls the
	 * helper functions when approaching a split boundary, and we save the
	 * information at that point.  That allows us to go back and split the
	 * page at the boundary points if we eventually overflow the maximum
	 * page size.
	 *
	 * Finally, fixed-size column-store pages can split under (very) rare
	 * circumstances, but they're usually allocated at a fixed page size,
	 * never anything smaller.
	 */
	r->page_size = max;
	r->split_size = page->type == WT_PAGE_COL_FIX ?
	    max :
	    WT_SPLIT_PAGE_SIZE(max, btree->allocsize, btree->split_pct);

	/*
	 * If the maximum page size is the same as the split page size, there
	 * is no need to maintain split boundaries within a larger page.
	 */
	r->bnd_state =
	    max == r->split_size ? SPLIT_TRACKING_OFF : SPLIT_BOUNDARY;

	/*
	 * Initialize the array of boundary items and set the initial record
	 * number and buffer address.
	 */
	r->bnd_next = 0;
	WT_RET(__rec_split_bnd_grow(session, r));
	r->bnd[0].recno = recno;
	r->bnd[0].start = WT_PAGE_HEADER_BYTE(btree, dsk);

	/* Initialize the total entries. */
	r->total_entries = 0;

	/*
	 * Set the caller's information and configure so the loop calls us
	 * when approaching the split boundary.
	 */
	r->recno = recno;
	r->entries = 0;
	r->first_free = WT_PAGE_HEADER_BYTE(btree, dsk);
	r->space_avail = r->split_size - WT_PAGE_HEADER_BYTE_SIZE(btree);

	/* New page, compression off. */
	r->key_pfx_compress = r->key_sfx_compress = 0;

	return (0);
}

/*
 * __rec_split --
 *	Handle the page reconciliation bookkeeping.  (Did you know "bookkeeper"
 * has 3 doubled letters in a row?  Sweet-tooth does, too.)
 */
static int
__rec_split(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
	WT_BTREE *btree;
	WT_BOUNDARY *bnd;
	WT_PAGE_HEADER *dsk;
	uint32_t current_len;

	/*
	 * Handle page-buffer size tracking; we have to do this work in every
	 * reconciliation loop, and I don't want to repeat the code that many
	 * times.
	 */
	btree = session->btree;
	dsk = r->dsk.mem;

	/* Hitting a page boundary resets the dictionary, in all cases. */
	if (btree->dictionary)
		__rec_dictionary_reset(r);

	/*
	 * There are 3 cases we have to handle.
	 *
	 * #1
	 * Not done, and about to cross a split boundary, in which case we save
	 * away the current boundary information and return.
	 *
	 * #2
	 * Not done, and about to cross the max boundary, in which case we have
	 * to physically split the page -- use the saved split information to
	 * write all the split pages.
	 *
	 * #3
	 * Not done, and about to cross the split boundary, but we've already
	 * done the split thing when we approached the max boundary, in which
	 * case we write the page and keep going.
	 *
	 * Cases #1 and #2 are the hard ones: we're called when we're about to
	 * cross each split boundary, and we save information away so we can
	 * split if we have to.  We're also called when we're about to cross
	 * the maximum page boundary: in that case, we do the actual split,
	 * clean things up, then keep going.
	 */
	switch (r->bnd_state) {
	case SPLIT_BOUNDARY:				/* Case #1 */
		/*
		 * Save the information about where we are when the split would
		 * have happened.
		 */
		WT_RET(__rec_split_bnd_grow(session, r));
		bnd = &r->bnd[r->bnd_next++];

		/* Set the number of entries for the just finished chunk. */
		bnd->entries = r->entries - r->total_entries;
		r->total_entries = r->entries;

		/*
		 * Set the starting record number, buffer address and promotion
		 * key for the next chunk, clear the entries (not required, but
		 * cleaner).
		 */
		++bnd;
		bnd->recno = r->recno;
		bnd->start = r->first_free;
		if (dsk->type == WT_PAGE_ROW_INT ||
		    dsk->type == WT_PAGE_ROW_LEAF)
			WT_RET(__rec_split_row_promote(session, r, dsk->type));
		bnd->entries = 0;

		/*
		 * Set the space available to another split-size chunk, if we
		 * have one.  If we don't have room for another split chunk,
		 * add whatever space remains in the maximum page size, and
		 * hope it's enough.
		 */
		current_len = WT_PTRDIFF32(r->first_free, dsk);
		if (current_len + r->split_size <= r->page_size)
			r->space_avail =
			    r->split_size - WT_PAGE_HEADER_BYTE_SIZE(btree);
		else {
			r->bnd_state = SPLIT_MAX;
			r->space_avail = (r->page_size -
			    WT_PAGE_HEADER_BYTE_SIZE(btree)) - current_len;
		}
		break;
	case SPLIT_MAX:					/* Case #2 */
		/*
		 * It didn't all fit into a single page.
		 *
		 * Cycle through the saved split-point information, writing the
		 * split chunks we have tracked.
		 */
		WT_RET(__rec_split_fixup(session, r));

		/* We're done saving split chunks. */
		r->bnd_state = SPLIT_TRACKING_OFF;
		break;
	case SPLIT_TRACKING_OFF:			/* Case #3 */
		WT_RET(__rec_split_bnd_grow(session, r));
		bnd = &r->bnd[r->bnd_next++];

		/*
		 * It didn't all fit, but either we've already noticed it and
		 * are now processing the rest of the page at the split-size
		 * boundaries, or the split size was the same as the page size,
		 * so we never bothered with saving split-point information.
		 *
		 * Finalize the header information and write the page.
		 */
		dsk->recno = bnd->recno;
		dsk->u.entries = r->entries;
		r->dsk.size = WT_PTRDIFF32(r->first_free, dsk);
		WT_RET(__rec_split_write(session, r, bnd, &r->dsk, 0));

		/*
		 * Set the starting record number and promotion key for the next
		 * chunk, clear the entries (not required, but cleaner).
		 */
		++bnd;
		bnd->recno = r->recno;
		if (dsk->type == WT_PAGE_ROW_INT ||
		    dsk->type == WT_PAGE_ROW_LEAF)
			WT_RET(__rec_split_row_promote(session, r, dsk->type));
		bnd->entries = 0;

		/*
		 * Set the caller's entry count and buffer information for the
		 * next chunk.  We only get here if we're not splitting or have
		 * already split, so it's split-size chunks from here on out.
		 */
		r->entries = 0;
		r->first_free = WT_PAGE_HEADER_BYTE(btree, dsk);
		r->space_avail =
		    r->split_size - WT_PAGE_HEADER_BYTE_SIZE(btree);
		break;
	}
	return (0);
}

/*
 * __rec_split_finish --
 *	Finish processing a split page.
 */
static int
__rec_split_finish(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
	WT_BOUNDARY *bnd;
	WT_PAGE_HEADER *dsk;
	int checkpoint;

	/*
	 * We're done reconciling a page.
	 *
	 * First, we only arrive here with no entries to write if the page was
	 * entirely empty (if the page wasn't empty, the only reason to split,
	 * resetting entries to 0, is because there's another entry to write,
	 * which then sets entries to 1).  If the page was empty, we eventually
	 * delete it.
	 */
	if (r->entries == 0) {
		WT_ASSERT_RET(session, r->bnd_next == 0);
		return (0);
	}

	/*
	 * Second, check our split status:
	 *
	 * If we have already split, put the remaining data in the next boundary
	 * slot.
	 *
	 * If we have not yet split, the reconciled page fit into a maximum page
	 * size, all of our boundary checking was wasted.   Change the first
	 * boundary slot to represent the full page (the first boundary slot is
	 * largely correct, just update the number of entries).
	 */
	if (r->bnd_state == SPLIT_TRACKING_OFF) {
		WT_RET(__rec_split_bnd_grow(session, r));
		bnd = &r->bnd[r->bnd_next++];
	} else {
		r->bnd_next = 1;
		bnd = &r->bnd[0];
		bnd->entries = r->entries;
	}

	/*
	 * Third, check to see if we're creating a checkpoint: any time we write
	 * the root page of the tree, we tell the underlying block manager so it
	 * can write and return any additional information checkpoints require.
	 */
	checkpoint = r->bnd_next == 1 && WT_PAGE_IS_ROOT(r->page);

	/* Finalize the header information and write the page. */
	dsk = r->dsk.mem;
	dsk->recno = bnd->recno;
	dsk->u.entries = r->entries;
	r->dsk.size = WT_PTRDIFF32(r->first_free, dsk);
	return (__rec_split_write(session, r, bnd, &r->dsk, checkpoint));
}

/*
 * __rec_split_fixup --
 *	Fix up after crossing the maximum page boundary.
 */
static int
__rec_split_fixup(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
	WT_BOUNDARY *bnd;
	WT_BTREE *btree;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_PAGE_HEADER *dsk;
	uint32_t i, len;
	uint8_t *dsk_start;

	/*
	 * When we overflow physical limits of the page, we walk the list of
	 * split chunks we've created and write those pages out, then update
	 * the caller's information.
	 */
	btree = session->btree;

	/*
	 * The data isn't laid out on a page boundary or nul padded; copy it to
	 * a clean, aligned, padded buffer before writing it.
	 *
	 * Allocate a scratch buffer to hold the new disk image.  Copy the
	 * WT_PAGE_HEADER header onto the scratch buffer, most of the header
	 * information remains unchanged between the pages.
	 */
	WT_RET(__wt_scr_alloc(session, r->split_size, &tmp));
	dsk = tmp->mem;
	memcpy(dsk, r->dsk.mem, WT_PAGE_HEADER_SIZE);

	/*
	 * For each split chunk we've created, update the disk image and copy
	 * it into place.
	 */
	dsk_start = WT_PAGE_HEADER_BYTE(btree, dsk);
	for (i = 0, bnd = r->bnd; i < r->bnd_next; ++i, ++bnd) {
		/* Copy the page contents to the temporary buffer. */
		len = WT_PTRDIFF32((bnd + 1)->start, bnd->start);
		memcpy(dsk_start, bnd->start, len);

		/* Finalize the header information and write the page. */
		dsk->recno = bnd->recno;
		dsk->u.entries = bnd->entries;
		tmp->size = WT_PAGE_HEADER_BYTE_SIZE(btree) + len;
		WT_ERR(__rec_split_write(session, r, bnd, tmp, 0));
	}

	/*
	 * There is probably a remnant in the working buffer that didn't get
	 * written; copy it down to the beginning of the working buffer, and
	 * update the starting record number.
	 *
	 * Confirm the remnant is no larger than the available split buffer.
	 *
	 * Fix up our caller's information.
	 */
	len = WT_PTRDIFF32(r->first_free, bnd->start);
	WT_ASSERT_ERR(
	    session, len < r->split_size - WT_PAGE_HEADER_BYTE_SIZE(btree));

	dsk = r->dsk.mem;
	dsk_start = WT_PAGE_HEADER_BYTE(btree, dsk);
	(void)memmove(dsk_start, bnd->start, len);

	r->entries -= r->total_entries;
	r->first_free = dsk_start + len;
	r->space_avail =
	    (r->split_size - WT_PAGE_HEADER_BYTE_SIZE(btree)) - len;

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __rec_split_write --
 *	Write a disk block out for the split helper functions.
 */
static int
__rec_split_write(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, WT_BOUNDARY *bnd, WT_ITEM *buf, int checkpoint)
{
	WT_CELL *cell;
	WT_PAGE_HEADER *dsk;
	uint32_t size;
	uint8_t addr[WT_BTREE_MAX_ADDR_COOKIE];

	dsk = buf->mem;

	/*
	 * We always write an additional byte on row-store leaf pages after the
	 * key value pairs.  The reason is that zero-length value items are not
	 * written on the page and they're detected by finding two adjacent key
	 * cells.  If the last value item on a page is zero length, we need a
	 * key cell after it on the page to detect it.  The row-store leaf page
	 * reconciliation code made sure we had a spare byte in the buffer, now
	 * write a trailing zero-length key cell.  This isn't a valid key cell,
	 * but since it's not referenced by the entries on the page, no code but
	 * the code reading after the key cell, to find the key value, will ever
	 * see it.
	 */
#define	WT_TRAILING_KEY_CELL	(sizeof(uint8_t))
	if (dsk->type == WT_PAGE_ROW_LEAF) {
		WT_ASSERT_RET(session, buf->size < buf->memsize);

		cell = (WT_CELL *)&(((uint8_t *)buf->data)[buf->size]);
		__wt_cell_pack_key_empty(cell);
		++buf->size;
	}

	/*
	 * Write the chunk and save the location information.  There is one big
	 * question: if this is a checkpoint, we're going to have to wrap up
	 * our tracking information (freeing blocks we no longer need) before we
	 * can create the checkpoint, because checkpoints may write additional
	 * information.   We have to handle empty tree checkpoints elsewhere
	 * (because we don't write anything for empty tree checkpoints, they
	 * don't come through this path).  Given that fact, clear the boundary
	 * information as a reminder, and do the checkpoint at a later time,
	 * during wrapup.
	 */
	if (checkpoint) {
		bnd->addr.addr = NULL;
		bnd->addr.size = 0;
	} else {
		WT_RET(__wt_bm_write(session, buf, addr, &size));
		WT_RET(
		    __wt_strndup(session, (char *)addr, size, &bnd->addr.addr));
		bnd->addr.size = size;
		bnd->addr.leaf_no_overflow =
		    (dsk->type == WT_PAGE_COL_FIX ||
		    dsk->type == WT_PAGE_COL_VAR ||
		    dsk->type == WT_PAGE_ROW_LEAF) &&
		    r->ovfl_items == 0 ? 1 : 0;
	}

	return (0);
}

/*
 * __rec_split_row_promote --
 *	Key promotion for a row-store.
 */
static int
__rec_split_row_promote(WT_SESSION_IMPL *session, WT_RECONCILE *r, uint8_t type)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	uint32_t cnt, len, size;
	const uint8_t *pa, *pb;

	btree = session->btree;
	unpack = &_unpack;

	/*
	 * For a column-store, the promoted key is the recno and we already have
	 * a copy.  For a row-store, it's the first key on the page, a variable-
	 * length byte string, get a copy.
	 *
	 * This function is called from __rec_split at each split boundary, but
	 * that means we're not called before the first boundary.  It's painful,
	 * but we need to detect that case and copy the key from the page we're
	 * building.  We could simplify this by grabbing a copy of the first key
	 * we put on a page, perhaps in the function building keys for a page,
	 * but that's going to be uglier than this.
	 */
	if (r->bnd_next == 1) {
		/*
		 * The cell had better have a zero-length prefix: it's the first
		 * key on the page.  We also assert it's not a copy cell, even
		 * if we could copy the value, which we could, the first cell on
		 * a page had better not refer an earlier cell on the page.
		 */
		cell = WT_PAGE_HEADER_BYTE(btree, r->dsk.mem);
		__wt_cell_unpack(cell, unpack);
		WT_ASSERT_RET(session,
		    unpack->raw != WT_CELL_VALUE_COPY && unpack->prefix == 0);
		WT_RET(__wt_cell_unpack_copy(session, unpack, &r->bnd[0].key));
	}

	/*
	 * For the current slot, take the last key we built, after doing suffix
	 * compression.
	 *
	 * Suffix compression is a hack to shorten keys on internal pages.  We
	 * only need enough bytes in the promoted key to ensure searches go to
	 * the correct page: the promoted key has to be larger than the last key
	 * on the leaf page preceding it, but we don't need any more bytes than
	 * that.   In other words, we can discard any suffix bytes not required
	 * to distinguish between the key being promoted and the last key on the
	 * leaf page preceding it.  This can only be done for the first level of
	 * internal pages, you cannot repeat suffix truncation as you split up
	 * the tree, it loses too much information.
	 *
	 * One note: if the last key on the previous page was an overflow key,
	 * we don't have the in-memory key against which to compare, and don't
	 * try to do suffix compression.  The code for that case turns suffix
	 * compression off for the next key.
	 *
	 * The r->last key sorts before the r->cur key, so we'll either find a
	 * larger byte value in r->cur, or r->cur will be the longer key, and
	 * the interesting byte is one past the length of the shorter key.
	 */
	if (type == WT_PAGE_ROW_LEAF && r->key_sfx_compress) {
		pa = r->last->data;
		pb = r->cur->data;
		len = WT_MIN(r->last->size, r->cur->size);
		size = len + 1;
		for (cnt = 1; len > 0; ++cnt, --len, ++pa, ++pb)
			if (*pa != *pb) {
				size = cnt;
				break;
			}
	} else
		size = r->cur->size;
	return (__wt_buf_set(
	    session, &r->bnd[r->bnd_next].key, r->cur->data, size));
}

/*
 * __wt_rec_bulk_init --
 *	Bulk insert reconciliation initialization.
 */
int
__wt_rec_bulk_init(WT_CURSOR_BULK *cbulk)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	WT_RECONCILE *r;
	WT_SESSION_IMPL *session;
	uint64_t recno;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	btree = session->btree;
	page = cbulk->leaf;

	WT_RET(__rec_write_init(session, page, 0, &cbulk->reconcile));
	r = cbulk->reconcile;

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		recno = 1;
		break;
	case BTREE_ROW:
		recno = 0;
		break;
	WT_ILLEGAL_VALUE(session);
	}

	WT_RET(__rec_split_init(session, r, page, recno, btree->maxleafpage));

	return (0);
}

/*
 * __wt_rec_bulk_wrapup --
 *	Bulk insert reconciliation cleanup.
 */
int
__wt_rec_bulk_wrapup(WT_CURSOR_BULK *cbulk)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	WT_RECONCILE *r;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	r = cbulk->reconcile;
	btree = session->btree;

	switch (btree->type) {
	case BTREE_COL_FIX:
		if (cbulk->entry != 0)
			__rec_incr(session, r, cbulk->entry,
			    __bitstr_size(cbulk->entry * btree->bitcnt));
		break;
	case BTREE_COL_VAR:
		if (cbulk->rle != 0)
			WT_RET(__wt_rec_col_var_bulk_insert(cbulk));
		break;
	case BTREE_ROW:
		break;
	WT_ILLEGAL_VALUE(session);
	}

	page = cbulk->leaf;

	WT_RET(__rec_split_finish(session, r));
	WT_RET(__rec_write_wrapup(session, r, page));

	/* Mark the tree dirty so close performs a checkpoint. */
	btree->modified = 1;

	/* Mark the page's parent dirty. */
	WT_RET(__wt_page_modify_init(session, page->parent));
	__wt_page_modify_set(page->parent);

	__wt_rec_destroy(session, &cbulk->reconcile);

	return (0);
}

/*
 * __wt_rec_row_bulk_insert --
 *	Row-store bulk insert.
 */
int
__wt_rec_row_bulk_insert(WT_CURSOR_BULK *cbulk)
{
	WT_CURSOR *cursor;
	WT_KV *key, *val;
	WT_RECONCILE *r;
	WT_SESSION_IMPL *session;
	int ovfl_key;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	r = cbulk->reconcile;

	cursor = &cbulk->cbt.iface;
	key = &r->k;
	val = &r->v;
	WT_RET(__rec_cell_build_key(session, r,		/* Build key cell */
	    cursor->key.data, cursor->key.size, 0, &ovfl_key));
	WT_RET(__rec_cell_build_val(session, r,		/* Build value cell */
	    cursor->value.data, cursor->value.size, (uint64_t)0));

	/*
	 * Boundary, split or write the page.
	 *
	 * We write a trailing key cell on the page after the K/V pairs
	 * (see WT_TRAILING_KEY_CELL for more information).
	 */
	while (key->len + val->len + WT_TRAILING_KEY_CELL > r->space_avail) {
		/* Split the page. */
		WT_RET(__rec_split(session, r));

		/*
		 * Turn off prefix compression until a full key written
		 * to the new page, and (unless we're already working
		 * with an overflow key), rebuild the key without prefix
		 * compression.
		 */
		r->key_pfx_compress = 0;
		if (!ovfl_key)
			WT_RET(__rec_cell_build_key(
			    session, r, NULL, 0, 0, &ovfl_key));
	}

	/* Copy the key/value pair onto the page. */
	__rec_copy_incr(session, r, key);
	if (val->len != 0) {
		if (r->dictionary != NULL)
			WT_RET(__rec_dict_replace(session, r, 0, val));
		__rec_copy_incr(session, r, val);
	}

	/* Update compression state. */
	__rec_key_state_update(r, ovfl_key);

	return (0);
}

/*
 * __wt_rec_col_fix_bulk_insert --
 *	Fixed-length column-store bulk insert.
 */
int
__wt_rec_col_fix_bulk_insert(WT_CURSOR_BULK *cbulk)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_RECONCILE *r;
	WT_SESSION_IMPL *session;
	const uint8_t *data;
	uint32_t entries, page_entries, page_size;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	r = cbulk->reconcile;
	btree = session->btree;
	cursor = &cbulk->cbt.iface;

	if (cbulk->bitmap) {
		for (data = cursor->value.data, entries = cursor->value.size;
		    entries > 0;
		    entries -= page_entries, data += page_size) {
			page_entries = WT_MIN(entries,
			    r->space_avail * 8 / btree->bitcnt);
			page_size = __bitstr_size(page_entries * btree->bitcnt);

			memcpy(r->first_free, data, page_size);
			r->recno += page_entries;

			/* Leave the last page for wrapup. */
			if (entries > page_entries) {
				__rec_incr(session, r, page_entries, page_size);
				WT_RET(__rec_split(session, r));
			} else
				cbulk->entry = page_entries;
		}
		return (0);
	}

	if (cbulk->entry == cbulk->nrecs) {
		if (cbulk->entry != 0) {
			/*
			 * If everything didn't fit, update the counters and
			 * split.
			 *
			 * Boundary: split or write the page.
			 */
			__rec_incr(session, r, cbulk->entry,
			    __bitstr_size(cbulk->entry * btree->bitcnt));
			WT_RET(__rec_split(session, r));
		}
		cbulk->entry = 0;
		cbulk->nrecs = r->space_avail * 8 / btree->bitcnt;
	}

	__bit_setv(r->first_free,
	    cbulk->entry, btree->bitcnt, ((uint8_t *)cursor->value.data)[0]);
	++cbulk->entry;
	++r->recno;

	return (0);
}

/*
 * __wt_rec_col_var_bulk_insert --
 *	Variable-length column-store bulk insert.
 */
int
__wt_rec_col_var_bulk_insert(WT_CURSOR_BULK *cbulk)
{
	WT_SESSION_IMPL *session;
	WT_KV *val;
	WT_RECONCILE *r;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	r = cbulk->reconcile;

	val = &r->v;
	WT_RET(__rec_cell_build_val(
	    session, r, cbulk->cmp.data, cbulk->cmp.size, cbulk->rle));

	/* Boundary: split or write the page. */
	while (val->len > r->space_avail)
		WT_RET(__rec_split(session, r));

	/* Copy the value onto the page. */
	if (r->dictionary != NULL)
		WT_RET(__rec_dict_replace(session, r, cbulk->rle, val));
	__rec_copy_incr(session, r, val);

	/* Update the starting record number in case we split. */
	r->recno += cbulk->rle;

	return (0);
}

/*
 * __rec_col_int --
 *	Reconcile a column-store internal page.
 */
static int
__rec_col_int(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_BTREE *btree;

	btree = session->btree;

	WT_RET(__rec_split_init(
	    session, r, page, page->u.intl.recno, btree->maxintlpage));

	/*
	 * Walking the row-store internal pages is complicated by the fact that
	 * we're taking keys from the underlying disk image for the top-level
	 * page and we're taking keys from in-memory structures for merge pages.
	 * Column-store is simpler because the only information we copy is the
	 * record number and address, and it comes from in-memory structures in
	 * both the top-level and merge cases.  In short, both the top-level
	 * and merge page walks look the same, and we just call the merge page
	 * function on the top-level page.
	 */
	WT_RET(__rec_col_merge(session, r, page));

	/* Write the remnant page. */
	return (__rec_split_finish(session, r));
}

/*
 * __rec_col_merge --
 *	Recursively walk a column-store internal tree of merge pages.
 */
static int
__rec_col_merge(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_ADDR *addr;
	WT_KV *val;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_PAGE *rp;
	WT_REF *ref;
	uint32_t i;
	int modified;

	WT_BSTAT_INCR(session, rec_page_merge);

	val = &r->v;
	unpack = &_unpack;

	/* For each entry in the page... */
	WT_REF_FOREACH(page, ref, i) {
		/* Update the starting record number in case we split. */
		r->recno = ref->u.recno;

		/*
		 * The page may be emptied or internally created during a split.
		 * Deleted/split pages are merged into the parent and discarded.
		 */
		addr = NULL;
		WT_RET(__rec_page_modified(session, r, page, ref, &modified));
		if (modified) {
			rp = ref->page;
			switch (F_ISSET(rp->modify, WT_PM_REC_MASK)) {
			case WT_PM_REC_EMPTY:
				/*
				 * Column-store pages are almost never empty, as
				 * discarding a page would remove a chunk of the
				 * name space.  The exceptions are pages created
				 * when the tree is created, and never filled.
				 */
				continue;
			case WT_PM_REC_REPLACE:
				addr = &rp->modify->u.replace;
				break;
			case WT_PM_REC_SPLIT:
				WT_RET(__rec_col_merge(
				    session, r, rp->modify->u.split));
				continue;
			case WT_PM_REC_SPLIT_MERGE:
				WT_RET(__rec_col_merge(session, r, rp));
				continue;
			}
		}

		/*
		 * Build the value cell.  The child page address is in one of 3
		 * places: if the page was replaced, the page's modify structure
		 * references it and we built the value cell just above in the
		 * switch statement.  Else, the WT_REF->addr reference points to
		 * an on-page cell or an off-page WT_ADDR structure: if it's an
		 * on-page cell and we copy it from the page, else build a new
		 * cell.
		 */
		if (addr == NULL && __wt_off_page(page, ref->addr))
			addr = ref->addr;
		if (addr == NULL) {
			__wt_cell_unpack(ref->addr, unpack);
			val->buf.data = ref->addr;
			val->buf.size = __wt_cell_total_len(unpack);
			val->cell_len = 0;
			val->len = val->buf.size;
		} else
			__rec_cell_build_addr(r,
			    addr->addr, addr->size,
			    addr->leaf_no_overflow ?
			    WT_CELL_ADDR_LNO : WT_CELL_ADDR,
			    ref->u.recno);

		/* Boundary: split or write the page. */
		while (val->len > r->space_avail)
			WT_RET(__rec_split(session, r));

		/* Copy the value onto the page. */
		__rec_copy_incr(session, r, val);
	}

	return (0);
}

/*
 * __rec_col_fix --
 *	Reconcile a fixed-width, column-store leaf page.
 */
static int
__rec_col_fix(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_INSERT *ins;
	WT_INSERT_HEAD *append;
	WT_UPDATE *upd;
	uint64_t recno;
	uint32_t entry, nrecs;

	btree = session->btree;

	/* Update any changes to the original on-page data items. */
	WT_SKIP_FOREACH(ins, WT_COL_UPDATE_SINGLE(page)) {
		WT_RET(__rec_txn_read(session, r, ins->upd, &upd));
		if (upd == NULL)
			continue;
		__bit_setv_recno(
		    page, WT_INSERT_RECNO(ins), btree->bitcnt,
		    ((uint8_t *)WT_UPDATE_DATA(upd))[0]);
	}

	/* Allocate the memory. */
	WT_RET(__rec_split_init(session, r,
	    page, page->u.col_fix.recno, btree->maxleafpage));

	/* Copy the updated, disk-image bytes into place. */
	memcpy(r->first_free, page->u.col_fix.bitf,
	    __bitstr_size(page->entries * btree->bitcnt));

	/* Calculate the number of entries per page remainder. */
	entry = page->entries;
	nrecs = (r->space_avail * 8 / btree->bitcnt) - page->entries;
	r->recno += entry;

	/* Walk any append list. */
	append = WT_COL_APPEND(page);
	WT_SKIP_FOREACH(ins, append) {
		WT_RET(__rec_txn_read(session, r, ins->upd, &upd));
		if (upd == NULL)
			continue;
		for (;;) {
			/*
			 * The application may have inserted records which left
			 * gaps in the name space.
			 */
			for (recno = WT_INSERT_RECNO(ins);
			    nrecs > 0 && r->recno < recno;
			    --nrecs, ++entry, ++r->recno)
				__bit_setv(
				    r->first_free, entry, btree->bitcnt, 0);

			if (nrecs > 0) {
				__bit_setv(r->first_free, entry, btree->bitcnt,
				    ((uint8_t *)WT_UPDATE_DATA(upd))[0]);
				--nrecs;
				++entry;
				++r->recno;
				break;
			}

			/*
			 * If everything didn't fit, update the counters and
			 * split.
			 *
			 * Boundary: split or write the page.
			 */
			__rec_incr(session,
			    r, entry, __bitstr_size(entry * btree->bitcnt));
			WT_RET(__rec_split(session, r));

			/* Calculate the number of entries per page. */
			entry = 0;
			nrecs = r->space_avail * 8 / btree->bitcnt;
		}
	}

	/* Update the counters. */
	__rec_incr(session, r, entry, __bitstr_size(entry * btree->bitcnt));

	/* Write the remnant page. */
	return (__rec_split_finish(session, r));
}

/*
 * __rec_col_fix_slvg --
 *	Reconcile a fixed-width, column-store leaf page created during salvage.
 */
static int
__rec_col_fix_slvg(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, WT_PAGE *page, WT_SALVAGE_COOKIE *salvage)
{
	WT_BTREE *btree;
	uint64_t page_start, page_take;
	uint32_t entry, nrecs;

	btree = session->btree;

	/*
	 * !!!
	 * It's vanishingly unlikely and probably impossible for fixed-length
	 * column-store files to have overlapping key ranges.  It's possible
	 * for an entire key range to go missing (if a page is corrupted and
	 * lost), but because pages can't split, it shouldn't be possible to
	 * find pages where the key ranges overlap.  That said, we check for
	 * it during salvage and clean up after it here because it doesn't
	 * cost much and future column-store formats or operations might allow
	 * for fixed-length format ranges to overlap during salvage, and I
	 * don't want to have to retrofit the code later.
	 */
	WT_RET(__rec_split_init(session, r,
	    page, page->u.col_fix.recno, btree->maxleafpage));

	/* We may not be taking all of the entries on the original page. */
	page_take = salvage->take == 0 ? page->entries : salvage->take;
	page_start = salvage->skip == 0 ? 0 : salvage->skip;
	for (;;) {
		/* Calculate the number of entries per page. */
		entry = 0;
		nrecs = r->space_avail * 8 / btree->bitcnt;

		for (; nrecs > 0 && salvage->missing > 0;
		    --nrecs, --salvage->missing, ++entry)
			__bit_setv(r->first_free, entry, btree->bitcnt, 0);

		for (; nrecs > 0 && page_take > 0;
		    --nrecs, --page_take, ++page_start, ++entry)
			__bit_setv(r->first_free, entry, btree->bitcnt,
			    __bit_getv(page->u.col_fix.bitf,
				(uint32_t)page_start, btree->bitcnt));

		r->recno += entry;
		__rec_incr(
		    session, r, entry, __bitstr_size(entry * btree->bitcnt));

		/*
		 * If everything didn't fit, then we have to force a split and
		 * keep going.
		 *
		 * Boundary: split or write the page.
		 */
		if (salvage->missing == 0 && page_take == 0)
			break;
		WT_RET(__rec_split(session, r));
	}

	/* Write the remnant page. */
	return (__rec_split_finish(session, r));
}

/*
 * __rec_col_var_helper --
 *	Create a column-store variable length record cell and write it onto a
 * page.
 */
static int
__rec_col_var_helper(WT_SESSION_IMPL *session, WT_RECONCILE *r,
    WT_SALVAGE_COOKIE *salvage,
    WT_ITEM *value, int deleted, int ovfl, uint64_t rle)
{
	WT_KV *val;

	val = &r->v;

	/*
	 * Occasionally, salvage needs to discard records from the beginning or
	 * end of the page, and because the items may be part of a RLE cell, do
	 * the adjustments here.   It's not a mistake we don't bother telling
	 * our caller we've handled all the records from the page we care about,
	 * and can quit processing the page: salvage is a rare operation and I
	 * don't want to complicate our caller's loop.
	 */
	if (salvage != NULL) {
		if (salvage->done)
			return (0);
		if (salvage->skip != 0) {
			if (rle <= salvage->skip) {
				salvage->skip -= rle;
				return (0);
			}
			salvage->skip = 0;
			rle -= salvage->skip;
		}
		if (salvage->take != 0) {
			if (rle <= salvage->take)
				salvage->take -= rle;
			else {
				rle = salvage->take;
				salvage->take = 0;
			}
			if (salvage->take == 0)
				salvage->done = 1;
		}
	}

	if (deleted) {
		val->cell_len = __wt_cell_pack_del(&val->cell, rle);
		val->buf.data = NULL;
		val->buf.size = 0;
		val->len = val->cell_len;
	} else if (ovfl) {
		val->cell_len = __wt_cell_pack_ovfl(
		    &val->cell, WT_CELL_VALUE_OVFL, rle, value->size);
		val->buf.data = value->data;
		val->buf.size = value->size;
		val->len = val->cell_len + value->size;
	} else
		WT_RET(__rec_cell_build_val(
		    session, r, value->data, value->size, rle));

	/* Boundary: split or write the page. */
	while (val->len > r->space_avail)
		WT_RET(__rec_split(session, r));

	/* Copy the value onto the page. */
	if (!deleted && !ovfl && r->dictionary != NULL)
		WT_RET(__rec_dict_replace(session, r, rle, val));
	__rec_copy_incr(session, r, val);

	/* Update the starting record number in case we split. */
	r->recno += rle;

	return (0);
}

/*
 * __rec_col_var --
 *	Reconcile a variable-width column-store leaf page.
 */
static int
__rec_col_var(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, WT_PAGE *page, WT_SALVAGE_COOKIE *salvage)
{
	enum { OVFL_IGNORE, OVFL_UNUSED, OVFL_USED } ovfl_state;
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	WT_DECL_ITEM(orig);
	WT_DECL_RET;
	WT_INSERT *ins;
	WT_INSERT_HEAD *append;
	WT_ITEM *last;
	WT_UPDATE *upd;
	uint64_t n, nrepeat, repeat_count, rle, slvg_missing, src_recno;
	uint32_t i, size;
	int deleted, last_deleted, orig_deleted, update_no_copy;
	const void *data;

	btree = session->btree;
	last = r->last;
	unpack = &_unpack;

	WT_RET(__wt_scr_alloc(session, 0, &orig));
	data = NULL;
	size = 0;

	WT_RET(__rec_split_init(
	    session, r, page, page->u.col_var.recno, btree->maxleafpage));

	/*
	 * The salvage code may be calling us to reconcile a page where there
	 * were missing records in the column-store name space.  In this case
	 * we write a single RLE element onto a new page, so we know it fits,
	 * then update the starting record number.
	 *
	 * Note that we DO NOT pass the salvage cookie to our helper function
	 * in this case, we're handling one of the salvage cookie fields on
	 * our own, and don't need assistance from the helper function.
	 */
	slvg_missing = salvage == NULL ? 0 : salvage->missing;
	if (slvg_missing)
		WT_ERR(__rec_col_var_helper(
		    session, r, NULL, NULL, 1, 0, slvg_missing));

	/*
	 * We track two data items through this loop: the previous (last) item
	 * and the current item: if the last item is the same as the current
	 * item, we increment the RLE count for the last item; if the last item
	 * is different from the current item, we write the last item onto the
	 * page, and replace it with the current item.  The r->recno counter
	 * tracks records written to the page, and is incremented by the helper
	 * function immediately after writing records to the page.  The record
	 * number of our source record, that is, the current item, is maintained
	 * in src_recno.
	 */
	src_recno = r->recno;

	/* For each entry in the in-memory page... */
	rle = 0;
	deleted = last_deleted = 0;
	WT_COL_FOREACH(page, cip, i) {
		ovfl_state = OVFL_IGNORE;
		if ((cell = WT_COL_PTR(page, cip)) == NULL) {
			nrepeat = 1;
			ins = NULL;
			orig_deleted = 1;
		} else {
			__wt_cell_unpack(cell, unpack);
			nrepeat = __wt_cell_rle(unpack);
			ins = WT_SKIP_FIRST(WT_COL_UPDATE(page, cip));

			/*
			 * If the original value is "deleted", there's no value
			 * to compare, we're done.
			 */
			orig_deleted = unpack->type == WT_CELL_DEL ? 1 : 0;
			if (orig_deleted)
				goto record_loop;

			/*
			 * Overflow items are tricky: we don't know until we're
			 * finished processing the set of values if we need the
			 * overflow value or not.  If we don't use the overflow
			 * item at all, we'll have to discard it (that's safe
			 * because once the original value is unused during any
			 * page reconciliation, it will never be needed again).
			 *
			 * Regardless, we avoid copying in overflow records: if
			 * there's a WT_INSERT entry that modifies a reference
			 * counted overflow record, we may have to write copies
			 * of the overflow record, and in that case we'll do the
			 * comparisons, but we don't read overflow items just to
			 * see if they match records on either side.
			 */
			if (unpack->ovfl) {
				ovfl_state = OVFL_UNUSED;
				goto record_loop;
			}

			/*
			 * Check for the common case where the underlying value
			 * is simple and avoid a copy.
			 */
			if (btree->huffman_value == NULL) {
				orig->data = unpack->data;
				orig->size = unpack->size;
				goto record_loop;
			}

			/*
			 * The data is Huffman encoded, which means we have to
			 * decode it in order to compare it with the last item
			 * we saw, which may have been an update string.  This
			 * guarantees we find every single pair of objects we
			 * can RLE encode, including applications updating an
			 * existing record where the new value happens (?) to
			 * match a Huffman-encoded value in a previous or next
			 * record.
			 */
			WT_ERR(__wt_cell_unpack_copy(session, unpack, orig));
		}

record_loop:	/*
		 * Generate on-page entries: loop repeat records, looking for
		 * WT_INSERT entries matching the record number.  The WT_INSERT
		 * lists are in sorted order, so only need check the next one.
		 */
		for (n = 0;
		    n < nrepeat; n += repeat_count, src_recno += repeat_count) {
			upd = NULL;
			if (ins != NULL && WT_INSERT_RECNO(ins) == src_recno) {
				WT_ERR(
				    __rec_txn_read(session, r, ins->upd, &upd));
				ins = WT_SKIP_NEXT(ins);
			}
			if (upd != NULL) {
				update_no_copy = 1;	/* No data copy */

				repeat_count = 1;

				deleted = WT_UPDATE_DELETED_ISSET(upd);
				if (!deleted) {
					data = WT_UPDATE_DATA(upd);
					size = upd->size;
				}
			} else {
				update_no_copy = 0;	/* Maybe data copy */

				/*
				 * The repeat count is the number of records up
				 * to the next WT_INSERT record, or up to the
				 * end of the entry if we have no more WT_INSERT
				 * records.
				 */
				if (ins == NULL)
					repeat_count = nrepeat - n;
				else
					repeat_count =
					    WT_INSERT_RECNO(ins) - src_recno;

				deleted = orig_deleted;
				if (deleted)
					goto compare;

				/*
				 * If we are handling overflow items, use the
				 * overflow item itself exactly once, after
				 * which we have to copy it into a buffer and
				 * from then on use a complete copy because we
				 * are re-creating a new overflow record each
				 * time.
				 */
				switch (ovfl_state) {
				case OVFL_UNUSED:
					/*
					 * Original is an overflow item, as yet
					 * unused -- use it now.
					 *
					 * Write out any record we're tracking.
					 */
					if (rle != 0) {
						WT_ERR(__rec_col_var_helper(
						    session, r, salvage, last,
						    last_deleted, 0, rle));
						rle = 0;
					}

					/* Write the overflow item. */
					last->data = unpack->data;
					last->size = unpack->size;
					WT_ERR(__rec_col_var_helper(
					    session, r, salvage,
					    last, 0, 1, repeat_count));

					/* Track if page has overflow items. */
					r->ovfl_items = 1;

					ovfl_state = OVFL_USED;
					continue;
				case OVFL_USED:
					/*
					 * Original is an overflow item; we used
					 * it for a key and now we need another
					 * copy; read it into memory.
					 */
					WT_ERR(__wt_cell_unpack_copy(
					    session, unpack, orig));

					ovfl_state = OVFL_IGNORE;
					/* FALLTHROUGH */
				case OVFL_IGNORE:
					/*
					 * Original is an overflow item and we
					 * were forced to copy it into memory,
					 * or the original wasn't an overflow
					 * item; use the data copied into orig.
					 */
					data = orig->data;
					size = orig->size;
					break;
				}
			}

compare:		/*
			 * If we have a record against which to compare, and
			 * the records compare equal, increment the rle counter
			 * and continue.  If the records don't compare equal,
			 * output the last record and swap the last and current
			 * buffers: do NOT update the starting record number,
			 * we've been doing that all along.
			 */
			if (rle != 0) {
				if ((deleted && last_deleted) ||
				    (!last_deleted && !deleted &&
				    last->size == size &&
				    memcmp(last->data, data, size) == 0)) {
					rle += repeat_count;
					continue;
				}
				WT_ERR(__rec_col_var_helper(session, r,
				    salvage, last, last_deleted, 0, rle));
			}

			/*
			 * Swap the current/last state.
			 *
			 * Reset RLE counter and turn on comparisons.
			 */
			if (!deleted) {
				/*
				 * We can't simply assign the data values into
				 * the last buffer because they may have come
				 * from a copy built from an encoded/overflow
				 * cell and creating the next record is going
				 * to overwrite that memory.  Check, because
				 * encoded/overflow cells aren't that common
				 * and we'd like to avoid the copy.  If data
				 * was taken from the current unpack structure
				 * (which points into the page), or was taken
				 * from an update structure, we can just use
				 * the pointers, they're not moving.
				 */
				if (data == unpack->data || update_no_copy) {
					last->data = data;
					last->size = size;
				} else
					WT_ERR(__wt_buf_set(
					    session, last, data, size));
			}
			last_deleted = deleted;
			rle = repeat_count;
		}

		/*
		 * If we had a reference to an overflow record we never used,
		 * discard the underlying blocks, they're no longer useful.
		 * One complication: we must cache a copy before discarding the
		 * on-disk version if there's a transaction in the system that
		 * might read the original value.
		 */
		if (ovfl_state == OVFL_UNUSED) {
			WT_ERR(__wt_rec_track_onpage_addr(
			    session, page, unpack->data, unpack->size));
			WT_ERR(__wt_val_ovfl_cache(session, page, upd, unpack));
		}
	}

	/* Walk any append list. */
	append = WT_COL_APPEND(page);
	WT_SKIP_FOREACH(ins, append) {
		WT_ERR(__rec_txn_read(session, r, ins->upd, &upd));
		if (upd == NULL)
			continue;
		for (n = WT_INSERT_RECNO(ins); src_recno <= n; ++src_recno) {
			/*
			 * The application may have inserted records which left
			 * gaps in the name space.
			 */
			if (src_recno < n)
				deleted = 1;
			else {
				deleted = WT_UPDATE_DELETED_ISSET(upd);
				if (!deleted) {
					data = WT_UPDATE_DATA(upd);
					size = upd->size;
				}
			}

			/*
			 * Handle RLE accounting and comparisons -- see comment
			 * above, this code fragment does the same thing.
			 */
			if (rle != 0) {
				if ((deleted && last_deleted) ||
				    (!last_deleted && !deleted &&
				    last->size == size &&
				    memcmp(last->data, data, size) == 0)) {
					++rle;
					continue;
				}
				WT_ERR(__rec_col_var_helper(session, r,
				    salvage, last, last_deleted, 0, rle));
			}

			/*
			 * Swap the current/last state.  We always assign the
			 * data values to the buffer because they can only be
			 * the data from a WT_UPDATE structure.
			 *
			 * Reset RLE counter and turn on comparisons.
			 */
			if (!deleted) {
				last->data = data;
				last->size = size;
			}
			last_deleted = deleted;
			rle = 1;
		}
	}

	/* If we were tracking a record, write it. */
	if (rle != 0)
		WT_ERR(__rec_col_var_helper(
		    session, r, salvage, last, last_deleted, 0, rle));

	/* Write the remnant page. */
	ret = __rec_split_finish(session, r);

err:	__wt_scr_free(&orig);
	return (ret);
}

/*
 * __rec_row_int --
 *	Reconcile a row-store internal page.
 */
static int
__rec_row_int(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_ADDR *addr;
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *kpack, _kpack, *vpack, _vpack;
	WT_IKEY *ikey;
	WT_KV *key, *val;
	WT_PAGE *rp;
	WT_REF *ref;
	uint32_t i, size;
	u_int vtype;
	int modified, onpage_ovfl, ovfl_key;
	const void *p;

	btree = session->btree;

	key = &r->k;
	kpack = &_kpack;
	val = &r->v;
	vpack = &_vpack;

	WT_RET(__rec_split_init(session, r, page, 0ULL, btree->maxintlpage));

	/*
	 * Ideally, we'd never store the 0th key on row-store internal pages
	 * because it's never used during tree search and there's no reason
	 * to waste the space.  The problem is how we do splits: when we split,
	 * we've potentially picked out several "split points" in the buffer
	 * which is overflowing the maximum page size, and when the overflow
	 * happens, we go back and physically split the buffer, at those split
	 * points, into new pages.  It would be both difficult and expensive
	 * to re-process the 0th key at each split point to be an empty key,
	 * so we don't do that.  However, we are reconciling an internal page
	 * for whatever reason, and the 0th key is known to be useless.  We
	 * truncate the key to a single byte, instead of removing it entirely,
	 * it simplifies various things in other parts of the code (we don't
	 * have to special case transforming the page from its disk image to
	 * its in-memory version, for example).
	 */
	r->cell_zero = 1;

	/* For each entry in the in-memory page... */
	WT_REF_FOREACH(page, ref, i) {
		/*
		 * Keys are always instantiated for row-store internal pages,
		 * set the WT_IKEY reference, and unpack the cell if the key
		 * references one.
		 */
		ikey = ref->u.key;
		if (ikey->cell_offset == 0)
			cell = NULL;
		else {
			cell = WT_PAGE_REF_OFFSET(page, ikey->cell_offset);
			__wt_cell_unpack(cell, kpack);
		}

		/*
		 * We need to know if we're using on-page overflow key cell in
		 * a few places below, initialize the unpacked cell's overflow
		 * value so there's an easy test.
		 */
		onpage_ovfl = cell != NULL && kpack->ovfl == 1 ? 1 : 0;

		vtype = 0;
		addr = NULL;
		rp = ref->page;
		WT_RET(__rec_page_modified(session, r, page, ref, &modified));

		/*
		 * A modified WT_REF with no child page must be a page marked
		 * deleted without being read.
		 */
		if (modified && rp == NULL) {
			/*
			 * If the WT_REF addr field is cleared, not only is the
			 * leaf page deleted, but there are no older readers in
			 * the system, and there's no need to write this cell.
			 */
			if (ref->addr == NULL) {
				/*
				 * Overflow keys referencing discarded pages are
				 * no longer useful, schedule them for discard.
				 * Don't worry about instantiation, internal
				 * page keys are always instantiated.  Don't
				 * worry about reuse, reusing this key in this
				 * reconciliation is unlikely.
				 */
				if (onpage_ovfl)
					WT_RET(__wt_rec_track_onpage_addr(
					    session, page,
					    kpack->data, kpack->size));
				continue;
			}

			/*
			 * There must be older readers in the system, write a
			 * special "deleted address" cell.
			 */
			vtype = WT_CELL_ADDR_DEL;
		}

		/*
		 * The page may be emptied or internally created during a split.
		 * Deleted/split pages are merged into the parent and discarded.
		 *
		 * There's one special case we have to handle here: the internal
		 * page being merged has a potentially incorrect first key and
		 * we need to replace it with the one we have.  The problem is
		 * caused by the fact that the page search algorithm coerces the
		 * 0th key on any internal page to be smaller than any search
		 * key.  We do that because we don't want to have to update the
		 * internal pages every time a new "smallest" key is inserted
		 * into the tree.  But, if a new "smallest" key is inserted into
		 * our split-created subtree, and we don't update the internal
		 * page, when we merge that internal page into its parent page,
		 * the key may be incorrect (or more likely, have been coerced
		 * to a single byte because it's an internal page's 0th key).
		 * Imagine the following tree:
		 *
		 *	2	5	40	internal page
		 *		|
		 * 	    10  | 20		split-created internal page
		 *	    |
		 *	    6			inserted smallest key
		 *
		 * after a simple merge, we'd have corruption:
		 *
		 *	2    10    20	40	merged internal page
		 *	     |
		 *	     6			key sorts before parent's key
		 *
		 * To fix this problem, we take the higher-level page's key as
		 * our first key, because that key sorts before any possible
		 * key inserted into the subtree, and discard whatever 0th key
		 * is on the split-created internal page.
		 */
		if (modified && rp != NULL)
			switch (F_ISSET(rp->modify, WT_PM_REC_MASK)) {
			case WT_PM_REC_EMPTY:
				/*
				 * Overflow keys referencing empty pages are no
				 * longer useful, schedule them for discard.
				 * Don't worry about instantiation, internal
				 * page keys are always instantiated.  Don't
				 * worry about reuse, reusing this key in this
				 * reconciliation is unlikely.
				 */
				if (onpage_ovfl)
					WT_RET(__wt_rec_track_onpage_addr(
					    session, page,
					    kpack->data, kpack->size));
				continue;
			case WT_PM_REC_REPLACE:
				/*
				 * If the page is replaced, the page's modify
				 * structure has the page's address.
				 */
				addr = &rp->modify->u.replace;
				break;
			case WT_PM_REC_SPLIT:
			case WT_PM_REC_SPLIT_MERGE:
				/*
				 * Overflow keys referencing split pages are no
				 * longer useful (the split page's key is the
				 * interesting key); schedule them for discard.
				 * Don't worry about instantiation, internal
				 * page keys are always instantiated.  Don't
				 * worry about reuse, reusing this key in this
				 * reconciliation is unlikely.
				 */
				if (onpage_ovfl)
					WT_RET(__wt_rec_track_onpage_addr(
					    session, page,
					    kpack->data, kpack->size));

				r->merge_ref = ref;
				WT_RET(__rec_row_merge(session, r,
				    F_ISSET(rp->modify, WT_PM_REC_SPLIT_MERGE) ?
				    rp : rp->modify->u.split));
				continue;
			case 0:
				/*
				 * Hasn't been written since it was modified,
				 * we want to reference the original page.
				 */
				break;
			WT_ILLEGAL_VALUE(session);
			}

		/*
		 * Build the value cell, the child's page address.  In the case
		 * of a page replacement, addr points to the page's replacement
		 * address, else use WT_REF.addr, which points to an on-page
		 * cell or an off-page WT_ADDR structure.   In the case of page
		 * deletion, the cell type has also been set, otherwise use the
		 * information from the addr or original cell.
		 */
		if (addr == NULL && __wt_off_page(page, ref->addr))
			addr = ref->addr;
		if (addr == NULL) {
			__wt_cell_unpack(ref->addr, vpack);
			p = vpack->data;
			size = vpack->size;
			if (vtype == 0)
				vtype = vpack->raw;
		} else {
			p = addr->addr;
			size = addr->size;
			if (vtype == 0)
				vtype = addr->leaf_no_overflow ?
				    WT_CELL_ADDR_LNO : WT_CELL_ADDR;
		}
		__rec_cell_build_addr(r, p, size, vtype, 0);

		/*
		 * If the key is an overflow key, check to see if we've entered
		 * the key into the tracking system.  In that case, the original
		 * overflow key blocks have been freed, we have to build a new
		 * key.  If there's no tracking entry, use the original blocks.
		 */
		if (onpage_ovfl &&
		    __wt_rec_track_onpage_srch(page, kpack->data, kpack->size))
			onpage_ovfl = 0;

		/*
		 * Build key cell.
		 *
		 * Truncate any 0th key, internal pages don't need 0th keys.
		 */
		if (onpage_ovfl) {
			key->buf.data = cell;
			key->buf.size = __wt_cell_total_len(kpack);
			key->cell_len = 0;
			key->len = key->buf.size;
			ovfl_key = 1;
		} else
			WT_RET(__rec_cell_build_key(session, r,
			    WT_IKEY_DATA(ikey), r->cell_zero ? 1 : ikey->size,
			    1, &ovfl_key));
		r->cell_zero = 0;

		/*
		 * Boundary, split or write the page.
		 */
		while (key->len + val->len > r->space_avail) {
			/*
			 * In one path above, we copied the key from the page
			 * rather than building the actual key.  In that case,
			 * we have to build the actual key now because we are
			 * about to promote it.
			 */
			if (onpage_ovfl) {
				WT_RET(__wt_buf_set(session,
				    r->cur, WT_IKEY_DATA(ikey), ikey->size));
				onpage_ovfl = 0;
			}
			WT_RET(__rec_split(session, r));

			/*
			 * Turn off prefix compression until a full key written
			 * to the new page, and (unless we're already working
			 * with an overflow key), rebuild the key without prefix
			 * compression.
			 */
			r->key_pfx_compress = 0;
			if (!ovfl_key)
				WT_RET(__rec_cell_build_key(
				    session, r, NULL, 0, 1, &ovfl_key));
		}

		/* Copy the key and value onto the page. */
		__rec_copy_incr(session, r, key);
		__rec_copy_incr(session, r, val);

		/* Update compression state. */
		__rec_key_state_update(r, ovfl_key);
	}

	/* Write the remnant page. */
	return (__rec_split_finish(session, r));
}

/*
 * __rec_row_merge --
 *	Recursively walk a row-store internal tree of merge pages.
 */
static int
__rec_row_merge(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_ADDR *addr;
	WT_CELL_UNPACK *vpack, _vpack;
	WT_IKEY *ikey;
	WT_KV *key, *val;
	WT_PAGE *rp;
	WT_REF *ref;
	uint32_t i, size;
	u_int vtype;
	int modified, ovfl_key;
	const void *p;

	WT_BSTAT_INCR(session, rec_page_merge);

	key = &r->k;
	val = &r->v;
	vpack = &_vpack;

	/* For each entry in the in-memory page... */
	WT_REF_FOREACH(page, ref, i) {
		vtype = 0;
		addr = NULL;
		rp = ref->page;
		WT_RET(__rec_page_modified(session, r, page, ref, &modified));

		/*
		 * A modified WT_REF with no child page must be a page marked
		 * deleted without being read.
		 */
		if (modified && rp == NULL) {
			/*
			 * If the WT_REF addr field is cleared, not only is the
			 * leaf page deleted, but there are no older readers in
			 * the system, and there's no need to write this cell.
			 */
			if (ref->addr == NULL)
				continue;

			/*
			 * There must be older readers in the system, write a
			 * special "deleted address" cell.
			 */
			vtype = WT_CELL_ADDR_DEL;
		}

		/*
		 * The page may be emptied or internally created during a split.
		 * Deleted/split pages are merged into the parent and discarded.
		 */
		if (modified && rp != NULL)
			switch (F_ISSET(rp->modify, WT_PM_REC_MASK)) {
			case WT_PM_REC_EMPTY:
				continue;
			case WT_PM_REC_REPLACE:
				/*
				 * If the page is replaced, the page's modify
				 * structure has the page's address.
				 */
				addr = &rp->modify->u.replace;
				break;
			case WT_PM_REC_SPLIT:
			case WT_PM_REC_SPLIT_MERGE:
				/*
				 * If we have a merge key set, we're working our
				 * way down a merge tree.  If we have not set a
				 * merge key, we're starting descent of a new
				 * merge tree, set the merge key.
				 */
				if (r->merge_ref == NULL)
					r->merge_ref = ref;
				WT_RET(__rec_row_merge(session, r,
				    F_ISSET(rp->modify, WT_PM_REC_SPLIT_MERGE) ?
				    rp : rp->modify->u.split));
				continue;
			case 0:
				/*
				 * Hasn't been written since it was modified,
				 * we want to reference the original page.
				 */
				modified = 0;
				break;
			WT_ILLEGAL_VALUE(session);
			}

		/*
		 * Build the value cell, the child's page address.  In the case
		 * of a page replacement, addr points to the page's replacement
		 * address, else use WT_REF.addr, which points to an on-page
		 * cell or an off-page WT_ADDR structure.   In the case of page
		 * deletion, the cell type has also been set, otherwise use the
		 * information from the addr or original cell.
		 */
		if (addr == NULL && __wt_off_page(page, ref->addr))
			addr = ref->addr;
		if (addr == NULL) {
			__wt_cell_unpack(ref->addr, vpack);
			p = vpack->data;
			size = vpack->size;
			if (vtype == 0)
				vtype = vpack->raw;
		} else {
			p = addr->addr;
			size = addr->size;
			if (vtype == 0)
				vtype = addr->leaf_no_overflow ?
				    WT_CELL_ADDR_LNO : WT_CELL_ADDR;
		}
		__rec_cell_build_addr(r, p, size, vtype, 0);

		/*
		 * Build the key cell.  If this is the first key in a "to be
		 * merged" subtree, use the merge correction key saved in the
		 * top-level parent page when this function was called.
		 *
		 * Truncate any 0th key, internal pages don't need 0th keys.
		 */
		ikey = r->merge_ref == NULL ? ref->u.key : r->merge_ref->u.key;
		r->merge_ref = NULL;
		WT_RET(__rec_cell_build_key(session, r, WT_IKEY_DATA(ikey),
		    r->cell_zero ? 1 : ikey->size, 1, &ovfl_key));
		r->cell_zero = 0;

		/*
		 * Boundary, split or write the page.
		 */
		while (key->len + val->len > r->space_avail) {
			WT_RET(__rec_split(session, r));

			/*
			 * Turn off prefix compression until a full key written
			 * to the new page, and (unless we're already working
			 * with an overflow key), rebuild the key without prefix
			 * compression.
			 */
			r->key_pfx_compress = 0;
			if (!ovfl_key)
				WT_RET(__rec_cell_build_key(
				    session, r, NULL, 0, 1, &ovfl_key));
		}

		/* Copy the key and value onto the page. */
		__rec_copy_incr(session, r, key);
		__rec_copy_incr(session, r, val);

		/* Update compression state. */
		__rec_key_state_update(r, ovfl_key);
	}

	return (0);
}

/*
 * __rec_row_leaf --
 *	Reconcile a row-store leaf page.
 */
static int
__rec_row_leaf(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, WT_PAGE *page, WT_SALVAGE_COOKIE *salvage)
{
	WT_BTREE *btree;
	WT_CELL *cell, *val_cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_DECL_ITEM(tmpkey);
	WT_DECL_ITEM(tmpval);
	WT_DECL_RET;
	WT_IKEY *ikey;
	WT_INSERT *ins;
	WT_KV *key, *val;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint64_t slvg_skip;
	uint32_t i, size;
	int dictionary, onpage_ovfl, ovfl_key;
	const void *p;

	btree = session->btree;
	slvg_skip = salvage == NULL ? 0 : salvage->skip;

	key = &r->k;
	val = &r->v;
	unpack = &_unpack;

	WT_RET(__rec_split_init(session, r, page, 0ULL, btree->maxleafpage));

	/*
	 * Write any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	if ((ins = WT_SKIP_FIRST(WT_ROW_INSERT_SMALLEST(page))) != NULL)
		WT_RET(__rec_row_leaf_insert(session, r, ins));

	/*
	 * Temporary buffers in which to instantiate any uninstantiated keys
	 * or value items we need.
	 */
	WT_RET(__wt_scr_alloc(session, 0, &tmpkey));
	WT_RET(__wt_scr_alloc(session, 0, &tmpval));

	/* For each entry in the page... */
	WT_ROW_FOREACH(page, rip, i) {
		/*
		 * The salvage code, on some rare occasions, wants to reconcile
		 * a page but skip some leading records on the page.  Because
		 * the row-store leaf reconciliation function copies keys from
		 * the original disk page, this is non-trivial -- just changing
		 * the in-memory pointers isn't sufficient, we have to change
		 * the WT_CELL structures on the disk page, too.  It's ugly, but
		 * we pass in a value that tells us how many records to skip in
		 * this case.
		 */
		if (slvg_skip != 0) {
			--slvg_skip;
			continue;
		}

		/*
		 * Set the WT_IKEY reference (if the key was instantiated), and
		 * the key cell reference.
		 */
		ikey = WT_ROW_KEY_COPY(rip);
		if (__wt_off_page(page, ikey))
			cell = WT_PAGE_REF_OFFSET(page, ikey->cell_offset);
		else {
			cell = (WT_CELL *)ikey;
			ikey = NULL;
		}

		/* Build value cell. */
		dictionary = 0;
		if ((val_cell = __wt_row_value(page, rip)) != NULL)
			__wt_cell_unpack(val_cell, unpack);
		WT_ERR(
		    __rec_txn_read(session, r, WT_ROW_UPDATE(page, rip), &upd));
		if (upd == NULL) {
			/*
			 * When the page was read into memory, there may not
			 * have been a value item.
			 *
			 * If there was a value item, check if it's a dictionary
			 * cell (a copy of another item on the page).  If it's a
			 * copy, we have to create a new value item as the old
			 * item might have been discarded from the page.
			 */
			if (val_cell == NULL) {
				val->buf.data = NULL;
				val->buf.size = 0;
				val->cell_len = 0;
				val->len = val->buf.size;
			} else if (unpack->raw == WT_CELL_VALUE_COPY) {
				/* If the item is Huffman encoded, decode it. */
				if (btree->huffman_value == NULL) {
					p = unpack->data;
					size = unpack->size;
				} else {
					WT_ERR(__wt_huffman_decode(session,
					    btree->huffman_value,
					    unpack->data, unpack->size,
					    tmpval));
					p = tmpval->data;
					size = tmpval->size;
				}
				WT_ERR(__rec_cell_build_val(
				    session, r, p, size, (uint64_t)0));
				dictionary = 1;
			} else {
				val->buf.data = val_cell;
				val->buf.size = __wt_cell_total_len(unpack);
				val->cell_len = 0;
				val->len = val->buf.size;

				/* Track if page has overflow items. */
				if (unpack->ovfl)
					r->ovfl_items = 1;
			}
		} else {
			/*
			 * If the original value was an overflow and we've not
			 * already done so, discard it.  One complication: we
			 * must cache a copy before discarding the on-disk
			 * version if there's a transaction in the system that
			 * might read the original value.
			 */
			if (val_cell != NULL && unpack->ovfl) {
				WT_ERR(__wt_rec_track_onpage_addr(
				    session, page, unpack->data, unpack->size));
				WT_ERR(__wt_val_ovfl_cache(
				    session, page, rip, unpack));
			}

			/* If this key/value pair was deleted, we're done. */
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				/*
				 * Overflow keys referencing discarded values
				 * are no longer useful, schedule the discard
				 * of the backing blocks.  Don't worry about
				 * reuse, reusing the key in this reconciliation
				 * is unlikely.
				 *
				 * Keys are part of the name-space though, we
				 * can't remove them from the in-memory tree;
				 * if an overflow key was never instantiated,
				 * do it now.
				 */
				__wt_cell_unpack(cell, unpack);
				if (unpack->ovfl) {
					if (ikey == NULL)
						WT_ERR(__wt_row_key_copy(
						    session, page, rip, NULL));
					WT_ERR(__wt_rec_track_onpage_addr(
					    session, page,
					    unpack->data, unpack->size));
				}

				/*
				 * We aren't actually creating the key so we
				 * can't use bytes from this key to provide
				 * prefix information for a subsequent key.
				 */
				tmpkey->size = 0;

				/* Proceed with appended key/value pairs. */
				goto leaf_insert;
			}

			/*
			 * If no value, nothing needs to be copied.  Otherwise,
			 * build the value's WT_CELL chunk from the most recent
			 * update value.
			 */
			if (upd->size == 0)
				val->cell_len = val->len = val->buf.size = 0;
			else {
				WT_ERR(__rec_cell_build_val(session, r,
				    WT_UPDATE_DATA(upd), upd->size,
				    (uint64_t)0));
				dictionary = 1;
			}
		}

		/*
		 * If the key is an overflow key, check to see if we've entered
		 * the key into the tracking system.  In that case, the original
		 * overflow key blocks have been freed, we have to build a new
		 * key.  If there's no tracking entry, use the original blocks.
		 */
		__wt_cell_unpack(cell, unpack);
		onpage_ovfl = unpack->ovfl;
		if (onpage_ovfl &&
		    __wt_rec_track_onpage_srch(
		    page, unpack->data, unpack->size)) {
			onpage_ovfl = 0;
			WT_ASSERT(session, ikey != NULL);
		}

		/*
		 * Build key cell.
		 */
		if (onpage_ovfl) {
			key->buf.data = cell;
			key->buf.size = __wt_cell_total_len(unpack);
			key->cell_len = 0;
			key->len = key->buf.size;
			ovfl_key = 1;

			/*
			 * We aren't creating a key so we can't use this key as
			 * a prefix for a subsequent key.
			 */
			tmpkey->size = 0;

			/* Track if page has overflow items. */
			r->ovfl_items = 1;
		} else {
			/*
			 * Use an already instantiated key, or
			 * Use the key from the disk image, or
			 * Build a key from a previous key, or
			 * Instantiate the key from scratch.
			 */
			if (ikey != NULL) {
				tmpkey->data = WT_IKEY_DATA(ikey);
				tmpkey->size = ikey->size;
			} else if (btree->huffman_key == NULL &&
			    unpack->type == WT_CELL_KEY &&
			    unpack->prefix == 0) {
				tmpkey->data = unpack->data;
				tmpkey->size = unpack->size;
			} else if (btree->huffman_key == NULL &&
			    unpack->type == WT_CELL_KEY &&
			    tmpkey->size >= unpack->prefix) {
				/*
				 * The previous clause checked for a prefix of
				 * zero, which means the temporary buffer must
				 * have a non-zero size, and it references a
				 * valid key.
				 */
				WT_ASSERT(session, tmpkey->size != 0);

				/*
				 * If we previously built a prefix-compressed
				 * key in the temporary buffer, WT_ITEM->data
				 * will be the same as WT_ITEM->mem: grow the
				 * buffer and copy the suffix into place.
				 *
				 * If we previously pointed the temporary buffer
				 * at an in-memory or on-page key, WT_ITEM->data
				 * will not be the same as WT_ITEM->mem: grow
				 * the buffer, copy the prefix into place, reset
				 * the data field to point to the buffer memory,
				 * then copy the suffix into place.
				 */
				WT_ERR(__wt_buf_grow(session,
				    tmpkey, unpack->prefix + unpack->size));
				if (tmpkey->data != tmpkey->mem) {
					memcpy(tmpkey->mem, tmpkey->data,
					    unpack->prefix);
					tmpkey->data = tmpkey->mem;
				}
				memcpy((uint8_t *)tmpkey->data + unpack->prefix,
				    unpack->data, unpack->size);
				tmpkey->size = unpack->prefix + unpack->size;
			} else
				WT_ERR(__wt_row_key_copy(
				    session, page, rip, tmpkey));

			WT_ERR(__rec_cell_build_key(session, r,
			    tmpkey->data, tmpkey->size, 0, &ovfl_key));
		}

		/*
		 * Boundary, split or write the page.
		 *
		 * We write a trailing key cell on the page after the K/V pairs
		 * (see WT_TRAILING_KEY_CELL for more information).
		 */
		while (key->len +
		    val->len + WT_TRAILING_KEY_CELL > r->space_avail) {
			/*
			 * In one path above, we copied the key from the page
			 * rather than building the actual key.  In that case,
			 * we have to build the actual key now because we are
			 * about to promote it.
			 */
			if (onpage_ovfl) {
				WT_ERR(__wt_cell_unpack_copy(
				    session, unpack, r->cur));
				onpage_ovfl = 0;
			}
			WT_ERR(__rec_split(session, r));

			/*
			 * Turn off prefix compression until a full key written
			 * to the new page, and (unless we're already working
			 * with an overflow key), rebuild the key without prefix
			 * compression.
			 */
			r->key_pfx_compress = 0;
			if (!ovfl_key)
				WT_ERR(__rec_cell_build_key(
				    session, r, NULL, 0, 0, &ovfl_key));
		}

		/* Copy the key/value pair onto the page. */
		__rec_copy_incr(session, r, key);
		if (val->len != 0) {
			if (dictionary && r->dictionary != NULL)
				WT_ERR(__rec_dict_replace(session, r, 0, val));
			__rec_copy_incr(session, r, val);
		}

		/* Update compression state. */
		__rec_key_state_update(r, ovfl_key);

leaf_insert:	/* Write any K/V pairs inserted into the page after this key. */
		if ((ins = WT_SKIP_FIRST(WT_ROW_INSERT(page, rip))) != NULL)
			WT_ERR(__rec_row_leaf_insert(session, r, ins));
	}

	/* Write the remnant page. */
	ret = __rec_split_finish(session, r);

err:	__wt_scr_free(&tmpkey);
	__wt_scr_free(&tmpval);
	return (ret);
}

/*
 * __rec_row_leaf_insert --
 *	Walk an insert chain, writing K/V pairs.
 */
static int
__rec_row_leaf_insert(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins)
{
	WT_KV *key, *val;
	WT_UPDATE *upd;
	int ovfl_key;

	key = &r->k;
	val = &r->v;

	for (; ins != NULL; ins = WT_SKIP_NEXT(ins)) {
		/* Build value cell. */
		WT_RET(__rec_txn_read(session, r, ins->upd, &upd));
		if (upd == NULL || WT_UPDATE_DELETED_ISSET(upd))
			continue;
		if (upd->size == 0)
			val->len = 0;
		else
			WT_RET(__rec_cell_build_val(session, r,
			    WT_UPDATE_DATA(upd), upd->size, (uint64_t)0));

		WT_RET(__rec_cell_build_key(session, r,	/* Build key cell. */
		    WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins), 0, &ovfl_key));

		/*
		 * Boundary, split or write the page.
		 *
		 * We write a trailing key cell on the page after the K/V pairs
		 * (see WT_TRAILING_KEY_CELL for more information).
		 */
		while (key->len +
		    val->len + WT_TRAILING_KEY_CELL > r->space_avail) {
			WT_RET(__rec_split(session, r));

			/*
			 * Turn off prefix compression until a full key written
			 * to the new page, and (unless we're already working
			 * with an overflow key), rebuild the key without prefix
			 * compression.
			 */
			r->key_pfx_compress = 0;
			if (!ovfl_key)
				WT_RET(__rec_cell_build_key(
				    session, r, NULL, 0, 0, &ovfl_key));
		}

		/* Copy the key/value pair onto the page. */
		__rec_copy_incr(session, r, key);
		if (val->len != 0) {
			if (r->dictionary != NULL)
				WT_RET(__rec_dict_replace(session, r, 0, val));
			__rec_copy_incr(session, r, val);
		}

		/* Update compression state. */
		__rec_key_state_update(r, ovfl_key);
	}

	return (0);
}

/*
 * __rec_split_discard --
 *	Discard the pages resulting from a previous split.
 */
static int
__rec_split_discard(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;
	WT_REF *ref;
	uint32_t i;

	/*
	 * A page that split is being reconciled for the second, or subsequent
	 * time; discard any the underlying block space or overflow items used
	 * in the previous reconciliation.
	 *
	 * This routine would be trivial, and only walk a single page freeing
	 * any blocks that were written to support the split -- the problem is
	 * root splits.  In the case of root splits, we potentially have to
	 * cope with the underlying blocks of multiple pages, but also there
	 * may be overflow items that we have to resolve.
	 *
	 * These pages are discarded -- add them to the object tracking list.
	 */
	WT_REF_FOREACH(page, ref, i)
		WT_RET(__wt_rec_track(session, page,
		    ((WT_ADDR *)ref->addr)->addr,
		    ((WT_ADDR *)ref->addr)->size, NULL, 0, 0));
	WT_RET(__wt_rec_track_wrapup(session, page));

	if ((mod = page->modify) != NULL)
		switch (F_ISSET(mod, WT_PM_REC_MASK)) {
		case WT_PM_REC_SPLIT_MERGE:
			/*
			 * NOT root page split: this is the split merge page for
			 * a normal page split, and we don't need to do anything
			 * further.
			 */
			break;
		case WT_PM_REC_SPLIT:
			/*
			 * Root page split: continue walking the list of split
			 * pages, cleaning up as we go.
			 */
			WT_RET(__rec_split_discard(session, mod->u.split));
			break;
		case WT_PM_REC_REPLACE:
			/*
			 * Root page split: the last entry on the list.  There
			 * won't be a page to discard because writing the page
			 * created a checkpoint, not a replacement page.
			 */
			WT_ASSERT(session, mod->u.replace.addr == NULL);
			break;
		WT_ILLEGAL_VALUE(session);
		}
	return (0);
}

/*
 * __rec_write_wrapup  --
 *	Finish the reconciliation.
 */
static int
__rec_write_wrapup(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_BOUNDARY *bnd;
	WT_DECL_RET;
	WT_PAGE_MODIFY *mod;
	uint32_t i;

	btree = session->btree;
	mod = page->modify;

	/*
	 * This page may have previously been reconciled, and that information
	 * is now about to be replaced.  Make sure it's discarded at some point,
	 * and clear the underlying modification information, we're creating a
	 * new reality.
	 */
	switch (F_ISSET(mod, WT_PM_REC_MASK)) {
	case 0:	/*
		 * The page has never been reconciled before, track the original
		 * address blocks (if any).  The "if any" is for empty trees we
		 * create when a new tree is opened, and for previously deleted
		 * pages that are instantiated in memory.
		 *
		 * The exception is root pages are never tracked or free'd, they
		 * are checkpoints, and must be explicitly dropped.
		 */
		if (!WT_PAGE_IS_ROOT(page) && page->ref->addr != NULL)
			WT_RET(__wt_rec_track_onpage_ref(
			    session, page, page->parent, page->ref));
		break;
	case WT_PM_REC_EMPTY:				/* Page deleted */
		break;
	case WT_PM_REC_REPLACE:				/* 1-for-1 page swap */
		/*
		 * Discard the replacement leaf page's blocks.
		 *
		 * The exception is root pages are never tracked or free'd, they
		 * are checkpoints, and must be explicitly dropped.
		 */
		if (!WT_PAGE_IS_ROOT(page))
			WT_RET(__wt_rec_track(session, page,
			    mod->u.replace.addr, mod->u.replace.size,
			    NULL, 0, 0));

		/* Discard the replacement page's address. */
		__wt_free(session, mod->u.replace.addr);
		mod->u.replace.addr = NULL;
		mod->u.replace.size = 0;
		break;
	case WT_PM_REC_SPLIT:				/* Page split */
		/* Discard the split page. */
		WT_RET(__rec_split_discard(session, mod->u.split));
		__wt_page_out(session, &mod->u.split, 0);
		mod->u.split = NULL;
		break;
	case WT_PM_REC_SPLIT_MERGE:			/* Page split */
		/*
		 * We should never be here with a split-merge page: you cannot
		 * reconcile split-merge pages, they can only be merged into a
		 * parent.
		 */
		/* FALLTHROUGH */
	WT_ILLEGAL_VALUE(session);
	}
	F_CLR(mod, WT_PM_REC_MASK);

	/*
	 * Wrap up discarded block and overflow tracking.  If we are about to
	 * create a checkpoint, the system must be entirely consistent at that
	 * point, the underlying block manager is presumably going to do some
	 * action to resolve the list of allocated/free/whatever blocks that
	 * are associated with the checkpoint.
	 */
	WT_RET(__wt_rec_track_wrapup(session, page));

	switch (r->bnd_next) {
	case 0:						/* Page delete */
		WT_VERBOSE_RET(session, reconcile, "page %p empty", page);
		WT_BSTAT_INCR(session, rec_page_delete);

		/* If this is the root page, we need to create a sync point. */
		if (WT_PAGE_IS_ROOT(page))
			WT_RET(__wt_bm_checkpoint(session, NULL, btree->ckpt));

		/*
		 * If the page was empty, we want to discard it from the tree
		 * by discarding the parent's key when evicting the parent.
		 * Mark the page as deleted, then return success, leaving the
		 * page in memory.  If the page is subsequently modified, that
		 * is OK, we'll just reconcile it again.
		 */
		F_SET(mod, WT_PM_REC_EMPTY);
		break;
	case 1:						/* 1-for-1 page swap */
		/*
		 * Because WiredTiger's pages grow without splitting, we're
		 * replacing a single page with another single page most of
		 * the time.
		 *
		 * If this is a root page, then we don't have an address and we
		 * have to create a sync point.  The address was cleared when
		 * we were about to write the buffer so we know what to do here.
		 */
		bnd = &r->bnd[0];
		if (bnd->addr.addr == NULL)
			WT_RET(
			    __wt_bm_checkpoint(session, &r->dsk, btree->ckpt));
		else {
			mod->u.replace = bnd->addr;
			bnd->addr.addr = NULL;
		}

		F_SET(mod, WT_PM_REC_REPLACE);
		break;
	default:					/* Page split */
		WT_VERBOSE_RET(session, reconcile,
		    "page %p split into %" PRIu32 " pages",
		    page, r->bnd_next);

		switch (page->type) {
		case WT_PAGE_COL_INT:
		case WT_PAGE_ROW_INT:
			WT_BSTAT_INCR(session, rec_split_intl);
			break;
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
		case WT_PAGE_ROW_LEAF:
			WT_BSTAT_INCR(session, rec_split_leaf);
			break;
		WT_ILLEGAL_VALUE(session);
		}

#ifdef HAVE_VERBOSE
		if (WT_VERBOSE_ISSET(session, reconcile)) {
			WT_DECL_ITEM(tkey);
			if (page->type == WT_PAGE_ROW_INT ||
			    page->type == WT_PAGE_ROW_LEAF)
				WT_RET(__wt_scr_alloc(session, 0, &tkey));
			for (bnd = r->bnd, i = 0; i < r->bnd_next; ++bnd, ++i)
				switch (page->type) {
				case WT_PAGE_ROW_INT:
				case WT_PAGE_ROW_LEAF:
					WT_ERR(__wt_buf_set_printable(
					    session, tkey,
					    bnd->key.data, bnd->key.size));
					WT_VERBOSE_ERR(session, reconcile,
					    "split: starting key "
					    "%.*s",
					    (int)tkey->size,
					    (char *)tkey->data);
					break;
				case WT_PAGE_COL_FIX:
				case WT_PAGE_COL_INT:
				case WT_PAGE_COL_VAR:
					WT_VERBOSE_ERR(session, reconcile,
					    "split: starting recno %" PRIu64,
					    bnd->recno);
					break;
				WT_ILLEGAL_VALUE_ERR(session);
				}
err:			__wt_scr_free(&tkey);
			WT_RET(ret);
		}
#endif
		switch (page->type) {
		case WT_PAGE_ROW_INT:
		case WT_PAGE_ROW_LEAF:
			WT_RET(
			    __rec_split_row(session, r, page, &mod->u.split));
			break;
		case WT_PAGE_COL_INT:
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			WT_RET(
			    __rec_split_col(session, r, page, &mod->u.split));
			break;
		WT_ILLEGAL_VALUE(session);
		}

		F_SET(mod, WT_PM_REC_SPLIT);
		break;
	}

	/*
	 * Success.
	 *
	 * If modifications were skipped, the tree isn't clean.  The checkpoint
	 * call cleared the tree's modified value before it called the eviction
	 * thread, so we must explicitly reset the tree's modified flag.  We
	 * publish the change for clarity (the requirement is the value be set
	 * before a subsequent checkpoint reads it, and because the current
	 * checkpoint is waiting on this reconciliation to complete, there's no
	 * risk of that happening).
	 */
	if (r->upd_skipped)
		WT_PUBLISH(btree->modified, 1);

	/*
	 * If modifications were not skipped, the page might be clean; update
	 * the disk generation to the write generation as of when reconciliation
	 * started.
	 */
	if (!r->upd_skipped)
		mod->disk_gen = r->orig_write_gen;

	return (0);
}

/*
 * __rec_write_wrapup_err  --
 *	Finish the reconciliation on error.
 */
static int
__rec_write_wrapup_err(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_BOUNDARY *bnd;
	WT_DECL_RET;
	uint32_t i;

	/*
	 * On error, discard pages we've written, they're unreferenced by the
	 * tree.  This is not a question of correctness, we're avoiding block
	 * leaks.
	 */
	WT_TRET(__wt_rec_track_wrapup_err(session, page));
	for (bnd = r->bnd, i = 0; i < r->bnd_next; ++bnd, ++i)
		if (bnd->addr.addr != NULL) {
			WT_TRET(__wt_bm_free(
			    session, bnd->addr.addr, bnd->addr.size));
			bnd->addr.addr = NULL;
		}
	return (ret);
}

/*
 * __rec_split_row --
 *	Split a row-store page, creating a new internal page.
 */
static int
__rec_split_row(
    WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *orig, WT_PAGE **splitp)
{
	WT_ADDR *addr;
	WT_BOUNDARY *bnd;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_REF *ref;
	uint32_t i;

	/* Allocate a row-store internal page. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc_def(session, (size_t)r->bnd_next, &page->u.intl.t));

	/* Fill it in. */
	page->parent = orig->parent;
	page->ref = orig->ref;
	page->read_gen = __wt_cache_read_gen(session);
	page->entries = r->bnd_next;
	page->type = WT_PAGE_ROW_INT;

	/*
	 * We don't re-write parent pages when child pages split, which means
	 * we have only one slot to work with in the parent.  When a leaf page
	 * splits, we create a new internal page referencing the split pages,
	 * and when the leaf page is evicted, we update the leaf's slot in the
	 * parent to reference the new internal page in the tree (deepening the
	 * tree by a level).  We don't want the tree to deepen permanently, so
	 * we never write that new internal page to disk, we only merge it into
	 * the parent when the parent page is evicted.
	 *
	 * We set one flag (WT_PM_REC_SPLIT) on the original page so future
	 * reconciliations of its parent merge in the newly created split page.
	 * We set a different flag (WT_PM_REC_SPLIT_MERGE) on the created
	 * split page so after we evict the original page and replace it with
	 * the split page, the parent continues to merge in the split page.
	 * The flags are different because the original page can be evicted and
	 * its memory discarded, but the newly created split page cannot be
	 * evicted, it can only be merged into its parent.
	 */
	WT_ERR(__wt_page_modify_init(session, page));
	F_SET(page->modify, WT_PM_REC_SPLIT_MERGE);

	/* Enter each split page into the new, internal page. */
	for (ref = page->u.intl.t,
	    bnd = r->bnd, i = 0; i < r->bnd_next; ++ref, ++bnd, ++i) {
		WT_ERR(__wt_calloc(session, 1, sizeof(WT_ADDR), &addr));
		*addr = bnd->addr;
		bnd->addr.addr = NULL;

		ref->page = NULL;
		WT_ERR(__wt_row_ikey_alloc(session, 0,
		    bnd->key.data, bnd->key.size, &ref->u.key));
		ref->addr = addr;
		ref->state = WT_REF_DISK;
	}

	*splitp = page;
	return (0);

err:	__wt_page_out(session, &page, 0);
	return (ret);
}

/*
 * __rec_split_col --
 *	Split a column-store page, creating a new internal page.
 */
static int
__rec_split_col(
    WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *orig, WT_PAGE **splitp)
{
	WT_ADDR *addr;
	WT_BOUNDARY *bnd;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_REF *ref;
	uint32_t i;

	/* Allocate a column-store internal page. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc_def(session, (size_t)r->bnd_next, &page->u.intl.t));

	/* Fill it in. */
	page->parent = orig->parent;
	page->ref = orig->ref;
	page->read_gen = __wt_cache_read_gen(session);
	page->u.intl.recno = r->bnd[0].recno;
	page->entries = r->bnd_next;
	page->type = WT_PAGE_COL_INT;

	/*
	 * See the comment above in __rec_split_row().
	 */
	WT_ERR(__wt_page_modify_init(session, page));
	F_SET(page->modify, WT_PM_REC_SPLIT_MERGE);

	/* Enter each split page into the new, internal page. */
	for (ref = page->u.intl.t,
	    bnd = r->bnd, i = 0; i < r->bnd_next; ++ref, ++bnd, ++i) {
		WT_ERR(__wt_calloc(session, 1, sizeof(WT_ADDR), &addr));
		*addr= bnd->addr;
		bnd->addr.addr = NULL;

		ref->page = NULL;
		ref->u.recno = bnd->recno;
		ref->addr = addr;
		ref->state = WT_REF_DISK;
	}

	*splitp = page;
	return (0);

err:	__wt_page_out(session, &page, 0);
	return (ret);
}

/*
 * __rec_cell_build_key --
 *	Process a key and return a WT_CELL structure and byte string to be
 * stored on the page.
 */
static int
__rec_cell_build_key(WT_SESSION_IMPL *session, WT_RECONCILE *r,
    const void *data, uint32_t size, int is_internal, int *is_ovflp)
{
	WT_BTREE *btree;
	WT_KV *key;
	uint32_t pfx_max;
	uint8_t pfx;
	const uint8_t *a, *b;

	btree = session->btree;
	key = &r->k;
	*is_ovflp = 0;

	pfx = 0;
	if (data == NULL)
		/*
		 * When data is NULL, our caller has a prefix compressed key
		 * they can't use (probably because they just crossed a split
		 * point).  Use the full key saved when last called, instead.
		 */
		WT_RET(__wt_buf_set(
		    session, &key->buf, r->cur->data, r->cur->size));
	else {
		/*
		 * Save a copy of the key for later reference: we use the full
		 * key for prefix-compression comparisons, and if we are, for
		 * any reason, unable to use the compressed key we generate.
		 */
		WT_RET(__wt_buf_set(session, r->cur, data, size));

		/*
		 * Do prefix compression on the key.  We know by definition the
		 * previous key sorts before the current key, which means the
		 * keys must differ and we just need to compare up to the
		 * shorter of the two keys.   Also, we can't compress out more
		 * than 256 bytes, limit the comparison to that.
		 */
		if (r->key_pfx_compress) {
			pfx_max = UINT8_MAX;
			if (size < pfx_max)
				pfx_max = size;
			if (r->last->size < pfx_max)
				pfx_max = r->last->size;
			for (a = data, b = r->last->data; pfx < pfx_max; ++pfx)
				if (*a++ != *b++)
					break;
		}

		/* Copy the non-prefix bytes into the key buffer. */
		WT_RET(__wt_buf_set(
		    session, &key->buf, (uint8_t *)data + pfx, size - pfx));
	}

	/* Optionally compress the value using the Huffman engine. */
	if (btree->huffman_key != NULL)
		WT_RET(__wt_huffman_encode(session, btree->huffman_key,
		    key->buf.data, key->buf.size, &key->buf));

	/* Create an overflow object if the data won't fit. */
	if (key->buf.size >
	    (is_internal ? btree->maxintlitem : btree->maxleafitem)) {
		/*
		 * Overflow objects aren't prefix compressed -- rebuild any
		 * object that was prefix compressed.
		 */
		if (pfx == 0) {
			WT_BSTAT_INCR(session, rec_ovfl_key);

			*is_ovflp = 1;
			return (__rec_cell_build_ovfl(
			    session, r, key, WT_CELL_KEY_OVFL, (uint64_t)0));
		}
		return (__rec_cell_build_key(
		    session, r, NULL, 0, is_internal, is_ovflp));
	}

	key->cell_len = __wt_cell_pack_key(&key->cell, pfx, key->buf.size);
	key->len = key->cell_len + key->buf.size;

	return (0);
}

/*
 * __rec_cell_build_addr --
 *	Process an address reference and return a cell structure to be stored
 * on the page.
 */
static void
__rec_cell_build_addr(WT_RECONCILE *r,
    const void *addr, uint32_t size, u_int cell_type, uint64_t recno)
{
	WT_KV *val;

	val = &r->v;

	/*
	 * We don't check the address size because we can't store an address on
	 * an overflow page: if the address won't fit, the overflow page's
	 * address won't fit either.  This possibility must be handled by Btree
	 * configuration, we have to disallow internal page sizes that are too
	 * small with respect to the largest address cookie the underlying block
	 * manager might return.
	 */

	/*
	 * We don't copy the data into the buffer, it's not necessary; just
	 * re-point the buffer's data/length fields.
	 */
	val->buf.data = addr;
	val->buf.size = size;
	val->cell_len = __wt_cell_pack_addr(
	    &val->cell, cell_type, recno, val->buf.size);
	val->len = val->cell_len + val->buf.size;
}

/*
 * __rec_cell_build_val --
 *	Process a data item and return a WT_CELL structure and byte string to
 * be stored on the page.
 */
static int
__rec_cell_build_val(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, const void *data, uint32_t size, uint64_t rle)
{
	WT_BTREE *btree;
	WT_KV *val;

	btree = session->btree;
	val = &r->v;

	/*
	 * We don't copy the data into the buffer, it's not necessary; just
	 * re-point the buffer's data/length fields.
	 */
	val->buf.data = data;
	val->buf.size = size;

	/* Handle zero-length cells quickly. */
	if (size != 0) {
		/* Optionally compress the data using the Huffman engine. */
		if (btree->huffman_value != NULL)
			WT_RET(__wt_huffman_encode(
			    session, btree->huffman_value,
			    val->buf.data, val->buf.size, &val->buf));

		/* Create an overflow object if the data won't fit. */
		if (val->buf.size > btree->maxleafitem) {
			WT_BSTAT_INCR(session, rec_ovfl_value);

			return (__rec_cell_build_ovfl(
			    session, r, val, WT_CELL_VALUE_OVFL, rle));
		}
	}
	val->cell_len = __wt_cell_pack_data(&val->cell, rle, val->buf.size);
	val->len = val->cell_len + val->buf.size;

	return (0);
}

/*
 * __rec_cell_build_ovfl --
 *	Store overflow items in the file, returning the address cookie.
 */
static int
__rec_cell_build_ovfl(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, WT_KV *kv, uint8_t type, uint64_t rle)
{
	WT_BTREE *btree;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_HEADER *dsk;
	uint32_t size;
	int found;
	uint8_t *addr, buf[WT_BTREE_MAX_ADDR_COOKIE];

	btree = session->btree;
	page = r->page;

	/* Track if page has overflow items. */
	r->ovfl_items = 1;

	/*
	 * See if this overflow record has already been written and reuse it if
	 * possible.  Else, write a new overflow record.
	 */
	WT_RET(__wt_rec_track_ovfl_reuse(
	    session, page, kv->buf.data, kv->buf.size, &addr, &size, &found));
	if (!found) {
		/* Allocate a buffer big enough to write the overflow record. */
		size = kv->buf.size;
		WT_RET(__wt_bm_write_size(session, &size));
		WT_RET(__wt_scr_alloc(session, size, &tmp));

		/* Initialize the buffer: disk header and overflow record. */
		dsk = tmp->mem;
		memset(dsk, 0, WT_PAGE_HEADER_SIZE);
		dsk->type = WT_PAGE_OVFL;
		dsk->u.datalen = kv->buf.size;
		memcpy(WT_PAGE_HEADER_BYTE(btree, dsk),
		    kv->buf.data, kv->buf.size);
		tmp->size = WT_PAGE_HEADER_BYTE_SIZE(btree) + kv->buf.size;

		/* Write the buffer. */
		addr = buf;
		WT_ERR(__wt_bm_write(session, tmp, addr, &size));

		/* Track the overflow record. */
		WT_ERR(__wt_rec_track(session, page,
		    addr, size, kv->buf.data, kv->buf.size, WT_TRK_INUSE));
	}

	/* Set the callers K/V to reference the overflow record's address. */
	WT_ERR(__wt_buf_set(session, &kv->buf, addr, size));

	/* Build the cell and return. */
	kv->cell_len = __wt_cell_pack_ovfl(&kv->cell, type, rle, kv->buf.size);
	kv->len = kv->cell_len + kv->buf.size;

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * The dictionary --
 *	The rest of this file is support for dictionaries.
 *
 * It's difficult to write generic skiplist functions without turning a single
 * memory allocation into two, or requiring a function call instead of a simple
 * comparison.  Fortunately, skiplists are relatively simple things and we can
 * include them in-place.  If you need generic skip-list functions to modify,
 * this set wouldn't be a bad place to start.
 *
 * __rec_dictionary_skip_search --
 *	Search a dictionary skiplist.
 */
static WT_DICTIONARY *
__rec_dictionary_skip_search(WT_DICTIONARY **head, uint64_t hash)
{
	WT_DICTIONARY **e;
	int i;

	/*
	 * Start at the highest skip level, then go as far as possible at each
	 * level before stepping down to the next.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, e = &head[i]; i >= 0;)
		if (*e == NULL) {
			--i;
			--e;
		} else {
			if ((*e)->hash == hash)
				return (*e);
			if ((*e)->hash > hash)
				return (NULL);
			e = &(*e)->next[i];
		}

	/* NOTREACHED */
	return (NULL);
}

/*
 * __rec_dictionary_skip_search_stack --
 *	Search a dictionary skiplist, returning an insert/remove stack.
 */
static void
__rec_dictionary_skip_search_stack(
    WT_DICTIONARY **head, WT_DICTIONARY ***stack, uint64_t hash)
{
	WT_DICTIONARY **e;
	int i;

	/*
	 * Start at the highest skip level, then go as far as possible at each
	 * level before stepping down to the next.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, e = &head[i]; i >= 0;)
		if (*e == NULL || (*e)->hash >= hash)
			stack[i--] = e--;
		else
			e = &(*e)->next[i];
}

/*
 * __rec_dictionary_skip_insert --
 *	Insert an entry into the dictionary skip-list.
 */
static void
__rec_dictionary_skip_insert(
    WT_DICTIONARY **head, WT_DICTIONARY *e, uint64_t hash)
{
	WT_DICTIONARY **stack[WT_SKIP_MAXDEPTH];
	u_int i;

	/* Insert the new entry into the skiplist. */
	__rec_dictionary_skip_search_stack(head, stack, hash);
	for (i = 0; i < e->depth; ++i) {
		e->next[i] = *stack[i];
		*stack[i] = e;
	}
}

/*
 * __rec_dictionary_init --
 *	Allocate and initialize the dictionary.
 */
static int
__rec_dictionary_init(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
	u_int depth, i;

	/* Free any previous dictionary. */
	__rec_dictionary_free(session, r);

	WT_RET(__wt_calloc(session,
	    r->dictionary_slots, sizeof(WT_DICTIONARY *), &r->dictionary));
	for (i = 0; i < r->dictionary_slots; ++i) {
		depth = __wt_skip_choose_depth();
		WT_RET(__wt_calloc(session, 1,
		    sizeof(WT_DICTIONARY) + depth * sizeof(WT_DICTIONARY *),
		    &r->dictionary[i]));
		r->dictionary[i]->depth = depth;
	}
	return (0);
}

/*
 * __rec_dictionary_free --
 *	Free the dictionary.
 */
static void
__rec_dictionary_free(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
	u_int i;

	if (r->dictionary == NULL)
		return;

	/*
	 * We don't correct dictionary_slots when we fail during allocation,
	 * but that's OK, the value is either NULL or a memory reference to
	 * be free'd.
	 */
	for (i = 0; i < r->dictionary_slots; ++i)
		__wt_free(session, r->dictionary[i]);
	__wt_free(session, r->dictionary);
}

/*
 * __rec_dictionary_reset --
 *	Reset the dictionary when reconciliation restarts and when crossing a
 * page boundary (a potential split).
 */
static void
__rec_dictionary_reset(WT_RECONCILE *r)
{
	r->dictionary_next = 0;
	memset(r->dictionary_head, 0, sizeof(r->dictionary_head));
}

/*
 * __rec_dictionary_lookup --
 *	Check the dictionary for a matching value on this page.
 */
static int
__rec_dictionary_lookup(
    WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_KV *val, WT_DICTIONARY **dpp)
{
	WT_DICTIONARY *dp, *next;
	uint64_t hash;
	int match;

	*dpp = NULL;

	/* Search the dictionary, and return any match we find. */
	hash = __wt_hash_fnv64(val->buf.data, val->buf.size);
	for (dp = __rec_dictionary_skip_search(r->dictionary_head, hash);
	     dp != NULL && dp->hash == hash; dp = dp->next[0]) {
		WT_RET(__wt_cell_pack_data_match(
		    dp->cell, &val->cell, val->buf.data, &match));
		if (match) {
			WT_BSTAT_INCR(session, rec_dictionary);
			*dpp = dp;
			return (0);
		}
	}

	/*
	 * We're not doing value replacement in the dictionary.  We stop adding
	 * new entries if we run out of empty dictionary slots (but continue to
	 * use the existing entries).  I can't think of any reason a leaf page
	 * value is more likely to be seen because it was seen more recently
	 * than some other value: if we find working sets where that's not the
	 * case, it shouldn't be too difficult to maintain a pointer which is
	 * the next dictionary slot to re-use.
	 */
	if (r->dictionary_next >= r->dictionary_slots)
		return (0);

	/*
	 * Set the hash value, we'll add this entry into the dictionary when we
	 * write it into the page's disk image buffer (because that's when we
	 * know where on the page it will be written).
	 */
	next = r->dictionary[r->dictionary_next++];
	next->cell = NULL;		/* Not necessary, just cautious. */
	next->hash = hash;
	__rec_dictionary_skip_insert(r->dictionary_head, next, hash);
	*dpp = next;
	return (0);
}
