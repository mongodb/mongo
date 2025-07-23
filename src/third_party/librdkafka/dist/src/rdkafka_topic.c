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

#include "rd.h"
#include "rdkafka_int.h"
#include "rdkafka_msg.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_broker.h"
#include "rdkafka_cgrp.h"
#include "rdkafka_metadata.h"
#include "rdlog.h"
#include "rdsysqueue.h"
#include "rdtime.h"
#include "rdregex.h"

#if WITH_ZSTD
#include <zstd.h>
#endif


const char *rd_kafka_topic_state_names[] = {"unknown", "exists", "notexists",
                                            "error"};


static int
rd_kafka_topic_metadata_update(rd_kafka_topic_t *rkt,
                               const struct rd_kafka_metadata_topic *mdt,
                               rd_ts_t ts_insert);


/**
 * @brief Increases the app's topic reference count.
 *
 * The app refcounts are implemented separately from the librdkafka refcounts,
 * they are increased/decreased in a separate rkt_app_refcnt to keep track of
 * its use.
 *
 * This only covers topic_new() & topic_destroy().
 * The topic_t exposed in rd_kafka_message_t is NOT covered and is handled
 * like a standard internal -> app pointer conversion (keep_a()).
 */
static void rd_kafka_topic_keep_app(rd_kafka_topic_t *rkt) {
        if (rd_refcnt_add(&rkt->rkt_app_refcnt) == 1)
                rd_kafka_topic_keep(rkt);
}

/**
 * @brief drop rkt app reference
 */
static void rd_kafka_topic_destroy_app(rd_kafka_topic_t *app_rkt) {
        rd_kafka_topic_t *rkt = app_rkt;

        rd_assert(!rd_kafka_rkt_is_lw(app_rkt));

        if (unlikely(rd_refcnt_sub(&rkt->rkt_app_refcnt) == 0))
                rd_kafka_topic_destroy0(rkt); /* final app reference lost,
                                               * loose reference from
                                               * keep_app() */
}


/**
 * Final destructor for topic. Refcnt must be 0.
 */
void rd_kafka_topic_destroy_final(rd_kafka_topic_t *rkt) {
        rd_kafka_partition_msgid_t *partmsgid, *partmsgid_tmp;

        rd_kafka_assert(rkt->rkt_rk, rd_refcnt_get(&rkt->rkt_refcnt) == 0);

        rd_kafka_wrlock(rkt->rkt_rk);
        TAILQ_REMOVE(&rkt->rkt_rk->rk_topics, rkt, rkt_link);
        rkt->rkt_rk->rk_topic_cnt--;
        rd_kafka_wrunlock(rkt->rkt_rk);

        TAILQ_FOREACH_SAFE(partmsgid, &rkt->rkt_saved_partmsgids, link,
                           partmsgid_tmp) {
                rd_free(partmsgid);
        }

        rd_kafka_assert(rkt->rkt_rk, rd_list_empty(&rkt->rkt_desp));
        rd_list_destroy(&rkt->rkt_desp);

        rd_avg_destroy(&rkt->rkt_avg_batchsize);
        rd_avg_destroy(&rkt->rkt_avg_batchcnt);

        if (rkt->rkt_topic)
                rd_kafkap_str_destroy(rkt->rkt_topic);

        rd_kafka_anyconf_destroy(_RK_TOPIC, &rkt->rkt_conf);

        rwlock_destroy(&rkt->rkt_lock);
        rd_refcnt_destroy(&rkt->rkt_app_refcnt);
        rd_refcnt_destroy(&rkt->rkt_refcnt);

        rd_free(rkt);
}

/**
 * @brief Application topic object destroy.
 * @warning MUST ONLY BE CALLED BY THE APPLICATION.
 *          Use rd_kafka_topic_destroy0() for all internal use.
 */
void rd_kafka_topic_destroy(rd_kafka_topic_t *app_rkt) {
        rd_kafka_lwtopic_t *lrkt;
        if (unlikely((lrkt = rd_kafka_rkt_get_lw(app_rkt)) != NULL))
                rd_kafka_lwtopic_destroy(lrkt);
        else
                rd_kafka_topic_destroy_app(app_rkt);
}


/**
 * Finds and returns a topic based on its name, or NULL if not found.
 * The 'rkt' refcount is increased by one and the caller must call
 * rd_kafka_topic_destroy() when it is done with the topic to decrease
 * the refcount.
 *
 * Locality: any thread
 */
rd_kafka_topic_t *rd_kafka_topic_find_fl(const char *func,
                                         int line,
                                         rd_kafka_t *rk,
                                         const char *topic,
                                         int do_lock) {
        rd_kafka_topic_t *rkt;

        if (do_lock)
                rd_kafka_rdlock(rk);
        TAILQ_FOREACH(rkt, &rk->rk_topics, rkt_link) {
                if (!rd_kafkap_str_cmp_str(rkt->rkt_topic, topic)) {
                        rd_kafka_topic_keep(rkt);
                        break;
                }
        }
        if (do_lock)
                rd_kafka_rdunlock(rk);

        return rkt;
}

/**
 * Same semantics as ..find() but takes a Kafka protocol string instead.
 */
rd_kafka_topic_t *rd_kafka_topic_find0_fl(const char *func,
                                          int line,
                                          rd_kafka_t *rk,
                                          const rd_kafkap_str_t *topic) {
        rd_kafka_topic_t *rkt;

        rd_kafka_rdlock(rk);
        TAILQ_FOREACH(rkt, &rk->rk_topics, rkt_link) {
                if (!rd_kafkap_str_cmp(rkt->rkt_topic, topic)) {
                        rd_kafka_topic_keep(rkt);
                        break;
                }
        }
        rd_kafka_rdunlock(rk);

        return rkt;
}


/**
 * @brief rd_kafka_topic_t comparator.
 */
int rd_kafka_topic_cmp_rkt(const void *_a, const void *_b) {
        rd_kafka_topic_t *rkt_a = (void *)_a, *rkt_b = (void *)_b;

        if (rkt_a == rkt_b)
                return 0;

        return rd_kafkap_str_cmp(rkt_a->rkt_topic, rkt_b->rkt_topic);
}


/**
 * @brief Destroy/free a light-weight topic object.
 */
void rd_kafka_lwtopic_destroy(rd_kafka_lwtopic_t *lrkt) {
        rd_assert(rd_kafka_rkt_is_lw((const rd_kafka_topic_t *)lrkt));
        if (rd_refcnt_sub(&lrkt->lrkt_refcnt) > 0)
                return;

        rd_refcnt_destroy(&lrkt->lrkt_refcnt);
        rd_free(lrkt);
}


/**
 * @brief Create a new light-weight topic name-only handle.
 *
 * This type of object is a light-weight non-linked alternative
 * to the proper rd_kafka_itopic_t for outgoing APIs
 * (such as rd_kafka_message_t) when there is no full topic object available.
 */
rd_kafka_lwtopic_t *rd_kafka_lwtopic_new(rd_kafka_t *rk, const char *topic) {
        rd_kafka_lwtopic_t *lrkt;
        size_t topic_len = strlen(topic);

        lrkt = rd_malloc(sizeof(*lrkt) + topic_len + 1);

        memcpy(lrkt->lrkt_magic, "LRKT", 4);
        lrkt->lrkt_rk = rk;
        rd_refcnt_init(&lrkt->lrkt_refcnt, 1);
        lrkt->lrkt_topic = (char *)(lrkt + 1);
        memcpy(lrkt->lrkt_topic, topic, topic_len + 1);

        return lrkt;
}


/**
 * @returns a proper rd_kafka_topic_t object (not light-weight)
 *          based on the input rd_kafka_topic_t app object which may
 *          either be a proper topic (which is then returned) or a light-weight
 *          topic in which case it will look up or create the proper topic
 *          object.
 *
 *          This allows the application to (unknowingly) pass a light-weight
 *          topic object to any proper-aware public API.
 */
rd_kafka_topic_t *rd_kafka_topic_proper(rd_kafka_topic_t *app_rkt) {
        rd_kafka_lwtopic_t *lrkt;

        if (likely(!(lrkt = rd_kafka_rkt_get_lw(app_rkt))))
                return app_rkt;

        /* Create proper topic object */
        return rd_kafka_topic_new0(lrkt->lrkt_rk, lrkt->lrkt_topic, NULL, NULL,
                                   0);
}


/**
 * @brief Create new topic handle.
 *
 * @locality any
 */
rd_kafka_topic_t *rd_kafka_topic_new0(rd_kafka_t *rk,
                                      const char *topic,
                                      rd_kafka_topic_conf_t *conf,
                                      int *existing,
                                      int do_lock) {
        rd_kafka_topic_t *rkt;
        const struct rd_kafka_metadata_cache_entry *rkmce;
        const char *conf_err;
        const char *used_conf_str;

        /* Verify configuration.
         * Maximum topic name size + headers must never exceed message.max.bytes
         * which is min-capped to 1000.
         * See rd_kafka_broker_produce_toppar() and rdkafka_conf.c */
        if (!topic || strlen(topic) > 512) {
                if (conf)
                        rd_kafka_topic_conf_destroy(conf);
                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__INVALID_ARG, EINVAL);
                return NULL;
        }

        if (do_lock)
                rd_kafka_wrlock(rk);
        if ((rkt = rd_kafka_topic_find(rk, topic, 0 /*no lock*/))) {
                if (do_lock)
                        rd_kafka_wrunlock(rk);
                if (conf)
                        rd_kafka_topic_conf_destroy(conf);
                if (existing)
                        *existing = 1;
                return rkt;
        }

        if (!conf) {
                if (rk->rk_conf.topic_conf) {
                        conf = rd_kafka_topic_conf_dup(rk->rk_conf.topic_conf);
                        used_conf_str = "default_topic_conf";
                } else {
                        conf          = rd_kafka_topic_conf_new();
                        used_conf_str = "empty";
                }
        } else {
                used_conf_str = "user-supplied";
        }


        /* Verify and finalize topic configuration */
        if ((conf_err = rd_kafka_topic_conf_finalize(rk->rk_type, &rk->rk_conf,
                                                     conf))) {
                if (do_lock)
                        rd_kafka_wrunlock(rk);
                /* Incompatible configuration settings */
                rd_kafka_log(rk, LOG_ERR, "TOPICCONF",
                             "Incompatible configuration settings "
                             "for topic \"%s\": %s",
                             topic, conf_err);
                rd_kafka_topic_conf_destroy(conf);
                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__INVALID_ARG, EINVAL);
                return NULL;
        }

        if (existing)
                *existing = 0;

        rkt = rd_calloc(1, sizeof(*rkt));

        memcpy(rkt->rkt_magic, "IRKT", 4);

        rkt->rkt_topic = rd_kafkap_str_new(topic, -1);
        rkt->rkt_rk    = rk;

        rkt->rkt_ts_create = rd_clock();

        rkt->rkt_conf = *conf;
        rd_free(conf); /* explicitly not rd_kafka_topic_destroy()
                        * since we dont want to rd_free internal members,
                        * just the placeholder. The internal members
                        * were copied on the line above. */

        /* Partitioner */
        if (!rkt->rkt_conf.partitioner) {
                const struct {
                        const char *str;
                        void *part;
                } part_map[] = {
                    {"random", (void *)rd_kafka_msg_partitioner_random},
                    {"consistent", (void *)rd_kafka_msg_partitioner_consistent},
                    {"consistent_random",
                     (void *)rd_kafka_msg_partitioner_consistent_random},
                    {"murmur2", (void *)rd_kafka_msg_partitioner_murmur2},
                    {"murmur2_random",
                     (void *)rd_kafka_msg_partitioner_murmur2_random},
                    {"fnv1a", (void *)rd_kafka_msg_partitioner_fnv1a},
                    {"fnv1a_random",
                     (void *)rd_kafka_msg_partitioner_fnv1a_random},
                    {NULL}};
                int i;

                /* Use "partitioner" configuration property string, if set */
                for (i = 0; rkt->rkt_conf.partitioner_str && part_map[i].str;
                     i++) {
                        if (!strcmp(rkt->rkt_conf.partitioner_str,
                                    part_map[i].str)) {
                                rkt->rkt_conf.partitioner = part_map[i].part;
                                break;
                        }
                }

                /* Default partitioner: consistent_random */
                if (!rkt->rkt_conf.partitioner) {
                        /* Make sure part_map matched something, otherwise
                         * there is a discreprency between this code
                         * and the validator in rdkafka_conf.c */
                        assert(!rkt->rkt_conf.partitioner_str);

                        rkt->rkt_conf.partitioner =
                            rd_kafka_msg_partitioner_consistent_random;
                }
        }

        if (rkt->rkt_rk->rk_conf.sticky_partition_linger_ms > 0 &&
            rkt->rkt_conf.partitioner != rd_kafka_msg_partitioner_consistent &&
            rkt->rkt_conf.partitioner != rd_kafka_msg_partitioner_murmur2 &&
            rkt->rkt_conf.partitioner != rd_kafka_msg_partitioner_fnv1a) {
                rkt->rkt_conf.random_partitioner = rd_false;
        } else {
                rkt->rkt_conf.random_partitioner = rd_true;
        }

        /* Sticky partition assignment interval */
        rd_interval_init(&rkt->rkt_sticky_intvl);

        if (rkt->rkt_conf.queuing_strategy == RD_KAFKA_QUEUE_FIFO)
                rkt->rkt_conf.msg_order_cmp = rd_kafka_msg_cmp_msgid;
        else
                rkt->rkt_conf.msg_order_cmp = rd_kafka_msg_cmp_msgid_lifo;

        if (rkt->rkt_conf.compression_codec == RD_KAFKA_COMPRESSION_INHERIT)
                rkt->rkt_conf.compression_codec = rk->rk_conf.compression_codec;

        /* Translate compression level to library-specific level and check
         * upper bound */
        switch (rkt->rkt_conf.compression_codec) {
#if WITH_ZLIB
        case RD_KAFKA_COMPRESSION_GZIP:
                if (rkt->rkt_conf.compression_level ==
                    RD_KAFKA_COMPLEVEL_DEFAULT)
                        rkt->rkt_conf.compression_level = Z_DEFAULT_COMPRESSION;
                else if (rkt->rkt_conf.compression_level >
                         RD_KAFKA_COMPLEVEL_GZIP_MAX)
                        rkt->rkt_conf.compression_level =
                            RD_KAFKA_COMPLEVEL_GZIP_MAX;
                break;
#endif
        case RD_KAFKA_COMPRESSION_KLZ4:
                if (rkt->rkt_conf.compression_level ==
                    RD_KAFKA_COMPLEVEL_DEFAULT)
                        /* KLZ4 has no notion of system-wide default compression
                         * level, use zero in this case */
                        rkt->rkt_conf.compression_level = 0;
                else if (rkt->rkt_conf.compression_level >
                         RD_KAFKA_COMPLEVEL_KLZ4_MAX)
                        rkt->rkt_conf.compression_level =
                            RD_KAFKA_COMPLEVEL_KLZ4_MAX;
                break;
#if WITH_ZSTD
        case RD_KAFKA_COMPRESSION_ZSTD:
                if (rkt->rkt_conf.compression_level ==
                    RD_KAFKA_COMPLEVEL_DEFAULT)
                        rkt->rkt_conf.compression_level = 3;
                else if (rkt->rkt_conf.compression_level >
                         RD_KAFKA_COMPLEVEL_ZSTD_MAX)
                        rkt->rkt_conf.compression_level =
                            RD_KAFKA_COMPLEVEL_ZSTD_MAX;
                break;
#endif
        case RD_KAFKA_COMPRESSION_SNAPPY:
        default:
                /* Compression level has no effect in this case */
                rkt->rkt_conf.compression_level = RD_KAFKA_COMPLEVEL_DEFAULT;
        }

        rd_avg_init(&rkt->rkt_avg_batchsize, RD_AVG_GAUGE, 0,
                    rk->rk_conf.max_msg_size, 2,
                    rk->rk_conf.stats_interval_ms ? 1 : 0);
        rd_avg_init(&rkt->rkt_avg_batchcnt, RD_AVG_GAUGE, 0,
                    rk->rk_conf.batch_num_messages, 2,
                    rk->rk_conf.stats_interval_ms ? 1 : 0);

        rd_kafka_dbg(rk, TOPIC, "TOPIC", "New local topic: %.*s",
                     RD_KAFKAP_STR_PR(rkt->rkt_topic));

        rd_list_init(&rkt->rkt_desp, 16, NULL);
        rd_interval_init(&rkt->rkt_desp_refresh_intvl);
        TAILQ_INIT(&rkt->rkt_saved_partmsgids);
        rd_refcnt_init(&rkt->rkt_refcnt, 0);
        rd_refcnt_init(&rkt->rkt_app_refcnt, 0);

        rd_kafka_topic_keep(rkt);

        rwlock_init(&rkt->rkt_lock);

        /* Create unassigned partition */
        rkt->rkt_ua = rd_kafka_toppar_new(rkt, RD_KAFKA_PARTITION_UA);

        TAILQ_INSERT_TAIL(&rk->rk_topics, rkt, rkt_link);
        rk->rk_topic_cnt++;

        /* Populate from metadata cache. */
        if ((rkmce = rd_kafka_metadata_cache_find(rk, topic, 1 /*valid*/)) &&
            !rkmce->rkmce_mtopic.err) {
                if (existing)
                        *existing = 1;

                rd_kafka_topic_metadata_update(rkt, &rkmce->rkmce_mtopic,
                                               rkmce->rkmce_ts_insert);
        }

        if (do_lock)
                rd_kafka_wrunlock(rk);

        if (rk->rk_conf.debug & RD_KAFKA_DBG_CONF) {
                char desc[256];
                rd_snprintf(desc, sizeof(desc),
                            "Topic \"%s\" configuration (%s)", topic,
                            used_conf_str);
                rd_kafka_anyconf_dump_dbg(rk, _RK_TOPIC, &rkt->rkt_conf, desc);
        }

        return rkt;
}



/**
 * @brief Create new app topic handle.
 *
 * @locality application thread
 */
rd_kafka_topic_t *rd_kafka_topic_new(rd_kafka_t *rk,
                                     const char *topic,
                                     rd_kafka_topic_conf_t *conf) {
        rd_kafka_topic_t *rkt;
        int existing;

        rkt = rd_kafka_topic_new0(rk, topic, conf, &existing, 1 /*lock*/);
        if (!rkt)
                return NULL;

        /* Increase application refcount. */
        rd_kafka_topic_keep_app(rkt);

        /* Query for the topic leader (async) */
        if (!existing)
                rd_kafka_topic_leader_query(rk, rkt);

        /* Drop our reference since there is already/now an app refcnt */
        rd_kafka_topic_destroy0(rkt);

        return rkt;
}



/**
 * Sets the state for topic.
 * NOTE: rd_kafka_topic_wrlock(rkt) MUST be held
 */
static void rd_kafka_topic_set_state(rd_kafka_topic_t *rkt, int state) {

        if ((int)rkt->rkt_state == state)
                return;

        rd_kafka_dbg(rkt->rkt_rk, TOPIC, "STATE",
                     "Topic %s changed state %s -> %s", rkt->rkt_topic->str,
                     rd_kafka_topic_state_names[rkt->rkt_state],
                     rd_kafka_topic_state_names[state]);

        if (rkt->rkt_state == RD_KAFKA_TOPIC_S_ERROR)
                rkt->rkt_err = RD_KAFKA_RESP_ERR_NO_ERROR;

        rkt->rkt_state = state;
}

/**
 * Returns the name of a topic.
 * NOTE:
 *   The topic Kafka String representation is crafted with an extra byte
 *   at the end for the Nul that is not included in the length, this way
 *   we can use the topic's String directly.
 *   This is not true for Kafka Strings read from the network.
 */
const char *rd_kafka_topic_name(const rd_kafka_topic_t *app_rkt) {
        if (rd_kafka_rkt_is_lw(app_rkt))
                return rd_kafka_rkt_lw_const(app_rkt)->lrkt_topic;
        else
                return app_rkt->rkt_topic->str;
}


/**
 * @brief Update the broker that a topic+partition is delegated to.
 *
 * @param broker_id The id of the broker to associate the toppar with.
 * @param rkb A reference to the broker to delegate to (must match
 *        broker_id) or NULL if the toppar should be undelegated for
 *        any reason.
 * @param reason Human-readable reason for the update, included in debug log.
 *
 * @returns 1 if the broker delegation was changed, -1 if the broker
 *          delegation was changed and is now undelegated, else 0.
 *
 * @locks caller must have rd_kafka_toppar_lock(rktp)
 * @locality any
 */
int rd_kafka_toppar_broker_update(rd_kafka_toppar_t *rktp,
                                  int32_t broker_id,
                                  rd_kafka_broker_t *rkb,
                                  const char *reason) {

        rktp->rktp_broker_id = broker_id;

        if (!rkb) {
                int had_broker = rktp->rktp_broker ? 1 : 0;
                rd_kafka_toppar_broker_delegate(rktp, NULL);
                return had_broker ? -1 : 0;
        }

        if (rktp->rktp_broker) {
                if (rktp->rktp_broker == rkb) {
                        /* No change in broker */
                        return 0;
                }

                rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, TOPIC | RD_KAFKA_DBG_FETCH,
                             "TOPICUPD",
                             "Topic %s [%" PRId32
                             "]: migrating from "
                             "broker %" PRId32 " to %" PRId32
                             " (leader is "
                             "%" PRId32 "): %s",
                             rktp->rktp_rkt->rkt_topic->str,
                             rktp->rktp_partition,
                             rktp->rktp_broker->rkb_nodeid, rkb->rkb_nodeid,
                             rktp->rktp_leader_id, reason);
        }

        rd_kafka_toppar_broker_delegate(rktp, rkb);

        return 1;
}


/**
 * @brief Update a topic+partition for a new leader.
 *
 * @remark If a toppar is currently delegated to a preferred replica,
 *         it will not be delegated to the leader broker unless there
 *         has been a leader change.
 *
 * @param leader_id The id of the new leader broker.
 * @param leader A reference to the leader broker or NULL if the
 *        toppar should be undelegated for any reason.
 *
 * @returns 1 if the broker delegation was changed, -1 if the broker
 *        delegation was changed and is now undelegated, else 0.
 *
 * @locks caller must have rd_kafka_topic_wrlock(rkt)
 *        AND NOT rd_kafka_toppar_lock(rktp)
 * @locality any
 */
static int rd_kafka_toppar_leader_update(rd_kafka_topic_t *rkt,
                                         int32_t partition,
                                         int32_t leader_id,
                                         rd_kafka_broker_t *leader) {
        rd_kafka_toppar_t *rktp;
        rd_bool_t fetching_from_follower;
        int r = 0;

        rktp = rd_kafka_toppar_get(rkt, partition, 0);
        if (unlikely(!rktp)) {
                /* Have only seen this in issue #132.
                 * Probably caused by corrupt broker state. */
                rd_kafka_log(rkt->rkt_rk, LOG_WARNING, "BROKER",
                             "%s [%" PRId32
                             "] is unknown "
                             "(partition_cnt %i): "
                             "ignoring leader (%" PRId32 ") update",
                             rkt->rkt_topic->str, partition,
                             rkt->rkt_partition_cnt, leader_id);
                return -1;
        }

        rd_kafka_toppar_lock(rktp);

        fetching_from_follower =
            leader != NULL && rktp->rktp_broker != NULL &&
            rktp->rktp_broker->rkb_source != RD_KAFKA_INTERNAL &&
            rktp->rktp_broker != leader;

        if (fetching_from_follower && rktp->rktp_leader_id == leader_id) {
                rd_kafka_dbg(
                    rktp->rktp_rkt->rkt_rk, TOPIC, "BROKER",
                    "Topic %s [%" PRId32 "]: leader %" PRId32
                    " unchanged, "
                    "not migrating away from preferred replica %" PRId32,
                    rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                    leader_id, rktp->rktp_broker_id);
                r = 0;

        } else {

                if (rktp->rktp_leader_id != leader_id ||
                    rktp->rktp_leader != leader) {
                        /* Update leader if it has changed */
                        rktp->rktp_leader_id = leader_id;
                        if (rktp->rktp_leader)
                                rd_kafka_broker_destroy(rktp->rktp_leader);
                        if (leader)
                                rd_kafka_broker_keep(leader);
                        rktp->rktp_leader = leader;
                }

                /* Update handling broker */
                r = rd_kafka_toppar_broker_update(rktp, leader_id, leader,
                                                  "leader updated");
        }

        rd_kafka_toppar_unlock(rktp);

        rd_kafka_toppar_destroy(rktp); /* from get() */

        return r;
}


/**
 * @brief Revert the topic+partition delegation to the leader from
 *        a preferred replica.
 *
 * @returns 1 if the broker delegation was changed, -1 if the broker
 *          delegation was changed and is now undelegated, else 0.
 *
 * @locks none
 * @locality any
 */
int rd_kafka_toppar_delegate_to_leader(rd_kafka_toppar_t *rktp) {
        rd_kafka_broker_t *leader;
        int r;

        rd_kafka_rdlock(rktp->rktp_rkt->rkt_rk);
        rd_kafka_toppar_lock(rktp);

        rd_assert(rktp->rktp_leader_id != rktp->rktp_broker_id);

        rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, TOPIC, "BROKER",
                     "Topic %s [%" PRId32
                     "]: Reverting from preferred "
                     "replica %" PRId32 " to leader %" PRId32,
                     rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                     rktp->rktp_broker_id, rktp->rktp_leader_id);

        leader = rd_kafka_broker_find_by_nodeid(rktp->rktp_rkt->rkt_rk,
                                                rktp->rktp_leader_id);

        rd_kafka_toppar_unlock(rktp);
        rd_kafka_rdunlock(rktp->rktp_rkt->rkt_rk);

        rd_kafka_toppar_lock(rktp);
        r = rd_kafka_toppar_broker_update(
            rktp, rktp->rktp_leader_id, leader,
            "reverting from preferred replica to leader");
        rd_kafka_toppar_unlock(rktp);

        if (leader)
                rd_kafka_broker_destroy(leader);

        return r;
}



/**
 * @brief Save idempotent producer state for a partition that is about to
 *        be removed.
 *
 * @locks_required rd_kafka_wrlock(rkt), rd_kafka_toppar_lock(rktp)
 */
static void rd_kafka_toppar_idemp_msgid_save(rd_kafka_topic_t *rkt,
                                             const rd_kafka_toppar_t *rktp) {
        rd_kafka_partition_msgid_t *partmsgid = rd_malloc(sizeof(*partmsgid));
        partmsgid->partition                  = rktp->rktp_partition;
        partmsgid->msgid                      = rktp->rktp_msgid;
        partmsgid->pid                        = rktp->rktp_eos.pid;
        partmsgid->epoch_base_msgid           = rktp->rktp_eos.epoch_base_msgid;
        partmsgid->ts                         = rd_clock();

        TAILQ_INSERT_TAIL(&rkt->rkt_saved_partmsgids, partmsgid, link);
}


/**
 * @brief Restore idempotent producer state for a new/resurfacing partition.
 *
 * @locks_required rd_kafka_wrlock(rkt), rd_kafka_toppar_lock(rktp)
 */
static void rd_kafka_toppar_idemp_msgid_restore(rd_kafka_topic_t *rkt,
                                                rd_kafka_toppar_t *rktp) {
        rd_kafka_partition_msgid_t *partmsgid;

        TAILQ_FOREACH(partmsgid, &rkt->rkt_saved_partmsgids, link) {
                if (partmsgid->partition == rktp->rktp_partition)
                        break;
        }

        if (!partmsgid)
                return;

        rktp->rktp_msgid                = partmsgid->msgid;
        rktp->rktp_eos.pid              = partmsgid->pid;
        rktp->rktp_eos.epoch_base_msgid = partmsgid->epoch_base_msgid;

        rd_kafka_dbg(rkt->rkt_rk, EOS | RD_KAFKA_DBG_TOPIC, "MSGID",
                     "Topic %s [%" PRId32 "]: restored %s with MsgId %" PRIu64
                     " and "
                     "epoch base MsgId %" PRIu64
                     " that was saved upon removal %dms ago",
                     rkt->rkt_topic->str, rktp->rktp_partition,
                     rd_kafka_pid2str(partmsgid->pid), partmsgid->msgid,
                     partmsgid->epoch_base_msgid,
                     (int)((rd_clock() - partmsgid->ts) / 1000));

        TAILQ_REMOVE(&rkt->rkt_saved_partmsgids, partmsgid, link);
        rd_free(partmsgid);
}


/**
 * @brief Update the number of partitions for a topic and takes actions
 *        accordingly.
 *
 * @returns 1 if the partition count changed, else 0.
 *
 * @locks rd_kafka_topic_wrlock(rkt) MUST be held.
 */
static int rd_kafka_topic_partition_cnt_update(rd_kafka_topic_t *rkt,
                                               int32_t partition_cnt) {
        rd_kafka_t *rk = rkt->rkt_rk;
        rd_kafka_toppar_t **rktps;
        rd_kafka_toppar_t *rktp;
        rd_bool_t is_idempodent = rd_kafka_is_idempotent(rk);
        int32_t i;

        if (likely(rkt->rkt_partition_cnt == partition_cnt))
                return 0; /* No change in partition count */

        if (unlikely(rkt->rkt_partition_cnt != 0 &&
                     !rd_kafka_terminating(rkt->rkt_rk)))
                rd_kafka_log(rk, LOG_NOTICE, "PARTCNT",
                             "Topic %s partition count changed "
                             "from %" PRId32 " to %" PRId32,
                             rkt->rkt_topic->str, rkt->rkt_partition_cnt,
                             partition_cnt);
        else
                rd_kafka_dbg(rk, TOPIC, "PARTCNT",
                             "Topic %s partition count changed "
                             "from %" PRId32 " to %" PRId32,
                             rkt->rkt_topic->str, rkt->rkt_partition_cnt,
                             partition_cnt);


        /* Create and assign new partition list */
        if (partition_cnt > 0)
                rktps = rd_calloc(partition_cnt, sizeof(*rktps));
        else
                rktps = NULL;

        for (i = 0; i < partition_cnt; i++) {
                if (i >= rkt->rkt_partition_cnt) {
                        /* New partition. Check if its in the list of
                         * desired partitions first. */

                        rktp = rd_kafka_toppar_desired_get(rkt, i);
                        if (rktp) {
                                rd_kafka_toppar_lock(rktp);
                                rktp->rktp_flags &=
                                    ~(RD_KAFKA_TOPPAR_F_UNKNOWN |
                                      RD_KAFKA_TOPPAR_F_REMOVE);

                                /* Remove from desp list since the
                                 * partition is now known. */
                                rd_kafka_toppar_desired_unlink(rktp);
                        } else {
                                rktp = rd_kafka_toppar_new(rkt, i);

                                rd_kafka_toppar_lock(rktp);
                                rktp->rktp_flags &=
                                    ~(RD_KAFKA_TOPPAR_F_UNKNOWN |
                                      RD_KAFKA_TOPPAR_F_REMOVE);
                        }
                        rktps[i] = rktp;

                        if (is_idempodent)
                                /* Restore idempotent producer state for
                                 * this partition, if any. */
                                rd_kafka_toppar_idemp_msgid_restore(rkt, rktp);

                        rd_kafka_toppar_unlock(rktp);

                } else {
                        /* Existing partition, grab our own reference. */
                        rktps[i] = rd_kafka_toppar_keep(rkt->rkt_p[i]);
                        /* Loose previous ref */
                        rd_kafka_toppar_destroy(rkt->rkt_p[i]);
                }
        }

        /* Propagate notexist errors for desired partitions */
        RD_LIST_FOREACH(rktp, &rkt->rkt_desp, i) {
                rd_kafka_dbg(rkt->rkt_rk, TOPIC, "DESIRED",
                             "%s [%" PRId32
                             "]: "
                             "desired partition does not exist in cluster",
                             rkt->rkt_topic->str, rktp->rktp_partition);
                rd_kafka_toppar_enq_error(
                    rktp,
                    rkt->rkt_err ? rkt->rkt_err
                                 : RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION,
                    "desired partition is not available");
        }

        /* Remove excessive partitions */
        for (i = partition_cnt; i < rkt->rkt_partition_cnt; i++) {
                rktp = rkt->rkt_p[i];

                rd_kafka_dbg(rkt->rkt_rk, TOPIC, "REMOVE",
                             "%s [%" PRId32 "] no longer reported in metadata",
                             rkt->rkt_topic->str, rktp->rktp_partition);

                rd_kafka_toppar_lock(rktp);

                /* Idempotent/Transactional producer:
                 * We need to save each removed partition's base msgid for
                 * the (rare) chance the partition comes back,
                 * in which case we must continue with the correct msgid
                 * in future ProduceRequests.
                 *
                 * These base msgsid are restored (above) if/when partitions
                 * come back and the PID,Epoch hasn't changed.
                 *
                 * One situation where this might happen is if a broker goes
                 * out of sync and starts to wrongfully report an existing
                 * topic as non-existent, triggering the removal of partitions
                 * on the producer client. When metadata is eventually correct
                 * again and the topic is "re-created" on the producer, it
                 * must continue with the next msgid/baseseq. */
                if (is_idempodent && rd_kafka_pid_valid(rktp->rktp_eos.pid))
                        rd_kafka_toppar_idemp_msgid_save(rkt, rktp);

                rktp->rktp_flags |= RD_KAFKA_TOPPAR_F_UNKNOWN;

                if (rktp->rktp_flags & RD_KAFKA_TOPPAR_F_DESIRED) {
                        rd_kafka_dbg(rkt->rkt_rk, TOPIC, "DESIRED",
                                     "Topic %s [%" PRId32
                                     "] is desired "
                                     "but no longer known: "
                                     "moving back on desired list",
                                     rkt->rkt_topic->str, rktp->rktp_partition);

                        /* If this is a desired partition move it back on to
                         * the desired list since partition is no longer known*/
                        rd_kafka_toppar_desired_link(rktp);

                        if (!rd_kafka_terminating(rkt->rkt_rk))
                                rd_kafka_toppar_enq_error(
                                    rktp,
                                    rkt->rkt_err
                                        ? rkt->rkt_err
                                        : RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION,
                                    "desired partition is no longer "
                                    "available");

                        rd_kafka_toppar_broker_delegate(rktp, NULL);

                } else {
                        /* Tell handling broker to let go of the toppar */
                        rd_kafka_toppar_broker_leave_for_remove(rktp);
                }

                rd_kafka_toppar_unlock(rktp);

                rd_kafka_toppar_destroy(rktp);
        }

        if (rkt->rkt_p)
                rd_free(rkt->rkt_p);

        rkt->rkt_p = rktps;

        rkt->rkt_partition_cnt = partition_cnt;

        return 1;
}



/**
 * Topic 'rkt' does not exist: propagate to interested parties.
 * The topic's state must have been set to NOTEXISTS and
 * rd_kafka_topic_partition_cnt_update() must have been called prior to
 * calling this function.
 *
 * Locks: rd_kafka_topic_*lock() must be held.
 */
static void rd_kafka_topic_propagate_notexists(rd_kafka_topic_t *rkt,
                                               rd_kafka_resp_err_t err) {
        rd_kafka_toppar_t *rktp;
        int i;

        if (rkt->rkt_rk->rk_type != RD_KAFKA_CONSUMER)
                return;


        /* Notify consumers that the topic doesn't exist. */
        RD_LIST_FOREACH(rktp, &rkt->rkt_desp, i)
        rd_kafka_toppar_enq_error(rktp, err, "topic does not exist");
}


/**
 * Assign messages on the UA partition to available partitions.
 * Locks: rd_kafka_topic_*lock() must be held.
 */
static void rd_kafka_topic_assign_uas(rd_kafka_topic_t *rkt,
                                      rd_kafka_resp_err_t err) {
        rd_kafka_t *rk = rkt->rkt_rk;
        rd_kafka_toppar_t *rktp_ua;
        rd_kafka_msg_t *rkm, *tmp;
        rd_kafka_msgq_t uas         = RD_KAFKA_MSGQ_INITIALIZER(uas);
        rd_kafka_msgq_t failed      = RD_KAFKA_MSGQ_INITIALIZER(failed);
        rd_kafka_resp_err_t err_all = RD_KAFKA_RESP_ERR_NO_ERROR;
        int cnt;

        if (rkt->rkt_rk->rk_type != RD_KAFKA_PRODUCER)
                return;

        rktp_ua = rd_kafka_toppar_get(rkt, RD_KAFKA_PARTITION_UA, 0);
        if (unlikely(!rktp_ua)) {
                rd_kafka_dbg(rk, TOPIC, "ASSIGNUA",
                             "No UnAssigned partition available for %s",
                             rkt->rkt_topic->str);
                return;
        }

        /* Assign all unassigned messages to new topics. */
        rd_kafka_toppar_lock(rktp_ua);

        if (rkt->rkt_state == RD_KAFKA_TOPIC_S_ERROR) {
                err_all = rkt->rkt_err;
                rd_kafka_dbg(rk, TOPIC, "PARTCNT",
                             "Failing all %i unassigned messages in "
                             "topic %.*s due to permanent topic error: %s",
                             rktp_ua->rktp_msgq.rkmq_msg_cnt,
                             RD_KAFKAP_STR_PR(rkt->rkt_topic),
                             rd_kafka_err2str(err_all));
        } else if (rkt->rkt_state == RD_KAFKA_TOPIC_S_NOTEXISTS) {
                err_all = err;
                rd_kafka_dbg(rk, TOPIC, "PARTCNT",
                             "Failing all %i unassigned messages in "
                             "topic %.*s since topic does not exist: %s",
                             rktp_ua->rktp_msgq.rkmq_msg_cnt,
                             RD_KAFKAP_STR_PR(rkt->rkt_topic),
                             rd_kafka_err2str(err_all));
        } else {
                rd_kafka_dbg(rk, TOPIC, "PARTCNT",
                             "Partitioning %i unassigned messages in "
                             "topic %.*s to %" PRId32 " partitions",
                             rktp_ua->rktp_msgq.rkmq_msg_cnt,
                             RD_KAFKAP_STR_PR(rkt->rkt_topic),
                             rkt->rkt_partition_cnt);
        }

        rd_kafka_msgq_move(&uas, &rktp_ua->rktp_msgq);
        cnt = uas.rkmq_msg_cnt;
        rd_kafka_toppar_unlock(rktp_ua);

        TAILQ_FOREACH_SAFE(rkm, &uas.rkmq_msgs, rkm_link, tmp) {
                /* Fast-path for failing messages with forced partition or
                 * when all messages are to fail. */
                if (err_all || (rkm->rkm_partition != RD_KAFKA_PARTITION_UA &&
                                rkm->rkm_partition >= rkt->rkt_partition_cnt &&
                                rkt->rkt_state != RD_KAFKA_TOPIC_S_UNKNOWN)) {
                        rd_kafka_msgq_enq(&failed, rkm);
                        continue;
                }

                if (unlikely(rd_kafka_msg_partitioner(rkt, rkm, 0) != 0)) {
                        /* Desired partition not available */
                        rd_kafka_msgq_enq(&failed, rkm);
                }
        }

        rd_kafka_dbg(rk, TOPIC, "UAS",
                     "%i/%i messages were partitioned in topic %s",
                     cnt - failed.rkmq_msg_cnt, cnt, rkt->rkt_topic->str);

        if (failed.rkmq_msg_cnt > 0) {
                /* Fail the messages */
                rd_kafka_dbg(rk, TOPIC, "UAS",
                             "%" PRId32
                             "/%i messages failed partitioning "
                             "in topic %s",
                             failed.rkmq_msg_cnt, cnt, rkt->rkt_topic->str);
                rd_kafka_dr_msgq(
                    rkt, &failed,
                    err_all ? err_all : RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION);
        }

        rd_kafka_toppar_destroy(rktp_ua); /* from get() */
}


/**
 * @brief Mark topic as non-existent, unless metadata propagation configuration
 *        disallows it.
 *
 * @param err Propagate non-existent topic using this error code.
 *            If \p err is RD_KAFKA_RESP_ERR_TOPIC_EXCEPTION it means the
 *            topic is invalid and no propagation delay will be used.
 *
 * @returns true if the topic was marked as non-existent, else false.
 *
 * @locks topic_wrlock() MUST be held.
 */
rd_bool_t rd_kafka_topic_set_notexists(rd_kafka_topic_t *rkt,
                                       rd_kafka_resp_err_t err) {
        rd_ts_t remains_us;
        rd_bool_t permanent = err == RD_KAFKA_RESP_ERR_TOPIC_EXCEPTION;

        if (unlikely(rd_kafka_terminating(rkt->rkt_rk))) {
                /* Dont update metadata while terminating. */
                return rd_false;
        }

        rd_assert(err != RD_KAFKA_RESP_ERR_NO_ERROR);

        remains_us =
            (rkt->rkt_ts_create +
             (rkt->rkt_rk->rk_conf.metadata_propagation_max_ms * 1000)) -
            rkt->rkt_ts_metadata;

        if (!permanent && rkt->rkt_state == RD_KAFKA_TOPIC_S_UNKNOWN &&
            remains_us > 0) {
                /* Still allowing topic metadata to propagate. */
                rd_kafka_dbg(
                    rkt->rkt_rk, TOPIC | RD_KAFKA_DBG_METADATA, "TOPICPROP",
                    "Topic %.*s does not exist, allowing %dms "
                    "for metadata propagation before marking topic "
                    "as non-existent",
                    RD_KAFKAP_STR_PR(rkt->rkt_topic), (int)(remains_us / 1000));
                return rd_false;
        }

        rd_kafka_topic_set_state(rkt, RD_KAFKA_TOPIC_S_NOTEXISTS);

        rkt->rkt_flags &= ~RD_KAFKA_TOPIC_F_LEADER_UNAVAIL;

        /* Update number of partitions */
        rd_kafka_topic_partition_cnt_update(rkt, 0);

        /* Purge messages with forced partition */
        rd_kafka_topic_assign_uas(rkt, err);

        /* Propagate nonexistent topic info */
        rd_kafka_topic_propagate_notexists(rkt, err);

        return rd_true;
}

/**
 * @brief Mark topic as errored, such as when topic authorization fails.
 *
 * @param err Propagate error using this error code.
 *
 * @returns true if the topic was marked as errored, else false.
 *
 * @locality any
 * @locks topic_wrlock() MUST be held.
 */
rd_bool_t rd_kafka_topic_set_error(rd_kafka_topic_t *rkt,
                                   rd_kafka_resp_err_t err) {

        if (unlikely(rd_kafka_terminating(rkt->rkt_rk))) {
                /* Dont update metadata while terminating. */
                return rd_false;
        }

        rd_assert(err != RD_KAFKA_RESP_ERR_NO_ERROR);

        /* Same error, ignore. */
        if (rkt->rkt_state == RD_KAFKA_TOPIC_S_ERROR && rkt->rkt_err == err)
                return rd_true;

        rd_kafka_dbg(rkt->rkt_rk, TOPIC, "TOPICERROR",
                     "Topic %s has permanent error: %s", rkt->rkt_topic->str,
                     rd_kafka_err2str(err));

        rd_kafka_topic_set_state(rkt, RD_KAFKA_TOPIC_S_ERROR);

        rkt->rkt_err = err;

        /* Update number of partitions */
        rd_kafka_topic_partition_cnt_update(rkt, 0);

        /* Purge messages with forced partition */
        rd_kafka_topic_assign_uas(rkt, err);

        return rd_true;
}



/**
 * @brief Update a topic from metadata.
 *
 * @param ts_age absolute age (timestamp) of metadata.
 * @returns 1 if the number of partitions changed, 0 if not, and -1 if the
 *          topic is unknown.

 *
 * @locks rd_kafka_*lock() MUST be held.
 */
static int
rd_kafka_topic_metadata_update(rd_kafka_topic_t *rkt,
                               const struct rd_kafka_metadata_topic *mdt,
                               rd_ts_t ts_age) {
        rd_kafka_t *rk = rkt->rkt_rk;
        int upd        = 0;
        int j;
        rd_kafka_broker_t **partbrokers;
        int leader_cnt = 0;
        int old_state;

        if (mdt->err != RD_KAFKA_RESP_ERR_NO_ERROR)
                rd_kafka_dbg(rk, TOPIC | RD_KAFKA_DBG_METADATA, "METADATA",
                             "Error in metadata reply for "
                             "topic %s (PartCnt %i): %s",
                             rkt->rkt_topic->str, mdt->partition_cnt,
                             rd_kafka_err2str(mdt->err));

        if (unlikely(rd_kafka_terminating(rk))) {
                /* Dont update metadata while terminating, do this
                 * after acquiring lock for proper synchronisation */
                return -1;
        }

        /* Look up brokers before acquiring rkt lock to preserve lock order */
        partbrokers = rd_malloc(mdt->partition_cnt * sizeof(*partbrokers));

        for (j = 0; j < mdt->partition_cnt; j++) {
                if (mdt->partitions[j].leader == -1) {
                        partbrokers[j] = NULL;
                        continue;
                }

                partbrokers[j] = rd_kafka_broker_find_by_nodeid(
                    rk, mdt->partitions[j].leader);
        }


        rd_kafka_topic_wrlock(rkt);

        old_state            = rkt->rkt_state;
        rkt->rkt_ts_metadata = ts_age;

        /* Set topic state.
         * UNKNOWN_TOPIC_OR_PART may indicate that auto.create.topics failed */
        if (mdt->err == RD_KAFKA_RESP_ERR_TOPIC_EXCEPTION /*invalid topic*/ ||
            mdt->err == RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART)
                rd_kafka_topic_set_notexists(rkt, mdt->err);
        else if (mdt->partition_cnt > 0)
                rd_kafka_topic_set_state(rkt, RD_KAFKA_TOPIC_S_EXISTS);
        else if (mdt->err)
                rd_kafka_topic_set_error(rkt, mdt->err);

        /* Update number of partitions, but not if there are
         * (possibly intermittent) errors (e.g., "Leader not available"). */
        if (mdt->err == RD_KAFKA_RESP_ERR_NO_ERROR) {
                upd += rd_kafka_topic_partition_cnt_update(rkt,
                                                           mdt->partition_cnt);

                /* If the metadata times out for a topic (because all brokers
                 * are down) the state will transition to S_UNKNOWN.
                 * When updated metadata is eventually received there might
                 * not be any change to partition count or leader,
                 * but there may still be messages in the UA partition that
                 * needs to be assigned, so trigger an update for this case too.
                 * Issue #1985. */
                if (old_state == RD_KAFKA_TOPIC_S_UNKNOWN)
                        upd++;
        }

        /* Update leader for each partition */
        for (j = 0; j < mdt->partition_cnt; j++) {
                int r;
                rd_kafka_broker_t *leader;

                rd_kafka_dbg(rk, TOPIC | RD_KAFKA_DBG_METADATA, "METADATA",
                             "  Topic %s partition %i Leader %" PRId32,
                             rkt->rkt_topic->str, mdt->partitions[j].id,
                             mdt->partitions[j].leader);

                leader         = partbrokers[j];
                partbrokers[j] = NULL;

                /* Update leader for partition */
                r = rd_kafka_toppar_leader_update(rkt, mdt->partitions[j].id,
                                                  mdt->partitions[j].leader,
                                                  leader);

                upd += (r != 0 ? 1 : 0);

                if (leader) {
                        if (r != -1)
                                leader_cnt++;
                        /* Drop reference to broker (from find()) */
                        rd_kafka_broker_destroy(leader);
                }
        }

        /* If all partitions have leaders we can turn off fast leader query. */
        if (mdt->partition_cnt > 0 && leader_cnt == mdt->partition_cnt)
                rkt->rkt_flags &= ~RD_KAFKA_TOPIC_F_LEADER_UNAVAIL;

        if (mdt->err != RD_KAFKA_RESP_ERR_NO_ERROR && rkt->rkt_partition_cnt) {
                /* (Possibly intermittent) topic-wide error:
                 * remove leaders for partitions */

                for (j = 0; j < rkt->rkt_partition_cnt; j++) {
                        rd_kafka_toppar_t *rktp;
                        if (!rkt->rkt_p[j])
                                continue;

                        rktp = rkt->rkt_p[j];
                        rd_kafka_toppar_lock(rktp);
                        rd_kafka_toppar_broker_delegate(rktp, NULL);
                        rd_kafka_toppar_unlock(rktp);
                }
        }

        /* If there was an update to the partitions try to assign
         * unassigned messages to new partitions, or fail them */
        if (upd > 0)
                rd_kafka_topic_assign_uas(
                    rkt,
                    mdt->err ? mdt->err : RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC);

        rd_kafka_topic_wrunlock(rkt);

        /* Loose broker references */
        for (j = 0; j < mdt->partition_cnt; j++)
                if (partbrokers[j])
                        rd_kafka_broker_destroy(partbrokers[j]);

        rd_free(partbrokers);

        return upd;
}

/**
 * @brief Update topic by metadata, if topic is locally known.
 * @sa rd_kafka_topic_metadata_update()
 * @locks none
 */
int rd_kafka_topic_metadata_update2(rd_kafka_broker_t *rkb,
                                    const struct rd_kafka_metadata_topic *mdt) {
        rd_kafka_topic_t *rkt;
        int r;

        rd_kafka_wrlock(rkb->rkb_rk);
        if (!(rkt =
                  rd_kafka_topic_find(rkb->rkb_rk, mdt->topic, 0 /*!lock*/))) {
                rd_kafka_wrunlock(rkb->rkb_rk);
                return -1; /* Ignore topics that we dont have locally. */
        }

        r = rd_kafka_topic_metadata_update(rkt, mdt, rd_clock());

        rd_kafka_wrunlock(rkb->rkb_rk);

        rd_kafka_topic_destroy0(rkt); /* from find() */

        return r;
}



/**
 * @returns a list of all partitions (rktp's) for a topic.
 * @remark rd_kafka_topic_*lock() MUST be held.
 */
static rd_list_t *rd_kafka_topic_get_all_partitions(rd_kafka_topic_t *rkt) {
        rd_list_t *list;
        rd_kafka_toppar_t *rktp;
        int i;

        list = rd_list_new(rkt->rkt_partition_cnt +
                               rd_list_cnt(&rkt->rkt_desp) + 1 /*ua*/,
                           NULL);

        for (i = 0; i < rkt->rkt_partition_cnt; i++)
                rd_list_add(list, rd_kafka_toppar_keep(rkt->rkt_p[i]));

        RD_LIST_FOREACH(rktp, &rkt->rkt_desp, i)
        rd_list_add(list, rd_kafka_toppar_keep(rktp));

        if (rkt->rkt_ua)
                rd_list_add(list, rd_kafka_toppar_keep(rkt->rkt_ua));

        return list;
}



/**
 * Remove all partitions from a topic, including the ua.
 * Must only be called during rd_kafka_t termination.
 *
 * Locality: main thread
 */
void rd_kafka_topic_partitions_remove(rd_kafka_topic_t *rkt) {
        rd_kafka_toppar_t *rktp;
        rd_list_t *partitions;
        int i;

        /* Purge messages for all partitions outside the topic_wrlock since
         * a message can hold a reference to the topic_t and thus
         * would trigger a recursive lock dead-lock. */
        rd_kafka_topic_rdlock(rkt);
        partitions = rd_kafka_topic_get_all_partitions(rkt);
        rd_kafka_topic_rdunlock(rkt);

        RD_LIST_FOREACH(rktp, partitions, i) {
                rd_kafka_toppar_lock(rktp);
                rd_kafka_msgq_purge(rkt->rkt_rk, &rktp->rktp_msgq);
                rd_kafka_toppar_purge_and_disable_queues(rktp);
                rd_kafka_toppar_unlock(rktp);

                rd_kafka_toppar_destroy(rktp);
        }
        rd_list_destroy(partitions);

        rd_kafka_topic_keep(rkt);
        rd_kafka_topic_wrlock(rkt);

        /* Setting the partition count to 0 moves all partitions to
         * the desired list (rktp_desp). */
        rd_kafka_topic_partition_cnt_update(rkt, 0);

        /* Now clean out the desired partitions list.
         * Use reverse traversal to avoid excessive memory shuffling
         * in rd_list_remove() */
        RD_LIST_FOREACH_REVERSE(rktp, &rkt->rkt_desp, i) {
                /* Keep a reference while deleting from desired list */
                rd_kafka_toppar_keep(rktp);

                rd_kafka_toppar_lock(rktp);
                rd_kafka_toppar_desired_del(rktp);
                rd_kafka_toppar_unlock(rktp);

                rd_kafka_toppar_destroy(rktp);
        }

        rd_kafka_assert(rkt->rkt_rk, rkt->rkt_partition_cnt == 0);

        if (rkt->rkt_p)
                rd_free(rkt->rkt_p);

        rkt->rkt_p             = NULL;
        rkt->rkt_partition_cnt = 0;

        if ((rktp = rkt->rkt_ua)) {
                rkt->rkt_ua = NULL;
                rd_kafka_toppar_destroy(rktp);
        }

        rd_kafka_topic_wrunlock(rkt);

        rd_kafka_topic_destroy0(rkt);
}



/**
 * @returns the broker state (as a human readable string) if a query
 *          for the partition leader is necessary, else NULL.
 * @locality any
 * @locks rd_kafka_toppar_lock MUST be held
 */
static const char *rd_kafka_toppar_needs_query(rd_kafka_t *rk,
                                               rd_kafka_toppar_t *rktp) {
        int broker_state;

        if (!rktp->rktp_broker)
                return "not delegated";

        if (rktp->rktp_broker->rkb_source == RD_KAFKA_INTERNAL)
                return "internal";

        broker_state = rd_kafka_broker_get_state(rktp->rktp_broker);

        if (broker_state >= RD_KAFKA_BROKER_STATE_UP)
                return NULL;

        if (!rk->rk_conf.sparse_connections)
                return "down";

        /* Partition assigned to broker but broker does not
         * need a persistent connection, this typically means
         * the partition is not being fetched or not being produced to,
         * so there is no need to re-query the leader. */
        if (broker_state == RD_KAFKA_BROKER_STATE_INIT)
                return NULL;

        /* This is most likely a persistent broker,
         * which means the partition leader should probably
         * be re-queried to see if it needs changing. */
        return "down";
}



/**
 * @brief Scan all topics and partitions for:
 *  - timed out messages in UA partitions.
 *  - topics that needs to be created on the broker.
 *  - topics who's metadata is too old.
 *  - partitions with unknown leaders that require leader query.
 *
 * @locality rdkafka main thread
 */
void rd_kafka_topic_scan_all(rd_kafka_t *rk, rd_ts_t now) {
        rd_kafka_topic_t *rkt;
        rd_kafka_toppar_t *rktp;
        rd_list_t query_topics;

        rd_list_init(&query_topics, 0, rd_free);

        rd_kafka_rdlock(rk);
        TAILQ_FOREACH(rkt, &rk->rk_topics, rkt_link) {
                int p;
                int query_this           = 0;
                rd_kafka_msgq_t timedout = RD_KAFKA_MSGQ_INITIALIZER(timedout);

                rd_kafka_topic_wrlock(rkt);

                /* Check if metadata information has timed out. */
                if (rkt->rkt_state != RD_KAFKA_TOPIC_S_UNKNOWN &&
                    !rd_kafka_metadata_cache_topic_get(rk, rkt->rkt_topic->str,
                                                       1 /*only valid*/)) {
                        rd_kafka_dbg(rk, TOPIC, "NOINFO",
                                     "Topic %s metadata information timed out "
                                     "(%" PRId64 "ms old)",
                                     rkt->rkt_topic->str,
                                     (rd_clock() - rkt->rkt_ts_metadata) /
                                         1000);
                        rd_kafka_topic_set_state(rkt, RD_KAFKA_TOPIC_S_UNKNOWN);

                        query_this = 1;
                } else if (rkt->rkt_state == RD_KAFKA_TOPIC_S_UNKNOWN) {
                        rd_kafka_dbg(rk, TOPIC, "NOINFO",
                                     "Topic %s metadata information unknown",
                                     rkt->rkt_topic->str);
                        query_this = 1;
                }

                /* Just need a read-lock from here on. */
                rd_kafka_topic_wrunlock(rkt);
                rd_kafka_topic_rdlock(rkt);

                if (rkt->rkt_partition_cnt == 0) {
                        /* If this topic is unknown by brokers try
                         * to create it by sending a topic-specific
                         * metadata request.
                         * This requires "auto.create.topics.enable=true"
                         * on the brokers. */
                        rd_kafka_dbg(rk, TOPIC, "NOINFO",
                                     "Topic %s partition count is zero: "
                                     "should refresh metadata",
                                     rkt->rkt_topic->str);

                        query_this = 1;

                } else if (!rd_list_empty(&rkt->rkt_desp) &&
                           rd_interval_immediate(&rkt->rkt_desp_refresh_intvl,
                                                 10 * 1000 * 1000, 0) > 0) {
                        /* Query topic metadata if there are
                         * desired (non-existent) partitions.
                         * At most every 10 seconds. */
                        rd_kafka_dbg(rk, TOPIC, "DESIRED",
                                     "Topic %s has %d desired partition(s): "
                                     "should refresh metadata",
                                     rkt->rkt_topic->str,
                                     rd_list_cnt(&rkt->rkt_desp));

                        query_this = 1;
                }

                for (p = RD_KAFKA_PARTITION_UA; p < rkt->rkt_partition_cnt;
                     p++) {

                        if (!(rktp = rd_kafka_toppar_get(
                                  rkt, p,
                                  p == RD_KAFKA_PARTITION_UA ? rd_true
                                                             : rd_false)))
                                continue;

                        rd_kafka_toppar_lock(rktp);

                        /* Check that partition is delegated to a broker that
                         * is up, else add topic to query list. */
                        if (p != RD_KAFKA_PARTITION_UA) {
                                const char *leader_reason =
                                    rd_kafka_toppar_needs_query(rk, rktp);

                                if (leader_reason) {
                                        rd_kafka_dbg(rk, TOPIC, "QRYLEADER",
                                                     "Topic %s [%" PRId32
                                                     "]: "
                                                     "broker is %s: re-query",
                                                     rkt->rkt_topic->str,
                                                     rktp->rktp_partition,
                                                     leader_reason);
                                        query_this = 1;
                                }
                        } else {
                                if (rk->rk_type == RD_KAFKA_PRODUCER) {
                                        /* Scan UA partition for message
                                         * timeouts.
                                         * Proper partitions are scanned by
                                         * their toppar broker thread. */
                                        rd_kafka_msgq_age_scan(
                                            rktp, &rktp->rktp_msgq, &timedout,
                                            now, NULL);
                                }
                        }

                        rd_kafka_toppar_unlock(rktp);
                        rd_kafka_toppar_destroy(rktp);
                }

                rd_kafka_topic_rdunlock(rkt);

                /* Propagate delivery reports for timed out messages */
                if (rd_kafka_msgq_len(&timedout) > 0) {
                        rd_kafka_dbg(
                            rk, MSG, "TIMEOUT", "%s: %d message(s) timed out",
                            rkt->rkt_topic->str, rd_kafka_msgq_len(&timedout));
                        rd_kafka_dr_msgq(rkt, &timedout,
                                         RD_KAFKA_RESP_ERR__MSG_TIMED_OUT);
                }

                /* Need to re-query this topic's leader. */
                if (query_this &&
                    !rd_list_find(&query_topics, rkt->rkt_topic->str,
                                  (void *)strcmp))
                        rd_list_add(&query_topics,
                                    rd_strdup(rkt->rkt_topic->str));
        }
        rd_kafka_rdunlock(rk);

        if (!rd_list_empty(&query_topics))
                rd_kafka_metadata_refresh_topics(
                    rk, NULL, &query_topics, rd_true /*force even if cached
                                                      * info exists*/
                    ,
                    rk->rk_conf.allow_auto_create_topics,
                    rd_false /*!cgrp_update*/, "refresh unavailable topics");
        rd_list_destroy(&query_topics);
}


/**
 * Locks: rd_kafka_topic_*lock() must be held.
 */
int rd_kafka_topic_partition_available(const rd_kafka_topic_t *app_rkt,
                                       int32_t partition) {
        int avail;
        rd_kafka_toppar_t *rktp;
        rd_kafka_broker_t *rkb;

        /* This API must only be called from a partitioner and the
         * partitioner is always passed a proper topic */
        rd_assert(!rd_kafka_rkt_is_lw(app_rkt));

        rktp = rd_kafka_toppar_get(app_rkt, partition, 0 /*no ua-on-miss*/);
        if (unlikely(!rktp))
                return 0;

        rkb   = rd_kafka_toppar_broker(rktp, 1 /*proper broker*/);
        avail = rkb ? 1 : 0;
        if (rkb)
                rd_kafka_broker_destroy(rkb);
        rd_kafka_toppar_destroy(rktp);
        return avail;
}


void *rd_kafka_topic_opaque(const rd_kafka_topic_t *app_rkt) {
        const rd_kafka_lwtopic_t *lrkt;

        lrkt = rd_kafka_rkt_get_lw((rd_kafka_topic_t *)app_rkt);
        if (unlikely(lrkt != NULL)) {
                void *opaque;
                rd_kafka_topic_t *rkt;

                if (!(rkt = rd_kafka_topic_find(lrkt->lrkt_rk, lrkt->lrkt_topic,
                                                1 /*lock*/)))
                        return NULL;

                opaque = rkt->rkt_conf.opaque;

                rd_kafka_topic_destroy0(rkt); /* loose refcnt from find() */

                return opaque;
        }

        return app_rkt->rkt_conf.opaque;
}


int rd_kafka_topic_info_cmp(const void *_a, const void *_b) {
        const rd_kafka_topic_info_t *a = _a, *b = _b;
        int r;

        if ((r = strcmp(a->topic, b->topic)))
                return r;

        return RD_CMP(a->partition_cnt, b->partition_cnt);
}


/**
 * @brief string compare two topics.
 *
 * @param _a topic string (type char *)
 * @param _b rd_kafka_topic_info_t * pointer.
 */
int rd_kafka_topic_info_topic_cmp(const void *_a, const void *_b) {
        const char *a                  = _a;
        const rd_kafka_topic_info_t *b = _b;
        return strcmp(a, b->topic);
}


/**
 * Allocate new topic_info.
 * \p topic is copied.
 */
rd_kafka_topic_info_t *rd_kafka_topic_info_new(const char *topic,
                                               int partition_cnt) {
        rd_kafka_topic_info_t *ti;
        size_t tlen = strlen(topic) + 1;

        /* Allocate space for the topic along with the struct */
        ti        = rd_malloc(sizeof(*ti) + tlen);
        ti->topic = (char *)(ti + 1);
        memcpy((char *)ti->topic, topic, tlen);
        ti->partition_cnt = partition_cnt;

        return ti;
}

/**
 * Destroy/free topic_info
 */
void rd_kafka_topic_info_destroy(rd_kafka_topic_info_t *ti) {
        rd_free(ti);
}


/**
 * @brief Match \p topic to \p pattern.
 *
 * If pattern begins with "^" it is considered a regexp,
 * otherwise a simple string comparison is performed.
 *
 * @returns 1 on match, else 0.
 */
int rd_kafka_topic_match(rd_kafka_t *rk,
                         const char *pattern,
                         const char *topic) {
        char errstr[128];

        if (*pattern == '^') {
                int r = rd_regex_match(pattern, topic, errstr, sizeof(errstr));
                if (unlikely(r == -1))
                        rd_kafka_dbg(rk, TOPIC, "TOPICREGEX",
                                     "Topic \"%s\" regex \"%s\" "
                                     "matching failed: %s",
                                     topic, pattern, errstr);
                return r == 1;
        } else
                return !strcmp(pattern, topic);
}



/**
 * @brief Trigger broker metadata query for topic leader.
 *
 * @locks none
 */
void rd_kafka_topic_leader_query0(rd_kafka_t *rk,
                                  rd_kafka_topic_t *rkt,
                                  int do_rk_lock) {
        rd_list_t topics;

        rd_list_init(&topics, 1, rd_free);
        rd_list_add(&topics, rd_strdup(rkt->rkt_topic->str));

        rd_kafka_metadata_refresh_topics(
            rk, NULL, &topics, rd_false /*dont force*/,
            rk->rk_conf.allow_auto_create_topics, rd_false /*!cgrp_update*/,
            "leader query");

        rd_list_destroy(&topics);
}



/**
 * @brief Populate list \p topics with the topic names (strdupped char *) of
 *        all locally known or cached topics.
 *
 * @param cache_cntp is an optional pointer to an int that will be set to the
 *                   number of entries added to \p topics from the
 *                   metadata cache.
 * @remark \p rk lock MUST NOT be held
 */
void rd_kafka_local_topics_to_list(rd_kafka_t *rk,
                                   rd_list_t *topics,
                                   int *cache_cntp) {
        rd_kafka_topic_t *rkt;
        int cache_cnt;

        rd_kafka_rdlock(rk);
        rd_list_grow(topics, rk->rk_topic_cnt);
        TAILQ_FOREACH(rkt, &rk->rk_topics, rkt_link)
        rd_list_add(topics, rd_strdup(rkt->rkt_topic->str));
        cache_cnt = rd_kafka_metadata_cache_topics_to_list(rk, topics);
        if (cache_cntp)
                *cache_cntp = cache_cnt;
        rd_kafka_rdunlock(rk);
}


/**
 * @brief Unit test helper to set a topic's state to EXISTS
 *        with the given number of partitions.
 */
void rd_ut_kafka_topic_set_topic_exists(rd_kafka_topic_t *rkt,
                                        int partition_cnt,
                                        int32_t leader_id) {
        struct rd_kafka_metadata_topic mdt = {.topic =
                                                  (char *)rkt->rkt_topic->str,
                                              .partition_cnt = partition_cnt};
        int i;

        mdt.partitions = rd_alloca(sizeof(*mdt.partitions) * partition_cnt);

        for (i = 0; i < partition_cnt; i++) {
                memset(&mdt.partitions[i], 0, sizeof(mdt.partitions[i]));
                mdt.partitions[i].id     = i;
                mdt.partitions[i].leader = leader_id;
        }

        rd_kafka_wrlock(rkt->rkt_rk);
        rd_kafka_metadata_cache_topic_update(rkt->rkt_rk, &mdt, rd_true);
        rd_kafka_topic_metadata_update(rkt, &mdt, rd_clock());
        rd_kafka_wrunlock(rkt->rkt_rk);
}
