/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*******************************************
 * Global per-process structure.
 *******************************************/
/*
 * WT_PROCESS --
 *	Per-process information for the library.
 */
struct __wt_process {
	WT_SPINLOCK spinlock;		/* Per-process spinlock */

					/* Locked: connection queue */
	TAILQ_HEAD(__wt_connection_impl_qh, __wt_connection_impl) connqh;
	WT_CACHE_POOL *cache_pool;
#define	WT_TSC_DEFAULT_RATIO	1.0
	double	 tsc_nsec_ratio;	/* rdtsc ticks to nanoseconds */
	bool use_epochtime;		/* use expensive time */

					/* Checksum function */
#define	__wt_checksum(chunk, len)	__wt_process.checksum(chunk, len)
	uint32_t (*checksum)(const void *, size_t);
};
extern WT_PROCESS __wt_process;

/*
 * WT_KEYED_ENCRYPTOR --
 *	An list entry for an encryptor with a unique (name, keyid).
 */
struct __wt_keyed_encryptor {
	const char *keyid;		/* Key id of encryptor */
	int owned;			/* Encryptor needs to be terminated */
	size_t size_const;		/* The result of the sizing callback */
	WT_ENCRYPTOR *encryptor;	/* User supplied callbacks */
					/* Linked list of encryptors */
	TAILQ_ENTRY(__wt_keyed_encryptor) hashq;
	TAILQ_ENTRY(__wt_keyed_encryptor) q;
};

/*
 * WT_NAMED_COLLATOR --
 *	A collator list entry
 */
struct __wt_named_collator {
	const char *name;		/* Name of collator */
	WT_COLLATOR *collator;		/* User supplied object */
	TAILQ_ENTRY(__wt_named_collator) q;	/* Linked list of collators */
};

/*
 * WT_NAMED_COMPRESSOR --
 *	A compressor list entry
 */
struct __wt_named_compressor {
	const char *name;		/* Name of compressor */
	WT_COMPRESSOR *compressor;	/* User supplied callbacks */
					/* Linked list of compressors */
	TAILQ_ENTRY(__wt_named_compressor) q;
};

/*
 * WT_NAMED_DATA_SOURCE --
 *	A data source list entry
 */
struct __wt_named_data_source {
	const char *prefix;		/* Name of data source */
	WT_DATA_SOURCE *dsrc;		/* User supplied callbacks */
					/* Linked list of data sources */
	TAILQ_ENTRY(__wt_named_data_source) q;
};

/*
 * WT_NAMED_ENCRYPTOR --
 *	An encryptor list entry
 */
struct __wt_named_encryptor {
	const char *name;		/* Name of encryptor */
	WT_ENCRYPTOR *encryptor;	/* User supplied callbacks */
					/* Locked: list of encryptors by key */
	TAILQ_HEAD(__wt_keyedhash, __wt_keyed_encryptor)
				keyedhashqh[WT_HASH_ARRAY_SIZE];
	TAILQ_HEAD(__wt_keyed_qh, __wt_keyed_encryptor) keyedqh;
					/* Linked list of encryptors */
	TAILQ_ENTRY(__wt_named_encryptor) q;
};

/*
 * WT_NAMED_EXTRACTOR --
 *	An extractor list entry
 */
struct __wt_named_extractor {
	const char *name;		/* Name of extractor */
	WT_EXTRACTOR *extractor;		/* User supplied object */
	TAILQ_ENTRY(__wt_named_extractor) q;	/* Linked list of extractors */
};

/*
 * WT_CONN_CHECK_PANIC --
 *	Check if we've panicked and return the appropriate error.
 */
#define	WT_CONN_CHECK_PANIC(conn)					\
	(F_ISSET(conn, WT_CONN_PANIC) ? WT_PANIC : 0)
#define	WT_SESSION_CHECK_PANIC(session)					\
	WT_CONN_CHECK_PANIC(S2C(session))

/*
 * Macros to ensure the dhandle is inserted or removed from both the
 * main queue and the hashed queue.
 */
#define	WT_CONN_DHANDLE_INSERT(conn, dhandle, bucket) do {		\
	WT_ASSERT(session,						\
	    F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST_WRITE));	\
	TAILQ_INSERT_HEAD(&(conn)->dhqh, dhandle, q);			\
	TAILQ_INSERT_HEAD(&(conn)->dhhash[bucket], dhandle, hashq);	\
	++(conn)->dhandle_count;					\
} while (0)

#define	WT_CONN_DHANDLE_REMOVE(conn, dhandle, bucket) do {		\
	WT_ASSERT(session,						\
	    F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST_WRITE));	\
	TAILQ_REMOVE(&(conn)->dhqh, dhandle, q);			\
	TAILQ_REMOVE(&(conn)->dhhash[bucket], dhandle, hashq);		\
	--(conn)->dhandle_count;					\
} while (0)

/*
 * Macros to ensure the block is inserted or removed from both the
 * main queue and the hashed queue.
 */
#define	WT_CONN_BLOCK_INSERT(conn, block, bucket) do {			\
	TAILQ_INSERT_HEAD(&(conn)->blockqh, block, q);			\
	TAILQ_INSERT_HEAD(&(conn)->blockhash[bucket], block, hashq);	\
} while (0)

#define	WT_CONN_BLOCK_REMOVE(conn, block, bucket) do {			\
	TAILQ_REMOVE(&(conn)->blockqh, block, q);			\
	TAILQ_REMOVE(&(conn)->blockhash[bucket], block, hashq);		\
} while (0)

/*
 * WT_CONNECTION_IMPL --
 *	Implementation of WT_CONNECTION
 */
struct __wt_connection_impl {
	WT_CONNECTION iface;

	/* For operations without an application-supplied session */
	WT_SESSION_IMPL *default_session;
	WT_SESSION_IMPL  dummy_session;

	const char *cfg;		/* Connection configuration */

	WT_SPINLOCK api_lock;		/* Connection API spinlock */
	WT_SPINLOCK checkpoint_lock;	/* Checkpoint spinlock */
	WT_SPINLOCK fh_lock;		/* File handle queue spinlock */
	WT_SPINLOCK metadata_lock;	/* Metadata update spinlock */
	WT_SPINLOCK reconfig_lock;	/* Single thread reconfigure */
	WT_SPINLOCK schema_lock;	/* Schema operation spinlock */
	WT_RWLOCK table_lock;		/* Table list lock */
	WT_SPINLOCK turtle_lock;	/* Turtle file spinlock */
	WT_RWLOCK dhandle_lock;		/* Data handle list lock */

					/* Connection queue */
	TAILQ_ENTRY(__wt_connection_impl) q;
					/* Cache pool queue */
	TAILQ_ENTRY(__wt_connection_impl) cpq;

	const char *home;		/* Database home */
	const char *error_prefix;	/* Database error prefix */
	int is_new;			/* Connection created database */

	uint16_t compat_major;		/* Compatibility major version */
	uint16_t compat_minor;		/* Compatibility minor version */
#define	WT_CONN_COMPAT_NONE	UINT16_MAX
	uint16_t req_max_major;		/* Compatibility maximum major */
	uint16_t req_max_minor;		/* Compatibility maximum minor */
	uint16_t req_min_major;		/* Compatibility minimum major */
	uint16_t req_min_minor;		/* Compatibility minimum minor */

	WT_EXTENSION_API extension_api;	/* Extension API */

					/* Configuration */
	const WT_CONFIG_ENTRY **config_entries;

	const char *optrack_path;	/* Directory for operation logs */
	WT_FH *optrack_map_fh;		/* Name to id translation file. */
	WT_SPINLOCK optrack_map_spinlock; /* Translation file spinlock. */
	uintmax_t optrack_pid;		/* Cache the process ID. */

	void  **foc;			/* Free-on-close array */
	size_t  foc_cnt;		/* Array entries */
	size_t  foc_size;		/* Array size */

	WT_FH *lock_fh;			/* Lock file handle */

	/*
	 * The connection keeps a cache of data handles. The set of handles
	 * can grow quite large so we maintain both a simple list and a hash
	 * table of lists. The hash table key is based on a hash of the table
	 * URI.
	 */
					/* Locked: data handle hash array */
	TAILQ_HEAD(__wt_dhhash, __wt_data_handle) dhhash[WT_HASH_ARRAY_SIZE];
					/* Locked: data handle list */
	TAILQ_HEAD(__wt_dhandle_qh, __wt_data_handle) dhqh;
					/* Locked: LSM handle list. */
	TAILQ_HEAD(__wt_lsm_qh, __wt_lsm_tree) lsmqh;
					/* Locked: file list */
	TAILQ_HEAD(__wt_fhhash, __wt_fh) fhhash[WT_HASH_ARRAY_SIZE];
	TAILQ_HEAD(__wt_fh_qh, __wt_fh) fhqh;
					/* Locked: library list */
	TAILQ_HEAD(__wt_dlh_qh, __wt_dlh) dlhqh;

	WT_SPINLOCK block_lock;		/* Locked: block manager list */
	TAILQ_HEAD(__wt_blockhash, __wt_block) blockhash[WT_HASH_ARRAY_SIZE];
	TAILQ_HEAD(__wt_block_qh, __wt_block) blockqh;

	u_int dhandle_count;		/* Locked: handles in the queue */
	u_int open_btree_count;		/* Locked: open writable btree count */
	uint32_t next_file_id;		/* Locked: file ID counter */
	uint32_t open_file_count;	/* Atomic: open file handle count */
	uint32_t open_cursor_count;	/* Atomic: open cursor handle count */

	/*
	 * WiredTiger allocates space for 50 simultaneous sessions (threads of
	 * control) by default.  Growing the number of threads dynamically is
	 * possible, but tricky since server threads are walking the array
	 * without locking it.
	 *
	 * There's an array of WT_SESSION_IMPL pointers that reference the
	 * allocated array; we do it that way because we want an easy way for
	 * the server thread code to avoid walking the entire array when only a
	 * few threads are running.
	 */
	WT_SESSION_IMPL	*sessions;	/* Session reference */
	uint32_t	 session_size;	/* Session array size */
	uint32_t	 session_cnt;	/* Session count */

	size_t     session_scratch_max;	/* Max scratch memory per session */

	WT_CACHE  *cache;		/* Page cache */
	volatile uint64_t cache_size;	/* Cache size (either statically
					   configured or the current size
					   within a cache pool). */

	WT_TXN_GLOBAL txn_global;	/* Global transaction state */

	WT_RWLOCK hot_backup_lock;	/* Hot backup serialization */
	bool hot_backup;		/* Hot backup in progress */
	char **hot_backup_list;		/* Hot backup file list */

	WT_SESSION_IMPL *ckpt_session;	/* Checkpoint thread session */
	wt_thread_t	 ckpt_tid;	/* Checkpoint thread */
	bool		 ckpt_tid_set;	/* Checkpoint thread set */
	WT_CONDVAR	*ckpt_cond;	/* Checkpoint wait mutex */
#define	WT_CKPT_LOGSIZE(conn)	((conn)->ckpt_logsize != 0)
	wt_off_t	 ckpt_logsize;	/* Checkpoint log size period */
	bool		 ckpt_signalled;/* Checkpoint signalled */

	uint64_t  ckpt_usecs;		/* Checkpoint timer */
	uint64_t  ckpt_time_max;	/* Checkpoint time min/max */
	uint64_t  ckpt_time_min;
	uint64_t  ckpt_time_recent;	/* Checkpoint time recent/total */
	uint64_t  ckpt_time_total;

	/* Checkpoint stats and verbosity timers */
	struct timespec ckpt_timer_start;
	struct timespec ckpt_timer_scrub_end;

	/* Checkpoint progress message data */
	uint64_t ckpt_progress_msg_count;
	uint64_t ckpt_write_bytes;
	uint64_t ckpt_write_pages;

	uint32_t stat_flags;		/* Options declared in flags.py */

					/* Connection statistics */
	WT_CONNECTION_STATS *stats[WT_COUNTER_SLOTS];
	WT_CONNECTION_STATS *stat_array;

	WT_ASYNC	*async;		/* Async structure */
	bool		 async_cfg;	/* Global async configuration */
	uint32_t	 async_size;	/* Async op array size */
	uint32_t	 async_workers;	/* Number of async workers */

	WT_LSM_MANAGER	lsm_manager;	/* LSM worker thread information */

	WT_KEYED_ENCRYPTOR *kencryptor;	/* Encryptor for metadata and log */

	bool		 evict_server_running;/* Eviction server operating */

	WT_THREAD_GROUP  evict_threads;
	uint32_t	 evict_threads_max;/* Max eviction threads */
	uint32_t	 evict_threads_min;/* Min eviction threads */

#define	WT_STATLOG_FILENAME	"WiredTigerStat.%d.%H"
	WT_SESSION_IMPL *stat_session;	/* Statistics log session */
	wt_thread_t	 stat_tid;	/* Statistics log thread */
	bool		 stat_tid_set;	/* Statistics log thread set */
	WT_CONDVAR	*stat_cond;	/* Statistics log wait mutex */
	const char	*stat_format;	/* Statistics log timestamp format */
	WT_FSTREAM	*stat_fs;	/* Statistics log stream */
	/* Statistics log json table printing state flag */
	bool		 stat_json_tables;
	char		*stat_path;	/* Statistics log path format */
	char	       **stat_sources;	/* Statistics log list of objects */
	const char	*stat_stamp;	/* Statistics log entry timestamp */
	uint64_t	 stat_usecs;	/* Statistics log period */

/* AUTOMATIC FLAG VALUE GENERATION START */
#define	WT_CONN_LOG_ARCHIVE		0x001u	/* Archive is enabled */
#define	WT_CONN_LOG_DOWNGRADED		0x002u	/* Running older version */
#define	WT_CONN_LOG_ENABLED		0x004u	/* Logging is enabled */
#define	WT_CONN_LOG_EXISTED		0x008u	/* Log files found */
#define	WT_CONN_LOG_FORCE_DOWNGRADE	0x010u	/* Force downgrade */
#define	WT_CONN_LOG_RECOVER_DIRTY	0x020u	/* Recovering unclean */
#define	WT_CONN_LOG_RECOVER_DONE	0x040u	/* Recovery completed */
#define	WT_CONN_LOG_RECOVER_ERR		0x080u	/* Error if recovery required */
#define	WT_CONN_LOG_ZERO_FILL		0x100u	/* Manually zero files */
/* AUTOMATIC FLAG VALUE GENERATION STOP */
	uint32_t	 log_flags;	/* Global logging configuration */
	WT_CONDVAR	*log_cond;	/* Log server wait mutex */
	WT_SESSION_IMPL *log_session;	/* Log server session */
	wt_thread_t	 log_tid;	/* Log server thread */
	bool		 log_tid_set;	/* Log server thread set */
	WT_CONDVAR	*log_file_cond;	/* Log file thread wait mutex */
	WT_SESSION_IMPL *log_file_session;/* Log file thread session */
	wt_thread_t	 log_file_tid;	/* Log file thread */
	bool		 log_file_tid_set;/* Log file thread set */
	WT_CONDVAR	*log_wrlsn_cond;/* Log write lsn thread wait mutex */
	WT_SESSION_IMPL *log_wrlsn_session;/* Log write lsn thread session */
	wt_thread_t	 log_wrlsn_tid;	/* Log write lsn thread */
	bool		 log_wrlsn_tid_set;/* Log write lsn thread set */
	WT_LOG		*log;		/* Logging structure */
	WT_COMPRESSOR	*log_compressor;/* Logging compressor */
	uint32_t	 log_cursors;	/* Log cursor count */
	wt_off_t	 log_file_max;	/* Log file max size */
	const char	*log_path;	/* Logging path format */
	uint32_t	 log_prealloc;	/* Log file pre-allocation */
	uint16_t	 log_req_max;	/* Max required log version */
	uint16_t	 log_req_min;	/* Min required log version */
	uint32_t	 txn_logsync;	/* Log sync configuration */

	WT_SESSION_IMPL *meta_ckpt_session;/* Metadata checkpoint session */

	/*
	 * Is there a data/schema change that needs to be the part of a
	 * checkpoint.
	 */
	bool modified;

	WT_SESSION_IMPL *sweep_session;	   /* Handle sweep session */
	wt_thread_t	 sweep_tid;	   /* Handle sweep thread */
	int		 sweep_tid_set;	   /* Handle sweep thread set */
	WT_CONDVAR      *sweep_cond;	   /* Handle sweep wait mutex */
	uint64_t         sweep_idle_time;  /* Handle sweep idle time */
	uint64_t         sweep_interval;   /* Handle sweep interval */
	uint64_t         sweep_handles_min;/* Handle sweep minimum open */

	/* Set of btree IDs not being rolled back */
	uint8_t *stable_rollback_bitstring;
	uint32_t stable_rollback_maxfile;

					/* Locked: collator list */
	TAILQ_HEAD(__wt_coll_qh, __wt_named_collator) collqh;

					/* Locked: compressor list */
	TAILQ_HEAD(__wt_comp_qh, __wt_named_compressor) compqh;

					/* Locked: data source list */
	TAILQ_HEAD(__wt_dsrc_qh, __wt_named_data_source) dsrcqh;

					/* Locked: encryptor list */
	WT_SPINLOCK encryptor_lock;	/* Encryptor list lock */
	TAILQ_HEAD(__wt_encrypt_qh, __wt_named_encryptor) encryptqh;

					/* Locked: extractor list */
	TAILQ_HEAD(__wt_extractor_qh, __wt_named_extractor) extractorqh;

	void	*lang_private;		/* Language specific private storage */

	/* If non-zero, all buffers used for I/O will be aligned to this. */
	size_t buffer_alignment;

	uint64_t stashed_bytes;		/* Atomic: stashed memory statistics */
	uint64_t stashed_objects;
					/* Generations manager */
	volatile uint64_t generations[WT_GENERATIONS];

	wt_off_t data_extend_len;	/* file_extend data length */
	wt_off_t log_extend_len;	/* file_extend log length */

/* AUTOMATIC FLAG VALUE GENERATION START */
#define	WT_DIRECT_IO_CHECKPOINT	0x1u	/* Checkpoints */
#define	WT_DIRECT_IO_DATA	0x2u	/* Data files */
#define	WT_DIRECT_IO_LOG	0x4u	/* Log files */
/* AUTOMATIC FLAG VALUE GENERATION STOP */
	uint64_t direct_io;		/* O_DIRECT, FILE_FLAG_NO_BUFFERING */
	uint64_t write_through;		/* FILE_FLAG_WRITE_THROUGH */

	bool	 mmap;			/* mmap configuration */
	int page_size;			/* OS page size for mmap alignment */

/* AUTOMATIC FLAG VALUE GENERATION START */
#define	WT_VERB_API			0x000000001u
#define	WT_VERB_BLOCK			0x000000002u
#define	WT_VERB_CHECKPOINT		0x000000004u
#define	WT_VERB_CHECKPOINT_PROGRESS	0x000000008u
#define	WT_VERB_COMPACT			0x000000010u
#define	WT_VERB_ERROR_RETURNS		0x000000020u
#define	WT_VERB_EVICT			0x000000040u
#define	WT_VERB_EVICTSERVER		0x000000080u
#define	WT_VERB_EVICT_STUCK		0x000000100u
#define	WT_VERB_FILEOPS			0x000000200u
#define	WT_VERB_HANDLEOPS		0x000000400u
#define	WT_VERB_LOG			0x000000800u
#define	WT_VERB_LOOKASIDE		0x000001000u
#define	WT_VERB_LOOKASIDE_ACTIVITY	0x000002000u
#define	WT_VERB_LSM			0x000004000u
#define	WT_VERB_LSM_MANAGER		0x000008000u
#define	WT_VERB_METADATA		0x000010000u
#define	WT_VERB_MUTEX			0x000020000u
#define	WT_VERB_OVERFLOW		0x000040000u
#define	WT_VERB_READ			0x000080000u
#define	WT_VERB_REBALANCE		0x000100000u
#define	WT_VERB_RECONCILE		0x000200000u
#define	WT_VERB_RECOVERY		0x000400000u
#define	WT_VERB_RECOVERY_PROGRESS	0x000800000u
#define	WT_VERB_SALVAGE			0x001000000u
#define	WT_VERB_SHARED_CACHE		0x002000000u
#define	WT_VERB_SPLIT			0x004000000u
#define	WT_VERB_TEMPORARY		0x008000000u
#define	WT_VERB_THREAD_GROUP		0x010000000u
#define	WT_VERB_TIMESTAMP		0x020000000u
#define	WT_VERB_TRANSACTION		0x040000000u
#define	WT_VERB_VERIFY			0x080000000u
#define	WT_VERB_VERSION			0x100000000u
#define	WT_VERB_WRITE			0x200000000u
/* AUTOMATIC FLAG VALUE GENERATION STOP */
	uint64_t verbose;

	/*
	 * Variable with flags for which subsystems the diagnostic stress timing
	 * delays have been requested.
	 */
/* AUTOMATIC FLAG VALUE GENERATION START */
#define	WT_TIMING_STRESS_CHECKPOINT_SLOW	0x001u
#define	WT_TIMING_STRESS_LOOKASIDE_SWEEP	0x002u
#define	WT_TIMING_STRESS_SPLIT_1		0x004u
#define	WT_TIMING_STRESS_SPLIT_2		0x008u
#define	WT_TIMING_STRESS_SPLIT_3		0x010u
#define	WT_TIMING_STRESS_SPLIT_4		0x020u
#define	WT_TIMING_STRESS_SPLIT_5		0x040u
#define	WT_TIMING_STRESS_SPLIT_6		0x080u
#define	WT_TIMING_STRESS_SPLIT_7		0x100u
#define	WT_TIMING_STRESS_SPLIT_8		0x200u
/* AUTOMATIC FLAG VALUE GENERATION STOP */
	uint64_t timing_stress_flags;

#define	WT_STDERR(s)	(&S2C(s)->wt_stderr)
#define	WT_STDOUT(s)	(&S2C(s)->wt_stdout)
	WT_FSTREAM wt_stderr, wt_stdout;

	/*
	 * File system interface abstracted to support alternative file system
	 * implementations.
	 */
	WT_FILE_SYSTEM *file_system;

/* AUTOMATIC FLAG VALUE GENERATION START */
#define	WT_CONN_CACHE_CURSORS		0x0000001u
#define	WT_CONN_CACHE_POOL		0x0000002u
#define	WT_CONN_CKPT_SYNC		0x0000004u
#define	WT_CONN_CLOSING			0x0000008u
#define	WT_CONN_CLOSING_NO_MORE_OPENS	0x0000010u
#define	WT_CONN_CLOSING_TIMESTAMP	0x0000020u
#define	WT_CONN_COMPATIBILITY		0x0000040u
#define	WT_CONN_DATA_CORRUPTION		0x0000080u
#define	WT_CONN_EVICTION_NO_LOOKASIDE	0x0000100u
#define	WT_CONN_EVICTION_RUN		0x0000200u
#define	WT_CONN_IN_MEMORY		0x0000400u
#define	WT_CONN_LEAK_MEMORY		0x0000800u
#define	WT_CONN_LOOKASIDE_OPEN		0x0001000u
#define	WT_CONN_LSM_MERGE		0x0002000u
#define	WT_CONN_OPTRACK			0x0004000u
#define	WT_CONN_PANIC			0x0008000u
#define	WT_CONN_READONLY		0x0010000u
#define	WT_CONN_RECOVERING		0x0020000u
#define	WT_CONN_SALVAGE			0x0040000u
#define	WT_CONN_SERVER_ASYNC		0x0080000u
#define	WT_CONN_SERVER_CHECKPOINT	0x0100000u
#define	WT_CONN_SERVER_LOG		0x0200000u
#define	WT_CONN_SERVER_LSM		0x0400000u
#define	WT_CONN_SERVER_STATISTICS	0x0800000u
#define	WT_CONN_SERVER_SWEEP		0x1000000u
#define	WT_CONN_WAS_BACKUP		0x2000000u
/* AUTOMATIC FLAG VALUE GENERATION STOP */
	uint32_t flags;
};
