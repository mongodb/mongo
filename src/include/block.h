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
 * WT_FREE_ENTRY  --
 *	Encapsulation of an entry on the Btree free list.
 */
struct __wt_free_entry {
	TAILQ_ENTRY(__wt_free_entry) qa;	/* Address queue */
	TAILQ_ENTRY(__wt_free_entry) qs;	/* Size queue */

	off_t	 offset;			/* Disk offset */
	uint32_t size;				/* Size */
};

/*
 * WT_BLOCK --
 *	Encapsulation of the standard WiredTiger block manager.
 */
struct __wt_block {
	const char *name;		/* Name */

	WT_FH	*fh;			/* Backing file handle */

	uint64_t write_gen;		/* Write generation */

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
	 * We maintain page write-generations in the non-transactional case
	 * (where, instead of a transactional LSN, the value is a counter),
	 * as that's how salvage can determine the most recent page between
	 * pages overlapping the same key range.  The value has to persist,
	 * so it's included in the file's metadata.
	 */
	uint64_t write_gen;		/* 32-39: Write generation */
};
/*
 * WT_BLOCK_DESC_SIZE is the expected structure size -- we verify the build to
 * ensure the compiler hasn't inserted padding (padding won't cause failure,
 * since we reserve the first sector of the file for this information, but it
 * would be worth investigation, regardless).
 */
#define	WT_BLOCK_DESC_SIZE		40

/*
 * WT_BLOCK_HEADER --
 *	Blocks have a common header, a WT_PAGE_HEADER structure followed by a
 * block-manager specific structure: WT_BLOCK_HEADER is WiredTiger's default.
 */
struct __wt_block_header {
	/*
	 * We maintain page write-generations in the non-transactional case
	 * (where, instead of a transactional LSN, the value is a counter),
	 * as that's how salvage can determine the most recent page between
	 * pages overlapping the same key range.
	 *
	 * !!!
	 * The write-generation is "owned" by the btree layer, but it's easier
	 * to set it (when physically writing blocks), to persist it (in the
	 * WT_BLOCK_DESC structure, rather than the schema file), and restore
	 * it during salvage, in the block-manager layer.
	 */
	uint64_t write_gen;		/* 00-07: write generation */

	/*
	 * We write the page size in the on-disk page header because it makes
	 * salvage easier.  (If we don't know the expected page length, we'd
	 * have to read increasingly larger chunks from the file until we find
	 * one that checksums, and that's going to be harsh given WiredTiger's
	 * potentially large page sizes.)
	 */
	uint32_t size;			/* 08-11: on-disk page size */

	/*
	 * Page checksums are stored in two places.  First, a page's checksum is
	 * in the internal page that references a page as part of the address
	 * cookie.  This is done to ensure we detect corruption, as storing the
	 * checksum in the on-disk page implies a 1 in 2^32 chance corruption of
	 * the page will result in a valid checksum).  Second, a page's checksum
	 * is stored in the disk header.  This is for salvage, so that salvage
	 * knows when it's found a page that may be useful.
	 */
	uint32_t cksum;			/* 12-15: checksum */
};
/*
 * WT_BLOCK_HEADER_SIZE is the number of bytes we allocate for the structure: if
 * the compiler inserts padding it will break the world.
 */
#define	WT_BLOCK_HEADER_SIZE		16

/*
 * WT_BLOCK_HEADER_BYTE
 * WT_BLOCK_HEADER_BYTE_SIZE --
 *	The first usable data byte on the block (past the combined headers).
 */
#define	WT_BLOCK_HEADER_BYTE_SIZE					\
	(WT_PAGE_HEADER_SIZE + WT_BLOCK_HEADER_SIZE)
#define	WT_BLOCK_HEADER_BYTE(dsk)					\
	((void *)((uint8_t *)(dsk) + WT_BLOCK_HEADER_BYTE_SIZE))

/*
 * Don't compress the block's WT_PAGE_HEADER and WT_BLOCK_HEADER structures.
 * We need the WT_PAGE_HEADER in-memory size, and the WT_BLOCK_HEADER checksum
 * and on-disk size to be immediately available without decompression.  We use
 * the on-disk size and checksum during salvage to figure out where the blocks
 * are, and the in-memory size tells us how large a buffer we need to decompress
 * the block.  We could skip less than 64B, but a 64B boundary may offer better
 * alignment for the underlying compression engine, and skipping 64B won't make
 * a difference in terms of compression efficiency.
 */
#define	WT_BLOCK_COMPRESS_SKIP	64
