/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * WT_PROCESS --
 *	Per-process information for the library.
 */
struct __wt_process {
	WT_MTX *mtx;			/* Per-process mutex */

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
 * WT_SESSION_BUFFER --
 *	A structure to accumulate file changes on a per-thread basis.
 */
struct __wt_session_buffer {
	uint32_t len;			/* Buffer original size */
	uint32_t space_avail;		/* Buffer's available memory */
	uint8_t *first_free;		/* Buffer's first free byte */

	uint32_t in;			/* Buffer chunks in use */
	uint32_t out;			/* Buffer chunks not in use */
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
	WT_WORKQ_NONE=0,		/* No request */
	WT_WORKQ_FUNC=1,		/* Function, then return */
	WT_WORKQ_EVICT=2,		/* Function, then schedule evict */
	WT_WORKQ_EVICT_SCHED=3,		/* Waiting on evict to complete */
	WT_WORKQ_READ=4,		/* Function, then schedule read */
	WT_WORKQ_READ_SCHED=5		/* Waiting on read to complete */
} wq_state_t;

/* Get the connection implementation for a session. */
#define	S2C(session) ((WT_CONNECTION_IMPL *)(session)->iface.connection)

/*
 * WT_SESSION_IMPL --
 *	Implementation of WT_SESSION.
 */
struct __wt_session_impl {
	WT_SESSION iface;

	WT_MTX	 *mtx;			/* Blocking mutex */

	const char *name;		/* Name */
	WT_EVENT_HANDLER *event_handler;

	WT_BTREE *btree;		/* Current file */
	TAILQ_HEAD(__btrees, __wt_btree_session) btrees;

	WT_CURSOR *cursor;		/* Current cursor */
					/* All file cursors */
	TAILQ_HEAD(__file_cursors, wt_cursor) file_cursors;
					/* Cursors closed with the session */
	TAILQ_HEAD(__public_cursors, wt_cursor) public_cursors;

	WT_BTREE *schematab;		/* Schema tables */
	TAILQ_HEAD(__tables, __wt_table) tables;

	WT_BUF	logrec_buf;		/* Buffer for log records */
	WT_BUF	logprint_buf;		/* Buffer for debug log records */

	WT_BUF	**scratch;		/* Temporary memory for any function */
	u_int	 scratch_alloc;		/* Currently allocated */

					/* WT_SESSION_IMPL workQ request */
	wq_state_t volatile wq_state;	/* Request state */
	void    (*wq_func)		/* Function */
		    (WT_SESSION_IMPL *);
	void	 *wq_args;		/* Function argument */
	int	  wq_sleeping;		/* Thread is blocked */
	int	  wq_ret;		/* Return value */

	WT_HAZARD *hazard;		/* Hazard reference array */

	WT_SESSION_BUFFER *sb;		/* Per-thread update buffer */
	uint32_t update_alloc_size;	/* Allocation size */

	uint32_t flags;
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
 * WT_CONNECTION_IMPL --
 *	Implementation of WT_CONNECTION
 */
struct __wt_connection_impl {
	WT_CONNECTION iface;

	WT_SESSION_IMPL default_session;/* For operations without an
					   application-supplied session. */

	WT_SPINLOCK spinlock;		/* Connection spinlock */
					/* Connection queue */
	TAILQ_ENTRY(__wt_connection_impl) q;

	const char *home;		/* Database home */
	int is_new;			/* Connection created database */

	WT_FH *lock_fh;			/* Lock file handle */

	WT_SPINLOCK workq_lock;		/* workQ spinlock */
	pthread_t workq_tid;		/* workQ thread ID */
	pthread_t cache_evict_tid;	/* Cache eviction server thread ID */
	pthread_t cache_read_tid;	/* Cache read server thread ID */

					/* Locked: btree list */
	TAILQ_HEAD(wt_btree_qh, __wt_btree) btqh;

	TAILQ_HEAD(
	    __wt_fh_qh, __wt_fh) fhqh;	/* Locked: file list */

	TAILQ_HEAD(__wt_dlh_qh, __wt_dlh)
	    dlhqh;			/* Locked: library list */

	u_int btqcnt;			/* Locked: btree count */
	u_int next_file_id;		/* Locked: file ID counter */

	/*
	 * WiredTiger allocates space for 50 simultaneous sessions (threads of
	 * control) by default.  Growing the number of threads dynamically is
	 * possible, but tricky since the workQ is walking the array without
	 * locking it.
	 *
	 * There's an array of WT_SESSION_IMPL pointers that reference the
	 * allocated array; we do it that way because we want an easy way for
	 * the workQ code to avoid walking the entire array when only a few
	 * threads are running.
	 */
	WT_SESSION_IMPL	**sessions;		/* Session reference */
	void		 *session_array;	/* Session array */
	uint32_t	  session_cnt;		/* Session count */

	/*
	 * WiredTiger allocates space for 15 hazard references in each thread of
	 * control, by default.  There's no code path that requires more than 15
	 * pages at a time (and if we find one, the right change is to increase
	 * the default).
	 *
	 * The hazard array is separate from the WT_SESSION_IMPL array because
	 * we need to easily copy and search it when evicting pages from memory.
	 */
	WT_HAZARD *hazard;		/* Hazard references array */
	uint32_t   hazard_size;
	uint32_t   session_size;

	WT_CACHE  *cache;		/* Page cache */
	int64_t	   cache_size;

	WT_CONNECTION_STATS *stats;	/* Connection statistics */

	WT_FH	   *log_fh;		/* Logging file handle */

					/* Locked: collator list */
	TAILQ_HEAD(__wt_coll_qh, __wt_named_collator) collqh;

					/* Locked: compressor list */
	TAILQ_HEAD(__wt_comp_qh, __wt_named_compressor) compqh;

	FILE *msgfile;
	void (*msgcall)(const WT_CONNECTION_IMPL *, const char *);

	uint32_t verbose;

	uint32_t flags;
};

#define	API_CONF_DEFAULTS(h, n, cfg)					\
	{ __wt_confdfl_##h##_##n, (cfg), NULL }

#define	API_SESSION_INIT(s, h, n, cur, bt)				\
	WT_BTREE *__oldbtree = (s)->btree;				\
	const char *__oldname = (s)->name;				\
	(s)->cursor = (cur);						\
	(s)->btree = (bt);						\
	(s)->name = #h "." #n;						\

#define	API_CALL_NOCONF(s, h, n, cur, bt) do {				\
	API_SESSION_INIT(s, h, n, cur, bt);				\
	ret = 0

/* Standard entry point to the API.  Sets ret to 0 on success. */
#define	API_CALL(s, h, n, cur, bt, cfg, cfgvar)	do {			\
	const char *cfgvar[] = API_CONF_DEFAULTS(h, n, cfg);		\
	API_SESSION_INIT(s, h, n, cur, bt);				\
	WT_ERR((cfg != NULL) ?						\
	    __wt_config_check((s), __wt_confchk_##h##_##n, (cfg)) : 0)

#define	API_END(s)							\
	if ((s) != NULL) {						\
		(s)->btree = __oldbtree;				\
		(s)->name = __oldname;					\
	}								\
} while (0)

#define	SESSION_API_CALL(s, n, cfg, cfgvar)				\
	API_CALL(s, session, n, NULL, NULL, cfg, cfgvar);

#define	CONNECTION_API_CALL(conn, s, n, cfg, cfgvar)			\
	s = &conn->default_session;					\
	API_CALL(s, connection, n, NULL, NULL, cfg, cfgvar);		\

#define	CURSOR_API_CALL(cur, s, n, bt)					\
	(s) = (WT_SESSION_IMPL *)(cur)->session;			\
	API_CALL_NOCONF(s, cursor, n, (cur), bt);			\

#define	CURSOR_API_CALL_CONF(cur, s, n, bt, cfg, cfgvar)		\
	(s) = (WT_SESSION_IMPL *)(cur)->session;			\
	API_CALL(s, cursor, n, cur, bt, cfg, cfgvar);			\

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
#define	WT_BUF_INUSE					0x00000001
#define	WT_PAGE_FREE_IGNORE_DISK			0x00000001
#define	WT_REC_EVICT					0x00000004
#define	WT_REC_LOCKED					0x00000002
#define	WT_REC_SALVAGE					0x00000001
#define	WT_SERVER_RUN					0x00000002
#define	WT_SESSION_INTERNAL				0x00000002
#define	WT_SESSION_SALVAGE_QUIET_ERR			0x00000001
#define	WT_VERB_ALLOCATE				0x00000200
#define	WT_VERB_EVICTSERVER				0x00000100
#define	WT_VERB_FILEOPS					0x00000080
#define	WT_VERB_HAZARD					0x00000040
#define	WT_VERB_MUTEX					0x00000020
#define	WT_VERB_READ					0x00000010
#define	WT_VERB_READSERVER				0x00000008
#define	WT_VERB_RECONCILE				0x00000004
#define	WT_VERB_SALVAGE					0x00000002
#define	WT_VERB_WRITE					0x00000001
#define	WT_VERIFY					0x00000001
#define	WT_WORKQ_RUN					0x00000001
/*
 * API flags section: END
 * DO NOT EDIT: automatically built by dist/api_flags.py.
 */
