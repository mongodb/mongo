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

#include "rdkafka_telemetry_encode.h"
#include "nanopb/pb_encode.h"
#include "opentelemetry/metrics.pb.h"

#define THREE_ORDERS_MAGNITUDE 1000

typedef struct {
        opentelemetry_proto_metrics_v1_Metric **metrics;
        size_t count;
} rd_kafka_telemetry_metrics_repeated_t;

typedef struct {
        opentelemetry_proto_common_v1_KeyValue **key_values;
        size_t count;
} rd_kafka_telemetry_key_values_repeated_t;

#define calculate_avg(_avg_, _scale_factor_)                                   \
        ((_avg_).ra_v.avg / (double)_scale_factor_)

#define calculate_max(_avg_, _scale_factor_)                                   \
        RD_CEIL_INTEGER_DIVISION((_avg_).ra_v.maxv, _scale_factor_)

#define brokers_avg(_rk_, _avg_name_, _scale_factor_, _metric_)                \
        do {                                                                   \
                rd_kafka_broker_t *_rkb_;                                      \
                double avg = 0;                                                \
                int count  = 0;                                                \
                TAILQ_FOREACH(_rkb_, &(_rk_)->rk_brokers, rkb_link) {          \
                        rd_avg_t *rd_avg_rollover =                            \
                            &_rkb_->rkb_telemetry.rd_avg_rollover._avg_name_;  \
                        if (rd_avg_rollover->ra_v.cnt) {                       \
                                avg = (avg * count +                           \
                                       rd_avg_rollover->ra_v.sum) /            \
                                      (double)(count +                         \
                                               rd_avg_rollover->ra_v.cnt);     \
                                count += rd_avg_rollover->ra_v.cnt;            \
                        }                                                      \
                }                                                              \
                if (_scale_factor_ > 1)                                        \
                        (_metric_).double_value = avg / _scale_factor_;        \
                else                                                           \
                        (_metric_).double_value = avg;                         \
        } while (0)

#define brokers_max(_rk_, _avg_name_, _scale_factor_, _metric_)                \
        do {                                                                   \
                rd_kafka_broker_t *_rkb_;                                      \
                _metric_.int_value = 0;                                        \
                TAILQ_FOREACH(_rkb_, &(_rk_)->rk_brokers, rkb_link) {          \
                        _metric_.int_value =                                   \
                            RD_MAX(_metric_.int_value,                         \
                                   _rkb_->rkb_telemetry.rd_avg_rollover        \
                                       ._avg_name_.ra_v.maxv);                 \
                }                                                              \
                if (_scale_factor_ > 1)                                        \
                        (_metric_).int_value = RD_CEIL_INTEGER_DIVISION(       \
                            (_metric_).int_value, _scale_factor_);             \
        } while (0)

static rd_kafka_telemetry_metric_value_t
calculate_connection_creation_total(rd_kafka_t *rk,
                                    rd_kafka_broker_t *rkb_selected,
                                    rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t total;
        rd_kafka_broker_t *rkb;

        total.int_value = 0;
        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                const int32_t connects = rd_atomic32_get(&rkb->rkb_c.connects);
                if (!rk->rk_telemetry.delta_temporality)
                        total.int_value += connects;
                else
                        total.int_value +=
                            connects -
                            rkb->rkb_telemetry.rkb_historic_c.connects;
        }

        return total;
}

static rd_kafka_telemetry_metric_value_t
calculate_connection_creation_rate(rd_kafka_t *rk,
                                   rd_kafka_broker_t *rkb_selected,
                                   rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t total;
        rd_kafka_broker_t *rkb;
        rd_ts_t ts_last = rk->rk_telemetry.rk_historic_c.ts_last;

        total.double_value = 0;
        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                total.double_value +=
                    rd_atomic32_get(&rkb->rkb_c.connects) -
                    rkb->rkb_telemetry.rkb_historic_c.connects;
        }
        double seconds = (now_ns - ts_last) / 1e9;
        if (seconds > 1.0)
                total.double_value /= seconds;
        return total;
}

static rd_kafka_telemetry_metric_value_t
calculate_broker_avg_rtt(rd_kafka_t *rk,
                         rd_kafka_broker_t *rkb_selected,
                         rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t avg_rtt = RD_ZERO_INIT;
        avg_rtt.double_value                      = calculate_avg(
            rkb_selected->rkb_telemetry.rd_avg_rollover.rkb_avg_rtt,
            THREE_ORDERS_MAGNITUDE);
        return avg_rtt;
}

static rd_kafka_telemetry_metric_value_t
calculate_broker_max_rtt(rd_kafka_t *rk,
                         rd_kafka_broker_t *rkb_selected,
                         rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t max_rtt = RD_ZERO_INIT;
        max_rtt.int_value                         = calculate_max(
            rkb_selected->rkb_telemetry.rd_avg_rollover.rkb_avg_rtt,
            THREE_ORDERS_MAGNITUDE);
        return max_rtt;
}

static rd_kafka_telemetry_metric_value_t
calculate_produce_latency_avg(rd_kafka_t *rk,
                              rd_kafka_broker_t *rkb_selected,
                              rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t avg_rtt = RD_ZERO_INIT;
        brokers_avg(rk, rkb_avg_produce_latency, THREE_ORDERS_MAGNITUDE,
                    avg_rtt);
        return avg_rtt;
}

static rd_kafka_telemetry_metric_value_t
calculate_produce_latency_max(rd_kafka_t *rk,
                              rd_kafka_broker_t *rkb_selected,
                              rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t max_rtt = RD_ZERO_INIT;
        brokers_max(rk, rkb_avg_produce_latency, THREE_ORDERS_MAGNITUDE,
                    max_rtt);
        return max_rtt;
}

static rd_kafka_telemetry_metric_value_t
calculate_throttle_avg(rd_kafka_t *rk,
                       rd_kafka_broker_t *rkb_selected,
                       rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t avg_throttle;
        brokers_avg(rk, rkb_avg_throttle, 1, avg_throttle);
        return avg_throttle;
}


static rd_kafka_telemetry_metric_value_t
calculate_throttle_max(rd_kafka_t *rk,
                       rd_kafka_broker_t *rkb_selected,
                       rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t max_throttle;
        brokers_max(rk, rkb_avg_throttle, 1, max_throttle);
        return max_throttle;
}

static rd_kafka_telemetry_metric_value_t
calculate_queue_time_avg(rd_kafka_t *rk,
                         rd_kafka_broker_t *rkb_selected,
                         rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t avg_queue_time;
        brokers_avg(rk, rkb_avg_outbuf_latency, THREE_ORDERS_MAGNITUDE,
                    avg_queue_time);
        return avg_queue_time;
}

static rd_kafka_telemetry_metric_value_t
calculate_queue_time_max(rd_kafka_t *rk,
                         rd_kafka_broker_t *rkb_selected,
                         rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t max_queue_time;
        brokers_max(rk, rkb_avg_outbuf_latency, THREE_ORDERS_MAGNITUDE,
                    max_queue_time);
        return max_queue_time;
}

static rd_kafka_telemetry_metric_value_t
calculate_consumer_assigned_partitions(rd_kafka_t *rk,
                                       rd_kafka_broker_t *rkb_selected,
                                       rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t assigned_partitions;

        assigned_partitions.int_value =
            rk->rk_cgrp ? rk->rk_cgrp->rkcg_c.assignment_size : 0;
        return assigned_partitions;
}

static rd_kafka_telemetry_metric_value_t
calculate_consumer_rebalance_latency_avg(rd_kafka_t *rk,
                                         rd_kafka_broker_t *rkb_selected,
                                         rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t avg_rebalance_time;
        avg_rebalance_time.double_value = calculate_avg(
            rk->rk_telemetry.rd_avg_rollover.rk_avg_rebalance_latency,
            THREE_ORDERS_MAGNITUDE);
        return avg_rebalance_time;
}

static rd_kafka_telemetry_metric_value_t
calculate_consumer_rebalance_latency_max(rd_kafka_t *rk,
                                         rd_kafka_broker_t *rkb_selected,
                                         rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t max_rebalance_time;
        max_rebalance_time.int_value = calculate_max(
            rk->rk_telemetry.rd_avg_rollover.rk_avg_rebalance_latency,
            THREE_ORDERS_MAGNITUDE);
        return max_rebalance_time;
}

static rd_kafka_telemetry_metric_value_t
calculate_consumer_rebalance_latency_total(rd_kafka_t *rk,
                                           rd_kafka_broker_t *rkb_selected,
                                           rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t total_rebalance_time;
        total_rebalance_time.int_value = RD_CEIL_INTEGER_DIVISION(
            rk->rk_telemetry.rd_avg_rollover.rk_avg_rebalance_latency.ra_v.sum,
            THREE_ORDERS_MAGNITUDE);
        if (!rk->rk_telemetry.delta_temporality) {
                total_rebalance_time.int_value +=
                    rk->rk_telemetry.rk_historic_c.rebalance_latency_total;
        }
        return total_rebalance_time;
}

static rd_kafka_telemetry_metric_value_t
calculate_consumer_fetch_latency_avg(rd_kafka_t *rk,
                                     rd_kafka_broker_t *rkb_selected,
                                     rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t avg_fetch_time;
        brokers_avg(rk, rkb_avg_fetch_latency, THREE_ORDERS_MAGNITUDE,
                    avg_fetch_time);
        return avg_fetch_time;
}

static rd_kafka_telemetry_metric_value_t
calculate_consumer_fetch_latency_max(rd_kafka_t *rk,
                                     rd_kafka_broker_t *rkb_selected,
                                     rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t max_fetch_time;
        brokers_max(rk, rkb_avg_fetch_latency, THREE_ORDERS_MAGNITUDE,
                    max_fetch_time);
        return max_fetch_time;
}

static rd_kafka_telemetry_metric_value_t
calculate_consumer_poll_idle_ratio_avg(rd_kafka_t *rk,
                                       rd_kafka_broker_t *rkb_selected,
                                       rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t avg_poll_idle_avg;
        avg_poll_idle_avg.double_value = calculate_avg(
            rk->rk_telemetry.rd_avg_rollover.rk_avg_poll_idle_ratio, 1e6);
        return avg_poll_idle_avg;
}

static rd_kafka_telemetry_metric_value_t
calculate_consumer_commit_latency_avg(rd_kafka_t *rk,
                                      rd_kafka_broker_t *rkb_selected,
                                      rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t avg_commit_time;
        avg_commit_time.double_value = calculate_avg(
            rk->rk_telemetry.rd_avg_rollover.rk_avg_commit_latency,
            THREE_ORDERS_MAGNITUDE);
        return avg_commit_time;
}

static rd_kafka_telemetry_metric_value_t
calculate_consumer_commit_latency_max(rd_kafka_t *rk,
                                      rd_kafka_broker_t *rkb_selected,
                                      rd_ts_t now_ns) {
        rd_kafka_telemetry_metric_value_t max_commit_time;
        max_commit_time.int_value = calculate_max(
            rk->rk_telemetry.rd_avg_rollover.rk_avg_commit_latency,
            THREE_ORDERS_MAGNITUDE);
        return max_commit_time;
}

static void reset_historical_metrics(rd_kafka_t *rk, rd_ts_t now_ns) {
        rd_kafka_broker_t *rkb;

        rk->rk_telemetry.rk_historic_c.ts_last = now_ns;
        rk->rk_telemetry.rk_historic_c.rebalance_latency_total +=
            RD_CEIL_INTEGER_DIVISION(rk->rk_telemetry.rd_avg_rollover
                                         .rk_avg_rebalance_latency.ra_v.sum,
                                     THREE_ORDERS_MAGNITUDE);

        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                rkb->rkb_telemetry.rkb_historic_c.connects =
                    rd_atomic32_get(&rkb->rkb_c.connects);
        }
}

static const rd_kafka_telemetry_metric_value_calculator_t
    PRODUCER_METRIC_VALUE_CALCULATORS[RD_KAFKA_TELEMETRY_PRODUCER_METRIC__CNT] =
        {
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_CONNECTION_CREATION_RATE] =
                &calculate_connection_creation_rate,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_CONNECTION_CREATION_TOTAL] =
                &calculate_connection_creation_total,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_NODE_REQUEST_LATENCY_AVG] =
                &calculate_broker_avg_rtt,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_NODE_REQUEST_LATENCY_MAX] =
                &calculate_broker_max_rtt,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_THROTTLE_TIME_AVG] =
                &calculate_throttle_avg,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_THROTTLE_TIME_MAX] =
                &calculate_throttle_max,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_RECORD_QUEUE_TIME_AVG] =
                &calculate_queue_time_avg,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_RECORD_QUEUE_TIME_MAX] =
                &calculate_queue_time_max,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_LATENCY_AVG] =
                &calculate_produce_latency_avg,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_LATENCY_MAX] =
                &calculate_produce_latency_max,
};

static const rd_kafka_telemetry_metric_value_calculator_t
    CONSUMER_METRIC_VALUE_CALCULATORS[RD_KAFKA_TELEMETRY_CONSUMER_METRIC__CNT] = {
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_CONNECTION_CREATION_RATE] =
            &calculate_connection_creation_rate,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_CONNECTION_CREATION_TOTAL] =
            &calculate_connection_creation_total,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_NODE_REQUEST_LATENCY_AVG] =
            &calculate_broker_avg_rtt,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_NODE_REQUEST_LATENCY_MAX] =
            &calculate_broker_max_rtt,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_ASSIGNED_PARTITIONS] =
            &calculate_consumer_assigned_partitions,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_REBALANCE_LATENCY_AVG] =
            &calculate_consumer_rebalance_latency_avg,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_REBALANCE_LATENCY_MAX] =
            &calculate_consumer_rebalance_latency_max,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_REBALANCE_LATENCY_TOTAL] =
            &calculate_consumer_rebalance_latency_total,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_FETCH_MANAGER_FETCH_LATENCY_AVG] =
            &calculate_consumer_fetch_latency_avg,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_FETCH_MANAGER_FETCH_LATENCY_MAX] =
            &calculate_consumer_fetch_latency_max,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_POLL_IDLE_RATIO_AVG] =
            &calculate_consumer_poll_idle_ratio_avg,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_COMMIT_LATENCY_AVG] =
            &calculate_consumer_commit_latency_avg,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_COMMIT_LATENCY_MAX] =
            &calculate_consumer_commit_latency_max,
};

static const char *get_client_rack(const rd_kafka_t *rk) {
        return rk->rk_conf.client_rack &&
                       RD_KAFKAP_STR_LEN(rk->rk_conf.client_rack)
                   ? (const char *)rk->rk_conf.client_rack->str
                   : NULL;
}

static const char *get_group_id(const rd_kafka_t *rk) {
        return rk->rk_conf.group_id_str ? (const char *)rk->rk_conf.group_id_str
                                        : NULL;
}

static const char *get_group_instance_id(const rd_kafka_t *rk) {
        return rk->rk_conf.group_instance_id
                   ? (const char *)rk->rk_conf.group_instance_id
                   : NULL;
}

static const char *get_member_id(const rd_kafka_t *rk) {
        return rk->rk_cgrp && rk->rk_cgrp->rkcg_member_id &&
                       rk->rk_cgrp->rkcg_member_id->len > 0
                   ? (const char *)rk->rk_cgrp->rkcg_member_id->str
                   : NULL;
}

static const char *get_transactional_id(const rd_kafka_t *rk) {
        return rk->rk_conf.eos.transactional_id
                   ? (const char *)rk->rk_conf.eos.transactional_id
                   : NULL;
}

static const rd_kafka_telemetry_attribute_config_t producer_attributes[] = {
    {"client_rack", get_client_rack},
    {"transactional_id", get_transactional_id},
};

static const rd_kafka_telemetry_attribute_config_t consumer_attributes[] = {
    {"client_rack", get_client_rack},
    {"group_id", get_group_id},
    {"group_instance_id", get_group_instance_id},
    {"member_id", get_member_id},
};

static int
count_attributes(rd_kafka_t *rk,
                 const rd_kafka_telemetry_attribute_config_t *configs,
                 int config_count) {
        int count = 0, i;
        for (i = 0; i < config_count; ++i) {
                if (configs[i].getValue(rk)) {
                        count++;
                }
        }
        return count;
}

static void set_attributes(rd_kafka_t *rk,
                           rd_kafka_telemetry_resource_attribute_t *attributes,
                           const rd_kafka_telemetry_attribute_config_t *configs,
                           int config_count) {
        int attr_idx = 0, i;
        for (i = 0; i < config_count; ++i) {
                const char *value = configs[i].getValue(rk);
                if (value) {
                        attributes[attr_idx].name  = configs[i].name;
                        attributes[attr_idx].value = value;
                        attr_idx++;
                }
        }
}

static int
resource_attributes(rd_kafka_t *rk,
                    rd_kafka_telemetry_resource_attribute_t **attributes) {
        int count = 0;
        const rd_kafka_telemetry_attribute_config_t *configs;
        int config_count;

        if (rk->rk_type == RD_KAFKA_PRODUCER) {
                configs      = producer_attributes;
                config_count = RD_ARRAY_SIZE(producer_attributes);
        } else if (rk->rk_type == RD_KAFKA_CONSUMER) {
                configs      = consumer_attributes;
                config_count = RD_ARRAY_SIZE(consumer_attributes);
        } else {
                *attributes = NULL;
                return 0;
        }

        count = count_attributes(rk, configs, config_count);

        if (count == 0) {
                *attributes = NULL;
                return 0;
        }

        *attributes =
            rd_malloc(sizeof(rd_kafka_telemetry_resource_attribute_t) * count);

        set_attributes(rk, *attributes, configs, config_count);

        return count;
}

static bool
encode_string(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
        if (!pb_encode_tag_for_field(stream, field))
                return false;
        return pb_encode_string(stream, (uint8_t *)(*arg), strlen(*arg));
}

// TODO: Update to handle multiple data points.
static bool encode_number_data_point(pb_ostream_t *stream,
                                     const pb_field_t *field,
                                     void *const *arg) {
        opentelemetry_proto_metrics_v1_NumberDataPoint *data_point =
            (opentelemetry_proto_metrics_v1_NumberDataPoint *)*arg;
        if (!pb_encode_tag_for_field(stream, field))
                return false;

        return pb_encode_submessage(
            stream, opentelemetry_proto_metrics_v1_NumberDataPoint_fields,
            data_point);
}

static bool
encode_metric(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
        rd_kafka_telemetry_metrics_repeated_t *metricArr =
            (rd_kafka_telemetry_metrics_repeated_t *)*arg;
        size_t i;

        for (i = 0; i < metricArr->count; i++) {

                opentelemetry_proto_metrics_v1_Metric *metric =
                    metricArr->metrics[i];
                if (!pb_encode_tag_for_field(stream, field))
                        return false;

                if (!pb_encode_submessage(
                        stream, opentelemetry_proto_metrics_v1_Metric_fields,
                        metric))
                        return false;
        }
        return true;
}

static bool encode_scope_metrics(pb_ostream_t *stream,
                                 const pb_field_t *field,
                                 void *const *arg) {
        opentelemetry_proto_metrics_v1_ScopeMetrics *scope_metrics =
            (opentelemetry_proto_metrics_v1_ScopeMetrics *)*arg;
        if (!pb_encode_tag_for_field(stream, field))
                return false;

        return pb_encode_submessage(
            stream, opentelemetry_proto_metrics_v1_ScopeMetrics_fields,
            scope_metrics);
}

static bool encode_resource_metrics(pb_ostream_t *stream,
                                    const pb_field_t *field,
                                    void *const *arg) {
        opentelemetry_proto_metrics_v1_ResourceMetrics *resource_metrics =
            (opentelemetry_proto_metrics_v1_ResourceMetrics *)*arg;
        if (!pb_encode_tag_for_field(stream, field))
                return false;

        return pb_encode_submessage(
            stream, opentelemetry_proto_metrics_v1_ResourceMetrics_fields,
            resource_metrics);
}

static bool encode_key_value(pb_ostream_t *stream,
                             const pb_field_t *field,
                             void *const *arg) {
        if (!pb_encode_tag_for_field(stream, field))
                return false;
        opentelemetry_proto_common_v1_KeyValue *key_value =
            (opentelemetry_proto_common_v1_KeyValue *)*arg;
        return pb_encode_submessage(
            stream, opentelemetry_proto_common_v1_KeyValue_fields, key_value);
}

static bool encode_key_values(pb_ostream_t *stream,
                              const pb_field_t *field,
                              void *const *arg) {
        rd_kafka_telemetry_key_values_repeated_t *kv_arr =
            (rd_kafka_telemetry_key_values_repeated_t *)*arg;
        size_t i;

        for (i = 0; i < kv_arr->count; i++) {

                opentelemetry_proto_common_v1_KeyValue *kv =
                    kv_arr->key_values[i];
                if (!pb_encode_tag_for_field(stream, field))
                        return false;

                if (!pb_encode_submessage(
                        stream, opentelemetry_proto_common_v1_KeyValue_fields,
                        kv))
                        return false;
        }
        return true;
}

static void free_metrics(
    opentelemetry_proto_metrics_v1_Metric **metrics,
    char **metric_names,
    opentelemetry_proto_metrics_v1_NumberDataPoint **data_points,
    opentelemetry_proto_common_v1_KeyValue *datapoint_attributes_key_values,
    size_t count) {
        size_t i;
        for (i = 0; i < count; i++) {
                rd_free(data_points[i]);
                rd_free(metric_names[i]);
                rd_free(metrics[i]);
        }
        rd_free(data_points);
        rd_free(metric_names);
        rd_free(metrics);
        rd_free(datapoint_attributes_key_values);
}

static void free_resource_attributes(
    opentelemetry_proto_common_v1_KeyValue **resource_attributes_key_values,
    rd_kafka_telemetry_resource_attribute_t *resource_attributes_struct,
    size_t count) {
        size_t i;
        if (count == 0)
                return;
        for (i = 0; i < count; i++)
                rd_free(resource_attributes_key_values[i]);
        rd_free(resource_attributes_struct);
        rd_free(resource_attributes_key_values);
}

static void serialize_Metric(
    rd_kafka_t *rk,
    rd_kafka_broker_t *rkb,
    const rd_kafka_telemetry_metric_info_t *info,
    opentelemetry_proto_metrics_v1_Metric **metric,
    opentelemetry_proto_metrics_v1_NumberDataPoint **data_point,
    opentelemetry_proto_common_v1_KeyValue *data_point_attribute,
    rd_kafka_telemetry_metric_value_calculator_t metric_value_calculator,
    char **metric_name,
    bool is_per_broker,
    rd_ts_t now_ns) {
        rd_ts_t ts_last  = rk->rk_telemetry.rk_historic_c.ts_last,
                ts_start = rk->rk_telemetry.rk_historic_c.ts_start;
        size_t metric_name_len;
        if (info->is_int) {
                (*data_point)->which_value =
                    opentelemetry_proto_metrics_v1_NumberDataPoint_as_int_tag;
                (*data_point)->value.as_int =
                    metric_value_calculator(rk, rkb, now_ns).int_value;
        } else {
                (*data_point)->which_value =
                    opentelemetry_proto_metrics_v1_NumberDataPoint_as_double_tag;
                (*data_point)->value.as_double =
                    metric_value_calculator(rk, rkb, now_ns).double_value;
        }


        (*data_point)->time_unix_nano = now_ns;
        if (info->type == RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE ||
            (info->type == RD_KAFKA_TELEMETRY_METRIC_TYPE_SUM &&
             rk->rk_telemetry.delta_temporality))
                (*data_point)->start_time_unix_nano = ts_last;
        else
                (*data_point)->start_time_unix_nano = ts_start;

        if (is_per_broker) {
                data_point_attribute->key.funcs.encode = &encode_string;
                data_point_attribute->key.arg =
                    RD_KAFKA_TELEMETRY_METRIC_NODE_ID_ATTRIBUTE;
                data_point_attribute->has_value = true;
                data_point_attribute->value.which_value =
                    opentelemetry_proto_common_v1_AnyValue_int_value_tag;

                rd_kafka_broker_lock(rkb);
                data_point_attribute->value.value.int_value = rkb->rkb_nodeid;
                rd_kafka_broker_unlock(rkb);

                (*data_point)->attributes.funcs.encode = &encode_key_value;
                (*data_point)->attributes.arg          = data_point_attribute;
        }


        switch (info->type) {

        case RD_KAFKA_TELEMETRY_METRIC_TYPE_SUM: {
                (*metric)->which_data =
                    opentelemetry_proto_metrics_v1_Metric_sum_tag;
                (*metric)->data.sum.data_points.funcs.encode =
                    &encode_number_data_point;
                (*metric)->data.sum.data_points.arg = *data_point;
                (*metric)->data.sum.aggregation_temporality =
                    rk->rk_telemetry.delta_temporality
                        ? opentelemetry_proto_metrics_v1_AggregationTemporality_AGGREGATION_TEMPORALITY_DELTA
                        : opentelemetry_proto_metrics_v1_AggregationTemporality_AGGREGATION_TEMPORALITY_CUMULATIVE;
                (*metric)->data.sum.is_monotonic = true;
                break;
        }
        case RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE: {
                (*metric)->which_data =
                    opentelemetry_proto_metrics_v1_Metric_gauge_tag;
                (*metric)->data.gauge.data_points.funcs.encode =
                    &encode_number_data_point;
                (*metric)->data.gauge.data_points.arg = *data_point;
                break;
        }
        default:
                rd_assert(!"Unknown metric type");
                break;
        }

        (*metric)->description.funcs.encode = &encode_string;
        (*metric)->description.arg          = (void *)info->description;

        metric_name_len =
            strlen(RD_KAFKA_TELEMETRY_METRIC_PREFIX) + strlen(info->name) + 1;
        *metric_name = rd_calloc(1, metric_name_len);
        rd_snprintf(*metric_name, metric_name_len, "%s%s",
                    RD_KAFKA_TELEMETRY_METRIC_PREFIX, info->name);


        (*metric)->name.funcs.encode = &encode_string;
        (*metric)->name.arg          = *metric_name;

        /* Skipping unit as Java client does the same */
}

/**
 * @brief Encodes the metrics to opentelemetry_proto_metrics_v1_MetricsData and
 * returns the serialized data. Currently only supports encoding of connection
 * creation total by default
 */
rd_buf_t *rd_kafka_telemetry_encode_metrics(rd_kafka_t *rk) {
        rd_buf_t *rbuf = NULL;
        rd_kafka_broker_t *rkb;
        size_t message_size;
        void *buffer = NULL;
        pb_ostream_t stream;
        bool status;
        char **metric_names;
        const int *metrics_to_encode = rk->rk_telemetry.matched_metrics;
        const size_t metrics_to_encode_count =
            rk->rk_telemetry.matched_metrics_cnt;
        const rd_kafka_telemetry_metric_info_t *info =
            RD_KAFKA_TELEMETRY_METRIC_INFO(rk);
        size_t total_metrics_count = metrics_to_encode_count;
        size_t i, metric_idx = 0;

        if (!metrics_to_encode_count)
                return NULL;

        opentelemetry_proto_metrics_v1_MetricsData metrics_data =
            opentelemetry_proto_metrics_v1_MetricsData_init_zero;

        opentelemetry_proto_metrics_v1_ResourceMetrics resource_metrics =
            opentelemetry_proto_metrics_v1_ResourceMetrics_init_zero;

        opentelemetry_proto_metrics_v1_Metric **metrics;
        opentelemetry_proto_common_v1_KeyValue *
            *resource_attributes_key_values = NULL;
        opentelemetry_proto_common_v1_KeyValue
            *datapoint_attributes_key_values = NULL;
        opentelemetry_proto_metrics_v1_NumberDataPoint **data_points;
        rd_kafka_telemetry_metrics_repeated_t metrics_repeated;
        rd_kafka_telemetry_key_values_repeated_t resource_attributes_repeated;
        rd_kafka_telemetry_resource_attribute_t *resource_attributes_struct =
            NULL;
        rd_ts_t now_ns = rd_uclock() * 1000;
        rd_kafka_rdlock(rk);

        for (i = 0; i < metrics_to_encode_count; i++) {
                if (info[metrics_to_encode[i]].is_per_broker) {
                        total_metrics_count += rk->rk_broker_cnt.val - 1;
                }
        }

        rd_kafka_dbg(rk, TELEMETRY, "PUSH", "Serializing metrics");

        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                rd_avg_destroy(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_rtt);
                rd_avg_rollover(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_rtt,
                                &rkb->rkb_telemetry.rd_avg_current.rkb_avg_rtt);
                rd_avg_destroy(
                    &rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_outbuf_latency);
                rd_avg_rollover(
                    &rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_outbuf_latency,
                    &rkb->rkb_telemetry.rd_avg_current.rkb_avg_outbuf_latency);
                rd_avg_destroy(
                    &rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_throttle);
                rd_avg_rollover(
                    &rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_throttle,
                    &rkb->rkb_telemetry.rd_avg_current.rkb_avg_throttle);
                if (rk->rk_type == RD_KAFKA_CONSUMER) {
                        rd_avg_destroy(&rkb->rkb_telemetry.rd_avg_rollover
                                            .rkb_avg_fetch_latency);
                        rd_avg_rollover(&rkb->rkb_telemetry.rd_avg_rollover
                                             .rkb_avg_fetch_latency,
                                        &rkb->rkb_telemetry.rd_avg_current
                                             .rkb_avg_fetch_latency);
                } else if (rk->rk_type == RD_KAFKA_PRODUCER) {
                        rd_avg_destroy(&rkb->rkb_telemetry.rd_avg_rollover
                                            .rkb_avg_produce_latency);
                        rd_avg_rollover(&rkb->rkb_telemetry.rd_avg_rollover
                                             .rkb_avg_produce_latency,
                                        &rkb->rkb_telemetry.rd_avg_current
                                             .rkb_avg_produce_latency);
                }
        }

        if (rk->rk_type == RD_KAFKA_CONSUMER) {
                rd_avg_destroy(
                    &rk->rk_telemetry.rd_avg_rollover.rk_avg_poll_idle_ratio);
                rd_avg_rollover(
                    &rk->rk_telemetry.rd_avg_rollover.rk_avg_poll_idle_ratio,
                    &rk->rk_telemetry.rd_avg_current.rk_avg_poll_idle_ratio);

                rd_avg_destroy(
                    &rk->rk_telemetry.rd_avg_rollover.rk_avg_rebalance_latency);
                rd_avg_rollover(
                    &rk->rk_telemetry.rd_avg_rollover.rk_avg_rebalance_latency,
                    &rk->rk_telemetry.rd_avg_current.rk_avg_rebalance_latency);

                rd_avg_destroy(
                    &rk->rk_telemetry.rd_avg_rollover.rk_avg_commit_latency);
                rd_avg_rollover(
                    &rk->rk_telemetry.rd_avg_rollover.rk_avg_commit_latency,
                    &rk->rk_telemetry.rd_avg_current.rk_avg_commit_latency);
        }

        int resource_attributes_count =
            resource_attributes(rk, &resource_attributes_struct);
        rd_kafka_dbg(rk, TELEMETRY, "PUSH", "Resource attributes count: %d",
                     resource_attributes_count);
        if (resource_attributes_count > 0) {
                resource_attributes_key_values =
                    rd_malloc(sizeof(opentelemetry_proto_common_v1_KeyValue *) *
                              resource_attributes_count);
                int ind;
                for (ind = 0; ind < resource_attributes_count; ++ind) {
                        resource_attributes_key_values[ind] = rd_calloc(
                            1, sizeof(opentelemetry_proto_common_v1_KeyValue));
                        resource_attributes_key_values[ind]->key.funcs.encode =
                            &encode_string;
                        resource_attributes_key_values[ind]->key.arg =
                            (void *)resource_attributes_struct[ind].name;

                        resource_attributes_key_values[ind]->has_value = true;
                        resource_attributes_key_values[ind]->value.which_value =
                            opentelemetry_proto_common_v1_AnyValue_string_value_tag;
                        resource_attributes_key_values[ind]
                            ->value.value.string_value.funcs.encode =
                            &encode_string;
                        resource_attributes_key_values[ind]
                            ->value.value.string_value.arg =
                            (void *)resource_attributes_struct[ind].value;
                }
                resource_attributes_repeated.key_values =
                    resource_attributes_key_values;
                resource_attributes_repeated.count = resource_attributes_count;
                resource_metrics.has_resource      = true;
                resource_metrics.resource.attributes.funcs.encode =
                    &encode_key_values;
                resource_metrics.resource.attributes.arg =
                    &resource_attributes_repeated;
        }

        opentelemetry_proto_metrics_v1_ScopeMetrics scope_metrics =
            opentelemetry_proto_metrics_v1_ScopeMetrics_init_zero;

        opentelemetry_proto_common_v1_InstrumentationScope
            instrumentation_scope =
                opentelemetry_proto_common_v1_InstrumentationScope_init_zero;
        instrumentation_scope.name.funcs.encode    = &encode_string;
        instrumentation_scope.name.arg             = (void *)rd_kafka_name(rk);
        instrumentation_scope.version.funcs.encode = &encode_string;
        instrumentation_scope.version.arg = (void *)rd_kafka_version_str();

        scope_metrics.has_scope = true;
        scope_metrics.scope     = instrumentation_scope;

        metrics = rd_malloc(sizeof(opentelemetry_proto_metrics_v1_Metric *) *
                            total_metrics_count);
        data_points =
            rd_malloc(sizeof(opentelemetry_proto_metrics_v1_NumberDataPoint *) *
                      total_metrics_count);
        datapoint_attributes_key_values =
            rd_malloc(sizeof(opentelemetry_proto_common_v1_KeyValue) *
                      total_metrics_count);
        metric_names = rd_malloc(sizeof(char *) * total_metrics_count);
        rd_kafka_dbg(rk, TELEMETRY, "PUSH",
                     "Total metrics to be encoded count: %" PRIusz,
                     total_metrics_count);


        for (i = 0; i < metrics_to_encode_count; i++) {

                rd_kafka_telemetry_metric_value_calculator_t
                    metric_value_calculator =
                        (rk->rk_type == RD_KAFKA_PRODUCER)
                            ? PRODUCER_METRIC_VALUE_CALCULATORS
                                  [metrics_to_encode[i]]
                            : CONSUMER_METRIC_VALUE_CALCULATORS
                                  [metrics_to_encode[i]];
                if (info[metrics_to_encode[i]].is_per_broker) {
                        rd_kafka_broker_t *rkb;

                        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                                metrics[metric_idx] = rd_calloc(
                                    1,
                                    sizeof(
                                        opentelemetry_proto_metrics_v1_Metric));
                                data_points[metric_idx] = rd_calloc(
                                    1,
                                    sizeof(
                                        opentelemetry_proto_metrics_v1_NumberDataPoint));
                                serialize_Metric(
                                    rk, rkb, &info[metrics_to_encode[i]],
                                    &metrics[metric_idx],
                                    &data_points[metric_idx],
                                    &datapoint_attributes_key_values
                                        [metric_idx],
                                    metric_value_calculator,
                                    &metric_names[metric_idx], true, now_ns);
                                metric_idx++;
                        }
                        continue;
                }

                metrics[metric_idx] =
                    rd_calloc(1, sizeof(opentelemetry_proto_metrics_v1_Metric));
                data_points[metric_idx] = rd_calloc(
                    1, sizeof(opentelemetry_proto_metrics_v1_NumberDataPoint));

                serialize_Metric(rk, NULL, &info[metrics_to_encode[i]],
                                 &metrics[metric_idx], &data_points[metric_idx],
                                 &datapoint_attributes_key_values[metric_idx],
                                 metric_value_calculator,
                                 &metric_names[metric_idx], false, now_ns);
                metric_idx++;
        }

        /* Send empty metrics blob if no metrics are matched */
        if (total_metrics_count > 0) {
                metrics_repeated.metrics = metrics;
                metrics_repeated.count   = total_metrics_count;

                scope_metrics.metrics.funcs.encode = &encode_metric;
                scope_metrics.metrics.arg          = &metrics_repeated;


                resource_metrics.scope_metrics.funcs.encode =
                    &encode_scope_metrics;
                resource_metrics.scope_metrics.arg = &scope_metrics;

                metrics_data.resource_metrics.funcs.encode =
                    &encode_resource_metrics;
                metrics_data.resource_metrics.arg = &resource_metrics;
        }

        status = pb_get_encoded_size(
            &message_size, opentelemetry_proto_metrics_v1_MetricsData_fields,
            &metrics_data);
        if (!status) {
                rd_kafka_dbg(rk, TELEMETRY, "PUSH",
                             "Failed to get encoded size");
                goto fail;
        }

        rbuf = rd_buf_new(1, message_size);
        rd_buf_write_ensure(rbuf, message_size, message_size);
        message_size = rd_buf_get_writable(rbuf, &buffer);

        stream = pb_ostream_from_buffer(buffer, message_size);
        status = pb_encode(&stream,
                           opentelemetry_proto_metrics_v1_MetricsData_fields,
                           &metrics_data);

        if (!status) {
                rd_kafka_dbg(rk, TELEMETRY, "PUSH", "Encoding failed: %s",
                             PB_GET_ERROR(&stream));
                rd_buf_destroy_free(rbuf);
                goto fail;
        }
        rd_kafka_dbg(rk, TELEMETRY, "PUSH",
                     "Push Telemetry metrics encoded, size: %" PRIusz,
                     stream.bytes_written);
        rd_buf_write(rbuf, NULL, stream.bytes_written);

        reset_historical_metrics(rk, now_ns);

        free_metrics(metrics, metric_names, data_points,
                     datapoint_attributes_key_values, total_metrics_count);
        free_resource_attributes(resource_attributes_key_values,
                                 resource_attributes_struct,
                                 resource_attributes_count);
        rd_kafka_rdunlock(rk);

        return rbuf;

fail:
        free_metrics(metrics, metric_names, data_points,
                     datapoint_attributes_key_values, total_metrics_count);
        free_resource_attributes(resource_attributes_key_values,
                                 resource_attributes_struct,
                                 resource_attributes_count);
        rd_kafka_rdunlock(rk);

        return NULL;
}
