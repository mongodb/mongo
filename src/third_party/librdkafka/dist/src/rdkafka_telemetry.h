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


#ifndef _RD_KAFKA_TELEMETRY_H_
#define _RD_KAFKA_TELEMETRY_H_

#include "rdkafka_int.h"

#define RD_KAFKA_TELEMETRY_METRICS_ALL_METRICS_SUBSCRIPTION "*"
#define RD_KAFKA_TELEMETRY_METRIC_NAME_MAX_LEN              128

void rd_kafka_handle_get_telemetry_subscriptions(rd_kafka_t *rk,
                                                 rd_kafka_resp_err_t err);

void rd_kafka_handle_push_telemetry(rd_kafka_t *rk, rd_kafka_resp_err_t err);

void rd_kafka_telemetry_clear(rd_kafka_t *rk,
                              rd_bool_t clear_control_flow_fields);

void rd_kafka_telemetry_await_termination(rd_kafka_t *rk);

void rd_kafka_telemetry_schedule_termination(rd_kafka_t *rk);

void rd_kafka_set_telemetry_broker_maybe(rd_kafka_t *rk,
                                         rd_kafka_broker_t *rkb);
#endif /* _RD_KAFKA_TELEMETRY_H_ */
