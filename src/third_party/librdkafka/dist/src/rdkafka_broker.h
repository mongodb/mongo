/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012,2022, Magnus Edenhill
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

#ifndef _RDKAFKA_BROKER_H_
#define _RDKAFKA_BROKER_H_

#include "rdkafka_feature.h"


extern const char *rd_kafka_broker_state_names[];
extern const char *rd_kafka_secproto_names[];


/**
 * @enum Broker states
 */
typedef enum {
        RD_KAFKA_BROKER_STATE_INIT,
        RD_KAFKA_BROKER_STATE_DOWN,
        RD_KAFKA_BROKER_STATE_TRY_CONNECT,
        RD_KAFKA_BROKER_STATE_CONNECT,
        RD_KAFKA_BROKER_STATE_SSL_HANDSHAKE,
        RD_KAFKA_BROKER_STATE_AUTH_LEGACY,

        /* Any state >= STATE_UP means the Kafka protocol layer
         * is operational (to some degree). */
        RD_KAFKA_BROKER_STATE_UP,
        RD_KAFKA_BROKER_STATE_UPDATE,
        RD_KAFKA_BROKER_STATE_APIVERSION_QUERY,
        RD_KAFKA_BROKER_STATE_AUTH_HANDSHAKE,
        RD_KAFKA_BROKER_STATE_AUTH_REQ,
        RD_KAFKA_BROKER_STATE_REAUTH,
} rd_kafka_broker_state_t;

/**
 * @struct Broker state monitor.
 *
 * @warning The monitor object lifetime should be the same as
 *          the rd_kafka_t object, not shorter.
 */
typedef struct rd_kafka_broker_monitor_s {
        TAILQ_ENTRY(rd_kafka_broker_monitor_s) rkbmon_link; /**< rkb_monitors*/
        struct rd_kafka_broker_s *rkbmon_rkb; /**< Broker being monitored. */
        rd_kafka_q_t *rkbmon_q;               /**< Queue to enqueue op on. */

        /**< Callback triggered on the monitoree's op handler thread.
         *   Do note that the callback might be triggered even after
         *   it has been deleted due to the queueing nature of op queues. */
        void (*rkbmon_cb)(rd_kafka_broker_t *rkb);
} rd_kafka_broker_monitor_t;


/**
 * @struct Broker instance
 */
struct rd_kafka_broker_s { /* rd_kafka_broker_t */
        TAILQ_ENTRY(rd_kafka_broker_s) rkb_link;

        int32_t rkb_nodeid; /**< Broker Node Id.
                             *   @locks rkb_lock */
#define RD_KAFKA_NODEID_UA -1

        rd_sockaddr_list_t *rkb_rsal;
        rd_ts_t rkb_ts_rsal_last;
        const rd_sockaddr_inx_t *rkb_addr_last; /* Last used connect address */

        rd_kafka_transport_t *rkb_transport;

        uint32_t rkb_corrid;
        int rkb_connid; /* Connection id, increased by
                         * one for each connection by
                         * this broker. Used as a safe-guard
                         * to help troubleshooting buffer
                         * problems across disconnects. */

        rd_kafka_q_t *rkb_ops;

        mtx_t rkb_lock;

        int rkb_blocking_max_ms; /* Maximum IO poll blocking
                                  * time. */

        /* Toppars handled by this broker */
        TAILQ_HEAD(, rd_kafka_toppar_s) rkb_toppars;
        int rkb_toppar_cnt;

        /* Active toppars that are eligible for:
         *  - (consumer) fetching due to underflow
         *  - (producer) producing
         *
         * The circleq provides round-robin scheduling for both cases.
         */
        CIRCLEQ_HEAD(, rd_kafka_toppar_s) rkb_active_toppars;
        int rkb_active_toppar_cnt;
        rd_kafka_toppar_t *rkb_active_toppar_next; /* Next 'first' toppar
                                                    * in fetch list.
                                                    * This is used for
                                                    * round-robin. */


        rd_kafka_cgrp_t *rkb_cgrp;

        rd_ts_t rkb_ts_fetch_backoff;
        int rkb_fetching;

        rd_kafka_broker_state_t rkb_state; /**< Current broker state */

        rd_ts_t rkb_ts_state;                 /* Timestamp of last
                                               * state change */
        rd_interval_t rkb_timeout_scan_intvl; /* Waitresp timeout scan
                                               * interval. */

        rd_atomic32_t rkb_blocking_request_cnt; /* The number of
                                                 * in-flight blocking
                                                 * requests.
                                                 * A blocking request is
                                                 * one that is known to
                                                 * possibly block on the
                                                 * broker for longer than
                                                 * the typical processing
                                                 * time, e.g.:
                                                 * JoinGroup, SyncGroup */

        int rkb_features; /* Protocol features supported
                           * by this broker.
                           * See RD_KAFKA_FEATURE_* in
                           * rdkafka_proto.h */

        struct rd_kafka_ApiVersion *rkb_ApiVersions; /* Broker's supported APIs
                                                      * (MUST be sorted) */
        size_t rkb_ApiVersions_cnt;
        rd_interval_t rkb_ApiVersion_fail_intvl; /* Controls how long
                                                  * the fallback proto
                                                  * will be used after
                                                  * ApiVersionRequest
                                                  * failure. */

        rd_kafka_confsource_t rkb_source;
        struct {
                rd_atomic64_t tx_bytes;
                rd_atomic64_t tx; /**< Kafka requests */
                rd_atomic64_t tx_err;
                rd_atomic64_t tx_retries;
                rd_atomic64_t req_timeouts; /* Accumulated value */

                rd_atomic64_t rx_bytes;
                rd_atomic64_t rx; /**< Kafka responses */
                rd_atomic64_t rx_err;
                rd_atomic64_t rx_corrid_err; /* CorrId misses */
                rd_atomic64_t rx_partial;    /* Partial messages received
                                              * and dropped. */
                rd_atomic64_t zbuf_grow;     /* Compression/decompression buffer
                                                grows needed */
                rd_atomic64_t buf_grow;      /* rkbuf grows needed */
                rd_atomic64_t wakeups;       /* Poll wakeups */

                rd_atomic32_t connects; /**< Connection attempts,
                                         *   successful or not. */

                rd_atomic32_t disconnects; /**< Disconnects.
                                            *   Always peer-triggered. */

                rd_atomic64_t reqtype[RD_KAFKAP__NUM]; /**< Per request-type
                                                        *   counter */

                rd_atomic64_t ts_send; /**< Timestamp of last send */
                rd_atomic64_t ts_recv; /**< Timestamp of last receive */
        } rkb_c;

        struct {
                struct {
                        int32_t connects; /**< Connection attempts,
                                           *   successful or not. */
                } rkb_historic_c;

                struct {
                        rd_avg_t rkb_avg_rtt;      /* Current RTT avg */
                        rd_avg_t rkb_avg_throttle; /* Current throttle avg */
                        rd_avg_t
                            rkb_avg_outbuf_latency;       /**< Current latency
                                                           *   between buf_enq0
                                                           *   and writing to socket
                                                           */
                        rd_avg_t rkb_avg_fetch_latency;   /**< Current fetch
                                                           *   latency avg */
                        rd_avg_t rkb_avg_produce_latency; /**< Current produce
                                                           *   latency avg */
                } rd_avg_current;

                struct {
                        rd_avg_t rkb_avg_rtt; /**< Rolled over RTT avg */
                        rd_avg_t
                            rkb_avg_throttle; /**< Rolled over throttle avg */
                        rd_avg_t rkb_avg_outbuf_latency; /**< Rolled over outbuf
                                                          *   latency avg */
                        rd_avg_t rkb_avg_fetch_latency;  /**< Rolled over fetch
                                                          *   latency avg */
                        rd_avg_t
                            rkb_avg_produce_latency; /**< Rolled over produce
                                                      *   latency avg */
                } rd_avg_rollover;
        } rkb_telemetry;

        int rkb_req_timeouts; /* Current value */

        thrd_t rkb_thread;

        rd_refcnt_t rkb_refcnt;

        rd_kafka_t *rkb_rk;

        rd_kafka_buf_t *rkb_recv_buf;

        int rkb_max_inflight; /* Maximum number of in-flight
                               * requests to broker.
                               * Compared to rkb_waitresps length.*/
        rd_kafka_bufq_t rkb_outbufs;
        rd_kafka_bufq_t rkb_waitresps;
        rd_kafka_bufq_t rkb_retrybufs;

        rd_avg_t rkb_avg_int_latency;    /* Current internal latency period*/
        rd_avg_t rkb_avg_outbuf_latency; /**< Current latency
                                          *   between buf_enq0
                                          *   and writing to socket
                                          */
        rd_avg_t rkb_avg_rtt;            /* Current RTT period */
        rd_avg_t rkb_avg_throttle;       /* Current throttle period */

        /* These are all protected by rkb_lock */
        char rkb_name[RD_KAFKA_NODENAME_SIZE];     /* Displ name */
        char rkb_nodename[RD_KAFKA_NODENAME_SIZE]; /* host:port*/
        uint16_t rkb_port;                         /* TCP port */
        char *rkb_origname;                        /* Original
                                                    * host name */
        int rkb_nodename_epoch;                    /**< Bumped each time
                                                    *   the nodename is changed.
                                                    *   Compared to
                                                    *   rkb_connect_epoch
                                                    *   to trigger a reconnect
                                                    *   for logical broker
                                                    *   when the nodename is
                                                    *   updated. */
        int rkb_connect_epoch;                     /**< The value of
                                                    *   rkb_nodename_epoch at the
                                                    *   last connection attempt.
                                                    */

        /* Logging name is a copy of rkb_name, protected by its own mutex */
        char *rkb_logname;
        mtx_t rkb_logname_lock;

        rd_socket_t rkb_wakeup_fd[2]; /* Wake-up fds (r/w) to wake
                                       * up from IO-wait when
                                       * queues have content. */

        /**< Current, exponentially increased, reconnect backoff. */
        int rkb_reconnect_backoff_ms;

        /**< Absolute timestamp of next allowed reconnect. */
        rd_ts_t rkb_ts_reconnect;

        /** Absolute time of last connection attempt. */
        rd_ts_t rkb_ts_connect;

        /** True if a reauthentication is in progress. */
        rd_bool_t rkb_reauth_in_progress;

        /**< Persistent connection demand is tracked by
         *   a counter for each type of demand.
         *   The broker thread will maintain a persistent connection
         *   if any of the counters are non-zero, and revert to
         *   on-demand mode when they all reach zero.
         *   After incrementing any of the counters a broker wakeup
         *   should be signalled to expedite handling. */
        struct {
                /**< Producer: partitions are being produced to.
                 *   Consumer: partitions are being fetched from.
                 *
                 *   Counter is maintained by the broker handler thread
                 *   itself, no need for atomic/locking.
                 *   Is reset to 0 on each producer|consumer_serve() loop
                 *   and updated according to current need, which
                 *   will trigger a state transition to
                 *   TRY_CONNECT if a connection is needed. */
                int internal;

                /**< Consumer: Broker is the group coordinator.
                 *   Counter is maintained by cgrp logic in
                 *   rdkafka main thread.
                 *
                 *   Producer: Broker is the transaction coordinator.
                 *   Counter is maintained by rdkafka_idempotence.c.
                 *
                 *   All: A coord_req_t is waiting for this broker to come up.
                 */

                rd_atomic32_t coord;
        } rkb_persistconn;

        /**< Currently registered state monitors.
         *   @locks rkb_lock */
        TAILQ_HEAD(, rd_kafka_broker_monitor_s) rkb_monitors;

        /**< Coordinator request's broker monitor.
         *   Will trigger the coord_req fsm on broker state change. */
        rd_kafka_broker_monitor_t rkb_coord_monitor;

        rd_kafka_secproto_t rkb_proto;

        int rkb_down_reported; /* Down event reported */
#if WITH_SASL_CYRUS
        rd_kafka_timer_t rkb_sasl_kinit_refresh_tmr;
#endif


        /*
         * Log suppression
         */
        struct {
                /**< Log: compression type not supported by broker. */
                rd_interval_t unsupported_compression;

                /**< Log: KIP-62 not supported by broker. */
                rd_interval_t unsupported_kip62;

                /**< Log: KIP-345 not supported by broker. */
                rd_interval_t unsupported_kip345;

                /**< Log & Error: identical broker_fail() errors. */
                rd_interval_t fail_error;
        } rkb_suppress;

        /** Last error. This is used to suppress repeated logs. */
        struct {
                char errstr[512];        /**< Last error string */
                rd_kafka_resp_err_t err; /**< Last error code */
                int cnt;                 /**< Number of identical errors */
        } rkb_last_err;


        rd_kafka_timer_t rkb_sasl_reauth_tmr;
};

#define rd_kafka_broker_keep(rkb) rd_refcnt_add(&(rkb)->rkb_refcnt)
#define rd_kafka_broker_keep_fl(FUNC, LINE, RKB)                               \
        rd_refcnt_add_fl(FUNC, LINE, &(RKB)->rkb_refcnt)
#define rd_kafka_broker_lock(rkb)   mtx_lock(&(rkb)->rkb_lock)
#define rd_kafka_broker_unlock(rkb) mtx_unlock(&(rkb)->rkb_lock)


/**
 * @brief Locks broker, acquires the states, unlocks, and returns
 *        the state.
 * @locks broker_lock MUST NOT be held.
 * @locality any
 */
static RD_INLINE RD_UNUSED rd_kafka_broker_state_t
rd_kafka_broker_get_state(rd_kafka_broker_t *rkb) {
        rd_kafka_broker_state_t state;
        rd_kafka_broker_lock(rkb);
        state = rkb->rkb_state;
        rd_kafka_broker_unlock(rkb);
        return state;
}



/**
 * @returns true if the broker state is UP or UPDATE
 */
#define rd_kafka_broker_state_is_up(state)                                     \
        ((state) == RD_KAFKA_BROKER_STATE_UP ||                                \
         (state) == RD_KAFKA_BROKER_STATE_UPDATE)


/**
 * @returns true if the broker connection is up, else false.
 * @locks broker_lock MUST NOT be held.
 * @locality any
 */
static RD_UNUSED RD_INLINE rd_bool_t
rd_kafka_broker_is_up(rd_kafka_broker_t *rkb) {
        rd_kafka_broker_state_t state = rd_kafka_broker_get_state(rkb);
        return rd_kafka_broker_state_is_up(state);
}


/**
 * @brief Broker comparator
 */
static RD_UNUSED RD_INLINE int rd_kafka_broker_cmp(const void *_a,
                                                   const void *_b) {
        const rd_kafka_broker_t *a = _a, *b = _b;
        return RD_CMP(a, b);
}


/**
 * @returns true if broker supports \p features, else false.
 */
static RD_UNUSED int rd_kafka_broker_supports(rd_kafka_broker_t *rkb,
                                              int features) {
        const rd_bool_t do_lock = !thrd_is_current(rkb->rkb_thread);
        int r;

        if (do_lock)
                rd_kafka_broker_lock(rkb);

        r = (rkb->rkb_features & features) == features;

        if (do_lock)
                rd_kafka_broker_unlock(rkb);
        return r;
}

int16_t rd_kafka_broker_ApiVersion_supported(rd_kafka_broker_t *rkb,
                                             int16_t ApiKey,
                                             int16_t minver,
                                             int16_t maxver,
                                             int *featuresp);

int16_t rd_kafka_broker_ApiVersion_supported0(rd_kafka_broker_t *rkb,
                                              int16_t ApiKey,
                                              int16_t minver,
                                              int16_t maxver,
                                              int *featuresp,
                                              rd_bool_t do_lock);

rd_kafka_broker_t *rd_kafka_broker_find_by_nodeid0_fl(const char *func,
                                                      int line,
                                                      rd_kafka_t *rk,
                                                      int32_t nodeid,
                                                      int state,
                                                      rd_bool_t do_connect);

#define rd_kafka_broker_find_by_nodeid0(rk, nodeid, state, do_connect)         \
        rd_kafka_broker_find_by_nodeid0_fl(__FUNCTION__, __LINE__, rk, nodeid, \
                                           state, do_connect)
#define rd_kafka_broker_find_by_nodeid(rk, nodeid)                             \
        rd_kafka_broker_find_by_nodeid0(rk, nodeid, -1, rd_false)


/**
 * Filter out brokers that don't support Idempotent Producer.
 */
static RD_INLINE RD_UNUSED int
rd_kafka_broker_filter_non_idempotent(rd_kafka_broker_t *rkb, void *opaque) {
        return !(rkb->rkb_features & RD_KAFKA_FEATURE_IDEMPOTENT_PRODUCER);
}


rd_kafka_broker_t *rd_kafka_broker_any(rd_kafka_t *rk,
                                       int state,
                                       int (*filter)(rd_kafka_broker_t *rkb,
                                                     void *opaque),
                                       void *opaque,
                                       const char *reason);
rd_kafka_broker_t *rd_kafka_broker_any_up(rd_kafka_t *rk,
                                          int *filtered_cnt,
                                          int (*filter)(rd_kafka_broker_t *rkb,
                                                        void *opaque),
                                          void *opaque,
                                          const char *reason);
rd_kafka_broker_t *rd_kafka_broker_any_usable(rd_kafka_t *rk,
                                              int timeout_ms,
                                              rd_dolock_t do_lock,
                                              int features,
                                              const char *reason);

rd_kafka_broker_t *
rd_kafka_broker_prefer(rd_kafka_t *rk, int32_t broker_id, int state);

rd_kafka_broker_t *rd_kafka_broker_get_async(rd_kafka_t *rk,
                                             int32_t broker_id,
                                             int state,
                                             rd_kafka_enq_once_t *eonce);

rd_list_t *rd_kafka_brokers_get_nodeids_async(rd_kafka_t *rk,
                                              rd_kafka_enq_once_t *eonce);

rd_kafka_broker_t *
rd_kafka_broker_controller(rd_kafka_t *rk, int state, rd_ts_t abs_timeout);
rd_kafka_broker_t *rd_kafka_broker_controller_async(rd_kafka_t *rk,
                                                    int state,
                                                    rd_kafka_enq_once_t *eonce);

int rd_kafka_brokers_add0(rd_kafka_t *rk,
                          const char *brokerlist,
                          rd_bool_t is_bootstrap_server_list);
void rd_kafka_broker_set_state(rd_kafka_broker_t *rkb, int state);

void rd_kafka_broker_fail(rd_kafka_broker_t *rkb,
                          int level,
                          rd_kafka_resp_err_t err,
                          const char *fmt,
                          ...) RD_FORMAT(printf, 4, 5);

void rd_kafka_broker_conn_closed(rd_kafka_broker_t *rkb,
                                 rd_kafka_resp_err_t err,
                                 const char *errstr);

void rd_kafka_broker_destroy_final(rd_kafka_broker_t *rkb);

#define rd_kafka_broker_destroy(rkb)                                           \
        rd_refcnt_destroywrapper(&(rkb)->rkb_refcnt,                           \
                                 rd_kafka_broker_destroy_final(rkb))


void rd_kafka_broker_update(rd_kafka_t *rk,
                            rd_kafka_secproto_t proto,
                            const struct rd_kafka_metadata_broker *mdb,
                            rd_kafka_broker_t **rkbp);
rd_kafka_broker_t *rd_kafka_broker_add(rd_kafka_t *rk,
                                       rd_kafka_confsource_t source,
                                       rd_kafka_secproto_t proto,
                                       const char *name,
                                       uint16_t port,
                                       int32_t nodeid);

rd_kafka_broker_t *rd_kafka_broker_add_logical(rd_kafka_t *rk,
                                               const char *name);

/** @define returns true if broker is logical. No locking is needed. */
#define RD_KAFKA_BROKER_IS_LOGICAL(rkb) ((rkb)->rkb_source == RD_KAFKA_LOGICAL)

void rd_kafka_broker_set_nodename(rd_kafka_broker_t *rkb,
                                  rd_kafka_broker_t *from_rkb);

void rd_kafka_broker_connect_up(rd_kafka_broker_t *rkb);
void rd_kafka_broker_connect_done(rd_kafka_broker_t *rkb, const char *errstr);

int rd_kafka_send(rd_kafka_broker_t *rkb);
int rd_kafka_recv(rd_kafka_broker_t *rkb);

#define rd_kafka_dr_msgq(rkt, rkmq, err)                                       \
        rd_kafka_dr_msgq0(rkt, rkmq, err, NULL /*no produce result*/)

void rd_kafka_dr_msgq0(rd_kafka_topic_t *rkt,
                       rd_kafka_msgq_t *rkmq,
                       rd_kafka_resp_err_t err,
                       const rd_kafka_Produce_result_t *presult);

void rd_kafka_dr_implicit_ack(rd_kafka_broker_t *rkb,
                              rd_kafka_toppar_t *rktp,
                              uint64_t last_msgid);

void rd_kafka_broker_buf_enq1(rd_kafka_broker_t *rkb,
                              rd_kafka_buf_t *rkbuf,
                              rd_kafka_resp_cb_t *resp_cb,
                              void *opaque);

void rd_kafka_broker_buf_enq_replyq(rd_kafka_broker_t *rkb,
                                    rd_kafka_buf_t *rkbuf,
                                    rd_kafka_replyq_t replyq,
                                    rd_kafka_resp_cb_t *resp_cb,
                                    void *opaque);

void rd_kafka_broker_buf_retry(rd_kafka_broker_t *rkb, rd_kafka_buf_t *rkbuf);


rd_kafka_broker_t *rd_kafka_broker_internal(rd_kafka_t *rk);

void msghdr_print(rd_kafka_t *rk,
                  const char *what,
                  const struct msghdr *msg,
                  int hexdump);

int32_t rd_kafka_broker_id(rd_kafka_broker_t *rkb);
const char *rd_kafka_broker_name(rd_kafka_broker_t *rkb);
void rd_kafka_broker_wakeup(rd_kafka_broker_t *rkb, const char *reason);
int rd_kafka_all_brokers_wakeup(rd_kafka_t *rk,
                                int min_state,
                                const char *reason);

void rd_kafka_connect_any(rd_kafka_t *rk, const char *reason);

void rd_kafka_broker_purge_queues(rd_kafka_broker_t *rkb,
                                  int purge_flags,
                                  rd_kafka_replyq_t replyq);

int rd_kafka_brokers_get_state_version(rd_kafka_t *rk);
int rd_kafka_brokers_wait_state_change(rd_kafka_t *rk,
                                       int stored_version,
                                       int timeout_ms);
int rd_kafka_brokers_wait_state_change_async(rd_kafka_t *rk,
                                             int stored_version,
                                             rd_kafka_enq_once_t *eonce);
void rd_kafka_brokers_broadcast_state_change(rd_kafka_t *rk);

rd_kafka_broker_t *rd_kafka_broker_random0(const char *func,
                                           int line,
                                           rd_kafka_t *rk,
                                           rd_bool_t is_up,
                                           int state,
                                           int *filtered_cnt,
                                           int (*filter)(rd_kafka_broker_t *rk,
                                                         void *opaque),
                                           void *opaque);

#define rd_kafka_broker_random(rk, state, filter, opaque)                      \
        rd_kafka_broker_random0(__FUNCTION__, __LINE__, rk, rd_false, state,   \
                                NULL, filter, opaque)

#define rd_kafka_broker_random_up(rk, filter, opaque)                          \
        rd_kafka_broker_random0(__FUNCTION__, __LINE__, rk, rd_true,           \
                                RD_KAFKA_BROKER_STATE_UP, NULL, filter,        \
                                opaque)



/**
 * Updates the current toppar active round-robin next pointer.
 */
static RD_INLINE RD_UNUSED void
rd_kafka_broker_active_toppar_next(rd_kafka_broker_t *rkb,
                                   rd_kafka_toppar_t *sugg_next) {
        if (CIRCLEQ_EMPTY(&rkb->rkb_active_toppars) ||
            (void *)sugg_next == CIRCLEQ_ENDC(&rkb->rkb_active_toppars))
                rkb->rkb_active_toppar_next = NULL;
        else if (sugg_next)
                rkb->rkb_active_toppar_next = sugg_next;
        else
                rkb->rkb_active_toppar_next =
                    CIRCLEQ_FIRST(&rkb->rkb_active_toppars);
}


void rd_kafka_broker_active_toppar_add(rd_kafka_broker_t *rkb,
                                       rd_kafka_toppar_t *rktp,
                                       const char *reason);

void rd_kafka_broker_active_toppar_del(rd_kafka_broker_t *rkb,
                                       rd_kafka_toppar_t *rktp,
                                       const char *reason);


void rd_kafka_broker_schedule_connection(rd_kafka_broker_t *rkb);

void rd_kafka_broker_persistent_connection_add(rd_kafka_broker_t *rkb,
                                               rd_atomic32_t *acntp);

void rd_kafka_broker_persistent_connection_del(rd_kafka_broker_t *rkb,
                                               rd_atomic32_t *acntp);


void rd_kafka_broker_monitor_add(rd_kafka_broker_monitor_t *rkbmon,
                                 rd_kafka_broker_t *rkb,
                                 rd_kafka_q_t *rkq,
                                 void (*callback)(rd_kafka_broker_t *rkb));

void rd_kafka_broker_monitor_del(rd_kafka_broker_monitor_t *rkbmon);

void rd_kafka_broker_start_reauth_timer(rd_kafka_broker_t *rkb,
                                        int64_t connections_max_reauth_ms);

void rd_kafka_broker_start_reauth_cb(rd_kafka_timers_t *rkts, void *rkb);

int unittest_broker(void);

#endif /* _RDKAFKA_BROKER_H_ */
