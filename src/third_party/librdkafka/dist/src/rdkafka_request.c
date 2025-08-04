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

#include <stdarg.h>

#include "rdkafka_int.h"
#include "rdkafka_request.h"
#include "rdkafka_broker.h"
#include "rdkafka_offset.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_metadata.h"
#include "rdkafka_telemetry.h"
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
        case RD_KAFKA_RESP_ERR__SSL:
        case RD_KAFKA_RESP_ERR__TIMED_OUT:
        case RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT:
        case RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS_AFTER_APPEND:
                actions |= RD_KAFKA_ERR_ACTION_RETRY |
                           RD_KAFKA_ERR_ACTION_MSG_POSSIBLY_PERSISTED;
                break;

        case RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS:
        case RD_KAFKA_RESP_ERR_INVALID_MSG:
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
 * @param fields An array of fields to read from the buffer and set on
 *               the rktpar object, in the specified order, must end
 *               with RD_KAFKA_TOPIC_PARTITION_FIELD_END.
 *
 * @returns a newly allocated list on success, or NULL on parse error.
 */
rd_kafka_topic_partition_list_t *rd_kafka_buf_read_topic_partitions(
    rd_kafka_buf_t *rkbuf,
    rd_bool_t use_topic_id,
    rd_bool_t use_topic_name,
    size_t estimated_part_cnt,
    const rd_kafka_topic_partition_field_t *fields) {
        const int log_decode_errors = LOG_ERR;
        int32_t TopicArrayCnt;
        rd_kafka_topic_partition_list_t *parts = NULL;

        /* We assume here that the topic partition list is not NULL.
         * FIXME: check NULL topic array case, if required in future. */

        rd_kafka_buf_read_arraycnt(rkbuf, &TopicArrayCnt, RD_KAFKAP_TOPICS_MAX);

        parts = rd_kafka_topic_partition_list_new(
            RD_MAX(TopicArrayCnt * 4, (int)estimated_part_cnt));

        while (TopicArrayCnt-- > 0) {
                rd_kafkap_str_t kTopic;
                int32_t PartArrayCnt;
                char *topic = NULL;
                rd_kafka_Uuid_t topic_id;

                if (use_topic_id) {
                        rd_kafka_buf_read_uuid(rkbuf, &topic_id);
                }
                if (use_topic_name) {
                        rd_kafka_buf_read_str(rkbuf, &kTopic);
                        RD_KAFKAP_STR_DUPA(&topic, &kTopic);
                }

                rd_kafka_buf_read_arraycnt(rkbuf, &PartArrayCnt,
                                           RD_KAFKAP_PARTITIONS_MAX);


                while (PartArrayCnt-- > 0) {
                        int32_t Partition = -1, Epoch = -1234,
                                CurrentLeaderEpoch = -1234;
                        int64_t Offset             = -1234;
                        int16_t ErrorCode          = 0;
                        rd_kafka_topic_partition_t *rktpar;
                        int fi;

                        /*
                         * Read requested fields
                         */
                        for (fi = 0;
                             fields[fi] != RD_KAFKA_TOPIC_PARTITION_FIELD_END;
                             fi++) {
                                switch (fields[fi]) {
                                case RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION:
                                        rd_kafka_buf_read_i32(rkbuf,
                                                              &Partition);
                                        break;
                                case RD_KAFKA_TOPIC_PARTITION_FIELD_OFFSET:
                                        rd_kafka_buf_read_i64(rkbuf, &Offset);
                                        break;
                                case RD_KAFKA_TOPIC_PARTITION_FIELD_CURRENT_EPOCH:
                                        rd_kafka_buf_read_i32(
                                            rkbuf, &CurrentLeaderEpoch);
                                        break;
                                case RD_KAFKA_TOPIC_PARTITION_FIELD_EPOCH:
                                        rd_kafka_buf_read_i32(rkbuf, &Epoch);
                                        break;
                                case RD_KAFKA_TOPIC_PARTITION_FIELD_ERR:
                                        rd_kafka_buf_read_i16(rkbuf,
                                                              &ErrorCode);
                                        break;
                                case RD_KAFKA_TOPIC_PARTITION_FIELD_METADATA:
                                        rd_assert(!*"metadata not implemented");
                                        break;
                                case RD_KAFKA_TOPIC_PARTITION_FIELD_TIMESTAMP:
                                        rd_assert(
                                            !*"timestamp not implemented");
                                        break;
                                case RD_KAFKA_TOPIC_PARTITION_FIELD_NOOP:
                                        /* Fallback */
                                case RD_KAFKA_TOPIC_PARTITION_FIELD_END:
                                        break;
                                }
                        }

                        if (use_topic_id) {
                                rktpar =
                                    rd_kafka_topic_partition_list_add_with_topic_id(
                                        parts, topic_id, Partition);
                                if (use_topic_name)
                                        rktpar->topic = rd_strdup(topic);
                        } else if (use_topic_name) {
                                rktpar = rd_kafka_topic_partition_list_add(
                                    parts, topic, Partition);
                        } else {
                                rd_assert(!*"one of use_topic_id and "
                                          "use_topic_name should be true");
                        }

                        /* Use dummy sentinel values that are unlikely to be
                         * seen from the broker to know if we are to set these
                         * fields or not. */
                        if (Offset != -1234)
                                rktpar->offset = Offset;
                        if (Epoch != -1234)
                                rd_kafka_topic_partition_set_leader_epoch(
                                    rktpar, Epoch);
                        if (CurrentLeaderEpoch != -1234)
                                rd_kafka_topic_partition_set_current_leader_epoch(
                                    rktpar, CurrentLeaderEpoch);
                        rktpar->err = ErrorCode;

                        if (fi > 1)
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
 * @remark The \p parts list MUST be sorted by name if use_topic_id is false or
 * by id.
 */
int rd_kafka_buf_write_topic_partitions(
    rd_kafka_buf_t *rkbuf,
    const rd_kafka_topic_partition_list_t *parts,
    rd_bool_t skip_invalid_offsets,
    rd_bool_t only_invalid_offsets,
    rd_bool_t use_topic_id,
    rd_bool_t use_topic_name,
    const rd_kafka_topic_partition_field_t *fields) {
        size_t of_TopicArrayCnt;
        size_t of_PartArrayCnt = 0;
        int TopicArrayCnt = 0, PartArrayCnt = 0;
        int i;
        const rd_kafka_topic_partition_t *prev_topic = NULL;
        int cnt                                      = 0;

        rd_assert(!only_invalid_offsets ||
                  (only_invalid_offsets != skip_invalid_offsets));

        /* TopicArrayCnt */
        of_TopicArrayCnt = rd_kafka_buf_write_arraycnt_pos(rkbuf);

        for (i = 0; i < parts->cnt; i++) {
                const rd_kafka_topic_partition_t *rktpar = &parts->elems[i];
                rd_bool_t different_topics;
                int fi;

                if (rktpar->offset < 0) {
                        if (skip_invalid_offsets)
                                continue;
                } else if (only_invalid_offsets)
                        continue;

                if (use_topic_id) {
                        different_topics =
                            !prev_topic ||
                            rd_kafka_Uuid_cmp(
                                rd_kafka_topic_partition_get_topic_id(rktpar),
                                rd_kafka_topic_partition_get_topic_id(
                                    prev_topic));
                } else {
                        different_topics =
                            !prev_topic ||
                            strcmp(rktpar->topic, prev_topic->topic);
                }
                if (different_topics) {
                        /* Finish previous topic, if any. */
                        if (of_PartArrayCnt > 0) {
                                rd_kafka_buf_finalize_arraycnt(
                                    rkbuf, of_PartArrayCnt, PartArrayCnt);
                                /* Tags for previous topic struct */
                                rd_kafka_buf_write_tags_empty(rkbuf);
                        }


                        /* Topic */
                        if (use_topic_name)
                                rd_kafka_buf_write_str(rkbuf, rktpar->topic,
                                                       -1);
                        if (use_topic_id) {
                                rd_kafka_Uuid_t topic_id =
                                    rd_kafka_topic_partition_get_topic_id(
                                        rktpar);
                                rd_kafka_buf_write_uuid(rkbuf, &topic_id);
                        }

                        TopicArrayCnt++;
                        prev_topic = rktpar;
                        /* New topic so reset partition count */
                        PartArrayCnt = 0;

                        /* PartitionArrayCnt: updated later */
                        of_PartArrayCnt =
                            rd_kafka_buf_write_arraycnt_pos(rkbuf);
                }


                /*
                 * Write requested fields
                 */
                for (fi = 0; fields[fi] != RD_KAFKA_TOPIC_PARTITION_FIELD_END;
                     fi++) {
                        switch (fields[fi]) {
                        case RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION:
                                rd_kafka_buf_write_i32(rkbuf,
                                                       rktpar->partition);
                                break;
                        case RD_KAFKA_TOPIC_PARTITION_FIELD_OFFSET:
                                rd_kafka_buf_write_i64(rkbuf, rktpar->offset);
                                break;
                        case RD_KAFKA_TOPIC_PARTITION_FIELD_CURRENT_EPOCH:
                                rd_kafka_buf_write_i32(
                                    rkbuf,
                                    rd_kafka_topic_partition_get_current_leader_epoch(
                                        rktpar));
                                break;
                        case RD_KAFKA_TOPIC_PARTITION_FIELD_EPOCH:
                                rd_kafka_buf_write_i32(
                                    rkbuf,
                                    rd_kafka_topic_partition_get_leader_epoch(
                                        rktpar));
                                break;
                        case RD_KAFKA_TOPIC_PARTITION_FIELD_ERR:
                                rd_kafka_buf_write_i16(rkbuf, rktpar->err);
                                break;
                        case RD_KAFKA_TOPIC_PARTITION_FIELD_TIMESTAMP:
                                /* Current implementation is just
                                 * sending a NULL value */
                                rd_kafka_buf_write_i64(rkbuf, -1);
                                break;
                        case RD_KAFKA_TOPIC_PARTITION_FIELD_METADATA:
                                /* Java client 0.9.0 and broker <0.10.0 can't
                                 * parse Null metadata fields, so as a
                                 * workaround we send an empty string if
                                 * it's Null. */
                                if (!rktpar->metadata)
                                        rd_kafka_buf_write_str(rkbuf, "", 0);
                                else
                                        rd_kafka_buf_write_str(
                                            rkbuf, rktpar->metadata,
                                            rktpar->metadata_size);
                                break;
                        case RD_KAFKA_TOPIC_PARTITION_FIELD_NOOP:
                                break;
                        case RD_KAFKA_TOPIC_PARTITION_FIELD_END:
                                break;
                        }
                }


                if (fi > 1)
                        /* If there was more than one field written
                         * then this was a struct and thus needs the
                         * struct suffix tags written. */
                        rd_kafka_buf_write_tags_empty(rkbuf);

                PartArrayCnt++;
                cnt++;
        }

        if (of_PartArrayCnt > 0) {
                rd_kafka_buf_finalize_arraycnt(rkbuf, of_PartArrayCnt,
                                               PartArrayCnt);
                /* Tags for topic struct */
                rd_kafka_buf_write_tags_empty(rkbuf);
        }

        rd_kafka_buf_finalize_arraycnt(rkbuf, of_TopicArrayCnt, TopicArrayCnt);

        return cnt;
}


/**
 * @brief Read current leader from \p rkbuf.
 *
 * @param rkbuf buffer to read from
 * @param CurrentLeader is the CurrentLeader to populate.
 *
 * @return 1 on success, else -1 on parse error.
 */
int rd_kafka_buf_read_CurrentLeader(rd_kafka_buf_t *rkbuf,
                                    rd_kafkap_CurrentLeader_t *CurrentLeader) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_buf_read_i32(rkbuf, &CurrentLeader->LeaderId);
        rd_kafka_buf_read_i32(rkbuf, &CurrentLeader->LeaderEpoch);
        rd_kafka_buf_skip_tags(rkbuf);
        return 1;
err_parse:
        return -1;
}

/**
 * @brief Read NodeEndpoints from \p rkbuf.
 *
 * @param rkbuf buffer to read from
 * @param NodeEndpoints is the NodeEndpoints to populate.
 *
 * @return 1 on success, else -1 on parse error.
 */
int rd_kafka_buf_read_NodeEndpoints(rd_kafka_buf_t *rkbuf,
                                    rd_kafkap_NodeEndpoints_t *NodeEndpoints) {
        const int log_decode_errors = LOG_ERR;
        int32_t i;
        rd_kafka_buf_read_arraycnt(rkbuf, &NodeEndpoints->NodeEndpointCnt,
                                   RD_KAFKAP_BROKERS_MAX);
        rd_dassert(!NodeEndpoints->NodeEndpoints);
        NodeEndpoints->NodeEndpoints =
            rd_calloc(NodeEndpoints->NodeEndpointCnt,
                      sizeof(*NodeEndpoints->NodeEndpoints));

        for (i = 0; i < NodeEndpoints->NodeEndpointCnt; i++) {
                rd_kafka_buf_read_i32(rkbuf,
                                      &NodeEndpoints->NodeEndpoints[i].NodeId);
                rd_kafka_buf_read_str(rkbuf,
                                      &NodeEndpoints->NodeEndpoints[i].Host);
                rd_kafka_buf_read_i32(rkbuf,
                                      &NodeEndpoints->NodeEndpoints[i].Port);
                rd_kafka_buf_read_str(rkbuf,
                                      &NodeEndpoints->NodeEndpoints[i].Rack);
                rd_kafka_buf_skip_tags(rkbuf);
        }
        return 1;
err_parse:
        return -1;
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
 * @struct rd_kafka_ListOffsetRequest_parameters_s
 * @brief parameters for the rd_kafka_make_ListOffsetsRequest function.
 */
typedef struct rd_kafka_ListOffsetRequest_parameters_s {
        /** Partitions to request offsets for. */
        rd_kafka_topic_partition_list_t *rktpars;
        /** Isolation level. */
        rd_kafka_IsolationLevel_t isolation_level;
        /** Error string (optional). */
        char *errstr;
        /** Error string size (optional). */
        size_t errstr_size;
} rd_kafka_ListOffsetRequest_parameters_t;


static rd_kafka_ListOffsetRequest_parameters_t
rd_kafka_ListOffsetRequest_parameters_make(
    rd_kafka_topic_partition_list_t *rktpars,
    rd_kafka_IsolationLevel_t isolation_level,
    char *errstr,
    size_t errstr_size) {
        rd_kafka_ListOffsetRequest_parameters_t params = RD_ZERO_INIT;
        params.rktpars                                 = rktpars;
        params.isolation_level                         = isolation_level;
        params.errstr                                  = errstr;
        params.errstr_size                             = errstr_size;
        return params;
}

static rd_kafka_ListOffsetRequest_parameters_t *
rd_kafka_ListOffsetRequest_parameters_new(
    rd_kafka_topic_partition_list_t *rktpars,
    rd_kafka_IsolationLevel_t isolation_level,
    char *errstr,
    size_t errstr_size) {
        rd_kafka_ListOffsetRequest_parameters_t *params =
            rd_calloc(1, sizeof(*params));
        *params = rd_kafka_ListOffsetRequest_parameters_make(
            rktpars, isolation_level, errstr, errstr_size);
        return params;
}

static void rd_kafka_ListOffsetRequest_parameters_destroy_free(void *opaque) {
        rd_kafka_ListOffsetRequest_parameters_t *parameters = opaque;
        RD_IF_FREE(parameters->rktpars, rd_kafka_topic_partition_list_destroy);
        RD_IF_FREE(parameters->errstr, rd_free);
        rd_free(parameters);
}

static rd_kafka_buf_t *
rd_kafka_ListOffsetRequest_buf_new(rd_kafka_broker_t *rkb,
                                   rd_kafka_topic_partition_list_t *rktpars) {
        return rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_ListOffsets, 1,
            /* ReplicaId+IsolationLevel+TopicArrayCnt+Topic */
            4 + 1 + 4 + 100 +
                /* PartArrayCnt */
                4 +
                /* partition_cnt * Partition+Time+MaxNumOffs */
                (rktpars->cnt * (4 + 8 + 4)),
            rd_false);
}

/**
 * @brief Parses a ListOffsets reply.
 *
 * Returns the parsed offsets (and errors) in \p offsets which must have been
 * initialized by caller. If \p result_info is passed instead,
 * it's populated with rd_kafka_ListOffsetsResultInfo_t instances.
 *
 * Either \p offsets or \p result_info must be passed.
 * and the one that is passed is populated.
 *
 * @returns 0 on success, else an error (\p offsets may be completely or
 *          partially updated, depending on the nature of the error, and per
 *          partition error codes should be checked by the caller).
 */
rd_kafka_resp_err_t
rd_kafka_parse_ListOffsets(rd_kafka_buf_t *rkbuf,
                           rd_kafka_topic_partition_list_t *offsets,
                           rd_list_t *result_infos) {
        const int log_decode_errors = LOG_ERR;
        int32_t TopicArrayCnt;
        int16_t api_version;
        rd_kafka_resp_err_t all_err = RD_KAFKA_RESP_ERR_NO_ERROR;
        rd_bool_t return_result_infos;
        rd_assert((offsets != NULL) ^ (result_infos != NULL));
        return_result_infos = result_infos != NULL;

        api_version = rkbuf->rkbuf_reqhdr.ApiVersion;

        if (api_version >= 2)
                rd_kafka_buf_read_throttle_time(rkbuf);

        /* NOTE:
         * Broker may return offsets in a different constellation than
         * in the original request .*/

        rd_kafka_buf_read_arraycnt(rkbuf, &TopicArrayCnt, RD_KAFKAP_TOPICS_MAX);
        while (TopicArrayCnt-- > 0) {
                rd_kafkap_str_t Topic;
                int32_t PartArrayCnt;
                char *topic_name;

                rd_kafka_buf_read_str(rkbuf, &Topic);
                rd_kafka_buf_read_arraycnt(rkbuf, &PartArrayCnt,
                                           RD_KAFKAP_PARTITIONS_MAX);

                RD_KAFKAP_STR_DUPA(&topic_name, &Topic);

                while (PartArrayCnt-- > 0) {
                        int32_t Partition;
                        int16_t ErrorCode;
                        int32_t OffsetArrayCnt;
                        int64_t Offset      = -1;
                        int32_t LeaderEpoch = -1;
                        int64_t Timestamp   = -1;
                        rd_kafka_topic_partition_t *rktpar;

                        rd_kafka_buf_read_i32(rkbuf, &Partition);
                        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

                        if (api_version >= 1) {
                                rd_kafka_buf_read_i64(rkbuf, &Timestamp);
                                rd_kafka_buf_read_i64(rkbuf, &Offset);
                                if (api_version >= 4)
                                        rd_kafka_buf_read_i32(rkbuf,
                                                              &LeaderEpoch);
                                rd_kafka_buf_skip_tags(rkbuf);
                        } else if (api_version == 0) {
                                rd_kafka_buf_read_i32(rkbuf, &OffsetArrayCnt);
                                /* We only request one offset so just grab
                                 * the first one. */
                                while (OffsetArrayCnt-- > 0)
                                        rd_kafka_buf_read_i64(rkbuf, &Offset);
                        } else {
                                RD_NOTREACHED();
                        }

                        if (likely(!return_result_infos)) {
                                rktpar = rd_kafka_topic_partition_list_add(
                                    offsets, topic_name, Partition);
                                rktpar->err    = ErrorCode;
                                rktpar->offset = Offset;
                                rd_kafka_topic_partition_set_leader_epoch(
                                    rktpar, LeaderEpoch);
                        } else {
                                rktpar = rd_kafka_topic_partition_new(
                                    topic_name, Partition);
                                rktpar->err    = ErrorCode;
                                rktpar->offset = Offset;
                                rd_kafka_topic_partition_set_leader_epoch(
                                    rktpar, LeaderEpoch);
                                rd_kafka_ListOffsetsResultInfo_t *result_info =
                                    rd_kafka_ListOffsetsResultInfo_new(
                                        rktpar, Timestamp);
                                rd_list_add(result_infos, result_info);
                                rd_kafka_topic_partition_destroy(rktpar);
                        }

                        if (ErrorCode && !all_err)
                                all_err = ErrorCode;
                }

                rd_kafka_buf_skip_tags(rkbuf);
        }

        return all_err;

err_parse:
        return rkbuf->rkbuf_err;
}

/**
 * @brief Async maker for ListOffsetsRequest.
 */
static rd_kafka_resp_err_t
rd_kafka_make_ListOffsetsRequest(rd_kafka_broker_t *rkb,
                                 rd_kafka_buf_t *rkbuf,
                                 void *make_opaque) {
        rd_kafka_ListOffsetRequest_parameters_t *parameters = make_opaque;
        const rd_kafka_topic_partition_list_t *partitions = parameters->rktpars;
        int isolation_level = parameters->isolation_level;
        char *errstr        = parameters->errstr;
        size_t errstr_size  = parameters->errstr_size;
        int i;
        size_t of_TopicArrayCnt = 0, of_PartArrayCnt = 0;
        const char *last_topic = "";
        int32_t topic_cnt = 0, part_cnt = 0;
        int16_t ApiVersion;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_ListOffsets, 0, 7, NULL);
        if (ApiVersion == -1) {
                if (errstr) {
                        rd_snprintf(
                            errstr, errstr_size,
                            "ListOffsets (KIP-396) not supported "
                            "by broker, requires broker version >= 2.5.0");
                }
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        if (ApiVersion >= 6) {
                rd_kafka_buf_upgrade_flexver_request(rkbuf);
        }

        /* ReplicaId */
        rd_kafka_buf_write_i32(rkbuf, -1);

        /* IsolationLevel */
        if (ApiVersion >= 2)
                rd_kafka_buf_write_i8(rkbuf, isolation_level);

        /* TopicArrayCnt */
        of_TopicArrayCnt =
            rd_kafka_buf_write_arraycnt_pos(rkbuf); /* updated later */

        for (i = 0; i < partitions->cnt; i++) {
                const rd_kafka_topic_partition_t *rktpar =
                    &partitions->elems[i];

                if (strcmp(rktpar->topic, last_topic)) {
                        /* Finish last topic, if any. */
                        if (of_PartArrayCnt > 0) {
                                rd_kafka_buf_finalize_arraycnt(
                                    rkbuf, of_PartArrayCnt, part_cnt);
                                /* Topics tags */
                                rd_kafka_buf_write_tags_empty(rkbuf);
                        }

                        /* Topic */
                        rd_kafka_buf_write_str(rkbuf, rktpar->topic, -1);
                        topic_cnt++;
                        last_topic = rktpar->topic;
                        /* New topic so reset partition count */
                        part_cnt = 0;

                        /* PartitionArrayCnt: updated later */
                        of_PartArrayCnt =
                            rd_kafka_buf_write_arraycnt_pos(rkbuf);
                }

                /* Partition */
                rd_kafka_buf_write_i32(rkbuf, rktpar->partition);
                part_cnt++;

                if (ApiVersion >= 4)
                        /* CurrentLeaderEpoch */
                        rd_kafka_buf_write_i32(
                            rkbuf,
                            rd_kafka_topic_partition_get_current_leader_epoch(
                                rktpar));

                /* Time/Offset */
                rd_kafka_buf_write_i64(rkbuf, rktpar->offset);

                if (ApiVersion == 0) {
                        /* MaxNumberOfOffsets */
                        rd_kafka_buf_write_i32(rkbuf, 1);
                }

                /* Partitions tags */
                rd_kafka_buf_write_tags_empty(rkbuf);
        }

        if (of_PartArrayCnt > 0) {
                rd_kafka_buf_finalize_arraycnt(rkbuf, of_PartArrayCnt,
                                               part_cnt);
                /* Topics tags */
                rd_kafka_buf_write_tags_empty(rkbuf);
        }
        rd_kafka_buf_finalize_arraycnt(rkbuf, of_TopicArrayCnt, topic_cnt);

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
 *        Set absolute timeout \p timeout_ms if >= 0.
 */
void rd_kafka_ListOffsetsRequest(rd_kafka_broker_t *rkb,
                                 rd_kafka_topic_partition_list_t *partitions,
                                 rd_kafka_replyq_t replyq,
                                 rd_kafka_resp_cb_t *resp_cb,
                                 int timeout_ms,
                                 void *opaque) {
        rd_kafka_buf_t *rkbuf;
        rd_kafka_topic_partition_list_t *rktpars;
        rd_kafka_ListOffsetRequest_parameters_t *params;

        rktpars = rd_kafka_topic_partition_list_copy(partitions);
        rd_kafka_topic_partition_list_sort_by_topic(rktpars);

        params = rd_kafka_ListOffsetRequest_parameters_new(
            rktpars,
            (rd_kafka_IsolationLevel_t)rkb->rkb_rk->rk_conf.isolation_level,
            NULL, 0);

        rkbuf = rd_kafka_ListOffsetRequest_buf_new(rkb, partitions);

        if (timeout_ms >= 0)
                rd_kafka_buf_set_abs_timeout(rkbuf, timeout_ms, 0);

        /* Postpone creating the request contents until time to send,
         * at which time the ApiVersion is known. */
        rd_kafka_buf_set_maker(
            rkbuf, rd_kafka_make_ListOffsetsRequest, params,
            rd_kafka_ListOffsetRequest_parameters_destroy_free);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}

/**
 * @brief Send ListOffsetsRequest for offsets contained in the first
 *        element of  \p offsets, that is a rd_kafka_topic_partition_list_t.
 *        AdminClient compatible request callback.
 */
rd_kafka_resp_err_t rd_kafka_ListOffsetsRequest_admin(
    rd_kafka_broker_t *rkb,
    const rd_list_t *offsets /* rd_kafka_topic_partition_list_t*/,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque) {
        rd_kafka_ListOffsetRequest_parameters_t params;
        rd_kafka_IsolationLevel_t isolation_level;
        rd_kafka_topic_partition_list_t *topic_partitions;
        rd_kafka_buf_t *rkbuf;
        rd_kafka_resp_err_t err;
        topic_partitions = rd_list_elem(offsets, 0);

        isolation_level = RD_KAFKA_ISOLATION_LEVEL_READ_UNCOMMITTED;
        if (options && options->isolation_level.u.INT.v)
                isolation_level = options->isolation_level.u.INT.v;

        params = rd_kafka_ListOffsetRequest_parameters_make(
            topic_partitions, isolation_level, errstr, errstr_size);

        rkbuf = rd_kafka_ListOffsetRequest_buf_new(rkb, topic_partitions);

        err = rd_kafka_make_ListOffsetsRequest(rkb, rkbuf, &params);

        if (err) {
                rd_kafka_buf_destroy(rkbuf);
                rd_kafka_replyq_destroy(&replyq);
                return err;
        }

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
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

        if (!err) {
                err = rd_kafka_parse_ListOffsets(rkbuf, offsets, NULL);
        }
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

            RD_KAFKA_ERR_ACTION_REFRESH | RD_KAFKA_ERR_ACTION_RETRY,
            RD_KAFKA_RESP_ERR_UNKNOWN_LEADER_EPOCH,

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
 * @brief OffsetForLeaderEpochResponse handler.
 */
rd_kafka_resp_err_t rd_kafka_handle_OffsetForLeaderEpoch(
    rd_kafka_t *rk,
    rd_kafka_broker_t *rkb,
    rd_kafka_resp_err_t err,
    rd_kafka_buf_t *rkbuf,
    rd_kafka_buf_t *request,
    rd_kafka_topic_partition_list_t **offsets) {
        const int log_decode_errors = LOG_ERR;
        int16_t ApiVersion;

        if (err)
                goto err;

        ApiVersion = rkbuf->rkbuf_reqhdr.ApiVersion;

        if (ApiVersion >= 2)
                rd_kafka_buf_read_throttle_time(rkbuf);

        const rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_ERR,
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            ApiVersion >= 1 ? RD_KAFKA_TOPIC_PARTITION_FIELD_EPOCH
                            : RD_KAFKA_TOPIC_PARTITION_FIELD_NOOP,
            RD_KAFKA_TOPIC_PARTITION_FIELD_OFFSET,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};
        *offsets = rd_kafka_buf_read_topic_partitions(
            rkbuf, rd_false /*don't use topic_id*/, rd_true, 0, fields);
        if (!*offsets)
                goto err_parse;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err:
        return err;

err_parse:
        err = rkbuf->rkbuf_err;
        goto err;
}


/**
 * @brief Send OffsetForLeaderEpochRequest for partition(s).
 *
 */
void rd_kafka_OffsetForLeaderEpochRequest(
    rd_kafka_broker_t *rkb,
    rd_kafka_topic_partition_list_t *parts,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_OffsetForLeaderEpoch, 2, 2, NULL);
        /* If the supported ApiVersions are not yet known,
         * or this broker doesn't support it, we let this request
         * succeed or fail later from the broker thread where the
         * version is checked again. */
        if (ApiVersion == -1)
                ApiVersion = 2;

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_OffsetForLeaderEpoch, 1, 4 + (parts->cnt * 64),
            ApiVersion >= 4 /*flexver*/);

        /* Sort partitions by topic */
        rd_kafka_topic_partition_list_sort_by_topic(parts);

        /* Write partition list */
        const rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            /* CurrentLeaderEpoch */
            RD_KAFKA_TOPIC_PARTITION_FIELD_CURRENT_EPOCH,
            /* LeaderEpoch */
            RD_KAFKA_TOPIC_PARTITION_FIELD_EPOCH,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};
        rd_kafka_buf_write_topic_partitions(
            rkbuf, parts, rd_false /*include invalid offsets*/,
            rd_false /*skip valid offsets*/, rd_false /*don't use topic id*/,
            rd_true /*use topic name*/, fields);

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        /* Let caller perform retries */
        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_NO_RETRIES;

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
        int32_t GroupArrayCnt;
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

        if (ApiVersion >= 8) {
                rd_kafkap_str_t group_id;
                // Currently we are supporting only 1 group
                rd_kafka_buf_read_arraycnt(rkbuf, &GroupArrayCnt, 1);
                rd_kafka_buf_read_str(rkbuf, &group_id);
        }

        if (!*offsets)
                *offsets = rd_kafka_topic_partition_list_new(16);

        /* Set default offset for all partitions. */
        rd_kafka_topic_partition_list_set_offsets(rkb->rkb_rk, *offsets, 0,
                                                  RD_KAFKA_OFFSET_INVALID,
                                                  0 /* !is commit */);

        rd_kafka_buf_read_arraycnt(rkbuf, &TopicArrayCnt, RD_KAFKAP_TOPICS_MAX);
        for (i = 0; i < TopicArrayCnt; i++) {
                rd_kafkap_str_t topic;
                rd_kafka_Uuid_t *topic_id = NULL;
                int32_t PartArrayCnt;
                char *topic_name;
                int j;

                rd_kafka_buf_read_str(rkbuf, &topic);
                //                if(ApiVersion >= 9) {
                //                        topic_id = rd_kafka_Uuid_new();
                //                        rd_kafka_buf_read_uuid(rkbuf,
                //                        topic_id);
                //                }
                rd_kafka_buf_read_arraycnt(rkbuf, &PartArrayCnt,
                                           RD_KAFKAP_PARTITIONS_MAX);

                RD_KAFKAP_STR_DUPA(&topic_name, &topic);

                for (j = 0; j < PartArrayCnt; j++) {
                        int32_t partition;
                        rd_kafka_toppar_t *rktp;
                        rd_kafka_topic_partition_t *rktpar;
                        int32_t LeaderEpoch = -1;
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
                        if (!rktpar && add_part) {
                                if (topic_id) {
                                        rktpar =
                                            rd_kafka_topic_partition_list_add_with_topic_id(
                                                *offsets, *topic_id, partition);
                                } else {
                                        rktpar =
                                            rd_kafka_topic_partition_list_add(
                                                *offsets, topic_name,
                                                partition);
                                }
                        } else if (!rktpar) {
                                rd_rkb_dbg(rkb, TOPIC, "OFFSETFETCH",
                                           "OffsetFetchResponse: %s [%" PRId32
                                           "] "
                                           "not found in local list: ignoring",
                                           topic_name, partition);
                                continue;
                        }

                        seen_cnt++;

                        rktp = rd_kafka_topic_partition_get_toppar(
                            rk, rktpar, rd_false /*no create on miss*/);

                        /* broker reports invalid offset as -1 */
                        if (offset == -1)
                                rktpar->offset = RD_KAFKA_OFFSET_INVALID;
                        else
                                rktpar->offset = offset;

                        rd_kafka_topic_partition_set_leader_epoch(rktpar,
                                                                  LeaderEpoch);
                        rktpar->err = err2;

                        rd_rkb_dbg(rkb, TOPIC, "OFFSETFETCH",
                                   "OffsetFetchResponse: %s [%" PRId32
                                   "] "
                                   "offset %" PRId64 ", leader epoch %" PRId32
                                   ", metadata %d byte(s): %s",
                                   topic_name, partition, offset, LeaderEpoch,
                                   RD_KAFKAP_STR_LEN(&metadata),
                                   rd_kafka_err2name(rktpar->err));

                        if (update_toppar && !err2 && rktp) {
                                /* Update toppar's committed offset */
                                rd_kafka_toppar_lock(rktp);
                                rktp->rktp_committed_pos =
                                    rd_kafka_topic_partition_get_fetch_pos(
                                        rktpar);
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

                        /* Loose ref from get_toppar() */
                        if (rktp)
                                rd_kafka_toppar_destroy(rktp);

                        RD_IF_FREE(topic_id, rd_kafka_Uuid_destroy);
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
                    /* Allow retries if replyq is valid */
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
 * FIXME: Even though the version is upgraded to v9, currently we support
 *        only a single group.
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
                                 rd_bool_t use_topic_id,
                                 int32_t generation_id_or_member_epoch,
                                 rd_kafkap_str_t *member_id,
                                 rd_bool_t require_stable_offsets,
                                 int timeout,
                                 rd_kafka_replyq_t replyq,
                                 void (*resp_cb)(rd_kafka_t *,
                                                 rd_kafka_broker_t *,
                                                 rd_kafka_resp_err_t,
                                                 rd_kafka_buf_t *,
                                                 rd_kafka_buf_t *,
                                                 void *),
                                 void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion;
        size_t parts_size = 0;
        int PartCnt       = -1;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_OffsetFetch, 0, 9, NULL);

        if (parts) {
                parts_size = parts->cnt * 32;
        }

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_OffsetFetch, 1,
            /* GroupId + GenerationIdOrMemberEpoch + MemberId +
             * rd_kafka_buf_write_arraycnt_pos + Topics + RequireStable */
            32 + 4 + 50 + 4 + parts_size + 1, ApiVersion >= 6 /*flexver*/);

        if (ApiVersion >= 8) {
                /*
                 * Groups array count.
                 * Currently, only supporting 1 group.
                 * TODO: Update to use multiple groups.
                 */
                rd_kafka_buf_write_arraycnt(rkbuf, 1);
        }

        /* ConsumerGroup */
        rd_kafka_buf_write_str(rkbuf, group_id, -1);

        if (ApiVersion >= 9) {
                if (!member_id) {
                        rd_kafkap_str_t *null_member_id =
                            rd_kafkap_str_new(NULL, -1);
                        rd_kafka_buf_write_kstr(rkbuf, null_member_id);
                        rd_kafkap_str_destroy(null_member_id);
                } else {
                        rd_kafka_buf_write_kstr(rkbuf, member_id);
                }
                rd_kafka_buf_write_i32(rkbuf, generation_id_or_member_epoch);
        }

        if (parts) {
                /* Sort partitions by topic */
                rd_kafka_topic_partition_list_sort_by_topic(parts);

                /* Write partition list, filtering out partitions with valid
                 * offsets */
                const rd_kafka_topic_partition_field_t fields[] = {
                    RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
                    RD_KAFKA_TOPIC_PARTITION_FIELD_END};
                PartCnt = rd_kafka_buf_write_topic_partitions(
                    rkbuf, parts, rd_false /*include invalid offsets*/,
                    rd_false /*skip valid offsets */,
                    use_topic_id /* use_topic id */, rd_true /*use topic name*/,
                    fields);
        } else {
                rd_kafka_buf_write_arraycnt(rkbuf, PartCnt);
        }

        if (ApiVersion >= 8) {
                // Tags for the groups array
                rd_kafka_buf_write_tags_empty(rkbuf);
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
        const int log_decode_errors                     = LOG_ERR;
        int errcnt                                      = 0;
        int partcnt                                     = 0;
        int actions                                     = 0;
        rd_kafka_topic_partition_list_t *partitions     = NULL;
        rd_kafka_topic_partition_t *partition           = NULL;
        const rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            RD_KAFKA_TOPIC_PARTITION_FIELD_ERR,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};

        if (err)
                goto err;

        if (rd_kafka_buf_ApiVersion(rkbuf) >= 3)
                rd_kafka_buf_read_throttle_time(rkbuf);

        partitions = rd_kafka_buf_read_topic_partitions(
            rkbuf, rd_false /*don't use topic_id*/, rd_true /*use topic name*/,
            0, fields);

        if (!partitions)
                goto err_parse;

        partcnt = partitions->cnt;
        RD_KAFKA_TPLIST_FOREACH(partition, partitions) {
                rd_kafka_topic_partition_t *rktpar;

                rktpar = rd_kafka_topic_partition_list_find(
                    offsets, partition->topic, partition->partition);

                if (!rktpar) {
                        /* Received offset for topic/partition we didn't
                         * ask for, this shouldn't really happen. */
                        continue;
                }

                if (partition->err) {
                        rktpar->err = partition->err;
                        err         = partition->err;
                        errcnt++;
                        /* Accumulate actions for per-partition
                         * errors. */
                        actions |= rd_kafka_handle_OffsetCommit_error(
                            rkb, request, partition);
                }
        }
        rd_kafka_topic_partition_list_destroy(partitions);

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
        int tot_PartCnt = 0;
        int16_t ApiVersion;
        int features;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_OffsetCommit, 0, 9, &features);

        rd_kafka_assert(NULL, offsets != NULL);

        rkbuf = rd_kafka_buf_new_flexver_request(rkb, RD_KAFKAP_OffsetCommit, 1,
                                                 100 + (offsets->cnt * 128),
                                                 ApiVersion >= 8);

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

        /* Write partition list, filtering out partitions with valid
         * offsets */
        rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            RD_KAFKA_TOPIC_PARTITION_FIELD_OFFSET,
            ApiVersion >= 6 ? RD_KAFKA_TOPIC_PARTITION_FIELD_EPOCH
                            : RD_KAFKA_TOPIC_PARTITION_FIELD_NOOP,
            ApiVersion == 1 ? RD_KAFKA_TOPIC_PARTITION_FIELD_TIMESTAMP
                            : RD_KAFKA_TOPIC_PARTITION_FIELD_NOOP,
            RD_KAFKA_TOPIC_PARTITION_FIELD_METADATA,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};

        tot_PartCnt = rd_kafka_buf_write_topic_partitions(
            rkbuf, offsets, rd_true /*skip invalid offsets*/,
            rd_false /*include valid offsets */,
            rd_false /*don't use topic id*/, rd_true /*use topic name*/,
            fields);

        if (tot_PartCnt == 0) {
                /* No topic+partitions had valid offsets to commit. */
                rd_kafka_replyq_destroy(&replyq);
                rd_kafka_buf_destroy(rkbuf);
                return 0;
        }

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

        const rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};
        rd_kafka_buf_write_topic_partitions(
            rkbuf, grpoffsets->partitions,
            rd_false /*dont skip invalid offsets*/, rd_false /*any offset*/,
            rd_false /*don't use topic id*/, rd_true /*use topic name*/,
            fields);

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
        const rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};
        rd_kafka_buf_write_topic_partitions(
            rkbuf, rkgm->rkgm_assignment,
            rd_false /*don't skip invalid offsets*/, rd_false /* any offset */,
            rd_false /*don't use topic id*/, rd_true /*use topic name*/,
            fields);
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
                    rk->rk_cgrp->rkcg_group_assignment,
                    rk->rk_conf.client_rack);
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

void rd_kafka_ConsumerGroupHeartbeatRequest(
    rd_kafka_broker_t *rkb,
    const rd_kafkap_str_t *group_id,
    const rd_kafkap_str_t *member_id,
    int32_t member_epoch,
    const rd_kafkap_str_t *group_instance_id,
    const rd_kafkap_str_t *rack_id,
    int32_t rebalance_timeout_ms,
    const rd_kafka_topic_partition_list_t *subscribe_topics,
    const rd_kafkap_str_t *remote_assignor,
    const rd_kafka_topic_partition_list_t *current_assignments,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque) {

        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;
        size_t rkbuf_size = 0;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_ConsumerGroupHeartbeat, 0, 0, &features);

        if (rd_rkb_is_dbg(rkb, CGRP)) {
                char current_assignments_str[512] = "NULL";
                char subscribe_topics_str[512]    = "NULL";
                const char *member_id_str         = "NULL";
                const char *group_instance_id_str = "NULL";
                const char *remote_assignor_str   = "NULL";

                if (current_assignments) {
                        rd_kafka_topic_partition_list_str(
                            current_assignments, current_assignments_str,
                            sizeof(current_assignments_str), 0);
                }
                if (subscribe_topics) {
                        rd_kafka_topic_partition_list_str(
                            subscribe_topics, subscribe_topics_str,
                            sizeof(subscribe_topics_str), 0);
                }
                if (member_id)
                        member_id_str = member_id->str;
                if (group_instance_id)
                        group_instance_id_str = group_instance_id->str;
                if (remote_assignor)
                        remote_assignor_str = remote_assignor->str;

                rd_rkb_dbg(rkb, CGRP, "HEARTBEAT",
                           "ConsumerGroupHeartbeat of member id \"%s\", group "
                           "id \"%s\", "
                           "generation id %" PRId32
                           ", group instance id \"%s\""
                           ", current assignment \"%s\""
                           ", subscribe topics \"%s\""
                           ", remote assignor \"%s\"",
                           member_id_str, group_id->str, member_epoch,
                           group_instance_id_str, current_assignments_str,
                           subscribe_topics_str, remote_assignor_str);
        }

        size_t next_subscription_size = 0;

        if (subscribe_topics) {
                next_subscription_size =
                    ((subscribe_topics->cnt * (4 + 50)) + 4);
        }

        if (group_id)
                rkbuf_size += RD_KAFKAP_STR_SIZE(group_id);
        if (member_id)
                rkbuf_size += RD_KAFKAP_STR_SIZE(member_id);
        rkbuf_size += 4; /* MemberEpoch */
        if (group_instance_id)
                rkbuf_size += RD_KAFKAP_STR_SIZE(group_instance_id);
        if (rack_id)
                rkbuf_size += RD_KAFKAP_STR_SIZE(rack_id);
        rkbuf_size += 4; /* RebalanceTimeoutMs */
        if (next_subscription_size)
                rkbuf_size += next_subscription_size;
        if (remote_assignor)
                rkbuf_size += RD_KAFKAP_STR_SIZE(remote_assignor);
        if (current_assignments)
                rkbuf_size += (current_assignments->cnt * (16 + 100));
        rkbuf_size += 4; /* TopicPartitions */

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_ConsumerGroupHeartbeat, 1, rkbuf_size, rd_true);

        rd_kafka_buf_write_kstr(rkbuf, group_id);
        rd_kafka_buf_write_kstr(rkbuf, member_id);
        rd_kafka_buf_write_i32(rkbuf, member_epoch);
        rd_kafka_buf_write_kstr(rkbuf, group_instance_id);
        rd_kafka_buf_write_kstr(rkbuf, rack_id);
        rd_kafka_buf_write_i32(rkbuf, rebalance_timeout_ms);

        if (subscribe_topics) {
                size_t of_TopicsArrayCnt;
                int topics_cnt = subscribe_topics->cnt;

                /* write Topics */
                of_TopicsArrayCnt = rd_kafka_buf_write_arraycnt_pos(rkbuf);
                rd_kafka_buf_finalize_arraycnt(rkbuf, of_TopicsArrayCnt,
                                               topics_cnt);
                while (--topics_cnt >= 0)
                        rd_kafka_buf_write_str(
                            rkbuf, subscribe_topics->elems[topics_cnt].topic,
                            -1);

        } else {
                rd_kafka_buf_write_arraycnt(rkbuf, -1);
        }

        rd_kafka_buf_write_kstr(rkbuf, remote_assignor);

        if (current_assignments) {
                const rd_kafka_topic_partition_field_t
                    current_assignments_fields[] = {
                        RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
                        RD_KAFKA_TOPIC_PARTITION_FIELD_END};
                rd_kafka_buf_write_topic_partitions(
                    rkbuf, current_assignments, rd_false, rd_false,
                    rd_true /*use topic id*/, rd_false /*don't use topic name*/,
                    current_assignments_fields);
        } else {
                rd_kafka_buf_write_arraycnt(rkbuf, -1);
        }

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        /* FIXME:
         * 1) Improve this timeout to something less than
         *    `rkcg_heartbeat_intvl_ms` so that the next heartbeat
         *    is not skipped.
         * 2) Remove usage of `group_session_timeout_ms` altogether
         *    from the new protocol defined in KIP-848.
         */
        if (rkb->rkb_rk->rk_cgrp->rkcg_heartbeat_intvl_ms > 0) {
                rd_kafka_buf_set_abs_timeout(
                    rkbuf, rkb->rkb_rk->rk_cgrp->rkcg_heartbeat_intvl_ms, 0);
        } else {
                rd_kafka_buf_set_abs_timeout(
                    rkbuf, rkb->rkb_rk->rk_conf.group_session_timeout_ms, 0);
        }

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
}



/**
 * @brief Construct and send ListGroupsRequest to \p rkb
 *        with the states (const char *) in \p states,
 *        and the types (const char *) in \p types.
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
                                             const char **types,
                                             size_t types_cnt,
                                             rd_kafka_replyq_t replyq,
                                             rd_kafka_resp_cb_t *resp_cb,
                                             void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        size_t i;

        if (max_ApiVersion < 0)
                max_ApiVersion = 5;

        if (max_ApiVersion > ApiVersion) {
                /* Remark: don't check if max_ApiVersion is zero.
                 * As rd_kafka_broker_ApiVersion_supported cannot be checked
                 * in the application thread reliably . */
                ApiVersion = rd_kafka_broker_ApiVersion_supported(
                    rkb, RD_KAFKAP_ListGroups, 0, max_ApiVersion, NULL);
        }

        if (ApiVersion == -1) {
                return rd_kafka_error_new(
                    RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE,
                    "ListGroupsRequest not supported by broker");
        }

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_ListGroups, 1,
            /* rd_kafka_buf_write_arraycnt_pos + tags + StatesFilter */
            4 + 1 + 32 * states_cnt, ApiVersion >= 3 /* is_flexver */);

        if (ApiVersion >= 4) {
                rd_kafka_buf_write_arraycnt(rkbuf, states_cnt);
                for (i = 0; i < states_cnt; i++) {
                        rd_kafka_buf_write_str(rkbuf, states[i], -1);
                }
        }

        if (ApiVersion >= 5) {
                rd_kafka_buf_write_arraycnt(rkbuf, types_cnt);
                for (i = 0; i < types_cnt; i++) {
                        rd_kafka_buf_write_str(rkbuf, types[i], -1);
                }
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
 *        Uses \p include_authorized_operations to get
 *        group ACL authorized operations.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @return NULL on success, a new error instance that must be
 *         released with rd_kafka_error_destroy() in case of error.
 */
rd_kafka_error_t *
rd_kafka_DescribeGroupsRequest(rd_kafka_broker_t *rkb,
                               int16_t max_ApiVersion,
                               char **groups,
                               size_t group_cnt,
                               rd_bool_t include_authorized_operations,
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
                rd_kafka_buf_write_bool(rkbuf, include_authorized_operations);
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
        rd_kafka_op_t *rko                = opaque; /* Possibly NULL */
        rd_kafka_metadata_internal_t *mdi = NULL;
        const rd_list_t *topics           = request->rkbuf_u.Metadata.topics;
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

        err = rd_kafka_parse_Metadata(rkb, request, rkbuf, &mdi);
        if (err)
                goto err;

        if (rko && rko->rko_replyq.q) {
                /* Reply to metadata requester, passing on the metadata.
                 * Reuse requesting rko for the reply. */
                rko->rko_err            = err;
                rko->rko_u.metadata.md  = &mdi->metadata;
                rko->rko_u.metadata.mdi = mdi;
                rd_kafka_replyq_enq(&rko->rko_replyq, rko, 0);
                rko = NULL;
        } else {
                if (mdi)
                        rd_free(mdi);
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
                        rko->rko_err            = err;
                        rko->rko_u.metadata.md  = NULL;
                        rko->rko_u.metadata.mdi = NULL;
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
 * @brief Internal implementation of MetadataRequest.
 *
 *        - !topics && !topic_ids: only request brokers (if supported by
 *          broker, else all topics)
 *        - topics.cnt > 0 && topic_ids.cnt > 0: invalid request
 *        - topics.cnt > 0 || topic_ids.cnt > 0: only specified topics
 *          are requested
 *        - else: all topics in cluster are requested
 *
 * @param topics A list of topic names (char *) to request.
 * @param topic_ids A list of topic ids (rd_kafka_Uuid_t *) to request.
 * @param reason Metadata request reason
 * @param allow_auto_create_topics Allow broker-side auto topic creation.
 *                                 This is best-effort, depending on broker
 *                                 config and version.
 * @param include_cluster_authorized_operations Request for cluster
 *                                              authorized operations.
 * @param include_topic_authorized_operations Request for topic
 *                                            authorized operations.
 * @param cgrp_update Update cgrp in parse_Metadata (see comment there).
 * @param force_racks Force partition to rack mapping computation in
 *                    parse_Metadata (see comment there).
 * @param rko         (optional) rko with replyq for handling response.
 *                    Specifying an rko forces a metadata request even if
 *                    there is already a matching one in-transit.
 * @param resp_cb Callback to be used for handling response.
 * @param replyq replyq on which response is handled.
 * @param force rd_true: force a full request (including all topics and
 *                       brokers) even if there is such a request already
 *                       in flight.
 *              rd_false: check if there are multiple outstanding full
 *                        requests, and don't send one if there is already
 *                        one present. (See note below.)
 * @param opaque (optional) parameter to be passed to resp_cb.
 *
 * @return Error code:
 *         If full metadata for all topics is requested (or
 *         all brokers, which results in all-topics on older brokers) and
 *         there is already a full request in transit then this function
 *         will return  RD_KAFKA_RESP_ERR__PREV_IN_PROGRESS,
 *         otherwise RD_KAFKA_RESP_ERR_NO_ERROR.
 *
 * @remark Either \p topics or \p topic_ids must be set, but not both.
 * @remark If \p rko is specified, \p resp_cb, \p replyq, \p force, \p opaque
 *         should be NULL or rd_false.
 * @remark If \p rko is non-NULL or if \p force is true,
 *         the request is sent regardless.
 * @remark \p include_cluster_authorized_operations and
 *         \p include_topic_authorized_operations should not be set unless this
 *         MetadataRequest is for an admin operation.
 *
 * @sa rd_kafka_MetadataRequest().
 * @sa rd_kafka_MetadataRequest_resp_cb().
 */
static rd_kafka_resp_err_t
rd_kafka_MetadataRequest0(rd_kafka_broker_t *rkb,
                          const rd_list_t *topics,
                          const rd_list_t *topic_ids,
                          const char *reason,
                          rd_bool_t allow_auto_create_topics,
                          rd_bool_t include_cluster_authorized_operations,
                          rd_bool_t include_topic_authorized_operations,
                          rd_bool_t cgrp_update,
                          rd_bool_t force_racks,
                          rd_kafka_op_t *rko,
                          rd_kafka_resp_cb_t *resp_cb,
                          rd_kafka_replyq_t replyq,
                          rd_bool_t force,
                          void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        size_t of_TopicArrayCnt;
        int features;
        int topic_id_cnt;
        int total_topic_cnt;
        int topic_cnt                  = topics ? rd_list_cnt(topics) : 0;
        int *full_incr                 = NULL;
        void *handler_arg              = NULL;
        rd_kafka_resp_cb_t *handler_cb = rd_kafka_handle_Metadata;
        int16_t metadata_max_version   = 12;
        rd_kafka_replyq_t use_replyq   = replyq;

        /* In case we want cluster authorized operations in the Metadata
         * request, we must send a request with version not exceeding 10 because
         * KIP-700 deprecates those fields from the Metadata RPC. */
        if (include_cluster_authorized_operations)
                metadata_max_version = RD_MIN(metadata_max_version, 10);

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_Metadata, 0, metadata_max_version, &features);

        topic_id_cnt =
            (ApiVersion >= 10 && topic_ids) ? rd_list_cnt(topic_ids) : 0;
        rd_assert(topic_id_cnt == 0 || ApiVersion >= 12);

        total_topic_cnt = topic_cnt + topic_id_cnt;

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_Metadata, 1,
            4 + ((50 /*topic name */ + 16 /* topic id */) * total_topic_cnt) +
                1,
            ApiVersion >= 9);

        if (!reason)
                reason = "";

        rkbuf->rkbuf_u.Metadata.reason      = rd_strdup(reason);
        rkbuf->rkbuf_u.Metadata.cgrp_update = cgrp_update;
        rkbuf->rkbuf_u.Metadata.force_racks = force_racks;

        /* TopicArrayCnt */
        of_TopicArrayCnt = rd_kafka_buf_write_arraycnt_pos(rkbuf);

        if (!topics && !topic_ids) {
                /* v0: keep 0, brokers only not available,
                 * request all topics */
                /* v1-8: 0 means empty array, brokers only */
                if (ApiVersion >= 9) {
                        /* v9+: varint encoded empty array (1), brokers only */
                        rd_kafka_buf_finalize_arraycnt(rkbuf, of_TopicArrayCnt,
                                                       topic_cnt);
                }

                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "Request metadata for brokers only: %s", reason);
                full_incr =
                    &rkb->rkb_rk->rk_metadata_cache.rkmc_full_brokers_sent;

        } else if (total_topic_cnt == 0) {
                /* v0: keep 0, request all topics */
                if (ApiVersion >= 1 && ApiVersion < 9) {
                        /* v1-8: update to -1, all topics */
                        rd_kafka_buf_update_i32(rkbuf, of_TopicArrayCnt, -1);
                }
                /* v9+: keep 0, varint encoded null, all topics */

                rkbuf->rkbuf_u.Metadata.all_topics = 1;
                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "Request metadata for all topics: "
                           "%s",
                           reason);

                if (!rko)
                        full_incr = &rkb->rkb_rk->rk_metadata_cache
                                         .rkmc_full_topics_sent;

        } else {
                /* Cannot request topics by name and id at the same time */
                rd_dassert(!(topic_cnt > 0 && topic_id_cnt > 0));

                /* request cnt topics */
                rd_kafka_buf_finalize_arraycnt(rkbuf, of_TopicArrayCnt,
                                               total_topic_cnt);

                rd_rkb_dbg(rkb, METADATA, "METADATA",
                           "Request metadata for %d topic(s): "
                           "%s",
                           total_topic_cnt, reason);
        }

        if (full_incr) {
                /* Avoid multiple outstanding full requests
                 * (since they are redundant and side-effect-less).
                 * Forced requests (app using metadata() API or Admin API) are
                 * passed through regardless. */

                mtx_lock(&rkb->rkb_rk->rk_metadata_cache.rkmc_full_lock);
                if (!force &&
                    (*full_incr > 0 && (!rko || !rko->rko_u.metadata.force))) {
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
                rd_kafka_Uuid_t zero_uuid = RD_KAFKA_UUID_ZERO;

                /* Maintain a copy of the topics list so we can purge
                 * hints from the metadata cache on error. */
                rkbuf->rkbuf_u.Metadata.topics =
                    rd_list_copy(topics, rd_list_string_copy, NULL);

                RD_LIST_FOREACH(topic, topics, i) {
                        if (ApiVersion >= 10) {
                                rd_kafka_buf_write_uuid(rkbuf, &zero_uuid);
                        }
                        rd_kafka_buf_write_str(rkbuf, topic, -1);
                        /* Tags for previous topic */
                        rd_kafka_buf_write_tags_empty(rkbuf);
                }
        }

        if (ApiVersion >= 10 && topic_id_cnt > 0) {
                int i;
                rd_kafka_Uuid_t *topic_id;

                /* Maintain a copy of the topics list so we can purge
                 * hints from the metadata cache on error. */
                rkbuf->rkbuf_u.Metadata.topic_ids =
                    rd_list_copy(topic_ids, rd_list_Uuid_copy, NULL);

                RD_LIST_FOREACH(topic_id, topic_ids, i) {
                        rd_kafka_buf_write_uuid(rkbuf, topic_id);
                        rd_kafka_buf_write_str(rkbuf, NULL, -1);
                        /* Tags for previous topic */
                        rd_kafka_buf_write_tags_empty(rkbuf);
                }
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

        if (ApiVersion >= 8 && ApiVersion <= 10) {
                /* IncludeClusterAuthorizedOperations */
                rd_kafka_buf_write_bool(rkbuf,
                                        include_cluster_authorized_operations);
        }

        if (ApiVersion >= 8) {
                /* IncludeTopicAuthorizedOperations */
                rd_kafka_buf_write_bool(rkbuf,
                                        include_topic_authorized_operations);
        }

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        /* Metadata requests are part of the important control plane
         * and should go before most other requests (Produce, Fetch, etc). */
        rkbuf->rkbuf_prio = RD_KAFKA_PRIO_HIGH;

        /* The default handler is rd_kafka_handle_Metadata, but it can be
         * overriden to use a custom handler. */
        if (resp_cb)
                handler_cb = resp_cb;

        /* If a custom handler is provided, we also allow the caller to set a
         * custom argument which is passed as the opaque argument to the
         * handler. However, if we're using the default handler, it expects
         * either rko or NULL as its opaque argument (it forwards the response
         * to rko's replyq if it's non-NULL). */
        if (resp_cb && opaque)
                handler_arg = opaque;
        else
                handler_arg = rko;

        /* If a custom replyq is provided (and is valid), the response is
         * handled through on that replyq. By default, response is handled on
         * rk_ops, and the default handler (rd_kafka_handle_Metadata) forwards
         * the parsed result to rko's replyq when done. */
        if (!use_replyq.q)
                use_replyq = RD_KAFKA_REPLYQ(rkb->rkb_rk->rk_ops, 0);

        rd_kafka_broker_buf_enq_replyq(
            rkb, rkbuf, use_replyq,
            /* The default response handler is rd_kafka_handle_Metadata, but we
               allow alternate handlers to be configured. */
            handler_cb, handler_arg);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Construct and enqueue a MetadataRequest
 *
 *        - !topics && !topic_ids: only request brokers (if supported by
 *          broker, else all topics)
 *        - topics.cnt > 0 && topic_ids.cnt > 0: invalid request
 *        - topics.cnt > 0 || topic_ids.cnt > 0: only specified topics
 *          are requested
 *        - else: all topics in cluster are requested
 *
 * @param topics A list of topic names (char *) to request.
 * @param topic_ids A list of topic ids (rd_kafka_Uuid_t *) to request.
 * @param reason    - metadata request reason
 * @param allow_auto_create_topics - allow broker-side auto topic creation.
 *                                   This is best-effort, depending on broker
 *                                   config and version.
 * @param cgrp_update - Update cgrp in parse_Metadata (see comment there).
 * @param force_racks - Force partition to rack mapping computation in
 *                      parse_Metadata (see comment there).
 * @param rko       - (optional) rko with replyq for handling response.
 *                    Specifying an rko forces a metadata request even if
 *                    there is already a matching one in-transit.
 *
 * @return Error code:
 *         If full metadata for all topics is requested (or
 *         all brokers, which results in all-topics on older brokers) and
 *         there is already a full request in transit then this function
 *         will return  RD_KAFKA_RESP_ERR__PREV_IN_PROGRESS,
 *         otherwise RD_KAFKA_RESP_ERR_NO_ERROR.
 *         If \p rko is non-NULL, the request is sent regardless.
 *
 * @remark Either \p topics or \p topic_ids must be set, but not both.
 */
rd_kafka_resp_err_t rd_kafka_MetadataRequest(rd_kafka_broker_t *rkb,
                                             const rd_list_t *topics,
                                             rd_list_t *topic_ids,
                                             const char *reason,
                                             rd_bool_t allow_auto_create_topics,
                                             rd_bool_t cgrp_update,
                                             rd_bool_t force_racks,
                                             rd_kafka_op_t *rko) {
        return rd_kafka_MetadataRequest0(
            rkb, topics, topic_ids, reason, allow_auto_create_topics,
            rd_false /*don't include cluster authorized operations*/,
            rd_false /*don't include topic authorized operations*/, cgrp_update,
            force_racks, rko,
            /* We use the default rd_kafka_handle_Metadata rather than a custom
               resp_cb */
            NULL,
            /* Use default replyq which works with the default handler
               rd_kafka_handle_Metadata. */
            RD_KAFKA_NO_REPLYQ,
            /* If the request needs to be forced, rko_u.metadata.force will be
               set. We don't provide an explicit parameter force. */
            rd_false, NULL);
}

/**
 * @brief Construct and enqueue a MetadataRequest which use
 *        response callback \p resp_cb instead of a rko.
 *
 *        - !topics && !topic_ids: only request brokers (if supported by
 *          broker, else all topics)
 *        - topics.cnt > 0 && topic_ids.cnt > 0: invalid request
 *        - topics.cnt > 0 || topic_ids.cnt > 0: only specified topics
 *          are requested
 *        - else: all topics in cluster are requested
 *
 * @param topics A list of topic names (char *) to request.
 * @param topic_ids A list of topic ids (rd_kafka_Uuid_t *) to request.
 * @param reason Metadata request reason
 * @param allow_auto_create_topics Allow broker-side auto topic creation.
 *                                 This is best-effort, depending on broker
 *                                 config and version.
 * @param include_cluster_authorized_operations Request for cluster
 *                                              authorized operations.
 * @param include_topic_authorized_operations Request for topic
 *                                            authorized operations.
 * @param cgrp_update Update cgrp in parse_Metadata (see comment there).
 * @param force_racks Force partition to rack mapping computation in
 *                    parse_Metadata (see comment there).
 * @param resp_cb Callback to be used for handling response.
 * @param replyq replyq on which response is handled.
 * @param force Force request even if in progress.
 * @param opaque (optional) parameter to be passed to resp_cb.
 *
 * @return Error code:
 *         If full metadata for all topics is requested (or
 *         all brokers, which results in all-topics on older brokers) and
 *         there is already a full request in transit then this function
 *         will return  RD_KAFKA_RESP_ERR__PREV_IN_PROGRESS,
 *         otherwise RD_KAFKA_RESP_ERR_NO_ERROR.
 *
 *  @remark Either \p topics or \p topic_ids must be set, but not both.
 */
rd_kafka_resp_err_t rd_kafka_MetadataRequest_resp_cb(
    rd_kafka_broker_t *rkb,
    const rd_list_t *topics,
    const rd_list_t *topics_ids,
    const char *reason,
    rd_bool_t allow_auto_create_topics,
    rd_bool_t include_cluster_authorized_operations,
    rd_bool_t include_topic_authorized_operations,
    rd_bool_t cgrp_update,
    rd_bool_t force_racks,
    rd_kafka_resp_cb_t *resp_cb,
    rd_kafka_replyq_t replyq,
    rd_bool_t force,
    void *opaque) {
        return rd_kafka_MetadataRequest0(
            rkb, topics, topics_ids, reason, allow_auto_create_topics,
            include_cluster_authorized_operations,
            include_topic_authorized_operations, cgrp_update, force_racks,
            NULL /* No op - using custom resp_cb. */, resp_cb, replyq, force,
            opaque);
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
            rkb, RD_KAFKAP_ApiVersion, 1, 3, ApiVersion >= 3 /*flexver*/);

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

        rd_kafka_buf_read_kbytes(rkbuf, &auth_data);

        if (request->rkbuf_reqhdr.ApiVersion >= 1) {
                int64_t session_lifetime_ms;
                rd_kafka_buf_read_i64(rkbuf, &session_lifetime_ms);

                if (session_lifetime_ms)
                        rd_kafka_dbg(
                            rk, SECURITY, "REAUTH",
                            "Received session lifetime %ld ms from broker",
                            session_lifetime_ms);
                rd_kafka_broker_start_reauth_timer(rkb, session_lifetime_ms);
        }

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
        int16_t ApiVersion;
        int features;

        rkbuf = rd_kafka_buf_new_request(rkb, RD_KAFKAP_SaslAuthenticate, 0, 0);

        /* Should be sent before any other requests since it is part of
         * the initial connection handshake. */
        rkbuf->rkbuf_prio = RD_KAFKA_PRIO_FLASH;

        /* Broker does not support -1 (Null) for this field */
        rd_kafka_buf_write_bytes(rkbuf, buf ? buf : "", size);

        /* There are no errors that can be retried, instead
         * close down the connection and reconnect on failure. */
        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_NO_RETRIES;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_SaslAuthenticate, 0, 1, &features);
        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        if (replyq.q)
                rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb,
                                               opaque);
        else /* in broker thread */
                rd_kafka_broker_buf_enq1(rkb, rkbuf, resp_cb, opaque);
}

/**
 * @name Leader discovery (KIP-951)
 * @{
 */

void rd_kafkap_leader_discovery_tmpabuf_add_alloc_brokers(
    rd_tmpabuf_t *tbuf,
    rd_kafkap_NodeEndpoints_t *NodeEndpoints) {
        int i;
        size_t md_brokers_size =
            NodeEndpoints->NodeEndpointCnt * sizeof(rd_kafka_metadata_broker_t);
        size_t mdi_brokers_size = NodeEndpoints->NodeEndpointCnt *
                                  sizeof(rd_kafka_metadata_broker_internal_t);
        rd_tmpabuf_add_alloc_times(tbuf, md_brokers_size, 2);
        rd_tmpabuf_add_alloc(tbuf, mdi_brokers_size);
        for (i = 0; i < NodeEndpoints->NodeEndpointCnt; i++) {
                size_t HostSize =
                    RD_KAFKAP_STR_LEN(&NodeEndpoints->NodeEndpoints[i].Host) +
                    1;
                rd_tmpabuf_add_alloc(tbuf, HostSize);
        }
}

void rd_kafkap_leader_discovery_tmpabuf_add_alloc_topics(rd_tmpabuf_t *tbuf,
                                                         int topic_cnt) {
        rd_tmpabuf_add_alloc(tbuf,
                             sizeof(rd_kafka_metadata_topic_t) * topic_cnt);
        rd_tmpabuf_add_alloc(tbuf, sizeof(rd_kafka_metadata_topic_internal_t) *
                                       topic_cnt);
}

void rd_kafkap_leader_discovery_tmpabuf_add_alloc_topic(rd_tmpabuf_t *tbuf,
                                                        char *topic_name,
                                                        int32_t partition_cnt) {
        if (topic_name) {
                rd_tmpabuf_add_alloc(tbuf, strlen(topic_name) + 1);
        }
        rd_tmpabuf_add_alloc(tbuf, sizeof(rd_kafka_metadata_partition_t) *
                                       partition_cnt);
        rd_tmpabuf_add_alloc(tbuf,
                             sizeof(rd_kafka_metadata_partition_internal_t) *
                                 partition_cnt);
}

void rd_kafkap_leader_discovery_metadata_init(rd_kafka_metadata_internal_t *mdi,
                                              int32_t broker_id) {
        memset(mdi, 0, sizeof(*mdi));
        mdi->metadata.orig_broker_id       = broker_id;
        mdi->controller_id                 = -1;
        mdi->cluster_authorized_operations = -1;
}

void rd_kafkap_leader_discovery_set_brokers(
    rd_tmpabuf_t *tbuf,
    rd_kafka_metadata_internal_t *mdi,
    rd_kafkap_NodeEndpoints_t *NodeEndpoints) {
        int i;
        rd_kafka_metadata_t *md = &mdi->metadata;

        size_t md_brokers_size =
            NodeEndpoints->NodeEndpointCnt * sizeof(rd_kafka_metadata_broker_t);
        size_t mdi_brokers_size = NodeEndpoints->NodeEndpointCnt *
                                  sizeof(rd_kafka_metadata_broker_internal_t);

        md->broker_cnt      = NodeEndpoints->NodeEndpointCnt;
        md->brokers         = rd_tmpabuf_alloc(tbuf, md_brokers_size);
        mdi->brokers_sorted = rd_tmpabuf_alloc(tbuf, md_brokers_size);
        mdi->brokers        = rd_tmpabuf_alloc(tbuf, mdi_brokers_size);

        for (i = 0; i < NodeEndpoints->NodeEndpointCnt; i++) {
                rd_kafkap_NodeEndpoint_t *NodeEndpoint =
                    &NodeEndpoints->NodeEndpoints[i];
                rd_kafka_metadata_broker_t *mdb           = &md->brokers[i];
                rd_kafka_metadata_broker_internal_t *mdbi = &mdi->brokers[i];
                mdb->id   = NodeEndpoint->NodeId;
                mdb->host = NULL;
                if (!RD_KAFKAP_STR_IS_NULL(&NodeEndpoint->Host)) {
                        mdb->host = rd_tmpabuf_alloc(
                            tbuf, RD_KAFKAP_STR_LEN(&NodeEndpoint->Host) + 1);
                        rd_snprintf(mdb->host,
                                    RD_KAFKAP_STR_LEN(&NodeEndpoint->Host) + 1,
                                    "%.*s",
                                    RD_KAFKAP_STR_PR(&NodeEndpoint->Host));
                }
                mdb->port = NodeEndpoints->NodeEndpoints[i].Port;

                /* Metadata internal fields */
                mdbi->id      = mdb->id;
                mdbi->rack_id = NULL;
        }

        qsort(mdi->brokers, md->broker_cnt, sizeof(mdi->brokers[0]),
              rd_kafka_metadata_broker_internal_cmp);
        memcpy(mdi->brokers_sorted, md->brokers,
               sizeof(*mdi->brokers_sorted) * md->broker_cnt);
        qsort(mdi->brokers_sorted, md->broker_cnt, sizeof(*mdi->brokers_sorted),
              rd_kafka_metadata_broker_cmp);
}

void rd_kafkap_leader_discovery_set_topic_cnt(rd_tmpabuf_t *tbuf,
                                              rd_kafka_metadata_internal_t *mdi,
                                              int topic_cnt) {

        rd_kafka_metadata_t *md = &mdi->metadata;

        md->topic_cnt = topic_cnt;
        md->topics    = rd_tmpabuf_alloc(tbuf, sizeof(*md->topics) * topic_cnt);
        mdi->topics = rd_tmpabuf_alloc(tbuf, sizeof(*mdi->topics) * topic_cnt);
}

void rd_kafkap_leader_discovery_set_topic(rd_tmpabuf_t *tbuf,
                                          rd_kafka_metadata_internal_t *mdi,
                                          int topic_idx,
                                          rd_kafka_Uuid_t topic_id,
                                          char *topic_name,
                                          int partition_cnt) {

        rd_kafka_metadata_t *md                  = &mdi->metadata;
        rd_kafka_metadata_topic_t *mdt           = &md->topics[topic_idx];
        rd_kafka_metadata_topic_internal_t *mdti = &mdi->topics[topic_idx];

        memset(mdt, 0, sizeof(*mdt));
        mdt->topic =
            topic_name ? rd_tmpabuf_alloc(tbuf, strlen(topic_name) + 1) : NULL;
        mdt->partition_cnt = partition_cnt;
        mdt->partitions =
            rd_tmpabuf_alloc(tbuf, sizeof(*mdt->partitions) * partition_cnt);

        if (topic_name)
                rd_snprintf(mdt->topic, strlen(topic_name) + 1, "%s",
                            topic_name);

        memset(mdti, 0, sizeof(*mdti));
        mdti->partitions =
            rd_tmpabuf_alloc(tbuf, sizeof(*mdti->partitions) * partition_cnt);
        mdti->topic_id                    = topic_id;
        mdti->topic_authorized_operations = -1;
}

void rd_kafkap_leader_discovery_set_CurrentLeader(
    rd_tmpabuf_t *tbuf,
    rd_kafka_metadata_internal_t *mdi,
    int topic_idx,
    int partition_idx,
    int32_t partition_id,
    rd_kafkap_CurrentLeader_t *CurrentLeader) {

        rd_kafka_metadata_t *md = &mdi->metadata;
        rd_kafka_metadata_partition_t *mdp =
            &md->topics[topic_idx].partitions[partition_idx];
        rd_kafka_metadata_partition_internal_t *mdpi =
            &mdi->topics[topic_idx].partitions[partition_idx];

        memset(mdp, 0, sizeof(*mdp));
        mdp->id     = partition_id;
        mdp->leader = CurrentLeader->LeaderId,

        memset(mdpi, 0, sizeof(*mdpi));
        mdpi->id           = partition_id;
        mdpi->leader_epoch = CurrentLeader->LeaderEpoch;
}
/**@}*/

static int rd_kafkap_Produce_reply_tags_partition_parse(
    rd_kafka_buf_t *rkbuf,
    uint64_t tagtype,
    uint64_t taglen,
    rd_kafkap_Produce_reply_tags_t *ProduceTags,
    rd_kafkap_Produce_reply_tags_Partition_t *PartitionTags) {
        switch (tagtype) {
        case 0: /* CurrentLeader */
                if (rd_kafka_buf_read_CurrentLeader(
                        rkbuf, &PartitionTags->CurrentLeader) == -1)
                        goto err_parse;
                ProduceTags->leader_change_cnt++;
                return 1;
        default:
                return 0;
        }
err_parse:
        return -1;
}

static int
rd_kafkap_Produce_reply_tags_parse(rd_kafka_buf_t *rkbuf,
                                   uint64_t tagtype,
                                   uint64_t taglen,
                                   rd_kafkap_Produce_reply_tags_t *tags) {
        switch (tagtype) {
        case 0: /* NodeEndpoints */
                if (rd_kafka_buf_read_NodeEndpoints(rkbuf,
                                                    &tags->NodeEndpoints) == -1)
                        goto err_parse;
                return 1;
        default:
                return 0;
        }
err_parse:
        return -1;
}

static void rd_kafka_handle_Produce_metadata_update(
    rd_kafka_broker_t *rkb,
    rd_kafkap_Produce_reply_tags_t *ProduceTags) {
        if (ProduceTags->leader_change_cnt) {
                rd_kafka_metadata_t *md           = NULL;
                rd_kafka_metadata_internal_t *mdi = NULL;
                rd_kafkap_Produce_reply_tags_Partition_t *Partition;
                rd_tmpabuf_t tbuf;
                int32_t nodeid;
                rd_kafka_op_t *rko;

                rd_kafka_broker_lock(rkb);
                nodeid = rkb->rkb_nodeid;
                rd_kafka_broker_unlock(rkb);

                rd_tmpabuf_new(&tbuf, 0, rd_true /*assert on fail*/);
                rd_tmpabuf_add_alloc(&tbuf, sizeof(*mdi));
                rd_kafkap_leader_discovery_tmpabuf_add_alloc_brokers(
                    &tbuf, &ProduceTags->NodeEndpoints);
                rd_kafkap_leader_discovery_tmpabuf_add_alloc_topics(&tbuf, 1);
                rd_kafkap_leader_discovery_tmpabuf_add_alloc_topic(
                    &tbuf, ProduceTags->Topic.TopicName, 1);
                rd_tmpabuf_finalize(&tbuf);

                mdi = rd_tmpabuf_alloc(&tbuf, sizeof(*mdi));
                md  = &mdi->metadata;

                rd_kafkap_leader_discovery_metadata_init(mdi, nodeid);

                rd_kafkap_leader_discovery_set_brokers(
                    &tbuf, mdi, &ProduceTags->NodeEndpoints);

                rd_kafkap_leader_discovery_set_topic_cnt(&tbuf, mdi, 1);

                rd_kafkap_leader_discovery_set_topic(
                    &tbuf, mdi, 0, RD_KAFKA_UUID_ZERO,
                    ProduceTags->Topic.TopicName, 1);

                Partition = &ProduceTags->Topic.Partition;
                rd_kafkap_leader_discovery_set_CurrentLeader(
                    &tbuf, mdi, 0, 0, Partition->Partition,
                    &Partition->CurrentLeader);

                rko = rd_kafka_op_new(RD_KAFKA_OP_METADATA_UPDATE);
                rko->rko_u.metadata.md  = md;
                rko->rko_u.metadata.mdi = mdi;
                rd_kafka_q_enq(rkb->rkb_rk->rk_ops, rko);
        }
}

static void rd_kafkap_Produce_reply_tags_destroy(
    rd_kafkap_Produce_reply_tags_t *reply_tags) {
        RD_IF_FREE(reply_tags->Topic.TopicName, rd_free);
        RD_IF_FREE(reply_tags->NodeEndpoints.NodeEndpoints, rd_free);
}


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
                              rd_kafka_Produce_result_t *result) {
        int32_t TopicArrayCnt;
        int32_t PartitionArrayCnt;
        struct {
                int32_t Partition;
                int16_t ErrorCode;
                int64_t Offset;
        } hdr;
        const int log_decode_errors                = LOG_ERR;
        int64_t log_start_offset                   = -1;
        rd_kafkap_str_t TopicName                  = RD_ZERO_INIT;
        rd_kafkap_Produce_reply_tags_t ProduceTags = RD_ZERO_INIT;

        rd_kafka_buf_read_arraycnt(rkbuf, &TopicArrayCnt, RD_KAFKAP_TOPICS_MAX);
        if (TopicArrayCnt != 1)
                goto err;

        /* Since we only produce to one single topic+partition in each
         * request we assume that the reply only contains one topic+partition
         * and that it is the same that we requested.
         * If not the broker is buggy. */
        if (request->rkbuf_reqhdr.ApiVersion >= 10)
                rd_kafka_buf_read_str(rkbuf, &TopicName);
        else
                rd_kafka_buf_skip_str(rkbuf);
        rd_kafka_buf_read_arraycnt(rkbuf, &PartitionArrayCnt,
                                   RD_KAFKAP_PARTITIONS_MAX);

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

        if (request->rkbuf_reqhdr.ApiVersion >= 8) {
                int i;
                int32_t RecordErrorsCnt;
                rd_kafkap_str_t ErrorMessage;
                rd_kafka_buf_read_arraycnt(rkbuf, &RecordErrorsCnt, -1);
                if (RecordErrorsCnt) {
                        result->record_errors = rd_calloc(
                            RecordErrorsCnt, sizeof(*result->record_errors));
                        result->record_errors_cnt = RecordErrorsCnt;
                        for (i = 0; i < RecordErrorsCnt; i++) {
                                int32_t BatchIndex;
                                rd_kafkap_str_t BatchIndexErrorMessage;
                                rd_kafka_buf_read_i32(rkbuf, &BatchIndex);
                                rd_kafka_buf_read_str(rkbuf,
                                                      &BatchIndexErrorMessage);
                                result->record_errors[i].batch_index =
                                    BatchIndex;
                                if (!RD_KAFKAP_STR_IS_NULL(
                                        &BatchIndexErrorMessage))
                                        result->record_errors[i].errstr =
                                            RD_KAFKAP_STR_DUP(
                                                &BatchIndexErrorMessage);
                                /* RecordError tags */
                                rd_kafka_buf_skip_tags(rkbuf);
                        }
                }

                rd_kafka_buf_read_str(rkbuf, &ErrorMessage);
                if (!RD_KAFKAP_STR_IS_NULL(&ErrorMessage))
                        result->errstr = RD_KAFKAP_STR_DUP(&ErrorMessage);
        }

        if (request->rkbuf_reqhdr.ApiVersion >= 10) {
                rd_kafkap_Produce_reply_tags_Topic_t *TopicTags =
                    &ProduceTags.Topic;
                rd_kafkap_Produce_reply_tags_Partition_t *PartitionTags =
                    &TopicTags->Partition;

                /* Partition tags count */
                TopicTags->TopicName     = RD_KAFKAP_STR_DUP(&TopicName);
                PartitionTags->Partition = hdr.Partition;
        }

        /* Partition tags */
        rd_kafka_buf_read_tags(rkbuf,
                               rd_kafkap_Produce_reply_tags_partition_parse,
                               &ProduceTags, &ProduceTags.Topic.Partition);

        /* Topic tags */
        rd_kafka_buf_skip_tags(rkbuf);

        if (request->rkbuf_reqhdr.ApiVersion >= 1) {
                int32_t Throttle_Time;
                rd_kafka_buf_read_i32(rkbuf, &Throttle_Time);

                rd_kafka_op_throttle_time(rkb, rkb->rkb_rk->rk_rep,
                                          Throttle_Time);
        }

        /* ProduceResponse tags */
        rd_kafka_buf_read_tags(rkbuf, rd_kafkap_Produce_reply_tags_parse,
                               &ProduceTags);

        rd_kafka_handle_Produce_metadata_update(rkb, &ProduceTags);

        rd_kafkap_Produce_reply_tags_destroy(&ProduceTags);
        return hdr.ErrorCode;
err_parse:
        rd_kafkap_Produce_reply_tags_destroy(&ProduceTags);
        return rkbuf->rkbuf_err;
err:
        rd_kafkap_Produce_reply_tags_destroy(&ProduceTags);
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
                         * In case the message is possibly persisted
                         * we still treat it as not persisted,
                         * expecting DUPLICATE_SEQUENCE_NUMBER
                         * in case it was persisted or NO_ERROR in case
                         * it wasn't. */
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
 * @brief Set \p batch error codes, corresponding to the indices that caused
 *        the error in 'presult->record_errors', to INVALID_RECORD and
 *        the rest to _INVALID_DIFFERENT_RECORD.
 *
 * @param presult Produce result structure
 * @param batch Batch of messages
 *
 * @locks none
 * @locality broker thread (but not necessarily the leader broker thread)
 */
static void rd_kafka_msgbatch_handle_Produce_result_record_errors(
    const rd_kafka_Produce_result_t *presult,
    rd_kafka_msgbatch_t *batch) {
        rd_kafka_msg_t *rkm = TAILQ_FIRST(&batch->msgq.rkmq_msgs);
        if (presult->record_errors) {
                int i = 0, j = 0;
                while (rkm) {
                        if (j < presult->record_errors_cnt &&
                            presult->record_errors[j].batch_index == i) {
                                rkm->rkm_u.producer.errstr =
                                    presult->record_errors[j].errstr;
                                /* If the batch contained only a single record
                                 * error, then we can unambiguously use the
                                 * error corresponding to the partition-level
                                 * error code. */
                                if (presult->record_errors_cnt > 1)
                                        rkm->rkm_err =
                                            RD_KAFKA_RESP_ERR_INVALID_RECORD;
                                j++;
                        } else {
                                /* If the response contains record errors, then
                                 * the records which failed validation will be
                                 * present in the response. To avoid confusion
                                 * for the remaining records, we return a
                                 * generic error code. */
                                rkm->rkm_u.producer.errstr =
                                    "Failed to append record because it was "
                                    "part of a batch "
                                    "which had one more more invalid records";
                                rkm->rkm_err =
                                    RD_KAFKA_RESP_ERR__INVALID_DIFFERENT_RECORD;
                        }
                        rkm = TAILQ_NEXT(rkm, rkm_link);
                        i++;
                }
        } else if (presult->errstr) {
                while (rkm) {
                        rkm->rkm_u.producer.errstr = presult->errstr;
                        rkm                        = TAILQ_NEXT(rkm, rkm_link);
                }
        }
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
    const rd_kafka_Produce_result_t *presult,
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
                    .update_next_ack = rd_false,
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

                /* Change error codes if necessary */
                rd_kafka_msgbatch_handle_Produce_result_record_errors(presult,
                                                                      batch);
                /* Enqueue messages for delivery report. */
                rd_kafka_dr_msgq0(rktp->rktp_rkt, &batch->msgq, err, presult);
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
        rd_kafka_msgbatch_t *batch = &request->rkbuf_batch;
        rd_kafka_toppar_t *rktp    = batch->rktp;
        rd_kafka_Produce_result_t *result =
            rd_kafka_Produce_result_new(RD_KAFKA_OFFSET_INVALID, -1);

        /* Unit test interface: inject errors */
        if (unlikely(rk->rk_conf.ut.handle_ProduceResponse != NULL)) {
                err = rk->rk_conf.ut.handle_ProduceResponse(
                    rkb->rkb_rk, rkb->rkb_nodeid, batch->first_msgid, err);
        }

        /* Parse Produce reply (unless the request errored) */
        if (!err && reply)
                err = rd_kafka_handle_Produce_parse(rkb, rktp, reply, request,
                                                    result);

        rd_kafka_msgbatch_handle_Produce_result(rkb, batch, err, result,
                                                request);
        rd_kafka_Produce_result_destroy(result);
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

        const rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            RD_KAFKA_TOPIC_PARTITION_FIELD_OFFSET,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};
        rd_kafka_buf_write_topic_partitions(
            rkbuf, partitions, rd_false /*don't skip invalid offsets*/,
            rd_false /*any offset*/, rd_false /*don't use topic id*/,
            rd_true /*use topic name*/, fields);

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
            rkb, RD_KAFKAP_AlterConfigs, 0, 2, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "AlterConfigs (KIP-133) not supported "
                            "by broker, requires broker version >= 0.11.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_flexver_request(rkb, RD_KAFKAP_AlterConfigs, 1,
                                                 rd_list_cnt(configs) * 200,
                                                 ApiVersion >= 2);

        /* #Resources */
        rd_kafka_buf_write_arraycnt(rkbuf, rd_list_cnt(configs));

        RD_LIST_FOREACH(config, configs, i) {
                const rd_kafka_ConfigEntry_t *entry;
                int ei;

                /* ResourceType */
                rd_kafka_buf_write_i8(rkbuf, config->restype);

                /* ResourceName */
                rd_kafka_buf_write_str(rkbuf, config->name, -1);

                /* #Configs */
                rd_kafka_buf_write_arraycnt(rkbuf,
                                            rd_list_cnt(&config->config));

                RD_LIST_FOREACH(entry, &config->config, ei) {
                        /* Name */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->name, -1);
                        /* Value (nullable) */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->value, -1);

                        rd_kafka_buf_write_tags_empty(rkbuf);
                }

                rd_kafka_buf_write_tags_empty(rkbuf);
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


rd_kafka_resp_err_t rd_kafka_IncrementalAlterConfigsRequest(
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
            rkb, RD_KAFKAP_IncrementalAlterConfigs, 0, 1, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "IncrementalAlterConfigs (KIP-339) not supported "
                            "by broker, requires broker version >= 2.3.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_IncrementalAlterConfigs, 1,
            rd_list_cnt(configs) * 200, ApiVersion >= 1);

        /* #Resources */
        rd_kafka_buf_write_arraycnt(rkbuf, rd_list_cnt(configs));

        RD_LIST_FOREACH(config, configs, i) {
                const rd_kafka_ConfigEntry_t *entry;
                int ei;

                /* ResourceType */
                rd_kafka_buf_write_i8(rkbuf, config->restype);

                /* ResourceName */
                rd_kafka_buf_write_str(rkbuf, config->name, -1);

                /* #Configs */
                rd_kafka_buf_write_arraycnt(rkbuf,
                                            rd_list_cnt(&config->config));

                RD_LIST_FOREACH(entry, &config->config, ei) {
                        /* Name */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->name, -1);
                        /* ConfigOperation */
                        rd_kafka_buf_write_i8(rkbuf, entry->a.op_type);
                        /* Value (nullable) */
                        rd_kafka_buf_write_str(rkbuf, entry->kv->value, -1);

                        rd_kafka_buf_write_tags_empty(rkbuf);
                }

                rd_kafka_buf_write_tags_empty(rkbuf);
        }

        /* timeout */
        op_timeout = rd_kafka_confval_get_int(&options->operation_timeout);
        if (op_timeout > rkb->rkb_rk->rk_conf.socket_timeout_ms)
                rd_kafka_buf_set_abs_timeout(rkbuf, op_timeout + 1000, 0);

        /* ValidateOnly */
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
 * @brief Construct and send ElectLeadersRequest to \p rkb
 *        with the partitions (ElectLeaders_t*) in \p elect_leaders, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
rd_kafka_resp_err_t rd_kafka_ElectLeadersRequest(
    rd_kafka_broker_t *rkb,
    const rd_list_t *elect_leaders /*(rd_kafka_EleactLeaders_t*)*/,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion;
        const rd_kafka_ElectLeaders_t *elect_leaders_request;
        int rd_buf_size_estimate;
        int op_timeout;

        if (rd_list_cnt(elect_leaders) == 0) {
                rd_snprintf(errstr, errstr_size,
                            "No partitions specified for leader election");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        elect_leaders_request = rd_list_elem(elect_leaders, 0);

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_ElectLeaders, 0, 2, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "ElectLeaders Admin API (KIP-460) not supported "
                            "by broker, requires broker version >= 2.4.0");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rd_buf_size_estimate =
            1 /* ElectionType */ + 4 /* #TopicPartitions */ + 4 /* TimeoutMs */;
        if (elect_leaders_request->partitions)
                rd_buf_size_estimate +=
                    (50 + 4) * elect_leaders_request->partitions->cnt;
        rkbuf = rd_kafka_buf_new_flexver_request(rkb, RD_KAFKAP_ElectLeaders, 1,
                                                 rd_buf_size_estimate,
                                                 ApiVersion >= 2);

        if (ApiVersion >= 1) {
                /* Election type */
                rd_kafka_buf_write_i8(rkbuf,
                                      elect_leaders_request->election_type);
        }

        /* Write partition list */
        if (elect_leaders_request->partitions) {
                const rd_kafka_topic_partition_field_t fields[] = {
                    RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
                    RD_KAFKA_TOPIC_PARTITION_FIELD_END};
                rd_kafka_buf_write_topic_partitions(
                    rkbuf, elect_leaders_request->partitions,
                    rd_false /*don't skip invalid offsets*/,
                    rd_false /* any offset */,
                    rd_false /* don't use topic_id */,
                    rd_true /* use topic_names */, fields);
        } else {
                rd_kafka_buf_write_arraycnt(rkbuf, -1);
        }

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

rd_kafka_resp_err_t
rd_kafka_GetTelemetrySubscriptionsRequest(rd_kafka_broker_t *rkb,
                                          char *errstr,
                                          size_t errstr_size,
                                          rd_kafka_replyq_t replyq,
                                          rd_kafka_resp_cb_t *resp_cb,
                                          void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_GetTelemetrySubscriptions, 0, 0, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "GetTelemetrySubscriptions (KIP-714) not supported "
                            "by broker, requires broker version >= 3.X.Y");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_GetTelemetrySubscriptions, 1,
            16 /* client_instance_id */, rd_true);

        rd_kafka_buf_write_uuid(rkbuf,
                                &rkb->rkb_rk->rk_telemetry.client_instance_id);

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

rd_kafka_resp_err_t
rd_kafka_PushTelemetryRequest(rd_kafka_broker_t *rkb,
                              rd_kafka_Uuid_t *client_instance_id,
                              int32_t subscription_id,
                              rd_bool_t terminating,
                              const rd_kafka_compression_t compression_type,
                              const void *metrics,
                              size_t metrics_size,
                              char *errstr,
                              size_t errstr_size,
                              rd_kafka_replyq_t replyq,
                              rd_kafka_resp_cb_t *resp_cb,
                              void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_PushTelemetry, 0, 0, NULL);
        if (ApiVersion == -1) {
                rd_snprintf(errstr, errstr_size,
                            "PushTelemetryRequest (KIP-714) not supported ");
                rd_kafka_replyq_destroy(&replyq);
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        size_t len = sizeof(rd_kafka_Uuid_t) + sizeof(int32_t) +
                     sizeof(rd_bool_t) + sizeof(compression_type) +
                     metrics_size;
        rkbuf = rd_kafka_buf_new_flexver_request(rkb, RD_KAFKAP_PushTelemetry,
                                                 1, len, rd_true);

        rd_kafka_buf_write_uuid(rkbuf, client_instance_id);
        rd_kafka_buf_write_i32(rkbuf, subscription_id);
        rd_kafka_buf_write_bool(rkbuf, terminating);
        rd_kafka_buf_write_i8(rkbuf, compression_type);

        rd_kafkap_bytes_t *metric_bytes =
            rd_kafkap_bytes_new(metrics, metrics_size);
        rd_kafka_buf_write_kbytes(rkbuf, metric_bytes);
        rd_free(metric_bytes);

        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_NO_RETRIES;


        /* Processing... */
        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

void rd_kafka_handle_GetTelemetrySubscriptions(rd_kafka_t *rk,
                                               rd_kafka_broker_t *rkb,
                                               rd_kafka_resp_err_t err,
                                               rd_kafka_buf_t *rkbuf,
                                               rd_kafka_buf_t *request,
                                               void *opaque) {
        int16_t ErrorCode           = 0;
        const int log_decode_errors = LOG_ERR;
        int32_t arraycnt;
        size_t i;
        rd_kafka_Uuid_t prev_client_instance_id =
            rk->rk_telemetry.client_instance_id;

        if (err == RD_KAFKA_RESP_ERR__DESTROY) {
                /* Termination */
                return;
        }

        if (err)
                goto err;

        rd_kafka_buf_read_throttle_time(rkbuf);

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

        if (ErrorCode) {
                err = ErrorCode;
                goto err;
        }

        rd_kafka_buf_read_uuid(rkbuf, &rk->rk_telemetry.client_instance_id);
        rd_kafka_buf_read_i32(rkbuf, &rk->rk_telemetry.subscription_id);

        rd_kafka_dbg(
            rk, TELEMETRY, "GETSUBSCRIPTIONS", "Parsing: client instance id %s",
            rd_kafka_Uuid_base64str(&rk->rk_telemetry.client_instance_id));
        rd_kafka_dbg(rk, TELEMETRY, "GETSUBSCRIPTIONS",
                     "Parsing: subscription id %" PRId32,
                     rk->rk_telemetry.subscription_id);

        rd_kafka_buf_read_arraycnt(rkbuf, &arraycnt, -1);

        if (arraycnt) {
                rk->rk_telemetry.accepted_compression_types_cnt = arraycnt;
                rk->rk_telemetry.accepted_compression_types =
                    rd_calloc(arraycnt, sizeof(rd_kafka_compression_t));

                for (i = 0; i < (size_t)arraycnt; i++)
                        rd_kafka_buf_read_i8(
                            rkbuf,
                            &rk->rk_telemetry.accepted_compression_types[i]);
        } else {
                rk->rk_telemetry.accepted_compression_types_cnt = 1;
                rk->rk_telemetry.accepted_compression_types =
                    rd_calloc(1, sizeof(rd_kafka_compression_t));
                rk->rk_telemetry.accepted_compression_types[0] =
                    RD_KAFKA_COMPRESSION_NONE;
        }

        rd_kafka_buf_read_i32(rkbuf, &rk->rk_telemetry.push_interval_ms);
        rd_kafka_buf_read_i32(rkbuf, &rk->rk_telemetry.telemetry_max_bytes);
        rd_kafka_buf_read_bool(rkbuf, &rk->rk_telemetry.delta_temporality);


        if (rk->rk_telemetry.subscription_id &&
            rd_kafka_Uuid_cmp(prev_client_instance_id,
                              rk->rk_telemetry.client_instance_id)) {
                rd_kafka_log(
                    rk, LOG_INFO, "GETSUBSCRIPTIONS",
                    "Telemetry client instance id changed from %s to %s",
                    rd_kafka_Uuid_base64str(&prev_client_instance_id),
                    rd_kafka_Uuid_base64str(
                        &rk->rk_telemetry.client_instance_id));
        }

        rd_kafka_dbg(rk, TELEMETRY, "GETSUBSCRIPTIONS",
                     "Parsing: push interval %" PRId32,
                     rk->rk_telemetry.push_interval_ms);

        rd_kafka_buf_read_arraycnt(rkbuf, &arraycnt, 1000);

        if (arraycnt) {
                rk->rk_telemetry.requested_metrics_cnt = arraycnt;
                rk->rk_telemetry.requested_metrics =
                    rd_calloc(arraycnt, sizeof(char *));

                for (i = 0; i < (size_t)arraycnt; i++) {
                        rd_kafkap_str_t Metric;
                        rd_kafka_buf_read_str(rkbuf, &Metric);
                        rk->rk_telemetry.requested_metrics[i] =
                            rd_strdup(Metric.str);
                }
        }

        rd_kafka_dbg(rk, TELEMETRY, "GETSUBSCRIPTIONS",
                     "Parsing: requested metrics count %" PRIusz,
                     rk->rk_telemetry.requested_metrics_cnt);

        rd_kafka_handle_get_telemetry_subscriptions(rk, err);
        return;

err_parse:
        err = rkbuf->rkbuf_err;
        goto err;

err:
        /* TODO: Add error handling actions, possibly call
         * rd_kafka_handle_get_telemetry_subscriptions with error. */
        rd_kafka_handle_get_telemetry_subscriptions(rk, err);
}

void rd_kafka_handle_PushTelemetry(rd_kafka_t *rk,
                                   rd_kafka_broker_t *rkb,
                                   rd_kafka_resp_err_t err,
                                   rd_kafka_buf_t *rkbuf,
                                   rd_kafka_buf_t *request,
                                   void *opaque) {
        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode;

        if (err == RD_KAFKA_RESP_ERR__DESTROY) {
                /* Termination */
                return;
        }

        if (err)
                goto err;


        rd_kafka_buf_read_throttle_time(rkbuf);

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

        if (ErrorCode) {
                err = ErrorCode;
                goto err;
        }
        rd_kafka_handle_push_telemetry(rk, err);
        return;
err_parse:
        err = rkbuf->rkbuf_err;
        goto err;

err:
        /* TODO: Add error handling actions, possibly call
         * rd_kafka_handle_push_telemetry with error. */
        rd_kafka_handle_push_telemetry(rk, err);
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
        rd_kafka_pid_t pid = {.id = 1000, .epoch = 0};
        rd_kafka_Produce_result_t *result =
            rd_kafka_Produce_result_new(1, 1000);
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
                                                result, request[i]);
        result->offset += r;
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
            RD_KAFKA_RESP_ERR_NOT_LEADER_FOR_PARTITION, result, request[i]);
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
            RD_KAFKA_RESP_ERR_OUT_OF_ORDER_SEQUENCE_NUMBER, result, request[i]);
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
            RD_KAFKA_RESP_ERR_OUT_OF_ORDER_SEQUENCE_NUMBER, result, request[i]);
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

        /* Sleep a short while to make sure the retry backoff expires.
         */
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
                    result, request[i]);
                result->offset += r;
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

        /* Verify the expected number of good delivery reports were seen
         */
        RD_UT_ASSERT(drcnt == msgcnt, "expected %d DRs, not %d", msgcnt, drcnt);

        rd_kafka_Produce_result_destroy(result);
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
