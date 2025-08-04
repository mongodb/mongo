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

#ifndef _RDKAFKA_RDKAFKA_TELEMETRY_DECODE_H
#define _RDKAFKA_RDKAFKA_TELEMETRY_DECODE_H
#include "rd.h"
#include "opentelemetry/metrics.pb.h"
#include "rdkafka_telemetry_encode.h"

typedef struct rd_kafka_telemetry_decode_interface_s {
        void (*decoded_string)(void *opaque, const uint8_t *decoded);
        void (*decoded_NumberDataPoint)(
            void *opaque,
            const opentelemetry_proto_metrics_v1_NumberDataPoint *decoded);
        void (*decoded_int64)(void *opaque, int64_t decoded);
        void (*decoded_type)(void *opaque,
                             rd_kafka_telemetry_metric_type_t type);
        void (*decode_error)(void *opaque, const char *error, ...);
        void *opaque;
} rd_kafka_telemetry_decode_interface_t;

int rd_kafka_telemetry_uncompress_metrics_payload(
    rd_kafka_broker_t *rkb,
    rd_kafka_compression_t compression_type,
    void *compressed_payload,
    size_t compressed_payload_size,
    void **uncompressed_payload,
    size_t *uncompressed_payload_size);
int rd_kafka_telemetry_decode_metrics(
    rd_kafka_telemetry_decode_interface_t *decode_interface,
    void *buffer,
    size_t size);

#endif /* _RDKAFKA_RDKAFKA_TELEMETRY_DECODE_H */
