/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_PAGE_HEADER --
 *	Blocks have a common header, a WT_PAGE_HEADER structure followed by a
 * block-manager specific structure.
 */
struct __wt_page_header {
	/*
	 * The record number of the first record of the page is stored on disk
	 * so we can figure out where the column-store leaf page fits into the
	 * key space during salvage.
	 */
	uint64_t recno;			/* 00-07: column-store starting recno */

	/*
	 * We maintain page write-generations in the non-transactional case
	 * as that's how salvage can determine the most recent page between
	 * pages overlapping the same key range.
	 */
	uint64_t write_gen;		/* 08-15: write generation */

	/*
	 * The page's in-memory size isn't rounded or aligned, it's the actual
	 * number of bytes the disk-image consumes when instantiated in memory.
	 */
	uint32_t mem_size;		/* 16-19: in-memory page size */

	union {
		uint32_t entries;	/* 20-23: number of cells on page */
		uint32_t datalen;	/* 20-23: overflow data length */
	} u;

	uint8_t type;			/* 24: page type */

#define	WT_PAGE_COMPRESSED	0x01	/* Page is compressed on disk */
	uint8_t flags;			/* 25: flags */

	/*
	 * End the structure with 2 bytes of padding: it wastes space, but it
	 * leaves the structure 32-bit aligned and having a few bytes to play
	 * with in the future can't hurt.
	 */
	uint8_t unused[2];		/* 26-27: unused padding */
};
/*
 * WT_PAGE_HEADER_SIZE is the number of bytes we allocate for the structure: if
 * the compiler inserts padding it will break the world.
 */
#define	WT_PAGE_HEADER_SIZE		28

/*
 * The block-manager specific information immediately follows the WT_PAGE_DISK
 * structure.
 */
#define	WT_BLOCK_HEADER_REF(dsk)					\
	((void *)((uint8_t *)(dsk) + WT_PAGE_HEADER_SIZE))

/*
 * WT_PAGE_HEADER_BYTE --
 * WT_PAGE_HEADER_BYTE_SIZE --
 *	The first usable data byte on the block (past the combined headers).
 */
#define	WT_PAGE_HEADER_BYTE_SIZE(btree)					\
	((u_int)(WT_PAGE_HEADER_SIZE + (btree)->block_header))
#define	WT_PAGE_HEADER_BYTE(btree, dsk)					\
	((void *)((uint8_t *)(dsk) + WT_PAGE_HEADER_BYTE_SIZE(btree)))

/*
 * WT_ADDR --
 *	An in-memory structure to hold a block's location.
 */
struct __wt_addr {
	uint8_t *addr;			/* Block-manager's cookie */
	uint32_t size;			/* Block-manager's cookie length */
	uint8_t  leaf_no_overflow;	/* 1/0: a leaf page w/o overflow */
};

/*
 * WT_PAGE_MODIFY --
 *	When a page is modified, there's additional information maintained as it
 * is written to disk.
 */
struct __wt_page_modify {
	/*
	 * The write generation is incremented after a page is modified.  That
	 * is, it tracks page versions.
	 *
	 * The write generation value is used to detect changes scheduled based
	 * on out-of-date information.  Two threads of control updating the same
	 * page could both search the page in state A.  When the updates are
	 * performed serially, one of the changes will happen after the page is
	 * modified, and the search state for the other thread might no longer
	 * be applicable.  To avoid this race, page write generations are copied
	 * into the search stack whenever a page is read, and check when a
	 * modification is serialized.  The serialized function compares each
	 * page's current write generation to the generation copied in the
	 * read/search; if the two values match, the search occurred on a
	 * current version of the page and the modification can proceed.  If the
	 * two generations differ, the serialized call returns an error and the
	 * operation must be restarted.
	 *
	 * The write-generation value could be stored on a per-entry basis if
	 * there's sufficient contention for the page as a whole.
	 *
	 * The write-generation is not declared volatile: write-generation is
	 * written by a serialized function when modifying a page, and must be
	 * flushed in order as the serialized updates are flushed.
	 *
	 * !!!
	 * 32-bit values are probably more than is needed: at some point we may
	 * need to clean up pages once there have been sufficient modifications
	 * to make our linked lists of inserted cells too slow to search, or as
	 * soon as enough memory is allocated in service of page modifications
	 * (although we should be able to release memory from the MVCC list as
	 * soon as there's no running thread/txn which might want that version
	 * of the data).   I've used 32-bit types instead of 16-bit types as I
	 * am less confident a 16-bit write to memory will invariably be atomic.
	 */
	uint32_t write_gen;

	/*
	 * The disk generation tracks page versions written to disk.  When a
	 * page is reconciled and written to disk, the thread doing that work
	 * is just another reader of the page, and other readers and writers
	 * can access the page at the same time.  For this reason, the thread
	 * reconciling the page logs the write generation of the page it read.
	 */
	uint32_t disk_gen;

	/*
	 * Track the highest transaction ID at which the page was written to
	 * disk.  This can be used to avoid trying to write the page multiple
	 * times if a snapshot is keeping old versions pinned (e.g., in a
	 * checkpoint).
	 */
	wt_txnid_t disk_txn;

	union {
		WT_PAGE *split;		/* Resulting split */
		WT_ADDR	 replace;	/* Resulting replacement */
	} u;

	/*
	 * Appended items to column-stores: there is only a single one of these
	 * per column-store tree.
	 */
	WT_INSERT_HEAD **append;	/* Appended items */

	/*
	 * Updated items in column-stores: variable-length RLE entries can
	 * expand to multiple entries which requires some kind of list we can
	 * expand on demand.  Updated items in fixed-length files could be done
	 * based on an WT_UPDATE array as in row-stores, but there can be a
	 * very large number of bits on a single page, and the cost of the
	 * WT_UPDATE array would be huge.
	 */
	WT_INSERT_HEAD **update;	/* Updated items */

	/*
	 * Track pages, blocks to discard: as pages are reconciled, overflow
	 * K/V items are discarded along with their underlying blocks, and as
	 * pages are evicted, split and emptied pages are merged into their
	 * parents and discarded.  If an overflow item was discarded and page
	 * reconciliation then failed, the in-memory tree would be corrupted.
	 * To keep the tree correct until we're sure page reconciliation has
	 * succeeded, we track the objects we'll discard when the reconciled
	 * page is evicted.
	 *
	 * Track overflow objects: if pages are reconciled more than once, an
	 * overflow item might be written repeatedly.  Instead, when overflow
	 * items are written we save a copy and resulting location so we only
	 * write them once.
	 */
	struct __wt_page_track {
		WT_ADDR  addr;		/* Overflow or block location */

		uint8_t *data;		/* Overflow data reference */
		uint32_t size;		/* Overflow data length */

#define	WT_TRK_DISCARD		0x001	/* Object was discarded */
#define	WT_TRK_INUSE		0x002	/* Object is currently in-use */
#define	WT_TRK_JUST_ADDED	0x004	/* Object added this reconciliation */
#define	WT_TRK_OBJECT		0x008	/* Slot set (not empty) */
#define	WT_TRK_ONPAGE		0x010	/* Object was referenced from a page */
#define	WT_TRK_OVFL_VALUE	0x020	/* Cached deleted overflow value */
		uint8_t  flags;
	} *track;			/* Array of tracked objects */
	uint32_t track_entries;		/* Total track slots */

	wt_txnid_t first_id;		/* Earliest transactional update, used
					 * to avoid errors from transaction ID
					 * wraparound.
					 */

#define	WT_PM_REC_EMPTY		0x01	/* Reconciliation: page empty */
#define	WT_PM_REC_REPLACE	0x02	/* Reconciliation: page replaced */
#define	WT_PM_REC_SPLIT		0x04	/* Reconciliation: page split */
#define	WT_PM_REC_SPLIT_MERGE	0x08	/* Reconciliation: page split merge */
#define	WT_PM_REC_MASK							\
	(WT_PM_REC_EMPTY |						\
	    WT_PM_REC_REPLACE | WT_PM_REC_SPLIT | WT_PM_REC_SPLIT_MERGE)
	uint8_t flags;			/* Page flags */
};

/*
 * WT_PAGE --
 * The WT_PAGE structure describes the in-memory page information.
 */
struct __wt_page {
	/*
	 * Two links to the parent: the physical parent page, and the internal
	 * page's reference structure used to find this page.
	 */
#define	WT_PAGE_IS_ROOT(page)						\
	((page)->parent == NULL)
	WT_PAGE	*parent;			/* Page's parent */
	WT_REF	*ref;				/* Parent reference */

	/* Per page-type information. */
	union {
		/*
		 * Column- and row-store internal page.  The recno is only used
		 * by column-store, but having the WT_REF array in the same page
		 * location makes some things simpler, and it doesn't cost us
		 * any memory, other structures in this union are still larger.
		 */
		struct {
			uint64_t    recno;	/* Starting recno */
			WT_REF *t;		/* Subtree */
		} intl;

		/* Row-store leaf page. */
		struct {
			WT_ROW	   *d;		/* K/V object pairs */

			/*
			 * The column-store leaf page modification structures
			 * live in the WT_PAGE_MODIFY structure to keep the
			 * WT_PAGE structure as small as possible for read-only
			 * pages.  For consistency, we could move the row-store
			 * modification structures into WT_PAGE_MODIFY too, but
			 * that doesn't shrink WT_PAGE any further and it would
			 * require really ugly naming inside of WT_PAGE_MODIFY
			 * to avoid growing that structure.
			 */
			WT_INSERT_HEAD	**ins;	/* Inserts */
			WT_UPDATE	**upd;	/* Updates */
		} row;

		/* Fixed-length column-store leaf page. */
		struct {
			uint64_t    recno;	/* Starting recno */
			uint8_t	   *bitf;	/* COL_FIX items */
		} col_fix;

		/* Variable-length column-store leaf page. */
		struct {
			uint64_t    recno;	/* Starting recno */
			WT_COL	   *d;		/* COL_VAR items */

			/*
			 * Variable-length column-store files maintain a list of
			 * RLE entries on the page so it's unnecessary to walk
			 * the page counting records to find a specific entry.
			 */
			WT_COL_RLE *repeats;	/* RLE array for lookups */
			uint32_t    nrepeats;	/* Number of repeat slots. */
		} col_var;
	} u;

	/* Page's on-disk representation: NULL for pages created in memory. */
	WT_PAGE_HEADER *dsk;

	/* If/when the page is modified, we need lots more information. */
	WT_PAGE_MODIFY *modify;

	/*
	 * The page's read generation acts as an LRU value for each page in the
	 * tree; it is used by the eviction server thread to select pages to be
	 * discarded from the in-memory tree.
	 *
	 * The read generation is a 64-bit value, if incremented frequently, a
	 * 32-bit value could overflow.
	 *
	 * The read generation is a piece of shared memory potentially accessed
	 * by many threads.  We don't want to update page read generations for
	 * in-cache workloads and suffer the cache misses, so we don't simply
	 * increment the read generation value on every access.  Instead, the
	 * read generation is initialized to 0, then set to a real value if the
	 * page is ever considered for eviction.  Once set to a real value, the
	 * read generation is potentially incremented every time the page is
	 * accessed.  To try and avoid incrementing the page at a fast rate in
	 * this case, the read generation is incremented to a future point.
	 *
	 * The read generation is not declared volatile or published: the read
	 * generation is set a lot, and we don't want to write it that much.
	 */
#define	WT_READ_GEN_NOTSET	0
#define	WT_READ_GEN_OLDEST	1
#define	WT_READ_GEN_STEP	1000
	uint64_t read_gen;

	/*
	 * In-memory pages optionally reference a number of entries originally
	 * read from disk and sizes the allocated arrays that describe the page.
	 */
	uint32_t entries;

	/* Memory attached to the page. */
	uint32_t memory_footprint;

#define	WT_PAGE_INVALID		0	/* Invalid page */
#define	WT_PAGE_BLOCK_MANAGER	1	/* Block-manager page */
#define	WT_PAGE_COL_FIX		2	/* Col-store fixed-len leaf */
#define	WT_PAGE_COL_INT		3	/* Col-store internal page */
#define	WT_PAGE_COL_VAR		4	/* Col-store var-length leaf page */
#define	WT_PAGE_OVFL		5	/* Overflow page */
#define	WT_PAGE_ROW_INT		6	/* Row-store internal page */
#define	WT_PAGE_ROW_LEAF	7	/* Row-store leaf page */
	uint8_t type;			/* Page type */

#define	WT_PAGE_BUILD_KEYS	0x01	/* Keys have been built in memory */
#define	WT_PAGE_DISK_NOT_ALLOC	0x02	/* Ignore disk image on page discard */
#define	WT_PAGE_EVICT_LRU	0x04	/* Page is on the LRU queue */
	uint8_t flags_atomic;		/* Atomic flags, use F_*_ATOMIC */
};

/*
 * WT_PAGE_DISK_OFFSET, WT_PAGE_REF_OFFSET --
 *	Return the offset/pointer of a pointer/offset in a page disk image.
 */
#define	WT_PAGE_DISK_OFFSET(page, p)					\
	WT_PTRDIFF32(p, (page)->dsk)
#define	WT_PAGE_REF_OFFSET(page, o)					\
	((void *)((uint8_t *)((page)->dsk) + (o)))

/*
 * Page state.
 *
 * Synchronization is based on the WT_REF->state field, which has a number of
 * possible states:
 *
 * WT_REF_DISK:
 *	The initial setting before a page is brought into memory, and set as a
 *	result of page eviction; the page is on disk, and must be read into
 *	memory before use.  WT_REF_DISK has a value of 0 (the default state
 *	after allocating cleared memory).
 *
 * WT_REF_DELETED:
 *	The page is on disk, but has been deleted from the tree; we can delete
 *	row-store leaf pages without reading them if they don't reference
 *	overflow items.
 *
 * WT_REF_EVICT_FORCE:
 *	An application thread has selected this page for eviction. No other
 *	hazard references should be granted. If eviction fails, the eviction
 *	server should set the state back to WT_REF_MEM.
 *
 * WT_REF_EVICT_WALK:
 *	The next page to be walked for LRU eviction.  This page is available
 *	for reads but not eviction.
 *
 * WT_REF_LOCKED:
 *	Locked for exclusive access.  In eviction, this page or a parent has
 *	been selected for eviction; once hazard pointers are checked, the page
 *	will be evicted.  When reading a page that was previously deleted, it
 *	is locked until the page is in memory with records marked deleted.  The
 *	thread that set the page to WT_REF_LOCKED has exclusive access, no
 *	other thread may use the WT_REF until the state is changed.
 *
 * WT_REF_MEM:
 *	Set by a reading thread once the page has been read from disk; the page
 *	is in the cache and the page reference is OK.
 *
 * WT_REF_READING:
 *	Set by a reading thread before reading an ordinary page from disk;
 *	other readers of the page wait until the read completes.  Sync can
 *	safely skip over such pages: they are clean by definition.
 *
 * The life cycle of a typical page goes like this: pages are read into memory
 * from disk and their state set to WT_REF_MEM.  When the page is selected for
 * eviction, the page state is set to WT_REF_LOCKED.  In all cases, evicting
 * threads reset the page's state when finished with the page: if eviction was
 * successful (a clean page was discarded, and a dirty page was written to disk
 * and then discarded), the page state is set to WT_REF_DISK; if eviction failed
 * because the page was busy, page state is reset to WT_REF_MEM.
 *
 * Readers check the state field and if it's WT_REF_MEM, they set a hazard
 * pointer to the page, flush memory and re-confirm the page state.  If the
 * page state is unchanged, the reader has a valid reference and can proceed.
 *
 * When an evicting thread wants to discard a page from the tree, it sets the
 * WT_REF_LOCKED state, flushes memory, then checks hazard pointers.  If a
 * hazard pointer is found, state is reset to WT_REF_MEM, restoring the page
 * to the readers.  If the evicting thread does not find a hazard pointer,
 * the page is evicted.
 */
enum __wt_page_state {
	WT_REF_DISK=0,			/* Page is on disk */
	WT_REF_DELETED,			/* Page is on disk, but deleted */
	WT_REF_EVICT_FORCE,		/* Page is ready for forced eviction */
	WT_REF_EVICT_WALK,		/* Next page for LRU eviction */
	WT_REF_LOCKED,			/* Page locked for exclusive access */
	WT_REF_MEM,			/* Page is in cache and valid */
	WT_REF_READING			/* Page being read */
};

/*
 * WT_REF --
 *	A single in-memory page and the state information used to determine if
 * it's OK to dereference the pointer to the page.
 */
struct __wt_ref {
	WT_PAGE *page;			/* In-memory page */

	void	*addr;			/* On-page cell or off_page WT_ADDR */

	union {
		uint64_t recno;		/* Column-store: starting recno */
		void	*key;		/* Row-store: on-page cell or WT_IKEY */
	} u;
	wt_txnid_t txnid;		/* Transaction ID */

	volatile WT_PAGE_STATE state;	/* Page state */
};

/*
 * WT_REF_FOREACH --
 * Walk the subtree array of an in-memory internal page.
 */
#define	WT_REF_FOREACH(page, ref, i)					\
	for ((i) = (page)->entries,					\
	    (ref) = (page)->u.intl.t; (i) > 0; ++(ref), --(i))

/*
 * WT_LINK_PAGE --
 * Link a child page into a reference in its parent.
 */
#define	WT_LINK_PAGE(ppage, pref, cpage) do {				\
	(pref)->page = (cpage);						\
	(cpage)->parent = (ppage);					\
	(cpage)->ref = (pref);						\
} while (0)

/*
 * WT_MERGE_STACK_MIN --
 * When stacks of in-memory pages become this deep, they are considered for
 * merging.
 *
 * WT_MERGE_FULL_PAGE --
 * When the result of a merge contains more than this number of keys, it is
 * considered "done" and will not be merged again.
 */
#define	WT_MERGE_STACK_MIN	3
#define	WT_MERGE_FULL_PAGE	100

/*
 * WT_ROW --
 * Each in-memory page row-store leaf page has an array of WT_ROW structures:
 * this is created from on-page data when a page is read from the file.  It's
 * sorted by key, fixed in size, and references data on the page.
 *
 * Multiple threads of control may be searching the in-memory row-store pages,
 * and the key may be instantiated at any time.  Code must be able to handle
 * both when the key has not been instantiated (the key field points into the
 * page's disk image), and when the key has been instantiated (the key field
 * points outside the page's disk image).  We don't need barriers because the
 * key is updated atomically, but code that reads the key field multiple times
 * is a very, very bad idea.  Specifically, do not do this:
 *
 *	key = rip->key;
 *	if (key_is_on_page(key)) {
 *		cell = rip->key;
 *	}
 *
 * The field is declared volatile (so the compiler knows it shouldn't read it
 * multiple times), and we obscure the field name and use a copy macro in all
 * references to the field (so the code doesn't read it multiple times), all
 * to make sure we don't introduce this bug (again).
 *
 * Casting the read to a (void *) is safe as we are not taking the address of
 * the object.
 */
struct __wt_row {
	void * volatile __key;		/* On-page cell or off-page WT_IKEY */
};
#define	WT_ROW_KEY_COPY(rip)	((rip)->__key)
#define	WT_ROW_KEY_SET(rip, v)	((rip)->__key) = (v)

/*
 * WT_ROW_FOREACH --
 *	Walk the entries of an in-memory row-store leaf page.
 */
#define	WT_ROW_FOREACH(page, rip, i)					\
	for ((i) = (page)->entries,					\
	    (rip) = (page)->u.row.d; (i) > 0; ++(rip), --(i))
#define	WT_ROW_FOREACH_REVERSE(page, rip, i)				\
	for ((i) = (page)->entries,					\
	    (rip) = (page)->u.row.d + ((page)->entries - 1);		\
	    (i) > 0; --(rip), --(i))

/*
 * WT_ROW_SLOT --
 *	Return the 0-based array offset based on a WT_ROW reference.
 */
#define	WT_ROW_SLOT(page, rip)						\
	((uint32_t)(((WT_ROW *)rip) - (page)->u.row.d))

/*
 * WT_COL --
 * Each in-memory variable-length column-store leaf page has an array of WT_COL
 * structures: this is created from on-page data when a page is read from the
 * file.  It's fixed in size, and references data on the page.
 */
struct __wt_col {
	/*
	 * Variable-length column-store data references are page offsets, not
	 * pointers (we boldly re-invent short pointers).  The trade-off is 4B
	 * per K/V pair on a 64-bit machine vs. a single cycle for the addition
	 * of a base pointer.  The on-page data is a WT_CELL (same as row-store
	 * pages).
	 *
	 * If the value is 0, it's a single, deleted record.
	 *
	 * Obscure the field name, code shouldn't use WT_COL->value, the public
	 * interface is WT_COL_PTR.
	 */
	uint32_t __value;
};

/*
 * WT_COL_RLE --
 * In variable-length column store leaf pages, we build an array of entries
 * with RLE counts greater than 1 when reading the page.  We can do a binary
 * search in this array, then an offset calculation to find the cell.
 */
struct __wt_col_rle {
	uint64_t recno;			/* Record number of first repeat. */
	uint64_t rle;			/* Repeat count. */
	uint32_t indx;			/* Slot of entry in col_var.d */
} WT_GCC_ATTRIBUTE((packed));

/*
 * WT_COL_PTR --
 *	Return a pointer corresponding to the data offset -- if the item doesn't
 * exist on the page, return a NULL.
 */
#define	WT_COL_PTR(page, cip)						\
	((cip)->__value == 0 ? NULL : WT_PAGE_REF_OFFSET(page, (cip)->__value))

/*
 * WT_COL_FOREACH --
 *	Walk the entries of variable-length column-store leaf page.
 */
#define	WT_COL_FOREACH(page, cip, i)					\
	for ((i) = (page)->entries,					\
	    (cip) = (page)->u.col_var.d; (i) > 0; ++(cip), --(i))

/*
 * WT_COL_SLOT --
 *	Return the 0-based array offset based on a WT_COL reference.
 */
#define	WT_COL_SLOT(page, cip)						\
	((uint32_t)(((WT_COL *)cip) - (page)->u.col_var.d))

/*
 * WT_IKEY --
 * Instantiated key: row-store keys are usually prefix compressed and sometimes
 * Huffman encoded or overflow objects.  Normally, a row-store page in-memory
 * key points to the on-page WT_CELL, but in some cases, we instantiate the key
 * in memory, in which case the row-store page in-memory key points to a WT_IKEY
 * structure.
 */
struct __wt_ikey {
	uint32_t size;			/* Key length */

	/*
	 * If we no longer point to the key's on-page WT_CELL, we can't find its
	 * related value.  Save the offset of the key cell in the page.
	 *
	 * Row-store cell references are page offsets, not pointers (we boldly
	 * re-invent short pointers).  The trade-off is 4B per K/V pair on a
	 * 64-bit machine vs. a single cycle for the addition of a base pointer.
	 */
	uint32_t  cell_offset;

	/* The key bytes immediately follow the WT_IKEY structure. */
#define	WT_IKEY_DATA(ikey)						\
	((void *)((uint8_t *)(ikey) + sizeof(WT_IKEY)))
};

/*
 * WT_UPDATE --
 * Entries on leaf pages can be updated, either modified or deleted.  Updates
 * to entries referenced from the WT_ROW and WT_COL arrays are stored in the
 * page's WT_UPDATE array.  When the first element on a page is updated, the
 * WT_UPDATE array is allocated, with one slot for every existing element in
 * the page.  A slot points to a WT_UPDATE structure; if more than one update
 * is done for an entry, WT_UPDATE structures are formed into a forward-linked
 * list.
 */
struct __wt_update {
	/*
	 * We use the maximum size as an is-deleted flag, which means we can't
	 * store 4GB objects; I'd rather do that than increase the size of this
	 * structure for a flag bit.
	 */
#define	WT_UPDATE_DELETED_ISSET(upd)	((upd)->size == UINT32_MAX)
#define	WT_UPDATE_DELETED_SET(upd)	((upd)->size = UINT32_MAX)
	uint32_t size;			/* update length */
	wt_txnid_t txnid;		/* update transaction */

	WT_UPDATE *next;		/* forward-linked list */

	/* The untyped value immediately follows the WT_UPDATE structure. */
#define	WT_UPDATE_DATA(upd)						\
	((void *)((uint8_t *)(upd) + sizeof(WT_UPDATE)))
};

/*
 * WT_INSERT --
 *
 * Row-store leaf pages support inserts of new K/V pairs.  When the first K/V
 * pair is inserted, the WT_INSERT_HEAD array is allocated, with one slot for
 * every existing element in the page, plus one additional slot.  A slot points
 * to a WT_INSERT_HEAD structure for the items which sort after the WT_ROW
 * element that references it and before the subsequent WT_ROW element; the
 * skiplist structure has a randomly chosen depth of next pointers in each
 * inserted node.
 *
 * The additional slot is because it's possible to insert items smaller than any
 * existing key on the page: for that reason, the first slot of the insert array
 * holds keys smaller than any other key on the page.
 *
 * In column-store variable-length run-length encoded pages, a single indx
 * entry may reference a large number of records, because there's a single
 * on-page entry representing many identical records.   (We don't expand those
 * entries when the page comes into memory, as that would require resources as
 * pages are moved to/from the cache, including read-only files.)  Instead, a
 * single indx entry represents all of the identical records originally found
 * on the page.
 *
 * Modifying (or deleting) run-length encoded column-store records is hard
 * because the page's entry no longer references a set of identical items.  We
 * handle this by "inserting" a new entry into the insert array, with its own
 * record number.  (This is the only case where it's possible to insert into a
 * column-store: only appends are allowed, as insert requires re-numbering
 * subsequent records.  Berkeley DB did support mutable records, but it won't
 * scale and it isn't useful enough to re-implement, IMNSHO.)
 */
struct __wt_insert {
	WT_UPDATE *upd;				/* value */

	union {
		uint64_t recno;			/* column-store record number */
		struct {
			uint32_t offset;	/* row-store key data start */
			uint32_t size;		/* row-store key data size */
		} key;
	} u;

#define	WT_INSERT_KEY_SIZE(ins) ((ins)->u.key.size)
#define	WT_INSERT_KEY(ins)						\
	((void *)((uint8_t *)(ins) + (ins)->u.key.offset))
#define	WT_INSERT_RECNO(ins)	((ins)->u.recno)

	WT_INSERT *next[0];			/* forward-linked skip list */
};

/*
 * Skiplist helper macros.
 */
#define	WT_SKIP_FIRST(ins_head)						\
	(((ins_head) == NULL) ? NULL : (ins_head)->head[0])
#define	WT_SKIP_LAST(ins_head)						\
	(((ins_head) == NULL) ? NULL : (ins_head)->tail[0])
#define	WT_SKIP_NEXT(ins)  ((ins)->next[0])
#define	WT_SKIP_FOREACH(ins, ins_head)					\
	for ((ins) = WT_SKIP_FIRST(ins_head);				\
	    (ins) != NULL;						\
	    (ins) = WT_SKIP_NEXT(ins))

/*
 * WT_INSERT_HEAD --
 * 	The head of a skiplist of WT_INSERT items.
 */
struct __wt_insert_head {
	WT_INSERT *head[WT_SKIP_MAXDEPTH];	/* first item on skiplists */
	WT_INSERT *tail[WT_SKIP_MAXDEPTH];	/* last item on skiplists */
};

/*
 * The row-store leaf page insert lists are arrays of pointers to structures,
 * and may not exist.  The following macros return an array entry if the array
 * of pointers and the specific structure exist, else NULL.
 */
#define	WT_ROW_INSERT_SLOT(page, slot)					\
	((page)->u.row.ins == NULL ? NULL : (page)->u.row.ins[slot])
#define	WT_ROW_INSERT(page, ip)						\
	WT_ROW_INSERT_SLOT(page, WT_ROW_SLOT(page, ip))
#define	WT_ROW_UPDATE(page, ip)						\
	((page)->u.row.upd == NULL ?					\
	    NULL : (page)->u.row.upd[WT_ROW_SLOT(page, ip)])
/*
 * WT_ROW_INSERT_SMALLEST references an additional slot past the end of the
 * the "one per WT_ROW slot" insert array.  That's because the insert array
 * requires an extra slot to hold keys that sort before any key found on the
 * original page.
 */
#define	WT_ROW_INSERT_SMALLEST(page)					\
	((page)->u.row.ins == NULL ? NULL : (page)->u.row.ins[(page)->entries])

/*
 * The column-store leaf page update lists are arrays of pointers to structures,
 * and may not exist.  The following macros return an array entry if the array
 * of pointers and the specific structure exist, else NULL.
 */
#define	WT_COL_UPDATE_SLOT(page, slot)					\
	((page)->modify == NULL || (page)->modify->update == NULL ?	\
	    NULL : (page)->modify->update[slot])
#define	WT_COL_UPDATE(page, ip)						\
	WT_COL_UPDATE_SLOT(page, WT_COL_SLOT(page, ip))

/*
 * WT_COL_UPDATE_SINGLE is a single WT_INSERT list, used for any fixed-length
 * column-store updates for a page.
 */
#define	WT_COL_UPDATE_SINGLE(page)					\
	WT_COL_UPDATE_SLOT(page, 0)

/*
 * WT_COL_APPEND is an WT_INSERT list, used for fixed- and variable-length
 * appends.
 */
#define	WT_COL_APPEND(page)						\
	((page)->modify != NULL &&					\
	    (page)->modify->append != NULL ? (page)->modify->append[0] : NULL)

/* WT_FIX_FOREACH walks fixed-length bit-fields on a disk page. */
#define	WT_FIX_FOREACH(btree, dsk, v, i)				\
	for ((i) = 0,							\
	    (v) = (i) < (dsk)->u.entries ?				\
	    __bit_getv(							\
	    WT_PAGE_HEADER_BYTE(btree, dsk), 0, (btree)->bitcnt) : 0;	\
	    (i) < (dsk)->u.entries; ++(i),				\
	    (v) = __bit_getv(						\
	    WT_PAGE_HEADER_BYTE(btree, dsk), i, (btree)->bitcnt))
