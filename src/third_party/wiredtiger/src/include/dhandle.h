/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

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
#define WT_DHANDLE_CAN_REOPEN(dhandle)                                                           \
    (F_MASK(                                                                                     \
       dhandle, WT_DHANDLE_DEAD | WT_DHANDLE_DROPPED | WT_DHANDLE_OPEN | WT_DHANDLE_OUTDATED) == \
      WT_DHANDLE_OPEN)

/* The metadata cursor's data handle. */
#define WT_SESSION_META_DHANDLE(s) (((WT_CURSOR_BTREE *)((s)->meta_cursor))->dhandle)

#define WT_DHANDLE_ACQUIRE(dhandle) (void)__wt_atomic_add32(&(dhandle)->references, 1)

#define WT_DHANDLE_RELEASE(dhandle) (void)__wt_atomic_sub32(&(dhandle)->references, 1)

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

#define WT_DHANDLE_IS_CHECKPOINT(dhandle) ((dhandle)->checkpoint != NULL)

/*
 * WT_WITH_DHANDLE_WRITE_LOCK_NOWAIT --
 *	Try to acquire write lock for the session's current dhandle, perform an operation, drop the
 *  lock.
 */
#define WT_WITH_DHANDLE_WRITE_LOCK_NOWAIT(session, ret, op)               \
    do {                                                                  \
        if (((ret) = __wt_session_dhandle_try_writelock(session)) == 0) { \
            op;                                                           \
            __wt_session_dhandle_writeunlock(session);                    \
        }                                                                 \
    } while (0)

enum wt_dhandle_type {
    WT_DHANDLE_TYPE_BTREE = 0,
    WT_DHANDLE_TYPE_LAYERED,
    WT_DHANDLE_TYPE_TABLE,
    WT_DHANDLE_TYPE_TIERED,
    WT_DHANDLE_TYPE_TIERED_TREE
};
/* Number of values above. */
#define WT_DHANDLE_TYPE_NUM (1 + WT_DHANDLE_TYPE_TIERED_TREE)

/*
 * WT_DATA_HANDLE --
 *	A handle for a generic named data source.
 */
struct __wt_data_handle {
    WT_RWLOCK rwlock; /* Lock for shared/exclusive ops */
    TAILQ_ENTRY(__wt_data_handle) q;
    TAILQ_ENTRY(__wt_data_handle) hashq;

    const char *name;           /* Object name as a URI */
    uint64_t name_hash;         /* Hash of name */
    const char *checkpoint;     /* Checkpoint name (or NULL) */
    int64_t checkpoint_order;   /* Checkpoint order number, when applicable */
    const char **cfg;           /* Configuration information */
    const char *meta_base;      /* Base metadata configuration */
    uint64_t meta_hash;         /* Base metadata hash */
    struct timespec base_upd;   /* Time of last metadata update with meta base */
    const char *orig_meta_base; /* Copy of the base metadata configuration */
    uint64_t orig_meta_hash;    /* Copy of base metadata hash */
    struct timespec orig_upd;   /* Time of original setup of meta base */
    /*
     * Sessions holding a connection's data handle and queued tiered storage work units will hold
     * references; sessions using a connection's data handle will have a non-zero in-use count.
     * Instances of cached cursors referencing the data handle appear in session_cache_ref.
     */
    wt_shared uint32_t references;   /* References to this handle */
    wt_shared int32_t session_inuse; /* Sessions using this handle */
    uint32_t excl_ref;               /* Refs of handle by excl_session */
    uint64_t timeofdeath;            /* Use count went to 0 */
    WT_SESSION_IMPL *excl_session;   /* Session with exclusive use, if any */

    WT_DATA_SOURCE *dsrc; /* Data source for this handle */
    void *handle;         /* Generic handle */

    wt_shared enum wt_dhandle_type type;

#define WT_DHANDLE_BTREE(dhandle)                                        \
    (__wt_atomic_load_enum(&(dhandle)->type) == WT_DHANDLE_TYPE_BTREE || \
      __wt_atomic_load_enum(&(dhandle)->type) == WT_DHANDLE_TYPE_TIERED)

    bool compact_skip; /* If the handle failed to compact */

    /*
     * Data handles can be closed without holding the schema lock; threads walk the list of open
     * handles, operating on them (checkpoint is the best example). To avoid sources disappearing
     * underneath checkpoint, lock the data handle when closing it.
     */
    WT_SPINLOCK close_lock; /* Lock to close the handle */

    /* Data-source statistics */
    WT_DSRC_STATS *stats[WT_STAT_DSRC_COUNTER_SLOTS];
    WT_DSRC_STATS *stat_array;

/*
 * Flags values over 0xfff are reserved for WT_BTREE_*. This lets us combine the dhandle and btree
 * flags when we need, for example, to pass both sets in a function call. These flags can only be
 * changed when a dhandle is locked exclusively.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_DHANDLE_DEAD 0x001u         /* Dead, awaiting discard */
#define WT_DHANDLE_DISAGG_META 0x002u  /* Disaggregated storage metadata */
#define WT_DHANDLE_DISCARD 0x004u      /* Close on release */
#define WT_DHANDLE_DISCARD_KILL 0x008u /* Mark dead on release */
#define WT_DHANDLE_DROPPED 0x010u      /* Handle is dropped */
#define WT_DHANDLE_EXCLUSIVE 0x020u    /* Exclusive access */
#define WT_DHANDLE_HS 0x040u           /* History store table */
#define WT_DHANDLE_IS_METADATA 0x080u  /* Metadata handle */
#define WT_DHANDLE_LOCK_ONLY 0x100u    /* Handle only used as a lock */
#define WT_DHANDLE_OPEN 0x200u         /* Handle is open */
#define WT_DHANDLE_OUTDATED 0x400u     /* Handle is outdated */
                                       /* AUTOMATIC FLAG VALUE GENERATION STOP 12 */
    uint16_t flags;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_DHANDLE_TS_ASSERT_READ_ALWAYS 0x1u /* Assert read always checking. */
#define WT_DHANDLE_TS_ASSERT_READ_NEVER 0x2u  /* Assert read never checking. */
#define WT_DHANDLE_TS_NEVER 0x4u              /* Handle never using timestamps checking. */
#define WT_DHANDLE_TS_ORDERED 0x8u            /* Handle using ordered timestamps checking. */
                                              /* AUTOMATIC FLAG VALUE GENERATION STOP 16 */
    uint16_t ts_flags;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_DHANDLE_LOCK_WRITE 0x1u /* Write lock is acquired. */
                                   /* AUTOMATIC FLAG VALUE GENERATION STOP 16 */
    uint16_t lock_flags;

    /* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_DHANDLE_ADVISORY_EVICTED 0x1u /* Btree is evicted */
                                         /* AUTOMATIC FLAG VALUE GENERATION STOP 16 */
    uint16_t advisory_flags;
};
