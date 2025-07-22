/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2017-2022, Magnus Edenhill
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

#ifndef _RDKAFKA_INTERCEPTOR_H
#define _RDKAFKA_INTERCEPTOR_H

rd_kafka_conf_res_t rd_kafka_interceptors_on_conf_set(rd_kafka_conf_t *conf,
                                                      const char *name,
                                                      const char *val,
                                                      char *errstr,
                                                      size_t errstr_size);
void rd_kafka_interceptors_on_conf_dup(rd_kafka_conf_t *new_conf,
                                       const rd_kafka_conf_t *old_conf,
                                       size_t filter_cnt,
                                       const char **filter);
void rd_kafka_interceptors_on_conf_destroy(rd_kafka_conf_t *conf);
void rd_kafka_interceptors_on_new(rd_kafka_t *rk, const rd_kafka_conf_t *conf);
void rd_kafka_interceptors_on_destroy(rd_kafka_t *rk);
void rd_kafka_interceptors_on_send(rd_kafka_t *rk,
                                   rd_kafka_message_t *rkmessage);
void rd_kafka_interceptors_on_acknowledgement(rd_kafka_t *rk,
                                              rd_kafka_message_t *rkmessage);
void rd_kafka_interceptors_on_acknowledgement_queue(
    rd_kafka_t *rk,
    rd_kafka_msgq_t *rkmq,
    rd_kafka_resp_err_t force_err);

void rd_kafka_interceptors_on_consume(rd_kafka_t *rk,
                                      rd_kafka_message_t *rkmessage);
void rd_kafka_interceptors_on_commit(
    rd_kafka_t *rk,
    const rd_kafka_topic_partition_list_t *offsets,
    rd_kafka_resp_err_t err);

void rd_kafka_interceptors_on_request_sent(rd_kafka_t *rk,
                                           int sockfd,
                                           const char *brokername,
                                           int32_t brokerid,
                                           int16_t ApiKey,
                                           int16_t ApiVersion,
                                           int32_t CorrId,
                                           size_t size);

void rd_kafka_interceptors_on_response_received(rd_kafka_t *rk,
                                                int sockfd,
                                                const char *brokername,
                                                int32_t brokerid,
                                                int16_t ApiKey,
                                                int16_t ApiVersion,
                                                int32_t CorrId,
                                                size_t size,
                                                int64_t rtt,
                                                rd_kafka_resp_err_t err);

void rd_kafka_interceptors_on_thread_start(rd_kafka_t *rk,
                                           rd_kafka_thread_type_t thread_type);
void rd_kafka_interceptors_on_thread_exit(rd_kafka_t *rk,
                                          rd_kafka_thread_type_t thread_type);

void rd_kafka_interceptors_on_broker_state_change(rd_kafka_t *rk,
                                                  int32_t broker_id,
                                                  const char *secproto,
                                                  const char *name,
                                                  int port,
                                                  const char *state);

void rd_kafka_conf_interceptor_ctor(int scope, void *pconf);
void rd_kafka_conf_interceptor_dtor(int scope, void *pconf);
void rd_kafka_conf_interceptor_copy(int scope,
                                    void *pdst,
                                    const void *psrc,
                                    void *dstptr,
                                    const void *srcptr,
                                    size_t filter_cnt,
                                    const char **filter);

void rd_kafka_interceptors_destroy(rd_kafka_conf_t *conf);

#endif /* _RDKAFKA_INTERCEPTOR_H */
