/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2015-2022, Magnus Edenhill,
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
#ifndef _RDKAFKA_PARTITION_H_
#define _RDKAFKA_PARTITION_H_

#include "rdkafka_topic.h"
#include "rdkafka_cgrp.h"
#include "rdkafka_broker.h"

extern const char *rd_kafka_fetch_states[];


/**
 * @brief Offset statistics
 */
struct offset_stats {
        rd_kafka_fetch_pos_t fetch_pos; /**< Next offset to fetch */
        int64_t eof_offset;             /**< Last offset we reported EOF for */
};

/**
 * @brief Reset offset_stats struct to default values
 */
static RD_UNUSED void rd_kafka_offset_stats_reset(struct offset_stats *offs) {
        offs->fetch_pos.offset       = 0;
        offs->fetch_pos.leader_epoch = -1;
        offs->eof_offset             = RD_KAFKA_OFFSET_INVALID;
}


/**
 * @brief Store information about a partition error for future use.
 */
struct rd_kafka_toppar_err {
        rd_kafka_resp_err_t err; /**< Error code */
        int actions;             /**< Request actions */
        rd_ts_t ts;              /**< Timestamp */
        uint64_t base_msgid;     /**< First msg msgid */
        int32_t base_seq;        /**< Idempodent Producer:
                                  *   first msg sequence */
        int32_t last_seq;        /**< Idempotent Producer:
                                  *   last msg sequence */
};

/**
 * @brief Fetchpos comparator, only offset is compared.
 */
static RD_UNUSED RD_INLINE int
rd_kafka_fetch_pos_cmp_offset(const rd_kafka_fetch_pos_t *a,
                              const rd_kafka_fetch_pos_t *b) {
        return (RD_CMP(a->offset, b->offset));
}

/**
 * @brief Fetchpos comparator, leader epoch has precedence
 *        iff both values are not null.
 */
static RD_UNUSED RD_INLINE int
rd_kafka_fetch_pos_cmp(const rd_kafka_fetch_pos_t *a,
                       const rd_kafka_fetch_pos_t *b) {
        if (a->leader_epoch == -1 || b->leader_epoch == -1)
                return rd_kafka_fetch_pos_cmp_offset(a, b);
        if (a->leader_epoch < b->leader_epoch)
                return -1;
        else if (a->leader_epoch > b->leader_epoch)
                return 1;
        else
                return rd_kafka_fetch_pos_cmp_offset(a, b);
}


static RD_UNUSED RD_INLINE void
rd_kafka_fetch_pos_init(rd_kafka_fetch_pos_t *fetchpos) {
        fetchpos->offset       = RD_KAFKA_OFFSET_INVALID;
        fetchpos->leader_epoch = -1;
}

const char *rd_kafka_fetch_pos2str(const rd_kafka_fetch_pos_t fetchpos);

static RD_UNUSED RD_INLINE rd_kafka_fetch_pos_t
rd_kafka_fetch_pos_make(int64_t offset,
                        int32_t leader_epoch,
                        rd_bool_t validated) {
        rd_kafka_fetch_pos_t fetchpos = {offset, leader_epoch, validated};
        return fetchpos;
}

#ifdef RD_HAS_STATEMENT_EXPRESSIONS
#define RD_KAFKA_FETCH_POS0(offset, leader_epoch, validated)                   \
        ({                                                                     \
                rd_kafka_fetch_pos_t _fetchpos = {offset, leader_epoch,        \
                                                  validated};                  \
                _fetchpos;                                                     \
        })
#else
#define RD_KAFKA_FETCH_POS0(offset, leader_epoch, validated)                   \
        rd_kafka_fetch_pos_make(offset, leader_epoch, validated)
#endif

#define RD_KAFKA_FETCH_POS(offset, leader_epoch)                               \
        RD_KAFKA_FETCH_POS0(offset, leader_epoch, rd_false)



typedef TAILQ_HEAD(rd_kafka_toppar_tqhead_s,
                   rd_kafka_toppar_s) rd_kafka_toppar_tqhead_t;

/**
 * Topic + Partition combination
 */
struct rd_kafka_toppar_s {                           /* rd_kafka_toppar_t */
        TAILQ_ENTRY(rd_kafka_toppar_s) rktp_rklink;  /* rd_kafka_t link */
        TAILQ_ENTRY(rd_kafka_toppar_s) rktp_rkblink; /* rd_kafka_broker_t link*/
        CIRCLEQ_ENTRY(rd_kafka_toppar_s)
        rktp_activelink;                              /* rkb_active_toppars */
        TAILQ_ENTRY(rd_kafka_toppar_s) rktp_rktlink;  /* rd_kafka_topic_t link*/
        TAILQ_ENTRY(rd_kafka_toppar_s) rktp_cgrplink; /* rd_kafka_cgrp_t link */
        TAILQ_ENTRY(rd_kafka_toppar_s)
        rktp_txnlink;               /**< rd_kafka_t.rk_eos.
                                     *   txn_pend_rktps
                                     *   or txn_rktps */
        rd_kafka_topic_t *rktp_rkt; /**< This toppar's topic object */
        int32_t rktp_partition;
        // LOCK: toppar_lock() + topic_wrlock()
        // LOCK: .. in partition_available()
        int32_t rktp_leader_id;              /**< Current leader id.
                                              *   This is updated directly
                                              *   from metadata. */
        int32_t rktp_broker_id;              /**< Current broker id. */
        rd_kafka_broker_t *rktp_leader;      /**< Current leader broker.
                                              *   This updated simultaneously
                                              *   with rktp_leader_id. */
        rd_kafka_broker_t *rktp_broker;      /**< Current preferred broker
                                              *   (usually the leader).
                                              *   This updated asynchronously
                                              *   by issuing JOIN op to
                                              *   broker thread, so be careful
                                              *   in using this since it
                                              *   may lag. */
        rd_kafka_broker_t *rktp_next_broker; /**< Next preferred broker after
                                              *   async migration op. */
        rd_refcnt_t rktp_refcnt;
        mtx_t rktp_lock;

        // LOCK: toppar_lock. toppar_insert_msg(), concat_msgq()
        // LOCK: toppar_lock. toppar_enq_msg(), deq_msg(), toppar_retry_msgq()
        rd_kafka_q_t *rktp_msgq_wakeup_q; /**< Wake-up queue */
        rd_kafka_msgq_t rktp_msgq;        /* application->rdkafka queue.
                                           * protected by rktp_lock */
        rd_kafka_msgq_t rktp_xmit_msgq;   /* internal broker xmit queue.
                                           * local to broker thread. */

        int rktp_fetch; /* On rkb_active_toppars list */

        /* Consumer */
        rd_kafka_q_t *rktp_fetchq; /* Queue of fetched messages
                                    * from broker.
                                    * Broker thread -> App */
        rd_kafka_q_t *rktp_ops;    /* * -> Main thread */

        rd_atomic32_t rktp_msgs_inflight; /**< Current number of
                                           *   messages in-flight to/from
                                           *   the broker. */

        uint64_t rktp_msgid; /**< Current/last message id.
                              *   Each message enqueued on a
                              *   non-UA partition will get a
                              *   partition-unique sequencial
                              *   number assigned.
                              *   This number is used to
                              *   re-enqueue the message
                              *   on resends but making sure
                              *   the input ordering is still
                              *   maintained, and used by
                              *   the idempotent producer.
                              *   Starts at 1.
                              *   Protected by toppar_lock */
        struct {
                rd_kafka_pid_t pid;        /**< Partition's last known
                                            *   Producer Id and epoch.
                                            *   Protected by toppar lock.
                                            *   Only updated in toppar
                                            *   handler thread. */
                uint64_t acked_msgid;      /**< Highest acknowledged message.
                                            *   Protected by toppar lock. */
                uint64_t epoch_base_msgid; /**< This Producer epoch's
                                            *   base msgid.
                                            *   When a new epoch is
                                            *   acquired, or on transaction
                                            *   abort, the base_seq is set to
                                            *   the current rktp_msgid so that
                                            *   sub-sequent produce
                                            *   requests will have
                                            *   a sequence number series
                                            *   starting at 0.
                                            *   Protected by toppar_lock */
                int32_t next_ack_seq;      /**< Next expected ack sequence.
                                            *   Protected by toppar lock. */
                int32_t next_err_seq;      /**< Next expected error sequence.
                                            *   Used when draining outstanding
                                            *   issues.
                                            *   This value will be the same
                                            *   as next_ack_seq until a
                                            *   drainable error occurs,
                                            *   in which case it
                                            *   will advance past next_ack_seq.
                                            *   next_ack_seq can never be larger
                                            *   than next_err_seq.
                                            *   Protected by toppar lock. */
                rd_bool_t wait_drain;      /**< All inflight requests must
                                            *   be drained/finish before
                                            *   resuming producing.
                                            *   This is set to true
                                            *   when a leader change
                                            *   happens so that the
                                            *   in-flight messages for the
                                            *   old brokers finish before
                                            *   the new broker starts sending.
                                            *   This as a step to ensure
                                            *   consistency.
                                            *   Only accessed from toppar
                                            *   handler thread. */
        } rktp_eos;

        /**
         * rktp version barriers
         *
         * rktp_version is the application/controller side's
         * authoritative version, it depicts the most up to date state.
         * This is what q_filter() matches an rko_version to.
         *
         * rktp_op_version is the last/current received state handled
         * by the toppar in the broker thread. It is updated to rktp_version
         * when receiving a new op.
         *
         * rktp_fetch_version is the current fetcher decision version.
         * It is used in fetch_decide() to see if the fetch decision
         * needs to be updated by comparing to rktp_op_version.
         *
         * Example:
         *   App thread   : Send OP_START (v1 bump): rktp_version=1
         *   Broker thread: Recv OP_START (v1): rktp_op_version=1
         *   Broker thread: fetch_decide() detects that
         *                  rktp_op_version != rktp_fetch_version and
         *                  sets rktp_fetch_version=1.
         *   Broker thread: next Fetch request has it's tver state set to
         *                  rktp_fetch_verison (v1).
         *
         *   App thread   : Send OP_SEEK (v2 bump): rktp_version=2
         *   Broker thread: Recv OP_SEEK (v2): rktp_op_version=2
         *   Broker thread: Recv IO FetchResponse with tver=1,
         *                  when enqueued on rktp_fetchq they're discarded
         *                  due to old version (tver<rktp_version).
         *   Broker thread: fetch_decide() detects version change and
         *                  sets rktp_fetch_version=2.
         *   Broker thread: next Fetch request has tver=2
         *   Broker thread: Recv IO FetchResponse with tver=2 which
         *                  is same as rktp_version so message is forwarded
         *                  to app.
         */
        rd_atomic32_t rktp_version; /* Latest op version.
                                     * Authoritative (app thread)*/
        int32_t rktp_op_version;    /* Op version of curr command
                                     * state from.
                                     * (broker thread) */
        int32_t rktp_fetch_version; /* Op version of curr fetch.
                                       (broker thread) */

        enum {
                RD_KAFKA_TOPPAR_FETCH_NONE = 0,
                RD_KAFKA_TOPPAR_FETCH_STOPPING,
                RD_KAFKA_TOPPAR_FETCH_STOPPED,
                RD_KAFKA_TOPPAR_FETCH_OFFSET_QUERY,
                RD_KAFKA_TOPPAR_FETCH_OFFSET_WAIT,
                RD_KAFKA_TOPPAR_FETCH_VALIDATE_EPOCH_WAIT,
                RD_KAFKA_TOPPAR_FETCH_ACTIVE,
        } rktp_fetch_state; /* Broker thread's state */

#define RD_KAFKA_TOPPAR_FETCH_IS_STARTED(fetch_state)                          \
        ((fetch_state) >= RD_KAFKA_TOPPAR_FETCH_OFFSET_QUERY)

        int32_t rktp_leader_epoch; /**< Last known partition leader epoch,
                                    *   or -1. */

        int32_t rktp_fetch_msg_max_bytes; /* Max number of bytes to
                                           * fetch.
                                           * Locality: broker thread
                                           */

        rd_ts_t rktp_ts_fetch_backoff; /* Back off fetcher for
                                        * this partition until this
                                        * absolute timestamp
                                        * expires. */

        /** Offset to query broker for. */
        rd_kafka_fetch_pos_t rktp_query_pos;

        /** Next fetch start position.
         *  This is set up start, seek, resume, etc, to tell
         *  the fetcher where to start fetching.
         *  It is not updated for each fetch, see
         *  rktp_offsets.fetch_pos for that.
         *  @locality toppar thread */
        rd_kafka_fetch_pos_t rktp_next_fetch_start;

        /** The previous next fetch position.
         *  @locality toppar thread */
        rd_kafka_fetch_pos_t rktp_last_next_fetch_start;

        /** The offset to verify.
         *  @locality toppar thread */
        rd_kafka_fetch_pos_t rktp_offset_validation_pos;

        /** Application's position.
         *  This is the latest offset delivered to application + 1.
         *  It is reset to INVALID_OFFSET when partition is
         *  unassigned/stopped/seeked. */
        rd_kafka_fetch_pos_t rktp_app_pos;

        /** Last stored offset, but maybe not yet committed. */
        rd_kafka_fetch_pos_t rktp_stored_pos;

        /* Last stored metadata, but
         * maybe not committed yet. */
        void *rktp_stored_metadata;
        size_t rktp_stored_metadata_size;

        /** Offset currently being committed */
        rd_kafka_fetch_pos_t rktp_committing_pos;

        /** Last (known) committed offset */
        rd_kafka_fetch_pos_t rktp_committed_pos;

        rd_ts_t rktp_ts_committed_offset; /**< Timestamp of last commit */

        struct offset_stats rktp_offsets;     /* Current offsets.
                                               * Locality: broker thread*/
        struct offset_stats rktp_offsets_fin; /* Finalized offset for stats.
                                               * Updated periodically
                                               * by broker thread.
                                               * Locks: toppar_lock */

        int64_t rktp_ls_offset; /**< Current last stable offset
                                 *   Locks: toppar_lock */
        int64_t rktp_hi_offset; /* Current high watermark offset.
                                 * Locks: toppar_lock */
        int64_t rktp_lo_offset; /* Current broker low offset.
                                 * This is outside of the stats
                                 * struct due to this field
                                 * being populated by the
                                 * toppar thread rather than
                                 * the broker thread.
                                 * Locality: toppar thread
                                 * Locks: toppar_lock */

        rd_ts_t rktp_ts_offset_lag;

        char *rktp_offset_path; /* Path to offset file */
        FILE *rktp_offset_fp;   /* Offset file pointer */

        rd_kafka_resp_err_t rktp_last_error; /**< Last Fetch error.
                                              *   Used for suppressing
                                              *   reoccuring errors.
                                              * @locality broker thread */

        rd_kafka_cgrp_t *rktp_cgrp; /* Belongs to this cgrp */

        rd_bool_t rktp_started; /**< Fetcher is instructured to
                                 *   start.
                                 *   This is used by cgrp to keep
                                 *   track of whether the toppar has
                                 *   been started or not. */

        rd_kafka_replyq_t rktp_replyq; /* Current replyq+version
                                        * for propagating
                                        * major operations, e.g.,
                                        * FETCH_STOP. */
        // LOCK: toppar_lock().  RD_KAFKA_TOPPAR_F_DESIRED
        // LOCK: toppar_lock().  RD_KAFKA_TOPPAR_F_UNKNOWN
        int rktp_flags;
#define RD_KAFKA_TOPPAR_F_DESIRED                                              \
        0x1 /* This partition is desired                                       \
             * by a consumer. */
#define RD_KAFKA_TOPPAR_F_UNKNOWN                                              \
        0x2                                /* Topic is not yet or no longer    \
                                            * seen on a broker. */
#define RD_KAFKA_TOPPAR_F_OFFSET_STORE 0x4 /* Offset store is active */
#define RD_KAFKA_TOPPAR_F_OFFSET_STORE_STOPPING                                \
        0x8                              /* Offset store stopping              \
                                          */
#define RD_KAFKA_TOPPAR_F_APP_PAUSE 0x10 /* App pause()d consumption */
#define RD_KAFKA_TOPPAR_F_LIB_PAUSE 0x20 /* librdkafka paused consumption */
#define RD_KAFKA_TOPPAR_F_REMOVE    0x40 /* partition removed from cluster */
#define RD_KAFKA_TOPPAR_F_LEADER_ERR                                           \
        0x80 /* Operation failed:                                              \
              * leader might be missing.                                       \
              * Typically set from                                             \
              * ProduceResponse failure. */
#define RD_KAFKA_TOPPAR_F_PEND_TXN                                             \
        0x100 /* Partition is pending being added                              \
               * to a producer transaction. */
#define RD_KAFKA_TOPPAR_F_IN_TXN                                               \
        0x200                            /* Partition is part of               \
                                          * a producer transaction. */
#define RD_KAFKA_TOPPAR_F_ON_DESP 0x400  /**< On rkt_desp list */
#define RD_KAFKA_TOPPAR_F_ON_CGRP 0x800  /**< On rkcg_toppars list */
#define RD_KAFKA_TOPPAR_F_ON_RKB  0x1000 /**< On rkb_toppars list */
#define RD_KAFKA_TOPPAR_F_ASSIGNED                                             \
        0x2000 /**< Toppar is part of the consumer                             \
                *   assignment. */
#define RD_KAFKA_TOPPAR_F_VALIDATING                                           \
        0x4000 /**< Toppar is currently requesting validation. */

        /*
         * Timers
         */
        rd_kafka_timer_t rktp_offset_query_tmr;  /* Offset query timer */
        rd_kafka_timer_t rktp_offset_commit_tmr; /* Offset commit timer */
        rd_kafka_timer_t rktp_offset_sync_tmr;   /* Offset file sync timer */
        rd_kafka_timer_t rktp_consumer_lag_tmr;  /* Consumer lag monitoring
                                                  * timer */
        rd_kafka_timer_t rktp_validate_tmr;      /**< Offset and epoch
                                                  *   validation retry timer */

        rd_interval_t rktp_lease_intvl;         /**< Preferred replica lease
                                                 *   period */
        rd_interval_t rktp_new_lease_intvl;     /**< Controls max frequency
                                                 *   at which a new preferred
                                                 *   replica lease can be
                                                 *   created for a toppar.
                                                 */
        rd_interval_t rktp_new_lease_log_intvl; /**< .. and how often
                                                 *   we log about it. */
        rd_interval_t rktp_metadata_intvl;      /**< Controls max frequency
                                                 *   of metadata requests
                                                 *   in preferred replica
                                                 *   handler.
                                                 */

        int rktp_wait_consumer_lag_resp; /* Waiting for consumer lag
                                          * response. */

        struct rd_kafka_toppar_err rktp_last_err; /**< Last produce error */


        struct {
                rd_atomic64_t tx_msgs;      /**< Producer: sent messages */
                rd_atomic64_t tx_msg_bytes; /**<  .. bytes */
                rd_atomic64_t rx_msgs;      /**< Consumer: received messages */
                rd_atomic64_t rx_msg_bytes; /**<  .. bytes */
                rd_atomic64_t producer_enq_msgs; /**< Producer: enqueued msgs */
                rd_atomic64_t rx_ver_drops;      /**< Consumer: outdated message
                                                  *             drops. */
        } rktp_c;
};

/**
 * @struct This is a separately allocated glue object used in
 *         rd_kafka_topic_partition_t._private to allow referencing both
 *         an rktp and/or a leader epoch. Both are optional.
 *         The rktp, if non-NULL, owns a refcount.
 *
 * This glue object is not always set in ._private, but allocated on demand
 * as necessary.
 */
typedef struct rd_kafka_topic_partition_private_s {
        /** Reference to a toppar. Optional, may be NULL. */
        rd_kafka_toppar_t *rktp;
        /** Current Leader epoch, if known, else -1.
         *  this is set when the API needs to send the last epoch known
         *  by the client. */
        int32_t current_leader_epoch;
        /** Leader epoch if known, else -1. */
        int32_t leader_epoch;
        /** Topic id. */
        rd_kafka_Uuid_t topic_id;
} rd_kafka_topic_partition_private_t;


/**
 * Check if toppar is paused (consumer).
 * Locks: toppar_lock() MUST be held.
 */
#define RD_KAFKA_TOPPAR_IS_PAUSED(rktp)                                        \
        ((rktp)->rktp_flags &                                                  \
         (RD_KAFKA_TOPPAR_F_APP_PAUSE | RD_KAFKA_TOPPAR_F_LIB_PAUSE))



/**
 * @brief Increase refcount and return rktp object.
 */
#define rd_kafka_toppar_keep(RKTP)                                             \
        rd_kafka_toppar_keep0(__FUNCTION__, __LINE__, RKTP)

#define rd_kafka_toppar_keep_fl(FUNC, LINE, RKTP)                              \
        rd_kafka_toppar_keep0(FUNC, LINE, RKTP)

static RD_UNUSED RD_INLINE rd_kafka_toppar_t *
rd_kafka_toppar_keep0(const char *func, int line, rd_kafka_toppar_t *rktp) {
        rd_refcnt_add_fl(func, line, &rktp->rktp_refcnt);
        return rktp;
}

void rd_kafka_toppar_destroy_final(rd_kafka_toppar_t *rktp);

#define rd_kafka_toppar_destroy(RKTP)                                          \
        do {                                                                   \
                rd_kafka_toppar_t *_RKTP = (RKTP);                             \
                if (unlikely(rd_refcnt_sub(&_RKTP->rktp_refcnt) == 0))         \
                        rd_kafka_toppar_destroy_final(_RKTP);                  \
        } while (0)



#define rd_kafka_toppar_lock(rktp)   mtx_lock(&(rktp)->rktp_lock)
#define rd_kafka_toppar_unlock(rktp) mtx_unlock(&(rktp)->rktp_lock)

static const char *
rd_kafka_toppar_name(const rd_kafka_toppar_t *rktp) RD_UNUSED;
static const char *rd_kafka_toppar_name(const rd_kafka_toppar_t *rktp) {
        static RD_TLS char ret[256];

        rd_snprintf(ret, sizeof(ret), "%.*s [%" PRId32 "]",
                    RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                    rktp->rktp_partition);

        return ret;
}
rd_kafka_toppar_t *rd_kafka_toppar_new0(rd_kafka_topic_t *rkt,
                                        int32_t partition,
                                        const char *func,
                                        int line);
#define rd_kafka_toppar_new(rkt, partition)                                    \
        rd_kafka_toppar_new0(rkt, partition, __FUNCTION__, __LINE__)
void rd_kafka_toppar_purge_and_disable_queues(rd_kafka_toppar_t *rktp);
void rd_kafka_toppar_set_fetch_state(rd_kafka_toppar_t *rktp, int fetch_state);
void rd_kafka_toppar_insert_msg(rd_kafka_toppar_t *rktp, rd_kafka_msg_t *rkm);
void rd_kafka_toppar_enq_msg(rd_kafka_toppar_t *rktp,
                             rd_kafka_msg_t *rkm,
                             rd_ts_t now);
int rd_kafka_retry_msgq(rd_kafka_msgq_t *destq,
                        rd_kafka_msgq_t *srcq,
                        int incr_retry,
                        int max_retries,
                        rd_ts_t backoff,
                        rd_kafka_msg_status_t status,
                        int (*cmp)(const void *a, const void *b),
                        rd_bool_t exponential_backoff,
                        int retry_ms,
                        int retry_max_ms);
void rd_kafka_msgq_insert_msgq(rd_kafka_msgq_t *destq,
                               rd_kafka_msgq_t *srcq,
                               int (*cmp)(const void *a, const void *b));
int rd_kafka_toppar_retry_msgq(rd_kafka_toppar_t *rktp,
                               rd_kafka_msgq_t *rkmq,
                               int incr_retry,
                               rd_kafka_msg_status_t status);
void rd_kafka_toppar_insert_msgq(rd_kafka_toppar_t *rktp,
                                 rd_kafka_msgq_t *rkmq);
void rd_kafka_toppar_enq_error(rd_kafka_toppar_t *rktp,
                               rd_kafka_resp_err_t err,
                               const char *reason);
rd_kafka_toppar_t *rd_kafka_toppar_get0(const char *func,
                                        int line,
                                        const rd_kafka_topic_t *rkt,
                                        int32_t partition,
                                        int ua_on_miss);
#define rd_kafka_toppar_get(rkt, partition, ua_on_miss)                        \
        rd_kafka_toppar_get0(__FUNCTION__, __LINE__, rkt, partition, ua_on_miss)
rd_kafka_toppar_t *rd_kafka_toppar_get2(rd_kafka_t *rk,
                                        const char *topic,
                                        int32_t partition,
                                        int ua_on_miss,
                                        int create_on_miss);
rd_kafka_toppar_t *rd_kafka_toppar_get_avail(const rd_kafka_topic_t *rkt,
                                             int32_t partition,
                                             int ua_on_miss,
                                             rd_kafka_resp_err_t *errp);

rd_kafka_toppar_t *rd_kafka_toppar_desired_get(rd_kafka_topic_t *rkt,
                                               int32_t partition);
void rd_kafka_toppar_desired_add0(rd_kafka_toppar_t *rktp);
rd_kafka_toppar_t *rd_kafka_toppar_desired_add(rd_kafka_topic_t *rkt,
                                               int32_t partition);
void rd_kafka_toppar_desired_link(rd_kafka_toppar_t *rktp);
void rd_kafka_toppar_desired_unlink(rd_kafka_toppar_t *rktp);
void rd_kafka_toppar_desired_del(rd_kafka_toppar_t *rktp);

void rd_kafka_toppar_next_offset_handle(rd_kafka_toppar_t *rktp,
                                        rd_kafka_fetch_pos_t next_pos);

void rd_kafka_toppar_broker_delegate(rd_kafka_toppar_t *rktp,
                                     rd_kafka_broker_t *rkb);


rd_kafka_resp_err_t rd_kafka_toppar_op_fetch_start(rd_kafka_toppar_t *rktp,
                                                   rd_kafka_fetch_pos_t pos,
                                                   rd_kafka_q_t *fwdq,
                                                   rd_kafka_replyq_t replyq);

rd_kafka_resp_err_t rd_kafka_toppar_op_fetch_stop(rd_kafka_toppar_t *rktp,
                                                  rd_kafka_replyq_t replyq);

rd_kafka_resp_err_t rd_kafka_toppar_op_seek(rd_kafka_toppar_t *rktp,
                                            rd_kafka_fetch_pos_t pos,
                                            rd_kafka_replyq_t replyq);

rd_kafka_resp_err_t
rd_kafka_toppar_op_pause(rd_kafka_toppar_t *rktp, int pause, int flag);

void rd_kafka_toppar_fetch_stopped(rd_kafka_toppar_t *rktp,
                                   rd_kafka_resp_err_t err);



rd_ts_t rd_kafka_broker_consumer_toppar_serve(rd_kafka_broker_t *rkb,
                                              rd_kafka_toppar_t *rktp);


void rd_kafka_toppar_offset_fetch(rd_kafka_toppar_t *rktp,
                                  rd_kafka_replyq_t replyq);

void rd_kafka_toppar_offset_request(rd_kafka_toppar_t *rktp,
                                    rd_kafka_fetch_pos_t query_pos,
                                    int backoff_ms);

void rd_kafka_toppar_purge_internal_fetch_queue_maybe(rd_kafka_toppar_t *rktp);

int rd_kafka_toppar_purge_queues(rd_kafka_toppar_t *rktp,
                                 int purge_flags,
                                 rd_bool_t include_xmit_msgq);

rd_kafka_broker_t *rd_kafka_toppar_broker(rd_kafka_toppar_t *rktp,
                                          int proper_broker);
void rd_kafka_toppar_leader_unavailable(rd_kafka_toppar_t *rktp,
                                        const char *reason,
                                        rd_kafka_resp_err_t err);

void rd_kafka_toppar_pause(rd_kafka_toppar_t *rktp, int flag);
void rd_kafka_toppar_resume(rd_kafka_toppar_t *rktp, int flag);

rd_kafka_resp_err_t rd_kafka_toppar_op_pause_resume(rd_kafka_toppar_t *rktp,
                                                    int pause,
                                                    int flag,
                                                    rd_kafka_replyq_t replyq);
rd_kafka_resp_err_t
rd_kafka_toppars_pause_resume(rd_kafka_t *rk,
                              rd_bool_t pause,
                              rd_async_t async,
                              int flag,
                              rd_kafka_topic_partition_list_t *partitions);


rd_kafka_topic_partition_t *rd_kafka_topic_partition_new(const char *topic,
                                                         int32_t partition);
void rd_kafka_topic_partition_destroy_free(void *ptr);
rd_kafka_topic_partition_t *
rd_kafka_topic_partition_copy(const rd_kafka_topic_partition_t *src);
void *rd_kafka_topic_partition_copy_void(const void *src);
void rd_kafka_topic_partition_destroy_free(void *ptr);
rd_kafka_topic_partition_t *
rd_kafka_topic_partition_new_from_rktp(rd_kafka_toppar_t *rktp);
rd_kafka_topic_partition_t *
rd_kafka_topic_partition_new_with_topic_id(rd_kafka_Uuid_t topic_id,
                                           int32_t partition);
void rd_kafka_topic_partition_set_topic_id(rd_kafka_topic_partition_t *rktpar,
                                           rd_kafka_Uuid_t topic_id);
rd_kafka_Uuid_t
rd_kafka_topic_partition_get_topic_id(const rd_kafka_topic_partition_t *rktpar);

void rd_kafka_topic_partition_list_init(
    rd_kafka_topic_partition_list_t *rktparlist,
    int size);
void rd_kafka_topic_partition_list_destroy_free(void *ptr);

void rd_kafka_topic_partition_list_clear(
    rd_kafka_topic_partition_list_t *rktparlist);

rd_kafka_topic_partition_t *rd_kafka_topic_partition_list_add0(
    const char *func,
    int line,
    rd_kafka_topic_partition_list_t *rktparlist,
    const char *topic,
    int32_t partition,
    rd_kafka_toppar_t *rktp,
    const rd_kafka_topic_partition_private_t *parpriv);

rd_kafka_topic_partition_t *rd_kafka_topic_partition_list_add_with_topic_id(
    rd_kafka_topic_partition_list_t *rktparlist,
    rd_kafka_Uuid_t topic_id,
    int32_t partition);

rd_kafka_topic_partition_t *
rd_kafka_topic_partition_list_add_with_topic_name_and_id(
    rd_kafka_topic_partition_list_t *rktparlist,
    rd_kafka_Uuid_t topic_id,
    const char *topic,
    int32_t partition);

rd_kafka_topic_partition_t *rd_kafka_topic_partition_list_upsert(
    rd_kafka_topic_partition_list_t *rktparlist,
    const char *topic,
    int32_t partition);

rd_kafka_topic_partition_t *rd_kafka_topic_partition_list_add_copy(
    rd_kafka_topic_partition_list_t *rktparlist,
    const rd_kafka_topic_partition_t *rktpar);


void rd_kafka_topic_partition_list_add_list(
    rd_kafka_topic_partition_list_t *dst,
    const rd_kafka_topic_partition_list_t *src);

/**
 * Traverse rd_kafka_topic_partition_list_t.
 *
 * @warning \p TPLIST modifications are not allowed.
 */
#define RD_KAFKA_TPLIST_FOREACH(RKTPAR, TPLIST)                                \
        for (RKTPAR = &(TPLIST)->elems[0];                                     \
             (RKTPAR) < &(TPLIST)->elems[(TPLIST)->cnt]; RKTPAR++)

/**
 * Traverse rd_kafka_topic_partition_list_t.
 *
 * @warning \p TPLIST modifications are not allowed, but removal of the
 *          current \p RKTPAR element is allowed.
 */
#define RD_KAFKA_TPLIST_FOREACH_REVERSE(RKTPAR, TPLIST)                        \
        for (RKTPAR = &(TPLIST)->elems[(TPLIST)->cnt - 1];                     \
             (RKTPAR) >= &(TPLIST)->elems[0]; RKTPAR--)

int rd_kafka_topic_partition_match(rd_kafka_t *rk,
                                   const rd_kafka_group_member_t *rkgm,
                                   const rd_kafka_topic_partition_t *rktpar,
                                   const char *topic,
                                   int *matched_by_regex);


int rd_kafka_topic_partition_cmp(const void *_a, const void *_b);
int rd_kafka_topic_partition_by_id_cmp(const void *_a, const void *_b);
unsigned int rd_kafka_topic_partition_hash(const void *a);

int rd_kafka_topic_partition_list_find_idx(
    const rd_kafka_topic_partition_list_t *rktparlist,
    const char *topic,
    int32_t partition);

rd_kafka_topic_partition_t *rd_kafka_topic_partition_list_find_by_id(
    const rd_kafka_topic_partition_list_t *rktparlist,
    rd_kafka_Uuid_t topic_id,
    int32_t partition);

int rd_kafka_topic_partition_list_find_idx_by_id(
    const rd_kafka_topic_partition_list_t *rktparlist,
    rd_kafka_Uuid_t topic_id,
    int32_t partition);

rd_kafka_topic_partition_t *rd_kafka_topic_partition_list_find_topic_by_name(
    const rd_kafka_topic_partition_list_t *rktparlist,
    const char *topic);

rd_kafka_topic_partition_t *rd_kafka_topic_partition_list_find_topic_by_id(
    const rd_kafka_topic_partition_list_t *rktparlist,
    rd_kafka_Uuid_t topic_id);

void rd_kafka_topic_partition_list_sort_by_topic(
    rd_kafka_topic_partition_list_t *rktparlist);

void rd_kafka_topic_partition_list_sort_by_topic_id(
    rd_kafka_topic_partition_list_t *rktparlist);

void rd_kafka_topic_partition_list_reset_offsets(
    rd_kafka_topic_partition_list_t *rktparlist,
    int64_t offset);

int rd_kafka_topic_partition_list_set_offsets(
    rd_kafka_t *rk,
    rd_kafka_topic_partition_list_t *rktparlist,
    int from_rktp,
    int64_t def_value,
    int is_commit);

int rd_kafka_topic_partition_list_count_abs_offsets(
    const rd_kafka_topic_partition_list_t *rktparlist);

int rd_kafka_topic_partition_list_cmp(const void *_a,
                                      const void *_b,
                                      int (*cmp)(const void *, const void *));

/**
 * Creates a new empty topic partition private.
 *
 * @remark This struct is dynamically allocated and hence should be freed.
 */
static RD_UNUSED RD_INLINE rd_kafka_topic_partition_private_t *
rd_kafka_topic_partition_private_new() {
        rd_kafka_topic_partition_private_t *parpriv;
        parpriv                       = rd_calloc(1, sizeof(*parpriv));
        parpriv->leader_epoch         = -1;
        parpriv->current_leader_epoch = -1;
        return parpriv;
}

/**
 * @returns (and creates if necessary) the ._private glue object.
 */
static RD_UNUSED RD_INLINE rd_kafka_topic_partition_private_t *
rd_kafka_topic_partition_get_private(rd_kafka_topic_partition_t *rktpar) {
        rd_kafka_topic_partition_private_t *parpriv;

        if (!(parpriv = rktpar->_private)) {
                parpriv          = rd_kafka_topic_partition_private_new();
                rktpar->_private = parpriv;
        }

        return parpriv;
}


/**
 * @returns the partition leader current epoch, if relevant and known,
 *          else -1.
 *
 * @param rktpar Partition object.
 *
 * @remark See KIP-320 for more information.
 */
int32_t rd_kafka_topic_partition_get_current_leader_epoch(
    const rd_kafka_topic_partition_t *rktpar);


/**
 * @brief Sets the partition leader current epoch (use -1 to clear).
 *
 * @param rktpar Partition object.
 * @param leader_epoch Partition leader current epoch, use -1 to reset.
 *
 * @remark See KIP-320 for more information.
 */
void rd_kafka_topic_partition_set_current_leader_epoch(
    rd_kafka_topic_partition_t *rktpar,
    int32_t leader_epoch);

/**
 * @returns the partition's rktp if set (no refcnt increase), else NULL.
 */
static RD_INLINE RD_UNUSED rd_kafka_toppar_t *
rd_kafka_topic_partition_toppar(rd_kafka_t *rk,
                                const rd_kafka_topic_partition_t *rktpar) {
        const rd_kafka_topic_partition_private_t *parpriv;

        if ((parpriv = rktpar->_private))
                return parpriv->rktp;

        return NULL;
}

rd_kafka_toppar_t *
rd_kafka_topic_partition_ensure_toppar(rd_kafka_t *rk,
                                       rd_kafka_topic_partition_t *rktpar,
                                       rd_bool_t create_on_miss);

/**
 * @returns (and sets if necessary) the \p rktpar's ._private.
 * @remark a new reference is returned.
 */
static RD_INLINE RD_UNUSED rd_kafka_toppar_t *
rd_kafka_topic_partition_get_toppar(rd_kafka_t *rk,
                                    rd_kafka_topic_partition_t *rktpar,
                                    rd_bool_t create_on_miss) {
        rd_kafka_toppar_t *rktp;

        rktp =
            rd_kafka_topic_partition_ensure_toppar(rk, rktpar, create_on_miss);

        if (rktp)
                rd_kafka_toppar_keep(rktp);

        return rktp;
}



void rd_kafka_topic_partition_list_update_toppars(
    rd_kafka_t *rk,
    rd_kafka_topic_partition_list_t *rktparlist,
    rd_bool_t create_on_miss);


void rd_kafka_topic_partition_list_query_leaders_async(
    rd_kafka_t *rk,
    const rd_kafka_topic_partition_list_t *rktparlist,
    int timeout_ms,
    rd_kafka_replyq_t replyq,
    rd_kafka_op_cb_t *cb,
    void *opaque);

rd_kafka_resp_err_t rd_kafka_topic_partition_list_query_leaders(
    rd_kafka_t *rk,
    rd_kafka_topic_partition_list_t *rktparlist,
    rd_list_t *leaders,
    int timeout_ms);

int rd_kafka_topic_partition_list_get_topics(
    rd_kafka_t *rk,
    rd_kafka_topic_partition_list_t *rktparlist,
    rd_list_t *rkts);

int rd_kafka_topic_partition_list_get_topic_names(
    const rd_kafka_topic_partition_list_t *rktparlist,
    rd_list_t *topics,
    int include_regex);

void rd_kafka_topic_partition_list_log(
    rd_kafka_t *rk,
    const char *fac,
    int dbg,
    const rd_kafka_topic_partition_list_t *rktparlist);

#define RD_KAFKA_FMT_F_OFFSET   0x1 /* Print offset */
#define RD_KAFKA_FMT_F_ONLY_ERR 0x2 /* Only include errored entries */
#define RD_KAFKA_FMT_F_NO_ERR   0x4 /* Dont print error string */
const char *rd_kafka_topic_partition_list_str(
    const rd_kafka_topic_partition_list_t *rktparlist,
    char *dest,
    size_t dest_size,
    int fmt_flags);

void rd_kafka_topic_partition_list_update(
    rd_kafka_topic_partition_list_t *dst,
    const rd_kafka_topic_partition_list_t *src);

int rd_kafka_topic_partition_leader_cmp(const void *_a, const void *_b);

void rd_kafka_topic_partition_set_from_fetch_pos(
    rd_kafka_topic_partition_t *rktpar,
    const rd_kafka_fetch_pos_t fetchpos);

void rd_kafka_topic_partition_set_metadata_from_rktp_stored(
    rd_kafka_topic_partition_t *rktpar,
    const rd_kafka_toppar_t *rktp);

static RD_UNUSED rd_kafka_fetch_pos_t rd_kafka_topic_partition_get_fetch_pos(
    const rd_kafka_topic_partition_t *rktpar) {
        rd_kafka_fetch_pos_t fetchpos = {
            rktpar->offset, rd_kafka_topic_partition_get_leader_epoch(rktpar)};

        return fetchpos;
}


/**
 * @brief Match function that returns true if partition has a valid offset.
 */
static RD_UNUSED int
rd_kafka_topic_partition_match_valid_offset(const void *elem,
                                            const void *opaque) {
        const rd_kafka_topic_partition_t *rktpar = elem;
        return rktpar->offset >= 0;
}

rd_kafka_topic_partition_list_t *rd_kafka_topic_partition_list_match(
    const rd_kafka_topic_partition_list_t *rktparlist,
    int (*match)(const void *elem, const void *opaque),
    void *opaque);

size_t rd_kafka_topic_partition_list_sum(
    const rd_kafka_topic_partition_list_t *rktparlist,
    size_t (*cb)(const rd_kafka_topic_partition_t *rktpar, void *opaque),
    void *opaque);

rd_bool_t rd_kafka_topic_partition_list_has_duplicates(
    rd_kafka_topic_partition_list_t *rktparlist,
    rd_bool_t ignore_partition);

void rd_kafka_topic_partition_list_set_err(
    rd_kafka_topic_partition_list_t *rktparlist,
    rd_kafka_resp_err_t err);

rd_kafka_resp_err_t rd_kafka_topic_partition_list_get_err(
    const rd_kafka_topic_partition_list_t *rktparlist);

int rd_kafka_topic_partition_list_regex_cnt(
    const rd_kafka_topic_partition_list_t *rktparlist);

rd_kafka_topic_partition_list_t *rd_kafka_topic_partition_list_remove_regexes(
    const rd_kafka_topic_partition_list_t *rktparlist);

rd_kafkap_str_t *rd_kafka_topic_partition_list_combine_regexes(
    const rd_kafka_topic_partition_list_t *rktparlist);

void *rd_kafka_topic_partition_list_copy_opaque(const void *src, void *opaque);

/**
 * @brief Toppar + Op version tuple used for mapping Fetched partitions
 *        back to their fetch versions.
 */
struct rd_kafka_toppar_ver {
        rd_kafka_toppar_t *rktp;
        int32_t version;
};


/**
 * @brief Toppar + Op version comparator.
 */
static RD_INLINE RD_UNUSED int rd_kafka_toppar_ver_cmp(const void *_a,
                                                       const void *_b) {
        const struct rd_kafka_toppar_ver *a = _a, *b = _b;
        const rd_kafka_toppar_t *rktp_a = a->rktp;
        const rd_kafka_toppar_t *rktp_b = b->rktp;
        int r;

        if (rktp_a->rktp_rkt != rktp_b->rktp_rkt &&
            (r = rd_kafkap_str_cmp(rktp_a->rktp_rkt->rkt_topic,
                                   rktp_b->rktp_rkt->rkt_topic)))
                return r;

        return RD_CMP(rktp_a->rktp_partition, rktp_b->rktp_partition);
}

/**
 * @brief Frees up resources for \p tver but not the \p tver itself.
 */
static RD_INLINE RD_UNUSED void
rd_kafka_toppar_ver_destroy(struct rd_kafka_toppar_ver *tver) {
        rd_kafka_toppar_destroy(tver->rktp);
}


/**
 * @returns 1 if rko version is outdated, else 0.
 */
static RD_INLINE RD_UNUSED int rd_kafka_op_version_outdated(rd_kafka_op_t *rko,
                                                            int version) {
        if (!rko->rko_version)
                return 0;

        if (version)
                return rko->rko_version < version;

        if (rko->rko_rktp)
                return rko->rko_version <
                       rd_atomic32_get(&rko->rko_rktp->rktp_version);
        return 0;
}

void rd_kafka_toppar_offset_commit_result(
    rd_kafka_toppar_t *rktp,
    rd_kafka_resp_err_t err,
    rd_kafka_topic_partition_list_t *offsets);

void rd_kafka_toppar_broker_leave_for_remove(rd_kafka_toppar_t *rktp);


/**
 * @brief Represents a leader and the partitions it is leader for.
 */
struct rd_kafka_partition_leader {
        rd_kafka_broker_t *rkb;
        rd_kafka_topic_partition_list_t *partitions;
};

static RD_UNUSED void
rd_kafka_partition_leader_destroy(struct rd_kafka_partition_leader *leader) {
        rd_kafka_broker_destroy(leader->rkb);
        rd_kafka_topic_partition_list_destroy(leader->partitions);
        rd_free(leader);
}

void rd_kafka_partition_leader_destroy_free(void *ptr);

static RD_UNUSED struct rd_kafka_partition_leader *
rd_kafka_partition_leader_new(rd_kafka_broker_t *rkb) {
        struct rd_kafka_partition_leader *leader = rd_malloc(sizeof(*leader));
        leader->rkb                              = rkb;
        rd_kafka_broker_keep(rkb);
        leader->partitions = rd_kafka_topic_partition_list_new(0);
        return leader;
}

static RD_UNUSED int rd_kafka_partition_leader_cmp(const void *_a,
                                                   const void *_b) {
        const struct rd_kafka_partition_leader *a = _a, *b = _b;
        return rd_kafka_broker_cmp(a->rkb, b->rkb);
}


int rd_kafka_toppar_pid_change(rd_kafka_toppar_t *rktp,
                               rd_kafka_pid_t pid,
                               uint64_t base_msgid);

int rd_kafka_toppar_handle_purge_queues(rd_kafka_toppar_t *rktp,
                                        rd_kafka_broker_t *rkb,
                                        int purge_flags);
void rd_kafka_purge_ua_toppar_queues(rd_kafka_t *rk);

static RD_UNUSED int rd_kafka_toppar_topic_cmp(const void *_a, const void *_b) {
        const rd_kafka_toppar_t *a = _a, *b = _b;
        return strcmp(a->rktp_rkt->rkt_topic->str, b->rktp_rkt->rkt_topic->str);
}


/**
 * @brief Set's the partitions next fetch position, i.e., the next offset
 *        to start fetching from.
 *
 * @locks rd_kafka_toppar_lock(rktp) MUST be held.
 */
static RD_UNUSED RD_INLINE void
rd_kafka_toppar_set_next_fetch_position(rd_kafka_toppar_t *rktp,
                                        rd_kafka_fetch_pos_t next_pos) {
        rktp->rktp_next_fetch_start = next_pos;
}

/**
 * @brief Sets the offset validation position.
 *
 * @locks rd_kafka_toppar_lock(rktp) MUST be held.
 */
static RD_UNUSED RD_INLINE void rd_kafka_toppar_set_offset_validation_position(
    rd_kafka_toppar_t *rktp,
    rd_kafka_fetch_pos_t offset_validation_pos) {
        rktp->rktp_offset_validation_pos = offset_validation_pos;
}

rd_kafka_topic_partition_list_t *
rd_kafka_topic_partition_list_intersection_by_name(
    rd_kafka_topic_partition_list_t *a,
    rd_kafka_topic_partition_list_t *b);

rd_kafka_topic_partition_list_t *
rd_kafka_topic_partition_list_difference_by_name(
    rd_kafka_topic_partition_list_t *a,
    rd_kafka_topic_partition_list_t *b);

rd_kafka_topic_partition_list_t *
rd_kafka_topic_partition_list_union_by_name(rd_kafka_topic_partition_list_t *a,
                                            rd_kafka_topic_partition_list_t *b);

rd_kafka_topic_partition_list_t *
rd_kafka_topic_partition_list_intersection_by_id(
    rd_kafka_topic_partition_list_t *a,
    rd_kafka_topic_partition_list_t *b);

rd_kafka_topic_partition_list_t *rd_kafka_topic_partition_list_difference_by_id(
    rd_kafka_topic_partition_list_t *a,
    rd_kafka_topic_partition_list_t *b);

rd_kafka_topic_partition_list_t *
rd_kafka_topic_partition_list_union_by_id(rd_kafka_topic_partition_list_t *a,
                                          rd_kafka_topic_partition_list_t *b);

#endif /* _RDKAFKA_PARTITION_H_ */
