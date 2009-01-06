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
#define	WT_CACHE_DEFAULT_SIZE		(20 * MEGABYTE)	/* 20MB */

/* Convert a file address into a byte offset. */
#define	WT_FRAGS_TO_BYTES(db, frags)					\
	((off_t)(frags) * (db)->fragsize)

/* Return the number of fragments needed to hold N bytes. */
#define	WT_BYTES_TO_FRAGS(db, bytes)					\
	((u_int32_t)(((bytes) + ((db)->fragsize - 1)) / (db)->fragsize))

/* Return the number of fragments for a database page. */
#define	WT_FRAGS_PER_PAGE(db)						\
	((db)->pagesize / (db)->fragsize)

/* Return the fragments needed for an overflow item. */
#define	WT_OVERFLOW_BYTES_TO_FRAGS(db, len, frags) {			\
	off_t __bytes;							\
	 __bytes = (len) + sizeof(WT_PAGE_HDR);				\
	 (frags) = WT_BYTES_TO_FRAGS(db, __bytes);			\
}

/*
 * The first possible address is 0 (well, duh -- it's just strange to
 * see an address of 0 hard-coded in).   It is also always the first
 * logical page in the database because it's created first and never
 * replaced.
 *
 * The invalid address is the largest possible offset, which isn't a
 * possible fragment address.
 */
#define	WT_ADDR_FIRST_PAGE	0
#define	WT_ADDR_INVALID		UINT32_MAX

typedef	struct __wt_indx {
	/*
	 * The first part of the WT_INDX structure looks exactly like a DBT
	 * so we can feed it directly to a Btree comparison function.
	 */
	void	*data;			/* DBT: data */
	size_t	 size;			/* DBT: data length */

	u_int32_t addr;			/* WT_ITEM_{KEY_OVFL,OFFPAGE}->addr */

	/*
	 * Associated on-page data item.
	 *
	 * In the case of primary internal pages, the associated data item
	 * is a WT_ITEM_OFFPAGE.
	 *
	 * In the case of primary leaf pages, the associated data item is a
	 * WT_ITEM_DATA or WT_ITEM_DATA_OVFL, or a duplicate set (a group of
	 * WT_ITEM_DUP and WT_ITEM_DUP_OVFL items).
	 *
	 * In the case of off-page duplicate leaf pages, the associated data
	 * item is the same as the key.
	 */
	WT_ITEM *ditem;			/* Associated on-page data item */

	u_int32_t flags;
} WT_INDX;

struct __wt_page {
	u_int32_t    addr;			/* File block address */
	u_int32_t    frags;			/* Number of fragments */
	WT_PAGE_HDR *hdr;			/* The on-disk page */

	u_int8_t ref;				/* Reference count */
	TAILQ_ENTRY(__wt_page) q;		/* LRU queue */
	TAILQ_ENTRY(__wt_page) hq;		/* Hash queue */

	u_int8_t *first_free;			/* First free byte address */
	u_int32_t space_avail;			/* Available page memory */

	/*
	 * Each page has an associated, in-memory structure describing it.
	 * (This is where the on-page index array found in DB 1.85 and Berkeley
	 * DB moved.)   It's always sorted, but it's not always aa "key", for
	 * example, offpage duplicate leaf pages contain sorted data items,
	 * where the data is the interesting stuff.  For simplicity, and as
	 * it's always a sorted list, we call it a key, 
	 */
	WT_INDX	 *indx;				/* Key items  on the page */
	u_int32_t indx_count;			/* Entries in key index */
	u_int32_t indx_size;			/* Size of key index */

	u_int32_t flags;
};

/* Macro to walk the indexes of an in-memory page. */
#define	WT_INDX_FOREACH(page, ip, i)					\
	for ((i) = 0,							\
	    (ip) = (page)->indx; (i) < (page)->indx_count; ++(ip), ++(i))

/*
 * The database itself needs a chunk of memory that describes it.   Here's
 * the structure.
 */
struct __wt_page_desc {
#define	WT_BTREE_MAGIC		0x120897
	u_int32_t magic;		/* 00-03: Magic number */
#define	WT_BTREE_MAJOR_VERSION	1
	u_int32_t majorv;		/* 04-07: Major version */
#define	WT_BTREE_MINOR_VERSION	1
	u_int32_t minorv;		/* 08-11: Minor version */
	u_int32_t pagesize;		/* 12-15: Page size */
	u_int64_t base_recno;		/* 16-23: Base record number */
	u_int32_t root_addr;		/* 24-27: Root fragment */
	u_int32_t free_addr;		/* 28-31: Freelist fragment */
	u_int32_t unused[8];		/* 32-63: Spare */
};
/*
 * WT_DESC_SIZE is the expected WT_DESC size --  we check this on startup
 * to make sure the compiler hasn't inserted padding (which would break
 * the world).
 *
 * The size must be a multiple of a 4-byte boundary.
 */
#define	WT_DESC_SIZE		64

/*
 * All database pages have a common header.  There is no version number or
 * mode bits, and the page type and/or flags value will likely be modified
 * if any changes are made to the page layout.  (The page type and flags
 * come early in the header to make this simpler.)
 */
struct __wt_page_hdr {
	/* An LSN is 8 bytes: 4 bytes of file number, 4 bytes of file offset. */
	struct __wt_lsn {
		u_int32_t f;		/* 00-03: File number */
		u_int32_t o;		/* 04-07: File offset */
	} lsn;

	/*
	 * !!!
	 * The following comment describes the page layout for WiredTiger.
	 *
	 * The page type declares the purpose of the page and how to move
	 * through the page.
	 *
	 * WT_PAGE_INT:
	 * WT_PAGE_DUP_INT:
	 *	The page contains sorted key/offpage-reference pairs.  Keys
	 *	are on-page (WT_ITEM_KEY) or overflow (WT_ITEM_KEY_OVFL) items.
	 *	Offpage references are WT_ITEM_OFFPAGE items.
	 *
	 *	The u.entries field is the number of entries on the page.
	 *
	 * WT_PAGE_LEAF:
	 *	The page contains sorted key/data sets.  Keys are on-page
	 *	(WT_ITEM_KEY) or overflow (WT_ITEM_KEY_OVFL) items.  The data
	 *	sets are either: a single on-page (WT_ITEM_DATA) or overflow
	 *	(WT_ITEM_DATA_OVFL) item; a group of duplicate data items
	 *	where each duplicate is an on-page (WT_ITEM_DUP) or overflow
	 *	(WT_ITEM_DUP_OVFL) item; an offpage reference (WT_ITEM_OFFPAGE).
	 *
	 *	The u.entries field is the number of entries on the page.
	 *
	 * WT_PAGE_DUP_LEAF:
	 *	The page contains sorted data items.  The data items are
	 *	on-page (WT_ITEM_DUP) or overflow (WT_ITEM_DUP_OVFL).
	 *
	 *	The u.entries field is the number of entries on the page.
	 *
	 * WT_PAGE_OVFL:
	 *	Pages of this type hold overflow key/data items, it's just a
	 *	flat chunk of data.
	 *
	 *	The u.datalen field is the length of the data.
	 */
#define	WT_PAGE_INVALID		0	/* Invalid page */
#define	WT_PAGE_INT		1	/* Primary btree internal page */
#define	WT_PAGE_LEAF		2	/* Primary btree leaf page */
#define	WT_PAGE_DUP_INT		3	/* Off-page dup btree internal page */
#define	WT_PAGE_DUP_LEAF	4	/* Off-page dup btree leaf page */
#define	WT_PAGE_OVFL		5	/* Overflow page */
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
 * WT_HDR_SIZE is the expected WT_HDR size --  we check this on startup
 * to make sure the compiler hasn't inserted padding (which would break
 * the world).  The size must be a multiple of a 4-byte boundary.
 */
#define	WT_HDR_SIZE		32

/*
 * WT_PAGE_BYTE is the first usable data byte on the page.  Note the correction
 * for page addr of 0, the first fragment.
 */
#define	WT_PAGE_BYTE(page)						\
	(((u_int8_t *)(page)->hdr) +					\
	WT_HDR_SIZE + ((page)->addr == 0 ? WT_DESC_SIZE : 0))

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
#define	WT_ITEM_OFFPAGE		7	/* Offpage reference */
	u_int8_t  type;

	u_int8_t  unused[3];		/* Spacer to force 4-byte alignment */

	/*
	 * A variable length chunk of data.
	 */
};

/* WT_ITEM_BYTE is the first data byte for the item. */
#define	WT_ITEM_BYTE(item)						\
	((u_int8_t *)(item) + sizeof(WT_ITEM))

/*
 * The number of bytes required to store a WT_ITEM followed by len additional
 * bytes.  Align the entry and the data itself to a 4-byte boundary so it's
 * possible to directly access the item on the page.
 */
#define	WT_ITEM_SPACE_REQ(len)						\
	WT_ALIGN(sizeof(WT_ITEM) + (len), sizeof(u_int32_t))

/* WT_ITEM_NEXT is the first byte of the next item. */
#define	WT_ITEM_NEXT(item)						\
	((WT_ITEM *)((u_int8_t *)(item) + WT_ITEM_SPACE_REQ((item)->len)))

/* WT_ITEM_FOREACH is a for loop that walks the items on a page */
#define	WT_ITEM_FOREACH(page, item, i)					\
	for ((item) = (WT_ITEM *)WT_PAGE_BYTE(page),			\
	    (i) = (page)->hdr->u.entries;				\
	    (i) > 0; (item) = WT_ITEM_NEXT(item), --(i))		\

/*
 * Btree internal items and off-page duplicates reference another page.
 */
struct __wt_item_offp {
	u_int32_t  addr;		/* Subtree address */
	wt_recno_t records;		/* Subtree record count */
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
