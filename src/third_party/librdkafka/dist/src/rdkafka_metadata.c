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
#include "rdkafka_idempotence.h"
#include "rdkafka_metadata.h"

#include <string.h>
#include <stdarg.h>

/**
 * @brief Id comparator for rd_kafka_metadata_broker_internal_t
 */
int rd_kafka_metadata_broker_internal_cmp(const void *_a, const void *_b) {
        const rd_kafka_metadata_broker_internal_t *a = _a;
        const rd_kafka_metadata_broker_internal_t *b = _b;
        return RD_CMP(a->id, b->id);
}


/**
 * @brief Id comparator for struct rd_kafka_metadata_broker*
 */
int rd_kafka_metadata_broker_cmp(const void *_a, const void *_b) {
        const struct rd_kafka_metadata_broker *a = _a;
        const struct rd_kafka_metadata_broker *b = _b;
        return RD_CMP(a->id, b->id);
}


/**
 * @brief Id comparator for rd_kafka_metadata_partition_internal_t
 */
static int rd_kafka_metadata_partition_internal_cmp(const void *_a,
                                                    const void *_b) {
        const rd_kafka_metadata_partition_internal_t *a = _a;
        const rd_kafka_metadata_partition_internal_t *b = _b;
        return RD_CMP(a->id, b->id);
}

/**
 * @brief Helper function to clear a rd_kafka_metadata_partition.
 *
 * @note Does not deallocate the rd_kafka_metadata_partition itself.
 * @note Should not be used if there is an metadata struct allocated with
 * tmpabuf in which rd_kafka_metadata_partition is contained.
 */
void rd_kafka_metadata_partition_clear(
    struct rd_kafka_metadata_partition *rkmp) {
        RD_IF_FREE(rkmp->isrs, rd_free);
        RD_IF_FREE(rkmp->replicas, rd_free);
}


rd_kafka_resp_err_t
rd_kafka_metadata(rd_kafka_t *rk,
                  int all_topics,
                  rd_kafka_topic_t *only_rkt,
                  const struct rd_kafka_metadata **metadatap,
                  int timeout_ms) {
        rd_kafka_q_t *rkq;
        rd_kafka_broker_t *rkb;
        rd_kafka_op_t *rko;
        rd_kafka_resp_err_t err;
        rd_ts_t ts_end = rd_timeout_init(timeout_ms);
        rd_list_t topics;
        rd_bool_t allow_auto_create_topics =
            rk->rk_conf.allow_auto_create_topics;

        do {
                /* Query any broker that is up, and if none are up pick the
                 * first one, if we're lucky it will be up before the timeout.
                 * Previous decommissioning brokers won't be returned by the
                 * function after receiving the _DESTROY_BROKER error
                 * below. */
                rkb =
                    rd_kafka_broker_any_usable(rk, timeout_ms, RD_DO_LOCK, 0,
                                               "application metadata request");
                if (!rkb)
                        return RD_KAFKA_RESP_ERR__TRANSPORT;

                rkq = rd_kafka_q_new(rk);

                rd_list_init(&topics, 0, rd_free);
                if (!all_topics) {
                        if (only_rkt)
                                rd_list_add(
                                    &topics,
                                    rd_strdup(rd_kafka_topic_name(only_rkt)));
                        else {
                                int cache_cnt;
                                rd_kafka_local_topics_to_list(
                                    rkb->rkb_rk, &topics, &cache_cnt);
                                /* Don't trigger auto-create
                                 * for cached topics */
                                if (rd_list_cnt(&topics) == cache_cnt)
                                        allow_auto_create_topics = rd_true;
                        }
                }

                /* Async: request metadata */
                rko = rd_kafka_op_new(RD_KAFKA_OP_METADATA);
                rd_kafka_op_set_replyq(rko, rkq, 0);
                rko->rko_u.metadata.force =
                    1; /* Force metadata request regardless
                        * of outstanding metadata requests. */
                rd_kafka_MetadataRequest(
                    rkb, &topics, NULL, "application requested",
                    allow_auto_create_topics,
                    /* cgrp_update:
                     * Only update consumer group state
                     * on response if this lists all
                     * topics in the cluster, since a
                     * partial request may make it seem
                     * like some subscribed topics are missing. */
                    all_topics ? rd_true : rd_false,
                    -1 /* same subscription version */,
                    rd_false /* force_racks */, rko);

                rd_list_destroy(&topics);
                rd_kafka_broker_destroy(rkb);

                /* Wait for reply (or timeout) */
                rko = rd_kafka_q_pop(rkq, rd_timeout_remains_us(ts_end), 0);

                rd_kafka_q_destroy_owner(rkq);

                /* Timeout */
                if (!rko)
                        return RD_KAFKA_RESP_ERR__TIMED_OUT;

                /* Error */
                err = rko->rko_err;
                if (err) {
                        rd_kafka_op_destroy(rko);
                        if (err != RD_KAFKA_RESP_ERR__DESTROY_BROKER)
                                return err;
                }

                /* In case selected broker was decommissioned,
                 * try again with a different broker. */
        } while (err == RD_KAFKA_RESP_ERR__DESTROY_BROKER);

        /* Reply: pass metadata pointer to application who now owns it*/
        rd_kafka_assert(rk, rko->rko_u.metadata.md);
        *metadatap              = rko->rko_u.metadata.md;
        rko->rko_u.metadata.md  = NULL;
        rko->rko_u.metadata.mdi = NULL;
        rd_kafka_op_destroy(rko);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



void rd_kafka_metadata_destroy(const struct rd_kafka_metadata *metadata) {
        rd_free((void *)metadata);
}


static rd_kafka_metadata_internal_t *rd_kafka_metadata_copy_internal(
    const rd_kafka_metadata_internal_t *src_internal,
    size_t size,
    rd_bool_t populate_racks) {
        struct rd_kafka_metadata *md;
        rd_kafka_metadata_internal_t *mdi;
        const struct rd_kafka_metadata *src = &src_internal->metadata;
        rd_tmpabuf_t tbuf;
        int i;

        /* metadata is stored in one contigious buffer where structs and
         * and pointed-to fields are layed out in a memory aligned fashion.
         * rd_tmpabuf_t provides the infrastructure to do this.
         * Because of this we copy all the structs verbatim but
         * any pointer fields needs to be copied explicitly to update
         * the pointer address. */
        rd_tmpabuf_new(&tbuf, size, rd_true /*assert on fail*/);
        rd_tmpabuf_finalize(&tbuf);
        mdi = rd_tmpabuf_write(&tbuf, src, sizeof(*mdi));
        md  = &mdi->metadata;

        rd_tmpabuf_write_str(&tbuf, src->orig_broker_name);


        /* Copy Brokers */
        md->brokers = rd_tmpabuf_write(&tbuf, src->brokers,
                                       src->broker_cnt * sizeof(*src->brokers));
        /* Copy internal Brokers */
        mdi->brokers =
            rd_tmpabuf_write(&tbuf, src_internal->brokers,
                             src->broker_cnt * sizeof(*src_internal->brokers));

        for (i = 0; i < md->broker_cnt; i++) {
                md->brokers[i].host =
                    rd_tmpabuf_write_str(&tbuf, src->brokers[i].host);
                if (src_internal->brokers[i].rack_id) {
                        mdi->brokers[i].rack_id = rd_tmpabuf_write_str(
                            &tbuf, src_internal->brokers[i].rack_id);
                }
        }


        /* Copy TopicMetadata */
        md->topics = rd_tmpabuf_write(&tbuf, src->topics,
                                      md->topic_cnt * sizeof(*md->topics));
        /* Copy internal TopicMetadata */
        mdi->topics =
            rd_tmpabuf_write(&tbuf, src_internal->topics,
                             md->topic_cnt * sizeof(*src_internal->topics));

        for (i = 0; i < md->topic_cnt; i++) {
                int j;

                md->topics[i].topic =
                    rd_tmpabuf_write_str(&tbuf, src->topics[i].topic);


                /* Copy partitions */
                md->topics[i].partitions =
                    rd_tmpabuf_write(&tbuf, src->topics[i].partitions,
                                     md->topics[i].partition_cnt *
                                         sizeof(*md->topics[i].partitions));
                /* Copy internal partitions */
                mdi->topics[i].partitions = rd_tmpabuf_write(
                    &tbuf, src_internal->topics[i].partitions,
                    md->topics[i].partition_cnt *
                        sizeof(*src_internal->topics[i].partitions));

                for (j = 0; j < md->topics[i].partition_cnt; j++) {
                        int k;
                        char *rack;
                        rd_list_t *curr_list;

                        /* Copy replicas and ISRs */
                        md->topics[i].partitions[j].replicas = rd_tmpabuf_write(
                            &tbuf, src->topics[i].partitions[j].replicas,
                            md->topics[i].partitions[j].replica_cnt *
                                sizeof(*md->topics[i].partitions[j].replicas));

                        md->topics[i].partitions[j].isrs = rd_tmpabuf_write(
                            &tbuf, src->topics[i].partitions[j].isrs,
                            md->topics[i].partitions[j].isr_cnt *
                                sizeof(*md->topics[i].partitions[j].isrs));

                        mdi->topics[i].partitions[j].racks_cnt = 0;
                        mdi->topics[i].partitions[j].racks     = NULL;

                        /* Iterate through replicas and populate racks, if
                         * needed. */
                        if (!populate_racks)
                                continue;

                        /* This is quite possibly a recomputation, because we've
                         * already done this for the src_internal. However,
                         * since the racks need to point inside the tmpbuf, we
                         * make this calculation again. Since this is done only
                         * in a case of a full metadata refresh, this will be
                         * fairly rare. */
                        curr_list = rd_list_new(0, NULL);
                        for (k = 0; k < md->topics[i].partitions[j].replica_cnt;
                             k++) {
                                rd_kafka_metadata_broker_internal_t key = {
                                    .id = md->topics[i]
                                              .partitions[j]
                                              .replicas[k]};
                                rd_kafka_metadata_broker_internal_t *found =
                                    bsearch(
                                        &key, mdi->brokers, md->broker_cnt,
                                        sizeof(
                                            rd_kafka_metadata_broker_internal_t),
                                        rd_kafka_metadata_broker_internal_cmp);
                                if (!found || !found->rack_id)
                                        continue;
                                rd_list_add(curr_list, found->rack_id);
                        }

                        if (!rd_list_cnt(curr_list)) {
                                rd_list_destroy(curr_list);
                                continue;
                        }

                        rd_list_deduplicate(&curr_list, rd_strcmp2);

                        mdi->topics[i].partitions[j].racks_cnt =
                            rd_list_cnt(curr_list);
                        mdi->topics[i].partitions[j].racks = rd_tmpabuf_alloc(
                            &tbuf, sizeof(char *) * rd_list_cnt(curr_list));
                        RD_LIST_FOREACH(rack, curr_list, k) {
                                /* We don't copy here,`rack` points to memory
                                 * inside `mdi` already, and it's allocated
                                 * within a tmpabuf. So, the lifetime of
                                 * mdi->topics[i].partitions[j].racks[k] is the
                                 * same as the lifetime of the outer `mdi`. */
                                mdi->topics[i].partitions[j].racks[k] = rack;
                        }
                        rd_list_destroy(curr_list);
                }
        }

        /* Check for tmpabuf errors */
        if (rd_tmpabuf_failed(&tbuf))
                rd_kafka_assert(NULL, !*"metadata copy failed");

        /* Deliberately not destroying the tmpabuf since we return
         * its allocated memory. */

        return mdi;
}


/**
 * @returns a newly allocated copy of metadata \p src of size \p size
 */
rd_kafka_metadata_internal_t *
rd_kafka_metadata_copy(const rd_kafka_metadata_internal_t *src_internal,
                       size_t size) {
        return rd_kafka_metadata_copy_internal(src_internal, size, rd_false);
}


/**
 * @returns a newly allocated copy of metadata \p src of size \p size, with
 * partition racks included.
 */
rd_kafka_metadata_internal_t *rd_kafka_metadata_copy_add_racks(
    const rd_kafka_metadata_internal_t *src_internal,
    size_t size) {
        return rd_kafka_metadata_copy_internal(src_internal, size, rd_true);
}

/**
 * @brief Update topic state and information based on topic metadata.
 *
 * @param mdt Topic metadata.
 * @param mdit Topic internal metadata.
 *
 * @locality rdkafka main thread
 * @locks_acquired rd_kafka_wrlock(rk)
 */
static void rd_kafka_parse_Metadata_update_topic(
    rd_kafka_broker_t *rkb,
    const rd_kafka_metadata_topic_t *mdt,
    const rd_kafka_metadata_topic_internal_t *mdit) {

        rd_rkb_dbg(rkb, METADATA, "METADATA",
                   /* The indent below is intentional */
                   "  Topic %s with %i partitions%s%s", mdt->topic,
                   mdt->partition_cnt, mdt->err ? ": " : "",
                   mdt->err ? rd_kafka_err2str(mdt->err) : "");

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
                rd_kafka_topic_metadata_update2(rkb, mdt, mdit);
        }
}

/**
 * @brief Only brokers with Metadata version >= 9 have reliable leader
 *        epochs. Before that version, leader epoch must be treated
 *        as missing (-1).
 *
 * @param rkb The broker
 * @return Is this a broker version with reliable leader epochs?
 *
 * @locality rdkafka main thread
 */
rd_bool_t rd_kafka_has_reliable_leader_epochs(rd_kafka_broker_t *rkb) {
        int features;
        int16_t ApiVersion = 0;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_Metadata, 0, 9, &features);

        return ApiVersion >= 9;
}

/* Populates the topic partition to rack mapping for the the topic given by
 * `topic_idx` in the `mdi`. It's assumed that the internal broker metadata is
 * already populated. */
static void
rd_kafka_populate_metadata_topic_racks(rd_tmpabuf_t *tbuf,
                                       size_t topic_idx,
                                       rd_kafka_metadata_internal_t *mdi) {
        rd_kafka_metadata_broker_internal_t *brokers_internal;
        size_t broker_cnt;
        int i;
        rd_kafka_metadata_topic_t *mdt;
        rd_kafka_metadata_topic_internal_t *mdti;

        rd_dassert(mdi->brokers);
        rd_dassert(mdi->metadata.topic_cnt > (int)topic_idx);

        brokers_internal = mdi->brokers;
        broker_cnt       = mdi->metadata.broker_cnt;

        mdt  = &mdi->metadata.topics[topic_idx];
        mdti = &mdi->topics[topic_idx];

        for (i = 0; i < mdt->partition_cnt; i++) {
                int j;
                rd_kafka_metadata_partition_t *mdp = &mdt->partitions[i];
                rd_kafka_metadata_partition_internal_t *mdpi =
                    &mdti->partitions[i];

                rd_list_t *curr_list;
                char *rack;

                if (mdp->replica_cnt == 0)
                        continue;

                curr_list =
                    rd_list_new(0, NULL); /* use a list for de-duplication */
                for (j = 0; j < mdp->replica_cnt; j++) {
                        rd_kafka_metadata_broker_internal_t key = {
                            .id = mdp->replicas[j]};
                        rd_kafka_metadata_broker_internal_t *broker =
                            bsearch(&key, brokers_internal, broker_cnt,
                                    sizeof(rd_kafka_metadata_broker_internal_t),
                                    rd_kafka_metadata_broker_internal_cmp);
                        if (!broker || !broker->rack_id)
                                continue;
                        rd_list_add(curr_list, broker->rack_id);
                }
                rd_list_deduplicate(&curr_list, rd_strcmp2);

                mdpi->racks_cnt = rd_list_cnt(curr_list);
                mdpi->racks =
                    rd_tmpabuf_alloc(tbuf, sizeof(char *) * mdpi->racks_cnt);
                RD_LIST_FOREACH(rack, curr_list, j) {
                        mdpi->racks[j] = rack; /* Don't copy, rack points inside
                                                  tbuf already*/
                }
                rd_list_destroy(curr_list);
        }
}

/**
 * @brief Decommission brokers that are not in the metadata.
 */
static void rd_kafka_metadata_decommission_unavailable_brokers(
    rd_kafka_t *rk,
    rd_kafka_metadata_t *md,
    rd_kafka_broker_t *rkb_current) {
        rd_kafka_broker_t *rkb;
        rd_bool_t has_learned_brokers = rd_false;
        rd_list_t brokers_to_decommission;
        int i;

        rd_kafka_wrlock(rk);
        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                if (rkb->rkb_source == RD_KAFKA_LEARNED) {
                        has_learned_brokers = rd_true;
                        break;
                }
        }
        if (!has_learned_brokers) {
                rd_kafka_wrunlock(rk);
                return;
        }

        rd_list_init(&brokers_to_decommission,
                     rd_atomic32_get(&rk->rk_broker_cnt), NULL);
        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                rd_bool_t purge_broker;

                if (rkb->rkb_source == RD_KAFKA_LOGICAL)
                        continue;

                purge_broker = rd_true;
                if (rkb->rkb_source == RD_KAFKA_LEARNED) {
                        /* Don't purge the broker if it's available in
                         * metadata. */
                        for (i = 0; i < md->broker_cnt; i++) {
                                if (md->brokers[i].id == rkb->rkb_nodeid) {
                                        purge_broker = rd_false;
                                        break;
                                }
                        }
                }

                if (!purge_broker)
                        continue;

                /* Don't try to decommission already decommissioning brokers
                 * otherwise they could be already destroyed when
                 * `rd_kafka_broker_decommission` is called below. */
                if (rd_list_find(&rk->wait_decommissioned_brokers, rkb,
                                 rd_list_cmp_ptr) != NULL)
                        continue;

                rd_list_add(&brokers_to_decommission, rkb);
        }
        RD_LIST_FOREACH(rkb, &brokers_to_decommission, i) {
                rd_kafka_broker_decommission(rk, rkb,
                                             &rk->wait_decommissioned_thrds);
                rd_list_add(&rk->wait_decommissioned_brokers, rkb);
        }
        rd_list_destroy(&brokers_to_decommission);
        rd_kafka_wrunlock(rk);
}

/* Internal implementation for parsing Metadata. */
static rd_kafka_resp_err_t
rd_kafka_parse_Metadata0(rd_kafka_broker_t *rkb,
                         rd_kafka_buf_t *request,
                         rd_kafka_buf_t *rkbuf,
                         rd_kafka_metadata_internal_t **mdip,
                         rd_list_t *request_topics,
                         const char *reason) {
        rd_kafka_t *rk = rkb->rkb_rk;
        int i, j, k;
        rd_tmpabuf_t tbuf;
        rd_kafka_metadata_internal_t *mdi = NULL;
        rd_kafka_metadata_t *md           = NULL;
        size_t rkb_namelen;
        const int log_decode_errors  = LOG_ERR;
        rd_list_t *missing_topics    = NULL;
        rd_list_t *missing_topic_ids = NULL;

        const rd_list_t *requested_topics    = request_topics;
        const rd_list_t *requested_topic_ids = NULL;
        rd_bool_t all_topics                 = rd_false;
        rd_bool_t cgrp_update                = rd_false;
        rd_bool_t has_reliable_leader_epochs =
            rd_kafka_has_reliable_leader_epochs(rkb);
        int ApiVersion                = rkbuf->rkbuf_reqhdr.ApiVersion;
        rd_kafkap_str_t cluster_id    = RD_ZERO_INIT;
        int32_t controller_id         = -1;
        rd_kafka_resp_err_t err       = RD_KAFKA_RESP_ERR_NO_ERROR;
        int broker_changes            = 0;
        int cache_changes             = 0;
        int cgrp_subscription_version = -1;
        int16_t ErrorCode             = 0;

        /* If client rack is present, the metadata cache (topic or full) needs
         * to contain the partition to rack map. */
        rd_bool_t has_client_rack = rk->rk_conf.client_rack &&
                                    RD_KAFKAP_STR_LEN(rk->rk_conf.client_rack);
        rd_bool_t compute_racks = has_client_rack;

        if (request) {
                requested_topics    = request->rkbuf_u.Metadata.topics;
                requested_topic_ids = request->rkbuf_u.Metadata.topic_ids;
                all_topics          = request->rkbuf_u.Metadata.all_topics;
                cgrp_update =
                    request->rkbuf_u.Metadata.cgrp_update && rk->rk_cgrp;
                compute_racks |= request->rkbuf_u.Metadata.force_racks;
                cgrp_subscription_version =
                    request->rkbuf_u.Metadata.cgrp_subscription_version;
        }

        /* If there's reason is NULL, set it to a human-readable string. */
        if (!reason)
                reason = "(no reason)";

        /* Ignore metadata updates when terminating */
        if (rd_kafka_terminating(rkb->rkb_rk)) {
                err = RD_KAFKA_RESP_ERR__DESTROY;
                goto done;
        }

        rd_kafka_assert(NULL, thrd_is_current(rk->rk_thread));

        /* Remove topics from missing_topics as they are seen in Metadata. */
        if (requested_topics)
                missing_topics =
                    rd_list_copy(requested_topics, rd_list_string_copy, NULL);
        if (requested_topic_ids)
                missing_topic_ids =
                    rd_list_copy(requested_topic_ids, rd_list_Uuid_copy, NULL);

        rd_kafka_broker_lock(rkb);
        rkb_namelen = strlen(rkb->rkb_name) + 1;
        /* We assume that the marshalled representation is
         * no more than 4 times larger than the wire representation.
         * This is increased to 5 times in case if we want to compute partition
         * to rack mapping. */
        rd_tmpabuf_new(&tbuf, 0, rd_false /*dont assert on fail*/);
        rd_tmpabuf_add_alloc(&tbuf, sizeof(*mdi));
        rd_tmpabuf_add_alloc(&tbuf, rkb_namelen);
        rd_tmpabuf_add_alloc(&tbuf, rkbuf->rkbuf_totlen *
                                        (4 + (compute_racks ? 1 : 0)));

        rd_tmpabuf_finalize(&tbuf);

        if (!(mdi = rd_tmpabuf_alloc(&tbuf, sizeof(*mdi)))) {
                rd_kafka_broker_unlock(rkb);
                err = RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE;
                goto err;
        }

        md                 = &mdi->metadata;
        md->orig_broker_id = rkb->rkb_nodeid;
        md->orig_broker_name =
            rd_tmpabuf_write(&tbuf, rkb->rkb_name, rkb_namelen);
        rd_kafka_broker_unlock(rkb);

        if (ApiVersion >= 3)
                rd_kafka_buf_read_throttle_time(rkbuf);

        /* Read Brokers */
        rd_kafka_buf_read_arraycnt(rkbuf, &md->broker_cnt,
                                   RD_KAFKAP_BROKERS_MAX);

        if (!(md->brokers = rd_tmpabuf_alloc(&tbuf, md->broker_cnt *
                                                        sizeof(*md->brokers))))
                rd_kafka_buf_parse_fail(rkbuf,
                                        "%d brokers: tmpabuf memory shortage",
                                        md->broker_cnt);

        if (!(mdi->brokers = rd_tmpabuf_alloc(
                  &tbuf, md->broker_cnt * sizeof(*mdi->brokers))))
                rd_kafka_buf_parse_fail(
                    rkbuf, "%d internal brokers: tmpabuf memory shortage",
                    md->broker_cnt);

        if (!(mdi->brokers_sorted = rd_tmpabuf_alloc(
                  &tbuf, md->broker_cnt * sizeof(*mdi->brokers_sorted))))
                rd_kafka_buf_parse_fail(
                    rkbuf, "%d sorted brokers: tmpabuf memory shortage",
                    md->broker_cnt);

        for (i = 0; i < md->broker_cnt; i++) {
                rd_kafka_buf_read_i32a(rkbuf, md->brokers[i].id);
                rd_kafka_buf_read_str_tmpabuf(rkbuf, &tbuf,
                                              md->brokers[i].host);
                rd_kafka_buf_read_i32a(rkbuf, md->brokers[i].port);

                mdi->brokers[i].id = md->brokers[i].id;
                if (ApiVersion >= 1) {
                        rd_kafka_buf_read_str_tmpabuf(rkbuf, &tbuf,
                                                      mdi->brokers[i].rack_id);
                } else {
                        mdi->brokers[i].rack_id = NULL;
                }

                rd_kafka_buf_skip_tags(rkbuf);
        }

        mdi->cluster_id = NULL;
        if (ApiVersion >= 2) {
                rd_kafka_buf_read_str(rkbuf, &cluster_id);
                if (cluster_id.str)
                        mdi->cluster_id =
                            rd_tmpabuf_write_str(&tbuf, cluster_id.str);
        }

        mdi->controller_id = -1;
        if (ApiVersion >= 1) {
                rd_kafka_buf_read_i32(rkbuf, &controller_id);
                mdi->controller_id = controller_id;
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "ClusterId: %.*s, ControllerId: %" PRId32,
                           RD_KAFKAP_STR_PR(&cluster_id), controller_id);
        }

        qsort(mdi->brokers, md->broker_cnt, sizeof(mdi->brokers[i]),
              rd_kafka_metadata_broker_internal_cmp);
        memcpy(mdi->brokers_sorted, md->brokers,
               sizeof(*mdi->brokers_sorted) * md->broker_cnt);
        qsort(mdi->brokers_sorted, md->broker_cnt, sizeof(*mdi->brokers_sorted),
              rd_kafka_metadata_broker_cmp);

        /* Read TopicMetadata */
        rd_kafka_buf_read_arraycnt(rkbuf, &md->topic_cnt, RD_KAFKAP_TOPICS_MAX);
        rd_rkb_dbg(rkb, METADATA, "METADATA", "%i brokers, %i topics",
                   md->broker_cnt, md->topic_cnt);

        if (!(md->topics =
                  rd_tmpabuf_alloc(&tbuf, md->topic_cnt * sizeof(*md->topics))))
                rd_kafka_buf_parse_fail(
                    rkbuf, "%d topics: tmpabuf memory shortage", md->topic_cnt);

        if (!(mdi->topics = rd_tmpabuf_alloc(&tbuf, md->topic_cnt *
                                                        sizeof(*mdi->topics))))
                rd_kafka_buf_parse_fail(
                    rkbuf, "%d internal topics: tmpabuf memory shortage",
                    md->topic_cnt);

        for (i = 0; i < md->topic_cnt; i++) {
                rd_kafka_buf_read_i16a(rkbuf, md->topics[i].err);
                rd_kafka_buf_read_str_tmpabuf(rkbuf, &tbuf,
                                              md->topics[i].topic);

                if (ApiVersion >= 10) {
                        rd_kafka_buf_read_uuid(rkbuf, &mdi->topics[i].topic_id);
                } else {
                        mdi->topics[i].topic_id = RD_KAFKA_UUID_ZERO;
                }

                if (ApiVersion >= 1)
                        rd_kafka_buf_read_bool(rkbuf,
                                               &mdi->topics[i].is_internal);

                /* PartitionMetadata */
                rd_kafka_buf_read_arraycnt(rkbuf, &md->topics[i].partition_cnt,
                                           RD_KAFKAP_PARTITIONS_MAX);

                if (!(md->topics[i].partitions = rd_tmpabuf_alloc(
                          &tbuf, md->topics[i].partition_cnt *
                                     sizeof(*md->topics[i].partitions))))
                        rd_kafka_buf_parse_fail(rkbuf,
                                                "%s: %d partitions: "
                                                "tmpabuf memory shortage",
                                                md->topics[i].topic,
                                                md->topics[i].partition_cnt);

                if (!(mdi->topics[i].partitions = rd_tmpabuf_alloc(
                          &tbuf, md->topics[i].partition_cnt *
                                     sizeof(*mdi->topics[i].partitions))))
                        rd_kafka_buf_parse_fail(rkbuf,
                                                "%s: %d internal partitions: "
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

                        mdi->topics[i].partitions[j].id =
                            md->topics[i].partitions[j].id;
                        if (ApiVersion >= 7) {
                                rd_kafka_buf_read_i32(
                                    rkbuf,
                                    &mdi->topics[i].partitions[j].leader_epoch);
                                if (!has_reliable_leader_epochs)
                                        mdi->topics[i]
                                            .partitions[j]
                                            .leader_epoch = -1;
                        } else {
                                mdi->topics[i].partitions[j].leader_epoch = -1;
                        }
                        mdi->topics[i].partitions[j].racks_cnt = 0;
                        mdi->topics[i].partitions[j].racks     = NULL;

                        /* Replicas */
                        rd_kafka_buf_read_arraycnt(
                            rkbuf, &md->topics[i].partitions[j].replica_cnt,
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
                        rd_kafka_buf_read_arraycnt(
                            rkbuf, &md->topics[i].partitions[j].isr_cnt,
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

                        if (ApiVersion >= 5) {
                                /* OfflineReplicas int32 array (ignored) */
                                int32_t offline_replicas_cnt;

                                /* #OfflineReplicas */
                                rd_kafka_buf_read_arraycnt(
                                    rkbuf, &offline_replicas_cnt,
                                    RD_KAFKAP_BROKERS_MAX);
                                rd_kafka_buf_skip(rkbuf, offline_replicas_cnt *
                                                             sizeof(int32_t));
                        }

                        rd_kafka_buf_skip_tags(rkbuf);
                }

                mdi->topics[i].topic_authorized_operations = -1;
                if (ApiVersion >= 8) {
                        int32_t TopicAuthorizedOperations;
                        /* TopicAuthorizedOperations */
                        rd_kafka_buf_read_i32(rkbuf,
                                              &TopicAuthorizedOperations);
                        mdi->topics[i].topic_authorized_operations =
                            TopicAuthorizedOperations;
                }

                rd_kafka_buf_skip_tags(rkbuf);
        }

        mdi->cluster_authorized_operations = -1;
        if (ApiVersion >= 8 && ApiVersion <= 10) {
                int32_t ClusterAuthorizedOperations;
                /* ClusterAuthorizedOperations */
                rd_kafka_buf_read_i32(rkbuf, &ClusterAuthorizedOperations);
                mdi->cluster_authorized_operations =
                    ClusterAuthorizedOperations;
        }

        if (ApiVersion >= 13) {
                rd_kafka_buf_read_i16(rkbuf, &ErrorCode);
        }

        rd_kafka_buf_skip_tags(rkbuf);

        if (ErrorCode) {
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "Metadata response: received top level "
                           "error code %" PRId16 ": %s",
                           ErrorCode, rd_kafka_err2str(ErrorCode));
                err = ErrorCode;
                goto err;
        }

        /* Entire Metadata response now parsed without errors:
         * update our internal state according to the response. */

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

        rd_kafka_metadata_decommission_unavailable_brokers(rk, md, rkb);

        for (i = 0; i < md->topic_cnt; i++) {

                /* Ignore topics in blacklist */
                if (rkb->rkb_rk->rk_conf.topic_blacklist &&
                    rd_kafka_pattern_match(rkb->rkb_rk->rk_conf.topic_blacklist,
                                           md->topics[i].topic)) {
                        rd_rkb_dbg(rkb, TOPIC | RD_KAFKA_DBG_METADATA,
                                   "BLACKLIST",
                                   "Ignoring blacklisted topic \"%s\" "
                                   "in metadata",
                                   md->topics[i].topic);
                        continue;
                }

                /* Sort partitions by partition id */
                qsort(md->topics[i].partitions, md->topics[i].partition_cnt,
                      sizeof(*md->topics[i].partitions),
                      rd_kafka_metadata_partition_id_cmp);
                qsort(mdi->topics[i].partitions, md->topics[i].partition_cnt,
                      sizeof(*mdi->topics[i].partitions),
                      rd_kafka_metadata_partition_internal_cmp);

                if (compute_racks)
                        rd_kafka_populate_metadata_topic_racks(&tbuf, i, mdi);

                /* Update topic state based on the topic metadata */
                rd_kafka_parse_Metadata_update_topic(rkb, &md->topics[i],
                                                     &mdi->topics[i]);

                if (requested_topics)
                        rd_list_free_cb(missing_topics,
                                        rd_list_remove_cmp(missing_topics,
                                                           md->topics[i].topic,
                                                           (void *)strcmp));
                if (requested_topic_ids)
                        rd_list_free_cb(
                            missing_topic_ids,
                            rd_list_remove_cmp(missing_topic_ids,
                                               &mdi->topics[i].topic_id,
                                               (void *)rd_kafka_Uuid_ptr_cmp));
                /* Only update cache when not asking
                 * for all topics or cache entry
                 * already exists. */
                rd_kafka_wrlock(rk);
                cache_changes += rd_kafka_metadata_cache_topic_update(
                    rk, &md->topics[i], &mdi->topics[i],
                    rd_false /*propagate later*/,
                    /* use has_client_rack rather than
                    compute_racks. We need cached rack ids
                    only in case we need to rejoin the group
                    if they change and client.rack is set
                    (KIP-881). */
                    has_client_rack, rd_kafka_has_reliable_leader_epochs(rkb));
                rd_kafka_wrunlock(rk);
        }

        /* Requested topics not seen in metadata? Propogate to topic code. */
        if (missing_topics) {
                char *topic;
                rd_rkb_dbg(rkb, TOPIC, "METADATA",
                           "%d/%d requested topic(s) seen in metadata"
                           " (lookup by name)",
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
        if (missing_topic_ids) {
                rd_kafka_Uuid_t *topic_id;
                rd_rkb_dbg(rkb, TOPIC, "METADATA",
                           "%d/%d requested topic(s) seen in metadata"
                           " (lookup by id)",
                           rd_list_cnt(requested_topic_ids) -
                               rd_list_cnt(missing_topic_ids),
                           rd_list_cnt(requested_topic_ids));
                for (i = 0; i < rd_list_cnt(missing_topic_ids); i++) {
                        rd_kafka_Uuid_t *missing_topic_id =
                            missing_topic_ids->rl_elems[i];
                        rd_rkb_dbg(rkb, TOPIC, "METADATA", "wanted %s",
                                   rd_kafka_Uuid_base64str(missing_topic_id));
                }
                RD_LIST_FOREACH(topic_id, missing_topic_ids, i) {
                        rd_kafka_topic_t *rkt;

                        rd_kafka_rdlock(rk);
                        rkt = rd_kafka_topic_find_by_topic_id(rkb->rkb_rk,
                                                              *topic_id);
                        rd_kafka_rdunlock(rk);
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
        rd_kafka_rebootstrap_tmr_restart(rkb->rkb_rk);

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
                rkb->rkb_rk->rk_ts_full_metadata = rkb->rkb_rk->rk_ts_metadata;
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "Cached full metadata with "
                           " %d topic(s): %s",
                           md->topic_cnt, reason);
        }
        /* Remove cache hints for the originally requested topics. */
        if (requested_topics)
                rd_kafka_metadata_cache_purge_hints(rk, requested_topics);
        if (requested_topic_ids)
                rd_kafka_metadata_cache_purge_hints_by_id(rk,
                                                          requested_topic_ids);

        if (cache_changes) {
                rd_kafka_metadata_cache_propagate_changes(rk);
                rd_kafka_metadata_cache_expiry_start(rk);
        }

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
        if (cgrp_update &&
            (all_topics ||
             ((requested_topics || requested_topic_ids) &&
              rd_kafka_cgrp_same_subscription_version(
                  rkb->rkb_rk->rk_cgrp, cgrp_subscription_version))))
                rd_kafka_cgrp_metadata_update_check(rkb->rkb_rk->rk_cgrp,
                                                    rd_true /*do join*/);

        if (rk->rk_type == RD_KAFKA_CONSUMER && rk->rk_cgrp &&
            rk->rk_cgrp->rkcg_group_protocol == RD_KAFKA_GROUP_PROTOCOL_CLASSIC)
                rd_interval_reset(&rk->rk_cgrp->rkcg_join_intvl);

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
        if (missing_topic_ids)
                rd_list_destroy(missing_topic_ids);

        /* This metadata request was triggered by someone wanting
         * the metadata information back as a reply, so send that reply now.
         * In this case we must not rd_free the metadata memory here,
         * the requestee will do.
         * The tbuf is explicitly not destroyed as we return its memory
         * to the caller. */
        *mdip = mdi;

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
        if (requested_topic_ids) {
                /* Failed requests shall purge cache hints for
                 * the requested topics. */
                rd_kafka_wrlock(rkb->rkb_rk);
                rd_kafka_metadata_cache_purge_hints_by_id(rk,
                                                          requested_topic_ids);
                rd_kafka_wrunlock(rkb->rkb_rk);
        }

        if (missing_topics)
                rd_list_destroy(missing_topics);
        if (missing_topic_ids)
                rd_list_destroy(missing_topic_ids);
        rd_tmpabuf_destroy(&tbuf);

        return err;
}


/**
 * @brief Handle a Metadata response message.
 *
 * @param request Initial Metadata request, containing the topic information.
 *                Must not be NULL.
 *                We require the topic information while parsing to make sure
 *                that there are no missing topics.
 * @param mdip A pointer to (rd_kafka_metadata_internal_t *) into which the
 *             metadata will be marshalled (set to NULL on error.)
 *
 * @returns an error code on parse failure, else NO_ERROR.
 *
 * @locality rdkafka main thread
 */
rd_kafka_resp_err_t
rd_kafka_parse_Metadata(rd_kafka_broker_t *rkb,
                        rd_kafka_buf_t *request,
                        rd_kafka_buf_t *rkbuf,
                        rd_kafka_metadata_internal_t **mdip) {
        const char *reason = request->rkbuf_u.Metadata.reason;
        return rd_kafka_parse_Metadata0(rkb, request, rkbuf, mdip, NULL,
                                        reason);
}

/**
 * @brief Handle a Metadata response message for admin requests.
 *
 * @param request_topics List containing topics in Metadata request. Must not
 *                       be NULL. It is more convenient in the Admin flow to
 *                       preserve the topic names rather than the initial
 *                       Metadata request.
 *                       We require the topic information while parsing to make
 *                      sure that there are no missing topics.
 * @param mdip A pointer to (rd_kafka_metadata_internal_t *) into which the
 *             metadata will be marshalled (set to NULL on error.)
 *
 * @returns an error code on parse failure, else NO_ERROR.
 *
 * @locality rdkafka main thread
 */
rd_kafka_resp_err_t
rd_kafka_parse_Metadata_admin(rd_kafka_broker_t *rkb,
                              rd_kafka_buf_t *rkbuf,
                              rd_list_t *request_topics,
                              rd_kafka_metadata_internal_t **mdip) {
        return rd_kafka_parse_Metadata0(rkb, NULL, rkbuf, mdip, request_topics,
                                        "(admin request)");
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
        rd_kafka_topic_partition_list_t *unmatched;
        rd_list_t cached_topics;
        const char *topic;

        rd_kafka_rdlock(rk);
        /* To keep track of which patterns and topics in `match` that
         * did not match any topic (or matched an errored topic), we
         * create a set of all topics to match in `unmatched` and then
         * remove from this set as a match is found.
         * Whatever remains in `unmatched` after all matching is performed
         * are the topics and patterns that did not match a topic. */
        unmatched = rd_kafka_topic_partition_list_copy(match);

        /* For each topic in the cluster, scan through the match list
         * to find matching topic. */
        rd_list_init(&cached_topics, rk->rk_metadata_cache.rkmc_cnt, rd_free);
        rd_kafka_metadata_cache_topics_to_list(rk, &cached_topics, rd_false);
        RD_LIST_FOREACH(topic, &cached_topics, ti) {
                const rd_kafka_metadata_topic_internal_t *mdti;
                const rd_kafka_metadata_topic_t *mdt =
                    rd_kafka_metadata_cache_topic_get(rk, topic, &mdti,
                                                      rd_true /* valid */);
                if (!mdt)
                        continue;

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

                        if (mdt->err) {
                                rd_kafka_topic_partition_list_add(
                                    errored, topic, RD_KAFKA_PARTITION_UA)
                                    ->err = mdt->err;
                                continue; /* Skip errored topics */
                        }

                        rd_list_add(tinfos, rd_kafka_topic_info_new_with_rack(
                                                topic, mdt->partition_cnt,
                                                mdti->partitions));

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
        rd_list_destroy(&cached_topics);

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
                const char *topic                       = match->elems[i].topic;
                const rd_kafka_metadata_topic_t *mtopic = NULL;

                /* Ignore topics in blacklist */
                if (rk->rk_conf.topic_blacklist &&
                    rd_kafka_pattern_match(rk->rk_conf.topic_blacklist, topic))
                        continue;

                struct rd_kafka_metadata_cache_entry *rkmce =
                    rd_kafka_metadata_cache_find(rk, topic, 1 /* valid */);
                if (rkmce)
                        mtopic = &rkmce->rkmce_mtopic;

                if (!mtopic)
                        rd_kafka_topic_partition_list_add(errored, topic,
                                                          RD_KAFKA_PARTITION_UA)
                            ->err = RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC;
                else if (mtopic->err)
                        rd_kafka_topic_partition_list_add(errored, topic,
                                                          RD_KAFKA_PARTITION_UA)
                            ->err = mtopic->err;
                else {
                        rd_list_add(tinfos,
                                    rd_kafka_topic_info_new_with_rack(
                                        topic, mtopic->partition_cnt,
                                        rkmce->rkmce_metadata_internal_topic
                                            .partitions));

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
                                 int32_t cgrp_subscription_version,
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
                                                     RD_KAFKA_RESP_ERR__NOENT);

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
                                             RD_KAFKA_RESP_ERR__WAIT_CACHE);
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

        rd_kafka_MetadataRequest(
            rkb, &q_topics, NULL, reason, allow_auto_create, cgrp_update,
            cgrp_subscription_version, rd_false /* force_racks */, NULL);

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
                    rd_false /*!cgrp_update*/, -1, reason);

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

        if (rkcg->rkcg_group_protocol == RD_KAFKA_GROUP_PROTOCOL_CLASSIC &&
            rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WILDCARD_SUBSCRIPTION) {
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
                    allow_auto_create_topics, rd_true /*cgrp_update*/,
                    rd_atomic32_get(&rkcg->rkcg_subscription_version), reason);

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
                                         rd_false /*no cgrp update */,
                                         -1 /* same subscription version */,
                                         reason, NULL);
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
        rd_kafka_MetadataRequest(
            rkb, &topics, NULL, reason, rd_false /*no auto create*/,
            rd_true /*cgrp update*/, -1 /* same subscription version */,
            rd_false /* force_rack */, NULL);
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
                          int32_t cgrp_subscription_version,
                          const char *reason,
                          rd_kafka_op_t *rko) {
        int destroy_rkb = 0;

        if (!rkb) {
                if (!(rkb = rd_kafka_broker_any_usable(rk, RD_POLL_NOWAIT,
                                                       RD_DO_LOCK, 0, reason)))
                        return RD_KAFKA_RESP_ERR__TRANSPORT;
                destroy_rkb = 1;
        }

        rd_kafka_MetadataRequest(
            rkb, topics, NULL, reason, allow_auto_create_topics, cgrp_update,
            cgrp_subscription_version, rd_false /* force racks */, rko);

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
                    rd_false /*!cgrp_update*/, -1, "partition leader query");

                /* Back off next query exponentially till we reach
                 * the retry backoff max ms */
                rd_kafka_timer_exp_backoff(
                    rkts, rtmr, rk->rk_conf.retry_backoff_ms * 1000,
                    rk->rk_conf.retry_backoff_max_ms * 1000,
                    RD_KAFKA_RETRY_JITTER_PERCENT);
        }

        rd_list_destroy(&topics);
}



/**
 * @brief Trigger fast leader query to quickly pick up on leader changes.
 *        The fast leader query is a quick query followed by later queries at
 *        exponentially increased intervals until no topics are missing
 *        leaders.
 *
 * @param force If true, run the query immediately without waiting for the
 * interval.
 *
 * @locks none
 * @locality any
 */
void rd_kafka_metadata_fast_leader_query(rd_kafka_t *rk, rd_bool_t force) {
        rd_ts_t next;

        /* Restart the timer if it will speed things up, or if forced. */
        next = rd_kafka_timer_next(
            &rk->rk_timers, &rk->rk_metadata_cache.rkmc_query_tmr, 1 /*lock*/);
        if (force || next == -1 /* not started */ ||
            next >
                (rd_ts_t)rk->rk_conf.metadata_refresh_fast_interval_ms * 1000) {
                rd_kafka_dbg(rk, METADATA | RD_KAFKA_DBG_TOPIC, "FASTQUERY",
                             "Starting fast leader query");
                rd_kafka_timer_start(
                    &rk->rk_timers, &rk->rk_metadata_cache.rkmc_query_tmr,
                    0 /* First request should be tried immediately */,
                    rd_kafka_metadata_leader_query_tmr_cb, NULL);
        }
}



/**
 * @brief Create mock Metadata (for testing) based on the provided topics.
 *
 * @param topics elements are checked for .topic and .partition_cnt
 * @param topic_cnt is the number of topic elements in \p topics.
 * @param replication_factor is the number of replicas of each partition (set to
 * -1 to ignore).
 * @param num_brokers is the number of brokers in the cluster.
 *
 * @returns a newly allocated metadata object that must be freed with
 *          rd_kafka_metadata_destroy().
 *
 * @note \p replication_factor and \p num_brokers must be used together for
 * setting replicas of each partition.
 *
 * @sa rd_kafka_metadata_copy()
 */
rd_kafka_metadata_t *
rd_kafka_metadata_new_topic_mock(const rd_kafka_metadata_topic_t *topics,
                                 size_t topic_cnt,
                                 int replication_factor,
                                 int num_brokers) {
        rd_kafka_metadata_internal_t *mdi;
        rd_kafka_metadata_t *md;
        rd_tmpabuf_t tbuf;
        size_t i;
        int curr_broker = 0;

        /* If the replication factor is given, num_brokers must also be given */
        rd_assert(replication_factor <= 0 || num_brokers > 0);

        /* Allocate contiguous buffer which will back all the memory
         * needed by the final metadata_t object */
        rd_tmpabuf_new(&tbuf, sizeof(*mdi), rd_true /*assert on fail*/);

        rd_tmpabuf_add_alloc(&tbuf, topic_cnt * sizeof(*md->topics));
        rd_tmpabuf_add_alloc(&tbuf, topic_cnt * sizeof(*mdi->topics));
        rd_tmpabuf_add_alloc(&tbuf, num_brokers * sizeof(*md->brokers));

        /* Calculate total partition count and topic names size before
         * allocating memory. */
        for (i = 0; i < topic_cnt; i++) {
                rd_tmpabuf_add_alloc(&tbuf, 1 + strlen(topics[i].topic));
                rd_tmpabuf_add_alloc(&tbuf,
                                     topics[i].partition_cnt *
                                         sizeof(*md->topics[i].partitions));
                rd_tmpabuf_add_alloc(&tbuf,
                                     topics[i].partition_cnt *
                                         sizeof(*mdi->topics[i].partitions));
                if (replication_factor > 0)
                        rd_tmpabuf_add_alloc_times(
                            &tbuf, replication_factor * sizeof(int),
                            topics[i].partition_cnt);
        }

        rd_tmpabuf_finalize(&tbuf);

        mdi = rd_tmpabuf_alloc(&tbuf, sizeof(*mdi));
        memset(mdi, 0, sizeof(*mdi));
        md = &mdi->metadata;

        md->topic_cnt = (int)topic_cnt;
        md->topics =
            rd_tmpabuf_alloc(&tbuf, md->topic_cnt * sizeof(*md->topics));
        mdi->topics =
            rd_tmpabuf_alloc(&tbuf, md->topic_cnt * sizeof(*mdi->topics));

        md->broker_cnt = num_brokers;
        mdi->brokers =
            rd_tmpabuf_alloc(&tbuf, md->broker_cnt * sizeof(*mdi->brokers));

        for (i = 0; i < (size_t)md->topic_cnt; i++) {
                int j;

                md->topics[i].topic =
                    rd_tmpabuf_write_str(&tbuf, topics[i].topic);
                md->topics[i].partition_cnt = topics[i].partition_cnt;
                md->topics[i].err           = RD_KAFKA_RESP_ERR_NO_ERROR;

                md->topics[i].partitions = rd_tmpabuf_alloc(
                    &tbuf, md->topics[i].partition_cnt *
                               sizeof(*md->topics[i].partitions));
                mdi->topics[i].partitions = rd_tmpabuf_alloc(
                    &tbuf, md->topics[i].partition_cnt *
                               sizeof(*mdi->topics[i].partitions));

                for (j = 0; j < md->topics[i].partition_cnt; j++) {
                        int k;
                        memset(&md->topics[i].partitions[j], 0,
                               sizeof(md->topics[i].partitions[j]));
                        memset(&mdi->topics[i].partitions[j], 0,
                               sizeof(mdi->topics[i].partitions[j]));
                        md->topics[i].partitions[j].id            = j;
                        mdi->topics[i].partitions[j].id           = j;
                        mdi->topics[i].partitions[j].leader_epoch = -1;
                        mdi->topics[i].partitions[j].racks_cnt    = 0;
                        mdi->topics[i].partitions[j].racks        = NULL;
                        md->topics[i].partitions[j].id            = j;

                        /* In case replication_factor is not given, don't set
                         * replicas. */
                        if (replication_factor <= 0)
                                continue;

                        md->topics[i].partitions[j].replicas = rd_tmpabuf_alloc(
                            &tbuf, replication_factor * sizeof(int));
                        md->topics[i].partitions[j].leader = curr_broker;
                        md->topics[i].partitions[j].replica_cnt =
                            replication_factor;
                        for (k = 0; k < replication_factor; k++) {
                                md->topics[i].partitions[j].replicas[k] =
                                    (j + k + curr_broker) % num_brokers;
                        }
                }
                if (num_brokers > 0)
                        curr_broker =
                            (curr_broker + md->topics[i].partition_cnt) %
                            num_brokers;
        }

        /* Check for tmpabuf errors */
        if (rd_tmpabuf_failed(&tbuf))
                rd_assert(!*"metadata mock failed");

        /* Not destroying the tmpabuf since we return
         * its allocated memory. */
        return md;
}

/* Implementation for rd_kafka_metadata_new_topic*mockv() */
static rd_kafka_metadata_t *
rd_kafka_metadata_new_topic_mockv_internal(size_t topic_cnt,
                                           int replication_factor,
                                           int num_brokers,
                                           va_list args) {
        rd_kafka_metadata_topic_t *topics;
        size_t i;

        topics = rd_alloca(sizeof(*topics) * topic_cnt);
        for (i = 0; i < topic_cnt; i++) {
                topics[i].topic         = va_arg(args, char *);
                topics[i].partition_cnt = va_arg(args, int);
        }

        return rd_kafka_metadata_new_topic_mock(
            topics, topic_cnt, replication_factor, num_brokers);
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
        rd_kafka_metadata_t *metadata;
        va_list ap;

        va_start(ap, topic_cnt);
        metadata =
            rd_kafka_metadata_new_topic_mockv_internal(topic_cnt, -1, 0, ap);
        va_end(ap);

        return metadata;
}

/**
 * @brief Create mock Metadata (for testing) based on the
 *        var-arg tuples of (const char *topic, int partition_cnt).
 *
 * @param replication_factor is the number of replicas of each partition.
 * @param num_brokers is the number of brokers in the cluster.
 * @param topic_cnt is the number of topic,partition_cnt tuples.
 *
 * @returns a newly allocated metadata object that must be freed with
 *          rd_kafka_metadata_destroy().
 *
 * @sa rd_kafka_metadata_new_topic_mock()
 */
rd_kafka_metadata_t *rd_kafka_metadata_new_topic_with_partition_replicas_mockv(
    int replication_factor,
    int num_brokers,
    size_t topic_cnt,
    ...) {
        rd_kafka_metadata_t *metadata;
        va_list ap;

        va_start(ap, topic_cnt);
        metadata = rd_kafka_metadata_new_topic_mockv_internal(
            topic_cnt, replication_factor, num_brokers, ap);
        va_end(ap);

        return metadata;
}

/**
 * @brief Create mock Metadata (for testing) based on arrays topic_names and
 * partition_cnts.
 *
 * @param replication_factor is the number of replicas of each partition.
 * @param num_brokers is the number of brokers in the cluster.
 * @param topic_names names of topics.
 * @param partition_cnts number of partitions in each topic.
 * @param topic_cnt number of topics.
 *
 * @return rd_kafka_metadata_t*
 *
 * @sa rd_kafka_metadata_new_topic_mock()
 */
rd_kafka_metadata_t *
rd_kafka_metadata_new_topic_with_partition_replicas_mock(int replication_factor,
                                                         int num_brokers,
                                                         char *topic_names[],
                                                         int *partition_cnts,
                                                         size_t topic_cnt) {
        rd_kafka_metadata_topic_t *topics;
        size_t i;

        topics = rd_alloca(sizeof(*topics) * topic_cnt);
        for (i = 0; i < topic_cnt; i++) {
                topics[i].topic         = topic_names[i];
                topics[i].partition_cnt = partition_cnts[i];
        }

        return rd_kafka_metadata_new_topic_mock(
            topics, topic_cnt, replication_factor, num_brokers);
}

/**
 * @brief Handle update of metadata received in the produce or fetch tags.
 *
 * @param rk Client instance.
 * @param rko Metadata update operation.
 *
 * @locality main thread
 * @locks none
 *
 * @return always RD_KAFKA_OP_RES_HANDLED
 */
rd_kafka_op_res_t
rd_kafka_metadata_update_op(rd_kafka_t *rk, rd_kafka_metadata_internal_t *mdi) {
        int i, j;
        rd_kafka_metadata_t *md       = &mdi->metadata;
        rd_bool_t cache_updated       = rd_false;
        rd_kafka_secproto_t rkb_proto = rk->rk_conf.security_protocol;


        for (i = 0; i < md->broker_cnt; i++) {
                rd_kafka_broker_update(rk, rkb_proto, &md->brokers[i], NULL);
        }

        for (i = 0; i < md->topic_cnt; i++) {
                struct rd_kafka_metadata_cache_entry *rkmce;
                int32_t partition_cache_changes = 0;
                rd_bool_t by_id =
                    !RD_KAFKA_UUID_IS_ZERO(mdi->topics[i].topic_id);
                rd_kafka_Uuid_t topic_id = RD_KAFKA_UUID_ZERO;
                char *topic              = NULL;

                if (by_id) {
                        rkmce = rd_kafka_metadata_cache_find_by_id(
                            rk, mdi->topics[i].topic_id, 1);
                        topic_id = mdi->topics[i].topic_id;
                } else {
                        rkmce = rd_kafka_metadata_cache_find(
                            rk, md->topics[i].topic, 1);
                        topic = md->topics[i].topic;
                }

                if (!rkmce) {
                        if (by_id) {
                                rd_kafka_log(
                                    rk, LOG_WARNING, "METADATAUPDATE",
                                    "Topic id %s not found in cache",
                                    rd_kafka_Uuid_base64str(&topic_id));
                        } else {
                                rd_kafka_log(rk, LOG_WARNING, "METADATAUPDATE",
                                             "Topic %s not found in cache",
                                             topic);
                        }
                        continue;
                }
                topic    = rkmce->rkmce_mtopic.topic;
                topic_id = rkmce->rkmce_metadata_internal_topic.topic_id;

                for (j = 0; j < md->topics[i].partition_cnt; j++) {
                        rd_kafka_broker_t *rkb;
                        rd_kafka_metadata_partition_t *mdp =
                            &md->topics[i].partitions[j];
                        ;
                        rd_kafka_metadata_partition_internal_t *mdpi =
                            &mdi->topics[i].partitions[j];
                        int32_t part = mdp->id, current_leader_epoch;

                        if (part >= rkmce->rkmce_mtopic.partition_cnt) {
                                rd_kafka_log(rk, LOG_WARNING, "METADATAUPDATE",
                                             "Partition %s(%s)[%" PRId32
                                             "]: not found "
                                             "in cache",
                                             topic,
                                             rd_kafka_Uuid_base64str(&topic_id),
                                             part);

                                continue;
                        }

                        rkb = rd_kafka_broker_find_by_nodeid(rk, mdp->leader);
                        if (!rkb) {
                                rd_kafka_log(rk, LOG_WARNING, "METADATAUPDATE",
                                             "Partition %s(%s)[%" PRId32
                                             "]: new leader"
                                             "%" PRId32 " not found in cache",
                                             topic,
                                             rd_kafka_Uuid_base64str(&topic_id),
                                             part, mdp->leader);
                                continue;
                        }

                        current_leader_epoch =
                            rkmce->rkmce_metadata_internal_topic
                                .partitions[part]
                                .leader_epoch;

                        if (mdpi->leader_epoch != -1 &&
                            current_leader_epoch > mdpi->leader_epoch) {
                                rd_kafka_broker_destroy(rkb);
                                rd_kafka_dbg(
                                    rk, METADATA, "METADATAUPDATE",
                                    "Partition %s(%s)[%" PRId32
                                    "]: leader epoch "
                                    "is "
                                    "not newer %" PRId32 " >= %" PRId32,
                                    topic, rd_kafka_Uuid_base64str(&topic_id),
                                    part, current_leader_epoch,
                                    mdpi->leader_epoch);
                                continue;
                        }
                        partition_cache_changes++;

                        /* Need to acquire the write lock to avoid dirty reads
                         * from other threads acquiring read locks. */
                        rd_kafka_wrlock(rk);
                        rkmce->rkmce_metadata_internal_topic.partitions[part]
                            .leader_epoch = mdpi->leader_epoch;
                        rkmce->rkmce_mtopic.partitions[part].leader =
                            mdp->leader;
                        rd_kafka_wrunlock(rk);
                        rd_kafka_broker_destroy(rkb);

                        rd_kafka_dbg(rk, METADATA, "METADATAUPDATE",
                                     "Partition %s(%s)[%" PRId32
                                     "]:"
                                     " updated with leader %" PRId32
                                     " and epoch %" PRId32,
                                     topic, rd_kafka_Uuid_base64str(&topic_id),
                                     part, mdp->leader, mdpi->leader_epoch);
                }

                if (partition_cache_changes > 0) {
                        cache_updated = rd_true;
                        rd_kafka_topic_metadata_update2(
                            rk->rk_internal_rkb, &rkmce->rkmce_mtopic,
                            &rkmce->rkmce_metadata_internal_topic);
                }
        }

        if (!cache_updated) {
                rd_kafka_dbg(rk, METADATA, "METADATAUPDATE",
                             "Cache was not updated");
                return RD_KAFKA_OP_RES_HANDLED;
        }

        rd_kafka_dbg(rk, METADATA, "METADATAUPDATE",
                     "Metadata cache updated, propagating changes");
        rd_kafka_metadata_cache_propagate_changes(rk);
        rd_kafka_metadata_cache_expiry_start(rk);

        return RD_KAFKA_OP_RES_HANDLED;
}
