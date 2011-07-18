/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * In WiredTiger there are "file allocation units", which is the smallest file
 * chunk that can be allocated.  The smallest file allocation unit is 512B; the
 * largest is 128MB.  (The maximum of 128MB is enforced by the software, it
 * could be set as high as 4GB.)  Btree leaf and internal pages, as well as
 * overflow chunks, are allocated in groups of 1 or more allocation units.
 *
 * We use 32-bit unsigned integers to store file locations on file pages, and
 * all such file locations are counts of file allocation units.  In the code
 * these are called "addrs".  To simplify bookkeeping, page sizes must also be
 * a multiple of the allocation unit size.
 */
#define	WT_ADDR_INVALID		(UINT32_MAX - 0)	/* Invalid address */

/*
 * The minimum maximum file size is almost 2TB (2^9 x (2^32 - 2)), and the
 * maximum maximum file size is almost 512PB (2^27 x 2^32 - 2).
 *
 * In summary, small file allocation units limit the file size, (but minimize
 * wasted space when storing overflow items), and when the allocation unit
 * grows, the maximum size of the file grows as well.
 *
 * The minimum btree leaf and internal page sizes are 512B, the maximum 512MB.
 * (The maximum of 512MB is enforced by the software, it could be set as high
 * as 4GB.)
 *
 * Key and data item lengths are stored in 32-bit unsigned integers, meaning
 * the largest key or data item is 4GB (minus a few bytes).  Record numbers
 * are stored in 64-bit unsigned integers, meaning the largest record number
 * is "really, really big".
 */
#define	WT_BTREE_ALLOCATION_SIZE_MIN	(512)
#define	WT_BTREE_ALLOCATION_SIZE_MAX	(128 * WT_MEGABYTE)
#define	WT_BTREE_PAGE_SIZE_MAX		(512 * WT_MEGABYTE)

/* The file's description is written into the first 512B of the file. */
#define	WT_BTREE_DESC_SECTOR		512

/*
 * Limit the maximum size of a single object to 4GB - 512B: in some places we
 * allocate memory to store objects plus associated data structures, in other
 * places we need out-of-band values in object sizes.   512B is far more space
 * than we ever need, but I'm not eager to debug any off-by-ones, and storing
 * a 4GB object in the file is flatly insane, anyway.
 */
#define	WT_BTREE_OBJECT_SIZE_MAX	(UINT32_MAX - 512)

/*
 * Underneath the Btree code is the OS layer, where sizes are stored as numbers
 * of bytes.   In the OS layer, 32-bits is too small (a file might be larger
 * than 4GB), so we use a standard type known to hold the size of a file, off_t.
 *
 * The first 512B of the file hold the file's description.  Since I don't want
 * to give up a full allocation-size to the file's description, we offset addrs
 * by 512B.
 */
/* Convert a data address to/from a byte offset. */
#define	WT_ADDR_TO_OFF(btree, addr)					\
	(WT_BTREE_DESC_SECTOR + (off_t)(addr) * (off_t)(btree)->allocsize)
#define	WT_OFF_TO_ADDR(btree, off)					\
	((uint32_t)((off - WT_BTREE_DESC_SECTOR) / (btree)->allocsize))
#define	WT_FILE_OFF_MAX(btree)						\
	WT_ADDR_TO_OFF(btree, UINT32_MAX - 1)

/*
 * WT_BTREE_DESC --
 *	The file's description.
 */
struct __wt_btree_desc {
#define	WT_BTREE_MAGIC		120897
	uint32_t magic;			/* 00-03: Magic number */
#define	WT_BTREE_MAJOR_VERSION	0
	uint16_t majorv;		/* 04-05: Major version */
#define	WT_BTREE_MINOR_VERSION	1
	uint16_t minorv;		/* 06-07: Minor version */

	/*
	 * We store two page addr/size pairs: the root page for the tree and
	 * the free-list.
	 */
	uint32_t root_addr;		/* 08-11: Root page address */
	uint32_t root_size;		/* 12-15: Root page length */

	uint32_t free_addr;		/* 16-19: Free list page address */
	uint32_t free_size;		/* 20-23: Free list page length */

	/*
	 * We maintain page LSN's for the file in the non-transactional case
	 * (where, instead of a log reference, the LSN is simply a counter),
	 * as that's how salvage can determine the most recent page between
	 * pages overlapping the same key range.  This non-transactional LSN
	 * has to be persistent, and so it's included in the file's metadata.
	 */
	uint64_t lsn;			/* 24-32: Non-transactional page LSN */
};
/*
 * WT_BTREE_DESC_SIZE is the expected structure size -- we verify the build to
 * ensure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_BTREE_DESC_SIZE		32

/*
 * WT_DISK_REQUIRED--
 *	Return bytes needed for byte length, rounded to an allocation unit.
 */
#define	WT_DISK_REQUIRED(session, size)					\
	(WT_ALIGN((size) + WT_PAGE_DISK_SIZE, (session)->btree->allocsize))

/*
 * WT_PAGE_DISK --
 *
 * All on-disk pages have a common header, defined by the WT_PAGE_DISK
 * structure.  The header has no version number or mode bits, and the page type
 * and/or flags value will have to be modified when changes are made to the page
 * layout.  (The page type appears early in the header to make this simpler.)
 * In other words, the page type declares the contents of the page and how to
 * read it.
 */
struct __wt_page_disk {
	/*
	 * The record number of the first record of the page is stored on disk
	 * because, if the internal page referencing a column-store leaf page
	 * is corrupted, it's the only way to know where the leaf page fits in
	 * the key space during salvage.
	 */
	uint64_t recno;			/* 00-07: column-store starting recno */

	/*
	 * The LSN is a 64-bit chunk to make assignment and comparisons easier,
	 * but it's 2 32-bit values underneath: a file number and a file offset.
	 */
#define	WT_LSN_FILE(lsn)						\
	((uint32_t)(((lsn) & 0xffffffff00000000ULL) >> 32))
#define	WT_LSN_OFFSET(lsn)						\
	((uint32_t)((lsn) & 0xffffffff))
#define	WT_LSN_INCR(lsn)						\
	(++(lsn))
	uint64_t lsn;			/* 08-15: LSN file/offset pair */

	uint32_t checksum;		/* 16-19: checksum */

	/*
	 * We don't need the page length for normal processing as the page's
	 * parent knows how big it is.  However, we write the page size in the
	 * page header because it makes salvage easier.  (If we don't know the
	 * expected page length, we'd have to read increasingly larger chunks
	 * from the file until we find one that checksums, and that's going to
	 * be unpleasant given WiredTiger's large page sizes.)
	 */
	uint32_t size;			/* 20-23: size of page */

	/*
	 * If the page has been stream compressed, it has 2 sizes: the on-disk
	 * compressed size, and the in-memory size.  Store the in-memory size
	 * in the page header because otherwise we have no idea how big a chunk
	 * of memory we need to expand the page.
	 */
	uint32_t memsize;		/* 24-27: in-memory page size */

	union {
		uint32_t entries;	/* 28-31: number of cells on page */
		uint32_t datalen;	/* 28-31: overflow data length */
	} u;

	uint8_t type;			/* 32: page type */

	/*
	 * End the the WT_PAGE_DISK structure with 3 bytes of padding: it wastes
	 * space, but it leaves the WT_PAGE_DISK structure 32-bit aligned and
	 * having a small amount of space to play with in the future can't hurt.
	 */
	uint8_t unused[3];		/* 33-35: unused padding */
};
/*
 * WT_PAGE_DISK_SIZE is the expected structure size -- we verify the build to
 * ensure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_PAGE_DISK_SIZE		36

/*
 * WT_PAGE_DISK_BYTE --
 *	The first usable data byte on the page (past the header).
 */
#define	WT_PAGE_DISK_BYTE(dsk)						\
	((void *)((uint8_t *)(dsk) + WT_PAGE_DISK_SIZE))

/*
 * WT_DISK_OFFSET, WT_REF_OFFSET --
 *	Return the offset/pointer of a pointer/offset in a page disk image.
 */
#define	WT_DISK_OFFSET(dsk, p)						\
	((uint32_t)((uint8_t *)(p) - (uint8_t *)(dsk)))
#define	WT_REF_OFFSET(page, o)						\
	((void *)((uint8_t *)((page)->dsk) + (o)))

/*
 * WT_PAGE --
 * The WT_PAGE structure describes the in-memory information about a file page.
 */
struct __wt_page {
	/*
	 * Two links to the parent's WT_PAGE structure -- the physical parent
	 * page, and the WT_REF structure used to find this page.
	 */
#define	WT_PAGE_IS_ROOT(page)						\
	((page)->parent == NULL)
	WT_PAGE	*parent;		/* Page's parent */
	WT_REF	*parent_ref;		/* Page's parent reference */

	/*
	 * The read generation is incremented each time the page is searched,
	 * and acts as an LRU value for each page in the tree; it is read by
	 * the eviction server thread to select pages to be discarded from the
	 * in-memory tree.
	 *
	 * The read generation is a 64-bit value; incremented every time the
	 * page is searched, a 32-bit value could overflow.
	 *
	 * The read-generation is not declared volatile: read-generation is set
	 * a lot (on every access), and we don't want to write it that much.
	 */
	 uint64_t read_gen;

	/*
	 * The write generation is incremented after the workQ modifies a page
	 * that is, it tracks page versions.
	 *	The write generation value is used to detect changes scheduled
	 * based on out-of-date information.  Two threads of control updating
	 * the same page could both search the page in state A, and schedule
	 * the change for the workQ.  Since the workQ performs changes serially,
	 * one of the changes will happen after the page is modified, and the
	 * search state for the other thread might no longer be applicable.  To
	 * avoid this race, page write generations are copied into the search
	 * stack whenever a page is read, and passed to the workQ thread when a
	 * modification is scheduled.  The workQ thread compares each page's
	 * current write generation to the generation copied in the read/search;
	 * if the two values match, the search occurred on a current version of
	 * the page and the modification can proceed.  If the two generations
	 * differ, the workQ thread returns an error and the operation must be
	 * restarted.
	 *	The write-generation value could be stored on a per-entry basis
	 * if there's sufficient contention for the page as a whole.
	 *
	 * The write-generation is not declared volatile: write-generation is
	 * written by the workQ when modifying a page, and must be flushed in
	 * a specific order as the workQ flushes its changes.
	 *
	 * XXX
	 * 32-bit values are probably more than is needed: at some point we may
	 * need to clean up pages once there have been sufficient modifications
	 * to make our linked lists of inserted cells too slow to search, or as
	 * soon as enough memory is allocated in service of page modifications
	 * (although we should be able to release memory from the MVCC list as
	 * soon as there's no running thread/txn which might want that version
	 * of the data).   I've used 32-bit types instead of 16-bit types as I
	 * am less confident a 16-bit write to memory will be atomic.
	 */
#define	WT_PAGE_SET_MODIFIED(p) do {					\
	++(p)->write_gen;						\
	F_CLR(p, WT_PAGE_DELETED);					\
	F_SET(p, WT_PAGE_MODIFIED);					\
} while (0)
#define	WT_PAGE_IS_MODIFIED(p)	(F_ISSET(p, WT_PAGE_MODIFIED))
	uint32_t write_gen;

	/* But the entries are wildly different, based on the page type. */
	union {
		/* Row-store internal page. */
		struct {
			WT_ROW_REF *t;		/* Subtrees */
		} row_int;

		/* Row-store leaf page. */
		struct {
			WT_ROW	   *d;		/* K/V object pairs */
			WT_INSERT **ins;	/* Inserts */
			WT_UPDATE **upd;	/* Updates */
		} row_leaf;

		/* Column-store internal page. */
		struct {
			uint64_t    recno;	/* Starting recno */
			uint32_t    ext_entries;/* Extension entries */
			WT_COL_REF *t;		/* Subtrees */
		} col_int;

		/* Column-store leaf page. */
		struct {
			uint64_t    recno;	/* Starting recno */
			WT_COL	   *d;		/* V objects */
			WT_INSERT **ins;	/* Inserts (RLE) */
			WT_UPDATE **upd;	/* Updates */
		} col_leaf;

		/* Bulk-loaded linked list. */
		struct {
			uint64_t   recno;	/* Starting recno */
			WT_INSERT *ins;		/* Bulk-loaded K/V or V items */
			WT_UPDATE *upd;		/* Bulk-loaded V items */
		} bulk;
	} u;

	/* Page's on-disk representation: NULL for pages created in memory. */
	WT_PAGE_DISK *dsk;

	/*
	 * In-memory pages optionally reference a number of entries, originally
	 * read from disk.
	 */
	uint32_t entries;

	/*
	 * In-memory pages may have an optional memory allocation; this field
	 * is only so the appropriate calculations are done when the page is
	 * discarded.
	 */
	uint32_t memory_footprint;

#define	WT_PAGE_INVALID		0	/* Invalid page */
#define	WT_PAGE_COL_FIX		1	/* Col-store fixed-len leaf */
#define	WT_PAGE_COL_INT		2	/* Col-store internal page */
#define	WT_PAGE_COL_RLE		3	/* Col-store run-length encoded leaf */
#define	WT_PAGE_COL_VAR		4	/* Col-store var-length leaf page */
#define	WT_PAGE_OVFL		5	/* Overflow page */
#define	WT_PAGE_ROW_INT		6	/* Row-store internal page */
#define	WT_PAGE_ROW_LEAF	7	/* Row-store leaf page */
#define	WT_PAGE_FREELIST	8	/* Free-list page */
	uint8_t type;			/* Page type */

#define	WT_PAGE_BULK_LOAD	0x01	/* Page bulk loaded */
#define	WT_PAGE_DELETED		0x02	/* Page was empty at reconciliation */
#define	WT_PAGE_INITIAL_EMPTY	0x04	/* Empty page created during open */
#define	WT_PAGE_MERGE		0x08	/* Page to merge in reconciliation */
#define	WT_PAGE_MODIFIED	0x10	/* Page is modified */
#define	WT_PAGE_PINNED		0x20	/* Page is pinned */
	uint8_t flags;			/* Page flags */
};

/*
 * WT_PADDR, WT_PSIZE --
 *	A page's address and size.  We don't maintain the page's address/size in
 * the page: a page's address/size is found in the page parent's WT_REF struct,
 * and like a person with two watches can never be sure what time it is, having
 * two places to find a piece of information leads to confusion.
 */
#define	WT_PADDR(p)	((p)->parent_ref->addr)
#define	WT_PSIZE(p)	((p)->parent_ref->size)

/*
 * WT_REF --
 * A single in-memory page and the state information used to determine if it's
 * OK to dereference the pointer to the page.
 *
 * Synchronization is based on the WT_REF->state field, which has 4 states:
 *
 * WT_REF_DISK:
 *      The default setting before any pages are brought into memory, and set
 *	by the eviction server after page reconciliation (when the page has
 *	been discarded or written to disk, and remains backed by the disk);
 *	the page is on disk, and needs to be read into memory before use.
 * WT_REF_LOCKED:
 *	Set by the eviction server; the eviction server has selected this page
 *	for eviction and is checking hazard references.
 * WT_REF_MEM:
 *	Set by the read server when the page is read from disk; the page is
 *	in the cache and the page reference is OK.
 *
 * The life cycle of a typical page goes like this: pages are read into memory
 * from disk and the read server sets their state to WT_REF_MEM.  When the
 * eviction server selects the page for eviction, it sets the page state to
 * WT_REF_LOCKED.  In all cases, the eviction server resets the page's state
 * when it's finished with the page: if eviction was successful (a clean page
 * was simply discarded, and a dirty page was written to disk), the server sets
 * the page state to WT_REF_DISK; if eviction failed because the page was busy,
 * the page state is reset to WT_REF_MEM.
 *
 * Readers check the state field and if it's WT_REF_MEM, they set a hazard
 * reference to the page, flush memory and re-confirm the page state.  If the
 * page state is unchanged, the reader has a valid reference and can proceed.
 *
 * When the eviction server wants to discard a page from the tree, it sets the
 * WT_REF_LOCKED flag, flushes memory, then checks hazard references.  If the
 * eviction server finds a hazard reference, it resets the state to WT_REF_MEM,
 * restoring the page to the readers.  If the eviction server does not find a
 * hazard reference, the page is evicted.
 */
struct __wt_ref {
	WT_PAGE *page;			/* In-memory page */

	/*
	 * Page state.
	 *
	 * WT_REF_DISK has a value of 0, the default state after allocating
	 * cleared memory.
	 */
#define	WT_REF_DISK		0	/* Page is on disk */
#define	WT_REF_LOCKED		1	/* Page being evaluated for eviction */
#define	WT_REF_MEM		2	/* Page is in cache and valid */
	uint32_t volatile state;

	uint32_t addr;			/* Backing disk address */
	uint32_t size;			/* Backing disk size */
};

/*
 * WT_IKEY --
 * Instantiated key: row-store keys are usually prefix compressed and sometimes
 * Huffman encoded or overflow objects.  Normally, a row-store page in-memory
 * key points to the on-page WT_CELL, but in some cases, we instantiate the key
 * in memory, in which case the row-store page in-memory key points to a WT_IKEY
 * structure.
 */
struct __wt_ikey {
	WT_SESSION_BUFFER *sb;		/* Session buffer holding the WT_IKEY */

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
 * WT_ROW_REF --
 * Row-store internal page subtree entries.
 */
struct __wt_row_ref {
	void	*key;			/* On-page cell or off-page WT_IKEY */

	WT_REF	 ref;			/* Subtree page */
#define	WT_ROW_REF_ADDR(rref)	((rref)->ref.addr)
#define	WT_ROW_REF_PAGE(rref)	((rref)->ref.page)
#define	WT_ROW_REF_SIZE(rref)	((rref)->ref.size)
#define	WT_ROW_REF_STATE(rref)	((rref)->ref.state)
};

/*
 * WT_ROW_REF_FOREACH --
 * Macro to walk the off-page subtree array of an in-memory internal page.
 */
#define	WT_ROW_REF_FOREACH(page, rref, i)				\
	for ((i) = (page)->entries,					\
	    (rref) = (page)->u.row_int.t; (i) > 0; ++(rref), --(i))

/*
 * WT_COL_REF --
 * Column-store internal page subtree entries.
 */
struct __wt_col_ref {
	uint64_t recno;			/* Starting record number */

	WT_REF	 ref;			/* Subtree page */
#define	WT_COL_REF_ADDR(cref)	((cref)->ref.addr)
#define	WT_COL_REF_PAGE(cref)	((cref)->ref.page)
#define	WT_COL_REF_SIZE(cref)	((cref)->ref.size)
#define	WT_COL_REF_STATE(cref)	((cref)->ref.state)
};

/*
 * WT_COL_REF_FOREACH --
 * Macro to walk the off-page subtree array of an in-memory internal page.
 */
#define	WT_COL_REF_FOREACH(page, cref, i)				\
	for ((i) = (page)->entries,					\
	    (cref) = (page)->u.col_int.t; (i) > 0; ++(cref), --(i))

/*
 * WT_ROW --
 * Each in-memory page row-store leaf page has an array of WT_ROW structures:
 * this is created from on-page data when a page is read from the file.  It's
 * sorted by key, fixed in size, and references data on the page.
 */
struct __wt_row {
	void	*key;			/* On-page cell or off-page WT_IKEY */
};

/*
 * WT_ROW_FOREACH --
 *	Walk the entries of an in-memory row-store leaf page.
 */
#define	WT_ROW_FOREACH(page, rip, i)					\
	for ((i) = (page)->entries,					\
	    (rip) = (page)->u.row_leaf.d; (i) > 0; ++(rip), --(i))
#define	WT_ROW_FOREACH_REVERSE(page, rip, i)				\
	for ((i) = (page)->entries,					\
	    (rip) = (page)->u.row_leaf.d + ((page)->entries - 1);	\
	    (i) > 0; --(rip), --(i))

/*
 * WT_ROW_SLOT --
 *	Return the 0-based array offset based on a WT_ROW reference.
 */
#define	WT_ROW_SLOT(page, rip)						\
	((uint32_t)(((WT_ROW *)rip) - (page)->u.row_leaf.d))

/*
 * WT_COL --
 * Each in-memory column-store leaf page has an array of WT_COL structures:
 * this is created from on-page data when a page is read from the file.
 * It's fixed in size, and references data on the page.
 */
struct __wt_col {
	/*
	 * Col-store data references are page offsets, not pointers (we boldly
	 * re-invent short pointers).  The trade-off is 4B per K/V pair on a
	 * 64-bit machine vs. a single cycle for the addition of a base pointer.
	 *
	 * The on-page data is untyped for column-store pages -- if the page
	 * has variable-length objects, it's a WT_CELL (like row-store pages).
	 * If the page has fixed-length objects, it's untyped bytes.
	 *
	 * If the value is 0, it's a single, deleted record.   While this might
	 * be marginally faster than looking at the page, the real reason for
	 * this is to simplify extending column-store files: a newly allocated
	 * WT_COL array translates to a set of deleted records, which is exactly
	 * what we want.
	 *
	 * Obscure the field name, code shouldn't use WT_COL->value, the public
	 * interface is WT_COL_PTR.
	 */
	uint32_t __value;
};

/*
 * WT_COL_PTR --
 *     Return a pointer corresponding to the data offset -- if the item doesn't
 * exist on the page, return a NULL.
 */
#define	WT_COL_PTR(page, cip)						\
	((cip)->__value == 0 ? NULL : WT_REF_OFFSET(page, (cip)->__value))

/*
 * WT_COL_FOREACH --
 *	Walk the entries of an in-memory column-store leaf page.
 */
#define	WT_COL_FOREACH(page, cip, i)					\
	for ((i) = (page)->entries,					\
	    (cip) = (page)->u.col_leaf.d; (i) > 0; ++(cip), --(i))

/*
 * WT_COL_SLOT --
 *	Return the 0-based array offset based on a WT_COL reference.
 */
#define	WT_COL_SLOT(page, cip)						\
	((uint32_t)(((WT_COL *)cip) - (page)->u.col_leaf.d))

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
	WT_SESSION_BUFFER *sb;		/* session buffer holding this update */

	WT_UPDATE *next;		/* forward-linked list */

	/*
	 * We can't store 4GB cells: we're short by a few bytes because each
	 * change/insert item requires a leading WT_UPDATE structure.  For that
	 * reason, we can use the maximum size as an is-deleted flag and don't
	 * have to increase the size of this structure for a flag bit.
	 */
#define	WT_UPDATE_DELETED_ISSET(upd)	((upd)->size == UINT32_MAX)
#define	WT_UPDATE_DELETED_SET(upd)	((upd)->size = UINT32_MAX)
	uint32_t size;			/* update length */

	/* The untyped value immediately follows the WT_UPDATE structure. */
#define	WT_UPDATE_DATA(upd)						\
	((void *)((uint8_t *)(upd) + sizeof(WT_UPDATE)))
};

/*
 * WT_INSERT --
 *
 * Row-store leaf pages support inserts of new K/V pairs.  When the first K/V
 * pair is inserted, the WT_INSERT array is allocated, with one slot for every
 * existing element in the page, plus one additional slot.  A slot points to a
 * WT_INSERT structure which sorts after the WT_ROW element that references it
 * and before the subsequent WT_ROW element; if more than one insert is done
 * between two page entries, the WT_INSERT structures are formed into a key-
 * sorted, forward-linked list.  The additional slot is because it's possible to
 * insert items smaller than any existing key on the page -- for that reason,
 * the first slot of the insert array holds keys smaller than any other key on
 * the page.
 *
 * In column-store fixed-length run-length encoded pages (WT_PAGE_COL_RLE type
 * pages), a single indx entry may reference a large number of records, because
 * there's a single on-page entry that represents many identical records.   (We
 * can't expand those entries when the page comes into memory, as that would
 * require resources as pages are moved to/from the cache, including read-only
 * files.)  Instead, a single indx entry represents all of the identical records
 * originally found on the page.
 *	Modifying (or deleting) run-length encoded column-store records is hard
 * because the page's entry no longer references a set of identical items.  We
 * handle this by "inserting" a new entry into the insert array.  This is the
 * only case where it's possible to "insert" into a column-store, it's normally
 * only possible to append to a column-store as insert requires re-numbering
 * subsequent records.  (Berkeley DB did support mutable records, but it won't
 * scale and it isn't useful enough to re-implement, IMNSHO.)
 */
struct __wt_insert {
	WT_SESSION_BUFFER *sb;		/* session buffer holding this update */

	WT_INSERT *next;		/* forward-linked list */

	WT_UPDATE *upd;			/* value */

	/*
	 * In a row-store leaf page, the WT_INSERT structure is immediately
	 * followed by a key-size/key pair.
	 */
#define	WT_INSERT_KEY_SIZE(ins)						\
	(*(uint32_t *)((uint8_t *)(ins) + sizeof(WT_INSERT)))
#define	WT_INSERT_KEY(ins)						\
	((void *)((uint8_t *)(ins) + (sizeof(WT_INSERT) + sizeof(uint32_t))))
	/*
	 * In a column-store leaf page, the WT_INSERT structure is immediately
	 * followed by a record number.
	 */
#define	WT_INSERT_RECNO(ins)						\
	(*(uint64_t *)((uint8_t *)(ins) + sizeof(WT_INSERT)))
};

/*
 * The row- and column-store leaf page insert and update arrays are arrays of
 * pointers to structures, and may not exist.  The following macros return an
 * array entry if the array of pointers and the specific structure exist, else
 * NULL.
 */
#define	WT_COL_UPDATE(page, ip)						\
	((page)->u.col_leaf.upd == NULL ?				\
	    NULL : (page)->u.col_leaf.upd[WT_COL_SLOT(page, ip)])
#define	WT_COL_INSERT(page, ip)						\
	((page)->u.col_leaf.ins == NULL ?				\
	    NULL : (page)->u.col_leaf.ins[WT_COL_SLOT(page, ip)])
/*
 * WT_ROW_INSERT_SMALLEST references an additional slot past the end of the
 * the "one per WT_ROW slot" insert array.  That's because the insert array
 * requires an extra slot to hold keys that sort before any key found on the
 * original page.
 */
#define	WT_ROW_INSERT_SMALLEST(page)					\
	((page)->u.row_leaf.ins == NULL ?				\
	    NULL : (page)->u.row_leaf.ins[(page)->entries])
#define	WT_ROW_INSERT(page, ip)						\
	((page)->u.row_leaf.ins == NULL ?				\
	    NULL : (page)->u.row_leaf.ins[WT_ROW_SLOT(page, ip)])
#define	WT_ROW_UPDATE(page, ip)						\
	((page)->u.row_leaf.upd == NULL ?				\
	    NULL : (page)->u.row_leaf.upd[WT_ROW_SLOT(page, ip)])

/*
 * WT_OFF --
 *	Row-store internal pages reference subtrees with no record count, and
 * row- and column-store overflow key and data items.
 *
 * WT_OFF_RECORD --
 *      Column-store internal pages reference subtrees including total record
 * counts for the subtree.
 *
 * !!!
 * Note the initial two fields of the WT_OFF and WT_OFF_RECORD fields are the
 * same -- this is deliberate, and we use it to pass references to places that
 * only care about the addr/size pair.
 */
struct __wt_off {
	uint32_t addr;			/* Subtree root page address */
	uint32_t size;			/* Subtree root page length */
};
/*
 * WT_OFF_SIZE is the expected structure size -- we verify the build to
 * ensure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_OFF_SIZE	8
/*
 *
 * Compilers pad the WT_OFF_RECORD structure because of the 64-bit record count
 * field.  This is an on-disk structure, which means we require a fixed size,
 * so we declare it as two 32-bit fields and cast it.  We haven't yet found a
 * compiler that aligns the 32-bit fields such that a cast won't work; if we
 * find one, we'll have to go to bit masks, or to copying bytes to/from a local
 * variable.
 */
struct __wt_off_record {
	uint32_t addr;			/* Subtree root page address */
	uint32_t size;			/* Subtree root page length */

#define	WT_RECNO(offp)		(*(uint64_t *)(&(offp)->__record_chunk[0]))
	uint32_t __record_chunk[2];	/* Subtree record count */
};
/*
 * WT_OFF_RECORD_SIZE is the expected structure size -- we verify the build to
 * ensure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_OFF_RECORD_SIZE	16

/*
 * WT_OFF_FOREACH --
 *	Walks WT_OFF/WT_OFF_RECORD references on a page, incrementing a pointer
 *	based on its declared type.
 */
#define	WT_OFF_FOREACH(dsk, offp, i)					\
	for ((offp) = WT_PAGE_DISK_BYTE(dsk),				\
	    (i) = (dsk)->u.entries; (i) > 0; ++(offp), --(i))

/*
 * On-page "deleted" flags for fixed-length column-store data cells -- steal
 * the top bit of the data.
 */
#define	WT_FIX_DELETE_BYTE	0x80
#define	WT_FIX_DELETE_ISSET(b)	(((uint8_t *)(b))[0] & WT_FIX_DELETE_BYTE)
#define	WT_FIX_DELETE_SET(b)	(((uint8_t *)(b))[0] = WT_FIX_DELETE_BYTE)

/* WT_FIX_FOREACH is a loop that walks fixed-length references on a page. */
#define	WT_FIX_FOREACH(btree, dsk, p, i)				\
	for ((p) = WT_PAGE_DISK_BYTE(dsk),				\
	    (i) = (dsk)->u.entries; (i) > 0; --(i),			\
	    (p) = (uint8_t *)(p) + (btree)->fixed_len)

/*
 * WT_RLE_REPEAT_FOREACH is a loop that walks fixed-length, run-length encoded
 * entries on a page.
 */
#define	WT_RLE_REPEAT_FOREACH(btree, dsk, p, i)				\
	for ((p) = WT_PAGE_DISK_BYTE(dsk),				\
	    (i) = (dsk)->u.entries; (i) > 0; --(i),			\
	    (p) = (uint8_t *)(p) + (btree)->fixed_len + sizeof(uint16_t))

/*
 * WT_RLE_REPEAT_COUNT and WT_RLE_REPEAT_DATA reference the data and count
 * values for fixed-length, run-length encoded page entries.
 */
#define	WT_RLE_REPEAT_COUNT(p)	(*(uint16_t *)(p))
#define	WT_RLE_REPEAT_DATA(p)	((uint8_t *)(p) + sizeof(uint16_t))

/*
 * WT_RLE_REPEAT_ITERATE is a loop that walks fixed-length, run-length encoded
 * references on a page, visiting each entry the appropriate number of times.
 */
#define	WT_RLE_REPEAT_ITERATE(btree, dsk, p, i, j)			\
	WT_RLE_REPEAT_FOREACH(btree, dsk, p, i)				\
		for ((j) = WT_RLE_REPEAT_COUNT(p); (j) > 0; --(j))

/*
 * General purpose macros for the Btree implementation.
 */
/*
 * In diagnostic mode we track the locations from which hazard references
 * were acquired.
 */
#ifdef HAVE_DIAGNOSTIC
#define	__wt_page_in(a, b, c, d)					\
	__wt_page_in_func(a, b, c, d, __FILE__, __LINE__)
#else
#define	__wt_page_in(a, b, c, d)					\
	__wt_page_in_func(a, b, c, d)
#endif

/*
 * WT_FREE_ENTRY  --
 *	Encapsulation of an entry on the Btree free list.
 */
struct __wt_free_entry {
	TAILQ_ENTRY(__wt_free_entry) qa;	/* Address queue */
	TAILQ_ENTRY(__wt_free_entry) qs;	/* Size queue */

	uint32_t addr;				/* Disk offset */
	uint32_t size;				/* Size */
};

/*
 * WT_SALVAGE_COOKIE --
 *	Encapsulation of salvage information for reconciliation.
 */
struct __wt_salvage_cookie {
	uint64_t missing;			/* Initial items to create */
	uint64_t skip;				/* Initial items to skip */
	uint64_t take;				/* Items to take */

	int	 done;				/* Ignore the rest */
};

#if defined(__cplusplus)
}
#endif
