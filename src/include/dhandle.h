/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * XXX
 * The server threads use their own WT_SESSION_IMPL handles because they may
 * want to block (for example, the eviction server calls reconciliation, and
 * some of the reconciliation diagnostic code reads pages), and the user's
 * session handle is already blocking on a server thread.  The problem is the
 * server thread needs to reference the correct btree handle, and that's
 * hanging off the application's thread of control.  For now, I'm just making
 * it obvious where that's getting done.
 */
#define	WT_SET_BTREE_IN_SESSION(s, b)	((s)->dhandle = b->dhandle)
#define	WT_CLEAR_BTREE_IN_SESSION(s)	((s)->dhandle = NULL)

/*
 * WT_DATA_HANDLE --
 *	A handle for a generic named data source.
 */
struct __wt_data_handle {
	WT_RWLOCK *rwlock;		/* Lock for shared/exclusive ops */
	uint32_t   refcnt;		/* Sessions using this handle */
	TAILQ_ENTRY(__wt_data_handle) q;/* Linked list of handles */

	const char *name;		/* Object name as a URI */
	const char *checkpoint;		/* Checkpoint name (or NULL) */
	const char **cfg;		/* Configuration information */

	WT_DATA_SOURCE *dsrc;		/* Data source for this handle */
	void *handle;			/* Generic handle */

	WT_DSRC_STATS stats;		/* Data-source statistics */

	/* Flags values over 0xff are reserved for WT_BTREE_* */
#define	WT_DHANDLE_DISCARD	        0x01	/* Discard on release */
#define	WT_DHANDLE_DISCARD_CLOSE	0x02	/* Close on release */
#define	WT_DHANDLE_EXCLUSIVE	        0x04	/* Need exclusive access */
#define	WT_DHANDLE_LOCK_ONLY	        0x08	/* Handle only used as a lock */
#define	WT_DHANDLE_OPEN		        0x10	/* Handle is open */
	uint32_t flags;
};
