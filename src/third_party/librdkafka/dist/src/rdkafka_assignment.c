/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2020-2022, Magnus Edenhill
 *               2023 Confluent Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


/**
 * @name Consumer assignment state.
 *
 * Responsible for managing the state of assigned partitions.
 *
 *
 ******************************************************************************
 * rd_kafka_assignment_serve()
 * ---------------------------
 *
 * It is important to call rd_kafka_assignment_serve() after each change
 * to the assignment through assignment_add, assignment_subtract or
 * assignment_clear as those functions only modify the assignment but does
 * not take any action to transition partitions to or from the assignment
 * states.
 *
 * The reason assignment_serve() is not automatically called from these
 * functions is for the caller to be able to set the current state before
 * the side-effects of serve() kick in, such as the call to
 * rd_kafka_cgrp_assignment_done() that in turn will set the cgrp state.
 *
 *
 *
 ******************************************************************************
 * Querying for committed offsets (.queried list)
 * ----------------------------------------------
 *
 * We only allow one outstanding query (fetch committed offset), this avoids
 * complex handling of partitions that are assigned, unassigned and reassigned
 * all within the window of a OffsetFetch request.
 * Consider the following case:
 *
 *  1. tp1 and tp2 are incrementally assigned.
 *  2. An OffsetFetchRequest is sent for tp1 and tp2
 *  3. tp2 is incremental unassigned.
 *  4. Broker sends OffsetFetchResponse with offsets tp1=10, tp2=20.
 *  4. Some other consumer commits offsets 30 for tp2.
 *  5. tp2 is incrementally assigned again.
 *  6. The OffsetFetchResponse is received.
 *
 * Without extra handling the consumer would start fetching tp1 at offset 10
 * (which is correct) and tp2 at offset 20 (which is incorrect, the last
 *  committed offset is now 30).
 *
 * To alleviate this situation we remove unassigned partitions from the
 * .queried list, and in the OffsetFetch response handler we only use offsets
 * for partitions that are on the .queried list.
 *
 * To make sure the tp1 offset is used and not re-queried we only allow
 * one outstanding OffsetFetch request at the time, meaning that at step 5
 * a new OffsetFetch request will not be sent and tp2 will remain in the
 * .pending list until the outstanding OffsetFetch response is received in
 * step 6. At this point tp2 will transition to .queried and a new
 * OffsetFetch request will be sent.
 *
 * This explanation is more verbose than the code involved.
 *
 ******************************************************************************
 *
 *
 * @remark Try to keep any cgrp state out of this file.
 *
 * FIXME: There are some pretty obvious optimizations that needs to be done here
 *        with regards to partition_list_t lookups. But we can do that when
 *        we know the current implementation works correctly.
 */

#include "rdkafka_int.h"
#include "rdkafka_offset.h"
#include "rdkafka_request.h"


static void rd_kafka_assignment_dump(rd_kafka_t *rk) {
        rd_kafka_dbg(rk, CGRP, "DUMP",
                     "Assignment dump (started_cnt=%d, wait_stop_cnt=%d)",
                     rk->rk_consumer.assignment.started_cnt,
                     rk->rk_consumer.assignment.wait_stop_cnt);

        rd_kafka_topic_partition_list_log(rk, "DUMP_ALL", RD_KAFKA_DBG_CGRP,
                                          rk->rk_consumer.assignment.all);

        rd_kafka_topic_partition_list_log(rk, "DUMP_PND", RD_KAFKA_DBG_CGRP,
                                          rk->rk_consumer.assignment.pending);

        rd_kafka_topic_partition_list_log(rk, "DUMP_QRY", RD_KAFKA_DBG_CGRP,
                                          rk->rk_consumer.assignment.queried);

        rd_kafka_topic_partition_list_log(rk, "DUMP_REM", RD_KAFKA_DBG_CGRP,
                                          rk->rk_consumer.assignment.removed);
}

/**
 * @brief Apply the fetched committed offsets to the current assignment's
 *        queried partitions.
 *
 * @param err is the request-level error, if any. The caller is responsible
 *            for raising this error to the application. It is only used here
 *            to avoid taking actions.
 *
 * Called from the FetchOffsets response handler below.
 */
static void
rd_kafka_assignment_apply_offsets(rd_kafka_t *rk,
                                  rd_kafka_topic_partition_list_t *offsets,
                                  rd_kafka_resp_err_t err) {
        rd_kafka_topic_partition_t *rktpar;

        RD_KAFKA_TPLIST_FOREACH(rktpar, offsets) {
                /* May be NULL, borrow ref. */
                rd_kafka_toppar_t *rktp =
                    rd_kafka_topic_partition_toppar(rk, rktpar);

                if (!rd_kafka_topic_partition_list_del(
                        rk->rk_consumer.assignment.queried, rktpar->topic,
                        rktpar->partition)) {
                        rd_kafka_dbg(rk, CGRP, "OFFSETFETCH",
                                     "Ignoring OffsetFetch "
                                     "response for %s [%" PRId32
                                     "] which is no "
                                     "longer in the queried list "
                                     "(possibly unassigned?)",
                                     rktpar->topic, rktpar->partition);
                        continue;
                }

                if (err == RD_KAFKA_RESP_ERR_STALE_MEMBER_EPOCH ||
                    rktpar->err == RD_KAFKA_RESP_ERR_STALE_MEMBER_EPOCH) {
                        rd_kafka_topic_partition_t *rktpar_copy;

                        rd_kafka_dbg(rk, CGRP, "OFFSETFETCH",
                                     "Adding %s [%" PRId32
                                     "] back to pending "
                                     "list because of stale member epoch",
                                     rktpar->topic, rktpar->partition);

                        rktpar_copy = rd_kafka_topic_partition_list_add_copy(
                            rk->rk_consumer.assignment.pending, rktpar);
                        /* Need to reset offset to STORED to query for
                         * the committed offset again. If the offset is
                         * kept INVALID then auto.offset.reset will be
                         * triggered.
                         *
                         * Not necessary if err is UNSTABLE_OFFSET_COMMIT
                         * because the buffer is retried there. */
                        rktpar_copy->offset = RD_KAFKA_OFFSET_STORED;

                } else if (err == RD_KAFKA_RESP_ERR_UNSTABLE_OFFSET_COMMIT ||
                           rktpar->err ==
                               RD_KAFKA_RESP_ERR_UNSTABLE_OFFSET_COMMIT) {
                        /* Ongoing transactions are blocking offset retrieval.
                         * This is typically retried from the OffsetFetch
                         * handler but we can come here if the assignment
                         * (and thus the assignment.version) was changed while
                         * the OffsetFetch request was in-flight, in which case
                         * we put this partition back on the pending list for
                         * later handling by the assignment state machine. */

                        rd_kafka_dbg(rk, CGRP, "OFFSETFETCH",
                                     "Adding %s [%" PRId32
                                     "] back to pending "
                                     "list because on-going transaction is "
                                     "blocking offset retrieval",
                                     rktpar->topic, rktpar->partition);

                        rd_kafka_topic_partition_list_add_copy(
                            rk->rk_consumer.assignment.pending, rktpar);

                } else if (rktpar->err) {
                        /* Partition-level error */
                        rd_kafka_consumer_err(
                            rk->rk_consumer.q, RD_KAFKA_NODEID_UA, rktpar->err,
                            0, rktpar->topic, rktp, RD_KAFKA_OFFSET_INVALID,
                            "Failed to fetch committed offset for "
                            "group \"%s\" topic %s [%" PRId32 "]: %s",
                            rk->rk_group_id->str, rktpar->topic,
                            rktpar->partition, rd_kafka_err2str(rktpar->err));

                        /* The partition will not be added back to .pending
                         * and thus only reside on .all until the application
                         * unassigns it and possible re-assigns it. */

                } else if (!err) {
                        /* If rktpar->offset is RD_KAFKA_OFFSET_INVALID it means
                         * there was no committed offset for this partition.
                         * serve_pending() will now start this partition
                         * since the offset is set to INVALID (rather than
                         * STORED) and the partition fetcher will employ
                         * auto.offset.reset to know what to do. */

                        /* Add partition to pending list where serve()
                         * will start the fetcher. */
                        rd_kafka_dbg(rk, CGRP, "OFFSETFETCH",
                                     "Adding %s [%" PRId32
                                     "] back to pending "
                                     "list with offset %s",
                                     rktpar->topic, rktpar->partition,
                                     rd_kafka_offset2str(rktpar->offset));

                        rd_kafka_topic_partition_list_add_copy(
                            rk->rk_consumer.assignment.pending, rktpar);
                }
                /* Do nothing for request-level errors (err is set). */
        }

        /* In case of stale member epoch we retry to serve the
         * assignment only after a successful ConsumerGroupHeartbeat. */
        if (offsets->cnt > 0 && err != RD_KAFKA_RESP_ERR_STALE_MEMBER_EPOCH)
                rd_kafka_assignment_serve(rk);
}



/**
 * @brief Reply handler for OffsetFetch queries from the assignment code.
 *
 * @param opaque Is a malloced int64_t* containing the assignment version at the
 *               time of the request.
 *
 * @locality rdkafka main thread
 */
static void rd_kafka_assignment_handle_OffsetFetch(rd_kafka_t *rk,
                                                   rd_kafka_broker_t *rkb,
                                                   rd_kafka_resp_err_t err,
                                                   rd_kafka_buf_t *reply,
                                                   rd_kafka_buf_t *request,
                                                   void *opaque) {
        rd_kafka_topic_partition_list_t *offsets = NULL;
        int64_t *req_assignment_version          = (int64_t *)opaque;
        /* Only allow retries if there's been no change to the assignment,
         * otherwise rely on assignment state machine to retry. */
        rd_bool_t allow_retry =
            *req_assignment_version == rk->rk_consumer.assignment.version;

        if (err == RD_KAFKA_RESP_ERR__DESTROY) {
                /* Termination, quick cleanup. */
                rd_free(req_assignment_version);
                return;
        }

        err = rd_kafka_handle_OffsetFetch(
            rk, rkb, err, reply, request, &offsets,
            rd_true /* Update toppars */, rd_true /* Add parts */, allow_retry);
        if (err == RD_KAFKA_RESP_ERR__IN_PROGRESS) {
                if (offsets)
                        rd_kafka_topic_partition_list_destroy(offsets);
                return; /* retrying */
        }

        rd_free(req_assignment_version);

        /* offsets may be NULL for certain errors, such
         * as ERR__TRANSPORT. */
        if (!offsets && !allow_retry) {
                rd_dassert(err);
                if (!err)
                        err = RD_KAFKA_RESP_ERR__NO_OFFSET;

                rd_kafka_dbg(rk, CGRP, "OFFSET", "Offset fetch error: %s",
                             rd_kafka_err2str(err));
                rd_kafka_consumer_err(
                    rk->rk_consumer.q, rd_kafka_broker_id(rkb), err, 0, NULL,
                    NULL, RD_KAFKA_OFFSET_INVALID,
                    "Failed to fetch committed "
                    "offsets for partitions "
                    "in group \"%s\": %s",
                    rk->rk_group_id->str, rd_kafka_err2str(err));

                return;
        }

        if (err) {
                switch (err) {
                case RD_KAFKA_RESP_ERR_STALE_MEMBER_EPOCH:
                        rk->rk_cgrp->rkcg_consumer_flags |=
                            RD_KAFKA_CGRP_CONSUMER_F_SERVE_PENDING;
                        rd_kafka_cgrp_consumer_expedite_next_heartbeat(
                            rk->rk_cgrp,
                            "OffsetFetch error: Stale member epoch");
                        break;
                case RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID:
                        rd_kafka_cgrp_consumer_expedite_next_heartbeat(
                            rk->rk_cgrp, "OffsetFetch error: Unknown member");
                        break;
                default:
                        rd_kafka_dbg(
                            rk, CGRP, "OFFSET",
                            "Offset fetch error for %d partition(s): %s",
                            offsets->cnt, rd_kafka_err2str(err));
                        rd_kafka_consumer_err(
                            rk->rk_consumer.q, rd_kafka_broker_id(rkb), err, 0,
                            NULL, NULL, RD_KAFKA_OFFSET_INVALID,
                            "Failed to fetch committed offsets for "
                            "%d partition(s) in group \"%s\": %s",
                            offsets->cnt, rk->rk_group_id->str,
                            rd_kafka_err2str(err));
                }
        }

        /* Apply the fetched offsets to the assignment */
        rd_kafka_assignment_apply_offsets(rk, offsets, err);

        rd_kafka_topic_partition_list_destroy(offsets);
}


/**
 * @brief Decommission all partitions in the removed list.
 *
 * @returns >0 if there are removal operations in progress, else 0.
 */
static int rd_kafka_assignment_serve_removals(rd_kafka_t *rk) {
        rd_kafka_topic_partition_t *rktpar;
        int valid_offsets = 0;

        RD_KAFKA_TPLIST_FOREACH(rktpar, rk->rk_consumer.assignment.removed) {
                rd_kafka_toppar_t *rktp =
                    rd_kafka_topic_partition_ensure_toppar(
                        rk, rktpar, rd_true); /* Borrow ref */
                int was_pending, was_queried;

                /* Remove partition from pending and querying lists,
                 * if it happens to be there.
                 * Outstanding OffsetFetch query results will be ignored
                 * for partitions that are no longer on the .queried list. */
                was_pending = rd_kafka_topic_partition_list_del(
                    rk->rk_consumer.assignment.pending, rktpar->topic,
                    rktpar->partition);
                was_queried = rd_kafka_topic_partition_list_del(
                    rk->rk_consumer.assignment.queried, rktpar->topic,
                    rktpar->partition);

                if (rktp->rktp_started) {
                        /* Partition was started, stop the fetcher. */
                        rd_assert(rk->rk_consumer.assignment.started_cnt > 0);

                        rd_kafka_toppar_op_fetch_stop(
                            rktp, RD_KAFKA_REPLYQ(rk->rk_ops, 0));
                        rk->rk_consumer.assignment.wait_stop_cnt++;
                }

                /* Reset the (lib) pause flag which may have been set by
                 * the cgrp when scheduling the rebalance callback. */
                rd_kafka_toppar_op_pause_resume(rktp, rd_false /*resume*/,
                                                RD_KAFKA_TOPPAR_F_LIB_PAUSE,
                                                RD_KAFKA_NO_REPLYQ);

                rd_kafka_toppar_lock(rktp);

                /* Save the currently stored offset and epoch on .removed
                 * so it will be committed below. */
                rd_kafka_topic_partition_set_from_fetch_pos(
                    rktpar, rktp->rktp_stored_pos);
                rd_kafka_topic_partition_set_metadata_from_rktp_stored(rktpar,
                                                                       rktp);
                valid_offsets += !RD_KAFKA_OFFSET_IS_LOGICAL(rktpar->offset);

                /* Reset the stored offset to invalid so that
                 * a manual offset-less commit() or the auto-committer
                 * will not commit a stored offset from a previous
                 * assignment (issue #2782). */
                rd_kafka_offset_store0(
                    rktp, RD_KAFKA_FETCH_POS(RD_KAFKA_OFFSET_INVALID, -1), NULL,
                    0, rd_true, RD_DONT_LOCK);

                /* Partition is no longer desired */
                rd_kafka_toppar_desired_del(rktp);

                rd_assert((rktp->rktp_flags & RD_KAFKA_TOPPAR_F_ASSIGNED));
                rktp->rktp_flags &= ~RD_KAFKA_TOPPAR_F_ASSIGNED;

                rd_kafka_toppar_unlock(rktp);

                rd_kafka_dbg(rk, CGRP, "REMOVE",
                             "Removing %s [%" PRId32
                             "] from assignment "
                             "(started=%s, pending=%s, queried=%s, "
                             "stored offset=%s)",
                             rktpar->topic, rktpar->partition,
                             RD_STR_ToF(rktp->rktp_started),
                             RD_STR_ToF(was_pending), RD_STR_ToF(was_queried),
                             rd_kafka_offset2str(rktpar->offset));
        }

        rd_kafka_dbg(rk, CONSUMER | RD_KAFKA_DBG_CGRP, "REMOVE",
                     "Served %d removed partition(s), "
                     "with %d offset(s) to commit",
                     rk->rk_consumer.assignment.removed->cnt, valid_offsets);

        /* If enable.auto.commit=true:
         * Commit final offsets to broker for the removed partitions,
         * unless this is a consumer destruction with a close() call. */
        if (valid_offsets > 0 &&
            rk->rk_conf.offset_store_method == RD_KAFKA_OFFSET_METHOD_BROKER &&
            rk->rk_cgrp && rk->rk_conf.enable_auto_commit &&
            !rd_kafka_destroy_flags_no_consumer_close(rk))
                rd_kafka_cgrp_assigned_offsets_commit(
                    rk->rk_cgrp, rk->rk_consumer.assignment.removed,
                    rd_false /* use offsets from .removed */,
                    "unassigned partitions");

        rd_kafka_topic_partition_list_clear(rk->rk_consumer.assignment.removed);

        return rk->rk_consumer.assignment.wait_stop_cnt +
               rk->rk_consumer.wait_commit_cnt;
}


/**
 * @brief Serve all partitions in the pending list.
 *
 * This either (asynchronously) queries the partition's committed offset, or
 * if the start offset is known, starts the partition fetcher.
 *
 * @returns >0 if there are pending operations in progress for the current
 *          assignment, else 0.
 */
static int rd_kafka_assignment_serve_pending(rd_kafka_t *rk) {
        rd_kafka_topic_partition_list_t *partitions_to_query = NULL;
        /* We can query committed offsets only if all of the following are true:
         *  - We have a group coordinator.
         *  - There are no outstanding commits (since we might need to
         *    read back those commits as our starting position).
         *  - There are no outstanding queries already (since we want to
         *    avoid using a earlier queries response for a partition that
         *    is unassigned and then assigned again).
         */
        rd_kafka_broker_t *coord =
            rk->rk_cgrp ? rd_kafka_cgrp_get_coord(rk->rk_cgrp) : NULL;
        rd_bool_t can_query_offsets =
            coord && rk->rk_consumer.wait_commit_cnt == 0 &&
            rk->rk_consumer.assignment.queried->cnt == 0;
        int i;

        if (can_query_offsets)
                partitions_to_query = rd_kafka_topic_partition_list_new(
                    rk->rk_consumer.assignment.pending->cnt);

        /* Scan the list backwards so removals are cheap (no array shuffle) */
        for (i = rk->rk_consumer.assignment.pending->cnt - 1; i >= 0; i--) {
                rd_kafka_topic_partition_t *rktpar =
                    &rk->rk_consumer.assignment.pending->elems[i];
                /* Borrow ref */
                rd_kafka_toppar_t *rktp =
                    rd_kafka_topic_partition_ensure_toppar(rk, rktpar, rd_true);

                rd_assert(!rktp->rktp_started);

                if (!RD_KAFKA_OFFSET_IS_LOGICAL(rktpar->offset) ||
                    rktpar->offset == RD_KAFKA_OFFSET_BEGINNING ||
                    rktpar->offset == RD_KAFKA_OFFSET_END ||
                    rktpar->offset == RD_KAFKA_OFFSET_INVALID ||
                    rktpar->offset <= RD_KAFKA_OFFSET_TAIL_BASE) {
                        /* The partition fetcher can handle absolute
                         * as well as beginning/end/tail start offsets, so we're
                         * ready to start the fetcher now.
                         * The INVALID offset means there was no committed
                         * offset and the partition fetcher will employ
                         * auto.offset.reset.
                         *
                         * Start fetcher for partition and forward partition's
                         * fetchq to consumer group's queue. */

                        rd_kafka_dbg(rk, CGRP, "SRVPEND",
                                     "Starting pending assigned partition "
                                     "%s [%" PRId32 "] at %s",
                                     rktpar->topic, rktpar->partition,
                                     rd_kafka_fetch_pos2str(
                                         rd_kafka_topic_partition_get_fetch_pos(
                                             rktpar)));

                        /* Reset the (lib) pause flag which may have been set by
                         * the cgrp when scheduling the rebalance callback. */
                        rd_kafka_toppar_op_pause_resume(
                            rktp, rd_false /*resume*/,
                            RD_KAFKA_TOPPAR_F_LIB_PAUSE, RD_KAFKA_NO_REPLYQ);

                        /* Start the fetcher */
                        rktp->rktp_started = rd_true;
                        rk->rk_consumer.assignment.started_cnt++;

                        rd_kafka_toppar_op_fetch_start(
                            rktp,
                            rd_kafka_topic_partition_get_fetch_pos(rktpar),
                            rk->rk_consumer.q, RD_KAFKA_NO_REPLYQ);


                } else if (can_query_offsets) {
                        /* Else use the last committed offset for partition.
                         * We can't rely on any internal cached committed offset
                         * so we'll accumulate a list of partitions that need
                         * to be queried and then send FetchOffsetsRequest
                         * to the group coordinator. */

                        rd_dassert(!rd_kafka_topic_partition_list_find(
                            rk->rk_consumer.assignment.queried, rktpar->topic,
                            rktpar->partition));

                        rd_kafka_topic_partition_list_add_copy(
                            partitions_to_query, rktpar);

                        rd_kafka_topic_partition_list_add_copy(
                            rk->rk_consumer.assignment.queried, rktpar);

                        rd_kafka_dbg(rk, CGRP, "SRVPEND",
                                     "Querying committed offset for pending "
                                     "assigned partition %s [%" PRId32 "]",
                                     rktpar->topic, rktpar->partition);


                } else {
                        rd_kafka_dbg(
                            rk, CGRP, "SRVPEND",
                            "Pending assignment partition "
                            "%s [%" PRId32
                            "] can't fetch committed "
                            "offset yet "
                            "(cgrp state %s, awaiting %d commits, "
                            "%d partition(s) already being queried)",
                            rktpar->topic, rktpar->partition,
                            rk->rk_cgrp
                                ? rd_kafka_cgrp_state_names[rk->rk_cgrp
                                                                ->rkcg_state]
                                : "n/a",
                            rk->rk_consumer.wait_commit_cnt,
                            rk->rk_consumer.assignment.queried->cnt);

                        continue; /* Keep rktpar on pending list */
                }

                /* Remove rktpar from the pending list */
                rd_kafka_topic_partition_list_del_by_idx(
                    rk->rk_consumer.assignment.pending, i);
        }


        if (!can_query_offsets) {
                if (coord)
                        rd_kafka_broker_destroy(coord);
                return rk->rk_consumer.assignment.pending->cnt +
                       rk->rk_consumer.assignment.queried->cnt;
        }


        if (partitions_to_query->cnt > 0) {
                int64_t *req_assignment_version = rd_malloc(sizeof(int64_t));
                *req_assignment_version = rk->rk_consumer.assignment.version;

                rd_kafka_dbg(rk, CGRP, "OFFSETFETCH",
                             "Fetching committed offsets for "
                             "%d pending partition(s) in assignment",
                             partitions_to_query->cnt);

                rd_kafka_OffsetFetchRequest(
                    coord, rk->rk_group_id->str, partitions_to_query, rd_false,
                    -1, NULL,
                    rk->rk_conf.isolation_level ==
                        RD_KAFKA_READ_COMMITTED /*require_stable_offsets*/,
                    0, /* Timeout */
                    RD_KAFKA_REPLYQ(rk->rk_ops, 0),
                    rd_kafka_assignment_handle_OffsetFetch,
                    /* Must be freed by handler */
                    (void *)req_assignment_version);
        }

        if (coord)
                rd_kafka_broker_destroy(coord);

        rd_kafka_topic_partition_list_destroy(partitions_to_query);

        return rk->rk_consumer.assignment.pending->cnt +
               rk->rk_consumer.assignment.queried->cnt;
}



/**
 * @brief Serve updates to the assignment.
 *
 * Call on:
 * - assignment changes
 * - wait_commit_cnt reaches 0
 * - partition fetcher is stopped
 */
void rd_kafka_assignment_serve(rd_kafka_t *rk) {
        int inp_removals = 0;
        int inp_pending  = 0;

        rd_kafka_assignment_dump(rk);

        /* Serve any partitions that should be removed */
        if (rk->rk_consumer.assignment.removed->cnt > 0)
                inp_removals = rd_kafka_assignment_serve_removals(rk);

        /* Serve any partitions in the pending list that need further action,
         * unless we're waiting for a previous assignment change (an unassign
         * in some form) to propagate, or outstanding offset commits
         * to finish (since we might need the committed offsets as start
         * offsets). */
        if (rk->rk_consumer.assignment.wait_stop_cnt == 0 &&
            rk->rk_consumer.wait_commit_cnt == 0 && inp_removals == 0 &&
            rk->rk_consumer.assignment.pending->cnt > 0)
                inp_pending = rd_kafka_assignment_serve_pending(rk);

        if (inp_removals + inp_pending +
                rk->rk_consumer.assignment.queried->cnt +
                rk->rk_consumer.assignment.wait_stop_cnt +
                rk->rk_consumer.wait_commit_cnt ==
            0) {
                /* No assignment operations in progress,
                 * signal assignment done back to cgrp to let it
                 * transition to its next state if necessary.
                 * We may emit this signalling more than necessary and it is
                 * up to the cgrp to only take action if needed, based on its
                 * state. */
                rd_kafka_cgrp_assignment_done(rk->rk_cgrp);
        } else {
                rd_kafka_dbg(rk, CGRP, "ASSIGNMENT",
                             "Current assignment of %d partition(s) "
                             "with %d pending adds, %d offset queries, "
                             "%d partitions awaiting stop and "
                             "%d offset commits in progress",
                             rk->rk_consumer.assignment.all->cnt, inp_pending,
                             rk->rk_consumer.assignment.queried->cnt,
                             rk->rk_consumer.assignment.wait_stop_cnt,
                             rk->rk_consumer.wait_commit_cnt);
        }
}


/**
 * @returns true if the current or previous assignment has operations in
 *          progress, such as waiting for partition fetchers to stop.
 */
rd_bool_t rd_kafka_assignment_in_progress(rd_kafka_t *rk) {
        return rk->rk_consumer.wait_commit_cnt > 0 ||
               rk->rk_consumer.assignment.wait_stop_cnt > 0 ||
               rk->rk_consumer.assignment.pending->cnt > 0 ||
               rk->rk_consumer.assignment.queried->cnt > 0 ||
               rk->rk_consumer.assignment.removed->cnt > 0;
}


/**
 * @brief Clear the current assignment.
 *
 * @remark Make sure to call rd_kafka_assignment_serve() after successful
 *         return from this function.
 *
 * @returns the number of partitions removed.
 */
int rd_kafka_assignment_clear(rd_kafka_t *rk) {
        int cnt = rk->rk_consumer.assignment.all->cnt;

        if (cnt == 0) {
                rd_kafka_dbg(rk, CONSUMER | RD_KAFKA_DBG_CGRP, "CLEARASSIGN",
                             "No current assignment to clear");
                return 0;
        }

        rd_kafka_dbg(rk, CONSUMER | RD_KAFKA_DBG_CGRP, "CLEARASSIGN",
                     "Clearing current assignment of %d partition(s)",
                     rk->rk_consumer.assignment.all->cnt);

        rd_kafka_topic_partition_list_clear(rk->rk_consumer.assignment.pending);
        rd_kafka_topic_partition_list_clear(rk->rk_consumer.assignment.queried);

        rd_kafka_topic_partition_list_add_list(
            rk->rk_consumer.assignment.removed, rk->rk_consumer.assignment.all);
        rd_kafka_topic_partition_list_clear(rk->rk_consumer.assignment.all);

        rk->rk_consumer.assignment.version++;

        return cnt;
}


/**
 * @brief Adds \p partitions to the current assignment.
 *
 * Will return error if trying to add a partition that is already in the
 * assignment.
 *
 * @remark Make sure to call rd_kafka_assignment_serve() after successful
 *         return from this function.
 */
rd_kafka_error_t *
rd_kafka_assignment_add(rd_kafka_t *rk,
                        rd_kafka_topic_partition_list_t *partitions) {
        rd_bool_t was_empty = rk->rk_consumer.assignment.all->cnt == 0;
        int i;

        /* Make sure there are no duplicates, invalid partitions, or
         * invalid offsets in the input partitions. */
        rd_kafka_topic_partition_list_sort(partitions, NULL, NULL);

        for (i = 0; i < partitions->cnt; i++) {
                rd_kafka_topic_partition_t *rktpar = &partitions->elems[i];
                const rd_kafka_topic_partition_t *prev =
                    i > 0 ? &partitions->elems[i - 1] : NULL;

                if (RD_KAFKA_OFFSET_IS_LOGICAL(rktpar->offset) &&
                    rktpar->offset != RD_KAFKA_OFFSET_BEGINNING &&
                    rktpar->offset != RD_KAFKA_OFFSET_END &&
                    rktpar->offset != RD_KAFKA_OFFSET_STORED &&
                    rktpar->offset != RD_KAFKA_OFFSET_INVALID &&
                    rktpar->offset > RD_KAFKA_OFFSET_TAIL_BASE)
                        return rd_kafka_error_new(
                            RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "%s [%" PRId32
                            "] has invalid start offset %" PRId64,
                            rktpar->topic, rktpar->partition, rktpar->offset);

                if (prev && !rd_kafka_topic_partition_cmp(rktpar, prev))
                        return rd_kafka_error_new(
                            RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Duplicate %s [%" PRId32 "] in input list",
                            rktpar->topic, rktpar->partition);

                if (rd_kafka_topic_partition_list_find(
                        rk->rk_consumer.assignment.all, rktpar->topic,
                        rktpar->partition))
                        return rd_kafka_error_new(RD_KAFKA_RESP_ERR__CONFLICT,
                                                  "%s [%" PRId32
                                                  "] is already part of the "
                                                  "current assignment",
                                                  rktpar->topic,
                                                  rktpar->partition);

                /* Translate RD_KAFKA_OFFSET_INVALID to RD_KAFKA_OFFSET_STORED,
                 * i.e., read from committed offset, since we use INVALID
                 * internally to differentiate between querying for
                 * committed offset (STORED) and no committed offset (INVALID).
                 */
                if (rktpar->offset == RD_KAFKA_OFFSET_INVALID)
                        rktpar->offset = RD_KAFKA_OFFSET_STORED;

                /* Get toppar object for each partition.
                 * This is to make sure the rktp stays alive while unassigning
                 * any previous assignment in the call to
                 * assignment_clear() below. */
                rd_kafka_topic_partition_ensure_toppar(rk, rktpar, rd_true);
        }

        /* Mark all partition objects as assigned and reset the stored
         * offsets back to invalid in case it was explicitly stored during
         * the time the partition was not assigned. */
        for (i = 0; i < partitions->cnt; i++) {
                rd_kafka_topic_partition_t *rktpar = &partitions->elems[i];
                rd_kafka_toppar_t *rktp =
                    rd_kafka_topic_partition_ensure_toppar(rk, rktpar, rd_true);

                rd_kafka_toppar_lock(rktp);

                rd_assert(!(rktp->rktp_flags & RD_KAFKA_TOPPAR_F_ASSIGNED));
                rktp->rktp_flags |= RD_KAFKA_TOPPAR_F_ASSIGNED;

                /* Reset the stored offset to INVALID to avoid the race
                 * condition described in rdkafka_offset.h */
                rd_kafka_offset_store0(
                    rktp, RD_KAFKA_FETCH_POS(RD_KAFKA_OFFSET_INVALID, -1), NULL,
                    0, rd_true /* force */, RD_DONT_LOCK);

                rd_kafka_toppar_unlock(rktp);
        }


        /* Add the new list of partitions to the current assignment.
         * Only need to sort the final assignment if it was non-empty
         * to begin with since \p partitions is sorted above. */
        rd_kafka_topic_partition_list_add_list(rk->rk_consumer.assignment.all,
                                               partitions);
        if (!was_empty)
                rd_kafka_topic_partition_list_sort(
                    rk->rk_consumer.assignment.all, NULL, NULL);

        /* And add to .pending for serve_pending() to handle. */
        rd_kafka_topic_partition_list_add_list(
            rk->rk_consumer.assignment.pending, partitions);


        rd_kafka_dbg(rk, CONSUMER | RD_KAFKA_DBG_CGRP, "ASSIGNMENT",
                     "Added %d partition(s) to assignment which "
                     "now consists of %d partition(s) where of %d are in "
                     "pending state and %d are being queried",
                     partitions->cnt, rk->rk_consumer.assignment.all->cnt,
                     rk->rk_consumer.assignment.pending->cnt,
                     rk->rk_consumer.assignment.queried->cnt);

        rk->rk_consumer.assignment.version++;

        return NULL;
}


/**
 * @brief Remove \p partitions from the current assignment.
 *
 * Will return error if trying to remove a partition that is not in the
 * assignment.
 *
 * @remark Make sure to call rd_kafka_assignment_serve() after successful
 *         return from this function.
 */
rd_kafka_error_t *
rd_kafka_assignment_subtract(rd_kafka_t *rk,
                             rd_kafka_topic_partition_list_t *partitions) {
        int i;
        int matched_queried_partitions = 0;
        int assignment_pre_cnt;

        if (rk->rk_consumer.assignment.all->cnt == 0 && partitions->cnt > 0)
                return rd_kafka_error_new(
                    RD_KAFKA_RESP_ERR__INVALID_ARG,
                    "Can't subtract from empty assignment");

        /* Verify that all partitions in \p partitions are in the assignment
         * before starting to modify the assignment. */
        rd_kafka_topic_partition_list_sort(partitions, NULL, NULL);

        for (i = 0; i < partitions->cnt; i++) {
                rd_kafka_topic_partition_t *rktpar = &partitions->elems[i];

                if (!rd_kafka_topic_partition_list_find(
                        rk->rk_consumer.assignment.all, rktpar->topic,
                        rktpar->partition))
                        return rd_kafka_error_new(
                            RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "%s [%" PRId32
                            "] can't be unassigned since "
                            "it is not in the current assignment",
                            rktpar->topic, rktpar->partition);

                rd_kafka_topic_partition_ensure_toppar(rk, rktpar, rd_true);
        }


        assignment_pre_cnt = rk->rk_consumer.assignment.all->cnt;

        /* Remove partitions in reverse order to avoid excessive
         * array shuffling of .all.
         * Add the removed partitions to .pending for serve() to handle. */
        for (i = partitions->cnt - 1; i >= 0; i--) {
                const rd_kafka_topic_partition_t *rktpar =
                    &partitions->elems[i];

                if (!rd_kafka_topic_partition_list_del(
                        rk->rk_consumer.assignment.all, rktpar->topic,
                        rktpar->partition))
                        RD_BUG("Removed partition %s [%" PRId32
                               "] not found "
                               "in assignment.all",
                               rktpar->topic, rktpar->partition);

                if (rd_kafka_topic_partition_list_del(
                        rk->rk_consumer.assignment.queried, rktpar->topic,
                        rktpar->partition))
                        matched_queried_partitions++;
                else
                        rd_kafka_topic_partition_list_del(
                            rk->rk_consumer.assignment.pending, rktpar->topic,
                            rktpar->partition);

                /* Add to .removed list which will be served by
                 * serve_removals(). */
                rd_kafka_topic_partition_list_add_copy(
                    rk->rk_consumer.assignment.removed, rktpar);
        }

        rd_kafka_dbg(rk, CGRP, "REMOVEASSIGN",
                     "Removed %d partition(s) "
                     "(%d with outstanding offset queries) from assignment "
                     "of %d partition(s)",
                     partitions->cnt, matched_queried_partitions,
                     assignment_pre_cnt);

        if (rk->rk_consumer.assignment.all->cnt == 0) {
                /* Some safe checking */
                rd_assert(rk->rk_consumer.assignment.pending->cnt == 0);
                rd_assert(rk->rk_consumer.assignment.queried->cnt == 0);
        }

        rk->rk_consumer.assignment.version++;

        return NULL;
}


/**
 * @brief Call when partition fetcher has stopped.
 */
void rd_kafka_assignment_partition_stopped(rd_kafka_t *rk,
                                           rd_kafka_toppar_t *rktp) {
        rd_assert(rk->rk_consumer.assignment.wait_stop_cnt > 0);
        rk->rk_consumer.assignment.wait_stop_cnt--;

        rd_assert(rktp->rktp_started);
        rktp->rktp_started = rd_false;

        rd_assert(rk->rk_consumer.assignment.started_cnt > 0);
        rk->rk_consumer.assignment.started_cnt--;

        /* If this was the last partition we awaited stop for, serve the
         * assignment to transition any existing assignment to the next state */
        if (rk->rk_consumer.assignment.wait_stop_cnt == 0) {
                rd_kafka_dbg(rk, CGRP, "STOPSERVE",
                             "All partitions awaiting stop are now "
                             "stopped: serving assignment");
                rd_kafka_assignment_serve(rk);
        }
}


/**
 * @brief Pause fetching of the currently assigned partitions.
 *
 * Partitions will be resumed by calling rd_kafka_assignment_resume() or
 * from either serve_removals() or serve_pending() above.
 */
void rd_kafka_assignment_pause(rd_kafka_t *rk, const char *reason) {

        if (rk->rk_consumer.assignment.all->cnt == 0)
                return;

        rd_kafka_dbg(rk, CGRP, "PAUSE",
                     "Pausing fetchers for %d assigned partition(s): %s",
                     rk->rk_consumer.assignment.all->cnt, reason);

        rd_kafka_toppars_pause_resume(rk, rd_true /*pause*/, RD_ASYNC,
                                      RD_KAFKA_TOPPAR_F_LIB_PAUSE,
                                      rk->rk_consumer.assignment.all);
}

/**
 * @brief Resume fetching of the currently assigned partitions which have
 *        previously been paused by rd_kafka_assignment_pause().
 */
void rd_kafka_assignment_resume(rd_kafka_t *rk, const char *reason) {

        if (rk->rk_consumer.assignment.all->cnt == 0)
                return;

        rd_kafka_dbg(rk, CGRP, "PAUSE",
                     "Resuming fetchers for %d assigned partition(s): %s",
                     rk->rk_consumer.assignment.all->cnt, reason);

        rd_kafka_toppars_pause_resume(rk, rd_false /*resume*/, RD_ASYNC,
                                      RD_KAFKA_TOPPAR_F_LIB_PAUSE,
                                      rk->rk_consumer.assignment.all);
}



/**
 * @brief Destroy assignment state (but not \p assignment itself)
 */
void rd_kafka_assignment_destroy(rd_kafka_t *rk) {
        if (!rk->rk_consumer.assignment.all)
                return; /* rd_kafka_assignment_init() not called */
        rd_kafka_topic_partition_list_destroy(rk->rk_consumer.assignment.all);
        rd_kafka_topic_partition_list_destroy(
            rk->rk_consumer.assignment.pending);
        rd_kafka_topic_partition_list_destroy(
            rk->rk_consumer.assignment.queried);
        rd_kafka_topic_partition_list_destroy(
            rk->rk_consumer.assignment.removed);
}


/**
 * @brief Initialize the assignment struct.
 */
void rd_kafka_assignment_init(rd_kafka_t *rk) {
        rk->rk_consumer.assignment.all = rd_kafka_topic_partition_list_new(100);
        rk->rk_consumer.assignment.pending =
            rd_kafka_topic_partition_list_new(100);
        rk->rk_consumer.assignment.queried =
            rd_kafka_topic_partition_list_new(100);
        rk->rk_consumer.assignment.removed =
            rd_kafka_topic_partition_list_new(100);
}
