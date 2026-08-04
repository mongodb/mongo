/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Follower role: picking up the leader's checkpoints from the page log, and the role transitions.
 * Everything a running follower executes goes through the same reader and worker loops as a leader,
 * in node.c.
 */

#include "schema_disagg_abort.h"

/*
 * follower_pick_up_checkpoint --
 *     Fetch the latest complete checkpoint from the page log and apply it to the follower
 *     connection. Writes the ready sentinel after the first successful pickup.
 *
 * Skip a checkpoint already adopted: a lone follower has no leader producing new ones, and a
 *     leader's checkpoint writes nothing when nothing changed.
 */
static void
follower_pick_up_checkpoint(
  WORKLOAD_STATE *state, WT_SESSION *session, WT_PAGE_LOG *page_log, bool *picked_up)
{
    WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS ckpt_args = {0};

    const int ret = page_log->pl_get_complete_checkpoint(page_log, session, &ckpt_args);
    if (ret == WT_NOTFOUND)
        return;
    testutil_check(ret);

    if (ckpt_args.checkpoint_lsn == state->adopted_ckpt_lsn) {
        free(ckpt_args.checkpoint_metadata.mem);
        return;
    }

    WT_CONNECTION *conn = session->connection;
    char meta_config[4096];
    testutil_snprintf(meta_config, sizeof(meta_config), "disaggregated=(checkpoint_meta=\"%.*s\")",
      (int)ckpt_args.checkpoint_metadata.size, (const char *)ckpt_args.checkpoint_metadata.data);
    testutil_check(conn->reconfigure(conn, meta_config));
    free(ckpt_args.checkpoint_metadata.mem);
    state->adopted_ckpt_lsn = ckpt_args.checkpoint_lsn;

    if (!*picked_up) {
        testutil_sentinel(NULL, FOLLOWER_READY_FILE);
        *picked_up = true;
    }
}

/*
 * follower_adopt_latest --
 *     Adopt the latest complete checkpoint from the page log, if there is one: the last act of
 *     following before a lone step-up. A node without a live peer has no reader picking checkpoints
 *     up, and must not lead over a stale or empty view of the database.
 */
void
follower_adopt_latest(WORKLOAD_STATE *state)
{
    WT_CONNECTION *conn = state->conn;

    WT_SESSION *session;
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    WT_PAGE_LOG *page_log;
    testutil_check(conn->get_page_log(conn, "palite", &page_log));

    bool picked_up = true; /* No ready sentinel: this is not the initial pickup. */
    follower_pick_up_checkpoint(state, session, page_log, &picked_up);

    testutil_check(page_log->terminate(page_log, NULL));
    testutil_check(session->close(session, NULL));

    println("Node %" PRIu32 ": adopted the latest checkpoint before step-up", state->cfg->node_id);
}

/*
 * follower_leave --
 *     Step out of following: nothing to do. The reader was already joined when the phase stopped,
 *     and the connection stays open for the step-up's reconfigure.
 */
static void
follower_leave(WORKLOAD_STATE *state, uint64_t final_counter)
{
    WT_UNUSED(state);
    WT_UNUSED(final_counter);
}

/*
 * follower_enter --
 *     This method is called when the node enters the follower role.
 */
static void
follower_enter(WORKLOAD_STATE *state, uint64_t final_counter)
{
    /* This node produced its checkpoints rather than adopting them; nothing to skip. */
    state->adopted_ckpt_lsn = 0;

    /*
     * This node keeps publishing the operations it applies for the new leader, so its frontier must
     * cover the whole term. Everything the new leader relays was allocated above it.
     */
    set_frontier(state->conn, final_counter);
}

/*
 * follower_checkpoint --
 *     Adopt the leader's checkpoint. The drain barrier comes first, so everything at or below the
 *     checkpoint's stable frontier is applied locally before its metadata is adopted; later
 *     publishes and commits then stay above the adopted stable values.
 */
static void
follower_checkpoint(
  WORKLOAD_STATE *state, WT_SESSION *session, CKPT_CTX *ckpt, const SCHEMA_EVENT *ev)
{
    WT_UNUSED(ev); /* A follower adopts what the page log holds; the event is only the trigger. */

    workload_drain_barrier(state);
    follower_pick_up_checkpoint(state, session, ckpt->page_log, &ckpt->picked_up);
}

const NODE_ROLE node_role_follower = {"follower", "debug=(skip_checkpoint=true)", false,
  follower_leave, follower_enter, follower_checkpoint};
