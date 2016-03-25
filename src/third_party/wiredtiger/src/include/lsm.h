/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

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
	WT_SESSION_IMPL	*session;	/* Session */
	WT_CONDVAR	*work_cond;	/* Owned by the manager */
	wt_thread_t	tid;		/* Thread id */
	u_int		id;		/* My manager slot id */
	uint32_t	type;		/* Types of operations handled */
#define	WT_LSM_WORKER_RUN	0x01
	uint32_t	flags;		/* Worker flags */
};

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

	uint64_t *switch_txn;		/* Switch txn for each chunk */
	size_t txnid_alloc;

	u_int update_count;		/* Updates performed. */

#define	WT_CLSM_ACTIVE		0x001   /* Incremented the session count */
#define	WT_CLSM_BULK		0x002   /* Open for snapshot isolation */
#define	WT_CLSM_ITERATE_NEXT    0x004   /* Forward iteration */
#define	WT_CLSM_ITERATE_PREV    0x008   /* Backward iteration */
#define	WT_CLSM_MERGE           0x010   /* Merge cursor, don't update */
#define	WT_CLSM_MINOR_MERGE	0x020   /* Minor merge, include tombstones */
#define	WT_CLSM_MULTIPLE        0x040   /* Multiple cursors have values for the
					   current key */
#define	WT_CLSM_OPEN_READ	0x080   /* Open for reads */
#define	WT_CLSM_OPEN_SNAPSHOT	0x100   /* Open for snapshot isolation */
	uint32_t flags;
};

/*
 * WT_LSM_CHUNK --
 *	A single chunk (file) in an LSM tree.
 */
struct __wt_lsm_chunk {
	const char *uri;		/* Data source for this chunk */
	const char *bloom_uri;		/* URI of Bloom filter, if any */
	struct timespec create_ts;	/* Creation time (for rate limiting) */
	uint64_t count;			/* Approximate count of records */
	uint64_t size;			/* Final chunk size */

	uint64_t switch_txn;		/*
					 * Largest transaction that can write
					 * to this chunk, set by a worker
					 * thread when the chunk is switched
					 * out, or by compact to get the most
					 * recent chunk flushed.
					 */

	uint32_t id;			/* ID used to generate URIs */
	uint32_t generation;		/* Merge generation */
	uint32_t refcnt;		/* Number of worker thread references */
	uint32_t bloom_busy;		/* Number of worker thread references */

	int8_t empty;			/* 1/0: checkpoint missing */
	int8_t evicted;			/* 1/0: in-memory chunk was evicted */
	uint8_t flushing;		/* 1/0: chunk flush in progress */

#define	WT_LSM_CHUNK_BLOOM	0x01
#define	WT_LSM_CHUNK_MERGING	0x02
#define	WT_LSM_CHUNK_ONDISK	0x04
#define	WT_LSM_CHUNK_STABLE	0x08
	uint32_t flags;
};

/*
 * Different types of work units. Used by LSM worker threads to choose which
 * type of work they will execute, and by work units to define which action
 * is required.
 */
#define	WT_LSM_WORK_BLOOM	0x01	/* Create a bloom filter */
#define	WT_LSM_WORK_DROP	0x02	/* Drop unused chunks */
#define	WT_LSM_WORK_FLUSH	0x04	/* Flush a chunk to disk */
#define	WT_LSM_WORK_MERGE	0x08	/* Look for a tree merge */
#define	WT_LSM_WORK_SWITCH	0x10	/* Switch to new in-memory chunk */

/*
 * WT_LSM_WORK_UNIT --
 *	A definition of maintenance that an LSM tree needs done.
 */
struct __wt_lsm_work_unit {
	TAILQ_ENTRY(__wt_lsm_work_unit) q;	/* Worker unit queue */
	uint32_t	type;			/* Type of operation */
#define	WT_LSM_WORK_FORCE	0x0001		/* Force operation */
	uint32_t	flags;			/* Flags for operation */
	WT_LSM_TREE *lsm_tree;
};

/*
 * WT_LSM_MANAGER --
 *	A structure that holds resources used to manage any LSM trees in a
 *	database.
 */
struct __wt_lsm_manager {
	/*
	 * Queues of work units for LSM worker threads. We maintain three
	 * queues, to allow us to keep each queue FIFO, rather than needing
	 * to manage the order of work by shuffling the queue order.
	 * One queue for switches - since switches should never wait for other
	 *   work to be done.
	 * One queue for application requested work. For example flushing
	 *   and creating bloom filters.
	 * One queue that is for longer running operations such as merges.
	 */
	TAILQ_HEAD(__wt_lsm_work_switch_qh, __wt_lsm_work_unit)  switchqh;
	TAILQ_HEAD(__wt_lsm_work_app_qh, __wt_lsm_work_unit)	  appqh;
	TAILQ_HEAD(__wt_lsm_work_manager_qh, __wt_lsm_work_unit) managerqh;
	WT_SPINLOCK	switch_lock;	/* Lock for switch queue */
	WT_SPINLOCK	app_lock;	/* Lock for application queue */
	WT_SPINLOCK	manager_lock;	/* Lock for manager queue */
	WT_CONDVAR     *work_cond;	/* Used to notify worker of activity */
	uint32_t	lsm_workers;	/* Current number of LSM workers */
	uint32_t	lsm_workers_max;
#define	WT_LSM_MAX_WORKERS	20
#define	WT_LSM_MIN_WORKERS	3
	WT_LSM_WORKER_ARGS lsm_worker_cookies[WT_LSM_MAX_WORKERS];
};

/*
 * The value aggressive needs to get to before it influences how merges
 * are chosen. The default value translates to enough level 0 chunks being
 * generated to create a second level merge.
 */
#define	WT_LSM_AGGRESSIVE_THRESHOLD	2

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
	int collator_owned;

	uint32_t refcnt;		/* Number of users of the tree */
	WT_SESSION_IMPL *excl_session;	/* Session has exclusive lock */

#define	LSM_TREE_MAX_QUEUE	100
	uint32_t queue_ref;
	WT_RWLOCK *rwlock;
	TAILQ_ENTRY(__wt_lsm_tree) q;

	uint64_t dsk_gen;

	uint64_t ckpt_throttle;		/* Rate limiting due to checkpoints */
	uint64_t merge_throttle;	/* Rate limiting due to merges */
	uint64_t chunk_fill_ms;		/* Estimate of time to fill a chunk */
	struct timespec last_flush_ts;	/* Timestamp last flush finished */
	uint64_t chunks_flushed;	/* Count of chunks flushed since open */
	struct timespec merge_aggressive_ts;/* Timestamp for merge aggression */
	struct timespec work_push_ts;	/* Timestamp last work unit added */
	uint64_t merge_progressing;	/* Bumped when merges are active */
	uint32_t merge_syncing;		/* Bumped when merges are syncing */

	/* Configuration parameters */
	uint32_t bloom_bit_count;
	uint32_t bloom_hash_count;
	uint32_t chunk_count_limit;	/* Limit number of chunks */
	uint64_t chunk_size;
	uint64_t chunk_max;		/* Maximum chunk a merge creates */
	u_int merge_min, merge_max;

#define	WT_LSM_BLOOM_MERGED				0x00000001
#define	WT_LSM_BLOOM_OFF				0x00000002
#define	WT_LSM_BLOOM_OLDEST				0x00000004
	uint32_t bloom;			/* Bloom creation policy */

	WT_LSM_CHUNK **chunk;		/* Array of active LSM chunks */
	size_t chunk_alloc;		/* Space allocated for chunks */
	uint32_t nchunks;		/* Number of active chunks */
	uint32_t last;			/* Last allocated ID */
	bool modified;			/* Have there been updates? */

	WT_LSM_CHUNK **old_chunks;	/* Array of old LSM chunks */
	size_t old_alloc;		/* Space allocated for old chunks */
	u_int nold_chunks;		/* Number of old chunks */
	uint32_t freeing_old_chunks;	/* Whether chunks are being freed */
	uint32_t merge_aggressiveness;	/* Increase amount of work per merge */

	/*
	 * We maintain a set of statistics outside of the normal statistics
	 * area, copying them into place when a statistics cursor is created.
	 */
#define	WT_LSM_TREE_STAT_INCR(session, fld) do {			\
	if (FLD_ISSET(S2C(session)->stat_flags, WT_CONN_STAT_FAST))	\
		++(fld);						\
} while (0)
#define	WT_LSM_TREE_STAT_INCRV(session, fld, v) do {			\
	if (FLD_ISSET(S2C(session)->stat_flags, WT_CONN_STAT_FAST))	\
		(fld) += (int64_t)(v);					\
} while (0)
	int64_t bloom_false_positive;
	int64_t bloom_hit;
	int64_t bloom_miss;
	int64_t lsm_checkpoint_throttle;
	int64_t lsm_lookup_no_bloom;
	int64_t lsm_merge_throttle;

	/*
	 * The tree is open for business. This used to be a flag, but it is
	 * susceptible to races.
	 */
	bool active;

#define	WT_LSM_TREE_AGGRESSIVE_TIMER	0x01	/* Timer for merge aggression */
#define	WT_LSM_TREE_COMPACTING		0x02	/* Tree being compacted */
#define	WT_LSM_TREE_MERGES		0x04	/* Tree should run merges */
#define	WT_LSM_TREE_NEED_SWITCH		0x08	/* New chunk needs creating */
#define	WT_LSM_TREE_OPEN		0x10	/* The tree is open */
#define	WT_LSM_TREE_THROTTLE		0x20	/* Throttle updates */
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
