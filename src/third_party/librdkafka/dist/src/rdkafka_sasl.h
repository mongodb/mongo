/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2015-2022, Magnus Edenhill
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

#ifndef _RDKAFKA_SASL_H_
#define _RDKAFKA_SASL_H_



int rd_kafka_sasl_recv(rd_kafka_transport_t *rktrans,
                       const void *buf,
                       size_t len,
                       char *errstr,
                       size_t errstr_size);
int rd_kafka_sasl_io_event(rd_kafka_transport_t *rktrans,
                           int events,
                           char *errstr,
                           size_t errstr_size);
void rd_kafka_sasl_close(rd_kafka_transport_t *rktrans);
int rd_kafka_sasl_client_new(rd_kafka_transport_t *rktrans,
                             char *errstr,
                             size_t errstr_size);

void rd_kafka_sasl_broker_term(rd_kafka_broker_t *rkb);
void rd_kafka_sasl_broker_init(rd_kafka_broker_t *rkb);

int rd_kafka_sasl_init(rd_kafka_t *rk, char *errstr, size_t errstr_size);
void rd_kafka_sasl_term(rd_kafka_t *rk);

rd_bool_t rd_kafka_sasl_ready(rd_kafka_t *rk);

void rd_kafka_sasl_global_term(void);
int rd_kafka_sasl_global_init(void);

int rd_kafka_sasl_select_provider(rd_kafka_t *rk,
                                  char *errstr,
                                  size_t errstr_size);

#endif /* _RDKAFKA_SASL_H_ */
