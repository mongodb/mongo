/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#define WT_CHECKPOINT_SESSION_FLAGS (WT_SESSION_CAN_WAIT | WT_SESSION_IGNORE_CACHE_SIZE)

/*
 * Inactive should always be 0. Other states are roughly ordered by appearance in the checkpoint
 * life cycle.
 */
typedef enum {
    WT_CHECKPOINT_STATE_INACTIVE,
    WT_CHECKPOINT_STATE_APPLY_META,
    WT_CHECKPOINT_STATE_APPLY_BTREE,
    WT_CHECKPOINT_STATE_UPDATE_OLDEST,
    WT_CHECKPOINT_STATE_SYNC_FILE,
    WT_CHECKPOINT_STATE_EVICT_FILE,
    WT_CHECKPOINT_STATE_BM_SYNC,
    WT_CHECKPOINT_STATE_RESOLVE,
    WT_CHECKPOINT_STATE_POSTPROCESS,
    WT_CHECKPOINT_STATE_HS,
    WT_CHECKPOINT_STATE_HS_SYNC,
    WT_CHECKPOINT_STATE_COMMIT,
    WT_CHECKPOINT_STATE_META_CKPT,
    WT_CHECKPOINT_STATE_META_SYNC,
    WT_CHECKPOINT_STATE_ROLLBACK,
    WT_CHECKPOINT_STATE_LOG,
    WT_CHECKPOINT_STATE_CKPT_TREE,
    WT_CHECKPOINT_STATE_RUNNING,
    WT_CHECKPOINT_STATE_ESTABLISH,
    WT_CHECKPOINT_STATE_START_TXN
} WT_CHECKPOINT_STATE;

struct __wt_checkpoint_cleanup {
    WT_SESSION_IMPL *session; /* checkpoint cleanup session */
    wt_thread_t tid;          /* checkpoint cleanup thread */
    int tid_set;              /* checkpoint cleanup thread set */
    WT_CONDVAR *cond;         /* checkpoint cleanup wait mutex */
    uint64_t interval;        /* Checkpoint cleanup interval */
};
