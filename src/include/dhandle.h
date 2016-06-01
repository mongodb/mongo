/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Helpers for calling a function with a data handle in session->dhandle
 * then restoring afterwards.
 */
#define	WT_WITH_DHANDLE(s, d, e) do {					\
	WT_DATA_HANDLE *__saved_dhandle = (s)->dhandle;			\
	(s)->dhandle = (d);						\
	e;								\
	(s)->dhandle = __saved_dhandle;					\
} while (0)

#define	WT_WITH_BTREE(s, b, e)	WT_WITH_DHANDLE(s, (b)->dhandle, e)

/* Call a function without the caller's data handle, restore afterwards. */
#define	WT_WITHOUT_DHANDLE(s, e) WT_WITH_DHANDLE(s, NULL, e)

/*
 * Call a function with the caller's data handle, restore it afterwards in case
 * it is overwritten.
 */
#define	WT_SAVE_DHANDLE(s, e) WT_WITH_DHANDLE(s, (s)->dhandle, e)

/* Check if a handle is inactive. */
#define	WT_DHANDLE_INACTIVE(dhandle)					\
	(F_ISSET(dhandle, WT_DHANDLE_DEAD) ||				\
	!F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE | WT_DHANDLE_OPEN))

/* The metadata cursor's data handle. */
#define	WT_SESSION_META_DHANDLE(s)					\
	(((WT_CURSOR_BTREE *)((s)->meta_cursor))->btree->dhandle)

/*
 * WT_DATA_HANDLE --
 *	A handle for a generic named data source.
 */
struct __wt_data_handle {
	WT_RWLOCK *rwlock;		/* Lock for shared/exclusive ops */
	TAILQ_ENTRY(__wt_data_handle) q;
	TAILQ_ENTRY(__wt_data_handle) hashq;

	/*
	 * Sessions caching a connection's data handle will have a non-zero
	 * reference count; sessions using a connection's data handle will
	 * have a non-zero in-use count.
	 */
	uint32_t session_ref;		/* Sessions referencing this handle */
	int32_t	 session_inuse;		/* Sessions using this handle */
	uint32_t excl_ref;		/* Refs of handle by excl_session */
	time_t	 timeofdeath;		/* Use count went to 0 */
	WT_SESSION_IMPL *excl_session;	/* Session with exclusive use, if any */

	uint64_t name_hash;		/* Hash of name */
	const char *name;		/* Object name as a URI */
	const char *checkpoint;		/* Checkpoint name (or NULL) */
	const char **cfg;		/* Configuration information */

	WT_DATA_SOURCE *dsrc;		/* Data source for this handle */
	void *handle;			/* Generic handle */

	/*
	 * Data handles can be closed without holding the schema lock; threads
	 * walk the list of open handles, operating on them (checkpoint is the
	 * best example).  To avoid sources disappearing underneath checkpoint,
	 * lock the data handle when closing it.
	 */
	WT_SPINLOCK	close_lock;	/* Lock to close the handle */

					/* Data-source statistics */
	WT_DSRC_STATS *stats[WT_COUNTER_SLOTS];
	WT_DSRC_STATS  stat_array[WT_COUNTER_SLOTS];

	/* Flags values over 0xff are reserved for WT_BTREE_* */
#define	WT_DHANDLE_DEAD		        0x01	/* Dead, awaiting discard */
#define	WT_DHANDLE_DISCARD	        0x02	/* Discard on release */
#define	WT_DHANDLE_DISCARD_FORCE	0x04	/* Force discard on release */
#define	WT_DHANDLE_EXCLUSIVE	        0x08	/* Need exclusive access */
#define	WT_DHANDLE_IS_METADATA		0x10	/* Metadata handle */
#define	WT_DHANDLE_LOCK_ONLY	        0x20	/* Handle only used as a lock */
#define	WT_DHANDLE_OPEN		        0x40	/* Handle is open */
	uint32_t flags;
};
