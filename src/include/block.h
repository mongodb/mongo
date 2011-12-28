/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * WiredTiger's block manager interface.
 */
#define	WT_BM_MAX_ADDR_COOKIE		255	/* Maximum address cookie */

/*
 * The file's description is written into the first 512B of the file, which
 * means we can use an offset of 0 as an invalid offset.
 */
#define	WT_BLOCK_DESC_SECTOR		512
#define	WT_BLOCK_INVALID_OFFSET		0

/*
 * WT_BLOCK --
 *	Encapsulation of the standard WiredTiger block manager.
 */
struct __wt_block {
	const char *name;		/* Name */

	WT_FH	*fh;			/* Backing file handle */

	uint64_t lsn;			/* LSN file/offset pair */

	uint32_t allocsize;		/* Allocation size */
	int	 checksum;		/* If checksums configured */

	WT_COMPRESSOR *compressor;	/* Page compressor */

					/* Freelist support */
	WT_SPINLOCK freelist_lock;	/* Lock to protect the freelist. */
	uint64_t freelist_bytes;	/* Free-list byte count */
	uint32_t freelist_entries;	/* Free-list entry count */
					/* Free-list queues */
	TAILQ_HEAD(__wt_free_qah, __wt_free_entry) freeqa;
	TAILQ_HEAD(__wt_free_qsh, __wt_free_entry) freeqs;
	int	 freelist_dirty;	/* Free-list has been modified */

	off_t	 free_offset;		/* Free-list addr/size/checksum  */
	uint32_t free_size;
	uint32_t free_cksum;

					/* Salvage support */
	off_t	 slvg_off;		/* Salvage file offset */

					/* Verification support */
	uint32_t frags;			/* Total frags */
	uint8_t *fragbits;		/* Frag tracking bit list */

#define	WT_BLOCK_OK	0x01		/* File successfully opened */
	uint32_t flags;
};

/*
 * WT_BLOCK_DESC --
 *	The file's description.
 */
struct __wt_block_desc {
#define	WT_BLOCK_MAGIC		120897
	uint32_t magic;			/* 00-03: Magic number */
#define	WT_BTREE_MAJOR_VERSION	0
	uint16_t majorv;		/* 04-05: Major version */
#define	WT_BTREE_MINOR_VERSION	1
	uint16_t minorv;		/* 06-07: Minor version */

	uint32_t cksum;			/* 08-11: Description block checksum */

	uint32_t unused;		/* 12-15: Padding */
#define	WT_BLOCK_FREELIST_MAGIC	071002
	uint64_t free_offset;		/* 16-23: Free list page address */
	uint32_t free_size;		/* 24-27: Free list page length */
	uint32_t free_cksum;		/* 28-31: Free list page checksum */

	/*
	 * We maintain page LSN's for the file in the non-transactional case
	 * (where, instead of a log reference, the LSN is simply a counter),
	 * as that's how salvage can determine the most recent page between
	 * pages overlapping the same key range.  This non-transactional LSN
	 * has to be persistent, and so it's included in the file's metadata.
	 */
	uint64_t lsn;			/* 32-39: Non-transactional page LSN */
};
/*
 * WT_BLOCK_DESC_SIZE is the expected structure size -- we verify the build to
 * ensure the compiler hasn't inserted padding (padding won't cause failure,
 * since we reserve the first sector of the file for this information, but it
 * would be worth investigation, regardless).
 */
#define	WT_BLOCK_DESC_SIZE		40

/*
 * Don't compress the first 32B of the block (almost all of the WT_PAGE_DISK
 * structure) because we need the block's checksum and on-disk and in-memory
 * sizes to be immediately available without decompression (the checksum and
 * the on-disk block sizes are used during salvage to figure out where the
 * blocks are, and the in-memory page size tells us how large a buffer we need
 * to decompress the file block.  We could take less than 32B, but a 32B
 * boundary is probably better alignment for the underlying compression engine,
 * and skipping 32B won't matter in terms of compression efficiency.
 */
#define	WT_COMPRESS_SKIP	32

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
	 * so we can figure out where the column-store leaf page fits into the
	 * key space during salvage.
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

	/*
	 * Page checksums are stored in two places.  First, a page's checksum is
	 * stored in the tree page that references a page as part of the address
	 * cookie.  This is done to ensure we detect corruption, as storing the
	 * checksum in the on-disk page implies a 1 in 2^32 chance corruption of
	 * the page will result in a valid checksum).  Second, a page's checksum
	 * is stored in the disk header.  This is for salvage, so that salvage
	 * knows when it's found a page that has some chance of being useful.
	 * This isn't risky because the complete address cookie in the reference
	 * page is compared before we connect the two pages back together.
	 */
	uint32_t cksum;			/* 16-19: checksum */

	/*
	 * We write the page size in the on-disk page header because it makes
	 * salvage easier.  (If we don't know the expected page length, we'd
	 * have to read increasingly larger chunks from the file until we find
	 * one that checksums, and that's going to be harsh given WiredTiger's
	 * large page sizes.)
	 *
	 * We also store an in-memory size because otherwise we'd have no idea
	 * how much memory to allocate in order to expand a compressed page.
	 */
	uint32_t size;			/* 20-23: on-disk page size */
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
 * WT_DISK_REQUIRED--
 *	Return bytes needed for byte length, rounded to an allocation unit.
 */
#define	WT_DISK_REQUIRED(block, size)					\
	(WT_ALIGN((size) + WT_PAGE_DISK_SIZE, ((WT_BLOCK *)(block))->allocsize))

/*
 * WT_PAGE_DISK_BYTE --
 *	The first usable data byte on the page (past the header).
 */
#define	WT_PAGE_DISK_BYTE(dsk)						\
	((void *)((uint8_t *)(dsk) + WT_PAGE_DISK_SIZE))

/*
 * WT_FREE_ENTRY  --
 *	Encapsulation of an entry on the Btree free list.
 */
struct __wt_free_entry {
	TAILQ_ENTRY(__wt_free_entry) qa;	/* Address queue */
	TAILQ_ENTRY(__wt_free_entry) qs;	/* Size queue */

	off_t	 offset;			/* Disk offset */
	uint32_t size;				/* Size */
};
