/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Supported btree formats: the "current" version is the maximum supported
 * major/minor versions.
 */
#define	WT_BTREE_MAJOR_VERSION_MIN	1	/* Oldest version supported */
#define	WT_BTREE_MINOR_VERSION_MIN	1

#define	WT_BTREE_MAJOR_VERSION_MAX	1	/* Newest version supported */
#define	WT_BTREE_MINOR_VERSION_MAX	1

/*
 * The maximum btree leaf and internal page size is 512MB (2^29).  The limit
 * is enforced in software, it could be larger, specifically, the underlying
 * default block manager can support 4GB (2^32).  Currently, the maximum page
 * size must accommodate our dependence on the maximum page size fitting into
 * a number of bits less than 32; see the row-store page key-lookup functions
 * for the magic.
 */
#define	WT_BTREE_PAGE_SIZE_MAX		(512 * WT_MEGABYTE)

/*
 * The length of variable-length column-store values and row-store keys/values
 * are stored in a 4B type, so the largest theoretical key/value item is 4GB.
 * However, in the WT_UPDATE structure we use the UINT32_MAX size as a "deleted"
 * flag, and second, the size of an overflow object is constrained by what an
 * underlying block manager can actually write.  (For example, in the default
 * block manager, writing an overflow item includes the underlying block's page
 * header and block manager specific structure, aligned to an allocation-sized
 * unit).  The btree engine limits the size of a single object to (4GB - 1KB);
 * that gives us additional bytes if we ever want to store a structure length
 * plus the object size in 4B, or if we need additional flag values.  Attempts
 * to store large key/value items in the tree trigger an immediate check to the
 * block manager, to make sure it can write the item.  Storing 4GB objects in a
 * btree borders on clinical insanity, anyway.
 *
 * Record numbers are stored in 64-bit unsigned integers, meaning the largest
 * record number is "really, really big".
 */
#define	WT_BTREE_MAX_OBJECT_SIZE	(UINT32_MAX - 1024)

/*
 * A location in a file is a variable-length cookie, but it has a maximum size
 * so it's easy to create temporary space in which to store them.  (Locations
 * can't be much larger than this anyway, they must fit onto the minimum size
 * page because a reference to an overflow page is itself a location.)
 */
#define	WT_BTREE_MAX_ADDR_COOKIE	255	/* Maximum address cookie */

/* Evict pages if we see this many consecutive deleted records. */
#define	WT_BTREE_DELETE_THRESHOLD	1000

/*
 * WT_BTREE --
 *	A btree handle.
 */
struct __wt_btree {
	WT_DATA_HANDLE *dhandle;

	WT_CKPT	  *ckpt;		/* Checkpoint information */

	enum {	BTREE_COL_FIX=1,	/* Fixed-length column store */
		BTREE_COL_VAR=2,	/* Variable-length column store */
		BTREE_ROW=3		/* Row-store */
	} type;				/* Type */

	const char *key_format;		/* Key format */
	const char *value_format;	/* Value format */
	uint8_t bitcnt;			/* Fixed-length field size in bits */

	WT_COLLATOR *collator;		/* Row-store comparator */
	int collator_owned;		/* The collator needs to be freed */

	uint32_t id;			/* File ID, for logging */

	uint32_t key_gap;		/* Row-store prefix key gap */

	uint32_t allocsize;		/* Allocation size */
	uint32_t maxintlpage;		/* Internal page max size */
	uint32_t maxintlkey;		/* Internal page max key size */
	uint32_t maxleafpage;		/* Leaf page max size */
	uint32_t maxleafkey;		/* Leaf page max key size */
	uint32_t maxleafvalue;		/* Leaf page max value size */
	uint64_t maxmempage;		/* In-memory page max size */
	uint64_t splitmempage;		/* In-memory split trigger size */

	void *huffman_key;		/* Key huffman encoding */
	void *huffman_value;		/* Value huffman encoding */

	enum {	CKSUM_ON=1,		/* On */
		CKSUM_OFF=2,		/* Off */
		CKSUM_UNCOMPRESSED=3	/* Uncompressed blocks only */
	} checksum;			/* Checksum configuration */

	/*
	 * Reconciliation...
	 */
	u_int dictionary;		/* Dictionary slots */
	bool  internal_key_truncate;	/* Internal key truncate */
	int   maximum_depth;		/* Maximum tree depth */
	bool  prefix_compression;	/* Prefix compression */
	u_int prefix_compression_min;	/* Prefix compression min */
#define	WT_SPLIT_DEEPEN_MIN_CHILD_DEF	10000
	u_int split_deepen_min_child;	/* Minimum entries to deepen tree */
#define	WT_SPLIT_DEEPEN_PER_CHILD_DEF	100
	u_int split_deepen_per_child;	/* Entries per child when deepened */
	int   split_pct;		/* Split page percent */
	WT_COMPRESSOR *compressor;	/* Page compressor */
	WT_KEYED_ENCRYPTOR *kencryptor;	/* Page encryptor */
	WT_RWLOCK *ovfl_lock;		/* Overflow lock */

	uint64_t last_recno;		/* Column-store last record number */

	WT_REF root;			/* Root page reference */
	int modified;			/* If the tree ever modified */
	bool bulk_load_ok;		/* Bulk-load is a possibility */

	WT_BM	*bm;			/* Block manager reference */
	u_int	 block_header;		/* WT_PAGE_HEADER_BYTE_SIZE */

	uint64_t checkpoint_gen;	/* Checkpoint generation */
	uint64_t rec_max_txn;		/* Maximum txn seen (clean trees) */
	uint64_t write_gen;		/* Write generation */

	WT_REF	   *evict_ref;		/* Eviction thread's location */
	uint64_t    evict_priority;	/* Relative priority of cached pages */
	u_int	    evict_walk_period;	/* Skip this many LRU walks */
	u_int	    evict_walk_skips;	/* Number of walks skipped */
	u_int	    evict_disabled;	/* Eviction disabled count */
	volatile uint32_t evict_busy;	/* Count of threads in eviction */

	enum {
		WT_CKPT_OFF, WT_CKPT_PREPARE, WT_CKPT_RUNNING
	} checkpointing;		/* Checkpoint in progress */

	/*
	 * We flush pages from the tree (in order to make checkpoint faster),
	 * without a high-level lock.  To avoid multiple threads flushing at
	 * the same time, lock the tree.
	 */
	WT_SPINLOCK	flush_lock;	/* Lock to flush the tree's pages */

	/* Flags values up to 0xff are reserved for WT_DHANDLE_* */
#define	WT_BTREE_BULK		0x00100	/* Bulk-load handle */
#define	WT_BTREE_IN_MEMORY	0x00200	/* Cache-resident object */
#define	WT_BTREE_LOOKASIDE	0x00400	/* Look-aside table */
#define	WT_BTREE_NO_CHECKPOINT	0x00800	/* Disable checkpoints */
#define	WT_BTREE_NO_EVICTION	0x01000	/* Disable eviction */
#define	WT_BTREE_NO_LOGGING	0x02000	/* Disable logging */
#define	WT_BTREE_REBALANCE	0x04000	/* Handle is for rebalance */
#define	WT_BTREE_SALVAGE	0x08000	/* Handle is for salvage */
#define	WT_BTREE_SKIP_CKPT	0x10000	/* Handle skipped checkpoint */
#define	WT_BTREE_UPGRADE	0x20000	/* Handle is for upgrade */
#define	WT_BTREE_VERIFY		0x40000	/* Handle is for verify */
	uint32_t flags;
};

/* Flags that make a btree handle special (not for normal use). */
#define	WT_BTREE_SPECIAL_FLAGS	 					\
	(WT_BTREE_BULK | WT_BTREE_REBALANCE |				\
	WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY)

/*
 * WT_SALVAGE_COOKIE --
 *	Encapsulation of salvage information for reconciliation.
 */
struct __wt_salvage_cookie {
	uint64_t missing;			/* Initial items to create */
	uint64_t skip;				/* Initial items to skip */
	uint64_t take;				/* Items to take */

	bool	 done;				/* Ignore the rest */
};
