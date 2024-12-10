/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include "checkpoint_private.h"

struct __wt_ckpt_session {
    WT_SPINLOCK lock; /* Checkpoint spinlock */

    uint64_t write_gen; /* Write generation override, during checkpoint cursor ops */

    /* Checkpoint handles */
    WT_DATA_HANDLE **handle; /* Handle list */
    u_int handle_next;       /* Next empty slot */
    size_t handle_allocated; /* Bytes allocated */

    /* Checkpoint crash. */
    u_int crash_point; /* Crash point in the middle of checkpoint process */

    /* Named checkpoint drop list, during a checkpoint */
    WT_ITEM *drop_list;

    /* Checkpoint time of current checkpoint, during a checkpoint */
    uint64_t current_sec;
};

struct __wt_ckpt_connection {
    WT_SESSION_IMPL *session;       /* Checkpoint thread session */
    wt_thread_t tid;                /* Checkpoint thread */
    bool tid_set;                   /* Checkpoint thread set */
    WT_CONDVAR *cond;               /* Checkpoint wait mutex */
    wt_shared uint64_t most_recent; /* Clock value of most recent checkpoint */
#define WT_CKPT_LOGSIZE(conn) (__wt_atomic_loadi64(&(conn)->ckpt.logsize) != 0)
    wt_shared wt_off_t logsize; /* Checkpoint log size period */
    bool signalled;             /* Checkpoint signalled */

    uint64_t apply;           /* Checkpoint handles applied */
    uint64_t apply_time;      /* Checkpoint applied handles gather time */
    uint64_t drop;            /* Checkpoint handles drop */
    uint64_t drop_time;       /* Checkpoint handles drop time */
    uint64_t lock;            /* Checkpoint handles lock */
    uint64_t lock_time;       /* Checkpoint handles lock time */
    uint64_t meta_check;      /* Checkpoint handles metadata check */
    uint64_t meta_check_time; /* Checkpoint handles metadata check time */
    uint64_t skip;            /* Checkpoint handles skipped */
    uint64_t skip_time;       /* Checkpoint skipped handles gather time */
    uint64_t usecs;           /* Checkpoint timer */

    uint64_t scrub_max; /* Checkpoint scrub time min/max */
    uint64_t scrub_min;
    uint64_t scrub_recent; /* Checkpoint scrub time recent/total */
    uint64_t scrub_total;

    uint64_t prep_max; /* Checkpoint prepare time min/max */
    uint64_t prep_min;
    uint64_t prep_recent; /* Checkpoint prepare time recent/total */
    uint64_t prep_total;
    uint64_t time_max; /* Checkpoint time min/max */
    uint64_t time_min;
    uint64_t time_recent; /* Checkpoint time recent/total */
    uint64_t time_total;

    /* Checkpoint stats and verbosity timers */
    struct timespec prep_end;
    struct timespec prep_start;
    struct timespec timer_start;
    struct timespec timer_scrub_end;

    /* Checkpoint progress message data */
    uint64_t progress_msg_count;
    uint64_t write_bytes;
    uint64_t write_pages;

    /* Last checkpoint connection's base write generation */
    uint64_t last_base_write_gen;
};

/*
 * WT_CKPT_BLOCK_MODS --
 *     Block modifications from an incremental identifier going forward.
 */
struct __wt_ckpt_block_mods {
    const char *id_str;

    WT_ITEM bitstring;
    uint64_t nbits; /* Number of bits in bitstring */

    uint64_t offset; /* Zero bit offset for bitstring */
    uint64_t granularity;
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CKPT_BLOCK_MODS_RENAME 0x1u /* Entry is from a rename */
#define WT_CKPT_BLOCK_MODS_VALID 0x2u  /* Entry is valid */
                                       /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/*
 * WT_CKPT --
 *     Encapsulation of checkpoint information, shared by the metadata, the btree engine, and the
 * block manager.
 */
#define WT_CHECKPOINT "WiredTigerCheckpoint"
#define WT_CKPT_FOREACH(ckptbase, ckpt) for ((ckpt) = (ckptbase); (ckpt)->name != NULL; ++(ckpt))
#define WT_CKPT_FOREACH_NAME_OR_ORDER(ckptbase, ckpt) \
    for ((ckpt) = (ckptbase); (ckpt)->name != NULL || (ckpt)->order != 0; ++(ckpt))

struct __wt_ckpt {
    char *name; /* Name or NULL */

    /*
     * Each internal checkpoint name is appended with a generation to make it a unique name. We're
     * solving two problems: when two checkpoints are taken quickly, the timer may not be unique
     * and/or we can even see time travel on the second checkpoint if we snapshot the time
     * in-between nanoseconds rolling over. Second, if we reset the generational counter when new
     * checkpoints arrive, we could logically re-create specific checkpoints, racing with cursors
     * open on those checkpoints. I can't think of any way to return incorrect results by racing
     * with those cursors, but it's simpler not to worry about it.
     */
    int64_t order; /* Checkpoint order */

    uint64_t sec; /* Wall clock time */

    uint64_t size; /* Checkpoint size */

    uint64_t write_gen;     /* Write generation */
    uint64_t run_write_gen; /* Runtime write generation. */

    char *block_metadata;   /* Block-stored metadata */
    char *block_checkpoint; /* Block-stored checkpoint */

    WT_CKPT_BLOCK_MODS backup_blocks[WT_BLKINCR_MAX];

    WT_TIME_AGGREGATE ta; /* Validity window */

    WT_ITEM addr; /* Checkpoint cookie string */
    WT_ITEM raw;  /* Checkpoint cookie raw */

    void *bpriv; /* Block manager private */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CKPT_ADD 0x01u             /* Checkpoint to be added */
#define WT_CKPT_BLOCK_MODS_LIST 0x02u /* Return list of modified blocks */
#define WT_CKPT_DELETE 0x04u          /* Checkpoint to be deleted */
#define WT_CKPT_FAKE 0x08u            /* Checkpoint is a fake */
#define WT_CKPT_UPDATE 0x10u          /* Checkpoint requires update */
                                      /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/*
 * WT_CKPT_SNAPSHOT --
 *     Snapshot and timestamp information associated with a checkpoint.
 */
struct __wt_ckpt_snapshot {
    uint64_t ckpt_id;
    uint64_t oldest_ts;
    uint64_t stable_ts;
    uint64_t snapshot_write_gen;
    uint64_t snapshot_min;
    uint64_t snapshot_max;
    uint64_t *snapshot_txns;
    uint32_t snapshot_count;
};

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
    WT_CHECKPOINT_STATE_ACTIVE,
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

/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

extern int __wt_checkpoint(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_close(WT_SESSION_IMPL *session, bool final)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_get_handles(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_server_create(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_server_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_sync(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_checkpoint(WT_SESSION_IMPL *session, const char *cfg[], bool waiting)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern void __wt_checkpoint_free(WT_SESSION_IMPL *session, WT_CKPT *ckpt);
extern void __wt_checkpoint_progress(WT_SESSION_IMPL *session, bool closing);
extern void __wt_checkpoint_signal(WT_SESSION_IMPL *session, wt_off_t logsize);
extern void __wt_checkpoint_tree_reconcile_update(WT_SESSION_IMPL *session, WT_TIME_AGGREGATE *ta);
extern void __wt_ckptlist_free(WT_SESSION_IMPL *session, WT_CKPT **ckptbasep);
extern void __wt_ckptlist_saved_free(WT_SESSION_IMPL *session);

#ifdef HAVE_UNITTEST

#endif

/* DO NOT EDIT: automatically built by prototypes.py: END */
