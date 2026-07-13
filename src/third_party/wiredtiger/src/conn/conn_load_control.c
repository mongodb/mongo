/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __conn_load_control_configure --
 *     Configure the load control constructs.
 */
static void
__conn_load_control_configure(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_CONNECTION_LOAD_CONTROL *load_control;
    WT_EVICT *evict;

    uint64_t bytes_max;

    conn = S2C(session);
    bytes_max = conn->cache_size + 1;
    evict = conn->evict;

    load_control = &conn->load_control;

    /*
     * Load range is mapped to cache thresholds. Read load of 100% is mapped to eviction_trigger,
     * configured max cache fill ratio, and write load of 100% is mapped to eviction_dirty_trigger,
     * configured max dirty cache fill ratio.
     *
     * Load control subsystem will start rejecting the work based on the configured load control
     * threshold. Default load control threshold is 100%, which means load control will start
     * rejecting the work when cache fill ratio reaches eviction_trigger (i.e., 95%) for read and
     * eviction_dirty_trigger (i.e., 20%) for write.
     */

    /* Calculate max accepted for both read and write */
    __wt_atomic_store_uint64_relaxed(
      &load_control->read_load_max, (uint64_t)(bytes_max * evict->eviction_trigger / 100.0));
    __wt_atomic_store_uint64_relaxed(
      &load_control->write_load_max, (uint64_t)(bytes_max * evict->eviction_dirty_trigger / 100.0));

    return;
}

/*
 * __wti_conn_load_control_config --
 *     Configure or reconfigure the load control.
 */
int
__wti_conn_load_control_config(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_LOAD_CONTROL *load_control;
    WT_UNUSED(reconfig);

    load_control = &S2C(session)->load_control;

    WT_RET(__wt_config_gets(session, cfg, "load_control.enable", &cval));
    if (cval.val != 0)
        F_SET(load_control, WT_CONN_LOAD_CONTROL);

    /* load control threshold determines when the load control will be activated */

    WT_RET(__wt_config_gets(session, cfg, "load_control.control_threshold", &cval));
    __wt_atomic_store_uint16_relaxed(
      &load_control->control_threshold, (((uint16_t)cval.val > 1000) ? 1000 : (uint16_t)cval.val));

    /*
     * Load control thresholds are calculated based on the configuration settings of load control as
     * well as eviction. Hence they should be adjusted whenever the configuration of either eviction
     * or load control is changed.
     */
    __conn_load_control_configure(session);

    return (0);
}

/*
 * __conn_calc_load_pct --
 *     Calculate the percentage of part relative to whole. Returns 0 if whole is zero.
 */
static WT_INLINE float
__conn_calc_load_pct(uint64_t part, uint64_t whole)
{
    if (whole == 0)
        return (0.0F);

    return (WT_MIN((float)part * 100 / (float)whole, 1000.0F));
}

/*
 * __wt_conn_calc_read_load --
 *     Calculate and return the read load at the system level. Computed on demand from the load-shed
 *     check and the statistics path rather than in the cache accounting hot path. The cache fill
 *     ratio is amplified by (1 + recent read miss rate), so that a cache thrashing on disk reads is
 *     shed more aggressively (up to twice the fill ratio) than one serving mostly hits. The miss
 *     rate is sampled from the delta of the cumulative cache read and page request counters since
 *     the previous review. The counters advance concurrently with reads, so the sampled delta is
 *     approximate, which is acceptable for a load heuristic.
 */
uint16_t
__wt_conn_calc_read_load(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_CONNECTION_LOAD_CONTROL *load_control;
    float cache_miss_pct, read_load;
    uint64_t cache_read, cache_req, delta_read, delta_req, prev_read, prev_req;

    conn = S2C(session);
    load_control = &conn->load_control;

    read_load =
      __conn_calc_load_pct(__wt_cache_bytes_inuse(conn->cache), load_control->read_load_max);

    cache_read = (uint64_t)WT_STAT_CONN_READ(conn->stats, cache_read);
    cache_req = (uint64_t)WT_STAT_CONN_READ(conn->stats, cache_pages_requested);

    prev_req = __wt_atomic_load_uint64_relaxed(&load_control->prev_cache_pages_requested);
    delta_req = cache_req - prev_req;

    /*
     * When statistics are disabled the read counters stay at zero, and with no new page requests
     * since the last review the miss rate is treated as zero, giving the cache fill ratio alone.
     */
    cache_miss_pct = 0.0F;
    if (delta_req != 0) {
        prev_read = __wt_atomic_load_uint64_relaxed(&load_control->prev_cache_read);
        __wt_atomic_store_uint64_relaxed(&load_control->prev_cache_read, cache_read);
        __wt_atomic_store_uint64_relaxed(&load_control->prev_cache_pages_requested, cache_req);
        delta_read = cache_read - prev_read;

        cache_miss_pct = WT_MIN((float)delta_read * 100 / (float)delta_req, 100.0F);
    }

    return ((uint16_t)WT_MIN(read_load * (100 + cache_miss_pct) / 100, 1000.0F));
}

/*
 * __wti_conn_load_control_stats_update --
 *     Update the read and write load statistics.
 */
void
__wti_conn_load_control_stats_update(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_STATS **stats;

    stats = S2C(session)->stats;

    WT_STATP_CONN_SET(session, stats, read_load, __wt_conn_calc_read_load(session));
    WT_STATP_CONN_SET(session, stats, write_load, __wt_conn_calc_write_load(session));
}

/*
 * __wt_conn_calc_write_load --
 *     Calculate and return the write load at the system level. Computed on demand from the
 *     load-shed check and the statistics path rather than in the cache accounting hot path. The
 *     load is always calculated; the load control enable flag only governs whether work is shed.
 */
uint16_t
__wt_conn_calc_write_load(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_LOAD_CONTROL *load_control;
    float write_load;

    load_control = &S2C(session)->load_control;
    write_load = __conn_calc_load_pct(
      __wt_cache_dirty_inuse(S2C(session)->cache), load_control->write_load_max);
    return ((uint16_t)write_load);
}
