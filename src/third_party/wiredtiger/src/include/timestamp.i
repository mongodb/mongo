/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_time_window_init --
 *     Initialize the fields in a time window to their defaults.
 */
static inline void
__wt_time_window_init(WT_TIME_WINDOW *tw)
{
    tw->durable_start_ts = WT_TS_NONE;
    tw->start_ts = WT_TS_NONE;
    tw->start_txn = WT_TXN_NONE;

    tw->durable_stop_ts = WT_TS_NONE;
    tw->stop_ts = WT_TS_MAX;
    tw->stop_txn = WT_TXN_MAX;

    tw->prepare = 0;
}

/*
 * __wt_time_window_init_max --
 *     Initialize the fields in a time window to values that force an override.
 */
static inline void
__wt_time_window_init_max(WT_TIME_WINDOW *tw)
{
    tw->durable_start_ts = WT_TS_MAX;
    tw->start_ts = WT_TS_MAX;
    tw->start_txn = WT_TXN_MAX;

    tw->durable_stop_ts = WT_TS_MAX;
    tw->stop_ts = WT_TS_NONE;
    tw->stop_txn = WT_TXN_NONE;

    tw->prepare = 0;
}

/*
 * __wt_time_window_copy --
 *     Copy the values from one time window structure to another.
 */
static inline void
__wt_time_window_copy(WT_TIME_WINDOW *dest, WT_TIME_WINDOW *source)
{
    *dest = *source;
}

/*
 * __wt_time_window_is_empty --
 *     Return true if the time window is equivalent to the default time window.
 */
static inline bool
__wt_time_window_is_empty(WT_TIME_WINDOW *tw)
{
    return (tw->durable_start_ts == WT_TS_NONE && tw->start_ts == WT_TS_NONE &&
      tw->start_txn == WT_TXN_NONE && tw->durable_stop_ts == WT_TS_NONE &&
      tw->stop_ts == WT_TS_MAX && tw->stop_txn == WT_TXN_MAX && tw->prepare == 0);
}

/*
 * __wt_time_windows_equal --
 *     Return true if the time windows are the same.
 */
static inline bool
__wt_time_windows_equal(WT_TIME_WINDOW *tw1, WT_TIME_WINDOW *tw2)
{
    return (tw1->durable_start_ts == tw2->durable_start_ts && tw1->start_ts == tw2->start_ts &&
      tw1->start_txn == tw2->start_txn && tw1->durable_stop_ts == tw2->durable_stop_ts &&
      tw1->stop_ts == tw2->stop_ts && tw1->stop_txn == tw2->stop_txn &&
      tw1->prepare == tw2->prepare);
}

/*
 * __wt_time_window_set_start --
 *     Set the start values of a time window from those in an update structure.
 */
static inline void
__wt_time_window_set_start(WT_TIME_WINDOW *tw, WT_UPDATE *upd)
{
    /*
     * Durable timestamp can be 0 for prepared updates, in those cases use the prepared timestamp as
     * durable timestamp.
     */
    tw->durable_start_ts = tw->start_ts = upd->start_ts;
    if (upd->durable_ts != WT_TS_NONE)
        tw->durable_start_ts = upd->durable_ts;
    tw->start_txn = upd->txnid;
}

/*
 * __wt_time_window_set_stop --
 *     Set the start values of a time window from those in an update structure.
 */
static inline void
__wt_time_window_set_stop(WT_TIME_WINDOW *tw, WT_UPDATE *upd)
{
    /*
     * Durable timestamp can be 0 for prepared updates, in those cases use the prepared timestamp as
     * durable timestamp.
     */
    tw->durable_stop_ts = tw->stop_ts = upd->start_ts;
    if (upd->durable_ts != WT_TS_NONE)
        tw->durable_stop_ts = upd->durable_ts;
    tw->stop_txn = upd->txnid;
}

/*
 * __wt_time_aggregate_init --
 *     Initialize the fields in an aggregated time window to their defaults.
 */
static inline void
__wt_time_aggregate_init(WT_TIME_AGGREGATE *ta)
{
    /*
     * The aggregated durable timestamp values represent the maximum durable timestamp over set of
     * timestamps. These aggregated max values are used for rollback to stable operation to find out
     * whether the page has any timestamp updates more than stable timestamp.
     */
    ta->newest_start_durable_ts = WT_TS_NONE;
    ta->newest_stop_durable_ts = WT_TS_NONE;

    ta->oldest_start_ts = WT_TS_NONE;
    ta->oldest_start_txn = WT_TXN_NONE;

    ta->newest_stop_ts = WT_TS_MAX;
    ta->newest_stop_txn = WT_TXN_MAX;

    ta->prepare = 0;
}

/*
 * __wt_time_aggregate_init_max --
 *     Initialize the fields in an aggregated time window to maximum values, since this structure is
 *     generally populated by iterating over a set of timestamps and calculating max/min seen for
 *     each value, it's useful to be able to start with a negatively initialized structure.
 */
static inline void
__wt_time_aggregate_init_max(WT_TIME_AGGREGATE *ta)
{
    /*
     * The aggregated durable timestamp values represent the maximum durable timestamp over set of
     * timestamps. These aggregated max values are used for rollback to stable operation to find out
     * whether the page has any timestamp updates more than stable timestamp.
     */
    ta->newest_start_durable_ts = WT_TS_NONE;
    ta->newest_stop_durable_ts = WT_TS_NONE;

    ta->oldest_start_ts = WT_TS_MAX;
    ta->oldest_start_txn = WT_TXN_MAX;

    ta->newest_stop_ts = WT_TS_NONE;
    ta->newest_stop_txn = WT_TXN_NONE;

    ta->prepare = 0;
}

/*
 * __wt_time_aggregate_is_empty --
 *     Return true if the time aggregate is equivalent to the default time aggregate.
 */
static inline bool
__wt_time_aggregate_is_empty(WT_TIME_AGGREGATE *ta)
{
    return (ta->newest_start_durable_ts == WT_TS_NONE && ta->newest_stop_durable_ts == WT_TS_NONE &&
      ta->oldest_start_ts == WT_TS_MAX && ta->oldest_start_txn == WT_TXN_MAX &&
      ta->newest_stop_ts == WT_TS_NONE && ta->newest_stop_txn == WT_TXN_NONE && ta->prepare == 0);
}

/*
 * __wt_time_aggregate_copy --
 *     Copy the values from one time aggregate structure to another.
 */
static inline void
__wt_time_aggregate_copy(WT_TIME_AGGREGATE *dest, WT_TIME_AGGREGATE *source)
{
    *dest = *source;
}

/*
 * __wt_time_aggregate_update --
 *     Update the aggregated window to reflect for a new time window.
 */
static inline void
__wt_time_aggregate_update(WT_TIME_AGGREGATE *ta, WT_TIME_WINDOW *tw)
{
    ta->newest_start_durable_ts = WT_MAX(tw->durable_start_ts, ta->newest_start_durable_ts);
    ta->newest_stop_durable_ts = WT_MAX(tw->durable_stop_ts, ta->newest_stop_durable_ts);

    ta->oldest_start_ts = WT_MIN(tw->start_ts, ta->oldest_start_ts);
    ta->oldest_start_txn = WT_MIN(tw->start_txn, ta->oldest_start_txn);
    ta->newest_stop_ts = WT_MAX(tw->stop_ts, ta->newest_stop_ts);
    ta->newest_stop_txn = WT_MAX(tw->stop_txn, ta->newest_stop_txn);

    if (tw->prepare != 0)
        ta->prepare = 1;
}

/*
 * __wt_time_aggregate_merge --
 *     Merge an aggregated time window into another - choosing the most conservative value from
 *     each.
 */
static inline void
__wt_time_aggregate_merge(WT_TIME_AGGREGATE *dest, WT_TIME_AGGREGATE *source)
{
    dest->newest_start_durable_ts =
      WT_MAX(dest->newest_start_durable_ts, source->newest_start_durable_ts);
    dest->newest_stop_durable_ts =
      WT_MAX(dest->newest_stop_durable_ts, source->newest_stop_durable_ts);

    dest->oldest_start_ts = WT_MIN(dest->oldest_start_ts, source->oldest_start_ts);
    dest->oldest_start_txn = WT_MIN(dest->oldest_start_txn, source->oldest_start_txn);
    dest->newest_stop_ts = WT_MAX(dest->newest_stop_ts, source->newest_stop_ts);
    dest->newest_stop_txn = WT_MAX(dest->newest_stop_txn, source->newest_stop_txn);

    if (source->prepare != 0)
        dest->prepare = 1;
}
