/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2015-2022, Magnus Edenhill
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
#ifndef _RDKAFKA_ASSIGNOR_H_
#define _RDKAFKA_ASSIGNOR_H_

#include "rdkafka_metadata.h"

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
        /** Member rack id. */
        rd_kafkap_str_t *rkgm_rack_id;
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
        const rd_kafka_metadata_topic_internal_t *metadata_internal;
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
            const rd_kafka_topic_partition_list_t *owned_partitions,
            const rd_kafkap_str_t *rack_id);

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
        const rd_kafka_topic_partition_list_t *owned_partitions,
        const rd_kafkap_str_t *rack_id),
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
    const rd_kafka_topic_partition_list_t *owned_partitions,
    int generation,
    const rd_kafkap_str_t *rack_id);

rd_kafkap_bytes_t *rd_kafka_assignor_get_metadata_with_empty_userdata(
    const rd_kafka_assignor_t *rkas,
    void *assignor_state,
    const rd_list_t *topics,
    const rd_kafka_topic_partition_list_t *owned_partitions,
    const rd_kafkap_str_t *rack_id);


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
rd_bool_t
rd_kafka_use_rack_aware_assignment(rd_kafka_assignor_topic_t **topics,
                                   size_t topic_cnt,
                                   const rd_kafka_metadata_internal_t *mdi);

/**
 * @name Common unit test functions, macros, and enums to use across assignors.
 *
 *
 *
 */

/* Tests can be parametrized to contain either only broker racks, only consumer
 * racks or both.*/
typedef enum {
        RD_KAFKA_RANGE_ASSIGNOR_UT_NO_BROKER_RACK           = 0,
        RD_KAFKA_RANGE_ASSIGNOR_UT_NO_CONSUMER_RACK         = 1,
        RD_KAFKA_RANGE_ASSIGNOR_UT_BROKER_AND_CONSUMER_RACK = 2,
        RD_KAFKA_RANGE_ASSIGNOR_UT_CONFIG_CNT               = 3,
} rd_kafka_assignor_ut_rack_config_t;


void ut_populate_internal_broker_metadata(rd_kafka_metadata_internal_t *mdi,
                                          int num_broker_racks,
                                          rd_kafkap_str_t *all_racks[],
                                          size_t all_racks_cnt);

void ut_populate_internal_topic_metadata(rd_kafka_metadata_internal_t *mdi);

void ut_destroy_metadata(rd_kafka_metadata_t *md);

void ut_set_owned(rd_kafka_group_member_t *rkgm);

void ut_print_toppar_list(const rd_kafka_topic_partition_list_t *partitions);

void ut_init_member(rd_kafka_group_member_t *rkgm, const char *member_id, ...);

void ut_init_member_with_rackv(rd_kafka_group_member_t *rkgm,
                               const char *member_id,
                               const rd_kafkap_str_t *rack_id,
                               ...);

void ut_init_member_with_rack(rd_kafka_group_member_t *rkgm,
                              const char *member_id,
                              const rd_kafkap_str_t *rack_id,
                              char *topics[],
                              size_t topic_cnt);

int verifyAssignment0(const char *function,
                      int line,
                      rd_kafka_group_member_t *rkgm,
                      ...);

int verifyMultipleAssignment0(const char *function,
                              int line,
                              rd_kafka_group_member_t *rkgms,
                              size_t member_cnt,
                              ...);

int verifyNumPartitionsWithRackMismatch0(const char *function,
                                         int line,
                                         rd_kafka_metadata_t *metadata,
                                         rd_kafka_group_member_t *rkgms,
                                         size_t member_cnt,
                                         int expectedNumMismatch);

#define verifyAssignment(rkgm, ...)                                            \
        do {                                                                   \
                if (verifyAssignment0(__FUNCTION__, __LINE__, rkgm,            \
                                      __VA_ARGS__))                            \
                        return 1;                                              \
        } while (0)

#define verifyMultipleAssignment(rkgms, member_cnt, ...)                       \
        do {                                                                   \
                if (verifyMultipleAssignment0(__FUNCTION__, __LINE__, rkgms,   \
                                              member_cnt, __VA_ARGS__))        \
                        return 1;                                              \
        } while (0)

#define verifyNumPartitionsWithRackMismatch(metadata, rkgms, member_cnt,       \
                                            expectedNumMismatch)               \
        do {                                                                   \
                if (verifyNumPartitionsWithRackMismatch0(                      \
                        __FUNCTION__, __LINE__, metadata, rkgms, member_cnt,   \
                        expectedNumMismatch))                                  \
                        return 1;                                              \
        } while (0)

int verifyValidityAndBalance0(const char *func,
                              int line,
                              rd_kafka_group_member_t *members,
                              size_t member_cnt,
                              const rd_kafka_metadata_t *metadata);

#define verifyValidityAndBalance(members, member_cnt, metadata)                \
        do {                                                                   \
                if (verifyValidityAndBalance0(__FUNCTION__, __LINE__, members, \
                                              member_cnt, metadata))           \
                        return 1;                                              \
        } while (0)

int isFullyBalanced0(const char *function,
                     int line,
                     const rd_kafka_group_member_t *members,
                     size_t member_cnt);

#define isFullyBalanced(members, member_cnt)                                   \
        do {                                                                   \
                if (isFullyBalanced0(__FUNCTION__, __LINE__, members,          \
                                     member_cnt))                              \
                        return 1;                                              \
        } while (0)

/* Helper macro to initialize a consumer with or without a rack depending on the
 * value of parametrization. */
#define ut_initMemberConditionalRack(member_ptr, member_id, rack,              \
                                     parametrization, ...)                     \
        do {                                                                   \
                if (parametrization ==                                         \
                    RD_KAFKA_RANGE_ASSIGNOR_UT_NO_CONSUMER_RACK) {             \
                        ut_init_member(member_ptr, member_id, __VA_ARGS__);    \
                } else {                                                       \
                        ut_init_member_with_rackv(member_ptr, member_id, rack, \
                                                  __VA_ARGS__);                \
                }                                                              \
        } while (0)

/* Helper macro to initialize rd_kafka_metadata_t* with or without replicas
 * depending on the value of parametrization. This accepts variadic arguments
 * for topics. */
#define ut_initMetadataConditionalRack(metadataPtr, replication_factor,                \
                                       num_broker_racks, all_racks,                    \
                                       all_racks_cnt, parametrization, ...)            \
        do {                                                                           \
                int num_brokers = num_broker_racks > 0                                 \
                                      ? replication_factor * num_broker_racks          \
                                      : replication_factor;                            \
                if (parametrization ==                                                 \
                    RD_KAFKA_RANGE_ASSIGNOR_UT_NO_BROKER_RACK) {                       \
                        *(metadataPtr) =                                               \
                            rd_kafka_metadata_new_topic_mockv(__VA_ARGS__);            \
                } else {                                                               \
                        *(metadataPtr) =                                               \
                            rd_kafka_metadata_new_topic_with_partition_replicas_mockv( \
                                replication_factor, num_brokers, __VA_ARGS__);         \
                        ut_populate_internal_broker_metadata(                          \
                            rd_kafka_metadata_get_internal(*(metadataPtr)),            \
                            num_broker_racks, all_racks, all_racks_cnt);               \
                        ut_populate_internal_topic_metadata(                           \
                            rd_kafka_metadata_get_internal(*(metadataPtr)));           \
                }                                                                      \
        } while (0)


/* Helper macro to initialize rd_kafka_metadata_t* with or without replicas
 * depending on the value of parametrization. This accepts a list of topics,
 * rather than being variadic.
 */
#define ut_initMetadataConditionalRack0(                                       \
    metadataPtr, replication_factor, num_broker_racks, all_racks,              \
    all_racks_cnt, parametrization, topics, topic_cnt)                         \
        do {                                                                   \
                int num_brokers = num_broker_racks > 0                         \
                                      ? replication_factor * num_broker_racks  \
                                      : replication_factor;                    \
                if (parametrization ==                                         \
                    RD_KAFKA_RANGE_ASSIGNOR_UT_NO_BROKER_RACK) {               \
                        *(metadataPtr) = rd_kafka_metadata_new_topic_mock(     \
                            topics, topic_cnt, -1, 0);                         \
                } else {                                                       \
                        *(metadataPtr) = rd_kafka_metadata_new_topic_mock(     \
                            topics, topic_cnt, replication_factor,             \
                            num_brokers);                                      \
                        ut_populate_internal_broker_metadata(                  \
                            rd_kafka_metadata_get_internal(*(metadataPtr)),    \
                            num_broker_racks, all_racks, all_racks_cnt);       \
                        ut_populate_internal_topic_metadata(                   \
                            rd_kafka_metadata_get_internal(*(metadataPtr)));   \
                }                                                              \
        } while (0)


#endif /* _RDKAFKA_ASSIGNOR_H_ */
