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

#include "rdkafka_telemetry_decode.h"
#include "nanopb/pb_decode.h"
#include "rdunittest.h"
#include "rdkafka_lz4.h"
#include "rdgz.h"
#include "rdkafka_zstd.h"
#include "snappy.h"
#include "rdfloat.h"


#define _NANOPB_STRING_DECODE_MAX_BUFFER_SIZE 1024
#define MAX_LABELS                            10
#define UNITTEST_MARKER                       "unittest"

enum unit_test_string_decoding_state {
        STATE_LABELS,
        STATE_VERSION,
        STATE_METRIC_NAME,
        STATE_METRIC_DESCRIPTION,
        STATE_COMPLETE
};

struct unit_test_metric_label {
        char key[_NANOPB_STRING_DECODE_MAX_BUFFER_SIZE];
        char value[_NANOPB_STRING_DECODE_MAX_BUFFER_SIZE];
};

struct unit_test_data {
        rd_kafka_telemetry_metric_type_t type;
        int32_t current_field;
        struct unit_test_metric_label labels[MAX_LABELS];
        int label_count;
        char version[_NANOPB_STRING_DECODE_MAX_BUFFER_SIZE];
        char metric_name[_NANOPB_STRING_DECODE_MAX_BUFFER_SIZE];
        char metric_description[_NANOPB_STRING_DECODE_MAX_BUFFER_SIZE];
        int64_t metric_value_int;
        int64_t expected_metric_value_int;
        double metric_value_double;
        double expected_metric_value_double;
        int64_t int64_value;
        uint64_t metric_time;
        enum unit_test_string_decoding_state state;
        bool expecting_label_value;
};

static struct unit_test_data unit_test_data;

static void clear_unit_test_data(int64_t expected_value_int,
                                 double expected_value_double) {
        memset(&unit_test_data, 0, sizeof(unit_test_data));

        unit_test_data.type  = RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE;
        unit_test_data.state = STATE_LABELS;
        unit_test_data.expecting_label_value        = false;
        unit_test_data.version[0]                   = '\0';
        unit_test_data.metric_name[0]               = '\0';
        unit_test_data.metric_description[0]        = '\0';
        unit_test_data.current_field                = 0;
        unit_test_data.label_count                  = 0;
        unit_test_data.metric_value_int             = 0;
        unit_test_data.metric_value_double          = 0.0;
        unit_test_data.metric_time                  = 0;
        unit_test_data.int64_value                  = 0;
        unit_test_data.expected_metric_value_int    = expected_value_int;
        unit_test_data.expected_metric_value_double = expected_value_double;
}

static bool
decode_string(pb_istream_t *stream, const pb_field_t *field, void **arg) {
        rd_kafka_telemetry_decode_interface_t *decode_interface = *arg;
        uint8_t buffer[_NANOPB_STRING_DECODE_MAX_BUFFER_SIZE]   = {0};

        if (stream->bytes_left > sizeof(buffer) - 1) {
                RD_INTERFACE_CALL(decode_interface, decode_error,
                                  "String too long for buffer");
                return false;
        }

        if (!pb_read(stream, buffer, stream->bytes_left)) {
                RD_INTERFACE_CALL(decode_interface, decode_error,
                                  "Failed to read string");
                return false;
        }

        RD_INTERFACE_CALL(decode_interface, decoded_string, buffer);
        return true;
}

static bool
decode_key_value(pb_istream_t *stream, const pb_field_t *field, void **arg) {
        rd_kafka_telemetry_decode_interface_t *decode_interface = *arg;
        opentelemetry_proto_common_v1_KeyValue key_value =
            opentelemetry_proto_common_v1_KeyValue_init_zero;
        key_value.key.funcs.decode                      = &decode_string;
        key_value.key.arg                               = decode_interface;
        key_value.value.value.string_value.funcs.decode = &decode_string;
        key_value.value.value.string_value.arg          = decode_interface;
        if (!pb_decode(stream, opentelemetry_proto_common_v1_KeyValue_fields,
                       &key_value)) {
                RD_INTERFACE_CALL(decode_interface, decode_error,
                                  "Failed to decode KeyValue: %s",
                                  PB_GET_ERROR(stream));
                return false;
        }

        if (key_value.value.which_value ==
            opentelemetry_proto_common_v1_AnyValue_int_value_tag) {
                RD_INTERFACE_CALL(decode_interface, decoded_int64,
                                  key_value.value.value.int_value);
        }

        return true;
}

static bool decode_number_data_point(pb_istream_t *stream,
                                     const pb_field_t *field,
                                     void **arg) {
        rd_kafka_telemetry_decode_interface_t *decode_interface = *arg;
        opentelemetry_proto_metrics_v1_NumberDataPoint data_point =
            opentelemetry_proto_metrics_v1_NumberDataPoint_init_zero;
        data_point.attributes.funcs.decode = &decode_key_value;
        data_point.attributes.arg          = decode_interface;
        if (!pb_decode(stream,
                       opentelemetry_proto_metrics_v1_NumberDataPoint_fields,
                       &data_point)) {
                RD_INTERFACE_CALL(decode_interface, decode_error,
                                  "Failed to decode NumberDataPoint: %s",
                                  PB_GET_ERROR(stream));
                return false;
        }

        RD_INTERFACE_CALL(decode_interface, decoded_NumberDataPoint,
                          &data_point);
        return true;
}

// TODO: add support for other data types
static bool
data_msg_callback(pb_istream_t *stream, const pb_field_t *field, void **arg) {
        rd_kafka_telemetry_decode_interface_t *decode_interface = *arg;
        if (field->tag == opentelemetry_proto_metrics_v1_Metric_sum_tag) {
                opentelemetry_proto_metrics_v1_Sum *sum = field->pData;
                sum->data_points.funcs.decode = &decode_number_data_point;
                sum->data_points.arg          = decode_interface;
                if (decode_interface->decoded_type) {
                        RD_INTERFACE_CALL(decode_interface, decoded_type,
                                          RD_KAFKA_TELEMETRY_METRIC_TYPE_SUM);
                }
        } else if (field->tag ==
                   opentelemetry_proto_metrics_v1_Metric_gauge_tag) {
                opentelemetry_proto_metrics_v1_Gauge *gauge = field->pData;
                gauge->data_points.funcs.decode = &decode_number_data_point;
                gauge->data_points.arg          = decode_interface;
                if (decode_interface->decoded_type) {
                        RD_INTERFACE_CALL(decode_interface, decoded_type,
                                          RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE);
                }
        }
        return true;
}


static bool
decode_metric(pb_istream_t *stream, const pb_field_t *field, void **arg) {
        rd_kafka_telemetry_decode_interface_t *decode_interface = *arg;
        opentelemetry_proto_metrics_v1_Metric metric =
            opentelemetry_proto_metrics_v1_Metric_init_zero;
        metric.name.funcs.decode        = &decode_string;
        metric.name.arg                 = decode_interface;
        metric.description.funcs.decode = &decode_string;
        metric.description.arg          = decode_interface;
        metric.cb_data.funcs.decode     = &data_msg_callback;
        metric.cb_data.arg              = decode_interface;

        if (!pb_decode(stream, opentelemetry_proto_metrics_v1_Metric_fields,
                       &metric)) {
                RD_INTERFACE_CALL(decode_interface, decode_error,
                                  "Failed to decode Metric: %s",
                                  PB_GET_ERROR(stream));
                return false;
        }

        return true;
}

static bool decode_scope_metrics(pb_istream_t *stream,
                                 const pb_field_t *field,
                                 void **arg) {
        rd_kafka_telemetry_decode_interface_t *decode_interface = *arg;
        opentelemetry_proto_metrics_v1_ScopeMetrics scope_metrics =
            opentelemetry_proto_metrics_v1_ScopeMetrics_init_zero;
        scope_metrics.scope.name.funcs.decode    = &decode_string;
        scope_metrics.scope.name.arg             = decode_interface;
        scope_metrics.scope.version.funcs.decode = &decode_string;
        scope_metrics.scope.version.arg          = decode_interface;
        scope_metrics.metrics.funcs.decode       = &decode_metric;
        scope_metrics.metrics.arg                = decode_interface;

        if (!pb_decode(stream,
                       opentelemetry_proto_metrics_v1_ScopeMetrics_fields,
                       &scope_metrics)) {
                RD_INTERFACE_CALL(decode_interface, decode_error,
                                  "Failed to decode ScopeMetrics: %s",
                                  PB_GET_ERROR(stream));
                return false;
        }
        return true;
}

static bool decode_resource_metrics(pb_istream_t *stream,
                                    const pb_field_t *field,
                                    void **arg) {
        rd_kafka_telemetry_decode_interface_t *decode_interface = *arg;
        opentelemetry_proto_metrics_v1_ResourceMetrics resource_metrics =
            opentelemetry_proto_metrics_v1_ResourceMetrics_init_zero;
        resource_metrics.resource.attributes.funcs.decode = &decode_key_value;
        resource_metrics.resource.attributes.arg          = decode_interface;
        resource_metrics.scope_metrics.funcs.decode = &decode_scope_metrics;
        resource_metrics.scope_metrics.arg          = decode_interface;
        if (!pb_decode(stream,
                       opentelemetry_proto_metrics_v1_ResourceMetrics_fields,
                       &resource_metrics)) {
                RD_INTERFACE_CALL(decode_interface, decode_error,
                                  "Failed to decode ResourceMetrics: %s",
                                  PB_GET_ERROR(stream));
                return false;
        }
        return true;
}

#if WITH_SNAPPY

static int rd_kafka_snappy_decompress(rd_kafka_broker_t *rkb,
                                      const char *compressed,
                                      size_t compressed_size,
                                      void **outbuf,
                                      size_t *outbuf_len) {
        struct iovec iov = {.iov_base = NULL, .iov_len = 0};

        const char *inbuf = compressed;
        size_t inlen      = compressed_size;
        int r;
        static const unsigned char snappy_java_magic[] = {0x82, 'S', 'N', 'A',
                                                          'P',  'P', 'Y', 0};
        static const size_t snappy_java_hdrlen         = 8 + 4 + 4;

        /* snappy-java adds its own header (SnappyCodec)
         * which is not compatible with the official Snappy
         * implementation.
         *   8: magic, 4: version, 4: compatible
         * followed by any number of chunks:
         *   4: length
         * ...: snappy-compressed data. */
        if (likely(inlen > snappy_java_hdrlen + 4 &&
                   !memcmp(inbuf, snappy_java_magic, 8))) {
                /* snappy-java framing */
                char errstr[128];

                inbuf = inbuf + snappy_java_hdrlen;
                inlen -= snappy_java_hdrlen;
                iov.iov_base = rd_kafka_snappy_java_uncompress(
                    inbuf, inlen, &iov.iov_len, errstr, sizeof(errstr));

                if (unlikely(!iov.iov_base)) {
                        rd_rkb_dbg(
                            rkb, MSG, "SNAPPY",
                            "Snappy decompression for message failed: %s: "
                            "ignoring message",
                            errstr);
                        return -1;  // Indicates decompression error
                }


        } else {
                /* No framing */

                /* Acquire uncompressed length */
                if (unlikely(!rd_kafka_snappy_uncompressed_length(
                        inbuf, inlen, &iov.iov_len))) {
                        rd_rkb_dbg(
                            rkb, MSG, "SNAPPY",
                            "Failed to get length of Snappy compressed payload "
                            "for message (%" PRIusz
                            " bytes): "
                            "ignoring message",
                            inlen);
                        return -1;  // Indicates decompression error
                }

                /* Allocate output buffer for uncompressed data */
                iov.iov_base = rd_malloc(iov.iov_len);
                if (unlikely(!iov.iov_base)) {
                        rd_rkb_dbg(rkb, MSG, "SNAPPY",
                                   "Failed to allocate Snappy decompress "
                                   "buffer of size %" PRIusz
                                   " for message (%" PRIusz
                                   " bytes): %s: "
                                   "ignoring message",
                                   *outbuf_len, inlen, rd_strerror(errno));
                        return -1;  // Indicates memory allocation error
                }

                /* Uncompress to outbuf */
                if (unlikely((r = rd_kafka_snappy_uncompress(inbuf, inlen,
                                                             iov.iov_base)))) {
                        rd_rkb_dbg(
                            rkb, MSG, "SNAPPY",
                            "Failed to decompress Snappy payload for message "
                            "(%" PRIusz
                            " bytes): %s: "
                            "ignoring message",
                            inlen, rd_strerror(errno));
                        rd_free(iov.iov_base);
                        return -1;  // Indicates decompression error
                }
        }
        *outbuf     = iov.iov_base;
        *outbuf_len = iov.iov_len;
        return 0;
}
#endif

/*
 * Decompress a payload using the specified compression type. Allocates memory
 * for uncompressed payload.
 * @returns 0 on success, -1 on failure. Allocated memory in
 * uncompressed_payload and its size in uncompressed_payload_size.
 */
int rd_kafka_telemetry_uncompress_metrics_payload(
    rd_kafka_broker_t *rkb,
    rd_kafka_compression_t compression_type,
    void *compressed_payload,
    size_t compressed_payload_size,
    void **uncompressed_payload,
    size_t *uncompressed_payload_size) {
        int r = -1;
        switch (compression_type) {
#if WITH_ZLIB
        case RD_KAFKA_COMPRESSION_GZIP:
                *uncompressed_payload = rd_gz_decompress(
                    compressed_payload, (int)compressed_payload_size,
                    (uint64_t *)uncompressed_payload_size);
                if (*uncompressed_payload == NULL)
                        r = -1;
                else
                        r = 0;
                break;
#endif
        case RD_KAFKA_COMPRESSION_KLZ4:
                r = rd_kafka_lz4_decompress(
                    rkb, 0, 0, compressed_payload, compressed_payload_size,
                    uncompressed_payload, uncompressed_payload_size);
                break;
#if WITH_ZSTD
        case RD_KAFKA_COMPRESSION_ZSTD:
                r = rd_kafka_zstd_decompress(
                    rkb, compressed_payload, compressed_payload_size,
                    uncompressed_payload, uncompressed_payload_size);
                break;
#endif
#if WITH_SNAPPY
        case RD_KAFKA_COMPRESSION_SNAPPY:
                r = rd_kafka_snappy_decompress(
                    rkb, compressed_payload, compressed_payload_size,
                    uncompressed_payload, uncompressed_payload_size);
                break;
#endif
        default:
                rd_kafka_log(rkb->rkb_rk, LOG_WARNING, "TELEMETRY",
                             "Unknown compression type: %d", compression_type);
                break;
        }
        return r;
}

/**
 * Decode a metric from a buffer encoded with
 * opentelemetry_proto_metrics_v1_MetricsData datatype. Used for testing and
 * debugging.
 *
 * @param decode_interface The decode_interface to pass as arg when decoding the
 * buffer.
 * @param buffer The buffer to decode.
 * @param size The size of the buffer.
 */
int rd_kafka_telemetry_decode_metrics(
    rd_kafka_telemetry_decode_interface_t *decode_interface,
    void *buffer,
    size_t size) {
        opentelemetry_proto_metrics_v1_MetricsData metricsData =
            opentelemetry_proto_metrics_v1_MetricsData_init_zero;

        pb_istream_t stream              = pb_istream_from_buffer(buffer, size);
        metricsData.resource_metrics.arg = decode_interface;
        metricsData.resource_metrics.funcs.decode = &decode_resource_metrics;

        bool status = pb_decode(
            &stream, opentelemetry_proto_metrics_v1_MetricsData_fields,
            &metricsData);
        if (!status) {
                RD_INTERFACE_CALL(decode_interface, decode_error,
                                  "Failed to decode MetricsData: %s",
                                  PB_GET_ERROR(&stream));
        }
        return status;
}

static void unit_test_telemetry_decoded_string(void *opaque,
                                               const uint8_t *decoded) {

        switch (unit_test_data.state) {
        case STATE_LABELS:
                if (strcmp((const char *)decoded, UNITTEST_MARKER) == 0) {
                        unit_test_data.state                 = STATE_VERSION;
                        unit_test_data.expecting_label_value = false;
                } else if (unit_test_data.expecting_label_value) {
                        rd_snprintf(unit_test_data
                                        .labels[unit_test_data.label_count - 1]
                                        .value,
                                    sizeof(unit_test_data.labels[0].value),
                                    "%s", decoded);
                        unit_test_data.expecting_label_value = false;
                } else {
                        if (unit_test_data.label_count < MAX_LABELS) {
                                rd_snprintf(
                                    unit_test_data
                                        .labels[unit_test_data.label_count]
                                        .key,
                                    sizeof(unit_test_data.labels[0].key), "%s",
                                    decoded);
                                unit_test_data.label_count++;
                                unit_test_data.expecting_label_value = true;
                        }
                }
                unit_test_data.current_field++;
                break;

        case STATE_VERSION:
                rd_snprintf(unit_test_data.version,
                            sizeof(unit_test_data.version), "%s", decoded);
                unit_test_data.state = STATE_METRIC_NAME;
                unit_test_data.current_field++;
                break;

        case STATE_METRIC_NAME:
                rd_snprintf(unit_test_data.metric_name,
                            sizeof(unit_test_data.metric_name), "%s", decoded);
                unit_test_data.state = STATE_METRIC_DESCRIPTION;
                unit_test_data.current_field++;
                break;

        case STATE_METRIC_DESCRIPTION:
                rd_snprintf(unit_test_data.metric_description,
                            sizeof(unit_test_data.metric_description), "%s",
                            decoded);
                unit_test_data.state = STATE_COMPLETE;
                unit_test_data.current_field++;
                break;

        case STATE_COMPLETE:
                break;
        }
}

static void unit_test_telemetry_decoded_NumberDataPoint(
    void *opaque,
    const opentelemetry_proto_metrics_v1_NumberDataPoint *decoded) {
        unit_test_data.metric_value_int    = decoded->value.as_int;
        unit_test_data.metric_value_double = decoded->value.as_double;
        unit_test_data.metric_time         = decoded->time_unix_nano;
        unit_test_data.current_field++;
}

static void unit_test_telemetry_decoded_int64(void *opaque,
                                              int64_t int64_value) {
        unit_test_data.int64_value = int64_value;
}

static void
unit_test_telemetry_decoded_type(void *opaque,
                                 rd_kafka_telemetry_metric_type_t type) {
        unit_test_data.type = type;
        unit_test_data.current_field++;
}

static void
unit_test_telemetry_decode_error(void *opaque, const char *error, ...) {
        char buffer[1024];
        va_list ap;
        va_start(ap, error);
        rd_vsnprintf(buffer, sizeof(buffer), error, ap);
        va_end(ap);
        RD_UT_SAY("%s", buffer);
        rd_assert(!*"Failure while decoding telemetry data");
}

int unit_test_telemetry(rd_kafka_type_t rk_type,
                        rd_kafka_telemetry_producer_metric_name_t metric_name,
                        const char *expected_name,
                        const char *expected_description,
                        rd_kafka_telemetry_metric_type_t expected_type,
                        rd_bool_t is_double,
                        rd_bool_t is_per_broker,
                        void (*set_metric_value)(rd_kafka_t *,
                                                 rd_kafka_broker_t *),
                        int64_t expected_value_int,
                        double expected_value_double) {
        rd_kafka_t *rk = rd_calloc(1, sizeof(*rk));
        rwlock_init(&rk->rk_lock);
        rd_kafka_conf_t *conf = rd_kafka_conf_new();
        char *client_rack = "rack1", *transactional_id = "tx-id",
             *group_id = "group-id", *group_instance_id = "group-instance-id";
        rd_kafka_conf_set(conf, "client.rack", client_rack, NULL, 0);
        rd_kafka_conf_set(conf, "transactional.id", transactional_id, NULL, 0);
        rd_kafka_conf_set(conf, "group.id", group_id, NULL, 0);
        rd_kafka_conf_set(conf, "group.instance.id", group_instance_id, NULL,
                          0);
        rk->rk_conf = *conf;
        rd_free(conf);

        rk->rk_type           = rk_type;
        rk->rk_cgrp           = rd_calloc(1, sizeof(*rk->rk_cgrp));
        rk->rk_broker_cnt.val = 1;
        rk->rk_telemetry.matched_metrics_cnt = 1;
        rk->rk_telemetry.matched_metrics =
            rd_malloc(sizeof(rd_kafka_telemetry_producer_metric_name_t) *
                      rk->rk_telemetry.matched_metrics_cnt);
        rk->rk_telemetry.matched_metrics[0] = metric_name;
        rk->rk_telemetry.rk_historic_c.ts_start =
            (rd_uclock() - 1000 * 1000) * 1000;
        rk->rk_telemetry.rk_historic_c.ts_last =
            (rd_uclock() - 1000 * 1000) * 1000;

        rd_avg_init(&rk->rk_telemetry.rd_avg_current.rk_avg_poll_idle_ratio,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
        rd_avg_init(&rk->rk_telemetry.rd_avg_current.rk_avg_commit_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
        rd_avg_init(&rk->rk_telemetry.rd_avg_current.rk_avg_rebalance_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);

        rd_avg_init(&rk->rk_telemetry.rd_avg_rollover.rk_avg_poll_idle_ratio,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
        rd_avg_init(&rk->rk_telemetry.rd_avg_rollover.rk_avg_commit_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
        rd_avg_init(&rk->rk_telemetry.rd_avg_rollover.rk_avg_rebalance_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);

        rd_strlcpy(rk->rk_name, "unittest", sizeof(rk->rk_name));
        clear_unit_test_data(expected_value_int, expected_value_double);

        rd_kafka_telemetry_decode_interface_t decode_interface = {
            .decoded_string = unit_test_telemetry_decoded_string,
            .decoded_NumberDataPoint =
                unit_test_telemetry_decoded_NumberDataPoint,
            .decoded_int64 = unit_test_telemetry_decoded_int64,
            .decoded_type  = unit_test_telemetry_decoded_type,
            .decode_error  = unit_test_telemetry_decode_error,
            .opaque        = &unit_test_data,
        };

        TAILQ_INIT(&rk->rk_brokers);

        rd_kafka_broker_t *rkb = rd_calloc(1, sizeof(*rkb));
        rkb->rkb_nodeid        = 1001;
        mtx_init(&rkb->rkb_lock, mtx_plain);

        rd_avg_init(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_rtt,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_outbuf_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_throttle,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_fetch_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);

        rd_avg_init(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_rtt,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_outbuf_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_throttle,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_fetch_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_produce_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_produce_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);

        set_metric_value(rk, rkb);

        TAILQ_INSERT_HEAD(&rk->rk_brokers, rkb, rkb_link);
        rd_buf_t *rbuf              = rd_kafka_telemetry_encode_metrics(rk);
        void *metrics_payload       = rbuf->rbuf_wpos->seg_p;
        size_t metrics_payload_size = rbuf->rbuf_wpos->seg_of;
        RD_UT_SAY("metrics_payload_size: %" PRIusz, metrics_payload_size);

        RD_UT_ASSERT(metrics_payload_size != 0, "Metrics payload zero");

        bool decode_status = rd_kafka_telemetry_decode_metrics(
            &decode_interface, metrics_payload, metrics_payload_size);

        RD_UT_ASSERT(decode_status == 1, "Decoding failed");
        RD_UT_ASSERT(unit_test_data.type == expected_type,
                     "Metric type mismatch");
        RD_UT_ASSERT(strcmp(unit_test_data.metric_name, expected_name) == 0,
                     "Metric name mismatch %s != %s",
                     unit_test_data.metric_name, expected_name);
        RD_UT_ASSERT(strcmp(unit_test_data.metric_description,
                            expected_description) == 0,
                     "Metric description mismatch");
        if (is_double)
                RD_UT_ASSERT(
                    rd_dbl_eq0(unit_test_data.metric_value_double,
                               unit_test_data.expected_metric_value_double,
                               0.01),
                    "Metric value mismatch");
        else
                RD_UT_ASSERT(unit_test_data.metric_value_int ==
                                 unit_test_data.expected_metric_value_int,
                             "Metric value mismatch");
        if (is_per_broker)
                RD_UT_ASSERT(unit_test_data.int64_value == 1001,
                             "Expected broker mismatch");
        RD_UT_ASSERT(unit_test_data.metric_time != 0, "Metric time mismatch");
        if (rk_type == RD_KAFKA_PRODUCER) {
                RD_UT_ASSERT(unit_test_data.label_count == 2,
                             "Label count mismatch");
                RD_UT_ASSERT(
                    strcmp(unit_test_data.labels[0].key, "client_rack") == 0,
                    "Client rack key mismatch");
                RD_UT_ASSERT(
                    strcmp(unit_test_data.labels[0].value, client_rack) == 0,
                    "Client rack value mismatch");
                RD_UT_ASSERT(strcmp(unit_test_data.labels[1].key,
                                    "transactional_id") == 0,
                             "Transactional id key mismatch");
                RD_UT_ASSERT(strcmp(unit_test_data.labels[1].value,
                                    transactional_id) == 0,
                             "Transactional id value mismatch");
        } else {
                RD_UT_ASSERT(unit_test_data.label_count == 3,
                             "Label count mismatch");
                RD_UT_ASSERT(
                    strcmp(unit_test_data.labels[0].key, "client_rack") == 0,
                    "Client rack key mismatch");
                RD_UT_ASSERT(
                    strcmp(unit_test_data.labels[0].value, client_rack) == 0,
                    "Client rack value mismatch");
                RD_UT_ASSERT(strcmp(unit_test_data.labels[1].key, "group_id") ==
                                 0,
                             "Group id key mismatch");
                RD_UT_ASSERT(strcmp(unit_test_data.labels[1].value, group_id) ==
                                 0,
                             "Group id value mismatch");
                RD_UT_ASSERT(strcmp(unit_test_data.labels[2].key,
                                    "group_instance_id") == 0,
                             "Group instance id key mismatch");
                RD_UT_ASSERT(strcmp(unit_test_data.labels[2].value,
                                    group_instance_id) == 0,
                             "Group instance id value mismatch");
        }

        rd_free(rk->rk_telemetry.matched_metrics);
        rd_buf_destroy_free(rbuf);
        rd_avg_destroy(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_rtt);
        rd_avg_destroy(
            &rkb->rkb_telemetry.rd_avg_current.rkb_avg_outbuf_latency);
        rd_avg_destroy(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_throttle);
        rd_avg_destroy(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_rtt);
        rd_avg_destroy(
            &rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_outbuf_latency);
        rd_avg_destroy(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_throttle);

        rd_avg_destroy(
            &rkb->rkb_telemetry.rd_avg_current.rkb_avg_fetch_latency);
        rd_avg_destroy(
            &rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_fetch_latency);

        rd_avg_destroy(&rk->rk_telemetry.rd_avg_current.rk_avg_poll_idle_ratio);
        rd_avg_destroy(
            &rk->rk_telemetry.rd_avg_rollover.rk_avg_poll_idle_ratio);

        rd_avg_destroy(
            &rk->rk_telemetry.rd_avg_current.rk_avg_rebalance_latency);
        rd_avg_destroy(
            &rk->rk_telemetry.rd_avg_rollover.rk_avg_rebalance_latency);

        rd_avg_destroy(&rk->rk_telemetry.rd_avg_current.rk_avg_commit_latency);
        rd_avg_destroy(&rk->rk_telemetry.rd_avg_rollover.rk_avg_commit_latency);

        rd_avg_destroy(
            &rkb->rkb_telemetry.rd_avg_current.rkb_avg_produce_latency);
        rd_avg_destroy(
            &rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_produce_latency);

        mtx_destroy(&rkb->rkb_lock);
        rd_free(rkb);
        rwlock_destroy(&rk->rk_lock);
        rd_free(rk->rk_cgrp);
        rd_kafka_anyconf_destroy(_RK_GLOBAL, &rk->rk_conf);
        rd_free(rk);
        RD_UT_PASS();
        return 0;
}

void unit_test_telemetry_set_connects(rd_kafka_t *rk, rd_kafka_broker_t *rkb) {
        rkb->rkb_c.connects.val = 1;
}

void unit_test_telemetry_set_connects2(rd_kafka_t *rk, rd_kafka_broker_t *rkb) {
        rkb->rkb_c.connects.val = 2;
}

void unit_test_telemetry_set_rtt(rd_kafka_t *rk, rd_kafka_broker_t *rkb) {
        rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_rtt, 1000);
        rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_rtt, 1000);
}

void unit_test_telemetry_set_throttle_time(rd_kafka_t *rk,
                                           rd_kafka_broker_t *rkb) {
        rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_throttle, 1);
        rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_throttle, 1);
        rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_throttle, 1);
}

void unit_test_telemetry_set_queue_time(rd_kafka_t *rk,
                                        rd_kafka_broker_t *rkb) {
        rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_outbuf_latency,
                   1000);
        rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_outbuf_latency,
                   1000);
        rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_outbuf_latency,
                   1000);
        rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_outbuf_latency,
                   1000);
}

void unit_test_telemetry_set_produce_latency(rd_kafka_t *rk,
                                             rd_kafka_broker_t *rkb) {
        rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_produce_latency,
                   1000);
        rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_produce_latency,
                   1000);
}

void unit_test_telemetry_set_coordinator_assigned_partitions(
    rd_kafka_t *rk,
    rd_kafka_broker_t *rkb) {
        rk->rk_cgrp->rkcg_c.assignment_size = 1;
}

void unit_test_telemetry_set_rebalance_latency(rd_kafka_t *rk,
                                               rd_kafka_broker_t *rkb) {
        rd_avg_add(&rk->rk_telemetry.rd_avg_current.rk_avg_rebalance_latency,
                   1000);
}

void unit_test_telemetry_set_fetch_latency(rd_kafka_t *rk,
                                           rd_kafka_broker_t *rkb) {
        rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_fetch_latency,
                   1000);
        rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_fetch_latency,
                   1000);
}

void unit_test_telemetry_set_poll_idle_ratio(rd_kafka_t *rk,
                                             rd_kafka_broker_t *rkb) {
        rd_avg_add(&rk->rk_telemetry.rd_avg_current.rk_avg_poll_idle_ratio,
                   1000000);
        rd_avg_add(&rk->rk_telemetry.rd_avg_current.rk_avg_poll_idle_ratio,
                   1000000);
        rd_avg_add(&rk->rk_telemetry.rd_avg_current.rk_avg_poll_idle_ratio,
                   1000000);
}

void unit_test_telemetry_set_commit_latency(rd_kafka_t *rk,
                                            rd_kafka_broker_t *rkb) {
        rd_avg_add(&rk->rk_telemetry.rd_avg_current.rk_avg_commit_latency,
                   1000);
        rd_avg_add(&rk->rk_telemetry.rd_avg_current.rk_avg_commit_latency,
                   1000);
}

int unit_test_telemetry_gauge(void) {
        int fails                            = 0;
        int64_t default_expected_value_int   = 1;
        double default_expected_value_double = 1.0;
        /* Producer metrics */
        fails += unit_test_telemetry(
            RD_KAFKA_PRODUCER,
            RD_KAFKA_TELEMETRY_METRIC_PRODUCER_CONNECTION_CREATION_RATE,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "producer.connection.creation.rate",
            "The rate of connections established per second.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_true, rd_false,
            unit_test_telemetry_set_connects, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_PRODUCER,
            RD_KAFKA_TELEMETRY_METRIC_PRODUCER_NODE_REQUEST_LATENCY_AVG,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "producer.node.request.latency.avg",
            "The average request latency in ms for a node.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_true, rd_true,
            unit_test_telemetry_set_rtt, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_PRODUCER,
            RD_KAFKA_TELEMETRY_METRIC_PRODUCER_NODE_REQUEST_LATENCY_MAX,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "producer.node.request.latency.max",
            "The maximum request latency in ms for a node.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_false, rd_true,
            unit_test_telemetry_set_rtt, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_PRODUCER,
            RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_THROTTLE_TIME_AVG,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "producer.produce.throttle.time.avg",
            "The average throttle time in ms for a node.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_true, rd_false,
            unit_test_telemetry_set_throttle_time, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_PRODUCER,
            RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_THROTTLE_TIME_MAX,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "producer.produce.throttle.time.max",
            "The maximum throttle time in ms for a node.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_false, rd_false,
            unit_test_telemetry_set_throttle_time, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_PRODUCER,
            RD_KAFKA_TELEMETRY_METRIC_PRODUCER_RECORD_QUEUE_TIME_AVG,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX "producer.record.queue.time.avg",
            "The average time in ms a record spends in the producer queue.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_true, rd_false,
            unit_test_telemetry_set_queue_time, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_PRODUCER,
            RD_KAFKA_TELEMETRY_METRIC_PRODUCER_RECORD_QUEUE_TIME_MAX,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX "producer.record.queue.time.max",
            "The maximum time in ms a record spends in the producer queue.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_false, rd_false,
            unit_test_telemetry_set_queue_time, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_PRODUCER,
            RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_LATENCY_AVG,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX "producer.request.latency.avg",
            "The average request latency in ms for produce requests.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_true, rd_false,
            unit_test_telemetry_set_produce_latency, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_PRODUCER,
            RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_LATENCY_MAX,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX "producer.request.latency.max",
            "The maximum request latency in ms for produce requests.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_false, rd_false,
            unit_test_telemetry_set_produce_latency, default_expected_value_int,
            default_expected_value_double);

        /* Consumer metrics */
        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_CONNECTION_CREATION_RATE,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "consumer.connection.creation.rate",
            "The rate of connections established per second.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_true, rd_false,
            unit_test_telemetry_set_connects, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_NODE_REQUEST_LATENCY_AVG,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "consumer.node.request.latency.avg",
            "The average request latency in ms for a node.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_true, rd_true,
            unit_test_telemetry_set_rtt, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_NODE_REQUEST_LATENCY_MAX,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "consumer.node.request.latency.max",
            "The maximum request latency in ms for a node.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_false, rd_true,
            unit_test_telemetry_set_rtt, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_ASSIGNED_PARTITIONS,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "consumer.coordinator.assigned.partitions",
            "The number of partitions currently assigned to this consumer.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_false, rd_false,
            unit_test_telemetry_set_coordinator_assigned_partitions,
            default_expected_value_int, default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_REBALANCE_LATENCY_AVG,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "consumer.coordinator.rebalance.latency.avg",
            "The average rebalance latency in ms for the "
            "consumer coordinator.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_true, rd_false,
            unit_test_telemetry_set_rebalance_latency,
            default_expected_value_int, default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_REBALANCE_LATENCY_MAX,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "consumer.coordinator.rebalance.latency.max",
            "The maximum rebalance latency in ms for the "
            "consumer coordinator.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_false, rd_false,
            unit_test_telemetry_set_rebalance_latency,
            default_expected_value_int, default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_FETCH_MANAGER_FETCH_LATENCY_AVG,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "consumer.fetch.manager.fetch.latency.avg",
            "The average fetch latency in ms for the fetch manager.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_true, rd_false,
            unit_test_telemetry_set_fetch_latency, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_FETCH_MANAGER_FETCH_LATENCY_MAX,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "consumer.fetch.manager.fetch.latency.max",
            "The maximum fetch latency in ms for the fetch manager.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_false, rd_false,
            unit_test_telemetry_set_fetch_latency, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_POLL_IDLE_RATIO_AVG,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX "consumer.poll.idle.ratio.avg",
            "The average ratio of idle to poll for a consumer.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_true, rd_false,
            unit_test_telemetry_set_poll_idle_ratio, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_COMMIT_LATENCY_AVG,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "consumer.coordinator.commit.latency.avg",
            "The average commit latency in ms for the consumer coordinator.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_true, rd_false,
            unit_test_telemetry_set_commit_latency, default_expected_value_int,
            default_expected_value_double);
        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_COMMIT_LATENCY_MAX,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "consumer.coordinator.commit.latency.max",
            "The maximum commit latency in ms for the consumer coordinator.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE, rd_false, rd_false,
            unit_test_telemetry_set_commit_latency, default_expected_value_int,
            default_expected_value_double);
        return fails;
}

int unit_test_telemetry_sum(void) {
        int fails                            = 0;
        int64_t default_expected_value_int   = 1;
        double default_expected_value_double = 1.0;

        /* Producer metrics */
        fails += unit_test_telemetry(
            RD_KAFKA_PRODUCER,
            RD_KAFKA_TELEMETRY_METRIC_PRODUCER_CONNECTION_CREATION_TOTAL,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "producer.connection.creation.total",
            "The total number of connections established.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_SUM, rd_false, rd_false,
            unit_test_telemetry_set_connects, default_expected_value_int,
            default_expected_value_double);

        /* Consumer metrics */
        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_CONNECTION_CREATION_TOTAL,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "consumer.connection.creation.total",
            "The total number of connections established.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_SUM, rd_false, rd_false,
            unit_test_telemetry_set_connects, default_expected_value_int,
            default_expected_value_double);
        /* Test with expected value 2 */
        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_CONNECTION_CREATION_TOTAL,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "consumer.connection.creation.total",
            "The total number of connections established.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_SUM, rd_false, rd_false,
            unit_test_telemetry_set_connects2, 2, 0.0);

        fails += unit_test_telemetry(
            RD_KAFKA_CONSUMER,
            RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_REBALANCE_LATENCY_TOTAL,
            RD_KAFKA_TELEMETRY_METRIC_PREFIX
            "consumer.coordinator.rebalance.latency.total",
            "The total rebalance latency in ms for the "
            "consumer coordinator.",
            RD_KAFKA_TELEMETRY_METRIC_TYPE_SUM, rd_false, rd_false,
            unit_test_telemetry_set_rebalance_latency,
            default_expected_value_int, default_expected_value_double);
        return fails;
}

int unittest_telemetry_decode(void) {
        int fails = 0;
        fails += unit_test_telemetry_gauge();
        fails += unit_test_telemetry_sum();
        return fails;
}
