/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

typedef enum {
	WT_ASYNCOP_ENQUEUED,	/* Placed on the work queue */
	WT_ASYNCOP_FREE,	/* Able to be allocated to user */
	WT_ASYNCOP_READY,	/* Allocated and ready for user to use */
	WT_ASYNCOP_WORKING	/* Operation in progress by worker */
} WT_ASYNC_STATE;

typedef enum {
	WT_ASYNC_FLUSH_NONE=0,		/* No flush in progress */
	WT_ASYNC_FLUSH_COMPLETE,	/* Notify flush caller it's done */
	WT_ASYNC_FLUSH_IN_PROGRESS,	/* Prevent other callers */
	WT_ASYNC_FLUSHING		/* Notify workers */
} WT_ASYNC_FLUSH_STATE;

#define	MAX_ASYNC_SLEEP_USECS	100000	/* Maximum sleep waiting for work */
#define	MAX_ASYNC_YIELD		200	/* Maximum number of yields for work */

#define	O2C(op)	((WT_CONNECTION_IMPL *)(op)->iface.connection)
#define	O2S(op)								\
    (((WT_CONNECTION_IMPL *)(op)->iface.connection)->default_session)
/*
 * WT_ASYNC_FORMAT --
 *	The URI/config/format cache.
 */
struct __wt_async_format {
	STAILQ_ENTRY(__wt_async_format) q;
	const char	*config;
	uint64_t	cfg_hash;		/* Config hash */
	const char	*uri;
	uint64_t	uri_hash;		/* URI hash */
	const char	*key_format;
	const char	*value_format;
};

/*
 * WT_ASYNC_OP_IMPL --
 *	Implementation of the WT_ASYNC_OP.
 */
struct __wt_async_op_impl {
	WT_ASYNC_OP	iface;

	WT_ASYNC_CALLBACK	*cb;

	uint32_t	internal_id;	/* Array position id. */
	uint64_t	unique_id;	/* Unique identifier. */

	WT_ASYNC_FORMAT *format;	/* Format structure */
	WT_ASYNC_STATE	state;		/* Op state */
	WT_ASYNC_OPTYPE	optype;		/* Operation type */
};

/*
 * Definition of the async subsystem.
 */
struct __wt_async {
	/*
	 * Ops array protected by the ops_lock.
	 */
	WT_SPINLOCK		 ops_lock;      /* Locked: ops array */
	WT_ASYNC_OP_IMPL	 *async_ops;	/* Async ops */
#define	OPS_INVALID_INDEX	0xffffffff
	uint32_t		 ops_index;	/* Active slot index */
	uint64_t		 op_id;		/* Unique ID counter */
	WT_ASYNC_OP_IMPL	 **async_queue;	/* Async ops work queue */
	uint32_t		 async_qsize;	/* Async work queue size */
	/*
	 * We need to have two head and tail values.  All but one is
	 * maintained as an ever increasing value to ease wrap around.
	 *
	 * alloc_head: the next one to allocate for producers.
	 * head: the current head visible to consumers.
	 * head is always <= alloc_head.
	 * alloc_tail: the next slot for consumers to dequeue.
	 * alloc_tail is always <= head.
	 * tail_slot: the last slot consumed.
	 * A producer may need wait for tail_slot to advance.
	 */
	uint64_t		 alloc_head;	/* Next slot to enqueue */
	uint64_t		 head;		/* Head visible to worker */
	uint64_t		 alloc_tail;	/* Next slot to dequeue */
	uint64_t		 tail_slot;	/* Worker slot consumed */

	STAILQ_HEAD(__wt_async_format_qh, __wt_async_format) formatqh;
	int			 cur_queue;	/* Currently enqueued */
	int			 max_queue;	/* Maximum enqueued */
	WT_ASYNC_FLUSH_STATE	 flush_state;	/* Queue flush state */
	/* Notify any waiting threads when flushing is done. */
	WT_CONDVAR		*flush_cond;
	WT_ASYNC_OP_IMPL	 flush_op;	/* Special flush op */
	uint32_t		 flush_count;	/* Worker count */
	uint64_t		 flush_gen;	/* Flush generation number */

#define	WT_ASYNC_MAX_WORKERS	20
	WT_SESSION_IMPL		*worker_sessions[WT_ASYNC_MAX_WORKERS];
					/* Async worker threads */
	wt_thread_t		 worker_tids[WT_ASYNC_MAX_WORKERS];

	uint32_t		 flags;	/* Currently unused. */
};

/*
 * WT_ASYNC_CURSOR --
 *	Async container for a cursor.  Each async worker thread
 *	has a cache of async cursors to reuse for operations.
 */
struct __wt_async_cursor {
	STAILQ_ENTRY(__wt_async_cursor) q;	/* Worker cache */
	uint64_t	cfg_hash;		/* Config hash */
	uint64_t	uri_hash;		/* URI hash */
	WT_CURSOR	*c;			/* WT cursor */
};

/*
 * WT_ASYNC_WORKER_STATE --
 *	State for an async worker thread.
 */
struct __wt_async_worker_state {
	uint32_t	id;
	STAILQ_HEAD(__wt_cursor_qh, __wt_async_cursor)	cursorqh;
	uint32_t	num_cursors;
};
