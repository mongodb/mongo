/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#define WT_CHECKPOINT_SESSION_FLAGS (WT_SESSION_CAN_WAIT | WT_SESSION_IGNORE_CACHE_SIZE)

#define WT_CHECKPOINT_STATE_INACTIVE 0
#define WT_CHECKPOINT_STATE_APPLY_META 1
#define WT_CHECKPOINT_STATE_APPLY_BTREE 2
#define WT_CHECKPOINT_STATE_UPDATE_OLDEST 3
#define WT_CHECKPOINT_STATE_SYNC_FILE 4
#define WT_CHECKPOINT_STATE_EVICT_FILE 5
#define WT_CHECKPOINT_STATE_BM_SYNC 6
#define WT_CHECKPOINT_STATE_RESOLVE 7
#define WT_CHECKPOINT_STATE_POSTPROCESS 8
#define WT_CHECKPOINT_STATE_HS 9
#define WT_CHECKPOINT_STATE_HS_SYNC 10
#define WT_CHECKPOINT_STATE_COMMIT 11
#define WT_CHECKPOINT_STATE_META_CKPT 12
#define WT_CHECKPOINT_STATE_META_SYNC 13
#define WT_CHECKPOINT_STATE_ROLLBACK 14
#define WT_CHECKPOINT_STATE_LOG 15
#define WT_CHECKPOINT_STATE_RUNNING 16
#define WT_CHECKPOINT_STATE_ESTABLISH 17
#define WT_CHECKPOINT_STATE_START_TXN 18
#define WT_CHECKPOINT_STATE_CKPT_TREE 19

struct __wt_checkpoint_cleanup {
    WT_SESSION_IMPL *session; /* checkpoint cleanup session */
    wt_thread_t tid;          /* checkpoint cleanup thread */
    int tid_set;              /* checkpoint cleanup thread set */
    WT_CONDVAR *cond;         /* checkpoint cleanup wait mutex */
    uint64_t interval;        /* Checkpoint cleanup interval */
};
