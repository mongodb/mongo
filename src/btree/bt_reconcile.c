/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"
#include "cell.i"

struct __rec_boundary;		typedef struct __rec_boundary WT_BOUNDARY;
struct __rec_discard;		typedef struct __rec_discard WT_DISCARD;
struct __rec_kv;		typedef struct __rec_kv WT_KV;

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
	int salvage;			/* Called from salvage */

	int	 btree_split_min;	/* use tiny split sizes (debugging) */
	uint32_t btree_split_pct;	/* Split page percent */

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

		WT_OFF off;		/* Split's written addr/size pair */

		/*
		 * The key for a row-store page; no column-store key is needed
		 * because the page's recno, stored in the recno field, is the
		 * column-store key.
		 */
		WT_BUF key;		/* Promoted row-store key */

		/*
		 * If we're creating a new, in-memory version of the reconciled
		 * page, we'll save it here for connection between the newly
		 * created split page and previously existing in-memory pages.
		 */
		WT_PAGE_DISK *imp_dsk;	/* New disk image */
	} *bnd;				/* Saved boundaries */
	uint32_t bnd_next;		/* Next boundary slot */
	uint32_t bnd_entries;		/* Total boundary slots */
	uint32_t bnd_allocated;		/* Bytes allocated */

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
	 * are packaged here rather than passing pointer to stack locations
	 * through the code.
	 */
	uint64_t recno;			/* Current record number */
	uint32_t entries;		/* Current number of entries */
	uint8_t *first_free;		/* Current first free byte */
	uint32_t space_avail;		/* Remaining space in this chunk */

	/*
	 * If reconciling an in-memory page we're not evicting from memory, we
	 * may split a page referencing an in-memory subtree.  In the same way
	 * we create a set of new disk images that reflect the split of the
	 * original page's disk image, we have to create a set of new in-memory
	 * images reflecting the split of the original page's in-memory image.
	 * Once that's done, the new in-memory image must reference the same
	 * subtrees as the old in-memory image; we maintain a list of in-memory
	 * subtrees the original page referenced to make that fixup possible.
	 *
	 * We obviously could maintain this list on a per-split-chunk basis:
	 * copy it into the WT_BOUNDARY structure if the boundary is reached.
	 * We're doing a binary search of this array during fixup and I don't
	 * expect it to be large.
	 */
	WT_REF **imref;			/* In-memory subtree reference list */
	uint32_t imref_next;		/* Next list slot */
	uint32_t imref_entries;		/* Total list slots */
	uint32_t imref_allocated;	/* Bytes allocated */
	uint32_t imref_found;		/* Fast check for search */

	int	    cell_zero;		/* Row-store internal page 0th key */
	WT_ROW_REF *merge_ref;		/* Row-store merge correction key */

	WT_HAZARD  *hazard;		/* Copy of the hazard references */
	uint32_t    hazard_elem;	/* Number of entries in the list */

	/*
	 * WT_KV--
	 *	An on-page key/value item we're building.
	 */
	struct __rec_kv {
		WT_BUF	 buf;		/* Data */
		WT_CELL	 cell;		/* Cell and cell's length */
		uint32_t cell_len;
		uint32_t len;		/* Total length of cell + data */

		WT_OFF	 off;		/* Associated off-page value */
	} k, v;				/* Key/Value being built */

	WT_BUF *full, _full;		/* Full-key being built */
	WT_BUF *last, _last;		/* Last full-key built */

	int	key_pfx_compress;	/* If can prefix-compress next key */
	int     key_pfx_compress_conf;	/* If prefix compression configured */
	int	key_sfx_compress;	/* If can suffix-compress next key */
	int     key_sfx_compress_conf;	/* If suffix compression configured */
} WT_RECONCILE;

#undef	STATIN
#define	STATIN	static inline

static int  __hazard_bsearch_cmp(const void *, const void *);
static void __hazard_copy(WT_SESSION_IMPL *);
static int  __hazard_exclusive(WT_SESSION_IMPL *, WT_REF *);
static int  __hazard_qsort_cmp(const void *, const void *);
STATIN void __rec_cell_build_deleted(WT_KV *);
static int  __rec_cell_build_key(WT_SESSION_IMPL *, const void *, uint32_t);
static int  __rec_cell_build_ovfl(WT_SESSION_IMPL *, WT_KV *, u_int);
static int  __rec_cell_build_val(WT_SESSION_IMPL *, void *, uint32_t);
static void __rec_col_extend_truncate(WT_PAGE *);
static int  __rec_col_fix(WT_SESSION_IMPL *, WT_PAGE *, uint64_t);
static int  __rec_col_fix_bulk(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_col_int(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_col_merge(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_col_rle(WT_SESSION_IMPL *, WT_PAGE *, uint64_t);
static int  __rec_col_rle_bulk(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_col_split(WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE **);
static int  __rec_col_var(WT_SESSION_IMPL *, WT_PAGE *, uint64_t);
static int  __rec_col_var_bulk(WT_SESSION_IMPL *, WT_PAGE *);
STATIN void __rec_copy_incr(WT_SESSION_IMPL *, WT_RECONCILE *, WT_KV *);
static int  __rec_discard_add(WT_SESSION_IMPL *, WT_PAGE *, uint32_t, uint32_t);
STATIN int  __rec_discard_add_ovfl(WT_SESSION_IMPL *, WT_CELL *);
static int  __rec_discard_evict(WT_SESSION_IMPL *);
static void __rec_discard_init(WT_RECONCILE *);
static int  __rec_imref_add(WT_SESSION_IMPL *, WT_REF *);
static int  __rec_imref_bsearch_cmp(const void *, const void *);
static int  __rec_imref_fixup(WT_SESSION_IMPL *,
		WT_BOUNDARY *, WT_PAGE *, WT_REF *, WT_PAGE **);
static void __rec_imref_init(WT_RECONCILE *);
static int  __rec_imref_qsort_cmp(const void *, const void *);
STATIN void __rec_incr(WT_SESSION_IMPL *, WT_RECONCILE *, uint32_t);
static int  __rec_init(WT_SESSION_IMPL *, uint32_t);
STATIN void __rec_key_state_update(WT_RECONCILE *, int);
static int  __rec_ovfl_delete(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_parent_update(WT_SESSION_IMPL *,
		WT_PAGE *, WT_PAGE *, uint32_t, uint32_t, uint32_t);
static void __rec_parent_update_clean(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_row_int(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_row_leaf(WT_SESSION_IMPL *, WT_PAGE *, uint64_t);
static int  __rec_row_leaf_insert(WT_SESSION_IMPL *, WT_INSERT *);
static int  __rec_row_merge(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_row_split(WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE **);
static int  __rec_split(WT_SESSION_IMPL *);
STATIN int  __rec_split_bnd_grow(WT_SESSION_IMPL *);
STATIN void __rec_split_buf_prep(WT_SESSION_IMPL *, WT_BUF *, uint32_t *);
static int  __rec_split_finish(WT_SESSION_IMPL *);
static int  __rec_split_fixup(WT_SESSION_IMPL *);
static int  __rec_split_init(
		WT_SESSION_IMPL *, WT_PAGE *, uint64_t, uint32_t, uint32_t);
static int  __rec_split_row_promote(WT_SESSION_IMPL *, uint8_t);
static int  __rec_split_write(WT_SESSION_IMPL *, WT_BOUNDARY *, WT_BUF *);
static int  __rec_subtree(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_subtree_col(WT_SESSION_IMPL *, WT_PAGE *);
static void __rec_subtree_col_clear(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_subtree_row(WT_SESSION_IMPL *, WT_PAGE *);
static void __rec_subtree_row_clear(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_wrapup(WT_SESSION_IMPL *, WT_PAGE *);

/*
 * When discarding a page, we discard a number of things: pages merged into the
 * page, overflow items referenced by the page, and the underlying blocks.  It
 * is all one discard family, but there are 3 different interfaces.
 */
#define	__rec_discard_add_block(session, addr, size)			\
	__rec_discard_add(session, NULL, addr, size)
#define	__rec_discard_add_page(session, page)				\
	__rec_discard_add(session, page, WT_PADDR(page), WT_PSIZE(page))

/*
 * __wt_page_reconcile --
 *	Format an in-memory page to its on-disk format, and write it.
 */
int
__wt_page_reconcile_int(WT_SESSION_IMPL *session,
    WT_PAGE *page, uint64_t salvage_cookie, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	int ret;

	btree = session->btree;
	cache = S2C(session)->cache;
	ret = 0;

	WT_VERBOSE(session, EVICT,
	    "reconcile %s page addr %" PRIu32 " (type %s)",
	    WT_PAGE_IS_MODIFIED(page) ? "dirty" : "clean",
	    WT_PADDR(page), __wt_page_type_string(page->type));

	/*
	 * Handle pages marked for deletion or split.
	 *
	 * Check both before checking for clean pages: deleted and split pages
	 * operate outside the rules for normal pages.
	 *
	 * Pages have their WT_PAGE_DELETED flag set if they're reconciled and
	 * are found to have no valid entries.
	 *
	 * Pages have their WT_PAGE_MERGE flag set if they're being merged into
	 * their parent (for example, created as part of an internal split).
	 *
	 * For deleted or split pages, there's nothing to do, they are merged
	 * into their parent when their parent is reconciled.
	 */
	if (F_ISSET(page, WT_PAGE_DELETED | WT_PAGE_MERGE))
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
		else {
			__rec_col_extend_truncate(page);
			WT_RET(__rec_col_fix(session, page, salvage_cookie));
		}
		break;
	case WT_PAGE_COL_RLE:
		if (F_ISSET(page, WT_PAGE_BULK_LOAD))
			WT_RET(__rec_col_rle_bulk(session, page));
		else {
			__rec_col_extend_truncate(page);
			WT_RET(__rec_col_rle(session, page, salvage_cookie));
		}
		break;
	case WT_PAGE_COL_VAR:
		if (F_ISSET(page, WT_PAGE_BULK_LOAD))
			WT_RET(__rec_col_var_bulk(session, page));
		else {
			__rec_col_extend_truncate(page);
			WT_RET(__rec_col_var(session, page, salvage_cookie));
		}
		break;
	case WT_PAGE_COL_INT:
		WT_RET(__rec_col_int(session, page));
		break;
	case WT_PAGE_ROW_INT:
		WT_RET(__rec_row_int(session, page));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_RET(__rec_row_leaf(session, page, salvage_cookie));
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
	    F_ISSET(btree->root_page.page, WT_PAGE_MERGE)) {
		F_CLR(btree->root_page.page, WT_PAGE_MERGE);
		F_SET(btree->root_page.page, WT_PAGE_PINNED);
		WT_PAGE_SET_MODIFIED(btree->root_page.page);
		ret =
		    __wt_page_reconcile(session, btree->root_page.page, flags);
	}

	return (ret);
}

/*
 * __rec_init --
 *	Initialize the reconciliation structure.
 */
static int
__rec_init(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
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

		/* Connect prefix compression pointers/buffers. */
		r->full = &r->_full;
		r->last = &r->_last;

		/* Configuration. */
		WT_RET(__wt_config_getones(session,
		    session->btree->config, "split_min", &cval));
		if (cval.val != 0)
			r->btree_split_min = 1;

		WT_RET(__wt_config_getones(session,
		    session->btree->config, "split_pct", &cval));
		r->btree_split_pct = (uint32_t)cval.val;

		WT_RET(__wt_config_getones(session,
		    session->btree->config,
		    "internal_key_truncate", &cval));
		r->key_sfx_compress_conf = (cval.val != 0);

		WT_RET(__wt_config_getones(session,
		    session->btree->config,
		    "prefix_compression", &cval));
		r->key_pfx_compress_conf = (cval.val != 0);
	}

	r->evict = LF_ISSET(WT_REC_EVICT);
	r->locked = LF_ISSET(WT_REC_LOCKED);
	r->salvage = LF_ISSET(WT_REC_SALVAGE);

	/*
	 * During internal page reconciliation we track referenced objects that
	 * are discarded when the page is discarded.  That tracking is done per
	 * reconciliation run, initialize it before anything else.
	 */
	__rec_discard_init(r);

	/*
	 * During internal page reconciliation we track in-memory subtrees we
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
__wt_rec_destroy(WT_SESSION_IMPL *session)
{
	WT_BOUNDARY *bnd;
	WT_RECONCILE *r;
	uint32_t i;

	if ((r = S2C(session)->cache->rec) == NULL)
		return;

	__wt_buf_free(session, &r->dsk);

	if (r->discard != NULL)
		__wt_free(session, r->discard);

	if (r->bnd != NULL) {
		for (bnd = r->bnd, i = 0; i < r->bnd_entries; ++bnd, ++i) {
			__wt_buf_free(session, &bnd->key);
			if (bnd->imp_dsk != NULL)
				__wt_free(session, bnd->imp_dsk);
		}
		__wt_free(session, r->bnd);
	}

	if (r->imref != NULL)
		__wt_free(session, r->imref);

	if (r->hazard != NULL)
		__wt_free(session, r->hazard);

	__wt_buf_free(session, &r->k.buf);
	__wt_buf_free(session, &r->v.buf);
	__wt_buf_free(session, &r->_full);
	__wt_buf_free(session, &r->_last);

	__wt_free(session, S2C(session)->cache->rec);
}

/*
 * __rec_subtree --
 *	Get exclusive access to a subtree for reconciliation.
 */
static int
__rec_subtree(WT_SESSION_IMPL *session, WT_PAGE *page)
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
__rec_subtree_col(WT_SESSION_IMPL *session, WT_PAGE *parent)
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
		if (!F_ISSET(page, WT_PAGE_DELETED | WT_PAGE_MERGE)) {
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
		if (!F_ISSET(page, WT_PAGE_DELETED | WT_PAGE_MERGE))
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
				WT_RET(__rec_ovfl_delete(session, page));
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
__rec_subtree_col_clear(WT_SESSION_IMPL *session, WT_PAGE *parent)
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
__rec_subtree_row(WT_SESSION_IMPL *session, WT_PAGE *parent)
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
		if (!F_ISSET(page, WT_PAGE_DELETED | WT_PAGE_MERGE)) {
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
		if (!F_ISSET(page, WT_PAGE_DELETED | WT_PAGE_MERGE))
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
			case WT_PAGE_ROW_LEAF:
				WT_RET(__rec_ovfl_delete(session, page));
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
__rec_subtree_row_clear(WT_SESSION_IMPL *session, WT_PAGE *parent)
{
	WT_PAGE *page;
	WT_ROW_REF *rref;
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
 * __rec_ovfl_delete --
 *	Walk the cells of a deleted disk page and schedule any overflow items
 * for discard.
 */
static int
__rec_ovfl_delete(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CELL *cell;
	WT_PAGE_DISK *dsk;
	uint32_t i;

	dsk = page->dsk;

	/*
	 * For row-internal pages, the disk image was discarded because there
	 * were no overflow items.
	 */
	if (dsk == NULL)
		return (0);

	/*
	 * We're deleting the page, which means any overflow item we ever had
	 * is deleted as well.
	 */
	WT_CELL_FOREACH(dsk, cell, i)
		WT_RET(__rec_discard_add_ovfl(session, cell));

	return (0);
}

/*
 * __rec_copy_incr --
 *	Copy a key/value cell and buffer pair onto a page.
 */
static inline void
__rec_copy_incr(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_KV *kv)
{
	uint32_t len;
	uint8_t *p, *t;

	/*
	 * If there's only one chunk of data to copy (because the cell and data
	 * are being copied from the original disk page), the cell length won't
	 * be set, the WT_BUF data/length will reference the data to be copied.
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
	__rec_incr(session, r, kv->len);
}

/*
 * __rec_incr --
 *	Update the memory tracking structure for a new entry.
 */
static inline void
__rec_incr(WT_SESSION_IMPL *session, WT_RECONCILE *r, uint32_t size)
{
	/*
	 * The buffer code is fragile and prone to off-by-one errors --
	 * check for overflow in diagnostic mode.
	 */
	WT_ASSERT(session, r->space_avail >= size);
	WT_ASSERT(session, WT_PTRDIFF32(
	    r->first_free + size, r->dsk.mem) <= r->page_size);

	++r->entries;
	r->space_avail -= size;
	r->first_free += size;
}

/*
 * __rec_split_buf_prep --
 *	Get a page buffer ready for writing.
 */
static inline void
__rec_split_buf_prep(WT_SESSION_IMPL *session, WT_BUF *buf, uint32_t *sizep)
{
	WT_BTREE *btree;
	uint32_t alloc_len, current_len, write_len;

	btree = session->btree;

	/*
	 * Figure out the page's final size, and how many bytes need to be
	 * cleared at the end.
	 */
	current_len = buf->size;
	alloc_len = WT_ALIGN(current_len, btree->allocsize);
	write_len = alloc_len - current_len;

	/*
	 * There are lots of offset calculations going on in this code, make
	 * sure we don't overflow the end of the temporary buffer.
	 */
	WT_ASSERT(session, current_len + write_len <= buf->mem_size);

	if (write_len != 0)
		memset((uint8_t *)buf->mem + current_len, 0, write_len);

	/* Return the page's final size. */
	*sizep = alloc_len;
}

/*
 * __rec_split_bnd_grow --
 *	Grow the boundary array as necessary.
 */
static inline int
__rec_split_bnd_grow(WT_SESSION_IMPL *session)
{
	WT_RECONCILE *r;

	r = S2C(session)->cache->rec;

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
    WT_PAGE *page, uint64_t recno, uint32_t max, uint32_t min)
{
	WT_BTREE *btree;
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;

	r = S2C(session)->cache->rec;
	btree = session->btree;

						/* New page, compression off. */
	r->key_pfx_compress = r->key_sfx_compress = 0;

	/* Ensure the scratch buffer is large enough. */
	WT_RET(__wt_buf_initsize(session, &r->dsk, (size_t)max));

	/*
	 * Some fields of the disk image are fixed based on the original page,
	 * set them.
	 */
	dsk = r->dsk.mem;
	WT_CLEAR(*dsk);
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
	 */
	r->page_size = max;
	if (r->btree_split_min)
		r->split_size = min;
	else {
		r->split_size = WT_ALIGN(
		    (max * r->btree_split_pct) / 100, btree->allocsize);
		if (r->split_size < min)
			r->split_size = min;
	}

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
	WT_RET(__rec_split_bnd_grow(session));
	r->bnd[0].recno = recno;
	r->bnd[0].start = WT_PAGE_DISK_BYTE(dsk);

	/* Initialize the total entries. */
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
__rec_split(WT_SESSION_IMPL *session)
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
	switch (r->bnd_state) {
	case SPLIT_BOUNDARY:				/* Case #1 */
		/*
		 * Save the information about where we are when the split would
		 * have happened.
		 */
		WT_RET(__rec_split_bnd_grow(session));
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
			WT_RET(__rec_split_row_promote(session, dsk->type));
		bnd->entries = 0;

		/*
		 * Set the space available to another split-size chunk, if we
		 * have one.  If we don't have room for another split chunk,
		 * add whatever space remains in the maximum page size, and
		 * hope it's enough.
		 */
		current_len = WT_PTRDIFF32(r->first_free, dsk);
		if (current_len + r->split_size <= r->page_size)
			r->space_avail = r->split_size - WT_PAGE_DISK_SIZE;
		else {
			r->bnd_state = SPLIT_MAX;
			r->space_avail =
			    (r->page_size - WT_PAGE_DISK_SIZE) - current_len;
		}
		break;
	case SPLIT_MAX:					/* Case #2 */
		/*
		 * It didn't all fit into a single page.
		 *
		 * Cycle through the saved split-point information, writing the
		 * split chunks we have tracked.
		 */
		WT_RET(__rec_split_fixup(session));

		/* We're done saving split chunks. */
		r->bnd_state = SPLIT_TRACKING_OFF;
		break;
	case SPLIT_TRACKING_OFF:			/* Case #3 */
		WT_RET(__rec_split_bnd_grow(session));
		bnd = &r->bnd[r->bnd_next++];

		/*
		 * It didn't all fit, but either we've already noticed it and
		 * are now processing the rest of the page at the split-size
		 * boundaries, or the split size was the same as the page size,
		 * so we never bothered with saving split-point information.
		 *
		 * Write the current disk image.
		 */
		dsk->recno = bnd->recno;
		dsk->u.entries = r->entries;
		r->dsk.size = WT_PTRDIFF32(r->first_free, dsk);
		WT_RET(__rec_split_write(session, bnd, &r->dsk));

		/*
		 * Set the starting record number and promotion key for the next
		 * chunk, clear the entries (not required, but cleaner).
		 */
		++bnd;
		bnd->recno = r->recno;
		if (dsk->type == WT_PAGE_ROW_INT ||
		    dsk->type == WT_PAGE_ROW_LEAF)
			WT_RET(__rec_split_row_promote(session, dsk->type));
		bnd->entries = 0;

		/*
		 * Set the caller's entry count and buffer information for the
		 * next chunk.  We only get here if we're not splitting or have
		 * already split, so it's split-size chunks from here on out.
		 */
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
__rec_split_finish(WT_SESSION_IMPL *session)
{
	WT_BOUNDARY *bnd;
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;

	r = S2C(session)->cache->rec;

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
		WT_ASSERT(session, r->bnd_next == 0);
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
		WT_RET(__rec_split_bnd_grow(session));
		bnd = &r->bnd[r->bnd_next++];
	} else {
		r->bnd_next = 1;
		bnd = &r->bnd[0];
		bnd->entries = r->entries;
	}

	/* Write the remaining information. */
	dsk = r->dsk.mem;
	dsk->recno = bnd->recno;
	dsk->u.entries = r->entries;
	r->dsk.size = WT_PTRDIFF32(r->first_free, dsk);
	return (__rec_split_write(session, bnd, &r->dsk));
}

/*
 * __rec_split_fixup --
 *	Fix up after crossing the maximum page boundary.
 */
static int
__rec_split_fixup(WT_SESSION_IMPL *session)
{
	WT_BOUNDARY *bnd;
	WT_BUF *tmp;
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;
	uint32_t i, len;
	uint8_t *dsk_start;
	int ret;

	/*
	 * When we overflow physical limits of the page, we walk the list of
	 * split chunks we've created and write those pages out, then update
	 * the caller's information.
	 */
	r = S2C(session)->cache->rec;
	tmp = NULL;
	ret = 0;

	/*
	 * The data isn't laid out on a page boundary or nul padded; copy it to
	 * a clean, aligned, padded buffer before writing it.
	 *
	 * Allocate a scratch buffer to hold the new disk image.  Copy the
	 * WT_PAGE_DISK header onto the scratch buffer, most of the header
	 * information remains unchanged between the pages.
	 */
	WT_RET(__wt_scr_alloc(session, r->split_size, &tmp));
	dsk = tmp->mem;
	dsk_start = WT_PAGE_DISK_BYTE(dsk);
	memcpy(dsk, r->dsk.mem, WT_PAGE_DISK_SIZE);

	/*
	 * For each split chunk we've created, update the disk image and copy
	 * it into place.
	 */
	for (i = 0, bnd = r->bnd; i < r->bnd_next; ++i, ++bnd) {
		/* Copy the page contents to the temporary buffer. */
		len = WT_PTRDIFF32((bnd + 1)->start, bnd->start);
		memcpy(dsk_start, bnd->start, len);

		/* Write the page. */
		dsk->recno = bnd->recno;
		dsk->u.entries = bnd->entries;
		tmp->size = WT_PAGE_DISK_SIZE + len;
		WT_ERR(__rec_split_write(session, bnd, tmp));
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
__rec_split_write(WT_SESSION_IMPL *session, WT_BOUNDARY *bnd, WT_BUF *buf)
{
	WT_CELL *cell;
	WT_PAGE_DISK *dsk;
	WT_RECONCILE *r;
	uint32_t addr, size, notused;

	r = S2C(session)->cache->rec;
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
		WT_ASSERT(session, buf->size < buf->mem_size);

		cell = (WT_CELL *)&(((uint8_t *)buf->data)[buf->size]);
		__wt_cell_set_fixed(cell, WT_CELL_KEY, &notused);
		++buf->size;
	}

	/*
	 * Set the disk block size and clear trailing bytes.
	 * Allocate file space.
	 * Write the disk block.
	 * Return the addr/size pair.
	 */
	__rec_split_buf_prep(session, buf, &size);
	WT_RET(__wt_block_alloc(session, &addr, size));
	WT_RET(__wt_disk_write(session, dsk, addr, size));

	bnd->off.addr = addr;
	bnd->off.size = size;

	/*
	 * If we're evicting the page, there's no more work to do.
	 *
	 * If we're not evicting the page, we'll need a new in-memory version;
	 * save a copy of the disk image for later.  (We can't use the memory
	 * in the original buffer because this memory is eventually attached to
	 * an in-memory page as its disk image.)
	 */
	if (r->evict)
		bnd->imp_dsk = NULL;
	else {
		WT_RET(__wt_calloc(
		    session, (size_t)size, sizeof(uint8_t), &bnd->imp_dsk));
		memcpy(bnd->imp_dsk, dsk, size);
	}
	return (0);
}

/*
 * __rec_split_row_promote --
 *	Key promotion for a row-store.
 */
static int
__rec_split_row_promote(WT_SESSION_IMPL *session, uint8_t type)
{
	WT_CELL *cell;
	WT_RECONCILE *r;
	uint32_t cnt, len, size;
	const uint8_t *pa, *pb;

	r = S2C(session)->cache->rec;

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
		 * key on the page.  (If it doesn't have a zero-length prefix,
		 * __wt_cell_copy() won't be sufficient any way, we'd only copy
		 * the non-prefix-compressed portion of the key.)
		 */
		cell = WT_PAGE_DISK_BYTE(r->dsk.mem);
		WT_ASSERT(session,
		    __wt_cell_prefix(cell) == 0 ||
		    __wt_cell_type(cell) == WT_CELL_KEY_OVFL);
		WT_RET(__wt_cell_copy(session, cell, &r->bnd[0].key));
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
	 * The r->last key sorts before the r->full key, so we'll either find a
	 * larger byte value in r->full, or r->full will be the longer key. One
	 * caveat: if the largest key on the previous page was an overflow key,
	 * we don't have a key against which to compare, and we can't do suffix
	 * compression.
	 */
	if (type == WT_PAGE_ROW_LEAF && r->key_sfx_compress) {
		pa = r->last->data;
		pb = r->full->data;
		len = WT_MIN(r->last->size, r->full->size);
		size = len + 1;
		for (cnt = 1; len > 0; ++cnt, --len, ++pa, ++pb)
			if (*pa != *pb) {
				size = cnt;
				break;
			}
	} else
		size = r->full->size;
	return (__wt_buf_set(
	    session, &r->bnd[r->bnd_next].key, r->full->data, size));
}

/*
 * __rec_col_int --
 *	Reconcile a column-store internal page.
 */
static int
__rec_col_int(WT_SESSION_IMPL *session, WT_PAGE *page)
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
__rec_col_merge(WT_SESSION_IMPL *session, WT_PAGE *page)
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
			if (F_ISSET(rp, WT_PAGE_DELETED | WT_PAGE_MERGE)) {
				WT_ASSERT(
				    session, !F_ISSET(rp, WT_PAGE_DELETED));
				if (F_ISSET(rp, WT_PAGE_MERGE))
					WT_RET(__rec_col_merge(session, rp));
				continue;
			}

			/* Save any in-memory subtree reference. */
			WT_RET(__rec_imref_add(session, &cref->ref));
		}

		/* Boundary: split or write the page. */
		while (sizeof(WT_OFF_RECORD) > r->space_avail)
			WT_RET(__rec_split(session));

		/*
		 * Copy a new WT_OFF_RECORD structure onto the page; any
		 * off-page reference must be a valid disk address.
		 */
		WT_ASSERT(session, WT_COL_REF_ADDR(cref) != WT_ADDR_INVALID);
		off.addr = WT_COL_REF_ADDR(cref);
		off.size = WT_COL_REF_SIZE(cref);
		WT_RECNO(&off) = cref->recno;
		memcpy(r->first_free, &off, sizeof(WT_OFF_RECORD));
		__rec_incr(session, r, WT_SIZEOF32(WT_OFF_RECORD));
	}

	return (0);
}

/*
 * __rec_col_fix --
 *	Reconcile a fixed-width, column-store leaf page (does not handle
 * run-length encoding).
 */
static int
__rec_col_fix(WT_SESSION_IMPL *session, WT_PAGE *page, uint64_t slvg_missing)
{
	WT_BTREE *btree;
	WT_BUF *tmp;
	WT_COL *cip;
	WT_RECONCILE *r;
	WT_UPDATE *upd;
	uint32_t i, len;
	uint8_t *data;
	int ret;
	void *cipdata;

	r = S2C(session)->cache->rec;
	btree = session->btree;
	tmp = NULL;
	ret = 0;

	/*
	 * We need a "deleted" data item to store on the page; get a scratch
	 * buffer, clear the contents and set the delete flag.
	 */
	len = btree->fixed_len;
	WT_ERR(__wt_scr_alloc(session, len, &tmp));
	memset(tmp->mem, 0, len);
	WT_FIX_DELETE_SET(tmp->mem);

	WT_ERR(__rec_split_init(session, page,
	    page->u.col_leaf.recno,
	    session->btree->leafmax, session->btree->leafmin));

	/*
	 * The salvage code may be calling us to reconcile a page where there
	 * were missing records in the column-store name space.
	 */
	for (; slvg_missing > 0; --slvg_missing) {
		/* Boundary: split or write the page. */
		while (len > r->space_avail)
			WT_RET(__rec_split(session));

		memcpy(r->first_free, tmp->mem, len);
		__rec_incr(session, r, len);

		/* Update the starting record number in case we split. */
		++r->recno;
	}

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
			if (cipdata == NULL || WT_FIX_DELETE_ISSET(cipdata))
				data = tmp->mem;	/* Deleted */
			else
				data = cipdata;		/* On-disk data */
		}

		/* Boundary: split or write the page. */
		while (len > r->space_avail)
			WT_RET(__rec_split(session));

		memcpy(r->first_free, data, len);
		__rec_incr(session, r, len);

		/* Update the starting record number in case we split. */
		++r->recno;
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
__rec_col_fix_bulk(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_RECONCILE *r;
	WT_UPDATE *upd;
	uint32_t len;

	r = S2C(session)->cache->rec;
	btree = session->btree;
	len = btree->fixed_len;

	WT_RET(__rec_split_init(session, page,
	    page->u.bulk.recno,
	    session->btree->leafmax, session->btree->leafmin));

	/* For each entry in the update list... */
	for (upd = page->u.bulk.upd; upd != NULL; upd = upd->next) {
		/* Boundary: split or write the page. */
		while (len > r->space_avail)
			WT_RET(__rec_split(session));

		memcpy(r->first_free, WT_UPDATE_DATA(upd), len);
		__rec_incr(session, r, len);

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
__rec_col_rle(WT_SESSION_IMPL *session, WT_PAGE *page, uint64_t slvg_missing)
{
	WT_BTREE *btree;
	WT_BUF *tmp;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_RECONCILE *r;
	uint32_t i, len;
	uint16_t n, nrepeat, repeat_count;
	uint8_t *data, *last_data;
	int ret;
	void *cipdata;

	r = S2C(session)->cache->rec;
	btree = session->btree;
	tmp = NULL;
	last_data = NULL;
	ret = 0;

	/*
	 * We need a "deleted" data item to store on the page.  Make sure the
	 * session's scratch buffer is big enough.  Clear the buffer's contents
	 * and set the delete flag.
	 */
	len = btree->fixed_len;
	WT_ERR(__wt_scr_alloc(session, len, &tmp));
	memset(tmp->mem, 0, len);
	WT_FIX_DELETE_SET(tmp->mem);

	WT_RET(__rec_split_init(session, page,
	    page->u.col_leaf.recno,
	    session->btree->leafmax, session->btree->leafmin));

	/*
	 * The salvage code may be calling us to reconcile a page where there
	 * were missing records in the column-store name space.
	 */
	for (; slvg_missing > 0; slvg_missing -= repeat_count) {
		/* Boundary: split or write the page. */
		while (len + WT_SIZEOF32(uint16_t) > r->space_avail)
			WT_ERR(__rec_split(session));

		data = r->first_free;
		repeat_count = (uint16_t)WT_MIN(UINT16_MAX, slvg_missing);
		WT_RLE_REPEAT_COUNT(data) = repeat_count;
		memcpy(WT_RLE_REPEAT_DATA(data), tmp->mem, len);
		__rec_incr(session, r, len + WT_SIZEOF32(uint16_t));

		/* Update the starting record number in case we split. */
		r->recno += repeat_count;
	}

	/* For each entry in the in-memory page... */
	WT_COL_FOREACH(page, cip, i) {
		cipdata = WT_COL_PTR(page, cip);
		ins = WT_COL_INSERT(page, cip),

		/*
		 * Generate entries for the new page: loop through the repeat
		 * records, checking for WT_INSERT entries matching the record
		 * number.
		 *
		 * Note the increment of recno in the for loop to update the
		 * starting record number in case we split.
		 */
		nrepeat = cipdata == NULL ? 1 : WT_RLE_REPEAT_COUNT(cipdata);
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
				if (cipdata == NULL ||
				    WT_FIX_DELETE_ISSET(cipdata))
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

			/* Boundary: split or write the page. */
			while (len + WT_SIZEOF32(uint16_t) > r->space_avail)
				WT_ERR(__rec_split(session));

			last_data = r->first_free;
			WT_RLE_REPEAT_COUNT(last_data) = repeat_count;
			memcpy(WT_RLE_REPEAT_DATA(last_data), data, len);
			__rec_incr(session, r, len + WT_SIZEOF32(uint16_t));
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
__rec_col_rle_bulk(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_RECONCILE *r;
	WT_UPDATE *upd;
	uint32_t len;
	uint8_t *data, *last_data;

	r = S2C(session)->cache->rec;
	btree = session->btree;
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

		/* Boundary: split or write the page. */
		while (len + WT_SIZEOF32(uint16_t) > r->space_avail)
			WT_RET(__rec_split(session));

		last_data = r->first_free;
		WT_RLE_REPEAT_COUNT(last_data) = 1;
		memcpy(WT_RLE_REPEAT_DATA(last_data), data, len);
		__rec_incr(session, r, len + WT_SIZEOF32(uint16_t));
	}

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_col_extend_truncate --
 *	Discard deleted entries from the end of the name space.
 */
static void
__rec_col_extend_truncate(WT_PAGE *page)
{
	WT_COL *cip;
	WT_PAGE *t;

	/*
	 * Extending a column-store file can result in thousands of deleted
	 * entries at the end of the file.  (The first insert triggers the
	 * creation of a large number of empty slots which are then filled
	 * in.)  We don't want to write thousands of deleted entries out to
	 * the file, so we discard them here.
	 *
	 * This only affects leaf pages (internal pages are extended on demand,
	 * and so we'll never write empty slots even if they were allocated).
	 *
	 * First, check to see if this page is at the end of the name space.
	 */
	for (t = page; !WT_PAGE_IS_ROOT(t); t = t->parent)
		if (WT_ROW_REF_SLOT(
		    t->parent, t->parent_ref) != t->parent->entries - 1)
			return;

	/*
	 * If we're at the end of the name space, discard any deleted entries
	 * from the page (always leaving at least one entry on the page, it's
	 * not possible to delete column-store pages).   Keep it simple, don't
	 * look for records that existed but were then deleted, that requires
	 * looking at update chains and on-page overflow objects.
	 *
	 * If we ever support deleting column-store pages it would be worth it
	 * to do that work, because it would actually shrink the file over time.
	 * Since we can only shrink a single page here, I don't bother.  Another
	 * possibility would be to review the end of the column-store file on
	 * close, and take the time to discard all of the deleted records from
	 * the end of the file.  Close is a relatively rare operation, slowing
	 * it down shouldn't be too awful.
	 */
	cip = &page->u.col_leaf.d[page->entries - 1];
	for (; page->entries > 1; --cip, --page->entries)
		if (WT_COL_PTR(page, cip) != NULL ||
		    WT_COL_UPDATE(page, cip) != NULL)
			return;
}

/*
 * __rec_col_var --
 *	Reconcile a variable-width column-store leaf page.
 */
static int
__rec_col_var(WT_SESSION_IMPL *session, WT_PAGE *page, uint64_t slvg_missing)
{
	WT_CELL *cell;
	WT_COL *cip;
	WT_KV *val;
	WT_RECONCILE *r;
	WT_UPDATE *upd;
	uint32_t i;

	r = S2C(session)->cache->rec;
	val = &r->v;

	WT_RET(__rec_split_init(session, page,
	    page->u.col_leaf.recno,
	    session->btree->leafmax, session->btree->leafmin));

	/*
	 * The salvage code may be calling us to reconcile a page where there
	 * were missing records in the column-store name space.
	 */
	for (; slvg_missing > 0; --slvg_missing) {
		__rec_cell_build_deleted(val);

		/* Boundary: split or write the page. */
		while (val->len > r->space_avail)
			WT_RET(__rec_split(session));

		/* Copy the value onto the page. */
		__rec_copy_incr(session, r, val);

		/* Update the starting record number in case we split. */
		++r->recno;
	}

	/* For each entry in the in-memory page... */
	WT_COL_FOREACH(page, cip, i) {
		/*
		 * Get a reference to the value: it's a deleted cell, an update,
		 * or the original on-page item.
		 */
		cell = WT_COL_PTR(page, cip);
		if ((upd = WT_COL_UPDATE(page, cip)) == NULL) {
			if (cell == NULL)
				__rec_cell_build_deleted(val);
			else {
				val->buf.data = cell;
				val->buf.size = __wt_cell_len(cell);
				val->cell_len = 0;
				val->len = val->buf.size;
			}
		} else {
			/*
			 * If we update an overflow value, free the underlying
			 * file space.
			 */
			if (cell != NULL)
				WT_RET(__rec_discard_add_ovfl(session, cell));

			/*
			 * Check for deletion, else build the value's WT_CELL
			 * chunk from the most recent update value.
			 */
			if (WT_UPDATE_DELETED_ISSET(upd))
				__rec_cell_build_deleted(val);
			else
				WT_RET(__rec_cell_build_val(
				    session, WT_UPDATE_DATA(upd), upd->size));
		}

		/* Boundary: split or write the page. */
		while (val->len > r->space_avail)
			WT_RET(__rec_split(session));

		/* Copy the value onto the page. */
		__rec_copy_incr(session, r, val);

		/* Update the starting record number in case we split. */
		++r->recno;
	}

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_col_var_bulk --
 *	Reconcile a bulk-loaded, variable-width column-store leaf page.
 */
static int
__rec_col_var_bulk(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_KV *val;
	WT_RECONCILE *r;
	WT_UPDATE *upd;

	r = S2C(session)->cache->rec;
	val = &r->v;

	WT_RET(__rec_split_init(session, page,
	    page->u.bulk.recno,
	    session->btree->leafmax, session->btree->leafmin));

	/* For each entry in the update list... */
	for (upd = page->u.bulk.upd; upd != NULL; upd = upd->next) {
		WT_RET(__rec_cell_build_val(
		    session, WT_UPDATE_DATA(upd), upd->size));

		/* Boundary: split or write the page. */
		while (val->len > r->space_avail)
			WT_RET(__rec_split(session));

		/* Copy the value onto the page. */
		__rec_copy_incr(session, r, val);

		/* Update the starting record number in case we split. */
		++r->recno;
	}

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_row_int --
 *	Reconcile a row-store internal page.
 */
static int
__rec_row_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CELL *cell;
	WT_IKEY *ikey;
	WT_KV *key, *val;
	WT_PAGE *rp;
	WT_RECONCILE *r;
	WT_ROW_REF *rref;
	uint32_t i;
	int ovfl_key;

	r = S2C(session)->cache->rec;
	key = &r->k;
	val = &r->v;

	WT_RET(__rec_split_init(session,
	    page, 0ULL, session->btree->intlmax, session->btree->intlmin));

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
	 * it simplifies various things in other parts of the code: we don't
	 * have to special case page verification, or transforming the page
	 * from its disk image to its in-memory version.
	 */
	r->cell_zero = 1;

	/*
	 * The value cells all look the same -- we can set it up once and then
	 * just reset the addr/size pairs we're writing after the cell.
	 */
	__wt_cell_set_fixed(&val->cell, WT_CELL_OFF, &val->cell_len);
	val->buf.data = &val->off;
	val->buf.size = WT_SIZEOF32(WT_OFF);
	val->len = val->cell_len + WT_SIZEOF32(WT_OFF);

	/* For each entry in the in-memory page... */
	WT_ROW_REF_FOREACH(page, rref, i) {
		/*
		 * Keys are always instantiated for row-store internal pages,
		 * set the WT_IKEY reference.  We may have key overflow items
		 * on the page, in which case the disk image was retained and
		 * the key's WT_CELL reference was set.
		 */
		ikey = rref->key;
		cell = (ikey->cell_offset == 0) ?
		    NULL : WT_REF_OFFSET(page, ikey->cell_offset);

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
			if (F_ISSET(rp, WT_PAGE_DELETED | WT_PAGE_MERGE)) {
				/* Delete overflow keys for merged pages. */
				if (cell != NULL)
					WT_RET(__rec_discard_add_ovfl(
					    session, cell));

				/* Merge split subtrees */
				if (F_ISSET(rp, WT_PAGE_MERGE)) {
					r->merge_ref = rref;
					WT_RET(__rec_row_merge(session, rp));
				}
				continue;
			}

			/* Save any in-memory subtree reference. */
			WT_RET(__rec_imref_add(session, &rref->ref));
		}

		/*
		 * Build key cell.
		 *
		 * If the key is an overflow item, assume prefix compression
		 * won't make things better, and simply copy it.
		 *
		 * Truncate any 0th key, internal pages don't need 0th keys.
		 */
		if (cell != NULL) {
			key->buf.data = cell;
			key->buf.size = __wt_cell_len(cell);
			key->cell_len = 0;
			key->len = key->buf.size;
			ovfl_key = 1;
		} else {
			WT_RET(__rec_cell_build_key(session,
			    WT_IKEY_DATA(ikey), r->cell_zero ? 1 : ikey->size));
			ovfl_key = __wt_cell_type_is_ovfl(&key->cell);
		}
		r->cell_zero = 0;

		/*
		 * Boundary, split or write the page.  If the K/V pair doesn't
		 * fit: split the page, turn off compression (until a full key
		 * is written to the page), change to a non-prefix-compressed
		 * key.
		 */
		while (key->len + val->len > r->space_avail) {
			/*
			 * We have to have a copy of any overflow key because
			 * we're about to promote it.
			 */
			if (ovfl_key && cell != NULL)
				WT_RET(__wt_cell_copy(session, cell, r->full));
			WT_RET(__rec_split(session));

			r->key_pfx_compress = 0;
			if (!ovfl_key) {
				WT_RET(__rec_cell_build_key(session, NULL, 0));
				ovfl_key = __wt_cell_type_is_ovfl(&key->cell);
			}
		}

		/* Copy the key onto the page. */
		__rec_copy_incr(session, r, key);

		/*
		 * Copy the off-page reference onto the page; any off-page
		 * reference must be a valid disk address.
		 */
		WT_ASSERT(session, WT_ROW_REF_ADDR(rref) != WT_ADDR_INVALID);
		val->off.addr = WT_ROW_REF_ADDR(rref);
		val->off.size = WT_ROW_REF_SIZE(rref);
		__rec_copy_incr(session, r, val);

		/* Update compression state. */
		__rec_key_state_update(r, ovfl_key);
	}

	/* Write the remnant page. */
	return (__rec_split_finish(session));
}

/*
 * __rec_row_merge --
 *	Recursively walk a row-store internal tree of merge pages.
 */
static int
__rec_row_merge(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_IKEY *ikey;
	WT_KV *key, *val;
	WT_PAGE *rp;
	WT_RECONCILE *r;
	WT_ROW_REF *rref;
	uint32_t i;
	int ovfl_key;

	r = S2C(session)->cache->rec;
	key = &r->k;
	val = &r->v;

	/* For each entry in the in-memory page... */
	WT_ROW_REF_FOREACH(page, rref, i) {
		/*
		 * The page may be deleted or internally created during a split.
		 * Deleted/split pages are merged into the parent and discarded.
		 */
		if (WT_ROW_REF_STATE(rref) != WT_REF_DISK) {
			rp = WT_ROW_REF_PAGE(rref);
			if (F_ISSET(rp, WT_PAGE_DELETED | WT_PAGE_MERGE)) {
				/* Merge split subtrees */
				if (F_ISSET(rp, WT_PAGE_MERGE))
					WT_RET(__rec_row_merge(session, rp));
				continue;
			}

			/* Save any in-memory subtree reference. */
			WT_RET(__rec_imref_add(session, &rref->ref));
		}

		/*
		 * Build the key cell.  If this is the first key in a "to be
		 * merged" subtree, use the merge correction key saved in the
		 * top-level parent page when this function was called.
		 *
		 * Truncate any 0th key, internal pages don't need 0th keys.
		 */
		ikey = r->merge_ref == NULL ? rref->key : r->merge_ref->key;
		r->merge_ref = NULL;
		WT_RET(__rec_cell_build_key(session,
		    WT_IKEY_DATA(ikey), r->cell_zero ? 1 : ikey->size));
		r->cell_zero = 0;
		ovfl_key = __wt_cell_type_is_ovfl(&key->cell);

		/*
		 * Boundary, split or write the page.  If the K/V pair doesn't
		 * fit: split the page, turn off compression (until a full key
		 * is written to the page), change to a non-prefix-compressed
		 * key.
		 */
		while (key->len + val->len > r->space_avail) {
			WT_RET(__rec_split(session));

			r->key_pfx_compress = 0;
			if (!ovfl_key) {
				WT_RET(__rec_cell_build_key(session, NULL, 0));
				ovfl_key = __wt_cell_type_is_ovfl(&key->cell);
			}
		}

		/* Copy the key onto the page. */
		__rec_copy_incr(session, r, key);

		/*
		 * Copy the off-page reference onto the page; any off-page
		 * reference must be a valid disk address.
		 */
		WT_ASSERT(session, WT_ROW_REF_ADDR(rref) != WT_ADDR_INVALID);
		val->off.addr = WT_ROW_REF_ADDR(rref);
		val->off.size = WT_ROW_REF_SIZE(rref);
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
__rec_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page, uint64_t slvg_skip)
{
	WT_BUF *tmp;
	WT_CELL *cell, *val_cell;
	WT_IKEY *ikey;
	WT_INSERT *ins;
	WT_KV *key, *val;
	WT_RECONCILE *r;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;
	int ovfl_key, ret;

	r = S2C(session)->cache->rec;
	tmp = NULL;
	ret = 0;

	key = &r->k;
	val = &r->v;

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
	 * A temporary buffer in which to instantiate any uninstantiated keys.
	 * From this point on, we need to jump to the err label on error so the
	 * buffer is discarded.
	 */
	WT_RET(__wt_scr_alloc(session, 0, &tmp));

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
		if (__wt_off_page(page, rip->key)) {
			ikey = rip->key;
			cell = WT_REF_OFFSET(page, ikey->cell_offset);
		} else {
			ikey = NULL;
			cell = rip->key;
		}
							/* Build value cell. */
		if ((upd = WT_ROW_UPDATE(page, rip)) == NULL) {
			/*
			 * Copy the item off the page -- however, when the page
			 * was read into memory, there may not have been a value
			 * item, that is, it may have been zero length.
			 */
			if ((val_cell = __wt_row_value(page, rip)) == NULL)
				val->buf.size = 0;
			else {
				val->buf.data = val_cell;
				val->buf.size = __wt_cell_len(val_cell);
			}
			val->cell_len = 0;
			val->len = val->buf.size;
		} else {
			/*
			 * If we updated an overflow value, free the underlying
			 * file space.
			 */
			if ((val_cell = __wt_row_value(page, rip)) != NULL)
				WT_ERR(
				    __rec_discard_add_ovfl(session, val_cell));

			/*
			 * If this key/value pair was deleted, we're done.  If
			 * we deleted an overflow key, free the underlying file
			 * space.
			 */
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				WT_ERR(__rec_discard_add_ovfl(session, cell));
				goto leaf_insert;
			}

			/*
			 * If no value, nothing needs to be copied.  Otherwise,
			 * build the value's WT_CELL chunk from the most recent
			 * update value.
			 */
			if (upd->size == 0)
				val->cell_len = val->len = val->buf.size = 0;
			else
				WT_ERR(__rec_cell_build_val(
				    session, WT_UPDATE_DATA(upd), upd->size));
		}

		/*
		 * Build key cell.
		 *
		 * If the key is an overflow item, assume prefix compression
		 * won't make things better, and simply copy it.
		 */
		if (__wt_cell_type(cell) == WT_CELL_KEY_OVFL) {
			key->buf.data = cell;
			key->buf.size = __wt_cell_len(cell);
			key->cell_len = 0;
			key->len = key->buf.size;
			ovfl_key = 1;
		} else if (ikey != NULL) {
			WT_ERR(__rec_cell_build_key(
			    session, WT_IKEY_DATA(ikey), ikey->size));
			ovfl_key = __wt_cell_type_is_ovfl(&key->cell);
		} else {
			WT_ERR(__wt_row_key(session, page, rip, tmp));
			WT_ERR(__rec_cell_build_key(
			    session, tmp->data, tmp->size));
			ovfl_key = __wt_cell_type_is_ovfl(&key->cell);
		}

		/*
		 * Boundary, split or write the page.  If the K/V pair doesn't
		 * fit: split the page, switch to the non-prefix-compressed key
		 * and turn off compression until a full key is written to the
		 * new page.
		 *
		 * We write a trailing key cell on the page after the K/V pairs
		 * (see WT_TRAILING_KEY_CELL for more information).
		 */
		while (key->len +
		    val->len + WT_TRAILING_KEY_CELL > r->space_avail) {
			/*
			 * We have to have a copy of any overflow key because
			 * we're about to promote it.
			 */
			if (ovfl_key &&
			    __wt_cell_type(cell) == WT_CELL_KEY_OVFL)
				WT_RET(__wt_cell_copy(session, cell, r->full));
			WT_ERR(__rec_split(session));

			r->key_pfx_compress = 0;
			if (!ovfl_key) {
				WT_ERR(__rec_cell_build_key(session, NULL, 0));
				ovfl_key = __wt_cell_type_is_ovfl(&key->cell);
			}
		}

		/* Copy the key/value pair onto the page. */
		__rec_copy_incr(session, r, key);
		if (val->len != 0)
			__rec_copy_incr(session, r, val);

		/* Update compression state. */
		__rec_key_state_update(r, ovfl_key);

leaf_insert:	/* Write any K/V pairs inserted into the page after this key. */
		if ((ins = WT_ROW_INSERT(page, rip)) != NULL)
			WT_ERR(__rec_row_leaf_insert(session, ins));
	}

	/* Write the remnant page. */
	ret = __rec_split_finish(session);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __rec_row_leaf_insert --
 *	Walk an insert chain, writing K/V pairs.
 */
static int
__rec_row_leaf_insert(WT_SESSION_IMPL *session, WT_INSERT *ins)
{
	WT_KV *key, *val;
	WT_RECONCILE *r;
	WT_UPDATE *upd;
	int ovfl_key;

	r = S2C(session)->cache->rec;
	key = &r->k;
	val = &r->v;

	for (; ins != NULL; ins = ins->next) {
		upd = ins->upd;				/* Build value cell. */
		if (WT_UPDATE_DELETED_ISSET(upd))
			continue;
		if (upd->size == 0)
			val->len = 0;
		else
			WT_RET(__rec_cell_build_val(
			    session, WT_UPDATE_DATA(upd), upd->size));

		WT_RET(__rec_cell_build_key(		/* Build key cell. */
		    session, WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins)));
		ovfl_key = __wt_cell_type_is_ovfl(&key->cell);

		/*
		 * Boundary, split or write the page.  If the K/V pair doesn't
		 * fit: split the page, switch to the non-prefix-compressed key
		 * and turn off compression until a full key is written to the
		 * new page.
		 *
		 * We write a trailing key cell on the page after the K/V pairs
		 * (see WT_TRAILING_KEY_CELL for more information).
		 */
		while (key->len +
		    val->len + WT_TRAILING_KEY_CELL > r->space_avail) {
			WT_RET(__rec_split(session));

			r->key_pfx_compress = 0;
			if (!ovfl_key) {
				WT_RET(__rec_cell_build_key(session, NULL, 0));
				ovfl_key = __wt_cell_type_is_ovfl(&key->cell);
			}
		}

		/* Copy the key/value pair onto the page. */
		__rec_copy_incr(session, r, key);
		if (val->len != 0)
			__rec_copy_incr(session, r, val);

		/* Update compression state. */
		__rec_key_state_update(r, ovfl_key);
	}

	return (0);
}

/*
 * __rec_wrapup  --
 *	Resolve the WT_RECONCILE information.
 */
static int
__rec_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BOUNDARY *bnd;
	WT_BTREE *btree;
	WT_PAGE *replace;
	WT_RECONCILE *r;

	r = S2C(session)->cache->rec;
	btree = session->btree;

	/*
	 * If the page was empty, we want to eventually discard it from the
	 * tree by merging it into its parent, not just evict it from memory.
	 */
	if (r->bnd_next == 0) {
		WT_VERBOSE(session, EVICT,
		    "reconcile: delete page %" PRIu32 " (%" PRIu32 "B)",
		    WT_PADDR(page), WT_PSIZE(page));
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
	if (r->bnd_next == 1) {
		bnd = &r->bnd[0];

		WT_VERBOSE(session, EVICT,
		    "reconcile: move %" PRIu32 " to %" PRIu32
		    ", (%" PRIu32 "B to %" PRIu32 "B)",
		    WT_PADDR(page), bnd->off.addr,
		    WT_PSIZE(page), bnd->off.size);

		/*
		 * Create a new in-memory version of this page if necessary.
		 * If we're evicting this page, the new state is "on-disk",
		 * else, in-memory.
		 */
		if (r->evict)
			WT_RET(__rec_parent_update(session, page, NULL,
			    bnd->off.addr, bnd->off.size, WT_REF_DISK));
		else {
			WT_RET(__rec_imref_fixup(
			    session, bnd,
			    page->parent, page->parent_ref, &replace));
			WT_RET(__rec_parent_update(session, page, replace,
			    bnd->off.addr, bnd->off.size, WT_REF_MEM));
		}
	} else {
		/*
		 * A page grew so large we had to divide it into two or more
		 * physical pages -- create a new internal page.
		 */
		WT_VERBOSE(session, EVICT,
		    "reconcile: %" PRIu32 " (%" PRIu32 "B) splitting",
		    WT_PADDR(page), WT_PSIZE(page));

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
			WT_RET(__rec_row_split(session, page, &replace));
			break;
		case WT_PAGE_COL_INT:
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_RLE:
		case WT_PAGE_COL_VAR:
			WT_RET(__rec_col_split(session, page, &replace));
			break;
		WT_ILLEGAL_FORMAT(session);
		}

		/*
		 * Update the parent to reference the newly created internal
		 * page.
		 */
		WT_RET(__rec_parent_update(
		    session, page, replace, WT_ADDR_INVALID, 0, WT_REF_MEM));
	}

	/*
	 * Queue the original page to be discarded, we're done with it.  Salvage
	 * is a special case because it's giving us modified pages to reconcile,
	 * and it has to free them itself.
	 */
	if (!r->salvage)
		WT_RET(__rec_discard_add_page(session, page));

	/* Discard everything we've queued for discard. */
	WT_RET(__rec_discard_evict(session));

	return (0);
}

/*
 * __rec_parent_update_clean  --
 *	Update a parent page's reference for an evicted, clean page.
 */
static void
__rec_parent_update_clean(WT_SESSION_IMPL *session, WT_PAGE *page)
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
__rec_parent_update(WT_SESSION_IMPL *session, WT_PAGE *page,
    WT_PAGE *replace, uint32_t addr, uint32_t size, uint32_t state)
{
	WT_REF *parent_ref;

	WT_ASSERT(session,
	    replace == NULL || page->parent == replace->parent);
	WT_ASSERT(session,
	    replace == NULL || page->parent_ref == replace->parent_ref);

	parent_ref = page->parent_ref;

	/*
	 * If we're replacing a valid addr/size pair, free the original disk
	 * blocks, they're no longer in use.
	 */
	if (parent_ref->addr != WT_ADDR_INVALID)
		WT_RET(__rec_discard_add_block(
		    session, parent_ref->addr, parent_ref->size));

	/*
	 * Updating the state of the parent reference turns on the change.  Set
	 * the parent's WT_REF addr/size pair and memory reference, flush memory
	 * and then update the state of the entry (no further flush is required
	 * as the page state is declared volatile).
	 */
	parent_ref->addr = addr;
	parent_ref->size = size;
	parent_ref->page = replace;
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
 * __rec_cell_build_deleted --
 *	Create a deleted cell for a variable-length column-store.
 */
static inline void
__rec_cell_build_deleted(WT_KV *val)
{
	__wt_cell_set_fixed(&val->cell, WT_CELL_DEL, &val->cell_len);
	val->buf.size = 0;
	val->len = val->cell_len;
}

/*
 * __rec_cell_build_key --
 *	Process a key and return a WT_CELL structure and byte string to be
 * stored on the page.
 */
static int
__rec_cell_build_key(WT_SESSION_IMPL *session, const void *data, uint32_t size)
{
	WT_BTREE *btree;
	WT_KV *key;
	WT_RECONCILE *r;
	uint32_t pfx_len, pfx;
	const uint8_t *a, *b;

	r = S2C(session)->cache->rec;
	btree = session->btree;
	key = &r->k;

	pfx = 0;
	if (data == NULL)
		/*
		 * When data is NULL, our caller has a prefix compressed key
		 * they can't use (probably because they just crossed a split
		 * point).  Use the full key saved when last called, instead.
		 */
		WT_RET(__wt_buf_set(
		    session, &key->buf, r->full->data, r->full->size));
	else {
		/*
		 * Save a copy of the key for later reference: we use the full
		 * key for prefix-compression comparisons, and if we are, for
		 * any reason, unable to use the compressed key we generate.
		 */
		WT_RET(__wt_buf_set(session, r->full, data, size));

		/*
		 * Do prefix compression on the key.  We know by definition the
		 * previous key sorts before the current key, which means the
		 * keys must differ and we just need to compare up to the
		 * shorter of the two keys.   Also, we can't compress out more
		 * than 256 bytes, limit the comparison to that.
		 */
		if (r->key_pfx_compress) {
			pfx_len = size;
			if (pfx_len > r->last->size)
				pfx_len = r->last->size;
			if (pfx_len > UINT8_MAX)
				pfx_len = UINT8_MAX;
			for (a = data, b = r->last->data; pfx < pfx_len; ++pfx)
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
	if (key->buf.size > btree->leafitemsize) {
		WT_STAT_INCR(btree->stats, overflow_key);

		/*
		 * Overflow objects aren't prefix compressed -- rebuild any
		 * object that was prefix compressed.
		 */
		return ((pfx == 0) ?
		    __rec_cell_build_ovfl(session, key, WT_CELL_KEY_OVFL) :
		    __rec_cell_build_key(session, NULL, 0));
	}

	__wt_cell_set(session,
	    &key->cell, WT_CELL_KEY, pfx, key->buf.size, &key->cell_len);
	key->len = key->cell_len + key->buf.size;

	return (0);
}

/*
 * __rec_cell_build_val --
 *	Process a data item and return a WT_CELL structure and byte string to
 * be stored on the page.
 */
static int
__rec_cell_build_val(WT_SESSION_IMPL *session, void *data, uint32_t size)
{
	WT_BTREE *btree;
	WT_KV *val;
	WT_RECONCILE *r;

	r = S2C(session)->cache->rec;
	btree = session->btree;
	val = &r->v;

	/*
	 * We don't copy the data into the buffer, it's not necessary; just
	 * re-point the buffers data/length fields.
	 */
	val->buf.data = data;
	val->buf.size = size;

	/* Handle zero-length cells quickly. */
	if (size == 0) {
		__wt_cell_set(
		    session, &val->cell, WT_CELL_DATA, 0, 0, &val->cell_len);
		val->len = val->cell_len + val->buf.size;
		return (0);
	}

	/* Optionally compress the data using the Huffman engine. */
	if (btree->huffman_value != NULL)
		WT_RET(__wt_huffman_encode(session, btree->huffman_value,
		    val->buf.data, val->buf.size, &val->buf));

	/* Create an overflow object if the data won't fit. */
	if (val->buf.size > btree->leafitemsize) {
		WT_STAT_INCR(btree->stats, overflow_data);

		return (__rec_cell_build_ovfl(session, val, WT_CELL_DATA_OVFL));
	}

	__wt_cell_set(session,
	    &val->cell, WT_CELL_DATA, 0, val->buf.size, &val->cell_len);
	val->len = val->cell_len + val->buf.size;
	return (0);
}

/*
 * __rec_cell_build_ovfl --
 *	Store bulk-loaded overflow items in the file, returning the WT_OFF.
 */
static int
__rec_cell_build_ovfl(WT_SESSION_IMPL *session, WT_KV *kv, u_int type)
{
	WT_BUF *tmp;
	WT_PAGE_DISK *dsk;
	uint32_t addr, size;
	int ret;

	tmp = NULL;
	ret = 0;

	/* Allocate a scratch buffer big enough to hold the overflow chunk. */
	size = WT_DISK_REQUIRED(session, kv->buf.size);
	WT_RET(__wt_scr_alloc(session, size, &tmp));

	/*
	 * Set up the chunk; clear any unused bytes so the contents are never
	 * random.
	 */
	dsk = tmp->mem;
	memset(dsk, 0, WT_PAGE_DISK_SIZE);
	dsk->type = WT_PAGE_OVFL;
	dsk->u.datalen = kv->buf.size;
	memcpy(WT_PAGE_DISK_BYTE(dsk), kv->buf.data, kv->buf.size);
	memset((uint8_t *)WT_PAGE_DISK_BYTE(dsk) +
	    kv->buf.size, 0, size - (WT_PAGE_DISK_SIZE + kv->buf.size));

	/*
	 * Allocate a file address (we're the only writer of the file, so we
	 * can allocate file space on demand).  Fill in the WT_OFF structure.
	 */
	WT_ERR(__wt_block_alloc(session, &addr, size));
	kv->off.addr = addr;
	kv->off.size = size;

	/* Set the callers K/V to reference the WT_OFF structure. */
	kv->buf.data = &kv->off;
	kv->buf.size = sizeof(kv->off);
	__wt_cell_set_fixed(&kv->cell, type, &kv->cell_len);
	kv->len = kv->cell_len + kv->buf.size;

	ret = __wt_disk_write(session, dsk, addr, size);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __rec_key_state_update --
 *	Update prefix and suffix compression based on the last key.
 */
static inline void
__rec_key_state_update(WT_RECONCILE *r, int ovfl_key)
{
	WT_BUF *a;

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
	 * "last key" is the largest key on the previous page, and "full key" is
	 * the first key on the next page, which is being promoted.  In some
	 * cases we can discard bytes from the "full key" that are not needed to
	 * distinguish between the "last key" and "full key", compressing the
	 * size of keys on internal nodes.  If we just built an overflow key,
	 * we're not going to update the "last key", making suffix compression
	 * impossible for the next key.   Alternatively, we could remember where
	 * the last key was on the page, detect it's an overflow key, read it
	 * from disk and do suffix compression, but that's too much work for an
	 * unlikely event.
	 *
	 * If we're not writing an overflow key on the page, update the last-key
	 * value and turn on both prefix and suffix compression.
	 */
	if (ovfl_key)
		r->key_sfx_compress = 0;
	else {
		a = r->full;
		r->full = r->last;
		r->last = a;

		r->key_pfx_compress = r->key_pfx_compress_conf;
		r->key_sfx_compress = r->key_sfx_compress_conf;
	}
}

/*
 * __rec_row_split --
 *	Update a row-store parent page's reference when a page is split.
 */
static int
__rec_row_split(WT_SESSION_IMPL *session, WT_PAGE *orig, WT_PAGE **splitp)
{
	WT_BOUNDARY *bnd;
	WT_PAGE *imp, *page;
	WT_RECONCILE *r;
	WT_ROW_REF *rref;
	uint32_t i;
	int ret;

	r = S2C(session)->cache->rec;
	ret = 0;

	/* Allocate a row-store internal page. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc_def(
	    session, (size_t)r->bnd_next, &page->u.row_int.t));

	/* Fill it in. */
	page->parent = orig->parent;
	page->parent_ref = orig->parent_ref;
	page->read_gen = __wt_cache_read_gen(session);
	page->entries = r->bnd_next;
	page->type = WT_PAGE_ROW_INT;
	WT_PAGE_SET_MODIFIED(page);
	/*
	 * Newly created internal pages are not persistent as we don't want the
	 * tree to deepen whenever a leaf page splits.  Flag the page for merge
	 * into its parent when the parent is reconciled.
	 */
	F_SET(page, WT_PAGE_MERGE);

	/* Enter each split page into the new, internal page. */
	for (rref = page->u.row_int.t,
	    bnd = r->bnd, i = 0; i < r->bnd_next; ++rref, ++bnd, ++i) {
		WT_RET(__wt_row_ikey_alloc(session, 0,
		    bnd->key.data, bnd->key.size, (WT_IKEY **)&rref->key));
		WT_ROW_REF_ADDR(rref) = bnd->off.addr;
		WT_ROW_REF_SIZE(rref) = bnd->off.size;

		/*
		 * If there's an in-memory version of the page, connect it to
		 * this parent and fix up its in-memory references.
		 */
		if (bnd->imp_dsk == NULL) {
			WT_ROW_REF_PAGE(rref) = NULL;
			WT_ROW_REF_STATE(rref) = WT_REF_DISK;
		} else {
			WT_RET(__rec_imref_fixup(
			    session, bnd, page, &rref->ref, &imp));
			WT_ROW_REF_PAGE(rref) = imp;
			WT_ROW_REF_STATE(rref) = WT_REF_MEM;
		}

		WT_VERBOSE(session, EVICT,
		    "split: %" PRIu32 " (%" PRIu32 "B)",
		    bnd->off.addr, bnd->off.size);
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
__rec_col_split(WT_SESSION_IMPL *session, WT_PAGE *orig, WT_PAGE **splitp)
{
	WT_BOUNDARY *bnd;
	WT_COL_REF *cref;
	WT_PAGE *imp, *page;
	WT_RECONCILE *r;
	uint32_t i;
	int ret;

	r = S2C(session)->cache->rec;
	ret = 0;

	/* Allocate a column-store internal page. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc_def(
	    session, (size_t)r->bnd_next, &page->u.col_int.t));

	/* Fill it in. */
	page->parent = orig->parent;
	page->parent_ref = orig->parent_ref;
	page->read_gen = __wt_cache_read_gen(session);
	page->u.col_int.recno = r->bnd[0].recno;
	page->entries = r->bnd_next;
	page->type = WT_PAGE_COL_INT;
	WT_PAGE_SET_MODIFIED(page);
	/*
	 * Newly created internal pages are not persistent as we don't want the
	 * tree to deepen whenever a leaf page splits.  Flag the page for merge
	 * into its parent when the parent is reconciled.
	 */
	F_SET(page, WT_PAGE_MERGE);

	/* Enter each split page into the new, internal page. */
	for (cref = page->u.col_int.t,
	    bnd = r->bnd, i = 0; i < r->bnd_next; ++cref, ++bnd, ++i) {
		cref->recno = bnd->recno;
		WT_COL_REF_ADDR(cref) = bnd->off.addr;
		WT_COL_REF_SIZE(cref) = bnd->off.size;

		/*
		 * If there's an in-memory version of the page, connect it to
		 * this parent and fix up its in-memory references.
		 */
		if (bnd->imp_dsk == NULL) {
			WT_COL_REF_PAGE(cref) = NULL;
			WT_COL_REF_STATE(cref) = WT_REF_DISK;
		} else {
			WT_RET(__rec_imref_fixup(
			    session, bnd, page, &cref->ref, &imp));
			WT_COL_REF_PAGE(cref) = imp;
			WT_COL_REF_STATE(cref) = WT_REF_MEM;
		}

		WT_VERBOSE(session, EVICT,
		    "split: %" PRIu32 " (%" PRIu32 "B), "
		    "starting record %" PRIu64,
		    bnd->off.addr, bnd->off.size, bnd->recno);
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
__rec_imref_fixup(WT_SESSION_IMPL *session,
    WT_BOUNDARY *bnd, WT_PAGE *parent, WT_REF *parent_ref, WT_PAGE **pagep)
{
	WT_COL_REF *cref;
	WT_PAGE *page;
	WT_RECONCILE *r;
	WT_REF **searchp, *ref;
	WT_ROW_REF *rref;
	uint32_t i;

	r = S2C(session)->cache->rec;

	/*
	 * Create the in-memory version of the page from the disk image.  We'll
	 * free this page on error, so clear the disk image reference to ensure
	 * we don't free it twice.
	 */
	WT_RET(
	    __wt_page_inmem(session, parent, parent_ref, bnd->imp_dsk, &page));
	bnd->imp_dsk = NULL;
	*pagep = page;

	/*
	 * If we've found all the in-memory references we expect to find, don't
	 * keep searching.
	 */
	if (r->imref_found >= r->imref_next)
		return (0);

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
__rec_imref_add(WT_SESSION_IMPL *session, WT_REF *ref)
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
 * __rec_discard_add_ovfl --
 *	If the cell argument references an overflow chunk, schedule it for
 * discard.
 */
static inline int
__rec_discard_add_ovfl(WT_SESSION_IMPL *session, WT_CELL *cell)
{
	WT_OFF ovfl;

	if (__wt_cell_type_is_ovfl(cell)) {
		__wt_cell_off(cell, &ovfl);
		return (__rec_discard_add(session, NULL, ovfl.addr, ovfl.size));
	}
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
__rec_discard_add(
    WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t addr, uint32_t size)
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
__rec_discard_evict(WT_SESSION_IMPL *session)
{
	WT_DISCARD *discard;
	WT_RECONCILE *r;
	uint32_t i;

	r = S2C(session)->cache->rec;

	for (discard = r->discard, i = 0; i < r->discard_next; ++discard, ++i)
		if (discard->page == NULL)
			WT_RET(__wt_block_free(
			    session, discard->addr, discard->size));
		else {
			WT_VERBOSE(session, EVICT,
			    "discard addr %" PRIu32 "/%" PRIu32 " (%s)",
			    discard->addr, discard->size,
			    __wt_page_type_string(discard->page->type));
			__wt_page_free(session, discard->page, 0);
		}
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
__hazard_copy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_RECONCILE *r;
	uint32_t elem, i, j;

	r = S2C(session)->cache->rec;
	conn = S2C(session);

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
__hazard_exclusive(WT_SESSION_IMPL *session, WT_REF *ref)
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
