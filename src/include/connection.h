/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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
};
extern WT_PROCESS __wt_process;

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
 * Allocate some additional slots for internal sessions.  There is a default
 * session for each connection, plus a session for the eviction thread.
 */
#define	WT_NUM_INTERNAL_SESSIONS	2

/*
 * Periodically clear out unused dhandles from the connection list.
 */
#define	CONN_DHANDLE_SWEEP_PERIOD	10

/*
 * WT_CONNECTION_IMPL --
 *	Implementation of WT_CONNECTION
 */
struct __wt_connection_impl {
	WT_CONNECTION iface;

	/* For operations without an application-supplied session */
	WT_SESSION_IMPL *default_session;
	WT_SESSION_IMPL  dummy_session;

	WT_SPINLOCK api_lock;		/* Connection API spinlock */
	WT_SPINLOCK checkpoint_lock;	/* Checkpoint spinlock */
	WT_SPINLOCK fh_lock;		/* File handle queue spinlock */
	WT_SPINLOCK schema_lock;	/* Schema operation spinlock */
	WT_SPINLOCK serial_lock;	/* Serial function call spinlock */

					/* Connection queue */
	TAILQ_ENTRY(__wt_connection_impl) q;
					/* Cache pool queue */
	TAILQ_ENTRY(__wt_connection_impl) cpq;

	const char *home;		/* Database home */
	const char *error_prefix;	/* Database error prefix */
	int is_new;			/* Connection created database */

	int connection_initialized;	/* Connection is initialized */

	WT_EXTENSION_API extension_api;	/* Extension API */

					/* Configuration */
	const WT_CONFIG_ENTRY **config_entries;

	void  **foc;			/* Free-on-close array */
	size_t  foc_cnt;		/* Array entries */
	size_t  foc_size;		/* Array size */

	WT_FH *lock_fh;			/* Lock file handle */

	pthread_t cache_evict_tid;	/* Eviction server thread ID */
	int	  cache_evict_tid_set;	/* Eviction server thread ID set */

	int32_t	dhandle_dead;		/* Not locked: dead dhandles seen */
	WT_SPINLOCK dhandle_lock;	/* Locked: dhandle sweep */
					/* Locked: data handle list */
	TAILQ_HEAD(__wt_dhandle_qh, __wt_data_handle) dhqh;
					/* Locked: LSM handle list. */
	TAILQ_HEAD(__wt_lsm_qh, __wt_lsm_tree) lsmqh;
					/* Locked: file list */
	TAILQ_HEAD(__wt_fh_qh, __wt_fh) fhqh;
					/* Locked: library list */
	TAILQ_HEAD(__wt_dlh_qh, __wt_dlh) dlhqh;

	WT_SPINLOCK block_lock;		/* Locked: block manager list */
	TAILQ_HEAD(__wt_block_qh, __wt_block) blockqh;

	u_int open_btree_count;		/* Locked: open writable btree count */
	u_int next_file_id;		/* Locked: file ID counter */

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

	/*
	 * WiredTiger allocates space for a fixed number of hazard pointers
	 * in each thread of control.
	 */
	uint32_t   hazard_max;		/* Hazard array size */

	WT_CACHE  *cache;		/* Page cache */
	uint64_t   cache_size;

	WT_TXN_GLOBAL txn_global;	/* Global transaction state */

	WT_SPINLOCK hot_backup_lock;	/* Hot backup serialization */
	int hot_backup;

	WT_SESSION_IMPL *ckpt_session;	/* Checkpoint thread session */
	pthread_t	 ckpt_tid;	/* Checkpoint thread */
	int		 ckpt_tid_set;	/* Checkpoint thread set */
	WT_CONDVAR	*ckpt_cond;	/* Checkpoint wait mutex */
	const char	*ckpt_config;	/* Checkpoint configuration */
	long		 ckpt_usecs;	/* Checkpoint period */

	WT_CONNECTION_STATS stats;	/* Connection statistics */
	int		 statistics;	/* Global statistics configuration */
	WT_SESSION_IMPL *stat_session;	/* Statistics log session */
	pthread_t	 stat_tid;	/* Statistics log thread */
	int		 stat_tid_set;	/* Statistics log thread set */
	WT_CONDVAR	*stat_cond;	/* Statistics log wait mutex */

	int		 stat_clear;	/* Statistics log clear */
	const char	*stat_format;	/* Statistics log timestamp format */
	FILE		*stat_fp;	/* Statistics log file handle */
	const char	*stat_path;	/* Statistics log path format */
	char	       **stat_sources;	/* Statistics log list of objects */
	const char	*stat_stamp;	/* Statistics log entry timestamp */
	long		 stat_usecs;	/* Statistics log period */

	int		 logging;	/* Global logging configuration */
	int		 archive;	/* Global archive configuration */
	WT_CONDVAR	*arch_cond;	/* Log archive wait mutex */
	WT_SESSION_IMPL *arch_session;	/* Log archive session */
	pthread_t	 arch_tid;	/* Log archive thread */
	int		 arch_tid_set;	/* Log archive thread set */
	WT_LOG		*log;		/* Logging structure */
	off_t		log_file_max;	/* Log file max size */
	const char	*log_path;	/* Logging path format */

					/* Locked: collator list */
	TAILQ_HEAD(__wt_coll_qh, __wt_named_collator) collqh;

					/* Locked: compressor list */
	TAILQ_HEAD(__wt_comp_qh, __wt_named_compressor) compqh;

					/* Locked: data source list */
	TAILQ_HEAD(__wt_dsrc_qh, __wt_named_data_source) dsrcqh;

	/* If non-zero, all buffers used for I/O will be aligned to this. */
	size_t buffer_alignment;

	uint32_t schema_gen;		/* Schema generation number */

	off_t	 data_extend_len;	/* file_extend data length */
	off_t	 log_extend_len;	/* file_extend log length */

	uint32_t direct_io;		/* O_DIRECT file type flags */
	int	 mmap;			/* mmap configuration */
	uint32_t verbose;

	uint32_t flags;
};
