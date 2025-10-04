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

#ifndef _RDKAFKA_METADATA_H_
#define _RDKAFKA_METADATA_H_

#include "rdavl.h"

/**
 * @brief Metadata partition internal container
 */
typedef struct rd_kafka_metadata_partition_internal_s {
        /** Partition Id */
        int32_t id;
        /** Partition leader epoch */
        int32_t leader_epoch;
        /* Racks for this partition. Sorted and de-duplicated. */
        char **racks;
        /* Count of the racks */
        size_t racks_cnt;
} rd_kafka_metadata_partition_internal_t;

/**
 * @brief Metadata topic internal container
 */
typedef struct rd_kafka_metadata_topic_internal_s {
        /** Internal metadata partition structs.
         *  same count as metadata.topics[i].partition_cnt.
         *  Sorted by Partition Id. */
        rd_kafka_metadata_partition_internal_t *partitions;
        rd_kafka_Uuid_t topic_id;
        int32_t topic_authorized_operations; /**< ACL operations allowed
                                              * for topic, -1 if not
                                              * supported by broker */
        rd_bool_t is_internal;               /**< Is topic internal to Kafka? */
} rd_kafka_metadata_topic_internal_t;


/**
 * @brief Metadata broker internal container
 */
typedef struct rd_kafka_metadata_broker_internal_s {
        /** Broker Id. */
        int32_t id;
        /** Rack Id (optional). */
        char *rack_id;
} rd_kafka_metadata_broker_internal_t;

/**
 * @brief Metadata internal container
 */
typedef struct rd_kafka_metadata_internal_s {
        rd_kafka_metadata_t
            metadata; /**< Public metadata struct. Must
                           be kept the first field so the pointer
                           can be cast to *rd_kafka_metadata_internal_t
                           when needed */
        /* Identical to metadata->brokers, but sorted by broker id. */
        struct rd_kafka_metadata_broker *brokers_sorted;
        /* Internal metadata brokers. Same count as metadata.broker_cnt.
         * Sorted by broker id. */
        rd_kafka_metadata_broker_internal_t *brokers;
        /* Internal metadata topics. Same count as metadata.topic_cnt. */
        rd_kafka_metadata_topic_internal_t *topics;
        char *cluster_id;  /**< Cluster id (optionally populated)*/
        int controller_id; /**< current controller id for cluster, -1 if not
                            * supported by broker. */
        int32_t cluster_authorized_operations; /**< ACL operations allowed
                                                * for cluster, -1 if not
                                                * supported by broker */
} rd_kafka_metadata_internal_t;

/**
 * @brief The internal metadata type corresponding to the
 *        public one.
 */
#define rd_kafka_metadata_get_internal(md) ((rd_kafka_metadata_internal_t *)md)

rd_bool_t rd_kafka_has_reliable_leader_epochs(rd_kafka_broker_t *rkb);

rd_kafka_resp_err_t
rd_kafka_parse_Metadata(rd_kafka_broker_t *rkb,
                        rd_kafka_buf_t *request,
                        rd_kafka_buf_t *rkbuf,
                        rd_kafka_metadata_internal_t **mdip);

rd_kafka_resp_err_t
rd_kafka_parse_Metadata_admin(rd_kafka_broker_t *rkb,
                              rd_kafka_buf_t *rkbuf,
                              rd_list_t *request_topics,
                              rd_kafka_metadata_internal_t **mdip);

rd_kafka_metadata_internal_t *
rd_kafka_metadata_copy(const rd_kafka_metadata_internal_t *mdi, size_t size);

rd_kafka_metadata_internal_t *
rd_kafka_metadata_copy_add_racks(const rd_kafka_metadata_internal_t *mdi,
                                 size_t size);

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

int rd_kafka_metadata_broker_internal_cmp(const void *_a, const void *_b);

int rd_kafka_metadata_broker_cmp(const void *_a, const void *_b);

void rd_kafka_metadata_partition_clear(
    struct rd_kafka_metadata_partition *rkmp);

#define rd_kafka_metadata_broker_internal_find(mdi, broker_id, broker)         \
        do {                                                                   \
                rd_kafka_metadata_broker_internal_t __key = {.id = broker_id}; \
                broker =                                                       \
                    bsearch(&__key, mdi->brokers, mdi->metadata.broker_cnt,    \
                            sizeof(rd_kafka_metadata_broker_internal_t),       \
                            rd_kafka_metadata_broker_internal_cmp);            \
        } while (0)


rd_kafka_metadata_t *
rd_kafka_metadata_new_topic_mock(const rd_kafka_metadata_topic_t *topics,
                                 size_t topic_cnt,
                                 int replication_factor,
                                 int num_brokers);
rd_kafka_metadata_t *rd_kafka_metadata_new_topic_mockv(size_t topic_cnt, ...);
rd_kafka_metadata_t *rd_kafka_metadata_new_topic_with_partition_replicas_mockv(
    int replication_factor,
    int num_brokers,
    size_t topic_cnt,
    ...);
rd_kafka_metadata_t *
rd_kafka_metadata_new_topic_with_partition_replicas_mock(int replication_factor,
                                                         int num_brokers,
                                                         char *topic_names[],
                                                         int *partition_cnts,
                                                         size_t topic_cnt);

/**
 * @{
 *
 * @brief Metadata cache
 */

struct rd_kafka_metadata_cache_entry {
        rd_avl_node_t rkmce_avlnode;       /* rkmc_avl */
        rd_avl_node_t rkmce_avlnode_by_id; /* rkmc_avl_by_id */
        TAILQ_ENTRY(rd_kafka_metadata_cache_entry) rkmce_link; /* rkmc_expiry */
        rd_ts_t rkmce_ts_expires;                              /* Expire time */
        rd_ts_t rkmce_ts_insert;                               /* Insert time */
        /** Last known leader epochs array (same size as the partition count),
         *  or NULL if not known. */
        rd_kafka_metadata_topic_t rkmce_mtopic; /* Cached topic metadata */
        /* Cached internal topic metadata */
        rd_kafka_metadata_topic_internal_t rkmce_metadata_internal_topic;
        /* rkmce_topics.partitions memory points here. */
};


#define RD_KAFKA_METADATA_CACHE_ERR_IS_TEMPORARY(ERR)                          \
        ((ERR) == RD_KAFKA_RESP_ERR__WAIT_CACHE ||                             \
         (ERR) == RD_KAFKA_RESP_ERR__NOENT)

#define RD_KAFKA_METADATA_CACHE_VALID(rkmce)                                   \
        !RD_KAFKA_METADATA_CACHE_ERR_IS_TEMPORARY((rkmce)->rkmce_mtopic.err)



struct rd_kafka_metadata_cache {
        rd_avl_t rkmc_avl;
        rd_avl_t rkmc_avl_by_id;
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



int rd_kafka_metadata_cache_delete_by_name(rd_kafka_t *rk, const char *topic);
int rd_kafka_metadata_cache_delete_by_topic_id(rd_kafka_t *rk,
                                               const rd_kafka_Uuid_t topic_id);
void rd_kafka_metadata_cache_expiry_start(rd_kafka_t *rk);
int rd_kafka_metadata_cache_purge_all_hints(rd_kafka_t *rk);
int rd_kafka_metadata_cache_topic_update(
    rd_kafka_t *rk,
    const rd_kafka_metadata_topic_t *mdt,
    const rd_kafka_metadata_topic_internal_t *mdit,
    rd_bool_t propagate,
    rd_bool_t include_metadata,
    rd_kafka_metadata_broker_internal_t *brokers,
    size_t broker_cnt,
    rd_bool_t only_existing);
void rd_kafka_metadata_cache_propagate_changes(rd_kafka_t *rk);
struct rd_kafka_metadata_cache_entry *
rd_kafka_metadata_cache_find(rd_kafka_t *rk, const char *topic, int valid);
struct rd_kafka_metadata_cache_entry *
rd_kafka_metadata_cache_find_by_id(rd_kafka_t *rk,
                                   const rd_kafka_Uuid_t topic_id,
                                   int valid);
void rd_kafka_metadata_cache_purge_hints(rd_kafka_t *rk,
                                         const rd_list_t *topics);
void rd_kafka_metadata_cache_purge_hints_by_id(rd_kafka_t *rk,
                                               const rd_list_t *topic_ids);
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

rd_kafka_op_res_t
rd_kafka_metadata_update_op(rd_kafka_t *rk, rd_kafka_metadata_internal_t *mdi);
/**@}*/
#endif /* _RDKAFKA_METADATA_H_ */
