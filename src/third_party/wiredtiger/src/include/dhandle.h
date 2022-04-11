/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Helpers for calling a function with a data handle in session->dhandle then restoring afterwards.
 */
#define WT_WITH_DHANDLE(s, d, e)                        \
    do {                                                \
        WT_DATA_HANDLE *__saved_dhandle = (s)->dhandle; \
        (s)->dhandle = (d);                             \
        e;                                              \
        (s)->dhandle = __saved_dhandle;                 \
    } while (0)

#define WT_WITH_BTREE(s, b, e) WT_WITH_DHANDLE(s, (b)->dhandle, e)

/* Call a function without the caller's data handle, restore afterwards. */
#define WT_WITHOUT_DHANDLE(s, e) WT_WITH_DHANDLE(s, NULL, e)

/*
 * Call a function with the caller's data handle, restore it afterwards in case it is overwritten.
 */
#define WT_SAVE_DHANDLE(s, e) WT_WITH_DHANDLE(s, (s)->dhandle, e)

/* Check if a handle is inactive. */
#define WT_DHANDLE_INACTIVE(dhandle) \
    (F_ISSET(dhandle, WT_DHANDLE_DEAD) || !F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE | WT_DHANDLE_OPEN))

/* Check if a handle could be reopened. */
#define WT_DHANDLE_CAN_REOPEN(dhandle) \
    (!F_ISSET(dhandle, WT_DHANDLE_DEAD | WT_DHANDLE_DROPPED) && F_ISSET(dhandle, WT_DHANDLE_OPEN))

/* The metadata cursor's data handle. */
#define WT_SESSION_META_DHANDLE(s) (((WT_CURSOR_BTREE *)((s)->meta_cursor))->dhandle)

#define WT_DHANDLE_ACQUIRE(dhandle) (void)__wt_atomic_add32(&(dhandle)->session_ref, 1)

#define WT_DHANDLE_RELEASE(dhandle) (void)__wt_atomic_sub32(&(dhandle)->session_ref, 1)

#define WT_DHANDLE_NEXT(session, dhandle, head, field)                                     \
    do {                                                                                   \
        WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST)); \
        if ((dhandle) == NULL)                                                             \
            (dhandle) = TAILQ_FIRST(head);                                                 \
        else {                                                                             \
            WT_DHANDLE_RELEASE(dhandle);                                                   \
            (dhandle) = TAILQ_NEXT(dhandle, field);                                        \
        }                                                                                  \
        if ((dhandle) != NULL)                                                             \
            WT_DHANDLE_ACQUIRE(dhandle);                                                   \
    } while (0)

/*
 * WT_DATA_HANDLE --
 *	A handle for a generic named data source.
 */
struct __wt_data_handle {
    WT_RWLOCK rwlock; /* Lock for shared/exclusive ops */
    TAILQ_ENTRY(__wt_data_handle) q;
    TAILQ_ENTRY(__wt_data_handle) hashq;

    const char *name;        /* Object name as a URI */
    uint64_t name_hash;      /* Hash of name */
    const char *checkpoint;  /* Checkpoint name (or NULL) */
    const char **cfg;        /* Configuration information */
    const char *meta_base;   /* Base metadata configuration */
    size_t meta_base_length; /* Base metadata length */
#ifdef HAVE_DIAGNOSTIC
    const char *orig_meta_base; /* Copy of the base metadata configuration */
#endif
    /*
     * Sessions holding a connection's data handle will have a non-zero reference count; sessions
     * using a connection's data handle will have a non-zero in-use count. Instances of cached
     * cursors referencing the data handle appear in session_cache_ref.
     */
    uint32_t session_ref;          /* Sessions referencing this handle */
    int32_t session_inuse;         /* Sessions using this handle */
    uint32_t excl_ref;             /* Refs of handle by excl_session */
    uint64_t timeofdeath;          /* Use count went to 0 */
    WT_SESSION_IMPL *excl_session; /* Session with exclusive use, if any */

    WT_DATA_SOURCE *dsrc; /* Data source for this handle */
    void *handle;         /* Generic handle */

    enum {
        WT_DHANDLE_TYPE_BTREE,
        WT_DHANDLE_TYPE_TABLE,
        WT_DHANDLE_TYPE_TIERED,
        WT_DHANDLE_TYPE_TIERED_TREE
    } type;

#define WT_DHANDLE_BTREE(dhandle) \
    ((dhandle)->type == WT_DHANDLE_TYPE_BTREE || (dhandle)->type == WT_DHANDLE_TYPE_TIERED)

    bool compact_skip; /* If the handle failed to compact */

    /*
     * Data handles can be closed without holding the schema lock; threads walk the list of open
     * handles, operating on them (checkpoint is the best example). To avoid sources disappearing
     * underneath checkpoint, lock the data handle when closing it.
     */
    WT_SPINLOCK close_lock; /* Lock to close the handle */

    /* Data-source statistics */
    WT_DSRC_STATS *stats[WT_COUNTER_SLOTS];
    WT_DSRC_STATS *stat_array;

/*
 * Flags values over 0xfff are reserved for WT_BTREE_*. This lets us combine the dhandle and btree
 * flags when we need, for example, to pass both sets in a function call.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_DHANDLE_DEAD 0x001u         /* Dead, awaiting discard */
#define WT_DHANDLE_DISCARD 0x002u      /* Close on release */
#define WT_DHANDLE_DISCARD_KILL 0x004u /* Mark dead on release */
#define WT_DHANDLE_DROPPED 0x008u      /* Handle is dropped */
#define WT_DHANDLE_EVICTED 0x010u      /* Btree is evicted (advisory) */
#define WT_DHANDLE_EXCLUSIVE 0x020u    /* Exclusive access */
#define WT_DHANDLE_HS 0x040u           /* History store table */
#define WT_DHANDLE_IS_METADATA 0x080u  /* Metadata handle */
#define WT_DHANDLE_LOCK_ONLY 0x100u    /* Handle only used as a lock */
#define WT_DHANDLE_OPEN 0x200u         /* Handle is open */
                                       /* AUTOMATIC FLAG VALUE GENERATION STOP 12 */
    uint32_t flags;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_DHANDLE_TS_ALWAYS 0x01u             /* Handle using always checking. */
#define WT_DHANDLE_TS_ASSERT_READ_ALWAYS 0x02u /* Assert read always checking. */
#define WT_DHANDLE_TS_ASSERT_READ_NEVER 0x04u  /* Assert read never checking. */
#define WT_DHANDLE_TS_ASSERT_WRITE 0x08u       /* Assert write checking. */
#define WT_DHANDLE_TS_MIXED_MODE 0x10u         /* Handle using mixed mode timestamps checking. */
#define WT_DHANDLE_TS_NEVER 0x20u              /* Handle never using timestamps checking. */
#define WT_DHANDLE_TS_ORDERED 0x40u            /* Handle using ordered timestamps checking. */
                                               /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t ts_flags;
};
