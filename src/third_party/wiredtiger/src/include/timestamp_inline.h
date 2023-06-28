/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/* Initialize the fields in a time window to their defaults. */
#define WT_TIME_WINDOW_INIT(tw)              \
    do {                                     \
        (tw)->durable_start_ts = WT_TS_NONE; \
        (tw)->start_ts = WT_TS_NONE;         \
        (tw)->start_txn = WT_TXN_NONE;       \
        (tw)->durable_stop_ts = WT_TS_NONE;  \
        (tw)->stop_ts = WT_TS_MAX;           \
        (tw)->stop_txn = WT_TXN_MAX;         \
        (tw)->prepare = 0;                   \
    } while (0)

/* Copy the values from one time window structure to another. */
#define WT_TIME_WINDOW_COPY(dest, source) (*(dest) = *(source))

/* Return true if the time window is equivalent to the default time window. */
#define WT_TIME_WINDOW_IS_EMPTY(tw)                                            \
    ((tw)->durable_start_ts == WT_TS_NONE && (tw)->start_ts == WT_TS_NONE &&   \
      (tw)->start_txn == WT_TXN_NONE && (tw)->durable_stop_ts == WT_TS_NONE && \
      (tw)->stop_ts == WT_TS_MAX && (tw)->stop_txn == WT_TXN_MAX && (tw)->prepare == 0)

/* Check if the start time window is set. */
#define WT_TIME_WINDOW_HAS_START(tw) \
    ((tw)->start_txn != WT_TXN_NONE || (tw)->start_ts != WT_TS_NONE)

/* Check if the stop time window is set. */
#define WT_TIME_WINDOW_HAS_STOP(tw) ((tw)->stop_txn != WT_TXN_MAX || (tw)->stop_ts != WT_TS_MAX)

/* Return true if the time windows are the same. */
#define WT_TIME_WINDOWS_EQUAL(tw1, tw2)                                                           \
    ((tw1)->durable_start_ts == (tw2)->durable_start_ts && (tw1)->start_ts == (tw2)->start_ts &&  \
      (tw1)->start_txn == (tw2)->start_txn && (tw1)->durable_stop_ts == (tw2)->durable_stop_ts && \
      (tw1)->stop_ts == (tw2)->stop_ts && (tw1)->stop_txn == (tw2)->stop_txn &&                   \
      (tw1)->prepare == (tw2)->prepare)

/* Return true if the stop time windows are the same. */
#define WT_TIME_WINDOWS_STOP_EQUAL(tw1, tw2)                                                 \
    ((tw1)->durable_stop_ts == (tw2)->durable_stop_ts && (tw1)->stop_ts == (tw2)->stop_ts && \
      (tw1)->stop_txn == (tw2)->stop_txn && (tw1)->prepare == (tw2)->prepare)

/*
 * Set the start values of a time window from those in an update structure. Durable timestamp can be
 * 0 for prepared updates, in those cases use the prepared timestamp as durable timestamp.
 */
#define WT_TIME_WINDOW_SET_START(tw, upd)                          \
    do {                                                           \
        (tw)->durable_start_ts = (tw)->start_ts = (upd)->start_ts; \
        if ((upd)->durable_ts != WT_TS_NONE)                       \
            (tw)->durable_start_ts = (upd)->durable_ts;            \
        (tw)->start_txn = (upd)->txnid;                            \
    } while (0)

/*
 * Set the start values of a time window from those in an update structure. Durable timestamp can be
 * 0 for prepared updates, in those cases use the prepared timestamp as durable timestamp.
 */
#define WT_TIME_WINDOW_SET_STOP(tw, upd)                         \
    do {                                                         \
        (tw)->durable_stop_ts = (tw)->stop_ts = (upd)->start_ts; \
        if ((upd)->durable_ts != WT_TS_NONE)                     \
            (tw)->durable_stop_ts = (upd)->durable_ts;           \
        (tw)->stop_txn = (upd)->txnid;                           \
    } while (0)

/* Copy the start values of a time window from another time window. */
#define WT_TIME_WINDOW_COPY_START(dest, source)                \
    do {                                                       \
        (dest)->durable_start_ts = (source)->durable_start_ts; \
        (dest)->start_ts = (source)->start_ts;                 \
        (dest)->start_txn = (source)->start_txn;               \
        (dest)->prepare = (source)->prepare;                   \
    } while (0)

/* Copy the stop values of a time window from another time window. */
#define WT_TIME_WINDOW_COPY_STOP(dest, source)               \
    do {                                                     \
        (dest)->durable_stop_ts = (source)->durable_stop_ts; \
        (dest)->stop_ts = (source)->stop_ts;                 \
        (dest)->stop_txn = (source)->stop_txn;               \
        (dest)->prepare = (source)->prepare;                 \
    } while (0)

/*
 * Initialize the fields in an aggregated time window to their defaults. The aggregated durable
 * timestamp values represent the maximum durable timestamp over set of timestamps. These aggregated
 * max values are used for rollback to stable operation to find out whether the page has any
 * timestamp updates more than stable timestamp.
 */
#define WT_TIME_AGGREGATE_INIT(ta)                  \
    do {                                            \
        (ta)->newest_start_durable_ts = WT_TS_NONE; \
        (ta)->newest_stop_durable_ts = WT_TS_NONE;  \
        (ta)->oldest_start_ts = WT_TS_NONE;         \
        (ta)->newest_txn = WT_TXN_NONE;             \
        (ta)->newest_stop_ts = WT_TS_MAX;           \
        (ta)->newest_stop_txn = WT_TXN_MAX;         \
        (ta)->prepare = 0;                          \
        (ta)->init_merge = 0;                       \
    } while (0)

/*
 * Initialize the fields in an aggregated time window to maximum values, since this structure is
 * generally populated by iterating over a set of timestamps and calculating max/min seen for each
 * value, it's useful to be able to start with a negatively initialized structure. The aggregated
 * durable timestamp values represent the maximum durable timestamp over set of timestamps. These
 * aggregated max values are used for rollback to stable operation to find out whether the page has
 * any timestamp updates more than stable timestamp.
 */
#define WT_TIME_AGGREGATE_INIT_MERGE(ta)            \
    do {                                            \
        (ta)->newest_start_durable_ts = WT_TS_NONE; \
        (ta)->newest_stop_durable_ts = WT_TS_NONE;  \
        (ta)->oldest_start_ts = WT_TS_MAX;          \
        (ta)->newest_txn = WT_TXN_NONE;             \
        (ta)->newest_stop_ts = WT_TS_NONE;          \
        (ta)->newest_stop_txn = WT_TXN_NONE;        \
        (ta)->prepare = 0;                          \
        (ta)->init_merge = 1;                       \
    } while (0)

/* Return true if the time aggregate is equivalent to the initialized time aggregate. */
#define WT_TIME_AGGREGATE_IS_EMPTY(ta)                                                         \
    ((ta)->init_merge == 0 ?                                                                   \
        ((ta)->newest_start_durable_ts == WT_TS_NONE &&                                        \
          (ta)->newest_stop_durable_ts == WT_TS_NONE && (ta)->oldest_start_ts == WT_TS_NONE && \
          (ta)->newest_txn == WT_TXN_NONE && (ta)->newest_stop_ts == WT_TS_MAX &&              \
          (ta)->newest_stop_txn == WT_TXN_MAX && (ta)->prepare == 0) :                         \
        ((ta)->newest_start_durable_ts == WT_TS_NONE &&                                        \
          (ta)->newest_stop_durable_ts == WT_TS_NONE && (ta)->oldest_start_ts == WT_TS_MAX &&  \
          (ta)->newest_txn == WT_TXN_NONE && (ta)->newest_stop_ts == WT_TS_NONE &&             \
          (ta)->newest_stop_txn == WT_TXN_NONE && (ta)->prepare == 0))

/* Copy the values from one time aggregate structure to another. */
#define WT_TIME_AGGREGATE_COPY(dest, source) (*(dest) = *(source))

/* Update the aggregated window to reflect for a new time window. */
#define WT_TIME_AGGREGATE_UPDATE(session, ta, tw)                              \
    do {                                                                       \
        WT_ASSERT(session, (ta)->init_merge == 1);                             \
        (ta)->newest_start_durable_ts =                                        \
          WT_MAX((tw)->durable_start_ts, (ta)->newest_start_durable_ts);       \
        (ta)->newest_stop_durable_ts =                                         \
          WT_MAX((tw)->durable_stop_ts, (ta)->newest_stop_durable_ts);         \
        (ta)->oldest_start_ts = WT_MIN((tw)->start_ts, (ta)->oldest_start_ts); \
        (ta)->newest_txn = WT_MAX((tw)->start_txn, (ta)->newest_txn);          \
        /*                                                                     \
         * Aggregation of newest transaction is calculated from both start and \
         * stop transactions. Consider only valid stop transactions.           \
         */                                                                    \
        if ((tw)->stop_txn != WT_TXN_MAX)                                      \
            (ta)->newest_txn = WT_MAX((tw)->stop_txn, (ta)->newest_txn);       \
        (ta)->newest_stop_ts = WT_MAX((tw)->stop_ts, (ta)->newest_stop_ts);    \
        (ta)->newest_stop_txn = WT_MAX((tw)->stop_txn, (ta)->newest_stop_txn); \
        if ((tw)->prepare != 0)                                                \
            (ta)->prepare = 1;                                                 \
    } while (0)

/*
 * Update a time aggregate from a page deleted structure. A page delete is equivalent to an entire
 * page of identical tombstones; this operation is equivalent to applying WT_TIME_AGGREGATE_UPDATE
 * for each tombstone. Note that it does not affect the start times.
 */
#define WT_TIME_AGGREGATE_UPDATE_PAGE_DEL(session, ta, page_del)                    \
    do {                                                                            \
        WT_ASSERT(session, (ta)->init_merge == 1);                                  \
        (ta)->newest_stop_durable_ts =                                              \
          WT_MAX((page_del)->durable_timestamp, (ta)->newest_stop_durable_ts);      \
        (ta)->newest_txn = WT_MAX((page_del)->txnid, (ta)->newest_txn);             \
        (ta)->newest_stop_ts = WT_MAX((page_del)->timestamp, (ta)->newest_stop_ts); \
        (ta)->newest_stop_txn = WT_MAX((page_del)->txnid, (ta)->newest_stop_txn);   \
    } while (0)

/* Merge an aggregated time window into another - choosing the most conservative value from each. */
#define WT_TIME_AGGREGATE_MERGE(session, dest, source)                                        \
    do {                                                                                      \
        WT_ASSERT(session, (dest)->init_merge == 1);                                          \
        (dest)->newest_start_durable_ts =                                                     \
          WT_MAX((dest)->newest_start_durable_ts, (source)->newest_start_durable_ts);         \
        (dest)->newest_stop_durable_ts =                                                      \
          WT_MAX((dest)->newest_stop_durable_ts, (source)->newest_stop_durable_ts);           \
        (dest)->oldest_start_ts = WT_MIN((dest)->oldest_start_ts, (source)->oldest_start_ts); \
        (dest)->newest_txn = WT_MAX((dest)->newest_txn, (source)->newest_txn);                \
        (dest)->newest_stop_ts = WT_MAX((dest)->newest_stop_ts, (source)->newest_stop_ts);    \
        (dest)->newest_stop_txn = WT_MAX((dest)->newest_stop_txn, (source)->newest_stop_txn); \
        /*                                                                                    \
         * Aggregation of newest transaction is calculated from both start and stop           \
         * transactions. Consider only valid stop transactions.                               \
         */                                                                                   \
        if ((dest)->newest_stop_txn != WT_TXN_MAX)                                            \
            (dest)->newest_txn = WT_MAX((dest)->newest_txn, (dest)->newest_stop_txn);         \
        if ((source)->prepare != 0)                                                           \
            (dest)->prepare = 1;                                                              \
    } while (0)

/* Abstract away checking whether all records in an aggregated time window have been deleted. */
#define WT_TIME_AGGREGATE_ALL_DELETED(ta) ((ta)->newest_stop_ts != WT_TS_MAX)

/*
 * Update a time aggregate in preparation for an obsolete visibility check. This deserves a macro,
 * since the mechanism for identifying whether an aggregated time window contains only obsolete (i.e
 * deleted) data requires checking two different timestamps. Note the output time aggregate might be
 * either empty initialized, or have been populated via prior calls to this macro with other
 * aggregated windows.
 */
#define WT_TIME_AGGREGATE_MERGE_OBSOLETE_VISIBLE(session, out_ta, in_ta)                         \
    do {                                                                                         \
        WT_ASSERT(session, (out_ta)->init_merge == 1);                                           \
        (out_ta)->newest_stop_durable_ts =                                                       \
          WT_MAX((out_ta)->newest_stop_durable_ts, (in_ta)->newest_stop_durable_ts);             \
        /*                                                                                       \
         * The durable and non-durable stop timestamps are interestingly different in that the   \
         * non-durable version encodes whether all records are deleted by setting WT_TS_MAX in   \
         * there are non-deleted records (the common case), but durable doesn't and records the  \
         * largest timestamp associated with any deleted record. Use this copy-macro to abstract \
         * that subtlety away. Since obsolete checks always want to know whether all content was \
         * removed, copy that semantic into the durable stop timestamp to make visibility        \
         * checking sensible.                                                                    \
         */                                                                                      \
        if (!WT_TIME_AGGREGATE_ALL_DELETED((in_ta)))                                             \
            (out_ta)->newest_stop_durable_ts = WT_TS_MAX;                                        \
                                                                                                 \
        (out_ta)->newest_txn = WT_MAX((out_ta)->newest_txn, (in_ta)->newest_txn);                \
        (out_ta)->newest_stop_ts = WT_MAX((out_ta)->newest_stop_ts, (in_ta)->newest_stop_ts);    \
        (out_ta)->newest_stop_txn = WT_MAX((out_ta)->newest_stop_txn, (in_ta)->newest_stop_txn); \
    } while (0)
