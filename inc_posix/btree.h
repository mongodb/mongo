/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * In WiredTiger there are "database allocation units", which is the smallest
 * database chunk that can be allocated.  The smallest database allocation unit
 * is 512B; the largest is 128MB.  (The maximum of 128MB is enforced by the
 * software, it could be set as high as 4GB.)  Btree leaf and internal pages,
 * as well as overflow chunks, are allocated in groups of 1 or more allocation
 * units.
 *
 * We use 32-bit unsigned integers to store file locations on database pages,
 * and all such file locations are counts of database allocation units.  In
 * the code these are called "addrs".  To simplify bookkeeping, page sizes must
 * be a multiple of the allocation unit size.
 *
 * This means the minimum maximum database file size is 2TB (2^9 x 2^32), and
 * the maximum maximum database file size is 512PB (2^27 x 2^32).
 *
 * In summary, small database allocation units limit the database file size,
 * (but minimize wasted space when storing overflow items), and when the
 * allocation unit grows, the maximum size of the database grows as well.
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

/*
 * Underneath the database layer is the cache and file layers.  In both, sizes
 * are stored as numbers of bytes.   In the cache layer, 32-bits is too small
 * (a cache might be larger than 4GB), so we use a 64-bit type.  In the file
 * layer, 32-bits might also be too small, but we have a standard type known to
 * hold the size of a file, an off_t.
 */
/* Convert a data address to/from a byte offset. */
#define	WT_ADDR_TO_OFF(db, addr)					\
	((off_t)(addr) * (db)->allocsize)
#define	WT_OFF_TO_ADDR(db, off)						\
	((uint32_t)((off) / (db)->allocsize))

/*
 * Return database allocation units needed for length (optionally including a
 * page header), rounded to an allocation unit.
 */
#define	WT_HDR_BYTES_TO_ALLOC(db, size)					\
	((uint32_t)WT_ALIGN((size) + sizeof(WT_PAGE_DISK), (db)->allocsize))

/*
 * The invalid address is the largest possible offset, which isn't a possible
 * database address.
 */
#define	WT_ADDR_INVALID		UINT32_MAX

/*
 * The database itself needs a chunk of memory that describes it.   Here's
 * the structure.  This structure is written into the first 512 bytes of
 * the file.
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
	uint64_t records;		/* 40-47: Offset record number */
	uint32_t free_addr;		/* 48-51: Free list page address */
	uint32_t free_size;		/* 52-55: Free list page length */

#define	WT_PAGE_DESC_RLE	0x01	/* Run-length encoding */
	uint32_t flags;			/* 56-59: Flags */

	uint8_t  fixed_len;		/* 60: Fixed length byte count */
	uint8_t  unused1[3];		/* 61-63: Unused */

	uint32_t unused2[112];		/* Unused */
};
/*
 * WT_PAGE_DESC_SIZE is the expected structure size -- we check at startup to
 * ensure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_PAGE_DESC_SIZE		512

/*
 * WT_PAGE --
 * The WT_PAGE structure describes the in-memory information about a database
 * page.
 */
struct __wt_page {
	/*
	 * This limits a page size to 4GB -- we could use off_t's here if we
	 * need something bigger, but the page-size configuration code limits
	 * page sizes already.
	 */
	uint32_t addr;			/* Original file allocation address */
	uint32_t size;			/* Size in bytes */

	uint64_t records;		/* Records in this subtree */

	WT_PAGE	*parent;		/* Page's parent */
	WT_OFF	*parent_off;		/* Page's parent reference */

	WT_PAGE_DISK *dsk;		/* Page's on-disk representation */

	/*
	 * We maintain 3 "generation" numbers for a page: the disk, read and
	 * write generations.
	 *
	 * The read generation is incremented each time the page is searched,
	 * and acts as an LRU value for each page in the tree; it is read by
	 * the eviction server thread to select pages to be discarded from the
	 * in-memory tree.
	 *
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
	 * to make our linked lists of inserted items too slow to search, or as
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
	uint32_t read_gen;
	uint32_t write_gen;

	/*
	 * Each in-memory page has an array of WT_ROW/WT_COL structures this is
	 * where the on-page index in DB 1.85 and Berkeley DB is created when a
	 * page is read from the file.  It's sorted by the key, fixed in size,
	 * and references data on the page.
	 *
	 * Complications:
	 *
	 * In WT_PAGE_ROW_LEAF pages there may be duplicate data items; in those
	 * cases, there is a single indx entry per key/data pair, but multiple
	 * indx entries reference the same memory location.
	 *
	 * In column store fixed-length run-length encoded pages (that is,
	 * WT_PAGE_COL_RLE type pages), a single indx entry may reference a
	 * large number of records, because there's a single on-page entry that
	 * represents many identical records.   (We can't expand those entries
	 * when the page comes into memory because that'd require unacceptable
	 * resources as pages are moved to/from the cache, including read-only
	 * files.)  Instead, a single indx entry represents all of the identical
	 * records originally found on the page.
	 */
	uint32_t indx_count;		/* On-disk entry count */
	union {				/* On-disk entry index */
		WT_COL	*icol;		/* On-disk column store entries */
		WT_ROW	*irow;		/* On-disk row store entries */
		void	*indx;		/* Generic index reference */
	} u;

	/*
	 * Data modifications or deletions are stored in the replacement array.
	 * When the first element on a page is modified, the array is allocated,
	 * with one slot for every existing element in the page.  A slot points
	 * to a WT_REPL structure; if more than one modification is done to a
	 * single entry, the WT_REPL structures are formed into a forward-linked
	 * list.
	 *
	 * Modifying (or deleting) run-length encoded column store records is
	 * problematical, because the index entry would no longer reference
	 * a set of identical items.  We handle this by "inserting" a new entry
	 * into an array that behaves much like the rinsert array.  This is the
	 * only case where it's possible to "insert" into a column store -- it's
	 * normally only possible to append to a column store as insert requires
	 * re-numbering all subsequent records.  (Berkeley DB did support the
	 * re-numbering functionality, but it won't perform well and it isn't
	 * useful enough to re-implement, IMNSHO.)
	 */
	union {
		WT_REPL	      **repl;	/* Modification/deletion index */
		WT_RLE_EXPAND **rleexp;	/* RLE expansion index */
	} u2;

	/*
	 * Subtree references are stored in the ref array.   When a page that
	 * references a subtree (where a subtree may be a single page), is read
	 * into memory, the ref array is populated with entries that can be
	 * used to bring the subtree page into memory.  That happens both for
	 * internal page types:
	 *	WT_PAGE_COL_INT
	 *	WT_PAGE_DUP_INT
	 *	WT_PAGE_ROW_INT
	 * and row-store leaf pages:
	 *	WT_PAGE_ROW_LEAF
	 * because row-store leaf pages reference off-page duplicate trees.
	 */
	union {
		WT_REF	 *ref;		/* Internal page references */
#define	WT_PAGE_DUP_TREES(p)		((p)->u3.dup != NULL)
		WT_REF	**dup;		/* Row-store off-page duplicate trees */
	} u3;

	uint32_t flags;
};
/*
 * WT_PAGE_SIZE is the expected structure size -- we check at startup to ensure
 * the compiler hasn't inserted padding.  The WT_PAGE structure is in-memory, so
 * padding it won't break the world, but we don't want to waste space, and there
 * are a lot of these structures.
 */
#define	WT_PAGE_SIZE	(6 * sizeof(void *) + 9 * sizeof(uint32_t))

/*
 * There are 4 different arrays which map one-to-one to the original on-disk
 * index: repl, rleexp, ref and dup.
 *
 * The WT_{COL,ROW}_SLOT macros return the appropriate array slot based on a
 * WT_{COL,ROW} reference.
 */
#define	WT_COL_SLOT(page, ip)	((uint32_t)((WT_COL *)(ip) - (page)->u.icol))
#define	WT_ROW_SLOT(page, ip)	((uint32_t)((WT_ROW *)(ip) - (page)->u.irow))

/*
 * The ref array is different from the other three in two ways: first, it
 * always exists on internal pages, and we don't need to test to see if it's
 * there.  Second, it's an array of structures, not an array of pointers to
 * individually allocated structures.  The WT_{COL,ROW}_REF macros return
 * the appropriate entry based on a WT_{COL,ROW} reference.
 */
#define	WT_COL_REF(page, ip)						\
	(&((page)->u3.ref[WT_COL_SLOT(page, ip)]))
#define	WT_ROW_REF(page, ip)						\
	(&((page)->u3.ref[WT_ROW_SLOT(page, ip)]))

/*
 * The other arrays may not exist, and are arrays of pointers to individually
 * allocated structures.   The following macros return an array entry if the
 * array of pointers and the specific structure exist, otherwise NULL.
 */
#define	__WT_COL_ARRAY(page, ip, field)					\
	((page)->field == NULL ? NULL : (page)->field[WT_COL_SLOT(page, ip)])
#define	WT_COL_REPL(page, ip)	__WT_COL_ARRAY(page, ip, u2.repl)
#define	WT_COL_RLEEXP(page, ip)	__WT_COL_ARRAY(page, ip, u2.rleexp)
#define	__WT_ROW_ARRAY(page, ip, field)					\
	((page)->field == NULL ? NULL : (page)->field[WT_ROW_SLOT(page, ip)])
#define	WT_ROW_REPL(page, ip)	__WT_ROW_ARRAY(page, ip, u2.repl)
#define	WT_ROW_DUP(page, ip)	__WT_ROW_ARRAY(page, ip, u3.dup)

/*
 * WT_REF --
 *	Page references: each references a single page, and it's the structure
 *	used to determine if it's OK to dereference the pointer to the page.
 *
 * There may be many threads traversing these entries; they fall into three
 * classes: (1) application threads walking through the tree searching database
 * database pages or calling a method like Db.sync; (2) a server thread reading
 * a new page into the tree from disk; (3) a server thread evicting a page from
 * the tree to disk.
 *
 * Synchronization is based on the WT_REF->state field:
 * WT_OK:
 *	The page reference is valid.  Readers check the state field and if it's
 *	set to WT_OK, they set a hazard reference to the page, flush memory and
 *	re-confirm the state of the page.  If the page is still WT_OK, they have
 *	a valid reference and can proceed.
 * WT_EVICT:
 *	The eviction server chose this page and is checking hazard references.
 *	When the eviction server wants to discard a page from the tree, it sets
 *	state to WT_EVICT, flushes memory, then checks hazard references.  If
 *	the eviction server finds a hazard reference, it resets the state to
 *	WT_OK, restoring the page to the readers.  If the eviction server does
 *	not find a hazard reference, the page is evicted.
 * WT_EMPTY:
 *      There is no page, and the only way to proceed is for the reader thread
 *	to set the page reference and change the state to WT_OK.
 */
struct __wt_ref {
	WT_PAGE *page;			/* In-memory page */

#define	WT_OK		0		/* Reference valid */
#define	WT_EMPTY	1		/* Not set */
#define	WT_EVICT	2		/* Selected for eviction */
	uint32_t volatile state;
};

/*
 * WT_REPL --
 *	Updates/deletes for a WT_{COL,ROW} entry.
 */
struct __wt_repl {
	WT_TOC_UPDATE *update;		/* update buffer holding this WT_REPL */
	WT_REPL *next;			/* forward-linked list */

	/*
	 * We can't store 4GB items:  we're short by a few bytes because each
	 * change/insert item requires a leading WT_REPL structure.  For that
	 * reason, we can use the maximum size as an is-deleted flag and don't
	 * have to increase the size of this structure for a flag bit.
	 */
#define	WT_REPL_DELETED_ISSET(repl)	((repl)->size == UINT32_MAX)
#define	WT_REPL_DELETED_SET(repl)	((repl)->size = UINT32_MAX)
	uint32_t size;			/* data length */

	/* The data immediately follows the repl structure. */
#define	WT_REPL_DATA(repl)						\
	((void *)((uint8_t *)repl + sizeof(WT_REPL)))
};

/*
 * WT_PAGE_DISK --
 *
 * All on-disk database pages have a common header, declared as the WT_PAGE_DISK
 * structure.  The header has no version number or mode bits, and the page type
 * and/or flags value will have to be modified when changes are made to the page
 * layout.  (The page type appears early in the header to make this simpler.)
 * In other words, the page type declares the contents of the page and how to
 * read it.
 *
 * For more information on page layouts and types, see the file btree_layout.
 */
struct __wt_page_disk {
	/*
	 * The record number of the first record on the page is stored for two
	 * reasons: first, we have to find the page's stack when reconciling
	 * leaf pages and second, when salvaging a database it's the only way
	 * to know where a column-store page fits in the keyspace.  (We could
	 * work around the first reason by storing the base record number in
	 * lthe WT_PAGE structure when we read a page into memory, but we can't
	 * work around the second reason.)
	 */
	uint64_t start_recno;		/* 00-07: column-store starting recno */
	uint32_t lsn[2];		/* 08-15: LSN */

	uint32_t checksum;		/* 16-19: checksum */

	union {
		uint32_t entries;	/* 20-23: number of items on page */
		uint32_t datalen;	/* 20-23: overflow data length */
	} u;

#define	WT_PAGE_INVALID		 0	/* Invalid page */
#define	WT_PAGE_FREE	 	 1	/* Page on the free list */
#define	WT_PAGE_COL_FIX		 2	/* Col store fixed-len leaf */
#define	WT_PAGE_COL_INT		 3	/* Col store internal page */
#define	WT_PAGE_COL_RLE	 	 4	/* Col store run-length encoded leaf */
#define	WT_PAGE_COL_VAR		 5	/* Col store var-length leaf page */
#define	WT_PAGE_DUP_INT		 6	/* Duplicate tree internal page */
#define	WT_PAGE_DUP_LEAF	 7	/* Duplicate tree leaf page */
#define	WT_PAGE_OVFL		 8	/* Overflow page */
#define	WT_PAGE_ROW_INT		 9	/* Row-store internal page */
#define	WT_PAGE_ROW_LEAF	10	/* Row-store leaf page */
	uint8_t type;			/* 24: page type */

	/*
	 * WiredTiger is no-overwrite: each time a page is written, it's written
	 * to an unused disk location so torn writes don't corrupt the database.
	 * This means that writing a page requires updating the page's parent to
	 * reference the new location.  We don't want to repeatedly write the
	 * parent on a database flush, so we sort the pages for writing based on
	 * their level in the tree.
	 *
	 * We don't need the tree level on disk and we could move this field to
	 * the WT_PAGE structure -- that said, it's only a byte, and it's a lot
	 * harder to figure out the tree level when reading a page into memory
	 * than to set it once when the page is created.
	 *
	 * Leaf pages are level 1, each higher level of the tree increases by 1.
	 * The maximum tree level is 255, larger than any practical fan-out.
	 */
#define	WT_NOLEVEL	0
#define	WT_LLEAF	1
	uint8_t level;			/* 25: tree level */

	/*
	 * It would be possible to decrease the size of the page header by two
	 * bytes by only writing out the first 26 bytes of the structure to the
	 * page, but I'm not bothering -- I don't think the space is worth it
	 * and having a little bit of on-page data to play with in the future
	 * can be a good thing.
	 */
	uint8_t unused[2];		/* 26-27: unused padding */
};
/*
 * WT_PAGE_DISK_SIZE is the expected structure size --  we check at startup to
 * ensure the compiler hasn't inserted padding (which would break the world).
 * The size must also be a multiple of a 4-byte boundary, because the header
 * is followed by WT_ITEM structures, which require 4-byte alignment.
 */
#define	WT_PAGE_DISK_SIZE		28

/*
 * WT_PAGE_BYTE is the first usable data byte on the page.
 */
#define	WT_PAGE_BYTE(page)						\
	((void *)(((uint8_t *)(page)->dsk) + WT_PAGE_DISK_SIZE))

/*
 * WT_ROW --
 * The WT_ROW structure describes the in-memory information about a single
 * key/data pair on a row store database page.
 */
struct __wt_row {
	/*
	 * WT_ROW structures are used to describe pages where there's a sort
	 * key (that is, a row-store, not a column-store, which is "sorted"
	 * by record number).
	 *
	 * The first fields of the WT_ROW structure are the same as the first
	 * fields of a DBT so we can hand it to a comparison function without
	 * copying (this is important for keys on internal pages).
	 *
	 * If a key requires processing (for example, an overflow key or an
	 * Huffman encoded key), the key field points to the on-page key,
	 * but the size is set to 0 to indicate the key is not yet processed.
	 */
	void	 *key;			/* Key */
	uint32_t size;			/* Key length */

	void	 *data;			/* Data */
};
/*
 * WT_ROW_SIZE is the expected structure size -- we check at startup to ensure
 * the compiler hasn't inserted padding.  The WT_ROW structure is in-memory, so
 * padding it won't break the world, but we don't want to waste space, and there
 * are a lot of these structures.
 */
#define	WT_ROW_SIZE	(2 * sizeof(void *) + sizeof(uint32_t))

/*
 * WT_ROW_INSERT --
 * The WT_ROW_INSERT structure describes the in-memory information about an
 * inserted key/data pair on a row store database page.
 */
struct __wt_row_insert {
	WT_ROW	entry;			/* key/data pair */
	WT_REPL *repl;			/* modifications/deletions */

	WT_ROW_INSERT *next;		/* forward-linked list */
};

/*
 * WT_COL --
 * The WT_COL structure describes the in-memory information about a single
 * item on a column-store database page.
 */
struct __wt_col {
	/*
	 * The on-page data is untyped for column-store pages -- if the page
	 * has variable-length objects, it's a WT_ITEM layout, like row-store
	 * pages.  If the page has fixed-length objects, it's untyped bytes.
	 */
	void	 *data;			/* on-page data */
};
/*
 * WT_COL_SIZE is the expected structure size --  we check at startup to ensure
 * the compiler hasn't inserted padding.  The WT_COL structure is in-memory, so
 * padding it won't break the world, but we don't want to waste space, and there
 * are a lot of these structures.
 */
#define	WT_COL_SIZE	(sizeof(void *))

/*
 * WT_RLE_EXPAND --
 * The WT_RLE_EXPAND structure describes the in-memory information about a
 * replaced key/data pair on a run-length encoded, column store database page.
 */
struct __wt_rle_expand {
	uint64_t recno;			/* recno */

	WT_REPL *repl;                  /* modifications/deletions */

	WT_RLE_EXPAND *next;		/* forward-linked list */
};

/*
 * WT_INDX_FOREACH --
 *	Walk the indexes of an in-memory page: works for both WT_ROW and WT_COL,
 * based on the type of ip.
 */
#define	WT_INDX_FOREACH(page, ip, i)					\
	for ((i) = (page)->indx_count,					\
	    (ip) = (page)->u.indx; (i) > 0; ++(ip), --(i))
/*
 * WT_ROW_INDX_IS_DUPLICATE --
 *	Compare the WT_ROW entry against the previous entry and return 1 if
 *	it's a duplicate key.
 */
#define	WT_ROW_INDX_IS_DUPLICATE(page, ip)				\
	(((ip) > (page)->u.irow && (ip)->key == ((ip) - 1)->key) ? 1 : 0)

/*
 * WT_REPL_FOREACH --
 * Macro to walk the replacement array of an in-memory page.
 */
#define	WT_REPL_FOREACH(page, replp, i)					\
	for ((i) = (page)->indx_count,					\
	    (replp) = (page)->u2.repl; (i) > 0; ++(replp), --(i))

/*
 * WT_RLE_EXPAND_FOREACH --
 * Macro to walk the run-length encoded column store expansion  array of an
 * in-memory page.
 */
#define	WT_RLE_EXPAND_FOREACH(page, exp, i)				\
	for ((i) = (page)->indx_count,					\
	    (exp) = (page)->u2.rleexp; (i) > 0; ++(exp), --(i))

/*
 * WT_REF_FOREACH --
 * Macro to walk the off-page subtree array of an in-memory internal page.
 */
#define	WT_REF_FOREACH(page, ref, i)					\
	for ((i) = (page)->indx_count,					\
	    (ref) = (page)->u3.ref; (i) > 0; ++(ref), --(i))

/*
 * WT_DUP_FOREACH --
 * Macro to walk the off-page duplicate array of an in-memory row-store page.
 */
#define	WT_DUP_FOREACH(page, dupp, i)					\
	for ((i) = (page)->indx_count,					\
	    (dupp) = (page)->u3.dup; (i) > 0; ++(dupp), --(i))

/*
 * On both row- and column-store internal pages, the on-page data referenced
 * by the WT_ROW/WT_COL data field is a WT_OFF structure, which contains a
 * record count and a page addr/size pair.   Macros to reach into the on-page
 * structure and return the values.
 */
#define	WT_COL_OFF(ip)							\
	((WT_OFF *)(((WT_COL *)ip)->data))
#define	WT_COL_OFF_RECORDS(ip)						\
	WT_RECORDS(WT_COL_OFF(ip))

#define	WT_ROW_OFF(ip)							\
	((WT_OFF *)WT_ITEM_BYTE(((WT_ROW *)ip)->data))
#define	WT_ROW_OFF_RECORDS(ip)						\
	WT_RECORDS(WT_ROW_OFF(ip))

/*
 * WT_ITEM --
 *	Trailing data length (in bytes) plus item type.
 *
 * After the page header, on pages with variable-length data, there are
 * variable-length items (all page types except WT_PAGE_COL_{INT,FIX,RLE}),
 * comprised of a list of WT_ITEMs in sorted order.  Or, specifically, 4
 * bytes followed by a variable length chunk.
 *
 * The first 8 bits of that 4 bytes holds an item type, followed by an item
 * length.  The item type defines the following set of bytes and the item
 * length specifies how long the item is.
 *
 * We encode the length and type in a 4-byte value to minimize the on-page
 * footprint as well as maintain alignment of the bytes that follow the item.
 * (The trade-off is this limits on-page database key or data items to 16MB.)
 * The bottom 24-bits are the length of the subsequent data, the next 4-bits are
 * the type, and the top 4-bits are unused.   We could use the unused 4-bits to
 * provide more length, but 16MB seems sufficient for on-page items.
 *
 * The __item_chunk field should never be directly accessed, there are macros
 * to extract the type and length.
 *
 * WT_ITEMs are aligned to a 4-byte boundary, so it's OK to directly access the
 * __item_chunk field on the page.
 */
#define	WT_ITEM_MAX_LEN	(16 * 1024 * 1024 - 1)
struct __wt_item {
	uint32_t __item_chunk;
};
/*
 * WT_ITEM_SIZE is the expected structure size --  we check at startup to make
 * sure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_ITEM_SIZE	4

/*
 * There are 4 basic types: keys, duplicate keys, data items and duplicate data
 * items, each of which has an overflow form.  Items are followed by additional
 * data, which varies by type: a key, duplicate key, data or duplicate item is
 * followed by a set of bytes; a WT_OVFL structure follows an overflow form.
 * There are two additional types: First, a deleted type (a place-holder for
 * deleted items where the item cannot be removed, for example, an column store
 * item that must remain to preserve the record count).   Second, a subtree
 * reference for keys that reference subtrees of information (for example, an
 * internal Btree page has a key and a reference to the tree that contains all
 * key/data pairs greater than the internal page's key, or, a leaf Btree page
 * where a key references all of the duplicate data items for the key when the
 * duplicate data items can no longer fit onto the Btree leaf page).
 *
 * Here's the usage by page type:
 *
 * WT_PAGE_ROW_INT (row-store internal pages):
 * -- Variable-length key and offpage-reference pairs (a WT_ITEM_KEY or
 *    WT_ITEM_KEY_OVFL item, followed by a WT_ITEM_OFF item).
 *
 * WT_PAGE_ROW_LEAF (row-store leaf pages):
 * -- Variable-length key and variable-length/data pairs (a WT_ITEM_KEY or
 *    WT_ITEM_KEY_OVFL item followed by a WT_ITEM_DATA or WT_ITEM_DATA_OVFL
 *    item);
 * -- Variable-length key and set of duplicates moved into a separate tree
 *    (a WT_ITEM_KEY or WT_ITEM_KEY_OVFL item followed by a WT_ITEM_OFF item);
 * -- Variable-length key and set of duplicates not yet moved into a separate
 *    tree (a WT_ITEM_KEY/KEY_OVFL item followed by two or more
 *    WT_ITEM_DATA_DUP or WT_ITEM_DATA_DUP_OVFL items).
 *
 * WT_PAGE_DUP_INT (row-store offpage duplicates internal pages):
 * -- Variable-length duplicate key and offpage-reference pairs (a
 *    WT_ITEM_KEY_DUP or WT_ITEM_DATA_DUPKEY_OVFL item followed by a
 *    WT_ITEM_OFF item).
 *
 * WT_PAGE_DUP_LEAF (row-store offpage duplicates leaf pages):
 * -- Variable-length data items (WT_ITEM_DATA_DUP/DUP_OVFL_ITEM).
 *
 * WT_PAGE_COL_VAR (Column-store leaf page storing variable-length items):
 * -- Variable-length data items (WT_ITEM_DATA/DATA_OVFL/DEL).
 *
 * WT_PAGE_COL_INT (Column-store internal page):
 * WT_PAGE_COL_FIX (Column-store leaf page storing fixed-length items):
 * WT_PAGE_COL_RLE (Column-store leaf page storing fixed-length items):
 * WT_PAGE_OVFL (Overflow page):
 *	These pages contain fixed-sized structures (WT_PAGE_COL_{INT,FIX,RLE}),
 *	or a string of bytes (WT_PAGE_OVFL), not WT_ITEM structures.
 *
 * There are currently 10 item types, requiring 4 bits, with 6 values unused.
 *
 * We could compress the item types in a couple of ways.  We could merge the
 * WT_ITEM_KEY and WT_ITEM_KEY_DUP types, but that would require we know the
 * underlying page type in order to know how an item might be encoded (that
 * is, if it's an off-page duplicate key, encoded using the Huffman data coder,
 * or a Btree row store key, encoded using the Huffman key encoder). We could
 * also use a bit to mean overflow, merging all overflow types into a single
 * bit plus the ""primary" item type, but that would require more bit shuffling
 * than the current scheme.
 */
#define	WT_ITEM_KEY		0x00000000 /* Key */
#define	WT_ITEM_KEY_OVFL	0x01000000 /* Key: overflow */
#define	WT_ITEM_KEY_DUP		0x02000000 /* Key: dup internal tree */
#define	WT_ITEM_KEY_DUP_OVFL	0x03000000 /* Key: dup internal tree overflow */
#define	WT_ITEM_DATA		0x04000000 /* Data */
#define	WT_ITEM_DATA_OVFL	0x05000000 /* Data: overflow */
#define	WT_ITEM_DATA_DUP	0x06000000 /* Data: duplicate */
#define	WT_ITEM_DATA_DUP_OVFL	0x07000000 /* Data: duplicate overflow */
#define	WT_ITEM_DEL		0x08000000 /* Deleted */
#define	WT_ITEM_OFF		0x09000000 /* Off-page reference */

#define	WT_ITEM_TYPE(addr)						\
	(((WT_ITEM *)(addr))->__item_chunk & 0x0f000000)
#define	WT_ITEM_LEN(addr)						\
	(((WT_ITEM *)(addr))->__item_chunk & 0x00ffffff)
#define	WT_ITEM_SET(addr, type, size)					\
	(((WT_ITEM *)(addr))->__item_chunk = (type) | (size))
#define	WT_ITEM_SET_LEN(addr, size)					\
	WT_ITEM_SET(addr, WT_ITEM_TYPE(addr), size)
#define	WT_ITEM_SET_TYPE(addr, type)					\
	WT_ITEM_SET(addr, type, WT_ITEM_LEN(addr))

/* WT_ITEM_BYTE is the first data byte for an item. */
#define	WT_ITEM_BYTE(addr)						\
	((uint8_t *)(addr) + sizeof(WT_ITEM))

/*
 * On row-store pages, the on-page data referenced by the WT_ROW data field
 * may be a WT_OVFL (which contains the address for the start of the overflow
 * pages and its length), or a WT_OFF structure.  These macros do the cast
 * to the right type.
 */
#define	WT_ITEM_BYTE_OFF(addr)						\
	((WT_OFF *)(WT_ITEM_BYTE(addr)))
#define	WT_ITEM_BYTE_OVFL(addr)						\
	((WT_OVFL *)(WT_ITEM_BYTE(addr)))

/*
 * Bytes required to store a WT_ITEM followed by additional bytes of data.
 * Align the WT_ITEM and the subsequent data to a 4-byte boundary so the
 * WT_ITEMs on a page all start at a 4-byte boundary.
 */
#define	WT_ITEM_SPACE_REQ(size)						\
	WT_ALIGN(sizeof(WT_ITEM) + (size), sizeof(uint32_t))

/* WT_ITEM_NEXT is the first byte of the next item. */
#define	WT_ITEM_NEXT(item)						\
	((WT_ITEM *)((uint8_t *)(item) + WT_ITEM_SPACE_REQ(WT_ITEM_LEN(item))))

/* WT_ITEM_FOREACH is a loop that walks the items on a page */
#define	WT_ITEM_FOREACH(page, item, i)					\
	for ((item) = (WT_ITEM *)WT_PAGE_BYTE(page),			\
	    (i) = (page)->dsk->u.entries;				\
	    (i) > 0; (item) = WT_ITEM_NEXT(item), --(i))

/*
 * WT_OFF --
 *	Btree internal items and offpage duplicates reference another tree.
 */
struct __wt_off {
/*
 * Solaris and the gcc compiler on Linux pad the WT_OFF structure because of the
 * 64-bit records field.   This is an on-disk structure, which means we have to
 * have a fixed size, without padding, so we declare it as two 32-bit fields and
 * cast it.  We haven't yet found a compiler that aligns the 32-bit fields such
 * that a cast won't work; if we find one, we'll have to go to bit masks, or to
 * reading/write the bytes to/from a local variable.
 */
#define	WT_RECORDS(offp)	(*(uint64_t *)(&(offp)->__record_chunk[0]))
	uint32_t __record_chunk[2];	/* Subtree record count */
	uint32_t addr;			/* Subtree root page address */
	uint32_t size;			/* Subtree root page length */
};
/*
 * WT_OFF_SIZE is the expected structure size -- we check at startup to
 * ensure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_OFF_SIZE	16

/* WT_OFF_FOREACH is a loop that walks offpage references on a page */
#define	WT_OFF_FOREACH(page, offp, i)					\
	for ((offp) = (WT_OFF *)WT_PAGE_BYTE(page),			\
	    (i) = (page)->dsk->u.entries; (i) > 0; ++(offp), --(i))

/*
 * Btree overflow items reference another page, and so the data is another
 * structure.
 */
struct __wt_ovfl {
	uint32_t addr;			/* Overflow address */
	uint32_t size;			/* Overflow length */
};
/*
 * WT_OVFL_SIZE is the expected structure size --  we check at startup to
 * ensure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_OVFL_SIZE	8

/*
 * On-page "deleted" flags for fixed-length column store data items -- steal
 * the top bit of the data.
 */
#define	WT_FIX_DELETE_BYTE	0x80
#define	WT_FIX_DELETE_ISSET(b)	(((uint8_t *)(b))[0] & WT_FIX_DELETE_BYTE)
#define	WT_FIX_DELETE_SET(b)	(((uint8_t *)(b))[0] = WT_FIX_DELETE_BYTE)

/* WT_FIX_FOREACH is a loop that walks fixed-length references on a page. */
#define	WT_FIX_FOREACH(db, page, p, i)					\
	for ((p) = WT_PAGE_BYTE(page),					\
	    (i) = (page)->dsk->u.entries; (i) > 0; --(i),		\
	    (p) = (uint8_t *)(p) + (db)->fixed_len)

/*
 * WT_RLE_REPEAT_FOREACH is a loop that walks fixed-length, run-length encoded
 * entries on a page.
 */
#define	WT_RLE_REPEAT_FOREACH(db, page, p, i)				\
	for ((p) = WT_PAGE_BYTE(page),					\
	    (i) = (page)->dsk->u.entries; (i) > 0; --(i),		\
	    (p) = (uint8_t *)(p) + (db)->fixed_len + sizeof(uint16_t))

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
#define	WT_RLE_REPEAT_ITERATE(db, page, p, i, j)			\
	WT_RLE_REPEAT_FOREACH(db, page, p, i)				\
		for ((j) = WT_RLE_REPEAT_COUNT(p); (j) > 0; --(j))

#if defined(__cplusplus)
}
#endif
