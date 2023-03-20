/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2019-2022 Magnus Edenhill
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

#ifndef _RDKAFKA_MOCK_H_
#define _RDKAFKA_MOCK_H_

#ifndef _RDKAFKA_H_
#error "rdkafka_mock.h must be included after rdkafka.h"
#endif

#ifdef __cplusplus
extern "C" {
#if 0
} /* Restore indent */
#endif
#endif


/**
 * @name Mock cluster
 *
 * Provides a mock Kafka cluster with a configurable number of brokers
 * that support a reasonable subset of Kafka protocol operations,
 * error injection, etc.
 *
 * There are two ways to use the mock clusters, the most simple approach
 * is to configure `test.mock.num.brokers` (to e.g. 3) on the rd_kafka_t
 * in an existing application, which will replace the configured
 * `bootstrap.servers` with the mock cluster brokers.
 * This approach is convenient to easily test existing applications.
 *
 * The second approach is to explicitly create a mock cluster on an
 * rd_kafka_t instance by using rd_kafka_mock_cluster_new().
 *
 * Mock clusters provide localhost listeners that can be used as the bootstrap
 * servers by multiple rd_kafka_t instances.
 *
 * Currently supported functionality:
 *  - Producer
 *  - Idempotent Producer
 *  - Transactional Producer
 *  - Low-level consumer
 *  - High-level balanced consumer groups with offset commits
 *  - Topic Metadata and auto creation
 *
 * @remark This is an experimental public API that is NOT covered by the
 *         librdkafka API or ABI stability guarantees.
 *
 *
 * @warning THIS IS AN EXPERIMENTAL API, SUBJECT TO CHANGE OR REMOVAL.
 *
 * @{
 */

typedef struct rd_kafka_mock_cluster_s rd_kafka_mock_cluster_t;


/**
 * @brief Create new mock cluster with \p broker_cnt brokers.
 *
 * The broker ids will start at 1 up to and including \p broker_cnt.
 *
 * The \p rk instance is required for internal book keeping but continues
 * to operate as usual.
 */
RD_EXPORT
rd_kafka_mock_cluster_t *rd_kafka_mock_cluster_new(rd_kafka_t *rk,
                                                   int broker_cnt);


/**
 * @brief Destroy mock cluster.
 */
RD_EXPORT
void rd_kafka_mock_cluster_destroy(rd_kafka_mock_cluster_t *mcluster);



/**
 * @returns the rd_kafka_t instance for a cluster as passed to
 *          rd_kafka_mock_cluster_new().
 */
RD_EXPORT rd_kafka_t *
rd_kafka_mock_cluster_handle(const rd_kafka_mock_cluster_t *mcluster);


/**
 * @returns the rd_kafka_mock_cluster_t instance as created by
 *          setting the `test.mock.num.brokers` configuration property,
 *          or NULL if no such instance.
 */
RD_EXPORT rd_kafka_mock_cluster_t *
rd_kafka_handle_mock_cluster(const rd_kafka_t *rk);



/**
 * @returns the mock cluster's bootstrap.servers list
 */
RD_EXPORT const char *
rd_kafka_mock_cluster_bootstraps(const rd_kafka_mock_cluster_t *mcluster);


/**
 * @brief Clear the cluster's error state for the given \p ApiKey.
 */
RD_EXPORT
void rd_kafka_mock_clear_request_errors(rd_kafka_mock_cluster_t *mcluster,
                                        int16_t ApiKey);


/**
 * @brief Push \p cnt errors in the \p ... va-arg list onto the cluster's
 *        error stack for the given \p ApiKey.
 *
 * \p ApiKey is the Kafka protocol request type, e.g., ProduceRequest (0).
 *
 * The following \p cnt protocol requests matching \p ApiKey will fail with the
 * provided error code and removed from the stack, starting with
 * the first error code, then the second, etc.
 *
 * Passing \c RD_KAFKA_RESP_ERR__TRANSPORT will make the mock broker
 * disconnect the client which can be useful to trigger a disconnect on certain
 * requests.
 */
RD_EXPORT
void rd_kafka_mock_push_request_errors(rd_kafka_mock_cluster_t *mcluster,
                                       int16_t ApiKey,
                                       size_t cnt,
                                       ...);


/**
 * @brief Same as rd_kafka_mock_push_request_errors() but takes
 *        an array of errors.
 */
RD_EXPORT void
rd_kafka_mock_push_request_errors_array(rd_kafka_mock_cluster_t *mcluster,
                                        int16_t ApiKey,
                                        size_t cnt,
                                        const rd_kafka_resp_err_t *errors);


/**
 * @brief Push \p cnt errors and RTT tuples in the \p ... va-arg list onto
 *        the broker's error stack for the given \p ApiKey.
 *
 * \p ApiKey is the Kafka protocol request type, e.g., ProduceRequest (0).
 *
 * Each entry is a tuple of:
 *   rd_kafka_resp_err_t err - error to return (or 0)
 *   int rtt_ms              - response RTT/delay in milliseconds (or 0)
 *
 * The following \p cnt protocol requests matching \p ApiKey will fail with the
 * provided error code and removed from the stack, starting with
 * the first error code, then the second, etc.
 *
 * @remark The broker errors take precedence over the cluster errors.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_mock_broker_push_request_error_rtts(rd_kafka_mock_cluster_t *mcluster,
                                             int32_t broker_id,
                                             int16_t ApiKey,
                                             size_t cnt,
                                             ...);



/**
 * @brief Get the count of errors in the broker's error stack for
 *        the given \p ApiKey.
 *
 * @param mcluster the mock cluster.
 * @param broker_id id of the broker in the cluster.
 * @param ApiKey is the Kafka protocol request type, e.g., ProduceRequest (0).
 * @param cntp pointer for receiving the count.
 *
 * @returns \c RD_KAFKA_RESP_ERR_NO_ERROR if the count was retrieved,
 * \c RD_KAFKA_RESP_ERR__UNKNOWN_BROKER if there was no broker with this id,
 * \c RD_KAFKA_RESP_ERR__INVALID_ARG if some of the parameters are not valid.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_mock_broker_error_stack_cnt(rd_kafka_mock_cluster_t *mcluster,
                                     int32_t broker_id,
                                     int16_t ApiKey,
                                     size_t *cntp);


/**
 * @brief Set the topic error to return in protocol requests.
 *
 * Currently only used for TopicMetadataRequest and AddPartitionsToTxnRequest.
 */
RD_EXPORT
void rd_kafka_mock_topic_set_error(rd_kafka_mock_cluster_t *mcluster,
                                   const char *topic,
                                   rd_kafka_resp_err_t err);


/**
 * @brief Creates a topic.
 *
 * This is an alternative to automatic topic creation as performed by
 * the client itself.
 *
 * @remark The Topic Admin API (CreateTopics) is not supported by the
 *         mock broker.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_mock_topic_create(rd_kafka_mock_cluster_t *mcluster,
                           const char *topic,
                           int partition_cnt,
                           int replication_factor);


/**
 * @brief Sets the partition leader.
 *
 * The topic will be created if it does not exist.
 *
 * \p broker_id needs to be an existing broker, or -1 to make the
 * partition leader-less.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_mock_partition_set_leader(rd_kafka_mock_cluster_t *mcluster,
                                   const char *topic,
                                   int32_t partition,
                                   int32_t broker_id);

/**
 * @brief Sets the partition's preferred replica / follower.
 *
 * The topic will be created if it does not exist.
 *
 * \p broker_id does not need to point to an existing broker.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_mock_partition_set_follower(rd_kafka_mock_cluster_t *mcluster,
                                     const char *topic,
                                     int32_t partition,
                                     int32_t broker_id);

/**
 * @brief Sets the partition's preferred replica / follower low and high
 *        watermarks.
 *
 * The topic will be created if it does not exist.
 *
 * Setting an offset to -1 will revert back to the leader's corresponding
 * watermark.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_mock_partition_set_follower_wmarks(rd_kafka_mock_cluster_t *mcluster,
                                            const char *topic,
                                            int32_t partition,
                                            int64_t lo,
                                            int64_t hi);


/**
 * @brief Disconnects the broker and disallows any new connections.
 *        This does NOT trigger leader change.
 *
 * @param mcluster Mock cluster instance.
 * @param broker_id Use -1 for all brokers, or >= 0 for a specific broker.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_mock_broker_set_down(rd_kafka_mock_cluster_t *mcluster,
                              int32_t broker_id);

/**
 * @brief Makes the broker accept connections again.
 *        This does NOT trigger leader change.
 *
 * @param mcluster Mock cluster instance.
 * @param broker_id Use -1 for all brokers, or >= 0 for a specific broker.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_mock_broker_set_up(rd_kafka_mock_cluster_t *mcluster,
                            int32_t broker_id);


/**
 * @brief Set broker round-trip-time delay in milliseconds.
 *
 * @param mcluster Mock cluster instance.
 * @param broker_id Use -1 for all brokers, or >= 0 for a specific broker.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_mock_broker_set_rtt(rd_kafka_mock_cluster_t *mcluster,
                             int32_t broker_id,
                             int rtt_ms);

/**
 * @brief Sets the broker's rack as reported in Metadata to the client.
 *
 * @param mcluster Mock cluster instance.
 * @param broker_id Use -1 for all brokers, or >= 0 for a specific broker.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_mock_broker_set_rack(rd_kafka_mock_cluster_t *mcluster,
                              int32_t broker_id,
                              const char *rack);



/**
 * @brief Explicitly sets the coordinator. If this API is not a standard
 *        hashing scheme will be used.
 *
 * @param key_type  "transaction" or "group"
 * @param key       The transactional.id or group.id
 * @param broker_id The new coordinator, does not have to be a valid broker.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_mock_coordinator_set(rd_kafka_mock_cluster_t *mcluster,
                              const char *key_type,
                              const char *key,
                              int32_t broker_id);



/**
 * @brief Set the allowed ApiVersion range for \p ApiKey.
 *
 *        Set \p MinVersion and \p MaxVersion to -1 to disable the API
 *        completely.
 *
 *        \p MaxVersion MUST not exceed the maximum implemented value,
 *        see rdkafka_mock_handlers.c.
 *
 * @param ApiKey Protocol request type/key
 * @param MinVersion Minimum version supported (or -1 to disable).
 * @param MinVersion Maximum version supported (or -1 to disable).
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_mock_set_apiversion(rd_kafka_mock_cluster_t *mcluster,
                             int16_t ApiKey,
                             int16_t MinVersion,
                             int16_t MaxVersion);


/**@}*/

#ifdef __cplusplus
}
#endif
#endif /* _RDKAFKA_MOCK_H_ */
