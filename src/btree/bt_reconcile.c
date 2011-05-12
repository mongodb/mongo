/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

struct __rec_boundary;		typedef struct __rec_boundary WT_BOUNDARY;
struct __rec_discard;		typedef struct __rec_discard WT_DISCARD;
struct __rec_split;		typedef struct __rec_split WT_SPLIT;

/*
 * WT_RECONCILE --
 *	Information tracking a single page reconciliation.
 */
typedef struct {
	/*
	 * Reconciliation is a fairly simple process: take an in-memory page,
	 * walk through each entry in the page, build a backing disk image in
	 * a temporary buffer that represents that information, and write that
	 * buffer to disk.
	 */
	WT_BUF dsk;			/* Temporary disk-image buffer */

	int evict;			/* The page is being discarded */
	int locked;			/* The tree is locked down */

	/*
	 * As pages are reconciled, split pages are merged into their parent
	 * and discarded; deleted pages and overflow K/V items are discarded
	 * along with their underlying blocks.  If a page or overflow item
	 * were discarded and page reconciliation then failed for any reason,
	 * the in-memory tree would be incorrect.  To keep the tree correct
	 * until we're sure page reconciliation will succeed, we keep a list
	 * of objects to discard when the reconciled page is discarded.
	 */
	struct __rec_discard {
		WT_PAGE *page;		/* Inactive page */

		uint32_t addr;		/* Page's addr/size, */
		uint32_t size;		/*    or block pair to delete */
	} *discard;			/* List of discard objects */
	uint32_t discard_next;		/* Next discard slot */
	uint32_t discard_entries;	/* Total discard slots */
	uint32_t discard_allocated;	/* Bytes allocated */

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
	 * realize we're going to have to split -- we don't want to start over.
	 *
	 * To keep from having to start over when we reach the maximum page
	 * size, track the page information when we approach a split boundary.
	 */
	struct __rec_boundary {
		uint64_t recno;		/* Split's starting record */
		uint32_t entries;	/* Split's entries */

		/*
		 * The first byte in the split chunk; the difference between
		 * the next slot's first byte and this slot's first byte is
		 * the length of the split chunk.
		 */
		uint8_t *start;		/* Split's first byte */
	} *bnd;				/* Saved boundaries */
	uint32_t bnd_next;		/* Next boundary slot */
	uint32_t bnd_entries;		/* Total boundary slots */
	uint32_t bnd_allocated;		/* Bytes allocated */

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
		state;

	/*
	 * We track current information about the current record number, the
	 * number of entries copied into the temporary buffer, where we are
	 * in the temporary buffer, and how much memory remains.  Those items
	 * are packaged here rather than passing pointer to stack locations
	 * through the code.
	 */
	uint64_t recno;			/* Current record number */
	uint32_t entries;		/* Current number of entries */
	uint8_t *first_free;		/* Current first free byte */
	uint32_t space_avail;		/* Remaining space in this chunk */

	/*
	 * We track the total number of page entries copied into split chunks
	 * so we can easily figure out how many entries in the current split
	 * chunk.
	 */
	uint32_t total_entries;		/* Total entries in splits */

	/*
	 * If reconciling an in-memory page we're not evicting from memory, we
	 * may split a page referencing an in-memory subtree.  In the same way
	 * we create a set of new disk images that reflect the split of the
	 * original page's disk image, we have to create a set of new in-memory
	 * images reflecting the split of the original page's in-memory image.
	 * Once that's done, the new in-memory image must references the same
	 * subtrees as the old in-memory image; we maintain a list of in-memory
	 * subtrees the original page referenced to make that fixup possible.
	 *
	 * We obviously could maintain this list on a per-split-chunk basis:
	 * copy it into the WT_BOUNDARY structure if the boundary is reached,
	 * then from the WT_BOUNDARY structure into the WT_SPLIT structure if
	 * we split.  We're binarily searching this array during fixup and I
	 * don't expect it to be that large -- I don't expect this to be my
	 * performance problem.
	 */
	WT_REF **imref;			/* In-memory subtree reference list */
	uint32_t imref_next;		/* Next list slot */
	uint32_t imref_entries;		/* Total list slots */
	uint32_t imref_allocated;	/* Bytes allocated */
	uint32_t imref_found;		/* Fast check for search */

	/*
	 * When a split happens, we create a new internal page referencing the
	 * split pages, and replace the original page with it.   In order to
	 * build that page, keep a list of all split pages written during the
	 * page's reconciliation.
	 */
	struct __rec_split {
		WT_OFF_RECORD off;	/* Address, size, recno */

		/*
		 * The key for a row-store page; no column-store key is needed
		 * because the page's recno, stored in the WT_OFF_RECORD, is
		 * the column-store key.
		 */
		WT_BUF key;		/* Row key */

		/*
		 * If we had to create an in-memory version of the page, we'll
		 * save it here for later connection between the newly created
		 * split page and previously existing in-memory pages.
		 *
		 * For the same reason, maintain the original page's subtree
		 * references for later fixup.
		 */
		WT_PAGE *imp;		/* Page's in-memory version */
	} *split;			/* Saved splits */
	uint32_t split_next;		/* Next list slot */
	uint32_t split_entries;		/* Total list slots */
	uint32_t split_allocated;	/* Bytes allocated */

	WT_ROW_REF *merge_ref;		/* Row-store merge correction key */

	WT_HAZARD *hazard;		/* Copy of the hazard references */
	uint32_t   hazard_elem;		/* Number of entries in the list */
} WT_RECONCILE;

static uint32_t __rec_allocation_size(SESSION *, WT_BUF *, uint8_t *);
static int  __rec_cell_build_key(SESSION *, WT_BUF *, WT_CELL *, WT_OVFL *);
static int  __rec_cell_build_ovfl(SESSION *, WT_BUF *, WT_OVFL *);
static int  __rec_cell_build_value(SESSION *, WT_BUF *, WT_CELL *, WT_OVFL *);
static int  __rec_col_fix(SESSION *, WT_PAGE *);
static int  __rec_col_fix_bulk(SESSION *, WT_PAGE *);
static int  __rec_col_int(SESSION *, WT_PAGE *);
static int  __rec_col_merge(SESSION *, WT_PAGE *);
static int  __rec_col_rle(SESSION *, WT_PAGE *);
static int  __rec_col_rle_bulk(SESSION *, WT_PAGE *);
static int  __rec_col_split(SESSION *, WT_PAGE **, WT_PAGE *);
static int  __rec_col_var(SESSION *, WT_PAGE *);
static int  __rec_col_var_bulk(SESSION *, WT_PAGE *);
static int  __rec_col_var_del(SESSION *, WT_PAGE *);
static int  __rec_discard_add(SESSION *, WT_PAGE *, uint32_t, uint32_t);
static int  __rec_discard_evict(SESSION *);
static void __rec_discard_init(WT_RECONCILE *);
static int  __rec_imref_add(SESSION *, WT_REF *);
static int  __rec_imref_bsearch_cmp(const void *, const void *);
static int  __rec_imref_fixup(SESSION *, WT_PAGE *);
static void __rec_imref_init(WT_RECONCILE *);
static int  __rec_imref_qsort_cmp(const void *, const void *);
static inline void __rec_incr(SESSION *, WT_RECONCILE *, uint32_t, int);
static int  __rec_init(SESSION *, uint32_t);
static int  __rec_parent_update(
		SESSION *, WT_PAGE *, WT_PAGE *, uint32_t, uint32_t, uint32_t);
static void __rec_parent_update_clean(SESSION *, WT_PAGE *);
static int  __rec_row_int(SESSION *, WT_PAGE *);
static int  __rec_row_int_del(SESSION *, WT_PAGE *);
static int  __rec_row_leaf(SESSION *, WT_PAGE *, uint32_t);
static int  __rec_row_leaf_del(SESSION *, WT_PAGE *);
static int  __rec_row_leaf_insert(SESSION *, WT_INSERT *);
static int  __rec_row_merge(SESSION *, WT_PAGE *);
static int  __rec_row_split(SESSION *, WT_PAGE **, WT_PAGE *);
static int  __rec_split(SESSION *);
static int  __rec_split_finish(SESSION *);
static int  __rec_split_fixup(SESSION *);
static int  __rec_split_init(
		SESSION *, WT_PAGE *, uint64_t, uint32_t, uint32_t);
static int  __rec_split_write(SESSION *, WT_BUF *, void *);
static int  __rec_subtree(SESSION *, WT_PAGE *);
static int  __rec_subtree_col(SESSION *, WT_PAGE *);
static void __rec_subtree_col_clear(SESSION *, WT_PAGE *);
static int  __rec_subtree_row(SESSION *, WT_PAGE *);
static void __rec_subtree_row_clear(SESSION *, WT_PAGE *);
static int  __rec_wrapup(SESSION *, WT_PAGE *);

static int  __hazard_bsearch_cmp(const void *, const void *);
static void __hazard_copy(SESSION *);
static int  __hazard_exclusive(SESSION *, WT_REF *);
static int  __hazard_qsort_cmp(const void *, const void *);

#define	__rec_discard_add_block(session, addr, size)			\
	__rec_discard_add(session, NULL, addr, size)
#define	__rec_discard_add_page(session, page)				\
	__rec_discard_add(session, page, WT_PADDR(page), WT_PSIZE(page))
#define	__rec_discard_add_ovfl(session, ovfl)				\
	__rec_discard_add(session, NULL,				\
	    (ovfl)->addr, WT_DISK_REQUIRED(session, (ovfl)->size))

/*
 * __rec_init --
 *	Initialize the reconciliation structure.
 */
static int
__rec_init(SESSION *session, uint32_t flags)
{
	CONNECTION *conn;
	WT_RECONCILE *r;

	/* Allocate a reconciliation structure if we don't already have one. */
	if ((r = S2C(session)->cache->rec) == NULL) {
		WT_RET(__wt_calloc_def(session, 1, &r));
		S2C(session)->cache->rec = r;

		/*
		 * Allocate memory for a copy of the hazard references -- it's
		 * a fixed size so doesn't need run-time adjustments.
		 */
		conn = S2C(session);
		WT_RET(__wt_calloc_def(session,
		    conn->session_size * conn->hazard_size, &r->hazard));
	}

	r->evict = LF_ISSET(WT_REC_EVICT);
	r->locked = LF_ISSET(WT_REC_LOCKED);

	/*
	 * During internal page reconcilition we track referenced objects that
	 * are discarded when the page is discarded.  That tracking is done per
	 * reconciliation run, initialize it before anything else.
	 */
	__rec_discard_init(r);

	/*
	 * During internal page reconcilition we track in-memory subtrees we
	 * find in the page.  That tracking is done per reconciliation run,
	 * initialize it before anything else.
	 */
	__rec_imref_init(r);

	return (0);
}

/*
 * __rec_destroy --
 *	Clean up the reconciliation structure.
 */
void
__wt_rec_destroy(SESSION *session)
{
	WT_RECONCILE *r;
	WT_SPLIT *spl;
	uint32_t i;

	if ((r = S2C(session)->cache->rec) == NULL)
		return;

	__wt_buf_free(session, &r->dsk);

	if (r->discard != NULL)
		__wt_free(session, r->discard);
	if (r->bnd != NULL)
		__wt_free(session, r->bnd);
	if (r->imref != NULL)
		__wt_free(session, r->imref);
	if (r->hazard != NULL)
		__wt_free(session, r->hazard);

	if (r->split != NULL) {
		for (spl = r->split, i = 0; i < r->split_entries; ++spl, ++i) {
			__wt_buf_free(session, &spl->key);
			if (spl->imp != NULL)
				__wt_page_free(
				    session, spl->imp, WT_ADDR_INVALID, 0);
		}
		__wt_free(session, r->split);
	}

	__wt_free(session, r);
}

/*
 * __rec_incr --
 *	Update the memory tracking structure for a new entry.
 */
static inline void
__rec_incr(SESSION *session, WT_RECONCILE *r, uint32_t size, int entries)
{
	/*
	 * The buffer code is fragile and prone to off-by-one errors --
	 * check for overflow in diagnostic mode.
	 */
	WT_ASSERT(session, r->space_avail >= size);
	WT_ASSERT(session, WT_PTRDIFF32(
	    r->first_free + size, r->dsk.mem) <= r->page_size);

	r->space_avail -= size;
	r->first_free += size;
	r->entries += (uint32_t)entries;
}

/*
 * __rec_allocation_size --
 *	Return the size to the minimum number of allocation units needed
 * (the page size can either grow or shrink), and zero out unused bytes.
 */
static inline uint32_t
__rec_allocation_size(SESSION *session, WT_BUF *buf, uint8_t *end)
{
	BTREE *btree;
	uint32_t alloc_len, current_len, write_len;

	btree = session->btree;

	current_len = WT_PTRDIFF32(end, buf->mem);
	alloc_len = WT_ALIGN(current_len, btree->allocsize);
	write_len = alloc_len - current_len;

	/*
	 * There are lots of offset calculations going on in this code, make
	 * sure we don't overflow the end of the temporary buffer.
	 */
	WT_ASSERT(
	    session, end + write_len <= (uint8_t *)buf->mem + buf->mem_size);

	if (write_len != 0)
		memset(end, 0, write_len);
	return (alloc_len);
}

/*
 * __wt_page_reconcile --
 *	Format an in-memory page to its on-disk format, and write it.
 */
int
__wt_page_reconcile(
    SESSION *session, WT_PAGE *page, uint32_t slvg_skip, uint32_t flags)
{
	BTREE *btree;
	WT_CACHE *cache;
	int ret;

	btree = session->btree;
	cache = S2C(session)->cache;
	ret = 0;

	WT_VERBOSE(S2C(session), WT_VERB_EVICT,
	    (session, "reconcile %s page addr %lu (type %s)",
	    WT_PAGE_IS_MODIFIED(page) ? "dirty" : "clean",
	    (u_long)WT_PADDR(page), __wt_page_type_string(page->type)));

	/*
	 * Handle pages marked for deletion or split.
	 *
	 * Check both before checking for clean pages: deleted and split pages
	 * operate outside the rules for normal pages.
	 *
	 * Pages have their WT_PAGE_DELETED flag set if they're reconciled and
	 * are found to have no valid entries.
	 *
	 * Pages have their WT_PAGE_SPLIT flag set if they're created as part
	 * of an internal split.
	 *
	 * For deleted or split pages, there's nothing to do, they are merged
	 * into their parent when their parent is reconciled.
	 */
	if (F_ISSET(page, WT_PAGE_DELETED | WT_PAGE_SPLIT))
		return (0);

	/* Initialize the reconciliation structure for each new run. */
	WT_RET(__rec_init(session, flags));

	/*
	 * Get exclusive access to the page and review the page's subtree: if
	 * evicting, confirm the page is a page we can evict, and get exclusive
	 * access to pages being merged into the reconciled page as part of
	 * reconciliation.
	 *
	 * If the check fails (for example, we find an in-memory page and it's
	 * an eviction attempt), we're done.
	 */
	WT_RET(__rec_subtree(session, page));

	/*
	 * Clean pages are simple: update the page parent's state and discard
	 * the page.
	 *
	 * Assert the page is being evicted: there's no reason to reconcile a
	 * clean page if not to evict it.)
	 */
	if (!WT_PAGE_IS_MODIFIED(page)) {
		WT_ASSERT(session, LF_ISSET(WT_REC_EVICT));

		WT_STAT_INCR(cache->stats, cache_evict_unmodified);

		WT_RET(__rec_discard_add_page(session, page));
		__rec_parent_update_clean(session, page);
		WT_RET(__rec_discard_evict(session));
		return (0);
	}

	/*
	 * Mark the page clean -- we have exclusive access to any page we're
	 * reconciling, so it doesn't matter when we do it.
	 */
	F_CLR(page, WT_PAGE_MODIFIED);
	WT_STAT_INCR(cache->stats, cache_evict_modified);

	/* Reconcile the page. */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		if (F_ISSET(page, WT_PAGE_BULK_LOAD))
			WT_RET(__rec_col_fix_bulk(session, page));
		else
			WT_RET(__rec_col_fix(session, page));
		break;
	case WT_PAGE_COL_RLE:
		if (F_ISSET(page, WT_PAGE_BULK_LOAD))
			WT_RET(__rec_col_rle_bulk(session, page));
		else
			WT_RET(__rec_col_rle(session, page));
		break;
	case WT_PAGE_COL_VAR:
		if (F_ISSET(page, WT_PAGE_BULK_LOAD))
			WT_RET(__rec_col_var_bulk(session, page));
		else
			WT_RET(__rec_col_var(session, page));
		break;
	case WT_PAGE_COL_INT:
		WT_RET(__rec_col_int(session, page));
		break;
	case WT_PAGE_ROW_INT:
		WT_RET(__rec_row_int(session, page));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_RET(__rec_row_leaf(session, page, slvg_skip));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/*
	 * Resolve the WT_RECONCILE information, update the parent, and evict
	 * the page if requested and possible.
	 */
	WT_RET(__rec_wrapup(session, page));

	/*
	 * Newly created internal pages are normally merged into their parents
	 * when said parent is reconciled.  Newly split root pages can't be
	 * merged (as they have no parent), the new root page must be written.
	 *
	 * We detect root splits when the root page is flagged as a split.  We
	 * do the check at the top level because I'd rather the reconciliation
	 * code not handle two pages at once, and we've just finished with the
	 * original page.
	 *
	 * Reconcile the new root page explicitly rather than waiting for a
	 * natural reconcile, because root splits result from walking the tree
	 * during a sync or close call, and the new root page is the one page
	 * that won't be visited as part of that walk.
	 */
	if (btree->root_page.state == WT_REF_MEM &&
	    F_ISSET(btree->root_page.page, WT_PAGE_SPLIT)) {
		F_CLR(btree->root_page.page, WT_PAGE_SPLIT);
		F_SET(btree->root_page.page, WT_PAGE_PINNED);
		WT_PAGE_SET_MODIFIED(btree->root_page.page);
		ret = __wt_page_reconcile(
		    session, btree->root_page.page, 0, flags);
	}

	return (ret);
}

/*
 * __rec_subtree --
 *	Get exclusive access to a subtree for reconciliation.
 */
static int
__rec_subtree(SESSION *session, WT_PAGE *page)
{
	WT_RECONCILE *r;
	int ret;

	r = S2C(session)->cache->rec;
	ret = 0;

	/*
	 * Attempt exclusive access to the page if our caller doesn't have the
	 * tree locked down.
	 */
	if (!r->locked)
		WT_RET(__hazard_exclusive(session, page->parent_ref));

	/*
	 * Walk the page's subtree, and make sure we can reconcile this page.
	 *
	 * When reconciling a page, it may reference deleted or split pages
	 * which will be merged into the reconciled page.
	 *
	 * If we find an in-memory page, check if we're trying to evict the
	 * page; that's not OK, you can't evict a page that references other
	 * in-memory pages, those pages have to be evicted first.  While the
	 * test is necessary, it shouldn't happen a lot: every reference to
	 * an internal page increments its read generation and so internal
	 * pages shouldn't be selected for eviction until their children have
	 * been evicted.
	 *
	 * If we find a split page, get exclusive access to the page and then
	 * continue, the split page will be merged into our page.
	 *
	 * If we find a deleted page, get exclusive access to the page and then
	 * check its status.  If still deleted, we can continue, the page will
	 * be merged into our page.  However, another thread of control might
	 * have inserted new material and the page is no longer deleted, which
	 * means the reconciliation fails.  (We could allow sync to proceed,
	 * but the sync is going to be inconsistent by definition, why bother?)
	 *
	 * If reconciliation isn't going to be possible, we have to clear any
	 * pages we locked while we were looking.  (I'm re-walking the tree
	 * rather than keeping track of the locked pages on purpose -- the
	 * only time this should ever fail is if eviction's LRU-based choice
	 * is very unlucky.)
	 *
	 * Finally, we do some additional cleanup work during this pass: first,
	 * add any deleted or split pages to our list of pages to discard when
	 * we discard the page being reconciled; second, review deleted pages
	 * for overflow items and schedule them for deletion as well.
	 */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		if ((ret = __rec_subtree_col(session, page)) != 0)
			if (!r->locked)
				__rec_subtree_col_clear(session, page);
		break;
	case WT_PAGE_ROW_INT:
		if ((ret = __rec_subtree_row(session, page)) != 0)
			if (!r->locked)
				__rec_subtree_row_clear(session, page);
		break;
	default:
		break;
	}

	/*
	 * If we're not going to reconcile this page, release our exclusive
	 * reference.
	 */
	if (ret != 0 && !r->locked)
		page->parent_ref->state = WT_REF_MEM;

	return (ret);
}

/*
 * __rec_subtree_col --
 *	Walk a column-store internal page's subtree, handling deleted and split
 *	pages.
 */
static int
__rec_subtree_col(SESSION *session, WT_PAGE *parent)
{
	WT_COL_REF *cref;
	WT_PAGE *page;
	WT_RECONCILE *r;
	WT_REF *parent_ref;
	uint32_t i;

	r = S2C(session)->cache->rec;

	/* For each entry in the page... */
	WT_COL_REF_FOREACH(parent, cref, i) {
		switch (WT_COL_REF_STATE(cref)) {
		case WT_REF_DISK:			/* On-disk */
			continue;
		case WT_REF_LOCKED:			/* Eviction candidate */
			WT_ASSERT(
			    session, WT_COL_REF_STATE(cref) != WT_REF_LOCKED);
			return (1);
		case WT_REF_MEM:			/* In-memory */
			break;
		}
		page = WT_COL_REF_PAGE(cref);

		/*
		 * We found an in-memory page: if the page won't be merged into
		 * its parent, then we can't evict the top-level page.  This is
		 * not a problem, it just means we chose badly when selecting a
		 * page for eviction.  If we're not evicting, sync is flushing
		 * dirty pages: ignore pages that aren't deleted or split.
		 */
		if (!F_ISSET(page, WT_PAGE_DELETED | WT_PAGE_SPLIT)) {
			if (r->evict)
				return (1);
			continue;
		}

		/*
		 * Attempt exclusive access to the page if our caller doesn't
		 * have the tree locked down.
		 */
		if (!r->locked)
			WT_RET(__hazard_exclusive(session, &cref->ref));

		/*
		 * Split pages are always merged into the parent regardless of
		 * their contents, but a deleted page might have changed state
		 * while we waited for exclusive access.   In other words, we
		 * had exclusive access to the parent, but another thread had
		 * a hazard reference to the deleted page in the parent's tree:
		 * while we waited, that thread inserted new material, and the
		 * deleted page became an in-memory page we can't merge, it has
		 * to be reconciled on its own.
		 *
		 * If this happens, we fail the reconciliation: in the case of
		 * eviction, we don't evict this page.  In the case of sync,
		 * this means sync was running while the tree active, we don't
		 * care.  In the case of close, this implies a problem, close
		 * is not allowed in active trees.
		 */
		if (!F_ISSET(page, WT_PAGE_DELETED | WT_PAGE_SPLIT))
			return (1);

		/*
		 * Deleted and split pages are discarded when reconciliation
		 * completes.
		 */
		WT_RET(__rec_discard_add_page(session, page));
		parent_ref = page->parent_ref;
		if (parent_ref->addr != WT_ADDR_INVALID)
			WT_RET(__rec_discard_add_block(
			    session, parent_ref->addr, parent_ref->size));

		/*
		 * Overflow items on deleted pages are also discarded when
		 * reconciliation completes.
		 */
		if (F_ISSET(page, WT_PAGE_DELETED))
			switch (page->type) {
			case WT_PAGE_COL_FIX:
			case WT_PAGE_COL_INT:
			case WT_PAGE_COL_RLE:
				break;
			case WT_PAGE_COL_VAR:
				WT_RET(__rec_col_var_del(session, page));
				break;
			WT_ILLEGAL_FORMAT(session);
			}

		/* Recurse down the tree. */
		if (page->type == WT_PAGE_COL_INT)
			WT_RET(__rec_subtree_col(session, page));
	}
	return (0);
}

/*
 * __rec_subtree_col_clear --
 *	Clear any pages for which we have exclusive access -- reconciliation
 *	isn't possible.
 */
static void
__rec_subtree_col_clear(SESSION *session, WT_PAGE *parent)
{
	WT_COL_REF *cref;
	WT_PAGE *page;
	uint32_t i;

	/* For each entry in the page... */
	WT_COL_REF_FOREACH(parent, cref, i)
		if (WT_COL_REF_STATE(cref) == WT_REF_LOCKED) {
			WT_COL_REF_STATE(cref) = WT_REF_MEM;

			/* Recurse down the tree. */
			page = WT_COL_REF_PAGE(cref);
			if (page->type == WT_PAGE_COL_INT)
				__rec_subtree_col_clear(session, page);
		}
}

/*
 * __rec_subtree_row --
 *	Walk a row-store internal page's subtree, handle deleted and split
 *	pages.
 */
static int
__rec_subtree_row(SESSION *session, WT_PAGE *parent)
{
	WT_PAGE *page;
	WT_RECONCILE *r;
	WT_REF *parent_ref;
	WT_ROW_REF *rref;
	uint32_t i;

	r = S2C(session)->cache->rec;

	/* For each entry in the page... */
	WT_ROW_REF_FOREACH(parent, rref, i) {
		switch (WT_ROW_REF_STATE(rref)) {
		case WT_REF_DISK:			/* On-disk */
			continue;
		case WT_REF_LOCKED:			/* Eviction candidate */
			WT_ASSERT(
			    session, WT_ROW_REF_STATE(rref) != WT_REF_LOCKED);
			return (1);
		case WT_REF_MEM:			/* In-memory */
			break;
		}
		page = WT_ROW_REF_PAGE(rref);

		/*
		 * We found an in-memory page: if the page won't be merged into
		 * its parent, then we can't evict the top-level page.  This is
		 * not a problem, it just means we chose badly when selecting a
		 * page for eviction.  If we're not evicting, sync is flushing
		 * dirty pages: ignore pages that aren't deleted or split.
		 */
		if (!F_ISSET(page, WT_PAGE_DELETED | WT_PAGE_SPLIT)) {
			if (r->evict)
				return (1);
			continue;
		}

		/*
		 * Attempt exclusive access to the page if our caller doesn't
		 * have the tree locked down.
		 */
		if (!r->locked)
			WT_RET(__hazard_exclusive(session, &rref->ref));

		/*
		 * Split pages are always merged into the parent regardless of
		 * their contents, but a deleted page might have changed state
		 * while we waited for exclusive access.   In other words, we
		 * had exclusive access to the parent, but another thread had
		 * a hazard reference to the deleted page in the parent's tree:
		 * while we waited, that thread inserted new material, and the
		 * deleted page became an in-memory page we can't merge, it has
		 * to be reconciled on its own.
		 *
		 * If this happens, we fail the reconciliation: in the case of
		 * eviction, we don't evict this page.  In the case of sync,
		 * this means sync was running while the tree active, we don't
		 * care.  In the case of close, this implies a problem, close
		 * is not allowed in active trees.
		 */
		if (!F_ISSET(page, WT_PAGE_DELETED | WT_PAGE_SPLIT))
			return (1);

		/*
		 * Deleted and split pages are discarded when reconciliation
		 * completes.
		 */
		WT_RET(__rec_discard_add_page(session, page));
		parent_ref = page->parent_ref;
		if (parent_ref->addr != WT_ADDR_INVALID)
			WT_RET(__rec_discard_add_block(
			    session, parent_ref->addr, parent_ref->size));

		/*
		 * Overflow items on deleted pages are also discarded when
		 * reconciliation completes.
		 */
		if (F_ISSET(page, WT_PAGE_DELETED))
			switch (page->type) {
			case WT_PAGE_ROW_INT:
				WT_RET(__rec_row_int_del(session, page));
				break;
			case WT_PAGE_ROW_LEAF:
				WT_RET(__rec_row_leaf_del(session, page));
				break;
			WT_ILLEGAL_FORMAT(session);
			}

		/* Recurse down the tree. */
		if (page->type == WT_PAGE_ROW_INT)
			WT_RET(__rec_subtree_row(session, page));
	}
	return (0);
}

/*
 * __rec_subtree_row_clear --
 *	Clear any pages for which we have exclusive access -- reconciliation
 *	isn't possible.
 */
static void
__rec_subtree_row_clear(SESSION *session, WT_PAGE *parent)
{
	WT_ROW_REF *rref;
	WT_PAGE *page;
	uint32_t i;

	/* For each entry in the page... */
	WT_ROW_REF_FOREACH(parent, rref, i)
		if (WT_ROW_REF_STATE(rref) == WT_REF_LOCKED) {
			WT_ROW_REF_STATE(rref) = WT_REF_MEM;

			/* Recurse down the tree. */
			page = WT_ROW_REF_PAGE(rref);
			if (page->type == WT_PAGE_ROW_INT)
				__rec_subtree_row_clear(session, page);
		}
}

/*
 * __rec_split_init --
 *	Initialization for the reconciliation split functions.
 */
static int
__rec_split_init(
    SESSION *session, WT_PAGE *page, uint64_t recno, uint32_t max, uint32_t min)
{
	BTREE *btree;
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;

	btree = session->btree;

	r = S2C(session)->cache->rec;

	/* Ensure the scratch buffer is large enough. */
	WT_RET(__wt_buf_setsize(session, &r->dsk, (size_t)max));

	/*
	 * Some fields of the disk image are fixed based on the original page,
	 * set them.
	 */
	dsk = r->dsk.mem;
	WT_CLEAR(*dsk);
	dsk->type = page->type;
	dsk->recno = recno;

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
	 * creating overflow items and compacted data, not to mention deleting
	 * overflow items from the underlying pages).  So, the loop calls the
	 * helper functions when approaching a split boundary, and we save the
	 * information at that point.  That allows us to go back and split the
	 * page at the boundary points if we eventually overflow the maximum
	 * page size.
	 */
	r->page_size = max;
	r->split_size = WT_ALIGN((max / 4) * 3, btree->allocsize);
#ifdef HAVE_DIAGNOSTIC
	/*
	 * This won't get tested enough if we don't force the code to create
	 * lots of splits.
	 */
	r->split_size = min;
#else
	WT_UNUSED(min);
#endif
	/*
	 * If the maximum page size is the same as the split page size, there
	 * is no need to maintain split boundaries within a larger page.
	 */
	r->state = max == r->split_size ? SPLIT_TRACKING_OFF : SPLIT_BOUNDARY;

	/*
	 * Initialize the arrays of saved and written page entries to
	 * reference the first slot.
	 */
	r->split_next = r->bnd_next = 0;

	/* Initialize the total entries in split chunks. */
	r->total_entries = 0;

	/*
	 * Set the caller's information and configure so the loop calls us
	 * when approaching the split boundary.
	 */
	r->recno = recno;
	r->entries = 0;
	r->first_free = WT_PAGE_DISK_BYTE(dsk);
	r->space_avail = r->split_size - WT_PAGE_DISK_SIZE;
	return (0);
}

/*
 * __rec_split --
 *	Handle the page reconciliation bookkeeping.  (Did you know "bookkeeper"
 * has 3 doubled letters in a row?  Sweet-tooth does, too.)
 */
static int
__rec_split(SESSION *session)
{
	WT_BOUNDARY *bnd;
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;
	uint32_t current_len;

	/*
	 * Handle page-buffer size tracking; we have to do this work in every
	 * reconciliation loop, and I don't want to repeat the code that many
	 * times.
	 */
	r = S2C(session)->cache->rec;
	dsk = r->dsk.mem;

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
	switch (r->state) {
	case SPLIT_BOUNDARY:				/* Case #1 */
		/*
		 * Make sure there's enough room in which to save the slot.
		 *
		 * The calculation is actually +1, because we save the start
		 * point one past the current entry -- make it +20 so we don't
		 * grow slot-by-slot.
		 */
		if (r->bnd_next + 1 >= r->bnd_entries) {
			WT_RET(__wt_realloc(session, &r->bnd_allocated,
			    (r->bnd_entries + 20) * sizeof(*r->bnd), &r->bnd));
			r->bnd_entries += 20;
		}

		/*
		 * Save the information about where we are when the split would
		 * have happened.
		 *
		 * The first time through, set the starting record number and
		 * buffer address for the first slot from well-known values.
		 */
		if (r->bnd_next == 0) {
			r->bnd[0].recno = dsk->recno;
			r->bnd[0].start = WT_PAGE_DISK_BYTE(dsk);
		}

		/* Set the number of entries for the just finished chunk. */
		bnd = &r->bnd[r->bnd_next++];
		bnd->entries = r->entries - r->total_entries;
		r->total_entries = r->entries;

		/*
		 * Set the starting record number and buffer address for the
		 * next chunk, clear the entries.
		 */
		++bnd;
		bnd->recno = r->recno;
		bnd->start = r->first_free;
		bnd->entries = 0;

		/*
		 * Set the space available to another split-size chunk, if we
		 * have one.
		 */
		current_len = WT_PTRDIFF32(r->first_free, dsk);
		if (current_len + r->split_size <= r->page_size) {
			r->space_avail = r->split_size - WT_PAGE_DISK_SIZE;
			break;
		}

		/*
		 * We don't have room for another split chunk.  Add whatever
		 * space remains in the maximum page size, hopefully it will
		 * be enough.
		 */
		r->state = SPLIT_MAX;
		r->space_avail =
		    (r->page_size - WT_PAGE_DISK_SIZE) - current_len;
		break;
	case SPLIT_MAX:					/* Case #2 */
		/*
		 * It didn't all fit, but we just noticed that.
		 *
		 * Cycle through the saved split-point information, writing the
		 * split chunks we have tracked.
		 */
		WT_RET(__rec_split_fixup(session));

		/* We're done saving split chunks. */
		r->state = SPLIT_TRACKING_OFF;
		break;
	case SPLIT_TRACKING_OFF:			/* Case #3 */
		/*
		 * It didn't all fit, but either we've already noticed it and
		 * are now processing the rest of the page at the split-size
		 * boundaries, or, the split size was the same as the page size,
		 * so we never bothered with saving split-point information.
		 *
		 * Write the current disk image.
		 */
		dsk->u.entries = r->entries;
		WT_RET(__rec_split_write(session, &r->dsk, r->first_free));

		/*
		 * Set the starting record number and buffer information for the
		 * next chunk; we only get here if we've never split, or have
		 * already split, so we're using using split-size chunks from
		 * here on out.
		 */
		dsk->recno = r->recno;
		r->entries = 0;
		r->first_free = WT_PAGE_DISK_BYTE(dsk);
		r->space_avail = r->split_size - WT_PAGE_DISK_SIZE;
		break;
	}
	return (0);
}

/*
 * __rec_split_finish --
 *	Wrap up reconciliation.
 */
static int
__rec_split_finish(SESSION *session)
{
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;

	r = S2C(session)->cache->rec;

	/*
	 * We're done reconciling a page; write anything we have accumulated
	 * in the buffer at this point.
	 */
	if (r->entries == 0)
		return (0);

	dsk = r->dsk.mem;
	dsk->u.entries = r->entries;
	return (__rec_split_write(session, &r->dsk, r->first_free));
}

/*
 * __rec_split_fixup --
 *	Physically split the already-written page data.
 */
static int
__rec_split_fixup(SESSION *session)
{
	WT_BOUNDARY *bnd;
	WT_BUF *tmp;
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;
	uint32_t i, len;
	uint8_t *dsk_start;
	int ret;

	ret = 0;

	/*
	 * When we overflow physical limits of the page, we walk the list of
	 * split chunks we've created and write those pages out, then update
	 * the caller's information.
	 */
	r = S2C(session)->cache->rec;

	/*
	 * The data isn't laid out on page-boundaries or nul-byte padded; copy
	 * it into a clean buffer before writing it.
	 *
	 * Allocate a scratch buffer to hold the new disk image.  Copy the
	 * WT_PAGE_DISK header onto the scratch buffer, most of the header
	 * information remains unchanged between the pages.
	 */
	WT_RET(__wt_scr_alloc(session, r->split_size, &tmp));
	memcpy(tmp->mem, r->dsk.mem, WT_PAGE_DISK_SIZE);

	/*
	 * For each split chunk we've created, update the disk image and copy
	 * it into place.
	 */
	dsk = tmp->mem;
	dsk_start = WT_PAGE_DISK_BYTE(dsk);
	for (i = 0, bnd = r->bnd; i < r->bnd_next; ++i, ++bnd) {
		/* Set the starting record number and number of entries. */
		dsk->recno = bnd->recno;
		dsk->u.entries = bnd->entries;

		/* Copy out the page contents, and write it. */
		len = WT_PTRDIFF32((bnd + 1)->start, bnd->start);
		memcpy(dsk_start, bnd->start, len);
		WT_ERR(__rec_split_write(session, tmp, dsk_start + len));
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
	WT_ASSERT(session, len < r->split_size - WT_PAGE_DISK_SIZE);

	dsk = r->dsk.mem;
	dsk->recno = bnd->recno;
	dsk_start = WT_PAGE_DISK_BYTE(dsk);
	(void)memmove(dsk_start, bnd->start, len);

	r->entries -= r->total_entries;
	r->first_free = dsk_start + len;
	r->space_avail = (r->split_size - WT_PAGE_DISK_SIZE) - len;

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __rec_split_write --
 *	Write a disk block out for the split helper functions.
 */
static int
__rec_split_write(SESSION *session, WT_BUF *buf, void *end)
{
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;
	WT_SPLIT *spl;
	uint32_t addr, size;
	int ret;

	r = S2C(session)->cache->rec;
	dsk = NULL;

	/*
	 * Set the disk block size and clear trailing bytes.
	 * Allocate file space.
	 * Write the disk block.
	 */
	size = __rec_allocation_size(session, buf, end);
	WT_RET(__wt_block_alloc(session, &addr, size));
	WT_RET(__wt_disk_write(session, buf->mem, addr, size));

	/* Save the key and addr/size pairs to update the parent. */
	if (r->split_next == r->split_entries) {
		WT_RET(__wt_realloc(session, &r->split_allocated,
		    (r->split_entries + 20) * sizeof(*r->split), &r->split));
		r->split_entries += 20;
	}
	spl = &r->split[r->split_next++];
	spl->off.addr = addr;
	spl->off.size = size;

	/*
	 * For a column-store, the key is the recno, for a row-store, it's the
	 * first key on the page, a variable-length byte string.
	 */
	dsk = buf->mem;
	switch (dsk->type) {
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_cell_process(
		    session, WT_PAGE_DISK_BYTE(dsk), &spl->key));
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		WT_RECNO(&spl->off) = dsk->recno;
		break;
	}

	/* If we're evicting the page, there's no more work to do. */
	if (r->evict) {
		spl->imp = NULL;
		return (0);
	}

	/*
	 * If we're not evicting the page, we'll need a new in-memory version:
	 * create an in-memory page and take any list of in-memory references.
	 */
	WT_RET(__wt_calloc(session, (size_t)size, sizeof(uint8_t), &dsk));
	memcpy(dsk, buf->mem, size);

	if ((ret = __wt_page_inmem(session, NULL, NULL, dsk, &page)) != 0) {
		__wt_free(session, dsk);
		return (ret);
	}
	spl->imp = page;
	return (0);
}

/*
 * __rec_col_int --
 *	Reconcile a column-store internal page.
 */
static int
__rec_col_int(SESSION *session, WT_PAGE *page)
{
	WT_RET(__rec_split_init(session, page,
	    page->u.col_int.recno,
	    session->btree->intlmax, session->btree->intlmin));

	/*
	 * Walking the row-store internal pages is complicated by the fact that
	 * we're taking keys from the underlying disk image for the top-level
	 * page and we're taking keys from in-memory structures for merge pages.
	 * Column-store is simpler because the only information we copy is the
	 * WT_OFF_RECORD structure, and it comes from in-memory structures in
	 * both the top-level and merge cases.  In short, both the top-level
	 * and merge page walks look the same, and we just call the merge page
	 * function on the top-level page.
	 */
	WT_RET(__rec_col_merge(session, page));

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_col_merge --
 *	Recursively walk a column-store internal tree of merge pages.
 */
static int
__rec_col_merge(SESSION *session, WT_PAGE *page)
{
	WT_COL_REF *cref;
	WT_OFF_RECORD off;
	WT_PAGE *rp;
	WT_RECONCILE *r;
	uint32_t i;

	r = S2C(session)->cache->rec;

	/* For each entry in the page... */
	WT_COL_REF_FOREACH(page, cref, i) {
		/* Update the starting record number in case we split. */
		r->recno = cref->recno;

		/*
		 * The page may be deleted or internally created during a split.
		 * Deleted/split pages are merged into the parent and discarded.
		 *
		 * !!!
		 * Column-store formats don't support deleted pages; they can
		 * shrink, but deleting a page would remove part of the record
		 * count name space.  This code is here for if/when they support
		 * deletes, but for now it's not OK.
		 */
		if (WT_COL_REF_STATE(cref) != WT_REF_DISK) {
			rp = WT_COL_REF_PAGE(cref);
			if (F_ISSET(rp, WT_PAGE_DELETED | WT_PAGE_SPLIT)) {
				WT_ASSERT(
				    session, !F_ISSET(rp, WT_PAGE_DELETED));
				if (F_ISSET(rp, WT_PAGE_SPLIT))
					WT_RET(__rec_col_merge(session, rp));
				continue;
			}

			/* Save any in-memory subtree reference. */
			WT_RET(__rec_imref_add(session, &cref->ref));
		}

		/* Any off-page reference must be a valid disk address. */
		WT_ASSERT(session, WT_COL_REF_ADDR(cref) != WT_ADDR_INVALID);

		/* Boundary: allocate, split or write the page. */
		while (sizeof(WT_OFF_RECORD) > r->space_avail)
			WT_RET(__rec_split(session));

		/* Copy a new WT_OFF_RECORD structure into place. */
		off.addr = WT_COL_REF_ADDR(cref);
		off.size = WT_COL_REF_SIZE(cref);
		WT_RECNO(&off) = cref->recno;
		memcpy(r->first_free, &off, sizeof(WT_OFF_RECORD));
		__rec_incr(session, r, WT_SIZEOF32(WT_OFF_RECORD), 1);
	}

	return (0);
}

/*
 * __rec_col_fix --
 *	Reconcile a fixed-width, column-store leaf page (does not handle
 * run-length encoding).
 */
static int
__rec_col_fix(SESSION *session, WT_PAGE *page)
{
	BTREE *btree;
	WT_BUF *tmp;
	WT_COL *cip;
	WT_RECONCILE *r;
	WT_UPDATE *upd;
	uint32_t i, len;
	uint8_t *data;
	void *cipdata;
	int ret;

	btree = session->btree;
	tmp = NULL;
	r = S2C(session)->cache->rec;
	ret = 0;

	/*
	 * We need a "deleted" data item to store on the page; get a scratch
	 * buffer, clear the contents and set the delete flag.
	 */
	len = btree->fixed_len;
	WT_ERR(__wt_scr_alloc(session, len, &tmp));
	memset(tmp->mem, 0, len);
	WT_FIX_DELETE_SET(tmp->mem);

	/*
	 * Fixed-size pages can't split, but we use the underlying helper
	 * functions because they don't add much overhead, and it's better
	 * if all the reconciliation functions look the same.
	 */
	WT_ERR(__rec_split_init(session, page,
	    page->u.col_leaf.recno,
	    session->btree->leafmax, session->btree->leafmin));

	/* For each entry in the in-memory page... */
	WT_COL_FOREACH(page, cip, i) {
		/*
		 * Get a reference to the data, on- or off- page, and see if
		 * it's been deleted.
		 */
		if ((upd = WT_COL_UPDATE(page, cip)) != NULL) {
			if (WT_UPDATE_DELETED_ISSET(upd))
				data = tmp->mem;	/* Deleted */
			else				/* Updated */
				data = WT_UPDATE_DATA(upd);
		} else {
			cipdata = WT_COL_PTR(page, cip);
			if (WT_FIX_DELETE_ISSET(cipdata))
				data = tmp->mem;	/* On-disk deleted */
			else
				data = cipdata;		/* On-disk data */
		}

		/*
		 * When reconciling a fixed-width page that doesn't support
		 * run-length encoding, the on-page information can't change
		 * size -- there's no reason to ever split such a page.
		 */
		memcpy(r->first_free, data, len);
		__rec_incr(session, r, len, 1);
	}

	/* Write the remnant page. */
	ret = __rec_split_finish(session);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __rec_col_fix_bulk --
 *	Reconcile a bulk-loaded, fixed-width column-store leaf page (does not
 * handle run-length encoding).
 */
static int
__rec_col_fix_bulk(SESSION *session, WT_PAGE *page)
{
	BTREE *btree;
	WT_RECONCILE *r;
	WT_UPDATE *upd;
	uint32_t len;

	btree = session->btree;
	r = S2C(session)->cache->rec;
	len = btree->fixed_len;

	WT_RET(__rec_split_init(session, page,
	    page->u.bulk.recno,
	    session->btree->leafmax, session->btree->leafmin));

	/* For each entry in the update list... */
	for (upd = page->u.bulk.upd; upd != NULL; upd = upd->next) {
		/* Boundary: allocate, split or write the page. */
		while (len > r->space_avail)
			WT_RET(__rec_split(session));

		memcpy(r->first_free, WT_UPDATE_DATA(upd), len);
		__rec_incr(session, r, len, 1);

		/* Update the starting record number in case we split. */
		++r->recno;
	}

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_col_rle --
 *	Reconcile a fixed-width, run-length encoded, column-store leaf page.
 */
static int
__rec_col_rle(SESSION *session, WT_PAGE *page)
{
	BTREE *btree;
	WT_BUF *tmp;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_RECONCILE *r;
	uint32_t i, len;
	uint16_t n, nrepeat, repeat_count;
	uint8_t *data, *last_data;
	void *cipdata;
	int ret;

	btree = session->btree;
	tmp = NULL;
	r = S2C(session)->cache->rec;
	last_data = NULL;
	ret = 0;

	/*
	 * We need a "deleted" data item to store on the page.  Make sure the
	 * SESSION's scratch buffer is big enough.  Clear the buffer's contents
	 * and set the delete flag.
	 */
	len = btree->fixed_len;
	WT_ERR(__wt_scr_alloc(session, len, &tmp));
	memset(tmp->mem, 0, len);
	WT_FIX_DELETE_SET(tmp->mem);

	WT_RET(__rec_split_init(session, page,
	    page->u.col_leaf.recno,
	    session->btree->leafmax, session->btree->leafmin));

	/* For each entry in the in-memory page... */
	WT_COL_FOREACH(page, cip, i) {
		cipdata = WT_COL_PTR(page, cip);
		/*
		 * Generate entries for the new page: loop through the repeat
		 * records, checking for WT_INSERT entries matching the record
		 * number.
		 *
		 * Note the increment of recno in the for loop to update the
		 * starting record number in case we split.
		 */
		ins = WT_COL_INSERT(page, cip),
		nrepeat = WT_RLE_REPEAT_COUNT(cipdata);
		for (n = 0;
		    n < nrepeat; n += repeat_count, r->recno += repeat_count) {
			if (ins != NULL && WT_INSERT_RECNO(ins) == r->recno) {
				/* Use the WT_INSERT's WT_UPDATE field. */
				if (WT_UPDATE_DELETED_ISSET(ins->upd))
					data = tmp->mem;
				else
					data = WT_UPDATE_DATA(ins->upd);
				repeat_count = 1;

				ins = ins->next;
			} else {
				if (WT_FIX_DELETE_ISSET(cipdata))
					data = tmp->mem;
				else
					data = WT_RLE_REPEAT_DATA(cipdata);
				/*
				 * The repeat count is the number of records
				 * up to the next WT_INSERT record, or up to
				 * the end of this entry if we have no more
				 * WT_INSERT records.
				 */
				if (ins == NULL)
					repeat_count = nrepeat - n;
				else
					repeat_count = (uint16_t)
					    (WT_INSERT_RECNO(ins) - r->recno);
			}

			/*
			 * In all cases, check the last entry written on the
			 * page to see if it's identical, and increment its
			 * repeat count where possible.
			 */
			if (last_data != NULL && memcmp(
			    WT_RLE_REPEAT_DATA(last_data), data, len) == 0 &&
			    WT_RLE_REPEAT_COUNT(last_data) < UINT16_MAX) {
				WT_RLE_REPEAT_COUNT(last_data) += repeat_count;
				continue;
			}

			/* Boundary: allocate, split or write the page. */
			while (len + WT_SIZEOF32(uint16_t) > r->space_avail)
				WT_ERR(__rec_split(session));

			last_data = r->first_free;
			WT_RLE_REPEAT_COUNT(last_data) = repeat_count;
			memcpy(WT_RLE_REPEAT_DATA(last_data), data, len);
			__rec_incr(session, r, len + WT_SIZEOF32(uint16_t), 1);
		}
	}

	/* Write the remnant page. */
	ret = __rec_split_finish(session);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __rec_col_rle_bulk --
 *	Reconcile a bulk-loaded, fixed-width, run-length encoded, column-store
 *	leaf page.
 */
static int
__rec_col_rle_bulk(SESSION *session, WT_PAGE *page)
{
	BTREE *btree;
	WT_UPDATE *upd;
	WT_RECONCILE *r;
	uint32_t len;
	uint8_t *data, *last_data;

	btree = session->btree;
	r = S2C(session)->cache->rec;
	len = btree->fixed_len;
	last_data = NULL;

	WT_RET(__rec_split_init(session, page,
	    page->u.bulk.recno,
	    session->btree->leafmax, session->btree->leafmin));

	/* For each entry in the update list... */
	for (upd = page->u.bulk.upd; upd != NULL; upd = upd->next, ++r->recno) {
		data = WT_UPDATE_DATA(upd);

		/*
		 * In all cases, check the last entry written on the page to
		 * see if it's identical, and increment its repeat count where
		 * possible.
		 */
		if (last_data != NULL && memcmp(
		    WT_RLE_REPEAT_DATA(last_data), data, len) == 0 &&
		    WT_RLE_REPEAT_COUNT(last_data) < UINT16_MAX) {
			++WT_RLE_REPEAT_COUNT(last_data);
			continue;
		}

		/* Boundary: allocate, split or write the page. */
		while (len + WT_SIZEOF32(uint16_t) > r->space_avail)
			WT_RET(__rec_split(session));

		last_data = r->first_free;
		WT_RLE_REPEAT_COUNT(last_data) = 1;
		memcpy(WT_RLE_REPEAT_DATA(last_data), data, len);
		__rec_incr(session, r, len + WT_SIZEOF32(uint16_t), 1);
	}

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_col_var --
 *	Reconcile a variable-width column-store leaf page.
 */
static int
__rec_col_var(SESSION *session, WT_PAGE *page)
{
	enum { DATA_DELETED, DATA_ON_PAGE, DATA_OFF_PAGE } data_loc;
	WT_BUF *value, _value;
	WT_CELL value_cell, *cell;
	WT_COL *cip;
	WT_OVFL value_ovfl;
	WT_RECONCILE *r;
	WT_UPDATE *upd;
	uint32_t i, len;

	r = S2C(session)->cache->rec;

	WT_CLEAR(value_cell);
	WT_CLEAR(_value);
	value = &_value;

	WT_RET(__rec_split_init(session, page,
	    page->u.col_leaf.recno,
	    session->btree->leafmax, session->btree->leafmin));

	/* For each entry in the in-memory page... */
	WT_COL_FOREACH(page, cip, i) {
		/*
		 * Get a reference to the value: it's either an update or the
		 * original on-page item.
		 */
		cell = WT_COL_PTR(page, cip);
		if ((upd = WT_COL_UPDATE(page, cip)) != NULL) {
			/*
			 * If we update an overflow value, free the underlying
			 * file space.
			 */
			if (WT_CELL_TYPE(cell) == WT_CELL_DATA_OVFL)
				WT_RET(__rec_discard_add_ovfl(
				    session, WT_CELL_BYTE_OVFL(cell)));

			/*
			 * Check for deletion, else build the value's WT_CELL
			 * chunk from the most recent update value.
			 */
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				WT_CELL_SET(&value_cell, WT_CELL_DEL, 0);
				len = WT_CELL_SPACE_REQ(0);
				data_loc = DATA_DELETED;
			} else {
				value->data = WT_UPDATE_DATA(upd);
				value->size = upd->size;
				WT_RET(__rec_cell_build_value(
				    session, value, &value_cell, &value_ovfl));
				len = WT_CELL_SPACE_REQ(value->size);
				data_loc = DATA_OFF_PAGE;
			}
		} else {
			value->data = cell;
			value->size = WT_CELL_LEN(cell);
			len = WT_CELL_SPACE_REQ(value->size);
			data_loc = DATA_ON_PAGE;
		}

		/* Boundary: allocate, split or write the page. */
		while (len > r->space_avail)
			WT_RET(__rec_split(session));

		switch (data_loc) {
		case DATA_DELETED:
			memcpy(r->first_free, &value_cell, sizeof(value_cell));
			break;
		case DATA_ON_PAGE:
			memcpy(r->first_free, value->data, len);
			break;
		case DATA_OFF_PAGE:
			memcpy(r->first_free, &value_cell, sizeof(value_cell));
			memcpy(r->first_free +
			    sizeof(value_cell), value->data, value->size);
			break;
		}
		__rec_incr(session, r, len, 1);

		/* Update the starting record number in case we split. */
		++r->recno;
	}

	/* Free any allocated memory. */
	if (value->mem != NULL)
		__wt_buf_free(session, value);

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_col_var_bulk --
 *	Reconcile a bulk-loaded, variable-width column-store leaf page.
 */
static int
__rec_col_var_bulk(SESSION *session, WT_PAGE *page)
{
	WT_BUF *value, _value;
	WT_CELL value_cell;
	WT_OVFL value_ovfl;
	WT_RECONCILE *r;
	WT_UPDATE *upd;
	uint32_t len;

	r = S2C(session)->cache->rec;

	WT_CLEAR(value_cell);
	WT_CLEAR(_value);
	value = &_value;

	WT_RET(__rec_split_init(session, page,
	    page->u.bulk.recno,
	    session->btree->leafmax, session->btree->leafmin));

	/* For each entry in the update list... */
	for (upd = page->u.bulk.upd; upd != NULL; upd = upd->next) {
		value->data = WT_UPDATE_DATA(upd);
		value->size = upd->size;
		WT_RET(__rec_cell_build_value(
		    session, value, &value_cell, &value_ovfl));
		len = WT_CELL_SPACE_REQ(value->size);

		/* Boundary: allocate, split or write the page. */
		while (len > r->space_avail)
			WT_RET(__rec_split(session));

		memcpy(r->first_free, &value_cell, sizeof(value_cell));
		memcpy(r->first_free +
		    sizeof(value_cell), value->data, value->size);

		__rec_incr(session, r, len, 1);

		/* Update the starting record number in case we split. */
		++r->recno;
	}

	/* Free any allocated memory. */
	if (value->mem != NULL)
		__wt_buf_free(session, value);

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_col_var_del --
 *	Walk a deleted variable-length column-store leaf page and schedule any
 *	overflow items for discard.
 */
static int
__rec_col_var_del(SESSION *session, WT_PAGE *page)
{
	WT_CELL *cell;
	WT_COL *cip;
	uint32_t i;

	/*
	 * We're deleting the page, which means any overflow item we ever had
	 * was deleted.
	 *
	 * We have to walk both the WT_ROW structures and the original page --
	 * see the comment at WT_ROW_AND_KEY_FOREACH for details.
	 *
	 * For each entry in the in-memory page...
	 */
	WT_COL_FOREACH(page, cip, i) {
		cell = WT_COL_PTR(page, cip);
		if (WT_CELL_TYPE(cell) == WT_CELL_DATA_OVFL)
			WT_RET(__rec_discard_add_ovfl(
			    session, WT_CELL_BYTE_OVFL(cell)));
	}
	return (0);
}

/*
 * __rec_row_int --
 *	Reconcile a row-store internal page.
 */
static int
__rec_row_int(SESSION *session, WT_PAGE *page)
{
	WT_CELL *key_cell, *value_cell;
	WT_OFF *from;
	WT_PAGE *rp;
	WT_RECONCILE *r;
	WT_ROW_REF *rref;
	uint32_t i, len;

	r = S2C(session)->cache->rec;

	WT_RET(__rec_split_init(session,
	    page, 0ULL, session->btree->intlmax, session->btree->intlmin));

	/*
	 * There are two kinds of row-store internal pages we reconcile: the
	 * first is a page created entirely in-memory, in which case there's
	 * no underlying disk image.  The second is a page read from disk,
	 * in which case we can take the keys from the underlying disk image.
	 *
	 * Internal pages created in-memory are always merged into their parent
	 * in order to keep the tree from growing deeper on every split.  For
	 * that reason, reconciliation of those pages consists of updating the
	 * page state and returning, as none of the real work of reconciliation
	 * is done until the parent page into which the created pages will be
	 * merged is itself reconciled.  In other words, we ignore internally
	 * created pages until that parent is reconciled, at which time we walk
	 * the subtree rooted in that parent and consolidate the merged pages.
	 *
	 * There is a special case: if the root splits, there's no parent into
	 * which it can be merged, so the reconciliation code turns off the
	 * merge flag, and reconciles the page anyway.  In that case we end up
	 * here, with no disk image.  This code is here to handle that specific
	 * case.
	 */
	if (page->dsk == NULL) {
		WT_RET(__rec_row_merge(session, page));
		return (__rec_split_finish(session));
	}

	/*
	 * We have to walk both the WT_ROW structures and the original page --
	 * see the comment at WT_ROW_REF_AND_KEY_FOREACH for details.
	 *
	 * For each entry in the in-memory page...
	 */
	WT_ROW_REF_AND_KEY_FOREACH(page, rref, key_cell, i) {
		/*
		 * The page may be deleted or internally created during a split.
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
		 * the key may be incorrect.  Imagine the following tree:
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
		 * To fix this problem, we take the original page's key as our
		 * first key, because we know that key sorts before any possible
		 * key inserted into the subtree, and discard whatever 0th key
		 * is on the split-created internal page.
		 */
		if (WT_ROW_REF_STATE(rref) != WT_REF_DISK) {
			rp = WT_ROW_REF_PAGE(rref);
			if (F_ISSET(rp, WT_PAGE_DELETED | WT_PAGE_SPLIT)) {
				/* Delete overflow keys for merged pages */
				if (WT_CELL_TYPE(key_cell) == WT_CELL_KEY_OVFL)
					WT_RET(__rec_discard_add_ovfl(session,
					    WT_CELL_BYTE_OVFL(key_cell)));

				/* Merge split subtrees */
				if (F_ISSET(rp, WT_PAGE_SPLIT)) {
					r->merge_ref = rref;
					WT_RET(__rec_row_merge(session, rp));
				}
				continue;
			}

			/* Save any in-memory subtree reference. */
			WT_RET(__rec_imref_add(session, &rref->ref));
		}

		/* Any off-page reference must be a valid disk address. */
		WT_ASSERT(session, WT_ROW_REF_ADDR(rref) != WT_ADDR_INVALID);

		value_cell = WT_CELL_NEXT(key_cell);
		len = WT_PTRDIFF32(WT_CELL_NEXT(value_cell), key_cell);

		/* Boundary: allocate, split or write the page. */
		while (len > r->space_avail)
			WT_RET(__rec_split(session));

		/*
		 * XXX
		 * Overwrite the original on-page information with new page
		 * locations and then copy the two WT_CELL's from the page;
		 * that will eventually change.
		 */
		from = WT_CELL_BYTE_OFF(value_cell);
		from->addr = WT_ROW_REF_ADDR(rref);
		from->size = WT_ROW_REF_SIZE(rref);

		/* Copy the key and re-written WT_OFF structure into place. */
		memcpy(r->first_free, key_cell, len);
		__rec_incr(session, r, len, 2);
	}

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_row_merge --
 *	Recursively walk a row-store internal tree of merge pages.
 */
static int
__rec_row_merge(SESSION *session, WT_PAGE *page)
{
	WT_BUF key;
	WT_CELL cell;
	WT_OFF off;
	WT_OVFL key_ovfl;
	WT_PAGE *rp;
	WT_RECONCILE *r;
	WT_ROW_REF *rref;
	uint32_t i;

	WT_CLEAR(key);
	r = S2C(session)->cache->rec;

	/* For each entry in the in-memory page... */
	WT_ROW_REF_FOREACH(page, rref, i) {
		/*
		 * The page may be deleted or internally created during a split.
		 * Deleted/split pages are merged into the parent and discarded.
		 */
		if (WT_ROW_REF_STATE(rref) != WT_REF_DISK) {
			rp = WT_ROW_REF_PAGE(rref);
			if (F_ISSET(rp, WT_PAGE_DELETED | WT_PAGE_SPLIT)) {
				/* Merge split subtrees */
				if (F_ISSET(rp, WT_PAGE_SPLIT))
					WT_RET(__rec_row_merge(session, rp));
				continue;
			}

			/* Save any in-memory subtree reference. */
			WT_RET(__rec_imref_add(session, &rref->ref));
		}

		/* Any off-page reference must be a valid disk address. */
		WT_ASSERT(session, WT_ROW_REF_ADDR(rref) != WT_ADDR_INVALID);

		/*
		 * Build a key to store on the page.  If this is the 0th key in
		 * a "to be merged" subtree, use merge correction key that was
		 * saved off before this function was called from the top-level
		 * parent page.
		 */
		if (r->merge_ref == NULL) {
			key.data = rref->key;
			key.size = rref->size;
		} else {
			key.data = r->merge_ref->key;
			key.size = r->merge_ref->size;
			r->merge_ref = NULL;
		}
		WT_RET(__rec_cell_build_key(session, &key, &cell, &key_ovfl));

		/* Boundary: allocate, split or write the page. */
		while (WT_CELL_SPACE_REQ(key.size) +
		    WT_CELL_SPACE_REQ(sizeof(WT_OFF)) > r->space_avail)
			WT_RET(__rec_split(session));

		/* Copy the key into place. */
		memcpy(r->first_free, &cell, sizeof(WT_CELL));
		memcpy(r->first_free + sizeof(WT_CELL), key.data, key.size);
		__rec_incr(session, r, WT_CELL_SPACE_REQ(key.size), 1);

		/* Copy the off-page reference into place. */
		off.addr = WT_ROW_REF_ADDR(rref);
		off.size = WT_ROW_REF_SIZE(rref);
		WT_CELL_SET(&cell, WT_CELL_OFF, sizeof(WT_OFF));
		memcpy(r->first_free, &cell, sizeof(WT_CELL));
		memcpy(r->first_free + sizeof(WT_CELL), &off, sizeof(WT_OFF));
		__rec_incr(session, r, WT_CELL_SPACE_REQ(sizeof(WT_OFF)), 1);
	}

	/* Free any allocated memory. */
	if (key.mem != NULL)
		__wt_buf_free(session, &key);

	return (0);
}

/*
 * __rec_row_int_del --
 *	Walk a deleted row-store internal page and schedule any overflow items
 *	for discard.
 */
static int
__rec_row_int_del(SESSION *session, WT_PAGE *page)
{
	WT_CELL *key_cell;
	WT_ROW_REF *rref;
	uint32_t i;

	/*
	 * We're deleting the page, which means any overflow item we ever had
	 * was deleted.
	 *
	 * We have to walk both the WT_ROW structures and the original page --
	 * see the comment at WT_ROW_REF_AND_KEY_FOREACH for details.
	 *
	 * For each entry in the in-memory page...
	 */
	WT_ROW_REF_AND_KEY_FOREACH(page, rref, key_cell, i)
		if (WT_CELL_TYPE(key_cell) == WT_CELL_KEY_OVFL)
			WT_RET(__rec_discard_add_ovfl(
			    session, WT_CELL_BYTE_OVFL(key_cell)));

	WT_UNUSED(rref);			/* Set but not read. */
	return (0);
}

/*
 * __rec_row_leaf --
 *	Reconcile a row-store leaf page.
 */
static int
__rec_row_leaf(SESSION *session, WT_PAGE *page, uint32_t slvg_skip)
{
	enum { DATA_ON_PAGE, DATA_OFF_PAGE, EMPTY_DATA } data_loc;
	WT_BUF value_buf;
	WT_CELL value_cell, *key_cell;
	WT_INSERT *ins;
	WT_OVFL value_ovfl;
	WT_RECONCILE *r;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i, key_len, value_len;
	void *ripvalue;

	WT_CLEAR(value_buf);
	r = S2C(session)->cache->rec;

	WT_RET(__rec_split_init(session,
	    page, 0ULL, session->btree->leafmax, session->btree->leafmin));

	/*
	 * Bulk-loaded pages are just an insert list and nothing more.  As
	 * row-store leaf pages already have to deal with insert lists, it's
	 * pretty easy to hack into that path.
	 */
	if (F_ISSET(page, WT_PAGE_BULK_LOAD)) {
		WT_RET(__rec_row_leaf_insert(session, page->u.bulk.ins));
		return (__rec_split_finish(session));
	}

	/*
	 * Write any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	if ((ins = WT_ROW_INSERT_SMALLEST(page)) != NULL)
		WT_RET(__rec_row_leaf_insert(session, ins));

	/*
	 * Walk the page, writing key/value pairs.
	 *
	 * We have to walk both the WT_ROW structures and the original page --
	 * see the comment at WT_ROW_AND_KEY_FOREACH for details.
	 */
	WT_ROW_AND_KEY_FOREACH(page, rip, key_cell, i) {
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
		 * Get a reference to the value.  We get the value first because
		 * it may have been deleted, in which case we ignore the pair.
		 */
		value_len = 0;
		if ((upd = WT_ROW_UPDATE(page, rip)) != NULL) {
			/*
			 * If we update an overflow value, free the underlying
			 * file space.
			 */
			if (!WT_ROW_EMPTY_ISSET(rip)) {
				ripvalue = WT_ROW_PTR(page, rip);
				if (WT_CELL_TYPE(ripvalue) == WT_CELL_DATA_OVFL)
					WT_RET(__rec_discard_add_ovfl(session,
					    WT_CELL_BYTE_OVFL(ripvalue)));
			}

			/*
			 * If this key/value pair was deleted, we're done.  If
			 * the key was an overflow item, free the underlying
			 * file space.
			 */
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				if (WT_CELL_TYPE(key_cell) == WT_CELL_KEY_OVFL)
					WT_RET(__rec_discard_add_ovfl(session,
					    WT_CELL_BYTE_OVFL(key_cell)));
				goto leaf_insert;
			}

			/*
			 * If no value, nothing needs to be copied.  Otherwise,
			 * build the value's WT_CELL chunk from the most recent
			 * update value.
			 */
			if (upd->size == 0)
				data_loc = EMPTY_DATA;
			else {
				value_buf.data = WT_UPDATE_DATA(upd);
				value_buf.size = upd->size;
				WT_RET(__rec_cell_build_value(session,
				    &value_buf, &value_cell, &value_ovfl));
				value_len = WT_CELL_SPACE_REQ(value_buf.size);
				data_loc = DATA_OFF_PAGE;
			}
		} else {
			/*
			 * Copy the item off the page -- however, when the page
			 * was read into memory, there may not have been a value
			 * item, that is, it may have been zero length.  Catch
			 * that case.
			 */
			if (WT_ROW_EMPTY_ISSET(rip))
				data_loc = EMPTY_DATA;
			else {
				ripvalue = WT_ROW_PTR(page, rip);
				value_buf.data = ripvalue;
				value_buf.size =
				    WT_CELL_SPACE_REQ(WT_CELL_LEN(ripvalue));
				value_len = value_buf.size;
				data_loc = DATA_ON_PAGE;
			}
		}

		/* Take the key's WT_CELL from the original page. */
		key_len = WT_CELL_SPACE_REQ(WT_CELL_LEN(key_cell));

		/* Boundary: allocate, split or write the page. */
		while (key_len + value_len > r->space_avail)
			WT_RET(__rec_split(session));

		/* Copy the key onto the page. */
		memcpy(r->first_free, key_cell, key_len);
		__rec_incr(session, r, key_len, 1);

		/* Copy the value onto the page. */
		switch (data_loc) {
		case DATA_ON_PAGE:
			memcpy(r->first_free, value_buf.data, value_len);
			__rec_incr(session, r, value_len, 1);
			break;
		case DATA_OFF_PAGE:
			memcpy(r->first_free, &value_cell, sizeof(value_cell));
			memcpy(r->first_free +
			    sizeof(WT_CELL), value_buf.data, value_buf.size);
			__rec_incr(session, r, value_len, 1);
			break;
		case EMPTY_DATA:
			break;
		}

leaf_insert:	/* Write any K/V pairs inserted into the page after this key. */
		if ((ins = WT_ROW_INSERT(page, rip)) != NULL)
			WT_RET(__rec_row_leaf_insert(session, ins));
	}

	/* Free any allocated memory. */
	if (value_buf.mem != NULL)
		__wt_buf_free(session, &value_buf);

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_row_leaf_del --
 *	Walk a deleted row-store leaf page and schedule any overflow items
 *	for discard.
 */
static int
__rec_row_leaf_del(SESSION *session, WT_PAGE *page)
{
	WT_CELL *key_cell;
	WT_ROW *rip;
	uint32_t i;
	void *ripvalue;

	/*
	 * We're deleting the page, which means any overflow item we ever had
	 * was deleted.
	 *
	 * We have to walk both the WT_ROW structures and the original page --
	 * see the comment at WT_ROW_AND_KEY_FOREACH for details.
	 *
	 * For each entry in the in-memory page...
	 */
	WT_ROW_AND_KEY_FOREACH(page, rip, key_cell, i) {
		if (WT_CELL_TYPE(key_cell) == WT_CELL_KEY_OVFL)
			WT_RET(__rec_discard_add_ovfl(session,
			    WT_CELL_BYTE_OVFL(key_cell)));
		if (!WT_ROW_EMPTY_ISSET(rip)) {
			ripvalue = WT_ROW_PTR(page, rip);
			if (WT_CELL_TYPE(ripvalue) == WT_CELL_DATA_OVFL)
				WT_RET(__rec_discard_add_ovfl(session,
				    WT_CELL_BYTE_OVFL(ripvalue)));
		}
	}

	return (0);
}

/*
 * __rec_row_leaf_insert --
 *	Walk an insert chain, writing K/V pairs.
 */
static int
__rec_row_leaf_insert(SESSION *session, WT_INSERT *ins)
{
	WT_BUF key_buf, value_buf;
	WT_CELL key_cell, value_cell;
	WT_OVFL key_ovfl, value_ovfl;
	WT_RECONCILE *r;
	WT_UPDATE *upd;
	uint32_t key_len, value_len;

	WT_CLEAR(key_buf);
	WT_CLEAR(value_buf);
	r = S2C(session)->cache->rec;

	for (; ins != NULL; ins = ins->next) {
		/* Build a value to store on the page. */
		upd = ins->upd;
		if (WT_UPDATE_DELETED_ISSET(upd))
			continue;
		if (upd->size == 0)
			value_len = 0;
		else {
			value_buf.data = WT_UPDATE_DATA(upd);
			value_buf.size = upd->size;
			WT_RET(__rec_cell_build_value(
			    session, &value_buf, &value_cell, &value_ovfl));
			value_len = WT_CELL_SPACE_REQ(value_buf.size);
		}

		/* Build a key to store on the page. */
		key_buf.data = WT_INSERT_KEY(ins);
		key_buf.size = WT_INSERT_KEY_SIZE(ins);
		WT_RET(__rec_cell_build_key(
		    session, &key_buf, &key_cell, &key_ovfl));
		key_len = WT_CELL_SPACE_REQ(key_buf.size);

		/* Boundary: allocate, split or write the page. */
		while (key_len + value_len > r->space_avail)
			WT_RET(__rec_split(session));

		/* Copy the key cell into place. */
		memcpy(r->first_free, &key_cell, sizeof(WT_CELL));
		memcpy(r->first_free + sizeof(WT_CELL),
		    key_buf.data, key_buf.size);
		__rec_incr(session, r, key_len, 1);

		/* Copy the value cell into place. */
		if (value_len == 0)
			continue;
		memcpy(r->first_free, &value_cell, sizeof(WT_CELL));
		memcpy(r->first_free + sizeof(WT_CELL),
		    value_buf.data, value_buf.size);
		__rec_incr(session, r, value_len, 1);
	}

	/* Free any allocated memory. */
	if (key_buf.mem != NULL)
		__wt_buf_free(session, &key_buf);
	if (value_buf.mem != NULL)
		__wt_buf_free(session, &value_buf);

	return (0);
}

/*
 * __rec_wrapup  --
 *	Resolve the WT_RECONCILE information.
 */
static int
__rec_wrapup(SESSION *session, WT_PAGE *page)
{
	BTREE *btree;
	WT_PAGE *imp;
	WT_RECONCILE *r;

	btree = session->btree;
	r = S2C(session)->cache->rec;

	/*
	 * If the page was empty, we want to eventually discard it from the
	 * tree by merging it into its parent, not just evict it from memory.
	 */
	if (r->split_next == 0) {
		WT_VERBOSE(S2C(session), WT_VERB_EVICT, (session,
		    "reconcile: delete page %lu (%luB)",
		    (u_long)WT_PADDR(page), (u_long)WT_PSIZE(page)));
		WT_STAT_INCR(btree->stats, page_delete);

		/*
		 * Deleted pages cannot be evicted; we're going to evict the
		 * file blocks and it's possible for a thread to traverse into
		 * them before we reconcile their parent.  That's a big problem
		 * we don't want to solve, so keep the page around, and if it is
		 * re-used before parent reconciliation, that's OK, we reconcile
		 * it again.
		 */
		F_SET(page, WT_PAGE_DELETED);

		/*
		 * XXX
		 * I'm pretty sure we can empty the entire tree and arrive here
		 * deleting the root page, and that's not OK: ignore for now.
		 */
		WT_ASSERT(session, !WT_PAGE_IS_ROOT(page));

		/*
		 * We're not going to reconcile this page after all -- release
		 * our exclusive reference to it, as well as any pages in its
		 * subtree that we locked down.
		 */
		if (!r->locked) {
			page->parent_ref->state = WT_REF_MEM;

			switch (page->type) {
			case WT_PAGE_COL_INT:
				__rec_subtree_col_clear(session, page);
				break;
			case WT_PAGE_ROW_INT:
				__rec_subtree_row_clear(session, page);
				break;
			default:
				break;
			}
		}

		/* The parent is dirty. */
		WT_PAGE_SET_MODIFIED(page->parent);

		return (0);
	}

	/*
	 * We're replacing a page's on-disk image with one or more on-disk
	 * images.  If the page is not being discarded from memory and
	 * references in-memory subtrees, we must reconnect its in-memory
	 * references to the existing pages.  Sort the original page list of
	 * references so it's easily searchable.
	 */
	r->imref_found = 0;
	if (r->imref_next != 0)
		qsort(r->imref, (size_t)r->imref_next,
		    sizeof(WT_REF *), __rec_imref_qsort_cmp);

	/*
	 * Because WiredTiger's pages grow without splitting, we're replacing a
	 * single page with another single page most of the time.
	 */
	if (r->split_next == 1) {
		WT_VERBOSE(S2C(session), WT_VERB_EVICT, (session,
		    "reconcile: move %lu to %lu, (%luB to %luB)",
		    (u_long)WT_PADDR(page), (u_long)r->split[0].off.addr,
		    (u_long)WT_PSIZE(page), (u_long)r->split[0].off.size));

		/* Queue the original page to be discarded, we're done. */
		WT_RET(__rec_discard_add_page(session, page));

		/*
		 * Update the page's parent reference -- when we created it,
		 * we didn't know if it would be part of a split or not, so
		 * we didn't know what page would be its parent.
		 *
		 * If we're evicting this page, the new state is "on-disk",
		 * else, in-memory.
		 *
		 * Make sure we do not free the page twice on error.
		 */
		if ((imp = r->split[0].imp) != NULL) {
			r->split[0].imp = NULL;
			imp->parent = page->parent;
			imp->parent_ref = page->parent_ref;
			if (r->imref_next != 0)
				WT_RET(__rec_imref_fixup(session, imp));
		}

		WT_RET(__rec_parent_update(session, page,
		    imp, r->split[0].off.addr, r->split[0].off.size,
		    r->evict ? WT_REF_DISK : WT_REF_MEM));

		/* We're done, discard everything we've queued for discard. */
		WT_RET(__rec_discard_evict(session));
		return (0);
	}

	/*
	 * A page grew so large we had to divide it into two or more physical
	 * pages -- create a new internal page.
	 */
	WT_VERBOSE(S2C(session), WT_VERB_EVICT,
	    (session, "reconcile: %lu (%luB) splitting",
	    (u_long)WT_PADDR(page), (u_long)WT_PSIZE(page)));

	switch (page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		WT_STAT_INCR(btree->stats, split_intl);
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_LEAF:
		WT_STAT_INCR(btree->stats, split_leaf);
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	switch (page->type) {
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__rec_row_split(session, &imp, page));
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		WT_RET(__rec_col_split(session, &imp, page));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/* Queue the original page to be discarded, we're done. */
	WT_RET(__rec_discard_add_page(session, page));

	/* Update the parent to reference the newly created internal page. */
	WT_RET(__rec_parent_update(
	    session, page, imp, WT_ADDR_INVALID, 0, WT_REF_MEM));

	/* We're done, discard everything we've queued for discard. */
	WT_RET(__rec_discard_evict(session));

	return (0);
}

/*
 * __rec_parent_update_clean  --
 *	Update a parent page's reference for an evicted, clean page.
 */
static void
__rec_parent_update_clean(SESSION *session, WT_PAGE *page)
{
	WT_REF *parent_ref;

	parent_ref = page->parent_ref;

	/*
	 * If a page is on disk, it must have a valid disk address -- with
	 * one exception: if you create a root page and never use it, then
	 * it won't have a disk address.
	 */
	WT_ASSERT(session,
	    WT_PAGE_IS_ROOT(page) || parent_ref->addr != WT_ADDR_INVALID);

	/*
	 * Update the relevant WT_REF structure; no memory flush is needed,
	 * the state field is declared volatile.
	 */
	parent_ref->page = NULL;
	parent_ref->state = WT_REF_DISK;
}

/*
 * __rec_parent_update  --
 *	Update a parent page's reference to a reconciled page.
 */
static int
__rec_parent_update(SESSION *session, WT_PAGE *page,
    WT_PAGE *replace, uint32_t addr, uint32_t size, uint32_t state)
{
	WT_REF *parent_ref;

	/*
	 * Update the relevant parent WT_REF structure, flush memory, and then
	 * update the state of the parent reference.  No further memory flush
	 * needed, the state field is declared volatile.
	 *
	 * If we're replacing a valid addr/size pair, free the original disk
	 * blocks, they're no longer in use.
	 */
	parent_ref = page->parent_ref;
	parent_ref->page = replace;
	if (replace != NULL) {
		WT_ASSERT(session, page->parent == replace->parent);
		WT_ASSERT(session, page->parent_ref == replace->parent_ref);
	}
	if (parent_ref->addr != WT_ADDR_INVALID)
		WT_RET(__wt_block_free(
		    session, parent_ref->addr, parent_ref->size));
	parent_ref->addr = addr;
	parent_ref->size = size;
	WT_MEMORY_FLUSH;
	parent_ref->state = state;

	/*
	 * If it's not the root page, mark the parent page dirty.
	 *
	 * There's no chance we need to flush this write -- the eviction thread
	 * is the only thread that eventually cares if the page is dirty or not,
	 * and it's our update that's making it dirty.   (The workQ thread does
	 * have to flush its set-page-modified update, of course).
	 *
	 * We don't care if we race with the workQ; if the workQ thread races
	 * with us, the page will still be marked dirty and that's all we care
	 * about.
	 */
	if (!WT_PAGE_IS_ROOT(page))
		WT_PAGE_SET_MODIFIED(page->parent);

	return (0);
}

/*
 * __rec_cell_build_key --
 *	Process an inserted key item and return an WT_CELL structure and byte
 *	string to be stored on the page.
 */
static int
__rec_cell_build_key(
    SESSION *session, WT_BUF *key, WT_CELL *cell, WT_OVFL *ovfl)
{
	BTREE *btree;
	uint32_t orig_size;

	btree = session->btree;

	WT_CELL_CLEAR(cell);

	/*
	 * We're called with a WT_BUF that references a data/size pair.  We can
	 * re-point that WT_BUF's data and size fields to other memory, and if
	 * we allocate memory in that WT_BUF, the caller must free it.
	 *
	 * Optionally compress the value using the Huffman engine.  For Huffman-
	 * encoded key/data cells, we need additional memory; use the SESSION
	 * key/value return memory: this routine is called during bulk insert
	 * and reconciliation, we aren't returning key/data pairs.
	 */
	if (btree->huffman_key != NULL) {
		orig_size = key->size;
		WT_RET(__wt_huffman_encode(
		    btree->huffman_key, key->data, orig_size, key));
		if (key->size > orig_size)
			WT_STAT_INCRV(btree->stats,
			    huffman_key, key->size - orig_size);
	}

	/* Create an overflow object if the data won't fit. */
	if (key->size > btree->leafitemsize) {
		WT_RET(__rec_cell_build_ovfl(session, key, ovfl));

		key->data = ovfl;
		key->size = sizeof(*ovfl);
		WT_CELL_SET(cell, WT_CELL_KEY_OVFL, key->size);
		WT_STAT_INCR(btree->stats, overflow_key);
	} else
		WT_CELL_SET(cell, WT_CELL_KEY, key->size);
	return (0);
}

/*
 * __rec_cell_build_value --
 *	Process an inserted data item and return an WT_CELL structure and byte
 *	string to be stored on the page.
 */
static int
__rec_cell_build_value(
    SESSION *session, WT_BUF *value, WT_CELL *cell, WT_OVFL *ovfl)
{
	BTREE *btree;
	uint32_t orig_size;

	btree = session->btree;

	WT_CELL_CLEAR(cell);

	/*
	 * We're called with a WT_BUF that references a data/size pair.  We can
	 * re-point that WT_BUF's data and size fields to other memory, and if
	 * we allocate memory in that WT_BUF, the caller must free it.
	 *
	 * Optionally compress the value using the Huffman engine.  For Huffman-
	 * encoded key/data cells, we need additional memory; use the SESSION
	 * key/value return memory: this routine is called during bulk insert
	 * and reconciliation, we aren't returning key/data pairs.
	 */
	WT_CELL_SET_TYPE(cell, WT_CELL_DATA);

	/*
	 * Handle zero-length cells quickly -- this is a common value, it's
	 * a deleted column-store variable length cell.
	 */
	if (value->size == 0) {
		WT_CELL_SET_LEN(cell, 0);
		return (0);
	}

	/* Optionally compress the data using the Huffman engine. */
	if (btree->huffman_value != NULL) {
		orig_size = value->size;
		WT_RET(__wt_huffman_encode(btree->huffman_value,
		    value->data, orig_size, value));
		if (value->size > orig_size)
			WT_STAT_INCRV(btree->stats,
			    huffman_value, value->size - orig_size);
	}

	/* Create an overflow object if the data won't fit. */
	if (value->size > btree->leafitemsize) {
		WT_RET(__rec_cell_build_ovfl(session, value, ovfl));

		value->data = ovfl;
		value->size = sizeof(*ovfl);
		WT_CELL_SET_TYPE(cell, WT_CELL_DATA_OVFL);
		WT_STAT_INCR(btree->stats, overflow_data);
	}

	WT_CELL_SET_LEN(cell, value->size);
	return (0);
}

/*
 * __rec_cell_build_ovfl --
 *	Store bulk-loaded overflow items in the file, returning the WT_OVFL.
 */
static int
__rec_cell_build_ovfl(SESSION *session, WT_BUF *buf, WT_OVFL *to)
{
	WT_BUF *tmp;
	WT_PAGE_DISK *dsk;
	uint32_t addr, size;
	int ret;

	tmp = NULL;
	ret = 0;

	/*
	 * Allocate a scratch buffer and make sure it's big enough to hold the
	 * overflow chunk.  Clear the memory so it's never random bytes.
	 */
	size = WT_DISK_REQUIRED(session, buf->size);
	WT_ERR(__wt_scr_alloc(session, size, &tmp));
	memset(tmp->mem, 0, size);

	/*
	 * Set up the chunk and allocate a file address.  We're the only user
	 * of the file, which means we can grab file space whenever we want.
	 */
	dsk = tmp->mem;
	dsk->type = WT_PAGE_OVFL;
	dsk->u.datalen = buf->size;
	memcpy(WT_PAGE_DISK_BYTE(dsk), buf->data, buf->size);
	WT_ERR(__wt_block_alloc(session, &addr, size));

	/* Fill in the return information. */
	to->addr = addr;
	to->size = buf->size;

	ret = __wt_disk_write(session, dsk, addr, size);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __rec_row_split --
 *	Update a row-store parent page's reference when a page is split.
 */
static int
__rec_row_split(SESSION *session, WT_PAGE **splitp, WT_PAGE *orig)
{
	WT_PAGE *imp, *page;
	WT_RECONCILE *r;
	WT_ROW_REF *rref;
	WT_SPLIT *spl;
	uint32_t i, size;
	int ret;
	void *key;

	r = S2C(session)->cache->rec;
	ret = 0;

	/* Allocate a row-store internal page. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc_def(
	    session, (size_t)r->split_next, &page->u.row_int.t));

	/* Fill it in. */
	page->parent = orig->parent;
	page->parent_ref = orig->parent_ref;
	page->read_gen = __wt_cache_read_gen(session);
	page->entries = r->split_next;
	page->type = WT_PAGE_ROW_INT;
	WT_PAGE_SET_MODIFIED(page);
	/*
	 * Newly created internal pages are not persistent as we don't want the
	 * tree to deepen whenever a leaf page splits.  Flag the page for merge
	 * into its parent when the parent is reconciled.
	 */
	F_SET(page, WT_PAGE_SPLIT);

	/* Enter each split page into the new, internal page. */
	for (rref = page->u.row_int.t,
	    spl = r->split, i = 0; i < r->split_next; ++rref, ++spl, ++i) {
		/*
		 * Steal the split buffer's pointer -- we could allocate and
		 * copy here, but that means split buffers would potentially
		 * grow without bound, this way we do the same number of
		 * memory allocations and the split buffers don't just keep
		 * getting bigger.
		 */
		__wt_buf_steal(session, &spl->key, &key, &size);
		__wt_key_set(rref, key, size);
		WT_ROW_REF_ADDR(rref) = spl->off.addr;
		WT_ROW_REF_SIZE(rref) = spl->off.size;

		/*
		 * If there's an in-memory version of the page, connect it to
		 * this parent and fix up its in-memory references.
		 */
		if ((imp = spl->imp) == NULL) {
			WT_ROW_REF_PAGE(rref) = NULL;
			WT_ROW_REF_STATE(rref) = WT_REF_DISK;
		} else {
			imp->parent = page;
			imp->parent_ref = &rref->ref;
			WT_ROW_REF_PAGE(rref) = imp;
			WT_ROW_REF_STATE(rref) = WT_REF_MEM;

			/*
			 * The created in-memory page is hooked into the new
			 * page, don't free it on error.
			 */
			spl->imp = NULL;

			/*
			 * If we've found all the in-memory references we expect
			 * to find, don't keep searching.
			 */
			if (r->imref_found < r->imref_next)
				WT_RET(__rec_imref_fixup(session, imp));
		}

		WT_VERBOSE(S2C(session), WT_VERB_EVICT, (session,
		    "split: %lu (%luB)",
		    (u_long)spl->off.addr, (u_long)spl->off.size));
	}

	/*
	 * We should have connected the same number of in-memory references as
	 * we found originally.
	 */
	WT_ASSERT(session, r->imref_found == r->imref_next);

	*splitp = page;
	return (0);

err:	__wt_free(session, page);
	return (ret);
}

/*
 * __rec_col_split --
 *	Update a column-store parent page's reference when a page is split.
 */
static int
__rec_col_split(SESSION *session, WT_PAGE **splitp, WT_PAGE *orig)
{
	WT_COL_REF *cref;
	WT_OFF_RECORD *off;
	WT_PAGE *imp, *page;
	WT_RECONCILE *r;
	WT_SPLIT *spl;
	uint32_t i;
	int ret;

	r = S2C(session)->cache->rec;
	ret = 0;

	/* Allocate a column-store internal page. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc_def(
	    session, (size_t)r->split_next, &page->u.col_int.t));

	/* Fill it in. */
	page->parent = orig->parent;
	page->parent_ref = orig->parent_ref;
	page->read_gen = __wt_cache_read_gen(session);
	page->u.col_int.recno = WT_RECNO(&r->split->off);
	page->entries = r->split_next;
	page->type = WT_PAGE_COL_INT;
	WT_PAGE_SET_MODIFIED(page);
	/*
	 * Newly created internal pages are not persistent as we don't want the
	 * tree to deepen whenever a leaf page splits.  Flag the page for merge
	 * into its parent when the parent is reconciled.
	 */
	F_SET(page, WT_PAGE_SPLIT);

	/* Enter each split page into the new, internal page. */
	for (cref = page->u.col_int.t,
	    spl = r->split, i = 0; i < r->split_next; ++cref, ++spl, ++i) {
		off = &spl->off;
		cref->recno = WT_RECNO(off);
		WT_COL_REF_ADDR(cref) = off->addr;
		WT_COL_REF_SIZE(cref) = off->size;

		/*
		 * If there's an in-memory version of the page, connect it to
		 * this parent and fix up its in-memory references.
		 */
		if ((imp = spl->imp) == NULL) {
			WT_COL_REF_PAGE(cref) = NULL;
			WT_COL_REF_STATE(cref) = WT_REF_DISK;
		} else {
			imp->parent = page;
			imp->parent_ref = &cref->ref;
			WT_COL_REF_PAGE(cref) = imp;
			WT_COL_REF_STATE(cref) = WT_REF_MEM;

			/*
			 * The created in-memory page is hooked into the new
			 * page, don't free it on error.
			 */
			spl->imp = NULL;

			/*
			 * If we've found all the in-memory references we expect
			 * to find, don't keep searching.
			 */
			if (r->imref_found < r->imref_next)
				WT_RET(__rec_imref_fixup(session, imp));
		}

		WT_VERBOSE(S2C(session), WT_VERB_EVICT, (session,
		    "split: %lu (%luB), starting record %llu",
		    (u_long)off->addr, (u_long)off->size,
		    (unsigned long long)WT_RECNO(&spl->off)));
	}

	/*
	 * We should have connected the same number of in-memory references as
	 * we found originally.
	 */
	WT_ASSERT(session, r->imref_found == r->imref_next);

	*splitp = page;
	return (0);

err:	__wt_free(session, page);
	return (ret);
}

/*
 * __rec_imref_qsort_cmp --
 *	Qsort sort function for the in-memory subtree list.
 */
static int
__rec_imref_qsort_cmp(const void *a, const void *b)
{
	uint32_t a_addr, b_addr;

	/* Sort the addr in ascending order. */
	a_addr = (*(WT_REF **)a)->addr;
	b_addr = (*(WT_REF **)b)->addr;
	return (a_addr > b_addr ? 1 : (a_addr < b_addr ? -1 : 0));
}

/*
 * __rec_imref_bsearch_cmp --
 *	Bsearch comparison routine for the in-memory subtree list.
 */
static int
__rec_imref_bsearch_cmp(const void *a, const void *b)
{
	WT_REF *search, *entry;

	search = (WT_REF *)a;
	entry = *(WT_REF **)b;

	return (search->addr > entry->addr ? 1 :
	    ((search->addr < entry->addr) ? -1 : 0));
}

/*
 * __rec_imref_fixup --
 *	Fix up after splitting a page referencing an in-memory subtree.
 */
static int
__rec_imref_fixup(SESSION *session, WT_PAGE *page)
{
	WT_COL_REF *cref;
	WT_RECONCILE *r;
	WT_REF **searchp, *ref;
	WT_ROW_REF *rref;
	uint32_t i;

	r = S2C(session)->cache->rec;

	/*
	 * Walk the page we just created, and for any addr that appears, check
	 * if the slot references an in-memory entry from the original page.
	 * For each connection we find, link the pages together, both parent to
	 * child and child to parent.
	 */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		WT_COL_REF_FOREACH(page, cref, i)
			if ((searchp = bsearch(&cref->ref, r->imref,
			    r->imref_next, sizeof(WT_REF *),
			    __rec_imref_bsearch_cmp)) != NULL) {
				ref = *searchp;
				ref->page->parent = page;
				ref->page->parent_ref = &cref->ref;
				cref->ref = *ref;
				if (++r->imref_found == r->imref_next)
					break;
			}
		break;
	case WT_PAGE_ROW_INT:
		WT_ROW_REF_FOREACH(page, rref, i)
			if ((searchp = bsearch(&rref->ref, r->imref,
			    r->imref_next, sizeof(WT_REF *),
			    __rec_imref_bsearch_cmp)) != NULL) {
				ref = *searchp;
				ref->page->parent = page;
				ref->page->parent_ref = &rref->ref;
				rref->ref = *ref;
				if (++r->imref_found == r->imref_next)
					break;
			}
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	return (0);
}

/*
 * __rec_imref_init --
 *	Initialize the list of in-memory subtree references.
 */
static void
__rec_imref_init(WT_RECONCILE *r)
{
	r->imref_next = 0;
}

/*
 * __rec_imref_add --
 *	Append a new reference to the list of in-memory subtree references.
 */
static int
__rec_imref_add(SESSION *session, WT_REF *ref)
{
	WT_RECONCILE *r;

	r = S2C(session)->cache->rec;

	if (r->imref_next == r->imref_entries) {
		WT_RET(__wt_realloc(session, &r->imref_allocated,
		    (r->imref_entries + 20) * sizeof(*r->imref),
		    &r->imref));
		r->imref_entries += 20;
	}
	r->imref[r->imref_next++] = ref;
	return (0);
}

/*
 * __rec_discard_init --
 *	Initialize the list of discard objects.
 */
static void
__rec_discard_init(WT_RECONCILE *r)
{
	r->discard_next = 0;
}

/*
 * __rec_discard_add --
 *	Append an object to the list of discard objects.
 */
static int
__rec_discard_add(SESSION *session, WT_PAGE *page, uint32_t addr, uint32_t size)
{
	WT_RECONCILE *r;

	r = S2C(session)->cache->rec;

	if (r->discard_next == r->discard_entries) {
		WT_RET(__wt_realloc(session, &r->discard_allocated,
		    (r->discard_entries + 20) * sizeof(*r->discard),
		    &r->discard));
		r->discard_entries += 20;
	}
	r->discard[r->discard_next].page = page;
	r->discard[r->discard_next].addr = addr;
	r->discard[r->discard_next].size = size;
	++r->discard_next;
	return (0);
}

/*
 * __rec_discard_evict --
 *	Discard the list of discard objects.
 */
static int
__rec_discard_evict(SESSION *session)
{
	WT_DISCARD *discard;
	WT_RECONCILE *r;
	uint32_t i;

	r = S2C(session)->cache->rec;

	for (discard = r->discard, i = 0; i < r->discard_next; ++discard, ++i)
		if (discard->page == NULL)
			WT_RET(__wt_block_free(
			    session, discard->addr, discard->size));
		else
			__wt_page_free(session,
			    discard->page, discard->addr, discard->size);
	return (0);
}

/*
 * __hazard_qsort_cmp --
 *	Qsort function: sort hazard list based on the page's address.
 */
static int
__hazard_qsort_cmp(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;

	a_page = ((WT_HAZARD *)a)->page;
	b_page = ((WT_HAZARD *)b)->page;

	return (a_page > b_page ? 1 : (a_page < b_page ? -1 : 0));
}

/*
 * __hazard_copy --
 *	Copy the hazard array and prepare it for searching.
 */
static void
__hazard_copy(SESSION *session)
{
	CONNECTION *conn;
	WT_RECONCILE *r;
	uint32_t elem, i, j;

	conn = S2C(session);
	r = S2C(session)->cache->rec;

	/* Copy the list of hazard references, compacting it as we go. */
	elem = conn->session_size * conn->hazard_size;
	for (i = j = 0; j < elem; ++j) {
		if (conn->hazard[j].page == NULL)
			continue;
		r->hazard[i] = conn->hazard[j];
		++i;
	}
	elem = i;

	/* Sort the list by page address. */
	qsort(r->hazard, (size_t)elem, sizeof(WT_HAZARD), __hazard_qsort_cmp);

	r->hazard_elem = elem;
}

/*
 * __hazard_bsearch_cmp --
 *	Bsearch function: search sorted hazard list.
 */
static int
__hazard_bsearch_cmp(const void *search, const void *b)
{
	void *entry;

	entry = ((WT_HAZARD *)b)->page;

	return (search > entry ? 1 : ((search < entry) ? -1 : 0));
}

/*
 * __hazard_exclusive --
 *	Request exclusive access to a page.
 */
static int
__hazard_exclusive(SESSION *session, WT_REF *ref)
{
	WT_RECONCILE *r;

	r = S2C(session)->cache->rec;

	/*
	 * Hazard references are acquired down the tree, which means we can't
	 * deadlock.
	 *
	 * Request exclusive access to the page; no memory flush needed, the
	 * state field is declared volatile.
	 */
	ref->state = WT_REF_LOCKED;

	/* Get a fresh copy of the hazard reference array. */
	__hazard_copy(session);

	/* If we find a matching hazard reference, the page is still in use. */
	if (bsearch(ref->page, r->hazard, r->hazard_elem,
	    sizeof(WT_HAZARD), __hazard_bsearch_cmp) == NULL)
		return (0);

	/* Return the page to in-use. */
	ref->state = WT_REF_MEM;

	return (1);
}
