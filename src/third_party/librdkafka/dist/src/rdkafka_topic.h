/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012,2013 Magnus Edenhill
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

#ifndef _RDKAFKA_TOPIC_H_
#define _RDKAFKA_TOPIC_H_

#include "rdlist.h"

extern const char *rd_kafka_topic_state_names[];


/**
 * @struct Light-weight topic object which only contains the topic name.
 *
 * For use in outgoing APIs (like rd_kafka_message_t) when there is
 * no proper topic object available.
 *
 * @remark lrkt_magic[4] MUST be the first field and be set to "LRKT".
 */
struct rd_kafka_lwtopic_s {
        char lrkt_magic[4];      /**< "LRKT" */
        rd_kafka_t *lrkt_rk;     /**< Pointer to the client instance. */
        rd_refcnt_t lrkt_refcnt; /**< Refcount */
        char *lrkt_topic;        /**< Points past this struct, allocated
                                  *   along with the struct. */
};

/** Casts a topic_t to a light-weight lwtopic_t */
#define rd_kafka_rkt_lw(rkt) ((rd_kafka_lwtopic_t *)rkt)

#define rd_kafka_rkt_lw_const(rkt) ((const rd_kafka_lwtopic_t *)rkt)

/**
 * @returns true if the topic object is a light-weight topic, else false.
 */
static RD_UNUSED RD_INLINE rd_bool_t
rd_kafka_rkt_is_lw(const rd_kafka_topic_t *app_rkt) {
        const rd_kafka_lwtopic_t *lrkt = rd_kafka_rkt_lw_const(app_rkt);
        return !memcmp(lrkt->lrkt_magic, "LRKT", 4);
}

/** @returns the lwtopic_t if \p rkt is a light-weight topic, else NULL. */
static RD_UNUSED RD_INLINE rd_kafka_lwtopic_t *
rd_kafka_rkt_get_lw(rd_kafka_topic_t *rkt) {
        if (rd_kafka_rkt_is_lw(rkt))
                return rd_kafka_rkt_lw(rkt);
        return NULL;
}

void rd_kafka_lwtopic_destroy(rd_kafka_lwtopic_t *lrkt);
rd_kafka_lwtopic_t *rd_kafka_lwtopic_new(rd_kafka_t *rk, const char *topic);

static RD_UNUSED RD_INLINE void
rd_kafka_lwtopic_keep(rd_kafka_lwtopic_t *lrkt) {
        rd_refcnt_add(&lrkt->lrkt_refcnt);
}



/**
 * @struct Holds partition + transactional PID + base sequence msgid.
 *
 * Used in rkt_saved_partmsgids to restore transactional/idempotency state
 * for a partition that is lost from metadata for some time and then returns.
 */
typedef struct rd_kafka_partition_msgid_s {
        TAILQ_ENTRY(rd_kafka_partition_msgid_s) link;
        int32_t partition;
        rd_kafka_pid_t pid;
        uint64_t msgid;
        uint64_t epoch_base_msgid;
        rd_ts_t ts;
} rd_kafka_partition_msgid_t;


/*
 * @struct Internal representation of a topic.
 *
 * @remark rkt_magic[4] MUST be the first field and be set to "IRKT".
 */
struct rd_kafka_topic_s {
        char rkt_magic[4]; /**< "IRKT" */

        TAILQ_ENTRY(rd_kafka_topic_s) rkt_link;

        rd_refcnt_t rkt_refcnt;

        rwlock_t rkt_lock;
        rd_kafkap_str_t *rkt_topic;

        rd_kafka_toppar_t *rkt_ua; /**< Unassigned partition (-1) */
        rd_kafka_toppar_t **rkt_p; /**< Partition array */
        int32_t rkt_partition_cnt;

        int32_t rkt_sticky_partition;   /**< Current sticky partition.
                                         *     @locks rkt_lock */
        rd_interval_t rkt_sticky_intvl; /**< Interval to assign new
                                         *   sticky partition. */

        rd_list_t rkt_desp;                   /* Desired partitions
                                               * that are not yet seen
                                               * in the cluster. */
        rd_interval_t rkt_desp_refresh_intvl; /**< Rate-limiter for
                                               *   desired partition
                                               *   metadata refresh. */

        rd_ts_t rkt_ts_create;   /**< Topic object creation time. */
        rd_ts_t rkt_ts_metadata; /* Timestamp of last metadata
                                  * update for this topic. */

        rd_refcnt_t rkt_app_refcnt; /**< Number of active rkt's new()ed
                                     *   by application. */

        enum { RD_KAFKA_TOPIC_S_UNKNOWN,   /* No cluster information yet */
               RD_KAFKA_TOPIC_S_EXISTS,    /* Topic exists in cluster */
               RD_KAFKA_TOPIC_S_NOTEXISTS, /* Topic is not known in cluster */
               RD_KAFKA_TOPIC_S_ERROR,     /* Topic exists but is in an errored
                                            * state, such as auth failure. */
        } rkt_state;

        int rkt_flags;
#define RD_KAFKA_TOPIC_F_LEADER_UNAVAIL                                        \
        0x1 /* Leader lost/unavailable                                         \
             * for at least one partition. */

        rd_kafka_resp_err_t rkt_err; /**< Permanent error. */

        rd_kafka_t *rkt_rk;

        rd_avg_t rkt_avg_batchsize; /**< Average batch size */
        rd_avg_t rkt_avg_batchcnt;  /**< Average batch message count */

        rd_kafka_topic_conf_t rkt_conf;

        /** Idempotent/Txn producer:
         *  The PID,Epoch,base Msgid state for removed partitions. */
        TAILQ_HEAD(, rd_kafka_partition_msgid_s) rkt_saved_partmsgids;
};

#define rd_kafka_topic_rdlock(rkt)   rwlock_rdlock(&(rkt)->rkt_lock)
#define rd_kafka_topic_wrlock(rkt)   rwlock_wrlock(&(rkt)->rkt_lock)
#define rd_kafka_topic_rdunlock(rkt) rwlock_rdunlock(&(rkt)->rkt_lock)
#define rd_kafka_topic_wrunlock(rkt) rwlock_wrunlock(&(rkt)->rkt_lock)



/**
 * @brief Increase refcount and return topic object.
 */
static RD_INLINE RD_UNUSED rd_kafka_topic_t *
rd_kafka_topic_keep(rd_kafka_topic_t *rkt) {
        rd_kafka_lwtopic_t *lrkt;
        if (unlikely((lrkt = rd_kafka_rkt_get_lw(rkt)) != NULL))
                rd_kafka_lwtopic_keep(lrkt);
        else
                rd_refcnt_add(&rkt->rkt_refcnt);
        return rkt;
}

void rd_kafka_topic_destroy_final(rd_kafka_topic_t *rkt);

rd_kafka_topic_t *rd_kafka_topic_proper(rd_kafka_topic_t *app_rkt);



/**
 * @brief Loose reference to topic object as increased by ..topic_keep().
 */
static RD_INLINE RD_UNUSED void rd_kafka_topic_destroy0(rd_kafka_topic_t *rkt) {
        rd_kafka_lwtopic_t *lrkt;
        if (unlikely((lrkt = rd_kafka_rkt_get_lw(rkt)) != NULL))
                rd_kafka_lwtopic_destroy(lrkt);
        else if (unlikely(rd_refcnt_sub(&rkt->rkt_refcnt) == 0))
                rd_kafka_topic_destroy_final(rkt);
}


rd_kafka_topic_t *rd_kafka_topic_new0(rd_kafka_t *rk,
                                      const char *topic,
                                      rd_kafka_topic_conf_t *conf,
                                      int *existing,
                                      int do_lock);

rd_kafka_topic_t *rd_kafka_topic_find_fl(const char *func,
                                         int line,
                                         rd_kafka_t *rk,
                                         const char *topic,
                                         int do_lock);
rd_kafka_topic_t *rd_kafka_topic_find0_fl(const char *func,
                                          int line,
                                          rd_kafka_t *rk,
                                          const rd_kafkap_str_t *topic);
#define rd_kafka_topic_find(rk, topic, do_lock)                                \
        rd_kafka_topic_find_fl(__FUNCTION__, __LINE__, rk, topic, do_lock)
#define rd_kafka_topic_find0(rk, topic)                                        \
        rd_kafka_topic_find0_fl(__FUNCTION__, __LINE__, rk, topic)
int rd_kafka_topic_cmp_rkt(const void *_a, const void *_b);

void rd_kafka_topic_partitions_remove(rd_kafka_topic_t *rkt);

rd_bool_t rd_kafka_topic_set_notexists(rd_kafka_topic_t *rkt,
                                       rd_kafka_resp_err_t err);
rd_bool_t rd_kafka_topic_set_error(rd_kafka_topic_t *rkt,
                                   rd_kafka_resp_err_t err);

/**
 * @returns the topic's permanent error, if any.
 *
 * @locality any
 * @locks_acquired rd_kafka_topic_rdlock(rkt)
 */
static RD_INLINE RD_UNUSED rd_kafka_resp_err_t
rd_kafka_topic_get_error(rd_kafka_topic_t *rkt) {
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        rd_kafka_topic_rdlock(rkt);
        if (rkt->rkt_state == RD_KAFKA_TOPIC_S_ERROR)
                err = rkt->rkt_err;
        rd_kafka_topic_rdunlock(rkt);
        return err;
}

int rd_kafka_topic_metadata_update2(rd_kafka_broker_t *rkb,
                                    const struct rd_kafka_metadata_topic *mdt);

void rd_kafka_topic_scan_all(rd_kafka_t *rk, rd_ts_t now);


typedef struct rd_kafka_topic_info_s {
        const char *topic; /**< Allocated along with struct */
        int partition_cnt;
} rd_kafka_topic_info_t;

int rd_kafka_topic_info_topic_cmp(const void *_a, const void *_b);
int rd_kafka_topic_info_cmp(const void *_a, const void *_b);
rd_kafka_topic_info_t *rd_kafka_topic_info_new(const char *topic,
                                               int partition_cnt);
void rd_kafka_topic_info_destroy(rd_kafka_topic_info_t *ti);

int rd_kafka_topic_match(rd_kafka_t *rk,
                         const char *pattern,
                         const char *topic);

int rd_kafka_toppar_broker_update(rd_kafka_toppar_t *rktp,
                                  int32_t broker_id,
                                  rd_kafka_broker_t *rkb,
                                  const char *reason);

int rd_kafka_toppar_delegate_to_leader(rd_kafka_toppar_t *rktp);

rd_kafka_resp_err_t rd_kafka_topics_leader_query_sync(rd_kafka_t *rk,
                                                      int all_topics,
                                                      const rd_list_t *topics,
                                                      int timeout_ms);
void rd_kafka_topic_leader_query0(rd_kafka_t *rk,
                                  rd_kafka_topic_t *rkt,
                                  int do_rk_lock);
#define rd_kafka_topic_leader_query(rk, rkt)                                   \
        rd_kafka_topic_leader_query0(rk, rkt, 1 /*lock*/)

#define rd_kafka_topic_fast_leader_query(rk)                                   \
        rd_kafka_metadata_fast_leader_query(rk)

void rd_kafka_local_topics_to_list(rd_kafka_t *rk,
                                   rd_list_t *topics,
                                   int *cache_cntp);

void rd_ut_kafka_topic_set_topic_exists(rd_kafka_topic_t *rkt,
                                        int partition_cnt,
                                        int32_t leader_id);

#endif /* _RDKAFKA_TOPIC_H_ */
