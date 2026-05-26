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
    __wt_atomic_store_uint8_relaxed(
      &load_control->control_threshold, (((uint8_t)cval.val > 200) ? 200 : (uint8_t)cval.val));

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
static WT_INLINE uint8_t
__conn_calc_load_pct(uint64_t part, uint64_t whole)
{
    if (whole == 0)
        return (0);

    return (((uint8_t)WT_MIN((part * 100) / whole, 200)));
}

/*
 * __wt_conn_calc_read_load --
 *     Calculate the read load at the system level.
 */
void
__wt_conn_calc_read_load(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_LOAD_CONTROL *load_control;
    uint64_t bytes_inuse, bytes_max;
    uint8_t load;

    load_control = &S2C(session)->load_control;
    if (F_ISSET(load_control, WT_CONN_LOAD_CONTROL)) {
        bytes_max = load_control->read_load_max;
        bytes_inuse = __wt_cache_bytes_inuse(S2C(session)->cache);

        load = __conn_calc_load_pct(bytes_inuse, bytes_max);
        __wt_atomic_store_uint8_relaxed(&load_control->read_load, load);
        WT_STAT_CONN_SET(session, read_load, load);
    }
    return;
}

/*
 * __wt_conn_calc_write_load --
 *     Calculate the write load at the system level.
 */
void
__wt_conn_calc_write_load(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_LOAD_CONTROL *load_control;
    uint64_t bytes_dirty, bytes_max;
    uint8_t load;

    load_control = &S2C(session)->load_control;
    if (F_ISSET(load_control, WT_CONN_LOAD_CONTROL)) {
        bytes_max = load_control->write_load_max;
        bytes_dirty = __wt_cache_dirty_inuse(S2C(session)->cache);

        load = __conn_calc_load_pct(bytes_dirty, bytes_max);
        __wt_atomic_store_uint8_relaxed(&load_control->write_load, load);
        WT_STAT_CONN_SET(session, write_load, load);
    }
    return;
}
