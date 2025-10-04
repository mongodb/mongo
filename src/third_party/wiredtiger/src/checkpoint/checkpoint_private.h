/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 * All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#define WTI_CHECKPOINT_SESSION_FLAGS (WT_SESSION_CAN_WAIT | WT_SESSION_IGNORE_CACHE_SIZE)
#define WTI_CKPT_FOREACH_NAME_OR_ORDER(ckptbase, ckpt) \
    for ((ckpt) = (ckptbase); (ckpt)->name != NULL || (ckpt)->order != 0; ++(ckpt))

/*
 * Inactive should always be 0. Other states are roughly ordered by appearance in the checkpoint
 * life cycle.
 */
typedef enum {
    WTI_CHECKPOINT_STATE_INACTIVE,
    WTI_CHECKPOINT_STATE_APPLY_META,
    WTI_CHECKPOINT_STATE_APPLY_BTREE,
    WTI_CHECKPOINT_STATE_UPDATE_OLDEST,
    WTI_CHECKPOINT_STATE_SYNC_FILE,
    WTI_CHECKPOINT_STATE_EVICT_FILE,
    WTI_CHECKPOINT_STATE_BM_SYNC,
    WTI_CHECKPOINT_STATE_RESOLVE,
    WTI_CHECKPOINT_STATE_POSTPROCESS,
    WTI_CHECKPOINT_STATE_HS,
    WTI_CHECKPOINT_STATE_HS_SYNC,
    WTI_CHECKPOINT_STATE_COMMIT,
    WTI_CHECKPOINT_STATE_META_CKPT,
    WTI_CHECKPOINT_STATE_META_SYNC,
    WTI_CHECKPOINT_STATE_ROLLBACK,
    WTI_CHECKPOINT_STATE_LOG,
    WTI_CHECKPOINT_STATE_CKPT_TREE,
    WTI_CHECKPOINT_STATE_ACTIVE,
    WTI_CHECKPOINT_STATE_ESTABLISH,
    WTI_CHECKPOINT_STATE_START_TXN
} WTI_CHECKPOINT_STATE;

/*
 * WTI_CKPT_HANDLE_STATS --
 *     Statistics related to handles.
 */
struct __wti_ckpt_handle_stats {
    uint64_t apply;           /* handles applied */
    uint64_t apply_time;      /* applied handles gather time */
    uint64_t drop;            /* handle checkpoints dropped */
    uint64_t drop_time;       /* handle checkpoints dropped time */
    uint64_t lock;            /* handles locked */
    uint64_t lock_time;       /* handles locked time */
    uint64_t meta_check;      /* handles metadata check */
    uint64_t meta_check_time; /* handles metadata check time */
    uint64_t skip;            /* handles skipped */
    uint64_t skip_time;       /* skipped handles gather time */
};

/*
 * WTI_CKPT_PROGRESS --
 *     Checkpoint progress.
 */
struct __wti_ckpt_progress {
    uint64_t msg_count;
    uint64_t write_bytes;
    uint64_t write_pages;
};

/*
 * WTI_CKPT_THREAD --
 *     Checkpoint server information.
 */
struct __wti_ckpt_thread {
    WT_CONDVAR *cond;         /* wait mutex */
    WT_SESSION_IMPL *session; /* session associated with thread */
    wt_thread_t tid;          /* thread id */
    bool tid_set;             /* thread set */
#define WT_CKPT_LOGSIZE(conn) (__wt_atomic_loadi64(&(conn)->ckpt.server.logsize) != 0)
    wt_shared wt_off_t logsize; /* thread log size period */
    bool signalled;             /* thread signalled */
    uint64_t usecs;             /* thread timer */
};

/*
 * WTI_CKPT_TIMER --
 *     Time-related statistics.
 */
struct __wti_ckpt_timer {
    struct timespec timer_end;
    struct timespec timer_start;
    uint64_t max;
    uint64_t min;
    uint64_t recent;
    uint64_t total;
};

/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

/* DO NOT EDIT: automatically built by prototypes.py: END */
