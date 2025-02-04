/*-
 * Copyright (c) 2025-present MongoDB, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_checkpoint_reset_handle_stats --
 *     Reset handle-related stats.
 */
void
__wt_checkpoint_reset_handle_stats(WT_SESSION_IMPL *session, WT_CKPT_CONNECTION *ckpt)
{
    WT_UNUSED(session);

    ckpt->handle_stats.apply = ckpt->handle_stats.drop = ckpt->handle_stats.lock =
      ckpt->handle_stats.meta_check = ckpt->handle_stats.skip = 0;
    ckpt->handle_stats.apply_time = ckpt->handle_stats.drop_time = ckpt->handle_stats.lock_time =
      ckpt->handle_stats.meta_check_time = ckpt->handle_stats.skip_time = 0;
}

/*
 * __wt_checkpoint_set_handle_stats --
 *     Set handle-related stats.
 */
void
__wt_checkpoint_set_handle_stats(
  WT_SESSION_IMPL *session, WT_CKPT_CONNECTION *ckpt, uint64_t gathering_handles_time_us)
{
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
 * __wt_checkpoint_update_handle_stats --
 *     Update the apply handle-related stats.
 */
void
__wt_checkpoint_update_handle_stats(
  WT_SESSION_IMPL *session, WT_CKPT_CONNECTION *ckpt, uint64_t time_us)
{
    if (F_ISSET(S2BT(session), WT_BTREE_SKIP_CKPT)) {
        ++ckpt->handle_stats.skip;
        ckpt->handle_stats.skip_time += time_us;
    } else {
        ++ckpt->handle_stats.apply;
        ckpt->handle_stats.apply_time += time_us;
    }
}
