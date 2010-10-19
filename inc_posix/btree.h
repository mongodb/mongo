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
	((u_int32_t)((off) / (db)->allocsize))

/*
 * Return database allocation units needed for length (optionally including a
 * page header), rounded to an allocation unit.
 */
#define	WT_BYTES_TO_ALLOC(db, size)					\
	((u_int32_t)WT_ALIGN((size), (db)->allocsize))
#define	WT_HDR_BYTES_TO_ALLOC(db, size)					\
	WT_BYTES_TO_ALLOC(db, (size) + sizeof(WT_PAGE_HDR))

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
	u_int32_t magic;		/* 00-03: Magic number */
#define	WT_BTREE_MAJOR_VERSION	0
	u_int16_t majorv;		/* 04-05: Major version */
#define	WT_BTREE_MINOR_VERSION	1
	u_int16_t minorv;		/* 06-07: Minor version */

#define	WT_BTREE_INTLMAX_DEFAULT	(2 * 1024)
#define	WT_BTREE_INTLMIN_DEFAULT	(2 * 1024)
	u_int32_t intlmax;		/* 08-11: Maximum intl page size */
	u_int32_t intlmin;		/* 12-15: Minimum intl page size */

#define	WT_BTREE_LEAFMAX_DEFAULT	WT_MEGABYTE
#define	WT_BTREE_LEAFMIN_DEFAULT	(32 * 1024)
	u_int32_t leafmax;		/* 16-19: Maximum leaf page size */
	u_int32_t leafmin;		/* 20-23: Minimum leaf page size */

	u_int64_t base_recno;		/* 24-31: Base record number */
	u_int32_t root_addr;		/* 32-35: Root page address */
	u_int32_t root_size;		/* 36-39: Root page length */
	u_int32_t free_addr;		/* 40-43: Free list page address */
	u_int32_t free_size;		/* 44-47: Free list page length */

#define	WT_PAGE_DESC_REPEAT	0x01	/* Repeat count compression */
#define	WT_PAGE_DESC_MASK	0x01	/* Valid bit mask */
	u_int32_t flags;		/* 48-51: Flags */

	u_int8_t  fixed_len;		/* 51-52: Fixed length byte count */
	u_int8_t  unused1[3];		/* Unused */

	u_int32_t unused2[114];		/* Unused */
};
/*
 * WT_PAGE_DESC_SIZE is the expected structure size -- we check at startup to
 * ensure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_PAGE_DESC_SIZE		512

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
	 * reason, we use a max size as an is-deleted flag so we don't have to
	 * increase the size of this structure.
	 */
#define	WT_REPL_DELETED_ISSET(repl)	((repl)->size == UINT32_MAX)
#define	WT_REPL_DELETED_SET(repl)	((repl)->size = UINT32_MAX)
	u_int32_t size;			/* data length */

	/* The data immediately follows the repl structure. */
#define	WT_REPL_DATA(repl)		((u_int8_t *)repl + sizeof(WT_REPL))
};

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
	u_int32_t addr;			/* Page's file allocation address */
	u_int32_t size;			/* Page size */

	WT_PAGE_HDR *hdr;		/* Page's on-disk representation */

	u_int8_t *first_free;		/* Page's first free byte address */
	u_int32_t space_avail;		/* Page's available memory */

	u_int64_t records;		/* Records in this subtree */

	/*
	 * The page's LRU access generation is set on each cache retrieval and
	 * used to find pages no longer useful in the cache.
	 */
	u_int32_t lru;			/* Read generation */

	/*
	 * The page modified flag is set by the workQ thread when it modifies
	 * a page: this means (1) the page has a hazard reference, and (2) no
	 * other thread is modifying the page when the modified field is set.
	 * For those reasons, the modified field doesn't need to be an atomic
	 * set, and could even be a bit flag.
	 *
	 * The modified set must be flushed before the page's hazard reference
	 * is released and the cache drain server is able to select this page.
	 * We rely on the memory flush in the workQ server loop (which happens
	 * before control returns to the calling thread, that is, before control
	 * returns to the thread that's holding the hazard reference), so there
	 * is no need to flush explicitly.
	 */
	u_int16_t modified;		/* Page is modified */
#define	WT_PAGE_MODIFY_ISSET(p)		((p)->modified)
#define	WT_PAGE_MODIFY_SET(p)		((p)->modified = 1)
#define	WT_PAGE_MODIFY_CLR(p)		((p)->modified = 0)

	/*
	 * The page's write-generation value is used to detect workQ changes
	 * scheduled, but based on out-of-date information.  If two threads
	 * of control update a single entry on a page, they could both search
	 * the page in state A, and schedule the change for the workQ.  Since
	 * the workQ performs the changes serially, one of the changes would
	 * happen after the page was modified, and so the search state would
	 * no longer be applicable.
	 *
	 * The write generation number is updated before the workQ modifies
	 * a page, and the page search routines copy the write generation
	 * number in the WT_TOC structure before searching the page.  Based
	 * on this, the workQ can examine the write generation number and if
	 * it isn't current, return WT_RESTART to restart the operation.
	 *
	 * The write-generation value could be moved to a per-entry basis if
	 * there's enough contention for a page.
	 *
	 * XXX
	 * 64KB seems big enough: at some point we're going to have to clean
	 * up pages as soon as there have been sufficient modifications to
	 * make our linked lists too slow to search.
	 */
	u_int16_t write_gen;		/* Write generation */
#define	WT_PAGE_WRITE_GEN(p)						\
	((p)->write_gen)

	/*
	 * Each disk page entry is referenced by an array of WT_ROW/WT_COL
	 * structures: this is where the on-page index in DB 1.85 and Berkeley
	 * DB is re-created, when a page is read into the cache.  It's sorted
	 * by the key, fixed in size, and references data on the page.
	 *
	 * Complications:
	 *
	 * In row store leaf pages there may be duplicate data items; in those
	 * cases, there is a single indx entry per key/data pair, but multiple
	 * indx entries will reference the same physical key from the original
	 * on-disk page.
	 *
	 * In column store repeat-count compressed fixed-length pages, a single
	 * indx entry may reference a large number of records, because there's
	 * a single on-page entry that represents many identical records.   (We
	 * can't expand those entries when the page comes into memory because
	 * that'd require unacceptable resources as pages are moved to/from the
	 * cache, including for read-only databases.)  Instead, a single indx
	 * entry represents all of the identical records.
	 */
	u_int32_t indx_count;		/* On-disk entry count */
	union {				/* On-disk entry index */
		WT_COL *icol;		/* On-disk column store entries */
		WT_ROW *irow;		/* On-disk row store entries */
		void *indx;
	} u;

	/*
	 * Data modifications or deletions are stored in the replacement array.
	 * When the first element on a page is modified, the array is allocated,
	 * with one slot for every existing element in the page.  A slot points
	 * to a WT_REPL structure; if more than one modification is done to a
	 * single entry, the WT_REPL structures are formed into a forward-linked
	 * list.
	 */
	WT_REPL **repl;			/* Modification index */

	/*
	 * Row store page insertions are stored in the insrow array.  When the
	 * first insertion is done to a row store page, the array is allocated,
	 * with one slot for every existing element in the page.  Each slot is
	 * a forward-linked list of new entries sorting greater than or equal to
	 * the entry with the same array offset in the original index.  Sorting
	 * is key, if you'll pardon the phrase: it has to be sorted or we can't
	 * search it efficiently.  Slots point to WT_ROW_INSERT structures.
	 */
	WT_ROW_INSERT **insrow;

	/*
	 * Modifying (or deleting) repeat-count compressed column store records
	 * is problematical, because the index entry would no longer reference
	 * a set of identical items.  We handle this by "inserting" a new entry
	 * into an array that behaves much like the rinsert array.  This is the
	 * only case where it's possible to insert into a column store -- it's
	 * normally only possible to append to a column store as insert requires
	 * re-numbering all subsequent records.  (Berkeley DB did support that
	 * functionality, but it never performed well and it isn't useful enough
	 * to re-implement, IMNSHO.)
	 */
	WT_COL_EXPAND **expcol;

	u_int32_t flags;
};

/*
 * WT_{COL,ROW}_SLOT --
 * There are 3 different arrays which map one-to-one to the original on-disk
 * index: repl, insrow and expcol.  WT_{COL,ROW}_SLOT returns the offset.
 *
 * WT_{COL,ROW}_ARRAY --
 * Return the the appropriate entry for one of the three arrays, or NULL if
 * there's no such entry.
 */
#define	WT_COL_SLOT(page, ip)	((WT_COL *)(ip) - (page)->u.icol)
#define	WT_COL_ARRAY(page, ip, array)					\
	((page)->array == NULL ? NULL : page->array[WT_COL_SLOT(page, ip)])
#define	WT_COL_REPL(page, ip)	WT_COL_ARRAY(page, ip, repl)
#define	WT_COL_EXPCOL(page, ip)	WT_COL_ARRAY(page, ip, expcol)

#define	WT_ROW_SLOT(page, ip)	((WT_ROW *)(ip) - (page)->u.irow)
#define	WT_ROW_ARRAY(page, ip, array)					\
	((page)->array == NULL ? NULL : page->array[WT_ROW_SLOT(page, ip)])
#define	WT_ROW_REPL(page, ip)	WT_ROW_ARRAY(page, ip, repl)
#define	WT_ROW_INSROW(page, ip)	WT_ROW_ARRAY(page, ip, insrow)

/*
 * WT_PAGE_HDR --
 *
 * All on-disk database pages have a common header, declared as the WT_PAGE_HDR
 * structure.  The header has no version number or mode bits, and the page type
 * and/or flags value will have to be modified when changes are made to the page
 * layout.  (The page type appears early in the header to make this simpler.)
 * In other words, the page type declares the contents of the page and how to
 * read it.
 *
 * For more information on page layouts and types, see the file btree_layout.
 */
struct __wt_page_hdr {
	u_int32_t lsn[2];		/* 00-07: LSN */

	u_int32_t checksum;		/* 08-11: checksum */

#define	WT_PAGE_INVALID		0	/* Invalid page */
#define	WT_PAGE_DESCRIPT	1	/* Database description page */
#define	WT_PAGE_COL_FIX		2	/* Col store fixed-length leaf page */
#define	WT_PAGE_COL_INT		3	/* Col store internal page */
#define	WT_PAGE_COL_VAR		4	/* Col store var-length leaf page */
#define	WT_PAGE_DUP_INT		5	/* Duplicate tree internal page */
#define	WT_PAGE_DUP_LEAF	6	/* Duplicate tree leaf page */
#define	WT_PAGE_OVFL		7	/* Overflow page */
#define	WT_PAGE_ROW_INT		8	/* Row-store internal page */
#define	WT_PAGE_ROW_LEAF	9	/* Row-store leaf page */
	u_int8_t type;			/* 12: page type */

	/*
	 * WiredTiger is no-overwrite: each time a page is written, it's written
	 * to an unused disk location so torn writes can't corrupt the database.
	 * This means that writing a page requires updating the page's parent to
	 * reference the new location.  We don't want to repeatedly write the
	 * parent on a database flush, so we sort the pages for writing based on
	 * their level in the tree.
	 *
	 * We don't need the tree level on disk and we could move this field to
	 * the WT_PAGE structure -- that said, it's only a byte, we are going to
	 * have unused bytes in this structure anyway, and it's a lot harder to
	 * figure out a tree level whenever we bring a page into memory than to
	 * just set it once when the page is created.
	 *
	 * Leaf pages are level 1, each higher level of the tree increases by 1.
	 * The maximum tree level is 255, larger than any practical fan-out.
	 */
#define	WT_LDESC	0
#define	WT_LLEAF	1
	u_int8_t level;			/* 13: tree level */

	u_int8_t unused[2];		/* 14-15: unused padding */

	union {
		u_int32_t datalen;	/* 16-19: overflow data length */
		u_int32_t entries;	/* 16-19: number of items on page */
	} u;
};
/*
 * WT_PAGE_HDR_SIZE is the expected structure size --  we check at startup to
 * ensure the compiler hasn't inserted padding (which would break the world).
 * The size must also be a multiple of a 4-byte boundary, because the header
 * is followed by WT_ITEM structures, which require 4-byte alignment.
 */
#define	WT_PAGE_HDR_SIZE		20

/*
 * WT_PAGE_BYTE is the first usable data byte on the page.
 */
#define	WT_PAGE_BYTE(page)	(((u_int8_t *)(page)->hdr) + WT_PAGE_HDR_SIZE)

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
#define	WT_KEY_PROCESS(ip)						\
	((ip)->size == 0)
#define	WT_KEY_SET(ip, _key, _size) do {				\
	(ip)->key = (_key);						\
	(ip)->size = _size;						\
} while (0)
#define	WT_KEY_SET_PROCESS(ip, _key) do {				\
	(ip)->key = (_key);						\
	(ip)->size = 0;							\
} while (0)
	void	 *key;			/* Key */
	u_int32_t size;			/* Key length */

	WT_ITEM	 *data;			/* Data */
};
/*
 * WT_ROW_SIZE is the expected structure size -- we check at startup to ensure
 * the compiler hasn't inserted padding.  The WT_ROW structure is in-memory, so
 * padding it won't break the world, but we don't want to waste space, and there
 * are a lot of these structures.
 */
#define	WT_ROW_SIZE	(2 * sizeof(void *) + sizeof(u_int32_t))
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
 * WT_COL_EXPAND --
 * The WT_COL_EXPAND structure describes the in-memory information about a
 * replaced key/data pair on a repeat-compressed, column store database page.
 */
struct __wt_col_expand {
	/*
	 * The stored "key" in the WT_COL_EXPAND structure isn't a record
	 * number: it's the offset in the set of records maintained for the
	 * index, that is, it's an offset from the starting record number
	 * for the original, on-disk page index.
	 *
	 * We know we can store the offset in 16-bits because that's the
	 * maximum number of records in a single compressed group.
	 */
	u_int16_t rcc_offset;		/* recno offset in this index */

	WT_REPL *repl;                  /* modifications/deletions */

	WT_COL_EXPAND *next;		/* forward-linked list */
};

/*
 * WT_INDX_FOREACH --
 * Macro to walk the indexes of an in-memory page: works for both WT_ROW and
 * WT_COL, based on the type of ip.
 */
#define	WT_INDX_FOREACH(page, ip, i)					\
	for ((i) = (page)->indx_count,					\
	    (ip) = (page)->u.indx; (i) > 0; ++(ip), --(i))

/*
 * WT_ROW_KEY_ON_PAGE --
 * Macro returns if a WT_ROW structure's key references on-page data.
 */
#define	WT_ROW_KEY_ON_PAGE(page, rip)					\
	((u_int8_t *)(rip)->key >= (u_int8_t *)(page)->hdr &&		\
	    (u_int8_t *)(rip)->key < (u_int8_t *)(page)->hdr + (page)->size)

/*
 * WT_REPL_FOREACH --
 * Macro to walk the replacement array of an in-memory page.
 */
#define	WT_REPL_FOREACH(page, replp, i)					\
	for ((i) = (page)->indx_count,					\
	    (replp) = (page)->repl; (i) > 0; ++(replp), --(i))

/*
 * WT_EXPCOL_FOREACH --
 * Macro to walk the repeat-count compressed column store expansion  array of
 * an in-memory page.
 */
#define	WT_EXPCOL_FOREACH(page, exp, i)					\
	for ((i) = (page)->indx_count,					\
	    (exp) = (page)->expcol; (i) > 0; ++(exp), --(i))

/*
 * On both row- and column-store internal pages, the on-page data referenced
 * by the WT_ROW/WT_COL data field is a WT_OFF structure, which contains a
 * record count and a page addr/size pair.   Macros to reach into the on-page
 * structure and return the values.
 */
#define	WT_COL_OFF_RECORDS(ip)						\
	WT_RECORDS((WT_OFF *)((ip)->data))
#define	WT_COL_OFF_ADDR(ip)						\
	(((WT_OFF *)((ip)->data))->addr)
#define	WT_COL_OFF_SIZE(ip)						\
	(((WT_OFF *)((ip)->data))->size)

#define	WT_ROW_OFF_RECORDS(ip)						\
	WT_RECORDS((WT_OFF *)WT_ITEM_BYTE((ip)->data))
#define	WT_ROW_OFF_ADDR(ip)						\
	(((WT_OFF *)WT_ITEM_BYTE((ip)->data))->addr)
#define	WT_ROW_OFF_SIZE(ip)						\
	(((WT_OFF *)WT_ITEM_BYTE((ip)->data))->size)

/*
 * WT_ITEM --
 *	Trailing data length (in bytes) plus item type.
 *
 * After the page header, on pages with variable-length data, there is
 * Pages with variable-length items (all page types except for WT_PAGE_COL_INT
 * and WT_PAGE_COL_LEAF_FIXED), are comprised of a list of WT_ITEMs in sorted
 * order.  Or, specifically, 4 bytes followed by a variable length chunk.
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
	u_int32_t __item_chunk;
};
/*
 * WT_ITEM_SIZE is the expected structure size --  we check at startup to make
 * sure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_ITEM_SIZE	4

/*
 * There are 3 basic types: keys, data items and duplicate data items, each of
 * which has an overflow form.  The item is followed by additional data, which
 * varies by type: a key, data or duplicate item is followed by a set of bytes;
 * a WT_OVFL structure follows an overflow form.
 *
 * We could compress the item types (for example, use a bit to mean overflow),
 * but it's simpler this way because we don't need the page type to know what
 * "WT_ITEM_KEY" really means.  We express the item types as bit masks because
 * it makes the macro for assignment faster, but they are integer values, not
 * unique bits.
 *
 * Here's the usage by page type:
 *
 * WT_PAGE_ROW_INT (row-store internal pages):
 * Variable-length key and offpage-reference pairs (a WT_ITEM_KEY/KEY_OVFL item
 * followed by a WT_ITEM_OFF item).
 *
 * WT_PAGE_ROW_LEAF (row-store primary leaf pages):
 * -- Variable-length key followed by a single variable-length/data item (a
 *    WT_ITEM_KEY/KEY_OVFL item followed by a WT_ITEM_DATA/DATA_OVFL item);
 * -- Variable-length key/offpage-reference pairs (a WT_ITEM_KEY/KEY_OVFL item
 *    followed by a WT_ITEM_OFF item);
 * -- Variable-length key followed a sets of duplicates that have not yet been
 *    moved into their own tree (a WT_ITEM_KEY/KEY_OVFL item followed by two
 *    or more WT_ITEM_DUP/DUP_OVFL items).
 *
 * WT_PAGE_DUP_INT (row-store offpage duplicates internal pages):
 * Variable-length key and offpage-reference pairs (a WT_ITEM_KEY/KEY_OVFL item
 * followed by a WT_ITEM_OFF item).
 *
 * WT_PAGE_DUP_LEAF (row-store offpage duplicates leaf pages):
 * Variable-length data items (WT_ITEM_DUP/DUP_OVFL).
 *
 * WT_PAGE_COL_VAR (Column-store leaf page storing variable-length items):
 * Variable-length data items (WT_ITEM_DATA/DATA_OVFL/DEL).
 *
 * WT_PAGE_COL_INT (Column-store internal page):
 * WT_PAGE_COL_FIX (Column-store leaf page storing fixed-length items):
 * WT_PAGE_OVFL (Overflow page):
 *	These pages contain fixed-sized structures (WT_PAGE_COL_INT and
 *	WT_PAGE_COL_FIX), or a string of bytes (WT_PAGE_OVFL), and so do
 *	not contain WT_ITEM structures.
 */
#define	WT_ITEM_KEY		0x01000000 /* Key */
#define	WT_ITEM_KEY_OVFL	0x02000000 /* Overflow key */
#define	WT_ITEM_DATA		0x03000000 /* Data */
#define	WT_ITEM_DATA_OVFL	0x04000000 /* Overflow data */
#define	WT_ITEM_DEL		0x05000000 /* Deleted */
#define	WT_ITEM_DUP		0x06000000 /* Duplicate data */
#define	WT_ITEM_DUP_OVFL	0x07000000 /* Overflow duplicate data */
#define	WT_ITEM_OFF		0x08000000 /* Offpage-tree reference */

#define	WT_ITEM_LEN(addr)						\
	(((WT_ITEM *)(addr))->__item_chunk & 0x00ffffff)
#define	WT_ITEM_LEN_SET(addr, size)					\
	(((WT_ITEM *)(addr))->__item_chunk = WT_ITEM_TYPE(addr) | (size))
#define	WT_ITEM_TYPE(addr)						\
	(((WT_ITEM *)(addr))->__item_chunk & 0x0f000000)
#define	WT_ITEM_TYPE_SET(addr, type)					\
	(((WT_ITEM *)(addr))->__item_chunk = WT_ITEM_LEN(addr) | (type))

/* WT_ITEM_BYTE is the first data byte for an item. */
#define	WT_ITEM_BYTE(addr)						\
	((u_int8_t *)(addr) + sizeof(WT_ITEM))

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
	WT_ALIGN(sizeof(WT_ITEM) + (size), sizeof(u_int32_t))

/* WT_ITEM_NEXT is the first byte of the next item. */
#define	WT_ITEM_NEXT(item)						\
	((WT_ITEM *)((u_int8_t *)(item) + WT_ITEM_SPACE_REQ(WT_ITEM_LEN(item))))

/* WT_ITEM_FOREACH is a loop that walks the items on a page */
#define	WT_ITEM_FOREACH(page, item, i)					\
	for ((item) = (WT_ITEM *)WT_PAGE_BYTE(page),			\
	    (i) = (page)->hdr->u.entries;				\
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
#define	WT_RECORDS(offp)	(*(u_int64_t *)(&(offp)->__record_chunk[0]))
	u_int32_t __record_chunk[2];	/* Subtree record count */
	u_int32_t addr;			/* Subtree root page address */
	u_int32_t size;			/* Subtree root page length */
};
/*
 * WT_OFF_SIZE is the expected structure size -- we check at startup to
 * ensure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_OFF_SIZE	16

/* WT_OFF_FOREACH is a loop that walks offpage references on a page */
#define	WT_OFF_FOREACH(page, offp, i)					\
	for ((offp) = (WT_OFF *)WT_PAGE_BYTE(page),			\
	    (i) = (page)->hdr->u.entries; (i) > 0; ++(offp), --(i))

/*
 * Btree overflow items reference another page, and so the data is another
 * structure.
 */
struct __wt_ovfl {
	u_int32_t addr;			/* Overflow address */
	u_int32_t size;			/* Overflow length */
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
#define	WT_FIX_DELETE_ISSET(b)	(((u_int8_t *)(b))[0] & WT_FIX_DELETE_BYTE)
#define	WT_FIX_DELETE_SET(b)	(((u_int8_t *)(b))[0] = WT_FIX_DELETE_BYTE)

/* WT_FIX_FOREACH is a loop that walks fixed-length references on a page. */
#define	WT_FIX_FOREACH(db, page, p, i)					\
	for ((p) = WT_PAGE_BYTE(page),					\
	    (i) = (page)->hdr->u.entries; (i) > 0; --(i),		\
	    (p) = (u_int8_t *)(p) + (db)->fixed_len)

/*
 * WT_FIX_REPEAT_FOREACH is a loop that walks fixed-length, repeat-counted
 * entries on a page.
 */
#define	WT_FIX_REPEAT_FOREACH(db, page, p, i)				\
	for ((p) = WT_PAGE_BYTE(page),					\
	    (i) = (page)->hdr->u.entries; (i) > 0; --(i),		\
	    (p) = (u_int8_t *)(p) + (db)->fixed_len + sizeof(u_int16_t))

/*
 * WT_FIX_REPEAT_COUNT and WT_FIX_REPEAT_DATA reference the data and count
 * values for repeat-compressed, fixed-length page entries.
 */
#define	WT_FIX_REPEAT_COUNT(p)	(*(u_int16_t *)(p))
#define	WT_FIX_REPEAT_DATA(p)	((u_int8_t *)(p) + sizeof(u_int16_t))

/*
 * WT_FIX_REPEAT_ITERATE is a loop that walks fixed-length, repeat-counted
 * references on a page, visiting each entry the appropriate number of times.
 */
#define	WT_FIX_REPEAT_ITERATE(db, page, p, i, j)			\
	WT_FIX_REPEAT_FOREACH(db, page, p, i)				\
		for ((j) = WT_FIX_REPEAT_COUNT(p); (j) > 0; --(j))

#if defined(__cplusplus)
}
#endif
