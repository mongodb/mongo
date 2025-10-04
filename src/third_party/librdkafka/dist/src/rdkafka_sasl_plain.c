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


/**
 * Builtin SASL PLAIN support when Cyrus SASL is not available
 */
#include "rdkafka_int.h"
#include "rdkafka_transport.h"
#include "rdkafka_transport_int.h"
#include "rdkafka_sasl.h"
#include "rdkafka_sasl_int.h"


/**
 * @brief Handle received frame from broker.
 */
static int rd_kafka_sasl_plain_recv(struct rd_kafka_transport_s *rktrans,
                                    const void *buf,
                                    size_t size,
                                    char *errstr,
                                    size_t errstr_size) {
        if (size)
                rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SASLPLAIN",
                           "Received non-empty SASL PLAIN (builtin) "
                           "response from broker (%" PRIusz " bytes)",
                           size);

        rd_kafka_sasl_auth_done(rktrans);

        return 0;
}


/**
 * @brief Initialize and start SASL PLAIN (builtin) authentication.
 *
 * Returns 0 on successful init and -1 on error.
 *
 * @locality broker thread
 */
int rd_kafka_sasl_plain_client_new(rd_kafka_transport_t *rktrans,
                                   const char *hostname,
                                   char *errstr,
                                   size_t errstr_size) {
        rd_kafka_broker_t *rkb = rktrans->rktrans_rkb;
        rd_kafka_t *rk         = rkb->rkb_rk;
        /* [authzid] UTF8NUL authcid UTF8NUL passwd */
        char *buf;
        int of     = 0;
        int zidlen = 0;
        int cidlen, pwlen;

        mtx_lock(&rk->rk_conf.sasl.lock);

        cidlen = rk->rk_conf.sasl.username
                     ? (int)strlen(rk->rk_conf.sasl.username)
                     : 0;
        pwlen = rk->rk_conf.sasl.password
                    ? (int)strlen(rk->rk_conf.sasl.password)
                    : 0;

        buf = rd_alloca(zidlen + 1 + cidlen + 1 + pwlen + 1);

        /* authzid: none (empty) */
        /* UTF8NUL */
        buf[of++] = 0;
        /* authcid */
        memcpy(&buf[of], rk->rk_conf.sasl.username, cidlen);
        of += cidlen;
        /* UTF8NUL */
        buf[of++] = 0;
        /* passwd */
        memcpy(&buf[of], rk->rk_conf.sasl.password, pwlen);
        of += pwlen;
        mtx_unlock(&rk->rk_conf.sasl.lock);

        rd_rkb_dbg(rkb, SECURITY, "SASLPLAIN",
                   "Sending SASL PLAIN (builtin) authentication token");

        if (rd_kafka_sasl_send(rktrans, buf, of, errstr, errstr_size))
                return -1;

        /* PLAIN is appearantly done here, but we still need to make sure
         * the PLAIN frame is sent and we get a response back (empty) */
        rktrans->rktrans_sasl.complete = 1;
        return 0;
}


/**
 * @brief Validate PLAIN config
 */
static int rd_kafka_sasl_plain_conf_validate(rd_kafka_t *rk,
                                             char *errstr,
                                             size_t errstr_size) {
        rd_bool_t both_set;

        mtx_lock(&rk->rk_conf.sasl.lock);
        both_set = rk->rk_conf.sasl.username && rk->rk_conf.sasl.password;
        mtx_unlock(&rk->rk_conf.sasl.lock);

        if (!both_set) {
                rd_snprintf(errstr, errstr_size,
                            "sasl.username and sasl.password must be set");
                return -1;
        }

        return 0;
}


const struct rd_kafka_sasl_provider rd_kafka_sasl_plain_provider = {
    .name          = "PLAIN (builtin)",
    .client_new    = rd_kafka_sasl_plain_client_new,
    .recv          = rd_kafka_sasl_plain_recv,
    .conf_validate = rd_kafka_sasl_plain_conf_validate};
