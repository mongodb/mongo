/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define WT_THREAD_PAUSE 10 /* Thread pause timeout in seconds */

/*
 * WT_THREAD --
 *	Encapsulation of a thread that belongs to a thread group.
 */
struct __wt_thread {
    WT_SESSION_IMPL *session;
    u_int id;
    wt_thread_t tid;

/*
 * WT_THREAD and thread-group function flags, merged because WT_THREAD_PANIC_FAIL appears in both
 * groups.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_THREAD_ACTIVE 0x1u     /* Thread is active or paused */
#define WT_THREAD_CAN_WAIT 0x2u   /* WT_SESSION_CAN_WAIT */
#define WT_THREAD_PANIC_FAIL 0x4u /* Panic if the thread fails */
#define WT_THREAD_RUN 0x8u        /* Thread is running */
                                  /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;

    /*
     * Condition signalled when a thread becomes active. Paused threads wait on this condition.
     */
    WT_CONDVAR *pause_cond;

    /* The check function used by all threads. */
    bool (*chk_func)(WT_SESSION_IMPL *session);
    /* The runner function used by all threads. */
    int (*run_func)(WT_SESSION_IMPL *session, WT_THREAD *context);
    /* The stop function used by all threads. */
    int (*stop_func)(WT_SESSION_IMPL *session, WT_THREAD *context);
};

/*
 * WT_THREAD_GROUP --
 *	Encapsulation of a group of utility threads.
 */
struct __wt_thread_group {
    uint32_t alloc;           /* Size of allocated group */
    uint32_t max;             /* Max threads in group */
    uint32_t min;             /* Min threads in group */
    uint32_t current_threads; /* Number of active threads */

    const char *name; /* Name */

    WT_RWLOCK lock; /* Protects group changes */

    /*
     * Condition signalled when wanting to wake up threads that are part of the group - for example
     * when shutting down. This condition can also be used by group owners to ensure state changes
     * are noticed.
     */
    WT_CONDVAR *wait_cond;

    /*
     * The threads need to be held in an array of arrays, not an array of structures because the
     * array is reallocated as it grows, which causes threads to loose track of their context is
     * realloc moves the memory.
     */
    WT_THREAD **threads;

    /* The check function used by all threads. */
    bool (*chk_func)(WT_SESSION_IMPL *session);
    /* The runner function used by all threads. */
    int (*run_func)(WT_SESSION_IMPL *session, WT_THREAD *context);
    /* The stop function used by all threads. May be NULL */
    int (*stop_func)(WT_SESSION_IMPL *session, WT_THREAD *context);
};
