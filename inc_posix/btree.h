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

	u_int32_t frags;			/* Fragments in the file */
};

/*
 * We only have 32-bits to hold file locations, so all file locations are
 * stored in counts of "fragments" (making a "fragment" the smallest unit
 * of allocation from the underlying file).  To simplify the bookkeeping,
 * the database page size must be a multiple of the fragment size, and the
 * database extent size must be a multiple of the page size.  The minimum
 * fragment size is 512B, so the minimum maximum database size is 2TB, and
 * the maximum maximum (assuming we could pass file offsets that large,
 * which we can't), is 4EB.   In short, nobody will ever complain this
 * code can't build a database sufficiently large for whatever the hell it
 * is they want to do.
 */
#define	WT_FRAG_MINIMUM_SIZE		(512)		/* 512B */
#define	WT_FRAG_DEFAULT_SIZE		(2048)		/* 2KB */
#define	WT_PAGE_DEFAULT_SIZE		(32768)		/* 32KB */
#define	WT_EXTENT_DEFAULT_SIZE		(10485760)	/* 10MB */

/* Convert a file address into a byte offset. */
#define	WT_FRAGS_TO_BYTES(db, frags)					\
	((size_t)(frags) * (db)->fragsize)
/* Return the number of fragments needed to hold N bytes. */
#define	WT_BYTES_TO_FRAGS(db, bytes)					\
	((bytes) / (db)->fragsize)

/* Return the fragments needed for an overflow item. */
#define	WT_OVERFLOW_BYTES_TO_FRAGS(db, len, frags) {			\
	off_t __bytes;							\
	 __bytes = (len) + sizeof(WT_PAGE_HDR);				\
	 __bytes = WT_ALIGN(__bytes, (db)->fragsize);			\
	 (frags) = WT_BYTES_TO_FRAGS(db, __bytes);			\
}

/*
 * Block 0 of the file is the btree root, and the invalid address.
 */
#define	WT_BTREE_ROOT	0
#define	WT_ADDR_INVALID	0

/*
 * Each page of the Btree has an associated, in-memory structure that
 * describes it.  (This is where the page index found in DB 1.85 and 
 * Berkeley DB moved.)
 */
struct __wt_page_inmem {
	u_int32_t addr;				/* File block address */

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
#define	WT_PAGE_BTREE_OVERFLOW	4	/* Btree overflow page */
	u_int16_t type;			/* 08-09: page type */

	u_int16_t flags;		/* 10-11: page flags */

	u_int32_t checksum;		/* 12-15: checksum */
	u_int32_t entries;		/* 16-19: number of items */

	u_int32_t prevaddr;		/* 20-23: previous page */
	u_int32_t nextaddr;		/* 24-27: next page */
};
#define	WT_HDR_SIZE		28
#define	WT_PAGE_DATA(hdr)	((u_int8_t *)(hdr) + WT_HDR_SIZE)

#define	WT_DATA_SPACE(pgsize)		((pgsize) - sizeof(WT_PAGE_HDR))

/*
 * After the header, there is a list of items in sorted order.  On btree
 * leaf pages, they are paired, key/data items.   On btree internal pages,
 * they are key items.
 */
struct __wt_item {
	u_int32_t len;			/* Data length, in bytes */

#define	WT_ITEM_INTERNAL	1
#define	WT_ITEM_OVERFLOW	2
#define	WT_ITEM_STANDARD	3
	u_int8_t  type;			/* Data type */
	u_int8_t  unused[3];		/* Spacer to force alignment */

	/*
	 * A variable length chunk of data.
	 */
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

	/*
	 * A variable length chunk of data.
	 */
};

/*
 * Btree overflow items reference another page, and so the data is another
 * structure.
 */
struct __wt_item_ovfl {
	u_int32_t addr;			/* Overflow page number */
	u_int32_t len;			/* Overflow length */
}; 

#if defined(__cplusplus)
}
#endif
