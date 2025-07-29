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


/**
 * This is the high level consumer API which is mutually exclusive
 * with the old legacy simple consumer.
 * Only one of these interfaces may be used on a given rd_kafka_t handle.
 */

#include "rdkafka_int.h"


rd_kafka_resp_err_t rd_kafka_unsubscribe(rd_kafka_t *rk) {
        rd_kafka_cgrp_t *rkcg;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return RD_KAFKA_RESP_ERR__UNKNOWN_GROUP;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req2(rkcg->rkcg_ops, RD_KAFKA_OP_SUBSCRIBE));
}


/** @returns 1 if the topic is invalid (bad regex, empty), else 0 if valid. */
static size_t _invalid_topic_cb(const rd_kafka_topic_partition_t *rktpar,
                                void *opaque) {
        rd_regex_t *re;
        char errstr[1];

        if (!*rktpar->topic)
                return 1;

        if (*rktpar->topic != '^')
                return 0;

        if (!(re = rd_regex_comp(rktpar->topic, errstr, sizeof(errstr))))
                return 1;

        rd_regex_destroy(re);

        return 0;
}


rd_kafka_resp_err_t
rd_kafka_subscribe(rd_kafka_t *rk,
                   const rd_kafka_topic_partition_list_t *topics) {

        rd_kafka_op_t *rko;
        rd_kafka_cgrp_t *rkcg;
        rd_kafka_topic_partition_list_t *topics_cpy;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return RD_KAFKA_RESP_ERR__UNKNOWN_GROUP;

        /* Validate topics */
        if (topics->cnt == 0 || rd_kafka_topic_partition_list_sum(
                                    topics, _invalid_topic_cb, NULL) > 0)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        topics_cpy = rd_kafka_topic_partition_list_copy(topics);
        if (rd_kafka_topic_partition_list_has_duplicates(
                topics_cpy, rd_true /*ignore partition field*/)) {
                rd_kafka_topic_partition_list_destroy(topics_cpy);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        rko                         = rd_kafka_op_new(RD_KAFKA_OP_SUBSCRIBE);
        rko->rko_u.subscribe.topics = topics_cpy;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(rkcg->rkcg_ops, rko, RD_POLL_INFINITE));
}


rd_kafka_error_t *
rd_kafka_assign0(rd_kafka_t *rk,
                 rd_kafka_assign_method_t assign_method,
                 const rd_kafka_topic_partition_list_t *partitions) {
        rd_kafka_op_t *rko;
        rd_kafka_cgrp_t *rkcg;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__UNKNOWN_GROUP,
                                          "Requires a consumer with group.id "
                                          "configured");

        rko = rd_kafka_op_new(RD_KAFKA_OP_ASSIGN);

        rko->rko_u.assign.method = assign_method;

        if (partitions)
                rko->rko_u.assign.partitions =
                    rd_kafka_topic_partition_list_copy(partitions);

        return rd_kafka_op_error_destroy(
            rd_kafka_op_req(rkcg->rkcg_ops, rko, RD_POLL_INFINITE));
}


rd_kafka_resp_err_t
rd_kafka_assign(rd_kafka_t *rk,
                const rd_kafka_topic_partition_list_t *partitions) {
        rd_kafka_error_t *error;
        rd_kafka_resp_err_t err;

        error = rd_kafka_assign0(rk, RD_KAFKA_ASSIGN_METHOD_ASSIGN, partitions);

        if (!error)
                err = RD_KAFKA_RESP_ERR_NO_ERROR;
        else {
                err = rd_kafka_error_code(error);
                rd_kafka_error_destroy(error);
        }

        return err;
}


rd_kafka_error_t *
rd_kafka_incremental_assign(rd_kafka_t *rk,
                            const rd_kafka_topic_partition_list_t *partitions) {
        if (!partitions)
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__INVALID_ARG,
                                          "partitions must not be NULL");

        return rd_kafka_assign0(rk, RD_KAFKA_ASSIGN_METHOD_INCR_ASSIGN,
                                partitions);
}


rd_kafka_error_t *rd_kafka_incremental_unassign(
    rd_kafka_t *rk,
    const rd_kafka_topic_partition_list_t *partitions) {
        if (!partitions)
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__INVALID_ARG,
                                          "partitions must not be NULL");

        return rd_kafka_assign0(rk, RD_KAFKA_ASSIGN_METHOD_INCR_UNASSIGN,
                                partitions);
}


int rd_kafka_assignment_lost(rd_kafka_t *rk) {
        rd_kafka_cgrp_t *rkcg;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return 0;

        return rd_kafka_cgrp_assignment_is_lost(rkcg) == rd_true;
}


const char *rd_kafka_rebalance_protocol(rd_kafka_t *rk) {
        rd_kafka_op_t *rko;
        rd_kafka_cgrp_t *rkcg;
        const char *result;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return NULL;

        rko = rd_kafka_op_req2(rkcg->rkcg_ops,
                               RD_KAFKA_OP_GET_REBALANCE_PROTOCOL);

        if (!rko)
                return NULL;
        else if (rko->rko_err) {
                rd_kafka_op_destroy(rko);
                return NULL;
        }

        result = rko->rko_u.rebalance_protocol.str;

        rd_kafka_op_destroy(rko);

        return result;
}


rd_kafka_resp_err_t
rd_kafka_assignment(rd_kafka_t *rk,
                    rd_kafka_topic_partition_list_t **partitions) {
        rd_kafka_op_t *rko;
        rd_kafka_resp_err_t err;
        rd_kafka_cgrp_t *rkcg;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return RD_KAFKA_RESP_ERR__UNKNOWN_GROUP;

        rko = rd_kafka_op_req2(rkcg->rkcg_ops, RD_KAFKA_OP_GET_ASSIGNMENT);
        if (!rko)
                return RD_KAFKA_RESP_ERR__TIMED_OUT;

        err = rko->rko_err;

        *partitions                  = rko->rko_u.assign.partitions;
        rko->rko_u.assign.partitions = NULL;
        rd_kafka_op_destroy(rko);

        if (!*partitions && !err) {
                /* Create an empty list for convenience of the caller */
                *partitions = rd_kafka_topic_partition_list_new(0);
        }

        return err;
}

rd_kafka_resp_err_t
rd_kafka_subscription(rd_kafka_t *rk,
                      rd_kafka_topic_partition_list_t **topics) {
        rd_kafka_op_t *rko;
        rd_kafka_resp_err_t err;
        rd_kafka_cgrp_t *rkcg;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return RD_KAFKA_RESP_ERR__UNKNOWN_GROUP;

        rko = rd_kafka_op_req2(rkcg->rkcg_ops, RD_KAFKA_OP_GET_SUBSCRIPTION);
        if (!rko)
                return RD_KAFKA_RESP_ERR__TIMED_OUT;

        err = rko->rko_err;

        *topics                     = rko->rko_u.subscribe.topics;
        rko->rko_u.subscribe.topics = NULL;
        rd_kafka_op_destroy(rko);

        if (!*topics && !err) {
                /* Create an empty list for convenience of the caller */
                *topics = rd_kafka_topic_partition_list_new(0);
        }

        return err;
}


rd_kafka_resp_err_t
rd_kafka_pause_partitions(rd_kafka_t *rk,
                          rd_kafka_topic_partition_list_t *partitions) {
        return rd_kafka_toppars_pause_resume(rk, rd_true /*pause*/, RD_SYNC,
                                             RD_KAFKA_TOPPAR_F_APP_PAUSE,
                                             partitions);
}


rd_kafka_resp_err_t
rd_kafka_resume_partitions(rd_kafka_t *rk,
                           rd_kafka_topic_partition_list_t *partitions) {
        return rd_kafka_toppars_pause_resume(rk, rd_false /*resume*/, RD_SYNC,
                                             RD_KAFKA_TOPPAR_F_APP_PAUSE,
                                             partitions);
}
