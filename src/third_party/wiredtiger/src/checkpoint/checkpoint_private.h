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

/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

/* DO NOT EDIT: automatically built by prototypes.py: END */
