/*-
 * Copyright (c) 2025-present MongoDB, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_checkpoint_handle_stats_clear --
 *     Clear handle-related stats.
 */
void
__wt_checkpoint_handle_stats_clear(WT_SESSION_IMPL *session)
{
    WT_CKPT_CONNECTION *ckpt = &S2C(session)->ckpt;

    ckpt->handle_stats.apply = ckpt->handle_stats.drop = ckpt->handle_stats.lock =
      ckpt->handle_stats.meta_check = ckpt->handle_stats.skip = 0;
    ckpt->handle_stats.apply_time = ckpt->handle_stats.drop_time = ckpt->handle_stats.lock_time =
      ckpt->handle_stats.meta_check_time = ckpt->handle_stats.skip_time = 0;
}

/*
 * __wt_checkpoint_timer_stats_clear --
 *     Clear timer-related stats.
 */
void
__wt_checkpoint_timer_stats_clear(WT_SESSION_IMPL *session)
{
    WT_CKPT_CONNECTION *ckpt = &S2C(session)->ckpt;

    ckpt->prepare.min = UINT64_MAX;
    ckpt->ckpt_api.min = UINT64_MAX;
    ckpt->scrub.min = UINT64_MAX;
}

/*
 * __wt_checkpoint_handle_stats --
 *     Update handle-related stats.
 */
void
__wt_checkpoint_handle_stats(WT_SESSION_IMPL *session, uint64_t gathering_handles_time_us)
{
    WT_CKPT_CONNECTION *ckpt = &S2C(session)->ckpt;

    WT_STAT_CONN_SET(session, checkpoint_handle_applied, ckpt->handle_stats.apply);
    WT_STAT_CONN_SET(session, checkpoint_handle_apply_duration, ckpt->handle_stats.apply_time);
    WT_STAT_CONN_SET(session, checkpoint_handle_drop_duration, ckpt->handle_stats.drop_time);
    WT_STAT_CONN_SET(session, checkpoint_handle_dropped, ckpt->handle_stats.drop);
    WT_STAT_CONN_SET(session, checkpoint_handle_duration, gathering_handles_time_us);
    WT_STAT_CONN_SET(session, checkpoint_handle_lock_duration, ckpt->handle_stats.lock_time);
    WT_STAT_CONN_SET(session, checkpoint_handle_locked, ckpt->handle_stats.lock);
    WT_STAT_CONN_SET(session, checkpoint_handle_meta_checked, ckpt->handle_stats.meta_check);
    WT_STAT_CONN_SET(
      session, checkpoint_handle_meta_check_duration, ckpt->handle_stats.meta_check_time);
    WT_STAT_CONN_SET(session, checkpoint_handle_skipped, ckpt->handle_stats.skip);
    WT_STAT_CONN_SET(session, checkpoint_handle_skip_duration, ckpt->handle_stats.skip_time);
}

/*
 * __wt_checkpoint_timer_stats --
 *     Update timer-related stats.
 */
void
__wt_checkpoint_timer_stats(WT_SESSION_IMPL *session)
{
    WT_CKPT_CONNECTION *ckpt = &S2C(session)->ckpt;

    WT_STAT_CONN_SET(session, checkpoint_scrub_max, ckpt->scrub.max);
    if (ckpt->scrub.min != UINT64_MAX)
        WT_STAT_CONN_SET(session, checkpoint_scrub_min, ckpt->scrub.min);
    WT_STAT_CONN_SET(session, checkpoint_scrub_recent, ckpt->scrub.recent);
    WT_STAT_CONN_SET(session, checkpoint_scrub_total, ckpt->scrub.total);

    WT_STAT_CONN_SET(session, checkpoint_prep_max, ckpt->prepare.max);
    if (ckpt->prepare.min != UINT64_MAX)
        WT_STAT_CONN_SET(session, checkpoint_prep_min, ckpt->prepare.min);
    WT_STAT_CONN_SET(session, checkpoint_prep_recent, ckpt->prepare.recent);
    WT_STAT_CONN_SET(session, checkpoint_prep_total, ckpt->prepare.total);

    WT_STAT_CONN_SET(session, checkpoint_time_max, ckpt->ckpt_api.max);
    if (ckpt->ckpt_api.min != UINT64_MAX)
        WT_STAT_CONN_SET(session, checkpoint_time_min, ckpt->ckpt_api.min);
    WT_STAT_CONN_SET(session, checkpoint_time_recent, ckpt->ckpt_api.recent);
    WT_STAT_CONN_SET(session, checkpoint_time_total, ckpt->ckpt_api.total);
}

/*
 * __wt_checkpoint_apply_or_skip_handle_stats --
 *     Update the apply or skip handle-related stats.
 */
void
__wt_checkpoint_apply_or_skip_handle_stats(WT_SESSION_IMPL *session, uint64_t time_us)
{
    WT_CKPT_CONNECTION *ckpt = &S2C(session)->ckpt;

    if (F_ISSET(S2BT(session), WT_BTREE_SKIP_CKPT)) {
        ++ckpt->handle_stats.skip;
        ckpt->handle_stats.skip_time += time_us;
    } else {
        ++ckpt->handle_stats.apply;
        ckpt->handle_stats.apply_time += time_us;
    }
}
