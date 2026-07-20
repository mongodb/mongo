/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * WTI_CLAYERED_ROLE --
 *	The leader/follower role resolved for a layered-table operation.
 */
typedef enum { WTI_CLAYERED_ROLE_FOLLOWER, WTI_CLAYERED_ROLE_LEADER } WTI_CLAYERED_ROLE;

/*
 * WTI_CURSOR_LAYERED --
 *	A layered table cursor.
 */
struct __wti_cursor_layered {
    WT_CURSOR iface;

    WT_DATA_HANDLE *dhandle;

    WT_CURSOR *current_cursor; /* The current cursor for iteration */
    WT_CURSOR *ingest_cursor;  /* The ingest table */
    WT_CURSOR *stable_cursor;  /* The stable table */

    uint64_t next_random_seed;
    u_int next_random_sample_size;

    uint64_t snapshot_gen;               /* Snapshot generation on last access */
    uint64_t read_timestamp;             /* Read timestamp on last access */
    uint64_t stable_checkpoint_meta_lsn; /* Checkpoint LSN stable cursor is on */
    WTI_CLAYERED_ROLE last_role;         /* Last-observed leader/follower role (change-detection) */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WTI_CLAYERED_ACTIVE 0x01u       /* Incremented the session count */
#define WTI_CLAYERED_ITERATE_NEXT 0x02u /* Forward iteration */
#define WTI_CLAYERED_ITERATE_PREV 0x04u /* Backward iteration */
#define WTI_CLAYERED_RANDOM 0x08u       /* Random cursor operations only */
#define WTI_CLAYERED_SIZE_STAT 0x10u    /* Accumulate the size summary on the active btree */
                                        /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/*
 * WTI_CLAYERED_OP_MODE --
 *	The kind of layered-table operation.
 */
typedef enum {
    WTI_CLAYERED_MODE_SEARCH,         /* search, search_near */
    WTI_CLAYERED_MODE_ITERATE,        /* next, prev */
    WTI_CLAYERED_MODE_RANDOM,         /* next_random */
    WTI_CLAYERED_MODE_SCAN,           /* largest_key */
    WTI_CLAYERED_MODE_WRITE,          /* reserve, modify; non-overwrite insert/update/remove */
    WTI_CLAYERED_MODE_WRITE_OVERWRITE /* overwrite insert/update/remove */
} WTI_CLAYERED_OP_MODE;

/*
 * WTI_CLAYERED_OP --
 *	State gathered once for a single layered table cursor operation.
 */
struct __wti_clayered_op {
    WTI_CURSOR_LAYERED *clayered;    /* back-pointer; session via CUR2S(clayered) */
    WT_CURSOR *ingest;               /* resolved slot == clayered->ingest_cursor (may be NULL) */
    WT_CURSOR *stable;               /* resolved slot == clayered->stable_cursor (may be NULL) */
    WT_TRUNCATE_LIST *truncate_list; /* the layered table's truncate list */
    WT_COLLATOR *collator;
};
