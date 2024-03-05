/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * WT_TRUNCATE_INFO
 *	A set of context associated with a range truncate operation.
 */
struct __wt_truncate_info {
    WT_SESSION_IMPL *session;
    const char *uri;
    WT_CURSOR *start;
    WT_CURSOR *stop;
    WT_ITEM *orig_start_key;
    WT_ITEM *orig_stop_key;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TRUNC_EXPLICIT_START 0x1u
#define WT_TRUNC_EXPLICIT_STOP 0x2u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};
