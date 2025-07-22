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


#include "rd.h"
#include "rdkafka_int.h"
#include "rdkafka_topic.h"
#include "rdkafka_broker.h"
#include "rdkafka_request.h"
#include "rdkafka_metadata.h"

#include <string.h>
/**
 * @{
 *
 * @brief Metadata cache
 *
 * The metadata cache consists of cached topic metadata as
 * retrieved from the cluster using MetadataRequest.
 *
 * The topic cache entries are made up \c struct rd_kafka_metadata_cache_entry
 * each containing the topic name, a copy of the topic's metadata
 * and a cache expiry time.
 *
 * On update any previous entry for the topic are removed and replaced
 * with a new entry.
 *
 * The cache is also populated when the topic metadata is being requested
 * for specific topics, this will not interfere with existing cache entries
 * for topics, but for any topics not currently in the cache a new
 * entry will be added with a flag (RD_KAFKA_METADATA_CACHE_VALID(rkmce))
 * indicating that the entry is waiting to be populated by the MetadataResponse.
 * Two special error codes are used for this purpose:
 *   RD_KAFKA_RESP_ERR__NOENT - to indicate that a topic needs to be queried,
 *   RD_KAFKA_RESP_ERR__WAIT_CACHE - to indicate that a topic is being queried
 *                                   and there is no need to re-query it prior
 *                                   to the current query finishing.
 *
 * The cache is locked in its entirety with rd_kafka_wr/rdlock() by the caller
 * and the returned cache entry must only be accessed during the duration
 * of the lock.
 *
 */



/**
 * @brief Remove and free cache entry.
 *
 * @remark The expiry timer is not updated, for simplicity.
 * @locks rd_kafka_wrlock()
 */
static RD_INLINE void
rd_kafka_metadata_cache_delete(rd_kafka_t *rk,
                               struct rd_kafka_metadata_cache_entry *rkmce,
                               int unlink_avl) {
        if (unlink_avl) {
                RD_AVL_REMOVE_ELM(&rk->rk_metadata_cache.rkmc_avl, rkmce);
                if (!RD_KAFKA_UUID_IS_ZERO(
                        rkmce->rkmce_metadata_internal_topic.topic_id)) {
                        RD_AVL_REMOVE_ELM(&rk->rk_metadata_cache.rkmc_avl_by_id,
                                          rkmce);
                }
        }
        TAILQ_REMOVE(&rk->rk_metadata_cache.rkmc_expiry, rkmce, rkmce_link);
        rd_kafka_assert(NULL, rk->rk_metadata_cache.rkmc_cnt > 0);
        rk->rk_metadata_cache.rkmc_cnt--;

        rd_free(rkmce);
}

/**
 * @brief Delete cache entry by topic name
 * @locks rd_kafka_wrlock()
 * @returns 1 if entry was found and removed, else 0.
 */
int rd_kafka_metadata_cache_delete_by_name(rd_kafka_t *rk, const char *topic) {
        struct rd_kafka_metadata_cache_entry *rkmce;

        rkmce = rd_kafka_metadata_cache_find(rk, topic, 1);
        if (rkmce)
                rd_kafka_metadata_cache_delete(rk, rkmce, 1);
        return rkmce ? 1 : 0;
}

/**
 * @brief Delete cache entry by topic id
 * @locks rd_kafka_wrlock()
 * @returns 1 if entry was found and removed, else 0.
 */
int rd_kafka_metadata_cache_delete_by_topic_id(rd_kafka_t *rk,
                                               const rd_kafka_Uuid_t topic_id) {
        struct rd_kafka_metadata_cache_entry *rkmce;

        rkmce = rd_kafka_metadata_cache_find_by_id(rk, topic_id, 1);
        if (rkmce)
                rd_kafka_metadata_cache_delete(rk, rkmce, 1);
        return rkmce ? 1 : 0;
}

static int rd_kafka_metadata_cache_evict(rd_kafka_t *rk);

/**
 * @brief Cache eviction timer callback.
 * @locality rdkafka main thread
 * @locks NOT rd_kafka_*lock()
 */
static void rd_kafka_metadata_cache_evict_tmr_cb(rd_kafka_timers_t *rkts,
                                                 void *arg) {
        rd_kafka_t *rk = arg;

        rd_kafka_wrlock(rk);
        rd_kafka_metadata_cache_evict(rk);
        rd_kafka_wrunlock(rk);
}


/**
 * @brief Evict timed out entries from cache and rearm timer for
 *        next expiry.
 *
 * @returns the number of entries evicted.
 *
 * @locks_required rd_kafka_wrlock()
 */
static int rd_kafka_metadata_cache_evict(rd_kafka_t *rk) {
        int cnt     = 0;
        rd_ts_t now = rd_clock();
        struct rd_kafka_metadata_cache_entry *rkmce;

        while ((rkmce = TAILQ_FIRST(&rk->rk_metadata_cache.rkmc_expiry)) &&
               rkmce->rkmce_ts_expires <= now) {
                rd_kafka_metadata_cache_delete(rk, rkmce, 1);
                cnt++;
        }

        if (rkmce)
                rd_kafka_timer_start(&rk->rk_timers,
                                     &rk->rk_metadata_cache.rkmc_expiry_tmr,
                                     rkmce->rkmce_ts_expires - now,
                                     rd_kafka_metadata_cache_evict_tmr_cb, rk);
        else
                rd_kafka_timer_stop(&rk->rk_timers,
                                    &rk->rk_metadata_cache.rkmc_expiry_tmr, 1);

        rd_kafka_dbg(rk, METADATA, "METADATA",
                     "Expired %d entries from metadata cache "
                     "(%d entries remain)",
                     cnt, rk->rk_metadata_cache.rkmc_cnt);

        if (cnt)
                rd_kafka_metadata_cache_propagate_changes(rk);

        return cnt;
}


/**
 * @brief Remove all cache hints,.
 *        This is done when the Metadata response has been parsed and
 *        replaced hints with existing topic information, thus this will
 *        only remove unmatched topics from the cache.
 *
 * @returns the number of purged hints
 *
 * @locks_required rd_kafka_wrlock()
 */
int rd_kafka_metadata_cache_purge_all_hints(rd_kafka_t *rk) {
        int cnt = 0;
        struct rd_kafka_metadata_cache_entry *rkmce, *tmp;

        TAILQ_FOREACH_SAFE(rkmce, &rk->rk_metadata_cache.rkmc_expiry,
                           rkmce_link, tmp) {
                if (!RD_KAFKA_METADATA_CACHE_VALID(rkmce)) {
                        rd_kafka_metadata_cache_delete(rk, rkmce, 1);
                        cnt++;
                }
        }

        return cnt;
}


/**
 * @brief Find cache entry by topic name
 *
 * @param valid: entry must be valid (not hint)
 *
 * @locks rd_kafka_*lock()
 */
struct rd_kafka_metadata_cache_entry *
rd_kafka_metadata_cache_find(rd_kafka_t *rk, const char *topic, int valid) {
        struct rd_kafka_metadata_cache_entry skel, *rkmce;
        skel.rkmce_mtopic.topic = (char *)topic;
        rkmce = RD_AVL_FIND(&rk->rk_metadata_cache.rkmc_avl, &skel);
        if (rkmce && (!valid || RD_KAFKA_METADATA_CACHE_VALID(rkmce)))
                return rkmce;
        return NULL;
}

/**
 * @brief Find cache entry by topic id
 *
 * @param valid: entry must be valid (not hint)
 *
 * @locks rd_kafka_*lock()
 */
struct rd_kafka_metadata_cache_entry *
rd_kafka_metadata_cache_find_by_id(rd_kafka_t *rk,
                                   const rd_kafka_Uuid_t topic_id,
                                   int valid) {
        struct rd_kafka_metadata_cache_entry skel, *rkmce;
        skel.rkmce_metadata_internal_topic.topic_id = topic_id;
        rkmce = RD_AVL_FIND(&rk->rk_metadata_cache.rkmc_avl_by_id, &skel);
        if (rkmce && (!valid || RD_KAFKA_METADATA_CACHE_VALID(rkmce)))
                return rkmce;
        return NULL;
}


/**
 * @brief Partition (id) comparator
 */
int rd_kafka_metadata_partition_id_cmp(const void *_a, const void *_b) {
        const rd_kafka_metadata_partition_t *a = _a, *b = _b;
        return RD_CMP(a->id, b->id);
}

/**
 * @brief Creates a new metadata cache entry.
 *
 * @param mdt Topic to insert in the cache entry.
 * @param mdti Topic to insert in the cache entry (internal structure).
 * @param include_racks Include partition racks.
 *
 * @return The new metadata cache entry, to free with `rd_free`.
 */
static struct rd_kafka_metadata_cache_entry *rd_kafka_metadata_cache_entry_new(
    const rd_kafka_metadata_topic_t *mtopic,
    const rd_kafka_metadata_topic_internal_t *metadata_internal_topic,
    rd_bool_t include_racks) {
        struct rd_kafka_metadata_cache_entry *rkmce;
        rd_tmpabuf_t tbuf;
        int i;

        /* Metadata is stored in one contigious buffer where structs and
         * and pointed-to fields are layed out in a memory aligned fashion.
         * rd_tmpabuf_t provides the infrastructure to do this.
         * Because of this we copy all the structs verbatim but
         * any pointer fields needs to be copied explicitly to update
         * the pointer address.
         * See also rd_kafka_metadata_cache_delete which frees this. */
        rd_tmpabuf_new(&tbuf, 0, rd_true /*assert on fail*/);

        rd_tmpabuf_add_alloc(&tbuf, sizeof(*rkmce));
        rd_tmpabuf_add_alloc(&tbuf, strlen(mtopic->topic) + 1);
        rd_tmpabuf_add_alloc(&tbuf, mtopic->partition_cnt *
                                        sizeof(*mtopic->partitions));
        rd_tmpabuf_add_alloc(&tbuf,
                             mtopic->partition_cnt *
                                 sizeof(*metadata_internal_topic->partitions));

        for (i = 0; include_racks && i < mtopic->partition_cnt; i++) {
                size_t j;
                rd_tmpabuf_add_alloc(
                    &tbuf, metadata_internal_topic->partitions[i].racks_cnt *
                               sizeof(char *));
                for (j = 0;
                     j < metadata_internal_topic->partitions[i].racks_cnt;
                     j++) {
                        rd_tmpabuf_add_alloc(
                            &tbuf, strlen(metadata_internal_topic->partitions[i]
                                              .racks[j]) +
                                       1);
                }
        }

        rd_tmpabuf_finalize(&tbuf);

        rkmce = rd_tmpabuf_alloc(&tbuf, sizeof(*rkmce));

        rkmce->rkmce_mtopic = *mtopic;

        rkmce->rkmce_metadata_internal_topic = *metadata_internal_topic;

        /* Copy topic name and update pointer */
        rkmce->rkmce_mtopic.topic = rd_tmpabuf_write_str(&tbuf, mtopic->topic);

        /* Copy partition array and update pointer */
        rkmce->rkmce_mtopic.partitions = rd_tmpabuf_write(
            &tbuf, mtopic->partitions,
            mtopic->partition_cnt * sizeof(*mtopic->partitions));

        /* Copy partition array (internal) and update pointer */
        rkmce->rkmce_metadata_internal_topic.partitions =
            rd_tmpabuf_write(&tbuf, metadata_internal_topic->partitions,
                             mtopic->partition_cnt *
                                 sizeof(*metadata_internal_topic->partitions));


        /* Sort partitions for future bsearch() lookups. */
        qsort(rkmce->rkmce_mtopic.partitions, rkmce->rkmce_mtopic.partition_cnt,
              sizeof(*rkmce->rkmce_mtopic.partitions),
              rd_kafka_metadata_partition_id_cmp);

        /* partitions (internal) are already sorted. */

        if (include_racks) {
                for (i = 0; i < rkmce->rkmce_mtopic.partition_cnt; i++) {
                        size_t j;
                        rd_kafka_metadata_partition_t *mdp =
                            &rkmce->rkmce_mtopic.partitions[i];
                        rd_kafka_metadata_partition_internal_t *mdpi =
                            &rkmce->rkmce_metadata_internal_topic.partitions[i];
                        rd_kafka_metadata_partition_internal_t *mdpi_orig =
                            &metadata_internal_topic->partitions[i];

                        if (mdp->replica_cnt == 0 || mdpi->racks_cnt == 0)
                                continue;

                        mdpi->racks = rd_tmpabuf_alloc(
                            &tbuf, sizeof(char *) * mdpi->racks_cnt);
                        for (j = 0; j < mdpi_orig->racks_cnt; j++)
                                mdpi->racks[j] = rd_tmpabuf_write_str(
                                    &tbuf, mdpi_orig->racks[j]);
                }
        }

        /* Clear uncached fields. */
        for (i = 0; i < mtopic->partition_cnt; i++) {
                rkmce->rkmce_mtopic.partitions[i].replicas    = NULL;
                rkmce->rkmce_mtopic.partitions[i].replica_cnt = 0;
                rkmce->rkmce_mtopic.partitions[i].isrs        = NULL;
                rkmce->rkmce_mtopic.partitions[i].isr_cnt     = 0;
        }

        return rkmce;
}

/**
 * @brief Add (and replace) cache entry for topic.
 *
 * @locks_required rd_kafka_wrlock()
 */
static struct rd_kafka_metadata_cache_entry *
rd_kafka_metadata_cache_insert(rd_kafka_t *rk,
                               struct rd_kafka_metadata_cache_entry *rkmce,
                               rd_ts_t now,
                               rd_ts_t ts_expires) {
        struct rd_kafka_metadata_cache_entry *old, *old_by_id = NULL;
        TAILQ_INSERT_TAIL(&rk->rk_metadata_cache.rkmc_expiry, rkmce,
                          rkmce_link);
        rk->rk_metadata_cache.rkmc_cnt++;
        rkmce->rkmce_ts_expires = ts_expires;
        rkmce->rkmce_ts_insert  = now;

        /* Insert (and replace existing) entry. */
        old = RD_AVL_INSERT(&rk->rk_metadata_cache.rkmc_avl, rkmce,
                            rkmce_avlnode);
        /* Insert (and replace existing) entry into the AVL tree sorted
         * by topic id. */
        if (!RD_KAFKA_UUID_IS_ZERO(
                rkmce->rkmce_metadata_internal_topic.topic_id)) {
                /* If topic id isn't zero insert cache entry into this tree */
                old_by_id = RD_AVL_INSERT(&rk->rk_metadata_cache.rkmc_avl_by_id,
                                          rkmce, rkmce_avlnode_by_id);
        }
        if (old &&
            !RD_KAFKA_UUID_IS_ZERO(
                old->rkmce_metadata_internal_topic.topic_id) &&
            rd_kafka_Uuid_cmp(rkmce->rkmce_metadata_internal_topic.topic_id,
                              old->rkmce_metadata_internal_topic.topic_id) !=
                0) {
                /* If it had a different topic id, remove it from the tree */
                RD_AVL_REMOVE_ELM(&rk->rk_metadata_cache.rkmc_avl_by_id, old);
        }
        if (old) {
                /* Delete and free old cache entry */
                rd_kafka_metadata_cache_delete(rk, old, 0);
        }
        if (old_by_id && old_by_id != old) {
                rd_dassert(
                    !*"Different cache entries for topic name and topic id");
        }

        /* Explicitly not freeing the tmpabuf since rkmce points to its
         * memory. */
        return rkmce;
}

/**
 * @brief Add (and replace) cache entry for topic.
 *
 * This makes a copy of \p mtopic and \p metadata_internal_topic ,
 *
 * @locks_required rd_kafka_wrlock()
 */
static struct rd_kafka_metadata_cache_entry *rd_kafka_metadata_cache_insert_new(
    rd_kafka_t *rk,
    const rd_kafka_metadata_topic_t *mtopic,
    const rd_kafka_metadata_topic_internal_t *metadata_internal_topic,
    rd_ts_t now,
    rd_ts_t ts_expires,
    rd_bool_t include_racks) {
        /* Create entry */
        struct rd_kafka_metadata_cache_entry *rkmce =
            rd_kafka_metadata_cache_entry_new(mtopic, metadata_internal_topic,
                                              include_racks);
        /* Insert/replace entry */
        return rd_kafka_metadata_cache_insert(rk, rkmce, now, ts_expires);
}

/**
 * @brief Purge the metadata cache
 *
 * @locks_required rd_kafka_wrlock()
 */
void rd_kafka_metadata_cache_purge(rd_kafka_t *rk, rd_bool_t purge_observers) {
        struct rd_kafka_metadata_cache_entry *rkmce;
        int was_empty = TAILQ_EMPTY(&rk->rk_metadata_cache.rkmc_expiry);

        while ((rkmce = TAILQ_FIRST(&rk->rk_metadata_cache.rkmc_expiry)))
                rd_kafka_metadata_cache_delete(rk, rkmce, 1);

        rd_kafka_timer_stop(&rk->rk_timers,
                            &rk->rk_metadata_cache.rkmc_expiry_tmr, 1);

        if (!was_empty)
                rd_kafka_metadata_cache_propagate_changes(rk);

        if (purge_observers)
                rd_list_clear(&rk->rk_metadata_cache.rkmc_observers);
}


/**
 * @brief Start or update the cache expiry timer.
 *        Typically done after a series of cache_topic_update()
 *
 * @locks rd_kafka_wrlock()
 */
void rd_kafka_metadata_cache_expiry_start(rd_kafka_t *rk) {
        struct rd_kafka_metadata_cache_entry *rkmce;

        if ((rkmce = TAILQ_FIRST(&rk->rk_metadata_cache.rkmc_expiry)))
                rd_kafka_timer_start(&rk->rk_timers,
                                     &rk->rk_metadata_cache.rkmc_expiry_tmr,
                                     rkmce->rkmce_ts_expires - rd_clock(),
                                     rd_kafka_metadata_cache_evict_tmr_cb, rk);
}

#define rd_kafka_metadata_cache_topic_update_replace_partition(                \
    current_partition, new_partition, current_partition_cnt,                   \
    new_partition_cnt, partition)                                              \
        ((partition) < (current_partition_cnt) &&                              \
                 (partition) >= (new_partition_cnt)                            \
             ? rd_false                                                        \
         : (partition) < (new_partition_cnt) &&                                \
                 (partition) >= (current_partition_cnt)                        \
             ? rd_true                                                         \
             : (new_partition).leader_epoch == -1 ||                           \
                   (new_partition).leader_epoch >=                             \
                       (current_partition.leader_epoch));


static struct rd_kafka_metadata_cache_entry *
rd_kafka_metadata_cache_topic_update_merge_partitions(
    rd_kafka_t *rk,
    struct rd_kafka_metadata_cache_entry *rkmce_current,
    const rd_kafka_metadata_topic_t *mdt,
    const rd_kafka_metadata_topic_internal_t *mdti,
    rd_bool_t include_racks,
    rd_bool_t has_reliable_epochs) {
        rd_tmpabuf_t tbuf;
        struct rd_kafka_metadata_cache_entry *rkmce;
        size_t i, current_partition_cnt, new_partition_cnt, partition_cnt;

        if (!has_reliable_epochs || !rkmce_current ||
            /* Different topic ids */
            rd_kafka_Uuid_cmp(
                mdti->topic_id,
                rkmce_current->rkmce_metadata_internal_topic.topic_id) != 0) {
                return rd_kafka_metadata_cache_entry_new(mdt, mdti,
                                                         include_racks);
        }

        current_partition_cnt = rkmce_current->rkmce_mtopic.partition_cnt;
        new_partition_cnt     = mdt->partition_cnt;
        partition_cnt = RD_MAX(current_partition_cnt, new_partition_cnt);

        rd_tmpabuf_new(&tbuf, sizeof(*rkmce), rd_true /*assert on fail*/);
        rd_tmpabuf_add_alloc(&tbuf, sizeof(*rkmce));
        rd_tmpabuf_add_alloc(&tbuf, strlen(mdt->topic) + 1);
        rd_tmpabuf_add_alloc(&tbuf, partition_cnt * sizeof(*mdt->partitions));
        rd_tmpabuf_add_alloc(&tbuf, partition_cnt * sizeof(*mdti->partitions));

        for (i = 0; include_racks && i < partition_cnt; i++) {
                size_t j;
                rd_kafka_metadata_partition_internal_t *partition_internal;
                rd_bool_t replace_partition =
                    rd_kafka_metadata_cache_topic_update_replace_partition(
                        rkmce_current->rkmce_metadata_internal_topic
                            .partitions[i],
                        mdti->partitions[i], current_partition_cnt,
                        new_partition_cnt, i);

                partition_internal =
                    replace_partition
                        ? &mdti->partitions[i]
                        : &rkmce_current->rkmce_metadata_internal_topic
                               .partitions[i];
                rd_tmpabuf_add_alloc(&tbuf, partition_internal->racks_cnt *
                                                sizeof(char *));
                for (j = 0; j < partition_internal->racks_cnt; j++) {
                        rd_tmpabuf_add_alloc(
                            &tbuf, strlen(partition_internal->racks[j]) + 1);
                }
        }

        rd_tmpabuf_finalize(&tbuf);

        rkmce = rd_tmpabuf_alloc(&tbuf, sizeof(*rkmce));

        rkmce->rkmce_mtopic = *mdt;

        rkmce->rkmce_metadata_internal_topic = *mdti;

        /* Copy topic name */
        rkmce->rkmce_mtopic.topic = rd_tmpabuf_write_str(&tbuf, mdt->topic);

        /* Allocate partition array */
        rkmce->rkmce_mtopic.partitions =
            rd_tmpabuf_alloc(&tbuf, partition_cnt * sizeof(*mdt->partitions));

        /* Allocate partition array (internal) */
        rkmce->rkmce_metadata_internal_topic.partitions =
            rd_tmpabuf_alloc(&tbuf, partition_cnt * sizeof(*mdti->partitions));

        for (i = 0; i < partition_cnt; i++) {
                struct rd_kafka_metadata_partition *partition;
                rd_kafka_metadata_partition_internal_t *partition_internal;

                rd_bool_t replace_partition =
                    rd_kafka_metadata_cache_topic_update_replace_partition(
                        rkmce_current->rkmce_metadata_internal_topic
                            .partitions[i],
                        mdti->partitions[i], current_partition_cnt,
                        new_partition_cnt, i);

                if (replace_partition) {
                        partition          = &mdt->partitions[i];
                        partition_internal = &mdti->partitions[i];
                } else {
                        partition = &rkmce_current->rkmce_mtopic.partitions[i];
                        partition_internal =
                            &rkmce_current->rkmce_metadata_internal_topic
                                 .partitions[i];
                }

                rkmce->rkmce_mtopic.partitions[i] = *partition;
                rkmce->rkmce_metadata_internal_topic.partitions[i] =
                    *partition_internal;

                if (include_racks) {
                        size_t j;
                        rkmce->rkmce_metadata_internal_topic.partitions[i]
                            .racks = rd_tmpabuf_alloc(
                            &tbuf,
                            partition_internal->racks_cnt * sizeof(char *));
                        rkmce->rkmce_metadata_internal_topic.partitions[i]
                            .racks_cnt = partition_internal->racks_cnt;
                        for (j = 0; j < partition_internal->racks_cnt; j++) {
                                rkmce->rkmce_metadata_internal_topic
                                    .partitions[i]
                                    .racks[j] = rd_tmpabuf_write_str(
                                    &tbuf, partition_internal->racks[j]);
                        }
                } else {
                        rkmce->rkmce_metadata_internal_topic.partitions[i]
                            .racks = NULL;
                        rkmce->rkmce_metadata_internal_topic.partitions[i]
                            .racks_cnt = 0;
                }
        }
        return rkmce;
}

/**
 * @brief Update the metadata cache for a single topic
 *        with the provided metadata.
 *
 * If the topic has a temporary error the existing entry is removed
 * and no new entry is added, which avoids the topic to be
 * suppressed in upcoming metadata requests because being in the cache.
 * In other words: we want to re-query errored topics.
 * If the broker reports ERR_UNKNOWN_TOPIC_OR_PART we add a negative cache
 * entry with an low expiry time, this is so that client code (cgrp) knows
 * the topic has been queried but did not exist, otherwise it would wait
 * forever for the unknown topic to surface.
 *
 * For permanent errors (authorization failures), we keep
 * the entry cached for metadata.max.age.ms.
 *
 * @param mdt Topic to insert in the cache entry.
 * @param mdti Topic to insert in the cache entry (internal structure).
 * @param propagate Propagate metadata cache changes now.
 * @param include_racks Include partition racks.
 * @param has_reliable_leader_epochs Comes from a broker with reliable leader
 * epochs.
 *
 * @return 1 on metadata change, 0 when no change was applied
 *
 * @remark The cache expiry timer will not be updated/started,
 *         call rd_kafka_metadata_cache_expiry_start() instead.
 *
 * @locks rd_kafka_wrlock()
 */
int rd_kafka_metadata_cache_topic_update(
    rd_kafka_t *rk,
    const rd_kafka_metadata_topic_t *mdt,
    const rd_kafka_metadata_topic_internal_t *mdti,
    rd_bool_t propagate,
    rd_bool_t include_racks,
    rd_bool_t has_reliable_leader_epochs) {
        rd_ts_t now        = rd_clock();
        rd_ts_t ts_expires = now + (rk->rk_conf.metadata_max_age_ms * 1000);
        int changed        = 0;

        if (likely(mdt->topic != NULL)) {
                struct rd_kafka_metadata_cache_entry *rkmce,
                    *rkmce_current                           = NULL;
                rd_kafka_metadata_topic_internal_t mdti_copy = *mdti;
                switch (mdt->err) {
                case RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART:
                        /* Cache unknown topics for metadata.propagation.max.ms
                         * to allow the cgrp logic to find negative cache hits.
                         * and to avoid false reappearances of the topic
                         * after deletion. */
                        ts_expires = RD_MIN(ts_expires, now + (100 * 1000));

                        /* Continue */
                case RD_KAFKA_RESP_ERR_NO_ERROR:
                        rkmce_current =
                            rd_kafka_metadata_cache_find(rk, mdt->topic, 1);
                        if (mdt->err ==
                                RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART &&
                            rkmce_current &&
                            RD_KAFKA_UUID_IS_ZERO(mdti->topic_id) &&
                            !RD_KAFKA_UUID_IS_ZERO(
                                rkmce_current->rkmce_metadata_internal_topic
                                    .topic_id)) {
                                /* Keep the existing topic id to detect
                                 * if the same id is received again
                                 * as existing */
                                mdti_copy.topic_id =
                                    rkmce_current->rkmce_metadata_internal_topic
                                        .topic_id;
                        }

                        /* Continue */
                case RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED:
                        rkmce =
                            rd_kafka_metadata_cache_topic_update_merge_partitions(
                                rk, rkmce_current, mdt, &mdti_copy,
                                include_racks, has_reliable_leader_epochs);
                        /* Insert/replace entry */
                        rd_kafka_metadata_cache_insert(rk, rkmce, now,
                                                       ts_expires);
                        changed = 1;
                        break;
                default:
                        break;
                }
        } else {
                /* Cache entry found but no topic name:
                 * delete it. */
                changed = rd_kafka_metadata_cache_delete_by_topic_id(
                    rk, mdti->topic_id);
        }

        if (changed && propagate)
                rd_kafka_metadata_cache_propagate_changes(rk);

        return changed;
}


/**
 * @brief Remove cache hints for topics in \p topics
 *        This is done when the Metadata response has been parsed and
 *        replaced hints with existing topic information, thus this will
 *        only remove unmatched topics from the cache.
 *
 * @locks rd_kafka_wrlock()
 */
void rd_kafka_metadata_cache_purge_hints(rd_kafka_t *rk,
                                         const rd_list_t *topics) {
        const char *topic;
        int i;
        int cnt = 0;

        RD_LIST_FOREACH(topic, topics, i) {
                struct rd_kafka_metadata_cache_entry *rkmce;

                if (!(rkmce =
                          rd_kafka_metadata_cache_find(rk, topic, 0 /*any*/)) ||
                    RD_KAFKA_METADATA_CACHE_VALID(rkmce))
                        continue;

                rd_kafka_metadata_cache_delete(rk, rkmce, 1 /*unlink avl*/);
                cnt++;
        }

        if (cnt > 0) {
                rd_kafka_dbg(rk, METADATA, "METADATA",
                             "Purged %d/%d cached topic hint(s)", cnt,
                             rd_list_cnt(topics));
                rd_kafka_metadata_cache_propagate_changes(rk);
        }
}

/**
 * @brief Remove cache hints for topic ids in \p topic_ids
 *        This is done when the Metadata response has been parsed and
 *        replaced hints with existing topic information, thus this will
 *        only remove unmatched topics from the cache.
 *
 * @locks rd_kafka_wrlock()
 */
void rd_kafka_metadata_cache_purge_hints_by_id(rd_kafka_t *rk,
                                               const rd_list_t *topic_ids) {
        const rd_kafka_Uuid_t *topic_id;
        int i;
        int cnt = 0;

        RD_LIST_FOREACH(topic_id, topic_ids, i) {
                struct rd_kafka_metadata_cache_entry *rkmce;

                if (!(rkmce = rd_kafka_metadata_cache_find_by_id(rk, *topic_id,
                                                                 0 /*any*/)) ||
                    RD_KAFKA_METADATA_CACHE_VALID(rkmce))
                        continue;

                rd_kafka_metadata_cache_delete(rk, rkmce, 1 /*unlink avl*/);
                cnt++;
        }

        if (cnt > 0) {
                rd_kafka_dbg(rk, METADATA, "METADATA",
                             "Purged %d/%d cached topic hint(s)", cnt,
                             rd_list_cnt(topic_ids));
                rd_kafka_metadata_cache_propagate_changes(rk);
        }
}


/**
 * @brief Inserts a non-valid entry for topics in \p topics indicating
 *        that a MetadataRequest is in progress.
 *        This avoids sending multiple MetadataRequests for the same topics
 *        if there are already outstanding requests, see
 *        \c rd_kafka_metadata_refresh_topics().
 *
 * @remark These non-valid cache entries' expire time is set to the
 *         MetadataRequest timeout.
 *
 * @param dst rd_list_t(char *topicname): if not NULL: populated with
 *        topics that were added as hints to cache, e.q., topics to query.
 * @param dst rd_list_t(char *topicname)
 * @param err is the error to set on hint cache entries,
 *            typically ERR__WAIT_CACHE.
 *
 * @returns the number of topic hints inserted.
 *
 * @locks_required rd_kafka_wrlock()
 */
int rd_kafka_metadata_cache_hint(rd_kafka_t *rk,
                                 const rd_list_t *topics,
                                 rd_list_t *dst,
                                 rd_kafka_resp_err_t err) {
        const char *topic;
        rd_ts_t now        = rd_clock();
        rd_ts_t ts_expires = now + (rk->rk_conf.socket_timeout_ms * 1000);
        int i;
        int cnt = 0;

        RD_LIST_FOREACH(topic, topics, i) {
                rd_kafka_metadata_topic_t mtopic = {.topic = (char *)topic,
                                                    .err   = err};
                rd_kafka_metadata_topic_internal_t metadata_internal_topic =
                    RD_ZERO_INIT;
                /*const*/ struct rd_kafka_metadata_cache_entry *rkmce;

                if ((rkmce =
                         rd_kafka_metadata_cache_find(rk, topic, 0 /*any*/))) {
                        if (RD_KAFKA_METADATA_CACHE_VALID(rkmce) ||
                            (dst && rkmce->rkmce_mtopic.err !=
                                        RD_KAFKA_RESP_ERR__NOENT))
                                continue;
                        rkmce->rkmce_mtopic.err = err;
                        /* FALLTHRU */
                }

                rd_kafka_metadata_cache_insert_new(rk, &mtopic,
                                                   &metadata_internal_topic,
                                                   now, ts_expires, rd_false);
                cnt++;

                if (dst)
                        rd_list_add(dst, rd_strdup(topic));
        }

        if (cnt > 0)
                rd_kafka_dbg(rk, METADATA, "METADATA",
                             "Hinted cache of %d/%d topic(s) being queried",
                             cnt, rd_list_cnt(topics));

        return cnt;
}


/**
 * @brief Same as rd_kafka_metadata_cache_hint() but takes
 *        a topic+partition list as input instead.
 *
 * @locks_acquired rd_kafka_wrlock()
 */
int rd_kafka_metadata_cache_hint_rktparlist(
    rd_kafka_t *rk,
    const rd_kafka_topic_partition_list_t *rktparlist,
    rd_list_t *dst) {
        rd_list_t topics;
        int r;

        rd_list_init(&topics, rktparlist->cnt, rd_free);
        rd_kafka_topic_partition_list_get_topic_names(rktparlist, &topics,
                                                      0 /*dont include regex*/);
        rd_kafka_wrlock(rk);
        r = rd_kafka_metadata_cache_hint(rk, &topics, dst,
                                         RD_KAFKA_RESP_ERR__WAIT_CACHE);
        rd_kafka_wrunlock(rk);

        rd_list_destroy(&topics);
        return r;
}


/**
 * @brief Cache entry comparator (on topic name)
 */
static int rd_kafka_metadata_cache_entry_cmp(const void *_a, const void *_b) {
        const struct rd_kafka_metadata_cache_entry *a = _a, *b = _b;
        return strcmp(a->rkmce_mtopic.topic, b->rkmce_mtopic.topic);
}

/**
 * @brief Cache entry comparator (on topic id)
 */
static int rd_kafka_metadata_cache_entry_by_id_cmp(const void *_a,
                                                   const void *_b) {
        const struct rd_kafka_metadata_cache_entry *a = _a, *b = _b;
        return rd_kafka_Uuid_cmp(a->rkmce_metadata_internal_topic.topic_id,
                                 b->rkmce_metadata_internal_topic.topic_id);
}


/**
 * @brief Initialize the metadata cache
 *
 * @locks rd_kafka_wrlock()
 */
void rd_kafka_metadata_cache_init(rd_kafka_t *rk) {
        rd_avl_init(&rk->rk_metadata_cache.rkmc_avl,
                    rd_kafka_metadata_cache_entry_cmp, 0);
        rd_avl_init(&rk->rk_metadata_cache.rkmc_avl_by_id,
                    rd_kafka_metadata_cache_entry_by_id_cmp, 0);
        TAILQ_INIT(&rk->rk_metadata_cache.rkmc_expiry);
        mtx_init(&rk->rk_metadata_cache.rkmc_full_lock, mtx_plain);
        mtx_init(&rk->rk_metadata_cache.rkmc_cnd_lock, mtx_plain);
        cnd_init(&rk->rk_metadata_cache.rkmc_cnd);
        rd_list_init(&rk->rk_metadata_cache.rkmc_observers, 8,
                     rd_kafka_enq_once_trigger_destroy);
}

/**
 * @brief Purge and destroy metadata cache.
 *
 * @locks_required rd_kafka_wrlock()
 */
void rd_kafka_metadata_cache_destroy(rd_kafka_t *rk) {
        rd_list_destroy(&rk->rk_metadata_cache.rkmc_observers);
        rd_kafka_timer_stop(&rk->rk_timers,
                            &rk->rk_metadata_cache.rkmc_query_tmr, 1 /*lock*/);
        rd_kafka_metadata_cache_purge(rk, rd_true /*observers too*/);
        mtx_destroy(&rk->rk_metadata_cache.rkmc_full_lock);
        mtx_destroy(&rk->rk_metadata_cache.rkmc_cnd_lock);
        cnd_destroy(&rk->rk_metadata_cache.rkmc_cnd);
        rd_avl_destroy(&rk->rk_metadata_cache.rkmc_avl);
        rd_avl_destroy(&rk->rk_metadata_cache.rkmc_avl_by_id);
}



/**
 * @brief Add eonce to list of async cache observers.
 *
 * @locks_required rd_kafka_wrlock()
 */
void rd_kafka_metadata_cache_wait_state_change_async(
    rd_kafka_t *rk,
    rd_kafka_enq_once_t *eonce) {
        rd_kafka_enq_once_add_source(eonce, "wait metadata cache change");
        rd_list_add(&rk->rk_metadata_cache.rkmc_observers, eonce);
}


/**
 * @brief Wait for cache update, or timeout.
 *
 * @returns 1 on cache update or 0 on timeout.
 * @locks none
 * @locality any
 */
int rd_kafka_metadata_cache_wait_change(rd_kafka_t *rk, int timeout_ms) {
        int r;
#if ENABLE_DEVEL
        rd_ts_t ts_start = rd_clock();
#endif
        mtx_lock(&rk->rk_metadata_cache.rkmc_cnd_lock);
        r = cnd_timedwait_ms(&rk->rk_metadata_cache.rkmc_cnd,
                             &rk->rk_metadata_cache.rkmc_cnd_lock, timeout_ms);
        mtx_unlock(&rk->rk_metadata_cache.rkmc_cnd_lock);

#if ENABLE_DEVEL
        rd_kafka_dbg(rk, METADATA, "CACHEWAIT", "%s wait took %dms: %s",
                     __FUNCTION__, (int)((rd_clock() - ts_start) / 1000),
                     r == thrd_success ? "succeeded" : "timed out");
#endif
        return r == thrd_success;
}


/**
 * @brief eonce trigger callback for rd_list_apply() call in
 *        rd_kafka_metadata_cache_propagate_changes()
 */
static int
rd_kafka_metadata_cache_propagate_changes_trigger_eonce(void *elem,
                                                        void *opaque) {
        rd_kafka_enq_once_t *eonce = elem;
        rd_kafka_enq_once_trigger(eonce, RD_KAFKA_RESP_ERR_NO_ERROR,
                                  "wait metadata cache change");
        return 0; /* remove eonce from list */
}


/**
 * @brief Propagate that the cache changed (but not what changed) to
 *        any cnd listeners and eonce observers.
 * @locks_required rd_kafka_wrlock(rk)
 * @locks_acquired rkmc_cnd_lock
 * @locality any
 */
void rd_kafka_metadata_cache_propagate_changes(rd_kafka_t *rk) {
        mtx_lock(&rk->rk_metadata_cache.rkmc_cnd_lock);
        cnd_broadcast(&rk->rk_metadata_cache.rkmc_cnd);
        mtx_unlock(&rk->rk_metadata_cache.rkmc_cnd_lock);

        /* Trigger observers */
        rd_list_apply(&rk->rk_metadata_cache.rkmc_observers,
                      rd_kafka_metadata_cache_propagate_changes_trigger_eonce,
                      NULL);
}

/**
 * @param mdtip If non NULL, it's set to a pointer to internal topic metadata,
 *              or to NULL if not found in cache.
 * @returns the shared metadata for a topic, or NULL if not found in
 *          cache.
 *
 * @locks rd_kafka_*lock()
 */
const rd_kafka_metadata_topic_t *rd_kafka_metadata_cache_topic_get(
    rd_kafka_t *rk,
    const char *topic,
    const rd_kafka_metadata_topic_internal_t **mdtip,
    int valid) {
        struct rd_kafka_metadata_cache_entry *rkmce;

        if (!(rkmce = rd_kafka_metadata_cache_find(rk, topic, valid))) {
                if (mdtip)
                        *mdtip = NULL;
                return NULL;
        }
        if (mdtip)
                *mdtip = &rkmce->rkmce_metadata_internal_topic;
        return &rkmce->rkmce_mtopic;
}



/**
 * @brief Looks up the shared metadata for a partition along with its topic.
 *
 * Cache entries with errors (such as auth errors) will not be returned unless
 * \p valid is set to false.
 *
 * @param mtopicp: pointer to topic metadata
 * @param mpartp: pointer to partition metadata
 * @param mdpip: pointer to internal partition metadata
 * @param valid: only return valid entries (no hints)
 *
 * @returns -1 if topic was not found in cache, 0 if topic was found
 *          but not the partition, 1 if both topic and partition was found.
 *
 * @locks rd_kafka_*lock()
 */
int rd_kafka_metadata_cache_topic_partition_get(
    rd_kafka_t *rk,
    const rd_kafka_metadata_topic_t **mtopicp,
    const rd_kafka_metadata_partition_t **mpartp,
    const rd_kafka_metadata_partition_internal_t **mdpip,
    const char *topic,
    int32_t partition,
    int valid) {

        const rd_kafka_metadata_topic_t *mtopic;
        const rd_kafka_metadata_topic_internal_t *mdti;
        const rd_kafka_metadata_partition_t *mpart;
        rd_kafka_metadata_partition_t skel = {.id = partition};

        *mtopicp = NULL;
        *mpartp  = NULL;
        *mdpip   = NULL;

        if (!(mtopic =
                  rd_kafka_metadata_cache_topic_get(rk, topic, &mdti, valid)))
                return -1;

        *mtopicp = mtopic;

        if (mtopic->err)
                return -1;

        /* Partitions array may be sparse so use bsearch lookup. */
        mpart = bsearch(&skel, mtopic->partitions, mtopic->partition_cnt,
                        sizeof(*mtopic->partitions),
                        rd_kafka_metadata_partition_id_cmp);

        if (!mpart)
                return 0;

        *mpartp = mpart;
        if (mdpip)
                *mdpip = &mdti->partitions[mpart->id];

        return 1;
}


/**
 * @returns the number of topics in \p topics that are in the cache.
 *
 * @param topics rd_list(const char *): topic names
 * @param metadata_agep: age of oldest entry will be returned.
 *
 * @locks rd_kafka_*lock()
 */
int rd_kafka_metadata_cache_topics_count_exists(rd_kafka_t *rk,
                                                const rd_list_t *topics,
                                                int *metadata_agep) {
        const char *topic;
        int i;
        int cnt     = 0;
        int max_age = -1;

        RD_LIST_FOREACH(topic, topics, i) {
                const struct rd_kafka_metadata_cache_entry *rkmce;
                int age;

                if (!(rkmce = rd_kafka_metadata_cache_find(rk, topic,
                                                           1 /*valid only*/)))
                        continue;

                age = (int)((rd_clock() - rkmce->rkmce_ts_insert) / 1000);
                if (age > max_age)
                        max_age = age;
                cnt++;
        }

        *metadata_agep = max_age;

        return cnt;
}


/**
 * @brief Add all topics in the metadata cache to \p topics, avoid duplicates.
 *
 * Element type is (char *topic_name).
 *
 * @param exclude_valid Exclude topics that have up to date metadata info.
 *
 * @returns the number of elements added to \p topics
 *
 * @locks_required rd_kafka_*lock()
 */
int rd_kafka_metadata_cache_topics_to_list(rd_kafka_t *rk,
                                           rd_list_t *topics,
                                           rd_bool_t exclude_valid) {
        const struct rd_kafka_metadata_cache_entry *rkmce;
        int precnt = rd_list_cnt(topics);

        TAILQ_FOREACH(rkmce, &rk->rk_metadata_cache.rkmc_expiry, rkmce_link) {
                /* Ignore topics that have up to date metadata info */
                if (exclude_valid && RD_KAFKA_METADATA_CACHE_VALID(rkmce))
                        continue;

                if (rd_list_find(topics, rkmce->rkmce_mtopic.topic,
                                 rd_list_cmp_str))
                        continue;

                rd_list_add(topics, rd_strdup(rkmce->rkmce_mtopic.topic));
        }

        return rd_list_cnt(topics) - precnt;
}


/**
 * @brief Dump cache to \p fp
 *
 * @locks rd_kafka_*lock()
 */
void rd_kafka_metadata_cache_dump(FILE *fp, rd_kafka_t *rk) {
        const struct rd_kafka_metadata_cache *rkmc = &rk->rk_metadata_cache;
        const struct rd_kafka_metadata_cache_entry *rkmce;
        rd_ts_t now = rd_clock();

        fprintf(fp, "Metadata cache with %d entries:\n", rkmc->rkmc_cnt);
        TAILQ_FOREACH(rkmce, &rkmc->rkmc_expiry, rkmce_link) {
                fprintf(fp,
                        "  %s (inserted %dms ago, expires in %dms, "
                        "%d partition(s), %s)%s%s\n",
                        rkmce->rkmce_mtopic.topic,
                        (int)((now - rkmce->rkmce_ts_insert) / 1000),
                        (int)((rkmce->rkmce_ts_expires - now) / 1000),
                        rkmce->rkmce_mtopic.partition_cnt,
                        RD_KAFKA_METADATA_CACHE_VALID(rkmce) ? "valid" : "hint",
                        rkmce->rkmce_mtopic.err ? " error: " : "",
                        rkmce->rkmce_mtopic.err
                            ? rd_kafka_err2str(rkmce->rkmce_mtopic.err)
                            : "");
        }
}

/**@}*/
