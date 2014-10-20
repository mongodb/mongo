/*-
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

	SLIST_ENTRY(__wt_data_handle_cache) l;
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
#define	S2C_SAFE(session) ((session) == NULL ? NULL : S2C(session))

/* Get the btree for a session */
#define	S2BT(session)	   ((WT_BTREE *)(session)->dhandle->handle)
#define	S2BT_SAFE(session) ((session)->dhandle == NULL ? NULL : S2BT(session))

/*
 * WT_SESSION_IMPL --
 *	Implementation of WT_SESSION.
 */
struct __wt_session_impl {
	WT_SESSION iface;

	void	*lang_private;		/* Language specific private storage */

	u_int active;			/* Non-zero if the session is in-use */

	const char *name;		/* Name */
	const char *lastop;		/* Last operation */
	uint32_t id;			/* UID, offset in session array */

	WT_CONDVAR *cond;		/* Condition variable */

	uint32_t rnd[2];		/* Random number generation state */

	WT_EVENT_HANDLER *event_handler;/* Application's event handlers */

	WT_DATA_HANDLE *dhandle;	/* Current data handle */

					/* Session handle reference list */
	SLIST_HEAD(__dhandles, __wt_data_handle_cache) dhandles;
#define	WT_DHANDLE_SWEEP_WAIT	60	/* Wait before discarding */
#define	WT_DHANDLE_SWEEP_PERIOD	20	/* Only sweep every 20 seconds */
	time_t last_sweep;		/* Last sweep for dead handles */

	WT_CURSOR *cursor;		/* Current cursor */
					/* Cursors closed with the session */
	TAILQ_HEAD(__cursors, __wt_cursor) cursors;

	WT_CURSOR_BACKUP *bkp_cursor;	/* Hot backup cursor */
	WT_COMPACT	 *compact;	/* Compact state */

	WT_BTREE *metafile;		/* Metadata file */
	void	*meta_track;		/* Metadata operation tracking */
	void	*meta_track_next;	/* Current position */
	void	*meta_track_sub;	/* Child transaction / save point */
	size_t	 meta_track_alloc;	/* Currently allocated */
	int	 meta_track_nest;	/* Nesting level of meta transaction */
#define	WT_META_TRACKING(session)	(session->meta_track_next != NULL)

	TAILQ_HEAD(__tables, __wt_table) tables;

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

	WT_TXN_ISOLATION isolation;
	WT_TXN	txn;			/* Transaction state */
	u_int	ncursors;		/* Count of active file cursors. */

	WT_REF **excl;			/* Eviction exclusive list */
	u_int	 excl_next;		/* Next empty slot */
	size_t	 excl_allocated;	/* Bytes allocated */

	void	*block_manager;		/* Block-manager support */
	int	(*block_manager_cleanup)(WT_SESSION_IMPL *);

	WT_DATA_HANDLE **ckpt_handle;	/* Checkpoint support */
	u_int   ckpt_handle_next;	/* Next empty slot */
	size_t  ckpt_handle_allocated;	/* Bytes allocated */

	void	*reconcile;		/* Reconciliation support */
	int	(*reconcile_cleanup)(WT_SESSION_IMPL *);

	int compaction;			/* Compaction did some work */

	/*
	 * The split stash memory and hazard information persist past session
	 * close, because they are accessed by threads of control other than
	 * the thread owning the session.  They live at the end of the
	 * structure so it's somewhat easier to clear everything but the fields
	 * that persist.
	 */
#define	WT_SESSION_CLEAR_SIZE(s)					\
	(WT_PTRDIFF(&(s)->flags, s) + sizeof((s)->flags))
	uint32_t flags;

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
	 * The number of hazard pointers that can be in use grows dynamically.
	 */
#define	WT_HAZARD_INCR		10
	uint32_t   hazard_size;		/* Allocated slots in hazard array. */
	uint32_t   nhazard;		/* Count of active hazard pointers */
	WT_HAZARD *hazard;		/* Hazard pointer array */
} WT_GCC_ATTRIBUTE((aligned(WT_CACHE_LINE_ALIGNMENT)));
