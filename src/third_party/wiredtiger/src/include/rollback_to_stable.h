/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * Helper macros for finer-grained RTS verbose messaging categories.
 */
#define WT_RTS_VERB_TAG_END "[END] "
#define WT_RTS_VERB_TAG_FILE_SKIP "[FILE_SKIP] "
#define WT_RTS_VERB_TAG_HS_ABORT_STOP "[HS_ABORT_STOP] "
#define WT_RTS_VERB_TAG_HS_GT_ONDISK "[HS_GT_ONDISK] "
#define WT_RTS_VERB_TAG_HS_RESTORE_TOMBSTONE "[HS_RESTORE_TOMBSTONE] "
#define WT_RTS_VERB_TAG_HS_STOP_OBSOLETE "[HS_STOP_OBSOLETE] "
#define WT_RTS_VERB_TAG_HS_TREE_FINAL_PASS "[HS_TREE_FINAL_PASS] "
#define WT_RTS_VERB_TAG_HS_TREE_ROLLBACK "[HS_TREE_ROLLBACK] "
#define WT_RTS_VERB_TAG_HS_TREE_SKIP "[HS_TREE_SKIP] "
#define WT_RTS_VERB_TAG_HS_TRUNCATED "[HS_TRUNCATED] "
#define WT_RTS_VERB_TAG_HS_TRUNCATING "[HS_TRUNCATING] "
#define WT_RTS_VERB_TAG_HS_UPDATE_ABORT "[HS_UPDATE_ABORT] "
#define WT_RTS_VERB_TAG_HS_UPDATE_REMOVE "[HS_UPDATE_REMOVE] "
#define WT_RTS_VERB_TAG_HS_UPDATE_RESTORED "[HS_UPDATE_RESTORED] "
#define WT_RTS_VERB_TAG_HS_UPDATE_VALID "[HS_UPDATE_VALID] "
#define WT_RTS_VERB_TAG_INIT "[INIT] "
#define WT_RTS_VERB_TAG_INSERT_LIST_CHECK "[INSERT_LIST_CHECK] "
#define WT_RTS_VERB_TAG_INSERT_LIST_UPDATE_ABORT "[INSERT_LIST_UPDATE_ABORT] "
#define WT_RTS_VERB_TAG_KEY_CLEAR_REMOVE "[KEY_CLEAR_REMOVE] "
#define WT_RTS_VERB_TAG_KEY_REMOVED "[KEY_REMOVED] "
#define WT_RTS_VERB_TAG_NO_STABLE "[NO_STABLE] "
#define WT_RTS_VERB_TAG_ONDISK_ABORT_CHECK "[ONDISK_ABORT_CHECK] "
#define WT_RTS_VERB_TAG_ONDISK_ABORT_TW "[ONDISK_ABORT_TW] "
#define WT_RTS_VERB_TAG_ONDISK_KEY_ROLLBACK "[ONDISK_KEY_ROLLBACK] "
#define WT_RTS_VERB_TAG_ONDISK_KV_FIX "[ONDISK_KV_FIX] "
#define WT_RTS_VERB_TAG_ONDISK_KV_REMOVE "[ONDISK_KV_REMOVE] "
#define WT_RTS_VERB_TAG_PAGE_ABORT_CHECK "[PAGE_ABORT_CHECK] "
#define WT_RTS_VERB_TAG_PAGE_DELETE "[PAGE_DELETE] "
#define WT_RTS_VERB_TAG_PAGE_ROLLBACK "[PAGE_ROLLBACK] "
#define WT_RTS_VERB_TAG_PAGE_UNSKIPPED "[PAGE_UNSKIPPED] "
#define WT_RTS_VERB_TAG_RECOVER_CKPT "[RECOVER_CKPT] "
#define WT_RTS_VERB_TAG_SHUTDOWN_RTS "[SHUTDOWN_RTS] "
#define WT_RTS_VERB_TAG_SKIP_DAMAGE "[SKIP_DAMAGE] "
#define WT_RTS_VERB_TAG_SKIP_DEL "[SKIP_DEL] "
#define WT_RTS_VERB_TAG_SKIP_DEL_NULL "[SKIP_DEL_NULL] "
#define WT_RTS_VERB_TAG_SKIP_UNMODIFIED "[SKIP_UNMODIFIED] "
#define WT_RTS_VERB_TAG_STABLE_PG_WALK_SKIP "[STABLE_PG_WALK_SKIP] "
#define WT_RTS_VERB_TAG_STABLE_UPDATE_FOUND "[STABLE_UPDATE_FOUND] "
#define WT_RTS_VERB_TAG_TREE "[TREE] "
#define WT_RTS_VERB_TAG_TREE_LOGGING "[TREE_LOGGING] "
#define WT_RTS_VERB_TAG_TREE_OBJECT_LOG "[TREE_OBJECT_LOG] "
#define WT_RTS_VERB_TAG_TREE_SKIP "[TREE_SKIP] "
#define WT_RTS_VERB_TAG_UPDATE_ABORT "[UPDATE_ABORT] "
#define WT_RTS_VERB_TAG_UPDATE_CHAIN_VERIFY "[UPDATE_CHAIN_VERIFY] "
#define WT_RTS_VERB_TAG_WAIT_THREADS "[WAIT_THREADS] "

#define WT_CHECK_RECOVERY_FLAG_TXNID(session, txnid)                                           \
    (F_ISSET(S2C(session), WT_CONN_RECOVERING) && S2C(session)->recovery_ckpt_snap_min != 0 && \
      (txnid) >= S2C(session)->recovery_ckpt_snap_min)

/* Enable rollback to stable verbose messaging during recovery. */
#define WT_VERB_RECOVERY_RTS(session)                                                              \
    (F_ISSET(S2C(session), WT_CONN_RECOVERING) ?                                                   \
        WT_DECL_VERBOSE_MULTI_CATEGORY(((WT_VERBOSE_CATEGORY[]){WT_VERB_RECOVERY, WT_VERB_RTS})) : \
        WT_DECL_VERBOSE_MULTI_CATEGORY(((WT_VERBOSE_CATEGORY[]){WT_VERB_RTS})))

/* Increment a connection stat, or the dry-run version if needed. */
#define WT_RTS_STAT_CONN_INCR(session, stat)           \
    do {                                               \
        if (!S2C(session)->rts->dryrun)                \
            WT_STAT_CONN_INCR(session, stat);          \
        else                                           \
            WT_STAT_CONN_INCR(session, stat##_dryrun); \
    } while (0)

/* Increment a connection and data handle stat, or the dry-run version if needed. */
#define WT_RTS_STAT_CONN_DATA_INCR(session, stat)           \
    do {                                                    \
        if (!S2C(session)->rts->dryrun)                     \
            WT_STAT_CONN_DSRC_INCR(session, stat);          \
        else                                                \
            WT_STAT_CONN_DSRC_INCR(session, stat##_dryrun); \
    } while (0)

#define WT_RTS_MAX_WORKERS 10
/*
 * WT_RTS_WORK_UNIT --
 *  RTS thread operating work unit.
 */
struct __wt_rts_work_unit {
    TAILQ_ENTRY(__wt_rts_work_unit) q; /* Worker unit queue */
    char *uri;
    wt_timestamp_t rollback_timestamp;
};

/*
 * WT_ROLLBACK_TO_STABLE --
 *	Rollback to stable singleton, contains the interface to rollback to stable along
 *	with context used by rollback to stable.
 */
struct __wt_rollback_to_stable {
    /* Methods. */
    int (*rollback_to_stable_one)(WT_SESSION_IMPL *, const char *, bool *);
    int (*rollback_to_stable)(WT_SESSION_IMPL *, const char *[], bool);

    /* RTS thread information. */
    WT_THREAD_GROUP thread_group;
    uint32_t threads_num;

    /* The configuration of RTS worker threads at the connection level. */
    uint32_t cfg_threads_num;

    /* Locked: RTS system work queue. */
    TAILQ_HEAD(__wt_rts_qh, __wt_rts_work_unit) rtsqh;
    WT_SPINLOCK rts_lock; /* RTS work queue spinlock */

    /* Configuration. */
    bool dryrun;
};

/*
 * WT_RTS_COOKIE --
 *   State passed through to callbacks during the session walk logic when checking for active
 *   transactions or open cursors before an RTS.
 */
struct __wt_rts_cookie {
    bool ret_txn_active;
    bool ret_cursor_active;
};
