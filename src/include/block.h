/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
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
 * The block allocator maintains two primary skiplists: first, the by-offset
 * list linking WT_EXT elements and sorted by file offset (low-to-high):
 * this list has an entry for every free chunk in the file.  The second primary
 * skiplist is the by-size list linking WT_SIZE elements and sorted by chunk
 * size (low-to-high).  This list has an entry for every free chunk size seen
 * since the list was created.
 *	Additionally, each WT_SIZE element has a skiplist of its own, linking
 * WT_EXT elements and sorted by file offset (low-to-high).  This list has an
 * entry for every free chunk in the file of a particular size.
 *	The trickiness is that each individual WT_EXT element appears on two
 * skiplists.  In order to minimize allocation calls, we allocate a single
 * array of WT_EXT pointers at the end of the WT_EXT structure, for both
 * skiplists, and store the depth of the skiplist in the WT_EXT structure.
 * The skiplist entries for the offset skiplist start at WT_EXT.next[0] and
 * the entries for the size skiplist start at WT_EXT.next[WT_EXT.depth].
 */

/*
 * WT_EXTLIST --
 *	An extent list.
 */
struct __wt_extlist {
	const char *name;			/* Name */

	uint64_t bytes;				/* Byte count */
	uint32_t entries;			/* Entry count */

	WT_EXT	*off[WT_SKIP_MAXDEPTH];		/* Size/offset skiplists */
	WT_SIZE *size[WT_SKIP_MAXDEPTH];
};

/*
 * WT_EXT --
 *	Encapsulation of an extent, either allocated or freed within the
 * snapshot.
 */
struct __wt_ext {
	off_t	 off;				/* Extent's file offset */
	off_t	 size;				/* Extent's Size */

	uint8_t	 depth;				/* Skip list depth */

	/*
	 * Variable-length array, sized by the number of skiplist elements.
	 * The first depth array entries are the address skiplist elements,
	 * the second depth array entries are the size skiplist.
	 */
	WT_EXT	*next[0];			/* Offset, size skiplists */
};

/*
 * WT_SIZE --
 *	Encapsulation of a block size skiplist entry.
 */
struct __wt_size {
	off_t	 size;				/* Size */

	uint8_t	 depth;				/* Skip list depth */

	WT_EXT	*off[WT_SKIP_MAXDEPTH];		/* Per-size offset skiplist */

	/* Variable-length array, sized by the number of skiplist elements. */
	WT_SIZE *next[0];			/* Size skiplist */
};

/*
 * WT_EXT_FOREACH --
 *	Walk a block manager skiplist.
 * WT_EXT_FOREACH_OFF --
 *	Walk a block manager skiplist where the WT_EXT.next entries are offset
 * by the depth.
 */
#define	WT_EXT_FOREACH(skip, head)					\
	for ((skip) = (head)[0];					\
	    (skip) != NULL; (skip) = (skip)->next[0])
#define	WT_EXT_FOREACH_OFF(skip, head)					\
	for ((skip) = (head)[0];					\
	    (skip) != NULL; (skip) = (skip)->next[(skip)->depth])

/*
 * Snapshot cookie: carries a version number as I don't want to rev the schema
 * file version should the default block manager snapshot format change.
 *
 * Version #1 snapshot cookie format:
 *	[1] [root addr] [alloc addr] [avail addr] [discard addr]
 *	    [file size] [write generation]
 */
#define	WT_BM_SNAPSHOT_VERSION		1	/* Snapshot format version */
#define	WT_BLOCK_EXTLIST_MAGIC		71002	/* Identify a list */
struct __wt_block_snapshot {
	uint8_t	 version;			/* Version */

	off_t	 root_offset;			/* The root */
	uint32_t root_cksum, root_size;

	WT_EXTLIST  alloc;			/* Extents allocated */
	off_t	 alloc_offset;			/* Written allocation list */
	uint32_t alloc_cksum, alloc_size;

	WT_EXTLIST  avail;			/* Extents available */
	off_t	 avail_offset;			/* Written available list */
	uint32_t avail_cksum, avail_size;

	WT_EXTLIST  discard;			/* Extents discarded */
	off_t	 discard_offset;		/* Written discard list */
	uint32_t discard_cksum, discard_size;

	off_t	 file_size;			/* File size */

	uint64_t write_gen;			/* Write generation */
};

/*
 * WT_BLOCK --
 *	Encapsulation of the standard WiredTiger block manager.
 */
struct __wt_block {
	const char *name;		/* Name */

	WT_FH	*fh;			/* Backing file handle */

	uint32_t allocsize;		/* Allocation size */
	int	 checksum;		/* If checksums configured */

	WT_SPINLOCK	  live_lock;	/* Lock to protect the live snapshot. */
	WT_BLOCK_SNAPSHOT live;		/* Live snapshot */
	int		  live_load;	/* Live snapshot loaded */

	WT_COMPRESSOR *compressor;	/* Page compressor */

					/* Salvage support */
	int	 slvg;			/* If performing salvage. */
	off_t	 slvg_off;		/* Salvage file offset */

					/* Verification support */
	uint32_t frags;			/* Total frags */
	uint8_t *fragbits;		/* Frag tracking bit list */

};

/*
 * WT_BLOCK_DESC --
 *	The file's description.
 */
struct __wt_block_desc {
#define	WT_BLOCK_MAGIC		120897
	uint32_t magic;			/* 00-03: Magic number */
#define	WT_BLOCK_MAJOR_VERSION	1
	uint16_t majorv;		/* 04-05: Major version */
#define	WT_BLOCK_MINOR_VERSION	0
	uint16_t minorv;		/* 06-07: Minor version */

	uint32_t cksum;			/* 08-11: Description block checksum */

	uint32_t unused;		/* 12-15: Padding */
};
/*
 * WT_BLOCK_DESC_SIZE is the expected structure size -- we verify the build to
 * ensure the compiler hasn't inserted padding (padding won't cause failure,
 * since we reserve the first sector of the file for this information, but it
 * would be worth investigation, regardless).
 */
#define	WT_BLOCK_DESC_SIZE		16

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
	uint32_t disk_size;		/* 08-11: on-disk page size */

	/*
	 * Page checksums are stored in two places.  First, a page's checksum
	 * is in the internal page that references a page as part of the
	 * address cookie.  This is done to improve the chances that we detect
	 * corruption (e.g., overwriting a page with another valid page image).
	 * Second, a page's checksum is stored in the disk header.  This is for
	 * salvage, so that salvage knows when it has found a page that may be
	 * useful.
	 *
	 * Applications can turn off checksums, which is a promise that the
	 * file can never become corrupted, but people sometimes make promises
	 * they can't keep.  If no checksums are configured, we use a pattern
	 * of alternating bits as the checksum, as that is unlikely to occur as
	 * the result of corruption in the file.  If a page happens to checksum
	 * to this special bit pattern, we bump it by one during reads and
	 * writes to avoid ambiguity.
	 */
#define	WT_BLOCK_CHECKSUM_NOT_SET	0xA5C35A3C
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
