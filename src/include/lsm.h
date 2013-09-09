/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_CURSOR_LSM --
 *	An LSM cursor.
 */
struct __wt_cursor_lsm {
	WT_CURSOR iface;

	WT_LSM_TREE *lsm_tree;
	uint64_t dsk_gen;

	u_int nchunks;			/* Number of chunks in the cursor */
	u_int nupdates;			/* Updates needed (including
					   snapshot isolation checks). */
	WT_BLOOM **blooms;		/* Bloom filter handles. */
	size_t bloom_alloc;

	WT_CURSOR **cursors;		/* Cursor handles. */
	size_t cursor_alloc;

	WT_CURSOR *current;     	/* The current cursor for iteration */
	WT_LSM_CHUNK *primary_chunk;	/* The current primary chunk */

	uint64_t *txnid_max;		/* Maximum txn for each chunk */
	size_t txnid_alloc;

#define	WT_CLSM_ITERATE_NEXT    0x01    /* Forward iteration */
#define	WT_CLSM_ITERATE_PREV    0x02    /* Backward iteration */
#define	WT_CLSM_MERGE           0x04    /* Merge cursor, don't update */
#define	WT_CLSM_MINOR_MERGE	0x08    /* Minor merge, include tombstones */
#define	WT_CLSM_MULTIPLE        0x10    /* Multiple cursors have values for the
					   current key */
#define	WT_CLSM_OPEN_READ	0x20    /* Open for reads */
#define	WT_CLSM_OPEN_SNAPSHOT	0x40    /* Open for snapshot isolation */
	uint32_t flags;
};

/*
 * WT_LSM_CHUNK --
 *	A single chunk (file) in an LSM tree.
 */
struct __wt_lsm_chunk {
	uint32_t id;			/* ID used to generate URIs */
	uint32_t generation;		/* Merge generation */
	const char *uri;		/* Data source for this chunk */
	const char *bloom_uri;		/* URI of Bloom filter, if any */
	uint64_t count;			/* Approximate count of records */
	struct timespec create_ts;	/* Creation time (for rate limiting) */

	uint64_t txnid_max;		/* Newest transactional update */

#define	WT_LSM_CHUNK_BLOOM	0x01
#define	WT_LSM_CHUNK_EVICTED	0x02
#define	WT_LSM_CHUNK_MERGING	0x04
#define	WT_LSM_CHUNK_ONDISK	0x08
#define	WT_LSM_CHUNK_STABLE	0x10
	uint32_t flags;
};

/*
 * WT_LSM_TREE --
 *	An LSM tree.
 */
struct __wt_lsm_tree {
	const char *name, *config, *filename;
	const char *key_format, *value_format;
	const char *bloom_config, *file_config;

	WT_COLLATOR *collator;
	const char *collator_name;

	int refcnt;			/* Number of users of the tree */
	WT_RWLOCK *rwlock;
	WT_CONDVAR *work_cond;		/* Used to notify worker of activity */
	TAILQ_ENTRY(__wt_lsm_tree) q;

	WT_DSRC_STATS stats;		/* LSM-level statistics */

	uint64_t dsk_gen;

	long throttle_sleep;		/* Rate limiting */

	/* Configuration parameters */
	uint32_t bloom_bit_count;
	uint32_t bloom_hash_count;
	uint32_t chunk_size;
	u_int merge_max;
	u_int merge_threads;

#define	WT_LSM_BLOOM_MERGED				0x00000001
#define	WT_LSM_BLOOM_NEWEST				0x00000002
#define	WT_LSM_BLOOM_OFF				0x00000004
#define	WT_LSM_BLOOM_OLDEST				0x00000008
	uint32_t bloom;			/* Bloom creation policy */

#define	WT_LSM_MAX_WORKERS	10
					/* Passed to thread_create */
	WT_SESSION_IMPL *worker_sessions[WT_LSM_MAX_WORKERS];
					/* LSM worker thread(s) */
	pthread_t worker_tids[WT_LSM_MAX_WORKERS];
	WT_SESSION_IMPL *ckpt_session;	/* For checkpoint worker */
	pthread_t ckpt_tid;		/* LSM checkpoint worker thread */

	WT_SESSION_IMPL *bloom_session;	/* For bloom worker */
	pthread_t bloom_tid;		/* LSM bloom worker thread */

	WT_LSM_CHUNK **chunk;		/* Array of active LSM chunks */
	size_t chunk_alloc;		/* Space allocated for chunks */
	u_int nchunks;			/* Number of active chunks */
	uint32_t last;			/* Last allocated ID */

	WT_LSM_CHUNK **old_chunks;	/* Array of old LSM chunks */
	size_t old_alloc;		/* Space allocated for old chunks */
	u_int nold_chunks;		/* Number of old chunks */
	u_int old_avail;		/* Available old chunk slots */

#define	WT_LSM_TREE_LOCKED	0x01
#define	WT_LSM_TREE_NEED_SWITCH	0x02
#define	WT_LSM_TREE_OPEN	0x04
#define	WT_LSM_TREE_THROTTLE	0x08
#define	WT_LSM_TREE_WORKING	0x10
	uint32_t flags;
};

/*
 * WT_LSM_DATA_SOURCE --
 *	Implementation of the WT_DATA_SOURCE interface for LSM.
 */
struct __wt_lsm_data_source {
	WT_DATA_SOURCE iface;

	WT_RWLOCK *rwlock;
};

/*
 * WT_LSM_WORKER_COOKIE --
 *	State for an LSM worker thread.
 */
struct __wt_lsm_worker_cookie {
	WT_LSM_CHUNK **chunk_array;
	size_t chunk_alloc;
	u_int nchunks;
};

/*
 * WT_LSM_WORKER_ARGS --
 *	State for an LSM worker thread.
 */
struct __wt_lsm_worker_args {
	WT_LSM_TREE *lsm_tree;
	u_int id;
};
