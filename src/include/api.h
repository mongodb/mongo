/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

struct __wt_btree_session {
	WT_BTREE *btree;

	TAILQ_ENTRY(__wt_btree_session) q;
};

/*******************************************
 * Implementation of WT_SESSION
 *******************************************/
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

/*
 * WT_SEARCH --
 *	Search return values.
 */
struct __wt_search {
	WT_PAGE		*page;		/* page */
	uint32_t	 write_gen;	/* leaf page's write-generation */
	int		 match;		/* an exact match */
	void		*ip;		/* WT_{COL,ROW} reference */
	uint32_t	 slot;		/* WT_{COL,ROW} slot */

	WT_UPDATE      **upd;		/* WT_UPDATE base insert node */
	WT_UPDATE	*vupdate;	/* WT_UPDATE value node */

	WT_INSERT_HEAD **inshead;	/* WT_INSERT_HEAD node */
					/* Previous inserts */
	WT_INSERT      **ins[WT_SKIP_MAXDEPTH];

	uint8_t		 v;		/* Bitfield search value */
};

#define	S2C(session) ((WT_CONNECTION_IMPL *)(session)->iface.connection)

/*
 * WT_SESSION_IMPL --
 *	Implementation of WT_SESSION.
 */
struct __wt_session_impl {
	WT_SESSION iface;

	WT_MTX	 *mtx;			/* Blocking mutex */

	const char *name;		/* Name */

	WT_BTREE *btree;		/* Current file */
	WT_CURSOR *cursor;		/* Current cursor */

	WT_BUF	**scratch;		/* Temporary memory for any function */
	u_int	 scratch_alloc;		/* Currently allocated */

	WT_BUF	logrec_buf;		/* Buffer for log records */
	WT_BUF	logprint_buf;		/* Buffer for debug log records */

					/* WT_SESSION_IMPL workQ request */
	wq_state_t volatile wq_state;	/* Request state */
	int	  wq_ret;		/* Return value */
	int     (*wq_func)(WT_SESSION_IMPL *);	/* Function */
	void	 *wq_args;		/* Function argument */
	int	  wq_sleeping;		/* Thread is blocked */

	WT_HAZARD *hazard;		/* Hazard reference array */

	WT_SESSION_BUFFER *sb;		/* Per-thread update buffer */
	uint32_t update_alloc_size;	/* Allocation size */

	WT_SEARCH srch;                 /* Search results */

	WT_EVENT_HANDLER *event_handler;

	TAILQ_HEAD(__cursors, wt_cursor) cursors;

	TAILQ_HEAD(__btrees, __wt_btree_session) btrees;
	WT_BTREE *schematab;		/* Schema tables */

	TAILQ_HEAD(__tables, __wt_table) tables;

	uint32_t flags;
};

/*
 * WT_CONNECTION_IMPL --
 *	Implementation of WT_CONNECTION
 */
struct __wt_connection_impl {
	WT_CONNECTION iface;

	const char *home;

	WT_SESSION_IMPL default_session;/* For operations without an
					   application-supplied session. */

	WT_MTX *mtx;			/* Global mutex */

	pthread_t workq_tid;		/* workQ thread ID */
	pthread_t cache_evict_tid;	/* Cache eviction server thread ID */
	pthread_t cache_read_tid;	/* Cache read server thread ID */

	TAILQ_HEAD(wt_btree_qh, __wt_btree) dbqh; /* Locked: database list */
	u_int dbqcnt;			/* Locked: database list count */

	TAILQ_HEAD(
	    __wt_fh_qh, __wt_fh) fhqh;	/* Locked: file list */
	u_int next_file_id;		/* Locked: file ID counter */

	TAILQ_HEAD(__wt_dlh_qh, __wt_dlh)
	    dlhqh;			/* Locked: library list */

	uint32_t volatile api_gen;	/* API generation number */

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
	uint32_t	  session_cnt;		/* Session count */
	void		 *session_array;	/* Session array */

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

	WT_CONN_STATS *stats;		/* Database statistics */

	WT_FH	   *log_fh;		/* Logging file handle */
	const char *sep;		/* Display separator line */

	TAILQ_HEAD(__wt_comp_qh, __wt_named_compressor)
	    compqh;			/* Locked: compressor list */

	FILE *msgfile;
	void (*msgcall)(const WT_CONNECTION_IMPL *, const char *);

	uint32_t verbose;

	uint32_t flags;
};

#define	API_CONF_DEFAULTS(h, n, cfg)					\
	{ __wt_confdfl_##h##_##n, (cfg), NULL }

#define	API_SESSION_INIT(s, h, n, cur, bt)				\
	const char *__oldname = (s)->name;				\
	(s)->cursor = (cur);						\
	(s)->btree = (bt);						\
	(s)->name = #h "." #n;						\

#define	API_CALL_NOCONF(s, h, n, cur, bt) do {				\
	API_SESSION_INIT(s, h, n, cur, bt)

#define	API_CALL(s, h, n, cur, bt, cfg, cfgvar)	do {			\
	const char *cfgvar[] = API_CONF_DEFAULTS(h, n, cfg);		\
	API_SESSION_INIT(s, h, n, cur, bt);				\
	if (cfg != NULL)						\
		WT_RET(__wt_config_check((s), __wt_confchk_##h##_##n, (cfg)))

#define	API_END(s)							\
	if ((s) != NULL)						\
		(s)->name = __oldname;					\
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

/*
 * DO NOT EDIT: automatically built by dist/api_flags.py.
 * API flags section: BEGIN
 */
#define	WT_BUF_INUSE					0x00000001
#define	WT_ERR_QUIET					0x00000002
#define	WT_PAGE_FREE_IGNORE_DISK			0x00000001
#define	WT_REC_EVICT					0x00000004
#define	WT_REC_LOCKED					0x00000002
#define	WT_REC_SALVAGE					0x00000001
#define	WT_SERVER_RUN					0x00000002
#define	WT_SESSION_INTERNAL				0x00000001
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
#define	WT_WALK_CACHE					0x00000001
#define	WT_WORKQ_RUN					0x00000001
/*
 * API flags section: END
 * DO NOT EDIT: automatically built by dist/api_flags.py.
 */
