/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

typedef struct {
	WT_CURSOR iface;

	BTREE *btree;
	WT_WALK walk;
	WT_PAGE *page;
	WT_COL *cip;
	WT_ROW *rip;
	WT_INSERT *ins;                 /* For walking through RLE pages. */
	wiredtiger_recno_t recno;
	uint32_t nitems;
	uint32_t nrepeats;
} CURSOR_BTREE;

/*
 * WT_STACK --
 *	We maintain a stack of parent pages as we build the tree, encapsulated
 *	in this structure.
 */
typedef struct {
	WT_PAGE	*page;			/* page header */
	uint8_t	*first_free;		/* page's first free byte */
	uint32_t space_avail;		/* page's space available */

	WT_BUF *tmp;			/* page-in-a-buffer */
	void *data;			/* last on-page WT_COL/WT_ROW */
} WT_STACK_ELEM;

typedef struct {
	WT_STACK_ELEM *elem;		/* stack */
	u_int size;			/* stack size */
} WT_STACK;

typedef struct {
	CURSOR_BTREE cbt;

	uint8_t	 page_type;			/* Page type */
	uint64_t recno;				/* Total record number */
	uint32_t ipp;				/* Items per page */

	WT_BUF	 key;				/* Parent key buffer */

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

struct __btree {
	CONNECTION *conn;		/* Enclosing connection */
	uint32_t refcnt;		/* Sessions with this tree open. */

	TAILQ_ENTRY(__btree) q;		/* Linked list of databases */

	char	 *name;			/* File name */
	mode_t	  mode;			/* File create mode */

	uint64_t  lsn;			/* LSN file/offset pair */

	uint32_t file_id;		/* In-memory file ID */
	WT_FH	 *fh;			/* Backing file handle */

	WT_REF	root_page;		/* Root page reference */

	uint32_t free_addr;		/* Free page */
	uint32_t free_size;

	uint32_t freelist_entries;	/* Free list entry count */
	TAILQ_HEAD(__wt_free_qah, __wt_free_entry) freeqa;
	TAILQ_HEAD(__wt_free_qsh, __wt_free_entry) freeqs;
	int	 freelist_dirty;	/* Free-list modified */

	WT_WALK evict_walk;		/* Eviction thread's walk state */

	void *huffman_key;		/* Key huffman encoding */
	void *huffman_value;		/* Value huffman encoding */

	WT_BTREE_STATS *stats;		/* Btree handle statistics */
	WT_BTREE_FILE_STATS *fstats;	/* Btree file statistics */

	/*
	 * DO NOT EDIT: automatically built by dist/api.py.
	 * BTREE getter/setter variables: BEGIN
	 */
	int btree_compare_int;

	int (*btree_compare)(BTREE *, const WT_ITEM *, const WT_ITEM *);

	uint32_t intlitemsize;
	uint32_t leafitemsize;

	uint32_t allocsize;
	uint32_t intlmin;
	uint32_t intlmax;
	uint32_t leafmin;
	uint32_t leafmax;

	uint32_t fixed_len;
	const char *dictionary;
	/*
	 * BTREE getter/setter variables: END
	 * DO NOT EDIT: automatically built by dist/api.py.
	 */

	const char *config;		/* Config settings. */
	int config_dirty;		/* Config string modified. */

	uint32_t flags;
};

struct __btree_session {
	BTREE *btree;

	const char *key_format;
	const char *value_format;

	TAILQ_ENTRY(__btree_session) q;
};

/*******************************************
 * Application session information
 *******************************************/
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

struct __session {
	WT_SESSION iface;

	WT_EVENT_HANDLER *event_handler;

	TAILQ_ENTRY(__session) q;
	TAILQ_HEAD(__cursors, wt_cursor) cursors;

	TAILQ_HEAD(__btrees, __btree_session) btrees;

	WT_MTX	 *mtx;			/* Blocking mutex */

	const char *name;		/* Name */

	BTREE	*btree;			/* Current file */
	WT_CURSOR *cursor;		/* Current cursor */

	WT_BUF	**scratch;		/* Temporary memory for any function */
	u_int	 scratch_alloc;		/* Currently allocated */

	WT_BUF	logrec_buf;		/* Buffer for log records */
	WT_BUF	logprint_buf;		/* Buffer for debug log records */

					/* SESSION workQ request */
	wq_state_t volatile wq_state;	/* Request state */
	int	  wq_ret;		/* Return value */
	int     (*wq_func)(SESSION *);	/* Function */
	void	 *wq_args;		/* Function argument */
	int	  wq_sleeping;		/* Thread is blocked */

	WT_HAZARD *hazard;		/* Hazard reference array */

	SESSION_BUFFER *sb;		/* Per-thread update buffer */
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

	void (*msgcall)(const CONNECTION *, const char *);

	FILE *msgfile;

	uint32_t flags;
};

/*******************************************
 * Implementation of WT_CONNECTION
 *******************************************/
struct __connection {
	WT_CONNECTION iface;

	const char *home;

	SESSION default_session;	/* For operations without an
					   application-supplied session. */

	WT_MTX *mtx;			/* Global mutex */

	pthread_t workq_tid;		/* workQ thread ID */
	pthread_t cache_evict_tid;	/* Cache eviction server thread ID */
	pthread_t cache_read_tid;	/* Cache read server thread ID */

	TAILQ_HEAD(wt_btree_qh, __btree) dbqh; /* Locked: database list */
	u_int dbqcnt;			/* Locked: database list count */

	TAILQ_HEAD(
	    __wt_fh_qh, __wt_fh) fhqh;	/* Locked: file list */
	u_int next_file_id;		/* Locked: file ID counter */

	uint32_t volatile api_gen;	/* API generation number */

	/*
	 * WiredTiger allocates space for 50 simultaneous threads of control by
	 * default.   The Env.toc_max_set method tunes this if the application
	 * needs more.   Growing the number of threads dynamically is possible,
	 * but tricky since the workQ is walking the array without locking it.
	 *
	 * There's an array of SESSION pointers that reference the allocated
	 * array; we do it that way because we want an easy way for the workQ
	 * code to avoid walking the entire array when only a few threads are
	 * running.
	 */
	SESSION	**sessions;		/* TOC reference */
	uint32_t toc_cnt;		/* TOC count */
	void	 *toc_array;		/* TOC array */

	TAILQ_HEAD(__sessions, __session) sessions_head;

	/*
	 * WiredTiger allocates space for 15 hazard references in each thread of
	 * control, by default.  The Env.hazard_max_set method tunes this if an
	 * application needs more, but that shouldn't happen, there's no code
	 * path that requires more than 15 pages at a time (and if we find one,
	 * the right change is to increase the default).  The method is there
	 * just in case an application starts failing in the field.
	 *
	 * The hazard array is separate from the SESSION array because we must
	 * be able to easily copy and search it when evicting pages from the
	 * cache.
	 */
	WT_HAZARD *hazard;		/* Hazard references array */

	WT_CACHE  *cache;		/* Page cache */

	WT_CONN_STATS *stats;		/* Database statistics */

	WT_FH	  *log_fh;		/* Logging file handle */
	const char *sep;		/* Display separator line */

	/*
	 * DO NOT EDIT: automatically built by dist/api.py.
	 * CONNECTION getter/setter variables: BEGIN
	 */
	uint64_t cache_size;

	uint32_t data_update_max;

	uint32_t data_update_min;

	uint32_t hazard_size;

	void (*msgcall)(const CONNECTION *, const char *);

	FILE *msgfile;

	uint32_t session_size;

	uint32_t verbose;
	/*
	 * CONNECTION getter/setter variables: END
	 * DO NOT EDIT: automatically built by dist/api.py.
	 */

	uint32_t flags;
};

#define	API_CONF_INIT(h, n, cfg)	const char *__cfg[] =	\
	{ __wt_confdfl_##h##_##n, (cfg), NULL }

#define	API_SESSION_INIT(s, h, n, cur, bt)				\
	(s)->cursor = (cur);						\
	(s)->btree = (bt);						\
	(s)->name = #h "." #n;					\

#define	API_CALL_NOCONF(s, h, n, cur, bt)	do {			\
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
	(s) = (SESSION *)(cur)->session;				\
	API_CALL_NOCONF(s, cursor, n, (cur), bt);			\

#define	CURSOR_API_CALL_CONF(cur, s, n, bt, cfg)			\
	(s) = (SESSION *)(cur)->session;				\
	API_CALL(s, cursor, n, cur, bt, cfg);				\

/*******************************************
 * Prototypes.
 *******************************************/
int	 wiredtiger_env_init(CONNECTION **, uint32_t);
void	 wiredtiger_err_stream(FILE *);
int	 wiredtiger_simple_setup(
    const char *, WT_EVENT_HANDLER *, const char *, BTREE **);
int	 wiredtiger_simple_teardown(const char *, BTREE *);

extern WT_EVENT_HANDLER *__wt_event_handler_default;
extern WT_EVENT_HANDLER *__wt_event_handler_verbose;

/*
 * DO NOT EDIT: automatically built by dist/api_flags.py.
 * API flags section: BEGIN
 */
#define	WT_ASCII_ENGLISH				0x00000008
#define	WT_BUF_INUSE					0x00000001
#define	WT_COLUMN					0x00000004
#define	WT_CREATE					0x00000001
#define	WT_DEBUG					0x00000002
#define	WT_DUMP_PRINT					0x00000001
#define	WT_HUFFMAN_KEY					0x00000004
#define	WT_HUFFMAN_VALUE				0x00000002
#define	WT_RDONLY					0x00000002
#define	WT_REC_EVICT					0x00000002
#define	WT_REC_LOCKED					0x00000001
#define	WT_RLE						0x00000001
#define	WT_SERVER_RUN					0x00000002
#define	WT_SESSION_INTERNAL				0x00000001
#define	WT_TELEPHONE					0x00000001
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
