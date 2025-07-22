/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
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

#include "rdkafka_int.h"
#include "rdkafka_broker.h"
#include "rdkafka_request.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_assignor.h"
#include "rdkafka_offset.h"
#include "rdkafka_metadata.h"
#include "rdkafka_cgrp.h"
#include "rdkafka_interceptor.h"
#include "rdmap.h"

#include "rdunittest.h"

#include <ctype.h>
#include <stdarg.h>

static void rd_kafka_cgrp_offset_commit_tmr_cb(rd_kafka_timers_t *rkts,
                                               void *arg);
static rd_kafka_error_t *
rd_kafka_cgrp_assign(rd_kafka_cgrp_t *rkcg,
                     rd_kafka_topic_partition_list_t *assignment);
static rd_kafka_error_t *rd_kafka_cgrp_unassign(rd_kafka_cgrp_t *rkcg);
static rd_kafka_error_t *
rd_kafka_cgrp_incremental_assign(rd_kafka_cgrp_t *rkcg,
                                 rd_kafka_topic_partition_list_t *partitions);
static rd_kafka_error_t *
rd_kafka_cgrp_incremental_unassign(rd_kafka_cgrp_t *rkcg,
                                   rd_kafka_topic_partition_list_t *partitions);

static rd_kafka_op_res_t rd_kafka_cgrp_op_serve(rd_kafka_t *rk,
                                                rd_kafka_q_t *rkq,
                                                rd_kafka_op_t *rko,
                                                rd_kafka_q_cb_type_t cb_type,
                                                void *opaque);

static void rd_kafka_cgrp_group_leader_reset(rd_kafka_cgrp_t *rkcg,
                                             const char *reason);

static RD_INLINE int rd_kafka_cgrp_try_terminate(rd_kafka_cgrp_t *rkcg);

static void rd_kafka_cgrp_revoke_all_rejoin(rd_kafka_cgrp_t *rkcg,
                                            rd_bool_t assignment_lost,
                                            rd_bool_t initiating,
                                            const char *reason);
static void rd_kafka_cgrp_revoke_all_rejoin_maybe(rd_kafka_cgrp_t *rkcg,
                                                  rd_bool_t assignment_lost,
                                                  rd_bool_t initiating,
                                                  const char *reason);

static void rd_kafka_cgrp_group_is_rebalancing(rd_kafka_cgrp_t *rkcg);

static void
rd_kafka_cgrp_max_poll_interval_check_tmr_cb(rd_kafka_timers_t *rkts,
                                             void *arg);
static rd_kafka_resp_err_t
rd_kafka_cgrp_subscribe(rd_kafka_cgrp_t *rkcg,
                        rd_kafka_topic_partition_list_t *rktparlist);

static void rd_kafka_cgrp_group_assignment_set(
    rd_kafka_cgrp_t *rkcg,
    const rd_kafka_topic_partition_list_t *partitions);
static void rd_kafka_cgrp_group_assignment_modify(
    rd_kafka_cgrp_t *rkcg,
    rd_bool_t add,
    const rd_kafka_topic_partition_list_t *partitions);

static void
rd_kafka_cgrp_handle_assignment(rd_kafka_cgrp_t *rkcg,
                                rd_kafka_topic_partition_list_t *assignment);

static void rd_kafka_cgrp_consumer_assignment_done(rd_kafka_cgrp_t *rkcg);

/**
 * @returns true if the current assignment is lost.
 */
rd_bool_t rd_kafka_cgrp_assignment_is_lost(rd_kafka_cgrp_t *rkcg) {
        return rd_atomic32_get(&rkcg->rkcg_assignment_lost) != 0;
}


/**
 * @brief Call when the current assignment has been lost, with a
 *        human-readable reason.
 */
static void rd_kafka_cgrp_assignment_set_lost(rd_kafka_cgrp_t *rkcg,
                                              char *fmt,
                                              ...) RD_FORMAT(printf, 2, 3);
static void
rd_kafka_cgrp_assignment_set_lost(rd_kafka_cgrp_t *rkcg, char *fmt, ...) {
        va_list ap;
        char reason[256];

        if (!rkcg->rkcg_group_assignment)
                return;

        va_start(ap, fmt);
        rd_vsnprintf(reason, sizeof(reason), fmt, ap);
        va_end(ap);

        rd_kafka_dbg(rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP, "LOST",
                     "Group \"%s\": "
                     "current assignment of %d partition(s) lost: %s",
                     rkcg->rkcg_group_id->str, rkcg->rkcg_group_assignment->cnt,
                     reason);

        rd_atomic32_set(&rkcg->rkcg_assignment_lost, rd_true);
}


/**
 * @brief Call when the current assignment is no longer considered lost, with a
 *        human-readable reason.
 */
static void
rd_kafka_cgrp_assignment_clear_lost(rd_kafka_cgrp_t *rkcg, char *fmt, ...) {
        va_list ap;
        char reason[256];

        if (!rd_atomic32_get(&rkcg->rkcg_assignment_lost))
                return;

        va_start(ap, fmt);
        rd_vsnprintf(reason, sizeof(reason), fmt, ap);
        va_end(ap);

        rd_kafka_dbg(rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP, "LOST",
                     "Group \"%s\": "
                     "current assignment no longer considered lost: %s",
                     rkcg->rkcg_group_id->str, reason);

        rd_atomic32_set(&rkcg->rkcg_assignment_lost, rd_false);
}


/**
 * @brief The rebalance protocol currently in use. This will be
 *        RD_KAFKA_REBALANCE_PROTOCOL_NONE if the consumer has not
 *        (yet) joined a group, else it will match the rebalance
 *        protocol of the configured assignor(s).
 *
 * @locality main thread
 */
rd_kafka_rebalance_protocol_t
rd_kafka_cgrp_rebalance_protocol(rd_kafka_cgrp_t *rkcg) {
        if (rkcg->rkcg_group_protocol == RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                if (!(rkcg->rkcg_consumer_flags &
                      RD_KAFKA_CGRP_CONSUMER_F_SUBSCRIBED_ONCE))
                        return RD_KAFKA_REBALANCE_PROTOCOL_NONE;

                return RD_KAFKA_REBALANCE_PROTOCOL_COOPERATIVE;
        }

        if (!rkcg->rkcg_assignor)
                return RD_KAFKA_REBALANCE_PROTOCOL_NONE;
        return rkcg->rkcg_assignor->rkas_protocol;
}



/**
 * @returns true if the cgrp is awaiting a protocol response. This prohibits
 *          the join-state machine to proceed before the current state
 *          is done.
 */
static rd_bool_t rd_kafka_cgrp_awaiting_response(rd_kafka_cgrp_t *rkcg) {
        return rkcg->rkcg_wait_resp != -1;
}


/**
 * @brief Set flag indicating we are waiting for a coordinator response
 *        for the given request.
 *
 * This is used for specific requests to postpone rejoining the group if
 * there are outstanding JoinGroup or SyncGroup requests.
 *
 * @locality main thread
 */
static void rd_kafka_cgrp_set_wait_resp(rd_kafka_cgrp_t *rkcg, int16_t ApiKey) {
        rd_assert(rkcg->rkcg_wait_resp == -1);
        rkcg->rkcg_wait_resp = ApiKey;
}

/**
 * @brief Clear the flag that says we're waiting for a coordinator response
 *        for the given \p request.
 *
 * @param request Original request, possibly NULL (for errors).
 *
 * @locality main thread
 */
static void rd_kafka_cgrp_clear_wait_resp(rd_kafka_cgrp_t *rkcg,
                                          int16_t ApiKey) {
        rd_assert(rkcg->rkcg_wait_resp == ApiKey);
        rkcg->rkcg_wait_resp = -1;
}

/**
 * @brief No-op, just serves for awaking the main loop when needed.
 *        TODO: complete the refactor and serve directly from here.
 */
static void rd_kafka_cgrp_serve_timer_cb(rd_kafka_timers_t *rkts, void *arg) {
}

/**
 * @struct Auxillary glue type used for COOPERATIVE rebalance set operations.
 */
typedef struct PartitionMemberInfo_s {
        const rd_kafka_group_member_t *member;
        rd_bool_t members_match;
} PartitionMemberInfo_t;

static PartitionMemberInfo_t *
PartitionMemberInfo_new(const rd_kafka_group_member_t *member,
                        rd_bool_t members_match) {
        PartitionMemberInfo_t *pmi;

        pmi                = rd_calloc(1, sizeof(*pmi));
        pmi->member        = member;
        pmi->members_match = members_match;

        return pmi;
}

static void PartitionMemberInfo_free(void *p) {
        PartitionMemberInfo_t *pmi = p;
        rd_free(pmi);
}

typedef RD_MAP_TYPE(const rd_kafka_topic_partition_t *,
                    PartitionMemberInfo_t *) map_toppar_member_info_t;


/**
 * @returns true if consumer has joined the group and thus requires a leave.
 *
 * `rkcg_member_id` is sufficient to know this with "classic" group protocol.
 */
#define RD_KAFKA_CGRP_HAS_JOINED_CLASSIC(rkcg)                                 \
        (rkcg->rkcg_group_protocol == RD_KAFKA_GROUP_PROTOCOL_CLASSIC &&       \
         rkcg->rkcg_member_id != NULL &&                                       \
         RD_KAFKAP_STR_LEN((rkcg)->rkcg_member_id) > 0)

/**
 * @returns true if consumer has joined the group and thus requires a leave.
 *
 * With "consumer" group protocol we cannot rely on the `rkcg_member_id`
 * as it's client generated.
 */
#define RD_KAFKA_CGRP_HAS_JOINED_CONSUMER(rkcg)                                \
        (rkcg->rkcg_group_protocol == RD_KAFKA_GROUP_PROTOCOL_CONSUMER &&      \
         rkcg->rkcg_generation_id > 0)

#define RD_KAFKA_CGRP_HAS_JOINED(rkcg)                                         \
        (RD_KAFKA_CGRP_HAS_JOINED_CLASSIC(rkcg) ||                             \
         RD_KAFKA_CGRP_HAS_JOINED_CONSUMER(rkcg))


/**
 * @returns true if cgrp is waiting for a rebalance_cb to be handled by
 *          the application.
 */
#define RD_KAFKA_CGRP_WAIT_ASSIGN_CALL(rkcg)                                   \
        ((rkcg)->rkcg_join_state ==                                            \
             RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_CALL ||                      \
         (rkcg)->rkcg_join_state ==                                            \
             RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_CALL)

/**
 * @returns true if a rebalance is in progress.
 *
 * 1. In WAIT_JOIN or WAIT_METADATA state with a member-id set,
 *    this happens on rejoin.
 * 2. In WAIT_SYNC waiting for the group to rebalance on the broker.
 * 3. in *_WAIT_UNASSIGN_TO_COMPLETE waiting for unassigned partitions to
 *    stop fetching, et.al.
 * 4. In _WAIT_*ASSIGN_CALL waiting for the application to handle the
 *    assignment changes in its rebalance callback and then call *assign().
 * 5. An incremental rebalancing is in progress.
 * 6. A rebalance-induced rejoin is in progress.
 */
#define RD_KAFKA_CGRP_REBALANCING(rkcg)                                        \
        ((RD_KAFKA_CGRP_HAS_JOINED(rkcg) &&                                    \
          ((rkcg)->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_WAIT_JOIN ||    \
           (rkcg)->rkcg_join_state ==                                          \
               RD_KAFKA_CGRP_JOIN_STATE_WAIT_METADATA)) ||                     \
         (rkcg)->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_WAIT_SYNC ||      \
         (rkcg)->rkcg_join_state ==                                            \
             RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_TO_COMPLETE ||             \
         (rkcg)->rkcg_join_state ==                                            \
             RD_KAFKA_CGRP_JOIN_STATE_WAIT_INCR_UNASSIGN_TO_COMPLETE ||        \
         (rkcg)->rkcg_join_state ==                                            \
             RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_CALL ||                      \
         (rkcg)->rkcg_join_state ==                                            \
             RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_CALL ||                    \
         (rkcg)->rkcg_rebalance_incr_assignment != NULL ||                     \
         (rkcg)->rkcg_rebalance_rejoin)



const char *rd_kafka_cgrp_state_names[] = {
    "init",       "term",        "query-coord",
    "wait-coord", "wait-broker", "wait-broker-transport",
    "up"};

const char *rd_kafka_cgrp_join_state_names[] = {
    "init",
    "wait-join",
    "wait-metadata",
    "wait-sync",
    "wait-assign-call",
    "wait-unassign-call",
    "wait-unassign-to-complete",
    "wait-incr-unassign-to-complete",
    "steady",
};


/**
 * @brief Change the cgrp state.
 *
 * @returns 1 if the state was changed, else 0.
 */
static int rd_kafka_cgrp_set_state(rd_kafka_cgrp_t *rkcg, int state) {
        if ((int)rkcg->rkcg_state == state)
                return 0;

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPSTATE",
                     "Group \"%.*s\" changed state %s -> %s "
                     "(join-state %s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                     rd_kafka_cgrp_state_names[state],
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);
        rkcg->rkcg_state          = state;
        rkcg->rkcg_ts_statechange = rd_clock();

        rd_kafka_brokers_broadcast_state_change(rkcg->rkcg_rk);

        return 1;
}

/**
 * @brief Set the cgrp last error and current timestamp
 *        as last error timestamp.
 */
static void rd_kafka_cgrp_set_last_err(rd_kafka_cgrp_t *rkcg,
                                       rd_kafka_resp_err_t rkcg_last_err) {
        rkcg->rkcg_last_err    = rkcg_last_err;
        rkcg->rkcg_ts_last_err = rd_clock();
}

/**
 * @brief Clears cgrp last error and its timestamp.
 */
static void rd_kafka_cgrp_clear_last_err(rd_kafka_cgrp_t *rkcg) {
        rkcg->rkcg_last_err    = RD_KAFKA_RESP_ERR_NO_ERROR;
        rkcg->rkcg_ts_last_err = 0;
}

/**
 * @brief Clears cgrp last error if it's an heartbeat related error like
 *        a topic authorization failed one.
 */
static void
rd_kafka_cgrp_maybe_clear_heartbeat_failed_err(rd_kafka_cgrp_t *rkcg) {
        if (rkcg->rkcg_last_err ==
            RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED) {
                rd_kafka_cgrp_clear_last_err(rkcg);
        }
}


void rd_kafka_cgrp_set_join_state(rd_kafka_cgrp_t *rkcg, int join_state) {
        if ((int)rkcg->rkcg_join_state == join_state)
                return;

        if (rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_INIT ||
            rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_STEADY) {
                /* Start timer when leaving the INIT or STEADY state */
                rkcg->rkcg_ts_rebalance_start = rd_clock();
        } else if (join_state == RD_KAFKA_CGRP_JOIN_STATE_STEADY) {
                /* End timer when reaching the STEADY state */
                rd_dassert(rkcg->rkcg_ts_rebalance_start);
                rd_avg_add(&rkcg->rkcg_rk->rk_telemetry.rd_avg_current
                                .rk_avg_rebalance_latency,
                           rd_clock() - rkcg->rkcg_ts_rebalance_start);
        }

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPJOINSTATE",
                     "Group \"%.*s\" changed join state %s -> %s "
                     "(state %s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
                     rd_kafka_cgrp_join_state_names[join_state],
                     rd_kafka_cgrp_state_names[rkcg->rkcg_state]);
        rkcg->rkcg_join_state = join_state;
}


void rd_kafka_cgrp_destroy_final(rd_kafka_cgrp_t *rkcg) {
        rd_kafka_assert(rkcg->rkcg_rk, !rkcg->rkcg_subscription);
        rd_kafka_assert(rkcg->rkcg_rk, !rkcg->rkcg_group_leader.members);
        rd_kafka_assert(rkcg->rkcg_rk, !rkcg->rkcg_subscription_topics);
        rd_kafka_assert(rkcg->rkcg_rk, !rkcg->rkcg_subscription_regex);
        rd_kafka_cgrp_set_member_id(rkcg, NULL);
        rd_kafka_topic_partition_list_destroy(rkcg->rkcg_current_assignment);
        RD_IF_FREE(rkcg->rkcg_target_assignment,
                   rd_kafka_topic_partition_list_destroy);
        RD_IF_FREE(rkcg->rkcg_next_target_assignment,
                   rd_kafka_topic_partition_list_destroy);
        if (rkcg->rkcg_group_instance_id)
                rd_kafkap_str_destroy(rkcg->rkcg_group_instance_id);
        if (rkcg->rkcg_group_remote_assignor)
                rd_kafkap_str_destroy(rkcg->rkcg_group_remote_assignor);
        if (rkcg->rkcg_client_rack)
                rd_kafkap_str_destroy(rkcg->rkcg_client_rack);
        rd_kafka_q_destroy_owner(rkcg->rkcg_q);
        rd_kafka_q_destroy_owner(rkcg->rkcg_ops);
        rd_kafka_q_destroy_owner(rkcg->rkcg_wait_coord_q);
        rd_kafka_assert(rkcg->rkcg_rk, TAILQ_EMPTY(&rkcg->rkcg_topics));
        rd_kafka_assert(rkcg->rkcg_rk, rd_list_empty(&rkcg->rkcg_toppars));
        rd_list_destroy(&rkcg->rkcg_toppars);
        rd_list_destroy(rkcg->rkcg_subscribed_topics);
        rd_kafka_topic_partition_list_destroy(rkcg->rkcg_errored_topics);
        if (rkcg->rkcg_assignor && rkcg->rkcg_assignor->rkas_destroy_state_cb &&
            rkcg->rkcg_assignor_state)
                rkcg->rkcg_assignor->rkas_destroy_state_cb(
                    rkcg->rkcg_assignor_state);
        rd_free(rkcg);
}



/**
 * @brief Update the absolute session timeout following a successfull
 *        response from the coordinator.
 *        This timeout is used to enforce the session timeout in the
 *        consumer itself.
 *
 * @param reset if true the timeout is updated even if the session has expired.
 */
static RD_INLINE void
rd_kafka_cgrp_update_session_timeout(rd_kafka_cgrp_t *rkcg, rd_bool_t reset) {
        if (reset || rkcg->rkcg_ts_session_timeout != 0)
                rkcg->rkcg_ts_session_timeout =
                    rd_clock() +
                    (rkcg->rkcg_rk->rk_conf.group_session_timeout_ms * 1000);
}



rd_kafka_cgrp_t *rd_kafka_cgrp_new(rd_kafka_t *rk,
                                   rd_kafka_group_protocol_t group_protocol,
                                   const rd_kafkap_str_t *group_id,
                                   const rd_kafkap_str_t *client_id) {
        rd_kafka_cgrp_t *rkcg;
        rkcg = rd_calloc(1, sizeof(*rkcg));

        rkcg->rkcg_rk             = rk;
        rkcg->rkcg_group_protocol = group_protocol;
        rkcg->rkcg_group_id       = group_id;
        rkcg->rkcg_client_id      = client_id;
        rkcg->rkcg_coord_id       = -1;
        rkcg->rkcg_generation_id  = -1;
        rkcg->rkcg_wait_resp      = -1;

        rkcg->rkcg_ops                      = rd_kafka_q_new(rk);
        rkcg->rkcg_ops->rkq_serve           = rd_kafka_cgrp_op_serve;
        rkcg->rkcg_ops->rkq_opaque          = rkcg;
        rkcg->rkcg_wait_coord_q             = rd_kafka_q_new(rk);
        rkcg->rkcg_wait_coord_q->rkq_serve  = rkcg->rkcg_ops->rkq_serve;
        rkcg->rkcg_wait_coord_q->rkq_opaque = rkcg->rkcg_ops->rkq_opaque;
        rkcg->rkcg_q                        = rd_kafka_consume_q_new(rk);
        rkcg->rkcg_group_instance_id =
            rd_kafkap_str_new(rk->rk_conf.group_instance_id, -1);
        rkcg->rkcg_group_remote_assignor =
            rd_kafkap_str_new(rk->rk_conf.group_remote_assignor, -1);

        if (!RD_KAFKAP_STR_LEN(rkcg->rkcg_rk->rk_conf.client_rack))
                rkcg->rkcg_client_rack = rd_kafkap_str_new(NULL, -1);
        else
                rkcg->rkcg_client_rack =
                    rd_kafkap_str_copy(rkcg->rkcg_rk->rk_conf.client_rack);
        rkcg->rkcg_next_subscription = NULL;
        TAILQ_INIT(&rkcg->rkcg_topics);
        rd_list_init(&rkcg->rkcg_toppars, 32, NULL);

        if (rkcg->rkcg_group_protocol == RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                rd_kafka_Uuid_t uuid = rd_kafka_Uuid_random();
                rd_kafka_cgrp_set_member_id(rkcg,
                                            rd_kafka_Uuid_base64str(&uuid));
        } else {
                rd_kafka_cgrp_set_member_id(rkcg, "");
        }

        rkcg->rkcg_subscribed_topics =
            rd_list_new(0, rd_kafka_topic_info_destroy_free);
        rd_interval_init(&rkcg->rkcg_coord_query_intvl);
        rd_interval_init(&rkcg->rkcg_heartbeat_intvl);
        rd_interval_init(&rkcg->rkcg_join_intvl);
        rd_interval_init(&rkcg->rkcg_timeout_scan_intvl);
        rd_atomic32_init(&rkcg->rkcg_assignment_lost, rd_false);
        rd_atomic32_init(&rkcg->rkcg_terminated, rd_false);
        rd_atomic32_init(&rkcg->rkcg_subscription_version, 0);
        rkcg->rkcg_current_assignment = rd_kafka_topic_partition_list_new(0);
        rkcg->rkcg_target_assignment  = NULL;
        rkcg->rkcg_next_target_assignment = NULL;

        rkcg->rkcg_errored_topics = rd_kafka_topic_partition_list_new(0);

        /* Create a logical group coordinator broker to provide
         * a dedicated connection for group coordination.
         * This is needed since JoinGroup may block for up to
         * max.poll.interval.ms, effectively blocking and timing out
         * any other protocol requests (such as Metadata).
         * The address for this broker will be updated when
         * the group coordinator is assigned. */
        rkcg->rkcg_coord = rd_kafka_broker_add_logical(rk, "GroupCoordinator");

        if (rk->rk_conf.enable_auto_commit &&
            rk->rk_conf.auto_commit_interval_ms > 0)
                rd_kafka_timer_start(
                    &rk->rk_timers, &rkcg->rkcg_offset_commit_tmr,
                    rk->rk_conf.auto_commit_interval_ms * 1000ll,
                    rd_kafka_cgrp_offset_commit_tmr_cb, rkcg);

        if (rkcg->rkcg_group_protocol == RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                rd_kafka_log(rk, LOG_WARNING, "CGRP",
                             "KIP-848 Consumer Group Protocol is in 'Preview' "
                             "and MUST NOT be used in production");
        }

        return rkcg;
}


/**
 * @brief Set the group coordinator broker.
 */
static void rd_kafka_cgrp_coord_set_broker(rd_kafka_cgrp_t *rkcg,
                                           rd_kafka_broker_t *rkb) {

        rd_assert(rkcg->rkcg_curr_coord == NULL);

        rd_assert(RD_KAFKA_CGRP_BROKER_IS_COORD(rkcg, rkb));

        rkcg->rkcg_curr_coord = rkb;
        rd_kafka_broker_keep(rkb);

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "COORDSET",
                     "Group \"%.*s\" coordinator set to broker %s",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_kafka_broker_name(rkb));

        /* Reset query interval to trigger an immediate
         * coord query if required */
        if (!rd_interval_disabled(&rkcg->rkcg_coord_query_intvl))
                rd_interval_reset(&rkcg->rkcg_coord_query_intvl);

        rd_kafka_cgrp_set_state(rkcg,
                                RD_KAFKA_CGRP_STATE_WAIT_BROKER_TRANSPORT);

        rd_kafka_broker_persistent_connection_add(
            rkcg->rkcg_coord, &rkcg->rkcg_coord->rkb_persistconn.coord);

        /* Set the logical coordinator's nodename to the
         * proper broker's nodename, this will trigger a (re)connect
         * to the new address. */
        rd_kafka_broker_set_nodename(rkcg->rkcg_coord, rkb);
}


/**
 * @brief Reset/clear the group coordinator broker.
 */
static void rd_kafka_cgrp_coord_clear_broker(rd_kafka_cgrp_t *rkcg) {
        rd_kafka_broker_t *rkb = rkcg->rkcg_curr_coord;

        rd_assert(rkcg->rkcg_curr_coord);
        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "COORDCLEAR",
                     "Group \"%.*s\" broker %s is no longer coordinator",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_kafka_broker_name(rkb));

        rd_assert(rkcg->rkcg_coord);

        rd_kafka_broker_persistent_connection_del(
            rkcg->rkcg_coord, &rkcg->rkcg_coord->rkb_persistconn.coord);

        /* Clear the ephemeral broker's nodename.
         * This will also trigger a disconnect. */
        rd_kafka_broker_set_nodename(rkcg->rkcg_coord, NULL);

        rkcg->rkcg_curr_coord = NULL;
        rd_kafka_broker_destroy(rkb); /* from set_coord_broker() */
}


/**
 * @brief Update/set the group coordinator.
 *
 * Will do nothing if there's been no change.
 *
 * @returns 1 if the coordinator, or state, was updated, else 0.
 */
static int rd_kafka_cgrp_coord_update(rd_kafka_cgrp_t *rkcg, int32_t coord_id) {

        /* Don't do anything while terminating */
        if (rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_TERM)
                return 0;

        /* Check if coordinator changed */
        if (rkcg->rkcg_coord_id != coord_id) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPCOORD",
                             "Group \"%.*s\" changing coordinator %" PRId32
                             " -> %" PRId32,
                             RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                             rkcg->rkcg_coord_id, coord_id);

                /* Update coord id */
                rkcg->rkcg_coord_id = coord_id;

                /* Clear previous broker handle, if any */
                if (rkcg->rkcg_curr_coord)
                        rd_kafka_cgrp_coord_clear_broker(rkcg);

                rd_kafka_cgrp_consumer_expedite_next_heartbeat(
                    rkcg, "coordinator changed");
        }


        if (rkcg->rkcg_curr_coord) {
                /* There is already a known coordinator and a
                 * corresponding broker handle. */
                if (rkcg->rkcg_state != RD_KAFKA_CGRP_STATE_UP)
                        return rd_kafka_cgrp_set_state(
                            rkcg, RD_KAFKA_CGRP_STATE_WAIT_BROKER_TRANSPORT);

        } else if (rkcg->rkcg_coord_id != -1) {
                rd_kafka_broker_t *rkb;

                /* Try to find the coordinator broker handle */
                rd_kafka_rdlock(rkcg->rkcg_rk);
                rkb = rd_kafka_broker_find_by_nodeid(rkcg->rkcg_rk, coord_id);
                rd_kafka_rdunlock(rkcg->rkcg_rk);

                /* It is possible, due to stale metadata, that the
                 * coordinator id points to a broker we still don't know
                 * about. In this case the client will continue
                 * querying metadata and querying for the coordinator
                 * until a match is found. */

                if (rkb) {
                        /* Coordinator is known and broker handle exists */
                        rd_kafka_cgrp_coord_set_broker(rkcg, rkb);
                        rd_kafka_broker_destroy(rkb); /*from find_by_nodeid()*/

                        return 1;
                } else {
                        /* Coordinator is known but no corresponding
                         * broker handle. */
                        return rd_kafka_cgrp_set_state(
                            rkcg, RD_KAFKA_CGRP_STATE_WAIT_BROKER);
                }

        } else {
                /* Coordinator still not known, re-query */
                if (rkcg->rkcg_state >= RD_KAFKA_CGRP_STATE_WAIT_COORD)
                        return rd_kafka_cgrp_set_state(
                            rkcg, RD_KAFKA_CGRP_STATE_QUERY_COORD);
        }

        return 0; /* no change */
}



/**
 * Handle FindCoordinator response
 */
static void rd_kafka_cgrp_handle_FindCoordinator(rd_kafka_t *rk,
                                                 rd_kafka_broker_t *rkb,
                                                 rd_kafka_resp_err_t err,
                                                 rd_kafka_buf_t *rkbuf,
                                                 rd_kafka_buf_t *request,
                                                 void *opaque) {
        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode           = 0;
        int32_t CoordId;
        rd_kafkap_str_t CoordHost = RD_ZERO_INIT;
        int32_t CoordPort;
        rd_kafka_cgrp_t *rkcg               = opaque;
        struct rd_kafka_metadata_broker mdb = RD_ZERO_INIT;
        char *errstr                        = NULL;
        int actions;

        if (likely(!(ErrorCode = err))) {
                if (rkbuf->rkbuf_reqhdr.ApiVersion >= 1)
                        rd_kafka_buf_read_throttle_time(rkbuf);

                rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

                if (rkbuf->rkbuf_reqhdr.ApiVersion >= 1) {
                        rd_kafkap_str_t ErrorMsg;

                        rd_kafka_buf_read_str(rkbuf, &ErrorMsg);

                        if (!RD_KAFKAP_STR_IS_NULL(&ErrorMsg))
                                RD_KAFKAP_STR_DUPA(&errstr, &ErrorMsg);
                }

                rd_kafka_buf_read_i32(rkbuf, &CoordId);
                rd_kafka_buf_read_str(rkbuf, &CoordHost);
                rd_kafka_buf_read_i32(rkbuf, &CoordPort);
        }

        if (ErrorCode)
                goto err;


        mdb.id = CoordId;
        RD_KAFKAP_STR_DUPA(&mdb.host, &CoordHost);
        mdb.port = CoordPort;

        rd_rkb_dbg(rkb, CGRP, "CGRPCOORD",
                   "Group \"%.*s\" coordinator is %s:%i id %" PRId32,
                   RD_KAFKAP_STR_PR(rkcg->rkcg_group_id), mdb.host, mdb.port,
                   mdb.id);
        rd_kafka_broker_update(rkb->rkb_rk, rkb->rkb_proto, &mdb, NULL);

        rd_kafka_cgrp_coord_update(rkcg, CoordId);
        rd_kafka_cgrp_serve(rkcg); /* Serve updated state, if possible */
        return;

err_parse: /* Parse error */
        ErrorCode = rkbuf->rkbuf_err;
        /* FALLTHRU */

err:
        if (!errstr)
                errstr = (char *)rd_kafka_err2str(ErrorCode);

        rd_rkb_dbg(rkb, CGRP, "CGRPCOORD",
                   "Group \"%.*s\" FindCoordinator response error: %s: %s",
                   RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                   rd_kafka_err2name(ErrorCode), errstr);

        if (ErrorCode == RD_KAFKA_RESP_ERR__DESTROY)
                return;

        actions = rd_kafka_err_action(
            rkb, ErrorCode, request,

            RD_KAFKA_ERR_ACTION_RETRY | RD_KAFKA_ERR_ACTION_REFRESH,
            RD_KAFKA_RESP_ERR_GROUP_COORDINATOR_NOT_AVAILABLE,

            RD_KAFKA_ERR_ACTION_RETRY, RD_KAFKA_RESP_ERR__TRANSPORT,

            RD_KAFKA_ERR_ACTION_RETRY, RD_KAFKA_RESP_ERR__TIMED_OUT,

            RD_KAFKA_ERR_ACTION_RETRY, RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE,

            RD_KAFKA_ERR_ACTION_RETRY, RD_KAFKA_RESP_ERR__DESTROY_BROKER,

            RD_KAFKA_ERR_ACTION_END);



        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                rd_kafka_cgrp_coord_update(rkcg, -1);
        } else {
                if (!(actions & RD_KAFKA_ERR_ACTION_RETRY) &&
                    rkcg->rkcg_last_err != ErrorCode) {
                        /* Propagate non-retriable errors to the application */
                        rd_kafka_consumer_err(
                            rkcg->rkcg_q, rd_kafka_broker_id(rkb), ErrorCode, 0,
                            NULL, NULL, RD_KAFKA_OFFSET_INVALID,
                            "FindCoordinator response error: %s", errstr);

                        /* Suppress repeated errors */
                        rd_kafka_cgrp_set_last_err(rkcg, ErrorCode);
                }

                if (ErrorCode == RD_KAFKA_RESP_ERR__DESTROY_BROKER) {
                        /* This error is one-time and should cause
                         * an immediate retry. */
                        rd_interval_reset(&rkcg->rkcg_coord_query_intvl);
                }

                /* Retries are performed by the timer-intervalled
                 * coord queries, continue querying */
                rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_QUERY_COORD);
        }

        rd_kafka_cgrp_serve(rkcg); /* Serve updated state, if possible */
}


/**
 * Query for coordinator.
 * Ask any broker in state UP
 *
 * Locality: main thread
 */
void rd_kafka_cgrp_coord_query(rd_kafka_cgrp_t *rkcg, const char *reason) {
        rd_kafka_broker_t *rkb;
        rd_kafka_resp_err_t err;

        rkb = rd_kafka_broker_any_usable(
            rkcg->rkcg_rk, RD_POLL_NOWAIT, RD_DO_LOCK,
            RD_KAFKA_FEATURE_BROKER_GROUP_COORD, "coordinator query");

        if (!rkb) {
                /* Reset the interval because there were no brokers. When a
                 * broker becomes available, we want to query it immediately. */
                rd_interval_reset(&rkcg->rkcg_coord_query_intvl);
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPQUERY",
                             "Group \"%.*s\": "
                             "no broker available for coordinator query: %s",
                             RD_KAFKAP_STR_PR(rkcg->rkcg_group_id), reason);
                return;
        }

        rd_rkb_dbg(rkb, CGRP, "CGRPQUERY",
                   "Group \"%.*s\": querying for coordinator: %s",
                   RD_KAFKAP_STR_PR(rkcg->rkcg_group_id), reason);

        err = rd_kafka_FindCoordinatorRequest(
            rkb, RD_KAFKA_COORD_GROUP, rkcg->rkcg_group_id->str,
            RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
            rd_kafka_cgrp_handle_FindCoordinator, rkcg);

        if (err) {
                rd_rkb_dbg(rkb, CGRP, "CGRPQUERY",
                           "Group \"%.*s\": "
                           "unable to send coordinator query: %s",
                           RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                           rd_kafka_err2str(err));
                rd_kafka_broker_destroy(rkb);
                return;
        }

        if (rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_QUERY_COORD)
                rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_WAIT_COORD);

        rd_kafka_broker_destroy(rkb);

        /* Back off the next intervalled query with a jitter since we just sent
         * one. */
        rd_interval_reset_to_now_with_jitter(&rkcg->rkcg_coord_query_intvl, 0,
                                             500,
                                             RD_KAFKA_RETRY_JITTER_PERCENT);
}

/**
 * @brief Mark the current coordinator as dead.
 *
 * @locality main thread
 */
void rd_kafka_cgrp_coord_dead(rd_kafka_cgrp_t *rkcg,
                              rd_kafka_resp_err_t err,
                              const char *reason) {
        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "COORD",
                     "Group \"%.*s\": "
                     "marking the coordinator (%" PRId32 ") dead: %s: %s",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id), rkcg->rkcg_coord_id,
                     rd_kafka_err2str(err), reason);

        rd_kafka_cgrp_coord_update(rkcg, -1);

        /* Re-query for coordinator */
        rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_QUERY_COORD);
        rd_kafka_cgrp_coord_query(rkcg, reason);
}


/**
 * @returns a new reference to the current coordinator, if available, else NULL.
 *
 * @locality rdkafka main thread
 * @locks_required none
 * @locks_acquired none
 */
rd_kafka_broker_t *rd_kafka_cgrp_get_coord(rd_kafka_cgrp_t *rkcg) {
        if (rkcg->rkcg_state != RD_KAFKA_CGRP_STATE_UP || !rkcg->rkcg_coord)
                return NULL;

        rd_kafka_broker_keep(rkcg->rkcg_coord);

        return rkcg->rkcg_coord;
}

#define rd_kafka_cgrp_will_leave(rkcg)                                         \
        (rkcg->rkcg_flags & (RD_KAFKA_CGRP_F_LEAVE_ON_UNASSIGN_DONE |          \
                             RD_KAFKA_CGRP_F_WAIT_LEAVE))

#define rd_kafka_cgrp_consumer_will_rejoin(rkcg)                               \
        (rkcg->rkcg_consumer_flags &                                           \
         (RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN |                               \
          RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN_TO_COMPLETE))

#define rd_kafka_cgrp_consumer_subscription_preconditions_met(rkcg)            \
        (!RD_KAFKA_CGRP_REBALANCING(rkcg) &&                                   \
         rkcg->rkcg_consumer_flags &                                           \
             RD_KAFKA_CGRP_CONSUMER_F_SEND_NEW_SUBSCRIPTION)

static int32_t
rd_kafka_cgrp_subscription_set(rd_kafka_cgrp_t *rkcg,
                               rd_kafka_topic_partition_list_t *rktparlist);

/**
 * @brief Apply next subscription in \p rkcg , if set.
 */
static void rd_kafka_cgrp_consumer_apply_next_subscribe(rd_kafka_cgrp_t *rkcg) {
        if (rkcg->rkcg_next_subscription) {
                if (unlikely(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE)) {
                        rd_kafka_topic_partition_list_destroy(
                            rkcg->rkcg_next_subscription);
                        rkcg->rkcg_next_subscription = NULL;
                        return;
                }

                rd_kafka_cgrp_subscription_set(rkcg,
                                               rkcg->rkcg_next_subscription);
                rkcg->rkcg_next_subscription = NULL;

                rd_kafka_cgrp_consumer_expedite_next_heartbeat(
                    rkcg, "subscription changed");
        }
}

/**
 * @brief cgrp handling of LeaveGroup responses
 * @param opaque must be the cgrp handle.
 * @locality rdkafka main thread (unless err==ERR__DESTROY)
 */
static void rd_kafka_cgrp_handle_LeaveGroup(rd_kafka_t *rk,
                                            rd_kafka_broker_t *rkb,
                                            rd_kafka_resp_err_t err,
                                            rd_kafka_buf_t *rkbuf,
                                            rd_kafka_buf_t *request,
                                            void *opaque) {
        rd_kafka_cgrp_t *rkcg       = opaque;
        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode           = 0;

        if (err) {
                ErrorCode = err;
                goto err;
        }

        if (request->rkbuf_reqhdr.ApiVersion >= 1)
                rd_kafka_buf_read_throttle_time(rkbuf);

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

err:
        if (ErrorCode)
                rd_kafka_dbg(rkb->rkb_rk, CGRP, "LEAVEGROUP",
                             "LeaveGroup response error in state %s: %s",
                             rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                             rd_kafka_err2str(ErrorCode));
        else
                rd_kafka_dbg(rkb->rkb_rk, CGRP, "LEAVEGROUP",
                             "LeaveGroup response received in state %s",
                             rd_kafka_cgrp_state_names[rkcg->rkcg_state]);

        if (ErrorCode != RD_KAFKA_RESP_ERR__DESTROY) {
                rd_assert(thrd_is_current(rk->rk_thread));
                rkcg->rkcg_flags &= ~RD_KAFKA_CGRP_F_WAIT_LEAVE;
                rd_kafka_cgrp_try_terminate(rkcg);
        }



        return;

err_parse:
        ErrorCode = rkbuf->rkbuf_err;
        goto err;
}

static void rd_kafka_cgrp_consumer_reset(rd_kafka_cgrp_t *rkcg) {
        if (rkcg->rkcg_group_protocol != RD_KAFKA_GROUP_PROTOCOL_CONSUMER)
                return;

        rkcg->rkcg_generation_id = 0;
        rd_kafka_topic_partition_list_destroy(rkcg->rkcg_current_assignment);
        RD_IF_FREE(rkcg->rkcg_target_assignment,
                   rd_kafka_topic_partition_list_destroy);
        rkcg->rkcg_target_assignment = NULL;
        RD_IF_FREE(rkcg->rkcg_next_target_assignment,
                   rd_kafka_topic_partition_list_destroy);
        rkcg->rkcg_next_target_assignment = NULL;
        rkcg->rkcg_current_assignment = rd_kafka_topic_partition_list_new(0);

        /* Leave only specified flags, reset the rest */
        rkcg->rkcg_consumer_flags =
            (rkcg->rkcg_consumer_flags &
             RD_KAFKA_CGRP_CONSUMER_F_SUBSCRIBED_ONCE) |
            (rkcg->rkcg_consumer_flags &
             RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN_TO_COMPLETE);
}

/**
 * @brief cgrp handling of ConsumerGroupHeartbeat response after leaving group
 * @param opaque must be the cgrp handle.
 * @locality rdkafka main thread (unless err==ERR__DESTROY)
 */
static void
rd_kafka_cgrp_handle_ConsumerGroupHeartbeat_leave(rd_kafka_t *rk,
                                                  rd_kafka_broker_t *rkb,
                                                  rd_kafka_resp_err_t err,
                                                  rd_kafka_buf_t *rkbuf,
                                                  rd_kafka_buf_t *request,
                                                  void *opaque) {
        rd_kafka_cgrp_t *rkcg       = opaque;
        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode           = 0;

        if (err) {
                ErrorCode = err;
                goto err;
        }

        rd_kafka_buf_read_throttle_time(rkbuf);

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

err:
        if (ErrorCode)
                rd_kafka_dbg(
                    rkb->rkb_rk, CGRP, "LEAVEGROUP",
                    "ConsumerGroupHeartbeat response error in state %s: %s",
                    rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                    rd_kafka_err2str(ErrorCode));
        else
                rd_kafka_dbg(
                    rkb->rkb_rk, CGRP, "LEAVEGROUP",
                    "ConsumerGroupHeartbeat response received in state %s",
                    rd_kafka_cgrp_state_names[rkcg->rkcg_state]);

        rd_kafka_cgrp_consumer_reset(rkcg);

        if (ErrorCode != RD_KAFKA_RESP_ERR__DESTROY) {
                rd_assert(thrd_is_current(rk->rk_thread));
                rkcg->rkcg_flags &= ~RD_KAFKA_CGRP_F_WAIT_LEAVE;
                rd_kafka_cgrp_try_terminate(rkcg);
        }

        return;

err_parse:
        ErrorCode = rkbuf->rkbuf_err;
        goto err;
}

static void rd_kafka_cgrp_consumer_leave(rd_kafka_cgrp_t *rkcg) {
        int32_t member_epoch = -1;

        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WAIT_LEAVE) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "LEAVE",
                             "Group \"%.*s\": leave (in state %s): "
                             "ConsumerGroupHeartbeat already in-transit",
                             RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                             rd_kafka_cgrp_state_names[rkcg->rkcg_state]);
                return;
        }

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "LEAVE",
                     "Group \"%.*s\": leave (in state %s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_kafka_cgrp_state_names[rkcg->rkcg_state]);

        rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_WAIT_LEAVE;
        if (RD_KAFKA_CGRP_IS_STATIC_MEMBER(rkcg)) {
                member_epoch = -2;
        }

        if (rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_UP) {
                rd_rkb_dbg(rkcg->rkcg_curr_coord, CONSUMER, "LEAVE",
                           "Leaving group");
                rd_kafka_ConsumerGroupHeartbeatRequest(
                    rkcg->rkcg_coord, rkcg->rkcg_group_id, rkcg->rkcg_member_id,
                    member_epoch, rkcg->rkcg_group_instance_id,
                    NULL /* no rack */, -1 /* no rebalance_timeout_ms */,
                    NULL /* no subscription topics */,
                    NULL /* no regex subscription */,
                    NULL /* no remote assignor */,
                    NULL /* no current assignment */,
                    RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
                    rd_kafka_cgrp_handle_ConsumerGroupHeartbeat_leave, rkcg);
        } else {
                rd_kafka_cgrp_handle_ConsumerGroupHeartbeat_leave(
                    rkcg->rkcg_rk, rkcg->rkcg_coord,
                    RD_KAFKA_RESP_ERR__WAIT_COORD, NULL, NULL, rkcg);
        }
}

static void rd_kafka_cgrp_leave(rd_kafka_cgrp_t *rkcg) {
        char *member_id;

        RD_KAFKAP_STR_DUPA(&member_id, rkcg->rkcg_member_id);

        /* Leaving the group invalidates the member id, reset it
         * now to avoid an ERR_UNKNOWN_MEMBER_ID on the next join. */
        rd_kafka_cgrp_set_member_id(rkcg, "");

        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WAIT_LEAVE) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "LEAVE",
                             "Group \"%.*s\": leave (in state %s): "
                             "LeaveGroupRequest already in-transit",
                             RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                             rd_kafka_cgrp_state_names[rkcg->rkcg_state]);
                return;
        }

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "LEAVE",
                     "Group \"%.*s\": leave (in state %s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_kafka_cgrp_state_names[rkcg->rkcg_state]);

        rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_WAIT_LEAVE;

        if (rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_UP) {
                rd_rkb_dbg(rkcg->rkcg_curr_coord, CONSUMER, "LEAVE",
                           "Leaving group");
                rd_kafka_LeaveGroupRequest(
                    rkcg->rkcg_coord, rkcg->rkcg_group_id->str, member_id,
                    RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
                    rd_kafka_cgrp_handle_LeaveGroup, rkcg);
        } else
                rd_kafka_cgrp_handle_LeaveGroup(rkcg->rkcg_rk, rkcg->rkcg_coord,
                                                RD_KAFKA_RESP_ERR__WAIT_COORD,
                                                NULL, NULL, rkcg);
}


/**
 * @brief Leave group, if desired.
 *
 * @returns true if a LeaveGroup was issued, else false.
 */
static rd_bool_t rd_kafka_cgrp_leave_maybe(rd_kafka_cgrp_t *rkcg) {

        /* We were not instructed to leave in the first place. */
        if (!(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_LEAVE_ON_UNASSIGN_DONE))
                return rd_false;

        rkcg->rkcg_flags &= ~RD_KAFKA_CGRP_F_LEAVE_ON_UNASSIGN_DONE;

        /* Don't send Leave when terminating with NO_CONSUMER_CLOSE flag */
        if (rd_kafka_destroy_flags_no_consumer_close(rkcg->rkcg_rk))
                return rd_false;

        if (rkcg->rkcg_group_protocol == RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                rd_kafka_cgrp_consumer_leave(rkcg);
        } else {
                /* KIP-345: Static group members must not send a
                 * LeaveGroupRequest on termination. */
                if (RD_KAFKA_CGRP_IS_STATIC_MEMBER(rkcg) &&
                    rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE)
                        return rd_false;

                rd_kafka_cgrp_leave(rkcg);
        }

        return rd_true;
}

/**
 * @brief Enqueues a rebalance op, delegating responsibility of calling
 *        incremental_assign / incremental_unassign to the application.
 *        If there is no rebalance handler configured, or the action
 *        should not be delegated to the application for some other
 *        reason, incremental_assign / incremental_unassign will be called
 *        automatically, immediately.
 *
 * @param rejoin whether or not to rejoin the group following completion
 *        of the incremental assign / unassign.
 *
 * @remarks does not take ownership of \p partitions.
 */
void rd_kafka_rebalance_op_incr(rd_kafka_cgrp_t *rkcg,
                                rd_kafka_resp_err_t err,
                                rd_kafka_topic_partition_list_t *partitions,
                                rd_bool_t rejoin,
                                const char *reason) {
        rd_kafka_error_t *error;

        /* Flag to rejoin after completion of the incr_assign or incr_unassign,
           if required. */
        rkcg->rkcg_rebalance_rejoin = rejoin;

        rd_kafka_wrlock(rkcg->rkcg_rk);
        rkcg->rkcg_c.ts_rebalance = rd_clock();
        rkcg->rkcg_c.rebalance_cnt++;
        rd_kafka_wrunlock(rkcg->rkcg_rk);

        if (rd_kafka_destroy_flags_no_consumer_close(rkcg->rkcg_rk) ||
            rd_kafka_fatal_error_code(rkcg->rkcg_rk)) {
                /* Total unconditional unassign in these cases */
                rd_kafka_cgrp_unassign(rkcg);

                /* Now serve the assignment to make updates */
                rd_kafka_assignment_serve(rkcg->rkcg_rk);
                goto done;
        }

        rd_kafka_cgrp_set_join_state(
            rkcg, err == RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS
                      ? RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_CALL
                      : RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_CALL);

        /* Schedule application rebalance callback/event if enabled */
        if (rkcg->rkcg_rk->rk_conf.enabled_events & RD_KAFKA_EVENT_REBALANCE) {
                rd_kafka_op_t *rko;

                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "ASSIGN",
                             "Group \"%s\": delegating incremental %s of %d "
                             "partition(s) to application on queue %s: %s",
                             rkcg->rkcg_group_id->str,
                             err == RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS
                                 ? "revoke"
                                 : "assign",
                             partitions->cnt,
                             rd_kafka_q_dest_name(rkcg->rkcg_q), reason);

                /* Pause currently assigned partitions while waiting for
                 * rebalance callback to get called to make sure the
                 * application will not receive any more messages that
                 * might block it from serving the rebalance callback
                 * and to not process messages for partitions it
                 * might have lost in the rebalance. */
                rd_kafka_assignment_pause(rkcg->rkcg_rk,
                                          "incremental rebalance");

                rko          = rd_kafka_op_new(RD_KAFKA_OP_REBALANCE);
                rko->rko_err = err;
                rko->rko_u.rebalance.partitions =
                    rd_kafka_topic_partition_list_copy(partitions);

                if (rd_kafka_q_enq(rkcg->rkcg_q, rko))
                        goto done; /* Rebalance op successfully enqueued */

                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRP",
                             "Group \"%s\": ops queue is disabled, not "
                             "delegating partition %s to application",
                             rkcg->rkcg_group_id->str,
                             err == RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS
                                 ? "unassign"
                                 : "assign");
                /* FALLTHRU */
        }

        /* No application rebalance callback/event handler, or it is not
         * available, do the assign/unassign ourselves.
         * We need to be careful here not to trigger assignment_serve()
         * since it may call into the cgrp code again, in which case we
         * can't really track what the outcome state will be. */

        if (err == RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS)
                error = rd_kafka_cgrp_incremental_assign(rkcg, partitions);
        else
                error = rd_kafka_cgrp_incremental_unassign(rkcg, partitions);

        if (error) {
                rd_kafka_log(rkcg->rkcg_rk, LOG_ERR, "REBALANCE",
                             "Group \"%s\": internal incremental %s "
                             "of %d partition(s) failed: %s: "
                             "unassigning all partitions and rejoining",
                             rkcg->rkcg_group_id->str,
                             err == RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS
                                 ? "unassign"
                                 : "assign",
                             partitions->cnt, rd_kafka_error_string(error));
                rd_kafka_error_destroy(error);

                rd_kafka_cgrp_set_join_state(rkcg,
                                             /* This is a clean state for
                                              * assignment_done() to rejoin
                                              * from. */
                                             RD_KAFKA_CGRP_JOIN_STATE_STEADY);
                rd_kafka_assignment_clear(rkcg->rkcg_rk);
        }

        /* Now serve the assignment to make updates */
        rd_kafka_assignment_serve(rkcg->rkcg_rk);

done:
        /* Update the current group assignment based on the
         * added/removed partitions. */
        rd_kafka_cgrp_group_assignment_modify(
            rkcg, err == RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS, partitions);
}


/**
 * @brief Enqueues a rebalance op, delegating responsibility of calling
 *        assign / unassign to the application. If there is no rebalance
 *        handler configured, or the action should not be delegated to the
 *        application for some other reason, assign / unassign will be
 *        called automatically.
 *
 * @remarks \p partitions is copied.
 */
static void rd_kafka_rebalance_op(rd_kafka_cgrp_t *rkcg,
                                  rd_kafka_resp_err_t err,
                                  rd_kafka_topic_partition_list_t *assignment,
                                  const char *reason) {
        rd_kafka_error_t *error;

        rd_kafka_wrlock(rkcg->rkcg_rk);
        rkcg->rkcg_c.ts_rebalance = rd_clock();
        rkcg->rkcg_c.rebalance_cnt++;
        rd_kafka_wrunlock(rkcg->rkcg_rk);

        if (rd_kafka_destroy_flags_no_consumer_close(rkcg->rkcg_rk) ||
            rd_kafka_fatal_error_code(rkcg->rkcg_rk)) {
                /* Unassign */
                rd_kafka_cgrp_unassign(rkcg);

                /* Now serve the assignment to make updates */
                rd_kafka_assignment_serve(rkcg->rkcg_rk);
                goto done;
        }

        rd_assert(assignment != NULL);

        rd_kafka_cgrp_set_join_state(
            rkcg, err == RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS
                      ? RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_CALL
                      : RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_CALL);

        /* Schedule application rebalance callback/event if enabled */
        if (rkcg->rkcg_rk->rk_conf.enabled_events & RD_KAFKA_EVENT_REBALANCE) {
                rd_kafka_op_t *rko;

                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "ASSIGN",
                             "Group \"%s\": delegating %s of %d partition(s) "
                             "to application on queue %s: %s",
                             rkcg->rkcg_group_id->str,
                             err == RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS
                                 ? "revoke"
                                 : "assign",
                             assignment->cnt,
                             rd_kafka_q_dest_name(rkcg->rkcg_q), reason);

                /* Pause currently assigned partitions while waiting for
                 * rebalance callback to get called to make sure the
                 * application will not receive any more messages that
                 * might block it from serving the rebalance callback
                 * and to not process messages for partitions it
                 * might have lost in the rebalance. */
                rd_kafka_assignment_pause(rkcg->rkcg_rk, "rebalance");

                rko          = rd_kafka_op_new(RD_KAFKA_OP_REBALANCE);
                rko->rko_err = err;
                rko->rko_u.rebalance.partitions =
                    rd_kafka_topic_partition_list_copy(assignment);

                if (rd_kafka_q_enq(rkcg->rkcg_q, rko))
                        goto done; /* Rebalance op successfully enqueued */

                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRP",
                             "Group \"%s\": ops queue is disabled, not "
                             "delegating partition %s to application",
                             rkcg->rkcg_group_id->str,
                             err == RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS
                                 ? "unassign"
                                 : "assign");

                /* FALLTHRU */
        }

        /* No application rebalance callback/event handler, or it is not
         * available, do the assign/unassign ourselves.
         * We need to be careful here not to trigger assignment_serve()
         * since it may call into the cgrp code again, in which case we
         * can't really track what the outcome state will be. */

        if (err == RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS)
                error = rd_kafka_cgrp_assign(rkcg, assignment);
        else
                error = rd_kafka_cgrp_unassign(rkcg);

        if (error) {
                rd_kafka_log(rkcg->rkcg_rk, LOG_ERR, "REBALANCE",
                             "Group \"%s\": internal %s "
                             "of %d partition(s) failed: %s: "
                             "unassigning all partitions and rejoining",
                             rkcg->rkcg_group_id->str,
                             err == RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS
                                 ? "unassign"
                                 : "assign",
                             rkcg->rkcg_group_assignment->cnt,
                             rd_kafka_error_string(error));
                rd_kafka_error_destroy(error);

                rd_kafka_cgrp_set_join_state(rkcg,
                                             /* This is a clean state for
                                              * assignment_done() to rejoin
                                              * from. */
                                             RD_KAFKA_CGRP_JOIN_STATE_STEADY);
                rd_kafka_assignment_clear(rkcg->rkcg_rk);
        }

        /* Now serve the assignment to make updates */
        rd_kafka_assignment_serve(rkcg->rkcg_rk);

done:
        if (err == RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS)
                rd_kafka_cgrp_group_assignment_set(rkcg, assignment);
        else
                rd_kafka_cgrp_group_assignment_set(rkcg, NULL);
}

/**
 * @brief Rejoin the group (KIP-848).
 */
static void
rd_kafka_cgrp_consumer_rejoin(rd_kafka_cgrp_t *rkcg, const char *fmt, ...) {
        char reason[512];
        va_list ap;
        char astr[128];

        va_start(ap, fmt);
        rd_vsnprintf(reason, sizeof(reason), fmt, ap);
        va_end(ap);

        if (rkcg->rkcg_group_assignment)
                rd_snprintf(astr, sizeof(astr), " with %d owned partition(s)",
                            rkcg->rkcg_group_assignment->cnt);
        else
                rd_snprintf(astr, sizeof(astr), " without an assignment");

        if (rkcg->rkcg_subscription || rkcg->rkcg_next_subscription) {
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP, "REJOIN",
                    "Group \"%s\": %s group%s: %s", rkcg->rkcg_group_id->str,
                    rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_INIT
                        ? "Joining"
                        : "Rejoining",
                    astr, reason);
        } else {
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP, "NOREJOIN",
                    "Group \"%s\": Not %s group%s: %s: "
                    "no subscribed topics",
                    rkcg->rkcg_group_id->str,
                    rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_INIT
                        ? "joining"
                        : "rejoining",
                    astr, reason);
        }

        rd_kafka_cgrp_leave_maybe(rkcg);
        rd_kafka_cgrp_consumer_reset(rkcg);
        rd_kafka_cgrp_set_join_state(rkcg, RD_KAFKA_CGRP_JOIN_STATE_INIT);
        rd_kafka_cgrp_consumer_expedite_next_heartbeat(rkcg, "rejoining");
}

/**
 * @brief Rejoin the group.
 *
 * @remark This function must not have any side-effects but setting the
 *         join state.
 */
static void rd_kafka_cgrp_rejoin(rd_kafka_cgrp_t *rkcg, const char *fmt, ...)
    RD_FORMAT(printf, 2, 3);

static void rd_kafka_cgrp_rejoin(rd_kafka_cgrp_t *rkcg, const char *fmt, ...) {
        char reason[512];
        va_list ap;
        char astr[128];
        if (rkcg->rkcg_group_protocol == RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                rd_kafka_cgrp_consumer_rejoin(rkcg, fmt, ap);
                return;
        }

        va_start(ap, fmt);
        rd_vsnprintf(reason, sizeof(reason), fmt, ap);
        va_end(ap);

        if (rkcg->rkcg_group_assignment)
                rd_snprintf(astr, sizeof(astr), " with %d owned partition(s)",
                            rkcg->rkcg_group_assignment->cnt);
        else
                rd_snprintf(astr, sizeof(astr), " without an assignment");

        if (rkcg->rkcg_subscription || rkcg->rkcg_next_subscription) {
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP, "REJOIN",
                    "Group \"%s\": %s group%s: %s", rkcg->rkcg_group_id->str,
                    rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_INIT
                        ? "Joining"
                        : "Rejoining",
                    astr, reason);
        } else {
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP, "NOREJOIN",
                    "Group \"%s\": Not %s group%s: %s: "
                    "no subscribed topics",
                    rkcg->rkcg_group_id->str,
                    rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_INIT
                        ? "joining"
                        : "rejoining",
                    astr, reason);

                rd_kafka_cgrp_leave_maybe(rkcg);
        }

        rd_kafka_cgrp_set_join_state(rkcg, RD_KAFKA_CGRP_JOIN_STATE_INIT);
}


/**
 * @brief Collect all assigned or owned partitions from group members.
 *        The member field of each result element is set to the associated
 *        group member. The members_match field is set to rd_false.
 *
 * @param members Array of group members.
 * @param member_cnt Number of elements in members.
 * @param par_cnt The total number of partitions expected to be collected.
 * @param collect_owned If rd_true, rkgm_owned partitions will be collected,
 *        else rkgm_assignment partitions will be collected.
 */
static map_toppar_member_info_t *
rd_kafka_collect_partitions(const rd_kafka_group_member_t *members,
                            size_t member_cnt,
                            size_t par_cnt,
                            rd_bool_t collect_owned) {
        size_t i;
        map_toppar_member_info_t *collected = rd_calloc(1, sizeof(*collected));

        RD_MAP_INIT(collected, par_cnt, rd_kafka_topic_partition_cmp,
                    rd_kafka_topic_partition_hash,
                    rd_kafka_topic_partition_destroy_free,
                    PartitionMemberInfo_free);

        for (i = 0; i < member_cnt; i++) {
                size_t j;
                const rd_kafka_group_member_t *rkgm = &members[i];
                const rd_kafka_topic_partition_list_t *toppars =
                    collect_owned ? rkgm->rkgm_owned : rkgm->rkgm_assignment;

                for (j = 0; j < (size_t)toppars->cnt; j++) {
                        rd_kafka_topic_partition_t *rktpar =
                            rd_kafka_topic_partition_copy(&toppars->elems[j]);
                        PartitionMemberInfo_t *pmi =
                            PartitionMemberInfo_new(rkgm, rd_false);
                        RD_MAP_SET(collected, rktpar, pmi);
                }
        }

        return collected;
}


/**
 * @brief Set intersection. Returns a set of all elements of \p a that
 *        are also elements of \p b. Additionally, compares the members
 *        field of matching elements from \p a and \p b and if not NULL
 *        and equal, sets the members_match field in the result element
 *        to rd_true and the member field to equal that of the elements,
 *        else sets the members_match field to rd_false and member field
 *        to NULL.
 */
static map_toppar_member_info_t *
rd_kafka_member_partitions_intersect(map_toppar_member_info_t *a,
                                     map_toppar_member_info_t *b) {
        const rd_kafka_topic_partition_t *key;
        const PartitionMemberInfo_t *a_v;
        map_toppar_member_info_t *intersection =
            rd_calloc(1, sizeof(*intersection));

        RD_MAP_INIT(
            intersection, RD_MIN(a ? RD_MAP_CNT(a) : 1, b ? RD_MAP_CNT(b) : 1),
            rd_kafka_topic_partition_cmp, rd_kafka_topic_partition_hash,
            rd_kafka_topic_partition_destroy_free, PartitionMemberInfo_free);

        if (!a || !b)
                return intersection;

        RD_MAP_FOREACH(key, a_v, a) {
                rd_bool_t members_match;
                const PartitionMemberInfo_t *b_v = RD_MAP_GET(b, key);

                if (b_v == NULL)
                        continue;

                members_match =
                    a_v->member && b_v->member &&
                    rd_kafka_group_member_cmp(a_v->member, b_v->member) == 0;

                RD_MAP_SET(intersection, rd_kafka_topic_partition_copy(key),
                           PartitionMemberInfo_new(b_v->member, members_match));
        }

        return intersection;
}


/**
 * @brief Set subtraction. Returns a set of all elements of \p a
 *        that are not elements of \p b. Sets the member field in
 *        elements in the returned set to equal that of the
 *        corresponding element in \p a
 */
static map_toppar_member_info_t *
rd_kafka_member_partitions_subtract(map_toppar_member_info_t *a,
                                    map_toppar_member_info_t *b) {
        const rd_kafka_topic_partition_t *key;
        const PartitionMemberInfo_t *a_v;
        map_toppar_member_info_t *difference =
            rd_calloc(1, sizeof(*difference));

        RD_MAP_INIT(difference, a ? RD_MAP_CNT(a) : 1,
                    rd_kafka_topic_partition_cmp, rd_kafka_topic_partition_hash,
                    rd_kafka_topic_partition_destroy_free,
                    PartitionMemberInfo_free);

        if (!a)
                return difference;

        RD_MAP_FOREACH(key, a_v, a) {
                const PartitionMemberInfo_t *b_v =
                    b ? RD_MAP_GET(b, key) : NULL;

                if (!b_v)
                        RD_MAP_SET(
                            difference, rd_kafka_topic_partition_copy(key),
                            PartitionMemberInfo_new(a_v->member, rd_false));
        }

        return difference;
}


/**
 * @brief Adjust the partition assignment as provided by the assignor
 *        according to the COOPERATIVE protocol.
 */
static void rd_kafka_cooperative_protocol_adjust_assignment(
    rd_kafka_cgrp_t *rkcg,
    rd_kafka_group_member_t *members,
    int member_cnt) {

        /* https://cwiki.apache.org/confluence/display/KAFKA/KIP-429%3A+Kafk\
           a+Consumer+Incremental+Rebalance+Protocol */

        int i;
        int expected_max_assignment_size;
        int total_assigned = 0;
        int not_revoking   = 0;
        size_t par_cnt     = 0;
        const rd_kafka_topic_partition_t *toppar;
        const PartitionMemberInfo_t *pmi;
        map_toppar_member_info_t *assigned;
        map_toppar_member_info_t *owned;
        map_toppar_member_info_t *maybe_revoking;
        map_toppar_member_info_t *ready_to_migrate;
        map_toppar_member_info_t *unknown_but_owned;

        for (i = 0; i < member_cnt; i++)
                par_cnt += members[i].rkgm_owned->cnt;

        assigned = rd_kafka_collect_partitions(members, member_cnt, par_cnt,
                                               rd_false /*assigned*/);

        owned = rd_kafka_collect_partitions(members, member_cnt, par_cnt,
                                            rd_true /*owned*/);

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRP",
                     "Group \"%s\": Partitions owned by members: %d, "
                     "partitions assigned by assignor: %d",
                     rkcg->rkcg_group_id->str, (int)RD_MAP_CNT(owned),
                     (int)RD_MAP_CNT(assigned));

        /* Still owned by some members */
        maybe_revoking = rd_kafka_member_partitions_intersect(assigned, owned);

        /* Not previously owned by anyone */
        ready_to_migrate = rd_kafka_member_partitions_subtract(assigned, owned);

        /* Don't exist in assigned partitions */
        unknown_but_owned =
            rd_kafka_member_partitions_subtract(owned, assigned);

        /* Rough guess at a size that is a bit higher than
         * the maximum number of partitions likely to be
         * assigned to any partition. */
        expected_max_assignment_size =
            (int)(RD_MAP_CNT(assigned) / member_cnt) + 4;

        for (i = 0; i < member_cnt; i++) {
                rd_kafka_group_member_t *rkgm = &members[i];
                rd_kafka_topic_partition_list_destroy(rkgm->rkgm_assignment);

                rkgm->rkgm_assignment = rd_kafka_topic_partition_list_new(
                    expected_max_assignment_size);
        }

        /* For maybe-revoking-partitions, check if the owner has
         * changed. If yes, exclude them from the assigned-partitions
         * list to the new owner. The old owner will realize it does
         * not own it any more, revoke it and then trigger another
         * rebalance for these partitions to finally be reassigned.
         */
        RD_MAP_FOREACH(toppar, pmi, maybe_revoking) {
                if (!pmi->members_match)
                        /* Owner has changed. */
                        continue;

                /* Owner hasn't changed. */
                rd_kafka_topic_partition_list_add(pmi->member->rkgm_assignment,
                                                  toppar->topic,
                                                  toppar->partition);

                total_assigned++;
                not_revoking++;
        }

        /* For ready-to-migrate-partitions, it is safe to move them
         * to the new member immediately since we know no one owns
         * it before, and hence we can encode the owner from the
         * newly-assigned-partitions directly.
         */
        RD_MAP_FOREACH(toppar, pmi, ready_to_migrate) {
                rd_kafka_topic_partition_list_add(pmi->member->rkgm_assignment,
                                                  toppar->topic,
                                                  toppar->partition);
                total_assigned++;
        }

        /* For unknown-but-owned-partitions, it is also safe to just
         * give them back to whoever claimed to be their owners by
         * encoding them directly as well. If this is due to a topic
         * metadata update, then a later rebalance will be triggered
         * anyway.
         */
        RD_MAP_FOREACH(toppar, pmi, unknown_but_owned) {
                rd_kafka_topic_partition_list_add(pmi->member->rkgm_assignment,
                                                  toppar->topic,
                                                  toppar->partition);
                total_assigned++;
        }

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRP",
                     "Group \"%s\": COOPERATIVE protocol collection sizes: "
                     "maybe revoking: %d, ready to migrate: %d, unknown but "
                     "owned: %d",
                     rkcg->rkcg_group_id->str, (int)RD_MAP_CNT(maybe_revoking),
                     (int)RD_MAP_CNT(ready_to_migrate),
                     (int)RD_MAP_CNT(unknown_but_owned));

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRP",
                     "Group \"%s\": %d partitions assigned to consumers",
                     rkcg->rkcg_group_id->str, total_assigned);

        RD_MAP_DESTROY_AND_FREE(maybe_revoking);
        RD_MAP_DESTROY_AND_FREE(ready_to_migrate);
        RD_MAP_DESTROY_AND_FREE(unknown_but_owned);
        RD_MAP_DESTROY_AND_FREE(assigned);
        RD_MAP_DESTROY_AND_FREE(owned);
}


/**
 * @brief Parses and handles the MemberState from a SyncGroupResponse.
 */
static void rd_kafka_cgrp_handle_SyncGroup_memberstate(
    rd_kafka_cgrp_t *rkcg,
    rd_kafka_broker_t *rkb,
    rd_kafka_resp_err_t err,
    const rd_kafkap_bytes_t *member_state) {
        rd_kafka_buf_t *rkbuf                       = NULL;
        rd_kafka_topic_partition_list_t *assignment = NULL;
        const int log_decode_errors                 = LOG_ERR;
        int16_t Version;
        rd_kafkap_bytes_t UserData;

        /* Dont handle new assignments when terminating */
        if (!err && rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE)
                err = RD_KAFKA_RESP_ERR__DESTROY;

        if (err)
                goto err;

        if (RD_KAFKAP_BYTES_LEN(member_state) == 0) {
                /* Empty assignment. */
                assignment = rd_kafka_topic_partition_list_new(0);
                memset(&UserData, 0, sizeof(UserData));
                goto done;
        }

        /* Parse assignment from MemberState */
        rkbuf = rd_kafka_buf_new_shadow(
            member_state->data, RD_KAFKAP_BYTES_LEN(member_state), NULL);
        /* Protocol parser needs a broker handle to log errors on. */
        if (rkb) {
                rkbuf->rkbuf_rkb = rkb;
                rd_kafka_broker_keep(rkb);
        } else
                rkbuf->rkbuf_rkb = rd_kafka_broker_internal(rkcg->rkcg_rk);

        rd_kafka_buf_read_i16(rkbuf, &Version);
        const rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};
        if (!(assignment = rd_kafka_buf_read_topic_partitions(
                  rkbuf, rd_false /*don't use topic_id*/, rd_true, 0, fields)))
                goto err_parse;
        rd_kafka_buf_read_kbytes(rkbuf, &UserData);

done:
        rd_kafka_cgrp_update_session_timeout(rkcg, rd_true /*reset timeout*/);

        rd_assert(rkcg->rkcg_assignor);
        if (rkcg->rkcg_assignor->rkas_on_assignment_cb) {
                char *member_id;
                RD_KAFKAP_STR_DUPA(&member_id, rkcg->rkcg_member_id);
                rd_kafka_consumer_group_metadata_t *cgmd =
                    rd_kafka_consumer_group_metadata_new_with_genid(
                        rkcg->rkcg_rk->rk_conf.group_id_str,
                        rkcg->rkcg_generation_id, member_id,
                        rkcg->rkcg_rk->rk_conf.group_instance_id);
                rkcg->rkcg_assignor->rkas_on_assignment_cb(
                    rkcg->rkcg_assignor, &(rkcg->rkcg_assignor_state),
                    assignment, &UserData, cgmd);
                rd_kafka_consumer_group_metadata_destroy(cgmd);
        }

        // FIXME: Remove when we're done debugging.
        rd_kafka_topic_partition_list_log(rkcg->rkcg_rk, "ASSIGNMENT",
                                          RD_KAFKA_DBG_CGRP, assignment);

        /* Set the new assignment */
        rd_kafka_cgrp_handle_assignment(rkcg, assignment);

        rd_kafka_topic_partition_list_destroy(assignment);

        if (rkbuf)
                rd_kafka_buf_destroy(rkbuf);

        return;

err_parse:
        err = rkbuf->rkbuf_err;

err:
        if (rkbuf)
                rd_kafka_buf_destroy(rkbuf);

        if (assignment)
                rd_kafka_topic_partition_list_destroy(assignment);

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "GRPSYNC",
                     "Group \"%s\": synchronization failed: %s: rejoining",
                     rkcg->rkcg_group_id->str, rd_kafka_err2str(err));

        if (err == RD_KAFKA_RESP_ERR_FENCED_INSTANCE_ID)
                rd_kafka_set_fatal_error(rkcg->rkcg_rk, err,
                                         "Fatal consumer error: %s",
                                         rd_kafka_err2str(err));
        else if (err == RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION)
                rkcg->rkcg_generation_id = -1;
        else if (err == RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID)
                rd_kafka_cgrp_set_member_id(rkcg, "");

        if (rd_kafka_cgrp_rebalance_protocol(rkcg) ==
                RD_KAFKA_REBALANCE_PROTOCOL_COOPERATIVE &&
            (err == RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION ||
             err == RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID))
                rd_kafka_cgrp_revoke_all_rejoin(
                    rkcg, rd_true /*assignment is lost*/,
                    rd_true /*this consumer is initiating*/, "SyncGroup error");
        else
                rd_kafka_cgrp_rejoin(rkcg, "SyncGroup error: %s",
                                     rd_kafka_err2str(err));
}



/**
 * @brief Cgrp handler for SyncGroup responses. opaque must be the cgrp handle.
 */
static void rd_kafka_cgrp_handle_SyncGroup(rd_kafka_t *rk,
                                           rd_kafka_broker_t *rkb,
                                           rd_kafka_resp_err_t err,
                                           rd_kafka_buf_t *rkbuf,
                                           rd_kafka_buf_t *request,
                                           void *opaque) {
        rd_kafka_cgrp_t *rkcg         = opaque;
        const int log_decode_errors   = LOG_ERR;
        int16_t ErrorCode             = 0;
        rd_kafkap_bytes_t MemberState = RD_ZERO_INIT;
        int actions;

        if (rkcg->rkcg_join_state != RD_KAFKA_CGRP_JOIN_STATE_WAIT_SYNC) {
                rd_kafka_dbg(
                    rkb->rkb_rk, CGRP, "SYNCGROUP",
                    "SyncGroup response: discarding outdated request "
                    "(now in join-state %s)",
                    rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);
                rd_kafka_cgrp_clear_wait_resp(rkcg, RD_KAFKAP_SyncGroup);
                return;
        }

        if (err) {
                ErrorCode = err;
                goto err;
        }

        if (request->rkbuf_reqhdr.ApiVersion >= 1)
                rd_kafka_buf_read_throttle_time(rkbuf);

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);
        rd_kafka_buf_read_kbytes(rkbuf, &MemberState);

err:
        actions = rd_kafka_err_action(rkb, ErrorCode, request,
                                      RD_KAFKA_ERR_ACTION_END);

        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                /* Re-query for coordinator */
                rd_kafka_cgrp_op(rkcg, NULL, RD_KAFKA_NO_REPLYQ,
                                 RD_KAFKA_OP_COORD_QUERY, ErrorCode);
                /* FALLTHRU */
        }

        if (actions & RD_KAFKA_ERR_ACTION_RETRY) {
                if (rd_kafka_buf_retry(rkb, request))
                        return;
                /* FALLTHRU */
        }

        rd_kafka_dbg(rkb->rkb_rk, CGRP, "SYNCGROUP",
                     "SyncGroup response: %s (%d bytes of MemberState data)",
                     rd_kafka_err2str(ErrorCode),
                     RD_KAFKAP_BYTES_LEN(&MemberState));

        rd_kafka_cgrp_clear_wait_resp(rkcg, RD_KAFKAP_SyncGroup);

        if (ErrorCode == RD_KAFKA_RESP_ERR__DESTROY)
                return; /* Termination */

        rd_kafka_cgrp_handle_SyncGroup_memberstate(rkcg, rkb, ErrorCode,
                                                   &MemberState);

        return;

err_parse:
        ErrorCode = rkbuf->rkbuf_err;
        goto err;
}


/**
 * @brief Run group assignment.
 */
static void rd_kafka_cgrp_assignor_run(rd_kafka_cgrp_t *rkcg,
                                       rd_kafka_assignor_t *rkas,
                                       rd_kafka_resp_err_t err,
                                       rd_kafka_metadata_internal_t *metadata,
                                       rd_kafka_group_member_t *members,
                                       int member_cnt) {
        char errstr[512];

        if (err) {
                rd_snprintf(errstr, sizeof(errstr),
                            "Failed to get cluster metadata: %s",
                            rd_kafka_err2str(err));
                goto err;
        }

        *errstr = '\0';

        /* Run assignor */
        err = rd_kafka_assignor_run(rkcg, rkas, &metadata->metadata, members,
                                    member_cnt, errstr, sizeof(errstr));

        if (err) {
                if (!*errstr)
                        rd_snprintf(errstr, sizeof(errstr), "%s",
                                    rd_kafka_err2str(err));
                goto err;
        }

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_CONSUMER, "ASSIGNOR",
                     "Group \"%s\": \"%s\" assignor run for %d member(s)",
                     rkcg->rkcg_group_id->str, rkas->rkas_protocol_name->str,
                     member_cnt);

        if (rkas->rkas_protocol == RD_KAFKA_REBALANCE_PROTOCOL_COOPERATIVE)
                rd_kafka_cooperative_protocol_adjust_assignment(rkcg, members,
                                                                member_cnt);

        rd_kafka_cgrp_set_join_state(rkcg, RD_KAFKA_CGRP_JOIN_STATE_WAIT_SYNC);

        rd_kafka_cgrp_set_wait_resp(rkcg, RD_KAFKAP_SyncGroup);

        /* Respond to broker with assignment set or error */
        rd_kafka_SyncGroupRequest(
            rkcg->rkcg_coord, rkcg->rkcg_group_id, rkcg->rkcg_generation_id,
            rkcg->rkcg_member_id, rkcg->rkcg_group_instance_id, members,
            err ? 0 : member_cnt, RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
            rd_kafka_cgrp_handle_SyncGroup, rkcg);
        return;

err:
        rd_kafka_log(rkcg->rkcg_rk, LOG_ERR, "ASSIGNOR",
                     "Group \"%s\": failed to run assignor \"%s\" for "
                     "%d member(s): %s",
                     rkcg->rkcg_group_id->str, rkas->rkas_protocol_name->str,
                     member_cnt, errstr);

        rd_kafka_cgrp_rejoin(rkcg, "%s assignor failed: %s",
                             rkas->rkas_protocol_name->str, errstr);
}



/**
 * @brief Op callback from handle_JoinGroup
 */
static rd_kafka_op_res_t
rd_kafka_cgrp_assignor_handle_Metadata_op(rd_kafka_t *rk,
                                          rd_kafka_q_t *rkq,
                                          rd_kafka_op_t *rko) {
        rd_kafka_cgrp_t *rkcg = rk->rk_cgrp;

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED; /* Terminating */

        if (rkcg->rkcg_join_state != RD_KAFKA_CGRP_JOIN_STATE_WAIT_METADATA)
                return RD_KAFKA_OP_RES_HANDLED; /* From outdated state */

        if (!rkcg->rkcg_group_leader.members) {
                rd_kafka_dbg(rk, CGRP, "GRPLEADER",
                             "Group \"%.*s\": no longer leader: "
                             "not running assignor",
                             RD_KAFKAP_STR_PR(rkcg->rkcg_group_id));
                return RD_KAFKA_OP_RES_HANDLED;
        }

        rd_kafka_cgrp_assignor_run(rkcg, rkcg->rkcg_assignor, rko->rko_err,
                                   rko->rko_u.metadata.mdi,
                                   rkcg->rkcg_group_leader.members,
                                   rkcg->rkcg_group_leader.member_cnt);

        return RD_KAFKA_OP_RES_HANDLED;
}


/**
 * Parse single JoinGroup.Members.MemberMetadata for "consumer" ProtocolType
 *
 * Protocol definition:
 * https://cwiki.apache.org/confluence/display/KAFKA/Kafka+Client-side+Assignment+Proposal
 *
 * Returns 0 on success or -1 on error.
 */
static int rd_kafka_group_MemberMetadata_consumer_read(
    rd_kafka_broker_t *rkb,
    rd_kafka_group_member_t *rkgm,
    const rd_kafkap_bytes_t *MemberMetadata) {

        rd_kafka_buf_t *rkbuf;
        int16_t Version;
        int32_t subscription_cnt;
        rd_kafkap_bytes_t UserData;
        const int log_decode_errors = LOG_ERR;
        rd_kafka_resp_err_t err     = RD_KAFKA_RESP_ERR__BAD_MSG;

        /* Create a shadow-buffer pointing to the metadata to ease parsing. */
        rkbuf = rd_kafka_buf_new_shadow(
            MemberMetadata->data, RD_KAFKAP_BYTES_LEN(MemberMetadata), NULL);

        /* Protocol parser needs a broker handle to log errors on.
         * If none is provided, don't log errors (mainly for unit tests). */
        if (rkb) {
                rkbuf->rkbuf_rkb = rkb;
                rd_kafka_broker_keep(rkb);
        }

        rd_kafka_buf_read_i16(rkbuf, &Version);
        rd_kafka_buf_read_i32(rkbuf, &subscription_cnt);

        if (subscription_cnt > 10000 || subscription_cnt <= 0)
                goto err;

        rkgm->rkgm_subscription =
            rd_kafka_topic_partition_list_new(subscription_cnt);

        while (subscription_cnt-- > 0) {
                rd_kafkap_str_t Topic;
                char *topic_name;
                rd_kafka_buf_read_str(rkbuf, &Topic);
                RD_KAFKAP_STR_DUPA(&topic_name, &Topic);
                rd_kafka_topic_partition_list_add(
                    rkgm->rkgm_subscription, topic_name, RD_KAFKA_PARTITION_UA);
        }

        rd_kafka_buf_read_kbytes(rkbuf, &UserData);
        rkgm->rkgm_userdata = rd_kafkap_bytes_copy(&UserData);

        const rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};
        if (Version >= 1 &&
            !(rkgm->rkgm_owned = rd_kafka_buf_read_topic_partitions(
                  rkbuf, rd_false /*don't use topic_id*/, rd_true, 0, fields)))
                goto err;

        if (Version >= 2) {
                rd_kafka_buf_read_i32(rkbuf, &rkgm->rkgm_generation);
        }

        if (Version >= 3) {
                rd_kafkap_str_t RackId = RD_KAFKAP_STR_INITIALIZER;
                rd_kafka_buf_read_str(rkbuf, &RackId);
                rkgm->rkgm_rack_id = rd_kafkap_str_copy(&RackId);
        }

        rd_kafka_buf_destroy(rkbuf);

        return 0;

err_parse:
        err = rkbuf->rkbuf_err;

err:
        if (rkb)
                rd_rkb_dbg(rkb, CGRP, "MEMBERMETA",
                           "Failed to parse MemberMetadata for \"%.*s\": %s",
                           RD_KAFKAP_STR_PR(rkgm->rkgm_member_id),
                           rd_kafka_err2str(err));
        if (rkgm->rkgm_subscription) {
                rd_kafka_topic_partition_list_destroy(rkgm->rkgm_subscription);
                rkgm->rkgm_subscription = NULL;
        }

        rd_kafka_buf_destroy(rkbuf);
        return -1;
}


/**
 * @brief cgrp handler for JoinGroup responses
 * opaque must be the cgrp handle.
 *
 * @locality rdkafka main thread (unless ERR__DESTROY: arbitrary thread)
 */
static void rd_kafka_cgrp_handle_JoinGroup(rd_kafka_t *rk,
                                           rd_kafka_broker_t *rkb,
                                           rd_kafka_resp_err_t err,
                                           rd_kafka_buf_t *rkbuf,
                                           rd_kafka_buf_t *request,
                                           void *opaque) {
        rd_kafka_cgrp_t *rkcg       = opaque;
        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode           = 0;
        int32_t GenerationId;
        rd_kafkap_str_t Protocol, LeaderId;
        rd_kafkap_str_t MyMemberId = RD_KAFKAP_STR_INITIALIZER;
        int32_t member_cnt;
        int actions;
        int i_am_leader           = 0;
        rd_kafka_assignor_t *rkas = NULL;

        rd_kafka_cgrp_clear_wait_resp(rkcg, RD_KAFKAP_JoinGroup);

        if (err == RD_KAFKA_RESP_ERR__DESTROY ||
            rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE)
                return; /* Terminating */

        if (rkcg->rkcg_join_state != RD_KAFKA_CGRP_JOIN_STATE_WAIT_JOIN) {
                rd_kafka_dbg(
                    rkb->rkb_rk, CGRP, "JOINGROUP",
                    "JoinGroup response: discarding outdated request "
                    "(now in join-state %s)",
                    rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);
                return;
        }

        if (err) {
                ErrorCode = err;
                goto err;
        }

        if (request->rkbuf_reqhdr.ApiVersion >= 2)
                rd_kafka_buf_read_throttle_time(rkbuf);

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);
        rd_kafka_buf_read_i32(rkbuf, &GenerationId);
        rd_kafka_buf_read_str(rkbuf, &Protocol);
        rd_kafka_buf_read_str(rkbuf, &LeaderId);
        rd_kafka_buf_read_str(rkbuf, &MyMemberId);
        rd_kafka_buf_read_i32(rkbuf, &member_cnt);

        if (!ErrorCode && RD_KAFKAP_STR_IS_NULL(&Protocol)) {
                /* Protocol not set, we will not be able to find
                 * a matching assignor so error out early. */
                ErrorCode = RD_KAFKA_RESP_ERR__BAD_MSG;
        } else if (!ErrorCode) {
                char *protocol_name;
                RD_KAFKAP_STR_DUPA(&protocol_name, &Protocol);
                if (!(rkas = rd_kafka_assignor_find(rkcg->rkcg_rk,
                                                    protocol_name)) ||
                    !rkas->rkas_enabled) {
                        rd_kafka_dbg(rkb->rkb_rk, CGRP, "JOINGROUP",
                                     "Unsupported assignment strategy \"%s\"",
                                     protocol_name);
                        if (rkcg->rkcg_assignor) {
                                if (rkcg->rkcg_assignor
                                        ->rkas_destroy_state_cb &&
                                    rkcg->rkcg_assignor_state)
                                        rkcg->rkcg_assignor
                                            ->rkas_destroy_state_cb(
                                                rkcg->rkcg_assignor_state);
                                rkcg->rkcg_assignor_state = NULL;
                                rkcg->rkcg_assignor       = NULL;
                        }
                        ErrorCode = RD_KAFKA_RESP_ERR__UNKNOWN_PROTOCOL;
                }
        }

        rd_kafka_dbg(rkb->rkb_rk, CGRP, "JOINGROUP",
                     "JoinGroup response: GenerationId %" PRId32
                     ", "
                     "Protocol %.*s, LeaderId %.*s%s, my MemberId %.*s, "
                     "member metadata count "
                     "%" PRId32 ": %s",
                     GenerationId, RD_KAFKAP_STR_PR(&Protocol),
                     RD_KAFKAP_STR_PR(&LeaderId),
                     RD_KAFKAP_STR_LEN(&MyMemberId) &&
                             !rd_kafkap_str_cmp(&LeaderId, &MyMemberId)
                         ? " (me)"
                         : "",
                     RD_KAFKAP_STR_PR(&MyMemberId), member_cnt,
                     ErrorCode ? rd_kafka_err2str(ErrorCode) : "(no error)");

        if (!ErrorCode) {
                char *my_member_id;
                RD_KAFKAP_STR_DUPA(&my_member_id, &MyMemberId);
                rd_kafka_cgrp_set_member_id(rkcg, my_member_id);
                rkcg->rkcg_generation_id = GenerationId;
                i_am_leader = !rd_kafkap_str_cmp(&LeaderId, &MyMemberId);
        } else {
                rd_interval_backoff(&rkcg->rkcg_join_intvl, 1000 * 1000);
                goto err;
        }

        if (rkcg->rkcg_assignor && rkcg->rkcg_assignor != rkas) {
                if (rkcg->rkcg_assignor->rkas_destroy_state_cb &&
                    rkcg->rkcg_assignor_state)
                        rkcg->rkcg_assignor->rkas_destroy_state_cb(
                            rkcg->rkcg_assignor_state);
                rkcg->rkcg_assignor_state = NULL;
        }
        rkcg->rkcg_assignor = rkas;

        if (i_am_leader) {
                rd_kafka_group_member_t *members;
                int i;
                int sub_cnt = 0;
                rd_list_t topics;
                rd_kafka_op_t *rko;
                rd_bool_t any_member_rack = rd_false;
                rd_kafka_dbg(rkb->rkb_rk, CGRP, "JOINGROUP",
                             "I am elected leader for group \"%s\" "
                             "with %" PRId32 " member(s)",
                             rkcg->rkcg_group_id->str, member_cnt);

                if (member_cnt > 100000) {
                        err = RD_KAFKA_RESP_ERR__BAD_MSG;
                        goto err;
                }

                rd_list_init(&topics, member_cnt, rd_free);

                members = rd_calloc(member_cnt, sizeof(*members));

                for (i = 0; i < member_cnt; i++) {
                        rd_kafkap_str_t MemberId;
                        rd_kafkap_bytes_t MemberMetadata;
                        rd_kafka_group_member_t *rkgm;
                        rd_kafkap_str_t GroupInstanceId =
                            RD_KAFKAP_STR_INITIALIZER;

                        rd_kafka_buf_read_str(rkbuf, &MemberId);
                        if (request->rkbuf_reqhdr.ApiVersion >= 5)
                                rd_kafka_buf_read_str(rkbuf, &GroupInstanceId);
                        rd_kafka_buf_read_kbytes(rkbuf, &MemberMetadata);

                        rkgm                 = &members[sub_cnt];
                        rkgm->rkgm_member_id = rd_kafkap_str_copy(&MemberId);
                        rkgm->rkgm_group_instance_id =
                            rd_kafkap_str_copy(&GroupInstanceId);
                        rd_list_init(&rkgm->rkgm_eligible, 0, NULL);
                        rkgm->rkgm_generation = -1;

                        if (rd_kafka_group_MemberMetadata_consumer_read(
                                rkb, rkgm, &MemberMetadata)) {
                                /* Failed to parse this member's metadata,
                                 * ignore it. */
                        } else {
                                sub_cnt++;
                                rkgm->rkgm_assignment =
                                    rd_kafka_topic_partition_list_new(
                                        rkgm->rkgm_subscription->cnt);
                                rd_kafka_topic_partition_list_get_topic_names(
                                    rkgm->rkgm_subscription, &topics,
                                    0 /*dont include regex*/);
                                if (!any_member_rack && rkgm->rkgm_rack_id &&
                                    RD_KAFKAP_STR_LEN(rkgm->rkgm_rack_id))
                                        any_member_rack = rd_true;
                        }
                }

                /* FIXME: What to do if parsing failed for some/all members?
                 *        It is a sign of incompatibility. */


                rd_kafka_cgrp_group_leader_reset(rkcg,
                                                 "JoinGroup response clean-up");

                rd_kafka_assert(NULL, rkcg->rkcg_group_leader.members == NULL);
                rkcg->rkcg_group_leader.members    = members;
                rkcg->rkcg_group_leader.member_cnt = sub_cnt;

                rd_kafka_cgrp_set_join_state(
                    rkcg, RD_KAFKA_CGRP_JOIN_STATE_WAIT_METADATA);

                /* The assignor will need metadata so fetch it asynchronously
                 * and run the assignor when we get a reply.
                 * Create a callback op that the generic metadata code
                 * will trigger when metadata has been parsed. */
                rko = rd_kafka_op_new_cb(
                    rkcg->rkcg_rk, RD_KAFKA_OP_METADATA,
                    rd_kafka_cgrp_assignor_handle_Metadata_op);
                rd_kafka_op_set_replyq(rko, rkcg->rkcg_ops, NULL);

                rd_kafka_MetadataRequest(
                    rkb, &topics, NULL, "partition assignor",
                    rd_false /*!allow_auto_create*/,
                    /* cgrp_update=false:
                     * Since the subscription list may not be identical
                     * across all members of the group and thus the
                     * Metadata response may not be identical to this
                     * consumer's subscription list, we want to
                     * avoid triggering a rejoin or error propagation
                     * on receiving the response since some topics
                     * may be missing. */
                    rd_false,
                    /* cgrp_update=false: no subscription version is used */
                    -1,
                    /* force_racks is true if any memeber has a client rack set,
                       since we will require partition to rack mapping in that
                       case for rack-aware assignors. */
                    any_member_rack, rko);
                rd_list_destroy(&topics);

        } else {
                rd_kafka_cgrp_set_join_state(
                    rkcg, RD_KAFKA_CGRP_JOIN_STATE_WAIT_SYNC);

                rd_kafka_cgrp_set_wait_resp(rkcg, RD_KAFKAP_SyncGroup);

                rd_kafka_SyncGroupRequest(
                    rkb, rkcg->rkcg_group_id, rkcg->rkcg_generation_id,
                    rkcg->rkcg_member_id, rkcg->rkcg_group_instance_id, NULL, 0,
                    RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
                    rd_kafka_cgrp_handle_SyncGroup, rkcg);
        }

err:
        actions = rd_kafka_err_action(
            rkb, ErrorCode, request, RD_KAFKA_ERR_ACTION_IGNORE,
            RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID,

            RD_KAFKA_ERR_ACTION_IGNORE, RD_KAFKA_RESP_ERR_MEMBER_ID_REQUIRED,

            RD_KAFKA_ERR_ACTION_IGNORE, RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION,

            RD_KAFKA_ERR_ACTION_PERMANENT, RD_KAFKA_RESP_ERR_FENCED_INSTANCE_ID,

            RD_KAFKA_ERR_ACTION_END);

        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                /* Re-query for coordinator */
                rd_kafka_cgrp_op(rkcg, NULL, RD_KAFKA_NO_REPLYQ,
                                 RD_KAFKA_OP_COORD_QUERY, ErrorCode);
        }

        /* No need for retries here since the join is intervalled,
         * see rkcg_join_intvl */

        if (ErrorCode) {
                if (ErrorCode == RD_KAFKA_RESP_ERR__DESTROY)
                        return; /* Termination */

                if (ErrorCode == RD_KAFKA_RESP_ERR_FENCED_INSTANCE_ID) {
                        rd_kafka_set_fatal_error(rkcg->rkcg_rk, ErrorCode,
                                                 "Fatal consumer error: %s",
                                                 rd_kafka_err2str(ErrorCode));
                        ErrorCode = RD_KAFKA_RESP_ERR__FATAL;

                } else if (actions & RD_KAFKA_ERR_ACTION_PERMANENT)
                        rd_kafka_consumer_err(
                            rkcg->rkcg_q, rd_kafka_broker_id(rkb), ErrorCode, 0,
                            NULL, NULL, RD_KAFKA_OFFSET_INVALID,
                            "JoinGroup failed: %s",
                            rd_kafka_err2str(ErrorCode));

                if (ErrorCode == RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID)
                        rd_kafka_cgrp_set_member_id(rkcg, "");
                else if (ErrorCode == RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION)
                        rkcg->rkcg_generation_id = -1;
                else if (ErrorCode == RD_KAFKA_RESP_ERR_MEMBER_ID_REQUIRED) {
                        /* KIP-394 requires member.id on initial join
                         * group request */
                        char *my_member_id;
                        RD_KAFKAP_STR_DUPA(&my_member_id, &MyMemberId);
                        rd_kafka_cgrp_set_member_id(rkcg, my_member_id);
                        /* Skip the join backoff */
                        rd_interval_reset(&rkcg->rkcg_join_intvl);
                }

                if (rd_kafka_cgrp_rebalance_protocol(rkcg) ==
                        RD_KAFKA_REBALANCE_PROTOCOL_COOPERATIVE &&
                    (ErrorCode == RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION ||
                     ErrorCode == RD_KAFKA_RESP_ERR_MEMBER_ID_REQUIRED))
                        rd_kafka_cgrp_revoke_all_rejoin(
                            rkcg, rd_true /*assignment is lost*/,
                            rd_true /*this consumer is initiating*/,
                            "JoinGroup error");
                else
                        rd_kafka_cgrp_rejoin(rkcg, "JoinGroup error: %s",
                                             rd_kafka_err2str(ErrorCode));
        }

        return;

err_parse:
        ErrorCode = rkbuf->rkbuf_err;
        goto err;
}


/**
 * @brief Check subscription against requested Metadata.
 */
static rd_kafka_op_res_t rd_kafka_cgrp_handle_Metadata_op(rd_kafka_t *rk,
                                                          rd_kafka_q_t *rkq,
                                                          rd_kafka_op_t *rko) {
        rd_kafka_cgrp_t *rkcg = rk->rk_cgrp;

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED; /* Terminating */

        if (rd_atomic32_get(&rkcg->rkcg_subscription_version) ==
            rko->rko_u.metadata.subscription_version)
                rd_kafka_cgrp_metadata_update_check(rkcg,
                                                    rd_false /*dont rejoin*/);

        return RD_KAFKA_OP_RES_HANDLED;
}


/**
 * @brief (Async) Refresh metadata (for cgrp's needs)
 *
 * @returns 1 if metadata refresh was requested, or 0 if metadata is
 *          up to date, or -1 if no broker is available for metadata requests.
 *
 * @locks none
 * @locality rdkafka main thread
 */
static int rd_kafka_cgrp_metadata_refresh(rd_kafka_cgrp_t *rkcg,
                                          int *metadata_agep,
                                          int32_t cgrp_subscription_version,
                                          const char *reason) {
        rd_kafka_t *rk = rkcg->rkcg_rk;
        rd_kafka_op_t *rko;
        rd_list_t topics;
        rd_kafka_resp_err_t err;

        rd_list_init(&topics, 8, rd_free);

        /* Insert all non-wildcard topics in cache. */
        rd_kafka_metadata_cache_hint_rktparlist(rkcg->rkcg_rk,
                                                rkcg->rkcg_subscription, NULL);

        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WILDCARD_SUBSCRIPTION) {
                /* For wildcard subscriptions make sure the
                 * cached full metadata isn't too old. */
                int metadata_age = -1;

                if (rk->rk_ts_full_metadata)
                        metadata_age =
                            (int)(rd_clock() - rk->rk_ts_full_metadata) / 1000;

                *metadata_agep = metadata_age;

                if (metadata_age != -1 &&
                    metadata_age <= rk->rk_conf.metadata_max_age_ms) {
                        rd_kafka_dbg(rk, CGRP | RD_KAFKA_DBG_METADATA,
                                     "CGRPMETADATA",
                                     "%s: metadata for wildcard subscription "
                                     "is up to date (%dms old)",
                                     reason, *metadata_agep);
                        rd_list_destroy(&topics);
                        return 0; /* Up-to-date */
                }

        } else {
                /* Check that all subscribed topics are in the cache. */
                int r;

                rd_kafka_topic_partition_list_get_topic_names(
                    rkcg->rkcg_subscription, &topics, 0 /*no regexps*/);

                rd_kafka_rdlock(rk);
                r = rd_kafka_metadata_cache_topics_count_exists(rk, &topics,
                                                                metadata_agep);
                rd_kafka_rdunlock(rk);

                if (r == rd_list_cnt(&topics)) {
                        rd_kafka_dbg(rk, CGRP | RD_KAFKA_DBG_METADATA,
                                     "CGRPMETADATA",
                                     "%s: metadata for subscription "
                                     "is up to date (%dms old)",
                                     reason, *metadata_agep);
                        rd_list_destroy(&topics);
                        return 0; /* Up-to-date and all topics exist. */
                }

                rd_kafka_dbg(rk, CGRP | RD_KAFKA_DBG_METADATA, "CGRPMETADATA",
                             "%s: metadata for subscription "
                             "only available for %d/%d topics (%dms old)",
                             reason, r, rd_list_cnt(&topics), *metadata_agep);
        }

        /* Async request, result will be triggered from
         * rd_kafka_parse_metadata(). */
        rko = rd_kafka_op_new_cb(rkcg->rkcg_rk, RD_KAFKA_OP_METADATA,
                                 rd_kafka_cgrp_handle_Metadata_op);
        rd_kafka_op_set_replyq(rko, rkcg->rkcg_ops, 0);

        err = rd_kafka_metadata_request(
            rkcg->rkcg_rk, NULL, &topics, rd_false /*!allow auto create */,
            rd_true /*cgrp_update*/, cgrp_subscription_version, reason, rko);
        if (err) {
                /* Hint cache that something is interested in
                 * these topics so that they will be included in
                 * a future all known_topics query. */

                rd_kafka_wrlock(rk);
                rd_kafka_metadata_cache_hint(rk, &topics, NULL,
                                             RD_KAFKA_RESP_ERR__NOENT);
                rd_kafka_wrunlock(rk);

                rd_kafka_dbg(rk, CGRP | RD_KAFKA_DBG_METADATA, "CGRPMETADATA",
                             "%s: need to refresh metadata (%dms old) "
                             "but no usable brokers available: %s",
                             reason, *metadata_agep, rd_kafka_err2str(err));
                rd_kafka_op_destroy(rko);
        }

        rd_list_destroy(&topics);

        return err ? -1 : 1;
}



static void rd_kafka_cgrp_join(rd_kafka_cgrp_t *rkcg,
                               int32_t cgrp_subscription_version) {
        int metadata_age, metadata_refresh_outcome;

        if (rkcg->rkcg_state != RD_KAFKA_CGRP_STATE_UP ||
            rkcg->rkcg_join_state != RD_KAFKA_CGRP_JOIN_STATE_INIT ||
            rd_kafka_cgrp_awaiting_response(rkcg))
                return;

        /* On max.poll.interval.ms failure, do not rejoin group until the
         * application has called poll. */
        if ((rkcg->rkcg_flags & RD_KAFKA_CGRP_F_MAX_POLL_EXCEEDED) &&
            rd_kafka_max_poll_exceeded(rkcg->rkcg_rk))
                return;

        rkcg->rkcg_flags &= ~RD_KAFKA_CGRP_F_MAX_POLL_EXCEEDED;

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "JOIN",
                     "Group \"%.*s\": join with %d subscribed topic(s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_list_cnt(rkcg->rkcg_subscribed_topics));


        /* See if we need to query metadata to continue:
         * - if subscription contains wildcards:
         *   * query all topics in cluster
         *
         * - if subscription does not contain wildcards but
         *   some topics are missing from the local metadata cache:
         *   * query subscribed topics (all cached ones)
         *
         * - otherwise:
         *   * rely on topic metadata cache
         */
        /* We need up-to-date full metadata to continue,
         * refresh metadata if necessary. */
        metadata_refresh_outcome = rd_kafka_cgrp_metadata_refresh(
            rkcg, &metadata_age, cgrp_subscription_version, "consumer join");
        if (metadata_refresh_outcome == 1) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_CONSUMER,
                             "JOIN",
                             "Group \"%.*s\": "
                             "postponing join until up-to-date "
                             "metadata is available",
                             RD_KAFKAP_STR_PR(rkcg->rkcg_group_id));

                rd_assert(
                    rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_INIT ||
                    /* Possible via rd_kafka_cgrp_modify_subscription */
                    rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_STEADY);

                rd_kafka_cgrp_set_join_state(
                    rkcg, RD_KAFKA_CGRP_JOIN_STATE_WAIT_METADATA);

                return; /* ^ async call */
        } else if (metadata_refresh_outcome == -1) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_CONSUMER,
                             "JOIN",
                             "Group \"%.*s\": "
                             "postponing join until up-to-date "
                             "metadata can be requested",
                             RD_KAFKAP_STR_PR(rkcg->rkcg_group_id));
                return; /* ^ async call */
        }

        if (rd_list_empty(rkcg->rkcg_subscribed_topics))
                rd_kafka_cgrp_metadata_update_check(rkcg,
                                                    rd_false /*dont join*/);

        if (rd_list_empty(rkcg->rkcg_subscribed_topics)) {
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_CONSUMER, "JOIN",
                    "Group \"%.*s\": "
                    "no matching topics based on %dms old metadata: "
                    "next metadata refresh in %dms",
                    RD_KAFKAP_STR_PR(rkcg->rkcg_group_id), metadata_age,
                    rkcg->rkcg_rk->rk_conf.metadata_refresh_interval_ms -
                        metadata_age);
                return;
        }

        rd_rkb_dbg(
            rkcg->rkcg_curr_coord, CONSUMER | RD_KAFKA_DBG_CGRP, "JOIN",
            "Joining group \"%.*s\" with %d subscribed topic(s) and "
            "member id \"%.*s\"",
            RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
            rd_list_cnt(rkcg->rkcg_subscribed_topics),
            rkcg->rkcg_member_id ? RD_KAFKAP_STR_LEN(rkcg->rkcg_member_id) : 0,
            rkcg->rkcg_member_id ? rkcg->rkcg_member_id->str : "");


        rd_kafka_cgrp_set_join_state(rkcg, RD_KAFKA_CGRP_JOIN_STATE_WAIT_JOIN);

        rd_kafka_cgrp_set_wait_resp(rkcg, RD_KAFKAP_JoinGroup);

        rd_kafka_JoinGroupRequest(
            rkcg->rkcg_coord, rkcg->rkcg_group_id, rkcg->rkcg_member_id,
            rkcg->rkcg_group_instance_id,
            rkcg->rkcg_rk->rk_conf.group_protocol_type,
            rkcg->rkcg_subscribed_topics, RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
            rd_kafka_cgrp_handle_JoinGroup, rkcg);
}

/**
 * Rejoin group on update to effective subscribed topics list
 */
static void rd_kafka_cgrp_revoke_rejoin(rd_kafka_cgrp_t *rkcg,
                                        const char *reason) {
        /*
         * Clean-up group leader duties, if any.
         */
        rd_kafka_cgrp_group_leader_reset(rkcg, "group (re)join");

        rd_kafka_dbg(
            rkcg->rkcg_rk, CGRP, "REJOIN",
            "Group \"%.*s\" (re)joining in join-state %s "
            "with %d assigned partition(s): %s",
            RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
            rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
            rkcg->rkcg_group_assignment ? rkcg->rkcg_group_assignment->cnt : 0,
            reason);

        rd_kafka_cgrp_revoke_all_rejoin(rkcg, rd_false /*not lost*/,
                                        rd_true /*initiating*/, reason);
}

/**
 * @brief Update the effective list of subscribed topics.
 *
 * Set \p tinfos to NULL to clear the list.
 *
 * @param tinfos rd_list_t(rd_kafka_topic_info_t *): new effective topic list
 *
 * @returns true on change, else false.
 *
 * @remark Takes ownership of \p tinfos
 */
static rd_bool_t rd_kafka_cgrp_update_subscribed_topics(rd_kafka_cgrp_t *rkcg,
                                                        rd_list_t *tinfos) {
        rd_kafka_topic_info_t *tinfo;
        int i;

        if (!tinfos) {
                if (!rd_list_empty(rkcg->rkcg_subscribed_topics))
                        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "SUBSCRIPTION",
                                     "Group \"%.*s\": "
                                     "clearing subscribed topics list (%d)",
                                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                                     rd_list_cnt(rkcg->rkcg_subscribed_topics));
                tinfos = rd_list_new(0, rd_kafka_topic_info_destroy_free);

        } else {
                if (rd_list_cnt(tinfos) == 0)
                        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "SUBSCRIPTION",
                                     "Group \"%.*s\": "
                                     "no topics in metadata matched "
                                     "subscription",
                                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id));
        }

        /* Sort for comparison */
        rd_list_sort(tinfos, rd_kafka_topic_info_cmp);

        /* Compare to existing to see if anything changed. */
        if (!rd_list_cmp(rkcg->rkcg_subscribed_topics, tinfos,
                         rd_kafka_topic_info_cmp)) {
                /* No change */
                rd_list_destroy(tinfos);
                return rd_false;
        }

        rd_kafka_dbg(
            rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_METADATA, "SUBSCRIPTION",
            "Group \"%.*s\": effective subscription list changed "
            "from %d to %d topic(s):",
            RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
            rd_list_cnt(rkcg->rkcg_subscribed_topics), rd_list_cnt(tinfos));

        RD_LIST_FOREACH(tinfo, tinfos, i)
        rd_kafka_dbg(rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_METADATA,
                     "SUBSCRIPTION", " Topic %s with %d partition(s)",
                     tinfo->topic, tinfo->partition_cnt);

        rd_list_destroy(rkcg->rkcg_subscribed_topics);

        rkcg->rkcg_subscribed_topics = tinfos;

        return rd_true;
}

/**
 * Compares a new target assignment with
 * existing consumer group assignment.
 *
 * Returns that they're the same assignment
 * in two cases:
 *
 * 1) If target assignment is present and the
 *    new assignment is same as target assignment,
 *    then we are already in process of adding that
 *    target assignment.
 * 2) If target assignment is not present and
 *    the new assignment is same as current assignment,
 *    then we are already at correct assignment.
 *
 * @param new_target_assignment New target assignment
 *
 * @return Is the new assignment different from what's being handled by
 *         group \p cgrp ?
 **/
static rd_bool_t rd_kafka_cgrp_consumer_is_new_assignment_different(
    rd_kafka_cgrp_t *rkcg,
    rd_kafka_topic_partition_list_t *new_target_assignment) {
        int is_assignment_different;
        if (rkcg->rkcg_target_assignment) {
                is_assignment_different = rd_kafka_topic_partition_list_cmp(
                    new_target_assignment, rkcg->rkcg_target_assignment,
                    rd_kafka_topic_partition_by_id_cmp);
        } else {
                is_assignment_different = rd_kafka_topic_partition_list_cmp(
                    new_target_assignment, rkcg->rkcg_current_assignment,
                    rd_kafka_topic_partition_by_id_cmp);
        }
        return is_assignment_different ? rd_true : rd_false;
}

static rd_kafka_op_res_t rd_kafka_cgrp_consumer_handle_next_assignment(
    rd_kafka_cgrp_t *rkcg,
    rd_kafka_topic_partition_list_t *new_target_assignment,
    rd_bool_t clear_next_assignment) {
        rd_bool_t is_assignment_different = rd_false;
        rd_bool_t has_next_target_assignment_to_clear =
            rkcg->rkcg_next_target_assignment && clear_next_assignment;
        if (rkcg->rkcg_consumer_flags & RD_KAFKA_CGRP_CONSUMER_F_WAIT_ACK) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "HEARTBEAT",
                             "Reconciliation in progress, "
                             "postponing next one");
                return RD_KAFKA_OP_RES_HANDLED;
        }

        is_assignment_different =
            rd_kafka_cgrp_consumer_is_new_assignment_different(
                rkcg, new_target_assignment);

        /* Starts reconcilation only when the group is in state
         * INIT or state STEADY, keeps it as next target assignment
         * otherwise. */
        if (!is_assignment_different) {
                if (has_next_target_assignment_to_clear) {
                        rd_kafka_topic_partition_list_destroy(
                            rkcg->rkcg_next_target_assignment);
                        rkcg->rkcg_next_target_assignment = NULL;
                }

                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "HEARTBEAT",
                             "Not reconciling new assignment: "
                             "Assignment is the same. "
                             "Next assignment %s",
                             (has_next_target_assignment_to_clear
                                  ? "cleared"
                                  : "not cleared"));

        } else if (rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_INIT ||
                   rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_STEADY) {
                rkcg->rkcg_consumer_flags |= RD_KAFKA_CGRP_CONSUMER_F_WAIT_ACK;
                if (rkcg->rkcg_target_assignment) {
                        rd_kafka_topic_partition_list_destroy(
                            rkcg->rkcg_target_assignment);
                }
                rkcg->rkcg_target_assignment =
                    rd_kafka_topic_partition_list_copy(new_target_assignment);

                if (has_next_target_assignment_to_clear) {
                        rd_kafka_topic_partition_list_destroy(
                            rkcg->rkcg_next_target_assignment);
                        rkcg->rkcg_next_target_assignment = NULL;
                }

                if (rd_kafka_is_dbg(rkcg->rkcg_rk, CGRP)) {
                        char rkcg_target_assignment_str[512] = "NULL";

                        rd_kafka_topic_partition_list_str(
                            rkcg->rkcg_target_assignment,
                            rkcg_target_assignment_str,
                            sizeof(rkcg_target_assignment_str), 0);

                        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "HEARTBEAT",
                                     "Reconciliation starts with new target "
                                     "assignment \"%s\". "
                                     "Next assignment %s",
                                     rkcg_target_assignment_str,
                                     (has_next_target_assignment_to_clear
                                          ? "cleared"
                                          : "not cleared"));
                }
                rd_kafka_cgrp_handle_assignment(rkcg,
                                                rkcg->rkcg_target_assignment);
        }

        return RD_KAFKA_OP_RES_HANDLED;
}

static rd_kafka_topic_partition_list_t *
rd_kafka_cgrp_consumer_assignment_with_metadata(
    rd_kafka_cgrp_t *rkcg,
    rd_kafka_topic_partition_list_t *assignment,
    rd_list_t **missing_topic_ids) {
        int i;
        rd_kafka_t *rk = rkcg->rkcg_rk;
        rd_kafka_topic_partition_list_t *assignment_with_metadata =
            rd_kafka_topic_partition_list_new(assignment->cnt);
        for (i = 0; i < assignment->cnt; i++) {
                struct rd_kafka_metadata_cache_entry *rkmce;
                rd_kafka_topic_partition_t *rktpar;
                char *topic_name = NULL;
                rd_kafka_Uuid_t request_topic_id =
                    rd_kafka_topic_partition_get_topic_id(
                        &assignment->elems[i]);

                rd_kafka_rdlock(rk);
                rkmce =
                    rd_kafka_metadata_cache_find_by_id(rk, request_topic_id, 1);

                if (rkmce)
                        topic_name = rd_strdup(rkmce->rkmce_mtopic.topic);
                rd_kafka_rdunlock(rk);

                if (unlikely(!topic_name)) {
                        rktpar = rd_kafka_topic_partition_list_find_topic_by_id(
                            rkcg->rkcg_current_assignment, request_topic_id);
                        if (rktpar)
                                topic_name = rd_strdup(rktpar->topic);
                }

                if (likely(topic_name != NULL)) {
                        rd_kafka_topic_partition_list_add_with_topic_name_and_id(
                            assignment_with_metadata, request_topic_id,
                            topic_name, assignment->elems[i].partition);
                        rd_free(topic_name);
                        continue;
                }

                if (missing_topic_ids) {
                        if (unlikely(!*missing_topic_ids))
                                *missing_topic_ids =
                                    rd_list_new(1, rd_list_Uuid_destroy);
                        rd_list_add(*missing_topic_ids,
                                    rd_kafka_Uuid_copy(&request_topic_id));
                }
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "HEARTBEAT",
                             "Metadata not found for the "
                             "assigned topic id: %s."
                             " Continuing without it",
                             rd_kafka_Uuid_base64str(&request_topic_id));
        }
        if (missing_topic_ids && *missing_topic_ids)
                rd_list_deduplicate(missing_topic_ids,
                                    (void *)rd_kafka_Uuid_ptr_cmp);
        return assignment_with_metadata;
}

/**
 * @brief Op callback from handle_JoinGroup
 */
static rd_kafka_op_res_t
rd_kafka_cgrp_consumer_handle_Metadata_op(rd_kafka_t *rk,
                                          rd_kafka_q_t *rkq,
                                          rd_kafka_op_t *rko) {
        rd_kafka_cgrp_t *rkcg = rk->rk_cgrp;
        rd_kafka_op_res_t assignment_handle_ret;
        rd_kafka_topic_partition_list_t *assignment_with_metadata;
        rd_bool_t all_partition_metadata_available;

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED; /* Terminating */

        if (!rkcg->rkcg_next_target_assignment)
                return RD_KAFKA_OP_RES_HANDLED;

        assignment_with_metadata =
            rd_kafka_cgrp_consumer_assignment_with_metadata(
                rkcg, rkcg->rkcg_next_target_assignment, NULL);

        all_partition_metadata_available =
            assignment_with_metadata->cnt ==
                    rkcg->rkcg_next_target_assignment->cnt
                ? rd_true
                : rd_false;

        if (rd_kafka_is_dbg(rkcg->rkcg_rk, CGRP)) {
                char assignment_with_metadata_str[512] = "NULL";

                rd_kafka_topic_partition_list_str(
                    assignment_with_metadata, assignment_with_metadata_str,
                    sizeof(assignment_with_metadata_str), 0);

                rd_kafka_dbg(
                    rkcg->rkcg_rk, CGRP, "HEARTBEAT",
                    "Metadata available for %d/%d of next target assignment, "
                    " which is: \"%s\"",
                    assignment_with_metadata->cnt,
                    rkcg->rkcg_next_target_assignment->cnt,
                    assignment_with_metadata_str);
        }

        assignment_handle_ret = rd_kafka_cgrp_consumer_handle_next_assignment(
            rkcg, assignment_with_metadata, all_partition_metadata_available);
        rd_kafka_topic_partition_list_destroy(assignment_with_metadata);
        return assignment_handle_ret;
}

void rd_kafka_cgrp_consumer_next_target_assignment_request_metadata(
    rd_kafka_t *rk,
    rd_kafka_broker_t *rkb) {
        rd_kafka_topic_partition_list_t *assignment_with_metadata;
        rd_kafka_op_t *rko;
        rd_kafka_cgrp_t *rkcg        = rk->rk_cgrp;
        rd_list_t *missing_topic_ids = NULL;

        if (!rkcg->rkcg_next_target_assignment->cnt) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "HEARTBEAT",
                             "No metadata to request, continuing");
                rd_kafka_topic_partition_list_t *new_target_assignment =
                    rd_kafka_topic_partition_list_new(0);
                rd_kafka_cgrp_consumer_handle_next_assignment(
                    rkcg, new_target_assignment, rd_true);
                rd_kafka_topic_partition_list_destroy(new_target_assignment);
                return;
        }


        assignment_with_metadata =
            rd_kafka_cgrp_consumer_assignment_with_metadata(
                rkcg, rkcg->rkcg_next_target_assignment, &missing_topic_ids);

        if (!missing_topic_ids) {
                /* Metadata is already available for all the topics. */
                rd_kafka_cgrp_consumer_handle_next_assignment(
                    rkcg, assignment_with_metadata, rd_true);
                rd_kafka_topic_partition_list_destroy(assignment_with_metadata);
                return;
        }
        rd_kafka_topic_partition_list_destroy(assignment_with_metadata);

        /* Request missing metadata. */
        rko = rd_kafka_op_new_cb(rkcg->rkcg_rk, RD_KAFKA_OP_METADATA,
                                 rd_kafka_cgrp_consumer_handle_Metadata_op);
        rd_kafka_op_set_replyq(rko, rkcg->rkcg_ops, NULL);
        rd_kafka_MetadataRequest(
            rkb, NULL, missing_topic_ids, "ConsumerGroupHeartbeat API Response",
            rd_false /*!allow_auto_create*/, rd_false,
            -1 /* no subscription version is used */, rd_false, rko);
        rd_list_destroy(missing_topic_ids);
}

/**
 * @brief Handle Heartbeat response.
 */
void rd_kafka_cgrp_handle_ConsumerGroupHeartbeat(rd_kafka_t *rk,
                                                 rd_kafka_broker_t *rkb,
                                                 rd_kafka_resp_err_t err,
                                                 rd_kafka_buf_t *rkbuf,
                                                 rd_kafka_buf_t *request,
                                                 void *opaque) {
        rd_kafka_cgrp_t *rkcg       = rk->rk_cgrp;
        const int log_decode_errors = LOG_ERR;
        int16_t error_code          = 0;
        int actions                 = 0;
        rd_kafkap_str_t error_str   = RD_KAFKAP_STR_INITIALIZER_EMPTY;
        rd_kafkap_str_t member_id;
        int32_t member_epoch;
        int32_t heartbeat_interval_ms;

        if (err == RD_KAFKA_RESP_ERR__DESTROY)
                return;

        rd_dassert(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT);

        if (rd_kafka_cgrp_will_leave(rkcg))
                err = RD_KAFKA_RESP_ERR__OUTDATED;
        if (err)
                goto err;

        rd_kafka_buf_read_throttle_time(rkbuf);

        rd_kafka_buf_read_i16(rkbuf, &error_code);
        rd_kafka_buf_read_str(rkbuf, &error_str);

        if (error_code) {
                err = error_code;
                goto err;
        }

        rd_kafka_buf_read_str(rkbuf, &member_id);
        if (!RD_KAFKAP_STR_IS_NULL(&member_id)) {
                rd_kafka_cgrp_set_member_id(rkcg, member_id.str);
        }

        rd_kafka_buf_read_i32(rkbuf, &member_epoch);
        rd_kafka_buf_read_i32(rkbuf, &heartbeat_interval_ms);

        int8_t are_assignments_present;
        rd_kafka_buf_read_i8(rkbuf, &are_assignments_present);
        rkcg->rkcg_generation_id = member_epoch;
        if (heartbeat_interval_ms > 0) {
                rkcg->rkcg_heartbeat_intvl_ms = heartbeat_interval_ms;
        }

        if (are_assignments_present == 1) {
                rd_kafka_topic_partition_list_t *assigned_topic_partitions;
                const rd_kafka_topic_partition_field_t assignments_fields[] = {
                    RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
                    RD_KAFKA_TOPIC_PARTITION_FIELD_END};
                assigned_topic_partitions = rd_kafka_buf_read_topic_partitions(
                    rkbuf, rd_true, rd_false /* Don't use Topic Name */, 0,
                    assignments_fields);

                if (rd_kafka_is_dbg(rk, CGRP)) {
                        char assigned_topic_partitions_str[512] = "NULL";

                        if (assigned_topic_partitions) {
                                rd_kafka_topic_partition_list_str(
                                    assigned_topic_partitions,
                                    assigned_topic_partitions_str,
                                    sizeof(assigned_topic_partitions_str), 0);
                        }

                        rd_kafka_dbg(
                            rk, CGRP, "HEARTBEAT",
                            "ConsumerGroupHeartbeat response received target "
                            "assignment \"%s\"",
                            assigned_topic_partitions_str);
                }

                if (assigned_topic_partitions) {
                        RD_IF_FREE(rkcg->rkcg_next_target_assignment,
                                   rd_kafka_topic_partition_list_destroy);
                        rkcg->rkcg_next_target_assignment = NULL;
                        if (rd_kafka_cgrp_consumer_is_new_assignment_different(
                                rkcg, assigned_topic_partitions)) {
                                rkcg->rkcg_next_target_assignment =
                                    assigned_topic_partitions;
                        } else {
                                rd_kafka_topic_partition_list_destroy(
                                    assigned_topic_partitions);
                                assigned_topic_partitions = NULL;
                        }
                }
        }

        if (rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_STEADY &&
            (rkcg->rkcg_consumer_flags & RD_KAFKA_CGRP_CONSUMER_F_WAIT_ACK) &&
            rkcg->rkcg_target_assignment) {
                if (rkcg->rkcg_consumer_flags &
                    RD_KAFKA_CGRP_CONSUMER_F_SENDING_ACK) {
                        if (rkcg->rkcg_current_assignment)
                                rd_kafka_topic_partition_list_destroy(
                                    rkcg->rkcg_current_assignment);
                        rkcg->rkcg_current_assignment =
                            rd_kafka_topic_partition_list_copy(
                                rkcg->rkcg_target_assignment);
                        rd_kafka_topic_partition_list_destroy(
                            rkcg->rkcg_target_assignment);
                        rkcg->rkcg_target_assignment = NULL;
                        rkcg->rkcg_consumer_flags &=
                            ~RD_KAFKA_CGRP_CONSUMER_F_WAIT_ACK;

                        if (rd_kafka_is_dbg(rkcg->rkcg_rk, CGRP)) {
                                char rkcg_current_assignment_str[512] = "NULL";

                                rd_kafka_topic_partition_list_str(
                                    rkcg->rkcg_current_assignment,
                                    rkcg_current_assignment_str,
                                    sizeof(rkcg_current_assignment_str), 0);

                                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "HEARTBEAT",
                                             "Target assignment acked, new "
                                             "current assignment "
                                             " \"%s\"",
                                             rkcg_current_assignment_str);
                        }
                } else if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_SUBSCRIPTION) {
                        /* We've finished reconciliation but we weren't
                         * sending an ack, need to send a new HB with the ack.
                         */
                        rd_kafka_cgrp_consumer_expedite_next_heartbeat(
                            rkcg, "not subscribed anymore");
                }
        }

        if (rkcg->rkcg_consumer_flags &
                RD_KAFKA_CGRP_CONSUMER_F_SERVE_PENDING &&
            rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_STEADY) {
                /* TODO: Check if this should be done only for the steady state?
                 */
                rd_kafka_assignment_serve(rk);
                rkcg->rkcg_consumer_flags &=
                    ~RD_KAFKA_CGRP_CONSUMER_F_SERVE_PENDING;
        }

        if (rkcg->rkcg_next_target_assignment) {
                if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_SUBSCRIPTION) {
                        rd_kafka_cgrp_consumer_next_target_assignment_request_metadata(
                            rk, rkb);
                } else {
                        /* Consumer left the group sending an HB request
                         * while this one was in-flight. */
                        rd_kafka_topic_partition_list_destroy(
                            rkcg->rkcg_next_target_assignment);
                        rkcg->rkcg_next_target_assignment = NULL;
                }
        }

        if (rd_kafka_cgrp_consumer_subscription_preconditions_met(rkcg))
                rd_kafka_cgrp_consumer_expedite_next_heartbeat(
                    rkcg, "send new subscription");

        rkcg->rkcg_flags &= ~RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT;
        rkcg->rkcg_consumer_flags &=
            ~RD_KAFKA_CGRP_CONSUMER_F_SENDING_NEW_SUBSCRIPTION &
            ~RD_KAFKA_CGRP_CONSUMER_F_SEND_FULL_REQUEST &
            ~RD_KAFKA_CGRP_CONSUMER_F_SENDING_ACK;
        rd_kafka_cgrp_maybe_clear_heartbeat_failed_err(rkcg);
        rkcg->rkcg_last_heartbeat_err         = RD_KAFKA_RESP_ERR_NO_ERROR;
        rkcg->rkcg_expedite_heartbeat_retries = 0;

        return;


err_parse:
        err = rkbuf->rkbuf_err;

err:
        rkcg->rkcg_last_heartbeat_err = err;
        rkcg->rkcg_flags &= ~RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT;

        switch (err) {
        case RD_KAFKA_RESP_ERR__DESTROY:
                /* quick cleanup */
                return;

        case RD_KAFKA_RESP_ERR_COORDINATOR_LOAD_IN_PROGRESS:
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CONSUMER, "HEARTBEAT",
                    "ConsumerGroupHeartbeat failed due to coordinator (%s) "
                    "loading in progress: %s: "
                    "retrying",
                    rkcg->rkcg_curr_coord
                        ? rd_kafka_broker_name(rkcg->rkcg_curr_coord)
                        : "none",
                    rd_kafka_err2str(err));
                actions = RD_KAFKA_ERR_ACTION_RETRY;
                break;

        case RD_KAFKA_RESP_ERR_NOT_COORDINATOR_FOR_GROUP:
        case RD_KAFKA_RESP_ERR_GROUP_COORDINATOR_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR__TRANSPORT:
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CONSUMER, "HEARTBEAT",
                    "ConsumerGroupHeartbeat failed due to coordinator (%s) "
                    "no longer available: %s: "
                    "re-querying for coordinator",
                    rkcg->rkcg_curr_coord
                        ? rd_kafka_broker_name(rkcg->rkcg_curr_coord)
                        : "none",
                    rd_kafka_err2str(err));
                /* Remain in joined state and keep querying for coordinator */
                actions = RD_KAFKA_ERR_ACTION_REFRESH;
                break;

        case RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID:
        case RD_KAFKA_RESP_ERR_FENCED_MEMBER_EPOCH:
                rd_kafka_dbg(rkcg->rkcg_rk, CONSUMER, "HEARTBEAT",
                             "ConsumerGroupHeartbeat failed due to: %s: "
                             "will rejoin the group",
                             rd_kafka_err2str(err));
                rkcg->rkcg_consumer_flags |=
                    RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN;
                return;

        case RD_KAFKA_RESP_ERR_INVALID_REQUEST:
        case RD_KAFKA_RESP_ERR_GROUP_MAX_SIZE_REACHED:
        case RD_KAFKA_RESP_ERR_UNSUPPORTED_ASSIGNOR:
        case RD_KAFKA_RESP_ERR_UNSUPPORTED_VERSION:
        case RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE:
        case RD_KAFKA_RESP_ERR_UNRELEASED_INSTANCE_ID:
        case RD_KAFKA_RESP_ERR_GROUP_AUTHORIZATION_FAILED:
                actions = RD_KAFKA_ERR_ACTION_FATAL;
                break;
        default:
                actions = rd_kafka_err_action(
                    rkb, err, request,

                    RD_KAFKA_ERR_ACTION_SPECIAL,
                    RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED,

                    RD_KAFKA_ERR_ACTION_END);
                break;
        }

        if (actions & RD_KAFKA_ERR_ACTION_FATAL) {
                rd_kafka_set_fatal_error(
                    rkcg->rkcg_rk, err,
                    "ConsumerGroupHeartbeat fatal error: %s",
                    rd_kafka_err2str(err));
                rd_kafka_cgrp_revoke_all_rejoin_maybe(
                    rkcg, rd_true, /*assignments lost*/
                    rd_true,       /*initiating*/
                    "Fatal error in ConsumerGroupHeartbeat API response");
                return;
        }

        if (!rkcg->rkcg_heartbeat_intvl_ms) {
                /* When an error happens on first HB, it should be always
                 * retried, unless fatal, to avoid entering a tight loop
                 * and to use exponential backoff. */
                actions |= RD_KAFKA_ERR_ACTION_RETRY;
        }

        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                /* Re-query for coordinator */
                rkcg->rkcg_consumer_flags |=
                    RD_KAFKA_CGRP_CONSUMER_F_SEND_FULL_REQUEST;
                rd_kafka_cgrp_coord_query(rkcg, rd_kafka_err2str(err));
                /* If coordinator changes, HB will be expedited. */
        }

        if (actions & RD_KAFKA_ERR_ACTION_SPECIAL) {
                rd_ts_t min_error_interval =
                    RD_MAX(rkcg->rkcg_heartbeat_intvl_ms * 1000,
                           /* default group.consumer.heartbeat.interval.ms */
                           5000000);
                if (rkcg->rkcg_last_err != err ||
                    (rd_clock() >
                     rkcg->rkcg_ts_last_err + min_error_interval)) {
                        rd_kafka_cgrp_set_last_err(rkcg, err);
                        rd_kafka_consumer_err(
                            rkcg->rkcg_q, rd_kafka_broker_id(rkb), err, 0, NULL,
                            NULL, err,
                            "ConsumerGroupHeartbeat failed: %s%s%.*s",
                            rd_kafka_err2str(err),
                            RD_KAFKAP_STR_LEN(&error_str) ? ": " : "",
                            RD_KAFKAP_STR_PR(&error_str));
                }
        }

        if (actions & RD_KAFKA_ERR_ACTION_RETRY &&
            rkcg->rkcg_flags & RD_KAFKA_CGRP_F_SUBSCRIPTION &&
            !rd_kafka_cgrp_will_leave(rkcg) &&
            rd_kafka_buf_retry(rkb, request)) {
                /* Retry */
                rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT;
        }
}


/**
 * @brief Handle Heartbeat response.
 */
void rd_kafka_cgrp_handle_Heartbeat(rd_kafka_t *rk,
                                    rd_kafka_broker_t *rkb,
                                    rd_kafka_resp_err_t err,
                                    rd_kafka_buf_t *rkbuf,
                                    rd_kafka_buf_t *request,
                                    void *opaque) {
        rd_kafka_cgrp_t *rkcg       = rk->rk_cgrp;
        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode           = 0;
        int actions                 = 0;

        if (err == RD_KAFKA_RESP_ERR__DESTROY)
                return;

        rd_dassert(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT);
        rkcg->rkcg_flags &= ~RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT;

        rkcg->rkcg_last_heartbeat_err = RD_KAFKA_RESP_ERR_NO_ERROR;

        if (err)
                goto err;

        if (request->rkbuf_reqhdr.ApiVersion >= 1)
                rd_kafka_buf_read_throttle_time(rkbuf);

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);
        if (ErrorCode) {
                err = ErrorCode;
                goto err;
        }

        rd_kafka_cgrp_update_session_timeout(
            rkcg, rd_false /*don't update if session has expired*/);

        return;

err_parse:
        err = rkbuf->rkbuf_err;
err:
        rkcg->rkcg_last_heartbeat_err = err;

        rd_kafka_dbg(
            rkcg->rkcg_rk, CGRP, "HEARTBEAT",
            "Group \"%s\" heartbeat error response in "
            "state %s (join-state %s, %d partition(s) assigned): %s",
            rkcg->rkcg_group_id->str,
            rd_kafka_cgrp_state_names[rkcg->rkcg_state],
            rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
            rkcg->rkcg_group_assignment ? rkcg->rkcg_group_assignment->cnt : 0,
            rd_kafka_err2str(err));

        if (rkcg->rkcg_join_state <= RD_KAFKA_CGRP_JOIN_STATE_WAIT_SYNC) {
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CGRP, "HEARTBEAT",
                    "Heartbeat response: discarding outdated "
                    "request (now in join-state %s)",
                    rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);
                return;
        }

        switch (err) {
        case RD_KAFKA_RESP_ERR__DESTROY:
                /* quick cleanup */
                return;

        case RD_KAFKA_RESP_ERR_NOT_COORDINATOR_FOR_GROUP:
        case RD_KAFKA_RESP_ERR_GROUP_COORDINATOR_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR__TRANSPORT:
                rd_kafka_dbg(rkcg->rkcg_rk, CONSUMER, "HEARTBEAT",
                             "Heartbeat failed due to coordinator (%s) "
                             "no longer available: %s: "
                             "re-querying for coordinator",
                             rkcg->rkcg_curr_coord
                                 ? rd_kafka_broker_name(rkcg->rkcg_curr_coord)
                                 : "none",
                             rd_kafka_err2str(err));
                /* Remain in joined state and keep querying for coordinator */
                actions = RD_KAFKA_ERR_ACTION_REFRESH;
                break;

        case RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS:
                rd_kafka_cgrp_update_session_timeout(
                    rkcg, rd_false /*don't update if session has expired*/);
                /* No further action if already rebalancing */
                if (RD_KAFKA_CGRP_WAIT_ASSIGN_CALL(rkcg))
                        return;
                rd_kafka_cgrp_group_is_rebalancing(rkcg);
                return;

        case RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID:
                rd_kafka_cgrp_set_member_id(rkcg, "");
                rd_kafka_cgrp_revoke_all_rejoin_maybe(rkcg, rd_true /*lost*/,
                                                      rd_true /*initiating*/,
                                                      "resetting member-id");
                return;

        case RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION:
                rkcg->rkcg_generation_id = -1;
                rd_kafka_cgrp_revoke_all_rejoin_maybe(rkcg, rd_true /*lost*/,
                                                      rd_true /*initiating*/,
                                                      "illegal generation");
                return;

        case RD_KAFKA_RESP_ERR_FENCED_INSTANCE_ID:
                rd_kafka_set_fatal_error(rkcg->rkcg_rk, err,
                                         "Fatal consumer error: %s",
                                         rd_kafka_err2str(err));
                rd_kafka_cgrp_revoke_all_rejoin_maybe(
                    rkcg, rd_true, /*assignment lost*/
                    rd_true,       /*initiating*/
                    "consumer fenced by "
                    "newer instance");
                return;

        default:
                actions = rd_kafka_err_action(rkb, err, request,
                                              RD_KAFKA_ERR_ACTION_END);
                break;
        }


        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                /* Re-query for coordinator */
                rd_kafka_cgrp_coord_query(rkcg, rd_kafka_err2str(err));
        }

        if (actions & RD_KAFKA_ERR_ACTION_RETRY &&
            rd_kafka_buf_retry(rkb, request)) {
                /* Retry */
                rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT;
                return;
        }
}



/**
 * @brief Send Heartbeat
 */
static void rd_kafka_cgrp_heartbeat(rd_kafka_cgrp_t *rkcg) {
        /* Don't send heartbeats if max.poll.interval.ms was exceeded */
        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_MAX_POLL_EXCEEDED)
                return;

        /* Skip heartbeat if we have one in transit */
        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT)
                return;

        rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT;
        rd_kafka_HeartbeatRequest(
            rkcg->rkcg_coord, rkcg->rkcg_group_id, rkcg->rkcg_generation_id,
            rkcg->rkcg_member_id, rkcg->rkcg_group_instance_id,
            RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0), rd_kafka_cgrp_handle_Heartbeat,
            NULL);
}

/**
 * Cgrp is now terminated: decommission it and signal back to application.
 */
static void rd_kafka_cgrp_terminated(rd_kafka_cgrp_t *rkcg) {
        if (rd_atomic32_get(&rkcg->rkcg_terminated))
                return; /* terminated() may be called multiple times,
                         * make sure to only terminate once. */

        rd_kafka_cgrp_group_assignment_set(rkcg, NULL);

        rd_kafka_assert(NULL, !rd_kafka_assignment_in_progress(rkcg->rkcg_rk));
        rd_kafka_assert(NULL, !rkcg->rkcg_group_assignment);
        rd_kafka_assert(NULL, rkcg->rkcg_rk->rk_consumer.wait_commit_cnt == 0);
        rd_kafka_assert(NULL, rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_TERM);

        rd_kafka_timer_stop(&rkcg->rkcg_rk->rk_timers,
                            &rkcg->rkcg_offset_commit_tmr, 1 /*lock*/);

        rd_kafka_q_purge(rkcg->rkcg_wait_coord_q);

        /* Disable ops queue since there will be no
         * (broker) thread serving it anymore after the unassign_broker
         * below.
         * As queue is forwarded to rk_ops, it cannot be purged,
         * so consumer group operation need to be served with a no-op
         * when `rkcg_terminated` is true. */

        rd_atomic32_set(&rkcg->rkcg_terminated, rd_true);
        rd_kafka_q_disable(rkcg->rkcg_ops);

        if (rkcg->rkcg_curr_coord)
                rd_kafka_cgrp_coord_clear_broker(rkcg);

        if (rkcg->rkcg_coord) {
                rd_kafka_broker_destroy(rkcg->rkcg_coord);
                rkcg->rkcg_coord = NULL;
        }

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPTERM",
                     "Consumer group sub-system terminated%s",
                     rkcg->rkcg_reply_rko ? " (will enqueue reply)" : "");

        if (rkcg->rkcg_reply_rko) {
                /* Signal back to application. */
                rd_kafka_replyq_enq(&rkcg->rkcg_reply_rko->rko_replyq,
                                    rkcg->rkcg_reply_rko, 0);
                rkcg->rkcg_reply_rko = NULL;
        }

        /* Remove cgrp application queue forwarding, if any. */
        rd_kafka_q_fwd_set(rkcg->rkcg_q, NULL);

        /* Destroy KIP-848 consumer group structures */
        rd_kafka_cgrp_consumer_reset(rkcg);
}


/**
 * If a cgrp is terminating and all outstanding ops are now finished
 * then progress to final termination and return 1.
 * Else returns 0.
 */
static RD_INLINE int rd_kafka_cgrp_try_terminate(rd_kafka_cgrp_t *rkcg) {

        if (rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_TERM)
                return 1;

        if (likely(!(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE)))
                return 0;

        /* Check if wait-coord queue has timed out.

           FIXME: Remove usage of `group_session_timeout_ms` for the new
                  consumer group protocol implementation defined in KIP-848.
        */
        if (rd_kafka_q_len(rkcg->rkcg_wait_coord_q) > 0 &&
            rkcg->rkcg_ts_terminate +
                    (rkcg->rkcg_rk->rk_conf.group_session_timeout_ms * 1000) <
                rd_clock()) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPTERM",
                             "Group \"%s\": timing out %d op(s) in "
                             "wait-for-coordinator queue",
                             rkcg->rkcg_group_id->str,
                             rd_kafka_q_len(rkcg->rkcg_wait_coord_q));
                rd_kafka_q_disable(rkcg->rkcg_wait_coord_q);
                if (rd_kafka_q_concat(rkcg->rkcg_ops,
                                      rkcg->rkcg_wait_coord_q) == -1) {
                        /* ops queue shut down, purge coord queue */
                        rd_kafka_q_purge(rkcg->rkcg_wait_coord_q);
                }
        }

        if (!RD_KAFKA_CGRP_WAIT_ASSIGN_CALL(rkcg) &&
            rd_list_empty(&rkcg->rkcg_toppars) &&
            !rd_kafka_assignment_in_progress(rkcg->rkcg_rk) &&
            rkcg->rkcg_rk->rk_consumer.wait_commit_cnt == 0 &&
            !(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WAIT_LEAVE)) {
                /* Since we might be deep down in a 'rko' handler
                 * called from cgrp_op_serve() we cant call terminated()
                 * directly since it will decommission the rkcg_ops queue
                 * that might be locked by intermediate functions.
                 * Instead set the TERM state and let the cgrp terminate
                 * at its own discretion. */
                rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_TERM);

                return 1;
        } else {
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CGRP, "CGRPTERM",
                    "Group \"%s\": "
                    "waiting for %s%d toppar(s), "
                    "%s"
                    "%d commit(s)%s%s%s (state %s, join-state %s) "
                    "before terminating",
                    rkcg->rkcg_group_id->str,
                    RD_KAFKA_CGRP_WAIT_ASSIGN_CALL(rkcg) ? "assign call, " : "",
                    rd_list_cnt(&rkcg->rkcg_toppars),
                    rd_kafka_assignment_in_progress(rkcg->rkcg_rk)
                        ? "assignment in progress, "
                        : "",
                    rkcg->rkcg_rk->rk_consumer.wait_commit_cnt,
                    (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WAIT_LEAVE)
                        ? ", wait-leave,"
                        : "",
                    rkcg->rkcg_rebalance_rejoin ? ", rebalance_rejoin," : "",
                    (rkcg->rkcg_rebalance_incr_assignment != NULL)
                        ? ", rebalance_incr_assignment,"
                        : "",
                    rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                    rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);
                return 0;
        }
}


/**
 * @brief Add partition to this cgrp management
 *
 * @locks none
 */
static void rd_kafka_cgrp_partition_add(rd_kafka_cgrp_t *rkcg,
                                        rd_kafka_toppar_t *rktp) {
        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "PARTADD",
                     "Group \"%s\": add %s [%" PRId32 "]",
                     rkcg->rkcg_group_id->str, rktp->rktp_rkt->rkt_topic->str,
                     rktp->rktp_partition);

        rd_kafka_toppar_lock(rktp);
        rd_assert(!(rktp->rktp_flags & RD_KAFKA_TOPPAR_F_ON_CGRP));
        rktp->rktp_flags |= RD_KAFKA_TOPPAR_F_ON_CGRP;
        rd_kafka_toppar_unlock(rktp);

        rd_kafka_toppar_keep(rktp);
        rd_list_add(&rkcg->rkcg_toppars, rktp);
}

/**
 * @brief Remove partition from this cgrp management
 *
 * @locks none
 */
static void rd_kafka_cgrp_partition_del(rd_kafka_cgrp_t *rkcg,
                                        rd_kafka_toppar_t *rktp) {
        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "PARTDEL",
                     "Group \"%s\": delete %s [%" PRId32 "]",
                     rkcg->rkcg_group_id->str, rktp->rktp_rkt->rkt_topic->str,
                     rktp->rktp_partition);

        rd_kafka_toppar_lock(rktp);
        rd_assert(rktp->rktp_flags & RD_KAFKA_TOPPAR_F_ON_CGRP);
        rktp->rktp_flags &= ~RD_KAFKA_TOPPAR_F_ON_CGRP;

        rd_kafka_toppar_purge_internal_fetch_queue_maybe(rktp);

        rd_kafka_toppar_unlock(rktp);

        rd_list_remove(&rkcg->rkcg_toppars, rktp);

        rd_kafka_toppar_destroy(rktp); /* refcnt from _add above */

        rd_kafka_cgrp_try_terminate(rkcg);
}



/**
 * @brief Defer offset commit (rko) until coordinator is available.
 *
 * @returns 1 if the rko was deferred or 0 if the defer queue is disabled
 *          or rko already deferred.
 */
static int rd_kafka_cgrp_defer_offset_commit(rd_kafka_cgrp_t *rkcg,
                                             rd_kafka_op_t *rko,
                                             const char *reason) {
        /* wait_coord_q is disabled session.timeout.ms after
         * group close() has been initated. */
        if (rko->rko_u.offset_commit.ts_timeout != 0 ||
            !rd_kafka_q_ready(rkcg->rkcg_wait_coord_q))
                return 0;

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "COMMIT",
                     "Group \"%s\": "
                     "unable to OffsetCommit in state %s: %s: "
                     "coordinator (%s) is unavailable: "
                     "retrying later",
                     rkcg->rkcg_group_id->str,
                     rd_kafka_cgrp_state_names[rkcg->rkcg_state], reason,
                     rkcg->rkcg_curr_coord
                         ? rd_kafka_broker_name(rkcg->rkcg_curr_coord)
                         : "none");

        rko->rko_flags |= RD_KAFKA_OP_F_REPROCESS;

        /* FIXME: Remove `group_session_timeout_ms` for the new protocol
         * defined in KIP-848 as this property is deprecated from client
         * side in the new protocol.
         */
        rko->rko_u.offset_commit.ts_timeout =
            rd_clock() +
            (rkcg->rkcg_rk->rk_conf.group_session_timeout_ms * 1000);
        rd_kafka_q_enq(rkcg->rkcg_wait_coord_q, rko);

        return 1;
}

/**
 * @brief Defer offset commit (rko) until coordinator is available (KIP-848).
 *
 * @returns 1 if the rko was deferred or 0 if the defer queue is disabled
 *          or rko already deferred.
 */
static int rd_kafka_cgrp_consumer_defer_offset_commit(rd_kafka_cgrp_t *rkcg,
                                                      rd_kafka_op_t *rko,
                                                      const char *reason) {
        /* wait_coord_q is disabled session.timeout.ms after
         * group close() has been initated. */
        if ((rko->rko_u.offset_commit.ts_timeout != 0 &&
             rd_clock() >= rko->rko_u.offset_commit.ts_timeout) ||
            !rd_kafka_q_ready(rkcg->rkcg_wait_coord_q))
                return 0;

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "COMMIT",
                     "Group \"%s\": "
                     "unable to OffsetCommit in state %s: %s: "
                     "retrying later",
                     rkcg->rkcg_group_id->str,
                     rd_kafka_cgrp_state_names[rkcg->rkcg_state], reason);

        rko->rko_flags |= RD_KAFKA_OP_F_REPROCESS;

        if (!rko->rko_u.offset_commit.ts_timeout) {
                rko->rko_u.offset_commit.ts_timeout =
                    rd_clock() +
                    (rkcg->rkcg_rk->rk_conf.group_session_timeout_ms * 1000);
        }

        /* Reset partition level error before retrying */
        rd_kafka_topic_partition_list_set_err(
            rko->rko_u.offset_commit.partitions, RD_KAFKA_RESP_ERR_NO_ERROR);

        rd_kafka_q_enq(rkcg->rkcg_wait_coord_q, rko);

        return 1;
}

/**
 * @brief Update the committed offsets for the partitions in \p offsets,
 *
 * @remark \p offsets may be NULL if \p err is set
 * @returns the number of partitions with errors encountered
 */
static int rd_kafka_cgrp_update_committed_offsets(
    rd_kafka_cgrp_t *rkcg,
    rd_kafka_resp_err_t err,
    rd_kafka_topic_partition_list_t *offsets) {
        int i;
        int errcnt = 0;

        /* Update toppars' committed offset or global error */
        for (i = 0; offsets && i < offsets->cnt; i++) {
                rd_kafka_topic_partition_t *rktpar = &offsets->elems[i];
                rd_kafka_toppar_t *rktp;

                /* Ignore logical offsets since they were never
                 * sent to the broker. */
                if (RD_KAFKA_OFFSET_IS_LOGICAL(rktpar->offset))
                        continue;

                /* Propagate global error to all partitions that don't have
                 * explicit error set. */
                if (err && !rktpar->err)
                        rktpar->err = err;

                if (rktpar->err) {
                        rd_kafka_dbg(rkcg->rkcg_rk, TOPIC, "OFFSET",
                                     "OffsetCommit failed for "
                                     "%s [%" PRId32
                                     "] at offset "
                                     "%" PRId64 " in join-state %s: %s",
                                     rktpar->topic, rktpar->partition,
                                     rktpar->offset,
                                     rd_kafka_cgrp_join_state_names
                                         [rkcg->rkcg_join_state],
                                     rd_kafka_err2str(rktpar->err));

                        errcnt++;
                        continue;
                }

                rktp = rd_kafka_topic_partition_get_toppar(rkcg->rkcg_rk,
                                                           rktpar, rd_false);
                if (!rktp)
                        continue;

                rd_kafka_toppar_lock(rktp);
                rktp->rktp_committed_pos =
                    rd_kafka_topic_partition_get_fetch_pos(rktpar);
                rd_kafka_toppar_unlock(rktp);

                rd_kafka_toppar_destroy(rktp); /* from get_toppar() */
        }

        return errcnt;
}


/**
 * @brief Propagate OffsetCommit results.
 *
 * @param rko_orig The original rko that triggered the commit, this is used
 *                 to propagate the result.
 * @param err Is the aggregated request-level error, or ERR_NO_ERROR.
 * @param errcnt Are the number of partitions in \p offsets that failed
 *               offset commit.
 */
static void rd_kafka_cgrp_propagate_commit_result(
    rd_kafka_cgrp_t *rkcg,
    rd_kafka_op_t *rko_orig,
    rd_kafka_resp_err_t err,
    int errcnt,
    rd_kafka_topic_partition_list_t *offsets) {

        const rd_kafka_t *rk        = rkcg->rkcg_rk;
        int offset_commit_cb_served = 0;

        /* If no special callback is set but a offset_commit_cb has
         * been set in conf then post an event for the latter. */
        if (!rko_orig->rko_u.offset_commit.cb && rk->rk_conf.offset_commit_cb) {
                rd_kafka_op_t *rko_reply = rd_kafka_op_new_reply(rko_orig, err);

                rd_kafka_op_set_prio(rko_reply, RD_KAFKA_PRIO_HIGH);

                if (offsets)
                        rko_reply->rko_u.offset_commit.partitions =
                            rd_kafka_topic_partition_list_copy(offsets);

                rko_reply->rko_u.offset_commit.cb =
                    rk->rk_conf.offset_commit_cb;
                rko_reply->rko_u.offset_commit.opaque = rk->rk_conf.opaque;

                rd_kafka_q_enq(rk->rk_rep, rko_reply);
                offset_commit_cb_served++;
        }


        /* Enqueue reply to requester's queue, if any. */
        if (rko_orig->rko_replyq.q) {
                rd_kafka_op_t *rko_reply = rd_kafka_op_new_reply(rko_orig, err);

                rd_kafka_op_set_prio(rko_reply, RD_KAFKA_PRIO_HIGH);

                /* Copy offset & partitions & callbacks to reply op */
                rko_reply->rko_u.offset_commit = rko_orig->rko_u.offset_commit;
                if (offsets)
                        rko_reply->rko_u.offset_commit.partitions =
                            rd_kafka_topic_partition_list_copy(offsets);
                if (rko_reply->rko_u.offset_commit.reason)
                        rko_reply->rko_u.offset_commit.reason =
                            rd_strdup(rko_reply->rko_u.offset_commit.reason);

                rd_kafka_replyq_enq(&rko_orig->rko_replyq, rko_reply, 0);
                offset_commit_cb_served++;
        }

        if (!offset_commit_cb_served && offsets &&
            (errcnt > 0 || (err != RD_KAFKA_RESP_ERR_NO_ERROR &&
                            err != RD_KAFKA_RESP_ERR__NO_OFFSET))) {
                /* If there is no callback or handler for this (auto)
                 * commit then log an error (#1043) */
                char tmp[512];

                rd_kafka_topic_partition_list_str(
                    offsets, tmp, sizeof(tmp),
                    /* Print per-partition errors unless there was a
                     * request-level error. */
                    RD_KAFKA_FMT_F_OFFSET |
                        (errcnt ? RD_KAFKA_FMT_F_ONLY_ERR : 0));

                rd_kafka_log(
                    rkcg->rkcg_rk, LOG_WARNING, "COMMITFAIL",
                    "Offset commit (%s) failed "
                    "for %d/%d partition(s) in join-state %s: "
                    "%s%s%s",
                    rko_orig->rko_u.offset_commit.reason,
                    errcnt ? errcnt : offsets->cnt, offsets->cnt,
                    rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
                    errcnt ? rd_kafka_err2str(err) : "", errcnt ? ": " : "",
                    tmp);
        }
}



/**
 * @brief Handle OffsetCommitResponse
 * Takes the original 'rko' as opaque argument.
 * @remark \p rkb, rkbuf, and request may be NULL in a number of
 *         error cases (e.g., _NO_OFFSET, _WAIT_COORD)
 */
static void rd_kafka_cgrp_op_handle_OffsetCommit(rd_kafka_t *rk,
                                                 rd_kafka_broker_t *rkb,
                                                 rd_kafka_resp_err_t err,
                                                 rd_kafka_buf_t *rkbuf,
                                                 rd_kafka_buf_t *request,
                                                 void *opaque) {
        rd_kafka_cgrp_t *rkcg   = rk->rk_cgrp;
        rd_kafka_op_t *rko_orig = opaque;
        rd_kafka_topic_partition_list_t *offsets =
            rko_orig->rko_u.offset_commit.partitions; /* maybe NULL */
        int errcnt;

        RD_KAFKA_OP_TYPE_ASSERT(rko_orig, RD_KAFKA_OP_OFFSET_COMMIT);

        err = rd_kafka_handle_OffsetCommit(rk, rkb, err, rkbuf, request,
                                           offsets, rd_false);

        /* Suppress empty commit debug logs if allowed */
        if (err != RD_KAFKA_RESP_ERR__NO_OFFSET ||
            !rko_orig->rko_u.offset_commit.silent_empty) {
                if (rkb)
                        rd_rkb_dbg(rkb, CGRP, "COMMIT",
                                   "OffsetCommit for %d partition(s) in "
                                   "join-state %s: "
                                   "%s: returned: %s",
                                   offsets ? offsets->cnt : -1,
                                   rd_kafka_cgrp_join_state_names
                                       [rkcg->rkcg_join_state],
                                   rko_orig->rko_u.offset_commit.reason,
                                   rd_kafka_err2str(err));
                else
                        rd_kafka_dbg(rk, CGRP, "COMMIT",
                                     "OffsetCommit for %d partition(s) in "
                                     "join-state "
                                     "%s: %s: "
                                     "returned: %s",
                                     offsets ? offsets->cnt : -1,
                                     rd_kafka_cgrp_join_state_names
                                         [rkcg->rkcg_join_state],
                                     rko_orig->rko_u.offset_commit.reason,
                                     rd_kafka_err2str(err));
        }

        /*
         * Error handling
         */
        switch (err) {
        case RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID:
                if (rkcg->rkcg_group_protocol ==
                    RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                        rd_kafka_cgrp_consumer_expedite_next_heartbeat(
                            rk->rk_cgrp, "OffsetCommit error: Unknown member");
                } else {
                        /* Revoke assignment and rebalance on unknown member */
                        rd_kafka_cgrp_set_member_id(rk->rk_cgrp, "");
                        rd_kafka_cgrp_revoke_all_rejoin_maybe(
                            rkcg, rd_true /*assignment is lost*/,
                            rd_true /*this consumer is initiating*/,
                            "OffsetCommit error: Unknown member");
                }
                break;

        case RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION:
                /* Revoke assignment and rebalance on illegal generation,
                 * only if not rebalancing, because a new generation id
                 * can be received soon after this error. */
                if (RD_KAFKA_CGRP_REBALANCING(rkcg))
                        break;

                rk->rk_cgrp->rkcg_generation_id = -1;
                rd_kafka_cgrp_revoke_all_rejoin_maybe(
                    rkcg, rd_true /*assignment is lost*/,
                    rd_true /*this consumer is initiating*/,
                    "OffsetCommit error: Illegal generation");
                break;

        case RD_KAFKA_RESP_ERR__IN_PROGRESS:
                return; /* Retrying */

        case RD_KAFKA_RESP_ERR_STALE_MEMBER_EPOCH:
                /* FIXME: Add logs.*/
                rd_kafka_cgrp_consumer_expedite_next_heartbeat(
                    rk->rk_cgrp, "OffsetCommit error: Stale member epoch");
                if (!rd_strcmp(rko_orig->rko_u.offset_commit.reason, "manual"))
                        /* Don't retry manual commits giving this error.
                         * TODO: do this in a faster and cleaner way
                         * with a bool. */
                        break;

                if (rd_kafka_cgrp_consumer_defer_offset_commit(
                        rkcg, rko_orig, rd_kafka_err2str(err)))
                        return;
                break;

        case RD_KAFKA_RESP_ERR_NOT_COORDINATOR:
        case RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR__TRANSPORT:
                /* The coordinator is not available, defer the offset commit
                 * to when the coordinator is back up again. */

                /* Future-proofing, see timeout_scan(). */
                rd_kafka_assert(NULL, err != RD_KAFKA_RESP_ERR__WAIT_COORD);

                if (rd_kafka_cgrp_defer_offset_commit(rkcg, rko_orig,
                                                      rd_kafka_err2str(err)))
                        return;
                break;

        default:
                break;
        }

        /* Call on_commit interceptors */
        if (err != RD_KAFKA_RESP_ERR__NO_OFFSET &&
            err != RD_KAFKA_RESP_ERR__DESTROY && offsets && offsets->cnt > 0)
                rd_kafka_interceptors_on_commit(rk, offsets, err);

        /* Keep track of outstanding commits */
        rd_kafka_assert(NULL, rk->rk_consumer.wait_commit_cnt > 0);
        rk->rk_consumer.wait_commit_cnt--;

        if (err == RD_KAFKA_RESP_ERR__DESTROY) {
                rd_kafka_op_destroy(rko_orig);
                return; /* Handle is terminating, this op may be handled
                         * by the op enq()ing thread rather than the
                         * rdkafka main thread, it is not safe to
                         * continue here. */
        }

        /* Update the committed offsets for each partition's rktp. */
        errcnt = rd_kafka_cgrp_update_committed_offsets(rkcg, err, offsets);

        if (err != RD_KAFKA_RESP_ERR__DESTROY &&
            !(err == RD_KAFKA_RESP_ERR__NO_OFFSET &&
              rko_orig->rko_u.offset_commit.silent_empty)) {
                /* Propagate commit results (success or permanent error)
                 * unless we're shutting down or commit was empty, or if
                 * there was a rebalance in progress. */
                rd_kafka_cgrp_propagate_commit_result(rkcg, rko_orig, err,
                                                      errcnt, offsets);
        }

        rd_kafka_op_destroy(rko_orig);

        /* If the current state was waiting for commits to finish we'll try to
         * transition to the next state. */
        if (rk->rk_consumer.wait_commit_cnt == 0)
                rd_kafka_assignment_serve(rk);
}


static size_t rd_kafka_topic_partition_has_absolute_offset(
    const rd_kafka_topic_partition_t *rktpar,
    void *opaque) {
        return rktpar->offset >= 0 ? 1 : 0;
}


/**
 * Commit a list of offsets.
 * Reuse the orignating 'rko' for the async reply.
 * 'rko->rko_payload' should either by NULL (to commit current assignment) or
 * a proper topic_partition_list_t with offsets to commit.
 * The offset list will be altered.
 *
 * \p rko...silent_empty: if there are no offsets to commit bail out
 *                        silently without posting an op on the reply queue.
 * \p set_offsets: set offsets and epochs in
 *                 rko->rko_u.offset_commit.partitions from the rktp's
 *                 stored offset.
 *
 * Locality: cgrp thread
 */
static void rd_kafka_cgrp_offsets_commit(rd_kafka_cgrp_t *rkcg,
                                         rd_kafka_op_t *rko,
                                         rd_bool_t set_offsets,
                                         const char *reason) {
        rd_kafka_topic_partition_list_t *offsets;
        rd_kafka_resp_err_t err;
        int valid_offsets = 0;
        int r;
        rd_kafka_buf_t *rkbuf;
        rd_kafka_op_t *reply;
        rd_kafka_consumer_group_metadata_t *cgmetadata;

        if (!(rko->rko_flags & RD_KAFKA_OP_F_REPROCESS)) {
                /* wait_commit_cnt has already been increased for
                 * reprocessed ops. */
                rkcg->rkcg_rk->rk_consumer.wait_commit_cnt++;
        }

        /* If offsets is NULL we shall use the current assignment
         * (not the group assignment). */
        if (!rko->rko_u.offset_commit.partitions &&
            rkcg->rkcg_rk->rk_consumer.assignment.all->cnt > 0) {
                if (rd_kafka_cgrp_assignment_is_lost(rkcg)) {
                        /* Not committing assigned offsets: assignment lost */
                        err = RD_KAFKA_RESP_ERR__ASSIGNMENT_LOST;
                        goto err;
                }

                rko->rko_u.offset_commit.partitions =
                    rd_kafka_topic_partition_list_copy(
                        rkcg->rkcg_rk->rk_consumer.assignment.all);
        }

        offsets = rko->rko_u.offset_commit.partitions;

        if (offsets) {
                /* Set offsets to commits */
                if (set_offsets)
                        rd_kafka_topic_partition_list_set_offsets(
                            rkcg->rkcg_rk, rko->rko_u.offset_commit.partitions,
                            1, RD_KAFKA_OFFSET_INVALID /* def */,
                            1 /* is commit */);

                /*  Check the number of valid offsets to commit. */
                valid_offsets = (int)rd_kafka_topic_partition_list_sum(
                    offsets, rd_kafka_topic_partition_has_absolute_offset,
                    NULL);
        }

        if (rd_kafka_fatal_error_code(rkcg->rkcg_rk)) {
                /* Commits are not allowed when a fatal error has been raised */
                err = RD_KAFKA_RESP_ERR__FATAL;
                goto err;
        }

        if (!valid_offsets) {
                /* No valid offsets */
                err = RD_KAFKA_RESP_ERR__NO_OFFSET;
                goto err;
        }

        if (rkcg->rkcg_state != RD_KAFKA_CGRP_STATE_UP) {
                rd_kafka_dbg(rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP,
                             "COMMIT",
                             "Deferring \"%s\" offset commit "
                             "for %d partition(s) in state %s: "
                             "no coordinator available",
                             reason, valid_offsets,
                             rd_kafka_cgrp_state_names[rkcg->rkcg_state]);

                if (rd_kafka_cgrp_defer_offset_commit(rkcg, rko, reason))
                        return;

                err = RD_KAFKA_RESP_ERR__WAIT_COORD;
                goto err;
        }


        rd_rkb_dbg(rkcg->rkcg_coord, CONSUMER | RD_KAFKA_DBG_CGRP, "COMMIT",
                   "Committing offsets for %d partition(s) with "
                   "generation-id %" PRId32 " in join-state %s: %s",
                   valid_offsets, rkcg->rkcg_generation_id,
                   rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
                   reason);

        cgmetadata = rd_kafka_consumer_group_metadata_new_with_genid(
            rkcg->rkcg_rk->rk_conf.group_id_str, rkcg->rkcg_generation_id,
            rkcg->rkcg_member_id->str,
            rkcg->rkcg_rk->rk_conf.group_instance_id);

        /* Send OffsetCommit */
        r = rd_kafka_OffsetCommitRequest(rkcg->rkcg_coord, cgmetadata, offsets,
                                         RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
                                         rd_kafka_cgrp_op_handle_OffsetCommit,
                                         rko, reason);
        rd_kafka_consumer_group_metadata_destroy(cgmetadata);

        /* Must have valid offsets to commit if we get here */
        rd_kafka_assert(NULL, r != 0);

        return;

err:
        if (err != RD_KAFKA_RESP_ERR__NO_OFFSET)
                rd_kafka_dbg(rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP,
                             "COMMIT", "OffsetCommit internal error: %s",
                             rd_kafka_err2str(err));

        /* Propagate error through dummy buffer object that will
         * call the response handler from the main loop, avoiding
         * any recursive calls from op_handle_OffsetCommit ->
         * assignment_serve() and then back to cgrp_assigned_offsets_commit() */

        reply         = rd_kafka_op_new(RD_KAFKA_OP_RECV_BUF);
        reply->rko_rk = rkcg->rkcg_rk; /* Set rk since the rkbuf will not
                                        * have a rkb to reach it. */
        reply->rko_err = err;

        rkbuf                   = rd_kafka_buf_new(0, 0);
        rkbuf->rkbuf_cb         = rd_kafka_cgrp_op_handle_OffsetCommit;
        rkbuf->rkbuf_opaque     = rko;
        reply->rko_u.xbuf.rkbuf = rkbuf;

        rd_kafka_q_enq(rkcg->rkcg_ops, reply);
}


/**
 * @brief Commit offsets assigned partitions.
 *
 * If \p offsets is NULL all partitions in the current assignment will be used.
 * If \p set_offsets is true the offsets to commit will be read from the
 * rktp's stored offset rather than the .offset fields in \p offsets.
 *
 * rkcg_wait_commit_cnt will be increased accordingly.
 */
void rd_kafka_cgrp_assigned_offsets_commit(
    rd_kafka_cgrp_t *rkcg,
    const rd_kafka_topic_partition_list_t *offsets,
    rd_bool_t set_offsets,
    const char *reason) {
        rd_kafka_op_t *rko;

        if (rd_kafka_cgrp_assignment_is_lost(rkcg)) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "AUTOCOMMIT",
                             "Group \"%s\": not committing assigned offsets: "
                             "assignment lost",
                             rkcg->rkcg_group_id->str);
                return;
        }

        rko = rd_kafka_op_new(RD_KAFKA_OP_OFFSET_COMMIT);
        rko->rko_u.offset_commit.reason = rd_strdup(reason);
        if (rkcg->rkcg_rk->rk_conf.enabled_events &
            RD_KAFKA_EVENT_OFFSET_COMMIT) {
                /* Send results to application */
                rd_kafka_op_set_replyq(rko, rkcg->rkcg_rk->rk_rep, 0);
                rko->rko_u.offset_commit.cb =
                    rkcg->rkcg_rk->rk_conf.offset_commit_cb; /*maybe NULL*/
                rko->rko_u.offset_commit.opaque = rkcg->rkcg_rk->rk_conf.opaque;
        }
        /* NULL partitions means current assignment */
        if (offsets)
                rko->rko_u.offset_commit.partitions =
                    rd_kafka_topic_partition_list_copy(offsets);
        rko->rko_u.offset_commit.silent_empty = 1;
        rd_kafka_cgrp_offsets_commit(rkcg, rko, set_offsets, reason);
}


/**
 * auto.commit.interval.ms commit timer callback.
 *
 * Trigger a group offset commit.
 *
 * Locality: rdkafka main thread
 */
static void rd_kafka_cgrp_offset_commit_tmr_cb(rd_kafka_timers_t *rkts,
                                               void *arg) {
        rd_kafka_cgrp_t *rkcg = arg;

        /* Don't attempt auto commit when rebalancing or initializing since
         * the rkcg_generation_id is most likely in flux. */
        if (rkcg->rkcg_subscription &&
            rkcg->rkcg_join_state != RD_KAFKA_CGRP_JOIN_STATE_STEADY)
                return;

        rd_kafka_cgrp_assigned_offsets_commit(
            rkcg, NULL, rd_true /*set offsets*/, "cgrp auto commit timer");
}


/**
 * @brief If rkcg_next_subscription or rkcg_next_unsubscribe are
 *        set, trigger a state change so that they are applied from the
 *        main dispatcher.
 *
 * @returns rd_true if a subscribe was scheduled, else false.
 */
static rd_bool_t
rd_kafka_trigger_waiting_subscribe_maybe(rd_kafka_cgrp_t *rkcg) {

        if (rkcg->rkcg_next_subscription || rkcg->rkcg_next_unsubscribe) {
                /* Skip the join backoff */
                rd_interval_reset(&rkcg->rkcg_join_intvl);
                rd_kafka_cgrp_rejoin(rkcg, "Applying next subscription");
                return rd_true;
        }

        return rd_false;
}

static void rd_kafka_cgrp_start_max_poll_interval_timer(rd_kafka_cgrp_t *rkcg) {
        /* If using subscribe(), start a timer to enforce
         * `max.poll.interval.ms`.
         * Instead of restarting the timer on each ...poll()
         * call, which would be costly (once per message),
         * set up an intervalled timer that checks a timestamp
         * (that is updated on ..poll()).
         * The timer interval is 2 hz. */
        rd_kafka_timer_start(
            &rkcg->rkcg_rk->rk_timers, &rkcg->rkcg_max_poll_interval_tmr,
            500 * 1000ll /* 500ms */,
            rd_kafka_cgrp_max_poll_interval_check_tmr_cb, rkcg);
}

/**
 * @brief Incrementally add to an existing partition assignment
 *        May update \p partitions but will not hold on to it.
 *
 * @returns an error object or NULL on success.
 */
static rd_kafka_error_t *
rd_kafka_cgrp_incremental_assign(rd_kafka_cgrp_t *rkcg,
                                 rd_kafka_topic_partition_list_t *partitions) {
        rd_kafka_error_t *error;

        error = rd_kafka_assignment_add(rkcg->rkcg_rk, partitions);
        if (error)
                return error;

        if (rkcg->rkcg_join_state ==
            RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_CALL) {
                rd_kafka_assignment_resume(rkcg->rkcg_rk,
                                           "incremental assign called");
                rd_kafka_cgrp_set_join_state(rkcg,
                                             RD_KAFKA_CGRP_JOIN_STATE_STEADY);
                if (rkcg->rkcg_subscription) {
                        rd_kafka_cgrp_start_max_poll_interval_timer(rkcg);
                }
        }

        rd_kafka_cgrp_assignment_clear_lost(rkcg,
                                            "incremental_assign() called");

        return NULL;
}


/**
 * @brief Incrementally remove partitions from an existing partition
 *        assignment. May update \p partitions but will not hold on
 *        to it.
 *
 * @remark This method does not unmark the current assignment as lost
 *         (if lost). That happens following _incr_unassign_done and
 *         a group-rejoin initiated.
 *
 * @returns An error object or NULL on success.
 */
static rd_kafka_error_t *rd_kafka_cgrp_incremental_unassign(
    rd_kafka_cgrp_t *rkcg,
    rd_kafka_topic_partition_list_t *partitions) {
        rd_kafka_error_t *error;

        error = rd_kafka_assignment_subtract(rkcg->rkcg_rk, partitions);
        if (error)
                return error;

        if (rkcg->rkcg_join_state ==
            RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_CALL) {
                rd_kafka_assignment_resume(rkcg->rkcg_rk,
                                           "incremental unassign called");
                rd_kafka_cgrp_set_join_state(
                    rkcg,
                    RD_KAFKA_CGRP_JOIN_STATE_WAIT_INCR_UNASSIGN_TO_COMPLETE);
        }

        rd_kafka_cgrp_assignment_clear_lost(rkcg,
                                            "incremental_unassign() called");

        return NULL;
}


/**
 * @brief Call when all incremental unassign operations are done to transition
 *        to the next state.
 */
static void rd_kafka_cgrp_incr_unassign_done(rd_kafka_cgrp_t *rkcg) {

        /* If this action was underway when a terminate was initiated, it will
         * be left to complete. Now that's done, unassign all partitions */
        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "UNASSIGN",
                             "Group \"%s\" is terminating, initiating full "
                             "unassign",
                             rkcg->rkcg_group_id->str);
                rd_kafka_cgrp_unassign(rkcg);
                return;
        }

        if (rkcg->rkcg_rebalance_incr_assignment) {

                /* This incremental unassign was part of a normal rebalance
                 * (in which the revoke set was not empty). Immediately
                 * trigger the assign that follows this revoke. The protocol
                 * dictates this should occur even if the new assignment
                 * set is empty.
                 *
                 * Also, since this rebalance had some revoked partitions,
                 * a re-join should occur following the assign.
                 */

                rd_kafka_rebalance_op_incr(rkcg,
                                           RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS,
                                           rkcg->rkcg_rebalance_incr_assignment,
                                           rd_true /*rejoin following assign*/,
                                           "cooperative assign after revoke");

                rd_kafka_topic_partition_list_destroy(
                    rkcg->rkcg_rebalance_incr_assignment);
                rkcg->rkcg_rebalance_incr_assignment = NULL;

                /* Note: rkcg_rebalance_rejoin is actioned / reset in
                 * rd_kafka_cgrp_incremental_assign call */

        } else if (rkcg->rkcg_rebalance_rejoin) {
                rkcg->rkcg_rebalance_rejoin = rd_false;

                /* There are some cases (lost partitions), where a rejoin
                 * should occur immediately following the unassign (this
                 * is not the case under normal conditions), in which case
                 * the rejoin flag will be set. */

                /* Skip the join backoff */
                rd_interval_reset(&rkcg->rkcg_join_intvl);

                rd_kafka_cgrp_rejoin(rkcg, "Incremental unassignment done");

        } else if (!rd_kafka_trigger_waiting_subscribe_maybe(rkcg)) {
                /* After this incremental unassignment we're now back in
                 * a steady state. */
                rd_kafka_cgrp_set_join_state(rkcg,
                                             RD_KAFKA_CGRP_JOIN_STATE_STEADY);
        }
}


/**
 * @brief Call when all absolute (non-incremental) unassign operations are done
 *        to transition to the next state.
 */
static void rd_kafka_cgrp_unassign_done(rd_kafka_cgrp_t *rkcg) {
        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "UNASSIGN",
                     "Group \"%s\": unassign done in state %s "
                     "(join-state %s)",
                     rkcg->rkcg_group_id->str,
                     rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);

        /* Leave group, if desired. */
        rd_kafka_cgrp_leave_maybe(rkcg);

        if (rkcg->rkcg_join_state !=
            RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_TO_COMPLETE)
                return;

        /* All partitions are unassigned. Rejoin the group. */

        /* Skip the join backoff */
        rd_interval_reset(&rkcg->rkcg_join_intvl);

        rd_kafka_cgrp_rejoin(rkcg, "Unassignment done");
}



/**
 * @brief Called from assignment code when all in progress
 *        assignment/unassignment operations are done, allowing the cgrp to
 *        transition to other states if needed.
 *
 * @remark This may be called spontaneously without any need for a state
 *         change in the rkcg.
 */
void rd_kafka_cgrp_assignment_done(rd_kafka_cgrp_t *rkcg) {
        if (rkcg->rkcg_group_protocol == RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                rd_kafka_cgrp_consumer_assignment_done(rkcg);
                return;
        }

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "ASSIGNDONE",
                     "Group \"%s\": "
                     "assignment operations done in join-state %s "
                     "(rebalance rejoin=%s)",
                     rkcg->rkcg_group_id->str,
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
                     RD_STR_ToF(rkcg->rkcg_rebalance_rejoin));

        switch (rkcg->rkcg_join_state) {
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_TO_COMPLETE:
                rd_kafka_cgrp_unassign_done(rkcg);
                break;

        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_INCR_UNASSIGN_TO_COMPLETE:
                rd_kafka_cgrp_incr_unassign_done(rkcg);
                break;

        case RD_KAFKA_CGRP_JOIN_STATE_STEADY:
                /* If an updated/next subscription is available, schedule it. */
                if (rd_kafka_trigger_waiting_subscribe_maybe(rkcg))
                        break;

                if (rkcg->rkcg_rebalance_rejoin) {
                        rkcg->rkcg_rebalance_rejoin = rd_false;

                        /* Skip the join backoff */
                        rd_interval_reset(&rkcg->rkcg_join_intvl);

                        rd_kafka_cgrp_rejoin(
                            rkcg,
                            "rejoining group to redistribute "
                            "previously owned partitions to other "
                            "group members");
                        break;
                }

                /* FALLTHRU */

        case RD_KAFKA_CGRP_JOIN_STATE_INIT:
                /* Check if cgrp is trying to terminate, which is safe to do
                 * in these two states. Otherwise we'll need to wait for
                 * the current state to decommission. */
                rd_kafka_cgrp_try_terminate(rkcg);
                break;

        default:
                break;
        }
}



/**
 * @brief Remove existing assignment.
 */
static rd_kafka_error_t *rd_kafka_cgrp_unassign(rd_kafka_cgrp_t *rkcg) {

        rd_kafka_assignment_clear(rkcg->rkcg_rk);

        if (rkcg->rkcg_join_state ==
            RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_CALL) {
                rd_kafka_assignment_resume(rkcg->rkcg_rk, "unassign called");
                rd_kafka_cgrp_set_join_state(
                    rkcg, RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_TO_COMPLETE);
        }

        rd_kafka_cgrp_assignment_clear_lost(rkcg, "unassign() called");

        return NULL;
}

/**
 * @brief Set new atomic partition assignment
 *        May update \p assignment but will not hold on to it.
 *
 * @returns NULL on success or an error if a fatal error has been raised.
 */
static rd_kafka_error_t *
rd_kafka_cgrp_assign(rd_kafka_cgrp_t *rkcg,
                     rd_kafka_topic_partition_list_t *assignment) {
        rd_kafka_error_t *error;

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_CONSUMER, "ASSIGN",
                     "Group \"%s\": new assignment of %d partition(s) "
                     "in join-state %s",
                     rkcg->rkcg_group_id->str, assignment ? assignment->cnt : 0,
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);

        /* Clear existing assignment, if any, and serve its removals. */
        if (rd_kafka_assignment_clear(rkcg->rkcg_rk))
                rd_kafka_assignment_serve(rkcg->rkcg_rk);

        error = rd_kafka_assignment_add(rkcg->rkcg_rk, assignment);
        if (error)
                return error;

        rd_kafka_cgrp_assignment_clear_lost(rkcg, "assign() called");

        if (rkcg->rkcg_join_state ==
            RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_CALL) {
                rd_kafka_assignment_resume(rkcg->rkcg_rk, "assign called");
                rd_kafka_cgrp_set_join_state(rkcg,
                                             RD_KAFKA_CGRP_JOIN_STATE_STEADY);
                if (rkcg->rkcg_subscription) {
                        rd_kafka_cgrp_start_max_poll_interval_timer(rkcg);
                }
        }

        return NULL;
}



/**
 * @brief Construct a typed map from list \p rktparlist with key corresponding
 *        to each element in the list and value NULL.
 *
 * @remark \p rktparlist may be NULL.
 */
static map_toppar_member_info_t *rd_kafka_toppar_list_to_toppar_member_info_map(
    rd_kafka_topic_partition_list_t *rktparlist) {
        map_toppar_member_info_t *map = rd_calloc(1, sizeof(*map));
        const rd_kafka_topic_partition_t *rktpar;

        RD_MAP_INIT(map, rktparlist ? rktparlist->cnt : 0,
                    rd_kafka_topic_partition_cmp, rd_kafka_topic_partition_hash,
                    rd_kafka_topic_partition_destroy_free,
                    PartitionMemberInfo_free);

        if (!rktparlist)
                return map;

        RD_KAFKA_TPLIST_FOREACH(rktpar, rktparlist)
        RD_MAP_SET(map, rd_kafka_topic_partition_copy(rktpar),
                   PartitionMemberInfo_new(NULL, rd_false));

        return map;
}


/**
 * @brief Construct a toppar list from map \p map with elements corresponding
 *        to the keys of \p map.
 */
static rd_kafka_topic_partition_list_t *
rd_kafka_toppar_member_info_map_to_list(map_toppar_member_info_t *map) {
        const rd_kafka_topic_partition_t *k;
        rd_kafka_topic_partition_list_t *list =
            rd_kafka_topic_partition_list_new((int)RD_MAP_CNT(map));

        RD_MAP_FOREACH_KEY(k, map) {
                rd_kafka_topic_partition_list_add_copy(list, k);
        }

        return list;
}


/**
 * @brief Handle a rebalance-triggered partition assignment
 *        (COOPERATIVE case).
 */
static void rd_kafka_cgrp_handle_assignment_cooperative(
    rd_kafka_cgrp_t *rkcg,
    rd_kafka_topic_partition_list_t *assignment) {
        map_toppar_member_info_t *new_assignment_set;
        map_toppar_member_info_t *old_assignment_set;
        map_toppar_member_info_t *newly_added_set;
        map_toppar_member_info_t *revoked_set;
        rd_kafka_topic_partition_list_t *newly_added;
        rd_kafka_topic_partition_list_t *revoked;

        new_assignment_set =
            rd_kafka_toppar_list_to_toppar_member_info_map(assignment);

        old_assignment_set = rd_kafka_toppar_list_to_toppar_member_info_map(
            rkcg->rkcg_group_assignment);

        newly_added_set = rd_kafka_member_partitions_subtract(
            new_assignment_set, old_assignment_set);
        revoked_set = rd_kafka_member_partitions_subtract(old_assignment_set,
                                                          new_assignment_set);

        newly_added = rd_kafka_toppar_member_info_map_to_list(newly_added_set);
        revoked     = rd_kafka_toppar_member_info_map_to_list(revoked_set);

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "COOPASSIGN",
                     "Group \"%s\": incremental assignment: %d newly added, "
                     "%d revoked partitions based on assignment of %d "
                     "partitions",
                     rkcg->rkcg_group_id->str, newly_added->cnt, revoked->cnt,
                     assignment->cnt);

        if (revoked->cnt > 0) {
                /* Setting rkcg_incr_assignment causes a follow on incremental
                 * assign rebalance op after completion of this incremental
                 * unassign op. */

                rkcg->rkcg_rebalance_incr_assignment = newly_added;
                newly_added                          = NULL;

                rd_kafka_rebalance_op_incr(rkcg,
                                           RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS,
                                           revoked, rd_false /*no rejoin
                                            following unassign*/
                                           ,
                                           "sync group revoke");

        } else {
                /* There are no revoked partitions - trigger the assign
                 * rebalance op, and flag that the group does not need
                 * to be re-joined */

                rd_kafka_rebalance_op_incr(
                    rkcg, RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS, newly_added,
                    rd_false /*no rejoin following assign*/,
                    "sync group assign");
        }

        if (newly_added)
                rd_kafka_topic_partition_list_destroy(newly_added);
        rd_kafka_topic_partition_list_destroy(revoked);
        RD_MAP_DESTROY_AND_FREE(revoked_set);
        RD_MAP_DESTROY_AND_FREE(newly_added_set);
        RD_MAP_DESTROY_AND_FREE(old_assignment_set);
        RD_MAP_DESTROY_AND_FREE(new_assignment_set);
}


/**
 * @brief Sets or clears the group's partition assignment for our consumer.
 *
 * Will replace the current group assignment, if any.
 */
static void rd_kafka_cgrp_group_assignment_set(
    rd_kafka_cgrp_t *rkcg,
    const rd_kafka_topic_partition_list_t *partitions) {

        if (rkcg->rkcg_group_assignment)
                rd_kafka_topic_partition_list_destroy(
                    rkcg->rkcg_group_assignment);

        if (partitions) {
                rkcg->rkcg_group_assignment =
                    rd_kafka_topic_partition_list_copy(partitions);
                rd_kafka_topic_partition_list_sort_by_topic(
                    rkcg->rkcg_group_assignment);
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "ASSIGNMENT",
                             "Group \"%s\": setting group assignment to %d "
                             "partition(s)",
                             rkcg->rkcg_group_id->str,
                             rkcg->rkcg_group_assignment->cnt);

        } else {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "ASSIGNMENT",
                             "Group \"%s\": clearing group assignment",
                             rkcg->rkcg_group_id->str);
                rkcg->rkcg_group_assignment = NULL;
        }

        rd_kafka_wrlock(rkcg->rkcg_rk);
        rkcg->rkcg_c.assignment_size =
            rkcg->rkcg_group_assignment ? rkcg->rkcg_group_assignment->cnt : 0;
        rd_kafka_wrunlock(rkcg->rkcg_rk);

        if (rkcg->rkcg_group_assignment)
                rd_kafka_topic_partition_list_log(
                    rkcg->rkcg_rk, "GRPASSIGNMENT", RD_KAFKA_DBG_CGRP,
                    rkcg->rkcg_group_assignment);
}


/**
 * @brief Adds or removes \p partitions from the current group assignment.
 *
 * @param add Whether to add or remove the partitions.
 *
 * @remark The added partitions must not already be on the group assignment,
 *         and the removed partitions must be on the group assignment.
 *
 * To be used with incremental rebalancing.
 *
 */
static void rd_kafka_cgrp_group_assignment_modify(
    rd_kafka_cgrp_t *rkcg,
    rd_bool_t add,
    const rd_kafka_topic_partition_list_t *partitions) {
        const rd_kafka_topic_partition_t *rktpar;
        int precnt;
        rd_kafka_dbg(
            rkcg->rkcg_rk, CGRP, "ASSIGNMENT",
            "Group \"%s\": %d partition(s) being %s group assignment "
            "of %d partition(s)",
            rkcg->rkcg_group_id->str, partitions->cnt,
            add ? "added to" : "removed from",
            rkcg->rkcg_group_assignment ? rkcg->rkcg_group_assignment->cnt : 0);

        if (partitions == rkcg->rkcg_group_assignment) {
                /* \p partitions is the actual assignment, which
                 * must mean it is all to be removed.
                 * Short-cut directly to set(NULL). */
                rd_assert(!add);
                rd_kafka_cgrp_group_assignment_set(rkcg, NULL);
                return;
        }

        if (add && (!rkcg->rkcg_group_assignment ||
                    rkcg->rkcg_group_assignment->cnt == 0)) {
                /* Adding to an empty assignment is a set operation. */
                rd_kafka_cgrp_group_assignment_set(rkcg, partitions);
                return;
        }

        if (!add) {
                /* Removing from an empty assignment is illegal. */
                rd_assert(rkcg->rkcg_group_assignment != NULL &&
                          rkcg->rkcg_group_assignment->cnt > 0);
        }


        precnt = rkcg->rkcg_group_assignment->cnt;
        RD_KAFKA_TPLIST_FOREACH(rktpar, partitions) {
                int idx;

                idx = rd_kafka_topic_partition_list_find_idx(
                    rkcg->rkcg_group_assignment, rktpar->topic,
                    rktpar->partition);

                if (add) {
                        rd_assert(idx == -1);

                        rd_kafka_topic_partition_list_add_copy(
                            rkcg->rkcg_group_assignment, rktpar);

                } else {
                        rd_assert(idx != -1);

                        rd_kafka_topic_partition_list_del_by_idx(
                            rkcg->rkcg_group_assignment, idx);
                }
        }

        if (add)
                rd_assert(precnt + partitions->cnt ==
                          rkcg->rkcg_group_assignment->cnt);
        else
                rd_assert(precnt - partitions->cnt ==
                          rkcg->rkcg_group_assignment->cnt);

        if (rkcg->rkcg_group_assignment->cnt == 0) {
                rd_kafka_topic_partition_list_destroy(
                    rkcg->rkcg_group_assignment);
                rkcg->rkcg_group_assignment = NULL;

        } else if (add)
                rd_kafka_topic_partition_list_sort_by_topic(
                    rkcg->rkcg_group_assignment);

        rd_kafka_wrlock(rkcg->rkcg_rk);
        rkcg->rkcg_c.assignment_size =
            rkcg->rkcg_group_assignment ? rkcg->rkcg_group_assignment->cnt : 0;
        rd_kafka_wrunlock(rkcg->rkcg_rk);

        if (rkcg->rkcg_group_assignment)
                rd_kafka_topic_partition_list_log(
                    rkcg->rkcg_rk, "GRPASSIGNMENT", RD_KAFKA_DBG_CGRP,
                    rkcg->rkcg_group_assignment);
}


/**
 * @brief Handle a rebalance-triggered partition assignment.
 *
 *        If a rebalance_cb has been registered we enqueue an op for the app
 *        and let the app perform the actual assign() call. Otherwise we
 *        assign() directly from here.
 *
 *        This provides the most flexibility, allowing the app to perform any
 *        operation it seem fit (e.g., offset writes or reads) before actually
 *        updating the assign():ment.
 */
static void
rd_kafka_cgrp_handle_assignment(rd_kafka_cgrp_t *rkcg,
                                rd_kafka_topic_partition_list_t *assignment) {

        if (rd_kafka_cgrp_rebalance_protocol(rkcg) ==
            RD_KAFKA_REBALANCE_PROTOCOL_COOPERATIVE) {
                rd_kafka_cgrp_handle_assignment_cooperative(rkcg, assignment);
        } else {

                rd_kafka_rebalance_op(rkcg,
                                      RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS,
                                      assignment, "new assignment");
        }
}


/**
 * Clean up any group-leader related resources.
 *
 * Locality: cgrp thread
 */
static void rd_kafka_cgrp_group_leader_reset(rd_kafka_cgrp_t *rkcg,
                                             const char *reason) {
        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "GRPLEADER",
                     "Group \"%.*s\": resetting group leader info: %s",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id), reason);

        if (rkcg->rkcg_group_leader.members) {
                int i;

                for (i = 0; i < rkcg->rkcg_group_leader.member_cnt; i++)
                        rd_kafka_group_member_clear(
                            &rkcg->rkcg_group_leader.members[i]);
                rkcg->rkcg_group_leader.member_cnt = 0;
                rd_free(rkcg->rkcg_group_leader.members);
                rkcg->rkcg_group_leader.members = NULL;
        }
}


/**
 * @brief React to a RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS broker response.
 */
static void rd_kafka_cgrp_group_is_rebalancing(rd_kafka_cgrp_t *rkcg) {

        if (rd_kafka_cgrp_rebalance_protocol(rkcg) ==
            RD_KAFKA_REBALANCE_PROTOCOL_EAGER) {
                rd_kafka_cgrp_revoke_all_rejoin_maybe(rkcg, rd_false /*lost*/,
                                                      rd_false /*initiating*/,
                                                      "rebalance in progress");
                return;
        }


        /* In the COOPERATIVE case, simply rejoin the group
         * - partitions are unassigned on SyncGroup response,
         * not prior to JoinGroup as with the EAGER case. */

        if (RD_KAFKA_CGRP_REBALANCING(rkcg)) {
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP, "REBALANCE",
                    "Group \"%.*s\": skipping "
                    "COOPERATIVE rebalance in state %s "
                    "(join-state %s)%s%s%s",
                    RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                    rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                    rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
                    RD_KAFKA_CGRP_WAIT_ASSIGN_CALL(rkcg)
                        ? " (awaiting assign call)"
                        : "",
                    (rkcg->rkcg_rebalance_incr_assignment != NULL)
                        ? " (incremental assignment pending)"
                        : "",
                    rkcg->rkcg_rebalance_rejoin ? " (rebalance rejoin)" : "");
                return;
        }

        rd_kafka_cgrp_rejoin(rkcg, "Group is rebalancing");
}



/**
 * @brief Triggers the application rebalance callback if required to
 *        revoke partitions, and transition to INIT state for (eventual)
 *        rejoin. Does nothing if a rebalance workflow is already in
 *        progress
 */
static void rd_kafka_cgrp_revoke_all_rejoin_maybe(rd_kafka_cgrp_t *rkcg,
                                                  rd_bool_t assignment_lost,
                                                  rd_bool_t initiating,
                                                  const char *reason) {
        if (RD_KAFKA_CGRP_REBALANCING(rkcg)) {
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP, "REBALANCE",
                    "Group \"%.*s\": rebalance (%s) "
                    "already in progress, skipping in state %s "
                    "(join-state %s) with %d assigned partition(s)%s%s%s: "
                    "%s",
                    RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                    rd_kafka_rebalance_protocol2str(
                        rd_kafka_cgrp_rebalance_protocol(rkcg)),
                    rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                    rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
                    rkcg->rkcg_group_assignment
                        ? rkcg->rkcg_group_assignment->cnt
                        : 0,
                    assignment_lost ? " (lost)" : "",
                    rkcg->rkcg_rebalance_incr_assignment
                        ? ", incremental assignment in progress"
                        : "",
                    rkcg->rkcg_rebalance_rejoin ? ", rejoin on rebalance" : "",
                    reason);
                return;
        }

        rd_kafka_cgrp_revoke_all_rejoin(rkcg, assignment_lost, initiating,
                                        reason);
}


/**
 * @brief Triggers the application rebalance callback if required to
 *        revoke partitions, and transition to INIT state for (eventual)
 *        rejoin.
 */
static void rd_kafka_cgrp_revoke_all_rejoin(rd_kafka_cgrp_t *rkcg,
                                            rd_bool_t assignment_lost,
                                            rd_bool_t initiating,
                                            const char *reason) {

        rd_kafka_rebalance_protocol_t protocol =
            rd_kafka_cgrp_rebalance_protocol(rkcg);

        rd_bool_t terminating =
            unlikely(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE);


        rd_kafka_dbg(
            rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP, "REBALANCE",
            "Group \"%.*s\" %s (%s) in state %s (join-state %s) "
            "with %d assigned partition(s)%s: %s",
            RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
            initiating ? "initiating rebalance" : "is rebalancing",
            rd_kafka_rebalance_protocol2str(protocol),
            rd_kafka_cgrp_state_names[rkcg->rkcg_state],
            rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
            rkcg->rkcg_group_assignment ? rkcg->rkcg_group_assignment->cnt : 0,
            assignment_lost ? " (lost)" : "", reason);

        rd_snprintf(rkcg->rkcg_c.rebalance_reason,
                    sizeof(rkcg->rkcg_c.rebalance_reason), "%s", reason);


        if (protocol == RD_KAFKA_REBALANCE_PROTOCOL_EAGER ||
            protocol == RD_KAFKA_REBALANCE_PROTOCOL_NONE) {
                /* EAGER case (or initial subscribe) - revoke partitions which
                 * will be followed by rejoin, if required. */

                if (assignment_lost)
                        rd_kafka_cgrp_assignment_set_lost(
                            rkcg, "%s: revoking assignment and rejoining",
                            reason);

                /* Schedule application rebalance op if there is an existing
                 * assignment (albeit perhaps empty) and there is no
                 * outstanding rebalance op in progress. */
                if (rkcg->rkcg_group_assignment &&
                    !RD_KAFKA_CGRP_WAIT_ASSIGN_CALL(rkcg)) {
                        rd_kafka_rebalance_op(
                            rkcg, RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS,
                            rkcg->rkcg_group_assignment, reason);
                } else {
                        /* Skip the join backoff */
                        rd_interval_reset(&rkcg->rkcg_join_intvl);

                        rd_kafka_cgrp_rejoin(rkcg, "%s", reason);
                }

                return;
        }


        /* COOPERATIVE case. */

        /* All partitions should never be revoked unless terminating, leaving
         * the group, or on assignment lost. Another scenario represents a
         * logic error. Fail fast in this case. */
        if (!(terminating || assignment_lost ||
              (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_LEAVE_ON_UNASSIGN_DONE))) {
                rd_kafka_log(rkcg->rkcg_rk, LOG_ERR, "REBALANCE",
                             "Group \"%s\": unexpected instruction to revoke "
                             "current assignment and rebalance "
                             "(terminating=%d, assignment_lost=%d, "
                             "LEAVE_ON_UNASSIGN_DONE=%d)",
                             rkcg->rkcg_group_id->str, terminating,
                             assignment_lost,
                             (rkcg->rkcg_flags &
                              RD_KAFKA_CGRP_F_LEAVE_ON_UNASSIGN_DONE));
                rd_dassert(!*"BUG: unexpected instruction to revoke "
                           "current assignment and rebalance");
        }

        if (rkcg->rkcg_group_assignment &&
            rkcg->rkcg_group_assignment->cnt > 0) {
                if (assignment_lost)
                        rd_kafka_cgrp_assignment_set_lost(
                            rkcg,
                            "%s: revoking incremental assignment "
                            "and rejoining",
                            reason);

                rd_kafka_dbg(rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP,
                             "REBALANCE",
                             "Group \"%.*s\": revoking "
                             "all %d partition(s)%s%s",
                             RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                             rkcg->rkcg_group_assignment->cnt,
                             terminating ? " (terminating)" : "",
                             assignment_lost ? " (assignment lost)" : "");

                rd_kafka_rebalance_op_incr(
                    rkcg, RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS,
                    rkcg->rkcg_group_assignment,
                    terminating ? rd_false : rd_true /*rejoin*/, reason);

                return;
        }

        if (terminating) {
                /* If terminating, then don't rejoin group. */
                rd_kafka_dbg(rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP,
                             "REBALANCE",
                             "Group \"%.*s\": consumer is "
                             "terminating, skipping rejoin",
                             RD_KAFKAP_STR_PR(rkcg->rkcg_group_id));
                return;
        }

        rd_kafka_cgrp_rejoin(rkcg, "Current assignment is empty");
}


/**
 * @brief `max.poll.interval.ms` enforcement check timer.
 *
 * @locality rdkafka main thread
 * @locks none
 */
static void
rd_kafka_cgrp_max_poll_interval_check_tmr_cb(rd_kafka_timers_t *rkts,
                                             void *arg) {
        rd_kafka_cgrp_t *rkcg = arg;
        rd_kafka_t *rk        = rkcg->rkcg_rk;
        int exceeded;

        exceeded = rd_kafka_max_poll_exceeded(rk);

        if (likely(!exceeded))
                return;

        rd_kafka_log(rk, LOG_WARNING, "MAXPOLL",
                     "Application maximum poll interval (%dms) "
                     "exceeded by %dms "
                     "(adjust max.poll.interval.ms for "
                     "long-running message processing): "
                     "leaving group",
                     rk->rk_conf.max_poll_interval_ms, exceeded);

        rd_kafka_consumer_err(rkcg->rkcg_q, RD_KAFKA_NODEID_UA,
                              RD_KAFKA_RESP_ERR__MAX_POLL_EXCEEDED, 0, NULL,
                              NULL, RD_KAFKA_OFFSET_INVALID,
                              "Application maximum poll interval (%dms) "
                              "exceeded by %dms",
                              rk->rk_conf.max_poll_interval_ms, exceeded);

        rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_MAX_POLL_EXCEEDED;

        rd_kafka_timer_stop(rkts, &rkcg->rkcg_max_poll_interval_tmr,
                            1 /*lock*/);

        if (rkcg->rkcg_group_protocol == RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                rd_kafka_cgrp_consumer_leave(rkcg);
                rkcg->rkcg_consumer_flags |=
                    RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN;
                rd_kafka_cgrp_consumer_expedite_next_heartbeat(
                    rkcg,
                    "max poll interval "
                    "exceeded");
        } else {
                /* Leave the group before calling rebalance since the standard
                 * leave will be triggered first after the rebalance callback
                 * has been served. But since the application is blocked still
                 * doing processing that leave will be further delayed.
                 *
                 * KIP-345: static group members should continue to respect
                 * `max.poll.interval.ms` but should not send a
                 * LeaveGroupRequest.
                 */
                if (!RD_KAFKA_CGRP_IS_STATIC_MEMBER(rkcg))
                        rd_kafka_cgrp_leave(rkcg);
                /* Timing out or leaving the group invalidates the member id,
                 * reset it now to avoid an ERR_UNKNOWN_MEMBER_ID on the next
                 * join. */
                rd_kafka_cgrp_set_member_id(rkcg, "");

                /* Trigger rebalance */
                rd_kafka_cgrp_revoke_all_rejoin_maybe(
                    rkcg, rd_true /*lost*/, rd_true /*initiating*/,
                    "max.poll.interval.ms exceeded");
        }
}


/**
 * @brief Generate consumer errors for each topic in the list.
 *
 * Also replaces the list of last reported topic errors so that repeated
 * errors are silenced.
 *
 * @param errored Errored topics.
 * @param error_prefix Error message prefix.
 *
 * @remark Assumes ownership of \p errored.
 */
static void rd_kafka_propagate_consumer_topic_errors(
    rd_kafka_cgrp_t *rkcg,
    rd_kafka_topic_partition_list_t *errored,
    const char *error_prefix) {
        int i;

        for (i = 0; i < errored->cnt; i++) {
                rd_kafka_topic_partition_t *topic = &errored->elems[i];
                rd_kafka_topic_partition_t *prev;

                rd_assert(topic->err);

                /* Normalize error codes, unknown topic may be
                 * reported by the broker, or the lack of a topic in
                 * metadata response is figured out by the client.
                 * Make sure the application only sees one error code
                 * for both these cases. */
                if (topic->err == RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC)
                        topic->err = RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;

                /* Check if this topic errored previously */
                prev = rd_kafka_topic_partition_list_find(
                    rkcg->rkcg_errored_topics, topic->topic,
                    RD_KAFKA_PARTITION_UA);

                if (prev && prev->err == topic->err)
                        continue; /* This topic already reported same error */

                rd_kafka_dbg(rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_TOPIC,
                             "TOPICERR", "%s: %s: %s", error_prefix,
                             topic->topic, rd_kafka_err2str(topic->err));

                /* Send consumer error to application */
                rd_kafka_consumer_err(
                    rkcg->rkcg_q, RD_KAFKA_NODEID_UA, topic->err, 0,
                    topic->topic, NULL, RD_KAFKA_OFFSET_INVALID, "%s: %s: %s",
                    error_prefix, topic->topic, rd_kafka_err2str(topic->err));
        }

        rd_kafka_topic_partition_list_destroy(rkcg->rkcg_errored_topics);
        rkcg->rkcg_errored_topics = errored;
}


/**
 * @brief Work out the topics currently subscribed to that do not
 *        match any pattern in \p subscription.
 */
static rd_kafka_topic_partition_list_t *rd_kafka_cgrp_get_unsubscribing_topics(
    rd_kafka_cgrp_t *rkcg,
    rd_kafka_topic_partition_list_t *subscription) {
        int i;
        rd_kafka_topic_partition_list_t *result;

        result = rd_kafka_topic_partition_list_new(
            rkcg->rkcg_subscribed_topics->rl_cnt);

        /* TODO: Something that isn't O(N*M) */
        for (i = 0; i < rkcg->rkcg_subscribed_topics->rl_cnt; i++) {
                int j;
                const char *topic =
                    ((rd_kafka_topic_info_t *)
                         rkcg->rkcg_subscribed_topics->rl_elems[i])
                        ->topic;

                for (j = 0; j < subscription->cnt; j++) {
                        const char *pattern = subscription->elems[j].topic;
                        if (rd_kafka_topic_match(rkcg->rkcg_rk, pattern,
                                                 topic)) {
                                break;
                        }
                }

                if (j == subscription->cnt)
                        rd_kafka_topic_partition_list_add(
                            result, topic, RD_KAFKA_PARTITION_UA);
        }

        if (result->cnt == 0) {
                rd_kafka_topic_partition_list_destroy(result);
                return NULL;
        }

        return result;
}


/**
 * @brief Determine the partitions to revoke, given the topics being
 *        unassigned.
 */
static rd_kafka_topic_partition_list_t *
rd_kafka_cgrp_calculate_subscribe_revoking_partitions(
    rd_kafka_cgrp_t *rkcg,
    const rd_kafka_topic_partition_list_t *unsubscribing) {
        rd_kafka_topic_partition_list_t *revoking;
        const rd_kafka_topic_partition_t *rktpar;

        if (!unsubscribing)
                return NULL;

        if (!rkcg->rkcg_group_assignment ||
            rkcg->rkcg_group_assignment->cnt == 0)
                return NULL;

        revoking =
            rd_kafka_topic_partition_list_new(rkcg->rkcg_group_assignment->cnt);

        /* TODO: Something that isn't O(N*M). */
        RD_KAFKA_TPLIST_FOREACH(rktpar, unsubscribing) {
                const rd_kafka_topic_partition_t *assigned;

                RD_KAFKA_TPLIST_FOREACH(assigned, rkcg->rkcg_group_assignment) {
                        if (!strcmp(assigned->topic, rktpar->topic)) {
                                rd_kafka_topic_partition_list_add(
                                    revoking, assigned->topic,
                                    assigned->partition);
                                continue;
                        }
                }
        }

        if (revoking->cnt == 0) {
                rd_kafka_topic_partition_list_destroy(revoking);
                revoking = NULL;
        }

        return revoking;
}

/**
 * @brief Set the new subscription and increase subscription version.
 *
 * @return The new subscription version.
 */
static int32_t
rd_kafka_cgrp_subscription_set(rd_kafka_cgrp_t *rkcg,
                               rd_kafka_topic_partition_list_t *rktparlist) {

        rkcg->rkcg_flags &= ~(RD_KAFKA_CGRP_F_SUBSCRIPTION |
                              RD_KAFKA_CGRP_F_WILDCARD_SUBSCRIPTION);
        RD_IF_FREE(rkcg->rkcg_subscription,
                   rd_kafka_topic_partition_list_destroy);
        RD_IF_FREE(rkcg->rkcg_subscription_topics,
                   rd_kafka_topic_partition_list_destroy);
        RD_IF_FREE(rkcg->rkcg_subscription_regex, rd_kafkap_str_destroy);

        rkcg->rkcg_subscription = rktparlist;

        if (rkcg->rkcg_subscription) {
                rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_SUBSCRIPTION;
                if (rd_kafka_topic_partition_list_regex_cnt(
                        rkcg->rkcg_subscription) > 0)
                        rkcg->rkcg_flags |=
                            RD_KAFKA_CGRP_F_WILDCARD_SUBSCRIPTION;

                if (rkcg->rkcg_group_protocol ==
                    RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                        rkcg->rkcg_subscription_regex =
                            rd_kafka_topic_partition_list_combine_regexes(
                                rkcg->rkcg_subscription);
                        rkcg->rkcg_subscription_topics =
                            rd_kafka_topic_partition_list_remove_regexes(
                                rkcg->rkcg_subscription);
                        rkcg->rkcg_consumer_flags |=
                            RD_KAFKA_CGRP_CONSUMER_F_SUBSCRIBED_ONCE |
                            RD_KAFKA_CGRP_CONSUMER_F_SEND_NEW_SUBSCRIPTION;
                        rd_kafka_cgrp_maybe_clear_heartbeat_failed_err(rkcg);
                }
        } else {
                rkcg->rkcg_subscription_regex  = NULL;
                rkcg->rkcg_subscription_topics = NULL;
                if (rkcg->rkcg_next_subscription) {
                        /* When unsubscribing clear next subscription too */
                        rd_kafka_topic_partition_list_destroy(
                            rkcg->rkcg_next_subscription);
                        rkcg->rkcg_next_subscription = NULL;
                }
        }

        return rd_atomic32_add(&rkcg->rkcg_subscription_version, 1);
}


/**
 * @brief Handle a new subscription that is modifying an existing subscription
 *        in the COOPERATIVE case.
 *
 * @remark Assumes ownership of \p rktparlist.
 */
static rd_kafka_resp_err_t
rd_kafka_cgrp_modify_subscription(rd_kafka_cgrp_t *rkcg,
                                  rd_kafka_topic_partition_list_t *rktparlist) {
        rd_kafka_topic_partition_list_t *unsubscribing_topics;
        rd_kafka_topic_partition_list_t *revoking;
        rd_list_t *tinfos;
        rd_kafka_topic_partition_list_t *errored;
        int metadata_age;
        int old_cnt = rkcg->rkcg_subscription->cnt;
        int32_t cgrp_subscription_version;

        /* Topics in rkcg_subscribed_topics that don't match any pattern in
           the new subscription. */
        unsubscribing_topics =
            rd_kafka_cgrp_get_unsubscribing_topics(rkcg, rktparlist);

        /* Currently assigned topic partitions that are no longer desired. */
        revoking = rd_kafka_cgrp_calculate_subscribe_revoking_partitions(
            rkcg, unsubscribing_topics);

        cgrp_subscription_version =
            rd_kafka_cgrp_subscription_set(rkcg, rktparlist);

        if (rd_kafka_cgrp_metadata_refresh(rkcg, &metadata_age,
                                           cgrp_subscription_version,
                                           "modify subscription") == 1) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_CONSUMER,
                             "MODSUB",
                             "Group \"%.*s\": postponing join until "
                             "up-to-date metadata is available",
                             RD_KAFKAP_STR_PR(rkcg->rkcg_group_id));

                rd_assert(
                    rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_INIT ||
                    /* Possible via rd_kafka_cgrp_modify_subscription */
                    rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_STEADY);

                rd_kafka_cgrp_set_join_state(
                    rkcg, RD_KAFKA_CGRP_JOIN_STATE_WAIT_METADATA);


                /* Revoke/join will occur after metadata refresh completes */
                if (revoking)
                        rd_kafka_topic_partition_list_destroy(revoking);
                if (unsubscribing_topics)
                        rd_kafka_topic_partition_list_destroy(
                            unsubscribing_topics);

                return RD_KAFKA_RESP_ERR_NO_ERROR;
        }

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_CONSUMER, "SUBSCRIBE",
                     "Group \"%.*s\": modifying subscription of size %d to "
                     "new subscription of size %d, removing %d topic(s), "
                     "revoking %d partition(s) (join-state %s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id), old_cnt,
                     rkcg->rkcg_subscription->cnt,
                     unsubscribing_topics ? unsubscribing_topics->cnt : 0,
                     revoking ? revoking->cnt : 0,
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);

        if (unsubscribing_topics)
                rd_kafka_topic_partition_list_destroy(unsubscribing_topics);

        /* Create a list of the topics in metadata that matches the new
         * subscription */
        tinfos = rd_list_new(rkcg->rkcg_subscription->cnt,
                             rd_kafka_topic_info_destroy_free);

        /* Unmatched topics will be added to the errored list. */
        errored = rd_kafka_topic_partition_list_new(0);

        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WILDCARD_SUBSCRIPTION)
                rd_kafka_metadata_topic_match(rkcg->rkcg_rk, tinfos,
                                              rkcg->rkcg_subscription, errored);
        else
                rd_kafka_metadata_topic_filter(
                    rkcg->rkcg_rk, tinfos, rkcg->rkcg_subscription, errored);

        /* Propagate consumer errors for any non-existent or errored topics.
         * The function takes ownership of errored. */
        rd_kafka_propagate_consumer_topic_errors(
            rkcg, errored, "Subscribed topic not available");

        if (rd_kafka_cgrp_update_subscribed_topics(rkcg, tinfos) && !revoking) {
                rd_kafka_cgrp_rejoin(rkcg, "Subscription modified");
                return RD_KAFKA_RESP_ERR_NO_ERROR;
        }

        if (revoking) {
                rd_kafka_dbg(rkcg->rkcg_rk, CONSUMER | RD_KAFKA_DBG_CGRP,
                             "REBALANCE",
                             "Group \"%.*s\" revoking "
                             "%d of %d partition(s)",
                             RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                             revoking->cnt, rkcg->rkcg_group_assignment->cnt);

                rd_kafka_rebalance_op_incr(
                    rkcg, RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS, revoking,
                    rd_true /*rejoin*/, "subscribe");

                rd_kafka_topic_partition_list_destroy(revoking);
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * Remove existing topic subscription (KIP 848).
 */
static rd_kafka_resp_err_t
rd_kafka_cgrp_consumer_unsubscribe(rd_kafka_cgrp_t *rkcg,
                                   rd_bool_t leave_group) {

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "UNSUBSCRIBE",
                     "Group \"%.*s\": unsubscribe from current %ssubscription "
                     "of size %d (leave group=%s, has joined=%s, %s, "
                     "join-state %s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rkcg->rkcg_subscription ? "" : "unset ",
                     rkcg->rkcg_subscription ? rkcg->rkcg_subscription->cnt : 0,
                     RD_STR_ToF(leave_group),
                     RD_STR_ToF(RD_KAFKA_CGRP_HAS_JOINED(rkcg)),
                     rkcg->rkcg_member_id ? rkcg->rkcg_member_id->str : "n/a",
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);

        rd_kafka_timer_stop(&rkcg->rkcg_rk->rk_timers,
                            &rkcg->rkcg_max_poll_interval_tmr, 1 /*lock*/);

        rd_kafka_cgrp_subscription_set(rkcg, NULL);

        /* When group is rejoining the leave group call is either:
         * - been done on max.poll.interval reached
         * - not necessary because member has been fenced
         *
         * When group is already leaving we just wait that previous
         * leave request finishes.
         */
        if (leave_group && !rd_kafka_cgrp_consumer_will_rejoin(rkcg) &&
            RD_KAFKA_CGRP_HAS_JOINED(rkcg) && !rd_kafka_cgrp_will_leave(rkcg)) {
                rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_LEAVE_ON_UNASSIGN_DONE;
                rd_kafka_cgrp_revoke_all_rejoin(rkcg, rd_false /*not lost*/,
                                                rd_true /*initiating*/,
                                                "unsubscribe");
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * Remove existing topic subscription.
 */
static rd_kafka_resp_err_t rd_kafka_cgrp_unsubscribe(rd_kafka_cgrp_t *rkcg,
                                                     rd_bool_t leave_group) {
        if (rkcg->rkcg_group_protocol == RD_KAFKA_GROUP_PROTOCOL_CONSUMER)
                return rd_kafka_cgrp_consumer_unsubscribe(rkcg, leave_group);

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "UNSUBSCRIBE",
                     "Group \"%.*s\": unsubscribe from current %ssubscription "
                     "of size %d (leave group=%s, has joined=%s, %s, "
                     "join-state %s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rkcg->rkcg_subscription ? "" : "unset ",
                     rkcg->rkcg_subscription ? rkcg->rkcg_subscription->cnt : 0,
                     RD_STR_ToF(leave_group),
                     RD_STR_ToF(RD_KAFKA_CGRP_HAS_JOINED(rkcg)),
                     rkcg->rkcg_member_id ? rkcg->rkcg_member_id->str : "n/a",
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);

        rd_kafka_timer_stop(&rkcg->rkcg_rk->rk_timers,
                            &rkcg->rkcg_max_poll_interval_tmr, 1 /*lock*/);

        rd_kafka_cgrp_subscription_set(rkcg, NULL);

        rd_kafka_cgrp_update_subscribed_topics(rkcg, NULL);

        /*
         * Clean-up group leader duties, if any.
         */
        rd_kafka_cgrp_group_leader_reset(rkcg, "unsubscribe");

        if (leave_group && RD_KAFKA_CGRP_HAS_JOINED(rkcg))
                rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_LEAVE_ON_UNASSIGN_DONE;

        /* FIXME: Why are we only revoking if !assignment_lost ? */
        if (!rd_kafka_cgrp_assignment_is_lost(rkcg))
                rd_kafka_cgrp_revoke_all_rejoin(rkcg, rd_false /*not lost*/,
                                                rd_true /*initiating*/,
                                                "unsubscribe");

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * Set new atomic topic subscription.
 */
static rd_kafka_resp_err_t
rd_kafka_cgrp_subscribe(rd_kafka_cgrp_t *rkcg,
                        rd_kafka_topic_partition_list_t *rktparlist) {
        int32_t subscription_version;
        rd_kafka_dbg(rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_CONSUMER, "SUBSCRIBE",
                     "Group \"%.*s\": subscribe to new %ssubscription "
                     "of %d topics (join-state %s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rktparlist ? "" : "unset ",
                     rktparlist ? rktparlist->cnt : 0,
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);

        if (rkcg->rkcg_rk->rk_conf.enabled_assignor_cnt == 0)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        /* If the consumer has raised a fatal error treat all subscribes as
           unsubscribe */
        if (rd_kafka_fatal_error_code(rkcg->rkcg_rk)) {
                if (rkcg->rkcg_subscription)
                        rd_kafka_cgrp_unsubscribe(rkcg,
                                                  rd_true /*leave group*/);
                return RD_KAFKA_RESP_ERR__FATAL;
        }

        /* Clear any existing postponed subscribe. */
        if (rkcg->rkcg_next_subscription)
                rd_kafka_topic_partition_list_destroy_free(
                    rkcg->rkcg_next_subscription);
        rkcg->rkcg_next_subscription = NULL;
        rkcg->rkcg_next_unsubscribe  = rd_false;

        if (RD_KAFKA_CGRP_REBALANCING(rkcg)) {
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_CONSUMER, "SUBSCRIBE",
                    "Group \"%.*s\": postponing "
                    "subscribe until previous rebalance "
                    "completes (join-state %s)",
                    RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                    rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);

                if (!rktparlist)
                        rkcg->rkcg_next_unsubscribe = rd_true;
                else
                        rkcg->rkcg_next_subscription = rktparlist;

                return RD_KAFKA_RESP_ERR_NO_ERROR;
        }

        if (rd_kafka_cgrp_rebalance_protocol(rkcg) ==
                RD_KAFKA_REBALANCE_PROTOCOL_COOPERATIVE &&
            rktparlist && rkcg->rkcg_subscription)
                return rd_kafka_cgrp_modify_subscription(rkcg, rktparlist);

        /* Remove existing subscription first */
        if (rkcg->rkcg_subscription)
                rd_kafka_cgrp_unsubscribe(
                    rkcg,
                    rktparlist
                        ? rd_false /* don't leave group if new subscription */
                        : rd_true /* leave group if no new subscription */);

        if (!rktparlist)
                return RD_KAFKA_RESP_ERR_NO_ERROR;

        subscription_version = rd_kafka_cgrp_subscription_set(rkcg, rktparlist);

        rd_kafka_cgrp_join(rkcg, subscription_version);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



/**
 * Same as cgrp_terminate() but called from the cgrp/main thread upon receiving
 * the op 'rko' from cgrp_terminate().
 *
 * NOTE: Takes ownership of 'rko'
 *
 * Locality: main thread
 */
void rd_kafka_cgrp_terminate0(rd_kafka_cgrp_t *rkcg, rd_kafka_op_t *rko) {

        rd_kafka_assert(NULL, thrd_is_current(rkcg->rkcg_rk->rk_thread));

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPTERM",
                     "Terminating group \"%.*s\" in state %s "
                     "with %d partition(s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                     rd_list_cnt(&rkcg->rkcg_toppars));

        if (unlikely(rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_TERM ||
                     (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE) ||
                     rkcg->rkcg_reply_rko != NULL)) {
                /* Already terminating or handling a previous terminate */
                if (rko) {
                        rd_kafka_q_t *rkq = rko->rko_replyq.q;
                        rko->rko_replyq.q = NULL;
                        rd_kafka_consumer_err(
                            rkq, RD_KAFKA_NODEID_UA,
                            RD_KAFKA_RESP_ERR__IN_PROGRESS,
                            rko->rko_replyq.version, NULL, NULL,
                            RD_KAFKA_OFFSET_INVALID, "Group is %s",
                            rkcg->rkcg_reply_rko ? "terminating"
                                                 : "terminated");
                        rd_kafka_q_destroy(rkq);
                        rd_kafka_op_destroy(rko);
                }
                return;
        }

        /* Mark for stopping, the actual state transition
         * is performed when all toppars have left. */
        rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_TERMINATE;
        if (rkcg->rkcg_group_protocol == RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                rkcg->rkcg_consumer_flags &=
                    ~RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN &
                    ~RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN_TO_COMPLETE;
        }
        rkcg->rkcg_ts_terminate = rd_clock();
        rkcg->rkcg_reply_rko    = rko;

        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_SUBSCRIPTION)
                rd_kafka_cgrp_unsubscribe(
                    rkcg,
                    /* Leave group if this is a controlled shutdown */
                    !rd_kafka_destroy_flags_no_consumer_close(rkcg->rkcg_rk));

        /* Reset the wait-for-LeaveGroup flag if there is an outstanding
         * LeaveGroupRequest being waited on (from a prior unsubscribe), but
         * the destroy flags have NO_CONSUMER_CLOSE set, which calls
         * for immediate termination. */
        if (rd_kafka_destroy_flags_no_consumer_close(rkcg->rkcg_rk))
                rkcg->rkcg_flags &= ~RD_KAFKA_CGRP_F_WAIT_LEAVE;

        /* If there's an oustanding rebalance which has not yet been
         * served by the application it will be served from consumer_close().
         * If the instance is being terminated with NO_CONSUMER_CLOSE we
         * trigger unassign directly to avoid stalling on rebalance callback
         * queues that are no longer served by the application. */
        if (!RD_KAFKA_CGRP_WAIT_ASSIGN_CALL(rkcg) ||
            rd_kafka_destroy_flags_no_consumer_close(rkcg->rkcg_rk))
                rd_kafka_cgrp_unassign(rkcg);

        /* Serve assignment so it can start to decommission */
        rd_kafka_assignment_serve(rkcg->rkcg_rk);

        /* Try to terminate right away if all preconditions are met. */
        rd_kafka_cgrp_try_terminate(rkcg);
}


/**
 * Terminate and decommission a cgrp asynchronously.
 *
 * Locality: any thread
 */
void rd_kafka_cgrp_terminate(rd_kafka_cgrp_t *rkcg, rd_kafka_replyq_t replyq) {
        rd_kafka_assert(NULL, !thrd_is_current(rkcg->rkcg_rk->rk_thread));
        rd_kafka_cgrp_op(rkcg, NULL, replyq, RD_KAFKA_OP_TERMINATE, 0);
}


struct _op_timeout_offset_commit {
        rd_ts_t now;
        rd_kafka_t *rk;
        rd_list_t expired;
};

/**
 * q_filter callback for expiring OFFSET_COMMIT timeouts.
 */
static int rd_kafka_op_offset_commit_timeout_check(rd_kafka_q_t *rkq,
                                                   rd_kafka_op_t *rko,
                                                   void *opaque) {
        struct _op_timeout_offset_commit *state =
            (struct _op_timeout_offset_commit *)opaque;

        if (likely(rko->rko_type != RD_KAFKA_OP_OFFSET_COMMIT ||
                   rko->rko_u.offset_commit.ts_timeout == 0 ||
                   rko->rko_u.offset_commit.ts_timeout > state->now)) {
                return 0;
        }

        rd_kafka_q_deq0(rkq, rko);

        /* Add to temporary list to avoid recursive
         * locking of rkcg_wait_coord_q. */
        rd_list_add(&state->expired, rko);
        return 1;
}


/**
 * Scan for various timeouts.
 */
static void rd_kafka_cgrp_timeout_scan(rd_kafka_cgrp_t *rkcg, rd_ts_t now) {
        struct _op_timeout_offset_commit ofc_state;
        int i, cnt = 0;
        rd_kafka_op_t *rko;

        ofc_state.now = now;
        ofc_state.rk  = rkcg->rkcg_rk;
        rd_list_init(&ofc_state.expired, 0, NULL);

        cnt += rd_kafka_q_apply(rkcg->rkcg_wait_coord_q,
                                rd_kafka_op_offset_commit_timeout_check,
                                &ofc_state);

        RD_LIST_FOREACH(rko, &ofc_state.expired, i)
        rd_kafka_cgrp_op_handle_OffsetCommit(rkcg->rkcg_rk, NULL,
                                             RD_KAFKA_RESP_ERR__WAIT_COORD,
                                             NULL, NULL, rko);

        rd_list_destroy(&ofc_state.expired);

        if (cnt > 0)
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPTIMEOUT",
                             "Group \"%.*s\": timed out %d op(s), %d remain",
                             RD_KAFKAP_STR_PR(rkcg->rkcg_group_id), cnt,
                             rd_kafka_q_len(rkcg->rkcg_wait_coord_q));
}


/**
 * @brief Handle an assign op.
 * @locality rdkafka main thread
 * @locks none
 */
static void rd_kafka_cgrp_handle_assign_op(rd_kafka_cgrp_t *rkcg,
                                           rd_kafka_op_t *rko) {
        rd_kafka_error_t *error = NULL;

        if (rd_kafka_fatal_error_code(rkcg->rkcg_rk) ||
            rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE) {
                /* Treat all assignments as unassign when a fatal error is
                 * raised or the cgrp is terminating. */

                rd_kafka_dbg(rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_CONSUMER,
                             "ASSIGN",
                             "Group \"%s\": Consumer %s: "
                             "treating assign as unassign",
                             rkcg->rkcg_group_id->str,
                             rd_kafka_fatal_error_code(rkcg->rkcg_rk)
                                 ? "has raised a fatal error"
                                 : "is terminating");

                if (rko->rko_u.assign.partitions) {
                        rd_kafka_topic_partition_list_destroy(
                            rko->rko_u.assign.partitions);
                        rko->rko_u.assign.partitions = NULL;
                }

                if (rkcg->rkcg_rebalance_incr_assignment) {
                        rd_kafka_topic_partition_list_destroy(
                            rkcg->rkcg_rebalance_incr_assignment);
                        rkcg->rkcg_rebalance_incr_assignment = NULL;
                }

                rko->rko_u.assign.method = RD_KAFKA_ASSIGN_METHOD_ASSIGN;

                if (rkcg->rkcg_join_state ==
                    RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_CALL) {
                        rd_kafka_cgrp_set_join_state(
                            rkcg, RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_CALL);
                }

        } else if (rd_kafka_cgrp_rebalance_protocol(rkcg) ==
                       RD_KAFKA_REBALANCE_PROTOCOL_COOPERATIVE &&
                   !(rko->rko_u.assign.method ==
                         RD_KAFKA_ASSIGN_METHOD_INCR_ASSIGN ||
                     rko->rko_u.assign.method ==
                         RD_KAFKA_ASSIGN_METHOD_INCR_UNASSIGN))
                error = rd_kafka_error_new(RD_KAFKA_RESP_ERR__STATE,
                                           "Changes to the current assignment "
                                           "must be made using "
                                           "incremental_assign() or "
                                           "incremental_unassign() "
                                           "when rebalance protocol type is "
                                           "COOPERATIVE");

        else if (rd_kafka_cgrp_rebalance_protocol(rkcg) ==
                     RD_KAFKA_REBALANCE_PROTOCOL_EAGER &&
                 !(rko->rko_u.assign.method == RD_KAFKA_ASSIGN_METHOD_ASSIGN))
                error = rd_kafka_error_new(RD_KAFKA_RESP_ERR__STATE,
                                           "Changes to the current assignment "
                                           "must be made using "
                                           "assign() when rebalance "
                                           "protocol type is EAGER");

        if (!error) {
                switch (rko->rko_u.assign.method) {
                case RD_KAFKA_ASSIGN_METHOD_ASSIGN:
                        /* New atomic assignment (partitions != NULL),
                         * or unassignment (partitions == NULL) */
                        if (rko->rko_u.assign.partitions)
                                error = rd_kafka_cgrp_assign(
                                    rkcg, rko->rko_u.assign.partitions);
                        else
                                error = rd_kafka_cgrp_unassign(rkcg);
                        break;
                case RD_KAFKA_ASSIGN_METHOD_INCR_ASSIGN:
                        error = rd_kafka_cgrp_incremental_assign(
                            rkcg, rko->rko_u.assign.partitions);
                        break;
                case RD_KAFKA_ASSIGN_METHOD_INCR_UNASSIGN:
                        error = rd_kafka_cgrp_incremental_unassign(
                            rkcg, rko->rko_u.assign.partitions);
                        break;
                default:
                        RD_NOTREACHED();
                        break;
                }

                /* If call succeeded serve the assignment */
                if (!error)
                        rd_kafka_assignment_serve(rkcg->rkcg_rk);
        }

        if (error) {
                /* Log error since caller might not check
                 * *assign() return value. */
                rd_kafka_log(rkcg->rkcg_rk, LOG_WARNING, "ASSIGN",
                             "Group \"%s\": application *assign() call "
                             "failed: %s",
                             rkcg->rkcg_group_id->str,
                             rd_kafka_error_string(error));
        }

        rd_kafka_op_error_reply(rko, error);
}

/**
 * @returns true if the session timeout has expired (due to no successful
 *          Heartbeats in session.timeout.ms) and triggers a rebalance.
 */
static rd_bool_t rd_kafka_cgrp_session_timeout_check(rd_kafka_cgrp_t *rkcg,
                                                     rd_ts_t now) {
        rd_ts_t delta;
        char buf[256];

        if (unlikely(!rkcg->rkcg_ts_session_timeout))
                return rd_true; /* Session has expired */

        delta = now - rkcg->rkcg_ts_session_timeout;
        if (likely(delta < 0))
                return rd_false;

        delta += rkcg->rkcg_rk->rk_conf.group_session_timeout_ms * 1000;

        rd_snprintf(buf, sizeof(buf),
                    "Consumer group session timed out (in join-state %s) after "
                    "%" PRId64
                    " ms without a successful response from the "
                    "group coordinator (broker %" PRId32 ", last error was %s)",
                    rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
                    delta / 1000, rkcg->rkcg_coord_id,
                    rd_kafka_err2str(rkcg->rkcg_last_heartbeat_err));

        rkcg->rkcg_last_heartbeat_err = RD_KAFKA_RESP_ERR_NO_ERROR;

        rd_kafka_log(rkcg->rkcg_rk, LOG_WARNING, "SESSTMOUT",
                     "%s: revoking assignment and rejoining group", buf);

        /* Prevent further rebalances */
        rkcg->rkcg_ts_session_timeout = 0;

        /* Timing out invalidates the member id, reset it
         * now to avoid an ERR_UNKNOWN_MEMBER_ID on the next join. */
        rd_kafka_cgrp_set_member_id(rkcg, "");

        /* Revoke and rebalance */
        rd_kafka_cgrp_revoke_all_rejoin_maybe(rkcg, rd_true /*lost*/,
                                              rd_true /*initiating*/, buf);

        return rd_true;
}


/**
 * @brief Apply the next waiting subscribe/unsubscribe, if any.
 */
static void rd_kafka_cgrp_apply_next_subscribe(rd_kafka_cgrp_t *rkcg) {
        rd_assert(rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_INIT);

        if (rkcg->rkcg_next_subscription) {
                rd_kafka_topic_partition_list_t *next_subscription =
                    rkcg->rkcg_next_subscription;
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "SUBSCRIBE",
                             "Group \"%s\": invoking waiting postponed "
                             "subscribe",
                             rkcg->rkcg_group_id->str);
                rkcg->rkcg_next_subscription = NULL;
                rd_kafka_cgrp_subscribe(rkcg, next_subscription);

        } else if (rkcg->rkcg_next_unsubscribe) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "SUBSCRIBE",
                             "Group \"%s\": invoking waiting postponed "
                             "unsubscribe",
                             rkcg->rkcg_group_id->str);
                rkcg->rkcg_next_unsubscribe = rd_false;
                rd_kafka_cgrp_unsubscribe(rkcg, rd_true /*Leave*/);
        }
}

/**
 * Client group's join state handling
 */
static void rd_kafka_cgrp_join_state_serve(rd_kafka_cgrp_t *rkcg) {
        rd_ts_t now = rd_clock();

        if (unlikely(rd_kafka_fatal_error_code(rkcg->rkcg_rk)))
                return;

        switch (rkcg->rkcg_join_state) {
        case RD_KAFKA_CGRP_JOIN_STATE_INIT:
                if (unlikely(rd_kafka_cgrp_awaiting_response(rkcg)))
                        break;

                /* If there is a next subscription, apply it.  */
                rd_kafka_cgrp_apply_next_subscribe(rkcg);

                /* If we have a subscription start the join process. */
                if (!rkcg->rkcg_subscription)
                        break;

                if (rd_interval_immediate(&rkcg->rkcg_join_intvl, 1000 * 1000,
                                          now) > 0)
                        rd_kafka_cgrp_join(
                            rkcg, -1 /* current subscription version */);
                break;

        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_JOIN:
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_METADATA:
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_SYNC:
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_TO_COMPLETE:
                /* FIXME: I think we might have to send heartbeats in
                 *        in WAIT_INCR_UNASSIGN, yes-no? */
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_INCR_UNASSIGN_TO_COMPLETE:
                break;

        case RD_KAFKA_CGRP_JOIN_STATE_STEADY:
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_CALL:
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_CALL:
                if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_SUBSCRIPTION &&
                    rd_interval(
                        &rkcg->rkcg_heartbeat_intvl,
                        rkcg->rkcg_rk->rk_conf.group_heartbeat_intvl_ms * 1000,
                        now) > 0)
                        rd_kafka_cgrp_heartbeat(rkcg);
                break;
        }
}


void rd_kafka_cgrp_consumer_group_heartbeat(rd_kafka_cgrp_t *rkcg,
                                            rd_bool_t full_request,
                                            rd_bool_t send_ack) {

        rd_kafkap_str_t *rkcg_group_instance_id                   = NULL;
        rd_kafkap_str_t *rkcg_client_rack                         = NULL;
        int max_poll_interval_ms                                  = -1;
        rd_kafka_topic_partition_list_t *rkcg_subscription_topics = NULL;
        rd_kafkap_str_t *rkcg_subscription_regex                  = NULL;
        rd_kafkap_str_t *rkcg_group_remote_assignor               = NULL;
        rd_kafka_topic_partition_list_t *rkcg_group_assignment    = NULL;
        int32_t member_epoch = rkcg->rkcg_generation_id;
        if (member_epoch < 0)
                member_epoch = 0;


        rkcg->rkcg_flags &= ~RD_KAFKA_CGRP_F_MAX_POLL_EXCEEDED;
        rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT;

        if (full_request) {
                rkcg_group_instance_id = rkcg->rkcg_group_instance_id;
                rkcg_client_rack       = rkcg->rkcg_client_rack;
                max_poll_interval_ms =
                    rkcg->rkcg_rk->rk_conf.max_poll_interval_ms;
                rkcg_subscription_topics   = rkcg->rkcg_subscription_topics;
                rkcg_subscription_regex    = rkcg->rkcg_subscription_regex;
                rkcg_group_remote_assignor = rkcg->rkcg_group_remote_assignor;
        }

        if (send_ack) {
                rkcg_group_assignment = rkcg->rkcg_target_assignment;
                rkcg->rkcg_consumer_flags |=
                    RD_KAFKA_CGRP_CONSUMER_F_SENDING_ACK;

                if (rd_kafka_is_dbg(rkcg->rkcg_rk, CGRP)) {
                        char rkcg_group_assignment_str[512] = "NULL";

                        if (rkcg_group_assignment) {
                                rd_kafka_topic_partition_list_str(
                                    rkcg_group_assignment,
                                    rkcg_group_assignment_str,
                                    sizeof(rkcg_group_assignment_str), 0);
                        }

                        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "HEARTBEAT",
                                     "Acknowledging target assignment \"%s\"",
                                     rkcg_group_assignment_str);
                }
        } else if (full_request) {
                rkcg_group_assignment = rkcg->rkcg_current_assignment;
        }

        if (rd_kafka_cgrp_consumer_subscription_preconditions_met(rkcg) ||
            rkcg->rkcg_consumer_flags &
                RD_KAFKA_CGRP_CONSUMER_F_SENDING_NEW_SUBSCRIPTION) {
                rkcg->rkcg_consumer_flags =
                    (rkcg->rkcg_consumer_flags &
                     ~RD_KAFKA_CGRP_CONSUMER_F_SEND_NEW_SUBSCRIPTION) |
                    RD_KAFKA_CGRP_CONSUMER_F_SENDING_NEW_SUBSCRIPTION;
                rkcg_subscription_topics = rkcg->rkcg_subscription_topics;
                rkcg_subscription_regex  = rkcg->rkcg_subscription_regex;

                if (rd_kafka_is_dbg(rkcg->rkcg_rk, CGRP)) {
                        char rkcg_new_subscription_str[512] = "NULL";

                        if (rkcg->rkcg_subscription) {
                                rd_kafka_topic_partition_list_str(
                                    rkcg->rkcg_subscription,
                                    rkcg_new_subscription_str,
                                    sizeof(rkcg_new_subscription_str), 0);
                        }

                        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "HEARTBEAT",
                                     "Sending new subscription \"%s\"",
                                     rkcg_new_subscription_str);
                }
        }

        rkcg->rkcg_expedite_heartbeat_retries++;
        rd_kafka_ConsumerGroupHeartbeatRequest(
            rkcg->rkcg_coord, rkcg->rkcg_group_id, rkcg->rkcg_member_id,
            member_epoch, rkcg_group_instance_id, rkcg_client_rack,
            max_poll_interval_ms, rkcg_subscription_topics,
            rkcg_subscription_regex, rkcg_group_remote_assignor,
            rkcg_group_assignment, RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
            rd_kafka_cgrp_handle_ConsumerGroupHeartbeat, NULL);
}

static rd_bool_t
rd_kafka_cgrp_consumer_heartbeat_preconditions_met(rd_kafka_cgrp_t *rkcg) {
        rd_dassert(
            !(rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_INIT &&
              rkcg->rkcg_flags & RD_KAFKA_CGRP_F_LEAVE_ON_UNASSIGN_DONE));

        if (!(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_SUBSCRIPTION))
                return rd_false;

        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT)
                return rd_false;

        if (rkcg->rkcg_consumer_flags &
            RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN_TO_COMPLETE)
                return rd_false;

        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_MAX_POLL_EXCEEDED &&
            rd_kafka_max_poll_exceeded(rkcg->rkcg_rk))
                return rd_false;

        if (rd_kafka_cgrp_will_leave(rkcg))
                return rd_false;

        return rd_true;
}

void rd_kafka_cgrp_consumer_serve(rd_kafka_cgrp_t *rkcg) {
        rd_bool_t full_request = rkcg->rkcg_consumer_flags &
                                 RD_KAFKA_CGRP_CONSUMER_F_SEND_FULL_REQUEST;
        rd_bool_t send_ack = rd_false;

        if (unlikely(rd_kafka_fatal_error_code(rkcg->rkcg_rk)))
                return;

        if (unlikely(rkcg->rkcg_consumer_flags &
                     RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN)) {
                if (RD_KAFKA_CGRP_REBALANCING(rkcg))
                        return;
                rkcg->rkcg_consumer_flags &=
                    ~RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN;
                rkcg->rkcg_consumer_flags |=
                    RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN_TO_COMPLETE;

                rd_kafka_dbg(
                    rkcg->rkcg_rk, CGRP, "HEARTBEAT",
                    "Revoking assignment as lost an rejoining in join state %s",
                    rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);

                rd_kafka_cgrp_revoke_all_rejoin(rkcg, rd_true, rd_true,
                                                "member fenced - rejoining");
        }

        switch (rkcg->rkcg_join_state) {
        case RD_KAFKA_CGRP_JOIN_STATE_INIT:
                rkcg->rkcg_consumer_flags &=
                    ~RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN_TO_COMPLETE;
                rd_kafka_cgrp_consumer_apply_next_subscribe(rkcg);
                full_request = rd_true;
                break;
        case RD_KAFKA_CGRP_JOIN_STATE_STEADY:
                if (rkcg->rkcg_consumer_flags &
                    RD_KAFKA_CGRP_CONSUMER_F_WAIT_ACK) {
                        send_ack = rd_true;
                }
                break;
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_CALL:
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_CALL:
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_INCR_UNASSIGN_TO_COMPLETE:
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_TO_COMPLETE:
                break;
        default:
                rd_assert(!*"unexpected state");
        }

        if (rd_kafka_cgrp_consumer_heartbeat_preconditions_met(rkcg)) {
                rd_ts_t next_heartbeat =
                    rd_interval(&rkcg->rkcg_heartbeat_intvl,
                                rkcg->rkcg_heartbeat_intvl_ms * 1000, 0);
                if (next_heartbeat > 0) {
                        rd_kafka_cgrp_consumer_group_heartbeat(
                            rkcg, full_request, send_ack);
                        next_heartbeat = rkcg->rkcg_heartbeat_intvl_ms * 1000;
                } else {
                        next_heartbeat = -1 * next_heartbeat;
                }
                if (likely(rkcg->rkcg_heartbeat_intvl_ms > 0)) {
                        if (rkcg->rkcg_serve_timer.rtmr_next >
                            (rd_clock() + next_heartbeat)) {
                                /* We stop the timer if it expires later
                                 * than expected and restart it below. */
                                rd_kafka_timer_stop(&rkcg->rkcg_rk->rk_timers,
                                                    &rkcg->rkcg_serve_timer, 0);
                        }

                        /* Scheduling a timer yields the main loop so
                         * 'restart' has to be set to false to avoid a tight
                         * loop. */
                        rd_kafka_timer_start_oneshot(
                            &rkcg->rkcg_rk->rk_timers, &rkcg->rkcg_serve_timer,
                            rd_false /*don't restart*/, next_heartbeat,
                            rd_kafka_cgrp_serve_timer_cb, NULL);
                }
        }
}

/**
 * Set new atomic topic subscription (KIP-848).
 *
 * @locality rdkafka main thread
 * @locks none
 */
static rd_kafka_resp_err_t
rd_kafka_cgrp_consumer_subscribe(rd_kafka_cgrp_t *rkcg,
                                 rd_kafka_topic_partition_list_t *rktparlist) {

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_CONSUMER, "SUBSCRIBE",
                     "Group \"%.*s\": subscribe to new %ssubscription "
                     "of %d topics (join-state %s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rktparlist ? "" : "unset ",
                     rktparlist ? rktparlist->cnt : 0,
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);

        /* If the consumer has raised a fatal error treat all subscribes as
           unsubscribe */
        if (rd_kafka_fatal_error_code(rkcg->rkcg_rk)) {
                if (rkcg->rkcg_subscription)
                        rd_kafka_cgrp_unsubscribe(rkcg,
                                                  rd_true /*leave group*/);
                return RD_KAFKA_RESP_ERR__FATAL;
        }

        if (rktparlist) {
                if (rkcg->rkcg_next_subscription)
                        rd_kafka_topic_partition_list_destroy(
                            rkcg->rkcg_next_subscription);
                rkcg->rkcg_next_subscription = rktparlist;

                /* If member is leaving, new subscription
                 * will be applied after the leave
                 * ConsumerGroupHeartbeat */
                if (!rd_kafka_cgrp_will_leave(rkcg))
                        rd_kafka_cgrp_consumer_apply_next_subscribe(rkcg);
        } else {
                rd_kafka_cgrp_consumer_unsubscribe(rkcg,
                                                   rd_true /*leave group*/);
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Call when all incremental unassign operations are done to transition
 *        to the next state.
 */
static void rd_kafka_cgrp_consumer_incr_unassign_done(rd_kafka_cgrp_t *rkcg) {

        /* If this action was underway when a terminate was initiated, it will
         * be left to complete. Now that's done, unassign all partitions */
        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "UNASSIGN",
                             "Group \"%s\" is terminating, initiating full "
                             "unassign",
                             rkcg->rkcg_group_id->str);
                rd_kafka_cgrp_unassign(rkcg);

                /* Leave group, if desired. */
                rd_kafka_cgrp_leave_maybe(rkcg);
                return;
        }

        if (rkcg->rkcg_rebalance_incr_assignment) {
                /* This incremental unassign was part of a normal rebalance
                 * (in which the revoke set was not empty). Immediately
                 * trigger the assign that follows this revoke. The protocol
                 * dictates this should occur even if the new assignment
                 * set is empty.
                 *
                 * Also, since this rebalance had some revoked partitions,
                 * a re-join should occur following the assign.
                 */

                rd_kafka_rebalance_op_incr(
                    rkcg, RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS,
                    rkcg->rkcg_rebalance_incr_assignment,
                    rd_false /* don't rejoin following assign*/,
                    "cooperative assign after revoke");

                rd_kafka_topic_partition_list_destroy(
                    rkcg->rkcg_rebalance_incr_assignment);
                rkcg->rkcg_rebalance_incr_assignment = NULL;

                /* Note: rkcg_rebalance_rejoin is actioned / reset in
                 * rd_kafka_cgrp_incremental_assign call */

        } else if (rkcg->rkcg_rebalance_rejoin) {
                rkcg->rkcg_rebalance_rejoin = rd_false;

                /* There are some cases (lost partitions), where a rejoin
                 * should occur immediately following the unassign (this
                 * is not the case under normal conditions), in which case
                 * the rejoin flag will be set. */

                rd_kafka_cgrp_rejoin(rkcg, "Incremental unassignment done");

        } else {
                /* After this incremental unassignment we're now back in
                 * a steady state. */
                rd_kafka_cgrp_set_join_state(rkcg,
                                             RD_KAFKA_CGRP_JOIN_STATE_STEADY);
                if (rkcg->rkcg_subscription) {
                        rd_kafka_cgrp_start_max_poll_interval_timer(rkcg);
                }
        }
}

/**
 * @brief KIP 848: Called from assignment code when all in progress
 *        assignment/unassignment operations are done, allowing the cgrp to
 *        transition to other states if needed.
 *
 * @param rkcg Consumer group.
 *
 * @remark This may be called spontaneously without any need for a state
 *         change in the rkcg.
 *
 * @locality rdkafka main thread
 * @locks none
 */
static void rd_kafka_cgrp_consumer_assignment_done(rd_kafka_cgrp_t *rkcg) {
        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "ASSIGNDONE",
                     "Group \"%s\": "
                     "assignment operations done in join-state %s "
                     "(rebalance rejoin=%s)",
                     rkcg->rkcg_group_id->str,
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
                     RD_STR_ToF(rkcg->rkcg_rebalance_rejoin));

        switch (rkcg->rkcg_join_state) {
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_TO_COMPLETE:
                rd_kafka_cgrp_unassign_done(rkcg);
                break;

        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_INCR_UNASSIGN_TO_COMPLETE:
                rd_kafka_cgrp_consumer_incr_unassign_done(rkcg);
                break;

        case RD_KAFKA_CGRP_JOIN_STATE_INIT:
        case RD_KAFKA_CGRP_JOIN_STATE_STEADY: {

                rd_bool_t not_in_group = rd_false;
                /*
                 * There maybe a case when there are no assignments are
                 * assigned to this consumer. In this case, while terminating
                 * the consumer can be in STEADY or INIT state and won't go
                 * to intermediate state. In this scenario, last leave call is
                 * done from here.
                 */
                not_in_group |= rd_kafka_cgrp_leave_maybe(rkcg);

                /* Check if cgrp is trying to terminate, which is safe to do
                 * in these two states. Otherwise we'll need to wait for
                 * the current state to decommission. */
                not_in_group |= rd_kafka_cgrp_try_terminate(rkcg);

                if (not_in_group)
                        break;

                if (rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_INIT) {
                        rd_kafka_cgrp_consumer_expedite_next_heartbeat(
                            rkcg, "Assignment Done: in init state");
                } else if (rkcg->rkcg_rebalance_rejoin) {
                        /* No need to expedite the HB here as it's being
                         * expedited in the rejoin call.*/
                        rkcg->rkcg_rebalance_rejoin = rd_false;
                        rd_kafka_cgrp_rejoin(
                            rkcg,
                            "Assignment Done: rejoining group to redistribute "
                            "previously owned partitions to other "
                            "group members");
                } else if (rkcg->rkcg_consumer_flags &
                           RD_KAFKA_CGRP_CONSUMER_F_WAIT_ACK) {
                        rd_kafka_cgrp_consumer_expedite_next_heartbeat(
                            rkcg,
                            "Assignment Done: in steady state, waiting for "
                            "ack");
                }
                break;
        }

        default:
                break;
        }
}

void rd_kafka_cgrp_consumer_expedite_next_heartbeat(rd_kafka_cgrp_t *rkcg,
                                                    const char *reason) {
        if (rkcg->rkcg_group_protocol != RD_KAFKA_GROUP_PROTOCOL_CONSUMER)
                return;

        rd_kafka_t *rk = rkcg->rkcg_rk;
        /* Calculate the exponential backoff. */
        int64_t backoff = 0;
        if (rkcg->rkcg_expedite_heartbeat_retries)
                backoff = 1 << (rkcg->rkcg_expedite_heartbeat_retries - 1);

        /* We are multiplying by 10 as (backoff_ms * percent * 1000)/100 ->
         * backoff_ms * jitter * 10 */
        backoff = rd_jitter(100 - RD_KAFKA_RETRY_JITTER_PERCENT,
                            100 + RD_KAFKA_RETRY_JITTER_PERCENT) *
                  backoff * 10;

        /* Backoff is limited by retry_backoff_max_ms. */
        if (backoff > rk->rk_conf.retry_backoff_max_ms * 1000)
                backoff = rk->rk_conf.retry_backoff_max_ms * 1000;

        /* Reset the interval as it happened `rkcg_heartbeat_intvl_ms`
         * milliseconds ago. */
        rd_interval_reset_to_now(&rkcg->rkcg_heartbeat_intvl,
                                 rd_clock() -
                                     rkcg->rkcg_heartbeat_intvl_ms * 1000);
        /* Set the exponential backoff. */
        rd_interval_backoff(&rkcg->rkcg_heartbeat_intvl, backoff);

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "HEARTBEAT",
                     "Expediting next heartbeat"
                     ", with backoff %" PRId64 ": %s",
                     backoff, reason);

        /* Scheduling the timer awakes main loop too. */
        rd_kafka_timer_start_oneshot(&rkcg->rkcg_rk->rk_timers,
                                     &rkcg->rkcg_serve_timer, rd_true, backoff,
                                     rd_kafka_cgrp_serve_timer_cb, NULL);
}

/**
 * Client group handling.
 * Called from main thread to serve the operational aspects of a cgrp.
 */
void rd_kafka_cgrp_serve(rd_kafka_cgrp_t *rkcg) {
        rd_kafka_broker_t *rkb = rkcg->rkcg_coord;
        int rkb_state          = RD_KAFKA_BROKER_STATE_INIT;
        rd_ts_t now;

        if (rkb) {
                rd_kafka_broker_lock(rkb);
                rkb_state = rkb->rkb_state;
                rd_kafka_broker_unlock(rkb);

                /* Go back to querying state if we lost the current coordinator
                 * connection. */
                if (rkb_state < RD_KAFKA_BROKER_STATE_UP &&
                    rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_UP)
                        rd_kafka_cgrp_set_state(
                            rkcg, RD_KAFKA_CGRP_STATE_QUERY_COORD);
        }

        now = rd_clock();

        /* Check for cgrp termination */
        if (unlikely(rd_kafka_cgrp_try_terminate(rkcg))) {
                rd_kafka_cgrp_terminated(rkcg);
                return; /* cgrp terminated */
        }

        /* Bail out if we're terminating. */
        if (unlikely(rd_kafka_terminating(rkcg->rkcg_rk)))
                return;

        /* Check session timeout regardless of current coordinator
         * connection state (rkcg_state) */
        if (rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_STEADY)
                rd_kafka_cgrp_session_timeout_check(rkcg, now);

retry:
        switch (rkcg->rkcg_state) {
        case RD_KAFKA_CGRP_STATE_TERM:
                break;

        case RD_KAFKA_CGRP_STATE_INIT:
                rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_QUERY_COORD);
                /* FALLTHRU */

        case RD_KAFKA_CGRP_STATE_QUERY_COORD:
                /* Query for coordinator. */
                if (rd_interval_immediate(&rkcg->rkcg_coord_query_intvl,
                                          500 * 1000, now) > 0)
                        rd_kafka_cgrp_coord_query(rkcg,
                                                  "intervaled in "
                                                  "state query-coord");
                break;

        case RD_KAFKA_CGRP_STATE_WAIT_COORD:
                /* Waiting for FindCoordinator response */
                break;

        case RD_KAFKA_CGRP_STATE_WAIT_BROKER:
                /* See if the group should be reassigned to another broker. */
                if (rd_kafka_cgrp_coord_update(rkcg, rkcg->rkcg_coord_id))
                        goto retry; /* Coordinator changed, retry state-machine
                                     * to speed up next transition. */

                /* Coordinator query */
                if (rd_interval(&rkcg->rkcg_coord_query_intvl, 1000 * 1000,
                                now) > 0)
                        rd_kafka_cgrp_coord_query(rkcg,
                                                  "intervaled in "
                                                  "state wait-broker");
                break;

        case RD_KAFKA_CGRP_STATE_WAIT_BROKER_TRANSPORT:
                /* Waiting for broker transport to come up.
                 * Also make sure broker supports groups. */
                if (rkb_state < RD_KAFKA_BROKER_STATE_UP || !rkb ||
                    !rd_kafka_broker_supports(
                        rkb, RD_KAFKA_FEATURE_BROKER_GROUP_COORD)) {
                        /* Coordinator query */
                        if (rd_interval(&rkcg->rkcg_coord_query_intvl,
                                        1000 * 1000, now) > 0)
                                rd_kafka_cgrp_coord_query(
                                    rkcg,
                                    "intervaled in state "
                                    "wait-broker-transport");

                } else {
                        rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_UP);

                        /* Serve join state to trigger (re)join */
                        if (rkcg->rkcg_group_protocol ==
                            RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                                rd_kafka_cgrp_consumer_serve(rkcg);
                        } else {
                                rd_kafka_cgrp_join_state_serve(rkcg);
                        }

                        /* Serve any pending partitions in the
                         * assignment */
                        rd_kafka_assignment_serve(rkcg->rkcg_rk);
                }
                break;

        case RD_KAFKA_CGRP_STATE_UP:
                /* Move any ops awaiting the coordinator to the ops queue
                 * for reprocessing. */
                rd_kafka_q_concat(rkcg->rkcg_ops, rkcg->rkcg_wait_coord_q);

                /* Relaxed coordinator queries. */
                if (rd_interval(&rkcg->rkcg_coord_query_intvl,
                                rkcg->rkcg_rk->rk_conf.coord_query_intvl_ms *
                                    1000,
                                now) > 0)
                        rd_kafka_cgrp_coord_query(rkcg,
                                                  "intervaled in state up");

                if (rkcg->rkcg_group_protocol ==
                    RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                        rd_kafka_cgrp_consumer_serve(rkcg);
                } else {
                        rd_kafka_cgrp_join_state_serve(rkcg);
                }

                break;
        }

        if (unlikely(rkcg->rkcg_state != RD_KAFKA_CGRP_STATE_UP &&
                     rd_interval(&rkcg->rkcg_timeout_scan_intvl, 1000 * 1000,
                                 now) > 0))
                rd_kafka_cgrp_timeout_scan(rkcg, now);
}



/**
 * Send an op to a cgrp.
 *
 * Locality: any thread
 */
void rd_kafka_cgrp_op(rd_kafka_cgrp_t *rkcg,
                      rd_kafka_toppar_t *rktp,
                      rd_kafka_replyq_t replyq,
                      rd_kafka_op_type_t type,
                      rd_kafka_resp_err_t err) {
        rd_kafka_op_t *rko;

        rko             = rd_kafka_op_new(type);
        rko->rko_err    = err;
        rko->rko_replyq = replyq;

        if (rktp)
                rko->rko_rktp = rd_kafka_toppar_keep(rktp);

        rd_kafka_q_enq(rkcg->rkcg_ops, rko);
}

/**
 * @brief Handle cgrp queue op.
 * @locality rdkafka main thread
 * @locks none
 */
static rd_kafka_op_res_t rd_kafka_cgrp_op_serve(rd_kafka_t *rk,
                                                rd_kafka_q_t *rkq,
                                                rd_kafka_op_t *rko,
                                                rd_kafka_q_cb_type_t cb_type,
                                                void *opaque) {
        rd_kafka_cgrp_t *rkcg = opaque;
        rd_kafka_toppar_t *rktp;
        rd_kafka_resp_err_t err;
        const int silent_op = rko->rko_type == RD_KAFKA_OP_RECV_BUF;
        if (unlikely(rd_atomic32_get(&rkcg->rkcg_terminated) == rd_true)) {
                if (rko)
                        rd_kafka_op_destroy(rko);
                return RD_KAFKA_OP_RES_HANDLED;
        }


        rktp = rko->rko_rktp;

        if (rktp && !silent_op)
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CGRP, "CGRPOP",
                    "Group \"%.*s\" received op %s in state %s "
                    "(join-state %s) for %.*s [%" PRId32 "]",
                    RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                    rd_kafka_op2str(rko->rko_type),
                    rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                    rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
                    RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                    rktp->rktp_partition);
        else if (!silent_op)
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CGRP, "CGRPOP",
                    "Group \"%.*s\" received op %s in state %s "
                    "(join-state %s)",
                    RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                    rd_kafka_op2str(rko->rko_type),
                    rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                    rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);

        switch ((int)rko->rko_type) {
        case RD_KAFKA_OP_NAME:
                /* Return the currently assigned member id. */
                if (rkcg->rkcg_member_id)
                        rko->rko_u.name.str =
                            RD_KAFKAP_STR_DUP(rkcg->rkcg_member_id);
                rd_kafka_op_reply(rko, 0);
                rko = NULL;
                break;

        case RD_KAFKA_OP_CG_METADATA:
                /* Return the current consumer group metadata. */
                rko->rko_u.cg_metadata =
                    rkcg->rkcg_member_id
                        ? rd_kafka_consumer_group_metadata_new_with_genid(
                              rkcg->rkcg_rk->rk_conf.group_id_str,
                              rkcg->rkcg_generation_id,
                              rkcg->rkcg_member_id->str,
                              rkcg->rkcg_rk->rk_conf.group_instance_id)
                        : NULL;
                rd_kafka_op_reply(rko, RD_KAFKA_RESP_ERR_NO_ERROR);
                rko = NULL;
                break;

        case RD_KAFKA_OP_OFFSET_FETCH:
                if (rkcg->rkcg_state != RD_KAFKA_CGRP_STATE_UP ||
                    (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE)) {
                        rd_kafka_op_handle_OffsetFetch(
                            rkcg->rkcg_rk, NULL, RD_KAFKA_RESP_ERR__WAIT_COORD,
                            NULL, NULL, rko);
                        rko = NULL; /* rko freed by handler */
                        break;
                }

                rd_kafka_OffsetFetchRequest(
                    rkcg->rkcg_coord, rk->rk_group_id->str,
                    rko->rko_u.offset_fetch.partitions, rd_false, -1, NULL,
                    rko->rko_u.offset_fetch.require_stable_offsets,
                    0, /* Timeout */
                    RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
                    rd_kafka_op_handle_OffsetFetch, rko);
                rko = NULL; /* rko now owned by request */
                break;

        case RD_KAFKA_OP_PARTITION_JOIN:
                rd_kafka_cgrp_partition_add(rkcg, rktp);

                /* If terminating tell the partition to leave */
                if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE)
                        rd_kafka_toppar_op_fetch_stop(rktp, RD_KAFKA_NO_REPLYQ);
                break;

        case RD_KAFKA_OP_PARTITION_LEAVE:
                rd_kafka_cgrp_partition_del(rkcg, rktp);
                break;

        case RD_KAFKA_OP_OFFSET_COMMIT:
                /* Trigger offsets commit. */
                rd_kafka_cgrp_offsets_commit(rkcg, rko,
                                             /* only set offsets
                                              * if no partitions were
                                              * specified. */
                                             rko->rko_u.offset_commit.partitions
                                                 ? 0
                                                 : 1 /* set_offsets*/,
                                             rko->rko_u.offset_commit.reason);
                rko = NULL; /* rko now owned by request */
                break;

        case RD_KAFKA_OP_COORD_QUERY:
                rd_kafka_cgrp_coord_query(
                    rkcg,
                    rko->rko_err ? rd_kafka_err2str(rko->rko_err) : "from op");
                break;

        case RD_KAFKA_OP_SUBSCRIBE:
                /* We just want to avoid reaching max poll interval,
                 * without anything else is done on poll. */
                rd_atomic64_set(&rk->rk_ts_last_poll, rd_clock());

                /* New atomic subscription (may be NULL) */
                if (rkcg->rkcg_group_protocol ==
                    RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                        err = rd_kafka_cgrp_consumer_subscribe(
                            rkcg, rko->rko_u.subscribe.topics);
                } else {
                        err = rd_kafka_cgrp_subscribe(
                            rkcg, rko->rko_u.subscribe.topics);
                }

                if (!err) /* now owned by rkcg */
                        rko->rko_u.subscribe.topics = NULL;

                rd_kafka_op_reply(rko, err);
                rko = NULL;
                break;

        case RD_KAFKA_OP_ASSIGN:
                rd_kafka_cgrp_handle_assign_op(rkcg, rko);
                rko = NULL;
                break;

        case RD_KAFKA_OP_GET_SUBSCRIPTION:
                if (rkcg->rkcg_next_subscription)
                        rko->rko_u.subscribe.topics =
                            rd_kafka_topic_partition_list_copy(
                                rkcg->rkcg_next_subscription);
                else if (rkcg->rkcg_next_unsubscribe)
                        rko->rko_u.subscribe.topics = NULL;
                else if (rkcg->rkcg_subscription)
                        rko->rko_u.subscribe.topics =
                            rd_kafka_topic_partition_list_copy(
                                rkcg->rkcg_subscription);
                rd_kafka_op_reply(rko, 0);
                rko = NULL;
                break;

        case RD_KAFKA_OP_GET_ASSIGNMENT:
                /* This is the consumer assignment, not the group assignment. */
                rko->rko_u.assign.partitions =
                    rd_kafka_topic_partition_list_copy(
                        rkcg->rkcg_rk->rk_consumer.assignment.all);

                rd_kafka_op_reply(rko, 0);
                rko = NULL;
                break;

        case RD_KAFKA_OP_GET_REBALANCE_PROTOCOL:
                rko->rko_u.rebalance_protocol.str =
                    rd_kafka_rebalance_protocol2str(
                        rd_kafka_cgrp_rebalance_protocol(rkcg));
                rd_kafka_op_reply(rko, RD_KAFKA_RESP_ERR_NO_ERROR);
                rko = NULL;
                break;

        case RD_KAFKA_OP_TERMINATE:
                rd_kafka_cgrp_terminate0(rkcg, rko);
                rko = NULL; /* terminate0() takes ownership */
                break;

        default:
                rd_kafka_assert(rkcg->rkcg_rk, !*"unknown type");
                break;
        }

        if (rko)
                rd_kafka_op_destroy(rko);

        return RD_KAFKA_OP_RES_HANDLED;
}

void rd_kafka_cgrp_set_member_id(rd_kafka_cgrp_t *rkcg, const char *member_id) {
        if (rkcg->rkcg_member_id && member_id &&
            !rd_kafkap_str_cmp_str(rkcg->rkcg_member_id, member_id))
                return; /* No change */

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "MEMBERID",
                     "Group \"%.*s\": updating member id \"%s\" -> \"%s\"",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rkcg->rkcg_member_id ? rkcg->rkcg_member_id->str
                                          : "(not-set)",
                     member_id ? member_id : "(not-set)");

        if (rkcg->rkcg_member_id) {
                rd_kafkap_str_destroy(rkcg->rkcg_member_id);
                rkcg->rkcg_member_id = NULL;
        }

        if (member_id)
                rkcg->rkcg_member_id = rd_kafkap_str_new(member_id, -1);
}


/**
 * @brief Determine owned partitions that no longer exist (partitions in
 *        deleted or re-created topics).
 */
static rd_kafka_topic_partition_list_t *
rd_kafka_cgrp_owned_but_not_exist_partitions(rd_kafka_cgrp_t *rkcg) {
        rd_kafka_topic_partition_list_t *result = NULL;
        const rd_kafka_topic_partition_t *curr;

        if (!rkcg->rkcg_group_assignment)
                return NULL;

        RD_KAFKA_TPLIST_FOREACH(curr, rkcg->rkcg_group_assignment) {
                if (rd_list_find(rkcg->rkcg_subscribed_topics, curr->topic,
                                 rd_kafka_topic_info_topic_cmp))
                        continue;

                if (!result)
                        result = rd_kafka_topic_partition_list_new(
                            rkcg->rkcg_group_assignment->cnt);

                rd_kafka_topic_partition_list_add_copy(result, curr);
        }

        return result;
}


/**
 * @brief Check if the latest metadata affects the current subscription:
 * - matched topic added
 * - matched topic removed
 * - matched topic's partition count change
 *
 * @locks none
 * @locality rdkafka main thread
 */
void rd_kafka_cgrp_metadata_update_check(rd_kafka_cgrp_t *rkcg,
                                         rd_bool_t do_join) {
        rd_list_t *tinfos;
        rd_kafka_topic_partition_list_t *errored;
        rd_bool_t changed;

        rd_kafka_assert(NULL, thrd_is_current(rkcg->rkcg_rk->rk_thread));

        if (rkcg->rkcg_group_protocol != RD_KAFKA_GROUP_PROTOCOL_CLASSIC)
                return;

        if (!rkcg->rkcg_subscription || rkcg->rkcg_subscription->cnt == 0)
                return;

        /*
         * Unmatched topics will be added to the errored list.
         */
        errored = rd_kafka_topic_partition_list_new(0);

        /*
         * Create a list of the topics in metadata that matches our subscription
         */
        tinfos = rd_list_new(rkcg->rkcg_subscription->cnt,
                             rd_kafka_topic_info_destroy_free);

        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WILDCARD_SUBSCRIPTION)
                rd_kafka_metadata_topic_match(rkcg->rkcg_rk, tinfos,
                                              rkcg->rkcg_subscription, errored);
        else
                rd_kafka_metadata_topic_filter(
                    rkcg->rkcg_rk, tinfos, rkcg->rkcg_subscription, errored);


        /*
         * Propagate consumer errors for any non-existent or errored topics.
         * The function takes ownership of errored.
         */
        rd_kafka_propagate_consumer_topic_errors(
            rkcg, errored, "Subscribed topic not available");

        /*
         * Update effective list of topics (takes ownership of \c tinfos)
         */
        changed = rd_kafka_cgrp_update_subscribed_topics(rkcg, tinfos);

        if (!do_join ||
            (!changed &&
             /* If we get the same effective list of topics as last time around,
              * but the join is waiting for this metadata query to complete,
              * then we should not return here but follow through with the
              * (re)join below. */
             rkcg->rkcg_join_state != RD_KAFKA_CGRP_JOIN_STATE_WAIT_METADATA))
                return;

        /* List of subscribed topics changed, trigger rejoin. */
        rd_kafka_dbg(rkcg->rkcg_rk,
                     CGRP | RD_KAFKA_DBG_METADATA | RD_KAFKA_DBG_CONSUMER,
                     "REJOIN",
                     "Group \"%.*s\": "
                     "subscription updated from metadata change: "
                     "rejoining group in state %s",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);

        if (rd_kafka_cgrp_rebalance_protocol(rkcg) ==
            RD_KAFKA_REBALANCE_PROTOCOL_COOPERATIVE) {

                /* Partitions from deleted topics */
                rd_kafka_topic_partition_list_t *owned_but_not_exist =
                    rd_kafka_cgrp_owned_but_not_exist_partitions(rkcg);

                if (owned_but_not_exist) {
                        rd_kafka_cgrp_assignment_set_lost(
                            rkcg, "%d subscribed topic(s) no longer exist",
                            owned_but_not_exist->cnt);

                        rd_kafka_rebalance_op_incr(
                            rkcg, RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS,
                            owned_but_not_exist,
                            rkcg->rkcg_group_leader.members != NULL
                            /* Rejoin group following revoke's
                             * unassign if we are leader and consumer
                             * group protocol is GENERIC */
                            ,
                            "topics not available");
                        rd_kafka_topic_partition_list_destroy(
                            owned_but_not_exist);

                } else {
                        /* Nothing to revoke, rejoin group if we are the
                         * leader.
                         * The KIP says to rejoin the group on metadata
                         * change only if we're the leader. But what if a
                         * non-leader is subscribed to a regex that the others
                         * aren't?
                         * Going against the KIP and rejoining here. */
                        rd_kafka_cgrp_rejoin(
                            rkcg,
                            "Metadata for subscribed topic(s) has "
                            "changed");
                }

        } else {
                /* EAGER */
                rd_kafka_cgrp_revoke_rejoin(rkcg,
                                            "Metadata for subscribed topic(s) "
                                            "has changed");
        }

        /* We shouldn't get stuck in this state. */
        rd_dassert(rkcg->rkcg_join_state !=
                   RD_KAFKA_CGRP_JOIN_STATE_WAIT_METADATA);
}


rd_kafka_consumer_group_metadata_t *
rd_kafka_consumer_group_metadata_new(const char *group_id) {
        rd_kafka_consumer_group_metadata_t *cgmetadata;

        cgmetadata = rd_kafka_consumer_group_metadata_new_with_genid(
            group_id, -1, "", NULL);

        return cgmetadata;
}

rd_kafka_consumer_group_metadata_t *
rd_kafka_consumer_group_metadata_new_with_genid(const char *group_id,
                                                int32_t generation_id,
                                                const char *member_id,
                                                const char *group_instance_id) {
        rd_kafka_consumer_group_metadata_t *cgmetadata;

        cgmetadata                = rd_calloc(1, sizeof(*cgmetadata));
        cgmetadata->group_id      = rd_strdup(group_id);
        cgmetadata->generation_id = generation_id;
        cgmetadata->member_id     = rd_strdup(member_id);
        if (group_instance_id)
                cgmetadata->group_instance_id = rd_strdup(group_instance_id);

        return cgmetadata;
}

rd_kafka_consumer_group_metadata_t *
rd_kafka_consumer_group_metadata(rd_kafka_t *rk) {
        rd_kafka_consumer_group_metadata_t *cgmetadata;
        rd_kafka_op_t *rko;
        rd_kafka_cgrp_t *rkcg;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return NULL;

        rko = rd_kafka_op_req2(rkcg->rkcg_ops, RD_KAFKA_OP_CG_METADATA);
        if (!rko)
                return NULL;

        cgmetadata             = rko->rko_u.cg_metadata;
        rko->rko_u.cg_metadata = NULL;
        rd_kafka_op_destroy(rko);

        return cgmetadata;
}

const char *rd_kafka_consumer_group_metadata_group_id(
    const rd_kafka_consumer_group_metadata_t *group_metadata) {
        return group_metadata->group_id;
}

const char *rd_kafka_consumer_group_metadata_member_id(
    const rd_kafka_consumer_group_metadata_t *group_metadata) {
        return group_metadata->member_id;
}

const char *rd_kafka_consumer_group_metadata_group_instance_id(
    const rd_kafka_consumer_group_metadata_t *group_metadata) {
        return group_metadata->group_instance_id;
}

int32_t rd_kafka_consumer_group_metadata_generation_id(
    const rd_kafka_consumer_group_metadata_t *group_metadata) {
        return group_metadata->generation_id;
}


void rd_kafka_consumer_group_metadata_destroy(
    rd_kafka_consumer_group_metadata_t *cgmetadata) {
        rd_free(cgmetadata->group_id);
        rd_free(cgmetadata->member_id);
        if (cgmetadata->group_instance_id)
                rd_free(cgmetadata->group_instance_id);
        rd_free(cgmetadata);
}

rd_kafka_consumer_group_metadata_t *rd_kafka_consumer_group_metadata_dup(
    const rd_kafka_consumer_group_metadata_t *cgmetadata) {
        rd_kafka_consumer_group_metadata_t *ret;

        ret                = rd_calloc(1, sizeof(*cgmetadata));
        ret->group_id      = rd_strdup(cgmetadata->group_id);
        ret->generation_id = cgmetadata->generation_id;
        ret->member_id     = rd_strdup(cgmetadata->member_id);
        if (cgmetadata->group_instance_id)
                ret->group_instance_id =
                    rd_strdup(cgmetadata->group_instance_id);

        return ret;
}

/*
 * Consumer group metadata serialization format v2:
 *    "CGMDv2:"<generation_id><group_id>"\0"<member_id>"\0" \
 *    <group_instance_id_is_null>[<group_instance_id>"\0"]
 * Where <group_id> is the group_id string.
 */
static const char rd_kafka_consumer_group_metadata_magic[7] = "CGMDv2:";

rd_kafka_error_t *rd_kafka_consumer_group_metadata_write(
    const rd_kafka_consumer_group_metadata_t *cgmd,
    void **bufferp,
    size_t *sizep) {
        char *buf;
        size_t size;
        size_t of          = 0;
        size_t magic_len   = sizeof(rd_kafka_consumer_group_metadata_magic);
        size_t groupid_len = strlen(cgmd->group_id) + 1;
        size_t generationid_len          = sizeof(cgmd->generation_id);
        size_t member_id_len             = strlen(cgmd->member_id) + 1;
        int8_t group_instance_id_is_null = cgmd->group_instance_id ? 0 : 1;
        size_t group_instance_id_is_null_len =
            sizeof(group_instance_id_is_null);
        size_t group_instance_id_len =
            cgmd->group_instance_id ? strlen(cgmd->group_instance_id) + 1 : 0;

        size = magic_len + groupid_len + generationid_len + member_id_len +
               group_instance_id_is_null_len + group_instance_id_len;

        buf = rd_malloc(size);

        memcpy(buf, rd_kafka_consumer_group_metadata_magic, magic_len);
        of += magic_len;

        memcpy(buf + of, &cgmd->generation_id, generationid_len);
        of += generationid_len;

        memcpy(buf + of, cgmd->group_id, groupid_len);
        of += groupid_len;

        memcpy(buf + of, cgmd->member_id, member_id_len);
        of += member_id_len;

        memcpy(buf + of, &group_instance_id_is_null,
               group_instance_id_is_null_len);
        of += group_instance_id_is_null_len;

        if (!group_instance_id_is_null)
                memcpy(buf + of, cgmd->group_instance_id,
                       group_instance_id_len);
        of += group_instance_id_len;

        rd_assert(of == size);

        *bufferp = buf;
        *sizep   = size;

        return NULL;
}


/*
 * Check that a string is printable, returning NULL if not or
 * a pointer immediately after the end of the string NUL
 * terminator if so.
 **/
static const char *str_is_printable(const char *s, const char *end) {
        const char *c;
        for (c = s; *c && c != end; c++)
                if (!isprint((int)*c))
                        return NULL;
        return c + 1;
}


rd_kafka_error_t *rd_kafka_consumer_group_metadata_read(
    rd_kafka_consumer_group_metadata_t **cgmdp,
    const void *buffer,
    size_t size) {
        const char *buf = (const char *)buffer;
        const char *end = buf + size;
        const char *next;
        size_t magic_len = sizeof(rd_kafka_consumer_group_metadata_magic);
        int32_t generation_id;
        size_t generationid_len = sizeof(generation_id);
        const char *group_id;
        const char *member_id;
        int8_t group_instance_id_is_null;
        const char *group_instance_id = NULL;

        if (size < magic_len + generationid_len + 1 + 1 + 1)
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__BAD_MSG,
                                          "Input buffer is too short");

        if (memcmp(buffer, rd_kafka_consumer_group_metadata_magic, magic_len))
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__BAD_MSG,
                                          "Input buffer is not a serialized "
                                          "consumer group metadata object");
        memcpy(&generation_id, buf + magic_len, generationid_len);

        group_id = buf + magic_len + generationid_len;
        next     = str_is_printable(group_id, end);
        if (!next)
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__BAD_MSG,
                                          "Input buffer group id is not safe");

        member_id = next;
        next      = str_is_printable(member_id, end);
        if (!next)
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__BAD_MSG,
                                          "Input buffer member id is not "
                                          "safe");

        group_instance_id_is_null = (int8_t) * (next++);
        if (!group_instance_id_is_null) {
                group_instance_id = next;
                next              = str_is_printable(group_instance_id, end);
                if (!next)
                        return rd_kafka_error_new(RD_KAFKA_RESP_ERR__BAD_MSG,
                                                  "Input buffer group "
                                                  "instance id is not safe");
        }

        if (next != end)
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__BAD_MSG,
                                          "Input buffer bad length");

        *cgmdp = rd_kafka_consumer_group_metadata_new_with_genid(
            group_id, generation_id, member_id, group_instance_id);

        return NULL;
}


static int
unittest_consumer_group_metadata_iteration(const char *group_id,
                                           int32_t generation_id,
                                           const char *member_id,
                                           const char *group_instance_id) {
        rd_kafka_consumer_group_metadata_t *cgmd;
        void *buffer, *buffer2;
        size_t size, size2;
        rd_kafka_error_t *error;

        cgmd = rd_kafka_consumer_group_metadata_new_with_genid(
            group_id, generation_id, member_id, group_instance_id);
        RD_UT_ASSERT(cgmd != NULL, "failed to create metadata");

        error = rd_kafka_consumer_group_metadata_write(cgmd, &buffer, &size);
        RD_UT_ASSERT(!error, "metadata_write failed: %s",
                     rd_kafka_error_string(error));

        rd_kafka_consumer_group_metadata_destroy(cgmd);

        cgmd  = NULL;
        error = rd_kafka_consumer_group_metadata_read(&cgmd, buffer, size);
        RD_UT_ASSERT(!error, "metadata_read failed: %s",
                     rd_kafka_error_string(error));

        /* Serialize again and compare buffers */
        error = rd_kafka_consumer_group_metadata_write(cgmd, &buffer2, &size2);
        RD_UT_ASSERT(!error, "metadata_write failed: %s",
                     rd_kafka_error_string(error));

        RD_UT_ASSERT(size == size2 && !memcmp(buffer, buffer2, size),
                     "metadata_read/write size or content mismatch: "
                     "size %" PRIusz ", size2 %" PRIusz,
                     size, size2);

        rd_kafka_consumer_group_metadata_destroy(cgmd);
        rd_free(buffer);
        rd_free(buffer2);

        return 0;
}


static int unittest_consumer_group_metadata(void) {
        const char *ids[] = {
            "mY. random id:.",
            "0",
            "2222222222222222222222221111111111111111111111111111112222",
            "",
            "NULL",
            NULL,
        };
        int i, j, k, gen_id;
        int ret;
        const char *group_id;
        const char *member_id;
        const char *group_instance_id;

        for (i = 0; ids[i]; i++) {
                for (j = 0; ids[j]; j++) {
                        for (k = 0; ids[k]; k++) {
                                for (gen_id = -1; gen_id < 1; gen_id++) {
                                        group_id          = ids[i];
                                        member_id         = ids[j];
                                        group_instance_id = ids[k];
                                        if (strcmp(group_instance_id, "NULL") ==
                                            0)
                                                group_instance_id = NULL;
                                        ret =
                                            unittest_consumer_group_metadata_iteration(
                                                group_id, gen_id, member_id,
                                                group_instance_id);
                                        if (ret)
                                                return ret;
                                }
                        }
                }
        }

        RD_UT_PASS();
}


static int unittest_set_intersect(void) {
        size_t par_cnt = 10;
        map_toppar_member_info_t *dst;
        rd_kafka_topic_partition_t *toppar;
        PartitionMemberInfo_t *v;
        char *id            = "id";
        rd_kafkap_str_t id1 = RD_KAFKAP_STR_INITIALIZER;
        rd_kafkap_str_t id2 = RD_KAFKAP_STR_INITIALIZER;
        rd_kafka_group_member_t *gm1;
        rd_kafka_group_member_t *gm2;

        id1.len = 2;
        id1.str = id;
        id2.len = 2;
        id2.str = id;

        map_toppar_member_info_t a = RD_MAP_INITIALIZER(
            par_cnt, rd_kafka_topic_partition_cmp,
            rd_kafka_topic_partition_hash,
            rd_kafka_topic_partition_destroy_free, PartitionMemberInfo_free);

        map_toppar_member_info_t b = RD_MAP_INITIALIZER(
            par_cnt, rd_kafka_topic_partition_cmp,
            rd_kafka_topic_partition_hash,
            rd_kafka_topic_partition_destroy_free, PartitionMemberInfo_free);

        gm1                         = rd_calloc(1, sizeof(*gm1));
        gm1->rkgm_member_id         = &id1;
        gm1->rkgm_group_instance_id = &id1;
        gm2                         = rd_calloc(1, sizeof(*gm2));
        gm2->rkgm_member_id         = &id2;
        gm2->rkgm_group_instance_id = &id2;

        RD_MAP_SET(&a, rd_kafka_topic_partition_new("t1", 4),
                   PartitionMemberInfo_new(gm1, rd_false));
        RD_MAP_SET(&a, rd_kafka_topic_partition_new("t2", 4),
                   PartitionMemberInfo_new(gm1, rd_false));
        RD_MAP_SET(&a, rd_kafka_topic_partition_new("t1", 7),
                   PartitionMemberInfo_new(gm1, rd_false));

        RD_MAP_SET(&b, rd_kafka_topic_partition_new("t2", 7),
                   PartitionMemberInfo_new(gm1, rd_false));
        RD_MAP_SET(&b, rd_kafka_topic_partition_new("t1", 4),
                   PartitionMemberInfo_new(gm2, rd_false));

        dst = rd_kafka_member_partitions_intersect(&a, &b);

        RD_UT_ASSERT(RD_MAP_CNT(&a) == 3, "expected a cnt to be 3 not %d",
                     (int)RD_MAP_CNT(&a));
        RD_UT_ASSERT(RD_MAP_CNT(&b) == 2, "expected b cnt to be 2 not %d",
                     (int)RD_MAP_CNT(&b));
        RD_UT_ASSERT(RD_MAP_CNT(dst) == 1, "expected dst cnt to be 1 not %d",
                     (int)RD_MAP_CNT(dst));

        toppar = rd_kafka_topic_partition_new("t1", 4);
        RD_UT_ASSERT((v = RD_MAP_GET(dst, toppar)), "unexpected element");
        RD_UT_ASSERT(v->members_match, "expected members to match");
        rd_kafka_topic_partition_destroy(toppar);

        RD_MAP_DESTROY(&a);
        RD_MAP_DESTROY(&b);
        RD_MAP_DESTROY(dst);
        rd_free(dst);

        rd_free(gm1);
        rd_free(gm2);

        RD_UT_PASS();
}


static int unittest_set_subtract(void) {
        size_t par_cnt = 10;
        rd_kafka_topic_partition_t *toppar;
        map_toppar_member_info_t *dst;

        map_toppar_member_info_t a = RD_MAP_INITIALIZER(
            par_cnt, rd_kafka_topic_partition_cmp,
            rd_kafka_topic_partition_hash,
            rd_kafka_topic_partition_destroy_free, PartitionMemberInfo_free);

        map_toppar_member_info_t b = RD_MAP_INITIALIZER(
            par_cnt, rd_kafka_topic_partition_cmp,
            rd_kafka_topic_partition_hash,
            rd_kafka_topic_partition_destroy_free, PartitionMemberInfo_free);

        RD_MAP_SET(&a, rd_kafka_topic_partition_new("t1", 4),
                   PartitionMemberInfo_new(NULL, rd_false));
        RD_MAP_SET(&a, rd_kafka_topic_partition_new("t2", 7),
                   PartitionMemberInfo_new(NULL, rd_false));

        RD_MAP_SET(&b, rd_kafka_topic_partition_new("t2", 4),
                   PartitionMemberInfo_new(NULL, rd_false));
        RD_MAP_SET(&b, rd_kafka_topic_partition_new("t1", 4),
                   PartitionMemberInfo_new(NULL, rd_false));
        RD_MAP_SET(&b, rd_kafka_topic_partition_new("t1", 7),
                   PartitionMemberInfo_new(NULL, rd_false));

        dst = rd_kafka_member_partitions_subtract(&a, &b);

        RD_UT_ASSERT(RD_MAP_CNT(&a) == 2, "expected a cnt to be 2 not %d",
                     (int)RD_MAP_CNT(&a));
        RD_UT_ASSERT(RD_MAP_CNT(&b) == 3, "expected b cnt to be 3 not %d",
                     (int)RD_MAP_CNT(&b));
        RD_UT_ASSERT(RD_MAP_CNT(dst) == 1, "expected dst cnt to be 1 not %d",
                     (int)RD_MAP_CNT(dst));

        toppar = rd_kafka_topic_partition_new("t2", 7);
        RD_UT_ASSERT(RD_MAP_GET(dst, toppar), "unexpected element");
        rd_kafka_topic_partition_destroy(toppar);

        RD_MAP_DESTROY(&a);
        RD_MAP_DESTROY(&b);
        RD_MAP_DESTROY(dst);
        rd_free(dst);

        RD_UT_PASS();
}


static int unittest_map_to_list(void) {
        rd_kafka_topic_partition_list_t *list;

        map_toppar_member_info_t map = RD_MAP_INITIALIZER(
            10, rd_kafka_topic_partition_cmp, rd_kafka_topic_partition_hash,
            rd_kafka_topic_partition_destroy_free, PartitionMemberInfo_free);

        RD_MAP_SET(&map, rd_kafka_topic_partition_new("t1", 101),
                   PartitionMemberInfo_new(NULL, rd_false));

        list = rd_kafka_toppar_member_info_map_to_list(&map);

        RD_UT_ASSERT(list->cnt == 1, "expecting list size of 1 not %d.",
                     list->cnt);
        RD_UT_ASSERT(list->elems[0].partition == 101,
                     "expecting partition 101 not %d",
                     list->elems[0].partition);
        RD_UT_ASSERT(!strcmp(list->elems[0].topic, "t1"),
                     "expecting topic 't1', not %s", list->elems[0].topic);

        rd_kafka_topic_partition_list_destroy(list);
        RD_MAP_DESTROY(&map);

        RD_UT_PASS();
}


static int unittest_list_to_map(void) {
        rd_kafka_topic_partition_t *toppar;
        map_toppar_member_info_t *map;
        rd_kafka_topic_partition_list_t *list =
            rd_kafka_topic_partition_list_new(1);

        rd_kafka_topic_partition_list_add(list, "topic1", 201);
        rd_kafka_topic_partition_list_add(list, "topic2", 202);

        map = rd_kafka_toppar_list_to_toppar_member_info_map(list);

        RD_UT_ASSERT(RD_MAP_CNT(map) == 2, "expected map cnt to be 2 not %d",
                     (int)RD_MAP_CNT(map));
        toppar = rd_kafka_topic_partition_new("topic1", 201);
        RD_UT_ASSERT(RD_MAP_GET(map, toppar),
                     "expected topic1 [201] to exist in map");
        rd_kafka_topic_partition_destroy(toppar);
        toppar = rd_kafka_topic_partition_new("topic2", 202);
        RD_UT_ASSERT(RD_MAP_GET(map, toppar),
                     "expected topic2 [202] to exist in map");
        rd_kafka_topic_partition_destroy(toppar);

        RD_MAP_DESTROY(map);
        rd_free(map);
        rd_kafka_topic_partition_list_destroy(list);

        RD_UT_PASS();
}

int unittest_member_metadata_serdes(void) {
        rd_list_t *topics = rd_list_new(0, rd_kafka_topic_info_destroy_free);
        rd_kafka_topic_partition_list_t *owned_partitions =
            rd_kafka_topic_partition_list_new(0);
        rd_kafkap_str_t *rack_id    = rd_kafkap_str_new("myrack", -1);
        const void *userdata        = NULL;
        const int32_t userdata_size = 0;
        const int generation        = 3;
        const char topic_name[]     = "mytopic";
        rd_kafka_group_member_t *rkgm;
        int version;

        rd_list_add(topics, rd_kafka_topic_info_new(topic_name, 3));
        rd_kafka_topic_partition_list_add(owned_partitions, topic_name, 0);
        rkgm = rd_calloc(1, sizeof(*rkgm));

        /* Note that the version variable doesn't actually change the Version
         *  field in the serialized message. It only runs the tests with/without
         *  additional fields added in that particular version. */
        for (version = 0; version <= 3; version++) {
                rd_kafkap_bytes_t *member_metadata;

                /* Serialize. */
                member_metadata =
                    rd_kafka_consumer_protocol_member_metadata_new(
                        topics, userdata, userdata_size,
                        version >= 1 ? owned_partitions : NULL,
                        version >= 2 ? generation : -1,
                        version >= 3 ? rack_id : NULL);

                /* Deserialize. */
                rd_kafka_group_MemberMetadata_consumer_read(NULL, rkgm,
                                                            member_metadata);

                /* Compare results. */
                RD_UT_ASSERT(rkgm->rkgm_subscription->cnt ==
                                 rd_list_cnt(topics),
                             "subscription size should be correct");
                RD_UT_ASSERT(!strcmp(topic_name,
                                     rkgm->rkgm_subscription->elems[0].topic),
                             "subscriptions should be correct");
                RD_UT_ASSERT(rkgm->rkgm_userdata->len == userdata_size,
                             "userdata should have the size 0");
                if (version >= 1)
                        RD_UT_ASSERT(!rd_kafka_topic_partition_list_cmp(
                                         rkgm->rkgm_owned, owned_partitions,
                                         rd_kafka_topic_partition_cmp),
                                     "owned partitions should be same");
                if (version >= 2)
                        RD_UT_ASSERT(generation == rkgm->rkgm_generation,
                                     "generation should be same");
                if (version >= 3)
                        RD_UT_ASSERT(
                            !rd_kafkap_str_cmp(rack_id, rkgm->rkgm_rack_id),
                            "rack id should be same");

                rd_kafka_group_member_clear(rkgm);
                rd_kafkap_bytes_destroy(member_metadata);
        }

        /* Clean up. */
        rd_list_destroy(topics);
        rd_kafka_topic_partition_list_destroy(owned_partitions);
        rd_kafkap_str_destroy(rack_id);
        rd_free(rkgm);

        RD_UT_PASS();
}


/**
 * @brief Consumer group unit tests
 */
int unittest_cgrp(void) {
        int fails = 0;

        fails += unittest_consumer_group_metadata();
        fails += unittest_set_intersect();
        fails += unittest_set_subtract();
        fails += unittest_map_to_list();
        fails += unittest_list_to_map();
        fails += unittest_member_metadata_serdes();

        return fails;
}
