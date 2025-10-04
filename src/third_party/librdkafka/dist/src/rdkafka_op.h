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
#ifndef _RDKAFKA_OP_H_
#define _RDKAFKA_OP_H_


#include "rdkafka_msg.h"
#include "rdkafka_timer.h"
#include "rdkafka_admin.h"


/* Forward declarations */
typedef struct rd_kafka_q_s rd_kafka_q_t;
typedef struct rd_kafka_toppar_s rd_kafka_toppar_t;
typedef struct rd_kafka_op_s rd_kafka_op_t;
typedef struct rd_kafka_broker_s rd_kafka_broker_t;

/* One-off reply queue + reply version.
 * All APIs that take a rd_kafka_replyq_t makes a copy of the
 * struct as-is and grabs hold of the existing .q refcount.
 * Think of replyq as a (Q,VERSION) tuple. */
typedef struct rd_kafka_replyq_s {
        rd_kafka_q_t *q;
        int32_t version;
#if ENABLE_DEVEL
        char *_id; /* Devel id used for debugging reference leaks.
                    * Is a strdup() of the caller's function name,
                    * which makes for easy debugging with valgrind. */
#endif
} rd_kafka_replyq_t;



/**
 * Flags used by:
 *   - rd_kafka_op_t.rko_flags
 *   - rd_kafka_buf_t.rkbuf_flags
 */
#define RD_KAFKA_OP_F_FREE        0x1  /* rd_free payload when done with it */
#define RD_KAFKA_OP_F_NO_RESPONSE 0x2  /* rkbuf: Not expecting a response */
#define RD_KAFKA_OP_F_CRC         0x4  /* rkbuf: Perform CRC calculation */
#define RD_KAFKA_OP_F_BLOCKING    0x8  /* rkbuf: blocking protocol request */
#define RD_KAFKA_OP_F_REPROCESS   0x10 /* cgrp: Reprocess at a later time. */
#define RD_KAFKA_OP_F_SENT        0x20 /* rkbuf: request sent on wire */
#define RD_KAFKA_OP_F_FLEXVER                                                  \
        0x40 /* rkbuf: flexible protocol version                               \
              *        (KIP-482) */
#define RD_KAFKA_OP_F_NEED_MAKE                                                \
        0x80 /* rkbuf: request content has not                                 \
              *        been made yet, the make                                 \
              *        callback will be triggered                              \
              *        to construct the request                                \
              *        right before it is sent. */
#define RD_KAFKA_OP_F_FORCE_CB                                                 \
        0x100 /* rko: force callback even if                                   \
               *      op type is eventable. */

typedef enum {
        RD_KAFKA_OP_NONE,         /* No specific type, use OP_CB */
        RD_KAFKA_OP_FETCH,        /* Kafka thread -> Application */
        RD_KAFKA_OP_ERR,          /* Kafka thread -> Application */
        RD_KAFKA_OP_CONSUMER_ERR, /* Kafka thread -> Application */
        RD_KAFKA_OP_DR,           /* Kafka thread -> Application
                                   * Produce message delivery report */
        RD_KAFKA_OP_STATS,        /* Kafka thread -> Application */

        RD_KAFKA_OP_OFFSET_COMMIT, /* any -> toppar's Broker thread */
        RD_KAFKA_OP_NODE_UPDATE,   /* any -> Broker thread: node update */

        RD_KAFKA_OP_XMIT_BUF, /* transmit buffer: any -> broker thread */
        RD_KAFKA_OP_RECV_BUF, /* received response buffer: broker thr -> any */
        RD_KAFKA_OP_XMIT_RETRY,   /* retry buffer xmit: any -> broker thread */
        RD_KAFKA_OP_FETCH_START,  /* Application -> toppar's handler thread */
        RD_KAFKA_OP_FETCH_STOP,   /* Application -> toppar's handler thread */
        RD_KAFKA_OP_SEEK,         /* Application -> toppar's handler thread */
        RD_KAFKA_OP_PAUSE,        /* Application -> toppar's handler thread */
        RD_KAFKA_OP_OFFSET_FETCH, /* Broker -> broker thread: fetch offsets
                                   * for topic. */

        RD_KAFKA_OP_PARTITION_JOIN,   /* * -> cgrp op:   add toppar to cgrp
                                       * * -> broker op: add toppar to broker */
        RD_KAFKA_OP_PARTITION_LEAVE,  /* * -> cgrp op:   remove toppar from cgrp
                                       * * -> broker op: remove toppar from rkb*/
        RD_KAFKA_OP_REBALANCE,        /* broker thread -> app:
                                       * group rebalance */
        RD_KAFKA_OP_TERMINATE,        /* For generic use */
        RD_KAFKA_OP_COORD_QUERY,      /* Query for coordinator */
        RD_KAFKA_OP_SUBSCRIBE,        /* New subscription */
        RD_KAFKA_OP_ASSIGN,           /* New assignment */
        RD_KAFKA_OP_GET_SUBSCRIPTION, /* Get current subscription.
                                       * Reuses u.subscribe */
        RD_KAFKA_OP_GET_ASSIGNMENT,   /* Get current assignment.
                                       * Reuses u.assign */
        RD_KAFKA_OP_THROTTLE,         /* Throttle info */
        RD_KAFKA_OP_NAME,             /* Request name */
        RD_KAFKA_OP_CG_METADATA,      /**< Request consumer metadata */
        RD_KAFKA_OP_OFFSET_RESET,     /* Offset reset */
        RD_KAFKA_OP_METADATA,         /* Metadata response */
        RD_KAFKA_OP_LOG,              /* Log */
        RD_KAFKA_OP_WAKEUP,           /* Wake-up signaling */
        RD_KAFKA_OP_CREATETOPICS, /**< Admin: CreateTopics: u.admin_request*/
        RD_KAFKA_OP_DELETETOPICS, /**< Admin: DeleteTopics: u.admin_request*/
        RD_KAFKA_OP_CREATEPARTITIONS, /**< Admin: CreatePartitions:
                                       *   u.admin_request*/
        RD_KAFKA_OP_ALTERCONFIGS, /**< Admin: AlterConfigs: u.admin_request*/
        RD_KAFKA_OP_INCREMENTALALTERCONFIGS, /**< Admin:
                                              *    IncrementalAlterConfigs:
                                              *    u.admin_request */
        RD_KAFKA_OP_DESCRIBECONFIGS,         /**< Admin: DescribeConfigs:
                                              *   u.admin_request*/
        RD_KAFKA_OP_DELETERECORDS,           /**< Admin: DeleteRecords:
                                              *   u.admin_request*/
        RD_KAFKA_OP_LISTCONSUMERGROUPS,      /**< Admin:
                                              *   ListConsumerGroups
                                              *   u.admin_request */
        RD_KAFKA_OP_DESCRIBECONSUMERGROUPS,  /**< Admin:
                                              *   DescribeConsumerGroups
                                              *   u.admin_request */
        RD_KAFKA_OP_DESCRIBECLUSTER,         /**< Admin:
                                              *   DescribeCluster
                                              *   u.admin_request */

        RD_KAFKA_OP_DESCRIBETOPICS, /**< Admin:
                                     *   DescribeTopics
                                     *   u.admin_request */
        RD_KAFKA_OP_DELETEGROUPS,   /**< Admin: DeleteGroups: u.admin_request*/
        RD_KAFKA_OP_DELETECONSUMERGROUPOFFSETS, /**< Admin:
                                                 *   DeleteConsumerGroupOffsets
                                                 *   u.admin_request */
        RD_KAFKA_OP_CREATEACLS,   /**< Admin: CreateAcls: u.admin_request*/
        RD_KAFKA_OP_DESCRIBEACLS, /**< Admin: DescribeAcls: u.admin_request*/
        RD_KAFKA_OP_DELETEACLS,   /**< Admin: DeleteAcls: u.admin_request*/
        RD_KAFKA_OP_ALTERCONSUMERGROUPOFFSETS, /**< Admin:
                                                *   AlterConsumerGroupOffsets
                                                *   u.admin_request */
        RD_KAFKA_OP_LISTCONSUMERGROUPOFFSETS,  /**< Admin:
                                                *   ListConsumerGroupOffsets
                                                *   u.admin_request */
        RD_KAFKA_OP_ADMIN_FANOUT,              /**< Admin: fanout request */
        RD_KAFKA_OP_ADMIN_RESULT,              /**< Admin API .._result_t */
        RD_KAFKA_OP_PURGE,                     /**< Purge queues */
        RD_KAFKA_OP_CONNECT,                   /**< Connect (to broker) */
        RD_KAFKA_OP_OAUTHBEARER_REFRESH,       /**< Refresh OAUTHBEARER token */
        RD_KAFKA_OP_MOCK,                      /**< Mock cluster command */
        RD_KAFKA_OP_BROKER_MONITOR,            /**< Broker state change */
        RD_KAFKA_OP_TXN,                       /**< Transaction command */
        RD_KAFKA_OP_GET_REBALANCE_PROTOCOL,    /**< Get rebalance protocol */
        RD_KAFKA_OP_LEADERS,                   /**< Partition leader query */
        RD_KAFKA_OP_BARRIER,                   /**< Version barrier bump */
        RD_KAFKA_OP_SASL_REAUTH, /**< Sasl reauthentication for broker */
        RD_KAFKA_OP_DESCRIBEUSERSCRAMCREDENTIALS, /* < Admin:
                                                     DescribeUserScramCredentials
                                                     u.admin_request >*/
        RD_KAFKA_OP_ALTERUSERSCRAMCREDENTIALS,    /* < Admin:
                                                     AlterUserScramCredentials
                                                     u.admin_request >*/
        RD_KAFKA_OP_LISTOFFSETS,     /**< Admin: ListOffsets u.admin_request >*/
        RD_KAFKA_OP_METADATA_UPDATE, /**< Metadata update (KIP 951) **/
        RD_KAFKA_OP_SET_TELEMETRY_BROKER, /**< Set preferred broker for
                                               telemetry. */
        RD_KAFKA_OP_TERMINATE_TELEMETRY,  /**< Start termination sequence for
                                               telemetry. */
        RD_KAFKA_OP_ELECTLEADERS,         /**< Admin:
                                           *   ElectLeaders
                                           *   u.admin_request */
        RD_KAFKA_OP__END
} rd_kafka_op_type_t;

/* Flags used with op_type_t */
#define RD_KAFKA_OP_CB       (int)(1 << 29) /* Callback op. */
#define RD_KAFKA_OP_REPLY    (int)(1 << 30) /* Reply op. */
#define RD_KAFKA_OP_FLAGMASK (RD_KAFKA_OP_CB | RD_KAFKA_OP_REPLY)


/**
 * @brief Op/queue priority levels.
 * @remark Since priority levels alter the FIFO order, pay extra attention
 *         to preserve ordering as deemed necessary.
 * @remark Priority should only be set on ops destined for application
 *         facing queues (rk_rep, rkcg_q, etc).
 */
typedef enum {
        RD_KAFKA_PRIO_NORMAL = 0, /* Normal bulk, messages, DRs, etc. */
        RD_KAFKA_PRIO_MEDIUM,     /* Prioritize in front of bulk,
                                   * still at some scale. e.g. logs, .. */
        RD_KAFKA_PRIO_HIGH,       /* Small scale high priority */
        RD_KAFKA_PRIO_FLASH       /* Micro scale, immediate delivery. */
} rd_kafka_prio_t;


/**
 * @brief Op handler result
 *
 * @remark When returning YIELD from a handler the handler will
 *         need to have made sure to either re-enqueue the op or destroy it
 *         since the caller will not touch the op anymore.
 */
typedef enum {
        RD_KAFKA_OP_RES_PASS,    /* Not handled, pass to caller */
        RD_KAFKA_OP_RES_HANDLED, /* Op was handled (through callbacks) */
        RD_KAFKA_OP_RES_KEEP,    /* Op was handled (through callbacks)
                                  * but must not be destroyed by op_handle().
                                  * It is NOT PERMITTED to return RES_KEEP
                                  * from a callback handling a ERR__DESTROY
                                  * event. */
        RD_KAFKA_OP_RES_YIELD    /* Callback called yield */
} rd_kafka_op_res_t;


/**
 * @brief Queue serve callback call type
 */
typedef enum {
        RD_KAFKA_Q_CB_INVALID,      /* dont use */
        RD_KAFKA_Q_CB_CALLBACK,     /* trigger callback based on op */
        RD_KAFKA_Q_CB_RETURN,       /* return op rather than trigger callback
                                     * (if possible)*/
        RD_KAFKA_Q_CB_FORCE_RETURN, /* return op, regardless of callback. */
        RD_KAFKA_Q_CB_EVENT /* like _Q_CB_RETURN but return event_t:ed op */
} rd_kafka_q_cb_type_t;

/**
 * @brief Queue serve callback
 * @remark See rd_kafka_op_res_t docs for return semantics.
 */
typedef rd_kafka_op_res_t(rd_kafka_q_serve_cb_t)(rd_kafka_t *rk,
                                                 struct rd_kafka_q_s *rkq,
                                                 struct rd_kafka_op_s *rko,
                                                 rd_kafka_q_cb_type_t cb_type,
                                                 void *opaque)
    RD_WARN_UNUSED_RESULT;

/**
 * @brief Enumerates the assign op sub-types.
 */
typedef enum {
        RD_KAFKA_ASSIGN_METHOD_ASSIGN,       /**< Absolute assign/unassign */
        RD_KAFKA_ASSIGN_METHOD_INCR_ASSIGN,  /**< Incremental assign */
        RD_KAFKA_ASSIGN_METHOD_INCR_UNASSIGN /**< Incremental unassign */
} rd_kafka_assign_method_t;

/**
 * @brief Op callback type
 */
typedef rd_kafka_op_res_t(rd_kafka_op_cb_t)(rd_kafka_t *rk,
                                            rd_kafka_q_t *rkq,
                                            struct rd_kafka_op_s *rko)
    RD_WARN_UNUSED_RESULT;

/* Forward declaration */
struct rd_kafka_admin_worker_cbs;
struct rd_kafka_admin_fanout_worker_cbs;


#define RD_KAFKA_OP_TYPE_ASSERT(rko, type)                                     \
        rd_assert(((rko)->rko_type & ~RD_KAFKA_OP_FLAGMASK) == (type))


struct rd_kafka_op_s {
        TAILQ_ENTRY(rd_kafka_op_s) rko_link;

        rd_kafka_op_type_t rko_type; /* Internal op type */
        rd_kafka_event_type_t rko_evtype;
        int rko_flags; /* See RD_KAFKA_OP_F_... above */
        int32_t rko_version;
        rd_kafka_resp_err_t rko_err;
        rd_kafka_error_t *rko_error;
        int32_t rko_len;          /* Depends on type, typically the
                                   * message length. */
        rd_kafka_prio_t rko_prio; /**< In-queue priority.
                                   *   Higher value means higher prio*/

        rd_kafka_toppar_t *rko_rktp;

        /*
         * Generic fields
         */

        /* Indicates request: enqueue reply on rko_replyq.q with .version.
         * .q is refcounted. */
        rd_kafka_replyq_t rko_replyq;

        /* Original queue's op serve callback and opaque, if any.
         * Mainly used for forwarded queues to use the original queue's
         * serve function from the forwarded position. */
        rd_kafka_q_serve_cb_t *rko_serve;
        void *rko_serve_opaque;

        rd_kafka_t *rko_rk;

#if ENABLE_DEVEL
        const char *rko_source; /**< Where op was created */
#endif

        /* RD_KAFKA_OP_CB */
        rd_kafka_op_cb_t *rko_op_cb;

        union {
                struct {
                        rd_kafka_buf_t *rkbuf;
                        rd_kafka_msg_t rkm;
                        int evidx;
                } fetch;

                struct {
                        rd_kafka_topic_partition_list_t *partitions;
                        /** Require stable (txn-commited) offsets */
                        rd_bool_t require_stable_offsets;
                        int do_free; /* free .partitions on destroy() */
                } offset_fetch;

                struct {
                        rd_kafka_topic_partition_list_t *partitions;
                        void (*cb)(rd_kafka_t *rk,
                                   rd_kafka_resp_err_t err,
                                   rd_kafka_topic_partition_list_t *offsets,
                                   void *opaque);
                        void *opaque;
                        int silent_empty; /**< Fail silently if there are no
                                           *   offsets to commit. */
                        rd_ts_t ts_timeout;
                        char *reason;
                } offset_commit;

                struct {
                        rd_kafka_topic_partition_list_t *topics;
                } subscribe; /* also used for GET_SUBSCRIPTION */

                struct {
                        rd_kafka_topic_partition_list_t *partitions;
                        rd_kafka_assign_method_t method;
                } assign; /* also used for GET_ASSIGNMENT */

                struct {
                        rd_kafka_topic_partition_list_t *partitions;
                } rebalance;

                struct {
                        const char *str;
                } rebalance_protocol;

                struct {
                        char *str;
                } name;

                rd_kafka_consumer_group_metadata_t *cg_metadata;

                struct {
                        int64_t offset;
                        char *errstr;
                        rd_kafka_msg_t rkm;
                        rd_kafka_topic_t *rkt;
                        int fatal; /**< This was a ERR__FATAL error that has
                                    *   been translated to the fatal error
                                    *   code. */
                } err;             /* used for ERR and CONSUMER_ERR */

                struct {
                        int throttle_time;
                        int32_t nodeid;
                        char *nodename;
                } throttle;

                struct {
                        char *json;
                        size_t json_len;
                } stats;

                struct {
                        rd_kafka_buf_t *rkbuf;
                } xbuf; /* XMIT_BUF and RECV_BUF */

                /* RD_KAFKA_OP_METADATA */
                struct {
                        rd_kafka_metadata_t *md;
                        rd_kafka_metadata_internal_t *mdi;
                        int force; /* force request regardless of outstanding
                                    * metadata requests. */
                } metadata;

                struct {
                        rd_kafka_topic_t *rkt;
                        rd_kafka_msgq_t msgq;
                        rd_kafka_msgq_t msgq2;
                        int do_purge2;
                        rd_kafka_Produce_result_t *presult;
                } dr;

                struct {
                        int32_t nodeid;
                        char nodename[RD_KAFKA_NODENAME_SIZE];
                } node;

                struct {
                        rd_kafka_fetch_pos_t pos;
                        int32_t broker_id; /**< Originating broker, or -1 */
                        char *reason;
                } offset_reset;

                struct {
                        rd_kafka_fetch_pos_t pos;
                        struct rd_kafka_cgrp_s *rkcg;
                } fetch_start; /* reused for SEEK */

                struct {
                        int pause;
                        int flag;
                } pause;

                struct {
                        char fac[64];
                        int level;
                        char *str;
                        int ctx;
                } log;

                struct {
                        rd_kafka_AdminOptions_t options;   /**< Copy of user's
                                                            * options */
                        rd_ts_t abs_timeout;               /**< Absolute timeout
                                                            *   for this request. */
                        rd_kafka_timer_t tmr;              /**< Timeout timer */
                        struct rd_kafka_enq_once_s *eonce; /**< Enqueue op
                                                            * only once,
                                                            * used to
                                                            * (re)trigger
                                                            * the request op
                                                            * upon broker state
                                                            * changes while
                                                            * waiting for the
                                                            * controller, or
                                                            * due to .tmr
                                                            * timeout. */
                        rd_list_t
                            args; /**< Type depends on request, e.g.
                                   *   rd_kafka_NewTopic_t for CreateTopics
                                   */

                        rd_kafka_buf_t *reply_buf; /**< Protocol reply,
                                                    *   temporary reference not
                                                    *   owned by this rko */

                        /**< Worker callbacks, see rdkafka_admin.c */
                        struct rd_kafka_admin_worker_cbs *cbs;

                        /** Worker state */
                        enum { RD_KAFKA_ADMIN_STATE_INIT,
                               RD_KAFKA_ADMIN_STATE_WAIT_BROKER,
                               RD_KAFKA_ADMIN_STATE_WAIT_CONTROLLER,
                               RD_KAFKA_ADMIN_STATE_WAIT_FANOUTS,
                               RD_KAFKA_ADMIN_STATE_CONSTRUCT_REQUEST,
                               RD_KAFKA_ADMIN_STATE_WAIT_RESPONSE,
                               RD_KAFKA_ADMIN_STATE_WAIT_BROKER_LIST,
                        } state;

                        int32_t broker_id; /**< Requested broker id to
                                            *   communicate with.
                                            *   Used for AlterConfigs, et.al,
                                            *   that needs to speak to a
                                            *   specific broker rather than
                                            *   the controller.
                                            *   See RD_KAFKA_ADMIN_TARGET_..
                                            *   for special values (coordinator,
                                            *   fanout, etc).
                                            */
                        /** The type of coordinator to look up */
                        rd_kafka_coordtype_t coordtype;
                        /** Which coordinator to look up */
                        char *coordkey;

                        /** Application's reply queue */
                        rd_kafka_replyq_t replyq;
                        rd_kafka_event_type_t reply_event_type;

                        /** A collection of fanout child ops. */
                        struct {
                                /** The type of request being fanned out.
                                 *  This is used for the ADMIN_RESULT. */
                                rd_kafka_op_type_t reqtype;

                                /** Worker callbacks, see rdkafka_admin.c */
                                struct rd_kafka_admin_fanout_worker_cbs *cbs;

                                /** Number of outstanding requests remaining to
                                 *  wait for. */
                                int outstanding;

                                /** Incremental results from fanouts.
                                 *  This list is pre-allocated to the number
                                 *  of input objects and can thus be set
                                 *  by index to retain original ordering. */
                                rd_list_t results;

                                /** Reply event type */
                                rd_kafka_event_type_t reply_event_type;

                        } fanout;

                        /** A reference to the parent ADMIN_FANOUT op that
                         *  spawned this op, if applicable. NULL otherwise. */
                        struct rd_kafka_op_s *fanout_parent;

                } admin_request;

                struct {
                        rd_kafka_op_type_t reqtype; /**< Request op type,
                                                     *   used for logging. */

                        rd_list_t args; /**< Args moved from the request op
                                         *   when the result op is created.
                                         *
                                         *   Type depends on request.
                                         */

                        char *errstr; /**< Error string, if rko_err
                                       *   is set, else NULL. */

                        /** Result cb for this op */
                        void (*result_cb)(rd_kafka_op_t *);

                        rd_list_t results; /**< Type depends on request type:
                                            *
                                            * (rd_kafka_topic_result_t *):
                                            * CreateTopics, DeleteTopics,
                                            * CreatePartitions.
                                            *
                                            * (rd_kafka_ConfigResource_t *):
                                            * AlterConfigs, DescribeConfigs
                                            * IncrementalAlterConfigs
                                            */

                        void *opaque; /**< Application's opaque as set by
                                       *   rd_kafka_AdminOptions_set_opaque
                                       */

                        /** A reference to the parent ADMIN_FANOUT op that
                         *  spawned this op, if applicable. NULL otherwise. */
                        struct rd_kafka_op_s *fanout_parent;
                } admin_result;

                struct {
                        int flags; /**< purge_flags from rd_kafka_purge() */
                } purge;

                /**< Mock cluster command */
                struct {
                        enum { RD_KAFKA_MOCK_CMD_TOPIC_SET_ERROR,
                               RD_KAFKA_MOCK_CMD_TOPIC_CREATE,
                               RD_KAFKA_MOCK_CMD_PART_SET_LEADER,
                               RD_KAFKA_MOCK_CMD_PART_SET_FOLLOWER,
                               RD_KAFKA_MOCK_CMD_PART_SET_FOLLOWER_WMARKS,
                               RD_KAFKA_MOCK_CMD_PART_PUSH_LEADER_RESPONSE,
                               RD_KAFKA_MOCK_CMD_BROKER_SET_UPDOWN,
                               RD_KAFKA_MOCK_CMD_BROKER_SET_RTT,
                               RD_KAFKA_MOCK_CMD_BROKER_SET_RACK,
                               RD_KAFKA_MOCK_CMD_COORD_SET,
                               RD_KAFKA_MOCK_CMD_APIVERSION_SET,
                               RD_KAFKA_MOCK_CMD_REQUESTED_METRICS_SET,
                               RD_KAFKA_MOCK_CMD_TELEMETRY_PUSH_INTERVAL_SET,
                        } cmd;

                        rd_kafka_resp_err_t err; /**< Error for:
                                                  *    TOPIC_SET_ERROR */
                        char *name;              /**< For:
                                                  *    TOPIC_SET_ERROR
                                                  *    TOPIC_CREATE
                                                  *    PART_SET_FOLLOWER
                                                  *    PART_SET_FOLLOWER_WMARKS
                                                  *    BROKER_SET_RACK
                                                  *    COORD_SET (key_type)
                                                  *    PART_PUSH_LEADER_RESPONSE
                                                  */
                        char *str;               /**< For:
                                                  *    COORD_SET (key) */
                        int32_t partition;       /**< For:
                                                  *    PART_SET_FOLLOWER
                                                  *    PART_SET_FOLLOWER_WMARKS
                                                  *    PART_SET_LEADER
                                                  *    APIVERSION_SET (ApiKey)
                                                  *    PART_PUSH_LEADER_RESPONSE
                                                  */
                        int32_t broker_id;       /**< For:
                                                  *    PART_SET_FOLLOWER
                                                  *    PART_SET_LEADER
                                                  *    BROKER_SET_UPDOWN
                                                  *    BROKER_SET_RACK
                                                  *    COORD_SET */
                        int64_t lo;              /**< Low offset, for:
                                                  *    TOPIC_CREATE (part cnt)
                                                  *    PART_SET_FOLLOWER_WMARKS
                                                  *    BROKER_SET_UPDOWN
                                                  *    APIVERSION_SET (minver)
                                                  *    BROKER_SET_RTT
                                                  */
                        int64_t hi;              /**< High offset, for:
                                                  *    TOPIC_CREATE (repl fact)
                                                  *    PART_SET_FOLLOWER_WMARKS
                                                  *    APIVERSION_SET (maxver)
                                                  *    REQUESTED_METRICS_SET (metrics_cnt)
                                                  *    TELEMETRY_PUSH_INTERVAL_SET (interval)
                                                  */
                        int32_t leader_id;       /**< Leader id, for:
                                                  *   PART_PUSH_LEADER_RESPONSE
                                                  */
                        int32_t leader_epoch;    /**< Leader epoch, for:
                                                  *   PART_PUSH_LEADER_RESPONSE
                                                  */
                        char **metrics;          /**< Metrics requested, for:
                                                  *   REQUESTED_METRICS_SET */
                } mock;

                struct {
                        struct rd_kafka_broker_s *rkb; /**< Broker who's state
                                                        *   changed. */
                        /**< Callback to trigger on the op handler's thread. */
                        void (*cb)(struct rd_kafka_broker_s *rkb);
                } broker_monitor;

                struct {
                        /** Consumer group metadata for send_offsets_to.. */
                        rd_kafka_consumer_group_metadata_t *cgmetadata;
                        /** Consumer group id for AddOffsetsTo.. */
                        char *group_id;
                        int timeout_ms;      /**< Operation timeout */
                        rd_ts_t abs_timeout; /**< Absolute time */
                        /**< Offsets to commit */
                        rd_kafka_topic_partition_list_t *offsets;
                } txn;

                struct {
                        /* This struct serves two purposes, the fields
                         * with "Request:" are used for the async workers state
                         * while the "Reply:" fields is a separate reply
                         * rko that is enqueued for the caller upon
                         * completion or failure. */

                        /** Request: Partitions to query.
                         *  Reply:   Queried partitions with .err field set. */
                        rd_kafka_topic_partition_list_t *partitions;

                        /** Request: Absolute timeout */
                        rd_ts_t ts_timeout;

                        /** Request: Metadata query timer */
                        rd_kafka_timer_t query_tmr;

                        /** Request: Timeout timer */
                        rd_kafka_timer_t timeout_tmr;

                        /** Request: Enqueue op only once, used to (re)trigger
                         *  metadata cache lookups, topic refresh, timeout. */
                        struct rd_kafka_enq_once_s *eonce;

                        /** Request: Caller's replyq */
                        rd_kafka_replyq_t replyq;

                        /** Request: Number of metadata queries made. */
                        int query_cnt;

                        /** Reply: Leaders (result)
                         * (rd_kafka_partition_leader*) */
                        rd_list_t *leaders;

                        /** Reply: Callback on completion (or failure) */
                        rd_kafka_op_cb_t *cb;

                        /** Reply: Callback opaque */
                        void *opaque;

                } leaders;

                struct {
                        /** Preferred broker for telemetry. */
                        rd_kafka_broker_t *rkb;
                } telemetry_broker;

        } rko_u;
};

TAILQ_HEAD(rd_kafka_op_head_s, rd_kafka_op_s);



const char *rd_kafka_op2str(rd_kafka_op_type_t type);
void rd_kafka_op_destroy(rd_kafka_op_t *rko);
rd_kafka_op_t *rd_kafka_op_new0(const char *source, rd_kafka_op_type_t type);
#if ENABLE_DEVEL
#define _STRINGIFYX(A) #A
#define _STRINGIFY(A)  _STRINGIFYX(A)
#define rd_kafka_op_new(type)                                                  \
        rd_kafka_op_new0(__FILE__ ":" _STRINGIFY(__LINE__), type)
#else
#define rd_kafka_op_new(type) rd_kafka_op_new0(NULL, type)
#endif
rd_kafka_op_t *rd_kafka_op_new_reply(rd_kafka_op_t *rko_orig,
                                     rd_kafka_resp_err_t err);
rd_kafka_op_t *rd_kafka_op_new_cb(rd_kafka_t *rk,
                                  rd_kafka_op_type_t type,
                                  rd_kafka_op_cb_t *cb);
int rd_kafka_op_reply(rd_kafka_op_t *rko, rd_kafka_resp_err_t err);
int rd_kafka_op_error_reply(rd_kafka_op_t *rko, rd_kafka_error_t *error);

#define rd_kafka_op_set_prio(rko, prio) ((rko)->rko_prio = prio)

#define rd_kafka_op_err(rk, err, ...)                                          \
        do {                                                                   \
                if (!((rk)->rk_conf.enabled_events & RD_KAFKA_EVENT_ERROR)) {  \
                        rd_kafka_log(rk, LOG_ERR, "ERROR", __VA_ARGS__);       \
                        break;                                                 \
                }                                                              \
                rd_kafka_q_op_err((rk)->rk_rep, err, __VA_ARGS__);             \
        } while (0)

void rd_kafka_q_op_err(rd_kafka_q_t *rkq,
                       rd_kafka_resp_err_t err,
                       const char *fmt,
                       ...) RD_FORMAT(printf, 3, 4);
void rd_kafka_consumer_err(rd_kafka_q_t *rkq,
                           int32_t broker_id,
                           rd_kafka_resp_err_t err,
                           int32_t version,
                           const char *topic,
                           rd_kafka_toppar_t *rktp,
                           int64_t offset,
                           const char *fmt,
                           ...) RD_FORMAT(printf, 8, 9);
rd_kafka_op_t *rd_kafka_op_req0(rd_kafka_q_t *destq,
                                rd_kafka_q_t *recvq,
                                rd_kafka_op_t *rko,
                                int timeout_ms);
rd_kafka_op_t *
rd_kafka_op_req(rd_kafka_q_t *destq, rd_kafka_op_t *rko, int timeout_ms);
rd_kafka_op_t *rd_kafka_op_req2(rd_kafka_q_t *destq, rd_kafka_op_type_t type);
rd_kafka_resp_err_t rd_kafka_op_err_destroy(rd_kafka_op_t *rko);
rd_kafka_error_t *rd_kafka_op_error_destroy(rd_kafka_op_t *rko);

rd_kafka_op_res_t rd_kafka_op_call(rd_kafka_t *rk,
                                   rd_kafka_q_t *rkq,
                                   rd_kafka_op_t *rko) RD_WARN_UNUSED_RESULT;

rd_kafka_op_t *rd_kafka_op_new_fetch_msg(rd_kafka_msg_t **rkmp,
                                         rd_kafka_toppar_t *rktp,
                                         int32_t version,
                                         rd_kafka_buf_t *rkbuf,
                                         rd_kafka_fetch_pos_t pos,
                                         size_t key_len,
                                         const void *key,
                                         size_t val_len,
                                         const void *val);

rd_kafka_op_t *rd_kafka_op_new_ctrl_msg(rd_kafka_toppar_t *rktp,
                                        int32_t version,
                                        rd_kafka_buf_t *rkbuf,
                                        rd_kafka_fetch_pos_t pos);

void rd_kafka_op_throttle_time(struct rd_kafka_broker_s *rkb,
                               rd_kafka_q_t *rkq,
                               int throttle_time);


rd_kafka_op_res_t
rd_kafka_op_handle(rd_kafka_t *rk,
                   rd_kafka_q_t *rkq,
                   rd_kafka_op_t *rko,
                   rd_kafka_q_cb_type_t cb_type,
                   void *opaque,
                   rd_kafka_q_serve_cb_t *callback) RD_WARN_UNUSED_RESULT;


extern rd_atomic32_t rd_kafka_op_cnt;

void rd_kafka_op_print(FILE *fp, const char *prefix, rd_kafka_op_t *rko);

void rd_kafka_fetch_op_app_prepare(rd_kafka_t *rk, rd_kafka_op_t *rko);


#define rd_kafka_op_is_ctrl_msg(rko)                                           \
        ((rko)->rko_type == RD_KAFKA_OP_FETCH && !(rko)->rko_err &&            \
         ((rko)->rko_u.fetch.rkm.rkm_flags & RD_KAFKA_MSG_F_CONTROL))



/**
 * @returns true if the rko's replyq is valid and the
 *          rko's rktp version (if any) is not outdated.
 */
#define rd_kafka_op_replyq_is_valid(RKO)                                       \
        (rd_kafka_replyq_is_valid(&(RKO)->rko_replyq) &&                       \
         !rd_kafka_op_version_outdated((RKO), 0))



/**
 * @returns the rko for a consumer message (RD_KAFKA_OP_FETCH).
 */
static RD_UNUSED rd_kafka_op_t *
rd_kafka_message2rko(rd_kafka_message_t *rkmessage) {
        rd_kafka_op_t *rko = rkmessage->_private;

        if (!rko || rko->rko_type != RD_KAFKA_OP_FETCH)
                return NULL;

        return rko;
}



#endif /* _RDKAFKA_OP_H_ */
