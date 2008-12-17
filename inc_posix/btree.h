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

/* Return the number of fragments for a database page. */
#define	WT_FRAGS_PER_PAGE(db)						\
	WT_BYTES_TO_FRAGS(db, (db)->pagesize)

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
 * describes it.  (This is where the on-page index array found in DB
 * 1.85 and Berkeley DB moved.)
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
 * All database pages have a common header.  There is no version number or
 * mode bits, and the page type and/or flags value will likely be modified
 * if any changes are made to the page layout.  (The page type and flags
 * come early in the header to make this simpler.)
 */
struct __wt_page_hdr {
	/* An LSN is 8 bytes: 4 bytes of file number, 4 bytes of file offset. */
	struct __wt_lsn {
		u_int32_t fileno;	/* 00-03: File number */
		u_int32_t offset;	/* 04-07: File offset */
	} lsn;

	/*
	 * The type declares the purpose of the page and how to move through
	 * the page.  We could compress the types (some of them have almost
	 * identical characteristics), but we have plenty of name space and
	 * the additional information makes salvage easier.
	 *
	 * WT_PAGE_OVFL:
	 *	A flat chunk of data.   The u.datalen field is the length
	 *	of the data.  This is used for overflow key and data items.
	 * WT_PAGE_ROOT:
	 * WT_PAGE_INT:
	 * WT_PAGE_LEAF:
	 *	The root, internal and leaf pages of the main btree (the
	 *	root is always page WT_BTREE_ROOT).  The u.entries field
	 *	is the number of entries on the page.
	 * WT_PAGE_DUP_ROOT:
	 * WT_PAGE_DUP_INT:
	 * WT_PAGE_DUP_LEAF:
	 *	The root, internal and leaf pages of an off-page duplicates
	 *	btree.  The u.entries field is the number of entries on the
	 *	page.
	 */
#define	WT_PAGE_INVALID		0	/* Invalid page */
#define	WT_PAGE_OVFL		1	/* Overflow page */
#define	WT_PAGE_ROOT		2	/* Primary btree root page */
#define	WT_PAGE_INT		3	/* Primary btree internal page */
#define	WT_PAGE_LEAF		4	/* Primary btree leaf page */
#define	WT_PAGE_DUP_ROOT	5	/* Off-page dup btree root page */
#define	WT_PAGE_DUP_INT		6	/* Off-page dup btree internal page */
#define	WT_PAGE_DUP_LEAF	7	/* Off-page dup btree leaf page */
	u_int8_t type;			/* 08: page index type */

	u_int8_t unused[3];		/* 09-11: unused padding */

	u_int32_t checksum;		/* 12-15: checksum */

	union {
		u_int32_t datalen;	/* 16-19: data length */
		u_int32_t entries;	/* 16-19: number of items */
	} u;

	/*
	 * Parent, forward and reverse page links.  Pages are linked at their
	 * level, that is, all the main btree leaf pages are linked, each set
	 * of off-page duplicate leaf pages are linked, and each level of
	 * internal pages are linked.
	 */
	u_int32_t prntaddr;		/* 20-23: parent page */
	u_int32_t prevaddr;		/* 24-27: previous page */
	u_int32_t nextaddr;		/* 28-31: next page */
};

/*
 * WT_HDR_SIZE is the expected headr size -- we check this when we startup
 * to make sure the compiler hasn't inserted padding (which would break
 * the world).
 *
 * The header size must be aligned on a 4-byte boundary -- data is written
 * immediately after the header, and the first byte of each chunk must be
 * aligned.
 */
#define	WT_HDR_SIZE		32

/*
 * WT_PAGE_DATA is the first data byte on the page.
 * WT_DATA_SPACE is the total bytes of data space on the page.
 */
#define	WT_PAGE_BYTE(hdr)		((u_int8_t *)(hdr) + WT_HDR_SIZE)
#define	WT_DATA_SPACE(pgsize)		((pgsize) - sizeof(WT_PAGE_HDR))

/*
 * After the header, there is a list of WT_ITEMs in sorted order.
 *
 * Internal pages are lists of single key items (which may be overflow items).
 *
 * Leaf pages contain paired key/data items or a single duplicate data items
 * (which may be overflow items).
 *
 * Off-page duplicate pages are lists of single data items (which may be
 * overflow items).
 */
struct __wt_item {
	u_int32_t len;			/* Trailing data length, in bytes */

	/*
	 * Item type.  There are 3 basic types: keys, data items and duplicate
	 * data items, each of which has an overflow form.  Each of the items
	 * is followed by additional information, which varies by type: a data
	 * or dup item is followed by a set of bytes, a WT_ITEM_OVFL structure
	 * follows an overflow item, and so on.
	 *
	 * On internal (primary or duplicate) pages, there are pairs of items:
	 * a WT_ITEM_INT followed by a single WT_ITEM_KEY or WT_ITEM_KEY_OVFL.
	 *
	 * On primary leaf pages, there is either a WT_ITEM_KEY followed by a
	 * single WT_ITEM_DATA, WT_ITEM_DATA_OVFL or WT_ITEM_DUP_OFFPAGE item,
	 * or a WT_ITEM_KEY followed by some number of either WT_ITEM_DUP or
	 * WT_ITEM_DUP_OVFL items.
	 *
	 * On duplicate leaf pages, there are WT_ITEM_DUP or WT_ITEM_DUP_OVFL
	 * items.
	 *
	 * Again, we could compress the values (and/or use a separate flag to
	 * indicate overflow), but it's simpler this way if only because we
	 * don't have to use the page type to figure out what "WT_ITEM_KEY"
	 * really means.
	 */
#define	WT_ITEM_KEY		1	/* Leaf/internal page key */
#define	WT_ITEM_KEY_OVFL	2	/* Leaf/internal page overflow key */
#define	WT_ITEM_DATA		3	/* Leaf page data item */
#define	WT_ITEM_DATA_OVFL	4	/* Leaf page overflow data item */
#define	WT_ITEM_DUP		5	/* Duplicate data item */
#define	WT_ITEM_DUP_OVFL	6	/* Duplicate overflow data item */
#define	WT_ITEM_INT		7	/* Internal page item */
#define	WT_ITEM_DUP_OFFPAGE	8	/* Offpage duplicates tree */
	u_int8_t  type;

	u_int8_t  unused[3];		/* Spacer to force 4-byte alignment */

	/*
	 * A variable length chunk of data.
	 */
};

/* WT_ITEM_BYTE is the first data byte for the item. */
#define	WT_ITEM_BYTE(item)		((u_int8_t *)(item) + sizeof(WT_ITEM))

/*
 * The number of bytes required to store a WT_ITEM followed by len additional
 * bytes.  Align the entry and the data itself to a 4-byte boundary so it's
 * possible to directly access the item on the page.
 */
#define	WT_ITEM_SPACE_REQ(len)						\
	WT_ALIGN(sizeof(WT_ITEM) + (len), sizeof(u_int32_t))

/*
 * Btree internal items reference another page, and so the data is another
 * structure.
 */
struct __wt_item_int {
	u_int32_t  len;			/* Data length, in bytes */

	u_int32_t  addr;		/* Subtree address */
	wt_recno_t records;		/* Subtree record count */
};

/*
 * Btree off-page duplicates reference another page, and so the data is
 * another structure.
 */
struct __wt_item_offp {
	u_int32_t  addr;		/* Off-page address */
	wt_recno_t records;		/* Off-page record count */
};

/*
 * Btree overflow items reference another page, and so the data is another
 * structure.
 */
struct __wt_item_ovfl {
	u_int32_t len;			/* Overflow length */
	u_int32_t addr;			/* Overflow address */
}; 

#if defined(__cplusplus)
}
#endif
