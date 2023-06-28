/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Helper macros for finer-grained RTS verbose messaging categories.
 */
#define WT_RTS_VERB_TAG_END "[END] "
#define WT_RTS_VERB_TAG_FILE_SKIP "[FILE_SKIP] "
#define WT_RTS_VERB_TAG_HS_ABORT_STOP "[HS_ABORT_STOP] "
#define WT_RTS_VERB_TAG_HS_GT_ONDISK "[HS_GT_ONDISK] "
#define WT_RTS_VERB_TAG_HS_RESTORE_TOMBSTONE "[HS_RESTORE_TOMBSTONE] "
#define WT_RTS_VERB_TAG_HS_STOP_OBSOLETE "[HS_STOP_OBSOLETE] "
#define WT_RTS_VERB_TAG_HS_TREE_ROLLBACK "[HS_TREE_ROLLBACK] "
#define WT_RTS_VERB_TAG_HS_TREE_SKIP "[HS_TREE_SKIP] "
#define WT_RTS_VERB_TAG_HS_TRUNCATED "[HS_TRUNCATED] "
#define WT_RTS_VERB_TAG_HS_UPDATE_ABORT "[HS_UPDATE_ABORT] "
#define WT_RTS_VERB_TAG_HS_UPDATE_RESTORED "[HS_UPDATE_RESTORED] "
#define WT_RTS_VERB_TAG_HS_UPDATE_VALID "[HS_UPDATE_VALID] "
#define WT_RTS_VERB_TAG_INIT "[INIT] "
#define WT_RTS_VERB_TAG_KEY_CLEAR_REMOVE "[KEY_CLEAR_REMOVE] "
#define WT_RTS_VERB_TAG_KEY_REMOVED "[KEY_REMOVED] "
#define WT_RTS_VERB_TAG_ONDISK_ABORT_TW "[ONDISK_ABORT_TW] "
#define WT_RTS_VERB_TAG_ONDISK_KEY_ROLLBACK "[ONDISK_KEY_ROLLBACK] "
#define WT_RTS_VERB_TAG_ONDISK_KV_REMOVE "[ONDISK_KV_REMOVE] "
#define WT_RTS_VERB_TAG_PAGE_ABORT_CHECK "[PAGE_ABORT_CHECK] "
#define WT_RTS_VERB_TAG_PAGE_ROLLBACK "[PAGE_ROLLBACK] "
#define WT_RTS_VERB_TAG_RECOVER_CKPT "[RECOVER_CKPT] "
#define WT_RTS_VERB_TAG_SHUTDOWN_RTS "[SHUTDOWN_RTS] "
#define WT_RTS_VERB_TAG_SKIP_DAMAGE "[SKIP_DAMAGE] "
#define WT_RTS_VERB_TAG_SKIP_DEL_NULL "[SKIP_DEL_NULL] "
#define WT_RTS_VERB_TAG_SKIP_UNMODIFIED "[SKIP_UNMODIFIED] "
#define WT_RTS_VERB_TAG_STABLE_PG_WALK_SKIP "[STABLE_PG_WALK_SKIP] "
#define WT_RTS_VERB_TAG_TREE "[TREE] "
#define WT_RTS_VERB_TAG_TREE_LOGGING "[TREE_LOGGING] "
#define WT_RTS_VERB_TAG_TREE_SKIP "[TREE_SKIP] "
#define WT_RTS_VERB_TAG_UPDATE_ABORT "[UPDATE_ABORT] "

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
            WT_STAT_CONN_DATA_INCR(session, stat);          \
        else                                                \
            WT_STAT_CONN_DATA_INCR(session, stat##_dryrun); \
    } while (0)

/*
 * WT_ROLLBACK_TO_STABLE --
 *	Rollback to stable singleton, contains the interface to rollback to stable along
 *	with context used by rollback to stable.
 */
struct __wt_rollback_to_stable {
    /* Methods. */
    int (*rollback_to_stable_one)(WT_SESSION_IMPL *, const char *, bool *);
    int (*rollback_to_stable)(WT_SESSION_IMPL *, const char *[], bool);

    /* Configuration. */
    bool dryrun;
};
