/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define WT_CONFIG_DEBUG(session, fmt, ...)                                 \
    if (FLD_ISSET(S2C(session)->debug_flags, WT_CONN_DEBUG_CONFIGURATION)) \
        __wt_verbose_warning(session, WT_VERB_CONFIGURATION, fmt, __VA_ARGS__);

/*
 * __evict_config_abs_to_pct --
 *     Evict configuration values can be either a percentage or an absolute size, this function
 *     converts an absolute size to a percentage.
 */
static WT_INLINE int
__evict_config_abs_to_pct(
  WT_SESSION_IMPL *session, double *param, const char *param_name, uint64_t cache_size, bool shared)
{
    double input;

    WT_ASSERT(session, param != NULL);
    input = *param;

    /*
     * Anything above 100 is an absolute value; convert it to percentage.
     */
    if (input > 100.0) {
        /*
         * In a shared cache configuration the cache size changes regularly. Therefore, we require a
         * percentage setting and do not allow an absolute size setting.
         */
        if (shared)
            WT_RET_MSG(session, EINVAL,
              "Shared cache configuration requires a percentage value for %s", param_name);
        /* An absolute value can't exceed the cache size. */
        if (input > cache_size)
            WT_RET_MSG(session, EINVAL, "%s should not exceed cache size", param_name);

        *param = (input * 100.0) / cache_size;
    }

    return (0);
}

/*
 * __evict_validate_config --
 *     Validate trigger and target values of given configs.
 */
static int
__evict_validate_config(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_EVICT *evict;
    bool shared;

    conn = S2C(session);
    evict = conn->evict;

    WT_RET(__wt_config_gets_none(session, cfg, "shared_cache.name", &cval));
    shared = cval.len != 0;

    /* Debug flags are not yet set when this function runs during connection open. Set it now. */
    WT_RET(__wt_config_gets(session, cfg, "debug_mode.configuration", &cval));
    if (cval.val)
        FLD_SET(conn->debug_flags, WT_CONN_DEBUG_CONFIGURATION);
    else
        FLD_CLR(conn->debug_flags, WT_CONN_DEBUG_CONFIGURATION);

    WT_RET(__wt_config_gets(session, cfg, "eviction_target", &cval));
    evict->eviction_target = (double)cval.val;
    WT_RET(__evict_config_abs_to_pct(
      session, &(evict->eviction_target), "eviction target", conn->cache_size, shared));

    WT_RET(__wt_config_gets(session, cfg, "eviction_trigger", &cval));
    evict->eviction_trigger = (double)cval.val;
    WT_RET(__evict_config_abs_to_pct(
      session, &(evict->eviction_trigger), "eviction trigger", conn->cache_size, shared));

    WT_RET(__wt_config_gets(session, cfg, "eviction_dirty_target", &cval));
    evict->eviction_dirty_target = (double)cval.val;
    WT_RET(__evict_config_abs_to_pct(
      session, &(evict->eviction_dirty_target), "eviction dirty target", conn->cache_size, shared));

    WT_RET(__wt_config_gets(session, cfg, "eviction_dirty_trigger", &cval));
    evict->eviction_dirty_trigger = (double)cval.val;
    WT_RET(__evict_config_abs_to_pct(session, &(evict->eviction_dirty_trigger),
      "eviction dirty trigger", conn->cache_size, shared));

    WT_RET(__wt_config_gets(session, cfg, "eviction_updates_target", &cval));
    evict->eviction_updates_target = (double)cval.val;
    WT_RET(__evict_config_abs_to_pct(session, &(evict->eviction_updates_target),
      "eviction updates target", conn->cache_size, shared));

    WT_RET(__wt_config_gets(session, cfg, "eviction_updates_trigger", &cval));
    evict->eviction_updates_trigger = (double)cval.val;
    WT_RET(__evict_config_abs_to_pct(session, &(evict->eviction_updates_trigger),
      "eviction updates trigger", conn->cache_size, shared));

    WT_RET(__wt_config_gets(session, cfg, "eviction_checkpoint_target", &cval));
    evict->eviction_checkpoint_target = (double)cval.val;
    WT_RET(__evict_config_abs_to_pct(session, &(evict->eviction_checkpoint_target),
      "eviction checkpoint target", conn->cache_size, shared));

    /* Check for invalid configurations and automatically fix them to suitable values. */
    if (evict->eviction_dirty_target > evict->eviction_target) {
        WT_CONFIG_DEBUG(session,
          "config eviction_dirty_target=%f cannot exceed eviction_target=%f. Setting "
          "eviction_dirty_target to %f.",
          evict->eviction_dirty_target, evict->eviction_target, evict->eviction_target);
        evict->eviction_dirty_target = evict->eviction_target;
    }

    if (evict->eviction_checkpoint_target > 0 &&
      evict->eviction_checkpoint_target < evict->eviction_dirty_target) {
        WT_CONFIG_DEBUG(session,
          "config eviction_checkpoint_target=%f cannot be less than eviction_dirty_target=%f. "
          "Setting "
          "eviction_checkpoint_target to %f.",
          evict->eviction_checkpoint_target, evict->eviction_dirty_target,
          evict->eviction_dirty_target);
        evict->eviction_checkpoint_target = evict->eviction_dirty_target;
    }

    if (evict->eviction_dirty_trigger > evict->eviction_trigger) {
        WT_CONFIG_DEBUG(session,
          "config eviction_dirty_trigger=%f cannot exceed eviction_trigger=%f. Setting "
          "eviction_dirty_trigger to %f.",
          evict->eviction_dirty_trigger, evict->eviction_trigger, evict->eviction_trigger);
        evict->eviction_dirty_trigger = evict->eviction_trigger;
    }

    if (evict->eviction_updates_target < DBL_EPSILON) {
        WT_CONFIG_DEBUG(session,
          "config eviction_updates_target (%f) cannot be zero. Setting "
          "to 50%% of eviction_dirty_target (%f).",
          evict->eviction_updates_target, evict->eviction_dirty_target / 2);
        evict->eviction_updates_target = evict->eviction_dirty_target / 2;
    }

    if (evict->eviction_updates_trigger < DBL_EPSILON) {
        WT_CONFIG_DEBUG(session,
          "config eviction_updates_trigger (%f) cannot be zero. Setting "
          "to 50%% of eviction_dirty_trigger (%f).",
          evict->eviction_updates_trigger, evict->eviction_dirty_trigger / 2);
        evict->eviction_updates_trigger = evict->eviction_dirty_trigger / 2;
    }

    /* Don't allow the trigger to be larger than the overall trigger. */
    if (evict->eviction_updates_trigger > evict->eviction_trigger) {
        WT_CONFIG_DEBUG(session,
          "config eviction_updates_trigger=%f cannot exceed eviction_trigger=%f. Setting "
          "eviction_updates_trigger to %f.",
          evict->eviction_updates_trigger, evict->eviction_trigger, evict->eviction_trigger);
        evict->eviction_updates_trigger = evict->eviction_trigger;
    }

    /* The target size must be lower than the trigger size or we will never get any work done. */
    if (evict->eviction_target >= evict->eviction_trigger)
        WT_RET_MSG(session, EINVAL, "eviction target must be lower than the eviction trigger");
    if (evict->eviction_dirty_target >= evict->eviction_dirty_trigger)
        WT_RET_MSG(
          session, EINVAL, "eviction dirty target must be lower than the eviction dirty trigger");
    if (evict->eviction_updates_target >= evict->eviction_updates_trigger)
        WT_RET_MSG(session, EINVAL,
          "eviction updates target must be lower than the eviction updates trigger");

    return (0);
}

/* !!!
 * __wt_evict_config --
 *     Parses eviction-related configuration strings during `wiredtiger_open` or
 *     `WT_CONNECTION::reconfigure` to set eviction parameters.
 *
 *     Input parameters:
 *       (1) `cfg[]`: a stack of configuration strings, where each string specifies a configuration
 *           option (e.g., `eviction.threads_max`). The full list of valid eviction configurations
 *           are defined in `api_data.py`.
 *       (2) `reconfig`: a boolean that indicates whether this function is being called during
 *           `WT_CONNECTION::reconfigure`.
 *
 *     Return an error code for invalid configurations.
 */
int
__wt_evict_config(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_EVICT *evict;
    uint32_t evict_threads_max, evict_threads_min;

    conn = S2C(session);
    evict = conn->evict;

    WT_ASSERT(session, evict != NULL);

    WT_RET(__evict_validate_config(session, cfg));

    WT_RET(__wt_config_gets(session, cfg, "eviction.threads_max", &cval));
    WT_ASSERT(session, cval.val > 0);
    evict_threads_max = (uint32_t)cval.val;

    WT_RET(__wt_config_gets(session, cfg, "eviction.threads_min", &cval));
    WT_ASSERT(session, cval.val > 0);
    evict_threads_min = (uint32_t)cval.val;

    if (evict_threads_min > evict_threads_max)
        WT_RET_MSG(
          session, EINVAL, "eviction=(threads_min) cannot be greater than eviction=(threads_max)");
    conn->evict_threads_max = evict_threads_max;
    conn->evict_threads_min = evict_threads_min;

    WT_RET(__wt_config_gets(session, cfg, "eviction.evict_sample_inmem", &cval));
    conn->evict_sample_inmem = cval.val != 0;

    WT_RET(__wt_config_gets(session, cfg, "eviction.evict_use_softptr", &cval));
    __wt_atomic_storebool(&conn->evict_use_npos, cval.val != 0);

    WT_RET(__wt_config_gets(session, cfg, "eviction.legacy_page_visit_strategy", &cval));
    conn->evict_legacy_page_visit_strategy = cval.val != 0;

    /* Retrieve the wait time and convert from milliseconds */
    WT_RET(__wt_config_gets(session, cfg, "cache_max_wait_ms", &cval));
    if (cval.val > 1)
        evict->cache_max_wait_us = (uint64_t)(cval.val * WT_THOUSAND);
    else if (cval.val == 1)
        evict->cache_max_wait_us = 1;
    else
        evict->cache_max_wait_us = 0;

    /* Retrieve the timeout value and convert from seconds */
    WT_RET(__wt_config_gets(session, cfg, "cache_stuck_timeout_ms", &cval));
    evict->cache_stuck_timeout_ms = (uint64_t)cval.val;

    /*
     * Resize the thread group if reconfiguring, otherwise the thread group will be initialized as
     * part of creating the connection workers.
     */
    if (reconfig)
        WT_RET(__wt_thread_group_resize(session, &conn->evict_threads, conn->evict_threads_min,
          conn->evict_threads_max, WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL));

    return (0);
}

/* !!!
 * __wt_evict_create --
 *     Set up eviction's internal structures and stats during `wiredtiger_open` to manage eviction.
 *     It must be called exactly once during `wiredtiger_open` and must be called before any
 *     eviction threads are spawned.
 *
 *     Input parameter:
 *       `cfg[]`: An array of configuration strings. This is passed to `__evict_config`, which
 *       handles all eviction-related configs (i.e., `eviction.*`) as part of the eviction
 *       setup process.
 *
 *     Return an error code for invalid configurations, memory allocation, or spinlock
 *     initialization failures.
 */
int
__wt_evict_create(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_EVICT *evict;
    int i;

    conn = S2C(session);

    WT_ASSERT(session, conn->evict == NULL);
    WT_RET(__wt_calloc_one(session, &conn->evict));

    evict = conn->evict;

    /* Use a common routine for run-time configuration options. */
    WT_RET(__wt_evict_config(session, cfg, false));

    /*
     * The lowest possible page read-generation has a special meaning, it marks a page for forcible
     * eviction; don't let it happen by accident.
     */
    evict->read_gen_oldest = WT_READGEN_START_VALUE;
    __wt_atomic_store64(&evict->read_gen, WT_READGEN_START_VALUE);

    WT_RET(__wt_cond_auto_alloc(
      session, "evict server", 10 * WT_THOUSAND, WT_MILLION, &evict->evict_cond));
    WT_RET(__wt_spin_init(session, &evict->evict_pass_lock, "evict pass"));
    WT_RET(__wt_spin_init(session, &evict->evict_queue_lock, "evict queues"));
    WT_RET(__wt_spin_init(session, &evict->evict_walk_lock, "evict walk"));
    if ((ret = __wt_open_internal_session(
           conn, "evict pass", false, WT_SESSION_NO_DATA_HANDLES, 0, &evict->walk_session)) != 0)
        WT_RET_MSG(NULL, ret, "Failed to create session for eviction walks");

    /* Allocate the LRU eviction queue. */
    evict->evict_slots = WTI_EVICT_WALK_BASE + WTI_EVICT_WALK_INCR;
    for (i = 0; i < WTI_EVICT_QUEUE_MAX; ++i) {
        WT_RET(__wt_calloc_def(session, evict->evict_slots, &evict->evict_queues[i].evict_queue));
        WT_RET(__wt_spin_init(session, &evict->evict_queues[i].evict_lock, "evict queue"));
    }

    /* Ensure there are always non-NULL queues. */
    evict->evict_current_queue = evict->evict_fill_queue = &evict->evict_queues[0];
    evict->evict_other_queue = &evict->evict_queues[1];
    evict->evict_urgent_queue = &evict->evict_queues[WTI_EVICT_URGENT_QUEUE];

    /*
     * We get/set some values in the evict statistics (rather than have two copies), configure them.
     */
    __wt_evict_stats_update(session);
    return (0);
}

/* !!!
 * __wt_evict_destroy --
 *     Release all memory and locks related to eviction, ensuring the eviction system is properly
 *     destroyed. It must be called exactly once during `WT_CONNECTION::close`, and must be called
 *     after all the eviction threads are destroyed (via `__wt_evict_threads_destroy`).
 *
 *     Return an error code if the internal eviction session cannot be closed.
 */
int
__wt_evict_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_EVICT *evict;
    int i;

    conn = S2C(session);
    evict = conn->evict;

    if (evict == NULL)
        return (0);

    __wt_cond_destroy(session, &evict->evict_cond);
    __wt_spin_destroy(session, &evict->evict_pass_lock);
    __wt_spin_destroy(session, &evict->evict_queue_lock);
    __wt_spin_destroy(session, &evict->evict_walk_lock);
    if (evict->walk_session != NULL)
        WT_TRET(__wt_session_close_internal(evict->walk_session));

    for (i = 0; i < WTI_EVICT_QUEUE_MAX; ++i) {
        __wt_spin_destroy(session, &evict->evict_queues[i].evict_lock);
        __wt_free(session, evict->evict_queues[i].evict_queue);
    }
    __wt_free(session, conn->evict);
    return (ret);
}

/* !!!
 * __wt_evict_stats_update --
 *     Initialize eviction stats, ensuring they start with initial values during the startup
 *     process. It should be called exactly once when initializing eviction. Running it outside
 *     of startup will not cause functional failures, but it will reset eviction-related stats.
 *
 *     FIXME-WT-13666: Investigate whether this function should be internal to prevent unintended
 *     stat resets.
 */
void
__wt_evict_stats_update(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_CONNECTION_STATS **stats;
    WT_EVICT *evict;

    conn = S2C(session);
    evict = conn->evict;
    stats = conn->stats;

    WT_STATP_CONN_SET(
      session, stats, eviction_maximum_page_size, __wt_atomic_load64(&evict->evict_max_page_size));
    WT_STATP_CONN_SET(
      session, stats, eviction_maximum_milliseconds, __wt_atomic_load64(&evict->evict_max_ms));
    WT_STATP_CONN_SET(
      session, stats, eviction_reentry_hs_eviction_milliseconds, evict->reentry_hs_eviction_ms);
    WT_STATP_CONN_SET(
      session, stats, eviction_maximum_gen_gap, __wt_atomic_load64(&evict->evict_max_gen_gap));
    WT_STATP_CONN_SET(session, stats, eviction_state, __wt_atomic_load32(&evict->flags));
    WT_STATP_CONN_SET(session, stats, eviction_aggressive_set, evict->evict_aggressive_score);
    WT_STATP_CONN_SET(session, stats, eviction_empty_score, evict->evict_empty_score);

    WT_STATP_CONN_SET(session, stats, eviction_active_workers,
      __wt_atomic_load32(&conn->evict_threads.current_threads));
    WT_STATP_CONN_SET(
      session, stats, eviction_stable_state_workers, evict->evict_tune_workers_best);

    /*
     * The number of files with active walks ~= number of hazard pointers in the walk session. Note:
     * reading without locking.
     */
    if (__wt_atomic_loadbool(&conn->evict_server_running))
        WT_STATP_CONN_SET(
          session, stats, eviction_walks_active, evict->walk_session->hazards.num_active);
}
