/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018-2022, Magnus Edenhill
 *               2023 Confluent Inc.
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

#ifndef _RDKAFKA_AUX_H_
#define _RDKAFKA_AUX_H_

/**
 * @name Auxiliary types
 */

#include "rdkafka_conf.h"

/**
 * @brief Topic [ + Error code + Error string ]
 *
 * @remark Public type.
 * @remark Single allocation.
 */
struct rd_kafka_topic_result_s {
        char *topic;             /**< Points to data */
        rd_kafka_resp_err_t err; /**< Error code */
        char *errstr;            /**< Points to data after topic, unless NULL */
        char data[1];            /**< topic followed by errstr */
};

void rd_kafka_topic_result_destroy(rd_kafka_topic_result_t *terr);
void rd_kafka_topic_result_free(void *ptr);

rd_kafka_topic_result_t *rd_kafka_topic_result_new(const char *topic,
                                                   ssize_t topic_size,
                                                   rd_kafka_resp_err_t err,
                                                   const char *errstr);

/**
 * @brief Group [ + Error object ]
 *
 * @remark Public type.
 * @remark Single allocation.
 */
struct rd_kafka_group_result_s {
        char *group;             /**< Points to data */
        rd_kafka_error_t *error; /**< Error object, or NULL on success */
        /** Partitions, used by DeleteConsumerGroupOffsets. */
        rd_kafka_topic_partition_list_t *partitions;
        char data[1]; /**< Group name */
};

void rd_kafka_group_result_destroy(rd_kafka_group_result_t *terr);
void rd_kafka_group_result_free(void *ptr);

rd_kafka_group_result_t *
rd_kafka_group_result_new(const char *group,
                          ssize_t group_size,
                          const rd_kafka_topic_partition_list_t *partitions,
                          rd_kafka_error_t *error);

/**
 * @brief Acl creation result [ Error code + Error string ]
 *
 * @remark Public type.
 * @remark Single allocation.
 */
struct rd_kafka_acl_result_s {
        rd_kafka_error_t *error; /**< Error object, or NULL on success. */
};

void rd_kafka_acl_result_destroy(rd_kafka_acl_result_t *acl_res);
void rd_kafka_acl_result_free(void *ptr);

rd_kafka_acl_result_t *rd_kafka_acl_result_new(rd_kafka_error_t *error);

rd_kafka_group_result_t *
rd_kafka_group_result_copy(const rd_kafka_group_result_t *groupres);
void *rd_kafka_group_result_copy_opaque(const void *src_groupres, void *opaque);
/**@}*/

/**
 * @struct Node represents a broker.
 * It's the public type.
 */
typedef struct rd_kafka_Node_s {
        int id;        /*< Node id */
        char *host;    /*< Node host */
        uint16_t port; /*< Node port */
        char *rack;    /*< (optional) Node rack id */
} rd_kafka_Node_t;

rd_kafka_Node_t *rd_kafka_Node_new(int32_t id,
                                   const char *host,
                                   uint16_t port,
                                   const char *rack_id);

rd_kafka_Node_t *rd_kafka_Node_new_from_brokers(
    int32_t id,
    const struct rd_kafka_metadata_broker *brokers_sorted,
    const rd_kafka_metadata_broker_internal_t *brokers_internal,
    int broker_cnt);

rd_kafka_Node_t *rd_kafka_Node_copy(const rd_kafka_Node_t *src);

void rd_kafka_Node_destroy(rd_kafka_Node_t *node);

void rd_kafka_Node_free(void *node);

/**
 * @brief Represents a topic partition result.
 *
 * @remark Public Type
 */
struct rd_kafka_topic_partition_result_s {
        rd_kafka_topic_partition_t *topic_partition;
        rd_kafka_error_t *error;
};

/**
 * @brief Create a new rd_kafka_topic_partition_result_t object.
 *
 * @param topic The topic name.
 * @param partition The partition number.
 * @param err The error code.
 * @param errstr The error string.
 *
 * @returns a newly allocated rd_kafka_topic_partition_result_t object.
 *          Use rd_kafka_topic_partition_result_destroy() to free object when
 *          done.
 */
rd_kafka_topic_partition_result_t *
rd_kafka_topic_partition_result_new(const char *topic,
                                    int32_t partition,
                                    rd_kafka_resp_err_t err,
                                    const char *errstr);

rd_kafka_topic_partition_result_t *rd_kafka_topic_partition_result_copy(
    const rd_kafka_topic_partition_result_t *src);

void *rd_kafka_topic_partition_result_copy_opaque(const void *src,
                                                  void *opaque);

void rd_kafka_topic_partition_result_destroy(
    rd_kafka_topic_partition_result_t *partition_result);

void rd_kafka_topic_partition_result_destroy_array(
    rd_kafka_topic_partition_result_t **partition_results,
    int32_t partition_results_cnt);

void rd_kafka_topic_partition_result_free(void *ptr);

#endif /* _RDKAFKA_AUX_H_ */
