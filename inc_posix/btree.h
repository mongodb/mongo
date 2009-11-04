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
 * We use 32-bits to store file locations on database pages, so all such file
 * locations are counts of "database allocation units" (making an "allocation
 * unit" the smallest database chunk allocated from an underlying file).  In
 * the code, these are all "addresses" or "addrs".  To simplify bookkeeping,
 * database internal and leaf page sizes, and extent size, must be a multiple
 * of the allocation unit size.
 *
 * The minimum database allocation unit is 512B so the minimum maximum database
 * size is 2TB, and the maximum maximum (assuming we could pass file offsets
 * that large, which we can't), is 4EB.   In summary, small allocation units
 * limit the database size, and as the allocation unit grows, the maximum size
 * of the database grows as well.
 *
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

/* Return bytes needed for an overflow item, rounded to an allocation unit. */
#define	WT_OVFL_BYTES(db, len)						\
	((u_int32_t)WT_ALIGN((len) + sizeof(WT_PAGE_HDR), (db)->allocsize))

/*
 * The first possible address is 0.  It is also always the first leaf page in
 * in the database because it's created first and never replaced.
 *
 * The invalid address is the largest possible offset, which isn't a possible
 * database address.
 */
#define	WT_ADDR_FIRST_PAGE	0
#define	WT_ADDR_INVALID		UINT32_MAX

typedef	struct __wt_indx {
	/*
	 * The first part of the WT_INDX structure looks exactly like a DBT
	 * so we can feed it to a Btree comparison function without copying.
	 */
	void	 *data;			/* DBT: data */
	u_int32_t size;			/* DBT: data length */

	/*
	 * Associated on-page data item.
	 *
	 * In the case of primary internal pages, the associated data item
	 * is a WT_ITEM_OFFP_INTL/LEAF.
	 *
	 * In the case of primary leaf pages, the associated data item is a
	 * WT_ITEM_DATA or WT_ITEM_DATA_OVFL, or a duplicate set (a group of
	 * WT_ITEM_DUP and WT_ITEM_DUP_OVFL items).
	 *
	 * In the case of off-page duplicate leaf pages, the associated data
	 * item is the same as the key.
	 *
	 * The following are macros to get to the 4 fields of the structures
	 * a ditem might reference.
	 */
#define	WT_INDX_OFFP_RECORDS(ip)					\
    WT_64_CAST(((WT_ITEM_OFFP *)WT_ITEM_BYTE((ip)->ditem))->records)
#define	WT_INDX_OFFP_ADDR(ip)						\
    (((WT_ITEM_OFFP *)WT_ITEM_BYTE((ip)->ditem))->addr)
#define	WT_INDX_OVFL_LEN(ip)						\
    (((WT_ITEM_OVFL *)WT_ITEM_BYTE((ip)->ditem))->len)
#define	WT_INDX_OVFL_ADDR(ip)						\
    (((WT_ITEM_OVFL *)WT_ITEM_BYTE((ip)->ditem))->addr)

	WT_ITEM *ditem;			/* Associated on-page data item */

	u_int32_t flags;
} WT_INDX;

struct __wt_page {
	/*********************************************************
	 * The following fields are owned by the cache layer.
	 *********************************************************/
	WT_FH	 *fh;			/* Page's file handle */
	off_t     offset;		/* Page's file offset */
	u_int32_t addr;			/* Page's allocation address */

	/*
	 * The page size is limited to 4GB by this type -- we could use
	 * off_t's here if we need something bigger, but the page-sizing
	 * code limits page sizes to 128MB.
	 */
	u_int32_t bytes;		/* Page size */

	u_int32_t generation;		/* LRU generation number */

	WT_PAGE *next;			/* Hash queue */

	WT_PAGE_HDR *hdr;		/* On-disk page */

	/*********************************************************
	 * The following fields are owned by the btree layer.
	 *********************************************************/
	u_int8_t *first_free;		/* First free byte address */
	u_int32_t space_avail;		/* Available page memory */

	/*
	 * Each page has an associated, in-memory structure describing it.
	 * (This is where the on-page index array found in DB 1.85 and Berkeley
	 * DB moved.)   It's always sorted, but it's not always aa "key", for
	 * example, offpage duplicate leaf pages contain sorted data items,
	 * where the data is the interesting stuff.  For simplicity, and as
	 * it's always a sorted list, we call it a key,
	 */
	WT_INDX	 *indx;			/* Key items  on the page */
	u_int32_t indx_count;		/* Entries in key index */
	u_int32_t indx_size;		/* Size of key index */

	u_int64_t records;		/* Records in this page and below */

	u_int32_t flags;
};

/* Macro to walk the indexes of an in-memory page. */
#define	WT_INDX_FOREACH(page, ip, i)					\
	for ((i) = (page)->indx_count,					\
	    (ip) = (page)->indx; (i) > 0; ++(ip), --(i))

/*
 * The database itself needs a chunk of memory that describes it.   Here's
 * the structure.
 *
 * !!!
 * Field order is important: there's a 8-byte type in the middle, and the
 * Solaris compiler inserts space into the structure if we don't put that
 * field on an 8-byte boundary.
 */
struct __wt_page_desc {
#define	WT_BTREE_MAGIC		120897
	u_int32_t magic;		/* 00-03: Magic number */
#define	WT_BTREE_MAJOR_VERSION	1
	u_int16_t majorv;		/* 04-05: Major version */
#define	WT_BTREE_MINOR_VERSION	1
	u_int16_t minorv;		/* 06-07: Minor version */
	u_int32_t leafsize;		/* 08-11: Leaf page size */
	u_int32_t intlsize;		/* 12-15: Internal page size */
	u_int64_t base_recno;		/* 16-23: Base record number */
	u_int32_t root_addr;		/* 24-27: Root address */
	u_int32_t free_addr;		/* 28-31: Freelist address */
	u_int32_t unused[8];		/* 32-63: Spare */
};
/*
 * WT_PAGE_DESC_SIZE is the expected structure size --  we check at startup to
 * ensure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_PAGE_DESC_SIZE		64

/*
 * All database pages have a common header.  There is no version number or
 * mode bits, and the page type and/or flags value will likely be modified
 * if any changes are made to the page layout.  (The page type and flags
 * come early in the header to make this simpler.)
 */
struct __wt_page_hdr {
	u_int32_t lsn[2];		/* 00-07: LSN */

	u_int32_t checksum;		/* 08-11: checksum */

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
	 *	Offpage references are WT_ITEM_OFFP_INTL/LEAF items.
	 *
	 *	The u.entries field is the number of entries on the page.
	 *
	 * WT_PAGE_LEAF:
	 *	The page contains sorted key/data sets.  Keys are on-page
	 *	(WT_ITEM_KEY) or overflow (WT_ITEM_KEY_OVFL) items.  The data
	 *	sets are either: a single on-page (WT_ITEM_DATA) or overflow
	 *	(WT_ITEM_DATA_OVFL) item; a group of duplicate data items
	 *	where each duplicate is an on-page (WT_ITEM_DUP) or overflow
	 *	(WT_ITEM_DUP_OVFL) item; an offpage reference (WT_ITEM_OFFP).
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
	u_int8_t type;			/* 12: page index type */

	u_int8_t unused[3];		/* 13-15: unused padding */

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
 * WT_PAGE_HDR_SIZE is the expected structure size --  we check at startup to
 * ensure the compiler hasn't inserted padding (which would break the world).
 * The size must be a multiple of a 4-byte boundary.
 *
 * It would be possible to reduce this by two bytes, by moving the odd-sized
 * fields to the end of the structure (and using something other than "sizeof"
 * for the check, as compilers usually pad the sizeof() operator to the next
 * 4-byte boundary), but I don't think two bytes are worth the effort.
 */
#define	WT_PAGE_HDR_SIZE		32

/*
 * WT_PAGE_BYTE is the first usable data byte on the page.  Note the correction
 * for page addr of 0, the first address.   It would be simpler to put this at
 * the end of the page, but that would make it more difficult to figure out the
 * page size in a just opened database.
 */
#define	WT_PAGE_BYTE(page)						\
	(((u_int8_t *)(page)->hdr) +					\
	WT_PAGE_HDR_SIZE + ((page)->addr == 0 ? WT_PAGE_DESC_SIZE : 0))

/*
 * After the page header, there is a list of WT_ITEMs in sorted order.
 */
struct __wt_item {
	/*
	 * Trailing data length (in bytes) plus item type.
	 *
	 * We encode the length and type in a single 4-byte value in order to
	 * minimize our on-page footprint as well as maintain alignment of the
	 * bytes that follow the item.   (The trade-off is this limits on-page
	 * database items to 16MB.)   The bottom 24-bits are the length of the
	 * trailing data, the next 4-bits are the type, and the top 4-bits are
	 * unused.   We could use the unused 4-bits to provide more length, but
	 * 16MB seems sufficient for on-page items.
	 *
	 * The __item_chunk field should never be directly accessed, there are
	 * macros to extract the type and length.
	 *
	 * WT_ITEMs are aligned to a 4-byte boundary, so it's OK to directly
	 * access the __item_chunk field on the page.
	 */
#define	WT_ITEM_MAX_LEN	(16 * 1024 * 1024 - 1)
	u_int32_t __item_chunk;
};

/*
 * Item type: there are 3 basic types: keys, data items and duplicate data
 * items, each of which has an overflow form.  Each of the items is followed
 * by additional information, which varies by type: a key, data or duplicate
 * item is followed by a set of bytes; a WT_ITEM_OVFL structure follows an
 * overflow item.
 *
 * On internal (primary or duplicate) pages, there are pairs of items: a
 * WT_ITEM_KEY/KEY_OVFL item followed by a WT_ITEM_OFFP_LEAF/INTL item.
 * We use different values for references to internal pages (OFFP_INTL)
 * vs. references to leaf pages (OFFP_LEAF) because we need to know the
 * size of the page we're about to read.
 *
 * On primary leaf pages, there's a WT_ITEM_KEY/KEY_OVFL item followed by one
 * WT_ITEM_DATA, WT_ITEM_DATA_OVFL or WT_ITEM_OFFP item, or a WT_ITEM_KEY/
 * KEY_OVFL item followed by some number of WT_ITEM_DUP or WT_ITEM_DUP_OVFL
 * items.
 *
 * On duplicate leaf pages, there are WT_ITEM_DUP or WT_ITEM_DUP_OVFL items.
 *
 * We could compress the item types (for example, use a bit to mean overflow),
 * but it's simpler this way because we don't need the page type to know what
 * "WT_ITEM_KEY" really means.  We express the item types as bit masks because
 * it makes the macro for assignment faster, but they are integer values, not
 * unique bits.
 */
#define	WT_ITEM_KEY		0x01000000 /* Leaf/internal page key */
#define	WT_ITEM_KEY_OVFL	0x02000000 /* Leaf/internal page overflow key */
#define	WT_ITEM_DATA		0x03000000 /* Leaf page data item */
#define	WT_ITEM_DATA_OVFL	0x04000000 /* Leaf page overflow data item */
#define	WT_ITEM_DUP		0x05000000 /* Duplicate data item */
#define	WT_ITEM_DUP_OVFL	0x06000000 /* Duplicate overflow data item */
#define	WT_ITEM_OFFP_INTL	0x07000000 /* Offpage internal page reference */
#define	WT_ITEM_OFFP_LEAF	0x08000000 /* Offpage leaf page reference */

#define	WT_ITEM_LEN(item)						\
	((item)->__item_chunk & 0x00ffffff)
#define	WT_ITEM_LEN_SET(item, len)					\
	((item)->__item_chunk = WT_ITEM_TYPE(item) | (len))
#define	WT_ITEM_TYPE(item)						\
	((item)->__item_chunk & 0x0f000000)
#define	WT_ITEM_TYPE_SET(item, type)					\
	((item)->__item_chunk = WT_ITEM_LEN(item) | (type))

/*
 * WT_ITEM_SIZE is the expected structure size --  we check at startup to make
 * sure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_ITEM_SIZE	4

/* WT_ITEM_BYTE is the first data byte for the item. */
#define	WT_ITEM_BYTE(item)						\
	((u_int8_t *)(item) + sizeof(WT_ITEM))

/*
 * The number of bytes required to store a WT_ITEM followed by len additional
 * bytes.  Align the entry and the data itself to a 4-byte boundary so it's
 * possible to directly access WT_ITEMs on the page.
 */
#define	WT_ITEM_SPACE_REQ(len)						\
	WT_ALIGN(sizeof(WT_ITEM) + (len), sizeof(u_int32_t))

/* WT_ITEM_NEXT is the first byte of the next item. */
#define	WT_ITEM_NEXT(item)						\
	((WT_ITEM *)((u_int8_t *)(item) + WT_ITEM_SPACE_REQ(WT_ITEM_LEN(item))))

/* WT_ITEM_FOREACH is a for loop that walks the items on a page */
#define	WT_ITEM_FOREACH(page, item, i)					\
	for ((item) = (WT_ITEM *)WT_PAGE_BYTE(page),			\
	    (i) = (page)->hdr->u.entries;				\
	    (i) > 0; (item) = WT_ITEM_NEXT(item), --(i))
/*
 * Btree internal items and off-page duplicates reference another page.
 *
 * !!!
 * There's some ugliness here.  We need a page's size before reading it, and as
 * internal and leaf nodes are potentially of different sizes, we have to know
 * the page's tree level to know its size.  We could store the level or type of
 * the root of the off-page tree in the WT_ITEM_OFFP structure, but that costs
 * another 4 bytes, with alignment.  Instead of paying that cost, we encode the
 * information in the item type, using WT_ITEM_OFFP_INTL for an internal page
 * reference, and WT_ITEM_OFFP_LEAF for a leaf page reference.  It complicates
 * the code of course, but paying 4 bytes per internal page item pair is real.
 */
struct __wt_item_offp {
/*
 * Solaris and the gcc compiler on Linux pad the WT_ITEM_OFFP structure because
 * of the 64-bit records field.   This is an on-disk structure, which means we
 * have to have a fixed size, without padding, so we declare it as two 32-bit
 * fields and cast it.  We haven't yet found a compiler that aligns the 32-bit
 * fields such that a cast won't work; if we find one, we'll have to go to bit
 * masks.
 */
#define	WT_64_CAST(array)	(*(u_int64_t *)(&array[0]))
	u_int32_t records[2];		/* Subtree record count */
	u_int32_t addr;			/* Subtree address */
};
/*
 * WT_ITEM_OFFP_SIZE is the expected structure size --  we check at startup to
 * ensure the compiler hasn't inserted padding (which would break the world).
 */
#define	WT_ITEM_OFFP_SIZE	12

/*
 * Btree overflow items reference another page, and so the data is another
 * structure.
 */
struct __wt_item_ovfl {
	u_int32_t len;			/* Overflow length */
	u_int32_t addr;			/* Overflow address */
};
/*
 * WT_ITEM_OVFL_SIZE is the expected structure size --  we check at startup to
 * ensure the compiler hasn't inserted padding (which would break the world).
 * The size must be a multiple of a 4-byte boundary.
 */
#define	WT_ITEM_OVFL_SIZE	8

#if defined(__cplusplus)
}
#endif
