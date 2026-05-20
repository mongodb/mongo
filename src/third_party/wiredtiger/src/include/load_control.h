/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

struct __wt_connection_load_control {
    uint8_t control_threshold;         /* threshold when the load control starts rejecting work */
    wt_shared uint8_t read_load;       /* connection read load */
    wt_shared uint8_t write_load;      /* connection write load */
    wt_shared uint64_t read_load_max;  /* cache max bytes equivalent eviction trigger */
    wt_shared uint64_t write_load_max; /* cache max dirty bytes equivalent to dirty trigger */

    /* cache eviction controls bit positions */
#define WT_CONN_LOAD_CONTROL 0x1u
    wt_shared uint32_t flags;
};
