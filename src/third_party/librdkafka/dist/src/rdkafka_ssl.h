/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2019-2022, Magnus Edenhill
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


#ifndef _RDKAFKA_SSL_H_
#define _RDKAFKA_SSL_H_

void rd_kafka_transport_ssl_close(rd_kafka_transport_t *rktrans);
int rd_kafka_transport_ssl_connect(rd_kafka_broker_t *rkb,
                                   rd_kafka_transport_t *rktrans,
                                   char *errstr,
                                   size_t errstr_size);
int rd_kafka_transport_ssl_handshake(rd_kafka_transport_t *rktrans);
ssize_t rd_kafka_transport_ssl_send(rd_kafka_transport_t *rktrans,
                                    rd_slice_t *slice,
                                    char *errstr,
                                    size_t errstr_size);
ssize_t rd_kafka_transport_ssl_recv(rd_kafka_transport_t *rktrans,
                                    rd_buf_t *rbuf,
                                    char *errstr,
                                    size_t errstr_size);


void rd_kafka_ssl_ctx_term(rd_kafka_t *rk);
int rd_kafka_ssl_ctx_init(rd_kafka_t *rk, char *errstr, size_t errstr_size);

void rd_kafka_ssl_term(void);
void rd_kafka_ssl_init(void);

const char *rd_kafka_ssl_last_error_str(void);

int rd_kafka_ssl_hmac(rd_kafka_broker_t *rkb,
                      const EVP_MD *evp,
                      const rd_chariov_t *in,
                      const rd_chariov_t *salt,
                      int itcnt,
                      rd_chariov_t *out);

#endif /* _RDKAFKA_SSL_H_ */
