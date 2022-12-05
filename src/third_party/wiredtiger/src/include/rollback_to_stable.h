/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define WT_CHECK_RECOVERY_FLAG_TXNID(session, txnid)                                           \
    (F_ISSET(S2C(session), WT_CONN_RECOVERING) && S2C(session)->recovery_ckpt_snap_min != 0 && \
      (txnid) >= S2C(session)->recovery_ckpt_snap_min)

/* Enable rollback to stable verbose messaging during recovery. */
#define WT_VERB_RECOVERY_RTS(session)                                                              \
    (F_ISSET(S2C(session), WT_CONN_RECOVERING) ?                                                   \
        WT_DECL_VERBOSE_MULTI_CATEGORY(((WT_VERBOSE_CATEGORY[]){WT_VERB_RECOVERY, WT_VERB_RTS})) : \
        WT_DECL_VERBOSE_MULTI_CATEGORY(((WT_VERBOSE_CATEGORY[]){WT_VERB_RTS})))

/*
 * WT_ROLLBACK_TO_STABLE --
 *	Rollback to stable singleton, contains the interface to rollback to stable along
 *	with context used by rollback to stable.
 */
struct __wt_rollback_to_stable {
    /* Methods */
    int (*rollback_to_stable_one)(WT_SESSION_IMPL *, const char *, bool *);
    int (*rollback_to_stable)(WT_SESSION_IMPL *, const char *[], bool);
};
