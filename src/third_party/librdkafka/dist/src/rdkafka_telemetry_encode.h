/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2023, Confluent Inc.
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

#ifndef _RDKAFKA_RDKAFKA_TELEMETRY_ENCODE_H
#define _RDKAFKA_RDKAFKA_TELEMETRY_ENCODE_H

#include "rdkafka_int.h"
#include "rdtypes.h"

#define RD_KAFKA_TELEMETRY_METRIC_PREFIX            "org.apache.kafka."
#define RD_KAFKA_TELEMETRY_METRIC_NODE_ID_ATTRIBUTE "node.id"

#define RD_KAFKA_TELEMETRY_METRIC_INFO(rk)                                     \
        (rk->rk_type == RD_KAFKA_PRODUCER                                      \
             ? RD_KAFKA_TELEMETRY_PRODUCER_METRICS_INFO                        \
             : RD_KAFKA_TELEMETRY_CONSUMER_METRICS_INFO)

#define RD_KAFKA_TELEMETRY_METRIC_CNT(rk)                                      \
        (rk->rk_type == RD_KAFKA_PRODUCER                                      \
             ? RD_KAFKA_TELEMETRY_PRODUCER_METRIC__CNT                         \
             : RD_KAFKA_TELEMETRY_CONSUMER_METRIC__CNT)


typedef enum {
        RD_KAFKA_TELEMETRY_METRIC_TYPE_SUM,
        RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE,
} rd_kafka_telemetry_metric_type_t;

typedef enum {
        RD_KAFKA_TELEMETRY_METRIC_PRODUCER_CONNECTION_CREATION_RATE,
        RD_KAFKA_TELEMETRY_METRIC_PRODUCER_CONNECTION_CREATION_TOTAL,
        RD_KAFKA_TELEMETRY_METRIC_PRODUCER_NODE_REQUEST_LATENCY_AVG,
        RD_KAFKA_TELEMETRY_METRIC_PRODUCER_NODE_REQUEST_LATENCY_MAX,
        RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_THROTTLE_TIME_AVG,
        RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_THROTTLE_TIME_MAX,
        RD_KAFKA_TELEMETRY_METRIC_PRODUCER_RECORD_QUEUE_TIME_AVG,
        RD_KAFKA_TELEMETRY_METRIC_PRODUCER_RECORD_QUEUE_TIME_MAX,
        RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_LATENCY_AVG,
        RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_LATENCY_MAX,
        RD_KAFKA_TELEMETRY_PRODUCER_METRIC__CNT
} rd_kafka_telemetry_producer_metric_name_t;

typedef enum {
        RD_KAFKA_TELEMETRY_METRIC_CONSUMER_CONNECTION_CREATION_RATE,
        RD_KAFKA_TELEMETRY_METRIC_CONSUMER_CONNECTION_CREATION_TOTAL,
        RD_KAFKA_TELEMETRY_METRIC_CONSUMER_NODE_REQUEST_LATENCY_AVG,
        RD_KAFKA_TELEMETRY_METRIC_CONSUMER_NODE_REQUEST_LATENCY_MAX,
        RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_ASSIGNED_PARTITIONS,
        RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_REBALANCE_LATENCY_AVG,
        RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_REBALANCE_LATENCY_MAX,
        RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_REBALANCE_LATENCY_TOTAL,
        RD_KAFKA_TELEMETRY_METRIC_CONSUMER_FETCH_MANAGER_FETCH_LATENCY_AVG,
        RD_KAFKA_TELEMETRY_METRIC_CONSUMER_FETCH_MANAGER_FETCH_LATENCY_MAX,
        RD_KAFKA_TELEMETRY_METRIC_CONSUMER_POLL_IDLE_RATIO_AVG,
        RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_COMMIT_LATENCY_AVG,
        RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_COMMIT_LATENCY_MAX,
        RD_KAFKA_TELEMETRY_CONSUMER_METRIC__CNT
} rd_kafka_telemetry_consumer_metric_name_t;

typedef union {
        int64_t int_value;
        double double_value;
} rd_kafka_telemetry_metric_value_t;

typedef rd_kafka_telemetry_metric_value_t (
    *rd_kafka_telemetry_metric_value_calculator_t)(
    rd_kafka_t *rk,
    rd_kafka_broker_t *rkb_selected,
    rd_ts_t now_nanos);

typedef struct {
        const char *name;
        const char *value;
} rd_kafka_telemetry_resource_attribute_t;

typedef struct {
        const char *name;
        const char *description;
        const char *unit;
        const rd_bool_t is_int;
        const rd_bool_t is_per_broker;
        rd_kafka_telemetry_metric_type_t type;
        rd_kafka_telemetry_metric_value_calculator_t calculate_value;
} rd_kafka_telemetry_metric_info_t;

typedef struct {
        const char *name;
        const char *(*getValue)(const rd_kafka_t *rk);
} rd_kafka_telemetry_attribute_config_t;

static const rd_kafka_telemetry_metric_info_t
    RD_KAFKA_TELEMETRY_PRODUCER_METRICS_INFO
        [RD_KAFKA_TELEMETRY_PRODUCER_METRIC__CNT] = {
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_CONNECTION_CREATION_RATE] =
                {.name = "producer.connection.creation.rate",
                 .description =
                     "The rate of connections established per second.",
                 .unit          = "1",
                 .is_int        = rd_false,
                 .is_per_broker = rd_false,
                 .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_CONNECTION_CREATION_TOTAL] =
                {.name        = "producer.connection.creation.total",
                 .description = "The total number of connections established.",
                 .unit        = "1",
                 .is_int      = rd_true,
                 .is_per_broker = rd_false,
                 .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_SUM},
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_NODE_REQUEST_LATENCY_AVG] =
                {.name        = "producer.node.request.latency.avg",
                 .description = "The average request latency in ms for a node.",
                 .unit        = "ms",
                 .is_int      = rd_false,
                 .is_per_broker = rd_true,
                 .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_NODE_REQUEST_LATENCY_MAX] =
                {.name        = "producer.node.request.latency.max",
                 .description = "The maximum request latency in ms for a node.",
                 .unit        = "ms",
                 .is_int      = rd_true,
                 .is_per_broker = rd_true,
                 .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_THROTTLE_TIME_AVG] =
                {.name          = "producer.produce.throttle.time.avg",
                 .description   = "The average throttle time in ms for a node.",
                 .unit          = "ms",
                 .is_int        = rd_false,
                 .is_per_broker = rd_false,
                 .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_THROTTLE_TIME_MAX] =
                {.name          = "producer.produce.throttle.time.max",
                 .description   = "The maximum throttle time in ms for a node.",
                 .unit          = "ms",
                 .is_int        = rd_true,
                 .is_per_broker = rd_false,
                 .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_RECORD_QUEUE_TIME_AVG] =
                {.name        = "producer.record.queue.time.avg",
                 .description = "The average time in ms a record spends in the "
                                "producer queue.",
                 .unit          = "ms",
                 .is_int        = rd_false,
                 .is_per_broker = rd_false,
                 .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_RECORD_QUEUE_TIME_MAX] =
                {.name        = "producer.record.queue.time.max",
                 .description = "The maximum time in ms a record spends in the "
                                "producer queue.",
                 .unit          = "ms",
                 .is_int        = rd_true,
                 .is_per_broker = rd_false,
                 .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_LATENCY_AVG] =
                {.name = "producer.request.latency.avg",
                 .description =
                     "The average request latency in ms for produce requests.",
                 .unit          = "ms",
                 .is_int        = rd_false,
                 .is_per_broker = rd_false,
                 .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_LATENCY_MAX] =
                {.name = "producer.request.latency.max",
                 .description =
                     "The maximum request latency in ms for produce requests.",
                 .unit          = "ms",
                 .is_int        = rd_true,
                 .is_per_broker = rd_false,
                 .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
};

static const rd_kafka_telemetry_metric_info_t RD_KAFKA_TELEMETRY_CONSUMER_METRICS_INFO
    [RD_KAFKA_TELEMETRY_CONSUMER_METRIC__CNT] = {
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_CONNECTION_CREATION_RATE] =
            {.name          = "consumer.connection.creation.rate",
             .description   = "The rate of connections established per second.",
             .unit          = "1",
             .is_int        = rd_false,
             .is_per_broker = rd_false,
             .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_CONNECTION_CREATION_TOTAL] =
            {.name          = "consumer.connection.creation.total",
             .description   = "The total number of connections established.",
             .unit          = "1",
             .is_int        = rd_true,
             .is_per_broker = rd_false,
             .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_SUM},
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_NODE_REQUEST_LATENCY_AVG] =
            {.name          = "consumer.node.request.latency.avg",
             .description   = "The average request latency in ms for a node.",
             .unit          = "ms",
             .is_int        = rd_false,
             .is_per_broker = rd_true,
             .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_NODE_REQUEST_LATENCY_MAX] =
            {.name          = "consumer.node.request.latency.max",
             .description   = "The maximum request latency in ms for a node.",
             .unit          = "ms",
             .is_int        = rd_true,
             .is_per_broker = rd_true,
             .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_ASSIGNED_PARTITIONS] =
            {.name        = "consumer.coordinator.assigned.partitions",
             .description = "The number of partitions currently assigned "
                            "to this consumer.",
             .unit          = "1",
             .is_int        = rd_true,
             .is_per_broker = rd_false,
             .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_REBALANCE_LATENCY_AVG] =
            {.name        = "consumer.coordinator.rebalance.latency.avg",
             .description = "The average rebalance latency in ms for the "
                            "consumer coordinator.",
             .unit          = "ms",
             .is_int        = rd_false,
             .is_per_broker = rd_false,
             .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_REBALANCE_LATENCY_MAX] =
            {.name        = "consumer.coordinator.rebalance.latency.max",
             .description = "The maximum rebalance latency in ms for the "
                            "consumer coordinator.",
             .unit          = "ms",
             .is_int        = rd_true,
             .is_per_broker = rd_false,
             .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_REBALANCE_LATENCY_TOTAL] =
            {.name        = "consumer.coordinator.rebalance.latency.total",
             .description = "The total rebalance latency in ms for the "
                            "consumer coordinator.",
             .unit          = "ms",
             .is_int        = rd_true,
             .is_per_broker = rd_false,
             .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_SUM},
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_FETCH_MANAGER_FETCH_LATENCY_AVG] =
            {.name = "consumer.fetch.manager.fetch.latency.avg",
             .description =
                 "The average fetch latency in ms for the fetch manager.",
             .unit          = "ms",
             .is_int        = rd_false,
             .is_per_broker = rd_false,
             .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_FETCH_MANAGER_FETCH_LATENCY_MAX] =
            {.name = "consumer.fetch.manager.fetch.latency.max",
             .description =
                 "The maximum fetch latency in ms for the fetch manager.",
             .unit          = "ms",
             .is_int        = rd_true,
             .is_per_broker = rd_false,
             .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_POLL_IDLE_RATIO_AVG] =
            {.name        = "consumer.poll.idle.ratio.avg",
             .description = "The average ratio of idle to poll for a consumer.",
             .unit        = "1",
             .is_int      = rd_false,
             .is_per_broker = rd_false,
             .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_COMMIT_LATENCY_AVG] =
            {.name        = "consumer.coordinator.commit.latency.avg",
             .description = "The average commit latency in ms for the consumer "
                            "coordinator.",
             .unit          = "ms",
             .is_int        = rd_false,
             .is_per_broker = rd_false,
             .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_COMMIT_LATENCY_MAX] =
            {.name        = "consumer.coordinator.commit.latency.max",
             .description = "The maximum commit latency in ms for the consumer "
                            "coordinator.",
             .unit          = "ms",
             .is_int        = rd_true,
             .is_per_broker = rd_false,
             .type          = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE},
};

rd_buf_t *rd_kafka_telemetry_encode_metrics(rd_kafka_t *rk);

#endif /* _RDKAFKA_RDKAFKA_TELEMETRY_ENCODE_H */
