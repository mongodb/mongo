/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2015, Magnus Edenhill
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

#ifndef _RDKAFKA_METADATA_H_
#define _RDKAFKA_METADATA_H_

#include "rdavl.h"

rd_kafka_resp_err_t rd_kafka_parse_Metadata(rd_kafka_broker_t *rkb,
                                            rd_kafka_buf_t *request,
                                            rd_kafka_buf_t *rkbuf,
                                            struct rd_kafka_metadata **mdp);

struct rd_kafka_metadata *
rd_kafka_metadata_copy(const struct rd_kafka_metadata *md, size_t size);

size_t
rd_kafka_metadata_topic_match(rd_kafka_t *rk,
                              rd_list_t *tinfos,
                              const rd_kafka_topic_partition_list_t *match,
                              rd_kafka_topic_partition_list_t *errored);
size_t
rd_kafka_metadata_topic_filter(rd_kafka_t *rk,
                               rd_list_t *tinfos,
                               const rd_kafka_topic_partition_list_t *match,
                               rd_kafka_topic_partition_list_t *errored);

void rd_kafka_metadata_log(rd_kafka_t *rk,
                           const char *fac,
                           const struct rd_kafka_metadata *md);



rd_kafka_resp_err_t
rd_kafka_metadata_refresh_topics(rd_kafka_t *rk,
                                 rd_kafka_broker_t *rkb,
                                 const rd_list_t *topics,
                                 rd_bool_t force,
                                 rd_bool_t allow_auto_create,
                                 rd_bool_t cgrp_update,
                                 const char *reason);
rd_kafka_resp_err_t
rd_kafka_metadata_refresh_known_topics(rd_kafka_t *rk,
                                       rd_kafka_broker_t *rkb,
                                       rd_bool_t force,
                                       const char *reason);
rd_kafka_resp_err_t
rd_kafka_metadata_refresh_consumer_topics(rd_kafka_t *rk,
                                          rd_kafka_broker_t *rkb,
                                          const char *reason);
rd_kafka_resp_err_t rd_kafka_metadata_refresh_brokers(rd_kafka_t *rk,
                                                      rd_kafka_broker_t *rkb,
                                                      const char *reason);
rd_kafka_resp_err_t rd_kafka_metadata_refresh_all(rd_kafka_t *rk,
                                                  rd_kafka_broker_t *rkb,
                                                  const char *reason);

rd_kafka_resp_err_t
rd_kafka_metadata_request(rd_kafka_t *rk,
                          rd_kafka_broker_t *rkb,
                          const rd_list_t *topics,
                          rd_bool_t allow_auto_create_topics,
                          rd_bool_t cgrp_update,
                          const char *reason,
                          rd_kafka_op_t *rko);



int rd_kafka_metadata_partition_id_cmp(const void *_a, const void *_b);

rd_kafka_metadata_t *
rd_kafka_metadata_new_topic_mock(const rd_kafka_metadata_topic_t *topics,
                                 size_t topic_cnt);
rd_kafka_metadata_t *rd_kafka_metadata_new_topic_mockv(size_t topic_cnt, ...);


/**
 * @{
 *
 * @brief Metadata cache
 */

struct rd_kafka_metadata_cache_entry {
        rd_avl_node_t rkmce_avlnode;                           /* rkmc_avl */
        TAILQ_ENTRY(rd_kafka_metadata_cache_entry) rkmce_link; /* rkmc_expiry */
        rd_ts_t rkmce_ts_expires;                              /* Expire time */
        rd_ts_t rkmce_ts_insert;                               /* Insert time */
        rd_kafka_metadata_topic_t rkmce_mtopic; /* Cached topic metadata */
        /* rkmce_partitions memory points here. */
};


#define RD_KAFKA_METADATA_CACHE_ERR_IS_TEMPORARY(ERR)                          \
        ((ERR) == RD_KAFKA_RESP_ERR__WAIT_CACHE ||                             \
         (ERR) == RD_KAFKA_RESP_ERR__NOENT)

#define RD_KAFKA_METADATA_CACHE_VALID(rkmce)                                   \
        !RD_KAFKA_METADATA_CACHE_ERR_IS_TEMPORARY((rkmce)->rkmce_mtopic.err)



struct rd_kafka_metadata_cache {
        rd_avl_t rkmc_avl;
        TAILQ_HEAD(, rd_kafka_metadata_cache_entry) rkmc_expiry;
        rd_kafka_timer_t rkmc_expiry_tmr;
        int rkmc_cnt;

        /* Protected by rk_lock */
        rd_list_t rkmc_observers; /**< (rd_kafka_enq_once_t*) */

        /* Protected by full_lock: */
        mtx_t rkmc_full_lock;
        int rkmc_full_topics_sent;  /* Full MetadataRequest for
                                     * all topics has been sent,
                                     * awaiting response. */
        int rkmc_full_brokers_sent; /* Full MetadataRequest for
                                     * all brokers (but not topics)
                                     * has been sent,
                                     * awaiting response. */

        rd_kafka_timer_t rkmc_query_tmr; /* Query timer for topic's without
                                          * leaders. */
        cnd_t rkmc_cnd;                  /* cache_wait_change() cond. */
        mtx_t rkmc_cnd_lock;             /* lock for rkmc_cnd */
};



void rd_kafka_metadata_cache_expiry_start(rd_kafka_t *rk);
void rd_kafka_metadata_cache_topic_update(rd_kafka_t *rk,
                                          const rd_kafka_metadata_topic_t *mdt,
                                          rd_bool_t propagate);
void rd_kafka_metadata_cache_update(rd_kafka_t *rk,
                                    const rd_kafka_metadata_t *md,
                                    int abs_update);
void rd_kafka_metadata_cache_propagate_changes(rd_kafka_t *rk);
struct rd_kafka_metadata_cache_entry *
rd_kafka_metadata_cache_find(rd_kafka_t *rk, const char *topic, int valid);
void rd_kafka_metadata_cache_purge_hints(rd_kafka_t *rk,
                                         const rd_list_t *topics);
int rd_kafka_metadata_cache_hint(rd_kafka_t *rk,
                                 const rd_list_t *topics,
                                 rd_list_t *dst,
                                 rd_kafka_resp_err_t err,
                                 rd_bool_t replace);

int rd_kafka_metadata_cache_hint_rktparlist(
    rd_kafka_t *rk,
    const rd_kafka_topic_partition_list_t *rktparlist,
    rd_list_t *dst,
    int replace);

const rd_kafka_metadata_topic_t *
rd_kafka_metadata_cache_topic_get(rd_kafka_t *rk, const char *topic, int valid);
int rd_kafka_metadata_cache_topic_partition_get(
    rd_kafka_t *rk,
    const rd_kafka_metadata_topic_t **mtopicp,
    const rd_kafka_metadata_partition_t **mpartp,
    const char *topic,
    int32_t partition,
    int valid);

int rd_kafka_metadata_cache_topics_count_exists(rd_kafka_t *rk,
                                                const rd_list_t *topics,
                                                int *metadata_agep);

void rd_kafka_metadata_fast_leader_query(rd_kafka_t *rk);

void rd_kafka_metadata_cache_init(rd_kafka_t *rk);
void rd_kafka_metadata_cache_destroy(rd_kafka_t *rk);
void rd_kafka_metadata_cache_purge(rd_kafka_t *rk, rd_bool_t purge_observers);
int rd_kafka_metadata_cache_wait_change(rd_kafka_t *rk, int timeout_ms);
void rd_kafka_metadata_cache_dump(FILE *fp, rd_kafka_t *rk);

int rd_kafka_metadata_cache_topics_to_list(rd_kafka_t *rk, rd_list_t *topics);

void rd_kafka_metadata_cache_wait_state_change_async(
    rd_kafka_t *rk,
    rd_kafka_enq_once_t *eonce);

/**@}*/
#endif /* _RDKAFKA_METADATA_H_ */
