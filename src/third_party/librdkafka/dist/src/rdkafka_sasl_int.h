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

#ifndef _RDKAFKA_SASL_INT_H_
#define _RDKAFKA_SASL_INT_H_

struct rd_kafka_sasl_provider {
        const char *name;

        /** Per client-instance (rk) initializer */
        int (*init)(rd_kafka_t *rk, char *errstr, size_t errstr_size);

        /** Per client-instance (rk) destructor */
        void (*term)(rd_kafka_t *rk);

        /** Returns rd_true if provider is ready to be used, else rd_false */
        rd_bool_t (*ready)(rd_kafka_t *rk);

        int (*client_new)(rd_kafka_transport_t *rktrans,
                          const char *hostname,
                          char *errstr,
                          size_t errstr_size);

        int (*recv)(struct rd_kafka_transport_s *s,
                    const void *buf,
                    size_t size,
                    char *errstr,
                    size_t errstr_size);
        void (*close)(struct rd_kafka_transport_s *);

        void (*broker_init)(rd_kafka_broker_t *rkb);
        void (*broker_term)(rd_kafka_broker_t *rkb);

        int (*conf_validate)(rd_kafka_t *rk, char *errstr, size_t errstr_size);
};

#ifdef _WIN32
extern const struct rd_kafka_sasl_provider rd_kafka_sasl_win32_provider;
#endif

#if WITH_SASL_CYRUS
extern const struct rd_kafka_sasl_provider rd_kafka_sasl_cyrus_provider;
void rd_kafka_sasl_cyrus_global_term(void);
int rd_kafka_sasl_cyrus_global_init(void);
#endif

extern const struct rd_kafka_sasl_provider rd_kafka_sasl_plain_provider;

#if WITH_SASL_SCRAM
extern const struct rd_kafka_sasl_provider rd_kafka_sasl_scram_provider;
#endif

#if WITH_SASL_OAUTHBEARER
extern const struct rd_kafka_sasl_provider rd_kafka_sasl_oauthbearer_provider;
#endif

void rd_kafka_sasl_auth_done(rd_kafka_transport_t *rktrans);
int rd_kafka_sasl_send(rd_kafka_transport_t *rktrans,
                       const void *payload,
                       int len,
                       char *errstr,
                       size_t errstr_size);

#endif /* _RDKAFKA_SASL_INT_H_ */
