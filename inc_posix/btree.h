/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Structure that describes a single Btree.
 */
struct __wt_btree {
	DB *db;					/* Enclosing DB handle */

	WT_FH *fh;				/* Backing file handle */

	u_int32_t blocks;			/* 512B blocks in the file */
};

/*
 * File offsets are multiples of 512B and stored internally in 32-bit unsigned
 * variables.  We separate file offsets from logical page numbers so we can
 * allocate other than page-size chunks from the file, for example, allocating
 * space for overflow key or data items.
 */
#define	WT_BLOCK_SIZE			(512)
#define	WT_BLOCKS_TO_BYTES(blocks)	((blocks) * WT_BLOCK_SIZE)
#define	WT_BYTES_TO_BLOCKS(bytes)					\
	(((bytes) + (WT_BLOCK_SIZE - 1)) / WT_BLOCK_SIZE)
#define	WT_PGNO_TO_BLOCKS(db, pgno)					\
	((db)->pagesize / WT_BLOCK_SIZE) * (pgno)

#define	WT_DEFAULT_PAGE_SIZE		32768	/* 32KB */
#define	WT_DEFAULT_EXTENT_SIZE		10485760/* 10MB */

/*
 * Block 0 of the file is the btree root, and the invalid page address.
 */
#define	WT_BTREE_ROOT	0
#define	WT_PGNO_INVALID	0

/*
 * Each page of the Btree has an associated, in-memory structure that
 * describes it.  (This is where the page index found in DB 1.85 and 
 * Berkeley DB moved.)
 */
struct __wt_page_inmem {
	u_int32_t pgno;				/* File block address */

	u_int32_t space_avail;			/* Available page memory */

	u_int8_t **indx;			/* Array of page references */
	u_int32_t indx_count;			/* Entries in indx */
	u_int32_t indx_size;			/* Size of indx array */

	WT_PAGE_HDR *page;			/* The actual page. */
};

/*
 * All database pages have a common header.  There is no version number, and
 * the page type and/or flags value must be revised to accommodate a change
 * in the page layout.  (The page type and flags come early in the header to
 * make this simpler.)
 */
struct __wt_page_hdr {
	/* An LSN is 8 bytes: 4 bytes of file number, 4 bytes of file offset. */
	struct __wt_lsn {
		u_int32_t fileno;	/* 00-03: File number */
		u_int32_t offset;	/* 04-07: File offset */
	} lsn;

#define	WT_PAGE_INVALID		0	/* Invalid page */
#define	WT_PAGE_BTREE_ROOT	1	/* Btree root page */
#define	WT_PAGE_BTREE_INTERNAL	2	/* Btree internal page */
#define	WT_PAGE_BTREE_LEAF	3	/* Btree leaf page */
	u_int16_t type;			/* 08-09: page type */

	u_int16_t flags;		/* 10-11: page flags */

	u_int32_t checksum;		/* 12-15: checksum */
	u_int32_t entries;		/* 16-19: number of items */

	u_int32_t prevpg;		/* 20-23: previous page */
	u_int32_t nextpg;		/* 24-27: next page */
};
#define	WT_HDR_SIZE		28

#define	WT_DATA_SPACE(pgsize)		((pgsize) - sizeof(WT_PAGE_HDR))

/*
 * After the header, there is a list of items in sorted order.  On btree
 * leaf pages, they are paired, key/data items.   On btree internal pages,
 * they are key items.   After the item, is a variable length chunk of
 * data.
 */
struct __wt_item {
	u_int32_t len;			/* Data length, in bytes */

#define	WT_ITEM_INTERNAL	1
#define	WT_ITEM_OVERFLOW	2
#define	WT_ITEM_STANDARD	3
	u_int8_t  type;			/* Data type */
	u_int8_t  unused[3];		/* Spacer to force alignment */
};

/*
 * The number of bytes required to store an item of len bytes.  Align the
 * entry and the data itself to a 4-byte boundary so we can directly access
 * the item on the page.
 */
#define	WT_ITEM_SPACE_REQ(len)						\
	WT_ALIGN(sizeof(WT_ITEM) + (len), sizeof(u_int32_t))

/*
 * Btree internal items reference another page, and so the data is another
 * structure.
 */
struct __wt_item_int {
	u_int32_t  len;			/* Data length, in bytes */
	u_int32_t  child;		/* Child page number */
	wt_recno_t records;		/* Subtree record count */
};

/*
 * Btree overflow items reference another page, and so the data is another
 * structure.
 */
struct __wt_item_ovfl {
	u_int32_t pgno;			/* Overflow page number */
}; 
	
#if defined(__cplusplus)
}
#endif
