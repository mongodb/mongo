/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * We format timestamps in a couple of ways, declare appropriate sized buffers. Hexadecimal is 2x
 * the size of the value. MongoDB format (high/low pairs of 4B unsigned integers, with surrounding
 * parenthesis and separating comma and space), is 2x the maximum digits from a 4B unsigned integer
 * plus 4. Both sizes include a trailing null byte as well.
 */
#define WT_TS_HEX_STRING_SIZE (2 * sizeof(wt_timestamp_t) + 1)
#define WT_TS_INT_STRING_SIZE (2 * 10 + 4 + 1)

/*
 * We need an appropriately sized buffer for formatted time points, aggregates and windows. This is
 * for time windows with 4 timestamps, 2 transaction IDs, prepare state and formatting. The
 * formatting is currently about 32 characters - enough space that we don't need to think about it.
 * Time points have less information that time aggregate windows - cater for the larger here.
 */
#define WT_TIME_STRING_SIZE (WT_TS_INT_STRING_SIZE * 4 + 20 * 2 + 64)

/* The time points that define a value's time window and associated prepare information. */
struct __wt_time_window {
    wt_timestamp_t durable_start_ts; /* default value: WT_TS_NONE */
    wt_timestamp_t start_ts;         /* default value: WT_TS_NONE */
    uint64_t start_txn;              /* default value: WT_TXN_NONE */

    wt_timestamp_t durable_stop_ts; /* default value: WT_TS_NONE */
    wt_timestamp_t stop_ts;         /* default value: WT_TS_MAX */
    uint64_t stop_txn;              /* default value: WT_TXN_MAX */

    /*
     * Prepare information isn't really part of a time window, but we need to aggregate it to the
     * internal page information in reconciliation, and this is the simplest place to put it.
     */
    uint8_t prepare;
};

/*
 * The time points that define an aggregated time window and associated prepare information.
 *
 * - newest_start_durable_ts - Newest valid start durable/commit timestamp
 * - newest_stop_durable_ts  - Newest valid stop durable/commit timestamp doesn't include WT_TS_MAX
 * - oldest_start_ts         - Oldest start commit timestamp
 * - newest_txn              - Newest valid start/stop commit transaction doesn't include
 *                             WT_TXN_MAX
 * - newest_stop_ts          - Newest stop commit timestamp include WT_TS_MAX
 * - newest_stop_txn         - Newest stop commit transaction include WT_TXN_MAX
 * - prepare                 - Prepared updates
 */
struct __wt_time_aggregate {
    wt_timestamp_t newest_start_durable_ts; /* default value: WT_TS_NONE */
    wt_timestamp_t newest_stop_durable_ts;  /* default value: WT_TS_NONE */

    wt_timestamp_t oldest_start_ts; /* default value: WT_TS_NONE */
    uint64_t newest_txn;            /* default value: WT_TXN_NONE */
    wt_timestamp_t newest_stop_ts;  /* default value: WT_TS_MAX */
    uint64_t newest_stop_txn;       /* default value: WT_TXN_MAX */

    uint8_t prepare;

    uint8_t init_merge; /* Initialized for aggregation and merge */
};
