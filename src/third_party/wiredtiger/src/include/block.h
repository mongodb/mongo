/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * WiredTiger's block manager interface.
 */

/*
 * The file's description is written into the first block of the file, which means we can use an
 * offset of 0 as an invalid offset.
 */
#define WT_BLOCK_INVALID_OFFSET 0

/*
 * The block manager maintains three per-checkpoint extent lists:
 *	alloc:	 the extents allocated in this checkpoint
 *	avail:	 the extents available for allocation
 *	discard: the extents freed in this checkpoint
 *
 * An extent list is based on two skiplists: first, a by-offset list linking
 * WT_EXT elements and sorted by file offset (low-to-high), second, a by-size
 * list linking WT_SIZE elements and sorted by chunk size (low-to-high).
 *
 * Additionally, each WT_SIZE element on the by-size has a skiplist of its own,
 * linking WT_EXT elements and sorted by file offset (low-to-high).  This list
 * has an entry for extents of a particular size.
 *
 * The trickiness is each individual WT_EXT element appears on two skiplists.
 * In order to minimize allocation calls, we allocate a single array of WT_EXT
 * pointers at the end of the WT_EXT structure, for both skiplists, and store
 * the depth of the skiplist in the WT_EXT structure.  The skiplist entries for
 * the offset skiplist start at WT_EXT.next[0] and the entries for the size
 * skiplist start at WT_EXT.next[WT_EXT.depth].
 *
 * One final complication: we only maintain the per-size skiplist for the avail
 * list, the alloc and discard extent lists are not searched based on size.
 */

/*
 * WT_EXTLIST --
 *	An extent list.
 */
struct __wt_extlist {
    char *name; /* Name */

    uint64_t bytes;   /* Byte count */
    uint32_t entries; /* Entry count */

    uint32_t objectid; /* Written object ID */
    wt_off_t offset;   /* Written extent offset */
    uint32_t checksum; /* Written extent checksum */
    uint32_t size;     /* Written extent size */

    bool track_size; /* Maintain per-size skiplist */

    WT_EXT *last; /* Cached last element */

    WT_EXT *off[WT_SKIP_MAXDEPTH]; /* Size/offset skiplists */
    WT_SIZE *sz[WT_SKIP_MAXDEPTH];
};

/*
 * WT_EXT --
 *	Encapsulation of an extent, either allocated or freed within the
 * checkpoint.
 */
struct __wt_ext {
    wt_off_t off;  /* Extent's file offset */
    wt_off_t size; /* Extent's Size */

    uint8_t depth; /* Skip list depth */

    /*
     * Variable-length array, sized by the number of skiplist elements. The first depth array
     * entries are the address skiplist elements, the second depth array entries are the size
     * skiplist.
     */
    WT_EXT *next[0]; /* Offset, size skiplists */
};

/*
 * WT_SIZE --
 *	Encapsulation of a block size skiplist entry.
 */
struct __wt_size {
    wt_off_t size; /* Size */

    uint8_t depth; /* Skip list depth */

    WT_EXT *off[WT_SKIP_MAXDEPTH]; /* Per-size offset skiplist */

    /*
     * We don't use a variable-length array for the size skiplist, we want to be able to use any
     * cached WT_SIZE structure as the head of a list, and we don't know the related WT_EXT
     * structure's depth.
     */
    WT_SIZE *next[WT_SKIP_MAXDEPTH]; /* Size skiplist */
};

/*
 * Per session handle cached block manager information.
 */
typedef struct {
    WT_EXT *ext_cache;   /* List of WT_EXT handles */
    u_int ext_cache_cnt; /* Count */

    WT_SIZE *sz_cache;  /* List of WT_SIZE handles */
    u_int sz_cache_cnt; /* Count */
} WT_BLOCK_MGR_SESSION;

/*
 * WT_EXT_FOREACH --
 *	Walk a block manager skiplist.
 * WT_EXT_FOREACH_OFF --
 *	Walk a block manager skiplist where the WT_EXT.next entries are offset
 * by the depth.
 */
#define WT_EXT_FOREACH(skip, head) \
    for ((skip) = (head)[0]; (skip) != NULL; (skip) = (skip)->next[0])
#define WT_EXT_FOREACH_OFF(skip, head) \
    for ((skip) = (head)[0]; (skip) != NULL; (skip) = (skip)->next[(skip)->depth])

/*
 * WT_EXT_FOREACH_FROM_OFFSET_INCL --
 *	Walk a by-offset skiplist from the given offset, starting with the extent that contains the
 * given offset if available.
 */
#define WT_EXT_FOREACH_FROM_OFFSET_INCL(skip, el, start)                        \
    for ((skip) = __wt_block_off_srch_inclusive((el), (start)); (skip) != NULL; \
         (skip) = (skip)->next[0])

/*
 * Checkpoint cookie: carries a version number as I don't want to rev the schema
 * file version should the default block manager checkpoint format change.
 *
 * Version #1 checkpoint cookie format:
 *	[1] [root addr] [alloc addr] [avail addr] [discard addr]
 *	    [file size] [checkpoint size] [write generation]
 */
#define WT_BM_CHECKPOINT_VERSION 1   /* Checkpoint format version */
#define WT_BLOCK_EXTLIST_MAGIC 71002 /* Identify a list */

/*
 * There are two versions of the extent list blocks: the original, and a second version where
 * current checkpoint information is appended to the avail extent list.
 */
#define WT_BLOCK_EXTLIST_VERSION_ORIG 0 /* Original version */
#define WT_BLOCK_EXTLIST_VERSION_CKPT 1 /* Checkpoint in avail output */

/*
 * Maximum buffer required to store a checkpoint: 1 version byte followed by
 * 14 packed 8B values.
 */
#define WT_BLOCK_CHECKPOINT_BUFFER (1 + 14 * WT_INTPACK64_MAXSIZE)

struct __wt_block_ckpt {
    uint8_t version; /* Version */

    uint32_t root_objectid;
    wt_off_t root_offset; /* The root */
    uint32_t root_checksum, root_size;

    WT_EXTLIST alloc;   /* Extents allocated */
    WT_EXTLIST avail;   /* Extents available */
    WT_EXTLIST discard; /* Extents discarded */

    wt_off_t file_size; /* Checkpoint file size */
    uint64_t ckpt_size; /* Checkpoint byte count */

    WT_EXTLIST ckpt_avail; /* Checkpoint free'd extents */

    /*
     * Checkpoint archive: the block manager may potentially free a lot of memory from the
     * allocation and discard extent lists when checkpoint completes. Put it off until the
     * checkpoint resolves, that lets the upper btree layer continue eviction sooner.
     */
    WT_EXTLIST ckpt_alloc;   /* Checkpoint archive */
    WT_EXTLIST ckpt_discard; /* Checkpoint archive */
};

#define WT_BLOCK_INVALID_PAGE_ID 0 /* Invalid page ID, e.g., if it's not allocated. */
#define WT_BLOCK_MIN_PAGE_ID 100   /* Minimum page ID that can be used for user data. */

/*
 * WT_BM --
 *	Block manager handle, references a single checkpoint in a btree.
 */
struct __wt_bm {
    /* Methods */
    int (*addr_invalid)(WT_BM *, WT_SESSION_IMPL *, const uint8_t *, size_t);
    int (*addr_string)(WT_BM *, WT_SESSION_IMPL *, WT_ITEM *, const uint8_t *, size_t);
    u_int (*block_header)(WT_BM *);
    bool (*can_truncate)(WT_BM *, WT_SESSION_IMPL *);
    int (*checkpoint)(WT_BM *, WT_SESSION_IMPL *, WT_ITEM *, WT_PAGE_BLOCK_META *, WT_CKPT *, bool);
    int (*checkpoint_last)(WT_BM *, WT_SESSION_IMPL *, char **, char **, WT_ITEM *);
    int (*checkpoint_load)(
      WT_BM *, WT_SESSION_IMPL *, const uint8_t *, size_t, uint8_t *, size_t *, bool);
    int (*checkpoint_resolve)(WT_BM *, WT_SESSION_IMPL *, bool);
    int (*checkpoint_start)(WT_BM *, WT_SESSION_IMPL *);
    int (*checkpoint_unload)(WT_BM *, WT_SESSION_IMPL *);
    int (*close)(WT_BM *, WT_SESSION_IMPL *);
    int (*compact_end)(WT_BM *, WT_SESSION_IMPL *);
    int (*compact_page_rewrite)(WT_BM *, WT_SESSION_IMPL *, uint8_t *, size_t *, bool *);
    int (*compact_page_skip)(WT_BM *, WT_SESSION_IMPL *, const uint8_t *, size_t, bool *);
    int (*compact_skip)(WT_BM *, WT_SESSION_IMPL *, bool *);
    void (*compact_progress)(WT_BM *, WT_SESSION_IMPL *);
    int (*compact_start)(WT_BM *, WT_SESSION_IMPL *);
    int (*corrupt)(WT_BM *, WT_SESSION_IMPL *, const uint8_t *, size_t);
    size_t (*encrypt_skip)(WT_BM *, WT_SESSION_IMPL *);
    int (*free)(WT_BM *, WT_SESSION_IMPL *, const uint8_t *, size_t);
    int (*get_page_ids)(WT_BM *, WT_SESSION_IMPL *, WT_ITEM *, size_t *, uint64_t);
    bool (*is_mapped)(WT_BM *, WT_SESSION_IMPL *);
    int (*map_discard)(WT_BM *, WT_SESSION_IMPL *, void *, size_t);
    int (*read)(
      WT_BM *, WT_SESSION_IMPL *, WT_ITEM *, WT_PAGE_BLOCK_META *, const uint8_t *, size_t);
    int (*read_multiple)(WT_BM *, WT_SESSION_IMPL *, WT_PAGE_BLOCK_META *, const uint8_t *, size_t,
      WT_ITEM *items, uint32_t *item_count);
    int (*salvage_end)(WT_BM *, WT_SESSION_IMPL *);
    int (*salvage_next)(WT_BM *, WT_SESSION_IMPL *, uint8_t *, size_t *, bool *);
    int (*salvage_start)(WT_BM *, WT_SESSION_IMPL *);
    int (*salvage_valid)(WT_BM *, WT_SESSION_IMPL *, uint8_t *, size_t, bool);
    int (*size)(WT_BM *, WT_SESSION_IMPL *, wt_off_t *);
    int (*stat)(WT_BM *, WT_SESSION_IMPL *, WT_DSRC_STATS *stats);
    int (*switch_object)(WT_BM *, WT_SESSION_IMPL *, uint32_t);
    int (*switch_object_end)(WT_BM *, WT_SESSION_IMPL *, uint32_t);
    int (*sync)(WT_BM *, WT_SESSION_IMPL *, bool);
    int (*verify_addr)(WT_BM *, WT_SESSION_IMPL *, const uint8_t *, size_t);
    int (*verify_end)(WT_BM *, WT_SESSION_IMPL *, bool verify_success);
    int (*verify_start)(WT_BM *, WT_SESSION_IMPL *, WT_CKPT *, const char *[]);
    int (*write)(
      WT_BM *, WT_SESSION_IMPL *, WT_ITEM *, WT_PAGE_BLOCK_META *, uint8_t *, size_t *, bool, bool);
    int (*write_size)(WT_BM *, WT_SESSION_IMPL *, size_t *);

    WT_BLOCK *block; /* Underlying file. For a multi-handle tree this will be the writable file. */
    WT_BLOCK *next_block; /* If doing a tier switch, this is going to be the new file. */
    WT_BLOCK *prev_block; /* If a tier switch was done, this was the old file. */

    void *map; /* Mapped region */
    size_t maplen;
    void *mapped_cookie;
    bool is_remote; /* Whether the storage is located on a remote host. */

    /*
     * For trees, such as tiered tables, that are allowed to have more than one backing file or
     * object, we maintain an array of the block handles used by the tree. We use a reader-writer
     * mutex to protect the array. We lock it for reading when looking for a handle in the array and
     * lock it for writing when adding or removing handles in the array.
     */
    bool is_multi_handle;
    WT_BLOCK **handle_array;       /* Array of block handles */
    size_t handle_array_allocated; /* Size of handle array */
    WT_RWLOCK handle_array_lock;   /* Lock for block handle array */
    u_int handle_array_next;       /* Next open slot */
    uint32_t max_flushed_objectid; /* Local objects at or below this id should be closed */

    /*
     * There's only a single block manager handle that can be written, all others are checkpoints.
     */
    bool is_live; /* The live system */
};

/*
 * WT_BLOCK --
 *	Block manager file handle.
 */
struct __wt_block {
    const char *name;  /* Name */
    uint32_t objectid; /* Object id */
    uint32_t ref;      /* References */

    TAILQ_ENTRY(__wt_block) q;     /* Linked list of handles */
    TAILQ_ENTRY(__wt_block) hashq; /* Hashed list of handles */

    WT_FH *fh;            /* Backing file handle */
    wt_off_t size;        /* Storage size */
    wt_off_t extend_size; /* File extended size */
    wt_off_t extend_len;  /* File extend chunk size */

    bool created_during_backup; /* Created during incremental backup */
    bool sync_on_checkpoint;    /* fsync the handle after the next checkpoint */
    bool remote;                /* Handle references non-local object */
    bool readonly;              /* Underlying file was opened only for reading */

    /* Configuration information, set when the file is opened. */
    wt_shared uint32_t allocfirst; /* Allocation is first-fit */
    uint32_t allocsize;            /* Allocation size */
    size_t os_cache;               /* System buffer cache flush max */
    size_t os_cache_max;
    size_t os_cache_dirty_max;

    u_int block_header; /* Header length */

    /*
     * There is only a single checkpoint in a file that can be written; stored here, only accessed
     * by one WT_BM handle.
     */
    WT_SPINLOCK live_lock; /* Live checkpoint lock */
    WT_BLOCK_CKPT live;    /* Live checkpoint */
    bool live_open;        /* Live system is open */
    enum {                 /* Live checkpoint status */
        WT_CKPT_NONE = 0,
        WT_CKPT_INPROGRESS,
        WT_CKPT_PANIC_ON_FAILURE,
        WT_CKPT_SALVAGE
    } ckpt_state;

    WT_CKPT *final_ckpt; /* Final live checkpoint write */

    /* Compaction support */
    bool compact_estimated;                    /* If compaction work has been estimated */
    int compact_pct_tenths;                    /* Percent to compact */
    uint64_t compact_bytes_reviewed;           /* Bytes reviewed */
    uint64_t compact_bytes_rewritten;          /* Bytes rewritten */
    uint64_t compact_bytes_rewritten_expected; /* The expected number of bytes to rewrite */
    uint64_t compact_internal_pages_reviewed;  /* Internal pages reviewed */
    uint64_t compact_pages_reviewed;           /* Pages reviewed */
    uint64_t compact_pages_rewritten;          /* Pages rewritten */
    uint64_t compact_pages_rewritten_expected; /* The expected number of pages to rewrite */
    uint64_t compact_pages_skipped;            /* Pages skipped */
    uint64_t compact_prev_pages_rewritten;     /* Pages rewritten during the previous iteration */
    wt_off_t compact_prev_size;                /* File size at the start of a compaction pass */
    uint32_t compact_session_id;               /* Session compacting */

    /* Salvage support */
    wt_off_t slvg_off; /* Salvage file offset */

    /* Verification support */
    bool verify;             /* If performing verification */
    bool verify_layout;      /* Print out file layout information */
    bool dump_tree_shape;    /* Print out tree shape */
    bool verify_strict;      /* Fail hard on any error */
    wt_off_t verify_size;    /* Checkpoint's file size */
    WT_EXTLIST verify_alloc; /* Verification allocation list */
    uint64_t frags;          /* Maximum frags in the file */
    uint8_t *fragfile;       /* Per-file frag tracking list */
    uint8_t *fragckpt;       /* Per-checkpoint frag tracking list */

    /* Multi-file support */
    wt_shared uint32_t read_count; /* Count of active read requests using this block handle */
};

/*
 * WT_BLOCK_DESC --
 *	The file's description.
 */
struct __wt_block_desc {
#define WT_BLOCK_MAGIC 120897
    uint32_t magic; /* 00-03: Magic number */
#define WT_BLOCK_MAJOR_VERSION 1
    uint16_t majorv; /* 04-05: Major version */
#define WT_BLOCK_MINOR_VERSION 0
    uint16_t minorv; /* 06-07: Minor version */

    uint32_t checksum; /* 08-11: Description block checksum */

    uint32_t unused; /* 12-15: Padding */
};
/*
 * WT_BLOCK_DESC_SIZE is the expected structure size -- we verify the build to ensure the compiler
 * hasn't inserted padding (padding won't cause failure, we reserve the first allocation-size block
 * of the file for this information, but it would be worth investigation, regardless).
 */
#define WT_BLOCK_DESC_SIZE 16

/*
 * __wt_block_desc_byteswap --
 *     Handle big- and little-endian transformation of a description block.
 */
static WT_INLINE void
__wt_block_desc_byteswap(WT_BLOCK_DESC *desc)
{
#ifdef WORDS_BIGENDIAN
    desc->magic = __wt_bswap32(desc->magic);
    desc->majorv = __wt_bswap16(desc->majorv);
    desc->minorv = __wt_bswap16(desc->minorv);
    desc->checksum = __wt_bswap32(desc->checksum);
#else
    WT_UNUSED(desc);
#endif
}

/*
 * WT_BLOCK_HEADER --
 *	Blocks have a common header, a WT_PAGE_HEADER structure followed by a
 * block-manager specific structure: WT_BLOCK_HEADER is WiredTiger's default.
 */
struct __wt_block_header {
    /*
     * We write the page size in the on-disk page header because it makes salvage easier. (If we
     * don't know the expected page length, we'd have to read increasingly larger chunks from the
     * file until we find one that checksums, and that's going to be harsh given WiredTiger's
     * potentially large page sizes.)
     */
    uint32_t disk_size; /* 00-03: on-disk page size */

    /*
     * Page checksums are stored in two places. First, the page checksum is written within the
     * internal page that references it as part of the address cookie. This is done to improve the
     * chances of detecting not only disk corruption but other bugs (for example, overwriting a page
     * with another valid page image). Second, a page's checksum is stored in the disk header. This
     * is for salvage, so salvage knows it has found a page that may be useful.
     */
    uint32_t checksum; /* 04-07: checksum */

/*
 * No automatic generation: flag values cannot change, they're written to disk.
 */
#define WT_BLOCK_DATA_CKSUM 0x1u /* Block data is part of the checksum */
    uint8_t flags;               /* 08: flags */

    /*
     * End the structure with 3 bytes of padding: it wastes space, but it leaves the structure
     * 32-bit aligned and having a few bytes to play with in the future can't hurt.
     */
    uint8_t unused[3]; /* 09-11: unused padding */
};

/*
 * WT_BLOCK_HEADER_SIZE is the number of bytes we allocate for the structure: if the compiler
 * inserts padding it will break the world.
 */
#define WT_BLOCK_HEADER_SIZE 12

/*
 * WT_BLOCK_HEADER_BYTE_SIZE --
 *	The first usable data byte on the block (past the combined headers).
 */
#define WT_BLOCK_HEADER_BYTE_SIZE (WT_PAGE_HEADER_SIZE + WT_BLOCK_HEADER_SIZE)
#define WT_BLOCK_HEADER_BYTE(dsk) ((void *)((uint8_t *)(dsk) + WT_BLOCK_HEADER_BYTE_SIZE))

/*
 * We don't compress or encrypt the block's WT_PAGE_HEADER or WT_BLOCK_HEADER structures because we
 * need both available with decompression or decryption. We use the WT_BLOCK_HEADER checksum and
 * on-disk size during salvage to figure out where the blocks are, and we use the WT_PAGE_HEADER
 * in-memory size during decompression and decryption to know how large a target buffer to allocate.
 * We can only skip the header information when doing encryption, but we skip the first 64B when
 * doing compression; a 64B boundary may offer better alignment for the underlying compression
 * engine, and skipping 64B shouldn't make any difference in terms of compression efficiency.
 */
#define WT_BLOCK_COMPRESS_SKIP 64

/*
 * WT_BLOCK_DISAGG --
 *	Block manager handle for disaggregated storage block manager.
 */
struct __wt_block_disagg {
    /*
     * The structure needs to exactly match the WT_BLOCK structure, since it can be treated as one
     * for connection caching and a few other things. For custom fields, see below. Ideally we would
     * split this into a public/private structure, similar to session handles, and customize file
     * and disagg handles as necessary. That's invasive so save the grunt work for now.
     */
    const char *name;  /* Name */
    uint32_t objectid; /* Object id */
    uint32_t ref;      /* References */

    TAILQ_ENTRY(__wt_block) q;     /* Linked list of handles */
    TAILQ_ENTRY(__wt_block) hashq; /* Hashed list of handles */

    /* Custom disaggregated fields. */
    uint64_t tableid;
    WT_PAGE_LOG_HANDLE *plhandle;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_BLOCK_DISAGG_HS 0x1u
    /*AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/*
 * WT_BLOCK_DISAGG_HEADER --
 *	The disaggregated block manager custom header
 */
struct __wt_block_disagg_header {
#define WT_BLOCK_DISAGG_MAGIC_BASE 0xdb
#define WT_BLOCK_DISAGG_MAGIC_DELTA 0xdd
    uint8_t magic; /* 00: magic byte, one of the values above */

    /*
     * As we create new versions, we bump the version number here, and consider what previous
     * versions are compatible with it.
     */
#define WT_BLOCK_DISAGG_VERSION 0x1u
    uint8_t version; /* 01: version of writer */

#define WT_BLOCK_DISAGG_COMPATIBLE_VERSION 0x1u
    uint8_t compatible_version; /* 02: minimum version of reader */

    uint8_t header_size; /* 03: size of unencrypted, uncompressed header */

    /*
     * Page checksums are stored in two places. Similarly to the default block header, except that
     * for pages that have deltas, the checksum of the most recent delta is stored in the internal
     * page, which must match the checksum found in this header. The checksum of the previous delta
     * or base page is stored in this block header, that must in turn match the checksum found in
     * the block header for the previous one. This is how we can verify that we have every expected
     * delta and that each delta is not corrupted.
     */
    uint32_t checksum;          /* 04-07: checksum */
    uint32_t previous_checksum; /* 08-11: checksum for previous delta or page */

/*
 * No automatic generation: flag values cannot change, they're written to disk.
 */
#define WT_BLOCK_DISAGG_DATA_CKSUM 0x1u /* Block data is part of the checksum */
#define WT_BLOCK_DISAGG_ENCRYPTED 0x2u  /* Data following header is encrypted */
#define WT_BLOCK_DISAGG_COMPRESSED 0x4u /* Data following header is compressed */
    uint8_t flags;                      /* 12: flags */

    /*
     * End the structure with 3 bytes of padding: it wastes space, but it leaves the structure
     * 32-bit aligned and having an extra couple bytes to play with in the future can't hurt.
     */
    uint8_t unused[3]; /* 13-15: unused padding */
};

/*
 * WT_BLOCK_DISAGG_HEADER_SIZE is the number of bytes we allocate for a base page and delta
 * structures: if the compiler inserts padding it will break the world.
 */
#define WT_BLOCK_DISAGG_HEADER_SIZE 16
#define WT_BLOCK_DISAGG_HEADER_BYTE_SIZE (WT_PAGE_HEADER_SIZE + WT_BLOCK_DISAGG_HEADER_SIZE)
#define WT_BLOCK_DISAGG_CHECKPOINT_BUFFER (1024)

/*
 * WT_BLOCK_DISAGG_ADDRESS_COOKIE --
 *	The disaggregated block manager address cookie.
 */
struct __wt_block_disagg_address_cookie {

    uint64_t page_id; /* Page ID */

/*
 * Flags for address cookies in disaggregated storage. No automatic generation: flag values cannot
 * change, because they are stored persistently.
 */
#define WT_BLOCK_DISAGG_ADDR_FLAG_DELTA 0x1u /* Address is a delta */

/* The mask of all currently supported flags (for verification). */
#define WT_BLOCK_DISAGG_ADDR_ALL_FLAGS (WT_BLOCK_DISAGG_ADDR_FLAG_DELTA)

    uint64_t flags; /* Flags for the address cookie */

    uint64_t lsn;      /* Log sequence number */
    uint64_t base_lsn; /* Base log sequence number */

    uint32_t size;     /* Size of the data */
    uint32_t checksum; /* Checksum of the data */
};
