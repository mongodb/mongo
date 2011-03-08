/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

/* Forward declarations of internal handle types. */
struct __btree;			typedef struct __btree BTREE;
struct __connection;		typedef struct __connection CONNECTION;
struct __session;		typedef struct __session SESSION;

typedef struct {
	WT_CURSOR iface;

	BTREE *btree;
	WT_WALK walk;
	WT_REF *ref;
	WT_COL *cip;
	WT_ROW *rip;
	WT_BUF *key_tmp, *value_tmp;
	uint32_t nitems;
} CURSOR_BTREE;

/*
 * WT_STACK --
 *	We maintain a stack of parent pages as we build the tree, encapsulated
 *	in this structure.
 */
typedef struct {
	WT_PAGE	*page;				/* page header */
	uint8_t	*first_free;			/* page's first free byte */
	uint32_t space_avail;			/* page's space available */

	WT_BUF *tmp;			/* page-in-a-buffer */
	void *data;				/* last on-page WT_COL/WT_ROW */
} WT_STACK_ELEM;

typedef struct {
	WT_STACK_ELEM *elem;			/* stack */
	u_int size;				/* stack size */
} WT_STACK;

typedef struct {
	CURSOR_BTREE cbt;

	WT_BUF *tmp;
	WT_PAGE *page;
	WT_STACK stack;
	uint64_t insert_cnt;
	uint32_t space_avail;
	uint8_t *first_free;
} CURSOR_BULK;

struct __btree {
	CONNECTION *conn;		/* Enclosing connection */

	TAILQ_ENTRY(__btree) q;		/* Linked list of databases */

	char	 *name;			/* File name */
	mode_t	  mode;			/* File create mode */

	uint32_t file_id;		/* In-memory file ID */
	WT_FH	 *fh;			/* Backing file handle */

	/*
	 * When a file is opened and/or created a hazard reference is taken on
	 * its root page, and the root page brought into memory.  If no root
	 * page has been acquired, there's usually not much work to do.
	 */
#define	WT_UNOPENED_FILE(btree)	((btree)->root_page.state != WT_REF_CACHE)

	WT_REF		root_page;	/* Root page reference */

	uint32_t free_addr;		/* Free page */
	uint32_t free_size;

	uint32_t freelist_entries;	/* Free list entry count */
	TAILQ_HEAD(__wt_free_qah, __wt_free_entry) freeqa;
	TAILQ_HEAD(__wt_free_qsh, __wt_free_entry) freeqs;

	WT_WALK evict_walk;		/* Eviction thread's walk state */

	void *huffman_key;		/* Key huffman encoding */
	void *huffman_data;		/* Data huffman encoding */

	WT_CELL empty_cell;		/* Empty cell */

	WT_STATS *stats;		/* Btree handle statistics */
	WT_STATS *fstats;		/* File statistics */

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

	/*
	 * DO NOT EDIT: automatically built by dist/api.py.
	 * BTREE methods: BEGIN
	 */
	int (*btree_compare_get)(
	    BTREE *, int (**)(BTREE *, const WT_ITEM *, const WT_ITEM *));

	int (*btree_compare_int_get)(
	    BTREE *, int *);

	int (*btree_compare_int_set)(
	    BTREE *, int );

	int (*btree_compare_set)(
	    BTREE *, int (*)(BTREE *, const WT_ITEM *, const WT_ITEM *));

	int (*btree_itemsize_get)(
	    BTREE *, uint32_t *, uint32_t *);

	int (*btree_itemsize_set)(
	    BTREE *, uint32_t , uint32_t );

	int (*btree_pagesize_get)(
	    BTREE *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *);

	int (*btree_pagesize_set)(
	    BTREE *, uint32_t , uint32_t , uint32_t , uint32_t , uint32_t );

	int (*bulk_load)(
	    BTREE *, void (*)(const char *, uint64_t), int (*)(BTREE *, WT_ITEM **, WT_ITEM **));

	int (*close)(
	    BTREE *, uint32_t );

	int (*col_del)(
	    BTREE *, SESSION *, uint64_t , uint32_t );

	int (*col_get)(
	    BTREE *, SESSION *, uint64_t , WT_ITEM *, uint32_t );

	int (*col_put)(
	    BTREE *, SESSION *, uint64_t , WT_ITEM *, uint32_t );

	int (*column_set)(
	    BTREE *, uint32_t , const char *, uint32_t );

	int (*dump)(
	    BTREE *, FILE *, void (*)(const char *, uint64_t), uint32_t );

	int (*huffman_set)(
	    BTREE *, uint8_t const *, u_int , uint32_t );

	int (*open)(
	    BTREE *, const char *, mode_t , uint32_t );

	int (*row_del)(
	    BTREE *, SESSION *, WT_ITEM *, uint32_t );

	int (*row_get)(
	    BTREE *, SESSION *, WT_ITEM *, WT_ITEM *, uint32_t );

	int (*row_put)(
	    BTREE *, SESSION *, WT_ITEM *, WT_ITEM *, uint32_t );

	int (*stat_clear)(
	    BTREE *, uint32_t );

	int (*stat_print)(
	    BTREE *, FILE *, uint32_t );

	int (*sync)(
	    BTREE *, void (*)(const char *, uint64_t), uint32_t );

	int (*verify)(
	    BTREE *, void (*)(const char *, uint64_t), uint32_t );
	/*
	 * BTREE methods: END
	 * DO NOT EDIT: automatically built by dist/api.py.
	 */

	uint32_t flags;
};

/*******************************************
 * Application session information
 *******************************************/
typedef	enum {
	WT_WORKQ_NONE=0,		/* No request */
	WT_WORKQ_FUNC=1,		/* Function, then return */
	WT_WORKQ_READ=2,		/* Function, then schedule read */
	WT_WORKQ_READ_SCHED=3		/* Waiting on read to complete */
} wq_state_t;

struct __session {
	WT_SESSION iface;

	WT_ERROR_HANDLER *error_handler;

	TAILQ_ENTRY(__session) q;
	TAILQ_HEAD(__cursors, WT_CURSOR) cursors;

	TAILQ_HEAD(__btrees, __btree) btrees;

	char err_buf[32];		/* Last-ditch error buffer */

	WT_MTX	 *mtx;			/* Blocking mutex */

	const char *name;		/* Name */

	BTREE	*btree;			/* Current file */
	WT_BUF	 key, value;	/* Returned key/value pairs */

	WT_BUF	*scratch;	/* Temporary memory for any function */
	u_int	 scratch_alloc;		/* Currently allocated */

					/* SESSION workQ request */
	wq_state_t volatile wq_state;	/* Request state */
	int	  wq_ret;		/* Return value */
	int     (*wq_func)(SESSION *);	/* Function */
	void	 *wq_args;		/* Function argument */
	int	  wq_sleeping;		/* Thread is blocked */

	WT_PAGE	**hazard;		/* Hazard reference array */

	WT_FLIST *flist;		/* Memory free list */

	SESSION_BUFFER *sb;		/* Per-thread update buffer */

					/* Search return values: */
	WT_PAGE   *srch_page;		/*    page */
	uint32_t   srch_write_gen;	/*    page's write-generation */
	void	  *srch_ip;		/*    WT_{COL,ROW} index */
	WT_UPDATE *srch_upd;		/*    WT_UPD array index */
					/* RLE column-store only: */
	WT_RLE_EXPAND *srch_exp;	/*    WT_RLE_EXPAND array index */

	void (*msgcall)(const CONNECTION *, const char *);

	FILE *msgfile;

	/*
	 * DO NOT EDIT: automatically built by dist/api.py.
	 * SESSION getter/setter variables: BEGIN
	 */	/*
	 * SESSION getter/setter variables: END
	 */
	/*
	 * DO NOT EDIT: automatically built by dist/api.py.
	 * SESSION methods: BEGIN
	 */
	int (*close)(
	    SESSION *, uint32_t );
	/*
	 * SESSION methods: END
	 * DO NOT EDIT: automatically built by dist/api.py.
	 */

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

	TAILQ_HEAD(__wt_btree_qh, __btree) dbqh; /* Locked: database list */
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
	WT_PAGE **hazard;		/* Hazard references array */

	WT_CACHE  *cache;		/* Page cache */

	WT_STATS *stats;		/* Environment handle statistics */
	WT_STATS *method_stats;		/* Environment method statistics */

#ifdef HAVE_DIAGNOSTIC
	WT_MTRACK *mtrack;		/* Memory tracking information */
#endif

	const char *sep;		/* Display separator line */

	uint32_t flags;

	/*
	 * DO NOT EDIT: automatically built by dist/api.py.
	 * CONNECTION getter/setter variables: BEGIN
	 */
	uint32_t cache_size;

	uint32_t data_update_initial;

	uint32_t data_update_max;

	uint32_t hazard_size;

	void (*msgcall)(const CONNECTION *, const char *);

	FILE *msgfile;

	uint32_t session_size;

	uint32_t verbose;
	/*
	 * CONNECTION getter/setter variables: END
	 * DO NOT EDIT: automatically built by dist/api.py.
	 */

	/*
	 * DO NOT EDIT: automatically built by dist/api.py.
	 * CONNECTION methods: BEGIN
	 */
	int (*btree)(
	    CONNECTION *, uint32_t , BTREE **);

	int (*cache_size_get)(
	    CONNECTION *, uint32_t *);

	int (*cache_size_set)(
	    CONNECTION *, uint32_t );

	int (*close)(
	    CONNECTION *, uint32_t );

	int (*data_update_initial_get)(
	    CONNECTION *, uint32_t *);

	int (*data_update_initial_set)(
	    CONNECTION *, uint32_t );

	int (*data_update_max_get)(
	    CONNECTION *, uint32_t *);

	int (*data_update_max_set)(
	    CONNECTION *, uint32_t );

	int (*hazard_size_get)(
	    CONNECTION *, uint32_t *);

	int (*hazard_size_set)(
	    CONNECTION *, uint32_t );

	int (*msgcall_get)(
	    CONNECTION *, void (**)(const CONNECTION *, const char *));

	int (*msgcall_set)(
	    CONNECTION *, void (*)(const CONNECTION *, const char *));

	int (*msgfile_get)(
	    CONNECTION *, FILE **);

	int (*msgfile_set)(
	    CONNECTION *, FILE *);

	int (*open)(
	    CONNECTION *, const char *, mode_t , uint32_t );

	int (*session)(
	    CONNECTION *, uint32_t , SESSION **);

	int (*session_size_get)(
	    CONNECTION *, uint32_t *);

	int (*session_size_set)(
	    CONNECTION *, uint32_t );

	int (*stat_clear)(
	    CONNECTION *, uint32_t );

	int (*stat_print)(
	    CONNECTION *, FILE *, uint32_t );

	int (*sync)(
	    CONNECTION *, void (*)(const char *, uint64_t), uint32_t );

	int (*verbose_get)(
	    CONNECTION *, uint32_t *);

	int (*verbose_set)(
	    CONNECTION *, uint32_t );
	/*
	 * CONNECTION methods: END
	 * DO NOT EDIT: automatically built by dist/api.py.
	 */
};

/*******************************************
 * Prototypes.
 *******************************************/
int	 wiredtiger_env_init(CONNECTION **, uint32_t);
void	 wiredtiger_err_stream(FILE *);
int	 wiredtiger_simple_setup(const char *, const char *, BTREE **);
int	 wiredtiger_simple_teardown(const char *, BTREE *);

extern WT_ERROR_HANDLER *__wt_error_handler_default;

/*
 * DO NOT EDIT: automatically built by dist/api_flags.py.
 * API flags section: BEGIN
 */
#define	WT_ASCII_ENGLISH				0x00000008
#define	WT_BUF_INUSE					0x00000001
#define	WT_COLUMN					0x00000004
#define	WT_CREATE					0x00000001
#define	WT_DATA_OVERWRITE				0x00000001
#define	WT_DEBUG					0x00000002
#define	WT_HUFFMAN_DATA					0x00000004
#define	WT_HUFFMAN_KEY					0x00000002
#define	WT_INSERT					0x00000001
#define	WT_MEMORY_CHECK					0x00000001
#define	WT_NOWRITE					0x00000002
#define	WT_OSWRITE					0x00000001
#define	WT_PRINTABLES					0x00000001
#define	WT_RDONLY					0x00000002
#define	WT_READ_EVICT					0x00000002
#define	WT_READ_PRIORITY				0x00000001
#define	WT_RLE						0x00000001
#define	WT_SERVER_RUN					0x00000004
#define	WT_TELEPHONE					0x00000001
#define	WT_VERB_ALL					0x00000020
#define	WT_VERB_EVICT					0x00000010
#define	WT_VERB_FILEOPS					0x00000008
#define	WT_VERB_HAZARD					0x00000004
#define	WT_VERB_MUTEX					0x00000002
#define	WT_VERB_READ					0x00000001
#define	WT_WALK_CACHE					0x00000001
#define	WT_WORKQ_RUN					0x00000002

#define	WT_APIMASK_BT_SEARCH_COL			0x00000001
#define	WT_APIMASK_BT_SEARCH_KEY_ROW			0x00000001
#define	WT_APIMASK_BT_TREE_WALK				0x00000001
#define	WT_APIMASK_BTREE				0x00000007
#define	WT_APIMASK_BTREE_CLOSE				0x00000003
#define	WT_APIMASK_BTREE_COL_DEL			0x00000000
#define	WT_APIMASK_BTREE_COL_GET			0x00000000
#define	WT_APIMASK_BTREE_COL_PUT			0x00000000
#define	WT_APIMASK_BTREE_COLUMN_SET			0x00000001
#define	WT_APIMASK_BTREE_DUMP				0x00000003
#define	WT_APIMASK_BTREE_HUFFMAN_SET			0x0000000f
#define	WT_APIMASK_BTREE_OPEN				0x00000003
#define	WT_APIMASK_BTREE_ROW_DEL			0x00000000
#define	WT_APIMASK_BTREE_ROW_GET			0x00000000
#define	WT_APIMASK_BTREE_ROW_PUT			0x00000000
#define	WT_APIMASK_BTREE_STAT_CLEAR			0x00000000
#define	WT_APIMASK_BTREE_STAT_PRINT			0x00000000
#define	WT_APIMASK_BTREE_SYNC				0x00000001
#define	WT_APIMASK_BTREE_VERIFY				0x00000000
#define	WT_APIMASK_BUF					0x00000001
#define	WT_APIMASK_CONN					0x00000007
#define	WT_APIMASK_CONNECTION_BTREE			0x00000000
#define	WT_APIMASK_CONNECTION_CLOSE			0x00000000
#define	WT_APIMASK_CONNECTION_OPEN			0x00000000
#define	WT_APIMASK_CONNECTION_SESSION			0x00000000
#define	WT_APIMASK_CONNECTION_STAT_CLEAR		0x00000000
#define	WT_APIMASK_CONNECTION_STAT_PRINT		0x00000000
#define	WT_APIMASK_CONNECTION_SYNC			0x00000000
#define	WT_APIMASK_CONNECTION_VERBOSE_SET		0x0000003f
#define	WT_APIMASK_SESSION				0x00000003
#define	WT_APIMASK_SESSION_CLOSE			0x00000000
#define	WT_APIMASK_WIREDTIGER_CONN_INIT			0x00000001
/*
 * API flags section: END
 * DO NOT EDIT: automatically built by dist/api_flags.py.
 */
