/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

struct __wt_compact_state {
    uint32_t file_count;        /* Number of files seen */
    uint32_t lsm_count;         /* Number of LSM trees seen */
    uint64_t free_space_target; /* Configured minimum space that should be recovered */
    uint64_t max_time;          /* Configured timeout */

    struct timespec begin;         /* Starting time */
    struct timespec last_progress; /* Last time a progress message was logged. */
};
