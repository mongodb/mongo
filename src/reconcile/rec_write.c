/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
	WT_REF  *ref;			/* Page being reconciled */
	WT_PAGE *page;
	uint32_t flags;			/* Caller's configuration */

	WT_ITEM	 dsk;			/* Temporary disk-image buffer */

	/* Track whether all changes to the page are written. */
	uint64_t max_txn;
	uint64_t first_dirty_txn;
	uint32_t orig_write_gen;

	/*
	 * If page updates are skipped because they are as yet unresolved, or
	 * the page has updates we cannot discard, the page is left "dirty":
	 * the page cannot be discarded and a subsequent reconciliation will
	 * be necessary to discard the page.
	 */
	int	 leave_dirty;

	/*
	 * Raw compression (don't get me started, as if normal reconciliation
	 * wasn't bad enough).  If an application wants absolute control over
	 * what gets written to disk, we give it a list of byte strings and it
	 * gives us back an image that becomes a file block.  Because we don't
	 * know the number of items we're storing in a block until we've done
	 * a lot of work, we turn off most compression: dictionary, copy-cell,
	 * prefix and row-store internal page suffix compression are all off.
	 */
	int	  raw_compression;
	uint32_t  raw_max_slots;	/* Raw compression array sizes */
	uint32_t *raw_entries;		/* Raw compression slot entries */
	uint32_t *raw_offsets;		/* Raw compression slot offsets */
	uint64_t *raw_recnos;		/* Raw compression recno count */
	WT_ITEM	  raw_destination;	/* Raw compression destination buffer */

	/*
	 * Track if reconciliation has seen any overflow items.  If a leaf page
	 * with no overflow items is written, the parent page's address cell is
	 * set to the leaf-no-overflow type.  This means we can delete the leaf
	 * page without reading it because we don't have to discard any overflow
	 * items it might reference.
	 *
	 * The test test is per-page reconciliation, that is, once we see an
	 * overflow item on the page, all subsequent leaf pages written for the
	 * page will not be leaf-no-overflow type, regardless of whether or not
	 * they contain overflow items.  In other words, leaf-no-overflow is not
	 * guaranteed to be set on every page that doesn't contain an overflow
	 * item, only that if it is set, the page contains no overflow items.
	 *
	 * The reason is because of raw compression: there's no easy/fast way to
	 * figure out if the rows selected by raw compression included overflow
	 * items, and the optimization isn't worth another pass over the data.
	 */
	int	ovfl_items;

	/*
	 * Track if reconciliation of a row-store leaf page has seen empty (zero
	 * length) values.  We don't write out anything for empty values, so if
	 * there are empty values on a page, we have to make two passes over the
	 * page when it's read to figure out how many keys it has, expensive in
	 * the common case of no empty values and (entries / 2) keys.  Likewise,
	 * a page with only empty values is another common data set, and keys on
	 * that page will be equal to the number of entries.  In both cases, set
	 * a flag in the page's on-disk header.
	 *
	 * The test is per-page reconciliation as described above for the
	 * overflow-item test.
	 */
	int	all_empty_value, any_empty_value;

	/*
	 * Reconciliation gets tricky if we have to split a page, which happens
	 * when the disk image we create exceeds the page type's maximum disk
	 * image size.
	 *
	 * First, the sizes of the page we're building.  If WiredTiger is doing
	 * page layout, page_size is the same as page_size_orig. We accumulate
	 * a "page size" of raw data and when we reach that size, we split the
	 * page into multiple chunks, eventually compressing those chunks.  When
	 * the application is doing page layout (raw compression is configured),
	 * page_size can continue to grow past page_size_orig, and we keep
	 * accumulating raw data until the raw compression callback accepts it.
	 */
	uint32_t page_size;		/* Set page size */
	uint32_t page_size_orig;	/* Saved set page size */

	/*
	 * Second, the split size: if we're doing the page layout, split to a
	 * smaller-than-maximum page size when a split is required so we don't
	 * repeatedly split a packed page.
	 */
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
		 * Offset is the byte offset in the initial split buffer of the
		 * first byte of the split chunk, recorded before we decide to
		 * split the page; the difference between chunk[1]'s offset and
		 * chunk[0]'s offset is chunk[0]'s length.
		 *
		 * Once we split a page, we stop filling in offset values, we're
		 * writing the split chunks as we find them.
		 */
		size_t offset;		/* Split's first byte */

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
		uint32_t size;		/* Split's size */
		uint32_t cksum;		/* Split's checksum */
		void    *dsk;		/* Split's disk image */

		/*
		 * When busy pages get large, we need to be able to evict them
		 * even when they contain unresolved updates, or updates which
		 * cannot be evicted because of running transactions.  In such
		 * cases, break the page into multiple blocks, write the blocks
		 * that can be evicted, saving lists of updates for blocks that
		 * cannot be evicted, then re-instantiate the blocks that cannot
		 * be evicted as new, in-memory pages, restoring the updates on
		 * those pages.
		 */
		WT_UPD_SKIPPED *skip;	/* Skipped updates */
		uint32_t	skip_next;
		size_t		skip_allocated;

		/*
		 * The key for a row-store page; no column-store key is needed
		 * because the page's recno, stored in the recno field, is the
		 * column-store key.
		 */
		WT_ITEM key;		/* Promoted row-store key */

		/*
		 * During wrapup, after reconciling the root page, we write a
		 * final block as part of a checkpoint.  If raw compression
		 * was configured, that block may have already been compressed.
		 */
		int already_compressed;
	} *bnd;				/* Saved boundaries */
	uint32_t bnd_next;		/* Next boundary slot */
	uint32_t bnd_next_max;		/* Maximum boundary slots used */
	size_t	 bnd_entries;		/* Total boundary slots */
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
		SPLIT_TRACKING_OFF=2,	/* No boundary checks */
		SPLIT_TRACKING_RAW=3 }	/* Underlying compression decides */
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
	size_t	 space_avail;		/* Remaining space in this chunk */

	/*
	 * While reviewing updates for each page, we store skipped updates here,
	 * and then move them to per-block areas as the blocks are defined.
	 */
	WT_UPD_SKIPPED *skip;		/* Skipped updates */
	uint32_t	skip_next;
	size_t		skip_allocated;

	/*
	 * We don't need to keep the 0th key around on internal pages, the
	 * search code ignores them as nothing can sort less by definition.
	 * There's some trickiness here, see the code for comments on how
	 * these fields work.
	 */
	int	 cell_zero;		/* Row-store internal page 0th key */

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
		size_t cell_len;
		size_t len;		/* Total length of cell + data */
	} k, v;				/* Key/Value being built */

	WT_ITEM *cur, _cur;		/* Key/Value being built */
	WT_ITEM *last, _last;		/* Last key/value built */

	int key_pfx_compress;		/* If can prefix-compress next key */
	int key_pfx_compress_conf;	/* If prefix compression configured */
	int key_sfx_compress;		/* If can suffix-compress next key */
	int key_sfx_compress_conf;	/* If suffix compression configured */

	int is_bulk_load;		/* If it's a bulk load */

	WT_SALVAGE_COOKIE *salvage;	/* If it's a salvage operation */

	uint32_t tested_ref_state;	/* Debugging information */
} WT_RECONCILE;

static void __rec_bnd_cleanup(WT_SESSION_IMPL *, WT_RECONCILE *, int);
static void __rec_cell_build_addr(
		WT_RECONCILE *, const void *, size_t, u_int, uint64_t);
static int  __rec_cell_build_int_key(WT_SESSION_IMPL *,
		WT_RECONCILE *, const void *, size_t, int *);
static int  __rec_cell_build_leaf_key(WT_SESSION_IMPL *,
		WT_RECONCILE *, const void *, size_t, int *);
static int  __rec_cell_build_ovfl(WT_SESSION_IMPL *,
		WT_RECONCILE *, WT_KV *, uint8_t, uint64_t);
static int  __rec_cell_build_val(WT_SESSION_IMPL *,
		WT_RECONCILE *, const void *, size_t, uint64_t);
static int  __rec_child_deleted(
		WT_SESSION_IMPL *, WT_RECONCILE *, WT_REF *, int *);
static int  __rec_col_fix(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_col_fix_slvg(WT_SESSION_IMPL *,
		WT_RECONCILE *, WT_PAGE *, WT_SALVAGE_COOKIE *);
static int  __rec_col_int(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_col_merge(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_col_var(WT_SESSION_IMPL *,
		WT_RECONCILE *, WT_PAGE *, WT_SALVAGE_COOKIE *);
static int  __rec_col_var_helper(WT_SESSION_IMPL *, WT_RECONCILE *,
		WT_SALVAGE_COOKIE *, WT_ITEM *, int, uint8_t, uint64_t);
static int  __rec_destroy_session(WT_SESSION_IMPL *);
static int  __rec_root_write(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static int  __rec_row_int(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_row_leaf(WT_SESSION_IMPL *,
		WT_RECONCILE *, WT_PAGE *, WT_SALVAGE_COOKIE *);
static int  __rec_row_leaf_insert(
		WT_SESSION_IMPL *, WT_RECONCILE *, WT_INSERT *);
static int  __rec_row_merge(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_split_col(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_split_discard(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_split_fixup(WT_SESSION_IMPL *, WT_RECONCILE *);
static int  __rec_split_row(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_split_row_promote(
		WT_SESSION_IMPL *, WT_RECONCILE *, WT_ITEM *, uint8_t);
static int  __rec_split_write(WT_SESSION_IMPL *,
		WT_RECONCILE *, WT_BOUNDARY *, WT_ITEM *, int);
static int  __rec_write_init(WT_SESSION_IMPL *,
		WT_REF *, uint32_t, WT_SALVAGE_COOKIE *, void *);
static int  __rec_write_wrapup(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int  __rec_write_wrapup_err(
		WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);

static void __rec_dictionary_free(WT_SESSION_IMPL *, WT_RECONCILE *);
static int  __rec_dictionary_init(WT_SESSION_IMPL *, WT_RECONCILE *, u_int);
static int  __rec_dictionary_lookup(
		WT_SESSION_IMPL *, WT_RECONCILE *, WT_KV *, WT_DICTIONARY **);
static void __rec_dictionary_reset(WT_RECONCILE *);

/*
 * __wt_reconcile --
 *	Reconcile an in-memory page into its on-disk format, and write it.
 */
int
__wt_reconcile(WT_SESSION_IMPL *session,
    WT_REF *ref, WT_SALVAGE_COOKIE *salvage, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	WT_RECONCILE *r;
	int page_lock, scan_lock, split_lock;

	conn = S2C(session);
	page = ref->page;
	mod = page->modify;
	page_lock = scan_lock = split_lock = 0;

	/* We're shouldn't get called with a clean page, that's an error. */
	if (!__wt_page_is_modified(page))
		WT_RET_MSG(session, WT_ERROR,
		    "Attempt to reconcile a clean page.");

	WT_RET(__wt_verbose(session,
	    WT_VERB_RECONCILE, "%s", __wt_page_type_string(page->type)));
	WT_STAT_FAST_CONN_INCR(session, rec_pages);
	WT_STAT_FAST_DATA_INCR(session, rec_pages);
	if (LF_ISSET(WT_EVICTING)) {
		WT_STAT_FAST_CONN_INCR(session, rec_pages_eviction);
		WT_STAT_FAST_DATA_INCR(session, rec_pages_eviction);
	}

#ifdef HAVE_DIAGNOSTIC
	{
	/*
	 * Check that transaction time always moves forward for a given page.
	 * If this check fails, reconciliation can free something that a future
	 * reconciliation will need.
	 */
	uint64_t oldest_id = __wt_txn_oldest_id(session);
	WT_ASSERT(session, WT_TXNID_LE(mod->last_oldest_id, oldest_id));
	mod->last_oldest_id = oldest_id;
	}
#endif

	/* Record the most recent transaction ID we will *not* write. */
	mod->disk_snap_min = session->txn.snap_min;

	/* Initialize the reconciliation structure for each new run. */
	WT_RET(__rec_write_init(
	    session, ref, flags, salvage, &session->reconcile));
	r = session->reconcile;

	/*
	 * The compaction process looks at the page's modification information;
	 * if compaction is running, acquire the page's lock.
	 */
	if (conn->compact_in_memory_pass) {
		WT_PAGE_LOCK(session, page);
		page_lock = 1;
	}

	/*
	 * Reconciliation reads the lists of updates, so obsolete updates cannot
	 * be discarded while reconciliation is in progress.
	 */
	for (;;) {
		F_CAS_ATOMIC(page, WT_PAGE_SCANNING, ret);
		if (ret == 0)
			break;
		__wt_yield();
	}
	scan_lock = 1;

	/*
	 * Mark internal pages as splitting to ensure we don't deadlock when
	 * performing an in-memory split during a checkpoint.
	 */
	if (WT_PAGE_IS_INTERNAL(page)) {
		for (;;) {
			F_CAS_ATOMIC(page, WT_PAGE_SPLIT_LOCKED, ret);
			if (ret == 0)
				break;
			__wt_yield();
		}
		split_lock = 1;
	}

	/* Reconcile the page. */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		if (salvage != NULL)
			ret = __rec_col_fix_slvg(session, r, page, salvage);
		else
			ret = __rec_col_fix(session, r, page);
		break;
	case WT_PAGE_COL_INT:
		WT_WITH_PAGE_INDEX(session,
		    ret = __rec_col_int(session, r, page));
		break;
	case WT_PAGE_COL_VAR:
		ret = __rec_col_var(session, r, page, salvage);
		break;
	case WT_PAGE_ROW_INT:
		WT_WITH_PAGE_INDEX(session,
		    ret = __rec_row_int(session, r, page));
		break;
	case WT_PAGE_ROW_LEAF:
		ret = __rec_row_leaf(session, r, page, salvage);
		break;
	WT_ILLEGAL_VALUE_SET(session);
	}

	/* Wrap up the page reconciliation. */
	if (ret == 0)
		ret = __rec_write_wrapup(session, r, page);
	else
		WT_TRET(__rec_write_wrapup_err(session, r, page));

	/* Release the locks we're holding. */
	if (split_lock)
		F_CLR_ATOMIC(page, WT_PAGE_SPLIT_LOCKED);
	if (scan_lock)
		F_CLR_ATOMIC(page, WT_PAGE_SCANNING);
	if (page_lock)
		WT_PAGE_UNLOCK(session, page);

	/*
	 * Clean up the boundary structures: some workloads result in millions
	 * of these structures, and if associated with some random session that
	 * got roped into doing forced eviction, they won't be discarded for the
	 * life of the session.
	 */
	__rec_bnd_cleanup(session, r, 0);

	WT_RET(ret);

	/*
	 * Root pages are special, splits have to be done, we can't put it off
	 * as the parent's problem any more.
	 */
	if (__wt_ref_is_root(ref)) {
		WT_WITH_PAGE_INDEX(session,
		    ret = __rec_root_write(session, page, flags));
		return (ret);
	}

	/*
	 * Otherwise, mark the page's parent dirty.
	 * Don't mark the tree dirty: if this reconciliation is in service of a
	 * checkpoint, it's cleared the tree's dirty flag, and we don't want to
	 * set it again as part of that walk.
	 */
	return (__wt_page_parent_modify_set(session, ref, 1));
}

/*
 * __rec_root_write --
 *	Handle the write of a root page.
 */
static int
__rec_root_write(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_DECL_RET;
	WT_PAGE *next;
	WT_PAGE_INDEX *pindex;
	WT_PAGE_MODIFY *mod;
	WT_REF fake_ref;
	uint32_t i;

	mod = page->modify;

	/*
	 * If a single root page was written (either an empty page or there was
	 * a 1-for-1 page swap), we've written root and checkpoint, we're done.
	 * If the root page split, write the resulting WT_REF array.  We already
	 * have an infrastructure for writing pages, create a fake root page and
	 * write it instead of adding code to write blocks based on the list of
	 * blocks resulting from a multiblock reconciliation.
	 */
	switch (F_ISSET(mod, WT_PM_REC_MASK)) {
	case WT_PM_REC_EMPTY:				/* Page is empty */
	case WT_PM_REC_REPLACE:				/* 1-for-1 page swap */
	case WT_PM_REC_REWRITE:				/* Rewrite */
		return (0);
	case WT_PM_REC_MULTIBLOCK:			/* Multiple blocks */
		break;
	WT_ILLEGAL_VALUE(session);
	}

	WT_RET(__wt_verbose(session, WT_VERB_SPLIT,
	    "root page split -> %" PRIu32 " pages", mod->mod_multi_entries));

	/*
	 * Create a new root page, initialize the array of child references,
	 * mark it dirty, then write it.
	 */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		WT_RET(__wt_page_alloc(session,
		    WT_PAGE_COL_INT, 1, mod->mod_multi_entries, 0, &next));
		break;
	case WT_PAGE_ROW_INT:
		WT_RET(__wt_page_alloc(session,
		    WT_PAGE_ROW_INT, 0, mod->mod_multi_entries, 0, &next));
		break;
	WT_ILLEGAL_VALUE(session);
	}

	WT_INTL_INDEX_GET(session, next, pindex);
	for (i = 0; i < mod->mod_multi_entries; ++i) {
		WT_ERR(__wt_multi_to_ref(session,
		    next, &mod->mod_multi[i], &pindex->index[i], NULL));
		pindex->index[i]->home = next;
	}

	/*
	 * We maintain a list of pages written for the root in order to free the
	 * backing blocks the next time the root is written.
	 */
	mod->mod_root_split = next;

	/*
	 * Mark the page dirty.
	 * Don't mark the tree dirty: if this reconciliation is in service of a
	 * checkpoint, it's cleared the tree's dirty flag, and we don't want to
	 * set it again as part of that walk.
	 */
	WT_ERR(__wt_page_modify_init(session, next));
	__wt_page_only_modify_set(session, next);

	/*
	 * Fake up a reference structure, and write the next root page.
	 */
	__wt_root_ref_init(&fake_ref, next, page->type == WT_PAGE_COL_INT);
	return (__wt_reconcile(session, &fake_ref, NULL, flags));

err:	__wt_page_out(session, &next);
	return (ret);
}

/*
 * __rec_raw_compression_config --
 *	Configure raw compression.
 */
static inline int
__rec_raw_compression_config(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_SALVAGE_COOKIE *salvage)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	/* Check if raw compression configured. */
	if (btree->compressor == NULL ||
	    btree->compressor->compress_raw == NULL)
		return (0);

	/* Only for row-store and variable-length column-store objects. */
	if (page->type == WT_PAGE_COL_FIX)
		return (0);

	/*
	 * Raw compression cannot support dictionary compression. (Technically,
	 * we could still use the raw callback on column-store variable length
	 * internal pages with dictionary compression configured, because
	 * dictionary compression only applies to column-store leaf pages, but
	 * that seems an unlikely use case.)
	 */
	if (btree->dictionary != 0)
		return (0);

	/* Raw compression cannot support prefix compression. */
	if (btree->prefix_compression != 0)
		return (0);

	/*
	 * Raw compression is also turned off during salvage: we can't allow
	 * pages to split during salvage, raw compression has no point if it
	 * can't manipulate the page size.
	 */
	if (salvage != NULL)
		return (0);

	return (1);
}

/*
 * __rec_write_init --
 *	Initialize the reconciliation structure.
 */
static int
__rec_write_init(WT_SESSION_IMPL *session,
    WT_REF *ref, uint32_t flags, WT_SALVAGE_COOKIE *salvage, void *reconcilep)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	WT_RECONCILE *r;

	btree = S2BT(session);
	page = ref->page;

	if ((r = *(WT_RECONCILE **)reconcilep) == NULL) {
		WT_RET(__wt_calloc_one(session, &r));

		*(WT_RECONCILE **)reconcilep = r;
		session->reconcile_cleanup = __rec_destroy_session;

		/* Connect pointers/buffers. */
		r->cur = &r->_cur;
		r->last = &r->_last;

		/* Disk buffers need to be aligned for writing. */
		F_SET(&r->dsk, WT_ITEM_ALIGNED);
	}

	/* Remember the configuration. */
	r->ref = ref;
	r->page = page;
	r->flags = flags;

	/* Track if the page can be marked clean. */
	r->leave_dirty = 0;

	/* Raw compression. */
	r->raw_compression =
	    __rec_raw_compression_config(session, page, salvage);
	r->raw_destination.flags = WT_ITEM_ALIGNED;

	/* Track overflow items. */
	r->ovfl_items = 0;

	/* Track empty values. */
	r->all_empty_value = 1;
	r->any_empty_value = 0;

	/* The list of cached, skipped updates. */
	r->skip_next = 0;

	/*
	 * Dictionary compression only writes repeated values once.  We grow
	 * the dictionary as necessary, always using the largest size we've
	 * seen.
	 *
	 * Reset the dictionary.
	 *
	 * Sanity check the size: 100 slots is the smallest dictionary we use.
	 */
	if (btree->dictionary != 0 && btree->dictionary > r->dictionary_slots)
		WT_RET(__rec_dictionary_init(session,
		    r, btree->dictionary < 100 ? 100 : btree->dictionary));
	__rec_dictionary_reset(r);

	/*
	 * Suffix compression shortens internal page keys by discarding trailing
	 * bytes that aren't necessary for tree navigation.  We don't do suffix
	 * compression if there is a custom collator because we don't know what
	 * bytes a custom collator might use.  Some custom collators (for
	 * example, a collator implementing reverse ordering of strings), won't
	 * have any problem with suffix compression: if there's ever a reason to
	 * implement suffix compression for custom collators, we can add a
	 * setting to the collator, configured when the collator is added, that
	 * turns on suffix compression.
	 *
	 * The raw compression routines don't even consider suffix compression,
	 * but it doesn't hurt to confirm that.
	 */
	r->key_sfx_compress_conf = 0;
	if (btree->collator == NULL &&
	    btree->internal_key_truncate && !r->raw_compression)
		r->key_sfx_compress_conf = 1;

	/*
	 * Prefix compression discards repeated prefix bytes from row-store leaf
	 * page keys.
	 */
	r->key_pfx_compress_conf = 0;
	if (btree->prefix_compression && page->type == WT_PAGE_ROW_LEAF)
		r->key_pfx_compress_conf = 1;

	r->salvage = salvage;

	/* Save the page's write generation before reading the page. */
	WT_ORDERED_READ(r->orig_write_gen, page->modify->write_gen);

	/*
	 * Running transactions may update the page after we write it, so
	 * this is the highest ID we can be confident we will see.
	 */
	r->first_dirty_txn = S2C(session)->txn_global.last_running;

	return (0);
}

/*
 * __rec_destroy --
 *	Clean up the reconciliation structure.
 */
static void
__rec_destroy(WT_SESSION_IMPL *session, void *reconcilep)
{
	WT_RECONCILE *r;

	if ((r = *(WT_RECONCILE **)reconcilep) == NULL)
		return;
	*(WT_RECONCILE **)reconcilep = NULL;

	__wt_buf_free(session, &r->dsk);

	__wt_free(session, r->raw_entries);
	__wt_free(session, r->raw_offsets);
	__wt_free(session, r->raw_recnos);
	__wt_buf_free(session, &r->raw_destination);

	__rec_bnd_cleanup(session, r, 1);

	__wt_free(session, r->skip);

	__wt_buf_free(session, &r->k.buf);
	__wt_buf_free(session, &r->v.buf);
	__wt_buf_free(session, &r->_cur);
	__wt_buf_free(session, &r->_last);

	__rec_dictionary_free(session, r);

	__wt_free(session, r);
}

/*
 * __rec_destroy_session --
 *	Clean up the reconciliation structure, session version.
 */
static int
__rec_destroy_session(WT_SESSION_IMPL *session)
{
	__rec_destroy(session, &session->reconcile);
	return (0);
}

/*
 * __rec_bnd_cleanup --
 *	Cleanup the boundary structure information.
 */
static void
__rec_bnd_cleanup(WT_SESSION_IMPL *session, WT_RECONCILE *r, int destroy)
{
	WT_BOUNDARY *bnd;
	uint32_t i, last_used;

	if (r->bnd == NULL)
		return;

	/*
	 * Free the boundary structures' memory.  In the case of normal cleanup,
	 * discard any memory we won't reuse in the next reconciliation; in the
	 * case of destruction, discard everything.
	 *
	 * During some big-page evictions we have seen boundary arrays that have
	 * millions of elements.  That should not be a normal event, but if the
	 * memory is associated with a random session, it won't be discarded
	 * until the session is closed. If there are more than 10,000 boundary
	 * structure elements, destroy the boundary array and we'll start over.
	 */
	if (destroy || r->bnd_entries > 10 * 1000) {
		for (bnd = r->bnd, i = 0; i < r->bnd_entries; ++bnd, ++i) {
			__wt_free(session, bnd->addr.addr);
			__wt_free(session, bnd->dsk);
			__wt_free(session, bnd->skip);
			__wt_buf_free(session, &bnd->key);
		}
		__wt_free(session, r->bnd);
		r->bnd_next = 0;
		r->bnd_entries = r->bnd_allocated = 0;
	} else {
		/*
		 * The boundary-next field points to the next boundary structure
		 * we were going to use, but there's no requirement that value
		 * be incremented before reconciliation updates the structure it
		 * points to, that is, there's no guarantee elements of the next
		 * boundary structure are still unchanged. Be defensive, clean
		 * up the "next" structure as well as the ones we know we used.
		 */
		last_used = r->bnd_next;
		if (last_used < r->bnd_entries)
			++last_used;
		for (bnd = r->bnd, i = 0; i < last_used; ++bnd, ++i) {
			__wt_free(session, bnd->addr.addr);
			__wt_free(session, bnd->dsk);
			__wt_free(session, bnd->skip);
		}
	}
}

/*
 * __rec_skip_update_save --
 *	Save a skipped WT_UPDATE list for later restoration.
 */
static int
__rec_skip_update_save(
    WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins, WT_ROW *rip)
{
	WT_RET(__wt_realloc_def(
	    session, &r->skip_allocated, r->skip_next + 1, &r->skip));
	r->skip[r->skip_next].ins = ins;
	r->skip[r->skip_next].rip = rip;
	++r->skip_next;
	return (0);
}

/*
 * __rec_skip_update_move --
 *	Move a skipped WT_UPDATE list from the per-page cache to a specific
 * block's list.
 */
static int
__rec_skip_update_move(
    WT_SESSION_IMPL *session, WT_BOUNDARY *bnd, WT_UPD_SKIPPED *skip)
{
	WT_RET(__wt_realloc_def(
	    session, &bnd->skip_allocated, bnd->skip_next + 1, &bnd->skip));
	bnd->skip[bnd->skip_next] = *skip;
	++bnd->skip_next;

	skip->ins = NULL;
	skip->rip = NULL;
	return (0);
}

/*
 * __rec_txn_read --
 *	Return the first visible update in a list (or NULL if none are visible),
 * set a flag if any updates were skipped, track the maximum transaction ID on
 * the page.
 */
static inline int
__rec_txn_read(WT_SESSION_IMPL *session, WT_RECONCILE *r,
    WT_INSERT *ins, WT_ROW *rip, WT_CELL_UNPACK *vpack, WT_UPDATE **updp)
{
	WT_DECL_RET;
	WT_ITEM ovfl;
	WT_PAGE *page;
	WT_UPDATE *upd, *upd_list, *upd_ovfl;
	size_t notused;
	uint64_t max_txn, min_txn, txnid;
	int skipped;

	*updp = NULL;

	page = r->page;

	/*
	 * If called with a WT_INSERT item, use its WT_UPDATE list (which must
	 * exist), otherwise check for an on-page row-store WT_UPDATE list
	 * (which may not exist). Return immediately if the item has no updates.
	 */
	if (ins == NULL) {
		if ((upd_list = WT_ROW_UPDATE(page, rip)) == NULL)
			return (0);
	} else
		upd_list = ins->upd;

	skipped = 0;
	for (max_txn = WT_TXN_NONE, min_txn = UINT64_MAX, upd = upd_list;
	    upd != NULL; upd = upd->next) {
		if ((txnid = upd->txnid) == WT_TXN_ABORTED)
			continue;

		/* Track the largest/smallest transaction IDs on the list. */
		if (WT_TXNID_LT(max_txn, txnid))
			max_txn = txnid;
		if (WT_TXNID_LT(txnid, min_txn))
			min_txn = txnid;
		if (WT_TXNID_LT(txnid, r->first_dirty_txn) &&
		    !__wt_txn_visible_all(session, txnid))
			r->first_dirty_txn = txnid;

		/*
		 * Record whether any updates were skipped on the way to finding
		 * the first visible update.
		 *
		 * If updates were skipped before the one being written, future
		 * reads without intervening modifications to the page could
		 * see a different value; if no updates were skipped, the page
		 * can safely be marked clean and does not need to be
		 * reconciled until modified again.
		 */
		if (*updp == NULL) {
			if (__wt_txn_visible(session, txnid))
				*updp = upd;
			else
				skipped = 1;
		}
	}

	/*
	 * Track the maximum transaction ID in the page.  We store this in the
	 * page at the end of reconciliation if no updates are skipped, it's
	 * used to avoid evicting clean pages from memory with changes required
	 * to satisfy a snapshot read.
	 */
	if (WT_TXNID_LT(r->max_txn, max_txn))
		r->max_txn = max_txn;

	/*
	 * If no updates were skipped and all updates are globally visible, the
	 * page can be marked clean and we're done, regardless of whether we're
	 * evicting or checkpointing.
	 *
	 * We have to check both: the oldest transaction ID may have moved while
	 * we were scanning the update list, so it is possible to skip an update
	 * but then find that by the end of the scan, all updates are stable.
	 */
	if (!skipped && __wt_txn_visible_all(session, max_txn))
		return (0);

	/*
	 * If some updates are not globally visible, or were skipped, the page
	 * cannot be marked clean.
	 */
	r->leave_dirty = 1;

	/* If we're not evicting, we're done, we know what we'll write. */
	if (!F_ISSET(r, WT_EVICTING))
		return (0);

	/* In some cases, there had better not be any updates we can't write. */
	if (F_ISSET(r, WT_SKIP_UPDATE_ERR))
		WT_PANIC_RET(session, EINVAL,
		    "reconciliation illegally skipped an update");

	/*
	 * If evicting and we aren't able to save/restore the not-yet-visible
	 * updates, the page can't be evicted.
	 */
	if (!F_ISSET(r, WT_SKIP_UPDATE_RESTORE))
		return (EBUSY);

	/*
	 * Evicting a page with not-yet-visible updates: save and restore the
	 * list of updates on a newly instantiated page.
	 *
	 * The order of the updates on the list matters so we can't move only
	 * the unresolved updates, we have to move the entire update list.
	 *
	 * Clear the returned update so our caller ignores the key/value pair
	 * in the case of an insert/append entry (everything we need is in the
	 * update list), and otherwise writes the original on-page key/value
	 * pair to which the update list applies.
	 */
	*updp = NULL;

	/*
	 * Handle the case were we don't want to write an original on-page value
	 * item to disk because it's been updated or removed.
	 *
	 * Here's the deal: an overflow value was updated or removed and its
	 * backing blocks freed.  If any transaction in the system might still
	 * read the value, a copy was cached in page reconciliation tracking
	 * memory, and the page cell set to WT_CELL_VALUE_OVFL_RM.  Eviction
	 * then chose the page and we're splitting it up in order to push parts
	 * of it out of memory.
	 *
	 * We could write the original on-page value item to disk... if we had
	 * a copy.  The cache may not have a copy (a globally visible update
	 * would have kept a value from ever being cached), or an update that
	 * subsequent became globally visible could cause a cached value to be
	 * discarded.  Either way, once there's a globally visible update, we
	 * may not have the value.
	 *
	 * Fortunately, if there's a globally visible update we don't care about
	 * the original version, so we simply ignore it, no transaction can ever
	 * try and read it.  If there isn't a globally visible update, there had
	 * better be a cached value.
	 *
	 * In the latter case, we could write the value out to disk, but (1) we
	 * are planning on re-instantiating this page in memory, it isn't going
	 * to disk, and (2) the value item is eventually going to be discarded,
	 * that seems like a waste of a write.  Instead, find the cached value
	 * and append it to the update list we're saving for later restoration.
	 */
	if (vpack != NULL && vpack->raw == WT_CELL_VALUE_OVFL_RM &&
	    !__wt_txn_visible_all(session, min_txn)) {
		if ((ret = __wt_ovfl_txnc_search(
		    page, vpack->data, vpack->size, &ovfl)) != 0)
			WT_PANIC_RET(session, ret,
			    "cached overflow item discarded early");

		/*
		 * Create an update structure with an impossibly low transaction
		 * ID and append it to the update list we're about to save.
		 * Restoring that update list when this page is re-instantiated
		 * creates an update for the key/value pair visible to every
		 * running transaction in the system, ensuring the on-page value
		 * will be ignored.
		 */
		WT_RET(__wt_update_alloc(session, &ovfl, &upd_ovfl, &notused));
		upd_ovfl->txnid = WT_TXN_NONE;
		for (upd = upd_list; upd->next != NULL; upd = upd->next)
			;
		upd->next = upd_ovfl;
	}

	return (__rec_skip_update_save(session, r, ins, rip));
}

/*
 * CHILD_RELEASE --
 *	Macros to clean up during internal-page reconciliation, releasing the
 * hazard pointer we're holding on child pages.
 */
#undef	CHILD_RELEASE
#define	CHILD_RELEASE(session, hazard, ref) do {			\
	if (hazard) {							\
		hazard = 0;						\
		WT_TRET(						\
		    __wt_page_release(session, ref, WT_READ_NO_EVICT));	\
	}								\
} while (0)
#undef	CHILD_RELEASE_ERR
#define	CHILD_RELEASE_ERR(session, hazard, ref) do {			\
	CHILD_RELEASE(session, hazard, ref);				\
	WT_ERR(ret);							\
} while (0)

/*
 * __rec_child_modify --
 *	Return if the internal page's child references any modifications.
 */
static int
__rec_child_modify(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, WT_REF *ref, int *hazardp, int *statep)
{
	WT_DECL_RET;
	WT_PAGE_MODIFY *mod;

	/* We may acquire a hazard pointer our caller must release. */
	*hazardp = 0;

#define	WT_CHILD_IGNORE		1		/* Deleted child: ignore */
#define	WT_CHILD_MODIFIED	2		/* Modified child */
#define	WT_CHILD_PROXY		3		/* Deleted child: proxy */
	*statep = 0;

	/*
	 * This function is called when walking an internal page to decide how
	 * to handle child pages referenced by the internal page, specifically
	 * if the child page is to be merged into its parent.
	 *
	 * Internal pages are reconciled for two reasons: first, when evicting
	 * an internal page, second by the checkpoint code when writing internal
	 * pages.  During eviction, the subtree is locked down so all pages
	 * should be in the WT_REF_DISK or WT_REF_LOCKED state. During
	 * checkpoint, any eviction that might affect our review of an internal
	 * page is prohibited, however, as the subtree is not reserved for our
	 * exclusive use, there are other page states that must be considered.
	 */
	for (;; __wt_yield())
		switch (r->tested_ref_state = ref->state) {
		case WT_REF_DISK:
			/* On disk, not modified by definition. */
			goto done;

		case WT_REF_DELETED:
			/*
			 * The child is in a deleted state.
			 *
			 * It's possible the state could change underneath us as
			 * the page is read in, and we can race between checking
			 * for a deleted state and looking at the transaction ID
			 * to see if the delete is visible to us.  Lock down the
			 * structure.
			 */
			if (!__wt_atomic_casv32(
			    &ref->state, WT_REF_DELETED, WT_REF_LOCKED))
				break;
			ret = __rec_child_deleted(session, r, ref, statep);
			WT_PUBLISH(ref->state, WT_REF_DELETED);
			goto done;

		case WT_REF_LOCKED:
			/*
			 * Locked.
			 *
			 * If evicting, the evicted page's subtree, including
			 * this child, was selected for eviction by us and the
			 * state is stable until we reset it, it's an in-memory
			 * state.  This is the expected state for a child being
			 * merged into a page (where the page was selected by
			 * the eviction server for eviction).
			 */
			if (F_ISSET(r, WT_EVICTING))
				goto in_memory;

			/*
			 * If called during checkpoint, the child is being
			 * considered by the eviction server or the child is a
			 * fast-delete page being read.  The eviction may have
			 * started before the checkpoint and so we must wait
			 * for the eviction to be resolved.  I suspect we could
			 * handle fast-delete reads, but we can't distinguish
			 * between the two and fast-delete reads aren't expected
			 * to be common.
			 */
			break;

		case WT_REF_MEM:
			/*
			 * In memory.
			 *
			 * If evicting, the evicted page's subtree, including
			 * this child, was selected for eviction by us and the
			 * state is stable until we reset it, it's an in-memory
			 * state.  This is the expected state for a child being
			 * merged into a page (where the page belongs to a file
			 * being discarded from the cache during close).
			 */
			if (F_ISSET(r, WT_EVICTING))
				goto in_memory;

			/*
			 * If called during checkpoint, acquire a hazard pointer
			 * so the child isn't evicted, it's an in-memory case.
			 *
			 * This call cannot return split/restart, dirty page
			 * eviction is shutout during checkpoint, all splits in
			 * process will have completed before we walk any pages
			 * for checkpoint.
			 */
			ret = __wt_page_in(session, ref,
			    WT_READ_CACHE | WT_READ_NO_EVICT |
			    WT_READ_NO_GEN | WT_READ_NO_WAIT);
			if (ret == WT_NOTFOUND) {
				ret = 0;
				break;
			}
			WT_RET(ret);
			*hazardp = 1;
			goto in_memory;

		case WT_REF_READING:
			/*
			 * Being read, not modified by definition.
			 *
			 * We should never be here during eviction, a child page
			 * in this state within an evicted page's subtree would
			 * have caused normally eviction to fail, and exclusive
			 * eviction shouldn't ever see pages being read.
			 */
			WT_ASSERT(session, !F_ISSET(r, WT_EVICTING));
			goto done;

		case WT_REF_SPLIT:
			/*
			 * The page was split out from under us.
			 *
			 * We should never be here during eviction, a child page
			 * in this state within an evicted page's subtree would
			 * have caused eviction to fail.
			 *
			 * We should never be here during checkpoint, dirty page
			 * eviction is shutout during checkpoint, all splits in
			 * process will have completed before we walk any pages
			 * for checkpoint.
			 */
			WT_ASSERT(session, ref->state != WT_REF_SPLIT);
			/* FALLTHROUGH */

		WT_ILLEGAL_VALUE(session);
		}

in_memory:
	/*
	 * In-memory states: the child is potentially modified if the page's
	 * modify structure has been instantiated. If the modify structure
	 * exists and the page has actually been modified, set that state.
	 * If that's not the case, we would normally use the original cell's
	 * disk address as our reference, but, if we're forced to instantiate
	 * a deleted child page and it's never modified, we end up here with
	 * a page that has a modify structure, no modifications, and no disk
	 * address.  Ignore those pages, they're not modified and there is no
	 * reason to write the cell.
	 */
	mod = ref->page->modify;
	if (mod != NULL && mod->flags != 0)
		*statep = WT_CHILD_MODIFIED;
	else if (ref->addr == NULL) {
		*statep = WT_CHILD_IGNORE;
		CHILD_RELEASE(session, *hazardp, ref);
	}

done:	WT_DIAGNOSTIC_YIELD;
	return (ret);
}

/*
 * __rec_child_deleted --
 *	Handle pages with leaf pages in the WT_REF_DELETED state.
 */
static int
__rec_child_deleted(
    WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *ref, int *statep)
{
	WT_BM *bm;
	WT_PAGE_DELETED *page_del;
	size_t addr_size;
	const uint8_t *addr;

	bm = S2BT(session)->bm;
	page_del = ref->page_del;

	/*
	 * Internal pages with child leaf pages in the WT_REF_DELETED state are
	 * a special case during reconciliation.  First, if the deletion was a
	 * result of a session truncate call, the deletion may not be visible to
	 * us.  In that case, we proceed as with any change that's not visible
	 * during reconciliation by setting the skipped flag and ignoring the
	 * change for the purposes of writing the internal page.
	 *
	 * In this case, there must be an associated page-deleted structure, and
	 * it holds the transaction ID we care about.
	 */
	if (page_del != NULL && !__wt_txn_visible(session, page_del->txnid)) {
		/*
		 * In some cases, there had better not be any updates we can't
		 * write.
		 */
		if (F_ISSET(r, WT_SKIP_UPDATE_ERR))
			WT_PANIC_RET(session, EINVAL,
			    "reconciliation illegally skipped an update");
	}

	/*
	 * The deletion is visible to us, deal with any underlying disk blocks.
	 *
	 * First, check to see if there is an address associated with this leaf:
	 * if there isn't, we're done, the underlying page is already gone.  If
	 * the page still exists, check for any transactions in the system that
	 * might want to see the page's state before it's deleted.
	 *
	 * If any such transactions exist, we cannot discard the underlying leaf
	 * page to the block manager because the transaction may eventually read
	 * it.  However, this write might be part of a checkpoint, and should we
	 * recover to that checkpoint, we'll need to delete the leaf page, else
	 * we'd leak it.  The solution is to write a proxy cell on the internal
	 * page ensuring the leaf page is eventually discarded.
	 *
	 * If no such transactions exist, we can discard the leaf page to the
	 * block manager and no cell needs to be written at all.  We do this
	 * outside of the underlying tracking routines because this action is
	 * permanent and irrevocable.  (Clearing the address means we've lost
	 * track of the disk address in a permanent way.  This is safe because
	 * there's no path to reading the leaf page again: if there's ever a
	 * read into this part of the name space again, the cache read function
	 * instantiates an entirely new page.)
	 */
	if (ref->addr != NULL &&
	    (page_del == NULL ||
	    __wt_txn_visible_all(session, page_del->txnid))) {
		WT_RET(__wt_ref_info(session, ref, &addr, &addr_size, NULL));
		WT_RET(bm->free(bm, session, addr, addr_size));

		if (__wt_off_page(ref->home, ref->addr)) {
			__wt_free(session, ((WT_ADDR *)ref->addr)->addr);
			__wt_free(session, ref->addr);
		}
		ref->addr = NULL;
	}

	/*
	 * If there are deleted child pages that we can't discard immediately,
	 * keep the page dirty so they are eventually freed.
	 */
	if (ref->addr != NULL) {
		r->leave_dirty = 1;

		/* This page cannot be evicted, quit now. */
		if (F_ISSET(r, WT_EVICTING))
			return (EBUSY);
	}

	/*
	 * Minor memory cleanup: if a truncate call deleted this page and we
	 * were ever forced to instantiate the page in memory, we would have
	 * built a list of updates in the page reference in order to be able
	 * to abort the truncate.  It's a cheap test to make that memory go
	 * away, we do it here because there's really nowhere else we do the
	 * checks.  In short, if we have such a list, and the backing address
	 * blocks are gone, there can't be any transaction that can abort.
	 */
	if (ref->addr == NULL && page_del != NULL) {
		__wt_free(session, ref->page_del->update_list);
		__wt_free(session, ref->page_del);
	}

	/*
	 * If there's still a disk address, then we have to write a proxy
	 * record, otherwise, we can safely ignore this child page.
	 */
	*statep = ref->addr == NULL ? WT_CHILD_IGNORE : WT_CHILD_PROXY;
	return (0);
}

/*
 * __rec_incr --
 *	Update the memory tracking structure for a set of new entries.
 */
static inline void
__rec_incr(WT_SESSION_IMPL *session, WT_RECONCILE *r, uint32_t v, size_t size)
{
	/*
	 * The buffer code is fragile and prone to off-by-one errors -- check
	 * for overflow in diagnostic mode.
	 */
	WT_ASSERT(session, r->space_avail >= size);
	WT_ASSERT(session,
	    WT_BLOCK_FITS(r->first_free, size, r->dsk.mem, r->dsk.memsize));

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
	size_t len;
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
    WT_SESSION_IMPL *session, WT_RECONCILE *r, uint64_t rle, WT_KV *val)
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
	if (val->buf.size <= WT_INTPACK32_MAXSIZE)
		return (0);
	WT_RET(__rec_dictionary_lookup(session, r, val, &dp));
	if (dp == NULL)
		return (0);

	/*
	 * If the dictionary cell reference is not set, we're creating a new
	 * entry in the dictionary, update its location.
	 *
	 * If the dictionary cell reference is set, we have a matching value.
	 * Create a copy cell instead.
	 */
	if (dp->cell == NULL)
		dp->cell = r->first_free;
	else {
		offset = WT_PTRDIFF(r->first_free, dp->cell);
		val->len = val->cell_len =
		    __wt_cell_pack_copy(&val->cell, rle, offset);
		val->buf.data = NULL;
		val->buf.size = 0;
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
	 * impossible for the next key. Alternatively, we could remember where
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
 * Macros from fixed-length entries to/from bytes.
 */
#define	WT_FIX_BYTES_TO_ENTRIES(btree, bytes)				\
    ((uint32_t)((((bytes) * 8) / (btree)->bitcnt)))
#define	WT_FIX_ENTRIES_TO_BYTES(btree, entries)				\
	((uint32_t)WT_ALIGN((entries) * (btree)->bitcnt, 8))

/*
 * __rec_leaf_page_max --
 *	Figure out the maximum leaf page size for the reconciliation.
 */
static inline uint32_t
__rec_leaf_page_max(WT_SESSION_IMPL *session,  WT_RECONCILE *r)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	uint32_t page_size;

	btree = S2BT(session);
	page = r->page;

	page_size = 0;
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		/*
		 * Column-store pages can grow if there are missing records
		 * (that is, we lost a chunk of the range, and have to write
		 * deleted records).  Fixed-length objects are a problem, if
		 * there's a big missing range, we could theoretically have to
		 * write large numbers of missing objects.
		 */
		page_size = (uint32_t)WT_ALIGN(WT_FIX_ENTRIES_TO_BYTES(btree,
		    r->salvage->take + r->salvage->missing), btree->allocsize);
		break;
	case WT_PAGE_COL_VAR:
		/*
		 * Column-store pages can grow if there are missing records
		 * (that is, we lost a chunk of the range, and have to write
		 * deleted records).  Variable-length objects aren't usually a
		 * problem because we can write any number of deleted records
		 * in a single page entry because of the RLE, we just need to
		 * ensure that additional entry fits.
		 */
		break;
	case WT_PAGE_ROW_LEAF:
	default:
		/*
		 * Row-store pages can't grow, salvage never does anything
		 * other than reduce the size of a page read from disk.
		 */
		break;
	}

	/*
	 * Default size for variable-length column-store and row-store pages
	 * during salvage is the maximum leaf page size.
	 */
	if (page_size < btree->maxleafpage)
		page_size = btree->maxleafpage;

	/*
	 * The page we read from the disk should be smaller than the page size
	 * we just calculated, check out of paranoia.
	 */
	if (page_size < page->dsk->mem_size)
		page_size = page->dsk->mem_size;

	/*
	 * Salvage is the backup plan: don't let this fail.
	 */
	return (page_size * 2);
}

/*
 * __rec_split_bnd_init --
 *	Initialize a single boundary structure.
 */
static void
__rec_split_bnd_init(WT_SESSION_IMPL *session, WT_BOUNDARY *bnd)
{
	bnd->offset = 0;
	bnd->recno = 0;
	bnd->entries = 0;

	__wt_free(session, bnd->addr.addr);
	WT_CLEAR(bnd->addr);
	bnd->size = 0;
	bnd->cksum = 0;
	__wt_free(session, bnd->dsk);

	__wt_free(session, bnd->skip);
	bnd->skip_next = 0;
	bnd->skip_allocated = 0;

	/*
	 * Don't touch the key, we re-use that memory in each new
	 * reconciliation.
	 */

	bnd->already_compressed = 0;
}

/*
 * __rec_split_bnd_grow --
 *	Grow the boundary array as necessary.
 */
static int
__rec_split_bnd_grow(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
	/*
	 * Make sure there's enough room for another boundary.  The calculation
	 * is +2, because when filling in the current boundary's information,
	 * we save start information for the next boundary (a byte offset and a
	 * record number or key), in the (current + 1) slot.
	 *
	 * For the same reason, we're always initializing one ahead.
	 */
	WT_RET(__wt_realloc_def(
	    session, &r->bnd_allocated, r->bnd_next + 2, &r->bnd));
	r->bnd_entries = r->bnd_allocated / sizeof(r->bnd[0]);

	__rec_split_bnd_init(session, &r->bnd[r->bnd_next + 1]);

	return (0);
}

/*
 * __wt_split_page_size --
 *	Split page size calculation: we don't want to repeatedly split every
 * time a new entry is added, so we split to a smaller-than-maximum page size.
 */
uint32_t
__wt_split_page_size(WT_BTREE *btree, uint32_t maxpagesize)
{
	uintmax_t a;
	uint32_t split_size;

	/*
	 * Ideally, the split page size is some percentage of the maximum page
	 * size rounded to an allocation unit (round to an allocation unit so
	 * we don't waste space when we write).
	 */
	a = maxpagesize;			/* Don't overflow. */
	split_size = (uint32_t)
	    WT_ALIGN((a * (u_int)btree->split_pct) / 100, btree->allocsize);

	/*
	 * If the result of that calculation is the same as the allocation unit
	 * (that happens if the maximum size is the same size as an allocation
	 * unit, use a percentage of the maximum page size).
	 */
	if (split_size == btree->allocsize)
		split_size = (uint32_t)((a * (u_int)btree->split_pct) / 100);

	return (split_size);
}

/*
 * __rec_split_init --
 *	Initialization for the reconciliation split functions.
 */
static int
__rec_split_init(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, WT_PAGE *page, uint64_t recno, uint32_t max)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_PAGE_HEADER *dsk;
	size_t corrected_page_size;

	btree = S2BT(session);
	bm = btree->bm;

	/*
	 * The maximum leaf page size governs when an in-memory leaf page splits
	 * into multiple on-disk pages; however, salvage can't be allowed to
	 * split, there's no parent page yet.  If we're doing salvage, override
	 * the caller's selection of a maximum page size, choosing a page size
	 * that ensures we won't split.
	 */
	if (r->salvage != NULL)
		max = __rec_leaf_page_max(session, r);

	/*
	 * Set the page sizes.  If we're doing the page layout, the maximum page
	 * size is the same as the page size.  If the application is doing page
	 * layout (raw compression is configured), we accumulate some amount of
	 * additional data because we don't know how well it will compress, and
	 * we don't want to increment our way up to the amount of data needed by
	 * the application to successfully compress to the target page size.
	 */
	r->page_size = r->page_size_orig = max;
	if (r->raw_compression)
		r->page_size *= 10;

	/*
	 * Ensure the disk image buffer is large enough for the max object, as
	 * corrected by the underlying block manager.
	 */
	corrected_page_size = r->page_size;
	WT_RET(bm->write_size(bm, session, &corrected_page_size));
	WT_RET(__wt_buf_init(session, &r->dsk, corrected_page_size));

	/*
	 * Clear the disk page's header and block-manager space, set the page
	 * type (the type doesn't change, and setting it later would require
	 * additional code in a few different places).
	 */
	dsk = r->dsk.mem;
	memset(dsk, 0, WT_PAGE_HEADER_BYTE_SIZE(btree));
	dsk->type = page->type;

	/*
	 * If we have to split, we want to choose a smaller page size for the
	 * split pages, because otherwise we could end up splitting one large
	 * packed page over and over. We don't want to pick the minimum size
	 * either, because that penalizes an application that did a bulk load
	 * and subsequently inserted a few items into packed pages.  Currently
	 * defaulted to 75%, but I have no empirical evidence that's "correct".
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
	 * Finally, all this doesn't matter for fixed-size column-store pages,
	 * raw compression, and salvage.  Fixed-size column store pages can
	 * split under (very) rare circumstances, but they're allocated at a
	 * fixed page size, never anything smaller.  In raw compression, the
	 * underlying compression routine decides when we split, so it's not
	 * our problem.  In salvage, as noted above, we can't split at all.
	 */
	if (r->raw_compression || r->salvage != NULL) {
		r->split_size = 0;
		r->space_avail = r->page_size - WT_PAGE_HEADER_BYTE_SIZE(btree);
	}
	else if (page->type == WT_PAGE_COL_FIX) {
		r->split_size = r->page_size;
		r->space_avail =
		    r->split_size - WT_PAGE_HEADER_BYTE_SIZE(btree);
	} else {
		r->split_size = __wt_split_page_size(btree, r->page_size);
		r->space_avail =
		    r->split_size - WT_PAGE_HEADER_BYTE_SIZE(btree);
	}
	r->first_free = WT_PAGE_HEADER_BYTE(btree, dsk);

	/* Initialize the first boundary. */
	r->bnd_next = 0;
	WT_RET(__rec_split_bnd_grow(session, r));
	__rec_split_bnd_init(session, &r->bnd[0]);
	r->bnd[0].recno = recno;
	r->bnd[0].offset = WT_PAGE_HEADER_BYTE_SIZE(btree);

	/*
	 * If the maximum page size is the same as the split page size, either
	 * because of the object type or application configuration, there isn't
	 * any need to maintain split boundaries within a larger page.
	 *
	 * No configuration for salvage here, because salvage can't split.
	 */
	if (r->raw_compression)
		r->bnd_state = SPLIT_TRACKING_RAW;
	else if (max == r->split_size)
		r->bnd_state = SPLIT_TRACKING_OFF;
	else
		r->bnd_state = SPLIT_BOUNDARY;

	/* Initialize the entry counters. */
	r->entries = r->total_entries = 0;

	/* Initialize the starting record number. */
	r->recno = recno;

	/* New page, compression off. */
	r->key_pfx_compress = r->key_sfx_compress = 0;

	return (0);
}

/*
 * __rec_is_checkpoint --
 *	Return if we're writing a checkpoint.
 */
static int
__rec_is_checkpoint(WT_RECONCILE *r, WT_BOUNDARY *bnd)
{
	/*
	 * Check to see if we're going to create a checkpoint.
	 *
	 * This function exists as a place to hang this comment.
	 *
	 * Any time we write the root page of the tree without splitting we are
	 * creating a checkpoint (and have to tell the underlying block manager
	 * so it creates and writes the additional information checkpoints
	 * require).  However, checkpoints are completely consistent, and so we
	 * have to resolve information about the blocks we're expecting to free
	 * as part of the checkpoint, before writing the checkpoint.  In short,
	 * we don't do checkpoint writes here; clear the boundary information as
	 * a reminder and create the checkpoint during wrapup.
	 */
	if (bnd == &r->bnd[0] && __wt_ref_is_root(r->ref)) {
		bnd->addr.addr = NULL;
		bnd->addr.size = 0;
		bnd->addr.type = 0;
		return (1);
	}
	return (0);
}

/*
 * __rec_split_row_promote_cell --
 *	Get a key from a cell for the purposes of promotion.
 */
static int
__rec_split_row_promote_cell(
    WT_SESSION_IMPL *session, WT_PAGE_HEADER *dsk, WT_ITEM *key)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *kpack, _kpack;

	btree = S2BT(session);
	kpack = &_kpack;

	/*
	 * The cell had better have a zero-length prefix and not be a copy cell;
	 * the first cell on a page cannot refer an earlier cell on the page.
	 */
	cell = WT_PAGE_HEADER_BYTE(btree, dsk);
	__wt_cell_unpack(cell, kpack);
	WT_ASSERT(session,
	    kpack->prefix == 0 && kpack->raw != WT_CELL_VALUE_COPY);

	WT_RET(__wt_cell_data_copy(session, dsk->type, kpack, key));
	return (0);
}

/*
 * __rec_split_row_promote --
 *	Key promotion for a row-store.
 */
static int
__rec_split_row_promote(
    WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_ITEM *key, uint8_t type)
{
	WT_BTREE *btree;
	WT_DECL_ITEM(update);
	WT_DECL_RET;
	WT_ITEM *max;
	WT_UPD_SKIPPED *skip;
	size_t cnt, len, size;
	uint32_t i;
	const uint8_t *pa, *pb;
	int cmp;

	/*
	 * For a column-store, the promoted key is the recno and we already have
	 * a copy.  For a row-store, it's the first key on the page, a variable-
	 * length byte string, get a copy.
	 *
	 * This function is called from the split code at each split boundary,
	 * but that means we're not called before the first boundary, and we
	 * will eventually have to get the first key explicitly when splitting
	 * a page.
	 *
	 * For the current slot, take the last key we built, after doing suffix
	 * compression.  The "last key we built" describes some process: before
	 * calling the split code, we must place the last key on the page before
	 * the boundary into the "last" key structure, and the first key on the
	 * page after the boundary into the "current" key structure, we're going
	 * to compare them for suffix compression.
	 *
	 * Suffix compression is a hack to shorten keys on internal pages.  We
	 * only need enough bytes in the promoted key to ensure searches go to
	 * the correct page: the promoted key has to be larger than the last key
	 * on the leaf page preceding it, but we don't need any more bytes than
	 * that. In other words, we can discard any suffix bytes not required
	 * to distinguish between the key being promoted and the last key on the
	 * leaf page preceding it.  This can only be done for the first level of
	 * internal pages, you cannot repeat suffix truncation as you split up
	 * the tree, it loses too much information.
	 *
	 * Note #1: if the last key on the previous page was an overflow key,
	 * we don't have the in-memory key against which to compare, and don't
	 * try to do suffix compression.  The code for that case turns suffix
	 * compression off for the next key, we don't have to deal with it here.
	 */
	if (type != WT_PAGE_ROW_LEAF || !r->key_sfx_compress)
		return (__wt_buf_set(session, key, r->cur->data, r->cur->size));

	btree = S2BT(session);
	WT_RET(__wt_scr_alloc(session, 0, &update));

	/*
	 * Note #2: if we skipped updates, an update key may be larger than the
	 * last key stored in the previous block (probable for append-centric
	 * workloads).  If there are skipped updates, check for one larger than
	 * the last key and smaller than the current key.
	 */
	max = r->last;
	for (i = r->skip_next; i > 0; --i) {
		skip = &r->skip[i - 1];
		if (skip->ins == NULL)
			WT_ERR(__wt_row_leaf_key(
			    session, r->page, skip->rip, update, 0));
		else {
			update->data = WT_INSERT_KEY(skip->ins);
			update->size = WT_INSERT_KEY_SIZE(skip->ins);
		}

		/* Compare against the current key, it must be less. */
		WT_ERR(__wt_compare(
		    session, btree->collator, update, r->cur, &cmp));
		if (cmp >= 0)
			continue;

		/* Compare against the last key, it must be greater. */
		WT_ERR(__wt_compare(
		    session, btree->collator, update, r->last, &cmp));
		if (cmp >= 0)
			max = update;

		/*
		 * The skipped updates are in key-sort order so the entry we're
		 * looking for is either the last one or the next-to-last one
		 * in the list.  Once we've compared an entry against the last
		 * key on the page, we're done.
		 */
		break;
	}

	/*
	 * The largest key on the last block must sort before the current key,
	 * so we'll either find a larger byte value in the current key, or the
	 * current key will be a longer key, and the interesting byte is one
	 * past the length of the shorter key.
	 */
	pa = max->data;
	pb = r->cur->data;
	len = WT_MIN(max->size, r->cur->size);
	size = len + 1;
	for (cnt = 1; len > 0; ++cnt, --len, ++pa, ++pb)
		if (*pa != *pb) {
			if (size != cnt) {
				WT_STAT_FAST_DATA_INCRV(session,
				    rec_suffix_compression, size - cnt);
				size = cnt;
			}
			break;
		}
	ret = __wt_buf_set(session, key, r->cur->data, size);

err:	__wt_scr_free(session, &update);
	return (ret);
}

/*
 * __rec_split_grow --
 *	Grow the split buffer.
 */
static int
__rec_split_grow(WT_SESSION_IMPL *session, WT_RECONCILE *r, size_t add_len)
{
	WT_BM *bm;
	WT_BTREE *btree;
	size_t corrected_page_size, len;

	btree = S2BT(session);
	bm = btree->bm;

	len = WT_PTRDIFF(r->first_free, r->dsk.mem);
	corrected_page_size = len + add_len;
	WT_RET(bm->write_size(bm, session, &corrected_page_size));
	WT_RET(__wt_buf_grow(session, &r->dsk, corrected_page_size));
	r->first_free = (uint8_t *)r->dsk.mem + len;
	WT_ASSERT(session, corrected_page_size >= len);
	r->space_avail = corrected_page_size - len;
	WT_ASSERT(session, r->space_avail >= add_len);
	return (0);
}

/*
 * __rec_split --
 *	Handle the page reconciliation bookkeeping.  (Did you know "bookkeeper"
 * has 3 doubled letters in a row?  Sweet-tooth does, too.)
 */
static int
__rec_split(WT_SESSION_IMPL *session, WT_RECONCILE *r, size_t next_len)
{
	WT_BOUNDARY *last, *next;
	WT_BTREE *btree;
	WT_PAGE_HEADER *dsk;
	size_t inuse;

	btree = S2BT(session);
	dsk = r->dsk.mem;

	/*
	 * We should never split during salvage, and we're about to drop core
	 * because there's no parent page.
	 */
	if (r->salvage != NULL)
		WT_PANIC_RET(session, WT_PANIC,
		    "%s page too large, attempted split during salvage",
		    __wt_page_type_string(r->page->type));

	/* Hitting a page boundary resets the dictionary, in all cases. */
	__rec_dictionary_reset(r);

	inuse = WT_PTRDIFF32(r->first_free, dsk);
	switch (r->bnd_state) {
	case SPLIT_BOUNDARY:
		/*
		 * We can get here if the first key/value pair won't fit.
		 * Additionally, grow the buffer to contain the current item if
		 * we haven't already consumed a reasonable portion of a split
		 * chunk.
		 */
		if (inuse < r->split_size / 2)
			break;

		/*
		 * About to cross a split boundary but not yet forced to split
		 * into multiple pages. If we have to split, this is one of the
		 * split points, save information about where we are when the
		 * split would have happened.
		 */
		WT_RET(__rec_split_bnd_grow(session, r));
		last = &r->bnd[r->bnd_next++];
		next = last + 1;

		/* Set the number of entries for the just finished chunk. */
		last->entries = r->entries - r->total_entries;
		r->total_entries = r->entries;

		/* Set the key for the next chunk. */
		next->recno = r->recno;
		if (dsk->type == WT_PAGE_ROW_INT ||
		    dsk->type == WT_PAGE_ROW_LEAF)
			WT_RET(__rec_split_row_promote(
			    session, r, &next->key, dsk->type));

		/*
		 * Set the starting buffer offset and clear the entries (the
		 * latter not required, but cleaner).
		 */
		next->offset = WT_PTRDIFF(r->first_free, dsk);
		next->entries = 0;

		/* Set the space available to another split-size chunk. */
		r->space_avail =
		    r->split_size - WT_PAGE_HEADER_BYTE_SIZE(btree);

		/*
		 * Adjust the space available to handle two cases:
		 *  - We don't have enough room for another full split-size
		 *    chunk on the page.
		 *  - We chose to fill past a page boundary because of a
		 *    large item.
		 */
		if (inuse + r->space_avail > r->page_size) {
			r->space_avail =
			    r->page_size > inuse ? (r->page_size - inuse) : 0;

			/* There are no further boundary points. */
			r->bnd_state = SPLIT_MAX;
		}

		/*
		 * Return if the next object fits into this page, else we have
		 * to split the page.
		 */
		if (r->space_avail >= next_len)
			return (0);

		/* FALLTHROUGH */
	case SPLIT_MAX:
		/*
		 * We're going to have to split and create multiple pages.
		 *
		 * Cycle through the saved split-point information, writing the
		 * split chunks we have tracked.  The underlying fixup function
		 * sets the space available and other information, and copied
		 * any unwritten chunk of data to the beginning of the buffer.
		 */
		WT_RET(__rec_split_fixup(session, r));

		/* We're done saving split chunks. */
		r->bnd_state = SPLIT_TRACKING_OFF;
		break;
	case SPLIT_TRACKING_OFF:
		/*
		 * We can get here if the first key/value pair won't fit.
		 * Additionally, grow the buffer to contain the current item if
		 * we haven't already consumed a reasonable portion of a split
		 * chunk.
		 */
		if (inuse < r->split_size / 2)
			break;

		/*
		 * The key/value pairs didn't fit into a single page, but either
		 * we've already noticed that and are now processing the rest of
		 * the pairs at split size boundaries, or the split size was the
		 * same as the page size, and we never bothered with split point
		 * information at all.
		 */
		WT_RET(__rec_split_bnd_grow(session, r));
		last = &r->bnd[r->bnd_next++];
		next = last + 1;

		/*
		 * Set the key for the next chunk (before writing the block, a
		 * key range is needed in that code).
		 */
		next->recno = r->recno;
		if (dsk->type == WT_PAGE_ROW_INT ||
		    dsk->type == WT_PAGE_ROW_LEAF)
			WT_RET(__rec_split_row_promote(
			    session, r, &next->key, dsk->type));

		/* Clear the entries (not required, but cleaner). */
		next->entries = 0;

		/* Finalize the header information and write the page. */
		dsk->recno = last->recno;
		dsk->u.entries = r->entries;
		dsk->mem_size = r->dsk.size = WT_PTRDIFF32(r->first_free, dsk);
		WT_RET(__rec_split_write(session, r, last, &r->dsk, 0));

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
	case SPLIT_TRACKING_RAW:
	WT_ILLEGAL_VALUE(session);
	}

	/*
	 * Overflow values can be larger than the maximum page size but still be
	 * "on-page". If the next key/value pair is larger than space available
	 * after a split has happened (in other words, larger than the maximum
	 * page size), create a page sized to hold that one key/value pair. This
	 * generally splits the page into key/value pairs before a large object,
	 * the object, and key/value pairs after the object. It's possible other
	 * key/value pairs will also be aggregated onto the bigger page before
	 * or after, if the page happens to hold them, but it won't necessarily
	 * happen that way.
	 */
	if (r->space_avail < next_len)
		WT_RET(__rec_split_grow(session, r, next_len));

	return (0);
}

/*
 * __rec_split_raw_worker --
 *	Handle the raw compression page reconciliation bookkeeping.
 */
static int
__rec_split_raw_worker(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, size_t next_len, int no_more_rows)
{
	WT_BM *bm;
	WT_BOUNDARY *last, *next;
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COMPRESSOR *compressor;
	WT_DECL_RET;
	WT_ITEM *dst, *write_ref;
	WT_PAGE_HEADER *dsk, *dsk_dst;
	WT_SESSION *wt_session;
	size_t corrected_page_size, extra_skip, len, result_len;
	uint64_t recno;
	uint32_t entry, i, result_slots, slots;
	int last_block;
	uint8_t *dsk_start;

	wt_session = (WT_SESSION *)session;
	btree = S2BT(session);
	bm = btree->bm;

	unpack = &_unpack;
	compressor = btree->compressor;
	dst = &r->raw_destination;
	dsk = r->dsk.mem;

	WT_RET(__rec_split_bnd_grow(session, r));
	last = &r->bnd[r->bnd_next];
	next = last + 1;

	/*
	 * We can get here if the first key/value pair won't fit.
	 */
	if (r->entries == 0)
		goto split_grow;

	/*
	 * Build arrays of offsets and cumulative counts of cells and rows in
	 * the page: the offset is the byte offset to the possible split-point
	 * (adjusted for an initial chunk that cannot be compressed), entries
	 * is the cumulative page entries covered by the byte offset, recnos is
	 * the cumulative rows covered by the byte offset.
	 */
	if (r->entries >= r->raw_max_slots) {
		__wt_free(session, r->raw_entries);
		__wt_free(session, r->raw_offsets);
		__wt_free(session, r->raw_recnos);
		r->raw_max_slots = 0;

		i = r->entries + 100;
		WT_RET(__wt_calloc_def(session, i, &r->raw_entries));
		WT_RET(__wt_calloc_def(session, i, &r->raw_offsets));
		if (dsk->type == WT_PAGE_COL_INT ||
		    dsk->type == WT_PAGE_COL_VAR)
			WT_RET(__wt_calloc_def(session, i, &r->raw_recnos));
		r->raw_max_slots = i;
	}

	/*
	 * We're going to walk the disk image, which requires setting the
	 * number of entries.
	 */
	dsk->u.entries = r->entries;

	/*
	 * We track the record number at each column-store split point, set an
	 * initial value.
	 */
	recno = 0;
	if (dsk->type == WT_PAGE_COL_VAR)
		recno = last->recno;

	entry = slots = 0;
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i) {
		++entry;

		/*
		 * Row-store pages can split at keys, but not at values,
		 * column-store pages can split at values.
		 */
		__wt_cell_unpack(cell, unpack);
		switch (unpack->type) {
		case WT_CELL_KEY:
		case WT_CELL_KEY_OVFL:
		case WT_CELL_KEY_SHORT:
			break;
		case WT_CELL_ADDR_DEL:
		case WT_CELL_ADDR_INT:
		case WT_CELL_ADDR_LEAF:
		case WT_CELL_ADDR_LEAF_NO:
		case WT_CELL_DEL:
		case WT_CELL_VALUE:
		case WT_CELL_VALUE_OVFL:
		case WT_CELL_VALUE_SHORT:
			if (dsk->type == WT_PAGE_COL_INT) {
				recno = unpack->v;
				break;
			}
			if (dsk->type == WT_PAGE_COL_VAR) {
				recno += __wt_cell_rle(unpack);
				break;
			}
			r->raw_entries[slots] = entry;
			continue;
		WT_ILLEGAL_VALUE(session);
		}

		/*
		 * We can't compress the first 64B of the block (it must be
		 * written without compression), and a possible split point
		 * may appear in that 64B; keep it simple, ignore the first
		 * allocation size of data, anybody splitting smaller than
		 * that (as calculated before compression), is doing it wrong.
		 */
		if ((len = WT_PTRDIFF(cell, dsk)) > btree->allocsize)
			r->raw_offsets[++slots] =
			    WT_STORE_SIZE(len - WT_BLOCK_COMPRESS_SKIP);

		if (dsk->type == WT_PAGE_COL_INT ||
		    dsk->type == WT_PAGE_COL_VAR)
			r->raw_recnos[slots] = recno;
		r->raw_entries[slots] = entry;
	}

	/*
	 * If we haven't managed to find at least one split point, we're done,
	 * don't bother calling the underlying compression function.
	 */
	if (slots == 0) {
		result_len = 0;
		result_slots = 0;
		goto no_slots;
	}

	/* The slot at array's end is the total length of the data. */
	r->raw_offsets[++slots] =
	    WT_STORE_SIZE(WT_PTRDIFF(cell, dsk) - WT_BLOCK_COMPRESS_SKIP);

	/*
	 * Allocate a destination buffer. If there's a pre-size function, call
	 * it to determine the destination buffer's size, else the destination
	 * buffer is documented to be at least the source size. (We can't use
	 * the target page size, any single key/value could be larger than the
	 * page size. Don't bother figuring out a minimum, just use the source
	 * size.)
	 *
	 * The destination buffer needs to be large enough for the final block
	 * size, corrected for the requirements of the underlying block manager.
	 * If the final block size is 8KB, that's a multiple of 512B and so the
	 * underlying block manager is fine with it.  But... we don't control
	 * what the pre_size method returns us as a required size, and we don't
	 * want to document the compress_raw method has to skip bytes in the
	 * buffer because that's confusing, so do something more complicated.
	 * First, find out how much space the compress_raw function might need,
	 * either the value returned from pre_size, or the initial source size.
	 * Add the compress-skip bytes, and then correct that value for the
	 * underlying block manager. As a result, we have a destination buffer
	 * that's large enough when calling the compress_raw method, and there
	 * are bytes in the header just for us.
	 */
	if (compressor->pre_size == NULL)
		result_len = (size_t)r->raw_offsets[slots];
	else
		WT_RET(compressor->pre_size(compressor, wt_session,
		    (uint8_t *)dsk + WT_BLOCK_COMPRESS_SKIP,
		    (size_t)r->raw_offsets[slots], &result_len));
	extra_skip = 0;
	if (btree->kencryptor != NULL)
		extra_skip = btree->kencryptor->size_const +
		    WT_ENCRYPT_LEN_SIZE;

	corrected_page_size = result_len + WT_BLOCK_COMPRESS_SKIP;
	WT_RET(bm->write_size(bm, session, &corrected_page_size));
	WT_RET(__wt_buf_init(session, dst, corrected_page_size));

	/*
	 * Copy the header bytes into the destination buffer, then call the
	 * compression function.
	 */
	memcpy(dst->mem, dsk, WT_BLOCK_COMPRESS_SKIP);
	ret = compressor->compress_raw(compressor, wt_session,
	    r->page_size_orig, btree->split_pct,
	    WT_BLOCK_COMPRESS_SKIP + extra_skip,
	    (uint8_t *)dsk + WT_BLOCK_COMPRESS_SKIP,
	    r->raw_offsets, slots,
	    (uint8_t *)dst->mem + WT_BLOCK_COMPRESS_SKIP,
	    result_len, no_more_rows, &result_len, &result_slots);
	switch (ret) {
	case EAGAIN:
		/*
		 * The compression function wants more rows; accumulate and
		 * retry.
		 *
		 * Reset the resulting slots count, just in case the compression
		 * function modified it before giving up.
		 */
		result_slots = 0;
		break;
	case 0:
		/*
		 * If the compression function returned zero result slots, it's
		 * giving up and we write the original data.  (This is a pretty
		 * bad result: we've not done compression on a block much larger
		 * than the maximum page size, but once compression gives up,
		 * there's not much else we can do.)
		 *
		 * If the compression function returned non-zero result slots,
		 * we were successful and have a block to write.
		 */
		if (result_slots == 0) {
			WT_STAT_FAST_DATA_INCR(session, compress_raw_fail);

			/*
			 * If there are no more rows, we can write the original
			 * data from the original buffer.
			 */
			if (no_more_rows)
				break;

			/*
			 * Copy the original data to the destination buffer, as
			 * if the compression function simply copied it.  Take
			 * all but the last row of the original data (the last
			 * row has to be set as the key for the next block).
			 */
			result_slots = slots - 1;
			result_len = r->raw_offsets[result_slots];
			WT_RET(__wt_buf_grow(
			    session, dst, result_len + WT_BLOCK_COMPRESS_SKIP));
			memcpy((uint8_t *)dst->mem + WT_BLOCK_COMPRESS_SKIP,
			    (uint8_t *)dsk + WT_BLOCK_COMPRESS_SKIP,
			    result_len);

			/*
			 * Mark it as uncompressed so the standard compression
			 * function is called before the buffer is written.
			 */
			last->already_compressed = 0;
		} else {
			WT_STAT_FAST_DATA_INCR(session, compress_raw_ok);

			/*
			 * If there are more rows and the compression function
			 * consumed all of the current data, there are problems:
			 * First, with row-store objects, we're potentially
			 * skipping updates, we must have a key for the next
			 * block so we know with what block a skipped update is
			 * associated.  Second, if the compression function
			 * compressed all of the data, we're not pushing it
			 * hard enough (unless we got lucky and gave it exactly
			 * the right amount to work with, which is unlikely).
			 * Handle both problems by accumulating more data any
			 * time we're not writing the last block and compression
			 * ate all of the rows.
			 */
			if (result_slots == slots && !no_more_rows)
				result_slots = 0;
			else
				last->already_compressed = 1;
		}
		break;
	default:
		return (ret);
	}

no_slots:
	/*
	 * Check for the last block we're going to write: if no more rows and
	 * we failed to compress anything, or we compressed everything, it's
	 * the last block.
	 */
	last_block = no_more_rows &&
	    (result_slots == 0 || result_slots == slots);

	if (result_slots != 0) {
		/*
		 * We have a block, finalize the header information.
		 */
		dst->size = result_len + WT_BLOCK_COMPRESS_SKIP;
		dsk_dst = dst->mem;
		dsk_dst->recno = last->recno;
		dsk_dst->mem_size =
		    r->raw_offsets[result_slots] + WT_BLOCK_COMPRESS_SKIP;
		dsk_dst->u.entries = r->raw_entries[result_slots - 1];

		/*
		 * There is likely a remnant in the working buffer that didn't
		 * get compressed; copy it down to the start of the buffer and
		 * update the starting record number, free space and so on.
		 * !!!
		 * Note use of memmove, the source and destination buffers can
		 * overlap.
		 */
		len = WT_PTRDIFF(
		    r->first_free, (uint8_t *)dsk + dsk_dst->mem_size);
		dsk_start = WT_PAGE_HEADER_BYTE(btree, dsk);
		(void)memmove(dsk_start, (uint8_t *)r->first_free - len, len);

		r->entries -= r->raw_entries[result_slots - 1];
		r->first_free = dsk_start + len;
		r->space_avail += r->raw_offsets[result_slots];
		WT_ASSERT(session, r->first_free + r->space_avail <=
		    (uint8_t *)r->dsk.mem + r->dsk.memsize);

		/*
		 * Set the key for the next block (before writing the block, a
		 * key range is needed in that code).
		 */
		switch (dsk->type) {
		case WT_PAGE_COL_INT:
			next->recno = r->raw_recnos[result_slots];
			break;
		case WT_PAGE_COL_VAR:
			next->recno = r->raw_recnos[result_slots - 1];
			break;
		case WT_PAGE_ROW_INT:
		case WT_PAGE_ROW_LEAF:
			next->recno = 0;
			if (!last_block) {
				/*
				 * Confirm there was uncompressed data remaining
				 * in the buffer, we're about to read it for the
				 * next chunk's initial key.
				 */
				WT_ASSERT(session, len > 0);
				WT_RET(__rec_split_row_promote_cell(
				    session, dsk, &next->key));
			}
			break;
		}
		write_ref = dst;
	} else if (no_more_rows) {
		/*
		 * Compression failed and there are no more rows to accumulate,
		 * write the original buffer instead.
		 */
		WT_STAT_FAST_DATA_INCR(session, compress_raw_fail);

		dsk->recno = last->recno;
		dsk->mem_size = r->dsk.size = WT_PTRDIFF32(r->first_free, dsk);
		dsk->u.entries = r->entries;

		r->entries = 0;
		r->first_free = WT_PAGE_HEADER_BYTE(btree, dsk);
		r->space_avail = r->page_size - WT_PAGE_HEADER_BYTE_SIZE(btree);

		write_ref = &r->dsk;
		last->already_compressed = 0;
	} else {
		/*
		 * Compression failed, there are more rows to accumulate and the
		 * compression function wants to try again; increase the size of
		 * the "page" and try again after we accumulate some more rows.
		 */
		WT_STAT_FAST_DATA_INCR(session, compress_raw_fail_temporary);
		goto split_grow;
	}

	/* We have a block, update the boundary counter. */
	++r->bnd_next;

	/*
	 * If we are writing the whole page in our first/only attempt, it might
	 * be a checkpoint (checkpoints are only a single page, by definition).
	 * Further, checkpoints aren't written here, the wrapup functions do the
	 * write, and they do the write from the original buffer location.  If
	 * it's a checkpoint and the block isn't in the right buffer, copy it.
	 *
	 * If it's not a checkpoint, write the block.
	 */
	if (r->bnd_next == 1 && last_block && __rec_is_checkpoint(r, last)) {
		if (write_ref == dst)
			WT_RET(__wt_buf_set(
			    session, &r->dsk, dst->mem, dst->size));
	} else
		WT_RET(
		    __rec_split_write(session, r, last, write_ref, last_block));

	/*
	 * We got called because there wasn't enough room in the buffer for the
	 * next key and we might or might not have written a block. In any case,
	 * make sure the next key fits into the buffer.
	 */
	if (r->space_avail < next_len) {
split_grow:	/*
		 * Double the page size and make sure we accommodate at least
		 * one more record. The reason for the latter is that we may
		 * be here because there's a large key/value pair that won't
		 * fit in our initial page buffer, even at its expanded size.
		 */
		r->page_size *= 2;
		return (__rec_split_grow(session, r, r->page_size + next_len));
	}
	return (0);
}

/*
 * __rec_raw_decompress --
 *	Decompress a raw-compressed image.
 */
static int
__rec_raw_decompress(
    WT_SESSION_IMPL *session, const void *image, size_t size, void *retp)
{
	WT_BTREE *btree;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_PAGE_HEADER const *dsk;
	size_t result_len;

	btree = S2BT(session);
	dsk = image;

	/*
	 * We skipped an update and we can't write a block, but unfortunately,
	 * the block has already been compressed. Decompress the block so we
	 * can subsequently re-instantiate it in memory.
	 */
	WT_RET(__wt_scr_alloc(session, dsk->mem_size, &tmp));
	memcpy(tmp->mem, image, WT_BLOCK_COMPRESS_SKIP);
	WT_ERR(btree->compressor->decompress(btree->compressor,
	    &session->iface,
	    (uint8_t *)image + WT_BLOCK_COMPRESS_SKIP,
	    size - WT_BLOCK_COMPRESS_SKIP,
	    (uint8_t *)tmp->mem + WT_BLOCK_COMPRESS_SKIP,
	    dsk->mem_size - WT_BLOCK_COMPRESS_SKIP,
	    &result_len));
	if (result_len != dsk->mem_size - WT_BLOCK_COMPRESS_SKIP)
		WT_ERR(__wt_illegal_value(session, btree->dhandle->name));

	WT_ERR(__wt_strndup(session, tmp->data, dsk->mem_size, retp));
	WT_ASSERT(session, __wt_verify_dsk_image(
	    session, "[raw evict split]", tmp->data, dsk->mem_size, 0) == 0);

err:	__wt_scr_free(session, &tmp);
	return (ret);
}

/*
 * __rec_split_raw --
 *	Raw compression split routine.
 */
static inline int
__rec_split_raw(WT_SESSION_IMPL *session, WT_RECONCILE *r, size_t next_len)
{
	return (__rec_split_raw_worker(session, r, next_len, 0));
}

/*
 * __rec_split_finish_std --
 *	Finish processing a page, standard version.
 */
static int
__rec_split_finish_std(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
	WT_BOUNDARY *bnd;
	WT_PAGE_HEADER *dsk;

	/* Adjust the boundary information based on our split status. */
	switch (r->bnd_state) {
	case SPLIT_BOUNDARY:
	case SPLIT_MAX:
		/*
		 * We never split, the reconciled page fit into a maximum page
		 * size.  Change the first boundary slot to represent the full
		 * page (the first boundary slot is largely correct, just update
		 * the number of entries).
		 */
		r->bnd_next = 0;
		break;
	case SPLIT_TRACKING_OFF:
		/*
		 * If we have already split, or aren't tracking boundaries, put
		 * the remaining data in the next boundary slot.
		 */
		WT_RET(__rec_split_bnd_grow(session, r));
		break;
	case SPLIT_TRACKING_RAW:
		/*
		 * We were configured for raw compression, but never actually
		 * wrote anything.
		 */
		break;
	WT_ILLEGAL_VALUE(session);
	}

	/*
	 * We only arrive here with no entries to write if the page was entirely
	 * empty, and if the page is empty, we merge it into its parent during
	 * the parent's reconciliation.  A page with skipped updates isn't truly
	 * empty, continue on.
	 */
	if (r->entries == 0 && r->skip_next == 0)
		return (0);

	/* Set the boundary reference and increment the count. */
	bnd = &r->bnd[r->bnd_next++];
	bnd->entries = r->entries;

	/* Finalize the header information. */
	dsk = r->dsk.mem;
	dsk->recno = bnd->recno;
	dsk->u.entries = r->entries;
	dsk->mem_size = r->dsk.size = WT_PTRDIFF32(r->first_free, dsk);

	/* If this is a checkpoint, we're done, otherwise write the page. */
	return (
	    __rec_is_checkpoint(r, bnd) ? 0 :
	    __rec_split_write(session, r, bnd, &r->dsk, 1));
}

/*
 * __rec_split_finish --
 *	Finish processing a page.
 */
static int
__rec_split_finish(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
	/* We're done reconciling - write the final page */
	if (r->raw_compression && r->entries != 0) {
		while (r->entries != 0)
			WT_RET(__rec_split_raw_worker(session, r, 0, 1));
	} else
		WT_RET(__rec_split_finish_std(session, r));

	return (0);
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
	size_t i, len;
	uint8_t *dsk_start, *p;

	/*
	 * When we overflow physical limits of the page, we walk the list of
	 * split chunks we've created and write those pages out, then update
	 * the caller's information.
	 */
	btree = S2BT(session);

	/*
	 * The data isn't laid out on a page boundary or nul padded; copy it to
	 * a clean, aligned, padded buffer before writing it.
	 *
	 * Allocate a scratch buffer to hold the new disk image.  Copy the
	 * WT_PAGE_HEADER header onto the scratch buffer, most of the header
	 * information remains unchanged between the pages.
	 */
	WT_RET(__wt_scr_alloc(session, r->dsk.memsize, &tmp));
	dsk = tmp->mem;
	memcpy(dsk, r->dsk.mem, WT_PAGE_HEADER_SIZE);

	/*
	 * For each split chunk we've created, update the disk image and copy
	 * it into place.
	 */
	dsk_start = WT_PAGE_HEADER_BYTE(btree, dsk);
	for (i = 0, bnd = r->bnd; i < r->bnd_next; ++i, ++bnd) {
		/* Copy the page contents to the temporary buffer. */
		len = (bnd + 1)->offset - bnd->offset;
		memcpy(dsk_start, (uint8_t *)r->dsk.mem + bnd->offset, len);

		/* Finalize the header information and write the page. */
		dsk->recno = bnd->recno;
		dsk->u.entries = bnd->entries;
		tmp->size = WT_PAGE_HEADER_BYTE_SIZE(btree) + len;
		dsk->mem_size = WT_STORE_SIZE(tmp->size);
		WT_ERR(__rec_split_write(session, r, bnd, tmp, 0));
	}

	/*
	 * There is probably a remnant in the working buffer that didn't get
	 * written, copy it down to the beginning of the working buffer.
	 *
	 * Confirm the remnant is no larger than a split-sized chunk, including
	 * header. We know that's the maximum sized remnant because we only have
	 * remnants if split switches from accumulating to a split boundary to
	 * accumulating to the end of the page (the other path here is when we
	 * hit a split boundary, there was room for another split chunk in the
	 * page, and the next item still wouldn't fit, in which case there is no
	 * remnant). So: we were accumulating to the end of the page and created
	 * a remnant. We know the remnant cannot be as large as a split-sized
	 * chunk, including header, because if there was room for that large a
	 * remnant, we wouldn't have switched from accumulating to a page end.
	 */
	p = (uint8_t *)r->dsk.mem + bnd->offset;
	len = WT_PTRDIFF(r->first_free, p);
	if (len >= r->split_size - WT_PAGE_HEADER_BYTE_SIZE(btree))
		WT_PANIC_ERR(session, EINVAL,
		    "Reconciliation remnant too large for the split buffer");
	dsk = r->dsk.mem;
	dsk_start = WT_PAGE_HEADER_BYTE(btree, dsk);
	(void)memmove(dsk_start, p, len);

	/*
	 * Fix up our caller's information, including updating the starting
	 * record number.
	 */
	r->entries -= r->total_entries;
	r->first_free = dsk_start + len;
	WT_ASSERT(session,
	    r->page_size >= (WT_PAGE_HEADER_BYTE_SIZE(btree) + len));
	r->space_avail =
	    r->split_size - (WT_PAGE_HEADER_BYTE_SIZE(btree) + len);

err:	__wt_scr_free(session, &tmp);
	return (ret);
}

/*
 * __rec_split_write --
 *	Write a disk block out for the split helper functions.
 */
static int
__rec_split_write(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, WT_BOUNDARY *bnd, WT_ITEM *buf, int last_block)
{
	WT_BTREE *btree;
	WT_DECL_ITEM(key);
	WT_DECL_RET;
	WT_MULTI *multi;
	WT_PAGE *page;
	WT_PAGE_HEADER *dsk;
	WT_PAGE_MODIFY *mod;
	WT_UPD_SKIPPED *skip;
	size_t addr_size;
	uint32_t bnd_slot, i, j;
	int cmp;
	uint8_t addr[WT_BTREE_MAX_ADDR_COOKIE];

	btree = S2BT(session);
	dsk = buf->mem;
	page = r->page;
	mod = page->modify;

	WT_RET(__wt_scr_alloc(session, 0, &key));

	/* Set the zero-length value flag in the page header. */
	if (dsk->type == WT_PAGE_ROW_LEAF) {
		F_CLR(dsk, WT_PAGE_EMPTY_V_ALL | WT_PAGE_EMPTY_V_NONE);

		if (r->entries != 0 && r->all_empty_value)
			F_SET(dsk, WT_PAGE_EMPTY_V_ALL);
		if (r->entries != 0 && !r->any_empty_value)
			F_SET(dsk, WT_PAGE_EMPTY_V_NONE);
	}

	/* Initialize the address (set the page type for the parent). */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		bnd->addr.type = WT_ADDR_LEAF_NO;
		break;
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_LEAF:
		bnd->addr.type = r->ovfl_items ? WT_ADDR_LEAF : WT_ADDR_LEAF_NO;
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		bnd->addr.type = WT_ADDR_INT;
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

	bnd->size = (uint32_t)buf->size;
	bnd->cksum = 0;

	/*
	 * Check if we've skipped updates that belong to this block, and move
	 * any to the per-block structure.  Quit as soon as we find a skipped
	 * update that doesn't belong to the block, they're in sorted order.
	 *
	 * This code requires a key be filled in for the next block (or the
	 * last block flag be set, if there's no next block).
	 */
	for (i = 0, skip = r->skip; i < r->skip_next; ++i, ++skip) {
		/* The last block gets all remaining skipped updates. */
		if (last_block) {
			WT_ERR(__rec_skip_update_move(session, bnd, skip));
			continue;
		}

		/*
		 * Get the skipped update's key and compare it with this block's
		 * key range.  If the skipped update list belongs with the block
		 * we're about to write, move it to the per-block memory.  Check
		 * only to the first update that doesn't go with the block, they
		 * must be in sorted order.
		 */
		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			if (WT_INSERT_RECNO(skip->ins) >= (bnd + 1)->recno)
				goto skip_check_complete;
			break;
		case WT_PAGE_ROW_LEAF:
			if (skip->ins == NULL)
				WT_ERR(__wt_row_leaf_key(
				    session, page, skip->rip, key, 0));
			else {
				key->data = WT_INSERT_KEY(skip->ins);
				key->size = WT_INSERT_KEY_SIZE(skip->ins);
			}
			WT_ERR(__wt_compare(session,
			    btree->collator, key, &(bnd + 1)->key, &cmp));
			if (cmp >= 0)
				goto skip_check_complete;
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}
		WT_ERR(__rec_skip_update_move(session, bnd, skip));
	}

skip_check_complete:
	/*
	 * If there are updates that weren't moved to the block, shuffle them to
	 * the beginning of the cached list (we maintain the skipped updates in
	 * sorted order, new skipped updates must be appended to the list).
	 */
	for (j = 0; i < r->skip_next; ++j, ++i)
		r->skip[j] = r->skip[i];
	r->skip_next = j;

	/*
	 * If we had to skip updates in order to build this disk image, we can't
	 * actually write it. Instead, we will re-instantiate the page using the
	 * disk image and the list of updates we skipped.
	 */
	if (bnd->skip != NULL) {
		/*
		 * If the buffer is compressed (raw compression was configured),
		 * we have to decompress it so we can instantiate it later. It's
		 * a slow and convoluted path, but it's also a rare one and it's
		 * not worth making it faster. Else, the disk image is ready,
		 * copy it into place for later. It's possible the disk image
		 * has no items; we have to flag that for verification, it's a
		 * special case since read/writing empty pages isn't generally
		 * allowed.
		 */
		if (bnd->already_compressed)
			WT_ERR(__rec_raw_decompress(
			    session, buf->data, buf->size, &bnd->dsk));
		else {
			WT_ERR(__wt_strndup(
			    session, buf->data, buf->size, &bnd->dsk));
			WT_ASSERT(session, __wt_verify_dsk_image(session,
			    "[evict split]", buf->data, buf->size, 1) == 0);
		}
		goto done;
	}

	/*
	 * If we wrote this block before, re-use it.  Pages get written in the
	 * same block order every time, only check the appropriate slot.  The
	 * expensive part of this test is the checksum, only do that work when
	 * there has been or will be a reconciliation of this page involving
	 * split pages.  This test isn't perfect: we're doing a checksum if a
	 * previous reconciliation of the page split or if we will split this
	 * time, but that test won't calculate a checksum on the first block
	 * the first time the page splits.
	 */
	bnd_slot = (uint32_t)(bnd - r->bnd);
	if (bnd_slot > 1 ||
	    (F_ISSET(mod, WT_PM_REC_MULTIBLOCK) && mod->mod_multi != NULL)) {
		/*
		 * There are page header fields which need to be cleared to get
		 * consistent checksums: specifically, the write generation and
		 * the memory owned by the block manager.  We are reusing the
		 * same buffer space each time, clear it before calculating the
		 * checksum.
		 */
		dsk->write_gen = 0;
		memset(WT_BLOCK_HEADER_REF(dsk), 0, btree->block_header);
		bnd->cksum = __wt_cksum(buf->data, buf->size);

		if (F_ISSET(mod, WT_PM_REC_MULTIBLOCK) &&
		    mod->mod_multi_entries > bnd_slot) {
			multi = &mod->mod_multi[bnd_slot];
			if (multi->size == bnd->size &&
			    multi->cksum == bnd->cksum) {
				multi->addr.reuse = 1;
				bnd->addr = multi->addr;

				WT_STAT_FAST_DATA_INCR(session, rec_page_match);
				goto done;
			}
		}
	}

	WT_ERR(__wt_bt_write(session,
	    buf, addr, &addr_size, 0, bnd->already_compressed));
	WT_ERR(__wt_strndup(session, addr, addr_size, &bnd->addr.addr));
	bnd->addr.size = (uint8_t)addr_size;

done:
err:	__wt_scr_free(session, &key);
	return (ret);
}

/*
 * __wt_bulk_init --
 *	Bulk insert initialization.
 */
int
__wt_bulk_init(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
{
	WT_BTREE *btree;
	WT_PAGE_INDEX *pindex;
	WT_RECONCILE *r;
	uint64_t recno;

	btree = S2BT(session);
	/*
	 * Bulk-load is only permitted on newly created files, not any empty
	 * file -- see the checkpoint code for a discussion.
	 */
	if (!btree->bulk_load_ok)
		WT_RET_MSG(session, EINVAL,
		    "bulk-load is only possible for newly created trees");

	/*
	 * Get a reference to the empty leaf page; we have exclusive access so
	 * we can take a copy of the page, confident the parent won't split.
	 */
	pindex = WT_INTL_INDEX_GET_SAFE(btree->root.page);
	cbulk->ref = pindex->index[0];
	cbulk->leaf = cbulk->ref->page;

	WT_RET(
	    __rec_write_init(session, cbulk->ref, 0, NULL, &cbulk->reconcile));
	r = cbulk->reconcile;
	r->is_bulk_load = 1;

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

	return (__rec_split_init(
	    session, r, cbulk->leaf, recno, btree->maxleafpage));
}

/*
 * __wt_bulk_wrapup --
 *	Bulk insert cleanup.
 */
int
__wt_bulk_wrapup(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
{
	WT_BTREE *btree;
	WT_PAGE *parent;
	WT_RECONCILE *r;

	r = cbulk->reconcile;
	btree = S2BT(session);

	switch (btree->type) {
	case BTREE_COL_FIX:
		if (cbulk->entry != 0)
			__rec_incr(session, r, cbulk->entry,
			    __bitstr_size(
			    (size_t)cbulk->entry * btree->bitcnt));
		break;
	case BTREE_COL_VAR:
		if (cbulk->rle != 0)
			WT_RET(__wt_bulk_insert_var(session, cbulk));
		break;
	case BTREE_ROW:
		break;
	WT_ILLEGAL_VALUE(session);
	}

	WT_RET(__rec_split_finish(session, r));
	WT_RET(__rec_write_wrapup(session, r, r->page));

	/* Mark the page's parent and the tree dirty. */
	parent = r->ref->home;
	WT_RET(__wt_page_modify_init(session, parent));
	__wt_page_modify_set(session, parent);

	__rec_destroy(session, &cbulk->reconcile);

	return (0);
}

/*
 * __wt_bulk_insert_row --
 *	Row-store bulk insert.
 */
int
__wt_bulk_insert_row(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_KV *key, *val;
	WT_RECONCILE *r;
	int ovfl_key;

	r = cbulk->reconcile;
	btree = S2BT(session);
	cursor = &cbulk->cbt.iface;

	key = &r->k;
	val = &r->v;
	WT_RET(__rec_cell_build_leaf_key(session, r,	/* Build key cell */
	    cursor->key.data, cursor->key.size, &ovfl_key));
	WT_RET(__rec_cell_build_val(session, r,		/* Build value cell */
	    cursor->value.data, cursor->value.size, (uint64_t)0));

	/* Boundary: split or write the page. */
	if (key->len + val->len > r->space_avail) {
		if (r->raw_compression)
			WT_RET(
			    __rec_split_raw(session, r, key->len + val->len));
		else {
			/*
			 * Turn off prefix compression until a full key written
			 * to the new page, and (unless already working with an
			 * overflow key), rebuild the key without compression.
			 */
			if (r->key_pfx_compress_conf) {
				r->key_pfx_compress = 0;
				if (!ovfl_key)
					WT_RET(__rec_cell_build_leaf_key(
					    session, r, NULL, 0, &ovfl_key));
			}

			WT_RET(__rec_split(session, r, key->len + val->len));
		}
	}

	/* Copy the key/value pair onto the page. */
	__rec_copy_incr(session, r, key);
	if (val->len == 0)
		r->any_empty_value = 1;
	else {
		r->all_empty_value = 0;
		if (btree->dictionary)
			WT_RET(__rec_dict_replace(session, r, 0, val));
		__rec_copy_incr(session, r, val);
	}

	/* Update compression state. */
	__rec_key_state_update(r, ovfl_key);

	return (0);
}

/*
 * __rec_col_fix_bulk_insert_split_check --
 *	Check if a bulk-loaded fixed-length column store page needs to split.
 */
static inline int
__rec_col_fix_bulk_insert_split_check(WT_CURSOR_BULK *cbulk)
{
	WT_BTREE *btree;
	WT_RECONCILE *r;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	r = cbulk->reconcile;
	btree = S2BT(session);

	if (cbulk->entry == cbulk->nrecs) {
		if (cbulk->entry != 0) {
			/*
			 * If everything didn't fit, update the counters and
			 * split.
			 *
			 * Boundary: split or write the page.
			 */
			__rec_incr(session, r, cbulk->entry,
			    __bitstr_size(
			    (size_t)cbulk->entry * btree->bitcnt));
			WT_RET(__rec_split(session, r, 0));
		}
		cbulk->entry = 0;
		cbulk->nrecs = WT_FIX_BYTES_TO_ENTRIES(btree, r->space_avail);
	}
	return (0);
}

/*
 * __wt_bulk_insert_fix --
 *	Fixed-length column-store bulk insert.
 */
int
__wt_bulk_insert_fix(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_RECONCILE *r;
	uint32_t entries, offset, page_entries, page_size;
	const uint8_t *data;

	r = cbulk->reconcile;
	btree = S2BT(session);
	cursor = &cbulk->cbt.iface;

	if (cbulk->bitmap) {
		if (((r->recno - 1) * btree->bitcnt) & 0x7)
			WT_RET_MSG(session, EINVAL,
			    "Bulk bitmap load not aligned on a byte boundary");
		for (data = cursor->value.data,
		    entries = (uint32_t)cursor->value.size;
		    entries > 0;
		    entries -= page_entries, data += page_size) {
			WT_RET(__rec_col_fix_bulk_insert_split_check(cbulk));

			page_entries =
			    WT_MIN(entries, cbulk->nrecs - cbulk->entry);
			page_size = __bitstr_size(page_entries * btree->bitcnt);
			offset = __bitstr_size(cbulk->entry * btree->bitcnt);
			memcpy(r->first_free + offset, data, page_size);
			cbulk->entry += page_entries;
			r->recno += page_entries;
		}
		return (0);
	}

	WT_RET(__rec_col_fix_bulk_insert_split_check(cbulk));

	__bit_setv(r->first_free,
	    cbulk->entry, btree->bitcnt, ((uint8_t *)cursor->value.data)[0]);
	++cbulk->entry;
	++r->recno;

	return (0);
}

/*
 * __wt_bulk_insert_var --
 *	Variable-length column-store bulk insert.
 */
int
__wt_bulk_insert_var(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
{
	WT_BTREE *btree;
	WT_KV *val;
	WT_RECONCILE *r;

	r = cbulk->reconcile;
	btree = S2BT(session);

	/*
	 * Store the bulk cursor's last buffer, not the current value, we're
	 * creating a duplicate count, which means we want the previous value
	 * seen, not the current value.
	 */
	val = &r->v;
	WT_RET(__rec_cell_build_val(
	    session, r, cbulk->last.data, cbulk->last.size, cbulk->rle));

	/* Boundary: split or write the page. */
	if (val->len > r->space_avail)
		WT_RET(r->raw_compression ?
		    __rec_split_raw(session, r, val->len) :
		    __rec_split(session, r, val->len));

	/* Copy the value onto the page. */
	if (btree->dictionary)
		WT_RET(__rec_dict_replace(session, r, cbulk->rle, val));
	__rec_copy_incr(session, r, val);

	/* Update the starting record number in case we split. */
	r->recno += cbulk->rle;

	return (0);
}

/*
 * __rec_vtype --
 *	Return a value cell's address type.
 */
static inline u_int
__rec_vtype(WT_ADDR *addr)
{
	if (addr->type == WT_ADDR_INT)
		return (WT_CELL_ADDR_INT);
	if (addr->type == WT_ADDR_LEAF)
		return (WT_CELL_ADDR_LEAF);
	return (WT_CELL_ADDR_LEAF_NO);
}

/*
 * __rec_col_int --
 *	Reconcile a column-store internal page.
 */
static int
__rec_col_int(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_ADDR *addr;
	WT_BTREE *btree;
	WT_CELL_UNPACK *vpack, _vpack;
	WT_DECL_RET;
	WT_KV *val;
	WT_PAGE *child;
	WT_REF *ref;
	int hazard, state;

	btree = S2BT(session);
	child = NULL;
	hazard = 0;

	val = &r->v;
	vpack = &_vpack;

	WT_RET(__rec_split_init(
	    session, r, page, page->pg_intl_recno, btree->maxintlpage));

	/* For each entry in the in-memory page... */
	WT_INTL_FOREACH_BEGIN(session, page, ref) {
		/* Update the starting record number in case we split. */
		r->recno = ref->key.recno;

		/*
		 * Modified child.
		 * The page may be emptied or internally created during a split.
		 * Deleted/split pages are merged into the parent and discarded.
		 */
		WT_ERR(__rec_child_modify(session, r, ref, &hazard, &state));
		addr = NULL;
		child = ref->page;

		/* Deleted child we don't have to write. */
		if (state == WT_CHILD_IGNORE) {
			CHILD_RELEASE_ERR(session, hazard, ref);
			continue;
		}

		/*
		 * Modified child.  Empty pages are merged into the parent and
		 * discarded.
		 */
		if (state == WT_CHILD_MODIFIED) {
			switch (F_ISSET(child->modify, WT_PM_REC_MASK)) {
			case WT_PM_REC_EMPTY:
				/*
				 * Column-store pages are almost never empty, as
				 * discarding a page would remove a chunk of the
				 * name space.  The exceptions are pages created
				 * when the tree is created, and never filled.
				 */
				CHILD_RELEASE_ERR(session, hazard, ref);
				continue;
			case WT_PM_REC_MULTIBLOCK:
				WT_ERR(__rec_col_merge(session, r, child));
				CHILD_RELEASE_ERR(session, hazard, ref);
				continue;
			case WT_PM_REC_REPLACE:
				addr = &child->modify->mod_replace;
				break;
			case WT_PM_REC_REWRITE:
				break;
			WT_ILLEGAL_VALUE_ERR(session);
			}
		} else
			/* No other states are expected for column stores. */
			WT_ASSERT(session, state == 0);

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
			__wt_cell_unpack(ref->addr, vpack);
			val->buf.data = ref->addr;
			val->buf.size = __wt_cell_total_len(vpack);
			val->cell_len = 0;
			val->len = val->buf.size;
		} else
			__rec_cell_build_addr(r, addr->addr, addr->size,
			    __rec_vtype(addr), ref->key.recno);
		CHILD_RELEASE_ERR(session, hazard, ref);

		/* Boundary: split or write the page. */
		if (val->len > r->space_avail)
			WT_ERR(r->raw_compression ?
			    __rec_split_raw(session, r, val->len) :
			    __rec_split(session, r, val->len));

		/* Copy the value onto the page. */
		__rec_copy_incr(session, r, val);
	} WT_INTL_FOREACH_END;

	/* Write the remnant page. */
	return (__rec_split_finish(session, r));

err:	CHILD_RELEASE(session, hazard, ref);
	return (ret);
}

/*
 * __rec_col_merge --
 *	Merge in a split page.
 */
static int
__rec_col_merge(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_ADDR *addr;
	WT_KV *val;
	WT_MULTI *multi;
	WT_PAGE_MODIFY *mod;
	uint32_t i;

	mod = page->modify;

	val = &r->v;

	/* For each entry in the split array... */
	for (multi = mod->mod_multi,
	    i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
		/* Update the starting record number in case we split. */
		r->recno = multi->key.recno;

		/* Build the value cell. */
		addr = &multi->addr;
		__rec_cell_build_addr(r,
		    addr->addr, addr->size, __rec_vtype(addr), r->recno);

		/* Boundary: split or write the page. */
		if (val->len > r->space_avail)
			WT_RET(r->raw_compression ?
			    __rec_split_raw(session, r, val->len) :
			    __rec_split(session, r, val->len));

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
	WT_UPDATE *upd;
	uint64_t recno;
	uint32_t entry, nrecs;

	btree = S2BT(session);

	WT_RET(__rec_split_init(
	    session, r, page, page->pg_fix_recno, btree->maxleafpage));

	/* Update any changes to the original on-page data items. */
	WT_SKIP_FOREACH(ins, WT_COL_UPDATE_SINGLE(page)) {
		WT_RET(__rec_txn_read(session, r, ins, NULL, NULL, &upd));
		if (upd != NULL)
			__bit_setv_recno(page, WT_INSERT_RECNO(ins),
			    btree->bitcnt, ((uint8_t *)WT_UPDATE_DATA(upd))[0]);
	}

	/* Copy the updated, disk-image bytes into place. */
	memcpy(r->first_free, page->pg_fix_bitf,
	    __bitstr_size((size_t)page->pg_fix_entries * btree->bitcnt));

	/* Calculate the number of entries per page remainder. */
	entry = page->pg_fix_entries;
	nrecs = WT_FIX_BYTES_TO_ENTRIES(
	    btree, r->space_avail) - page->pg_fix_entries;
	r->recno += entry;

	/* Walk any append list. */
	WT_SKIP_FOREACH(ins, WT_COL_APPEND(page)) {
		WT_RET(__rec_txn_read(session, r, ins, NULL, NULL, &upd));
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
			__rec_incr(session, r, entry,
			    __bitstr_size((size_t)entry * btree->bitcnt));
			WT_RET(__rec_split(session, r, 0));

			/* Calculate the number of entries per page. */
			entry = 0;
			nrecs = WT_FIX_BYTES_TO_ENTRIES(btree, r->space_avail);
		}
	}

	/* Update the counters. */
	__rec_incr(
	    session, r, entry, __bitstr_size((size_t)entry * btree->bitcnt));

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

	btree = S2BT(session);

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
	WT_RET(__rec_split_init(
	    session, r, page, page->pg_fix_recno, btree->maxleafpage));

	/* We may not be taking all of the entries on the original page. */
	page_take = salvage->take == 0 ? page->pg_fix_entries : salvage->take;
	page_start = salvage->skip == 0 ? 0 : salvage->skip;

	/* Calculate the number of entries per page. */
	entry = 0;
	nrecs = WT_FIX_BYTES_TO_ENTRIES(btree, r->space_avail);

	for (; nrecs > 0 && salvage->missing > 0;
	    --nrecs, --salvage->missing, ++entry)
		__bit_setv(r->first_free, entry, btree->bitcnt, 0);

	for (; nrecs > 0 && page_take > 0;
	    --nrecs, --page_take, ++page_start, ++entry)
		__bit_setv(r->first_free, entry, btree->bitcnt,
		    __bit_getv(page->pg_fix_bitf,
			(uint32_t)page_start, btree->bitcnt));

	r->recno += entry;
	__rec_incr(session, r, entry,
	    __bitstr_size((size_t)entry * btree->bitcnt));

	/*
	 * We can't split during salvage -- if everything didn't fit, it's
	 * all gone wrong.
	 */
	if (salvage->missing != 0 || page_take != 0)
		WT_PANIC_RET(session, WT_PANIC,
		    "%s page too large, attempted split during salvage",
		    __wt_page_type_string(page->type));

	/* Write the page. */
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
    WT_ITEM *value, int deleted, uint8_t overflow_type, uint64_t rle)
{
	WT_BTREE *btree;
	WT_KV *val;

	btree = S2BT(session);

	val = &r->v;

	/*
	 * Occasionally, salvage needs to discard records from the beginning or
	 * end of the page, and because the items may be part of a RLE cell, do
	 * the adjustments here. It's not a mistake we don't bother telling
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
			rle -= salvage->skip;
			salvage->skip = 0;
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
	} else if (overflow_type) {
		val->cell_len = __wt_cell_pack_ovfl(
		    &val->cell, overflow_type, rle, value->size);
		val->buf.data = value->data;
		val->buf.size = value->size;
		val->len = val->cell_len + value->size;
	} else
		WT_RET(__rec_cell_build_val(
		    session, r, value->data, value->size, rle));

	/* Boundary: split or write the page. */
	if (val->len > r->space_avail)
		WT_RET(r->raw_compression ?
		    __rec_split_raw(session, r, val->len) :
		    __rec_split(session, r, val->len));

	/* Copy the value onto the page. */
	if (!deleted && !overflow_type && btree->dictionary)
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
	WT_CELL_UNPACK *vpack, _vpack;
	WT_COL *cip;
	WT_DECL_ITEM(orig);
	WT_DECL_RET;
	WT_INSERT *ins;
	WT_ITEM *last;
	WT_UPDATE *upd;
	uint64_t n, nrepeat, repeat_count, rle, skip, src_recno;
	uint32_t i, size;
	int deleted, last_deleted, orig_deleted, update_no_copy;
	const void *data;

	btree = S2BT(session);
	last = r->last;
	vpack = &_vpack;

	WT_RET(__wt_scr_alloc(session, 0, &orig));
	data = NULL;
	size = 0;
	upd = NULL;

	WT_RET(__rec_split_init(
	    session, r, page, page->pg_var_recno, btree->maxleafpage));

	/*
	 * The salvage code may be calling us to reconcile a page where there
	 * were missing records in the column-store name space.  If taking the
	 * first record from on the page, it might be a deleted record, so we
	 * have to give the RLE code a chance to figure that out.  Else, if
	 * not taking the first record from the page, write a single element
	 * representing the missing records onto a new page.  (Don't pass the
	 * salvage cookie to our helper function in this case, we're handling
	 * one of the salvage cookie fields on our own, and we don't need the
	 * helper function's assistance.)
	 */
	rle = 0;
	last_deleted = 0;
	if (salvage != NULL && salvage->missing != 0) {
		if (salvage->skip == 0) {
			rle = salvage->missing;
			last_deleted = 1;

			/*
			 * Correct the number of records we're going to "take",
			 * pretending the missing records were on the page.
			 */
			salvage->take += salvage->missing;
		} else
			WT_ERR(__rec_col_var_helper(
			    session, r, NULL, NULL, 1, 0, salvage->missing));
	}

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
	src_recno = r->recno + rle;

	/* For each entry in the in-memory page... */
	WT_COL_FOREACH(page, cip, i) {
		ovfl_state = OVFL_IGNORE;
		if ((cell = WT_COL_PTR(page, cip)) == NULL) {
			nrepeat = 1;
			ins = NULL;
			orig_deleted = 1;
		} else {
			__wt_cell_unpack(cell, vpack);
			nrepeat = __wt_cell_rle(vpack);
			ins = WT_SKIP_FIRST(WT_COL_UPDATE(page, cip));

			/*
			 * If the original value is "deleted", there's no value
			 * to compare, we're done.
			 */
			orig_deleted = vpack->type == WT_CELL_DEL ? 1 : 0;
			if (orig_deleted)
				goto record_loop;

			/*
			 * Overflow items are tricky: we don't know until we're
			 * finished processing the set of values if we need the
			 * overflow value or not.  If we don't use the overflow
			 * item at all, we have to discard it from the backing
			 * file, otherwise we'll leak blocks on the checkpoint.
			 * That's safe because if the backing overflow value is
			 * still needed by any running transaction, we'll cache
			 * a copy in the reconciliation tracking structures.
			 *
			 * Regardless, we avoid copying in overflow records: if
			 * there's a WT_INSERT entry that modifies a reference
			 * counted overflow record, we may have to write copies
			 * of the overflow record, and in that case we'll do the
			 * comparisons, but we don't read overflow items just to
			 * see if they match records on either side.
			 */
			if (vpack->ovfl) {
				ovfl_state = OVFL_UNUSED;
				goto record_loop;
			}

			/*
			 * If data is Huffman encoded, we have to decode it in
			 * order to compare it with the last item we saw, which
			 * may have been an update string.  This guarantees we
			 * find every single pair of objects we can RLE encode,
			 * including applications updating an existing record
			 * where the new value happens (?) to match a Huffman-
			 * encoded value in a previous or next record.
			 */
			WT_ERR(__wt_dsk_cell_data_ref(
			    session, WT_PAGE_COL_VAR, vpack, orig));
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
				WT_ERR(__rec_txn_read(
				    session, r, ins, NULL, vpack, &upd));
				ins = WT_SKIP_NEXT(ins);
			}
			if (upd != NULL) {
				update_no_copy = 1;	/* No data copy */
				repeat_count = 1;	/* Single record */

				deleted = WT_UPDATE_DELETED_ISSET(upd);
				if (!deleted) {
					data = WT_UPDATE_DATA(upd);
					size = upd->size;
				}
			} else if (vpack->raw == WT_CELL_VALUE_OVFL_RM) {
				update_no_copy = 1;	/* No data copy */
				repeat_count = 1;	/* Single record */

				deleted = 0;

				/*
				 * If doing update save and restore, there's an
				 * update that's not globally visible, and the
				 * underlying value is a removed overflow value,
				 * we end up here.
				 *
				 * When the update save/restore code noticed the
				 * removed overflow value, it appended a copy of
				 * the cached, original overflow value to the
				 * update list being saved (ensuring the on-page
				 * item will never be accessed after the page is
				 * re-instantiated), then returned a NULL update
				 * to us.
				 *
				 * Assert the case: if we remove an underlying
				 * overflow object, checkpoint reconciliation
				 * should never see it again, there should be a
				 * visible update in the way.
				 *
				 * Write a placeholder.
				 */
				 WT_ASSERT(session,
				     F_ISSET(r, WT_SKIP_UPDATE_RESTORE));

				data = "@";
				size = 1;
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
					 * An as-yet-unused overflow item.
					 *
					 * We're going to copy the on-page cell,
					 * write out any record we're tracking.
					 */
					if (rle != 0) {
						WT_ERR(__rec_col_var_helper(
						    session, r, salvage, last,
						    last_deleted, 0, rle));
						rle = 0;
					}

					last->data = vpack->data;
					last->size = vpack->size;
					WT_ERR(__rec_col_var_helper(
					    session, r, salvage, last, 0,
					    WT_CELL_VALUE_OVFL, repeat_count));

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
					WT_ERR(__wt_dsk_cell_data_ref(session,
					    WT_PAGE_COL_VAR, vpack, orig));

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
					size = (uint32_t)orig->size;
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
				if (data == vpack->data || update_no_copy) {
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
		 *
		 * One complication: we must cache a copy before discarding the
		 * on-disk version if there's a transaction in the system that
		 * might read the original value.
		 */
		if (ovfl_state == OVFL_UNUSED &&
		    vpack->raw != WT_CELL_VALUE_OVFL_RM)
			WT_ERR(__wt_ovfl_cache(session, page, upd, vpack));
	}

	/* Walk any append list. */
	WT_SKIP_FOREACH(ins, WT_COL_APPEND(page)) {
		WT_ERR(__rec_txn_read(session, r, ins, NULL, NULL, &upd));
		if (upd == NULL)
			continue;
		for (n = WT_INSERT_RECNO(ins); src_recno <= n; ++src_recno) {
			/*
			 * The application may have inserted records which left
			 * gaps in the name space, and these gaps can be huge.
			 * If we're in a set of deleted records, skip the boring
			 * part.
			 */
			if (src_recno < n) {
				deleted = 1;
				if (last_deleted) {
					/*
					 * The record adjustment is decremented
					 * by one so we can naturally fall into
					 * the RLE accounting below, where we
					 * increment rle by one, then continue
					 * in the outer loop, where we increment
					 * src_recno by one.
					 */
					skip = (n - src_recno) - 1;
					rle += skip;
					src_recno += skip;
				}
			} else {
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

err:	__wt_scr_free(session, &orig);
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
	WT_DECL_RET;
	WT_IKEY *ikey;
	WT_KV *key, *val;
	WT_PAGE *child;
	WT_REF *ref;
	size_t size;
	u_int vtype;
	int hazard, key_onpage_ovfl, ovfl_key, state;
	const void *p;

	btree = S2BT(session);
	child = NULL;
	hazard = 0;

	key = &r->k;
	kpack = &_kpack;
	WT_CLEAR(*kpack);	/* -Wuninitialized */
	val = &r->v;
	vpack = &_vpack;
	WT_CLEAR(*vpack);	/* -Wuninitialized */

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
	WT_INTL_FOREACH_BEGIN(session, page, ref) {
		/*
		 * There are different paths if the key is an overflow item vs.
		 * a straight-forward on-page value. If an overflow item, we
		 * would have instantiated it, and we can use that fact to set
		 * things up.
		 *
		 * Note the cell reference and unpacked key cell are available
		 * only in the case of an instantiated, off-page key.
		 */
		ikey = __wt_ref_key_instantiated(ref);
		if (ikey == NULL || ikey->cell_offset == 0) {
			cell = NULL;
			key_onpage_ovfl = 0;
		} else {
			cell = WT_PAGE_REF_OFFSET(page, ikey->cell_offset);
			__wt_cell_unpack(cell, kpack);
			key_onpage_ovfl =
			    kpack->ovfl && kpack->raw != WT_CELL_KEY_OVFL_RM;
		}

		WT_ERR(__rec_child_modify(session, r, ref, &hazard, &state));
		addr = ref->addr;
		child = ref->page;

		/* Deleted child we don't have to write. */
		if (state == WT_CHILD_IGNORE) {
			/*
			 * Overflow keys referencing discarded pages are no
			 * longer useful, schedule them for discard.  Don't
			 * worry about instantiation, internal page keys are
			 * always instantiated.  Don't worry about reuse,
			 * reusing this key in this reconciliation is unlikely.
			 */
			if (key_onpage_ovfl)
				WT_ERR(__wt_ovfl_discard_add(
				    session, page, kpack->cell));
			CHILD_RELEASE_ERR(session, hazard, ref);
			continue;
		}

		/*
		 * Modified child.  Empty pages are merged into the parent and
		 * discarded.
		 */
		if (state == WT_CHILD_MODIFIED)
			switch (F_ISSET(child->modify, WT_PM_REC_MASK)) {
			case WT_PM_REC_EMPTY:
				/*
				 * Overflow keys referencing empty pages are no
				 * longer useful, schedule them for discard.
				 * Don't worry about instantiation, internal
				 * page keys are always instantiated.  Don't
				 * worry about reuse, reusing this key in this
				 * reconciliation is unlikely.
				 */
				if (key_onpage_ovfl)
					WT_ERR(__wt_ovfl_discard_add(
					    session, page, kpack->cell));
				CHILD_RELEASE_ERR(session, hazard, ref);
				continue;
			case WT_PM_REC_MULTIBLOCK:
				/*
				 * Overflow keys referencing split pages are no
				 * longer useful (the split page's key is the
				 * interesting key); schedule them for discard.
				 * Don't worry about instantiation, internal
				 * page keys are always instantiated.  Don't
				 * worry about reuse, reusing this key in this
				 * reconciliation is unlikely.
				 */
				if (key_onpage_ovfl)
					WT_ERR(__wt_ovfl_discard_add(
					    session, page, kpack->cell));

				WT_ERR(__rec_row_merge(session, r, child));
				CHILD_RELEASE_ERR(session, hazard, ref);
				continue;
			case WT_PM_REC_REPLACE:
				/*
				 * If the page is replaced, the page's modify
				 * structure has the page's address.
				 */
				addr = &child->modify->mod_replace;
				break;
			WT_ILLEGAL_VALUE_ERR(session);
			}

		/*
		 * Build the value cell, the child page's address.  Addr points
		 * to an on-page cell or an off-page WT_ADDR structure. There's
		 * a special cell type in the case of page deletion requiring
		 * a proxy cell, otherwise use the information from the addr or
		 * original cell.
		 */
		if (__wt_off_page(page, addr)) {
			p = addr->addr;
			size = addr->size;
			vtype = state == WT_CHILD_PROXY ?
			    WT_CELL_ADDR_DEL : __rec_vtype(addr);
		} else {
			__wt_cell_unpack(ref->addr, vpack);
			p = vpack->data;
			size = vpack->size;
			vtype = state == WT_CHILD_PROXY ?
			    WT_CELL_ADDR_DEL : (u_int)vpack->raw;
		}
		__rec_cell_build_addr(r, p, size, vtype, 0);
		CHILD_RELEASE_ERR(session, hazard, ref);

		/*
		 * Build key cell.
		 * Truncate any 0th key, internal pages don't need 0th keys.
		 */
		if (key_onpage_ovfl) {
			key->buf.data = cell;
			key->buf.size = __wt_cell_total_len(kpack);
			key->cell_len = 0;
			key->len = key->buf.size;
			ovfl_key = 1;
		} else {
			__wt_ref_key(page, ref, &p, &size);
			WT_ERR(__rec_cell_build_int_key(
			    session, r, p, r->cell_zero ? 1 : size, &ovfl_key));
		}
		r->cell_zero = 0;

		/* Boundary: split or write the page. */
		if (key->len + val->len > r->space_avail) {
			if (r->raw_compression)
				WT_ERR(__rec_split_raw(
				    session, r, key->len + val->len));
			else {
				/*
				 * In one path above, we copied address blocks
				 * from the page rather than building the actual
				 * key.  In that case, we have to build the key
				 * now because we are about to promote it.
				 */
				if (key_onpage_ovfl) {
					WT_ERR(__wt_buf_set(session, r->cur,
					    WT_IKEY_DATA(ikey), ikey->size));
					key_onpage_ovfl = 0;
				}
				WT_ERR(__rec_split(
				    session, r, key->len + val->len));
			}
		}

		/* Copy the key and value onto the page. */
		__rec_copy_incr(session, r, key);
		__rec_copy_incr(session, r, val);

		/* Update compression state. */
		__rec_key_state_update(r, ovfl_key);
	} WT_INTL_FOREACH_END;

	/* Write the remnant page. */
	return (__rec_split_finish(session, r));

err:	CHILD_RELEASE(session, hazard, ref);
	return (ret);
}

/*
 * __rec_row_merge --
 *	Merge in a split page.
 */
static int
__rec_row_merge(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_ADDR *addr;
	WT_KV *key, *val;
	WT_MULTI *multi;
	WT_PAGE_MODIFY *mod;
	uint32_t i;
	int ovfl_key;

	mod = page->modify;

	key = &r->k;
	val = &r->v;

	/* For each entry in the split array... */
	for (multi = mod->mod_multi,
	    i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
		/* Build the key and value cells. */
		WT_RET(__rec_cell_build_int_key(session, r,
		    WT_IKEY_DATA(multi->key.ikey),
		    r->cell_zero ? 1 : multi->key.ikey->size, &ovfl_key));
		r->cell_zero = 0;

		addr = &multi->addr;
		__rec_cell_build_addr(
		    r, addr->addr, addr->size, __rec_vtype(addr), 0);

		/* Boundary: split or write the page. */
		if (key->len + val->len > r->space_avail)
			WT_RET(r->raw_compression ?
			    __rec_split_raw(session, r, key->len + val->len) :
			    __rec_split(session, r, key->len + val->len));

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
	WT_CELL_UNPACK *kpack, _kpack, *vpack, _vpack;
	WT_DECL_ITEM(tmpkey);
	WT_DECL_ITEM(tmpval);
	WT_DECL_RET;
	WT_IKEY *ikey;
	WT_INSERT *ins;
	WT_KV *key, *val;
	WT_ROW *rip;
	WT_UPDATE *upd;
	size_t size;
	uint64_t slvg_skip;
	uint32_t i;
	int dictionary, key_onpage_ovfl, ovfl_key;
	const void *p;
	void *copy;

	btree = S2BT(session);
	slvg_skip = salvage == NULL ? 0 : salvage->skip;

	key = &r->k;
	val = &r->v;

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
		 * Figure out the key: set any cell reference (and unpack it),
		 * set any instantiated key reference.
		 */
		copy = WT_ROW_KEY_COPY(rip);
		(void)__wt_row_leaf_key_info(
		    page, copy, &ikey, &cell, NULL, NULL);
		if (cell == NULL)
			kpack = NULL;
		else {
			kpack = &_kpack;
			__wt_cell_unpack(cell, kpack);
		}

		/* Unpack the on-page value cell, and look for an update. */
		if ((val_cell =
		    __wt_row_leaf_value_cell(page, rip, NULL)) == NULL)
			vpack = NULL;
		else {
			vpack = &_vpack;
			__wt_cell_unpack(val_cell, vpack);
		}
		WT_ERR(__rec_txn_read(session, r, NULL, rip, vpack, &upd));

		/* Build value cell. */
		dictionary = 0;
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
			if (vpack == NULL) {
				val->buf.data = NULL;
				val->cell_len = val->len = val->buf.size = 0;
			} else if (vpack->raw == WT_CELL_VALUE_COPY) {
				/* If the item is Huffman encoded, decode it. */
				if (btree->huffman_value == NULL) {
					p = vpack->data;
					size = vpack->size;
				} else {
					WT_ERR(__wt_huffman_decode(session,
					    btree->huffman_value,
					    vpack->data, vpack->size,
					    tmpval));
					p = tmpval->data;
					size = tmpval->size;
				}
				WT_ERR(__rec_cell_build_val(
				    session, r, p, size, (uint64_t)0));
				dictionary = 1;
			} else if (vpack->raw == WT_CELL_VALUE_OVFL_RM) {
				/*
				 * If doing update save and restore in service
				 * of eviction, there's an update that's not
				 * globally visible, and the underlying value
				 * is a removed overflow value, we end up here.
				 *
				 * When the update save/restore code noticed the
				 * removed overflow value, it appended a copy of
				 * the cached, original overflow value to the
				 * update list being saved (ensuring any on-page
				 * item will never be accessed after the page is
				 * re-instantiated), then returned a NULL update
				 * to us.
				 *
				 * Assert the case.
				 */
				WT_ASSERT(session,
				    F_ISSET(r, WT_SKIP_UPDATE_RESTORE));

				/*
				 * If the key is also a removed overflow item,
				 * don't write anything at all.
				 *
				 * We don't have to write anything because the
				 * code re-instantiating the page gets the key
				 * to match the saved list of updates from the
				 * original page.  By not putting the key on
				 * the page, we'll move the key/value set from
				 * a row-store leaf page slot to an insert list,
				 * but that shouldn't matter.
				 *
				 * The reason we bother with the test is because
				 * overflows are expensive to write.  It's hard
				 * to imagine a real workload where this test is
				 * worth the effort, but it's a simple test.
				 */
				if (kpack != NULL &&
				    kpack->raw == WT_CELL_KEY_OVFL_RM)
					goto leaf_insert;

				/*
				 * The on-page value will never be accessed,
				 * write a placeholder record.
				 */
				WT_ERR(__rec_cell_build_val(
				    session, r, "@", 1, (uint64_t)0));
			} else {
				val->buf.data = val_cell;
				val->buf.size = __wt_cell_total_len(vpack);
				val->cell_len = 0;
				val->len = val->buf.size;

				/* Track if page has overflow items. */
				if (vpack->ovfl)
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
			if (vpack != NULL &&
			    vpack->ovfl && vpack->raw != WT_CELL_VALUE_OVFL_RM)
				WT_ERR(
				    __wt_ovfl_cache(session, page, rip, vpack));

			/* If this key/value pair was deleted, we're done. */
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				/*
				 * Overflow keys referencing discarded values
				 * are no longer useful, discard the backing
				 * blocks.  Don't worry about reuse, reusing
				 * keys from a row-store page reconciliation
				 * seems unlikely enough to ignore.
				 */
				if (kpack != NULL && kpack->ovfl &&
				    kpack->raw != WT_CELL_KEY_OVFL_RM) {
					/*
					 * Keys are part of the name-space, we
					 * can't remove them from the in-memory
					 * tree; if an overflow key was deleted
					 * without being instantiated (for
					 * example, cursor-based truncation, do
					 * it now.
					 */
					if (ikey == NULL)
						WT_ERR(__wt_row_leaf_key(
						    session,
						    page, rip, tmpkey, 1));

					WT_ERR(__wt_ovfl_discard_add(
					    session, page, kpack->cell));
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
			if (upd->size == 0) {
				val->buf.data = NULL;
				val->cell_len = val->len = val->buf.size = 0;
			} else {
				WT_ERR(__rec_cell_build_val(session, r,
				    WT_UPDATE_DATA(upd), upd->size,
				    (uint64_t)0));
				dictionary = 1;
			}
		}

		/*
		 * Build key cell.
		 *
		 * If the key is an overflow key that hasn't been removed, use
		 * the original backing blocks.
		 */
		key_onpage_ovfl = kpack != NULL &&
		    kpack->ovfl && kpack->raw != WT_CELL_KEY_OVFL_RM;
		if (key_onpage_ovfl) {
			key->buf.data = cell;
			key->buf.size = __wt_cell_total_len(kpack);
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
			 * Get the key from the page or an instantiated key, or
			 * inline building the key from a previous key (it's a
			 * fast path for simple, prefix-compressed keys), or by
			 * by building the key from scratch.
			 */
			if (__wt_row_leaf_key_info(page, copy,
			    NULL, &cell, &tmpkey->data, &tmpkey->size))
				goto build;

			kpack = &_kpack;
			__wt_cell_unpack(cell, kpack);
			if (btree->huffman_key == NULL &&
			    kpack->type == WT_CELL_KEY &&
			    tmpkey->size >= kpack->prefix) {
				/*
				 * The previous clause checked for a prefix of
				 * zero, which means the temporary buffer must
				 * have a non-zero size, and it references a
				 * valid key.
				 */
				WT_ASSERT(session, tmpkey->size != 0);

				/*
				 * Grow the buffer as necessary, ensuring data
				 * data has been copied into local buffer space,
				 * then append the suffix to the prefix already
				 * in the buffer.
				 *
				 * Don't grow the buffer unnecessarily or copy
				 * data we don't need, truncate the item's data
				 * length to the prefix bytes.
				 */
				tmpkey->size = kpack->prefix;
				WT_ERR(__wt_buf_grow(session,
				    tmpkey, tmpkey->size + kpack->size));
				memcpy((uint8_t *)tmpkey->mem + tmpkey->size,
				    kpack->data, kpack->size);
				tmpkey->size += kpack->size;
			} else
				WT_ERR(__wt_row_leaf_key_copy(
				    session, page, rip, tmpkey));
build:
			WT_ERR(__rec_cell_build_leaf_key(session, r,
			    tmpkey->data, tmpkey->size, &ovfl_key));
		}

		/* Boundary: split or write the page. */
		if (key->len + val->len > r->space_avail) {
			if (r->raw_compression)
				WT_ERR(__rec_split_raw(
				    session, r, key->len + val->len));
			else {
				/*
				 * In one path above, we copied address blocks
				 * from the page rather than building the actual
				 * key.  In that case, we have to build the key
				 * now because we are about to promote it.
				 */
				if (key_onpage_ovfl) {
					WT_ERR(__wt_dsk_cell_data_ref(session,
					    WT_PAGE_ROW_LEAF, kpack, r->cur));
					key_onpage_ovfl = 0;
				}

				/*
				 * Turn off prefix compression until a full key
				 * written to the new page, and (unless already
				 * working with an overflow key), rebuild the
				 * key without compression.
				 */
				if (r->key_pfx_compress_conf) {
					r->key_pfx_compress = 0;
					if (!ovfl_key)
						WT_ERR(
						    __rec_cell_build_leaf_key(
						    session,
						    r, NULL, 0, &ovfl_key));
				}

				WT_ERR(__rec_split(
				    session, r, key->len + val->len));
			}
		}

		/* Copy the key/value pair onto the page. */
		__rec_copy_incr(session, r, key);
		if (val->len == 0)
			r->any_empty_value = 1;
		else {
			r->all_empty_value = 0;
			if (dictionary && btree->dictionary)
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

err:	__wt_scr_free(session, &tmpkey);
	__wt_scr_free(session, &tmpval);
	return (ret);
}

/*
 * __rec_row_leaf_insert --
 *	Walk an insert chain, writing K/V pairs.
 */
static int
__rec_row_leaf_insert(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins)
{
	WT_BTREE *btree;
	WT_KV *key, *val;
	WT_UPDATE *upd;
	int ovfl_key;

	btree = S2BT(session);

	key = &r->k;
	val = &r->v;

	for (; ins != NULL; ins = WT_SKIP_NEXT(ins)) {
		/* Look for an update. */
		WT_RET(__rec_txn_read(session, r, ins, NULL, NULL, &upd));
		if (upd == NULL || WT_UPDATE_DELETED_ISSET(upd))
			continue;

		if (upd->size == 0)			/* Build value cell. */
			val->len = 0;
		else
			WT_RET(__rec_cell_build_val(session, r,
			    WT_UPDATE_DATA(upd), upd->size, (uint64_t)0));

							/* Build key cell. */
		WT_RET(__rec_cell_build_leaf_key(session, r,
		    WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins), &ovfl_key));

		/* Boundary: split or write the page. */
		if (key->len + val->len > r->space_avail) {
			if (r->raw_compression)
				WT_RET(__rec_split_raw(
				    session, r, key->len + val->len));
			else {
				/*
				 * Turn off prefix compression until a full key
				 * written to the new page, and (unless already
				 * working with an overflow key), rebuild the
				 * key without compression.
				 */
				if (r->key_pfx_compress_conf) {
					r->key_pfx_compress = 0;
					if (!ovfl_key)
						WT_RET(
						    __rec_cell_build_leaf_key(
						    session,
						    r, NULL, 0, &ovfl_key));
				}

				WT_RET(__rec_split(
				    session, r, key->len + val->len));
			}
		}

		/* Copy the key/value pair onto the page. */
		__rec_copy_incr(session, r, key);
		if (val->len == 0)
			r->any_empty_value = 1;
		else {
			r->all_empty_value = 0;
			if (btree->dictionary)
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
	WT_BM *bm;
	WT_DECL_RET;
	WT_PAGE_MODIFY *mod;
	WT_MULTI *multi;
	uint32_t i;

	bm = S2BT(session)->bm;
	mod = page->modify;

	/*
	 * A page that split is being reconciled for the second, or subsequent
	 * time; discard underlying block space used in the last reconciliation
	 * that is not being reused for this reconciliation.
	 */
	for (multi = mod->mod_multi,
	    i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
		switch (page->type) {
		case WT_PAGE_ROW_INT:
		case WT_PAGE_ROW_LEAF:
			__wt_free(session, multi->key.ikey);
			break;
		}
		if (multi->skip == NULL) {
			if (multi->addr.reuse)
				multi->addr.addr = NULL;
			else {
				WT_RET(bm->free(bm, session,
				    multi->addr.addr, multi->addr.size));
				__wt_free(session, multi->addr.addr);
			}
		} else {
			__wt_free(session, multi->skip);
			__wt_free(session, multi->skip_dsk);
		}
	}
	__wt_free(session, mod->mod_multi);
	mod->mod_multi_entries = 0;

	/*
	 * This routine would be trivial, and only walk a single page freeing
	 * any blocks written to support the split, except for root splits.
	 * In the case of root splits, we have to cope with multiple pages in
	 * a linked list, and we also have to discard overflow items written
	 * for the page.
	 */
	switch (page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		if (mod->mod_root_split == NULL)
			break;
		WT_RET(__rec_split_discard(session, mod->mod_root_split));
		WT_RET(__wt_ovfl_track_wrapup(session, mod->mod_root_split));
		__wt_page_out(session, &mod->mod_root_split);
		break;
	}

	return (ret);
}

/*
 * __rec_write_wrapup --
 *	Finish the reconciliation.
 */
static int
__rec_write_wrapup(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_BM *bm;
	WT_BOUNDARY *bnd;
	WT_BTREE *btree;
	WT_MULTI *multi;
	WT_PAGE_MODIFY *mod;
	WT_REF *ref;
	size_t addr_size;
	const uint8_t *addr;

	btree = S2BT(session);
	bm = btree->bm;
	mod = page->modify;
	ref = r->ref;

	/*
	 * This page may have previously been reconciled, and that information
	 * is now about to be replaced.  Make sure it's discarded at some point,
	 * and clear the underlying modification information, we're creating a
	 * new reality.
	 */
	switch (F_ISSET(mod, WT_PM_REC_MASK)) {
	case 0:	/*
		 * The page has never been reconciled before, free the original
		 * address blocks (if any).  The "if any" is for empty trees
		 * created when a new tree is opened or previously deleted pages
		 * instantiated in memory.
		 *
		 * The exception is root pages are never tracked or free'd, they
		 * are checkpoints, and must be explicitly dropped.
		 */
		if (__wt_ref_is_root(ref))
			break;
		if (ref->addr != NULL) {
			/*
			 * Free the page and clear the address (so we don't free
			 * it twice).
			 */
			WT_RET(__wt_ref_info(
			    session, ref, &addr, &addr_size, NULL));
			WT_RET(bm->free(bm, session, addr, addr_size));
			if (__wt_off_page(ref->home, ref->addr)) {
				__wt_free(
				    session, ((WT_ADDR *)ref->addr)->addr);
				__wt_free(session, ref->addr);
			}
			ref->addr = NULL;
		}
		break;
	case WT_PM_REC_EMPTY:				/* Page deleted */
		break;
	case WT_PM_REC_MULTIBLOCK:			/* Multiple blocks */
	case WT_PM_REC_REWRITE:				/* Rewrite */
		/*
		 * Discard the multiple replacement blocks.
		 */
		WT_RET(__rec_split_discard(session, page));
		break;
	case WT_PM_REC_REPLACE:				/* 1-for-1 page swap */
		/*
		 * Discard the replacement leaf page's blocks.
		 *
		 * The exception is root pages are never tracked or free'd, they
		 * are checkpoints, and must be explicitly dropped.
		 */
		if (!__wt_ref_is_root(ref))
			WT_RET(bm->free(bm, session,
			    mod->mod_replace.addr, mod->mod_replace.size));

		/* Discard the replacement page's address. */
		__wt_free(session, mod->mod_replace.addr);
		mod->mod_replace.size = 0;
		break;
	WT_ILLEGAL_VALUE(session);
	}
	F_CLR(mod, WT_PM_REC_MASK);

	/*
	 * Wrap up overflow tracking.  If we are about to create a checkpoint,
	 * the system must be entirely consistent at that point (the underlying
	 * block manager is presumably going to do some action to resolve the
	 * list of allocated/free/whatever blocks that are associated with the
	 * checkpoint).
	 */
	WT_RET(__wt_ovfl_track_wrapup(session, page));

	switch (r->bnd_next) {
	case 0:						/* Page delete */
		WT_RET(__wt_verbose(
		    session, WT_VERB_RECONCILE, "page %p empty", page));
		WT_STAT_FAST_DATA_INCR(session, rec_page_delete);

		/* If this is the root page, we need to create a sync point. */
		ref = r->ref;
		if (__wt_ref_is_root(ref))
			WT_RET(
			    bm->checkpoint(bm, session, NULL, btree->ckpt, 0));

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
		 */
		bnd = &r->bnd[0];

		/*
		 * If we're saving/restoring changes for this page, there's
		 * nothing to write. Allocate, then initialize the array of
		 * replacement blocks.
		 */
		if (bnd->skip != NULL) {
			WT_RET(__wt_calloc_def(
			    session, r->bnd_next, &mod->mod_multi));
			multi = mod->mod_multi;
			multi->skip = bnd->skip;
			multi->skip_entries = bnd->skip_next;
			bnd->skip = NULL;
			multi->skip_dsk = bnd->dsk;
			bnd->dsk = NULL;
			mod->mod_multi_entries = 1;

			F_SET(mod, WT_PM_REC_REWRITE);
			break;
		}

		/*
		 * If this is a root page, then we don't have an address and we
		 * have to create a sync point.  The address was cleared when
		 * we were about to write the buffer so we know what to do here.
		 */
		if (bnd->addr.addr == NULL)
			WT_RET(__wt_bt_write(session,
			    &r->dsk, NULL, NULL, 1, bnd->already_compressed));
		else {
			mod->mod_replace = bnd->addr;
			bnd->addr.addr = NULL;
		}

		F_SET(mod, WT_PM_REC_REPLACE);
		break;
	default:					/* Page split */
		WT_RET(__wt_verbose(session, WT_VERB_RECONCILE,
		    "page %p reconciled into %" PRIu32 " pages",
		    page, r->bnd_next));

		switch (page->type) {
		case WT_PAGE_COL_INT:
		case WT_PAGE_ROW_INT:
			WT_STAT_FAST_DATA_INCR(
			    session, rec_multiblock_internal);
			break;
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
		case WT_PAGE_ROW_LEAF:
			WT_STAT_FAST_DATA_INCR(session, rec_multiblock_leaf);
			break;
		WT_ILLEGAL_VALUE(session);
		}

		/* Display the actual split keys. */
		if (WT_VERBOSE_ISSET(session, WT_VERB_SPLIT)) {
			WT_DECL_ITEM(tkey);
			WT_DECL_RET;
			uint32_t i;

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
					WT_ERR(__wt_verbose(
					    session, WT_VERB_SPLIT,
					    "split: starting key "
					    "%.*s",
					    (int)tkey->size,
					    (const char *)tkey->data));
					break;
				case WT_PAGE_COL_FIX:
				case WT_PAGE_COL_INT:
				case WT_PAGE_COL_VAR:
					WT_ERR(__wt_verbose(
					    session, WT_VERB_SPLIT,
					    "split: starting recno %" PRIu64,
					    bnd->recno));
					break;
				WT_ILLEGAL_VALUE_ERR(session);
				}
err:			__wt_scr_free(session, &tkey);
			WT_RET(ret);
		}
		if (r->bnd_next > r->bnd_next_max) {
			r->bnd_next_max = r->bnd_next;
			WT_STAT_FAST_DATA_SET(
			    session, rec_multiblock_max, r->bnd_next_max);
		}

		switch (page->type) {
		case WT_PAGE_ROW_INT:
		case WT_PAGE_ROW_LEAF:
			WT_RET(__rec_split_row(session, r, page));
			break;
		case WT_PAGE_COL_INT:
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			WT_RET(__rec_split_col(session, r, page));
			break;
		WT_ILLEGAL_VALUE(session);
		}
		F_SET(mod, WT_PM_REC_MULTIBLOCK);
		break;
	}

	/*
	 * If updates were skipped, the tree isn't clean.  The checkpoint call
	 * cleared the tree's modified value before calling the eviction thread,
	 * so we must explicitly reset the tree's modified flag.  We insert a
	 * barrier after the change for clarity (the requirement is the value
	 * be set before a subsequent checkpoint reads it, and because the
	 * current checkpoint is waiting on this reconciliation to complete,
	 * there's no risk of that happening).
	 */
	if (r->leave_dirty) {
		mod->first_dirty_txn = r->first_dirty_txn;

		btree->modified = 1;
		WT_FULL_BARRIER();
	} else {
		/*
		 * If no updates were skipped, we have a new maximum transaction
		 * written for the page (used to decide if a clean page can be
		 * evicted). Set the highest transaction ID for the page.
		 *
		 * Track the highest transaction ID for the tree (used to decide
		 * if it's safe to discard all of the pages in the tree without
		 * further checking). Reconciliation in the service of eviction
		 * is multi-threaded, only update the tree's maximum transaction
		 * ID when doing a checkpoint. That's sufficient, we only care
		 * about the highest transaction ID of any update currently in
		 * the tree, and checkpoint visits every dirty page in the tree.
		 */
		mod->rec_max_txn = r->max_txn;
		if (!F_ISSET(r, WT_EVICTING) &&
		    WT_TXNID_LT(btree->rec_max_txn, r->max_txn))
			btree->rec_max_txn = r->max_txn;

		/*
		 * The page only might be clean; if the write generation is
		 * unchanged since reconciliation started, it's clean. If the
		 * write generation changed, the page has been written since
		 * we started reconciliation and remains dirty.
		 */
		if (__wt_atomic_cas32(&mod->write_gen, r->orig_write_gen, 0))
			__wt_cache_dirty_decr(session, page);
	}

	return (0);
}

/*
 * __rec_write_wrapup_err --
 *	Finish the reconciliation on error.
 */
static int
__rec_write_wrapup_err(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_BM *bm;
	WT_BOUNDARY *bnd;
	WT_DECL_RET;
	WT_MULTI *multi;
	WT_PAGE_MODIFY *mod;
	uint32_t i;

	bm = S2BT(session)->bm;
	mod = page->modify;

	/*
	 * Clear the address-reused flag from the multiblock reconciliation
	 * information (otherwise we might think the backing block is being
	 * reused on a subsequent reconciliation where we want to free it).
	 */
	switch (F_ISSET(mod, WT_PM_REC_MASK)) {
	case WT_PM_REC_MULTIBLOCK:
	case WT_PM_REC_REWRITE:
		for (multi = mod->mod_multi,
		    i = 0; i < mod->mod_multi_entries; ++multi, ++i)
			multi->addr.reuse = 0;
		break;
	}

	/*
	 * On error, discard blocks we've written, they're unreferenced by the
	 * tree.  This is not a question of correctness, we're avoiding block
	 * leaks.
	 *
	 * Don't discard backing blocks marked for reuse, they remain part of
	 * a previous reconciliation.
	 */
	WT_TRET(__wt_ovfl_track_wrapup_err(session, page));
	for (bnd = r->bnd, i = 0; i < r->bnd_next; ++bnd, ++i)
		if (bnd->addr.addr != NULL) {
			if (bnd->addr.reuse)
				bnd->addr.addr = NULL;
			else {
				WT_TRET(bm->free(bm, session,
				    bnd->addr.addr, bnd->addr.size));
				__wt_free(session, bnd->addr.addr);
			}
		}

	return (ret);
}

/*
 * __rec_split_row --
 *	Split a row-store page into a set of replacement blocks.
 */
static int
__rec_split_row(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_BOUNDARY *bnd;
	WT_MULTI *multi;
	WT_PAGE_MODIFY *mod;
	WT_REF *ref;
	uint32_t i;
	size_t size;
	void *p;

	mod = page->modify;

	/* We never set the first page's key, grab it from the original page. */
	ref = r->ref;
	if (__wt_ref_is_root(ref))
		WT_RET(__wt_buf_set(session, &r->bnd[0].key, "", 1));
	else {
		__wt_ref_key(ref->home, ref, &p, &size);
		WT_RET(__wt_buf_set(session, &r->bnd[0].key, p, size));
	}

	/* Allocate, then initialize the array of replacement blocks. */
	WT_RET(__wt_calloc_def(session, r->bnd_next, &mod->mod_multi));

	for (multi = mod->mod_multi,
	    bnd = r->bnd, i = 0; i < r->bnd_next; ++multi, ++bnd, ++i) {
		WT_RET(__wt_row_ikey_alloc(session, 0,
		    bnd->key.data, bnd->key.size, &multi->key.ikey));

		if (bnd->skip == NULL) {
			multi->addr = bnd->addr;
			multi->addr.reuse = 0;
			multi->size = bnd->size;
			multi->cksum = bnd->cksum;
			bnd->addr.addr = NULL;
		} else {
			multi->skip = bnd->skip;
			multi->skip_entries = bnd->skip_next;
			bnd->skip = NULL;
			multi->skip_dsk = bnd->dsk;
			bnd->dsk = NULL;
		}
	}
	mod->mod_multi_entries = r->bnd_next;

	return (0);
}

/*
 * __rec_split_col --
 *	Split a column-store page into a set of replacement blocks.
 */
static int
__rec_split_col(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
	WT_BOUNDARY *bnd;
	WT_MULTI *multi;
	WT_PAGE_MODIFY *mod;
	uint32_t i;

	mod = page->modify;

	/* Allocate, then initialize the array of replacement blocks. */
	WT_RET(__wt_calloc_def(session, r->bnd_next, &mod->mod_multi));

	for (multi = mod->mod_multi,
	    bnd = r->bnd, i = 0; i < r->bnd_next; ++multi, ++bnd, ++i) {
		multi->key.recno = bnd->recno;

		if (bnd->skip == NULL) {
			multi->addr = bnd->addr;
			multi->addr.reuse = 0;
			multi->size = bnd->size;
			multi->cksum = bnd->cksum;
			bnd->addr.addr = NULL;
		} else {
			multi->skip = bnd->skip;
			multi->skip_entries = bnd->skip_next;
			bnd->skip = NULL;
			multi->skip_dsk = bnd->dsk;
			bnd->dsk = NULL;
		}
	}
	mod->mod_multi_entries = r->bnd_next;

	return (0);
}

/*
 * __rec_cell_build_int_key --
 *	Process a key and return a WT_CELL structure and byte string to be
 * stored on a row-store internal page.
 */
static int
__rec_cell_build_int_key(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, const void *data, size_t size, int *is_ovflp)
{
	WT_BTREE *btree;
	WT_KV *key;

	*is_ovflp = 0;

	btree = S2BT(session);

	key = &r->k;

	/* Copy the bytes into the "current" and key buffers. */
	WT_RET(__wt_buf_set(session, r->cur, data, size));
	WT_RET(__wt_buf_set(session, &key->buf, data, size));

	/* Create an overflow object if the data won't fit. */
	if (size > btree->maxintlkey) {
		WT_STAT_FAST_DATA_INCR(session, rec_overflow_key_internal);

		*is_ovflp = 1;
		return (__rec_cell_build_ovfl(
		    session, r, key, WT_CELL_KEY_OVFL, (uint64_t)0));
	}

	key->cell_len = __wt_cell_pack_int_key(&key->cell, key->buf.size);
	key->len = key->cell_len + key->buf.size;

	return (0);
}

/*
 * __rec_cell_build_leaf_key --
 *	Process a key and return a WT_CELL structure and byte string to be
 * stored on a row-store leaf page.
 */
static int
__rec_cell_build_leaf_key(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, const void *data, size_t size, int *is_ovflp)
{
	WT_BTREE *btree;
	WT_KV *key;
	size_t pfx_max;
	uint8_t pfx;
	const uint8_t *a, *b;

	*is_ovflp = 0;

	btree = S2BT(session);

	key = &r->k;

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
		 * shorter of the two keys.
		 */
		if (r->key_pfx_compress) {
			/*
			 * We can't compress out more than 256 bytes, limit the
			 * comparison to that.
			 */
			pfx_max = UINT8_MAX;
			if (size < pfx_max)
				pfx_max = size;
			if (r->last->size < pfx_max)
				pfx_max = r->last->size;
			for (a = data, b = r->last->data; pfx < pfx_max; ++pfx)
				if (*a++ != *b++)
					break;

			/*
			 * Prefix compression may cost us CPU and memory when
			 * the page is re-loaded, don't do it unless there's
			 * reasonable gain.
			 */
			if (pfx < btree->prefix_compression_min)
				pfx = 0;
			else
				WT_STAT_FAST_DATA_INCRV(
				    session, rec_prefix_compression, pfx);
		}

		/* Copy the non-prefix bytes into the key buffer. */
		WT_RET(__wt_buf_set(
		    session, &key->buf, (uint8_t *)data + pfx, size - pfx));
	}

	/* Optionally compress the key using the Huffman engine. */
	if (btree->huffman_key != NULL)
		WT_RET(__wt_huffman_encode(session, btree->huffman_key,
		    key->buf.data, (uint32_t)key->buf.size, &key->buf));

	/* Create an overflow object if the data won't fit. */
	if (key->buf.size > btree->maxleafkey) {
		/*
		 * Overflow objects aren't prefix compressed -- rebuild any
		 * object that was prefix compressed.
		 */
		if (pfx == 0) {
			WT_STAT_FAST_DATA_INCR(session, rec_overflow_key_leaf);

			*is_ovflp = 1;
			return (__rec_cell_build_ovfl(
			    session, r, key, WT_CELL_KEY_OVFL, (uint64_t)0));
		}
		return (
		    __rec_cell_build_leaf_key(session, r, NULL, 0, is_ovflp));
	}

	key->cell_len = __wt_cell_pack_leaf_key(&key->cell, pfx, key->buf.size);
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
    const void *addr, size_t size, u_int cell_type, uint64_t recno)
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
	val->cell_len =
	    __wt_cell_pack_addr(&val->cell, cell_type, recno, val->buf.size);
	val->len = val->cell_len + val->buf.size;
}

/*
 * __rec_cell_build_val --
 *	Process a data item and return a WT_CELL structure and byte string to
 * be stored on the page.
 */
static int
__rec_cell_build_val(WT_SESSION_IMPL *session,
    WT_RECONCILE *r, const void *data, size_t size, uint64_t rle)
{
	WT_BTREE *btree;
	WT_KV *val;

	btree = S2BT(session);

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
			    val->buf.data, (uint32_t)val->buf.size, &val->buf));

		/* Create an overflow object if the data won't fit. */
		if (val->buf.size > btree->maxleafvalue) {
			WT_STAT_FAST_DATA_INCR(session, rec_overflow_value);

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
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_HEADER *dsk;
	size_t size;
	uint8_t *addr, buf[WT_BTREE_MAX_ADDR_COOKIE];

	btree = S2BT(session);
	bm = btree->bm;
	page = r->page;

	/* Track if page has overflow items. */
	r->ovfl_items = 1;

	/*
	 * See if this overflow record has already been written and reuse it if
	 * possible.  Else, write a new overflow record.
	 */
	if (!__wt_ovfl_reuse_search(session, page,
	    &addr, &size, kv->buf.data, kv->buf.size)) {
		/* Allocate a buffer big enough to write the overflow record. */
		size = kv->buf.size;
		WT_RET(bm->write_size(bm, session, &size));
		WT_RET(__wt_scr_alloc(session, size, &tmp));

		/* Initialize the buffer: disk header and overflow record. */
		dsk = tmp->mem;
		memset(dsk, 0, WT_PAGE_HEADER_SIZE);
		dsk->type = WT_PAGE_OVFL;
		dsk->u.datalen = (uint32_t)kv->buf.size;
		memcpy(WT_PAGE_HEADER_BYTE(btree, dsk),
		    kv->buf.data, kv->buf.size);
		dsk->mem_size = tmp->size =
		    WT_PAGE_HEADER_BYTE_SIZE(btree) + (uint32_t)kv->buf.size;

		/* Write the buffer. */
		addr = buf;
		WT_ERR(__wt_bt_write(session, tmp, addr, &size, 0, 0));

		/*
		 * Track the overflow record (unless it's a bulk load, which
		 * by definition won't ever reuse a record.
		 */
		if (!r->is_bulk_load)
			WT_ERR(__wt_ovfl_reuse_add(session, page,
			    addr, size, kv->buf.data, kv->buf.size));
	}

	/* Set the callers K/V to reference the overflow record's address. */
	WT_ERR(__wt_buf_set(session, &kv->buf, addr, size));

	/* Build the cell and return. */
	kv->cell_len = __wt_cell_pack_ovfl(&kv->cell, type, rle, kv->buf.size);
	kv->len = kv->cell_len + kv->buf.size;

err:	__wt_scr_free(session, &tmp);
	return (ret);
}

/*
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
	for (i = WT_SKIP_MAXDEPTH - 1, e = &head[i]; i >= 0;) {
		if (*e == NULL) {		/* Empty levels */
			--i;
			--e;
			continue;
		}

		/*
		 * Return any exact matches: we don't care in what search level
		 * we found a match.
		 */
		if ((*e)->hash == hash)		/* Exact match */
			return (*e);
		if ((*e)->hash > hash) {	/* Drop down a level */
			--i;
			--e;
		} else				/* Keep going at this level */
			e = &(*e)->next[i];
	}
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
		if (*e == NULL || (*e)->hash > hash)
			stack[i--] = e--;	/* Drop down a level */
		else
			e = &(*e)->next[i];	/* Keep going at this level */
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
__rec_dictionary_init(WT_SESSION_IMPL *session, WT_RECONCILE *r, u_int slots)
{
	u_int depth, i;

	/* Free any previous dictionary. */
	__rec_dictionary_free(session, r);

	r->dictionary_slots = slots;
	WT_RET(__wt_calloc(session,
	    r->dictionary_slots, sizeof(WT_DICTIONARY *), &r->dictionary));
	for (i = 0; i < r->dictionary_slots; ++i) {
		depth = __wt_skip_choose_depth(session);
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
	if (r->dictionary_slots) {
		r->dictionary_next = 0;
		memset(r->dictionary_head, 0, sizeof(r->dictionary_head));
	}
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
			WT_STAT_FAST_DATA_INCR(session, rec_dictionary);
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
