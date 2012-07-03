/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_PROCESS --
 *	Per-process information for the library.
 */
struct __wt_process {
	WT_SPINLOCK spinlock;		/* Per-process spinlock */

					/* Locked: connection queue */
	TAILQ_HEAD(__wt_connection_impl_qh, __wt_connection_impl) connqh;
};

/*******************************************
 * Implementation of WT_SESSION
 *******************************************/
/*
 * WT_BTREE_SESSION --
 *      Per-session cache of btree handles to avoid synchronization when
 *      opening cursors.
 */
struct __wt_btree_session {
	WT_BTREE *btree;

	TAILQ_ENTRY(__wt_btree_session) q;
};

/*
 * WT_HAZARD --
 *	A hazard reference.
 */
struct __wt_hazard {
	WT_PAGE *page;			/* Page address */
#ifdef HAVE_DIAGNOSTIC
	const char *file;		/* File/line where hazard acquired */
	int	    line;
#endif
};

typedef	enum {
	WT_SERIAL_NONE=0,		/* No request */
	WT_SERIAL_FUNC=1,		/* Function, then return */
	WT_SERIAL_EVICT=2,		/* Function, then schedule evict */
} wq_state_t;

/* Get the connection implementation for a session */
#define	S2C(session) ((WT_CONNECTION_IMPL *)(session)->iface.connection)

/*
 * WT_SESSION_IMPL --
 *	Implementation of WT_SESSION.
 */
struct __wt_session_impl {
	WT_SESSION iface;

	u_int active;			/* Non-zero if the session is in-use */

	WT_CONDVAR *cond;		/* Condition variable */

	const char *name;		/* Name */

	WT_EVENT_HANDLER *event_handler;/* Application's event handlers */

	WT_BTREE *btree;		/* Current file */
	TAILQ_HEAD(__btrees, __wt_btree_session) btrees;

	WT_BTREE *created_btree;	/* File being created */

	WT_CURSOR *cursor;		/* Current cursor */
					/* Cursors closed with the session */
	TAILQ_HEAD(__cursors, __wt_cursor) cursors;

	WT_BTREE *metafile;		/* Metadata file */
	void	*meta_track;		/* Metadata operation tracking */
	void	*meta_track_next;	/* Current position */
	void	*meta_track_sub;	/* Child transaction / save point */
	size_t	 meta_track_alloc;	/* Currently allocated */
#define	WT_META_TRACKING(session)	(session->meta_track_next != NULL)

	TAILQ_HEAD(__tables, __wt_table) tables;

	WT_ITEM	logrec_buf;		/* Buffer for log records */
	WT_ITEM	logprint_buf;		/* Buffer for debug log records */

	WT_ITEM	**scratch;		/* Temporary memory for any function */
	u_int	scratch_alloc;		/* Currently allocated */
#ifdef HAVE_DIAGNOSTIC
	/*
	 * It's hard to figure out from where a buffer was allocated after it's
	 * leaked, so in diagnostic mode we track them; DIAGNOSTIC can't simply
	 * add additional fields to WT_ITEM structures because they are visible
	 * to applications, create a parallel structure instead.
	 */
	struct __wt_scratch_track {
		const char *file;	/* Allocating file, line */
		int line;
	} *scratch_track;
#endif
					/* Serialized operation state */
	void	*wq_args;		/* Operation arguments */
	int	wq_sleeping;		/* Thread is blocked */
	int	wq_ret;			/* Return value */

	WT_TXN	txn;			/* Transaction state */

	void	*reconcile;		/* Reconciliation information */

	WT_REF **excl;			/* Eviction exclusive list */
	u_int	 excl_next;		/* Next empty slot */
	size_t	 excl_allocated;	/* Bytes allocated */

#define	WT_SYNC			1	/* Sync the file */
#define	WT_SYNC_DISCARD		2	/* Sync the file, discard pages */
#define	WT_SYNC_DISCARD_NOWRITE	3	/* Discard the file */
	int syncop;			/* File operation */

	uint32_t id;			/* Offset in conn->session_array */

	uint32_t flags;

	/*
	 * The hazard reference must be placed at the end of the structure: the
	 * structure is cleared when closed, all except the hazard reference.
	 * Putting the hazard reference at the end of the structure allows us to
	 * easily call a function to clear memory up to, but not including, the
	 * hazard reference.
	 */
#define	WT_SESSION_CLEAR(s)	memset(s, 0, WT_PTRDIFF(&(s)->hazard, s))
	WT_HAZARD *hazard;		/* Hazard reference array */
};

/*******************************************
 * Implementation of WT_CONNECTION
 *******************************************/
/*
 * WT_NAMED_COLLATOR --
 *	A collator list entry
 */
struct __wt_named_collator {
	const char *name;		/* Name of collator */
	WT_COLLATOR *collator;	        /* User supplied object */
	TAILQ_ENTRY(__wt_named_collator) q;	/* Linked list of collators */
};

/*
 * WT_NAMED_COMPRESSOR --
 *	A compressor list entry
 */
struct __wt_named_compressor {
	const char *name;		/* Name of compressor */
	WT_COMPRESSOR *compressor;	/* User supplied callbacks */
	TAILQ_ENTRY(__wt_named_compressor) q;	/* Linked list of compressors */
};

/*
 * WT_NAMED_DATA_SOURCE --
 *	A data source list entry
 */
struct __wt_named_data_source {
	const char *prefix;		/* Name of compressor */
	WT_DATA_SOURCE *dsrc;		/* User supplied callbacks */
	TAILQ_ENTRY(__wt_named_data_source) q;	/* Linked list of compressors */
};

/*
 * Allocate some additional slots for internal sessions.  There is a default
 * session for each connection, plus a session for the eviction thread.
 */
#define	WT_NUM_INTERNAL_SESSIONS	2

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
	WT_SPINLOCK fh_lock;		/* File handle queue spinlock */
	WT_SPINLOCK schema_lock;	/* Schema operation spinlock */
	WT_SPINLOCK serial_lock;	/* Serial function call spinlock */

					/* Connection queue */
	TAILQ_ENTRY(__wt_connection_impl) q;

	const char *home;		/* Database home */
	int is_new;			/* Connection created database */

	WT_FH *lock_fh;			/* Lock file handle */

	pthread_t cache_evict_tid;	/* Cache eviction server thread ID */
	pthread_t cache_read_tid;	/* Cache read server thread ID */

					/* Locked: btree list */
	TAILQ_HEAD(__wt_btree_qh, __wt_btree) btqh;
					/* Locked: file list */
	TAILQ_HEAD(__wt_fh_qh, __wt_fh) fhqh;

					/* Locked: library list */
	TAILQ_HEAD(__wt_dlh_qh, __wt_dlh) dlhqh;

	u_int btqcnt;			/* Locked: btree count */
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
	 * WiredTiger allocates space for 15 hazard references in each thread of
	 * control, by default.  There's no code path that requires more than 15
	 * pages at a time (and if we find one, the right change is to increase
	 * the default).
	 */
	uint32_t   hazard_size;		/* Hazard array size */

	WT_CACHE  *cache;		/* Page cache */
	uint64_t   cache_size;

	WT_TXN_GLOBAL txn_global;	/* Global transaction state. */

	WT_CONNECTION_STATS *stats;	/* Connection statistics */

	WT_FH	   *log_fh;		/* Logging file handle */

					/* Locked: collator list */
	TAILQ_HEAD(__wt_coll_qh, __wt_named_collator) collqh;

					/* Locked: compressor list */
	TAILQ_HEAD(__wt_comp_qh, __wt_named_compressor) compqh;

					/* Locked: data source list */
	TAILQ_HEAD(__wt_dsrc_qh, __wt_named_data_source) dsrcqh;

	FILE *msgfile;
	void (*msgcall)(const WT_CONNECTION_IMPL *, const char *);

	/* If non-zero, all buffers used for I/O will be aligned to this. */
	size_t buffer_alignment;

	uint32_t direct_io;
	uint32_t verbose;

	uint32_t flags;
};

/* Standard entry points to the API: declares/initializes local variables. */
#define	API_CONF_DEFAULTS(h, n, cfg)					\
	{ __wt_confdfl_##h##_##n, (cfg), NULL }

#define	API_SESSION_INIT(s, h, n, cur, bt)				\
	WT_BTREE *__oldbtree = (s)->btree;				\
	const char *__oldname = (s)->name;				\
	(s)->cursor = (cur);						\
	(s)->btree = (bt);						\
	(s)->name = #h "." #n;

#define	API_CALL_NOCONF(s, h, n, cur, bt) do {				\
	API_SESSION_INIT(s, h, n, cur, bt);

#define	API_CALL(s, h, n, cur, bt, cfg, cfgvar) do {			\
	const char *cfgvar[] = API_CONF_DEFAULTS(h, n, cfg);		\
	API_SESSION_INIT(s, h, n, cur, bt);				\
	WT_ERR(((cfg) != NULL) ?					\
	    __wt_config_check((s), __wt_confchk_##h##_##n, (cfg)) : 0)

#define	API_END(s)							\
	if ((s) != NULL) {						\
		(s)->btree = __oldbtree;				\
		(s)->name = __oldname;					\
	}								\
} while (0)

/* If an error is returned, mark that the transaction requires abort. */
#define	API_END_TXN_ERROR(s, ret)					\
	API_END(s);							\
	if ((ret) != 0 && (ret) != WT_NOTFOUND && (ret) != WT_DUPLICATE_KEY) \
		F_SET(&(s)->txn, TXN_ERROR)

/*
 * If a session or connection method is about to return WT_NOTFOUND (some
 * underlying object was not found), map it to ENOENT, only cursor methods
 * return WT_NOTFOUND.
 */
#define	API_END_NOTFOUND_MAP(s, ret)					\
	API_END(s);							\
	return ((ret) == WT_NOTFOUND ? ENOENT : (ret))

#define	CONNECTION_API_CALL(conn, s, n, cfg, cfgvar)			\
	s = (conn)->default_session;					\
	API_CALL(s, connection, n, NULL, NULL, cfg, cfgvar);		\

#define	SESSION_API_CALL(s, n, cfg, cfgvar)				\
	API_CALL(s, session, n, NULL, NULL, cfg, cfgvar);

#define	CURSOR_API_CALL(cur, s, n, bt, cfg, cfgvar)			\
	(s) = (WT_SESSION_IMPL *)(cur)->session;			\
	API_CALL(s, cursor, n, cur, bt, cfg, cfgvar)

#define	CURSOR_API_CALL_NOCONF(cur, s, n, bt)				\
	(s) = (WT_SESSION_IMPL *)(cur)->session;			\
	API_CALL_NOCONF(s, cursor, n, cur, bt);				\

/*******************************************
 * Global variables.
 *******************************************/
extern WT_EVENT_HANDLER *__wt_event_handler_default;
extern WT_EVENT_HANDLER *__wt_event_handler_verbose;
extern WT_PROCESS __wt_process;

/*
 * DO NOT EDIT: automatically built by dist/api_flags.py.
 * API flags section: BEGIN
 */
#define	WT_CONN_NOSYNC					0x00000004
#define	WT_CONN_TRANSACTIONAL				0x00000002
#define	WT_DIRECTIO_DATA				0x00000002
#define	WT_DIRECTIO_LOG					0x00000001
#define	WT_PAGE_FREE_IGNORE_DISK			0x00000001
#define	WT_REC_SINGLE					0x00000001
#define	WT_SERVER_RUN					0x00000001
#define	WT_SESSION_INTERNAL				0x00000004
#define	WT_SESSION_SALVAGE_QUIET_ERR			0x00000002
#define	WT_SESSION_SCHEMA_LOCKED			0x00000001
#define	WT_VERB_block					0x00001000
#define	WT_VERB_ckpt					0x00000800
#define	WT_VERB_evict					0x00000400
#define	WT_VERB_evictserver				0x00000200
#define	WT_VERB_fileops					0x00000100
#define	WT_VERB_hazard					0x00000080
#define	WT_VERB_mutex					0x00000040
#define	WT_VERB_read					0x00000020
#define	WT_VERB_readserver				0x00000010
#define	WT_VERB_reconcile				0x00000008
#define	WT_VERB_salvage					0x00000004
#define	WT_VERB_verify					0x00000002
#define	WT_VERB_write					0x00000001
/*
 * API flags section: END
 * DO NOT EDIT: automatically built by dist/api_flags.py.
 */
