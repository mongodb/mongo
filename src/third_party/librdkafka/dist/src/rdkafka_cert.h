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


#ifndef _RDKAFKA_CERT_H_
#define _RDKAFKA_CERT_H_


/**
 * @struct rd_kafka_cert
 *
 * @brief Internal representation of a cert_type,cert_enc,memory tuple.
 *
 * @remark Certificates are read-only after construction.
 */
typedef struct rd_kafka_cert_s {
        rd_kafka_cert_type_t type;
        rd_kafka_cert_enc_t encoding;
        rd_refcnt_t refcnt;
#if WITH_SSL
        X509 *x509;             /**< Certificate (public key) */
        STACK_OF(X509) * chain; /**< Certificate chain (public key) */
        EVP_PKEY *pkey;         /**< Private key */
        X509_STORE *store;      /**< CA trusted certificates */
#endif
} rd_kafka_cert_t;

void rd_kafka_conf_cert_dtor(int scope, void *pconf);
void rd_kafka_conf_cert_copy(int scope,
                             void *pdst,
                             const void *psrc,
                             void *dstptr,
                             const void *srcptr,
                             size_t filter_cnt,
                             const char **filter);

#endif /* _RDKAFKA_CERT_H_ */
