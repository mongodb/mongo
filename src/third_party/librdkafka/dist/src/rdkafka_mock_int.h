/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2019 Magnus Edenhill
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

#ifndef _RDKAFKA_MOCK_INT_H_
#define _RDKAFKA_MOCK_INT_H_

/**
 * @name Mock cluster - internal data types
 *
 */


/**
 * @struct Response error and/or RTT-delay to return to client.
 */
typedef struct rd_kafka_mock_error_rtt_s {
        rd_kafka_resp_err_t err; /**< Error response (or 0) */
        rd_ts_t rtt;             /**< RTT/delay in microseconds (or 0) */
} rd_kafka_mock_error_rtt_t;

/**
 * @struct A stack of errors or rtt latencies to return to the client,
 *         one by one until the stack is depleted.
 */
typedef struct rd_kafka_mock_error_stack_s {
        TAILQ_ENTRY(rd_kafka_mock_error_stack_s) link;
        int16_t ApiKey; /**< Optional ApiKey for which this stack
                         *   applies to, else -1. */
        size_t cnt;     /**< Current number of errors in .errs */
        size_t size;    /**< Current allocated size for .errs (in elements) */
        rd_kafka_mock_error_rtt_t *errs; /**< Array of errors/rtts */
} rd_kafka_mock_error_stack_t;

typedef TAILQ_HEAD(rd_kafka_mock_error_stack_head_s,
                   rd_kafka_mock_error_stack_s)
    rd_kafka_mock_error_stack_head_t;


/**
 * @struct Consumer group protocol name and metadata.
 */
typedef struct rd_kafka_mock_cgrp_proto_s {
        rd_kafkap_str_t *name;
        rd_kafkap_bytes_t *metadata;
} rd_kafka_mock_cgrp_proto_t;

/**
 * @struct Consumer group member
 */
typedef struct rd_kafka_mock_cgrp_member_s {
        TAILQ_ENTRY(rd_kafka_mock_cgrp_member_s) link;
        char *id;                 /**< MemberId */
        char *group_instance_id;  /**< Group instance id */
        rd_ts_t ts_last_activity; /**< Last activity, e.g., Heartbeat */
        rd_kafka_mock_cgrp_proto_t *protos;      /**< Protocol names */
        int proto_cnt;                           /**< Number of protocols */
        rd_kafkap_bytes_t *assignment;           /**< Current assignment */
        rd_kafka_buf_t *resp;                    /**< Current response buffer */
        struct rd_kafka_mock_connection_s *conn; /**< Connection, may be NULL
                                                  *   if there is no ongoing
                                                  *   request. */
} rd_kafka_mock_cgrp_member_t;

/**
 * @struct Consumer group.
 */
typedef struct rd_kafka_mock_cgrp_s {
        TAILQ_ENTRY(rd_kafka_mock_cgrp_s) link;
        struct rd_kafka_mock_cluster_s *cluster; /**< Cluster */
        struct rd_kafka_mock_connection_s *conn; /**< Connection */
        char *id;                                /**< Group Id */
        char *protocol_type;                     /**< Protocol type */
        char *protocol_name;                     /**< Elected protocol name */
        int32_t generation_id;                   /**< Generation Id */
        int session_timeout_ms;                  /**< Session timeout */
        enum { RD_KAFKA_MOCK_CGRP_STATE_EMPTY,   /* No members */
               RD_KAFKA_MOCK_CGRP_STATE_JOINING, /* Members are joining */
               RD_KAFKA_MOCK_CGRP_STATE_SYNCING, /* Syncing assignments */
               RD_KAFKA_MOCK_CGRP_STATE_REBALANCING, /* Rebalance triggered */
               RD_KAFKA_MOCK_CGRP_STATE_UP,          /* Group is operational */
        } state;                        /**< Consumer group state */
        rd_kafka_timer_t session_tmr;   /**< Session timeout timer */
        rd_kafka_timer_t rebalance_tmr; /**< Rebalance state timer */
        TAILQ_HEAD(, rd_kafka_mock_cgrp_member_s) members; /**< Group members */
        int member_cnt;      /**< Number of group members */
        int last_member_cnt; /**< Mumber of group members at last election */
        int assignment_cnt;  /**< Number of member assignments in last Sync */
        rd_kafka_mock_cgrp_member_t *leader; /**< Elected leader */
} rd_kafka_mock_cgrp_t;


/**
 * @struct TransactionalId + PID (+ optional sequence state)
 */
typedef struct rd_kafka_mock_pid_s {
        rd_kafka_pid_t pid;

        /* BaseSequence tracking (partition) */
        int8_t window;  /**< increases up to 5 */
        int8_t lo;      /**< Window low bucket: oldest */
        int8_t hi;      /**< Window high bucket: most recent */
        int32_t seq[5]; /**< Next expected BaseSequence for each bucket */

        char TransactionalId[1]; /**< Allocated after this structure */
} rd_kafka_mock_pid_t;

/**
 * @brief rd_kafka_mock_pid_t.pid Pid (not epoch) comparator
 */
static RD_UNUSED int rd_kafka_mock_pid_cmp_pid(const void *_a, const void *_b) {
        const rd_kafka_mock_pid_t *a = _a, *b = _b;

        if (a->pid.id < b->pid.id)
                return -1;
        else if (a->pid.id > b->pid.id)
                return 1;

        return 0;
}

/**
 * @brief rd_kafka_mock_pid_t.pid TransactionalId,Pid,epoch comparator
 */
static RD_UNUSED int rd_kafka_mock_pid_cmp(const void *_a, const void *_b) {
        const rd_kafka_mock_pid_t *a = _a, *b = _b;
        int r;

        r = strcmp(a->TransactionalId, b->TransactionalId);
        if (r)
                return r;

        if (a->pid.id < b->pid.id)
                return -1;
        else if (a->pid.id > b->pid.id)
                return 1;

        if (a->pid.epoch < b->pid.epoch)
                return -1;
        if (a->pid.epoch > b->pid.epoch)
                return 1;

        return 0;
}



/**
 * @struct A real TCP connection from the client to a mock broker.
 */
typedef struct rd_kafka_mock_connection_s {
        TAILQ_ENTRY(rd_kafka_mock_connection_s) link;
        rd_kafka_transport_t *transport; /**< Socket transport */
        rd_kafka_buf_t *rxbuf;           /**< Receive buffer */
        rd_kafka_bufq_t outbufs;         /**< Send buffers */
        short *poll_events;              /**< Events to poll, points to
                                          *   the broker's pfd array */
        struct sockaddr_in peer;         /**< Peer address */
        struct rd_kafka_mock_broker_s *broker;
        rd_kafka_timer_t write_tmr; /**< Socket write delay timer */
} rd_kafka_mock_connection_t;


/**
 * @struct Mock broker
 */
typedef struct rd_kafka_mock_broker_s {
        TAILQ_ENTRY(rd_kafka_mock_broker_s) link;
        int32_t id;
        char advertised_listener[128];
        struct sockaddr_in sin; /**< Bound address:port */
        uint16_t port;
        char *rack;
        rd_bool_t up;
        rd_ts_t rtt; /**< RTT in microseconds */

        rd_socket_t listen_s; /**< listen() socket */

        TAILQ_HEAD(, rd_kafka_mock_connection_s) connections;

        /**< Per-protocol request error stack.
         *   @locks mcluster->lock */
        rd_kafka_mock_error_stack_head_t errstacks;

        struct rd_kafka_mock_cluster_s *cluster;
} rd_kafka_mock_broker_t;


/**
 * @struct A Kafka-serialized MessageSet
 */
typedef struct rd_kafka_mock_msgset_s {
        TAILQ_ENTRY(rd_kafka_mock_msgset_s) link;
        int64_t first_offset; /**< First offset in batch */
        int64_t last_offset;  /**< Last offset in batch */
        rd_kafkap_bytes_t bytes;
        /* Space for bytes.data is allocated after the msgset_t */
} rd_kafka_mock_msgset_t;


/**
 * @struct Committed offset for a group and partition.
 */
typedef struct rd_kafka_mock_committed_offset_s {
        /**< mpart.committed_offsets */
        TAILQ_ENTRY(rd_kafka_mock_committed_offset_s) link;
        char *group;               /**< Allocated along with the struct */
        int64_t offset;            /**< Committed offset */
        rd_kafkap_str_t *metadata; /**< Metadata, allocated separately */
} rd_kafka_mock_committed_offset_t;


/**
 * @struct Mock partition
 */
typedef struct rd_kafka_mock_partition_s {
        TAILQ_ENTRY(rd_kafka_mock_partition_s) leader_link;
        int32_t id;

        int64_t start_offset;          /**< Actual/leader start offset */
        int64_t end_offset;            /**< Actual/leader end offset */
        int64_t follower_start_offset; /**< Follower's start offset */
        int64_t follower_end_offset;   /**< Follower's end offset */
        rd_bool_t update_follower_start_offset; /**< Keep follower_start_offset
                                                 *   in synch with start_offset
                                                 */
        rd_bool_t update_follower_end_offset;   /**< Keep follower_end_offset
                                                 *   in synch with end_offset
                                                 */

        TAILQ_HEAD(, rd_kafka_mock_msgset_s) msgsets;
        size_t size;     /**< Total size of all .msgsets */
        size_t cnt;      /**< Total count of .msgsets */
        size_t max_size; /**< Maximum size of all .msgsets, may be overshot. */
        size_t max_cnt;  /**< Maximum number of .msgsets */

        /**< Committed offsets */
        TAILQ_HEAD(, rd_kafka_mock_committed_offset_s) committed_offsets;

        rd_kafka_mock_broker_t *leader;
        rd_kafka_mock_broker_t **replicas;
        int replica_cnt;

        rd_list_t pidstates; /**< PID states */

        int32_t follower_id; /**< Preferred replica/follower */

        struct rd_kafka_mock_topic_s *topic;
} rd_kafka_mock_partition_t;


/**
 * @struct Mock topic
 */
typedef struct rd_kafka_mock_topic_s {
        TAILQ_ENTRY(rd_kafka_mock_topic_s) link;
        char *name;

        rd_kafka_mock_partition_t *partitions;
        int partition_cnt;

        rd_kafka_resp_err_t err; /**< Error to return in protocol requests
                                  *   for this topic. */

        struct rd_kafka_mock_cluster_s *cluster;
} rd_kafka_mock_topic_t;

/**
 * @struct Explicitly set coordinator.
 */
typedef struct rd_kafka_mock_coord_s {
        TAILQ_ENTRY(rd_kafka_mock_coord_s) link;
        rd_kafka_coordtype_t type;
        char *key;
        int32_t broker_id;
} rd_kafka_mock_coord_t;


typedef void(rd_kafka_mock_io_handler_t)(
    struct rd_kafka_mock_cluster_s *mcluster,
    rd_socket_t fd,
    int events,
    void *opaque);

struct rd_kafka_mock_api_handler {
        int16_t MinVersion;
        int16_t MaxVersion;
        int16_t FlexVersion; /**< First Flexible version */
        int (*cb)(rd_kafka_mock_connection_t *mconn, rd_kafka_buf_t *rkbuf);
};

extern const struct rd_kafka_mock_api_handler
    rd_kafka_mock_api_handlers[RD_KAFKAP__NUM];



/**
 * @struct Mock cluster.
 *
 * The cluster IO loop runs in a separate thread where all
 * broker IO is handled.
 *
 * No locking is needed.
 */
struct rd_kafka_mock_cluster_s {
        char id[32]; /**< Generated cluster id */

        rd_kafka_t *rk;

        int32_t controller_id; /**< Current controller */

        TAILQ_HEAD(, rd_kafka_mock_broker_s) brokers;
        int broker_cnt;

        TAILQ_HEAD(, rd_kafka_mock_topic_s) topics;
        int topic_cnt;

        TAILQ_HEAD(, rd_kafka_mock_cgrp_s) cgrps;

        /** Explicit coordinators (set with mock_set_coordinator()) */
        TAILQ_HEAD(, rd_kafka_mock_coord_s) coords;

        /** Current transactional producer PIDs.
         *  Element type is a malloced rd_kafka_mock_pid_t*. */
        rd_list_t pids;

        char *bootstraps; /**< bootstrap.servers */

        thrd_t thread; /**< Mock thread */

        rd_kafka_q_t *ops; /**< Control ops queue for interacting with the
                            *   cluster. */

        rd_socket_t wakeup_fds[2]; /**< Wake-up fds for use with .ops */

        rd_bool_t run; /**< Cluster will run while this value is true */

        int fd_cnt;         /**< Number of file descriptors */
        int fd_size;        /**< Allocated size of .fds
                             *   and .handlers */
        struct pollfd *fds; /**< Dynamic array */

        rd_kafka_broker_t *dummy_rkb; /**< Some internal librdkafka APIs
                                       *   that we are reusing requires a
                                       *   broker object, we use the
                                       *   internal broker and store it
                                       *   here for convenient access. */

        struct {
                int partition_cnt;      /**< Auto topic create part cnt */
                int replication_factor; /**< Auto topic create repl factor */
        } defaults;

        /**< Dynamic array of IO handlers for corresponding fd in .fds */
        struct {
                rd_kafka_mock_io_handler_t *cb; /**< Callback */
                void *opaque;                   /**< Callbacks' opaque */
        } * handlers;

        /**< Per-protocol request error stack. */
        rd_kafka_mock_error_stack_head_t errstacks;

        /**< Request handlers */
        struct rd_kafka_mock_api_handler api_handlers[RD_KAFKAP__NUM];

        /**< Mutex for:
         *   .errstacks
         *   .apiversions
         */
        mtx_t lock;

        rd_kafka_timers_t timers; /**< Timers */
};



rd_kafka_buf_t *rd_kafka_mock_buf_new_response(const rd_kafka_buf_t *request);
void rd_kafka_mock_connection_send_response(rd_kafka_mock_connection_t *mconn,
                                            rd_kafka_buf_t *resp);
void rd_kafka_mock_connection_set_blocking(rd_kafka_mock_connection_t *mconn,
                                           rd_bool_t blocking);

rd_kafka_mock_partition_t *
rd_kafka_mock_partition_find(const rd_kafka_mock_topic_t *mtopic,
                             int32_t partition);
rd_kafka_mock_topic_t *
rd_kafka_mock_topic_auto_create(rd_kafka_mock_cluster_t *mcluster,
                                const char *topic,
                                int partition_cnt,
                                rd_kafka_resp_err_t *errp);
rd_kafka_mock_topic_t *
rd_kafka_mock_topic_find(const rd_kafka_mock_cluster_t *mcluster,
                         const char *name);
rd_kafka_mock_topic_t *
rd_kafka_mock_topic_find_by_kstr(const rd_kafka_mock_cluster_t *mcluster,
                                 const rd_kafkap_str_t *kname);
rd_kafka_mock_broker_t *
rd_kafka_mock_cluster_get_coord(rd_kafka_mock_cluster_t *mcluster,
                                rd_kafka_coordtype_t KeyType,
                                const rd_kafkap_str_t *Key);

rd_kafka_mock_committed_offset_t *
rd_kafka_mock_committed_offset_find(const rd_kafka_mock_partition_t *mpart,
                                    const rd_kafkap_str_t *group);
rd_kafka_mock_committed_offset_t *
rd_kafka_mock_commit_offset(rd_kafka_mock_partition_t *mpart,
                            const rd_kafkap_str_t *group,
                            int64_t offset,
                            const rd_kafkap_str_t *metadata);

const rd_kafka_mock_msgset_t *
rd_kafka_mock_msgset_find(const rd_kafka_mock_partition_t *mpart,
                          int64_t offset,
                          rd_bool_t on_follower);

rd_kafka_resp_err_t
rd_kafka_mock_next_request_error(rd_kafka_mock_connection_t *mconn,
                                 rd_kafka_buf_t *resp);

rd_kafka_resp_err_t
rd_kafka_mock_partition_log_append(rd_kafka_mock_partition_t *mpart,
                                   const rd_kafkap_bytes_t *records,
                                   const rd_kafkap_str_t *TransactionalId,
                                   int64_t *BaseOffset);


/**
 * @returns true if the ApiVersion is supported, else false.
 */
static RD_UNUSED rd_bool_t
rd_kafka_mock_cluster_ApiVersion_check(const rd_kafka_mock_cluster_t *mcluster,
                                       int16_t ApiKey,
                                       int16_t ApiVersion) {
        return (ApiVersion >= mcluster->api_handlers[ApiKey].MinVersion &&
                ApiVersion <= mcluster->api_handlers[ApiKey].MaxVersion);
}


rd_kafka_resp_err_t
rd_kafka_mock_pid_find(rd_kafka_mock_cluster_t *mcluster,
                       const rd_kafkap_str_t *TransactionalId,
                       const rd_kafka_pid_t pid,
                       rd_kafka_mock_pid_t **mpidp);


/**
 * @name Mock consumer group (rdkafka_mock_cgrp.c)
 * @{
 */
void rd_kafka_mock_cgrp_member_active(rd_kafka_mock_cgrp_t *mcgrp,
                                      rd_kafka_mock_cgrp_member_t *member);
void rd_kafka_mock_cgrp_member_assignment_set(
    rd_kafka_mock_cgrp_t *mcgrp,
    rd_kafka_mock_cgrp_member_t *member,
    const rd_kafkap_bytes_t *Metadata);
rd_kafka_resp_err_t
rd_kafka_mock_cgrp_member_sync_set(rd_kafka_mock_cgrp_t *mcgrp,
                                   rd_kafka_mock_cgrp_member_t *member,
                                   rd_kafka_mock_connection_t *mconn,
                                   rd_kafka_buf_t *resp);
rd_kafka_resp_err_t
rd_kafka_mock_cgrp_member_leave(rd_kafka_mock_cgrp_t *mcgrp,
                                rd_kafka_mock_cgrp_member_t *member);
void rd_kafka_mock_cgrp_protos_destroy(rd_kafka_mock_cgrp_proto_t *protos,
                                       int proto_cnt);
rd_kafka_resp_err_t
rd_kafka_mock_cgrp_member_add(rd_kafka_mock_cgrp_t *mcgrp,
                              rd_kafka_mock_connection_t *mconn,
                              rd_kafka_buf_t *resp,
                              const rd_kafkap_str_t *MemberId,
                              const rd_kafkap_str_t *ProtocolType,
                              rd_kafka_mock_cgrp_proto_t *protos,
                              int proto_cnt,
                              int session_timeout_ms);
rd_kafka_resp_err_t
rd_kafka_mock_cgrp_check_state(rd_kafka_mock_cgrp_t *mcgrp,
                               rd_kafka_mock_cgrp_member_t *member,
                               const rd_kafka_buf_t *request,
                               int32_t generation_id);
rd_kafka_mock_cgrp_member_t *
rd_kafka_mock_cgrp_member_find(const rd_kafka_mock_cgrp_t *mcgrp,
                               const rd_kafkap_str_t *MemberId);
void rd_kafka_mock_cgrp_destroy(rd_kafka_mock_cgrp_t *mcgrp);
rd_kafka_mock_cgrp_t *rd_kafka_mock_cgrp_find(rd_kafka_mock_cluster_t *mcluster,
                                              const rd_kafkap_str_t *GroupId);
rd_kafka_mock_cgrp_t *
rd_kafka_mock_cgrp_get(rd_kafka_mock_cluster_t *mcluster,
                       const rd_kafkap_str_t *GroupId,
                       const rd_kafkap_str_t *ProtocolType);
void rd_kafka_mock_cgrps_connection_closed(rd_kafka_mock_cluster_t *mcluster,
                                           rd_kafka_mock_connection_t *mconn);


/**
 *@}
 */


#include "rdkafka_mock.h"

#endif /* _RDKAFKA_MOCK_INT_H_ */
