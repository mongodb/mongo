/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_THREAD --
 *	Encapsulation of a thread that belongs to a thread group.
 */
struct __wt_thread {
	WT_SESSION_IMPL *session;
	u_int id;
	wt_thread_t tid;

	/*
	 * WT_THREAD and thread-group function flags, merged because
	 * WT_THREAD_PANIC_FAIL appears in both groups.
	 */
#define	WT_THREAD_CAN_WAIT	0x01	/* WT_SESSION_CAN_WAIT */
#define	WT_THREAD_PANIC_FAIL	0x02	/* panic if the thread fails */
#define	WT_THREAD_RUN		0x04	/* thread is running */
	uint32_t flags;

	/* The runner function used by all threads. */
	int (*run_func)(WT_SESSION_IMPL *session, WT_THREAD *context);
};

/*
 * WT_THREAD_GROUP --
 *	Encapsulation of a group of utility threads.
 */
struct __wt_thread_group {
	uint32_t	 alloc;		/* Size of allocated group */
	uint32_t	 max;		/* Max threads in group */
	uint32_t	 min;		/* Min threads in group */
	uint32_t	 current_threads;/* Number of active threads */

	const char	*name;		/* Name */

	WT_RWLOCK	lock;		/* Protects group changes */

	/*
	 * Condition signalled when wanting to wake up threads that are
	 * part of the group - for example when shutting down. This condition
	 * can also be used by group owners to ensure state changes are noticed.
	 */
	WT_CONDVAR      *wait_cond;

	/*
	 * The threads need to be held in an array of arrays, not an array of
	 * structures because the array is reallocated as it grows, which
	 * causes threads to loose track of their context is realloc moves the
	 * memory.
	 */
	WT_THREAD **threads;

	/* The runner function used by all threads. */
	int (*run_func)(WT_SESSION_IMPL *session, WT_THREAD *context);
};
