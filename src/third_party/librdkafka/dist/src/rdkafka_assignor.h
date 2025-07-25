/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2015 Magnus Edenhill
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
#ifndef _RDKAFKA_ASSIGNOR_H_
#define _RDKAFKA_ASSIGNOR_H_



/*!
 * Enumerates the different rebalance protocol types.
 *
 * @sa rd_kafka_rebalance_protocol()
 */
typedef enum rd_kafka_rebalance_protocol_t {
        RD_KAFKA_REBALANCE_PROTOCOL_NONE,       /**< Rebalance protocol is
                                                     unknown */
        RD_KAFKA_REBALANCE_PROTOCOL_EAGER,      /**< Eager rebalance
                                                     protocol */
        RD_KAFKA_REBALANCE_PROTOCOL_COOPERATIVE /**< Cooperative
                                                     rebalance protocol*/
} rd_kafka_rebalance_protocol_t;



typedef struct rd_kafka_group_member_s {
        /** Subscribed topics (partition field is ignored). */
        rd_kafka_topic_partition_list_t *rkgm_subscription;
        /** Partitions assigned to this member after running the assignor.
         *  E.g., the current assignment coming out of the rebalance. */
        rd_kafka_topic_partition_list_t *rkgm_assignment;
        /** Partitions reported as currently owned by the member, read
         *  from consumer metadata. E.g., the current assignment going into
         *  the rebalance. */
        rd_kafka_topic_partition_list_t *rkgm_owned;
        /** List of eligible topics in subscription. E.g., subscribed topics
         *  that exist. */
        rd_list_t rkgm_eligible;
        /** Member id (e.g., client.id-some-uuid). */
        rd_kafkap_str_t *rkgm_member_id;
        /** Group instance id. */
        rd_kafkap_str_t *rkgm_group_instance_id;
        /** Member-specific opaque userdata. */
        rd_kafkap_bytes_t *rkgm_userdata;
        /** Member metadata, e.g., the currently owned partitions. */
        rd_kafkap_bytes_t *rkgm_member_metadata;
        /** Group generation id. */
        int rkgm_generation;
} rd_kafka_group_member_t;


int rd_kafka_group_member_cmp(const void *_a, const void *_b);

int rd_kafka_group_member_find_subscription(rd_kafka_t *rk,
                                            const rd_kafka_group_member_t *rkgm,
                                            const char *topic);


/**
 * Structure to hold metadata for a single topic and all its
 * subscribing members.
 */
typedef struct rd_kafka_assignor_topic_s {
        const rd_kafka_metadata_topic_t *metadata;
        rd_list_t members; /* rd_kafka_group_member_t * */
} rd_kafka_assignor_topic_t;


int rd_kafka_assignor_topic_cmp(const void *_a, const void *_b);


typedef struct rd_kafka_assignor_s {
        rd_kafkap_str_t *rkas_protocol_type;
        rd_kafkap_str_t *rkas_protocol_name;

        int rkas_enabled;

        /** Order for strategies. */
        int rkas_index;

        rd_kafka_rebalance_protocol_t rkas_protocol;

        rd_kafka_resp_err_t (*rkas_assign_cb)(
            rd_kafka_t *rk,
            const struct rd_kafka_assignor_s *rkas,
            const char *member_id,
            const rd_kafka_metadata_t *metadata,
            rd_kafka_group_member_t *members,
            size_t member_cnt,
            rd_kafka_assignor_topic_t **eligible_topics,
            size_t eligible_topic_cnt,
            char *errstr,
            size_t errstr_size,
            void *opaque);

        rd_kafkap_bytes_t *(*rkas_get_metadata_cb)(
            const struct rd_kafka_assignor_s *rkas,
            void *assignor_state,
            const rd_list_t *topics,
            const rd_kafka_topic_partition_list_t *owned_partitions);

        void (*rkas_on_assignment_cb)(
            const struct rd_kafka_assignor_s *rkas,
            void **assignor_state,
            const rd_kafka_topic_partition_list_t *assignment,
            const rd_kafkap_bytes_t *assignment_userdata,
            const rd_kafka_consumer_group_metadata_t *rkcgm);

        void (*rkas_destroy_state_cb)(void *assignor_state);

        int (*rkas_unittest)(void);

        void *rkas_opaque;
} rd_kafka_assignor_t;


rd_kafka_resp_err_t rd_kafka_assignor_add(
    rd_kafka_t *rk,
    const char *protocol_type,
    const char *protocol_name,
    rd_kafka_rebalance_protocol_t rebalance_protocol,
    rd_kafka_resp_err_t (*assign_cb)(
        rd_kafka_t *rk,
        const struct rd_kafka_assignor_s *rkas,
        const char *member_id,
        const rd_kafka_metadata_t *metadata,
        rd_kafka_group_member_t *members,
        size_t member_cnt,
        rd_kafka_assignor_topic_t **eligible_topics,
        size_t eligible_topic_cnt,
        char *errstr,
        size_t errstr_size,
        void *opaque),
    rd_kafkap_bytes_t *(*get_metadata_cb)(
        const struct rd_kafka_assignor_s *rkas,
        void *assignor_state,
        const rd_list_t *topics,
        const rd_kafka_topic_partition_list_t *owned_partitions),
    void (*on_assignment_cb)(const struct rd_kafka_assignor_s *rkas,
                             void **assignor_state,
                             const rd_kafka_topic_partition_list_t *assignment,
                             const rd_kafkap_bytes_t *userdata,
                             const rd_kafka_consumer_group_metadata_t *rkcgm),
    void (*destroy_state_cb)(void *assignor_state),
    int (*unittest_cb)(void),
    void *opaque);

rd_kafkap_bytes_t *rd_kafka_consumer_protocol_member_metadata_new(
    const rd_list_t *topics,
    const void *userdata,
    size_t userdata_size,
    const rd_kafka_topic_partition_list_t *owned_partitions);

rd_kafkap_bytes_t *rd_kafka_assignor_get_metadata_with_empty_userdata(
    const rd_kafka_assignor_t *rkas,
    void *assignor_state,
    const rd_list_t *topics,
    const rd_kafka_topic_partition_list_t *owned_partitions);


void rd_kafka_assignor_update_subscription(
    const rd_kafka_assignor_t *rkas,
    const rd_kafka_topic_partition_list_t *subscription);


rd_kafka_resp_err_t rd_kafka_assignor_run(struct rd_kafka_cgrp_s *rkcg,
                                          const rd_kafka_assignor_t *rkas,
                                          rd_kafka_metadata_t *metadata,
                                          rd_kafka_group_member_t *members,
                                          int member_cnt,
                                          char *errstr,
                                          size_t errstr_size);

rd_kafka_assignor_t *rd_kafka_assignor_find(rd_kafka_t *rk,
                                            const char *protocol);

int rd_kafka_assignors_init(rd_kafka_t *rk, char *errstr, size_t errstr_size);
void rd_kafka_assignors_term(rd_kafka_t *rk);



void rd_kafka_group_member_clear(rd_kafka_group_member_t *rkgm);


rd_kafka_resp_err_t rd_kafka_range_assignor_init(rd_kafka_t *rk);
rd_kafka_resp_err_t rd_kafka_roundrobin_assignor_init(rd_kafka_t *rk);
rd_kafka_resp_err_t rd_kafka_sticky_assignor_init(rd_kafka_t *rk);

#endif /* _RDKAFKA_ASSIGNOR_H_ */
