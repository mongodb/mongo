/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
 *               2023, Confluent Inc.
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
#ifndef _RDKAFKA_CGRP_H_
#define _RDKAFKA_CGRP_H_

#include "rdinterval.h"

#include "rdkafka_assignor.h"


/**
 * Client groups implementation
 *
 * Client groups handling for a single cgrp is assigned to a single
 * rd_kafka_broker_t object at any given time.
 * The main thread will call cgrp_serve() to serve its cgrps.
 *
 * This means that the cgrp itself does not need to be locked since it
 * is only ever used from the main thread.
 *
 */


extern const char *rd_kafka_cgrp_join_state_names[];

/**
 * Client group
 */
typedef struct rd_kafka_cgrp_s {
        const rd_kafkap_str_t *rkcg_group_id;
        rd_kafkap_str_t *rkcg_member_id; /* Last assigned MemberId */
        rd_kafkap_str_t *rkcg_group_instance_id;
        const rd_kafkap_str_t *rkcg_client_id;
        rd_kafkap_str_t *rkcg_client_rack;

        enum {
                /* Init state */
                RD_KAFKA_CGRP_STATE_INIT,

                /* Cgrp has been stopped. This is a final state */
                RD_KAFKA_CGRP_STATE_TERM,

                /* Query for group coordinator */
                RD_KAFKA_CGRP_STATE_QUERY_COORD,

                /* Outstanding query, awaiting response */
                RD_KAFKA_CGRP_STATE_WAIT_COORD,

                /* Wait ack from assigned cgrp manager broker thread */
                RD_KAFKA_CGRP_STATE_WAIT_BROKER,

                /* Wait for manager broker thread to connect to broker */
                RD_KAFKA_CGRP_STATE_WAIT_BROKER_TRANSPORT,

                /* Coordinator is up and manager is assigned. */
                RD_KAFKA_CGRP_STATE_UP,
        } rkcg_state;
        rd_ts_t rkcg_ts_statechange; /* Timestamp of last
                                      * state change. */


        enum {
                /* all: join or rejoin, possibly with an existing assignment. */
                RD_KAFKA_CGRP_JOIN_STATE_INIT,

                /* all: JoinGroupRequest sent, awaiting response. */
                RD_KAFKA_CGRP_JOIN_STATE_WAIT_JOIN,

                /* all: MetadataRequest sent, awaiting response.
                 *      While metadata requests may be issued at any time,
                 *      this state is only set upon a proper (re)join. */
                RD_KAFKA_CGRP_JOIN_STATE_WAIT_METADATA,

                /* Follower: SyncGroupRequest sent, awaiting response. */
                RD_KAFKA_CGRP_JOIN_STATE_WAIT_SYNC,

                /* all: waiting for application to call *_assign() */
                RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_CALL,

                /* all: waiting for application to call *_unassign() */
                RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_CALL,

                /* all: waiting for full assignment to decommission */
                RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN_TO_COMPLETE,

                /* all: waiting for partial assignment to decommission */
                RD_KAFKA_CGRP_JOIN_STATE_WAIT_INCR_UNASSIGN_TO_COMPLETE,

                /* all: synchronized and assigned
                 *      may be an empty assignment. */
                RD_KAFKA_CGRP_JOIN_STATE_STEADY,
        } rkcg_join_state;

        /* State when group leader */
        struct {
                rd_kafka_group_member_t *members;
                int member_cnt;
        } rkcg_group_leader;

        rd_kafka_q_t *rkcg_q;            /* Application poll queue */
        rd_kafka_q_t *rkcg_ops;          /* Manager ops queue */
        rd_kafka_q_t *rkcg_wait_coord_q; /* Ops awaiting coord */
        int rkcg_flags;
#define RD_KAFKA_CGRP_F_TERMINATE 0x1 /* Terminate cgrp (async) */
#define RD_KAFKA_CGRP_F_LEAVE_ON_UNASSIGN_DONE                                 \
        0x8 /* Send LeaveGroup when                                            \
             * unassign is done */
#define RD_KAFKA_CGRP_F_SUBSCRIPTION                                           \
        0x10 /* If set:                                                        \
              *   subscription                                                 \
              * else:                                                          \
              *   static assignment */
#define RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT                                   \
        0x20 /* A Heartbeat request                                            \
              * is in transit, dont                                            \
              * send a new one. */
#define RD_KAFKA_CGRP_F_WILDCARD_SUBSCRIPTION                                  \
        0x40 /* Subscription contains                                          \
              * wildcards. */
#define RD_KAFKA_CGRP_F_WAIT_LEAVE                                             \
        0x80 /* Wait for LeaveGroup                                            \
              * to be sent.                                                    \
              * This is used to stall                                          \
              * termination until                                              \
              * the LeaveGroupRequest                                          \
              * is responded to,                                               \
              * otherwise it risks                                             \
              * being dropped in the                                           \
              * output queue when                                              \
              * the broker is destroyed.                                       \
              */
#define RD_KAFKA_CGRP_F_MAX_POLL_EXCEEDED                                      \
        0x100 /**< max.poll.interval.ms                                        \
               *   was exceeded and we                                         \
               *   left the group.                                             \
               *   Do not rejoin until                                         \
               *   the application has                                         \
               *   polled again. */

        rd_interval_t rkcg_coord_query_intvl;  /* Coordinator query intvl*/
        rd_interval_t rkcg_heartbeat_intvl;    /* Heartbeat intvl */
        rd_kafka_timer_t rkcg_serve_timer;     /* Timer for next serve. */
        int rkcg_heartbeat_intvl_ms;           /* KIP 848: received
                                                * heartbeat interval in
                                                * milliseconds */
        rd_interval_t rkcg_join_intvl;         /* JoinGroup interval */
        rd_interval_t rkcg_timeout_scan_intvl; /* Timeout scanner */

        rd_ts_t rkcg_ts_session_timeout;             /**< Absolute session
                                                      *   timeout enforced by
                                                      *   the consumer, this
                                                      *   value is updated on
                                                      *   Heartbeat success,
                                                      *   etc. */
        rd_kafka_resp_err_t rkcg_last_heartbeat_err; /**< Last Heartbeat error,
                                                      *   used for logging. */

        TAILQ_HEAD(, rd_kafka_topic_s) rkcg_topics; /* Topics subscribed to */

        rd_list_t rkcg_toppars; /* Toppars subscribed to*/

        int32_t rkcg_generation_id; /* Current generation id (classic)
                                     * or member epoch (consumer). */

        rd_kafka_assignor_t *rkcg_assignor; /**< The current partition
                                             *   assignor. used by both
                                             *   leader and members. */
        void *rkcg_assignor_state;          /**< current partition
                                             *   assignor state */

        int32_t rkcg_coord_id; /**< Current coordinator id,
                                *   or -1 if not known. */

        rd_kafka_group_protocol_t
            rkcg_group_protocol; /**< Group protocol to use */

        rd_kafkap_str_t *rkcg_group_remote_assignor; /**< Group remote
                                                      *   assignor to use */

        rd_kafka_broker_t *rkcg_curr_coord; /**< Current coordinator
                                             *   broker handle, or NULL.
                                             *   rkcg_coord's nodename is
                                             *   updated to this broker's
                                             *   nodename when there is a
                                             *   coordinator change. */
        rd_kafka_broker_t *rkcg_coord;      /**< The dedicated coordinator
                                             *   broker handle.
                                             *   Will be updated when the
                                             *   coordinator changes. */

        int16_t rkcg_wait_resp; /**< Awaiting response for this
                                 *   ApiKey.
                                 *   Makes sure only one
                                 *   JoinGroup or SyncGroup
                                 *   request is outstanding.
                                 *   Unset value is -1. */

        /** Current subscription */
        rd_kafka_topic_partition_list_t *rkcg_subscription;
        /** The actual topics subscribed (after metadata+wildcard matching).
         *  Sorted. */
        rd_list_t *rkcg_subscribed_topics; /**< (rd_kafka_topic_info_t *) */
        /** Subscribed topics that are errored/not available. */
        rd_kafka_topic_partition_list_t *rkcg_errored_topics;
        /** If a SUBSCRIBE op is received during a COOPERATIVE rebalance,
         *  actioning this will be postponed until after the rebalance
         *  completes. The waiting subscription is stored here.
         *  Mutually exclusive with rkcg_next_subscription. */
        rd_kafka_topic_partition_list_t *rkcg_next_subscription;
        /** If a (un)SUBSCRIBE op is received during a COOPERATIVE rebalance,
         *  actioning this will be posponed until after the rebalance
         *  completes. This flag is used to signal a waiting unsubscribe
         *  operation. Mutually exclusive with rkcg_next_subscription. */
        rd_bool_t rkcg_next_unsubscribe;

        /** Assignment considered lost */
        rd_atomic32_t rkcg_assignment_lost;

        /** Current assignment of partitions from last SyncGroup response.
         *  NULL means no assignment, else empty or non-empty assignment.
         *
         * This group assignment is the actual set of partitions that were
         * assigned to our consumer by the consumer group leader and should
         * not be confused with the rk_consumer.assignment which is the
         * partitions assigned by the application using assign(), et.al.
         *
         * The group assignment and the consumer assignment are typically
         * identical, but not necessarily since an application is free to
         * assign() any partition, not just the partitions it is handed
         * through the rebalance callback.
         *
         * Yes, this nomenclature is ambigious but has historical reasons,
         * so for now just try to remember that:
         *  - group assignment == consumer group assignment.
         *  - assignment == actual used assignment, i.e., fetched partitions.
         *
         * @remark This list is always sorted.
         */
        rd_kafka_topic_partition_list_t *rkcg_group_assignment;

        /** The partitions to incrementally assign following a
         *  currently in-progress incremental unassign. */
        rd_kafka_topic_partition_list_t *rkcg_rebalance_incr_assignment;

        /** Current acked assignment, start with an empty list. */
        rd_kafka_topic_partition_list_t *rkcg_current_assignment;

        /** Assignment the is currently reconciling.
         *  Can be NULL in case there's no reconciliation ongoing. */
        rd_kafka_topic_partition_list_t *rkcg_target_assignment;

        /** Next assignment that will be reconciled once current
         *  reconciliation finishes. Can be NULL. */
        rd_kafka_topic_partition_list_t *rkcg_next_target_assignment;

        /** Number of backoff retries when expediting next heartbeat. */
        int rkcg_expedite_heartbeat_retries;

        /** Flags for KIP-848 state machine. */
        int rkcg_consumer_flags;
/** Coordinator is waiting for an acknowledgement of currently reconciled
 *  target assignment. Cleared when an HB succeeds
 *  after reconciliation finishes. */
#define RD_KAFKA_CGRP_CONSUMER_F_WAIT_ACK 0x1
/** Member is sending an acknowledgement for a reconciled assignment */
#define RD_KAFKA_CGRP_CONSUMER_F_SENDING_ACK 0x2
/** A new subscription needs to be sent to the Coordinator. */
#define RD_KAFKA_CGRP_CONSUMER_F_SEND_NEW_SUBSCRIPTION 0x4
/** A new subscription is being sent to the Coordinator. */
#define RD_KAFKA_CGRP_CONSUMER_F_SENDING_NEW_SUBSCRIPTION 0x8
/** Consumer has subscribed at least once,
 *  if it didn't happen rebalance protocol is still
 *  considered NONE, otherwise it depends on the
 *  configured partition assignors. */
#define RD_KAFKA_CGRP_CONSUMER_F_SUBSCRIBED_ONCE 0x10
/** Send a complete request in next heartbeat */
#define RD_KAFKA_CGRP_CONSUMER_F_SEND_FULL_REQUEST 0x20
/** Member is fenced, need to rejoin */
#define RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN 0x40
/** Member is fenced, rejoining */
#define RD_KAFKA_CGRP_CONSUMER_F_WAIT_REJOIN_TO_COMPLETE 0x80
/** Serve pending assignments after heartbeat */
#define RD_KAFKA_CGRP_CONSUMER_F_SERVE_PENDING 0x100

        /** Rejoin the group following a currently in-progress
         *  incremental unassign. */
        rd_bool_t rkcg_rebalance_rejoin;

        rd_kafka_resp_err_t rkcg_last_err; /* Last error propagated to
                                            * application.
                                            * This is for silencing
                                            * same errors. */

        rd_kafka_timer_t rkcg_offset_commit_tmr;     /* Offset commit timer */
        rd_kafka_timer_t rkcg_max_poll_interval_tmr; /**< Enforce the max
                                                      *   poll interval. */

        rd_kafka_t *rkcg_rk;

        rd_kafka_op_t *rkcg_reply_rko; /* Send reply for op
                                        * (OP_TERMINATE)
                                        * to this rko's queue. */

        rd_ts_t rkcg_ts_terminate; /* Timestamp of when
                                    * cgrp termination was
                                    * initiated. */

        rd_atomic32_t rkcg_terminated; /**< Consumer has been closed */

        /* Protected by rd_kafka_*lock() */
        struct {
                rd_ts_t ts_rebalance;       /* Timestamp of
                                             * last rebalance */
                int rebalance_cnt;          /* Number of
                                               rebalances */
                char rebalance_reason[256]; /**< Last rebalance
                                             *   reason */
                int assignment_size;        /* Partition count
                                             * of last rebalance
                                             * assignment */
        } rkcg_c;

        /* Timestamp of last rebalance start */
        rd_ts_t rkcg_ts_rebalance_start;

} rd_kafka_cgrp_t;



/* Check if broker is the coordinator */
#define RD_KAFKA_CGRP_BROKER_IS_COORD(rkcg, rkb)                               \
        ((rkcg)->rkcg_coord_id != -1 &&                                        \
         (rkcg)->rkcg_coord_id == (rkb)->rkb_nodeid)

/**
 * @returns true if cgrp is using static group membership
 */
#define RD_KAFKA_CGRP_IS_STATIC_MEMBER(rkcg)                                   \
        !RD_KAFKAP_STR_IS_NULL((rkcg)->rkcg_group_instance_id)

extern const char *rd_kafka_cgrp_state_names[];
extern const char *rd_kafka_cgrp_join_state_names[];

void rd_kafka_cgrp_destroy_final(rd_kafka_cgrp_t *rkcg);
rd_kafka_cgrp_t *rd_kafka_cgrp_new(rd_kafka_t *rk,
                                   rd_kafka_group_protocol_t group_protocol,
                                   const rd_kafkap_str_t *group_id,
                                   const rd_kafkap_str_t *client_id);
void rd_kafka_cgrp_serve(rd_kafka_cgrp_t *rkcg);

void rd_kafka_cgrp_op(rd_kafka_cgrp_t *rkcg,
                      rd_kafka_toppar_t *rktp,
                      rd_kafka_replyq_t replyq,
                      rd_kafka_op_type_t type,
                      rd_kafka_resp_err_t err);
void rd_kafka_cgrp_terminate0(rd_kafka_cgrp_t *rkcg, rd_kafka_op_t *rko);
void rd_kafka_cgrp_terminate(rd_kafka_cgrp_t *rkcg, rd_kafka_replyq_t replyq);


rd_kafka_resp_err_t rd_kafka_cgrp_topic_pattern_del(rd_kafka_cgrp_t *rkcg,
                                                    const char *pattern);
rd_kafka_resp_err_t rd_kafka_cgrp_topic_pattern_add(rd_kafka_cgrp_t *rkcg,
                                                    const char *pattern);

int rd_kafka_cgrp_topic_check(rd_kafka_cgrp_t *rkcg, const char *topic);

void rd_kafka_cgrp_set_member_id(rd_kafka_cgrp_t *rkcg, const char *member_id);

void rd_kafka_cgrp_set_join_state(rd_kafka_cgrp_t *rkcg, int join_state);

rd_kafka_broker_t *rd_kafka_cgrp_get_coord(rd_kafka_cgrp_t *rkcg);
void rd_kafka_cgrp_coord_query(rd_kafka_cgrp_t *rkcg, const char *reason);
void rd_kafka_cgrp_coord_dead(rd_kafka_cgrp_t *rkcg,
                              rd_kafka_resp_err_t err,
                              const char *reason);
void rd_kafka_cgrp_metadata_update_check(rd_kafka_cgrp_t *rkcg,
                                         rd_bool_t do_join);
#define rd_kafka_cgrp_get(rk) ((rk)->rk_cgrp)


void rd_kafka_cgrp_assigned_offsets_commit(
    rd_kafka_cgrp_t *rkcg,
    const rd_kafka_topic_partition_list_t *offsets,
    rd_bool_t set_offsets,
    const char *reason);

void rd_kafka_cgrp_assignment_done(rd_kafka_cgrp_t *rkcg);

rd_bool_t rd_kafka_cgrp_assignment_is_lost(rd_kafka_cgrp_t *rkcg);


struct rd_kafka_consumer_group_metadata_s {
        char *group_id;
        int32_t generation_id;
        char *member_id;
        char *group_instance_id; /**< Optional (NULL) */
};

rd_kafka_consumer_group_metadata_t *rd_kafka_consumer_group_metadata_dup(
    const rd_kafka_consumer_group_metadata_t *cgmetadata);

static RD_UNUSED const char *
rd_kafka_rebalance_protocol2str(rd_kafka_rebalance_protocol_t protocol) {
        switch (protocol) {
        case RD_KAFKA_REBALANCE_PROTOCOL_EAGER:
                return "EAGER";
        case RD_KAFKA_REBALANCE_PROTOCOL_COOPERATIVE:
                return "COOPERATIVE";
        default:
                return "NONE";
        }
}

void rd_kafka_cgrp_consumer_expedite_next_heartbeat(rd_kafka_cgrp_t *rkcg,
                                                    const char *reason);

#endif /* _RDKAFKA_CGRP_H_ */
