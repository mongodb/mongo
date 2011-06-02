/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

typedef struct {
	WT_CURSOR iface;

	WT_BTREE *btree;
	WT_WALK walk;
	WT_PAGE *page;
	WT_COL *cip;
	WT_ROW *rip;
	WT_INSERT *ins;                 /* For walking through RLE pages. */
	wiredtiger_recno_t recno;
	uint32_t nitems;
	uint32_t nrepeats;
} CURSOR_BTREE;

typedef struct {
	CURSOR_BTREE cbt;

	uint8_t	 page_type;			/* Page type */
	uint64_t recno;				/* Total record number */
	uint32_t ipp;				/* Items per page */

	/*
	 * K/V pairs for row-store leaf pages, and V objects for column-store
	 * leaf pages, are stored in singly-linked lists (the lists are never
	 * searched, only walked at reconciliation, so it's not so bad).
	 */
	WT_INSERT  *ins_base;			/* Base insert link */
	WT_INSERT **insp;			/* Next insert link */
	WT_UPDATE  *upd_base;			/* Base update link */
	WT_UPDATE **updp;			/* Next update link */
	uint32_t   ins_cnt;			/* Inserts on the list */

	/*
	 * Bulk load dynamically allocates an array of leaf-page references;
	 * when the bulk load finishes, we build an internal page for those
	 * references.
	 */
	WT_ROW_REF *rref;			/* List of row leaf pages */
	WT_COL_REF *cref;			/* List of column leaf pages */
	uint32_t ref_next;			/* Next leaf page slot */
	uint32_t ref_entries;			/* Total leaf page slots */
	uint32_t ref_allocated;			/* Bytes allocated */
} CURSOR_BULK;

typedef struct {
	WT_CURSOR iface;
} CURSOR_CONFIG;

typedef struct {
	WT_CURSOR iface;
} CURSOR_STAT;

struct __wt_btree {
	WT_CONNECTION_IMPL *conn;	/* Enclosing connection */
	TAILQ_ENTRY(__wt_btree) q;	/* Linked list of files */

	uint32_t refcnt;		/* Sessions with this tree open. */

	const char *config;		/* Configuration string */

	const char *name;		/* File name */

	enum {	BTREE_COL_FIX=1,	/* Fixed-length column store */
		BTREE_COL_RLE=2,	/* Fixed-length, RLE column store */
		BTREE_COL_VAR=3,	/* Variable-length column store */
		BTREE_ROW=4		/* Row-store */
	} type;				/* Type */

	uint64_t  lsn;			/* LSN file/offset pair */

	WT_FH	 *fh;			/* Backing file handle */

	WT_REF	root_page;		/* Root page reference */

	uint32_t freelist_entries;	/* Free-list entry count */
					/* Free-list queues */
	TAILQ_HEAD(__wt_free_qah, __wt_free_entry) freeqa;
	TAILQ_HEAD(__wt_free_qsh, __wt_free_entry) freeqs;
	int	 freelist_dirty;	/* Free-list has been modified */
	uint32_t free_addr;		/* Free-list addr/size pair */
	uint32_t free_size;

	WT_WALK evict_walk;		/* Eviction thread's walk state */

	void *huffman_key;		/* Key huffman encoding */
	void *huffman_value;		/* Value huffman encoding */

	uint32_t key_gap;		/* Btree instantiated key gap */
	WT_BUF   key_srch;		/* Search key buffer */

	uint32_t fixed_len;		/* Fixed-length record size */

	int btree_compare_int;		/* Integer keys */
					/* Comparison function */
	int (*btree_compare)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *);

	uint32_t intlitemsize;		/* Maximum item size for overflow */
	uint32_t leafitemsize;

	uint32_t allocsize;		/* Allocation size */
	uint32_t intlmin;		/* Min/max internal page size */
	uint32_t intlmax;
	uint32_t leafmin;		/* Min/max leaf page size */
	uint32_t leafmax;

	WT_BTREE_STATS *stats;		/* Btree handle statistics */
	WT_BTREE_FILE_STATS *fstats;	/* Btree file statistics */

#define	WT_BTREE_NO_EVICTION	0x01	/* Ignored by the eviction thread */
#define	WT_BTREE_VERIFY		0x02	/* Handle is for verify/salvage */
	uint32_t flags;
};

/*******************************************
 * Implementation of WT_SESSION
 *******************************************/
struct __wt_btree_session {
	WT_BTREE *btree;

	const char *key_format;
	const char *value_format;

	TAILQ_ENTRY(__wt_btree_session) q;
};

typedef	enum {
	WT_WORKQ_NONE=0,		/* No request */
	WT_WORKQ_FUNC=1,		/* Function, then return */
	WT_WORKQ_EVICT=2,		/* Function, then schedule evict */
	WT_WORKQ_EVICT_SCHED=3,		/* Waiting on evict to complete */
	WT_WORKQ_READ=4,		/* Function, then schedule read */
	WT_WORKQ_READ_SCHED=5		/* Waiting on read to complete */
} wq_state_t;

struct __wt_hazard {
	WT_PAGE *page;			/* Page address */
#ifdef HAVE_DIAGNOSTIC
	const char *file;		/* File/line where hazard acquired */
	int	    line;
#endif
};

struct __wt_session_impl {
	WT_SESSION iface;

	WT_EVENT_HANDLER *event_handler;

	TAILQ_ENTRY(__wt_session_impl) q;
	TAILQ_HEAD(__cursors, wt_cursor) cursors;

	TAILQ_HEAD(__btrees, __wt_btree_session) btrees;

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

					/* Search return values: */
	WT_PAGE		*srch_page;	/* page */
	uint32_t	 srch_write_gen;/* page's write-generation */
	int		 srch_match;	/* an exact match */
	void		*srch_ip;	/* WT_{COL,ROW} index */
	WT_UPDATE	*srch_vupdate;	/* WT_UPDATE value node */
	WT_INSERT      **srch_ins;	/* WT_INSERT insert node */
	WT_UPDATE      **srch_upd;	/* WT_UPDATE insert node */
	uint32_t	 srch_slot;	/* WT_INSERT/WT_UPDATE slot */

	void (*msgcall)(const WT_CONNECTION_IMPL *, const char *);

	FILE *msgfile;

	uint32_t flags;
};

/*******************************************
 * Implementation of WT_CONNECTION
 *******************************************/
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
	 * WiredTiger allocates space for 50 simultaneous threads of control by
	 * default.   The Env.toc_max_set method tunes this if the application
	 * needs more.   Growing the number of threads dynamically is possible,
	 * but tricky since the workQ is walking the array without locking it.
	 *
	 * There's an array of WT_SESSION_IMPL pointers that reference the
	 * allocated array; we do it that way because we want an easy way for
	 * the workQ code to avoid walking the entire array when only a few
	 * threads are running.
	 */
	WT_SESSION_IMPL	**sessions;		/* TOC reference */
	uint32_t toc_cnt;		/* TOC count */
	void	 *toc_array;		/* TOC array */

	TAILQ_HEAD(__sessions, __wt_session_impl) sessions_head;

	/*
	 * WiredTiger allocates space for 15 hazard references in each thread of
	 * control, by default.  The Env.hazard_max_set method tunes this if an
	 * application needs more, but that shouldn't happen, there's no code
	 * path that requires more than 15 pages at a time (and if we find one,
	 * the right change is to increase the default).  The method is there
	 * just in case an application starts failing in the field.
	 *
	 * The hazard array is separate from the WT_SESSION_IMPL array because
	 * we must be able to easily copy and search it when evicting pages from
	 * the cache.
	 */
	WT_HAZARD *hazard;		/* Hazard references array */

	WT_CACHE  *cache;		/* Page cache */

	WT_CONN_STATS *stats;		/* Database statistics */

	WT_FH	  *log_fh;		/* Logging file handle */
	const char *sep;		/* Display separator line */

	uint64_t cache_size;

	uint32_t data_update_max;

	uint32_t data_update_min;

	uint32_t hazard_size;

	void (*msgcall)(const WT_CONNECTION_IMPL *, const char *);

	FILE *msgfile;

	uint32_t session_size;

	uint32_t verbose;

	uint32_t flags;
};

#define	API_CONF_INIT(h, n, cfg)	const char *__cfg[] =		\
	{ __wt_confdfl_##h##_##n, (cfg), NULL }

#define	API_SESSION_INIT(s, h, n, cur, bt)				\
	(s)->cursor = (cur);						\
	(s)->btree = (bt);						\
	(s)->name = #h "." #n;						\

#define	API_CALL_NOCONF(s, h, n, cur, bt) do {				\
	API_SESSION_INIT(s, h, n, cur, bt)

#define	API_CALL(s, h, n, cur, bt, cfg)	do {				\
	API_CONF_INIT(h, n, cfg);					\
	API_SESSION_INIT(s, h, n, cur, bt);				\
	if (cfg != NULL)						\
		WT_RET(__wt_config_check((s), __cfg[0], (cfg)))

#define	API_END()	} while (0)

#define	SESSION_API_CALL(s, n, cfg)					\
	API_CALL(s, session, n, NULL, NULL, cfg);

#define	CONNECTION_API_CALL(conn, s, n, cfg)				\
	s = &conn->default_session;					\
	API_CALL(s, connection, n, NULL, NULL, cfg);			\

#define	CURSOR_API_CALL(cur, s, n, bt)					\
	(s) = (WT_SESSION_IMPL *)(cur)->session;			\
	API_CALL_NOCONF(s, cursor, n, (cur), bt);			\

#define	CURSOR_API_CALL_CONF(cur, s, n, bt, cfg)			\
	(s) = (WT_SESSION_IMPL *)(cur)->session;			\
	API_CALL(s, cursor, n, cur, bt, cfg);				\

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
#define	WT_DEBUG					0x00000002
#define	WT_DUMP_PRINT					0x00000001
#define	WT_REC_EVICT					0x00000002
#define	WT_REC_LOCKED					0x00000001
#define	WT_SERVER_RUN					0x00000002
#define	WT_SESSION_INTERNAL				0x00000001
#define	WT_VERB_EVICT					0x00000010
#define	WT_VERB_FILEOPS					0x00000008
#define	WT_VERB_HAZARD					0x00000004
#define	WT_VERB_MUTEX					0x00000002
#define	WT_VERB_READ					0x00000001
#define	WT_WALK_CACHE					0x00000001
#define	WT_WORKQ_RUN					0x00000001
#define	WT_WRITE					0x00000001
/*
 * API flags section: END
 * DO NOT EDIT: automatically built by dist/api_flags.py.
 */
