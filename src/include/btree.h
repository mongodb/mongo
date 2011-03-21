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
 * these are called "addrs".  To simplify bookkeeping, page sizes must be a
 * multiple of the allocation unit size.  There are two special addresses,
 * one for pages which don't exist, and one for pages that have been deleted.
 *
 * The minimum maximum file size is almost 2TB (2^9 x (2^32 - 2)), and the
 * maximum maximum file size is almost 512PB (2^27 x 2^32 - 2).
 *
 * In summary, small file allocation units limit the file size, (but minimize
 * wasted space when storing overflow items), and when the allocation unit
 * grows, the maximum size of the file grows as well.
 *
 * The minimum btree leaf and internal page sizes are 512B, the maximum 256MB.
 * (The maximum of 256MB is enforced by the software, it could be set as high
 * as 4GB.)
 *
 * Key and data item lengths are stored in 32-bit unsigned integers, meaning
 * the largest key or data item is 4GB.  Record numbers are stored in 64-bit
 * unsigned integers, meaning the largest record number is "huge".
 */

#define	WT_BTREE_ALLOCATION_SIZE	512
#define	WT_BTREE_ALLOCATION_SIZE_MAX	(128 * WT_MEGABYTE)
#define	WT_BTREE_PAGE_SIZE_MAX		(256 * WT_MEGABYTE)

#define	WT_ADDR_INVALID	UINT32_MAX	/* Invalid file address */

/*
 * Underneath the Btree code is the OS layer, where sizes are stored as numbers
 * of bytes.   In the OS layer, 32-bits is too small (a file might be larger
 * than 4GB), so we use a standard type known to hold the size of a file, off_t.
 */
/* Convert a data address to/from a byte offset. */
#define	WT_ADDR_TO_OFF(btree, addr)					\
	((off_t)(addr) * (btree)->allocsize)
#define	WT_OFF_TO_ADDR(btree, off)					\
	((uint32_t)((off) / (btree)->allocsize))

/*
 * Return file allocation units needed for length (optionally including a page
 * header), rounded to an allocation unit.
 */
#define	WT_HDR_BYTES_TO_ALLOC(btree, size)				\
	(WT_ALIGN((size) + WT_PAGE_DISK_SIZE, (btree)->allocsize))

/*
 * The file needs a description, here's the structure.  At the moment, this
 * structure is written into the first 512 bytes of the file, but that will
 * change in the future.
 *
 * !!!
 * Field order is important: there's a 8-byte type in the middle, and the
 * Solaris compiler inserts space into the structure if we don't put that
 * field on an 8-byte boundary.
 */
struct __wt_page_desc {
#define	WT_BTREE_MAGIC		120897
	uint32_t magic;			/* 00-03: Magic number */
#define	WT_BTREE_MAJOR_VERSION	0
	uint16_t majorv;		/* 04-05: Major version */
#define	WT_BTREE_MINOR_VERSION	1
	uint16_t minorv;		/* 06-07: Minor version */

#define	WT_BTREE_INTLMAX_DEFAULT	(2 * 1024)
#define	WT_BTREE_INTLMIN_DEFAULT	(2 * 1024)
	uint32_t intlmax;		/* 08-11: Maximum intl page size */
	uint32_t intlmin;		/* 12-15: Minimum intl page size */

#define	WT_BTREE_LEAFMAX_DEFAULT	WT_MEGABYTE
#define	WT_BTREE_LEAFMIN_DEFAULT	(32 * 1024)
	uint32_t leafmax;		/* 16-19: Maximum leaf page size */
	uint32_t leafmin;		/* 20-23: Minimum leaf page size */

	uint64_t recno_offset;		/* 24-31: Offset record number */
	uint32_t root_addr;		/* 32-35: Root page address */
	uint32_t root_size;		/* 36-39: Root page length */
	uint32_t free_addr;		/* 40-43: Free list page address */
	uint32_t free_size;		/* 44-47: Free list page length */

#define	WT_PAGE_DESC_RLE	0x01	/* Run-length encoding */
	uint32_t flags;			/* 48-51: Flags */

	uint8_t  fixed_len;		/* 52: Fixed length byte count */
	uint8_t  unused1[3];		/* 53-55: Unused */

	uint32_t unused2[114];		/* Unused */
};
/*
 * WT_PAGE_DESC_SIZE is the expected structure size -- we verify the build to
 * ensure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_PAGE_DESC_SIZE		512

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
	 * the keyspace during salvage.
	 */
	uint64_t recno;			/* 00-07: column-store starting recno */

	uint32_t lsn_file;		/* 08-11: LSN file */
	uint32_t lsn_off;		/* 12-15: LSN file offset */

	uint32_t checksum;		/* 16-19: checksum */

	union {
		uint32_t entries;	/* 20-23: number of cells on page */
		uint32_t datalen;	/* 20-23: overflow data length */
	} u;

	uint8_t type;			/* 24: page type */

	/*
	 * It would be possible to decrease the size of the page header by 3
	 * bytes by only writing out the first 25 bytes of the structure to the
	 * page, but I'm not bothering -- I don't think the space is worth it
	 * and having a little bit of on-page data to play with in the future
	 * can be a good thing.
	 */
	uint8_t unused[3];		/* 25-27: unused padding */
};
/*
 * WT_PAGE_DISK_SIZE is the expected structure size -- we verify the build to
 * ensure the compiler hasn't inserted padding (which would break the world).
 * The header is followed by WT_CELL structures, which require 4-byte
 * alignment.
 *
 * 64-bit compilers will pad the end of this structure to a multiple of 8
 * bytes. We take that into account when checking the size, and always use
 * WT_PAGE_DISK_SIZE rather than sizeof to avoid writing 4 bytes of padding to
 * every page.
 */
#define	WT_PAGE_DISK_SIZE		28

/*
 * WT_PAGE_DISK_BYTE --
 * The first usable data byte on the page (past the header).
 */
#define	WT_PAGE_DISK_BYTE(dsk)						\
	((void *)((uint8_t *)(dsk) + WT_PAGE_DISK_SIZE))

/*
 * WT_PAGE_DISK_OFFSET --
 *	Offset of a pointer in a page.
 */
#define	WT_PAGE_DISK_OFFSET(dsk, p)					\
	((uint32_t)((uint8_t *)(p) - (uint8_t *)(dsk)))
#define	WT_PAGE_DISK_REF(dsk, o)					\
	((void *)((uint8_t *)(dsk) + (o)))

/*
 * WT_REF --
 * A single in-memory page and the state information used to determine if it's
 * OK to dereference the pointer to the page.
 *
 * Synchronization is based on the WT_REF->state field, which is divided into
 * two parts, a state and two flags.  The state value is one of the following:
 *
 * WT_REF_MEM:
 *	Set by the read server on initial read from disk; the page is in the
 *	cache and the page reference is OK.
 * WT_REF_DISK:
 *      Set by the eviction server after page reconciliation; the page is on
 *	disk, but needs to be read into memory before use.
 * WT_REF_EVICTED:
 *	Set by the eviction server after page reconciliation; the page was
 *	logically evicted, but is waiting on its parent to be evicted so it
 *	can be merged into the parent.
 *
 * The flag values are one or more of the following:
 *
 * WT_REF_EVICT:
 *	Set by the eviction server; the eviction server has selected this page
 *	and is checking hazard references.
 * WT_REF_MERGE:
 *	Set by the eviction server during page reconciliation; the page is
 *	expected to be merged into its parent when the parent is reconciled.
 *
 * The life cycle of a page goes like this: pages are read into memory from disk
 * and the read server sets the state to WT_REF_MEM.  When the eviction server
 * selects the page for eviction, the server adds the WT_REF_EVICT flag.  (In
 * all cases, the eviction server clears the WT_REF_EVICT flag when finished
 * with the page.)  If eviction was possible, the server sets the page state to
 * WT_REF_DISK.
 *
 * There are more complicated scenarios:
 *
 * Scenario #1: the page is entirely empty.  In this case the eviction server
 * sets the WT_REF_MERGE and WT_REF_EVICTED flags.  Ideally, when the page's
 * parent is evicted, the empty page will be discarded.  Alternatively, if the
 * page is accessed again, the read server will clear the WT_REF_MERGE and
 * WT_REF_EVICTED flags and set the state to WT_REF_MEM.  This process repeats
 * until the parent of the page is evicted and the page is merged into the
 * parent, or the page is no longer empty and a more normal page reconciliation
 * will occur.
 *
 * Scenario #2: the page requires a split.  In this case the eviction server
 * creates a new, in-memory internal page to reference the split pages: this new
 * page has a WT_REF_MEM state and the WT_REF_MERGE and WT_REF_EVICTED flags.
 * Ideally, when the parent of the new page is evicted, the page will be merged
 * into the parent.  Alternatively, if the new page is accessed, the read server
 * will clear the WT_REF_EVICTED flag and set the state to WT_REF_MEM.  This
 * process will repeat until the parent of the page is evicted and the page is
 * merged into the parent (internal pages created for the purpose of a split are
 * never written to disk, they are always merged into the parent in order to
 * guarantee tree depth over time).
 *
 * Readers check the state field and if it's WT_REF_MEM, they set a hazard
 * reference to the page, flush memory and re-confirm the page state.  If the
 * page state is unchanged, the reader has a valid reference and can proceed.
 *
 * When the eviction server wants to discard a page from the tree, it sets the
 * WT_REF_EVICT flag, flushes memory, then checks hazard references.  If the
 * eviction server finds a hazard reference, it resets the state to WT_REF_MEM,
 * restoring the page to its readers.  If the eviction server does not find a
 * hazard reference, the page is evicted and the state set to WT_REF_DISK.
 */
struct __wt_ref {
	/*
	 * WT_REF_DISK has a value of 0, we're in the correct default state
	 * after allocating zero'd memory.
	 */
#define	WT_REF_DISK		0x00	/* Page is on disk */
#define	WT_REF_EVICTED		0x01	/* Page was evicted, not discarded */
#define	WT_REF_MEM		0x02	/* Page is in cache */
#define	WT_REF_EVICT		0x80	/* Page being evaluated for eviction */
#define	WT_REF_MERGE		0x40	/* Page should be merged into parent */
#define	WT_REF_STATE(s)							\
	((s) & (WT_REF_DISK | WT_REF_EVICTED | WT_REF_MEM))
#define	WT_REF_SET_STATE(ref, s) do {					\
	(ref)->state =							\
	    ((ref)->state & (WT_REF_EVICT | WT_REF_MERGE)) | (s);	\
} while (0)

	uint32_t volatile state;

	uint32_t addr;			/* Backing disk address */
	uint32_t size;			/* Backing disk size */

	/* !!!
	 * The layout is deliberate.  On a 64-bit machine, when you tuck this
	 * structure inside of a row-store internal page reference, no padding
	 * is needed because the 32-bit values line up.
	 */
	WT_PAGE *page;			/* In-memory page */
};

/*
 * WT_ROW_REF --
 * Row-store internal page subtree entries.
 */
struct __wt_row_ref {
	/*
	 * These two fields are the same as the first fields of a WT_ITEM so we
	 * can pass them to a comparison function without copying.
	 *
	 * If a key requires processing (for example, an overflow key or Huffman
	 * encoded key), the key field points to the on-page key, and the size
	 * is set to 0 to indicate the key is not yet processed.
	 */
	void	 *key;			/* Key */
	uint32_t  size;			/* Key length */

	WT_REF	  ref;			/* Subtree page */
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
	for ((i) = (page)->indx_count,					\
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
	for ((i) = (page)->indx_count,					\
	    (cref) = (page)->u.col_int.t; (i) > 0; ++(cref), --(i))

/*
 * WT_PAGE --
 * The WT_PAGE structure describes the in-memory information about a file page.
 */
struct __wt_page {
	/*
	 * Two links to the parent's WT_PAGE structure -- the physical parent
	 * page, and the WT_REF structure used to find this page.
	 */
	WT_PAGE	*parent;		/* Page's parent */
	WT_REF	*parent_ref;		/* Page's parent reference */

	/*
	 * We maintain 3 "generation" numbers for a page: the disk, read and
	 * write generations.
	 *
	 * The read generation is incremented each time the page is searched,
	 * and acts as an LRU value for each page in the tree; it is read by
	 * the eviction server thread to select pages to be discarded from the
	 * in-memory tree.
	 *
	 * The read generation is a 64-bit value; incremented every time the
	 * page is searched, a 32-bit value could overflow.
	 *
	 * We pin the root page of each tree in memory using an out-of-band LRU
	 * value.   If we ever add a flags field to this structure, the pinned
	 * flag could move there.
	 */
#define	WT_PAGE_SET_PIN(p)	((p)->read_gen = UINT64_MAX)
#define	WT_PAGE_IS_PINNED(p)	((p)->read_gen == UINT64_MAX)
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
	 * The disk generation is set to the current write generation before a
	 * page is reconciled and written to disk.  If the disk generation
	 * matches the write generation, the page must be clean; otherwise, the
	 * page was potentially modified after the last write, and must be
	 * re-written to disk before being discarded.
	 *
	 * XXX
	 * These aren't declared volatile: (1) disk-generation is read/written
	 * only when the page is reconciled -- it could be volatile but we
	 * explicitly flush it there instead; (2) read-generation gets set a
	 * lot (on every access), and we don't want to bother flushing it; (3)
	 * write-generation is written by the workQ when modifying a page, and
	 * must be flushed in a specific order as the workQ flushes its changes.
	 *
	 * XXX
	 * 32-bit values are probably more than is needed: at some point we may
	 * need to clean up pages once there have been sufficient modifications
	 * to make our linked lists of inserted cells too slow to search, or as
	 * soon as enough memory is allocated in service of page modifications
	 * (although we should be able to release memory from the MVCC list as
	 * soon as there's no running thread/txn which might want that version
	 * of the data).   I've used 32-bit types instead of 16-bit types as I
	 * am not positive a 16-bit write to memory will always be atomic.
	 */
#define	WT_PAGE_DISK_WRITE(p)		((p)->disk_gen = (p)->write_gen)
#define	WT_PAGE_IS_MODIFIED(p)		((p)->disk_gen != (p)->write_gen)
#define	WT_PAGE_SET_MODIFIED(p)		(++(p)->write_gen)
	uint32_t disk_gen;
	uint32_t write_gen;

	/* But the entries are wildly different, based on the page type. */
	union {
		/* Row-store internal information. */
		struct {
			WT_ROW_REF *t;		/* Subtrees */
			WT_UPDATE **upd;	/* Updates */
		} row_int;

		struct {
			WT_ROW	   *d;		/* K/V pairs */
			WT_UPDATE **upd;	/* Updates */
		} row_leaf;

		/* Column-store internal information. */
		struct {
			uint64_t recno;		/* Starting recno */

			WT_COL_REF *t;		/* Subtrees */
			WT_UPDATE **upd;	/* Updates */
		} col_int;

		/* Column-store leaf information. */
		struct {
			uint64_t recno;		/* Starting recno */

			WT_COL	   *d;		/* V objects */
			WT_UPDATE **upd;	/* Updates */

			WT_RLE_EXPAND **rleexp;	/* RLE expansion array */
		} col_leaf;
	} u;

	/*
	 * Page's on-disk representation: NULL for pages created in memory, and
	 * most code should never look at this value.
	 *
	 * XXX
	 * I don't think we actually need the addr field, it's only used as a
	 * unique identifier for an in-memory page, that is, debugging.  It's
	 * here because the bulk read code uses it, and we should try and get
	 * rid of it when that code gets re-written.
	 */
	WT_PAGE_DISK *XXdsk;
	uint32_t addr;				/* Original file address */
	uint32_t size;				/* Original size */

	/*
	 * Every in-memory page references a number of entries, originally
	 * based on the number of on-disk entries found.
	 */
	uint32_t indx_count;

#define	WT_PAGE_INVALID		0	/* Invalid page */
#define	WT_PAGE_COL_FIX		1	/* Col store fixed-len leaf */
#define	WT_PAGE_COL_INT		2	/* Col store internal page */
#define	WT_PAGE_COL_RLE		3	/* Col store run-length encoded leaf */
#define	WT_PAGE_COL_VAR		4	/* Col store var-length leaf page */
#define	WT_PAGE_OVFL		5	/* Page of untyped data */
#define	WT_PAGE_ROW_INT		6	/* Row-store internal page */
#define	WT_PAGE_ROW_LEAF	7	/* Row-store leaf page */
#define	WT_PAGE_FREELIST	8	/* Free-list page */
	uint8_t type;			/* Page type */
};
/*
 * WT_PAGE_SIZE is the expected structure size -- we verify the build to ensure
 * the compiler hasn't inserted padding.  The WT_PAGE structure is in-memory, so
 * padding it won't break the world, but we don't want to waste space, and there
 * are a lot of these structures.
 *
 * The compiler will pad this to be a multiple of the pointer size, so take
 * that into account.
 */
#define	WT_PAGE_SIZE							\
	WT_ALIGN(6 * sizeof(void *) + 2 * sizeof(uint64_t) +		\
	    5 * sizeof(uint32_t) + sizeof(uint8_t), sizeof(void *))

/*
 * WT_ROW --
 * Each in-memory page row-store leaf page has an array of WT_ROW structures:
 * this is created from on-page data when a page is read from the file.
 * It's sorted by key, fixed in size, and references data on the page.
 */
struct __wt_row {
	/*
	 * The first fields of the WT_ROW structure are the same as the first
	 * fields of a WT_ITEM so we can pass it to a comparison function
	 * without copying.
	 *
	 * If a key requires processing (for example, an overflow key or an
	 * Huffman encoded key), the key field points to the on-page key,
	 * but the size is set to 0 to indicate the key is not yet processed.
	 */
	void	 *key;			/* Key */
	uint32_t size;			/* Key length */

	void	 *value;		/* Data */
};
/*
 * WT_ROW_SIZE is the expected structure size -- we verify the build to ensure
 * the compiler hasn't inserted padding.  The WT_ROW structure is in-memory, so
 * padding it won't break the world, but we don't want to waste space, and there
 * are a lot of these structures.
 */
#define	WT_ROW_SIZE							\
	WT_ALIGN(2 * sizeof(void *) + sizeof(uint32_t), sizeof(void *))

/*
 * WT_ROW_INDX_FOREACH --
 *	Walk the entries of an in-memory row-store leaf page.
 */
#define	WT_ROW_INDX_FOREACH(page, rip, i)				\
	for ((i) = (page)->indx_count,					\
	    (rip) = (page)->u.row_leaf.d; (i) > 0; ++(rip), --(i))

/*
 * WT_ROW_INDX_SLOT --
 *	Return the array offset based on a WT_ROW reference.
 */
#define	WT_ROW_INDX_SLOT(page, rip)					\
	((uint32_t)(((WT_ROW *)rip) - (page)->u.row_leaf.d))

/*
 * WT_COL --
 * Each in-memory column-store leaf page has an array of WT_COL structures:
 * this is created from on-page data when a page is read from the file.
 * It's fixed in size, and references data on the page.
 *
 * In column-store fixed-length run-length encoded pages (WT_PAGE_COL_RLE type
 * pages), a single indx entry may reference a large number of records, because
 * there's a single on-page entry that represents many identical records.   (We
 * can't expand those entries when the page comes into memory, as that would
 * require resources as pages are moved to/from the cache, including read-only
 * files.)  Instead, a single indx entry represents all of the identical records
 * originally found on the page.
 */
struct __wt_col {
	/*
	 * Column-store leaf page references are page offsets, not pointers (we
	 * boldly re-invent short pointers).  The trade-off is 4B per data cell
	 * on a 64-bit machine vs. a single cycle to do an addition to the base
	 * pointer.
	 *
	 * The on-page data is untyped for column-store pages -- if the page
	 * has variable-length objects, it's a WT_CELL layout, like row-store
	 * pages.  If the page has fixed-length objects, it's untyped bytes.
	 */
	uint32_t data;
};
/*
 * WT_COL_SIZE is the expected structure size -- we verify the build to ensure
 * the compiler hasn't inserted padding.  The WT_COL structure is in-memory, so
 * padding it won't break the world, but we don't want to waste space, and there
 * are a lot of these structures.
 */
#define	WT_COL_SIZE	(sizeof(uint32_t))

/*
 * WT_COL_PTR --
 *	Return a pointer corresponding to the data offset.
 */
#define	WT_COL_PTR(page, cip)						\
	WT_PAGE_DISK_REF((page)->XXdsk, (cip)->data)

/*
 * WT_COL_INDX_FOREACH --
 *	Walk the entries of an in-memory column-store leaf page.
 */
#define	WT_COL_INDX_FOREACH(page, cip, i)				\
	for ((i) = (page)->indx_count,					\
	    (cip) = (page)->u.col_leaf.d; (i) > 0; ++(cip), --(i))

/*
 * WT_COL_INDX_SLOT --
 *	Return the array offset based on a WT_COL reference.
 */
#define	WT_COL_INDX_SLOT(page, cip)					\
	((uint32_t)(((WT_COL *)cip) - (page)->u.col_leaf.d))

/*
 * WT_UPDATE --
 *	Updates: entries on leaf pages can be modified or deleted, and new
 * entries can be inserted in row-store leaf pages, and both row-store and
 * column store internal pages.
 *
 * Modifications or deletions are stored in the update array of the page.
 * When the first element on a page is modified, the array is allocated, with
 * one slot for every existing element in the page.  A slot points to a
 * WT_UPDATE; if more than one update is done for a single entry, the WT_UPDATE
 * structures are formed into a forward-linked list.
 */
struct __wt_update {
	SESSION_BUFFER *sb;		/* session buffer holding this update */
	WT_UPDATE *next;		/* forward-linked list */

	/*
	 * We can't store 4GB cells: we're short by a few bytes because each
	 * change/insert item requires a leading WT_UPDATE structure.  For that
	 * reason, we can use the maximum size as an is-deleted flag and don't
	 * have to increase the size of this structure for a flag bit.
	 */
#define	WT_UPDATE_DELETED_ISSET(upd)	((upd)->size == UINT32_MAX)
#define	WT_UPDATE_DELETED_SET(upd)	((upd)->size = UINT32_MAX)
	uint32_t size;			/* data length */

	/* The untyped data immediately follows the upd structure. */
#define	WT_UPDATE_DATA(upd)						\
	((void *)((uint8_t *)upd + sizeof(WT_UPDATE)))
};

/*
 * WT_RLE_EXPAND --
 * In-memory information about a replaced key/data pair on a run-length encoded,
 * column-store file page.
 *
 * Modifying (or deleting) run-length encoded column-store records is tricky
 * because the page's entry no longer references a set of identical items.  We
 * handle this by "inserting" a new entry into the rleexp array.  This is the
 * only case where it's possible to "insert" into a column-store, it's normally
 * only possible to append to a column-store as insert requires re-numbering
 * subsequent records.  (Berkeley DB did support the re-numbering
 * functionality, but it won't scale and it isn't useful enough to
 * re-implement, IMNSHO.)
 */
struct __wt_rle_expand {
	uint64_t recno;			/* recno */

	WT_UPDATE *upd;			/* updates */

	WT_RLE_EXPAND *next;		/* forward-linked list */
};

/*
 * WT_RLE_EXPAND_FOREACH --
 * Macro to walk the run-length encoded column-store expansion  array of an
 * in-memory page.
 */
#define	WT_RLE_EXPAND_FOREACH(page, exp, i)				\
	for ((i) = (page)->indx_count,					\
	    (exp) = (page)->u.col_leaf.rleexp; (i) > 0; ++(exp), --(i))

/*
 * The upd and rleexp  arrays may not exist, and are arrays of pointers to
 * individually allocated structures.   The following macros return an array
 * entry if the array of pointers and the specific structure exist, otherwise
 * NULL.
 */
#define	__WT_COL_ARRAY(page, ip, field)					\
	((page)->field == NULL ?					\
	    NULL : (page)->field[WT_COL_INDX_SLOT(page, ip)])
#define	WT_COL_UPDATE(page, ip)						\
	__WT_COL_ARRAY(page, ip, u.col_leaf.upd)
#define	WT_COL_RLEEXP(page, ip)						\
	__WT_COL_ARRAY(page, ip, u.col_leaf.rleexp)

#define	__WT_ROW_ARRAY(page, ip, field)					\
	((page)->field == NULL ?					\
	    NULL : (page)->field[WT_ROW_INDX_SLOT(page, ip)])
#define	WT_ROW_UPDATE(page, ip)						\
	__WT_ROW_ARRAY(page, ip, u.row_leaf.upd)

/*
 * WT_ROW_{REF,INDX}_AND_KEY_FOREACH --
 *	Walk the indexes of a row-store in-memory page at the same time walking
 * the underlying page's key WT_CELLs.
 *
 * This macro is necessary for when we're walking both the in-memory structures
 * as well as the original page: the problem is keys that require processing.
 * When a page is read into memory from a file, the in-memory key/size pair is
 * set to reference an on-page group of bytes in the key's WT_CELL structure.
 * For uncompressed, small, simple keys, those bytes are usually what we want to
 * access, and the in-memory WT_ROW and WT_ROW_REF structures point to them.
 *
 * Keys that require processing are harder (for example, a Huffman encoded or
 * overflow key).  When we actually use a key requiring processing, we process
 * the key and set the in-memory key/size pair to reference the allocated memory
 * that holds the key -- which means we've lost any reference to the original
 * WT_CELL structure.  If we need the original key (for example, if reconciling
 * the page, or verifying or freeing overflow references, in-memory information
 * no longer gets us there).  As these are relatively rare operations performed
 * on (hopefully!) relatively rare key types, we don't want to increase the size
 * of the in-memory structurees to always reference the page.  Instead, walk the
 * original page at the same time we walk the in-memory structures so we can
 * find the original key WT_CELL.
 */
#define	WT_ROW_REF_AND_KEY_FOREACH(page, rref, key_cell, i)		\
	for ((key_cell) = WT_PAGE_DISK_BYTE((page)->XXdsk),		\
	    (rref) = (page)->u.row_int.t, (i) = (page)->indx_count;	\
	    (i) > 0;							\
	    ++(rref),							\
	    key_cell = --(i) == 0 ?					\
	    NULL : WT_CELL_NEXT(WT_CELL_NEXT(key_cell)))
#define	WT_ROW_INDX_AND_KEY_FOREACH(page, rip, key_cell, i)		\
	for ((key_cell) = WT_PAGE_DISK_BYTE((page)->XXdsk),		\
	    (rip) = (page)->u.row_leaf.d, (i) = (page)->indx_count;	\
	    (i) > 0;							\
	    ++(rip),							\
	    key_cell = --(i) == 0 ?					\
	    NULL : __wt_key_cell_next(key_cell))

/*
 * WT_CELL --
 *	Cell type plus trailing data length (in bytes).
 *
 * After the page header, on pages with variable-length data, there are
 * variable-length cells (all page types except WT_PAGE_COL_{INT,FIX,RLE}),
 * comprised of a list of WT_CELLs in sorted order.  Or, specifically, 4
 * bytes followed by a variable length chunk.
 *
 * The first 8 bits of that 4 bytes holds a cell type, followed by an cell
 * length.  The cell type defines the following set of bytes and the cell
 * length specifies how long the cell is.
 *
 * We encode the length and type in a 4-byte value to minimize the on-page
 * footprint as well as maintain alignment of the bytes that follow the cell.
 * (The trade-off is this limits on-page file key or data cells to 16MB.)
 * The bottom 24-bits are the length of the subsequent data, the next 4-bits are
 * the type, and the top 4-bits are unused.   We could use the unused 4-bits to
 * provide more length, but 16MB seems sufficient for on-page cells.
 *
 * The __cell_chunk field should never be directly accessed, there are macros
 * to extract the type and length.
 *
 * WT_CELLs are aligned to a 4-byte boundary, so it's OK to directly access the
 * __cell_chunk field on the page.
 */
#define	WT_CELL_MAX_LEN	(16 * 1024 * 1024 - 1)
struct __wt_cell {
	uint32_t __cell_chunk;
};
/*
 * WT_CELL_SIZE is the expected structure size -- we verify the build to make
 * sure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_CELL_SIZE	4

/*
 * There are 2 basic types: key and data cells, each of which has an overflow
 * form.  Items are followed by additional data, which varies by type: a key
 * or data cell is followed by a set of bytes; a WT_OVFL structure follows an
 * overflow form.
 *
 * There are 3 additional types: (1) a deleted type (a place-holder for deleted
 * cells where the cell cannot be removed, for example, a column-store cell
 * that must remain to preserve the record count); (2) a subtree reference for
 * keys that reference subtrees without an associated record count (a row-store
 * internal page has a key/reference pair for the tree containing all key/data
 * pairs greater than the key); (3) a subtree reference for keys that reference
 * subtrees with an associated record count (a column-store internal page has
 * a reference for the tree containing all records greater than the specified
 * record).
 *
 * Here's the usage by page type:
 *
 * WT_PAGE_ROW_INT (row-store internal pages):
 *	Variable-length keys with offpage-reference pairs (a WT_CELL_KEY or
 *	WT_CELL_KEY_OVFL cell, followed by a WT_CELL_OFF cell).
 *
 * WT_PAGE_COL_INT (Column-store internal page):
 *	Fixed-length WT_OFF_RECORD structures.
 *
 * WT_PAGE_ROW_LEAF (row-store leaf pages):
 *	Variable-length key and data pairs (a WT_CELL_KEY or WT_CELL_KEY_OVFL
 *	cell, followed by a WT_CELL_DATA or WT_CELL_DATA_OVFL cell).
 *
 * WT_PAGE_COL_FIX (Column-store leaf page storing fixed-length cells):
 * WT_PAGE_COL_RLE (Column-store leaf page storing fixed-length cells):
 *	Fixed-sized data cells.
 *
 * WT_PAGE_OVFL (Overflow page):
 *	A string of bytes.
 *
 * WT_PAGE_COL_VAR (Column-store leaf page storing variable-length cells):
 *	Variable-length data cells (WT_CELL_DATA/DATA_OVFL/DEL).
 *
 * There are currently 7 cell types, using 3 bits, with 5 spares.
 */
#define	WT_CELL_KEY		0x00000000 /* Key */
#define	WT_CELL_KEY_OVFL	0x01000000 /* Key: overflow */
#define	WT_CELL_DATA		0x02000000 /* Data */
#define	WT_CELL_DATA_OVFL	0x03000000 /* Data: overflow */
#define	WT_CELL_DEL		0x04000000 /* Deleted */
#define	WT_CELL_OFF		0x05000000 /* Off-page reference */
#define	WT_CELL_OFF_RECORD	0x06000000 /* Off-page reference with records */

#define	WT_CELL_TYPE(addr)						\
	(((WT_CELL *)(addr))->__cell_chunk & 0x0f000000)
#define	WT_CELL_LEN(addr)						\
	(((WT_CELL *)(addr))->__cell_chunk & 0x00ffffff)
#define	WT_CELL_SET(addr, type, size)					\
	(((WT_CELL *)(addr))->__cell_chunk = (type) | (size))
#define	WT_CELL_SET_LEN(addr, size)					\
	WT_CELL_SET(addr, WT_CELL_TYPE(addr), size)
#define	WT_CELL_SET_TYPE(addr, type)					\
	WT_CELL_SET(addr, type, WT_CELL_LEN(addr))

/* WT_CELL_BYTE is the first data byte for a cell. */
#define	WT_CELL_BYTE(addr)						\
	((uint8_t *)(addr) + sizeof(WT_CELL))

/*
 * On row-store pages, the on-page data referenced by the WT_ROW data field
 * may be WT_OFF, WT_OFF_RECORD or WT_OVFL structures.  These macros do the
 * cast to the right type.
 */
#define	WT_CELL_BYTE_OFF(addr)						\
	((WT_OFF *)(WT_CELL_BYTE(addr)))
#define	WT_CELL_BYTE_OFF_RECORD(addr)					\
	((WT_OFF_RECORD *)(WT_CELL_BYTE(addr)))
#define	WT_CELL_BYTE_OVFL(addr)						\
	((WT_OVFL *)(WT_CELL_BYTE(addr)))

/*
 * Bytes required to store a WT_CELL followed by additional bytes of data.
 * Align the WT_CELL and the subsequent data to a 4-byte boundary so the
 * WT_CELLs on a page all start at a 4-byte boundary.
 */
#define	WT_CELL_SPACE_REQ(size)						\
	WT_ALIGN(sizeof(WT_CELL) + (size), sizeof(uint32_t))

/* WT_CELL_NEXT is the first byte of the next cell. */
#define	WT_CELL_NEXT(cell)						\
	((WT_CELL *)((uint8_t *)(cell) + WT_CELL_SPACE_REQ(WT_CELL_LEN(cell))))

/* WT_CELL_FOREACH is a loop that walks the cells on a page */
#define	WT_CELL_FOREACH(dsk, cell, i)					\
	for ((cell) = WT_PAGE_DISK_BYTE(dsk),				\
	    (i) = (dsk)->u.entries;					\
	    (i) > 0; (cell) = WT_CELL_NEXT(cell), --(i))

/*
 * WT_OFF --
 *	Row-store internal pages reference subtrees with no record count.
 *
 * WT_OFF_RECORD --
 *      Column-store internal pages reference subtrees including total record
 *	counts for the subtree.
 *
 * !!!
 * Note the initial two fields of the WT_OFF and WT_OFF_RECORD fields are the
 * same -- this is deliberate, and we use it to pass references to places that
 * only care about the addr/size information.
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
 * Btree overflow cells reference another page, and so the data is another
 * structure.
 */
struct __wt_ovfl {
	uint32_t addr;			/* Overflow address */
	uint32_t size;			/* Overflow length */
};
/*
 * WT_OVFL_SIZE is the expected structure size -- we verify the build to
 * ensure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_OVFL_SIZE	8

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

#if defined(__cplusplus)
}
#endif
