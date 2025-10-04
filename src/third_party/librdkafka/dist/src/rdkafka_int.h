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

#ifndef _RDKAFKA_INT_H_
#define _RDKAFKA_INT_H_

#ifndef _WIN32
#define _GNU_SOURCE /* for strndup() */
#endif

#ifdef _MSC_VER
typedef int mode_t;
#endif

#include <fcntl.h>


#include "rdsysqueue.h"

#include "rdkafka.h"
#include "rd.h"
#include "rdlog.h"
#include "rdtime.h"
#include "rdaddr.h"
#include "rdinterval.h"
#include "rdavg.h"
#include "rdlist.h"

#if WITH_SSL
#include <openssl/ssl.h>
#endif



#define rd_kafka_assert(rk, cond)                                              \
        do {                                                                   \
                if (unlikely(!(cond)))                                         \
                        rd_kafka_crash(__FILE__, __LINE__, __FUNCTION__, (rk), \
                                       "assert: " #cond);                      \
        } while (0)


void RD_NORETURN rd_kafka_crash(const char *file,
                                int line,
                                const char *function,
                                rd_kafka_t *rk,
                                const char *reason);


/* Forward declarations */
struct rd_kafka_s;
struct rd_kafka_topic_s;
struct rd_kafka_msg_s;
struct rd_kafka_broker_s;
struct rd_kafka_toppar_s;
typedef struct rd_kafka_metadata_internal_s rd_kafka_metadata_internal_t;
typedef struct rd_kafka_toppar_s rd_kafka_toppar_t;
typedef struct rd_kafka_lwtopic_s rd_kafka_lwtopic_t;


/**
 * Protocol level sanity
 */
#define RD_KAFKAP_BROKERS_MAX    10000
#define RD_KAFKAP_TOPICS_MAX     1000000
#define RD_KAFKAP_PARTITIONS_MAX 100000


#define RD_KAFKA_OFFSET_IS_LOGICAL(OFF) ((OFF) < 0)


/**
 * @struct Represents a fetch position:
 *         an offset and an partition leader epoch (if known, else -1).
 */
typedef struct rd_kafka_fetch_pos_s {
        int64_t offset;
        int32_t leader_epoch;
        rd_bool_t validated;
} rd_kafka_fetch_pos_t;



#include "rdkafka_op.h"
#include "rdkafka_queue.h"
#include "rdkafka_msg.h"
#include "rdkafka_proto.h"
#include "rdkafka_buf.h"
#include "rdkafka_pattern.h"
#include "rdkafka_conf.h"
#include "rdkafka_transport.h"
#include "rdkafka_timer.h"
#include "rdkafka_assignor.h"
#include "rdkafka_metadata.h"
#include "rdkafka_mock.h"
#include "rdkafka_partition.h"
#include "rdkafka_assignment.h"
#include "rdkafka_coord.h"
#include "rdkafka_mock.h"

/**
 * Protocol level sanity
 */
#define RD_KAFKAP_BROKERS_MAX              10000
#define RD_KAFKAP_TOPICS_MAX               1000000
#define RD_KAFKAP_PARTITIONS_MAX           100000
#define RD_KAFKAP_GROUPS_MAX               100000
#define RD_KAFKAP_CONFIGS_MAX              10000
#define RD_KAFKAP_ABORTED_TRANSACTIONS_MAX 1000000

#define RD_KAFKA_OFFSET_IS_LOGICAL(OFF) ((OFF) < 0)



/**
 * @enum Idempotent Producer state
 */
typedef enum {
        RD_KAFKA_IDEMP_STATE_INIT,        /**< Initial state */
        RD_KAFKA_IDEMP_STATE_TERM,        /**< Instance is terminating */
        RD_KAFKA_IDEMP_STATE_FATAL_ERROR, /**< A fatal error has been raised */
        RD_KAFKA_IDEMP_STATE_REQ_PID,     /**< Request new PID */
        RD_KAFKA_IDEMP_STATE_WAIT_TRANSPORT, /**< Waiting for coordinator to
                                              *   become available. */
        RD_KAFKA_IDEMP_STATE_WAIT_PID, /**< PID requested, waiting for reply */
        RD_KAFKA_IDEMP_STATE_ASSIGNED, /**< New PID assigned */
        RD_KAFKA_IDEMP_STATE_DRAIN_RESET,    /**< Wait for outstanding
                                              *   ProduceRequests to finish
                                              *   before resetting and
                                              *   re-requesting a new PID. */
        RD_KAFKA_IDEMP_STATE_DRAIN_BUMP,     /**< Wait for outstanding
                                              *   ProduceRequests to finish
                                              *   before bumping the current
                                              *   epoch. */
        RD_KAFKA_IDEMP_STATE_WAIT_TXN_ABORT, /**< Wait for transaction abort
                                              *   to finish and trigger a
                                              *   drain and reset or bump. */
} rd_kafka_idemp_state_t;

/**
 * @returns the idemp_state_t string representation
 */
static RD_UNUSED const char *
rd_kafka_idemp_state2str(rd_kafka_idemp_state_t state) {
        static const char *names[] = {
            "Init",    "Terminate", "FatalError", "RequestPID", "WaitTransport",
            "WaitPID", "Assigned",  "DrainReset", "DrainBump",  "WaitTxnAbort"};
        return names[state];
}



/**
 * @enum Transactional Producer state
 */
typedef enum {
        /**< Initial state */
        RD_KAFKA_TXN_STATE_INIT,
        /**< Awaiting PID to be acquired by rdkafka_idempotence.c */
        RD_KAFKA_TXN_STATE_WAIT_PID,
        /**< PID acquired, but application has not made a successful
         *   init_transactions() call. */
        RD_KAFKA_TXN_STATE_READY_NOT_ACKED,
        /**< PID acquired, no active transaction. */
        RD_KAFKA_TXN_STATE_READY,
        /**< begin_transaction() has been called. */
        RD_KAFKA_TXN_STATE_IN_TRANSACTION,
        /**< commit_transaction() has been called. */
        RD_KAFKA_TXN_STATE_BEGIN_COMMIT,
        /**< commit_transaction() has been called and all outstanding
         *   messages, partitions, and offsets have been sent. */
        RD_KAFKA_TXN_STATE_COMMITTING_TRANSACTION,
        /**< Transaction successfully committed but application has not made
         *   a successful commit_transaction() call yet. */
        RD_KAFKA_TXN_STATE_COMMIT_NOT_ACKED,
        /**< begin_transaction() has been called. */
        RD_KAFKA_TXN_STATE_BEGIN_ABORT,
        /**< abort_transaction() has been called. */
        RD_KAFKA_TXN_STATE_ABORTING_TRANSACTION,
        /**< Transaction successfully aborted but application has not made
         *   a successful abort_transaction() call yet. */
        RD_KAFKA_TXN_STATE_ABORT_NOT_ACKED,
        /**< An abortable error has occurred. */
        RD_KAFKA_TXN_STATE_ABORTABLE_ERROR,
        /* A fatal error has occured. */
        RD_KAFKA_TXN_STATE_FATAL_ERROR
} rd_kafka_txn_state_t;


/**
 * @returns the txn_state_t string representation
 */
static RD_UNUSED const char *
rd_kafka_txn_state2str(rd_kafka_txn_state_t state) {
        static const char *names[] = {"Init",
                                      "WaitPID",
                                      "ReadyNotAcked",
                                      "Ready",
                                      "InTransaction",
                                      "BeginCommit",
                                      "CommittingTransaction",
                                      "CommitNotAcked",
                                      "BeginAbort",
                                      "AbortingTransaction",
                                      "AbortedNotAcked",
                                      "AbortableError",
                                      "FatalError"};
        return names[state];
}

/**
 * @enum Telemetry States
 */
typedef enum {
        /** Initial state, awaiting telemetry broker to be assigned */
        RD_KAFKA_TELEMETRY_AWAIT_BROKER,
        /** Telemetry broker assigned and GetSubscriptions scheduled */
        RD_KAFKA_TELEMETRY_GET_SUBSCRIPTIONS_SCHEDULED,
        /** GetSubscriptions request sent to the assigned broker */
        RD_KAFKA_TELEMETRY_GET_SUBSCRIPTIONS_SENT,
        /** PushTelemetry scheduled to send */
        RD_KAFKA_TELEMETRY_PUSH_SCHEDULED,
        /** PushTelemetry sent to the assigned broker */
        RD_KAFKA_TELEMETRY_PUSH_SENT,
        /** Client is being terminated and last PushTelemetry is scheduled to
         *  send */
        RD_KAFKA_TELEMETRY_TERMINATING_PUSH_SCHEDULED,
        /** Client is being terminated and last PushTelemetry is sent */
        RD_KAFKA_TELEMETRY_TERMINATING_PUSH_SENT,
        /** Telemetry is terminated */
        RD_KAFKA_TELEMETRY_TERMINATED,
} rd_kafka_telemetry_state_t;


static RD_UNUSED const char *
rd_kafka_telemetry_state2str(rd_kafka_telemetry_state_t state) {
        static const char *names[] = {"AwaitBroker",
                                      "GetSubscriptionsScheduled",
                                      "GetSubscriptionsSent",
                                      "PushScheduled",
                                      "PushSent",
                                      "TerminatingPushScheduled",
                                      "TerminatingPushSent",
                                      "Terminated"};
        return names[state];
}

static RD_UNUSED const char *rd_kafka_type2str(rd_kafka_type_t type) {
        static const char *types[] = {
            [RD_KAFKA_PRODUCER] = "producer",
            [RD_KAFKA_CONSUMER] = "consumer",
        };
        return types[type];
}

/**
 * Kafka handle, internal representation of the application's rd_kafka_t.
 */

struct rd_kafka_s {
        rd_kafka_q_t *rk_rep; /* kafka -> application reply queue */
        rd_kafka_q_t *rk_ops; /* any -> rdkafka main thread ops */

        TAILQ_HEAD(, rd_kafka_broker_s) rk_brokers;
        rd_list_t rk_broker_by_id; /* Fast id lookups. */
        rd_atomic32_t rk_broker_cnt;
        /**< Number of brokers in state >= UP */
        rd_atomic32_t rk_broker_up_cnt;
        /**< Number of logical brokers in state >= UP, this is a sub-set
         *   of rk_broker_up_cnt. */
        rd_atomic32_t rk_logical_broker_up_cnt;
        /**< Number of brokers that are down, only includes brokers
         *   that have had at least one connection attempt. */
        rd_atomic32_t rk_broker_down_cnt;
        /**< Logical brokers currently without an address.
         *   Used for calculating ERR__ALL_BROKERS_DOWN. */
        rd_atomic32_t rk_broker_addrless_cnt;

        mtx_t rk_internal_rkb_lock;
        rd_kafka_broker_t *rk_internal_rkb;

        /* Broadcasting of broker state changes to wake up
         * functions waiting for a state change. */
        cnd_t rk_broker_state_change_cnd;
        mtx_t rk_broker_state_change_lock;
        int rk_broker_state_change_version;
        /* List of (rd_kafka_enq_once_t*) objects waiting for broker
         * state changes. Protected by rk_broker_state_change_lock. */
        rd_list_t rk_broker_state_change_waiters; /**< (rd_kafka_enq_once_t*) */

        TAILQ_HEAD(, rd_kafka_topic_s) rk_topics;
        int rk_topic_cnt;

        struct rd_kafka_cgrp_s *rk_cgrp;

        rd_kafka_conf_t rk_conf;
        rd_kafka_q_t *rk_logq; /* Log queue if `log.queue` set */
        char rk_name[128];
        rd_kafkap_str_t *rk_client_id;
        rd_kafkap_str_t *rk_group_id; /* Consumer group id */

        rd_atomic32_t rk_terminate; /**< Set to RD_KAFKA_DESTROY_F_..
                                     *   flags instance
                                     *   is being destroyed.
                                     *   The value set is the
                                     *   destroy flags from
                                     *   rd_kafka_destroy*() and
                                     *   the two internal flags shown
                                     *   below.
                                     *
                                     * Order:
                                     * 1. user_flags | .._F_DESTROY_CALLED
                                     *    is set in rd_kafka_destroy*().
                                     * 2. consumer_close() is called
                                     *    for consumers.
                                     * 3. .._F_TERMINATE is set to
                                     *    signal all background threads
                                     *    to terminate.
                                     */

#define RD_KAFKA_DESTROY_F_TERMINATE                                           \
        0x1 /**< Internal flag to make sure                                    \
             *   rk_terminate is set to non-zero                               \
             *   value even if user passed                                     \
             *   no destroy flags. */
#define RD_KAFKA_DESTROY_F_DESTROY_CALLED                                      \
        0x2 /**< Application has called                                        \
             *  ..destroy*() and we've                                         \
             * begun the termination                                           \
             * process.                                                        \
             * This flag is needed to avoid                                    \
             * rk_terminate from being                                         \
             * 0 when destroy_flags()                                          \
             * is called with flags=0                                          \
             * and prior to _F_TERMINATE                                       \
             * has been set. */
#define RD_KAFKA_DESTROY_F_IMMEDIATE                                           \
        0x4 /**< Immediate non-blocking                                        \
             *   destruction without waiting                                   \
             *   for all resources                                             \
             *   to be cleaned up.                                             \
             *   WARNING: Memory and resource                                  \
             *            leaks possible.                                      \
             *   This flag automatically sets                                  \
             *   .._NO_CONSUMER_CLOSE. */


        rwlock_t rk_lock;
        rd_kafka_type_t rk_type;
        struct timeval rk_tv_state_change;

        rd_atomic64_t rk_ts_last_poll; /**< Timestamp of last application
                                        *   consumer_poll() call
                                        *   (or equivalent).
                                        *   Used to enforce
                                        *   max.poll.interval.ms.
                                        *   Set to INT64_MAX while polling
                                        *   to avoid reaching
                                        * max.poll.interval.ms. during that time
                                        * frame. Only relevant for consumer. */
        rd_ts_t rk_ts_last_poll_start; /**< Timestamp of last application
                                        *   consumer_poll() call start
                                        *   Only relevant for consumer.
                                        *   Not an atomic as Kafka consumer
                                        *   isn't thread safe. */
        rd_ts_t rk_ts_last_poll_end;   /**< Timestamp of last application
                                        *   consumer_poll() call end
                                        *   Only relevant for consumer.
                                        *   Not an atomic as Kafka consumer
                                        *   isn't thread safe. */
        /* First fatal error. */
        struct {
                rd_atomic32_t err; /**< rd_kafka_resp_err_t */
                char *errstr;      /**< Protected by rk_lock */
                int cnt;           /**< Number of errors raised, only
                                    *   the first one is stored. */
        } rk_fatal;

        rd_atomic32_t rk_last_throttle; /* Last throttle_time_ms value
                                         * from broker. */

        /* Locks: rd_kafka_*lock() */
        rd_ts_t rk_ts_metadata; /* Timestamp of most recent
                                 * metadata. */

        rd_kafka_metadata_internal_t
            *rk_full_metadata;       /* Last full metadata. */
        rd_ts_t rk_ts_full_metadata; /* Timestamp of .. */
        struct rd_kafka_metadata_cache rk_metadata_cache; /* Metadata cache */

        char *rk_clusterid;      /* ClusterId from metadata */
        int32_t rk_controllerid; /* ControllerId from metadata */

        /**< Producer: Delivery report mode */
        enum { RD_KAFKA_DR_MODE_NONE,  /**< No delivery reports */
               RD_KAFKA_DR_MODE_CB,    /**< Delivery reports through callback */
               RD_KAFKA_DR_MODE_EVENT, /**< Delivery reports through event API*/
        } rk_drmode;

        /* Simple consumer count:
         *  >0: Running in legacy / Simple Consumer mode,
         *   0: No consumers running
         *  <0: Running in High level consumer mode */
        rd_atomic32_t rk_simple_cnt;

        /**
         * Exactly Once Semantics and Idempotent Producer
         *
         * @locks rk_lock
         */
        struct {
                /*
                 * Idempotence
                 */
                rd_kafka_idemp_state_t idemp_state; /**< Idempotent Producer
                                                     *   state */
                rd_ts_t ts_idemp_state;             /**< Last state change */
                rd_kafka_pid_t pid; /**< Current Producer ID and Epoch */
                int epoch_cnt;      /**< Number of times pid/epoch changed */
                rd_atomic32_t inflight_toppar_cnt; /**< Current number of
                                                    *   toppars with inflight
                                                    *   requests. */
                rd_kafka_timer_t pid_tmr;          /**< PID FSM timer */

                /*
                 * Transactions
                 *
                 * All field access is from the rdkafka main thread,
                 * unless a specific lock is mentioned in the doc string.
                 *
                 */
                rd_atomic32_t txn_may_enq; /**< Transaction state allows
                                            *   application to enqueue
                                            *   (produce) messages. */

                rd_kafkap_str_t *transactional_id; /**< transactional.id */
                rd_kafka_txn_state_t txn_state;    /**< Transactional state.
                                                    *   @locks rk_lock */
                rd_ts_t ts_txn_state;              /**< Last state change.
                                                    *   @locks rk_lock */
                rd_kafka_broker_t *txn_coord;      /**< Transaction coordinator,
                                                    *   this is a logical broker.*/
                rd_kafka_broker_t *txn_curr_coord; /**< Current actual coord
                                                    *   broker.
                                                    *   This is only used to
                                                    *   check if the coord
                                                    *   changes. */
                rd_kafka_broker_monitor_t txn_coord_mon; /**< Monitor for
                                                          *   coordinator to
                                                          *   take action when
                                                          *   the broker state
                                                          *   changes. */
                rd_bool_t txn_requires_epoch_bump; /**< Coordinator epoch bump
                                                    *   required to recover from
                                                    *   idempotent producer
                                                    *   fatal error. */

                /**< Blocking transactional API application call
                 *   currently being handled, its state, reply queue and how
                 *   to handle timeout.
                 *   Only one transactional API call is allowed at any time.
                 *   Protected by the rk_lock. */
                struct {
                        char name[64];     /**< API name, e.g.,
                                            *   send_offsets_to_transaction.
                                            *   This is used to make sure
                                            *   conflicting APIs are not
                                            *   called simultaneously. */
                        rd_bool_t calling; /**< API is being actively called.
                                            *   I.e., application is blocking
                                            *   on a txn API call.
                                            *   This is used to make sure
                                            *   no concurrent API calls are
                                            *   being made. */
                        rd_kafka_error_t *error; /**< Last error from background
                                                  *   processing. This is only
                                                  *   set if the application's
                                                  *   API call timed out.
                                                  *   It will be returned on
                                                  *   the next call. */
                        rd_bool_t has_result;    /**< Indicates whether an API
                                                  *   result (possibly
                                                  *   intermediate) has been set.
                                                  */
                        cnd_t cnd;               /**< Application thread will
                                                  *   block on this cnd waiting
                                                  *   for a result to be set. */
                        mtx_t lock;              /**< Protects all fields of
                                                  *   txn_curr_api. */
                } txn_curr_api;


                int txn_req_cnt; /**< Number of transaction
                                  *   requests sent.
                                  *   This is incremented when a
                                  *   AddPartitionsToTxn or
                                  *   AddOffsetsToTxn request
                                  *   has been sent for the
                                  *   current transaction,
                                  *   to keep track of
                                  *   whether the broker is
                                  *   aware of the current
                                  *   transaction and thus
                                  *   requires an EndTxn request
                                  *   on abort or not. */

                /**< Timer to trigger registration of pending partitions */
                rd_kafka_timer_t txn_register_parts_tmr;

                /**< Lock for txn_pending_rktps and txn_waitresp_rktps */
                mtx_t txn_pending_lock;

                /**< Partitions pending being added to transaction. */
                rd_kafka_toppar_tqhead_t txn_pending_rktps;

                /**< Partitions in-flight added to transaction. */
                rd_kafka_toppar_tqhead_t txn_waitresp_rktps;

                /**< Partitions added and registered to transaction. */
                rd_kafka_toppar_tqhead_t txn_rktps;

                /**< Number of messages that failed delivery.
                 *   If this number is >0 on transaction_commit then an
                 *   abortable transaction error will be raised.
                 *   Is reset to zero on each begin_transaction(). */
                rd_atomic64_t txn_dr_fails;

                /**< Current transaction error. */
                rd_kafka_resp_err_t txn_err;

                /**< Current transaction error string, if any. */
                char *txn_errstr;

                /**< Last InitProducerIdRequest error. */
                rd_kafka_resp_err_t txn_init_err;

                /**< Waiting for transaction coordinator query response */
                rd_bool_t txn_wait_coord;

                /**< Transaction coordinator query timer */
                rd_kafka_timer_t txn_coord_tmr;
        } rk_eos;

        rd_atomic32_t rk_flushing; /**< Application is calling flush(). */

        /**
         * Consumer state
         *
         * @locality rdkafka main thread
         * @locks_required none
         */
        struct {
                /** Application consumer queue for messages, events and errors.
                 *  (typically points to rkcg_q) */
                rd_kafka_q_t *q;
                /** Current assigned partitions through assign() et.al. */
                rd_kafka_assignment_t assignment;
                /** Waiting for this number of commits to finish. */
                int wait_commit_cnt;
        } rk_consumer;

        /**<
         * Coordinator cache.
         *
         * @locks none
         * @locality rdkafka main thread
         */
        rd_kafka_coord_cache_t rk_coord_cache; /**< Coordinator cache */

        TAILQ_HEAD(, rd_kafka_coord_req_s)
        rk_coord_reqs; /**< Coordinator
                        *   requests */


        struct {
                mtx_t lock;           /* Protects acces to this struct */
                cnd_t cnd;            /* For waking up blocking injectors */
                unsigned int cnt;     /* Current message count */
                size_t size;          /* Current message size sum */
                unsigned int max_cnt; /* Max limit */
                size_t max_size;      /* Max limit */
        } rk_curr_msgs;

        rd_kafka_timers_t rk_timers;
        thrd_t rk_thread;

        int rk_initialized; /**< Will be > 0 when the rd_kafka_t
                             *   instance has been fully initialized. */

        int rk_init_wait_cnt; /**< Number of background threads that
                               *   need to finish initialization. */
        cnd_t rk_init_cnd;    /**< Cond-var used to wait for main thread
                               *   to finish its initialization before
                               *   before rd_kafka_new() returns. */
        mtx_t rk_init_lock;   /**< Lock for rk_init_wait and _cmd */

        rd_ts_t rk_ts_created; /**< Timestamp (monotonic clock) of
                                *   rd_kafka_t creation. */

        /**
         * Background thread and queue,
         * enabled by setting `background_event_cb()`.
         */
        struct {
                rd_kafka_q_t *q; /**< Queue served by background thread. */
                thrd_t thread;   /**< Background thread. */
                int calling;     /**< Indicates whether the event callback
                                  *   is being called, reset back to 0
                                  *   when the callback returns.
                                  *   This can be used for troubleshooting
                                  *   purposes. */
        } rk_background;


        /*
         * Logs, events or actions to rate limit / suppress
         */
        struct {
                /**< Log: No brokers support Idempotent Producer */
                rd_interval_t no_idemp_brokers;

                /**< Sparse connections: randomly select broker
                 *   to bring up. This interval should allow
                 *   for a previous connection to be established,
                 *   which varies between different environments:
                 *   Use 10 < reconnect.backoff.jitter.ms / 2 < 1000.
                 */
                rd_interval_t sparse_connect_random;
                /**< Lock for sparse_connect_random */
                mtx_t sparse_connect_lock;

                /**< Broker metadata refresh interval:
                 *   this is rate-limiting the number of topic-less
                 *   broker/cluster metadata refreshes when there are no
                 *   topics to refresh.
                 *   Will be refreshed every topic.metadata.refresh.interval.ms
                 *   but no more often than every 10s.
                 *   No locks: only accessed by rdkafka main thread. */
                rd_interval_t broker_metadata_refresh;

                /**< Suppression for allow.auto.create.topics=false not being
                 *   supported by the broker. */
                rd_interval_t allow_auto_create_topics;
        } rk_suppress;

        struct {
                void *handle; /**< Provider-specific handle struct pointer.
                               *   Typically assigned in provider's .init() */
                rd_kafka_q_t *callback_q; /**< SASL callback queue, if any. */
        } rk_sasl;

        struct {
                /* Fields for the control flow - unless guarded by lock, only
                 * accessed from main thread. */
                /**< Current state of the telemetry state machine. */
                rd_kafka_telemetry_state_t state;
                /**< Preferred broker for sending telemetry (Lock protected). */
                rd_kafka_broker_t *preferred_broker;
                /**< Timer for all the requests we schedule. */
                rd_kafka_timer_t request_timer;
                /**< Lock for preferred telemetry broker and state. */
                mtx_t lock;
                /**< Used to wait for termination (Lock protected). */
                cnd_t termination_cnd;

                /* Fields obtained from broker as a result of GetSubscriptions -
                 * only accessed from main thread.
                 */
                rd_kafka_Uuid_t client_instance_id;
                int32_t subscription_id;
                rd_kafka_compression_t *accepted_compression_types;
                size_t accepted_compression_types_cnt;
                int32_t push_interval_ms;
                int32_t telemetry_max_bytes;
                rd_bool_t delta_temporality;
                char **requested_metrics;
                size_t requested_metrics_cnt;
                /* TODO: Use rd_list_t to store the metrics */
                int *matched_metrics;
                size_t matched_metrics_cnt;

                struct {
                        rd_ts_t ts_last;  /**< Timestamp of last push */
                        rd_ts_t ts_start; /**< Timestamp from when collection
                                           *   started */
                        /** Total rebalance latency (ms) up to previous push */
                        uint64_t rebalance_latency_total;
                } rk_historic_c;

                struct {
                        rd_avg_t rk_avg_poll_idle_ratio;
                        rd_avg_t rk_avg_commit_latency; /**< Current commit
                                                         *   latency avg */
                        rd_avg_t
                            rk_avg_rebalance_latency; /**< Current rebalance
                                                       *   latency avg */
                } rd_avg_current;

                struct {
                        rd_avg_t rk_avg_poll_idle_ratio;
                        rd_avg_t rk_avg_commit_latency; /**< Rolled over commit
                                                         *   latency avg */
                        rd_avg_t
                            rk_avg_rebalance_latency; /**< Rolled over rebalance
                                                       *   latency avg */
                } rd_avg_rollover;

        } rk_telemetry;

        /* Test mocks */
        struct {
                rd_kafka_mock_cluster_t *cluster; /**< Mock cluster, created
                                                   *   by test.mock.num.brokers
                                                   */
                rd_atomic32_t cluster_cnt;        /**< Total number of mock
                                                   *   clusters, created either
                                                   *   through
                                                   *   test.mock.num.brokers
                                                   *   or mock_cluster_new().
                                                   */

        } rk_mock;
};

#define rd_kafka_wrlock(rk)   rwlock_wrlock(&(rk)->rk_lock)
#define rd_kafka_rdlock(rk)   rwlock_rdlock(&(rk)->rk_lock)
#define rd_kafka_rdunlock(rk) rwlock_rdunlock(&(rk)->rk_lock)
#define rd_kafka_wrunlock(rk) rwlock_wrunlock(&(rk)->rk_lock)


/**
 * @brief Add \p cnt messages and of total size \p size bytes to the
 *        internal bookkeeping of current message counts.
 *        If the total message count or size after add would exceed the
 *        configured limits \c queue.buffering.max.messages and
 *        \c queue.buffering.max.kbytes then depending on the value of
 *        \p block the function either blocks until enough space is available
 *        if \p block is 1, else immediately returns
 *        RD_KAFKA_RESP_ERR__QUEUE_FULL.
 *
 * @param rdmtx If non-null and \p block is set and blocking is to ensue,
 *              then unlock this mutex for the duration of the blocking
 *              and then reacquire with a read-lock.
 */
static RD_INLINE RD_UNUSED rd_kafka_resp_err_t
rd_kafka_curr_msgs_add(rd_kafka_t *rk,
                       unsigned int cnt,
                       size_t size,
                       int block,
                       rwlock_t *rdlock) {

        if (rk->rk_type != RD_KAFKA_PRODUCER)
                return RD_KAFKA_RESP_ERR_NO_ERROR;

        mtx_lock(&rk->rk_curr_msgs.lock);
        while (
            unlikely((rk->rk_curr_msgs.max_cnt > 0 &&
                      rk->rk_curr_msgs.cnt + cnt > rk->rk_curr_msgs.max_cnt) ||
                     (unsigned long long)(rk->rk_curr_msgs.size + size) >
                         (unsigned long long)rk->rk_curr_msgs.max_size)) {
                if (!block) {
                        mtx_unlock(&rk->rk_curr_msgs.lock);
                        return RD_KAFKA_RESP_ERR__QUEUE_FULL;
                }

                if (rdlock)
                        rwlock_rdunlock(rdlock);

                cnd_wait(&rk->rk_curr_msgs.cnd, &rk->rk_curr_msgs.lock);

                if (rdlock)
                        rwlock_rdlock(rdlock);
        }

        rk->rk_curr_msgs.cnt += cnt;
        rk->rk_curr_msgs.size += size;
        mtx_unlock(&rk->rk_curr_msgs.lock);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Subtract \p cnt messages of total size \p size from the
 *        current bookkeeping and broadcast a wakeup on the condvar
 *        for any waiting & blocking threads.
 */
static RD_INLINE RD_UNUSED void
rd_kafka_curr_msgs_sub(rd_kafka_t *rk, unsigned int cnt, size_t size) {
        int broadcast = 0;

        if (rk->rk_type != RD_KAFKA_PRODUCER)
                return;

        mtx_lock(&rk->rk_curr_msgs.lock);
        rd_kafka_assert(NULL, rk->rk_curr_msgs.cnt >= cnt &&
                                  rk->rk_curr_msgs.size >= size);

        /* If the subtraction would pass one of the thresholds
         * broadcast a wake-up to any waiting listeners. */
        if ((rk->rk_curr_msgs.cnt - cnt == 0) ||
            (rk->rk_curr_msgs.cnt >= rk->rk_curr_msgs.max_cnt &&
             rk->rk_curr_msgs.cnt - cnt < rk->rk_curr_msgs.max_cnt) ||
            (rk->rk_curr_msgs.size >= rk->rk_curr_msgs.max_size &&
             rk->rk_curr_msgs.size - size < rk->rk_curr_msgs.max_size))
                broadcast = 1;

        rk->rk_curr_msgs.cnt -= cnt;
        rk->rk_curr_msgs.size -= size;

        if (unlikely(broadcast))
                cnd_broadcast(&rk->rk_curr_msgs.cnd);

        mtx_unlock(&rk->rk_curr_msgs.lock);
}

static RD_INLINE RD_UNUSED void
rd_kafka_curr_msgs_get(rd_kafka_t *rk, unsigned int *cntp, size_t *sizep) {
        if (rk->rk_type != RD_KAFKA_PRODUCER) {
                *cntp  = 0;
                *sizep = 0;
                return;
        }

        mtx_lock(&rk->rk_curr_msgs.lock);
        *cntp  = rk->rk_curr_msgs.cnt;
        *sizep = rk->rk_curr_msgs.size;
        mtx_unlock(&rk->rk_curr_msgs.lock);
}

static RD_INLINE RD_UNUSED int rd_kafka_curr_msgs_cnt(rd_kafka_t *rk) {
        int cnt;
        if (rk->rk_type != RD_KAFKA_PRODUCER)
                return 0;

        mtx_lock(&rk->rk_curr_msgs.lock);
        cnt = rk->rk_curr_msgs.cnt;
        mtx_unlock(&rk->rk_curr_msgs.lock);

        return cnt;
}

/**
 * @brief Wait until \p tspec for curr_msgs to reach 0.
 *
 * @returns rd_true if zero is reached, or rd_false on timeout.
 *          The remaining messages are returned in \p *curr_msgsp
 */
static RD_INLINE RD_UNUSED rd_bool_t
rd_kafka_curr_msgs_wait_zero(rd_kafka_t *rk,
                             int timeout_ms,
                             unsigned int *curr_msgsp) {
        unsigned int cnt;
        struct timespec tspec;

        rd_timeout_init_timespec(&tspec, timeout_ms);

        mtx_lock(&rk->rk_curr_msgs.lock);
        while ((cnt = rk->rk_curr_msgs.cnt) > 0) {
                if (cnd_timedwait_abs(&rk->rk_curr_msgs.cnd,
                                      &rk->rk_curr_msgs.lock,
                                      &tspec) == thrd_timedout)
                        break;
        }
        mtx_unlock(&rk->rk_curr_msgs.lock);

        *curr_msgsp = cnt;
        return cnt == 0;
}

void rd_kafka_destroy_final(rd_kafka_t *rk);

void rd_kafka_global_init(void);

/**
 * @returns true if \p rk handle is terminating.
 *
 * @remark If consumer_close() is called from destroy*() it will be
 *         called prior to _F_TERMINATE being set and will thus not
 *         be able to use rd_kafka_terminating() to know it is shutting down.
 *         That code should instead just check that rk_terminate is non-zero
 *         (the _F_DESTROY_CALLED flag will be set).
 */
#define rd_kafka_terminating(rk)                                               \
        (rd_atomic32_get(&(rk)->rk_terminate) & RD_KAFKA_DESTROY_F_TERMINATE)

/**
 * @returns the destroy flags set matching \p flags, which might be
 *          a subset of the flags.
 */
#define rd_kafka_destroy_flags_check(rk, flags)                                \
        (rd_atomic32_get(&(rk)->rk_terminate) & (flags))

/**
 * @returns true if no consumer callbacks, or standard consumer_close
 *          behaviour, should be triggered. */
#define rd_kafka_destroy_flags_no_consumer_close(rk)                           \
        rd_kafka_destroy_flags_check(rk, RD_KAFKA_DESTROY_F_NO_CONSUMER_CLOSE)

#define rd_kafka_is_simple_consumer(rk)                                        \
        (rd_atomic32_get(&(rk)->rk_simple_cnt) > 0)
int rd_kafka_simple_consumer_add(rd_kafka_t *rk);


/**
 * @returns true if idempotency is enabled (producer only).
 */
#define rd_kafka_is_idempotent(rk) ((rk)->rk_conf.eos.idempotence)

/**
 * @returns true if the producer is transactional (producer only).
 */
#define rd_kafka_is_transactional(rk)                                          \
        ((rk)->rk_conf.eos.transactional_id != NULL)


#define RD_KAFKA_PURGE_F_ABORT_TXN                                             \
        0x100 /**< Internal flag used when                                     \
               *   aborting transaction */
#define RD_KAFKA_PURGE_F_MASK 0x107
const char *rd_kafka_purge_flags2str(int flags);


#include "rdkafka_topic.h"
#include "rdkafka_partition.h"



/**
 * Debug contexts
 */
#define RD_KAFKA_DBG_GENERIC     0x1
#define RD_KAFKA_DBG_BROKER      0x2
#define RD_KAFKA_DBG_TOPIC       0x4
#define RD_KAFKA_DBG_METADATA    0x8
#define RD_KAFKA_DBG_FEATURE     0x10
#define RD_KAFKA_DBG_QUEUE       0x20
#define RD_KAFKA_DBG_MSG         0x40
#define RD_KAFKA_DBG_PROTOCOL    0x80
#define RD_KAFKA_DBG_CGRP        0x100
#define RD_KAFKA_DBG_SECURITY    0x200
#define RD_KAFKA_DBG_FETCH       0x400
#define RD_KAFKA_DBG_INTERCEPTOR 0x800
#define RD_KAFKA_DBG_PLUGIN      0x1000
#define RD_KAFKA_DBG_CONSUMER    0x2000
#define RD_KAFKA_DBG_ADMIN       0x4000
#define RD_KAFKA_DBG_EOS         0x8000
#define RD_KAFKA_DBG_MOCK        0x10000
#define RD_KAFKA_DBG_ASSIGNOR    0x20000
#define RD_KAFKA_DBG_CONF        0x40000
#define RD_KAFKA_DBG_TELEMETRY   0x80000
#define RD_KAFKA_DBG_ALL         0xfffff
#define RD_KAFKA_DBG_NONE        0x0

/* Jitter Percent for exponential retry backoff */
#define RD_KAFKA_RETRY_JITTER_PERCENT 20

void rd_kafka_log0(const rd_kafka_conf_t *conf,
                   const rd_kafka_t *rk,
                   const char *extra,
                   int level,
                   int ctx,
                   const char *fac,
                   const char *fmt,
                   ...) RD_FORMAT(printf, 7, 8);

#define rd_kafka_log(rk, level, fac, ...)                                      \
        rd_kafka_log0(&rk->rk_conf, rk, NULL, level, RD_KAFKA_DBG_NONE, fac,   \
                      __VA_ARGS__)

#define rd_kafka_conf_is_dbg(conf, ctx)                                        \
        unlikely((conf).debug &(RD_KAFKA_DBG_##ctx))

#define rd_kafka_is_dbg(rk, ctx) (rd_kafka_conf_is_dbg(rk->rk_conf, ctx))

#define rd_kafka_dbg(rk, ctx, fac, ...)                                        \
        do {                                                                   \
                if (rd_kafka_is_dbg(rk, ctx))                                  \
                        rd_kafka_log0(&rk->rk_conf, rk, NULL, LOG_DEBUG,       \
                                      (RD_KAFKA_DBG_##ctx), fac, __VA_ARGS__); \
        } while (0)

/* dbg() not requiring an rk, just the conf object, for early logging */
#define rd_kafka_dbg0(conf, ctx, fac, ...)                                     \
        do {                                                                   \
                if (rd_kafka_conf_is_dbg(*conf, ctx))                          \
                        rd_kafka_log0(conf, NULL, NULL, LOG_DEBUG,             \
                                      (RD_KAFKA_DBG_##ctx), fac, __VA_ARGS__); \
        } while (0)

/* NOTE: The local copy of _logname is needed due rkb_logname_lock lock-ordering
 *       when logging another broker's name in the message. */
#define rd_rkb_log0(rkb, level, ctx, fac, ...)                                 \
        do {                                                                   \
                char _logname[RD_KAFKA_NODENAME_SIZE];                         \
                mtx_lock(&(rkb)->rkb_logname_lock);                            \
                rd_strlcpy(_logname, rkb->rkb_logname, sizeof(_logname));      \
                mtx_unlock(&(rkb)->rkb_logname_lock);                          \
                rd_kafka_log0(&(rkb)->rkb_rk->rk_conf, (rkb)->rkb_rk,          \
                              _logname, level, ctx, fac, __VA_ARGS__);         \
        } while (0)

#define rd_rkb_log(rkb, level, fac, ...)                                       \
        rd_rkb_log0(rkb, level, RD_KAFKA_DBG_NONE, fac, __VA_ARGS__)

#define rd_rkb_is_dbg(rkb, ctx) rd_kafka_is_dbg((rkb)->rkb_rk, ctx)

#define rd_rkb_dbg(rkb, ctx, fac, ...)                                         \
        do {                                                                   \
                if (rd_rkb_is_dbg(rkb, ctx)) {                                 \
                        rd_rkb_log0(rkb, LOG_DEBUG, (RD_KAFKA_DBG_##ctx), fac, \
                                    __VA_ARGS__);                              \
                }                                                              \
        } while (0)



extern rd_kafka_resp_err_t RD_TLS rd_kafka_last_error_code;

static RD_UNUSED RD_INLINE rd_kafka_resp_err_t
rd_kafka_set_last_error(rd_kafka_resp_err_t err, int errnox) {
        if (errnox) {
                /* MSVC:
                 * This is the correct way to set errno on Windows,
                 * but it is still pointless due to different errnos in
                 * in different runtimes:
                 * https://social.msdn.microsoft.com/Forums/vstudio/en-US/b4500c0d-1b69-40c7-9ef5-08da1025b5bf/setting-errno-from-within-a-dll?forum=vclanguage/
                 * errno is thus highly deprecated, and buggy, on Windows
                 * when using librdkafka as a dynamically loaded DLL. */
                rd_set_errno(errnox);
        }
        rd_kafka_last_error_code = err;
        return err;
}


int rd_kafka_set_fatal_error0(rd_kafka_t *rk,
                              rd_dolock_t do_lock,
                              rd_kafka_resp_err_t err,
                              const char *fmt,
                              ...) RD_FORMAT(printf, 4, 5);
#define rd_kafka_set_fatal_error(rk, err, fmt, ...)                            \
        rd_kafka_set_fatal_error0(rk, RD_DO_LOCK, err, fmt, __VA_ARGS__)

rd_kafka_error_t *rd_kafka_get_fatal_error(rd_kafka_t *rk);

static RD_INLINE RD_UNUSED rd_kafka_resp_err_t
rd_kafka_fatal_error_code(rd_kafka_t *rk) {
        /* This is an optimization to avoid an atomic read which are costly
         * on some platforms:
         * Fatal errors are currently raised by:
         * 1) the idempotent producer
         * 2) static consumers (group.instance.id)
         * 3) Group using consumer protocol (Introduced in KIP-848). See exact
         *    errors in rd_kafka_cgrp_handle_ConsumerGroupHeartbeat() */
        if ((rk->rk_type == RD_KAFKA_PRODUCER && rk->rk_conf.eos.idempotence) ||
            (rk->rk_type == RD_KAFKA_CONSUMER &&
             (rk->rk_conf.group_instance_id ||
              rk->rk_conf.group_protocol == RD_KAFKA_GROUP_PROTOCOL_CONSUMER)))
                return rd_atomic32_get(&rk->rk_fatal.err);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


extern rd_atomic32_t rd_kafka_thread_cnt_curr;
extern char RD_TLS rd_kafka_thread_name[64];

void rd_kafka_set_thread_name(const char *fmt, ...) RD_FORMAT(printf, 1, 2);
void rd_kafka_set_thread_sysname(const char *fmt, ...) RD_FORMAT(printf, 1, 2);

int rd_kafka_path_is_dir(const char *path);
rd_bool_t rd_kafka_dir_is_empty(const char *path);

rd_kafka_op_res_t rd_kafka_poll_cb(rd_kafka_t *rk,
                                   rd_kafka_q_t *rkq,
                                   rd_kafka_op_t *rko,
                                   rd_kafka_q_cb_type_t cb_type,
                                   void *opaque);

rd_kafka_resp_err_t rd_kafka_subscribe_rkt(rd_kafka_topic_t *rkt);


/**
 * @returns the number of milliseconds the maximum poll interval
 *          was exceeded, or 0 if not exceeded.
 *
 * @remark Only relevant for high-level consumer.
 *
 * @locality any
 * @locks none
 */
static RD_INLINE RD_UNUSED int rd_kafka_max_poll_exceeded(rd_kafka_t *rk) {
        rd_ts_t last_poll;
        int exceeded;

        if (rk->rk_type != RD_KAFKA_CONSUMER)
                return 0;

        last_poll = rd_atomic64_get(&rk->rk_ts_last_poll);

        /* Application is blocked in librdkafka function, see
         * rd_kafka_app_poll_start(). */
        if (last_poll == INT64_MAX)
                return 0;

        exceeded = (int)((rd_clock() - last_poll) / 1000ll) -
                   rk->rk_conf.max_poll_interval_ms;

        if (unlikely(exceeded > 0))
                return exceeded;

        return 0;
}

/**
 * @brief Call on entry to blocking polling function to indicate
 *        that the application is blocked waiting for librdkafka
 *        and that max.poll.interval.ms should not be enforced.
 *
 *        Call app_polled() Upon return from the function calling
 *        this function to register the application's last time of poll.
 *
 * @remark Only relevant for high-level consumer.
 *
 * @locality any
 * @locks none
 */
static RD_INLINE RD_UNUSED void
rd_kafka_app_poll_start(rd_kafka_t *rk, rd_ts_t now, rd_bool_t is_blocking) {
        if (rk->rk_type != RD_KAFKA_CONSUMER)
                return;

        if (!now)
                now = rd_clock();
        if (is_blocking)
                rd_atomic64_set(&rk->rk_ts_last_poll, INT64_MAX);
        if (rk->rk_ts_last_poll_end) {
                int64_t poll_idle_ratio = 0;
                rd_ts_t poll_interval   = now - rk->rk_ts_last_poll_start;
                if (poll_interval) {
                        rd_ts_t idle_interval =
                            rk->rk_ts_last_poll_end - rk->rk_ts_last_poll_start;
                        poll_idle_ratio =
                            idle_interval * 1000000 / poll_interval;
                }
                rd_avg_add(
                    &rk->rk_telemetry.rd_avg_current.rk_avg_poll_idle_ratio,
                    poll_idle_ratio);
                rk->rk_ts_last_poll_start = now;
                rk->rk_ts_last_poll_end   = 0;
        }
}

/**
 * @brief Set the last application poll time to now.
 *
 * @remark Only relevant for high-level consumer.
 *
 * @locality any
 * @locks none
 */
static RD_INLINE RD_UNUSED void rd_kafka_app_polled(rd_kafka_t *rk) {
        if (rk->rk_type == RD_KAFKA_CONSUMER) {
                rd_ts_t now = rd_clock();
                rd_atomic64_set(&rk->rk_ts_last_poll, now);
                if (unlikely(rk->rk_cgrp &&
                             rk->rk_cgrp->rkcg_group_protocol ==
                                 RD_KAFKA_GROUP_PROTOCOL_CONSUMER &&
                             rk->rk_cgrp->rkcg_flags &
                                 RD_KAFKA_CGRP_F_MAX_POLL_EXCEEDED)) {
                        rd_kafka_cgrp_consumer_expedite_next_heartbeat(
                            rk->rk_cgrp,
                            "app polled after poll interval exceeded");
                }
                if (!rk->rk_ts_last_poll_end)
                        rk->rk_ts_last_poll_end = now;
                rd_dassert(rk->rk_ts_last_poll_end >=
                           rk->rk_ts_last_poll_start);
        }
}



void rd_kafka_term_sig_handler(int sig);

/**
 * rdkafka_background.c
 */
int rd_kafka_background_thread_main(void *arg);
rd_kafka_resp_err_t rd_kafka_background_thread_create(rd_kafka_t *rk,
                                                      char *errstr,
                                                      size_t errstr_size);


#endif /* _RDKAFKA_INT_H_ */
