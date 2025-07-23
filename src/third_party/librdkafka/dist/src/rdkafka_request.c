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

#include <stdarg.h>

#include "rdkafka_int.h"
#include "rdkafka_request.h"
#include "rdkafka_broker.h"
#include "rdkafka_offset.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_metadata.h"
#include "rdkafka_msgset.h"
#include "rdkafka_idempotence.h"
#include "rdkafka_txnmgr.h"
#include "rdkafka_sasl.h"

#include "rdrand.h"
#include "rdstring.h"
#include "rdunittest.h"


/**
 * Kafka protocol request and response handling.
 * All of this code runs in the broker thread and uses op queues for
 * propagating results back to the various sub-systems operating in
 * other threads.
 */


/* RD_KAFKA_ERR_ACTION_.. to string map */
static const char *rd_kafka_actions_descs[] = {
    "Permanent",    "Ignore",  "Refresh",         "Retry",
    "Inform",       "Special", "MsgNotPersisted", "MsgPossiblyPersisted",
    "MsgPersisted", NULL,
};

const char *rd_kafka_actions2str(int actions) {
        static RD_TLS char actstr[128];
        return rd_flags2str(actstr, sizeof(actstr), rd_kafka_actions_descs,
                            actions);
}


/**
 * @brief Decide action(s) to take based on the returned error code.
 *
 * The optional var-args is a .._ACTION_END terminated list
 * of action,error tuples which overrides the general behaviour.
 * It is to be read as: for \p error, return \p action(s).
 *
 * @warning \p request, \p rkbuf and \p rkb may be NULL.
 */
int rd_kafka_err_action(rd_kafka_broker_t *rkb,
                        rd_kafka_resp_err_t err,
                        const rd_kafka_buf_t *request,
                        ...) {
        va_list ap;
        int actions = 0;
        int exp_act;

        if (!err)
                return 0;

        /* Match explicitly defined error mappings first. */
        va_start(ap, request);
        while ((exp_act = va_arg(ap, int))) {
                int exp_err = va_arg(ap, int);

                if (err == exp_err)
                        actions |= exp_act;
        }
        va_end(ap);

        /* Explicit error match. */
        if (actions) {
                if (err && rkb && request)
                        rd_rkb_dbg(
                            rkb, BROKER, "REQERR",
                            "%sRequest failed: %s: explicit actions %s",
                            rd_kafka_ApiKey2str(request->rkbuf_reqhdr.ApiKey),
                            rd_kafka_err2str(err),
                            rd_kafka_actions2str(actions));

                return actions;
        }

        /* Default error matching */
        switch (err) {
        case RD_KAFKA_RESP_ERR_NO_ERROR:
                break;
        case RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR_NOT_LEADER_FOR_PARTITION:
        case RD_KAFKA_RESP_ERR_BROKER_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR_REPLICA_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR_NOT_COORDINATOR:
        case RD_KAFKA_RESP_ERR__WAIT_COORD:
                /* Request metadata information update */
                actions |= RD_KAFKA_ERR_ACTION_REFRESH |
                           RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED;
                break;

        case RD_KAFKA_RESP_ERR_KAFKA_STORAGE_ERROR:
                /* Request metadata update and retry */
                actions |= RD_KAFKA_ERR_ACTION_REFRESH |
                           RD_KAFKA_ERR_ACTION_RETRY |
                           RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED;
                break;

        case RD_KAFKA_RESP_ERR__TRANSPORT:
        case RD_KAFKA_RESP_ERR__TIMED_OUT:
        case RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT:
        case RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS_AFTER_APPEND:
                actions |= RD_KAFKA_ERR_ACTION_RETRY |
                           RD_KAFKA_ERR_ACTION_MSG_POSSIBLY_PERSISTED;
                break;

        case RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS:
                /* Client-side wait-response/in-queue timeout */
        case RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE:
                actions |= RD_KAFKA_ERR_ACTION_RETRY |
                           RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED;
                break;

        case RD_KAFKA_RESP_ERR__PURGE_INFLIGHT:
                actions |= RD_KAFKA_ERR_ACTION_PERMANENT |
                           RD_KAFKA_ERR_ACTION_MSG_POSSIBLY_PERSISTED;
                break;

        case RD_KAFKA_RESP_ERR__BAD_MSG:
                /* Buffer parse failures are typically a client-side bug,
                 * treat them as permanent failures. */
                actions |= RD_KAFKA_ERR_ACTION_PERMANENT |
                           RD_KAFKA_ERR_ACTION_MSG_POSSIBLY_PERSISTED;
                break;

        case RD_KAFKA_RESP_ERR_COORDINATOR_LOAD_IN_PROGRESS:
                actions |= RD_KAFKA_ERR_ACTION_RETRY;
                break;

        case RD_KAFKA_RESP_ERR__DESTROY:
        case RD_KAFKA_RESP_ERR_INVALID_SESSION_TIMEOUT:
        case RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE:
        case RD_KAFKA_RESP_ERR__PURGE_QUEUE:
        default:
                actions |= RD_KAFKA_ERR_ACTION_PERMANENT |
                           RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED;
                break;
        }

        /* Fatal or permanent errors are not retriable */
        if (actions &
            (RD_KAFKA_ERR_ACTION_FATAL | RD_KAFKA_ERR_ACTION_PERMANENT))
                actions &= ~RD_KAFKA_ERR_ACTION_RETRY;

        /* If no request buffer was specified, which might be the case
         * in certain error call chains, mask out the retry action. */
        if (!request)
                actions &= ~RD_KAFKA_ERR_ACTION_RETRY;
        else if (request->rkbuf_reqhdr.ApiKey != RD_KAFKAP_Produce)
                /* Mask out message-related bits for non-Produce requests */
                actions &= ~RD_KAFKA_ERR_ACTION_MSG_FLAGS;

        if (err && actions && rkb && request)
                rd_rkb_dbg(
                    rkb, BROKER, "REQERR", "%sRequest failed: %s: actions %s",
                    rd_kafka_ApiKey2str(request->rkbuf_reqhdr.ApiKey),
                    rd_kafka_err2str(err), rd_kafka_actions2str(actions));

        return actions;
}


/**
 * @brief Read a list of topic+partitions+extra from \p rkbuf.
 *
 * @param rkbuf buffer to read from
 * @param estimated_part_cnt estimated number of partitions to read.
 * @param read_part_errs whether or not to read an error per partition.
 *
 * @returns a newly allocated list on success, or NULL on parse error.
 */
rd_kafka_topic_partition_list_t *
rd_kafka_buf_read_topic_partitions(rd_kafka_buf_t *rkbuf,
                                   size_t estimated_part_cnt,
                                   rd_bool_t read_offset,
                                   rd_bool_t read_part_errs) {
        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode           = 0;
        int32_t TopicArrayCnt;
        rd_kafka_topic_partition_list_t *parts = NULL;

        rd_kafka_buf_read_arraycnt(rkbuf, &TopicArrayCnt, RD_KAFKAP_TOPICS_MAX);

        parts = rd_kafka_topic_partition_list_new(
            RD_MAX(TopicArrayCnt, (int)estimated_part_cnt));

        while (TopicArrayCnt-- > 0) {
                rd_kafkap_str_t kTopic;
                int32_t PartArrayCnt;
                char *topic;

                rd_kafka_buf_read_str(rkbuf, &kTopic);
                rd_kafka_buf_read_arraycnt(rkbuf, &PartArrayCnt,
                                           RD_KAFKAP_PARTITIONS_MAX);

                RD_KAFKAP_STR_DUPA(&topic, &kTopic);

                while (PartArrayCnt-- > 0) {
                        int32_t Partition;
                        int64_t Offset;
                        rd_kafka_topic_partition_t *rktpar;

                        rd_kafka_buf_read_i32(rkbuf, &Partition);

                        rktpar = rd_kafka_topic_partition_list_add(parts, topic,
                                                                   Partition);

                        if (read_offset) {
                                rd_kafka_buf_read_i64(rkbuf, &Offset);
                                rktpar->offset = Offset;
                        }

                        if (read_part_errs) {
                                rd_kafka_buf_read_i16(rkbuf, &ErrorCode);
                                rktpar->err = ErrorCode;
                        }

                        rd_kafka_buf_skip_tags(rkbuf);
                }

                rd_kafka_buf_skip_tags(rkbuf);
        }

        return parts;

err_parse:
        if (parts)
                rd_kafka_topic_partition_list_destroy(parts);

        return NULL;
}


/**
 * @brief Write a list of topic+partitions+offsets+extra to \p rkbuf
 *
 * @returns the number of partitions written to buffer.
 *
 * @remark The \p parts list MUST be sorted.
 */
int rd_kafka_buf_write_topic_partitions(
    rd_kafka_buf_t *rkbuf,
    const rd_kafka_topic_partition_list_t *parts,
    rd_bool_t skip_invalid_offsets,
    rd_bool_t only_invalid_offsets,
    rd_bool_t write_Offset,
    rd_bool_t write_Epoch,
    rd_bool_t write_Metadata) {
        size_t of_TopicArrayCnt;
        size_t of_PartArrayCnt = 0;
        int TopicArrayCnt = 0, PartArrayCnt = 0;
        int i;
        const char *prev_topic = NULL;
        int cnt                = 0;
        rd_bool_t partition_id_only =
            !write_Offset && !write_Epoch && !write_Metadata;

        rd_assert(!only_invalid_offsets ||
                  (only_invalid_offsets != skip_invalid_offsets));

        /* TopicArrayCnt */
        of_TopicArrayCnt = rd_kafka_buf_write_arraycnt_pos(rkbuf);

        for (i = 0; i < parts->cnt; i++) {
                const rd_kafka_topic_partition_t *rktpar = &parts->elems[i];

                if (rktpar->offset < 0) {
                        if (skip_invalid_offsets)
                                continue;
                } else if (only_invalid_offsets)
                        continue;

                if (!prev_topic || strcmp(rktpar->topic, prev_topic)) {
                        /* Finish previous topic, if any. */
                        if (of_PartArrayCnt > 0) {
                                rd_kafka_buf_finalize_arraycnt(
                                    rkbuf, of_PartArrayCnt, PartArrayCnt);
                                /* Tags for previous topic struct */
                                rd_kafka_buf_write_tags(rkbuf);
                        }


                        /* Topic */
                        rd_kafka_buf_write_str(rkbuf, rktpar->topic, -1);
                        TopicArrayCnt++;
                        prev_topic = rktpar->topic;
                        /* New topic so reset partition count */
                        PartArrayCnt = 0;

                        /* PartitionArrayCnt: updated later */
                        of_PartArrayCnt =
                            rd_kafka_buf_write_arraycnt_pos(rkbuf);
                }

                /* Partition */
                rd_kafka_buf_write_i32(rkbuf, rktpar->partition);
                PartArrayCnt++;

                /* Time/Offset */
                if (write_Offset) {
                        rd_kafka_buf_write_i64(rkbuf, rktpar->offset);
                }

                if (write_Epoch) {
                        /* CommittedLeaderEpoch */
                        rd_kafka_buf_write_i32(rkbuf, -1);
                }

                if (write_Metadata) {
                        /* Metadata */
                        /* Java client 0.9.0 and broker <0.10.0 can't parse
                         * Null metadata fields, so as a workaround we send an
                         * empty string if it's Null. */
                        if (!rktpar->metadata)
                                rd_kafka_buf_write_str(rkbuf, "", 0);
                        else
                                rd_kafka_buf_write_str(rkbuf, rktpar->metadata,
                                                       rktpar->metadata_size);
                }

                /* Tags for partition struct */
                if (!partition_id_only)
                        rd_kafka_buf_write_tags(rkbuf);

                cnt++;
        }

        if (of_PartArrayCnt > 0) {
                rd_kafka_buf_finalize_arraycnt(rkbuf, of_PartArrayCnt,
                                               PartArrayCnt);
                /* Tags for topic struct */
                rd_kafka_buf_write_tags(rkbuf);
        }

        rd_kafka_buf_finalize_arraycnt(rkbuf, of_TopicArrayCnt, TopicArrayCnt);

        return cnt;
}


/**
 * @brief Send FindCoordinatorRequest.
 *
 * @param coordkey is the group.id for RD_KAFKA_COORD_GROUP,
 *                 and the transactional.id for RD_KAFKA_COORD_TXN
 */
rd_kafka_resp_err_t
rd_kafka_FindCoordinatorRequest(rd_kafka_broker_t *rkb,
                                rd_kafka_coordtype_t coordtype,
                                const char *coordkey,
                                rd_kafka_replyq_t replyq,
                                rd_kafka_resp_cb_t *resp_cb,
                                void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_FindCoordinator, 0, 2, NULL);

        if (coordtype != RD_KAFKA_COORD_GROUP && ApiVersion < 1)
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_FindCoordinator, 1,
                                         1 + 2 + strlen(coordkey));

        rd_kafka_buf_write_str(rkbuf, coordkey, -1);

        if (ApiVersion >= 1)
                rd_kafka_buf_write_i8(rkbuf, (int8_t)coordtype);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



/**
 * @brief Parses a ListOffsets reply.
 *
 * Returns the parsed offsets (and errors) in \p offsets which must have been
 * initialized by caller.
 *
 * @returns 0 on success, else an error (\p offsets may be completely or
 *          partially updated, depending on the nature of the error, and per
 *          partition error codes should be checked by the caller).
 */
static rd_kafka_resp_err_t
rd_kafka_parse_ListOffsets(rd_kafka_buf_t *rkbuf,
                           rd_kafka_topic_partition_list_t *offsets) {
        const int log_decode_errors = LOG_ERR;
        int32_t TopicArrayCnt;
        int16_t api_version;
        rd_kafka_resp_err_t all_err = RD_KAFKA_RESP_ERR_NO_ERROR;

        api_version = rkbuf->rkbuf_reqhdr.ApiVersion;

        if (api_version >= 2)
                rd_kafka_buf_read_throttle_time(rkbuf);

        /* NOTE:
         * Broker may return offsets in a different constellation than
         * in the original request .*/

        rd_kafka_buf_read_i32(rkbuf, &TopicArrayCnt);
        while (TopicArrayCnt-- > 0) {
                rd_kafkap_str_t ktopic;
                int32_t PartArrayCnt;
                char *topic_name;

                rd_kafka_buf_read_str(rkbuf, &ktopic);
                rd_kafka_buf_read_i32(rkbuf, &PartArrayCnt);

                RD_KAFKAP_STR_DUPA(&topic_name, &ktopic);

                while (PartArrayCnt-- > 0) {
                        int32_t kpartition;
                        int16_t ErrorCode;
                        int32_t OffsetArrayCnt;
                        int64_t Offset = -1;
                        rd_kafka_topic_partition_t *rktpar;

                        rd_kafka_buf_read_i32(rkbuf, &kpartition);
                        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

                        if (api_version >= 1) {
                                int64_t Timestamp;
                                rd_kafka_buf_read_i64(rkbuf, &Timestamp);
                                rd_kafka_buf_read_i64(rkbuf, &Offset);
                        } else if (api_version == 0) {
                                rd_kafka_buf_read_i32(rkbuf, &OffsetArrayCnt);
                                /* We only request one offset so just grab
                                 * the first one. */
                                while (OffsetArrayCnt-- > 0)
                                        rd_kafka_buf_read_i64(rkbuf, &Offset);
                        } else {
                                rd_kafka_assert(NULL, !*"NOTREACHED");
                        }

                        rktpar = rd_kafka_topic_partition_list_add(
                            offsets, topic_name, kpartition);
                        rktpar->err    = ErrorCode;
                        rktpar->offset = Offset;

                        if (ErrorCode && !all_err)
                                all_err = ErrorCode;
                }
        }

        return all_err;

err_parse:
        return rkbuf->rkbuf_err;
}



/**
 * @brief Parses and handles ListOffsets replies.
 *
 * Returns the parsed offsets (and errors) in \p offsets.
 * \p offsets must be initialized by the caller.
 *
 * @returns 0 on success, else an error. \p offsets may be populated on error,
 *          depending on the nature of the error.
 *          On error \p actionsp (unless NULL) is updated with the recommended
 *          error actions.
 */
rd_kafka_resp_err_t
rd_kafka_handle_ListOffsets(rd_kafka_t *rk,
                            rd_kafka_broker_t *rkb,
                            rd_kafka_resp_err_t err,
                            rd_kafka_buf_t *rkbuf,
                            rd_kafka_buf_t *request,
                            rd_kafka_topic_partition_list_t *offsets,
                            int *actionsp) {

        int actions;

        if (!err)
                err = rd_kafka_parse_ListOffsets(rkbuf, offsets);
        if (!err)
                return RD_KAFKA_RESP_ERR_NO_ERROR;

        actions = rd_kafka_err_action(
            rkb, err, request, RD_KAFKA_ERR_ACTION_PERMANENT,
            RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART,

            RD_KAFKA_ERR_ACTION_REFRESH,
            RD_KAFKA_RESP_ERR_NOT_LEADER_FOR_PARTITION,

            RD_KAFKA_ERR_ACTION_REFRESH,
            RD_KAFKA_RESP_ERR_REPLICA_NOT_AVAILABLE,

            RD_KAFKA_ERR_ACTION_REFRESH, RD_KAFKA_RESP_ERR_KAFKA_STORAGE_ERROR,

            RD_KAFKA_ERR_ACTION_REFRESH, RD_KAFKA_RESP_ERR_OFFSET_NOT_AVAILABLE,

            RD_KAFKA_ERR_ACTION_REFRESH | RD_KAFKA_ERR_ACTION_RETRY,
            RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE,

            RD_KAFKA_ERR_ACTION_REFRESH | RD_KAFKA_ERR_ACTION_RETRY,
            RD_KAFKA_RESP_ERR_FENCED_LEADER_EPOCH,

            RD_KAFKA_ERR_ACTION_RETRY, RD_KAFKA_RESP_ERR__TRANSPORT,

            RD_KAFKA_ERR_ACTION_RETRY, RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT,

            RD_KAFKA_ERR_ACTION_END);

        if (actionsp)
                *actionsp = actions;

        if (rkb)
                rd_rkb_dbg(
                    rkb, TOPIC, "OFFSET", "OffsetRequest failed: %s (%s)",
                    rd_kafka_err2str(err), rd_kafka_actions2str(actions));

        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                char tmp[256];
                /* Re-query for leader */
                rd_snprintf(tmp, sizeof(tmp), "ListOffsetsRequest failed: %s",
                            rd_kafka_err2str(err));
                rd_kafka_metadata_refresh_known_topics(rk, NULL,
                                                       rd_true /*force*/, tmp);
        }

        if ((actions & RD_KAFKA_ERR_ACTION_RETRY) &&
            rd_kafka_buf_retry(rkb, request))
                return RD_KAFKA_RESP_ERR__IN_PROGRESS;

        return err;
}



/**
 * @brief Async maker for ListOffsetsRequest.
 */
static rd_kafka_resp_err_t
rd_kafka_make_ListOffsetsRequest(rd_kafka_broker_t *rkb,
                                 rd_kafka_buf_t *rkbuf,
                                 void *make_opaque) {
        const rd_kafka_topic_partition_list_t *partitions =
            (const rd_kafka_topic_partition_list_t *)make_opaque;
        int i;
        size_t of_TopicArrayCnt = 0, of_PartArrayCnt = 0;
        const char *last_topic = "";
        int32_t topic_cnt = 0, part_cnt = 0;
        int16_t ApiVersion;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_ListOffsets, 0, 2, NULL);
        if (ApiVersion == -1)
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;

        /* ReplicaId */
        rd_kafka_buf_write_i32(rkbuf, -1);

        /* IsolationLevel */
        if (ApiVersion >= 2)
                rd_kafka_buf_write_i8(rkbuf,
                                      rkb->rkb_rk->rk_conf.isolation_level);

        /* TopicArrayCnt */
        of_TopicArrayCnt = rd_kafka_buf_write_i32(rkbuf, 0); /* updated later */

        for (i = 0; i < partitions->cnt; i++) {
                const rd_kafka_topic_partition_t *rktpar =
                    &partitions->elems[i];

                if (strcmp(rktpar->topic, last_topic)) {
                        /* Finish last topic, if any. */
                        if (of_PartArrayCnt > 0)
                                rd_kafka_buf_update_i32(rkbuf, of_PartArrayCnt,
                                                        part_cnt);

                        /* Topic */
                        rd_kafka_buf_write_str(rkbuf, rktpar->topic, -1);
                        topic_cnt++;
                        last_topic = rktpar->topic;
                        /* New topic so reset partition count */
                        part_cnt = 0;

                        /* PartitionArrayCnt: updated later */
                        of_PartArrayCnt = rd_kafka_buf_write_i32(rkbuf, 0);
                }

                /* Partition */
                rd_kafka_buf_write_i32(rkbuf, rktpar->partition);
                part_cnt++;

                /* Time/Offset */
                rd_kafka_buf_write_i64(rkbuf, rktpar->offset);

                if (ApiVersion == 0) {
                        /* MaxNumberOfOffsets */
                        rd_kafka_buf_write_i32(rkbuf, 1);
                }
        }

        if (of_PartArrayCnt > 0) {
                rd_kafka_buf_update_i32(rkbuf, of_PartArrayCnt, part_cnt);
                rd_kafka_buf_update_i32(rkbuf, of_TopicArrayCnt, topic_cnt);
        }

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_rkb_dbg(rkb, TOPIC, "OFFSET",
                   "ListOffsetsRequest (v%hd, opv %d) "
                   "for %" PRId32 " topic(s) and %" PRId32 " partition(s)",
                   ApiVersion, rkbuf->rkbuf_replyq.version, topic_cnt,
                   partitions->cnt);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Send ListOffsetsRequest for partitions in \p partitions.
 */
void rd_kafka_ListOffsetsRequest(rd_kafka_broker_t *rkb,
                                 rd_kafka_topic_partition_list_t *partitions,
                                 rd_kafka_replyq_t replyq,
                                 rd_kafka_resp_cb_t *resp_cb,
                                 void *opaque) {
        rd_kafka_buf_t *rkbuf;
        rd_kafka_topic_partition_list_t *make_parts;

        make_parts = rd_kafka_topic_partition_list_copy(partitions);
        rd_kafka_topic_partition_list_sort_by_topic(make_parts);

        rkbuf = rd_kafka_buf_new_request(
            rkb, RD_KAFKAP_ListOffsets, 1,
            /* ReplicaId+IsolationLevel+TopicArrayCnt+Topic */
            4 + 1 + 4 + 100 +
                /* PartArrayCnt */
                4 +
                /* partition_cnt * Partition+Time+MaxNumOffs */
                (make_parts->cnt * (4 + 8 + 4)));

        /* Postpone creating the request contents until time to send,
         * at which time the ApiVersion is known. */
        rd_kafka_buf_set_maker(rkbuf, rd_kafka_make_ListOffsetsRequest,
                               make_parts,
                               rd_kafka_topic_partition_list_destroy_free);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}


/**
 * Generic handler for OffsetFetch responses.
 * Offsets for included partitions will be propagated through the passed
 * 'offsets' list.
 *
 * @param rkbuf response buffer, may be NULL if \p err is set.
 * @param update_toppar update toppar's committed_offset
 * @param add_part if true add partitions from the response to \p *offsets,
 *                 else just update the partitions that are already
 *                 in \p *offsets.
 */
rd_kafka_resp_err_t
rd_kafka_handle_OffsetFetch(rd_kafka_t *rk,
                            rd_kafka_broker_t *rkb,
                            rd_kafka_resp_err_t err,
                            rd_kafka_buf_t *rkbuf,
                            rd_kafka_buf_t *request,
                            rd_kafka_topic_partition_list_t **offsets,
                            rd_bool_t update_toppar,
                            rd_bool_t add_part,
                            rd_bool_t allow_retry) {
        const int log_decode_errors = LOG_ERR;
        int32_t TopicArrayCnt;
        int64_t offset = RD_KAFKA_OFFSET_INVALID;
        int16_t ApiVersion;
        rd_kafkap_str_t metadata;
        int retry_unstable = 0;
        int i;
        int actions;
        int seen_cnt = 0;

        if (err)
                goto err;

        ApiVersion = rkbuf->rkbuf_reqhdr.ApiVersion;

        if (ApiVersion >= 3)
                rd_kafka_buf_read_throttle_time(rkbuf);

        if (!*offsets)
                *offsets = rd_kafka_topic_partition_list_new(16);

        /* Set default offset for all partitions. */
        rd_kafka_topic_partition_list_set_offsets(rkb->rkb_rk, *offsets, 0,
                                                  RD_KAFKA_OFFSET_INVALID,
                                                  0 /* !is commit */);

        rd_kafka_buf_read_arraycnt(rkbuf, &TopicArrayCnt, RD_KAFKAP_TOPICS_MAX);
        for (i = 0; i < TopicArrayCnt; i++) {
                rd_kafkap_str_t topic;
                int32_t PartArrayCnt;
                char *topic_name;
                int j;

                rd_kafka_buf_read_str(rkbuf, &topic);

                rd_kafka_buf_read_arraycnt(rkbuf, &PartArrayCnt,
                                           RD_KAFKAP_PARTITIONS_MAX);

                RD_KAFKAP_STR_DUPA(&topic_name, &topic);

                for (j = 0; j < PartArrayCnt; j++) {
                        int32_t partition;
                        rd_kafka_toppar_t *rktp;
                        rd_kafka_topic_partition_t *rktpar;
                        int32_t LeaderEpoch;
                        int16_t err2;

                        rd_kafka_buf_read_i32(rkbuf, &partition);
                        rd_kafka_buf_read_i64(rkbuf, &offset);
                        if (ApiVersion >= 5)
                                rd_kafka_buf_read_i32(rkbuf, &LeaderEpoch);
                        rd_kafka_buf_read_str(rkbuf, &metadata);
                        rd_kafka_buf_read_i16(rkbuf, &err2);
                        rd_kafka_buf_skip_tags(rkbuf);

                        rktpar = rd_kafka_topic_partition_list_find(
                            *offsets, topic_name, partition);
                        if (!rktpar && add_part)
                                rktpar = rd_kafka_topic_partition_list_add(
                                    *offsets, topic_name, partition);
                        else if (!rktpar) {
                                rd_rkb_dbg(rkb, TOPIC, "OFFSETFETCH",
                                           "OffsetFetchResponse: %s [%" PRId32
                                           "] "
                                           "not found in local list: ignoring",
                                           topic_name, partition);
                                continue;
                        }

                        seen_cnt++;

                        if (!(rktp = rktpar->_private)) {
                                rktp = rd_kafka_toppar_get2(
                                    rkb->rkb_rk, topic_name, partition, 0, 0);
                                /* May be NULL if topic is not locally known */
                                rktpar->_private = rktp;
                        }

                        /* broker reports invalid offset as -1 */
                        if (offset == -1)
                                rktpar->offset = RD_KAFKA_OFFSET_INVALID;
                        else
                                rktpar->offset = offset;
                        rktpar->err = err2;

                        rd_rkb_dbg(rkb, TOPIC, "OFFSETFETCH",
                                   "OffsetFetchResponse: %s [%" PRId32
                                   "] "
                                   "offset %" PRId64
                                   ", metadata %d byte(s): %s",
                                   topic_name, partition, offset,
                                   RD_KAFKAP_STR_LEN(&metadata),
                                   rd_kafka_err2name(rktpar->err));

                        if (update_toppar && !err2 && rktp) {
                                /* Update toppar's committed offset */
                                rd_kafka_toppar_lock(rktp);
                                rktp->rktp_committed_offset = rktpar->offset;
                                rd_kafka_toppar_unlock(rktp);
                        }

                        if (rktpar->err ==
                            RD_KAFKA_RESP_ERR_UNSTABLE_OFFSET_COMMIT)
                                retry_unstable++;


                        if (rktpar->metadata)
                                rd_free(rktpar->metadata);

                        if (RD_KAFKAP_STR_IS_NULL(&metadata)) {
                                rktpar->metadata      = NULL;
                                rktpar->metadata_size = 0;
                        } else {
                                rktpar->metadata = RD_KAFKAP_STR_DUP(&metadata);
                                rktpar->metadata_size =
                                    RD_KAFKAP_STR_LEN(&metadata);
                        }
                }

                rd_kafka_buf_skip_tags(rkbuf);
        }

        if (ApiVersion >= 2) {
                int16_t ErrorCode;
                rd_kafka_buf_read_i16(rkbuf, &ErrorCode);
                if (ErrorCode) {
                        err = ErrorCode;
                        goto err;
                }
        }


err:
        if (!*offsets)
                rd_rkb_dbg(rkb, TOPIC, "OFFFETCH", "OffsetFetch returned %s",
                           rd_kafka_err2str(err));
        else
                rd_rkb_dbg(rkb, TOPIC, "OFFFETCH",
                           "OffsetFetch for %d/%d partition(s) "
                           "(%d unstable partition(s)) returned %s",
                           seen_cnt, (*offsets)->cnt, retry_unstable,
                           rd_kafka_err2str(err));

        actions =
            rd_kafka_err_action(rkb, err, request, RD_KAFKA_ERR_ACTION_END);

        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                /* Re-query for coordinator */
                rd_kafka_cgrp_op(rkb->rkb_rk->rk_cgrp, NULL, RD_KAFKA_NO_REPLYQ,
                                 RD_KAFKA_OP_COORD_QUERY, err);
        }

        if (actions & RD_KAFKA_ERR_ACTION_RETRY || retry_unstable) {
                if (allow_retry && rd_kafka_buf_retry(rkb, request))
                        return RD_KAFKA_RESP_ERR__IN_PROGRESS;
                /* FALLTHRU */
        }

        return err;

err_parse:
        err = rkbuf->rkbuf_err;
        goto err;
}



/**
 * @brief Handle OffsetFetch response based on an RD_KAFKA_OP_OFFSET_FETCH
 *        rko in \p opaque.
 *
 * @param opaque rko wrapper for handle_OffsetFetch.
 *
 * The \c rko->rko_u.offset_fetch.partitions list will be filled in with
 * the fetched offsets.
 *
 * A reply will be sent on 'rko->rko_replyq' with type RD_KAFKA_OP_OFFSET_FETCH.
 *
 * @remark \p rkb, \p rkbuf and \p request are optional.
 *
 * @remark The \p request buffer may be retried on error.
 *
 * @locality cgrp's broker thread
 */
void rd_kafka_op_handle_OffsetFetch(rd_kafka_t *rk,
                                    rd_kafka_broker_t *rkb,
                                    rd_kafka_resp_err_t err,
                                    rd_kafka_buf_t *rkbuf,
                                    rd_kafka_buf_t *request,
                                    void *opaque) {
        rd_kafka_op_t *rko = opaque;
        rd_kafka_op_t *rko_reply;
        rd_kafka_topic_partition_list_t *offsets;

        RD_KAFKA_OP_TYPE_ASSERT(rko, RD_KAFKA_OP_OFFSET_FETCH);

        if (err == RD_KAFKA_RESP_ERR__DESTROY) {
                /* Termination, quick cleanup. */
                rd_kafka_op_destroy(rko);
                return;
        }

        offsets = rd_kafka_topic_partition_list_copy(
            rko->rko_u.offset_fetch.partitions);

        /* If all partitions already had usable offsets then there
         * was no request sent and thus no reply, the offsets list is
         * good to go.. */
        if (rkbuf) {
                /* ..else parse the response (or perror) */
                err = rd_kafka_handle_OffsetFetch(
                    rkb->rkb_rk, rkb, err, rkbuf, request, &offsets,
                    rd_false /*dont update rktp*/, rd_false /*dont add part*/,
                    /* Allow retries if replyq
                     * is valid */
                    rd_kafka_op_replyq_is_valid(rko));
                if (err == RD_KAFKA_RESP_ERR__IN_PROGRESS) {
                        if (offsets)
                                rd_kafka_topic_partition_list_destroy(offsets);
                        return; /* Retrying */
                }
        }

        rko_reply =
            rd_kafka_op_new(RD_KAFKA_OP_OFFSET_FETCH | RD_KAFKA_OP_REPLY);
        rko_reply->rko_err                       = err;
        rko_reply->rko_u.offset_fetch.partitions = offsets;
        rko_reply->rko_u.offset_fetch.do_free    = 1;
        if (rko->rko_rktp)
                rko_reply->rko_rktp = rd_kafka_toppar_keep(rko->rko_rktp);

        rd_kafka_replyq_enq(&rko->rko_replyq, rko_reply, 0);

        rd_kafka_op_destroy(rko);
}

/**
 * Send OffsetFetchRequest for a consumer group id.
 *
 * Any partition with a usable offset will be ignored, if all partitions
 * have usable offsets then no request is sent at all but an empty
 * reply is enqueued on the replyq.
 *
 * @param group_id Request offset for this group id.
 * @param parts (optional) List of topic partitions to request,
 *              or NULL to return all topic partitions associated with the
 *              group.
 * @param require_stable_offsets Whether broker should return stable offsets
 *                               (transaction-committed).
 * @param timeout Optional timeout to set to the buffer.
 */
void rd_kafka_OffsetFetchRequest(rd_kafka_broker_t *rkb,
                                 const char *group_id,
                                 rd_kafka_topic_partition_list_t *parts,
                                 rd_bool_t require_stable_offsets,
                                 int timeout,
                                 rd_kafka_replyq_t replyq,
                                 rd_kafka_resp_cb_t *resp_cb,
                                 void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion;
        size_t parts_size = 0;
        int PartCnt       = -1;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_OffsetFetch, 0, 7, NULL);

        if (parts) {
                parts_size = parts->cnt * 32;
        }

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_OffsetFetch, 1,
            /* GroupId + rd_kafka_buf_write_arraycnt_pos +
             * Topics + RequireStable */
            32 + 4 + parts_size + 1, ApiVersion >= 6 /*flexver*/);

        /* ConsumerGroup */
        rd_kafka_buf_write_str(rkbuf, group_id, -1);

        if (parts) {
                /* Sort partitions by topic */
                rd_kafka_topic_partition_list_sort_by_topic(parts);
                /* Write partition list, filtering out partitions with valid
                 * offsets */
                PartCnt = rd_kafka_buf_write_topic_partitions(
                    rkbuf, parts, rd_false /*include invalid offsets*/,
                    rd_false /*skip valid offsets */,
                    rd_false /*don't write offsets*/,
                    rd_false /*don't write epoch */,
                    rd_false /*don't write metadata*/);
        } else {
                rd_kafka_buf_write_arraycnt_pos(rkbuf);
        }

        if (ApiVersion >= 7) {
                /* RequireStable */
                rd_kafka_buf_write_i8(rkbuf, require_stable_offsets);
        }

        if (PartCnt == 0) {
                /* No partitions needs OffsetFetch, enqueue empty
                 * response right away. */
                rkbuf->rkbuf_replyq = replyq;
                rkbuf->rkbuf_cb     = resp_cb;
                rkbuf->rkbuf_opaque = opaque;
                rd_kafka_buf_callback(rkb->rkb_rk, rkb, 0, NULL, rkbuf);
                return;
        }

        if (timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, timeout + 1000, 0);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        if (parts) {
                rd_rkb_dbg(
                    rkb, TOPIC | RD_KAFKA_DBG_CGRP | RD_KAFKA_DBG_CONSUMER,
                    "OFFSET",
                    "Group %s OffsetFetchRequest(v%d) for %d/%d partition(s)",
                    group_id, ApiVersion, PartCnt, parts->cnt);
        } else {
                rd_rkb_dbg(
                    rkb, TOPIC | RD_KAFKA_DBG_CGRP | RD_KAFKA_DBG_CONSUMER,
                    "OFFSET",
                    "Group %s OffsetFetchRequest(v%d) for all partitions",
                    group_id, ApiVersion);
        }

        /* Let handler decide if retries should be performed */
        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_MAX_RETRIES;

        if (parts) {
                rd_rkb_dbg(rkb, CGRP | RD_KAFKA_DBG_CONSUMER, "OFFSET",
                           "Fetch committed offsets for %d/%d partition(s)",
                           PartCnt, parts->cnt);
        } else {
                rd_rkb_dbg(rkb, CGRP | RD_KAFKA_DBG_CONSUMER, "OFFSET",
                           "Fetch committed offsets all the partitions");
        }

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}



/**
 * @brief Handle per-partition OffsetCommit errors and returns actions flags.
 */
static int
rd_kafka_handle_OffsetCommit_error(rd_kafka_broker_t *rkb,
                                   rd_kafka_buf_t *request,
                                   const rd_kafka_topic_partition_t *rktpar) {

        /* These actions are mimicking AK's ConsumerCoordinator.java */

        return rd_kafka_err_action(
            rkb, rktpar->err, request,

            RD_KAFKA_ERR_ACTION_PERMANENT,
            RD_KAFKA_RESP_ERR_GROUP_AUTHORIZATION_FAILED,

            RD_KAFKA_ERR_ACTION_PERMANENT,
            RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED,


            RD_KAFKA_ERR_ACTION_PERMANENT,
            RD_KAFKA_RESP_ERR_OFFSET_METADATA_TOO_LARGE,

            RD_KAFKA_ERR_ACTION_PERMANENT,
            RD_KAFKA_RESP_ERR_INVALID_COMMIT_OFFSET_SIZE,


            RD_KAFKA_ERR_ACTION_RETRY,
            RD_KAFKA_RESP_ERR_COORDINATOR_LOAD_IN_PROGRESS,

            RD_KAFKA_ERR_ACTION_RETRY, RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART,


            /* .._SPECIAL: mark coordinator dead, refresh and retry */
            RD_KAFKA_ERR_ACTION_REFRESH | RD_KAFKA_ERR_ACTION_RETRY |
                RD_KAFKA_ERR_ACTION_SPECIAL,
            RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE,

            RD_KAFKA_ERR_ACTION_REFRESH | RD_KAFKA_ERR_ACTION_RETRY |
                RD_KAFKA_ERR_ACTION_SPECIAL,
            RD_KAFKA_RESP_ERR_NOT_COORDINATOR,

            /* Replicas possibly unavailable:
             * Refresh coordinator (but don't mark as dead (!.._SPECIAL)),
             * and retry */
            RD_KAFKA_ERR_ACTION_REFRESH | RD_KAFKA_ERR_ACTION_RETRY,
            RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT,


            /* FIXME: There are some cases in the Java code where
             *        this is not treated as a fatal error. */
            RD_KAFKA_ERR_ACTION_PERMANENT | RD_KAFKA_ERR_ACTION_FATAL,
            RD_KAFKA_RESP_ERR_FENCED_INSTANCE_ID,


            RD_KAFKA_ERR_ACTION_PERMANENT,
            RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS,


            RD_KAFKA_ERR_ACTION_PERMANENT, RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID,

            RD_KAFKA_ERR_ACTION_PERMANENT, RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION,

            RD_KAFKA_ERR_ACTION_END);
}


/**
 * @brief Handle OffsetCommit response.
 *
 * @remark \p offsets may be NULL if \p err is set
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if all partitions were successfully
 *          committed,
 *          RD_KAFKA_RESP_ERR__IN_PROGRESS if a retry was scheduled,
 *          or any other error code if the request was not retried.
 */
rd_kafka_resp_err_t
rd_kafka_handle_OffsetCommit(rd_kafka_t *rk,
                             rd_kafka_broker_t *rkb,
                             rd_kafka_resp_err_t err,
                             rd_kafka_buf_t *rkbuf,
                             rd_kafka_buf_t *request,
                             rd_kafka_topic_partition_list_t *offsets,
                             rd_bool_t ignore_cgrp) {
        const int log_decode_errors = LOG_ERR;
        int32_t TopicArrayCnt;
        int errcnt  = 0;
        int partcnt = 0;
        int i;
        int actions = 0;

        if (err)
                goto err;

        if (rd_kafka_buf_ApiVersion(rkbuf) >= 3)
                rd_kafka_buf_read_throttle_time(rkbuf);

        rd_kafka_buf_read_i32(rkbuf, &TopicArrayCnt);
        for (i = 0; i < TopicArrayCnt; i++) {
                rd_kafkap_str_t topic;
                char *topic_str;
                int32_t PartArrayCnt;
                int j;

                rd_kafka_buf_read_str(rkbuf, &topic);
                rd_kafka_buf_read_i32(rkbuf, &PartArrayCnt);

                RD_KAFKAP_STR_DUPA(&topic_str, &topic);

                for (j = 0; j < PartArrayCnt; j++) {
                        int32_t partition;
                        int16_t ErrorCode;
                        rd_kafka_topic_partition_t *rktpar;

                        rd_kafka_buf_read_i32(rkbuf, &partition);
                        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

                        rktpar = rd_kafka_topic_partition_list_find(
                            offsets, topic_str, partition);

                        if (!rktpar) {
                                /* Received offset for topic/partition we didn't
                                 * ask for, this shouldn't really happen. */
                                continue;
                        }

                        rktpar->err = ErrorCode;
                        if (ErrorCode) {
                                err = ErrorCode;
                                errcnt++;

                                /* Accumulate actions for per-partition
                                 * errors. */
                                actions |= rd_kafka_handle_OffsetCommit_error(
                                    rkb, request, rktpar);
                        }

                        partcnt++;
                }
        }

        /* If all partitions failed use error code
         * from last partition as the global error. */
        if (offsets && err && errcnt == partcnt)
                goto err;

        goto done;

err_parse:
        err = rkbuf->rkbuf_err;

err:
        if (!actions) /* Transport/Request-level error */
                actions = rd_kafka_err_action(rkb, err, request,

                                              RD_KAFKA_ERR_ACTION_REFRESH |
                                                  RD_KAFKA_ERR_ACTION_SPECIAL |
                                                  RD_KAFKA_ERR_ACTION_RETRY,
                                              RD_KAFKA_RESP_ERR__TRANSPORT,

                                              RD_KAFKA_ERR_ACTION_END);

        if (!ignore_cgrp && (actions & RD_KAFKA_ERR_ACTION_FATAL)) {
                rd_kafka_set_fatal_error(rk, err, "OffsetCommit failed: %s",
                                         rd_kafka_err2str(err));
                return err;
        }

        if (!ignore_cgrp && (actions & RD_KAFKA_ERR_ACTION_REFRESH) &&
            rk->rk_cgrp) {
                /* Mark coordinator dead or re-query for coordinator.
                 * ..dead() will trigger a re-query. */
                if (actions & RD_KAFKA_ERR_ACTION_SPECIAL)
                        rd_kafka_cgrp_coord_dead(rk->rk_cgrp, err,
                                                 "OffsetCommitRequest failed");
                else
                        rd_kafka_cgrp_coord_query(rk->rk_cgrp,
                                                  "OffsetCommitRequest failed");
        }

        if (!ignore_cgrp && actions & RD_KAFKA_ERR_ACTION_RETRY &&
            !(actions & RD_KAFKA_ERR_ACTION_PERMANENT) &&
            rd_kafka_buf_retry(rkb, request))
                return RD_KAFKA_RESP_ERR__IN_PROGRESS;

done:
        return err;
}

/**
 * @brief Send OffsetCommitRequest for a list of partitions.
 *
 * @param cgmetadata consumer group metadata.
 *
 * @param offsets - offsets to commit for each topic-partition.
 *
 * @returns 0 if none of the partitions in \p offsets had valid offsets,
 *          else 1.
 */
int rd_kafka_OffsetCommitRequest(rd_kafka_broker_t *rkb,
                                 rd_kafka_consumer_group_metadata_t *cgmetadata,
                                 rd_kafka_topic_partition_list_t *offsets,
                                 rd_kafka_replyq_t replyq,
                                 rd_kafka_resp_cb_t *resp_cb,
                                 void *opaque,
                                 const char *reason) {
        rd_kafka_buf_t *rkbuf;
        ssize_t of_TopicCnt    = -1;
        int TopicCnt           = 0;
        const char *last_topic = NULL;
        ssize_t of_PartCnt     = -1;
        int PartCnt            = 0;
        int tot_PartCnt        = 0;
        int i;
        int16_t ApiVersion;
        int features;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_OffsetCommit, 0, 7, &features);

        rd_kafka_assert(NULL, offsets != NULL);

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_OffsetCommit, 1,
                                         100 + (offsets->cnt * 128));

        /* ConsumerGroup */
        rd_kafka_buf_write_str(rkbuf, cgmetadata->group_id, -1);

        /* v1,v2 */
        if (ApiVersion >= 1) {
                /* ConsumerGroupGenerationId */
                rd_kafka_buf_write_i32(rkbuf, cgmetadata->generation_id);
                /* ConsumerId */
                rd_kafka_buf_write_str(rkbuf, cgmetadata->member_id, -1);
        }

        /* v7: GroupInstanceId */
        if (ApiVersion >= 7)
                rd_kafka_buf_write_str(rkbuf, cgmetadata->group_instance_id,
                                       -1);

        /* v2-4: RetentionTime */
        if (ApiVersion >= 2 && ApiVersion <= 4)
                rd_kafka_buf_write_i64(rkbuf, -1);

        /* Sort offsets by topic */
        rd_kafka_topic_partition_list_sort_by_topic(offsets);

        /* TopicArrayCnt: Will be updated when we know the number of topics. */
        of_TopicCnt = rd_kafka_buf_write_i32(rkbuf, 0);

        for (i = 0; i < offsets->cnt; i++) {
                rd_kafka_topic_partition_t *rktpar = &offsets->elems[i];

                /* Skip partitions with invalid offset. */
                if (rktpar->offset < 0)
                        continue;

                if (last_topic == NULL || strcmp(last_topic, rktpar->topic)) {
                        /* New topic */

                        /* Finalize previous PartitionCnt */
                        if (PartCnt > 0)
                                rd_kafka_buf_update_u32(rkbuf, of_PartCnt,
                                                        PartCnt);

                        /* TopicName */
                        rd_kafka_buf_write_str(rkbuf, rktpar->topic, -1);
                        /* PartitionCnt, finalized later */
                        of_PartCnt = rd_kafka_buf_write_i32(rkbuf, 0);
                        PartCnt    = 0;
                        last_topic = rktpar->topic;
                        TopicCnt++;
                }

                /* Partition */
                rd_kafka_buf_write_i32(rkbuf, rktpar->partition);
                PartCnt++;
                tot_PartCnt++;

                /* Offset */
                rd_kafka_buf_write_i64(rkbuf, rktpar->offset);

                /* v6: KIP-101 CommittedLeaderEpoch */
                if (ApiVersion >= 6)
                        rd_kafka_buf_write_i32(rkbuf, -1);

                /* v1: TimeStamp */
                if (ApiVersion == 1)
                        rd_kafka_buf_write_i64(rkbuf, -1);

                /* Metadata */
                /* Java client 0.9.0 and broker <0.10.0 can't parse
                 * Null metadata fields, so as a workaround we send an
                 * empty string if it's Null. */
                if (!rktpar->metadata)
                        rd_kafka_buf_write_str(rkbuf, "", 0);
                else
                        rd_kafka_buf_write_str(rkbuf, rktpar->metadata,
                                               rktpar->metadata_size);
        }

        if (tot_PartCnt == 0) {
                /* No topic+partitions had valid offsets to commit. */
                rd_kafka_replyq_destroy(&replyq);
                rd_kafka_buf_destroy(rkbuf);
                return 0;
        }

        /* Finalize previous PartitionCnt */
        if (PartCnt > 0)
                rd_kafka_buf_update_u32(rkbuf, of_PartCnt, PartCnt);

        /* Finalize TopicCnt */
        rd_kafka_buf_update_u32(rkbuf, of_TopicCnt, TopicCnt);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_rkb_dbg(rkb, TOPIC, "OFFSET",
                   "Enqueue OffsetCommitRequest(v%d, %d/%d partition(s))): %s",
                   ApiVersion, tot_PartCnt, offsets->cnt, reason);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return 1;
}

/**
 * @brief Construct and send OffsetDeleteRequest to \p rkb
 *        with the partitions in del_grpoffsets (DeleteConsumerGroupOffsets_t*)
 *        using \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @remark Only one del_grpoffsets element is supported.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_OffsetDeleteRequest(rd_kafka_broker_t *rkb,
                             /** (rd_kafka_DeleteConsumerGroupOffsets_t*) */
                             const rd_list_t *del_grpoffsets,
                             rd_kafka_AdminOptions_t *options,
                             char *errstr,
                             size_t errstr_size,
                             rd_kafka_replyq_t replyq,
                             rd_kafka_resp_cb_t *resp_cb,
                             void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;
        const rd_kafka_DeleteConsumerGroupOffsets_t *grpoffsets =
            rd_list_elem(del_grpoffsets, 0);

        rd_assert(rd_list_cnt(del_grpoffsets) == 1);

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_OffsetDelete, 0, 0, &features);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "OffsetDelete API (KIP-496) not supported "
                            "by broker, requires broker version >= 2.4.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_request(
            rkb, RD_KAFKAP_OffsetDelete, 1,
            2 + strlen(grpoffsets->group) + (64 * grpoffsets->partitions->cnt));

        /* GroupId */
        rd_kafka_buf_write_str(rkbuf, grpoffsets->group, -1);

        rd_kafka_buf_write_topic_partitions(
            rkbuf, grpoffsets->partitions,
            rd_false /*dont skip invalid offsets*/, rd_false /*any offset*/,
            rd_false /*dont write offsets*/, rd_false /*dont write epoch*/,
            rd_false /*dont write metadata*/);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



/**
 * @brief Write "consumer" protocol type MemberState for SyncGroupRequest to
 *        enveloping buffer \p rkbuf.
 */
static void
rd_kafka_group_MemberState_consumer_write(rd_kafka_buf_t *env_rkbuf,
                                          const rd_kafka_group_member_t *rkgm) {
        rd_kafka_buf_t *rkbuf;
        rd_slice_t slice;

        rkbuf = rd_kafka_buf_new(1, 100);
        rd_kafka_buf_write_i16(rkbuf, 0); /* Version */
        rd_assert(rkgm->rkgm_assignment);
        rd_kafka_buf_write_topic_partitions(
            rkbuf, rkgm->rkgm_assignment,
            rd_false /*don't skip invalid offsets*/, rd_false /* any offset */,
            rd_false /*don't write offsets*/, rd_false /*don't write epoch*/,
            rd_false /*don't write metadata*/);
        rd_kafka_buf_write_kbytes(rkbuf, rkgm->rkgm_userdata);

        /* Get pointer to binary buffer */
        rd_slice_init_full(&slice, &rkbuf->rkbuf_buf);

        /* Write binary buffer as Kafka Bytes to enveloping buffer. */
        rd_kafka_buf_write_i32(env_rkbuf, (int32_t)rd_slice_remains(&slice));
        rd_buf_write_slice(&env_rkbuf->rkbuf_buf, &slice);

        rd_kafka_buf_destroy(rkbuf);
}

/**
 * Send SyncGroupRequest
 */
void rd_kafka_SyncGroupRequest(rd_kafka_broker_t *rkb,
                               const rd_kafkap_str_t *group_id,
                               int32_t generation_id,
                               const rd_kafkap_str_t *member_id,
                               const rd_kafkap_str_t *group_instance_id,
                               const rd_kafka_group_member_t *assignments,
                               int assignment_cnt,
                               rd_kafka_replyq_t replyq,
                               rd_kafka_resp_cb_t *resp_cb,
                               void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int i;
        int16_t ApiVersion;
        int features;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_SyncGroup, 0, 3, &features);

        rkbuf = rd_kafka_buf_new_request(
            rkb, RD_KAFKAP_SyncGroup, 1,
            RD_KAFKAP_STR_SIZE(group_id) + 4 /* GenerationId */ +
                RD_KAFKAP_STR_SIZE(member_id) +
                RD_KAFKAP_STR_SIZE(group_instance_id) +
                4 /* array size group_assignment */ +
                (assignment_cnt * 100 /*guess*/));
        rd_kafka_buf_write_kstr(rkbuf, group_id);
        rd_kafka_buf_write_i32(rkbuf, generation_id);
        rd_kafka_buf_write_kstr(rkbuf, member_id);
        if (ApiVersion >= 3)
                rd_kafka_buf_write_kstr(rkbuf, group_instance_id);
        rd_kafka_buf_write_i32(rkbuf, assignment_cnt);

        for (i = 0; i < assignment_cnt; i++) {
                const rd_kafka_group_member_t *rkgm = &assignments[i];

                rd_kafka_buf_write_kstr(rkbuf, rkgm->rkgm_member_id);
                rd_kafka_group_MemberState_consumer_write(rkbuf, rkgm);
        }

        /* This is a blocking request */
        rkbuf->rkbuf_flags |= RD_KAFKA_OP_F_BLOCKING;
        rd_kafka_buf_set_abs_timeout(
            rkbuf,
            rkb->rkb_rk->rk_conf.group_session_timeout_ms +
                3000 /* 3s grace period*/,
            0);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}



/**
 * Send JoinGroupRequest
 */
void rd_kafka_JoinGroupRequest(rd_kafka_broker_t *rkb,
                               const rd_kafkap_str_t *group_id,
                               const rd_kafkap_str_t *member_id,
                               const rd_kafkap_str_t *group_instance_id,
                               const rd_kafkap_str_t *protocol_type,
                               const rd_list_t *topics,
                               rd_kafka_replyq_t replyq,
                               rd_kafka_resp_cb_t *resp_cb,
                               void *opaque) {
        rd_kafka_buf_t *rkbuf;
        rd_kafka_t *rk = rkb->rkb_rk;
        rd_kafka_assignor_t *rkas;
        int i;
        int16_t ApiVersion = 0;
        int features;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_JoinGroup, 0, 5, &features);


        rkbuf = rd_kafka_buf_new_request(
            rkb, RD_KAFKAP_JoinGroup, 1,
            RD_KAFKAP_STR_SIZE(group_id) + 4 /* sessionTimeoutMs */ +
                4 /* rebalanceTimeoutMs */ + RD_KAFKAP_STR_SIZE(member_id) +
                RD_KAFKAP_STR_SIZE(group_instance_id) +
                RD_KAFKAP_STR_SIZE(protocol_type) +
                4 /* array count GroupProtocols */ +
                (rd_list_cnt(topics) * 100));
        rd_kafka_buf_write_kstr(rkbuf, group_id);
        rd_kafka_buf_write_i32(rkbuf, rk->rk_conf.group_session_timeout_ms);
        if (ApiVersion >= 1)
                rd_kafka_buf_write_i32(rkbuf, rk->rk_conf.max_poll_interval_ms);
        rd_kafka_buf_write_kstr(rkbuf, member_id);
        if (ApiVersion >= 5)
                rd_kafka_buf_write_kstr(rkbuf, group_instance_id);
        rd_kafka_buf_write_kstr(rkbuf, protocol_type);
        rd_kafka_buf_write_i32(rkbuf, rk->rk_conf.enabled_assignor_cnt);

        RD_LIST_FOREACH(rkas, &rk->rk_conf.partition_assignors, i) {
                rd_kafkap_bytes_t *member_metadata;
                if (!rkas->rkas_enabled)
                        continue;
                rd_kafka_buf_write_kstr(rkbuf, rkas->rkas_protocol_name);
                member_metadata = rkas->rkas_get_metadata_cb(
                    rkas, rk->rk_cgrp->rkcg_assignor_state, topics,
                    rk->rk_cgrp->rkcg_group_assignment);
                rd_kafka_buf_write_kbytes(rkbuf, member_metadata);
                rd_kafkap_bytes_destroy(member_metadata);
        }

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        if (ApiVersion < 1 &&
            rk->rk_conf.max_poll_interval_ms >
                rk->rk_conf.group_session_timeout_ms &&
            rd_interval(&rkb->rkb_suppress.unsupported_kip62,
                        /* at most once per day */
                        (rd_ts_t)86400 * 1000 * 1000, 0) > 0)
                rd_rkb_log(rkb, LOG_NOTICE, "MAXPOLL",
                           "Broker does not support KIP-62 "
                           "(requires Apache Kafka >= v0.10.1.0): "
                           "consumer configuration "
                           "`max.poll.interval.ms` (%d) "
                           "is effectively limited "
                           "by `session.timeout.ms` (%d) "
                           "with this broker version",
                           rk->rk_conf.max_poll_interval_ms,
                           rk->rk_conf.group_session_timeout_ms);


        if (ApiVersion < 5 && rk->rk_conf.group_instance_id &&
            rd_interval(&rkb->rkb_suppress.unsupported_kip345,
                        /* at most once per day */
                        (rd_ts_t)86400 * 1000 * 1000, 0) > 0)
                rd_rkb_log(rkb, LOG_NOTICE, "STATICMEMBER",
                           "Broker does not support KIP-345 "
                           "(requires Apache Kafka >= v2.3.0): "
                           "consumer configuration "
                           "`group.instance.id` (%s) "
                           "will not take effect",
                           rk->rk_conf.group_instance_id);

        /* Absolute timeout */
        rd_kafka_buf_set_abs_timeout_force(
            rkbuf,
            /* Request timeout is max.poll.interval.ms + grace
             * if the broker supports it, else
             * session.timeout.ms + grace. */
            (ApiVersion >= 1 ? rk->rk_conf.max_poll_interval_ms
                             : rk->rk_conf.group_session_timeout_ms) +
                3000 /* 3s grace period*/,
            0);

        /* This is a blocking request */
        rkbuf->rkbuf_flags |= RD_KAFKA_OP_F_BLOCKING;

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}



/**
 * Send LeaveGroupRequest
 */
void rd_kafka_LeaveGroupRequest(rd_kafka_broker_t *rkb,
                                const char *group_id,
                                const char *member_id,
                                rd_kafka_replyq_t replyq,
                                rd_kafka_resp_cb_t *resp_cb,
                                void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_LeaveGroup, 0, 1, &features);

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_LeaveGroup, 1, 300);

        rd_kafka_buf_write_str(rkbuf, group_id, -1);
        rd_kafka_buf_write_str(rkbuf, member_id, -1);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        /* LeaveGroupRequests are best-effort, the local consumer
         * does not care if it succeeds or not, so the request timeout
         * is shortened.
         * Retries are not needed. */
        rd_kafka_buf_set_abs_timeout(rkbuf, 5000, 0);
        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_NO_RETRIES;

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}


/**
 * Handler for LeaveGroup responses
 * opaque must be the cgrp handle.
 */
void rd_kafka_handle_LeaveGroup(rd_kafka_t *rk,
                                rd_kafka_broker_t *rkb,
                                rd_kafka_resp_err_t err,
                                rd_kafka_buf_t *rkbuf,
                                rd_kafka_buf_t *request,
                                void *opaque) {
        rd_kafka_cgrp_t *rkcg       = opaque;
        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode           = 0;
        int actions;

        if (err) {
                ErrorCode = err;
                goto err;
        }

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

err:
        actions = rd_kafka_err_action(rkb, ErrorCode, request,
                                      RD_KAFKA_ERR_ACTION_END);

        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                /* Re-query for coordinator */
                rd_kafka_cgrp_op(rkcg, NULL, RD_KAFKA_NO_REPLYQ,
                                 RD_KAFKA_OP_COORD_QUERY, ErrorCode);
        }

        if (actions & RD_KAFKA_ERR_ACTION_RETRY) {
                if (rd_kafka_buf_retry(rkb, request))
                        return;
                /* FALLTHRU */
        }

        if (ErrorCode)
                rd_kafka_dbg(rkb->rkb_rk, CGRP, "LEAVEGROUP",
                             "LeaveGroup response: %s",
                             rd_kafka_err2str(ErrorCode));

        return;

err_parse:
        ErrorCode = rkbuf->rkbuf_err;
        goto err;
}



/**
 * Send HeartbeatRequest
 */
void rd_kafka_HeartbeatRequest(rd_kafka_broker_t *rkb,
                               const rd_kafkap_str_t *group_id,
                               int32_t generation_id,
                               const rd_kafkap_str_t *member_id,
                               const rd_kafkap_str_t *group_instance_id,
                               rd_kafka_replyq_t replyq,
                               rd_kafka_resp_cb_t *resp_cb,
                               void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_Heartbeat, 0, 3, &features);

        rd_rkb_dbg(rkb, CGRP, "HEARTBEAT",
                   "Heartbeat for group \"%s\" generation id %" PRId32,
                   group_id->str, generation_id);

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_Heartbeat, 1,
                                         RD_KAFKAP_STR_SIZE(group_id) +
                                             4 /* GenerationId */ +
                                             RD_KAFKAP_STR_SIZE(member_id));

        rd_kafka_buf_write_kstr(rkbuf, group_id);
        rd_kafka_buf_write_i32(rkbuf, generation_id);
        rd_kafka_buf_write_kstr(rkbuf, member_id);
        if (ApiVersion >= 3)
                rd_kafka_buf_write_kstr(rkbuf, group_instance_id);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_buf_set_abs_timeout(
            rkbuf, rkb->rkb_rk->rk_conf.group_session_timeout_ms, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}



/**
 * @brief Construct and send ListGroupsRequest to \p rkb
 *        with the states (const char *) in \p states.
 *        Uses \p max_ApiVersion as maximum API version,
 *        pass -1 to use the maximum available version.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @return NULL on success, a new error instance that must be
 *         released with rd_kafka_error_destroy() in case of error.
 */
rd_kafka_error_t *rd_kafka_ListGroupsRequest(rd_kafka_broker_t *rkb,
                                             int16_t max_ApiVersion,
                                             const char **states,
                                             size_t states_cnt,
                                             rd_kafka_replyq_t replyq,
                                             rd_kafka_resp_cb_t *resp_cb,
                                             void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        size_t i;
        rd_bool_t is_flexver = rd_false;

        if (max_ApiVersion < 0)
                max_ApiVersion = 4;

        if (max_ApiVersion > ApiVersion) {
                /* Remark: don't check if max_ApiVersion is zero.
                 * As rd_kafka_broker_ApiVersion_supported cannot be checked
                 * in the application thread reliably . */
                ApiVersion = rd_kafka_broker_ApiVersion_supported(
                    rkb, RD_KAFKAP_ListGroups, 0, max_ApiVersion, NULL);
                is_flexver = ApiVersion >= 3;
        }

        if (ApiVersion == -1) {
                return rd_kafka_error_new(
                    RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE,
                    "ListGroupsRequest not supported by broker");
        }

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_ListGroups, 1,
            /* rd_kafka_buf_write_arraycnt_pos + tags + StatesFilter */
            4 + 1 + 32 * states_cnt, is_flexver);

        if (ApiVersion >= 4) {
                size_t of_GroupsArrayCnt =
                    rd_kafka_buf_write_arraycnt_pos(rkbuf);
                for (i = 0; i < states_cnt; i++) {
                        rd_kafka_buf_write_str(rkbuf, states[i], -1);
                }
                rd_kafka_buf_finalize_arraycnt(rkbuf, of_GroupsArrayCnt, i);
        }
        if (is_flexver) {
                rd_kafka_buf_write_tags(rkbuf);
        }

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);
        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
        return NULL;
}

/**
 * @brief Construct and send DescribeGroupsRequest to \p rkb
 *        with the groups (const char *) in \p groups.
 *        Uses \p max_ApiVersion as maximum API version,
 *        pass -1 to use the maximum available version.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @return NULL on success, a new error instance that must be
 *         released with rd_kafka_error_destroy() in case of error.
 */
rd_kafka_error_t *rd_kafka_DescribeGroupsRequest(rd_kafka_broker_t *rkb,
                                                 int16_t max_ApiVersion,
                                                 char **groups,
                                                 size_t group_cnt,
                                                 rd_kafka_replyq_t replyq,
                                                 rd_kafka_resp_cb_t *resp_cb,
                                                 void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        size_t of_GroupsArrayCnt;

        if (max_ApiVersion < 0)
                max_ApiVersion = 4;

        if (max_ApiVersion > ApiVersion) {
                /* Remark: don't check if max_ApiVersion is zero.
                 * As rd_kafka_broker_ApiVersion_supported cannot be checked
                 * in the application thread reliably . */
                ApiVersion = rd_kafka_broker_ApiVersion_supported(
                    rkb, RD_KAFKAP_DescribeGroups, 0, max_ApiVersion, NULL);
        }

        if (ApiVersion == -1) {
                return rd_kafka_error_new(
                    RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE,
                    "DescribeGroupsRequest not supported by broker");
        }

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_DescribeGroups, 1,
            4 /* rd_kafka_buf_write_arraycnt_pos */ +
                1 /* IncludeAuthorizedOperations */ + 1 /* tags */ +
                32 * group_cnt /* Groups */,
            rd_false);

        /* write Groups */
        of_GroupsArrayCnt = rd_kafka_buf_write_arraycnt_pos(rkbuf);
        rd_kafka_buf_finalize_arraycnt(rkbuf, of_GroupsArrayCnt, group_cnt);
        while (group_cnt-- > 0)
                rd_kafka_buf_write_str(rkbuf, groups[group_cnt], -1);

        /* write IncludeAuthorizedOperations */
        if (ApiVersion >= 3) {
                /* TODO: implement KIP-430 */
                rd_kafka_buf_write_bool(rkbuf, rd_false);
        }

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);
        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
        return NULL;
}

/**
 * @brief Generic handler for Metadata responses
 *
 * @locality rdkafka main thread
 */
static void rd_kafka_handle_Metadata(rd_kafka_t *rk,
                                     rd_kafka_broker_t *rkb,
                                     rd_kafka_resp_err_t err,
                                     rd_kafka_buf_t *rkbuf,
                                     rd_kafka_buf_t *request,
                                     void *opaque) {
        rd_kafka_op_t *rko           = opaque; /* Possibly NULL */
        struct rd_kafka_metadata *md = NULL;
        const rd_list_t *topics      = request->rkbuf_u.Metadata.topics;
        int actions;

        rd_kafka_assert(NULL, err == RD_KAFKA_RESP_ERR__DESTROY ||
                                  thrd_is_current(rk->rk_thread));

        /* Avoid metadata updates when we're terminating. */
        if (rd_kafka_terminating(rkb->rkb_rk) ||
            err == RD_KAFKA_RESP_ERR__DESTROY) {
                /* Terminating */
                goto done;
        }

        if (err)
                goto err;

        if (!topics)
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "===== Received metadata: %s =====",
                           request->rkbuf_u.Metadata.reason);
        else
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "===== Received metadata "
                           "(for %d requested topics): %s =====",
                           rd_list_cnt(topics),
                           request->rkbuf_u.Metadata.reason);

        err = rd_kafka_parse_Metadata(rkb, request, rkbuf, &md);
        if (err)
                goto err;

        if (rko && rko->rko_replyq.q) {
                /* Reply to metadata requester, passing on the metadata.
                 * Reuse requesting rko for the reply. */
                rko->rko_err           = err;
                rko->rko_u.metadata.md = md;

                rd_kafka_replyq_enq(&rko->rko_replyq, rko, 0);
                rko = NULL;
        } else {
                if (md)
                        rd_free(md);
        }

        goto done;

err:
        actions = rd_kafka_err_action(rkb, err, request,

                                      RD_KAFKA_ERR_ACTION_RETRY,
                                      RD_KAFKA_RESP_ERR__PARTIAL,

                                      RD_KAFKA_ERR_ACTION_END);

        if (actions & RD_KAFKA_ERR_ACTION_RETRY) {
                if (rd_kafka_buf_retry(rkb, request))
                        return;
                /* FALLTHRU */
        } else {
                rd_rkb_log(rkb, LOG_WARNING, "METADATA",
                           "Metadata request failed: %s: %s (%dms): %s",
                           request->rkbuf_u.Metadata.reason,
                           rd_kafka_err2str(err),
                           (int)(request->rkbuf_ts_sent / 1000),
                           rd_kafka_actions2str(actions));
                /* Respond back to caller on non-retriable errors */
                if (rko && rko->rko_replyq.q) {
                        rko->rko_err           = err;
                        rko->rko_u.metadata.md = NULL;
                        rd_kafka_replyq_enq(&rko->rko_replyq, rko, 0);
                        rko = NULL;
                }
        }



        /* FALLTHRU */

done:
        if (rko)
                rd_kafka_op_destroy(rko);
}


/**
 * @brief Construct MetadataRequest (does not send)
 *
 * \p topics is a list of topic names (char *) to request.
 *
 * !topics          - only request brokers (if supported by broker, else
 *                    all topics)
 *  topics.cnt==0   - all topics in cluster are requested
 *  topics.cnt >0   - only specified topics are requested
 *
 * @param reason    - metadata request reason
 * @param allow_auto_create_topics - allow broker-side auto topic creation.
 *                                   This is best-effort, depending on broker
 *                                   config and version.
 * @param cgrp_update - Update cgrp in parse_Metadata (see comment there).
 * @param rko       - (optional) rko with replyq for handling response.
 *                    Specifying an rko forces a metadata request even if
 *                    there is already a matching one in-transit.
 *
 * If full metadata for all topics is requested (or all brokers, which
 * results in all-topics on older brokers) and there is already a full request
 * in transit then this function will return RD_KAFKA_RESP_ERR__PREV_IN_PROGRESS
 * otherwise RD_KAFKA_RESP_ERR_NO_ERROR. If \p rko is non-NULL the request
 * is sent regardless.
 */
rd_kafka_resp_err_t rd_kafka_MetadataRequest(rd_kafka_broker_t *rkb,
                                             const rd_list_t *topics,
                                             const char *reason,
                                             rd_bool_t allow_auto_create_topics,
                                             rd_bool_t cgrp_update,
                                             rd_kafka_op_t *rko) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;
        int topic_cnt  = topics ? rd_list_cnt(topics) : 0;
        int *full_incr = NULL;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_Metadata, 0, 4, &features);

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_Metadata, 1,
                                         4 + (50 * topic_cnt) + 1);

        if (!reason)
                reason = "";

        rkbuf->rkbuf_u.Metadata.reason      = rd_strdup(reason);
        rkbuf->rkbuf_u.Metadata.cgrp_update = cgrp_update;

        if (!topics && ApiVersion >= 1) {
                /* a null(0) array (in the protocol) represents no topics */
                rd_kafka_buf_write_i32(rkbuf, 0);
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "Request metadata for brokers only: %s", reason);
                full_incr =
                    &rkb->rkb_rk->rk_metadata_cache.rkmc_full_brokers_sent;

        } else {
                if (topic_cnt == 0 && !rko)
                        full_incr = &rkb->rkb_rk->rk_metadata_cache
                                         .rkmc_full_topics_sent;

                if (topic_cnt == 0 && ApiVersion >= 1)
                        rd_kafka_buf_write_i32(rkbuf, -1); /* Null: all topics*/
                else
                        rd_kafka_buf_write_i32(rkbuf, topic_cnt);

                if (topic_cnt == 0) {
                        rkbuf->rkbuf_u.Metadata.all_topics = 1;
                        rd_rkb_dbg(rkb, METADATA, "METADATA",
                                   "Request metadata for all topics: "
                                   "%s",
                                   reason);
                } else
                        rd_rkb_dbg(rkb, METADATA, "METADATA",
                                   "Request metadata for %d topic(s): "
                                   "%s",
                                   topic_cnt, reason);
        }

        if (full_incr) {
                /* Avoid multiple outstanding full requests
                 * (since they are redundant and side-effect-less).
                 * Forced requests (app using metadata() API) are passed
                 * through regardless. */

                mtx_lock(&rkb->rkb_rk->rk_metadata_cache.rkmc_full_lock);
                if (*full_incr > 0 && (!rko || !rko->rko_u.metadata.force)) {
                        mtx_unlock(
                            &rkb->rkb_rk->rk_metadata_cache.rkmc_full_lock);
                        rd_rkb_dbg(rkb, METADATA, "METADATA",
                                   "Skipping metadata request: %s: "
                                   "full request already in-transit",
                                   reason);
                        rd_kafka_buf_destroy(rkbuf);
                        return RD_KAFKA_RESP_ERR__PREV_IN_PROGRESS;
                }

                (*full_incr)++;
                mtx_unlock(&rkb->rkb_rk->rk_metadata_cache.rkmc_full_lock);
                rkbuf->rkbuf_u.Metadata.decr = full_incr;
                rkbuf->rkbuf_u.Metadata.decr_lock =
                    &rkb->rkb_rk->rk_metadata_cache.rkmc_full_lock;
        }


        if (topic_cnt > 0) {
                char *topic;
                int i;

                /* Maintain a copy of the topics list so we can purge
                 * hints from the metadata cache on error. */
                rkbuf->rkbuf_u.Metadata.topics =
                    rd_list_copy(topics, rd_list_string_copy, NULL);

                RD_LIST_FOREACH(topic, topics, i)
                rd_kafka_buf_write_str(rkbuf, topic, -1);
        }

        if (ApiVersion >= 4) {
                /* AllowAutoTopicCreation */
                rd_kafka_buf_write_bool(rkbuf, allow_auto_create_topics);

        } else if (rkb->rkb_rk->rk_type == RD_KAFKA_CONSUMER &&
                   !rkb->rkb_rk->rk_conf.allow_auto_create_topics &&
                   rd_kafka_conf_is_modified(&rkb->rkb_rk->rk_conf,
                                             "allow.auto.create.topics") &&
                   rd_interval(
                       &rkb->rkb_rk->rk_suppress.allow_auto_create_topics,
                       30 * 60 * 1000 /* every 30 minutes */, 0) >= 0) {
                /* Let user know we can't obey allow.auto.create.topics */
                rd_rkb_log(rkb, LOG_WARNING, "AUTOCREATE",
                           "allow.auto.create.topics=false not supported "
                           "by broker: requires broker version >= 0.11.0.0: "
                           "requested topic(s) may be auto created depending "
                           "on broker auto.create.topics.enable configuration");
        }


        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        /* Metadata requests are part of the important control plane
         * and should go before most other requests (Produce, Fetch, etc). */
        rkbuf->rkbuf_prio = RD_KAFKA_PRIO_HIGH;

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf,
                                       /* Handle response thru rk_ops,
                                        * but forward parsed result to
                                        * rko's replyq when done. */
                                       RD_KAFKA_REPLYQ(rkb->rkb_rk->rk_ops, 0),
                                       rd_kafka_handle_Metadata, rko);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



/**
 * @brief Parses and handles ApiVersion reply.
 *
 * @param apis will be allocated, populated and sorted
 *             with broker's supported APIs, or set to NULL.
 * @param api_cnt will be set to the number of elements in \p *apis
 *
 * @returns 0 on success, else an error.
 *
 * @remark A valid \p apis might be returned even if an error is returned.
 */
rd_kafka_resp_err_t
rd_kafka_handle_ApiVersion(rd_kafka_t *rk,
                           rd_kafka_broker_t *rkb,
                           rd_kafka_resp_err_t err,
                           rd_kafka_buf_t *rkbuf,
                           rd_kafka_buf_t *request,
                           struct rd_kafka_ApiVersion **apis,
                           size_t *api_cnt) {
        const int log_decode_errors = LOG_DEBUG;
        int32_t ApiArrayCnt;
        int16_t ErrorCode;
        int i = 0;

        *apis    = NULL;
        *api_cnt = 0;

        if (err)
                goto err;

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);
        err = ErrorCode;

        rd_kafka_buf_read_arraycnt(rkbuf, &ApiArrayCnt, 1000);
        if (err && ApiArrayCnt < 1) {
                /* Version >=3 returns the ApiVersions array if the error
                 * code is ERR_UNSUPPORTED_VERSION, previous versions don't */
                goto err;
        }

        rd_rkb_dbg(rkb, FEATURE, "APIVERSION", "Broker API support:");

        *apis = rd_malloc(sizeof(**apis) * ApiArrayCnt);

        for (i = 0; i < ApiArrayCnt; i++) {
                struct rd_kafka_ApiVersion *api = &(*apis)[i];

                rd_kafka_buf_read_i16(rkbuf, &api->ApiKey);
                rd_kafka_buf_read_i16(rkbuf, &api->MinVer);
                rd_kafka_buf_read_i16(rkbuf, &api->MaxVer);

                rd_rkb_dbg(rkb, FEATURE, "APIVERSION",
                           "  ApiKey %s (%hd) Versions %hd..%hd",
                           rd_kafka_ApiKey2str(api->ApiKey), api->ApiKey,
                           api->MinVer, api->MaxVer);

                /* Discard struct tags */
                rd_kafka_buf_skip_tags(rkbuf);
        }

        if (request->rkbuf_reqhdr.ApiVersion >= 1)
                rd_kafka_buf_read_throttle_time(rkbuf);

        /* Discard end tags */
        rd_kafka_buf_skip_tags(rkbuf);

        *api_cnt = ApiArrayCnt;
        qsort(*apis, *api_cnt, sizeof(**apis), rd_kafka_ApiVersion_key_cmp);

        goto done;

err_parse:
        /* If the broker does not support our ApiVersionRequest version it
         * will respond with a version 0 response, which will most likely
         * fail parsing. Instead of propagating the parse error we
         * propagate the original error, unless there isn't one in which case
         * we use the parse error. */
        if (!err)
                err = rkbuf->rkbuf_err;
err:
        /* There are no retryable errors. */

        if (*apis)
                rd_free(*apis);

        *apis    = NULL;
        *api_cnt = 0;

done:
        return err;
}



/**
 * @brief Send ApiVersionRequest (KIP-35)
 *
 * @param ApiVersion If -1 use the highest supported version, else use the
 *                   specified value.
 */
void rd_kafka_ApiVersionRequest(rd_kafka_broker_t *rkb,
                                int16_t ApiVersion,
                                rd_kafka_replyq_t replyq,
                                rd_kafka_resp_cb_t *resp_cb,
                                void *opaque) {
        rd_kafka_buf_t *rkbuf;

        if (ApiVersion == -1)
                ApiVersion = 3;

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_ApiVersion, 1, 4, ApiVersion >= 3 /*flexver*/);

        if (ApiVersion >= 3) {
                /* KIP-511 adds software name and version through the optional
                 * protocol fields defined in KIP-482. */

                /* ClientSoftwareName */
                rd_kafka_buf_write_str(rkbuf, rkb->rkb_rk->rk_conf.sw_name, -1);

                /* ClientSoftwareVersion */
                rd_kafka_buf_write_str(rkbuf, rkb->rkb_rk->rk_conf.sw_version,
                                       -1);
        }

        /* Should be sent before any other requests since it is part of
         * the initial connection handshake. */
        rkbuf->rkbuf_prio = RD_KAFKA_PRIO_FLASH;

        /* Non-supporting brokers will tear down the connection when they
         * receive an unknown API request, so dont retry request on failure. */
        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_NO_RETRIES;

        /* 0.9.0.x brokers will not close the connection on unsupported
         * API requests, so we minimize the timeout for the request.
         * This is a regression on the broker part. */
        rd_kafka_buf_set_abs_timeout(
            rkbuf, rkb->rkb_rk->rk_conf.api_version_request_timeout_ms, 0);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        if (replyq.q)
                rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb,
                                               opaque);
        else /* in broker thread */
                rd_kafka_broker_buf_enq1(rkb, rkbuf, resp_cb, opaque);
}


/**
 * Send SaslHandshakeRequest (KIP-43)
 */
void rd_kafka_SaslHandshakeRequest(rd_kafka_broker_t *rkb,
                                   const char *mechanism,
                                   rd_kafka_replyq_t replyq,
                                   rd_kafka_resp_cb_t *resp_cb,
                                   void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int mechlen = (int)strlen(mechanism);
        int16_t ApiVersion;
        int features;

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_SaslHandshake, 1,
                                         RD_KAFKAP_STR_SIZE0(mechlen));

        /* Should be sent before any other requests since it is part of
         * the initial connection handshake. */
        rkbuf->rkbuf_prio = RD_KAFKA_PRIO_FLASH;

        rd_kafka_buf_write_str(rkbuf, mechanism, mechlen);

        /* Non-supporting brokers will tear down the conneciton when they
         * receive an unknown API request or where the SASL GSSAPI
         * token type is not recognized, so dont retry request on failure. */
        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_NO_RETRIES;

        /* 0.9.0.x brokers will not close the connection on unsupported
         * API requests, so we minimize the timeout of the request.
         * This is a regression on the broker part. */
        if (!rkb->rkb_rk->rk_conf.api_version_request &&
            rkb->rkb_rk->rk_conf.socket_timeout_ms > 10 * 1000)
                rd_kafka_buf_set_abs_timeout(rkbuf, 10 * 1000 /*10s*/, 0);

        /* ApiVersion 1 / RD_KAFKA_FEATURE_SASL_REQ enables
         * the SaslAuthenticateRequest */
        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_SaslHandshake, 0, 1, &features);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        if (replyq.q)
                rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb,
                                               opaque);
        else /* in broker thread */
                rd_kafka_broker_buf_enq1(rkb, rkbuf, resp_cb, opaque);
}


/**
 * @brief Parses and handles an SaslAuthenticate reply.
 *
 * @returns 0 on success, else an error.
 *
 * @locality broker thread
 * @locks none
 */
void rd_kafka_handle_SaslAuthenticate(rd_kafka_t *rk,
                                      rd_kafka_broker_t *rkb,
                                      rd_kafka_resp_err_t err,
                                      rd_kafka_buf_t *rkbuf,
                                      rd_kafka_buf_t *request,
                                      void *opaque) {
        const int log_decode_errors = LOG_ERR;
        int16_t error_code;
        rd_kafkap_str_t error_str;
        rd_kafkap_bytes_t auth_data;
        char errstr[512];

        if (err) {
                rd_snprintf(errstr, sizeof(errstr),
                            "SaslAuthenticateRequest failed: %s",
                            rd_kafka_err2str(err));
                goto err;
        }

        rd_kafka_buf_read_i16(rkbuf, &error_code);
        rd_kafka_buf_read_str(rkbuf, &error_str);

        if (error_code) {
                /* Authentication failed */

                /* For backwards compatibility translate the
                 * new broker-side auth error code to our local error code. */
                if (error_code == RD_KAFKA_RESP_ERR_SASL_AUTHENTICATION_FAILED)
                        err = RD_KAFKA_RESP_ERR__AUTHENTICATION;
                else
                        err = error_code;

                rd_snprintf(errstr, sizeof(errstr), "%.*s",
                            RD_KAFKAP_STR_PR(&error_str));
                goto err;
        }

        rd_kafka_buf_read_bytes(rkbuf, &auth_data);

        /* Pass SASL auth frame to SASL handler */
        if (rd_kafka_sasl_recv(rkb->rkb_transport, auth_data.data,
                               (size_t)RD_KAFKAP_BYTES_LEN(&auth_data), errstr,
                               sizeof(errstr)) == -1) {
                err = RD_KAFKA_RESP_ERR__AUTHENTICATION;
                goto err;
        }

        return;


err_parse:
        err = rkbuf->rkbuf_err;
        rd_snprintf(errstr, sizeof(errstr),
                    "SaslAuthenticateResponse parsing failed: %s",
                    rd_kafka_err2str(err));

err:
        rd_kafka_broker_fail(rkb, LOG_ERR, err, "SASL authentication error: %s",
                             errstr);
}


/**
 * @brief Send SaslAuthenticateRequest (KIP-152)
 */
void rd_kafka_SaslAuthenticateRequest(rd_kafka_broker_t *rkb,
                                      const void *buf,
                                      size_t size,
                                      rd_kafka_replyq_t replyq,
                                      rd_kafka_resp_cb_t *resp_cb,
                                      void *opaque) {
        rd_kafka_buf_t *rkbuf;

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_SaslAuthenticate, 0, 0);

        /* Should be sent before any other requests since it is part of
         * the initial connection handshake. */
        rkbuf->rkbuf_prio = RD_KAFKA_PRIO_FLASH;

        /* Broker does not support -1 (Null) for this field */
        rd_kafka_buf_write_bytes(rkbuf, buf ? buf : "", size);

        /* There are no errors that can be retried, instead
         * close down the connection and reconnect on failure. */
        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_NO_RETRIES;

        if (replyq.q)
                rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb,
                                               opaque);
        else /* in broker thread */
                rd_kafka_broker_buf_enq1(rkb, rkbuf, resp_cb, opaque);
}



/**
 * @struct Hold temporary result and return values from ProduceResponse
 */
struct rd_kafka_Produce_result {
        int64_t offset;    /**< Assigned offset of first message */
        int64_t timestamp; /**< (Possibly assigned) offset of first message */
};

/**
 * @brief Parses a Produce reply.
 * @returns 0 on success or an error code on failure.
 * @locality broker thread
 */
static rd_kafka_resp_err_t
rd_kafka_handle_Produce_parse(rd_kafka_broker_t *rkb,
                              rd_kafka_toppar_t *rktp,
                              rd_kafka_buf_t *rkbuf,
                              rd_kafka_buf_t *request,
                              struct rd_kafka_Produce_result *result) {
        int32_t TopicArrayCnt;
        int32_t PartitionArrayCnt;
        struct {
                int32_t Partition;
                int16_t ErrorCode;
                int64_t Offset;
        } hdr;
        const int log_decode_errors = LOG_ERR;
        int64_t log_start_offset    = -1;

        rd_kafka_buf_read_i32(rkbuf, &TopicArrayCnt);
        if (TopicArrayCnt != 1)
                goto err;

        /* Since we only produce to one single topic+partition in each
         * request we assume that the reply only contains one topic+partition
         * and that it is the same that we requested.
         * If not the broker is buggy. */
        rd_kafka_buf_skip_str(rkbuf);
        rd_kafka_buf_read_i32(rkbuf, &PartitionArrayCnt);

        if (PartitionArrayCnt != 1)
                goto err;

        rd_kafka_buf_read_i32(rkbuf, &hdr.Partition);
        rd_kafka_buf_read_i16(rkbuf, &hdr.ErrorCode);
        rd_kafka_buf_read_i64(rkbuf, &hdr.Offset);

        result->offset = hdr.Offset;

        result->timestamp = -1;
        if (request->rkbuf_reqhdr.ApiVersion >= 2)
                rd_kafka_buf_read_i64(rkbuf, &result->timestamp);

        if (request->rkbuf_reqhdr.ApiVersion >= 5)
                rd_kafka_buf_read_i64(rkbuf, &log_start_offset);

        if (request->rkbuf_reqhdr.ApiVersion >= 1) {
                int32_t Throttle_Time;
                rd_kafka_buf_read_i32(rkbuf, &Throttle_Time);

                rd_kafka_op_throttle_time(rkb, rkb->rkb_rk->rk_rep,
                                          Throttle_Time);
        }


        return hdr.ErrorCode;

err_parse:
        return rkbuf->rkbuf_err;
err:
        return RD_KAFKA_RESP_ERR__BAD_MSG;
}


/**
 * @struct Hold temporary Produce error state
 */
struct rd_kafka_Produce_err {
        rd_kafka_resp_err_t err;      /**< Error code */
        int actions;                  /**< Actions to take */
        int incr_retry;               /**< Increase per-message retry cnt */
        rd_kafka_msg_status_t status; /**< Messages persistence status */

        /* Idempotent Producer */
        int32_t next_ack_seq;      /**< Next expected sequence to ack */
        int32_t next_err_seq;      /**< Next expected error sequence */
        rd_bool_t update_next_ack; /**< Update next_ack_seq */
        rd_bool_t update_next_err; /**< Update next_err_seq */
        rd_kafka_pid_t rktp_pid;   /**< Partition's current PID */
        int32_t last_seq;          /**< Last sequence in current batch */
};


/**
 * @brief Error-handling for Idempotent Producer-specific Produce errors.
 *
 * May update \p errp, \p actionsp and \p incr_retryp.
 *
 * The resulting \p actionsp are handled by the caller.
 *
 * @warning May be called on the old leader thread. Lock rktp appropriately!
 *
 * @locality broker thread (but not necessarily the leader broker)
 * @locks none
 */
static void
rd_kafka_handle_idempotent_Produce_error(rd_kafka_broker_t *rkb,
                                         rd_kafka_msgbatch_t *batch,
                                         struct rd_kafka_Produce_err *perr) {
        rd_kafka_t *rk          = rkb->rkb_rk;
        rd_kafka_toppar_t *rktp = batch->rktp;
        rd_kafka_msg_t *firstmsg, *lastmsg;
        int r;
        rd_ts_t now = rd_clock(), state_age;
        struct rd_kafka_toppar_err last_err;

        rd_kafka_rdlock(rkb->rkb_rk);
        state_age = now - rkb->rkb_rk->rk_eos.ts_idemp_state;
        rd_kafka_rdunlock(rkb->rkb_rk);

        firstmsg = rd_kafka_msgq_first(&batch->msgq);
        lastmsg  = rd_kafka_msgq_last(&batch->msgq);
        rd_assert(firstmsg && lastmsg);

        /* Store the last msgid of the batch
         * on the first message in case we need to retry
         * and thus reconstruct the entire batch. */
        if (firstmsg->rkm_u.producer.last_msgid) {
                /* last_msgid already set, make sure it
                 * actually points to the last message. */
                rd_assert(firstmsg->rkm_u.producer.last_msgid ==
                          lastmsg->rkm_u.producer.msgid);
        } else {
                firstmsg->rkm_u.producer.last_msgid =
                    lastmsg->rkm_u.producer.msgid;
        }

        if (!rd_kafka_pid_eq(batch->pid, perr->rktp_pid)) {
                /* Don't retry if PID changed since we can't
                 * guarantee correctness across PID sessions. */
                perr->actions = RD_KAFKA_ERR_ACTION_PERMANENT;
                perr->status  = RD_KAFKA_MSG_STATUS_POSSIBLY_PERSISTED;

                rd_rkb_dbg(rkb, MSG | RD_KAFKA_DBG_EOS, "ERRPID",
                           "%.*s [%" PRId32
                           "] PID mismatch: "
                           "request %s != partition %s: "
                           "failing messages with error %s",
                           RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                           rktp->rktp_partition, rd_kafka_pid2str(batch->pid),
                           rd_kafka_pid2str(perr->rktp_pid),
                           rd_kafka_err2str(perr->err));
                return;
        }

        /*
         * Special error handling
         */
        switch (perr->err) {
        case RD_KAFKA_RESP_ERR_OUT_OF_ORDER_SEQUENCE_NUMBER:
                /* Compare request's sequence to expected next
                 * acked sequence.
                 *
                 * Example requests in flight:
                 *   R1(base_seq:5) R2(10) R3(15) R4(20)
                 */

                /* Acquire the last partition error to help
                 * troubleshoot this problem. */
                rd_kafka_toppar_lock(rktp);
                last_err = rktp->rktp_last_err;
                rd_kafka_toppar_unlock(rktp);

                r = batch->first_seq - perr->next_ack_seq;

                if (r == 0) {
                        /* R1 failed:
                         * If this was the head-of-line request in-flight it
                         * means there is a state desynchronization between the
                         * producer and broker (a bug), in which case
                         * we'll raise a fatal error since we can no longer
                         * reason about the state of messages and thus
                         * not guarantee ordering or once-ness for R1,
                         * nor give the user a chance to opt out of sending
                         * R2 to R4 which would be retried automatically. */

                        rd_kafka_idemp_set_fatal_error(
                            rk, perr->err,
                            "ProduceRequest for %.*s [%" PRId32
                            "] "
                            "with %d message(s) failed "
                            "due to sequence desynchronization with "
                            "broker %" PRId32 " (%s, base seq %" PRId32
                            ", "
                            "idemp state change %" PRId64
                            "ms ago, "
                            "last partition error %s (actions %s, "
                            "base seq %" PRId32 "..%" PRId32
                            ", base msgid %" PRIu64 ", %" PRId64 "ms ago)",
                            RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                            rktp->rktp_partition,
                            rd_kafka_msgq_len(&batch->msgq), rkb->rkb_nodeid,
                            rd_kafka_pid2str(batch->pid), batch->first_seq,
                            state_age / 1000, rd_kafka_err2name(last_err.err),
                            rd_kafka_actions2str(last_err.actions),
                            last_err.base_seq, last_err.last_seq,
                            last_err.base_msgid,
                            last_err.ts ? (now - last_err.ts) / 1000 : -1);

                        perr->actions = RD_KAFKA_ERR_ACTION_PERMANENT;
                        perr->status  = RD_KAFKA_MSG_STATUS_POSSIBLY_PERSISTED;
                        perr->update_next_ack = rd_false;
                        perr->update_next_err = rd_true;

                } else if (r > 0) {
                        /* R2 failed:
                         * With max.in.flight > 1 we can have a situation
                         * where the first request in-flight (R1) to the broker
                         * fails, which causes the sub-sequent requests
                         * that are in-flight to have a non-sequential
                         * sequence number and thus fail.
                         * But these sub-sequent requests (R2 to R4) are not at
                         * the risk of being duplicated so we bump the epoch and
                         * re-enqueue the messages for later retry
                         * (without incrementing retries).
                         */
                        rd_rkb_dbg(
                            rkb, MSG | RD_KAFKA_DBG_EOS, "ERRSEQ",
                            "ProduceRequest for %.*s [%" PRId32
                            "] "
                            "with %d message(s) failed "
                            "due to skipped sequence numbers "
                            "(%s, base seq %" PRId32
                            " > "
                            "next seq %" PRId32
                            ") "
                            "caused by previous failed request "
                            "(%s, actions %s, "
                            "base seq %" PRId32 "..%" PRId32
                            ", base msgid %" PRIu64 ", %" PRId64
                            "ms ago): "
                            "recovering and retrying",
                            RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                            rktp->rktp_partition,
                            rd_kafka_msgq_len(&batch->msgq),
                            rd_kafka_pid2str(batch->pid), batch->first_seq,
                            perr->next_ack_seq, rd_kafka_err2name(last_err.err),
                            rd_kafka_actions2str(last_err.actions),
                            last_err.base_seq, last_err.last_seq,
                            last_err.base_msgid,
                            last_err.ts ? (now - last_err.ts) / 1000 : -1);

                        perr->incr_retry = 0;
                        perr->actions    = RD_KAFKA_ERR_ACTION_RETRY;
                        perr->status     = RD_KAFKA_MSG_STATUS_NOT_PERSISTED;
                        perr->update_next_ack = rd_false;
                        perr->update_next_err = rd_true;

                        rd_kafka_idemp_drain_epoch_bump(
                            rk, perr->err, "skipped sequence numbers");

                } else {
                        /* Request's sequence is less than next ack,
                         * this should never happen unless we have
                         * local bug or the broker did not respond
                         * to the requests in order. */
                        rd_kafka_idemp_set_fatal_error(
                            rk, perr->err,
                            "ProduceRequest for %.*s [%" PRId32
                            "] "
                            "with %d message(s) failed "
                            "with rewound sequence number on "
                            "broker %" PRId32
                            " (%s, "
                            "base seq %" PRId32 " < next seq %" PRId32
                            "): "
                            "last error %s (actions %s, "
                            "base seq %" PRId32 "..%" PRId32
                            ", base msgid %" PRIu64 ", %" PRId64 "ms ago)",
                            RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                            rktp->rktp_partition,
                            rd_kafka_msgq_len(&batch->msgq), rkb->rkb_nodeid,
                            rd_kafka_pid2str(batch->pid), batch->first_seq,
                            perr->next_ack_seq, rd_kafka_err2name(last_err.err),
                            rd_kafka_actions2str(last_err.actions),
                            last_err.base_seq, last_err.last_seq,
                            last_err.base_msgid,
                            last_err.ts ? (now - last_err.ts) / 1000 : -1);

                        perr->actions = RD_KAFKA_ERR_ACTION_PERMANENT;
                        perr->status  = RD_KAFKA_MSG_STATUS_POSSIBLY_PERSISTED;
                        perr->update_next_ack = rd_false;
                        perr->update_next_err = rd_false;
                }
                break;

        case RD_KAFKA_RESP_ERR_DUPLICATE_SEQUENCE_NUMBER:
                /* This error indicates that we successfully produced
                 * this set of messages before but this (supposed) retry failed.
                 *
                 * Treat as success, however offset and timestamp
                 * will be invalid. */

                /* Future improvement/FIXME:
                 * But first make sure the first message has actually
                 * been retried, getting this error for a non-retried message
                 * indicates a synchronization issue or bug. */
                rd_rkb_dbg(rkb, MSG | RD_KAFKA_DBG_EOS, "DUPSEQ",
                           "ProduceRequest for %.*s [%" PRId32
                           "] "
                           "with %d message(s) failed "
                           "due to duplicate sequence number: "
                           "previous send succeeded but was not acknowledged "
                           "(%s, base seq %" PRId32
                           "): "
                           "marking the messages successfully delivered",
                           RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                           rktp->rktp_partition,
                           rd_kafka_msgq_len(&batch->msgq),
                           rd_kafka_pid2str(batch->pid), batch->first_seq);

                /* Void error, delivery succeeded */
                perr->err             = RD_KAFKA_RESP_ERR_NO_ERROR;
                perr->actions         = 0;
                perr->status          = RD_KAFKA_MSG_STATUS_PERSISTED;
                perr->update_next_ack = rd_true;
                perr->update_next_err = rd_true;
                break;

        case RD_KAFKA_RESP_ERR_UNKNOWN_PRODUCER_ID:
                /* The broker/cluster lost track of our PID because
                 * the last message we produced has now been deleted
                 * (by DeleteRecords, compaction, or topic retention policy).
                 *
                 * If all previous messages are accounted for and this is not
                 * a retry we can simply bump the epoch and reset the sequence
                 * number and then retry the message(s) again.
                 *
                 * If there are outstanding messages not yet acknowledged
                 * then there is no safe way to carry on without risking
                 * duplication or reordering, in which case we fail
                 * the producer.
                 *
                 * In case of the transactional producer and a transaction
                 * coordinator that supports KIP-360 (>= AK 2.5, checked from
                 * the txnmgr, not here) we'll raise an abortable error and
                 * flag that the epoch needs to be bumped on the coordinator. */
                if (rd_kafka_is_transactional(rk)) {
                        rd_rkb_dbg(rkb, MSG | RD_KAFKA_DBG_EOS, "UNKPID",
                                   "ProduceRequest for %.*s [%" PRId32
                                   "] "
                                   "with %d message(s) failed "
                                   "due to unknown producer id "
                                   "(%s, base seq %" PRId32
                                   ", %d retries): "
                                   "failing the current transaction",
                                   RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                                   rktp->rktp_partition,
                                   rd_kafka_msgq_len(&batch->msgq),
                                   rd_kafka_pid2str(batch->pid),
                                   batch->first_seq,
                                   firstmsg->rkm_u.producer.retries);

                        /* Drain outstanding requests and bump epoch. */
                        rd_kafka_idemp_drain_epoch_bump(rk, perr->err,
                                                        "unknown producer id");

                        rd_kafka_txn_set_abortable_error_with_bump(
                            rk, RD_KAFKA_RESP_ERR_UNKNOWN_PRODUCER_ID,
                            "ProduceRequest for %.*s [%" PRId32
                            "] "
                            "with %d message(s) failed "
                            "due to unknown producer id",
                            RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                            rktp->rktp_partition,
                            rd_kafka_msgq_len(&batch->msgq));

                        perr->incr_retry = 0;
                        perr->actions    = RD_KAFKA_ERR_ACTION_PERMANENT;
                        perr->status     = RD_KAFKA_MSG_STATUS_NOT_PERSISTED;
                        perr->update_next_ack = rd_false;
                        perr->update_next_err = rd_true;
                        break;

                } else if (!firstmsg->rkm_u.producer.retries &&
                           perr->next_err_seq == batch->first_seq) {
                        rd_rkb_dbg(rkb, MSG | RD_KAFKA_DBG_EOS, "UNKPID",
                                   "ProduceRequest for %.*s [%" PRId32
                                   "] "
                                   "with %d message(s) failed "
                                   "due to unknown producer id "
                                   "(%s, base seq %" PRId32
                                   ", %d retries): "
                                   "no risk of duplication/reordering: "
                                   "resetting PID and retrying",
                                   RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                                   rktp->rktp_partition,
                                   rd_kafka_msgq_len(&batch->msgq),
                                   rd_kafka_pid2str(batch->pid),
                                   batch->first_seq,
                                   firstmsg->rkm_u.producer.retries);

                        /* Drain outstanding requests and bump epoch. */
                        rd_kafka_idemp_drain_epoch_bump(rk, perr->err,
                                                        "unknown producer id");

                        perr->incr_retry = 0;
                        perr->actions    = RD_KAFKA_ERR_ACTION_RETRY;
                        perr->status     = RD_KAFKA_MSG_STATUS_NOT_PERSISTED;
                        perr->update_next_ack = rd_false;
                        perr->update_next_err = rd_true;
                        break;
                }

                rd_kafka_idemp_set_fatal_error(
                    rk, perr->err,
                    "ProduceRequest for %.*s [%" PRId32
                    "] "
                    "with %d message(s) failed "
                    "due to unknown producer id ("
                    "broker %" PRId32 " %s, base seq %" PRId32
                    ", %d retries): "
                    "unable to retry without risking "
                    "duplication/reordering",
                    RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                    rktp->rktp_partition, rd_kafka_msgq_len(&batch->msgq),
                    rkb->rkb_nodeid, rd_kafka_pid2str(batch->pid),
                    batch->first_seq, firstmsg->rkm_u.producer.retries);

                perr->actions         = RD_KAFKA_ERR_ACTION_PERMANENT;
                perr->status          = RD_KAFKA_MSG_STATUS_POSSIBLY_PERSISTED;
                perr->update_next_ack = rd_false;
                perr->update_next_err = rd_true;
                break;

        default:
                /* All other errors are handled in the standard
                 * error Produce handler, which will set
                 * update_next_ack|err accordingly. */
                break;
        }
}



/**
 * @brief Error-handling for failed ProduceRequests
 *
 * @param errp Is the input and output error, it may be changed
 *             by this function.
 *
 * @returns 0 if no further processing of the request should be performed,
 *          such as triggering delivery reports, else 1.
 *
 * @warning May be called on the old leader thread. Lock rktp appropriately!
 *
 * @warning \p request may be NULL.
 *
 * @locality broker thread (but not necessarily the leader broker)
 * @locks none
 */
static int rd_kafka_handle_Produce_error(rd_kafka_broker_t *rkb,
                                         const rd_kafka_buf_t *request,
                                         rd_kafka_msgbatch_t *batch,
                                         struct rd_kafka_Produce_err *perr) {
        rd_kafka_t *rk          = rkb->rkb_rk;
        rd_kafka_toppar_t *rktp = batch->rktp;
        int is_leader;

        if (unlikely(perr->err == RD_KAFKA_RESP_ERR__DESTROY))
                return 0; /* Terminating */

        /* When there is a partition leader change any outstanding
         * requests to the old broker will be handled by the old
         * broker thread when the responses are received/timeout:
         * in this case we need to be careful with locking:
         * check once if we're the leader (which allows relaxed
         * locking), and cache the current rktp's eos state vars. */
        rd_kafka_toppar_lock(rktp);
        is_leader          = rktp->rktp_broker == rkb;
        perr->rktp_pid     = rktp->rktp_eos.pid;
        perr->next_ack_seq = rktp->rktp_eos.next_ack_seq;
        perr->next_err_seq = rktp->rktp_eos.next_err_seq;
        rd_kafka_toppar_unlock(rktp);

        /* All failures are initially treated as if the message
         * was not persisted, but the status may be changed later
         * for specific errors and actions. */
        perr->status = RD_KAFKA_MSG_STATUS_NOT_PERSISTED;

        /* Set actions for known errors (may be overriden later),
         * all other errors are considered permanent failures.
         * (also see rd_kafka_err_action() for the default actions). */
        perr->actions = rd_kafka_err_action(
            rkb, perr->err, request,

            RD_KAFKA_ERR_ACTION_REFRESH |
                RD_KAFKA_ERR_ACTION_MSG_POSSIBLY_PERSISTED,
            RD_KAFKA_RESP_ERR__TRANSPORT,

            RD_KAFKA_ERR_ACTION_REFRESH | RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED,
            RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART,

            RD_KAFKA_ERR_ACTION_PERMANENT |
                RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED,
            RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED,

            RD_KAFKA_ERR_ACTION_REFRESH | RD_KAFKA_ERR_ACTION_RETRY |
                RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED,
            RD_KAFKA_RESP_ERR_KAFKA_STORAGE_ERROR,

            RD_KAFKA_ERR_ACTION_RETRY | RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED,
            RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS,

            RD_KAFKA_ERR_ACTION_RETRY |
                RD_KAFKA_ERR_ACTION_MSG_POSSIBLY_PERSISTED,
            RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS_AFTER_APPEND,

            RD_KAFKA_ERR_ACTION_RETRY | RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED,
            RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE,

            RD_KAFKA_ERR_ACTION_RETRY |
                RD_KAFKA_ERR_ACTION_MSG_POSSIBLY_PERSISTED,
            RD_KAFKA_RESP_ERR__TIMED_OUT,

            RD_KAFKA_ERR_ACTION_PERMANENT |
                RD_KAFKA_ERR_ACTION_MSG_POSSIBLY_PERSISTED,
            RD_KAFKA_RESP_ERR__MSG_TIMED_OUT,

            /* All Idempotent Producer-specific errors are
             * initially set as permanent errors,
             * special handling may change the actions. */
            RD_KAFKA_ERR_ACTION_PERMANENT |
                RD_KAFKA_ERR_ACTION_MSG_POSSIBLY_PERSISTED,
            RD_KAFKA_RESP_ERR_OUT_OF_ORDER_SEQUENCE_NUMBER,

            RD_KAFKA_ERR_ACTION_PERMANENT |
                RD_KAFKA_ERR_ACTION_MSG_POSSIBLY_PERSISTED,
            RD_KAFKA_RESP_ERR_DUPLICATE_SEQUENCE_NUMBER,

            RD_KAFKA_ERR_ACTION_PERMANENT |
                RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED,
            RD_KAFKA_RESP_ERR_UNKNOWN_PRODUCER_ID,

            RD_KAFKA_ERR_ACTION_PERMANENT |
                RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED,
            RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH,

            /* Message was purged from out-queue due to
             * Idempotent Producer Id change */
            RD_KAFKA_ERR_ACTION_RETRY, RD_KAFKA_RESP_ERR__RETRY,

            RD_KAFKA_ERR_ACTION_END);

        rd_rkb_dbg(rkb, MSG, "MSGSET",
                   "%s [%" PRId32
                   "]: MessageSet with %i message(s) "
                   "(MsgId %" PRIu64 ", BaseSeq %" PRId32
                   ") "
                   "encountered error: %s (actions %s)%s",
                   rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                   rd_kafka_msgq_len(&batch->msgq), batch->first_msgid,
                   batch->first_seq, rd_kafka_err2str(perr->err),
                   rd_kafka_actions2str(perr->actions),
                   is_leader ? "" : " [NOT LEADER]");


        /*
         * Special handling for Idempotent Producer
         *
         * Note: Idempotent Producer-specific errors received
         *       on a non-idempotent producer will be passed through
         *       directly to the application.
         */
        if (rd_kafka_is_idempotent(rk))
                rd_kafka_handle_idempotent_Produce_error(rkb, batch, perr);

        /* Update message persistence status based on action flags.
         * None of these are typically set after an idempotent error,
         * which sets the status explicitly. */
        if (perr->actions & RD_KAFKA_ERR_ACTION_MSG_POSSIBLY_PERSISTED)
                perr->status = RD_KAFKA_MSG_STATUS_POSSIBLY_PERSISTED;
        else if (perr->actions & RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED)
                perr->status = RD_KAFKA_MSG_STATUS_NOT_PERSISTED;
        else if (perr->actions & RD_KAFKA_ERR_ACTION_MSG_PERSISTED)
                perr->status = RD_KAFKA_MSG_STATUS_PERSISTED;

        /* Save the last error for debugging sub-sequent errors,
         * useful for Idempotent Producer throubleshooting. */
        rd_kafka_toppar_lock(rktp);
        rktp->rktp_last_err.err        = perr->err;
        rktp->rktp_last_err.actions    = perr->actions;
        rktp->rktp_last_err.ts         = rd_clock();
        rktp->rktp_last_err.base_seq   = batch->first_seq;
        rktp->rktp_last_err.last_seq   = perr->last_seq;
        rktp->rktp_last_err.base_msgid = batch->first_msgid;
        rd_kafka_toppar_unlock(rktp);

        /*
         * Handle actions
         */
        if (perr->actions &
            (RD_KAFKA_ERR_ACTION_REFRESH | RD_KAFKA_ERR_ACTION_RETRY)) {
                /* Retry (refresh also implies retry) */

                if (perr->actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                        /* Request metadata information update.
                         * These errors imply that we have stale
                         * information and the request was
                         * either rejected or not sent -
                         * we don't need to increment the retry count
                         * when we perform a retry since:
                         *   - it is a temporary error (hopefully)
                         *   - there is no chance of duplicate delivery
                         */
                        rd_kafka_toppar_leader_unavailable(rktp, "produce",
                                                           perr->err);

                        /* We can't be certain the request wasn't
                         * sent in case of transport failure,
                         * so the ERR__TRANSPORT case will need
                         * the retry count to be increased,
                         * In case of certain other errors we want to
                         * avoid retrying for the duration of the
                         * message.timeout.ms to speed up error propagation. */
                        if (perr->err != RD_KAFKA_RESP_ERR__TRANSPORT &&
                            perr->err != RD_KAFKA_RESP_ERR_KAFKA_STORAGE_ERROR)
                                perr->incr_retry = 0;
                }

                /* If message timed out in queue, not in transit,
                 * we will retry at a later time but not increment
                 * the retry count since there is no risk
                 * of duplicates. */
                if (!rd_kafka_buf_was_sent(request))
                        perr->incr_retry = 0;

                if (!perr->incr_retry) {
                        /* If retries are not to be incremented then
                         * there is no chance of duplicates on retry, which
                         * means these messages were not persisted. */
                        perr->status = RD_KAFKA_MSG_STATUS_NOT_PERSISTED;
                }

                if (rd_kafka_is_idempotent(rk)) {
                        /* Any currently in-flight requests will
                         * fail with ERR_OUT_OF_ORDER_SEQUENCE_NUMBER,
                         * which should not be treated as a fatal error
                         * since this request and sub-sequent requests
                         * will be retried and thus return to order.
                         * Unless the error was a timeout, or similar,
                         * in which case the request might have made it
                         * and the messages are considered possibly persisted:
                         * in this case we allow the next in-flight response
                         * to be successful, in which case we mark
                         * this request's messages as succesfully delivered. */
                        if (perr->status &
                            RD_KAFKA_MSG_STATUS_POSSIBLY_PERSISTED)
                                perr->update_next_ack = rd_true;
                        else
                                perr->update_next_ack = rd_false;
                        perr->update_next_err = rd_true;

                        /* Drain outstanding requests so that retries
                         * are attempted with proper state knowledge and
                         * without any in-flight requests. */
                        rd_kafka_toppar_lock(rktp);
                        rd_kafka_idemp_drain_toppar(rktp,
                                                    "drain before retrying");
                        rd_kafka_toppar_unlock(rktp);
                }

                /* Since requests are specific to a broker
                 * we move the retryable messages from the request
                 * back to the partition queue (prepend) and then
                 * let the new broker construct a new request.
                 * While doing this we also make sure the retry count
                 * for each message is honoured, any messages that
                 * would exceeded the retry count will not be
                 * moved but instead fail below. */
                rd_kafka_toppar_retry_msgq(rktp, &batch->msgq, perr->incr_retry,
                                           perr->status);

                if (rd_kafka_msgq_len(&batch->msgq) == 0) {
                        /* No need do anything more with the request
                         * here since the request no longer has any
                         * messages associated with it. */
                        return 0;
                }
        }

        if (perr->actions & RD_KAFKA_ERR_ACTION_PERMANENT &&
            rd_kafka_is_idempotent(rk)) {
                if (rd_kafka_is_transactional(rk) &&
                    perr->err == RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH) {
                        /* Producer was fenced by new transactional producer
                         * with the same transactional.id */
                        rd_kafka_txn_set_fatal_error(
                            rk, RD_DO_LOCK, RD_KAFKA_RESP_ERR__FENCED,
                            "ProduceRequest for %.*s [%" PRId32
                            "] "
                            "with %d message(s) failed: %s "
                            "(broker %" PRId32 " %s, base seq %" PRId32
                            "): "
                            "transactional producer fenced by newer "
                            "producer instance",
                            RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                            rktp->rktp_partition,
                            rd_kafka_msgq_len(&batch->msgq),
                            rd_kafka_err2str(perr->err), rkb->rkb_nodeid,
                            rd_kafka_pid2str(batch->pid), batch->first_seq);

                        /* Drain outstanding requests and reset PID. */
                        rd_kafka_idemp_drain_reset(
                            rk, "fenced by new transactional producer");

                } else if (rd_kafka_is_transactional(rk)) {
                        /* When transactional any permanent produce failure
                         * would lead to an incomplete transaction, so raise
                         * an abortable transaction error. */
                        rd_kafka_txn_set_abortable_error(
                            rk, perr->err,
                            "ProduceRequest for %.*s [%" PRId32
                            "] "
                            "with %d message(s) failed: %s "
                            "(broker %" PRId32 " %s, base seq %" PRId32
                            "): "
                            "current transaction must be aborted",
                            RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                            rktp->rktp_partition,
                            rd_kafka_msgq_len(&batch->msgq),
                            rd_kafka_err2str(perr->err), rkb->rkb_nodeid,
                            rd_kafka_pid2str(batch->pid), batch->first_seq);

                } else if (rk->rk_conf.eos.gapless) {
                        /* A permanent non-idempotent error will lead to
                         * gaps in the message series, the next request
                         * will fail with ...ERR_OUT_OF_ORDER_SEQUENCE_NUMBER.
                         * To satisfy the gapless guarantee we need to raise
                         * a fatal error here. */
                        rd_kafka_idemp_set_fatal_error(
                            rk, RD_KAFKA_RESP_ERR__GAPLESS_GUARANTEE,
                            "ProduceRequest for %.*s [%" PRId32
                            "] "
                            "with %d message(s) failed: "
                            "%s (broker %" PRId32 " %s, base seq %" PRId32
                            "): "
                            "unable to satisfy gap-less guarantee",
                            RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                            rktp->rktp_partition,
                            rd_kafka_msgq_len(&batch->msgq),
                            rd_kafka_err2str(perr->err), rkb->rkb_nodeid,
                            rd_kafka_pid2str(batch->pid), batch->first_seq);

                        /* Drain outstanding requests and reset PID. */
                        rd_kafka_idemp_drain_reset(
                            rk, "unable to satisfy gap-less guarantee");

                } else {
                        /* If gapless is not set we bump the Epoch and
                         * renumber the messages to send. */

                        /* Drain outstanding requests and bump the epoch .*/
                        rd_kafka_idemp_drain_epoch_bump(rk, perr->err,
                                                        "message sequence gap");
                }

                perr->update_next_ack = rd_false;
                /* Make sure the next error will not raise a fatal error. */
                perr->update_next_err = rd_true;
        }

        if (perr->err == RD_KAFKA_RESP_ERR__TIMED_OUT ||
            perr->err == RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE) {
                /* Translate request-level timeout error code
                 * to message-level timeout error code. */
                perr->err = RD_KAFKA_RESP_ERR__MSG_TIMED_OUT;

        } else if (perr->err == RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED) {
                /* If we're no longer authorized to access the topic mark
                 * it as errored to deny further produce requests. */
                rd_kafka_topic_wrlock(rktp->rktp_rkt);
                rd_kafka_topic_set_error(rktp->rktp_rkt, perr->err);
                rd_kafka_topic_wrunlock(rktp->rktp_rkt);
        }

        return 1;
}

/**
 * @brief Handle ProduceResponse success for idempotent producer
 *
 * @warning May be called on the old leader thread. Lock rktp appropriately!
 *
 * @locks none
 * @locality broker thread (but not necessarily the leader broker thread)
 */
static void
rd_kafka_handle_idempotent_Produce_success(rd_kafka_broker_t *rkb,
                                           rd_kafka_msgbatch_t *batch,
                                           int32_t next_seq) {
        rd_kafka_t *rk          = rkb->rkb_rk;
        rd_kafka_toppar_t *rktp = batch->rktp;
        char fatal_err[512];
        uint64_t first_msgid, last_msgid;

        *fatal_err = '\0';

        first_msgid = rd_kafka_msgq_first(&batch->msgq)->rkm_u.producer.msgid;
        last_msgid  = rd_kafka_msgq_last(&batch->msgq)->rkm_u.producer.msgid;

        rd_kafka_toppar_lock(rktp);

        /* If the last acked msgid is higher than
         * the next message to (re)transmit in the message queue
         * it means a previous series of R1,R2 ProduceRequests
         * had R1 fail with uncertain persistence status,
         * such as timeout or transport error, but R2 succeeded,
         * which means the messages in R1 were in fact persisted.
         * In this case trigger delivery reports for all messages
         * in queue until we hit a non-acked message msgid. */
        if (unlikely(rktp->rktp_eos.acked_msgid < first_msgid - 1)) {
                rd_kafka_dr_implicit_ack(rkb, rktp, last_msgid);

        } else if (unlikely(batch->first_seq != rktp->rktp_eos.next_ack_seq &&
                            batch->first_seq == rktp->rktp_eos.next_err_seq)) {
                /* Response ordering is typically not a concern
                 * (but will not happen with current broker versions),
                 * unless we're expecting an error to be returned at
                 * this sequence rather than a success ack, in which
                 * case raise a fatal error. */

                /* Can't call set_fatal_error() while
                 * holding the toppar lock, so construct
                 * the error string here and call
                 * set_fatal_error() below after
                 * toppar lock has been released. */
                rd_snprintf(fatal_err, sizeof(fatal_err),
                            "ProduceRequest for %.*s [%" PRId32
                            "] "
                            "with %d message(s) "
                            "succeeded when expecting failure "
                            "(broker %" PRId32
                            " %s, "
                            "base seq %" PRId32
                            ", "
                            "next ack seq %" PRId32
                            ", "
                            "next err seq %" PRId32
                            ": "
                            "unable to retry without risking "
                            "duplication/reordering",
                            RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                            rktp->rktp_partition,
                            rd_kafka_msgq_len(&batch->msgq), rkb->rkb_nodeid,
                            rd_kafka_pid2str(batch->pid), batch->first_seq,
                            rktp->rktp_eos.next_ack_seq,
                            rktp->rktp_eos.next_err_seq);

                rktp->rktp_eos.next_err_seq = next_seq;
        }

        if (likely(!*fatal_err)) {
                /* Advance next expected err and/or ack sequence */

                /* Only step err seq if it hasn't diverged. */
                if (rktp->rktp_eos.next_err_seq == rktp->rktp_eos.next_ack_seq)
                        rktp->rktp_eos.next_err_seq = next_seq;

                rktp->rktp_eos.next_ack_seq = next_seq;
        }

        /* Store the last acked message sequence,
         * since retries within the broker cache window (5 requests)
         * will succeed for older messages we must only update the
         * acked msgid if it is higher than the last acked. */
        if (last_msgid > rktp->rktp_eos.acked_msgid)
                rktp->rktp_eos.acked_msgid = last_msgid;

        rd_kafka_toppar_unlock(rktp);

        /* Must call set_fatal_error() after releasing
         * the toppar lock. */
        if (unlikely(*fatal_err))
                rd_kafka_idemp_set_fatal_error(
                    rk, RD_KAFKA_RESP_ERR__INCONSISTENT, "%s", fatal_err);
}


/**
 * @brief Handle ProduceRequest result for a message batch.
 *
 * @warning \p request may be NULL.
 *
 * @localiy broker thread (but not necessarily the toppar's handler thread)
 * @locks none
 */
static void rd_kafka_msgbatch_handle_Produce_result(
    rd_kafka_broker_t *rkb,
    rd_kafka_msgbatch_t *batch,
    rd_kafka_resp_err_t err,
    const struct rd_kafka_Produce_result *presult,
    const rd_kafka_buf_t *request) {

        rd_kafka_t *rk               = rkb->rkb_rk;
        rd_kafka_toppar_t *rktp      = batch->rktp;
        rd_kafka_msg_status_t status = RD_KAFKA_MSG_STATUS_POSSIBLY_PERSISTED;
        rd_bool_t last_inflight;
        int32_t next_seq;

        /* Decrease partition's messages in-flight counter */
        rd_assert(rd_atomic32_get(&rktp->rktp_msgs_inflight) >=
                  rd_kafka_msgq_len(&batch->msgq));
        last_inflight = !rd_atomic32_sub(&rktp->rktp_msgs_inflight,
                                         rd_kafka_msgq_len(&batch->msgq));

        /* Next expected sequence (and handle wrap) */
        next_seq = rd_kafka_seq_wrap(batch->first_seq +
                                     rd_kafka_msgq_len(&batch->msgq));

        if (likely(!err)) {
                rd_rkb_dbg(rkb, MSG, "MSGSET",
                           "%s [%" PRId32
                           "]: MessageSet with %i message(s) "
                           "(MsgId %" PRIu64 ", BaseSeq %" PRId32 ") delivered",
                           rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                           rd_kafka_msgq_len(&batch->msgq), batch->first_msgid,
                           batch->first_seq);

                if (rktp->rktp_rkt->rkt_conf.required_acks != 0)
                        status = RD_KAFKA_MSG_STATUS_PERSISTED;

                if (rd_kafka_is_idempotent(rk))
                        rd_kafka_handle_idempotent_Produce_success(rkb, batch,
                                                                   next_seq);
        } else {
                /* Error handling */
                struct rd_kafka_Produce_err perr = {
                    .err             = err,
                    .incr_retry      = 1,
                    .status          = status,
                    .update_next_ack = rd_true,
                    .update_next_err = rd_true,
                    .last_seq        = (batch->first_seq +
                                 rd_kafka_msgq_len(&batch->msgq) - 1)};

                rd_kafka_handle_Produce_error(rkb, request, batch, &perr);

                /* Update next expected acked and/or err sequence. */
                if (perr.update_next_ack || perr.update_next_err) {
                        rd_kafka_toppar_lock(rktp);
                        if (perr.update_next_ack)
                                rktp->rktp_eos.next_ack_seq = next_seq;
                        if (perr.update_next_err)
                                rktp->rktp_eos.next_err_seq = next_seq;
                        rd_kafka_toppar_unlock(rktp);
                }

                err    = perr.err;
                status = perr.status;
        }


        /* Messages to retry will have been removed from the request's queue */
        if (likely(rd_kafka_msgq_len(&batch->msgq) > 0)) {
                /* Set offset, timestamp and status for each message. */
                rd_kafka_msgq_set_metadata(&batch->msgq, rkb->rkb_nodeid,
                                           presult->offset, presult->timestamp,
                                           status);

                /* Enqueue messages for delivery report. */
                rd_kafka_dr_msgq(rktp->rktp_rkt, &batch->msgq, err);
        }

        if (rd_kafka_is_idempotent(rk) && last_inflight)
                rd_kafka_idemp_inflight_toppar_sub(rk, rktp);
}


/**
 * @brief Handle ProduceResponse
 *
 * @param reply is NULL when `acks=0` and on various local errors.
 *
 * @remark ProduceRequests are never retried, retriable errors are
 *         instead handled by re-enqueuing the request's messages back
 *         on the partition queue to have a new ProduceRequest constructed
 *         eventually.
 *
 * @warning May be called on the old leader thread. Lock rktp appropriately!
 *
 * @locality broker thread (but not necessarily the leader broker thread)
 */
static void rd_kafka_handle_Produce(rd_kafka_t *rk,
                                    rd_kafka_broker_t *rkb,
                                    rd_kafka_resp_err_t err,
                                    rd_kafka_buf_t *reply,
                                    rd_kafka_buf_t *request,
                                    void *opaque) {
        rd_kafka_msgbatch_t *batch            = &request->rkbuf_batch;
        rd_kafka_toppar_t *rktp               = batch->rktp;
        struct rd_kafka_Produce_result result = {
            .offset = RD_KAFKA_OFFSET_INVALID, .timestamp = -1};

        /* Unit test interface: inject errors */
        if (unlikely(rk->rk_conf.ut.handle_ProduceResponse != NULL)) {
                err = rk->rk_conf.ut.handle_ProduceResponse(
                    rkb->rkb_rk, rkb->rkb_nodeid, batch->first_msgid, err);
        }

        /* Parse Produce reply (unless the request errored) */
        if (!err && reply)
                err = rd_kafka_handle_Produce_parse(rkb, rktp, reply, request,
                                                    &result);

        rd_kafka_msgbatch_handle_Produce_result(rkb, batch, err, &result,
                                                request);
}


/**
 * @brief Send ProduceRequest for messages in toppar queue.
 *
 * @returns the number of messages included, or 0 on error / no messages.
 *
 * @locality broker thread
 */
int rd_kafka_ProduceRequest(rd_kafka_broker_t *rkb,
                            rd_kafka_toppar_t *rktp,
                            const rd_kafka_pid_t pid,
                            uint64_t epoch_base_msgid) {
        rd_kafka_buf_t *rkbuf;
        rd_kafka_topic_t *rkt = rktp->rktp_rkt;
        size_t MessageSetSize = 0;
        int cnt;
        rd_ts_t now;
        int64_t first_msg_timeout;
        int tmout;

        /**
         * Create ProduceRequest with as many messages from the toppar
         * transmit queue as possible.
         */
        rkbuf = rd_kafka_msgset_create_ProduceRequest(
            rkb, rktp, &rktp->rktp_xmit_msgq, pid, epoch_base_msgid,
            &MessageSetSize);
        if (unlikely(!rkbuf))
                return 0;

        cnt = rd_kafka_msgq_len(&rkbuf->rkbuf_batch.msgq);
        rd_dassert(cnt > 0);

        rd_avg_add(&rktp->rktp_rkt->rkt_avg_batchcnt, (int64_t)cnt);
        rd_avg_add(&rktp->rktp_rkt->rkt_avg_batchsize, (int64_t)MessageSetSize);

        if (!rkt->rkt_conf.required_acks)
                rkbuf->rkbuf_flags |= RD_KAFKA_OP_F_NO_RESPONSE;

        /* Use timeout from first message in batch */
        now = rd_clock();
        first_msg_timeout =
            (rd_kafka_msgq_first(&rkbuf->rkbuf_batch.msgq)->rkm_ts_timeout -
             now) /
            1000;

        if (unlikely(first_msg_timeout <= 0)) {
                /* Message has already timed out, allow 100 ms
                 * to produce anyway */
                tmout = 100;
        } else {
                tmout = (int)RD_MIN(INT_MAX, first_msg_timeout);
        }

        /* Set absolute timeout (including retries), the
         * effective timeout for this specific request will be
         * capped by socket.timeout.ms */
        rd_kafka_buf_set_abs_timeout(rkbuf, tmout, now);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, RD_KAFKA_NO_REPLYQ,
                                       rd_kafka_handle_Produce, NULL);

        return cnt;
}


/**
 * @brief Construct and send CreateTopicsRequest to \p rkb
 *        with the topics (NewTopic_t*) in \p new_topics, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_CreateTopicsRequest(rd_kafka_broker_t *rkb,
                             const rd_list_t *new_topics /*(NewTopic_t*)*/,
                             rd_kafka_AdminOptions_t *options,
                             char *errstr,
                             size_t errstr_size,
                             rd_kafka_replyq_t replyq,
                             rd_kafka_resp_cb_t *resp_cb,
                             void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;
        int i = 0;
        rd_kafka_NewTopic_t *newt;
        int op_timeout;

        if (rd_list_cnt(new_topics) == 0) {
                rd_snprintf(errstr, errstr_size, "No topics to create");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_CreateTopics, 0, 4, &features);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "Topic Admin API (KIP-4) not supported "
                            "by broker, requires broker version >= 0.10.2.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        if (rd_kafka_confval_get_int(&options->validate_only) &&
            ApiVersion < 1) {
                rd_snprintf(errstr, errstr_size,
                            "CreateTopics.validate_only=true not "
                            "supported by broker");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_CreateTopics, 1,
                                         4 + (rd_list_cnt(new_topics) * 200) +
                                             4 + 1);

        /* #topics */
        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(new_topics));

        while ((newt = rd_list_elem(new_topics, i++))) {
                int partition;
                int ei = 0;
                const rd_kafka_ConfigEntry_t *entry;

                if (ApiVersion < 4) {
                        if (newt->num_partitions == -1) {
                                rd_snprintf(errstr, errstr_size,
                                            "Default partition count (KIP-464) "
                                            "not supported by broker, "
                                            "requires broker version <= 2.4.0");
                                rd_kafka_replyq_destroy(&replyq);
                                rd_kafka_buf_destroy(rkbuf);
                                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
                        }

                        if (newt->replication_factor == -1 &&
                            rd_list_empty(&newt->replicas)) {
                                rd_snprintf(errstr, errstr_size,
                                            "Default replication factor "
                                            "(KIP-464) "
                                            "not supported by broker, "
                                            "requires broker version <= 2.4.0");
                                rd_kafka_replyq_destroy(&replyq);
                                rd_kafka_buf_destroy(rkbuf);
                                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
                        }
                }

                /* topic */
                rd_kafka_buf_write_str(rkbuf, newt->topic, -1);

                if (rd_list_cnt(&newt->replicas)) {
                        /* num_partitions and replication_factor must be
                         * set to -1 if a replica assignment is sent. */
                        /* num_partitions */
                        rd_kafka_buf_write_i32(rkbuf, -1);
                        /* replication_factor */
                        rd_kafka_buf_write_i16(rkbuf, -1);
                } else {
                        /* num_partitions */
                        rd_kafka_buf_write_i32(rkbuf, newt->num_partitions);
                        /* replication_factor */
                        rd_kafka_buf_write_i16(
                            rkbuf, (int16_t)newt->replication_factor);
                }

                /* #replica_assignment */
                rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(&newt->replicas));

                /* Replicas per partition, see rdkafka_admin.[ch]
                 * for how these are constructed. */
                for (partition = 0; partition < rd_list_cnt(&newt->replicas);
                     partition++) {
                        const rd_list_t *replicas;
                        int ri = 0;

                        replicas = rd_list_elem(&newt->replicas, partition);
                        if (!replicas)
                                continue;

                        /* partition */
                        rd_kafka_buf_write_i32(rkbuf, partition);
                        /* #replicas */
                        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(replicas));

                        for (ri = 0; ri < rd_list_cnt(replicas); ri++) {
                                /* replica */
                                rd_kafka_buf_write_i32(
                                    rkbuf, rd_list_get_int32(replicas, ri));
                        }
                }

                /* #config_entries */
                rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(&newt->config));

                RD_LIST_FOREACH(entry, &newt->config, ei) {
                        /* config_name */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->name, -1);
                        /* config_value (nullable) */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->value, -1);
                }
        }

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        rd_kafka_buf_write_i32(rkbuf, op_timeout);

        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout + 1000, 0);

        if (ApiVersion >= 1) {
                /* validate_only */
                rd_kafka_buf_write_i8(
                    rkbuf, rd_kafka_confval_get_int(&options->validate_only));
        }

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Construct and send DeleteTopicsRequest to \p rkb
 *        with the topics (DeleteTopic_t *) in \p del_topics, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_DeleteTopicsRequest(rd_kafka_broker_t *rkb,
                             const rd_list_t *del_topics /*(DeleteTopic_t*)*/,
                             rd_kafka_AdminOptions_t *options,
                             char *errstr,
                             size_t errstr_size,
                             rd_kafka_replyq_t replyq,
                             rd_kafka_resp_cb_t *resp_cb,
                             void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;
        int i = 0;
        rd_kafka_DeleteTopic_t *delt;
        int op_timeout;

        if (rd_list_cnt(del_topics) == 0) {
                rd_snprintf(errstr, errstr_size, "No topics to delete");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_DeleteTopics, 0, 1, &features);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "Topic Admin API (KIP-4) not supported "
                            "by broker, requires broker version >= 0.10.2.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf =
            rd_kafka_buf_new_request(rkb, RD_KAFKAP_DeleteTopics, 1,
                                     /* FIXME */
                                     4 + (rd_list_cnt(del_topics) * 100) + 4);

        /* #topics */
        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(del_topics));

        while ((delt = rd_list_elem(del_topics, i++)))
                rd_kafka_buf_write_str(rkbuf, delt->topic, -1);

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        rd_kafka_buf_write_i32(rkbuf, op_timeout);

        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout + 1000, 0);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Construct and send DeleteRecordsRequest to \p rkb
 *        with the offsets to delete (rd_kafka_topic_partition_list_t *) in
 *        \p offsets_list, using \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @remark The rd_kafka_topic_partition_list_t in \p offsets_list must already
 *          be sorted.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_DeleteRecordsRequest(rd_kafka_broker_t *rkb,
                              /*(rd_kafka_topic_partition_list_t*)*/
                              const rd_list_t *offsets_list,
                              rd_kafka_AdminOptions_t *options,
                              char *errstr,
                              size_t errstr_size,
                              rd_kafka_replyq_t replyq,
                              rd_kafka_resp_cb_t *resp_cb,
                              void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;
        const rd_kafka_topic_partition_list_t *partitions;
        int op_timeout;

        partitions = rd_list_elem(offsets_list, 0);

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_DeleteRecords, 0, 1, &features);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "DeleteRecords Admin API (KIP-107) not supported "
                            "by broker, requires broker version >= 0.11.0");
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_DeleteRecords, 1,
                                         4 + (partitions->cnt * 100) + 4);

        rd_kafka_buf_write_topic_partitions(
            rkbuf, partitions, rd_false /*don't skip invalid offsets*/,
            rd_false /*any offset*/, rd_true /*do write offsets*/,
            rd_false /*don't write epoch*/, rd_false /*don't write metadata*/);

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        rd_kafka_buf_write_i32(rkbuf, op_timeout);

        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout + 1000, 0);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Construct and send CreatePartitionsRequest to \p rkb
 *        with the topics (NewPartitions_t*) in \p new_parts, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_CreatePartitionsRequest(rd_kafka_broker_t *rkb,
                                 /*(NewPartitions_t*)*/
                                 const rd_list_t *new_parts,
                                 rd_kafka_AdminOptions_t *options,
                                 char *errstr,
                                 size_t errstr_size,
                                 rd_kafka_replyq_t replyq,
                                 rd_kafka_resp_cb_t *resp_cb,
                                 void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int i              = 0;
        rd_kafka_NewPartitions_t *newp;
        int op_timeout;

        if (rd_list_cnt(new_parts) == 0) {
                rd_snprintf(errstr, errstr_size, "No partitions to create");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_CreatePartitions, 0, 0, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "CreatePartitions (KIP-195) not supported "
                            "by broker, requires broker version >= 1.0.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_CreatePartitions, 1,
                                         4 + (rd_list_cnt(new_parts) * 200) +
                                             4 + 1);

        /* #topics */
        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(new_parts));

        while ((newp = rd_list_elem(new_parts, i++))) {
                /* topic */
                rd_kafka_buf_write_str(rkbuf, newp->topic, -1);

                /* New partition count */
                rd_kafka_buf_write_i32(rkbuf, (int32_t)newp->total_cnt);

                /* #replica_assignment */
                if (rd_list_empty(&newp->replicas)) {
                        rd_kafka_buf_write_i32(rkbuf, -1);
                } else {
                        const rd_list_t *replicas;
                        int pi = -1;

                        rd_kafka_buf_write_i32(rkbuf,
                                               rd_list_cnt(&newp->replicas));

                        while (
                            (replicas = rd_list_elem(&newp->replicas, ++pi))) {
                                int ri = 0;

                                /* replica count */
                                rd_kafka_buf_write_i32(rkbuf,
                                                       rd_list_cnt(replicas));

                                /* replica */
                                for (ri = 0; ri < rd_list_cnt(replicas); ri++) {
                                        rd_kafka_buf_write_i32(
                                            rkbuf,
                                            rd_list_get_int32(replicas, ri));
                                }
                        }
                }
        }

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        rd_kafka_buf_write_i32(rkbuf, op_timeout);

        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout + 1000, 0);

        /* validate_only */
        rd_kafka_buf_write_i8(
            rkbuf, rd_kafka_confval_get_int(&options->validate_only));

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Construct and send AlterConfigsRequest to \p rkb
 *        with the configs (ConfigResource_t*) in \p configs, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_AlterConfigsRequest(rd_kafka_broker_t *rkb,
                             const rd_list_t *configs /*(ConfigResource_t*)*/,
                             rd_kafka_AdminOptions_t *options,
                             char *errstr,
                             size_t errstr_size,
                             rd_kafka_replyq_t replyq,
                             rd_kafka_resp_cb_t *resp_cb,
                             void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int i;
        const rd_kafka_ConfigResource_t *config;
        int op_timeout;

        if (rd_list_cnt(configs) == 0) {
                rd_snprintf(errstr, errstr_size,
                            "No config resources specified");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_AlterConfigs, 0, 0, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "AlterConfigs (KIP-133) not supported "
                            "by broker, requires broker version >= 0.11.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        /* incremental requires ApiVersion > FIXME */
        if (ApiVersion < 1 /* FIXME */ &&
            rd_kafka_confval_get_int(&options->incremental)) {
                rd_snprintf(errstr, errstr_size,
                            "AlterConfigs.incremental=true (KIP-248) "
                            "not supported by broker, "
                            "requires broker version >= 2.0.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_AlterConfigs, 1,
                                         rd_list_cnt(configs) * 200);

        /* #resources */
        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(configs));

        RD_LIST_FOREACH(config, configs, i) {
                const rd_kafka_ConfigEntry_t *entry;
                int ei;

                /* resource_type */
                rd_kafka_buf_write_i8(rkbuf, config->restype);

                /* resource_name */
                rd_kafka_buf_write_str(rkbuf, config->name, -1);

                /* #config */
                rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(&config->config));

                RD_LIST_FOREACH(entry, &config->config, ei) {
                        /* config_name */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->name, -1);
                        /* config_value (nullable) */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->value, -1);

                        if (ApiVersion == 1)
                                rd_kafka_buf_write_i8(rkbuf,
                                                      entry->a.operation);
                        else if (entry->a.operation != RD_KAFKA_ALTER_OP_SET) {
                                rd_snprintf(errstr, errstr_size,
                                            "Broker version >= 2.0.0 required "
                                            "for add/delete config "
                                            "entries: only set supported "
                                            "by this broker");
                                rd_kafka_buf_destroy(rkbuf);
                                rd_kafka_replyq_destroy(&replyq);
                                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
                        }
                }
        }

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout + 1000, 0);

        /* validate_only */
        rd_kafka_buf_write_i8(
            rkbuf, rd_kafka_confval_get_int(&options->validate_only));

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Construct and send DescribeConfigsRequest to \p rkb
 *        with the configs (ConfigResource_t*) in \p configs, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t rd_kafka_DescribeConfigsRequest(
    rd_kafka_broker_t *rkb,
    const rd_list_t *configs /*(ConfigResource_t*)*/,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int i;
        const rd_kafka_ConfigResource_t *config;
        int op_timeout;

        if (rd_list_cnt(configs) == 0) {
                rd_snprintf(errstr, errstr_size,
                            "No config resources specified");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_DescribeConfigs, 0, 1, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "DescribeConfigs (KIP-133) not supported "
                            "by broker, requires broker version >= 0.11.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_DescribeConfigs, 1,
                                         rd_list_cnt(configs) * 200);

        /* #resources */
        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(configs));

        RD_LIST_FOREACH(config, configs, i) {
                const rd_kafka_ConfigEntry_t *entry;
                int ei;

                /* resource_type */
                rd_kafka_buf_write_i8(rkbuf, config->restype);

                /* resource_name */
                rd_kafka_buf_write_str(rkbuf, config->name, -1);

                /* #config */
                if (rd_list_empty(&config->config)) {
                        /* Get all configs */
                        rd_kafka_buf_write_i32(rkbuf, -1);
                } else {
                        /* Get requested configs only */
                        rd_kafka_buf_write_i32(rkbuf,
                                               rd_list_cnt(&config->config));
                }

                RD_LIST_FOREACH(entry, &config->config, ei) {
                        /* config_name */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->name, -1);
                }
        }


        if (ApiVersion == 1) {
                /* include_synonyms */
                rd_kafka_buf_write_i8(rkbuf, 1);
        }

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout + 1000, 0);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Construct and send DeleteGroupsRequest to \p rkb
 *        with the groups (DeleteGroup_t *) in \p del_groups, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_DeleteGroupsRequest(rd_kafka_broker_t *rkb,
                             const rd_list_t *del_groups /*(DeleteGroup_t*)*/,
                             rd_kafka_AdminOptions_t *options,
                             char *errstr,
                             size_t errstr_size,
                             rd_kafka_replyq_t replyq,
                             rd_kafka_resp_cb_t *resp_cb,
                             void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;
        int i = 0;
        rd_kafka_DeleteGroup_t *delt;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_DeleteGroups, 0, 1, &features);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "DeleteGroups Admin API (KIP-229) not supported "
                            "by broker, requires broker version >= 1.1.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf =
            rd_kafka_buf_new_request(rkb, RD_KAFKAP_DeleteGroups, 1,
                                     4 + (rd_list_cnt(del_groups) * 100) + 4);

        /* #groups */
        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(del_groups));

        while ((delt = rd_list_elem(del_groups, i++)))
                rd_kafka_buf_write_str(rkbuf, delt->group, -1);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Returns the request size needed to send a specific AclBinding
 *        specified in \p acl, using the ApiVersion provided in
 *        \p ApiVersion.
 *
 * @returns and int16_t with the request size in bytes.
 */
static RD_INLINE size_t
rd_kafka_AclBinding_request_size(const rd_kafka_AclBinding_t *acl,
                                 int ApiVersion) {
        return 1 + 2 + (acl->name ? strlen(acl->name) : 0) + 2 +
               (acl->principal ? strlen(acl->principal) : 0) + 2 +
               (acl->host ? strlen(acl->host) : 0) + 1 + 1 +
               (ApiVersion > 0 ? 1 : 0);
}

/**
 * @brief Construct and send CreateAclsRequest to \p rkb
 *        with the acls (AclBinding_t*) in \p new_acls, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_CreateAclsRequest(rd_kafka_broker_t *rkb,
                           const rd_list_t *new_acls /*(AclBinding_t*)*/,
                           rd_kafka_AdminOptions_t *options,
                           char *errstr,
                           size_t errstr_size,
                           rd_kafka_replyq_t replyq,
                           rd_kafka_resp_cb_t *resp_cb,
                           void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion;
        int i;
        size_t len;
        int op_timeout;
        rd_kafka_AclBinding_t *new_acl;

        if (rd_list_cnt(new_acls) == 0) {
                rd_snprintf(errstr, errstr_size, "No acls to create");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_CreateAcls, 0, 1, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "ACLs Admin API (KIP-140) not supported "
                            "by broker, requires broker version >= 0.11.0.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        if (ApiVersion == 0) {
                RD_LIST_FOREACH(new_acl, new_acls, i) {
                        if (new_acl->resource_pattern_type !=
                            RD_KAFKA_RESOURCE_PATTERN_LITERAL) {
                                rd_snprintf(errstr, errstr_size,
                                            "Broker only supports LITERAL "
                                            "resource pattern types");
                                rd_kafka_replyq_destroy(&replyq);
                                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
                        }
                }
        } else {
                RD_LIST_FOREACH(new_acl, new_acls, i) {
                        if (new_acl->resource_pattern_type !=
                                RD_KAFKA_RESOURCE_PATTERN_LITERAL &&
                            new_acl->resource_pattern_type !=
                                RD_KAFKA_RESOURCE_PATTERN_PREFIXED) {
                                rd_snprintf(errstr, errstr_size,
                                            "Only LITERAL and PREFIXED "
                                            "resource patterns are supported "
                                            "when creating ACLs");
                                rd_kafka_replyq_destroy(&replyq);
                                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
                        }
                }
        }

        len = 4;
        RD_LIST_FOREACH(new_acl, new_acls, i) {
                len += rd_kafka_AclBinding_request_size(new_acl, ApiVersion);
        }

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_CreateAcls, 1, len);

        /* #acls */
        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(new_acls));

        RD_LIST_FOREACH(new_acl, new_acls, i) {
                rd_kafka_buf_write_i8(rkbuf, new_acl->restype);

                rd_kafka_buf_write_str(rkbuf, new_acl->name, -1);

                if (ApiVersion >= 1) {
                        rd_kafka_buf_write_i8(rkbuf,
                                              new_acl->resource_pattern_type);
                }

                rd_kafka_buf_write_str(rkbuf, new_acl->principal, -1);

                rd_kafka_buf_write_str(rkbuf, new_acl->host, -1);

                rd_kafka_buf_write_i8(rkbuf, new_acl->operation);

                rd_kafka_buf_write_i8(rkbuf, new_acl->permission_type);
        }

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout + 1000, 0);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Construct and send DescribeAclsRequest to \p rkb
 *        with the acls (AclBinding_t*) in \p acls, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t rd_kafka_DescribeAclsRequest(
    rd_kafka_broker_t *rkb,
    const rd_list_t *acls /*(rd_kafka_AclBindingFilter_t*)*/,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        const rd_kafka_AclBindingFilter_t *acl;
        int op_timeout;

        if (rd_list_cnt(acls) == 0) {
                rd_snprintf(errstr, errstr_size,
                            "No acl binding filters specified");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }
        if (rd_list_cnt(acls) > 1) {
                rd_snprintf(errstr, errstr_size,
                            "Too many acl binding filters specified");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        acl = rd_list_elem(acls, 0);

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_DescribeAcls, 0, 1, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "ACLs Admin API (KIP-140) not supported "
                            "by broker, requires broker version >= 0.11.0.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        if (ApiVersion == 0) {
                if (acl->resource_pattern_type !=
                        RD_KAFKA_RESOURCE_PATTERN_LITERAL &&
                    acl->resource_pattern_type !=
                        RD_KAFKA_RESOURCE_PATTERN_ANY) {
                        rd_snprintf(errstr, errstr_size,
                                    "Broker only supports LITERAL and ANY "
                                    "resource pattern types");
                        rd_kafka_replyq_destroy(&replyq);
                        return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
                }
        } else {
                if (acl->resource_pattern_type ==
                    RD_KAFKA_RESOURCE_PATTERN_UNKNOWN) {
                        rd_snprintf(errstr, errstr_size,
                                    "Filter contains UNKNOWN elements");
                        rd_kafka_replyq_destroy(&replyq);
                        return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
                }
        }

        rkbuf = rd_kafka_buf_new_request(
            rkb, RD_KAFKAP_DescribeAcls, 1,
            rd_kafka_AclBinding_request_size(acl, ApiVersion));

        /* resource_type */
        rd_kafka_buf_write_i8(rkbuf, acl->restype);

        /* resource_name filter */
        rd_kafka_buf_write_str(rkbuf, acl->name, -1);

        if (ApiVersion > 0) {
                /* resource_pattern_type (rd_kafka_ResourcePatternType_t) */
                rd_kafka_buf_write_i8(rkbuf, acl->resource_pattern_type);
        }

        /* principal filter */
        rd_kafka_buf_write_str(rkbuf, acl->principal, -1);

        /* host filter */
        rd_kafka_buf_write_str(rkbuf, acl->host, -1);

        /* operation (rd_kafka_AclOperation_t) */
        rd_kafka_buf_write_i8(rkbuf, acl->operation);

        /* permission type (rd_kafka_AclPermissionType_t) */
        rd_kafka_buf_write_i8(rkbuf, acl->permission_type);

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout + 1000, 0);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Construct and send DeleteAclsRequest to \p rkb
 *        with the acl filters (AclBindingFilter_t*) in \p del_acls, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_DeleteAclsRequest(rd_kafka_broker_t *rkb,
                           const rd_list_t *del_acls /*(AclBindingFilter_t*)*/,
                           rd_kafka_AdminOptions_t *options,
                           char *errstr,
                           size_t errstr_size,
                           rd_kafka_replyq_t replyq,
                           rd_kafka_resp_cb_t *resp_cb,
                           void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        const rd_kafka_AclBindingFilter_t *acl;
        int op_timeout;
        int i;
        size_t len;

        if (rd_list_cnt(del_acls) == 0) {
                rd_snprintf(errstr, errstr_size,
                            "No acl binding filters specified");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_DeleteAcls, 0, 1, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "ACLs Admin API (KIP-140) not supported "
                            "by broker, requires broker version >= 0.11.0.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        len = 4;

        RD_LIST_FOREACH(acl, del_acls, i) {
                if (ApiVersion == 0) {
                        if (acl->resource_pattern_type !=
                                RD_KAFKA_RESOURCE_PATTERN_LITERAL &&
                            acl->resource_pattern_type !=
                                RD_KAFKA_RESOURCE_PATTERN_ANY) {
                                rd_snprintf(errstr, errstr_size,
                                            "Broker only supports LITERAL "
                                            "and ANY resource pattern types");
                                rd_kafka_replyq_destroy(&replyq);
                                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
                        }
                } else {
                        if (acl->resource_pattern_type ==
                            RD_KAFKA_RESOURCE_PATTERN_UNKNOWN) {
                                rd_snprintf(errstr, errstr_size,
                                            "Filter contains UNKNOWN elements");
                                rd_kafka_replyq_destroy(&replyq);
                                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
                        }
                }

                len += rd_kafka_AclBinding_request_size(acl, ApiVersion);
        }

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_DeleteAcls, 1, len);

        /* #acls */
        rd_kafka_buf_write_i32(rkbuf, rd_list_cnt(del_acls));

        RD_LIST_FOREACH(acl, del_acls, i) {
                /* resource_type */
                rd_kafka_buf_write_i8(rkbuf, acl->restype);

                /* resource_name filter */
                rd_kafka_buf_write_str(rkbuf, acl->name, -1);

                if (ApiVersion > 0) {
                        /* resource_pattern_type
                         * (rd_kafka_ResourcePatternType_t) */
                        rd_kafka_buf_write_i8(rkbuf,
                                              acl->resource_pattern_type);
                }

                /* principal filter */
                rd_kafka_buf_write_str(rkbuf, acl->principal, -1);

                /* host filter */
                rd_kafka_buf_write_str(rkbuf, acl->host, -1);

                /* operation (rd_kafka_AclOperation_t) */
                rd_kafka_buf_write_i8(rkbuf, acl->operation);

                /* permission type (rd_kafka_AclPermissionType_t) */
                rd_kafka_buf_write_i8(rkbuf, acl->permission_type);
        }

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout + 1000, 0);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Parses and handles an InitProducerId reply.
 *
 * @locality rdkafka main thread
 * @locks none
 */
void rd_kafka_handle_InitProducerId(rd_kafka_t *rk,
                                    rd_kafka_broker_t *rkb,
                                    rd_kafka_resp_err_t err,
                                    rd_kafka_buf_t *rkbuf,
                                    rd_kafka_buf_t *request,
                                    void *opaque) {
        const int log_decode_errors = LOG_ERR;
        int16_t error_code;
        rd_kafka_pid_t pid;

        if (err)
                goto err;

        rd_kafka_buf_read_throttle_time(rkbuf);

        rd_kafka_buf_read_i16(rkbuf, &error_code);
        if ((err = error_code))
                goto err;

        rd_kafka_buf_read_i64(rkbuf, &pid.id);
        rd_kafka_buf_read_i16(rkbuf, &pid.epoch);

        rd_kafka_idemp_pid_update(rkb, pid);

        return;

err_parse:
        err = rkbuf->rkbuf_err;
err:
        if (err == RD_KAFKA_RESP_ERR__DESTROY)
                return;

        /* Retries are performed by idempotence state handler */
        rd_kafka_idemp_request_pid_failed(rkb, err);
}

/**
 * @brief Construct and send InitProducerIdRequest to \p rkb.
 *
 * @param transactional_id may be NULL.
 * @param transaction_timeout_ms may be set to -1.
 * @param current_pid the current PID to reset, requires KIP-360. If not NULL
 *                    and KIP-360 is not supported by the broker this function
 *                    will return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE.
 *
 *        The response (unparsed) will be handled by \p resp_cb served
 *        by queue \p replyq.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t
rd_kafka_InitProducerIdRequest(rd_kafka_broker_t *rkb,
                               const char *transactional_id,
                               int transaction_timeout_ms,
                               const rd_kafka_pid_t *current_pid,
                               char *errstr,
                               size_t errstr_size,
                               rd_kafka_replyq_t replyq,
                               rd_kafka_resp_cb_t *resp_cb,
                               void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion;

        if (current_pid) {
                ApiVersion = rd_kafka_broker_ApiVersion_supported(
                    rkb, RD_KAFKAP_InitProducerId, 3, 4, NULL);
                if (ApiVersion == -1) {
                        rd_snprintf(errstr, errstr_size,
                                    "InitProducerId (KIP-360) not supported by "
                                    "broker, requires broker version >= 2.5.0: "
                                    "unable to recover from previous "
                                    "transactional error");
                        rd_kafka_replyq_destroy(&replyq);
                        return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
                }
        } else {
                ApiVersion = rd_kafka_broker_ApiVersion_supported(
                    rkb, RD_KAFKAP_InitProducerId, 0, 4, NULL);

                if (ApiVersion == -1) {
                        rd_snprintf(errstr, errstr_size,
                                    "InitProducerId (KIP-98) not supported by "
                                    "broker, requires broker "
                                    "version >= 0.11.0");
                        rd_kafka_replyq_destroy(&replyq);
                        return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
                }
        }

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_InitProducerId, 1,
            2 + (transactional_id ? strlen(transactional_id) : 0) + 4 + 8 + 4,
            ApiVersion >= 2 /*flexver*/);

        /* transactional_id */
        rd_kafka_buf_write_str(rkbuf, transactional_id, -1);

        /* transaction_timeout_ms */
        rd_kafka_buf_write_i32(rkbuf, transaction_timeout_ms);

        if (ApiVersion >= 3) {
                /* Current PID */
                rd_kafka_buf_write_i64(rkbuf,
                                       current_pid ? current_pid->id : -1);
                /* Current Epoch */
                rd_kafka_buf_write_i16(rkbuf,
                                       current_pid ? current_pid->epoch : -1);
        }

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        /* Let the idempotence state handler perform retries */
        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_NO_RETRIES;

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Construct and send AddPartitionsToTxnRequest to \p rkb.
 *
 *        The response (unparsed) will be handled by \p resp_cb served
 *        by queue \p replyq.
 *
 * @param rktps MUST be sorted by topic name.
 *
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code.
 */
rd_kafka_resp_err_t
rd_kafka_AddPartitionsToTxnRequest(rd_kafka_broker_t *rkb,
                                   const char *transactional_id,
                                   rd_kafka_pid_t pid,
                                   const rd_kafka_toppar_tqhead_t *rktps,
                                   char *errstr,
                                   size_t errstr_size,
                                   rd_kafka_replyq_t replyq,
                                   rd_kafka_resp_cb_t *resp_cb,
                                   void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        rd_kafka_toppar_t *rktp;
        rd_kafka_topic_t *last_rkt = NULL;
        size_t of_TopicCnt;
        ssize_t of_PartCnt = -1;
        int TopicCnt = 0, PartCnt = 0;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_AddPartitionsToTxn, 0, 0, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "AddPartitionsToTxnRequest (KIP-98) not supported "
                            "by broker, requires broker version >= 0.11.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf =
            rd_kafka_buf_new_request(rkb, RD_KAFKAP_AddPartitionsToTxn, 1, 500);

        /* transactional_id */
        rd_kafka_buf_write_str(rkbuf, transactional_id, -1);

        /* PID */
        rd_kafka_buf_write_i64(rkbuf, pid.id);
        rd_kafka_buf_write_i16(rkbuf, pid.epoch);

        /* Topics/partitions array (count updated later) */
        of_TopicCnt = rd_kafka_buf_write_i32(rkbuf, 0);

        TAILQ_FOREACH(rktp, rktps, rktp_txnlink) {
                if (last_rkt != rktp->rktp_rkt) {

                        if (last_rkt) {
                                /* Update last topic's partition count field */
                                rd_kafka_buf_update_i32(rkbuf, of_PartCnt,
                                                        PartCnt);
                                of_PartCnt = -1;
                        }

                        /* Topic name */
                        rd_kafka_buf_write_kstr(rkbuf,
                                                rktp->rktp_rkt->rkt_topic);
                        /* Partition count, updated later */
                        of_PartCnt = rd_kafka_buf_write_i32(rkbuf, 0);

                        PartCnt = 0;
                        TopicCnt++;
                        last_rkt = rktp->rktp_rkt;
                }

                /* Partition id */
                rd_kafka_buf_write_i32(rkbuf, rktp->rktp_partition);
                PartCnt++;
        }

        /* Update last partition and topic count fields */
        if (of_PartCnt != -1)
                rd_kafka_buf_update_i32(rkbuf, (size_t)of_PartCnt, PartCnt);
        rd_kafka_buf_update_i32(rkbuf, of_TopicCnt, TopicCnt);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        /* Let the handler perform retries so that it can pick
         * up more added partitions. */
        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_NO_RETRIES;

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Construct and send AddOffsetsToTxnRequest to \p rkb.
 *
 *        The response (unparsed) will be handled by \p resp_cb served
 *        by queue \p replyq.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code.
 */
rd_kafka_resp_err_t
rd_kafka_AddOffsetsToTxnRequest(rd_kafka_broker_t *rkb,
                                const char *transactional_id,
                                rd_kafka_pid_t pid,
                                const char *group_id,
                                char *errstr,
                                size_t errstr_size,
                                rd_kafka_replyq_t replyq,
                                rd_kafka_resp_cb_t *resp_cb,
                                void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_AddOffsetsToTxn, 0, 0, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "AddOffsetsToTxnRequest (KIP-98) not supported "
                            "by broker, requires broker version >= 0.11.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf =
            rd_kafka_buf_new_request(rkb, RD_KAFKAP_AddOffsetsToTxn, 1, 100);

        /* transactional_id */
        rd_kafka_buf_write_str(rkbuf, transactional_id, -1);

        /* PID */
        rd_kafka_buf_write_i64(rkbuf, pid.id);
        rd_kafka_buf_write_i16(rkbuf, pid.epoch);

        /* Group Id */
        rd_kafka_buf_write_str(rkbuf, group_id, -1);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_MAX_RETRIES;

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



/**
 * @brief Construct and send EndTxnRequest to \p rkb.
 *
 *        The response (unparsed) will be handled by \p resp_cb served
 *        by queue \p replyq.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code.
 */
rd_kafka_resp_err_t rd_kafka_EndTxnRequest(rd_kafka_broker_t *rkb,
                                           const char *transactional_id,
                                           rd_kafka_pid_t pid,
                                           rd_bool_t committed,
                                           char *errstr,
                                           size_t errstr_size,
                                           rd_kafka_replyq_t replyq,
                                           rd_kafka_resp_cb_t *resp_cb,
                                           void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(rkb, RD_KAFKAP_EndTxn,
                                                          0, 1, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "EndTxnRequest (KIP-98) not supported "
                            "by broker, requires broker version >= 0.11.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_EndTxn, 1, 500);

        /* transactional_id */
        rd_kafka_buf_write_str(rkbuf, transactional_id, -1);

        /* PID */
        rd_kafka_buf_write_i64(rkbuf, pid.id);
        rd_kafka_buf_write_i16(rkbuf, pid.epoch);

        /* Committed */
        rd_kafka_buf_write_bool(rkbuf, committed);
        rkbuf->rkbuf_u.EndTxn.commit = committed;

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_MAX_RETRIES;

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



/**
 * @name Unit tests
 * @{
 *
 *
 *
 *
 */

/**
 * @brief Create \p cnt messages, starting at \p msgid, and add them
 *        to \p rkmq.
 *
 * @returns the number of messages added.
 */
static int ut_create_msgs(rd_kafka_msgq_t *rkmq, uint64_t msgid, int cnt) {
        int i;

        for (i = 0; i < cnt; i++) {
                rd_kafka_msg_t *rkm;

                rkm                       = ut_rd_kafka_msg_new(0);
                rkm->rkm_u.producer.msgid = msgid++;
                rkm->rkm_ts_enq           = rd_clock();
                rkm->rkm_ts_timeout = rkm->rkm_ts_enq + (900 * 1000 * 1000);

                rd_kafka_msgq_enq(rkmq, rkm);
        }

        return cnt;
}

/**
 * @brief Idempotent Producer request/response unit tests
 *
 * The current test verifies proper handling of the following case:
 *    Batch 0 succeeds
 *    Batch 1 fails with temporary error
 *    Batch 2,3 fails with out of order sequence
 *    Retry Batch 1-3 should succeed.
 */
static int unittest_idempotent_producer(void) {
        rd_kafka_t *rk;
        rd_kafka_conf_t *conf;
        rd_kafka_broker_t *rkb;
#define _BATCH_CNT      4
#define _MSGS_PER_BATCH 3
        const int msgcnt = _BATCH_CNT * _MSGS_PER_BATCH;
        int remaining_batches;
        uint64_t msgid = 1;
        rd_kafka_toppar_t *rktp;
        rd_kafka_pid_t pid                    = {.id = 1000, .epoch = 0};
        struct rd_kafka_Produce_result result = {.offset    = 1,
                                                 .timestamp = 1000};
        rd_kafka_queue_t *rkqu;
        rd_kafka_event_t *rkev;
        rd_kafka_buf_t *request[_BATCH_CNT];
        int rcnt             = 0;
        int retry_msg_cnt    = 0;
        int drcnt            = 0;
        rd_kafka_msgq_t rkmq = RD_KAFKA_MSGQ_INITIALIZER(rkmq);
        const char *tmp;
        int i, r;

        RD_UT_SAY("Verifying idempotent producer error handling");

        conf = rd_kafka_conf_new();
        rd_kafka_conf_set(conf, "batch.num.messages", "3", NULL, 0);
        rd_kafka_conf_set(conf, "retry.backoff.ms", "1", NULL, 0);
        if ((tmp = rd_getenv("TEST_DEBUG", NULL)))
                rd_kafka_conf_set(conf, "debug", tmp, NULL, 0);
        if (rd_kafka_conf_set(conf, "enable.idempotence", "true", NULL, 0) !=
            RD_KAFKA_CONF_OK)
                RD_UT_FAIL("Failed to enable idempotence");
        rd_kafka_conf_set_events(conf, RD_KAFKA_EVENT_DR);

        rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, NULL, 0);
        RD_UT_ASSERT(rk, "failed to create producer");

        rkqu = rd_kafka_queue_get_main(rk);

        /* We need a broker handle, use a logical broker to avoid
         * any connection attempts. */
        rkb = rd_kafka_broker_add_logical(rk, "unittest");

        /* Have the broker support everything so msgset_writer selects
         * the most up-to-date output features. */
        rd_kafka_broker_lock(rkb);
        rkb->rkb_features = RD_KAFKA_FEATURE_UNITTEST | RD_KAFKA_FEATURE_ALL;
        rd_kafka_broker_unlock(rkb);

        /* Get toppar */
        rktp = rd_kafka_toppar_get2(rk, "uttopic", 0, rd_false, rd_true);
        RD_UT_ASSERT(rktp, "failed to get toppar");

        /* Set the topic as exists so messages are enqueued on
         * the desired rktp away (otherwise UA partition) */
        rd_ut_kafka_topic_set_topic_exists(rktp->rktp_rkt, 1, -1);

        /* Produce messages */
        ut_create_msgs(&rkmq, 1, msgcnt);

        /* Set the pid */
        rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_WAIT_PID);
        rd_kafka_idemp_pid_update(rkb, pid);
        pid = rd_kafka_idemp_get_pid(rk);
        RD_UT_ASSERT(rd_kafka_pid_valid(pid), "PID is invalid");
        rd_kafka_toppar_pid_change(rktp, pid, msgid);

        remaining_batches = _BATCH_CNT;

        /* Create a ProduceRequest for each batch */
        for (rcnt = 0; rcnt < remaining_batches; rcnt++) {
                size_t msize;
                request[rcnt] = rd_kafka_msgset_create_ProduceRequest(
                    rkb, rktp, &rkmq, rd_kafka_idemp_get_pid(rk), 0, &msize);
                RD_UT_ASSERT(request[rcnt], "request #%d failed", rcnt);
        }

        RD_UT_ASSERT(rd_kafka_msgq_len(&rkmq) == 0,
                     "expected input message queue to be empty, "
                     "but still has %d message(s)",
                     rd_kafka_msgq_len(&rkmq));

        /*
         * Mock handling of each request
         */

        /* Batch 0: accepted */
        i = 0;
        r = rd_kafka_msgq_len(&request[i]->rkbuf_batch.msgq);
        RD_UT_ASSERT(r == _MSGS_PER_BATCH, ".");
        rd_kafka_msgbatch_handle_Produce_result(rkb, &request[i]->rkbuf_batch,
                                                RD_KAFKA_RESP_ERR_NO_ERROR,
                                                &result, request[i]);
        result.offset += r;
        RD_UT_ASSERT(rd_kafka_msgq_len(&rktp->rktp_msgq) == 0,
                     "batch %d: expected no messages in rktp_msgq, not %d", i,
                     rd_kafka_msgq_len(&rktp->rktp_msgq));
        rd_kafka_buf_destroy(request[i]);
        remaining_batches--;

        /* Batch 1: fail, triggering retry (re-enq on rktp_msgq) */
        i = 1;
        r = rd_kafka_msgq_len(&request[i]->rkbuf_batch.msgq);
        RD_UT_ASSERT(r == _MSGS_PER_BATCH, ".");
        rd_kafka_msgbatch_handle_Produce_result(
            rkb, &request[i]->rkbuf_batch,
            RD_KAFKA_RESP_ERR_NOT_LEADER_FOR_PARTITION, &result, request[i]);
        retry_msg_cnt += r;
        RD_UT_ASSERT(rd_kafka_msgq_len(&rktp->rktp_msgq) == retry_msg_cnt,
                     "batch %d: expected %d messages in rktp_msgq, not %d", i,
                     retry_msg_cnt, rd_kafka_msgq_len(&rktp->rktp_msgq));
        rd_kafka_buf_destroy(request[i]);

        /* Batch 2: OUT_OF_ORDER, triggering retry .. */
        i = 2;
        r = rd_kafka_msgq_len(&request[i]->rkbuf_batch.msgq);
        RD_UT_ASSERT(r == _MSGS_PER_BATCH, ".");
        rd_kafka_msgbatch_handle_Produce_result(
            rkb, &request[i]->rkbuf_batch,
            RD_KAFKA_RESP_ERR_OUT_OF_ORDER_SEQUENCE_NUMBER, &result,
            request[i]);
        retry_msg_cnt += r;
        RD_UT_ASSERT(rd_kafka_msgq_len(&rktp->rktp_msgq) == retry_msg_cnt,
                     "batch %d: expected %d messages in rktp_xmit_msgq, not %d",
                     i, retry_msg_cnt, rd_kafka_msgq_len(&rktp->rktp_msgq));
        rd_kafka_buf_destroy(request[i]);

        /* Batch 3: OUT_OF_ORDER, triggering retry .. */
        i = 3;
        r = rd_kafka_msgq_len(&request[i]->rkbuf_batch.msgq);
        rd_kafka_msgbatch_handle_Produce_result(
            rkb, &request[i]->rkbuf_batch,
            RD_KAFKA_RESP_ERR_OUT_OF_ORDER_SEQUENCE_NUMBER, &result,
            request[i]);
        retry_msg_cnt += r;
        RD_UT_ASSERT(rd_kafka_msgq_len(&rktp->rktp_msgq) == retry_msg_cnt,
                     "batch %d: expected %d messages in rktp_xmit_msgq, not %d",
                     i, retry_msg_cnt, rd_kafka_msgq_len(&rktp->rktp_msgq));
        rd_kafka_buf_destroy(request[i]);


        /* Retried messages will have been moved to rktp_msgq,
         * move them back to our local queue. */
        rd_kafka_toppar_lock(rktp);
        rd_kafka_msgq_move(&rkmq, &rktp->rktp_msgq);
        rd_kafka_toppar_unlock(rktp);

        RD_UT_ASSERT(rd_kafka_msgq_len(&rkmq) == retry_msg_cnt,
                     "Expected %d messages in retry queue, not %d",
                     retry_msg_cnt, rd_kafka_msgq_len(&rkmq));

        /* Sleep a short while to make sure the retry backoff expires. */
        rd_usleep(5 * 1000, NULL); /* 5ms */

        /*
         * Create requests for remaining batches.
         */
        for (rcnt = 0; rcnt < remaining_batches; rcnt++) {
                size_t msize;
                request[rcnt] = rd_kafka_msgset_create_ProduceRequest(
                    rkb, rktp, &rkmq, rd_kafka_idemp_get_pid(rk), 0, &msize);
                RD_UT_ASSERT(request[rcnt],
                             "Failed to create retry #%d (%d msgs in queue)",
                             rcnt, rd_kafka_msgq_len(&rkmq));
        }

        /*
         * Mock handling of each request, they will now succeed.
         */
        for (i = 0; i < rcnt; i++) {
                r = rd_kafka_msgq_len(&request[i]->rkbuf_batch.msgq);
                rd_kafka_msgbatch_handle_Produce_result(
                    rkb, &request[i]->rkbuf_batch, RD_KAFKA_RESP_ERR_NO_ERROR,
                    &result, request[i]);
                result.offset += r;
                rd_kafka_buf_destroy(request[i]);
        }

        retry_msg_cnt = 0;
        RD_UT_ASSERT(rd_kafka_msgq_len(&rktp->rktp_msgq) == retry_msg_cnt,
                     "batch %d: expected %d messages in rktp_xmit_msgq, not %d",
                     i, retry_msg_cnt, rd_kafka_msgq_len(&rktp->rktp_msgq));

        /*
         * Wait for delivery reports, they should all be successful.
         */
        while ((rkev = rd_kafka_queue_poll(rkqu, 1000))) {
                const rd_kafka_message_t *rkmessage;

                RD_UT_SAY("Got %s event with %d message(s)",
                          rd_kafka_event_name(rkev),
                          (int)rd_kafka_event_message_count(rkev));

                while ((rkmessage = rd_kafka_event_message_next(rkev))) {
                        RD_UT_SAY(" DR for message: %s: (persistence=%d)",
                                  rd_kafka_err2str(rkmessage->err),
                                  rd_kafka_message_status(rkmessage));
                        if (rkmessage->err)
                                RD_UT_WARN(" ^ Should not have failed");
                        else
                                drcnt++;
                }
                rd_kafka_event_destroy(rkev);
        }

        /* Should be no more messages in queues */
        r = rd_kafka_outq_len(rk);
        RD_UT_ASSERT(r == 0, "expected outq to return 0, not %d", r);

        /* Verify the expected number of good delivery reports were seen */
        RD_UT_ASSERT(drcnt == msgcnt, "expected %d DRs, not %d", msgcnt, drcnt);

        rd_kafka_queue_destroy(rkqu);
        rd_kafka_toppar_destroy(rktp);
        rd_kafka_broker_destroy(rkb);
        rd_kafka_destroy(rk);

        RD_UT_PASS();
        return 0;
}

/**
 * @brief Request/response unit tests
 */
int unittest_request(void) {
        int fails = 0;

        fails += unittest_idempotent_producer();

        return fails;
}

/**@}*/
