/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2013, Magnus Edenhill
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
#include "rdkafka_idempotence.h"
#include "rdkafka_metadata.h"

#include <string.h>
#include <stdarg.h>


rd_kafka_resp_err_t
rd_kafka_metadata(rd_kafka_t *rk,
                  int all_topics,
                  rd_kafka_topic_t *only_rkt,
                  const struct rd_kafka_metadata **metadatap,
                  int timeout_ms) {
        rd_kafka_q_t *rkq;
        rd_kafka_broker_t *rkb;
        rd_kafka_op_t *rko;
        rd_ts_t ts_end = rd_timeout_init(timeout_ms);
        rd_list_t topics;
        rd_bool_t allow_auto_create_topics =
            rk->rk_conf.allow_auto_create_topics;

        /* Query any broker that is up, and if none are up pick the first one,
         * if we're lucky it will be up before the timeout */
        rkb = rd_kafka_broker_any_usable(rk, timeout_ms, RD_DO_LOCK, 0,
                                         "application metadata request");
        if (!rkb)
                return RD_KAFKA_RESP_ERR__TRANSPORT;

        rkq = rd_kafka_q_new(rk);

        rd_list_init(&topics, 0, rd_free);
        if (!all_topics) {
                if (only_rkt)
                        rd_list_add(&topics,
                                    rd_strdup(rd_kafka_topic_name(only_rkt)));
                else {
                        int cache_cnt;
                        rd_kafka_local_topics_to_list(rkb->rkb_rk, &topics,
                                                      &cache_cnt);
                        /* Don't trigger auto-create for cached topics */
                        if (rd_list_cnt(&topics) == cache_cnt)
                                allow_auto_create_topics = rd_true;
                }
        }

        /* Async: request metadata */
        rko = rd_kafka_op_new(RD_KAFKA_OP_METADATA);
        rd_kafka_op_set_replyq(rko, rkq, 0);
        rko->rko_u.metadata.force = 1; /* Force metadata request regardless
                                        * of outstanding metadata requests. */
        rd_kafka_MetadataRequest(rkb, &topics, "application requested",
                                 allow_auto_create_topics,
                                 /* cgrp_update:
                                  * Only update consumer group state
                                  * on response if this lists all
                                  * topics in the cluster, since a
                                  * partial request may make it seem
                                  * like some subscribed topics are missing. */
                                 all_topics ? rd_true : rd_false, rko);

        rd_list_destroy(&topics);
        rd_kafka_broker_destroy(rkb);

        /* Wait for reply (or timeout) */
        rko = rd_kafka_q_pop(rkq, rd_timeout_remains_us(ts_end), 0);

        rd_kafka_q_destroy_owner(rkq);

        /* Timeout */
        if (!rko)
                return RD_KAFKA_RESP_ERR__TIMED_OUT;

        /* Error */
        if (rko->rko_err) {
                rd_kafka_resp_err_t err = rko->rko_err;
                rd_kafka_op_destroy(rko);
                return err;
        }

        /* Reply: pass metadata pointer to application who now owns it*/
        rd_kafka_assert(rk, rko->rko_u.metadata.md);
        *metadatap             = rko->rko_u.metadata.md;
        rko->rko_u.metadata.md = NULL;
        rd_kafka_op_destroy(rko);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



void rd_kafka_metadata_destroy(const struct rd_kafka_metadata *metadata) {
        rd_free((void *)metadata);
}


/**
 * @returns a newly allocated copy of metadata \p src of size \p size
 */
struct rd_kafka_metadata *
rd_kafka_metadata_copy(const struct rd_kafka_metadata *src, size_t size) {
        struct rd_kafka_metadata *md;
        rd_tmpabuf_t tbuf;
        int i;

        /* metadata is stored in one contigious buffer where structs and
         * and pointed-to fields are layed out in a memory aligned fashion.
         * rd_tmpabuf_t provides the infrastructure to do this.
         * Because of this we copy all the structs verbatim but
         * any pointer fields needs to be copied explicitly to update
         * the pointer address. */
        rd_tmpabuf_new(&tbuf, size, 1 /*assert on fail*/);
        md = rd_tmpabuf_write(&tbuf, src, sizeof(*md));

        rd_tmpabuf_write_str(&tbuf, src->orig_broker_name);


        /* Copy Brokers */
        md->brokers = rd_tmpabuf_write(&tbuf, src->brokers,
                                       md->broker_cnt * sizeof(*md->brokers));

        for (i = 0; i < md->broker_cnt; i++)
                md->brokers[i].host =
                    rd_tmpabuf_write_str(&tbuf, src->brokers[i].host);


        /* Copy TopicMetadata */
        md->topics = rd_tmpabuf_write(&tbuf, src->topics,
                                      md->topic_cnt * sizeof(*md->topics));

        for (i = 0; i < md->topic_cnt; i++) {
                int j;

                md->topics[i].topic =
                    rd_tmpabuf_write_str(&tbuf, src->topics[i].topic);


                /* Copy partitions */
                md->topics[i].partitions =
                    rd_tmpabuf_write(&tbuf, src->topics[i].partitions,
                                     md->topics[i].partition_cnt *
                                         sizeof(*md->topics[i].partitions));

                for (j = 0; j < md->topics[i].partition_cnt; j++) {
                        /* Copy replicas and ISRs */
                        md->topics[i].partitions[j].replicas = rd_tmpabuf_write(
                            &tbuf, src->topics[i].partitions[j].replicas,
                            md->topics[i].partitions[j].replica_cnt *
                                sizeof(*md->topics[i].partitions[j].replicas));

                        md->topics[i].partitions[j].isrs = rd_tmpabuf_write(
                            &tbuf, src->topics[i].partitions[j].isrs,
                            md->topics[i].partitions[j].isr_cnt *
                                sizeof(*md->topics[i].partitions[j].isrs));
                }
        }

        /* Check for tmpabuf errors */
        if (rd_tmpabuf_failed(&tbuf))
                rd_kafka_assert(NULL, !*"metadata copy failed");

        /* Delibarely not destroying the tmpabuf since we return
         * its allocated memory. */

        return md;
}



/**
 * @brief Handle a Metadata response message.
 *
 * @param topics are the requested topics (may be NULL)
 *
 * The metadata will be marshalled into 'struct rd_kafka_metadata*' structs.
 *
 * The marshalled metadata is returned in \p *mdp, (NULL on error).

 * @returns an error code on parse failure, else NO_ERRRO.
 *
 * @locality rdkafka main thread
 */
rd_kafka_resp_err_t rd_kafka_parse_Metadata(rd_kafka_broker_t *rkb,
                                            rd_kafka_buf_t *request,
                                            rd_kafka_buf_t *rkbuf,
                                            struct rd_kafka_metadata **mdp) {
        rd_kafka_t *rk = rkb->rkb_rk;
        int i, j, k;
        rd_tmpabuf_t tbuf;
        struct rd_kafka_metadata *md;
        size_t rkb_namelen;
        const int log_decode_errors       = LOG_ERR;
        rd_list_t *missing_topics         = NULL;
        const rd_list_t *requested_topics = request->rkbuf_u.Metadata.topics;
        rd_bool_t all_topics = request->rkbuf_u.Metadata.all_topics;
        rd_bool_t cgrp_update =
            request->rkbuf_u.Metadata.cgrp_update && rk->rk_cgrp;
        const char *reason = request->rkbuf_u.Metadata.reason
                                 ? request->rkbuf_u.Metadata.reason
                                 : "(no reason)";
        int ApiVersion             = request->rkbuf_reqhdr.ApiVersion;
        rd_kafkap_str_t cluster_id = RD_ZERO_INIT;
        int32_t controller_id      = -1;
        rd_kafka_resp_err_t err    = RD_KAFKA_RESP_ERR_NO_ERROR;
        int broker_changes         = 0;
        int cache_changes          = 0;

        rd_kafka_assert(NULL, thrd_is_current(rk->rk_thread));

        /* Remove topics from missing_topics as they are seen in Metadata. */
        if (requested_topics)
                missing_topics =
                    rd_list_copy(requested_topics, rd_list_string_copy, NULL);

        rd_kafka_broker_lock(rkb);
        rkb_namelen = strlen(rkb->rkb_name) + 1;
        /* We assume that the marshalled representation is
         * no more than 4 times larger than the wire representation. */
        rd_tmpabuf_new(&tbuf,
                       sizeof(*md) + rkb_namelen + (rkbuf->rkbuf_totlen * 4),
                       0 /*dont assert on fail*/);

        if (!(md = rd_tmpabuf_alloc(&tbuf, sizeof(*md)))) {
                rd_kafka_broker_unlock(rkb);
                err = RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE;
                goto err;
        }

        md->orig_broker_id = rkb->rkb_nodeid;
        md->orig_broker_name =
            rd_tmpabuf_write(&tbuf, rkb->rkb_name, rkb_namelen);
        rd_kafka_broker_unlock(rkb);

        if (ApiVersion >= 3)
                rd_kafka_buf_read_throttle_time(rkbuf);

        /* Read Brokers */
        rd_kafka_buf_read_i32a(rkbuf, md->broker_cnt);
        if (md->broker_cnt > RD_KAFKAP_BROKERS_MAX)
                rd_kafka_buf_parse_fail(rkbuf, "Broker_cnt %i > BROKERS_MAX %i",
                                        md->broker_cnt, RD_KAFKAP_BROKERS_MAX);

        if (!(md->brokers = rd_tmpabuf_alloc(&tbuf, md->broker_cnt *
                                                        sizeof(*md->brokers))))
                rd_kafka_buf_parse_fail(rkbuf,
                                        "%d brokers: tmpabuf memory shortage",
                                        md->broker_cnt);

        for (i = 0; i < md->broker_cnt; i++) {
                rd_kafka_buf_read_i32a(rkbuf, md->brokers[i].id);
                rd_kafka_buf_read_str_tmpabuf(rkbuf, &tbuf,
                                              md->brokers[i].host);
                rd_kafka_buf_read_i32a(rkbuf, md->brokers[i].port);

                if (ApiVersion >= 1) {
                        rd_kafkap_str_t rack;
                        rd_kafka_buf_read_str(rkbuf, &rack);
                }
        }

        if (ApiVersion >= 2)
                rd_kafka_buf_read_str(rkbuf, &cluster_id);

        if (ApiVersion >= 1) {
                rd_kafka_buf_read_i32(rkbuf, &controller_id);
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "ClusterId: %.*s, ControllerId: %" PRId32,
                           RD_KAFKAP_STR_PR(&cluster_id), controller_id);
        }



        /* Read TopicMetadata */
        rd_kafka_buf_read_i32a(rkbuf, md->topic_cnt);
        rd_rkb_dbg(rkb, METADATA, "METADATA", "%i brokers, %i topics",
                   md->broker_cnt, md->topic_cnt);

        if (md->topic_cnt > RD_KAFKAP_TOPICS_MAX)
                rd_kafka_buf_parse_fail(
                    rkbuf, "TopicMetadata_cnt %" PRId32 " > TOPICS_MAX %i",
                    md->topic_cnt, RD_KAFKAP_TOPICS_MAX);

        if (!(md->topics =
                  rd_tmpabuf_alloc(&tbuf, md->topic_cnt * sizeof(*md->topics))))
                rd_kafka_buf_parse_fail(
                    rkbuf, "%d topics: tmpabuf memory shortage", md->topic_cnt);

        for (i = 0; i < md->topic_cnt; i++) {
                rd_kafka_buf_read_i16a(rkbuf, md->topics[i].err);
                rd_kafka_buf_read_str_tmpabuf(rkbuf, &tbuf,
                                              md->topics[i].topic);
                if (ApiVersion >= 1) {
                        int8_t is_internal;
                        rd_kafka_buf_read_i8(rkbuf, &is_internal);
                }

                /* PartitionMetadata */
                rd_kafka_buf_read_i32a(rkbuf, md->topics[i].partition_cnt);
                if (md->topics[i].partition_cnt > RD_KAFKAP_PARTITIONS_MAX)
                        rd_kafka_buf_parse_fail(rkbuf,
                                                "TopicMetadata[%i]."
                                                "PartitionMetadata_cnt %i "
                                                "> PARTITIONS_MAX %i",
                                                i, md->topics[i].partition_cnt,
                                                RD_KAFKAP_PARTITIONS_MAX);

                if (!(md->topics[i].partitions = rd_tmpabuf_alloc(
                          &tbuf, md->topics[i].partition_cnt *
                                     sizeof(*md->topics[i].partitions))))
                        rd_kafka_buf_parse_fail(rkbuf,
                                                "%s: %d partitions: "
                                                "tmpabuf memory shortage",
                                                md->topics[i].topic,
                                                md->topics[i].partition_cnt);

                for (j = 0; j < md->topics[i].partition_cnt; j++) {
                        rd_kafka_buf_read_i16a(rkbuf,
                                               md->topics[i].partitions[j].err);
                        rd_kafka_buf_read_i32a(rkbuf,
                                               md->topics[i].partitions[j].id);
                        rd_kafka_buf_read_i32a(
                            rkbuf, md->topics[i].partitions[j].leader);

                        /* Replicas */
                        rd_kafka_buf_read_i32a(
                            rkbuf, md->topics[i].partitions[j].replica_cnt);
                        if (md->topics[i].partitions[j].replica_cnt >
                            RD_KAFKAP_BROKERS_MAX)
                                rd_kafka_buf_parse_fail(
                                    rkbuf,
                                    "TopicMetadata[%i]."
                                    "PartitionMetadata[%i]."
                                    "Replica_cnt "
                                    "%i > BROKERS_MAX %i",
                                    i, j,
                                    md->topics[i].partitions[j].replica_cnt,
                                    RD_KAFKAP_BROKERS_MAX);

                        if (!(md->topics[i].partitions[j].replicas =
                                  rd_tmpabuf_alloc(
                                      &tbuf,
                                      md->topics[i].partitions[j].replica_cnt *
                                          sizeof(*md->topics[i]
                                                      .partitions[j]
                                                      .replicas))))
                                rd_kafka_buf_parse_fail(
                                    rkbuf,
                                    "%s [%" PRId32
                                    "]: %d replicas: "
                                    "tmpabuf memory shortage",
                                    md->topics[i].topic,
                                    md->topics[i].partitions[j].id,
                                    md->topics[i].partitions[j].replica_cnt);


                        for (k = 0; k < md->topics[i].partitions[j].replica_cnt;
                             k++)
                                rd_kafka_buf_read_i32a(
                                    rkbuf,
                                    md->topics[i].partitions[j].replicas[k]);

                        /* Isrs */
                        rd_kafka_buf_read_i32a(
                            rkbuf, md->topics[i].partitions[j].isr_cnt);
                        if (md->topics[i].partitions[j].isr_cnt >
                            RD_KAFKAP_BROKERS_MAX)
                                rd_kafka_buf_parse_fail(
                                    rkbuf,
                                    "TopicMetadata[%i]."
                                    "PartitionMetadata[%i]."
                                    "Isr_cnt "
                                    "%i > BROKERS_MAX %i",
                                    i, j, md->topics[i].partitions[j].isr_cnt,
                                    RD_KAFKAP_BROKERS_MAX);

                        if (!(md->topics[i]
                                  .partitions[j]
                                  .isrs = rd_tmpabuf_alloc(
                                  &tbuf,
                                  md->topics[i].partitions[j].isr_cnt *
                                      sizeof(
                                          *md->topics[i].partitions[j].isrs))))
                                rd_kafka_buf_parse_fail(
                                    rkbuf,
                                    "%s [%" PRId32
                                    "]: %d isrs: "
                                    "tmpabuf memory shortage",
                                    md->topics[i].topic,
                                    md->topics[i].partitions[j].id,
                                    md->topics[i].partitions[j].isr_cnt);


                        for (k = 0; k < md->topics[i].partitions[j].isr_cnt;
                             k++)
                                rd_kafka_buf_read_i32a(
                                    rkbuf, md->topics[i].partitions[j].isrs[k]);
                }

                /* Sort partitions by partition id */
                qsort(md->topics[i].partitions, md->topics[i].partition_cnt,
                      sizeof(*md->topics[i].partitions),
                      rd_kafka_metadata_partition_id_cmp);
        }

        /* Entire Metadata response now parsed without errors:
         * update our internal state according to the response. */

        /* Avoid metadata updates when we're terminating. */
        if (rd_kafka_terminating(rkb->rkb_rk)) {
                err = RD_KAFKA_RESP_ERR__DESTROY;
                goto done;
        }

        if (md->broker_cnt == 0 && md->topic_cnt == 0) {
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "No brokers or topics in metadata: should retry");
                err = RD_KAFKA_RESP_ERR__PARTIAL;
                goto err;
        }

        /* Update our list of brokers. */
        for (i = 0; i < md->broker_cnt; i++) {
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "  Broker #%i/%i: %s:%i NodeId %" PRId32, i,
                           md->broker_cnt, md->brokers[i].host,
                           md->brokers[i].port, md->brokers[i].id);
                rd_kafka_broker_update(rkb->rkb_rk, rkb->rkb_proto,
                                       &md->brokers[i], NULL);
        }

        /* Update partition count and leader for each topic we know about */
        for (i = 0; i < md->topic_cnt; i++) {
                rd_kafka_metadata_topic_t *mdt = &md->topics[i];
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "  Topic #%i/%i: %s with %i partitions%s%s", i,
                           md->topic_cnt, mdt->topic, mdt->partition_cnt,
                           mdt->err ? ": " : "",
                           mdt->err ? rd_kafka_err2str(mdt->err) : "");

                /* Ignore topics in blacklist */
                if (rkb->rkb_rk->rk_conf.topic_blacklist &&
                    rd_kafka_pattern_match(rkb->rkb_rk->rk_conf.topic_blacklist,
                                           mdt->topic)) {
                        rd_rkb_dbg(rkb, TOPIC, "BLACKLIST",
                                   "Ignoring blacklisted topic \"%s\" "
                                   "in metadata",
                                   mdt->topic);
                        continue;
                }

                /* Ignore metadata completely for temporary errors. (issue #513)
                 *   LEADER_NOT_AVAILABLE: Broker is rebalancing
                 */
                if (mdt->err == RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE &&
                    mdt->partition_cnt == 0) {
                        rd_rkb_dbg(rkb, TOPIC, "METADATA",
                                   "Temporary error in metadata reply for "
                                   "topic %s (PartCnt %i): %s: ignoring",
                                   mdt->topic, mdt->partition_cnt,
                                   rd_kafka_err2str(mdt->err));
                } else {
                        /* Update local topic & partition state based
                         * on metadata */
                        rd_kafka_topic_metadata_update2(rkb, mdt);
                }

                if (requested_topics) {
                        rd_list_free_cb(missing_topics,
                                        rd_list_remove_cmp(missing_topics,
                                                           mdt->topic,
                                                           (void *)strcmp));
                        if (!all_topics) {
                                rd_kafka_wrlock(rk);
                                rd_kafka_metadata_cache_topic_update(
                                    rk, mdt, rd_false /*propagate later*/);
                                cache_changes++;
                                rd_kafka_wrunlock(rk);
                        }
                }
        }


        /* Requested topics not seen in metadata? Propogate to topic code. */
        if (missing_topics) {
                char *topic;
                rd_rkb_dbg(rkb, TOPIC, "METADATA",
                           "%d/%d requested topic(s) seen in metadata",
                           rd_list_cnt(requested_topics) -
                               rd_list_cnt(missing_topics),
                           rd_list_cnt(requested_topics));
                for (i = 0; i < rd_list_cnt(missing_topics); i++)
                        rd_rkb_dbg(rkb, TOPIC, "METADATA", "wanted %s",
                                   (char *)(missing_topics->rl_elems[i]));
                RD_LIST_FOREACH(topic, missing_topics, i) {
                        rd_kafka_topic_t *rkt;

                        rkt =
                            rd_kafka_topic_find(rkb->rkb_rk, topic, 1 /*lock*/);
                        if (rkt) {
                                /* Received metadata response contained no
                                 * information about topic 'rkt' and thus
                                 * indicates the topic is not available in the
                                 *  cluster.
                                 * Mark the topic as non-existent */
                                rd_kafka_topic_wrlock(rkt);
                                rd_kafka_topic_set_notexists(
                                    rkt, RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC);
                                rd_kafka_topic_wrunlock(rkt);

                                rd_kafka_topic_destroy0(rkt);
                        }
                }
        }


        rd_kafka_wrlock(rkb->rkb_rk);

        rkb->rkb_rk->rk_ts_metadata = rd_clock();

        /* Update cached cluster id. */
        if (RD_KAFKAP_STR_LEN(&cluster_id) > 0 &&
            (!rk->rk_clusterid ||
             rd_kafkap_str_cmp_str(&cluster_id, rk->rk_clusterid))) {
                rd_rkb_dbg(rkb, BROKER | RD_KAFKA_DBG_GENERIC, "CLUSTERID",
                           "ClusterId update \"%s\" -> \"%.*s\"",
                           rk->rk_clusterid ? rk->rk_clusterid : "",
                           RD_KAFKAP_STR_PR(&cluster_id));
                if (rk->rk_clusterid) {
                        rd_kafka_log(rk, LOG_WARNING, "CLUSTERID",
                                     "Broker %s reports different ClusterId "
                                     "\"%.*s\" than previously known \"%s\": "
                                     "a client must not be simultaneously "
                                     "connected to multiple clusters",
                                     rd_kafka_broker_name(rkb),
                                     RD_KAFKAP_STR_PR(&cluster_id),
                                     rk->rk_clusterid);
                        rd_free(rk->rk_clusterid);
                }

                rk->rk_clusterid = RD_KAFKAP_STR_DUP(&cluster_id);
                /* rd_kafka_clusterid() waits for a cache update even though
                 * the clusterid is not in the cache itself. (#3620) */
                cache_changes++;
        }

        /* Update controller id. */
        if (rkb->rkb_rk->rk_controllerid != controller_id) {
                rd_rkb_dbg(rkb, BROKER, "CONTROLLERID",
                           "ControllerId update %" PRId32 " -> %" PRId32,
                           rkb->rkb_rk->rk_controllerid, controller_id);
                rkb->rkb_rk->rk_controllerid = controller_id;
                broker_changes++;
        }

        if (all_topics) {
                rd_kafka_metadata_cache_update(rkb->rkb_rk, md,
                                               1 /*abs update*/);

                if (rkb->rkb_rk->rk_full_metadata)
                        rd_kafka_metadata_destroy(
                            rkb->rkb_rk->rk_full_metadata);
                rkb->rkb_rk->rk_full_metadata =
                    rd_kafka_metadata_copy(md, tbuf.of);
                rkb->rkb_rk->rk_ts_full_metadata = rkb->rkb_rk->rk_ts_metadata;
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "Caching full metadata with "
                           "%d broker(s) and %d topic(s): %s",
                           md->broker_cnt, md->topic_cnt, reason);
        } else {
                if (cache_changes)
                        rd_kafka_metadata_cache_propagate_changes(rk);
                rd_kafka_metadata_cache_expiry_start(rk);
        }

        /* Remove cache hints for the originally requested topics. */
        if (requested_topics)
                rd_kafka_metadata_cache_purge_hints(rk, requested_topics);

        rd_kafka_wrunlock(rkb->rkb_rk);

        if (broker_changes) {
                /* Broadcast broker metadata changes to listeners. */
                rd_kafka_brokers_broadcast_state_change(rkb->rkb_rk);
        }

        /* Check if cgrp effective subscription is affected by
         * new topic metadata.
         * Ignore if this was a broker-only refresh (no topics), or
         * the request was from the partition assignor (!cgrp_update)
         * which may contain only a sub-set of the subscribed topics (namely
         * the effective subscription of available topics) as to not
         * propagate non-included topics as non-existent. */
        if (cgrp_update && (requested_topics || all_topics))
                rd_kafka_cgrp_metadata_update_check(rkb->rkb_rk->rk_cgrp,
                                                    rd_true /*do join*/);

        /* Try to acquire a Producer ID from this broker if we
         * don't have one. */
        if (rd_kafka_is_idempotent(rkb->rkb_rk)) {
                rd_kafka_wrlock(rkb->rkb_rk);
                rd_kafka_idemp_pid_fsm(rkb->rkb_rk);
                rd_kafka_wrunlock(rkb->rkb_rk);
        }

done:
        if (missing_topics)
                rd_list_destroy(missing_topics);

        /* This metadata request was triggered by someone wanting
         * the metadata information back as a reply, so send that reply now.
         * In this case we must not rd_free the metadata memory here,
         * the requestee will do.
         * The tbuf is explicitly not destroyed as we return its memory
         * to the caller. */
        *mdp = md;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        err = rkbuf->rkbuf_err;
err:
        if (requested_topics) {
                /* Failed requests shall purge cache hints for
                 * the requested topics. */
                rd_kafka_wrlock(rkb->rkb_rk);
                rd_kafka_metadata_cache_purge_hints(rk, requested_topics);
                rd_kafka_wrunlock(rkb->rkb_rk);
        }

        if (missing_topics)
                rd_list_destroy(missing_topics);

        rd_tmpabuf_destroy(&tbuf);

        return err;
}


/**
 * @brief Add all topics in current cached full metadata
 *        that matches the topics in \p match
 *        to \p tinfos (rd_kafka_topic_info_t *).
 *
 * @param errored Any topic or wildcard pattern that did not match
 *                an available topic will be added to this list with
 *                the appropriate error set.
 *
 * @returns the number of topics matched and added to \p list
 *
 * @locks none
 * @locality any
 */
size_t
rd_kafka_metadata_topic_match(rd_kafka_t *rk,
                              rd_list_t *tinfos,
                              const rd_kafka_topic_partition_list_t *match,
                              rd_kafka_topic_partition_list_t *errored) {
        int ti, i;
        size_t cnt = 0;
        const struct rd_kafka_metadata *metadata;
        rd_kafka_topic_partition_list_t *unmatched;

        rd_kafka_rdlock(rk);
        metadata = rk->rk_full_metadata;
        if (!metadata) {
                rd_kafka_rdunlock(rk);
                return 0;
        }

        /* To keep track of which patterns and topics in `match` that
         * did not match any topic (or matched an errored topic), we
         * create a set of all topics to match in `unmatched` and then
         * remove from this set as a match is found.
         * Whatever remains in `unmatched` after all matching is performed
         * are the topics and patterns that did not match a topic. */
        unmatched = rd_kafka_topic_partition_list_copy(match);

        /* For each topic in the cluster, scan through the match list
         * to find matching topic. */
        for (ti = 0; ti < metadata->topic_cnt; ti++) {
                const char *topic = metadata->topics[ti].topic;

                /* Ignore topics in blacklist */
                if (rk->rk_conf.topic_blacklist &&
                    rd_kafka_pattern_match(rk->rk_conf.topic_blacklist, topic))
                        continue;

                /* Scan for matches */
                for (i = 0; i < match->cnt; i++) {
                        if (!rd_kafka_topic_match(rk, match->elems[i].topic,
                                                  topic))
                                continue;

                        /* Remove from unmatched */
                        rd_kafka_topic_partition_list_del(
                            unmatched, match->elems[i].topic,
                            RD_KAFKA_PARTITION_UA);

                        if (metadata->topics[ti].err) {
                                rd_kafka_topic_partition_list_add(
                                    errored, topic, RD_KAFKA_PARTITION_UA)
                                    ->err = metadata->topics[ti].err;
                                continue; /* Skip errored topics */
                        }

                        rd_list_add(
                            tinfos,
                            rd_kafka_topic_info_new(
                                topic, metadata->topics[ti].partition_cnt));

                        cnt++;
                }
        }
        rd_kafka_rdunlock(rk);

        /* Any topics/patterns still in unmatched did not match any
         * existing topics, add them to `errored`. */
        for (i = 0; i < unmatched->cnt; i++) {
                rd_kafka_topic_partition_t *elem = &unmatched->elems[i];

                rd_kafka_topic_partition_list_add(errored, elem->topic,
                                                  RD_KAFKA_PARTITION_UA)
                    ->err = RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC;
        }

        rd_kafka_topic_partition_list_destroy(unmatched);

        return cnt;
}


/**
 * @brief Add all topics in \p match that matches cached metadata.
 * @remark MUST NOT be used with wildcard topics,
 *         see rd_kafka_metadata_topic_match() for that.
 *
 * @param errored Non-existent and unauthorized topics are added to this
 *                list with the appropriate error code.
 *
 * @returns the number of topics matched and added to \p tinfos
 * @locks none
 */
size_t
rd_kafka_metadata_topic_filter(rd_kafka_t *rk,
                               rd_list_t *tinfos,
                               const rd_kafka_topic_partition_list_t *match,
                               rd_kafka_topic_partition_list_t *errored) {
        int i;
        size_t cnt = 0;

        rd_kafka_rdlock(rk);
        /* For each topic in match, look up the topic in the cache. */
        for (i = 0; i < match->cnt; i++) {
                const char *topic = match->elems[i].topic;
                const rd_kafka_metadata_topic_t *mtopic;

                /* Ignore topics in blacklist */
                if (rk->rk_conf.topic_blacklist &&
                    rd_kafka_pattern_match(rk->rk_conf.topic_blacklist, topic))
                        continue;

                mtopic =
                    rd_kafka_metadata_cache_topic_get(rk, topic, 1 /*valid*/);

                if (!mtopic)
                        rd_kafka_topic_partition_list_add(errored, topic,
                                                          RD_KAFKA_PARTITION_UA)
                            ->err = RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC;
                else if (mtopic->err)
                        rd_kafka_topic_partition_list_add(errored, topic,
                                                          RD_KAFKA_PARTITION_UA)
                            ->err = mtopic->err;
                else {
                        rd_list_add(tinfos, rd_kafka_topic_info_new(
                                                topic, mtopic->partition_cnt));

                        cnt++;
                }
        }
        rd_kafka_rdunlock(rk);

        return cnt;
}


void rd_kafka_metadata_log(rd_kafka_t *rk,
                           const char *fac,
                           const struct rd_kafka_metadata *md) {
        int i;

        rd_kafka_dbg(rk, METADATA, fac,
                     "Metadata with %d broker(s) and %d topic(s):",
                     md->broker_cnt, md->topic_cnt);

        for (i = 0; i < md->broker_cnt; i++) {
                rd_kafka_dbg(rk, METADATA, fac,
                             "  Broker #%i/%i: %s:%i NodeId %" PRId32, i,
                             md->broker_cnt, md->brokers[i].host,
                             md->brokers[i].port, md->brokers[i].id);
        }

        for (i = 0; i < md->topic_cnt; i++) {
                rd_kafka_dbg(
                    rk, METADATA, fac,
                    "  Topic #%i/%i: %s with %i partitions%s%s", i,
                    md->topic_cnt, md->topics[i].topic,
                    md->topics[i].partition_cnt, md->topics[i].err ? ": " : "",
                    md->topics[i].err ? rd_kafka_err2str(md->topics[i].err)
                                      : "");
        }
}



/**
 * @brief Refresh metadata for \p topics
 *
 * @param rk: used to look up usable broker if \p rkb is NULL.
 * @param rkb: use this broker, unless NULL then any usable broker from \p rk
 * @param force: force refresh even if topics are up-to-date in cache
 * @param allow_auto_create: Enable/disable auto creation of topics
 *                           (through MetadataRequest). Requires a modern
 *                           broker version.
 *                           Takes precedence over allow.auto.create.topics.
 * @param cgrp_update: Allow consumer group state update on response.
 *
 * @returns an error code
 *
 * @locality any
 * @locks none
 */
rd_kafka_resp_err_t
rd_kafka_metadata_refresh_topics(rd_kafka_t *rk,
                                 rd_kafka_broker_t *rkb,
                                 const rd_list_t *topics,
                                 rd_bool_t force,
                                 rd_bool_t allow_auto_create,
                                 rd_bool_t cgrp_update,
                                 const char *reason) {
        rd_list_t q_topics;
        int destroy_rkb = 0;

        if (!rk) {
                rd_assert(rkb);
                rk = rkb->rkb_rk;
        }

        rd_kafka_wrlock(rk);

        if (!rkb) {
                if (!(rkb = rd_kafka_broker_any_usable(
                          rk, RD_POLL_NOWAIT, RD_DONT_LOCK, 0, reason))) {
                        /* Hint cache that something is interested in
                         * these topics so that they will be included in
                         * a future all known_topics query. */
                        rd_kafka_metadata_cache_hint(rk, topics, NULL,
                                                     RD_KAFKA_RESP_ERR__NOENT,
                                                     0 /*dont replace*/);

                        rd_kafka_wrunlock(rk);
                        rd_kafka_dbg(rk, METADATA, "METADATA",
                                     "Skipping metadata refresh of %d topic(s):"
                                     " %s: no usable brokers",
                                     rd_list_cnt(topics), reason);

                        return RD_KAFKA_RESP_ERR__TRANSPORT;
                }
                destroy_rkb = 1;
        }

        rd_list_init(&q_topics, rd_list_cnt(topics), rd_free);

        if (!force) {

                /* Hint cache of upcoming MetadataRequest and filter
                 * out any topics that are already being requested.
                 * q_topics will contain remaining topics to query. */
                rd_kafka_metadata_cache_hint(rk, topics, &q_topics,
                                             RD_KAFKA_RESP_ERR__WAIT_CACHE,
                                             rd_false /*dont replace*/);
                rd_kafka_wrunlock(rk);

                if (rd_list_cnt(&q_topics) == 0) {
                        /* No topics need new query. */
                        rd_kafka_dbg(rk, METADATA, "METADATA",
                                     "Skipping metadata refresh of "
                                     "%d topic(s): %s: "
                                     "already being requested",
                                     rd_list_cnt(topics), reason);
                        rd_list_destroy(&q_topics);
                        if (destroy_rkb)
                                rd_kafka_broker_destroy(rkb);
                        return RD_KAFKA_RESP_ERR_NO_ERROR;
                }

        } else {
                rd_kafka_wrunlock(rk);
                rd_list_copy_to(&q_topics, topics, rd_list_string_copy, NULL);
        }

        rd_kafka_dbg(rk, METADATA, "METADATA",
                     "Requesting metadata for %d/%d topics: %s",
                     rd_list_cnt(&q_topics), rd_list_cnt(topics), reason);

        rd_kafka_MetadataRequest(rkb, &q_topics, reason, allow_auto_create,
                                 cgrp_update, NULL);

        rd_list_destroy(&q_topics);

        if (destroy_rkb)
                rd_kafka_broker_destroy(rkb);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Refresh metadata for known topics
 *
 * @param rk: used to look up usable broker if \p rkb is NULL.
 * @param rkb: use this broker, unless NULL then any usable broker from \p rk
 * @param force: refresh even if cache is up-to-date
 *
 * @returns an error code (__UNKNOWN_TOPIC if there are no local topics)
 *
 * @locality any
 * @locks none
 */
rd_kafka_resp_err_t
rd_kafka_metadata_refresh_known_topics(rd_kafka_t *rk,
                                       rd_kafka_broker_t *rkb,
                                       rd_bool_t force,
                                       const char *reason) {
        rd_list_t topics;
        rd_kafka_resp_err_t err;
        int cache_cnt = 0;
        rd_bool_t allow_auto_create_topics;

        if (!rk)
                rk = rkb->rkb_rk;

        rd_list_init(&topics, 8, rd_free);
        rd_kafka_local_topics_to_list(rk, &topics, &cache_cnt);

        /* Allow topic auto creation if there are locally known topics (rkt)
         * and not just cached (to be queried) topics. */
        allow_auto_create_topics = rk->rk_conf.allow_auto_create_topics &&
                                   rd_list_cnt(&topics) > cache_cnt;

        if (rd_list_cnt(&topics) == 0)
                err = RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC;
        else
                err = rd_kafka_metadata_refresh_topics(
                    rk, rkb, &topics, force, allow_auto_create_topics,
                    rd_false /*!cgrp_update*/, reason);

        rd_list_destroy(&topics);

        return err;
}


/**
 * @brief Refresh metadata for known and subscribed topics.
 *
 * @param rk used to look up usable broker if \p rkb is NULL..
 * @param rkb use this broker, unless NULL then any usable broker from \p rk.
 * @param reason reason of refresh, used in debug logs.
 *
 * @returns an error code (ERR__UNKNOWN_TOPIC if no topics are desired).
 *
 * @locality rdkafka main thread
 * @locks_required none
 * @locks_acquired rk(read)
 */
rd_kafka_resp_err_t
rd_kafka_metadata_refresh_consumer_topics(rd_kafka_t *rk,
                                          rd_kafka_broker_t *rkb,
                                          const char *reason) {
        rd_list_t topics;
        rd_kafka_resp_err_t err;
        rd_kafka_cgrp_t *rkcg;
        rd_bool_t allow_auto_create_topics =
            rk->rk_conf.allow_auto_create_topics;
        int cache_cnt = 0;

        if (!rk) {
                rd_assert(rkb);
                rk = rkb->rkb_rk;
        }

        rkcg = rk->rk_cgrp;
        rd_assert(rkcg != NULL);

        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WILDCARD_SUBSCRIPTION) {
                /* If there is a wildcard subscription we need to request
                 * all topics in the cluster so that we can perform
                 * regexp matching. */
                return rd_kafka_metadata_refresh_all(rk, rkb, reason);
        }

        rd_list_init(&topics, 8, rd_free);

        /* Add locally known topics, i.e., those that are currently
         * being consumed or otherwise referenced through topic_t objects. */
        rd_kafka_local_topics_to_list(rk, &topics, &cache_cnt);
        if (rd_list_cnt(&topics) == cache_cnt)
                allow_auto_create_topics = rd_false;

        /* Add subscribed (non-wildcard) topics, if any. */
        if (rkcg->rkcg_subscription)
                rd_kafka_topic_partition_list_get_topic_names(
                    rkcg->rkcg_subscription, &topics,
                    rd_false /*no wildcards*/);

        if (rd_list_cnt(&topics) == 0)
                err = RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC;
        else
                err = rd_kafka_metadata_refresh_topics(
                    rk, rkb, &topics, rd_true /*force*/,
                    allow_auto_create_topics, rd_true /*cgrp_update*/, reason);

        rd_list_destroy(&topics);

        return err;
}


/**
 * @brief Refresh broker list by metadata.
 *
 * Attempts to use sparse metadata request if possible, else falls back
 * on a full metadata request. (NOTE: sparse not implemented, KIP-4)
 *
 * @param rk: used to look up usable broker if \p rkb is NULL.
 * @param rkb: use this broker, unless NULL then any usable broker from \p rk
 *
 * @returns an error code
 *
 * @locality any
 * @locks none
 */
rd_kafka_resp_err_t rd_kafka_metadata_refresh_brokers(rd_kafka_t *rk,
                                                      rd_kafka_broker_t *rkb,
                                                      const char *reason) {
        return rd_kafka_metadata_request(rk, rkb, NULL /*brokers only*/,
                                         rd_false /*!allow auto create topics*/,
                                         rd_false /*no cgrp update */, reason,
                                         NULL);
}



/**
 * @brief Refresh metadata for all topics in cluster.
 *        This is a full metadata request which might be taxing on the
 *        broker if the cluster has many topics.
 *
 * @locality any
 * @locks none
 */
rd_kafka_resp_err_t rd_kafka_metadata_refresh_all(rd_kafka_t *rk,
                                                  rd_kafka_broker_t *rkb,
                                                  const char *reason) {
        int destroy_rkb = 0;
        rd_list_t topics;

        if (!rk) {
                rd_assert(rkb);
                rk = rkb->rkb_rk;
        }

        if (!rkb) {
                if (!(rkb = rd_kafka_broker_any_usable(rk, RD_POLL_NOWAIT,
                                                       RD_DO_LOCK, 0, reason)))
                        return RD_KAFKA_RESP_ERR__TRANSPORT;
                destroy_rkb = 1;
        }

        rd_list_init(&topics, 0, NULL); /* empty list = all topics */
        rd_kafka_MetadataRequest(rkb, &topics, reason,
                                 rd_false /*no auto create*/,
                                 rd_true /*cgrp update*/, NULL);
        rd_list_destroy(&topics);

        if (destroy_rkb)
                rd_kafka_broker_destroy(rkb);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**

 * @brief Lower-level Metadata request that takes a callback (with replyq set)
 *        which will be triggered after parsing is complete.
 *
 * @param cgrp_update Allow consumer group updates from the response.
 *
 * @locks none
 * @locality any
 */
rd_kafka_resp_err_t
rd_kafka_metadata_request(rd_kafka_t *rk,
                          rd_kafka_broker_t *rkb,
                          const rd_list_t *topics,
                          rd_bool_t allow_auto_create_topics,
                          rd_bool_t cgrp_update,
                          const char *reason,
                          rd_kafka_op_t *rko) {
        int destroy_rkb = 0;

        if (!rkb) {
                if (!(rkb = rd_kafka_broker_any_usable(rk, RD_POLL_NOWAIT,
                                                       RD_DO_LOCK, 0, reason)))
                        return RD_KAFKA_RESP_ERR__TRANSPORT;
                destroy_rkb = 1;
        }

        rd_kafka_MetadataRequest(rkb, topics, reason, allow_auto_create_topics,
                                 cgrp_update, rko);

        if (destroy_rkb)
                rd_kafka_broker_destroy(rkb);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Query timer callback to trigger refresh for topics
 *        that have partitions missing their leaders.
 *
 * @locks none
 * @locality rdkafka main thread
 */
static void rd_kafka_metadata_leader_query_tmr_cb(rd_kafka_timers_t *rkts,
                                                  void *arg) {
        rd_kafka_t *rk         = rkts->rkts_rk;
        rd_kafka_timer_t *rtmr = &rk->rk_metadata_cache.rkmc_query_tmr;
        rd_kafka_topic_t *rkt;
        rd_list_t topics;

        rd_kafka_wrlock(rk);
        rd_list_init(&topics, rk->rk_topic_cnt, rd_free);

        TAILQ_FOREACH(rkt, &rk->rk_topics, rkt_link) {
                int i, require_metadata;
                rd_kafka_topic_rdlock(rkt);

                if (rkt->rkt_state == RD_KAFKA_TOPIC_S_NOTEXISTS) {
                        /* Skip topics that are known to not exist. */
                        rd_kafka_topic_rdunlock(rkt);
                        continue;
                }

                require_metadata =
                    rkt->rkt_flags & RD_KAFKA_TOPIC_F_LEADER_UNAVAIL;

                /* Check if any partitions are missing brokers. */
                for (i = 0; !require_metadata && i < rkt->rkt_partition_cnt;
                     i++) {
                        rd_kafka_toppar_t *rktp = rkt->rkt_p[i];
                        rd_kafka_toppar_lock(rktp);
                        require_metadata =
                            !rktp->rktp_broker && !rktp->rktp_next_broker;
                        rd_kafka_toppar_unlock(rktp);
                }

                if (require_metadata || rkt->rkt_partition_cnt == 0)
                        rd_list_add(&topics, rd_strdup(rkt->rkt_topic->str));

                rd_kafka_topic_rdunlock(rkt);
        }

        rd_kafka_wrunlock(rk);

        if (rd_list_cnt(&topics) == 0) {
                /* No leader-less topics+partitions, stop the timer. */
                rd_kafka_timer_stop(rkts, rtmr, 1 /*lock*/);
        } else {
                rd_kafka_metadata_refresh_topics(
                    rk, NULL, &topics, rd_true /*force*/,
                    rk->rk_conf.allow_auto_create_topics,
                    rd_false /*!cgrp_update*/, "partition leader query");
                /* Back off next query exponentially until we reach
                 * the standard query interval - then stop the timer
                 * since the intervalled querier will do the job for us. */
                if (rk->rk_conf.metadata_refresh_interval_ms > 0 &&
                    rtmr->rtmr_interval * 2 / 1000 >=
                        rk->rk_conf.metadata_refresh_interval_ms)
                        rd_kafka_timer_stop(rkts, rtmr, 1 /*lock*/);
                else
                        rd_kafka_timer_exp_backoff(rkts, rtmr);
        }

        rd_list_destroy(&topics);
}



/**
 * @brief Trigger fast leader query to quickly pick up on leader changes.
 *        The fast leader query is a quick query followed by later queries at
 *        exponentially increased intervals until no topics are missing
 *        leaders.
 *
 * @locks none
 * @locality any
 */
void rd_kafka_metadata_fast_leader_query(rd_kafka_t *rk) {
        rd_ts_t next;

        /* Restart the timer if it will speed things up. */
        next = rd_kafka_timer_next(
            &rk->rk_timers, &rk->rk_metadata_cache.rkmc_query_tmr, 1 /*lock*/);
        if (next == -1 /* not started */ ||
            next >
                (rd_ts_t)rk->rk_conf.metadata_refresh_fast_interval_ms * 1000) {
                rd_kafka_dbg(rk, METADATA | RD_KAFKA_DBG_TOPIC, "FASTQUERY",
                             "Starting fast leader query");
                rd_kafka_timer_start(
                    &rk->rk_timers, &rk->rk_metadata_cache.rkmc_query_tmr,
                    rk->rk_conf.metadata_refresh_fast_interval_ms * 1000,
                    rd_kafka_metadata_leader_query_tmr_cb, NULL);
        }
}



/**
 * @brief Create mock Metadata (for testing) based on the provided topics.
 *
 * @param topics elements are checked for .topic and .partition_cnt
 * @param topic_cnt is the number of topic elements in \p topics.
 *
 * @returns a newly allocated metadata object that must be freed with
 *          rd_kafka_metadata_destroy().
 *
 * @sa rd_kafka_metadata_copy()
 */
rd_kafka_metadata_t *
rd_kafka_metadata_new_topic_mock(const rd_kafka_metadata_topic_t *topics,
                                 size_t topic_cnt) {
        rd_kafka_metadata_t *md;
        rd_tmpabuf_t tbuf;
        size_t topic_names_size = 0;
        int total_partition_cnt = 0;
        size_t i;

        /* Calculate total partition count and topic names size before
         * allocating memory. */
        for (i = 0; i < topic_cnt; i++) {
                topic_names_size += 1 + strlen(topics[i].topic);
                total_partition_cnt += topics[i].partition_cnt;
        }


        /* Allocate contiguous buffer which will back all the memory
         * needed by the final metadata_t object */
        rd_tmpabuf_new(
            &tbuf,
            sizeof(*md) + (sizeof(*md->topics) * topic_cnt) + topic_names_size +
                (64 /*topic name size..*/ * topic_cnt) +
                (sizeof(*md->topics[0].partitions) * total_partition_cnt),
            1 /*assert on fail*/);

        md = rd_tmpabuf_alloc(&tbuf, sizeof(*md));
        memset(md, 0, sizeof(*md));

        md->topic_cnt = (int)topic_cnt;
        md->topics =
            rd_tmpabuf_alloc(&tbuf, md->topic_cnt * sizeof(*md->topics));

        for (i = 0; i < (size_t)md->topic_cnt; i++) {
                int j;

                md->topics[i].topic =
                    rd_tmpabuf_write_str(&tbuf, topics[i].topic);
                md->topics[i].partition_cnt = topics[i].partition_cnt;
                md->topics[i].err           = RD_KAFKA_RESP_ERR_NO_ERROR;

                md->topics[i].partitions = rd_tmpabuf_alloc(
                    &tbuf, md->topics[i].partition_cnt *
                               sizeof(*md->topics[i].partitions));

                for (j = 0; j < md->topics[i].partition_cnt; j++) {
                        memset(&md->topics[i].partitions[j], 0,
                               sizeof(md->topics[i].partitions[j]));
                        md->topics[i].partitions[j].id = j;
                }
        }

        /* Check for tmpabuf errors */
        if (rd_tmpabuf_failed(&tbuf))
                rd_assert(!*"metadata mock failed");

        /* Not destroying the tmpabuf since we return
         * its allocated memory. */
        return md;
}


/**
 * @brief Create mock Metadata (for testing) based on the
 *        var-arg tuples of (const char *topic, int partition_cnt).
 *
 * @param topic_cnt is the number of topic,partition_cnt tuples.
 *
 * @returns a newly allocated metadata object that must be freed with
 *          rd_kafka_metadata_destroy().
 *
 * @sa rd_kafka_metadata_new_topic_mock()
 */
rd_kafka_metadata_t *rd_kafka_metadata_new_topic_mockv(size_t topic_cnt, ...) {
        rd_kafka_metadata_topic_t *topics;
        va_list ap;
        size_t i;

        topics = rd_alloca(sizeof(*topics) * topic_cnt);

        va_start(ap, topic_cnt);
        for (i = 0; i < topic_cnt; i++) {
                topics[i].topic         = va_arg(ap, char *);
                topics[i].partition_cnt = va_arg(ap, int);
        }
        va_end(ap);

        return rd_kafka_metadata_new_topic_mock(topics, topic_cnt);
}
