/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_DATA_HANDLE_CACHE --
 *	Per-session cache of handles to avoid synchronization when opening
 *	cursors.
 */
struct __wt_data_handle_cache {
	WT_DATA_HANDLE *dhandle;

	TAILQ_ENTRY(__wt_data_handle_cache) q;
	TAILQ_ENTRY(__wt_data_handle_cache) hashq;
};

/*
 * WT_HAZARD --
 *	A hazard pointer.
 */
struct __wt_hazard {
	WT_PAGE *page;			/* Page address */
#ifdef HAVE_DIAGNOSTIC
	const char *file;		/* File/line where hazard acquired */
	int	    line;
#endif
};

/* Get the connection implementation for a session */
#define	S2C(session)	  ((WT_CONNECTION_IMPL *)(session)->iface.connection)

/* Get the btree for a session */
#define	S2BT(session)	   ((WT_BTREE *)(session)->dhandle->handle)
#define	S2BT_SAFE(session) ((session)->dhandle == NULL ? NULL : S2BT(session))

/*
 * WT_SESSION_IMPL --
 *	Implementation of WT_SESSION.
 */
struct WT_COMPILER_TYPE_ALIGN(WT_CACHE_LINE_ALIGNMENT) __wt_session_impl {
	WT_SESSION iface;

	void	*lang_private;		/* Language specific private storage */

	u_int active;			/* Non-zero if the session is in-use */

	const char *name;		/* Name */
	const char *lastop;		/* Last operation */
	uint32_t id;			/* UID, offset in session array */

	WT_CONDVAR *cond;		/* Condition variable */

	WT_EVENT_HANDLER *event_handler;/* Application's event handlers */

	WT_DATA_HANDLE *dhandle;	/* Current data handle */

	/*
	 * Each session keeps a cache of data handles. The set of handles
	 * can grow quite large so we maintain both a simple list and a hash
	 * table of lists. The hash table key is based on a hash of the table
	 * URI. The hash table list is kept in allocated memory that lives
	 * across session close - so it is declared further down.
	 */
					/* Session handle reference list */
	TAILQ_HEAD(__dhandles, __wt_data_handle_cache) dhandles;
	time_t last_sweep;		/* Last sweep for dead handles */

	WT_CURSOR *cursor;		/* Current cursor */
					/* Cursors closed with the session */
	TAILQ_HEAD(__cursors, __wt_cursor) cursors;

	WT_CURSOR_BACKUP *bkp_cursor;	/* Hot backup cursor */

	WT_COMPACT	 *compact;	/* Compaction information */
	enum { WT_COMPACT_NONE=0,
	    WT_COMPACT_RUNNING, WT_COMPACT_SUCCESS } compact_state;

	/*
	 * Lookaside table cursor, sweep and eviction worker threads only.
	 */
	WT_CURSOR	*las_cursor;	/* Lookaside table cursor */

	WT_CURSOR *meta_cursor;		/* Metadata file */
	void	  *meta_track;		/* Metadata operation tracking */
	void	  *meta_track_next;	/* Current position */
	void	  *meta_track_sub;	/* Child transaction / save point */
	size_t	   meta_track_alloc;	/* Currently allocated */
	int	   meta_track_nest;	/* Nesting level of meta transaction */
#define	WT_META_TRACKING(session)	(session->meta_track_next != NULL)

	/*
	 * Each session keeps a cache of table handles. The set of handles
	 * can grow quite large so we maintain both a simple list and a hash
	 * table of lists. The hash table list is kept in allocated memory
	 * that lives across session close - so it is declared further down.
	 */
	TAILQ_HEAD(__tables, __wt_table) tables;

	WT_ITEM	**scratch;		/* Temporary memory for any function */
	u_int	  scratch_alloc;	/* Currently allocated */
	size_t	  scratch_cached;	/* Scratch bytes cached */
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

	WT_ITEM err;			/* Error buffer */

	WT_TXN_ISOLATION isolation;
	WT_TXN	txn;			/* Transaction state */
	WT_LSN	bg_sync_lsn;		/* Background sync operation LSN. */
	u_int	ncursors;		/* Count of active file cursors. */

	void	*block_manager;		/* Block-manager support */
	int	(*block_manager_cleanup)(WT_SESSION_IMPL *);

					/* Checkpoint handles */
	WT_DATA_HANDLE **ckpt_handle;	/* Handle list */
	u_int   ckpt_handle_next;	/* Next empty slot */
	size_t  ckpt_handle_allocated;	/* Bytes allocated */

	/*
	 * Operations acting on handles.
	 *
	 * The preferred pattern is to gather all of the required handles at
	 * the beginning of an operation, then drop any other locks, perform
	 * the operation, then release the handles.  This cannot be easily
	 * merged with the list of checkpoint handles because some operations
	 * (such as compact) do checkpoints internally.
	 */
	WT_DATA_HANDLE **op_handle;	/* Handle list */
	u_int   op_handle_next;		/* Next empty slot */
	size_t  op_handle_allocated;	/* Bytes allocated */

	void	*reconcile;		/* Reconciliation support */
	int	(*reconcile_cleanup)(WT_SESSION_IMPL *);

	uint32_t flags;

	/*
	 * The split stash memory and hazard information persist past session
	 * close because they are accessed by threads of control other than the
	 * thread owning the session.
	 *
	 * The random number state persists past session close because we don't
	 * want to repeatedly allocate repeated values for skiplist depth if the
	 * application isn't caching sessions.
	 *
	 * All of these fields live at the end of the structure so it's easier
	 * to clear everything but the fields that persist.
	 */
#define	WT_SESSION_CLEAR_SIZE(s)					\
	(WT_PTRDIFF(&(s)->rnd, s))

	WT_RAND_STATE rnd;		/* Random number generation state */

					/* Hashed handle reference list array */
	TAILQ_HEAD(__dhandles_hash, __wt_data_handle_cache) *dhhash;
					/* Hashed table reference list array */
	TAILQ_HEAD(__tables_hash, __wt_table) *tablehash;

	/*
	 * Splits can "free" memory that may still be in use, and we use a
	 * split generation number to track it, that is, the session stores a
	 * reference to the memory and allocates a split generation; when no
	 * session is reading from that split generation, the memory can be
	 * freed for real.
	 */
	struct __wt_split_stash {
		uint64_t    split_gen;	/* Split generation */
		void       *p;		/* Memory, length */
		size_t	    len;
	} *split_stash;			/* Split stash array */
	size_t  split_stash_cnt;	/* Array entries */
	size_t  split_stash_alloc;	/* Allocated bytes */

	uint64_t split_gen;		/* Reading split generation */

	/*
	 * Hazard pointers.
	 *
	 * Use the non-NULL state of the hazard field to know if the session has
	 * previously been initialized.
	 */
#define	WT_SESSION_FIRST_USE(s)						\
	((s)->hazard == NULL)

	/* The number of hazard pointers grows dynamically. */
#define	WT_HAZARD_INCR		1
	uint32_t   hazard_size;		/* Allocated slots in hazard array. */
	uint32_t   nhazard;		/* Count of active hazard pointers */
	WT_HAZARD *hazard;		/* Hazard pointer array */
};
