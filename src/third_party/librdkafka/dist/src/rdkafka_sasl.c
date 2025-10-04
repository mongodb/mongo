/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2015-2022, Magnus Edenhill
 *               2023, Confluent Inc.
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

#include "rdkafka_int.h"
#include "rdkafka_transport.h"
#include "rdkafka_transport_int.h"
#include "rdkafka_request.h"
#include "rdkafka_sasl.h"
#include "rdkafka_sasl_int.h"
#include "rdkafka_request.h"
#include "rdkafka_queue.h"

/**
 * @brief Send SASL auth data using legacy directly on socket framing.
 *
 * @warning This is a blocking call.
 */
static int rd_kafka_sasl_send_legacy(rd_kafka_transport_t *rktrans,
                                     const void *payload,
                                     int len,
                                     char *errstr,
                                     size_t errstr_size) {
        rd_buf_t buf;
        rd_slice_t slice;
        int32_t hdr;

        rd_buf_init(&buf, 1 + 1, sizeof(hdr));

        hdr = htobe32(len);
        rd_buf_write(&buf, &hdr, sizeof(hdr));
        if (payload)
                rd_buf_push(&buf, payload, len, NULL);

        rd_slice_init_full(&slice, &buf);

        /* Simulate blocking behaviour on non-blocking socket..
         * FIXME: This isn't optimal but is highly unlikely to stall since
         *        the socket buffer will most likely not be exceeded. */
        do {
                int r;

                r = (int)rd_kafka_transport_send(rktrans, &slice, errstr,
                                                 errstr_size);
                if (r == -1) {
                        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SASL",
                                   "SASL send failed: %s", errstr);
                        rd_buf_destroy(&buf);
                        return -1;
                }

                if (rd_slice_remains(&slice) == 0)
                        break;

                /* Avoid busy-looping */
                rd_usleep(10 * 1000, NULL);

        } while (1);

        rd_buf_destroy(&buf);

        return 0;
}

/**
 * @brief Send auth message with framing (either legacy or Kafka framing).
 *
 * @warning This is a blocking call when used with the legacy framing.
 */
int rd_kafka_sasl_send(rd_kafka_transport_t *rktrans,
                       const void *payload,
                       int len,
                       char *errstr,
                       size_t errstr_size) {
        rd_kafka_broker_t *rkb = rktrans->rktrans_rkb;

        rd_rkb_dbg(
            rkb, SECURITY, "SASL", "Send SASL %s frame to broker (%d bytes)",
            (rkb->rkb_features & RD_KAFKA_FEATURE_SASL_AUTH_REQ) ? "Kafka"
                                                                 : "legacy",
            len);

        /* Blocking legacy framed send directly on the socket */
        if (!(rkb->rkb_features & RD_KAFKA_FEATURE_SASL_AUTH_REQ))
                return rd_kafka_sasl_send_legacy(rktrans, payload, len, errstr,
                                                 errstr_size);

        /* Kafka-framed asynchronous send */
        rd_kafka_SaslAuthenticateRequest(
            rkb, payload, (size_t)len, RD_KAFKA_NO_REPLYQ,
            rd_kafka_handle_SaslAuthenticate, NULL);

        return 0;
}


/**
 * @brief Authentication succesful
 *
 * Transition to next connect state.
 */
void rd_kafka_sasl_auth_done(rd_kafka_transport_t *rktrans) {
        /* Authenticated */
        rd_kafka_broker_connect_up(rktrans->rktrans_rkb);
}


/**
 * @brief Handle SASL auth data from broker.
 *
 * @locality broker thread
 *
 * @returns -1 on error, else 0.
 */
int rd_kafka_sasl_recv(rd_kafka_transport_t *rktrans,
                       const void *buf,
                       size_t len,
                       char *errstr,
                       size_t errstr_size) {

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SASL",
                   "Received SASL frame from broker (%" PRIusz " bytes)", len);

        return rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.provider->recv(
            rktrans, buf, len, errstr, errstr_size);
}

/**
 * @brief Non-kafka-protocol framed SASL auth data receive event.
 *
 * @locality broker thread
 *
 * @returns -1 on error, else 0.
 */
int rd_kafka_sasl_io_event(rd_kafka_transport_t *rktrans,
                           int events,
                           char *errstr,
                           size_t errstr_size) {
        rd_kafka_buf_t *rkbuf;
        int r;
        const void *buf;
        size_t len;

        if (!(events & POLLIN))
                return 0;

        r = rd_kafka_transport_framed_recv(rktrans, &rkbuf, errstr,
                                           errstr_size);
        if (r == -1) {
                if (!strcmp(errstr, "Disconnected"))
                        rd_snprintf(errstr, errstr_size,
                                    "Disconnected: check client %s credentials "
                                    "and broker logs",
                                    rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl
                                        .mechanisms);
                return -1;
        } else if (r == 0) /* not fully received yet */
                return 0;

        if (rkbuf) {
                rd_slice_init_full(&rkbuf->rkbuf_reader, &rkbuf->rkbuf_buf);
                /* Seek past framing header */
                rd_slice_seek(&rkbuf->rkbuf_reader, 4);
                len = rd_slice_remains(&rkbuf->rkbuf_reader);
                buf = rd_slice_ensure_contig(&rkbuf->rkbuf_reader, len);
        } else {
                buf = NULL;
                len = 0;
        }

        r = rd_kafka_sasl_recv(rktrans, buf, len, errstr, errstr_size);

        if (rkbuf)
                rd_kafka_buf_destroy(rkbuf);

        return r;
}


/**
 * @brief Close SASL session (from transport code)
 * @remark May be called on non-SASL transports (no-op)
 */
void rd_kafka_sasl_close(rd_kafka_transport_t *rktrans) {
        /* The broker might not be up, and the transport might not exist in that
         * case.*/
        if (!rktrans)
                return;

        const struct rd_kafka_sasl_provider *provider =
            rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.provider;

        if (provider && provider->close)
                provider->close(rktrans);
}



/**
 * Initialize and start SASL authentication.
 *
 * Returns 0 on successful init and -1 on error.
 *
 * Locality: broker thread
 */
int rd_kafka_sasl_client_new(rd_kafka_transport_t *rktrans,
                             char *errstr,
                             size_t errstr_size) {
        int r;
        rd_kafka_broker_t *rkb = rktrans->rktrans_rkb;
        rd_kafka_t *rk         = rkb->rkb_rk;
        char *hostname, *t;
        const struct rd_kafka_sasl_provider *provider =
            rk->rk_conf.sasl.provider;

        /* Verify broker support:
         * - RD_KAFKA_FEATURE_SASL_GSSAPI - GSSAPI supported
         * - RD_KAFKA_FEATURE_SASL_HANDSHAKE - GSSAPI, PLAIN and possibly
         *   other mechanisms supported. */
        if (!strcmp(rk->rk_conf.sasl.mechanisms, "GSSAPI")) {
                if (!(rkb->rkb_features & RD_KAFKA_FEATURE_SASL_GSSAPI)) {
                        rd_snprintf(errstr, errstr_size,
                                    "SASL GSSAPI authentication not supported "
                                    "by broker");
                        return -1;
                }
        } else if (!(rkb->rkb_features & RD_KAFKA_FEATURE_SASL_HANDSHAKE)) {
                rd_snprintf(errstr, errstr_size,
                            "SASL Handshake not supported by broker "
                            "(required by mechanism %s)%s",
                            rk->rk_conf.sasl.mechanisms,
                            rk->rk_conf.api_version_request
                                ? ""
                                : ": try api.version.request=true");
                return -1;
        }

        rd_kafka_broker_lock(rktrans->rktrans_rkb);
        rd_strdupa(&hostname, rktrans->rktrans_rkb->rkb_nodename);
        rd_kafka_broker_unlock(rktrans->rktrans_rkb);

        if ((t = strchr(hostname, ':')))
                *t = '\0'; /* remove ":port" */

        rd_rkb_dbg(rkb, SECURITY, "SASL",
                   "Initializing SASL client: service name %s, "
                   "hostname %s, mechanisms %s, provider %s",
                   rk->rk_conf.sasl.service_name, hostname,
                   rk->rk_conf.sasl.mechanisms, provider->name);

        r = provider->client_new(rktrans, hostname, errstr, errstr_size);
        if (r != -1)
                rd_kafka_transport_poll_set(rktrans, POLLIN);

        return r;
}



rd_kafka_queue_t *rd_kafka_queue_get_sasl(rd_kafka_t *rk) {
        if (!rk->rk_sasl.callback_q)
                return NULL;

        return rd_kafka_queue_new0(rk, rk->rk_sasl.callback_q);
}


/**
 * Per handle SASL term.
 *
 * Locality: broker thread
 */
void rd_kafka_sasl_broker_term(rd_kafka_broker_t *rkb) {
        const struct rd_kafka_sasl_provider *provider =
            rkb->rkb_rk->rk_conf.sasl.provider;
        if (provider->broker_term)
                provider->broker_term(rkb);
}

/**
 * Broker SASL init.
 *
 * Locality: broker thread
 */
void rd_kafka_sasl_broker_init(rd_kafka_broker_t *rkb) {
        const struct rd_kafka_sasl_provider *provider =
            rkb->rkb_rk->rk_conf.sasl.provider;
        if (provider->broker_init)
                provider->broker_init(rkb);
}


/**
 * @brief Per-instance initializer using the selected provider
 *
 * @returns 0 on success or -1 on error.
 *
 * @locality app thread (from rd_kafka_new())
 */
int rd_kafka_sasl_init(rd_kafka_t *rk, char *errstr, size_t errstr_size) {
        const struct rd_kafka_sasl_provider *provider =
            rk->rk_conf.sasl.provider;

        if (provider && provider->init)
                return provider->init(rk, errstr, errstr_size);

        return 0;
}


/**
 * @brief Per-instance destructor for the selected provider
 *
 * @locality app thread (from rd_kafka_new()) or rdkafka main thread
 */
void rd_kafka_sasl_term(rd_kafka_t *rk) {
        const struct rd_kafka_sasl_provider *provider =
            rk->rk_conf.sasl.provider;

        if (provider && provider->term)
                provider->term(rk);

        RD_IF_FREE(rk->rk_sasl.callback_q, rd_kafka_q_destroy_owner);
}


/**
 * @returns rd_true if provider is ready to be used or SASL not configured,
 *          else rd_false.
 *
 * @locks none
 * @locality any thread
 */
rd_bool_t rd_kafka_sasl_ready(rd_kafka_t *rk) {
        const struct rd_kafka_sasl_provider *provider =
            rk->rk_conf.sasl.provider;

        if (provider && provider->ready)
                return provider->ready(rk);

        return rd_true;
}


/**
 * @brief Select SASL provider for configured mechanism (singularis)
 * @returns 0 on success or -1 on failure.
 */
int rd_kafka_sasl_select_provider(rd_kafka_t *rk,
                                  char *errstr,
                                  size_t errstr_size) {
        const struct rd_kafka_sasl_provider *provider = NULL;

        if (!strcmp(rk->rk_conf.sasl.mechanisms, "GSSAPI")) {
                /* GSSAPI / Kerberos */
#ifdef _WIN32
                provider = &rd_kafka_sasl_win32_provider;
#elif WITH_SASL_CYRUS
                provider = &rd_kafka_sasl_cyrus_provider;
#endif

        } else if (!strcmp(rk->rk_conf.sasl.mechanisms, "PLAIN")) {
                /* SASL PLAIN */
                provider = &rd_kafka_sasl_plain_provider;

        } else if (!strncmp(rk->rk_conf.sasl.mechanisms, "SCRAM-SHA-",
                            strlen("SCRAM-SHA-"))) {
                /* SASL SCRAM */
#if WITH_SASL_SCRAM
                provider = &rd_kafka_sasl_scram_provider;
#endif

        } else if (!strcmp(rk->rk_conf.sasl.mechanisms, "OAUTHBEARER")) {
                /* SASL OAUTHBEARER */
#if WITH_SASL_OAUTHBEARER
                provider = &rd_kafka_sasl_oauthbearer_provider;
#endif
        } else {
                /* Unsupported mechanism */
                rd_snprintf(errstr, errstr_size,
                            "Unsupported SASL mechanism: %s",
                            rk->rk_conf.sasl.mechanisms);
                return -1;
        }

        if (!provider) {
                rd_snprintf(errstr, errstr_size,
                            "No provider for SASL mechanism %s"
                            ": recompile librdkafka with "
#ifndef _WIN32
                            "libsasl2 or "
#endif
                            "openssl support. "
                            "Current build options:"
                            " PLAIN"
#ifdef _WIN32
                            " WindowsSSPI(GSSAPI)"
#endif
#if WITH_SASL_CYRUS
                            " SASL_CYRUS"
#endif
#if WITH_SASL_SCRAM
                            " SASL_SCRAM"
#endif
#if WITH_SASL_OAUTHBEARER
                            " OAUTHBEARER"
#endif
                            ,
                            rk->rk_conf.sasl.mechanisms);
                return -1;
        }

        rd_kafka_dbg(rk, SECURITY, "SASL",
                     "Selected provider %s for SASL mechanism %s",
                     provider->name, rk->rk_conf.sasl.mechanisms);

        /* Validate SASL config */
        if (provider->conf_validate &&
            provider->conf_validate(rk, errstr, errstr_size) == -1)
                return -1;

        rk->rk_conf.sasl.provider = provider;

        return 0;
}


rd_kafka_error_t *rd_kafka_sasl_background_callbacks_enable(rd_kafka_t *rk) {
        rd_kafka_queue_t *saslq, *bgq;

        if (!(saslq = rd_kafka_queue_get_sasl(rk)))
                return rd_kafka_error_new(
                    RD_KAFKA_RESP_ERR__NOT_CONFIGURED,
                    "No SASL mechanism using callbacks is configured");

        if (!(bgq = rd_kafka_queue_get_background(rk))) {
                rd_kafka_queue_destroy(saslq);
                return rd_kafka_error_new(
                    RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE,
                    "The background thread is not available");
        }

        rd_kafka_queue_forward(saslq, bgq);

        rd_kafka_queue_destroy(saslq);
        rd_kafka_queue_destroy(bgq);

        return NULL;
}


/**
 * Global SASL termination.
 */
void rd_kafka_sasl_global_term(void) {
#if WITH_SASL_CYRUS
        rd_kafka_sasl_cyrus_global_term();
#endif
}


/**
 * Global SASL init, called once per runtime.
 */
int rd_kafka_sasl_global_init(void) {
#if WITH_SASL_CYRUS
        return rd_kafka_sasl_cyrus_global_init();
#else
        return 0;
#endif
}

/**
 * Sets or resets the SASL (PLAIN or SCRAM) credentials used by this
 * client when making new connections to brokers.
 *
 * @returns NULL on success or an error object on error.
 */
rd_kafka_error_t *rd_kafka_sasl_set_credentials(rd_kafka_t *rk,
                                                const char *username,
                                                const char *password) {

        if (!username || !password)
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__INVALID_ARG,
                                          "Username and password are required");

        mtx_lock(&rk->rk_conf.sasl.lock);

        if (rk->rk_conf.sasl.username)
                rd_free(rk->rk_conf.sasl.username);
        rk->rk_conf.sasl.username = rd_strdup(username);

        if (rk->rk_conf.sasl.password)
                rd_free(rk->rk_conf.sasl.password);
        rk->rk_conf.sasl.password = rd_strdup(password);

        mtx_unlock(&rk->rk_conf.sasl.lock);

        rd_kafka_all_brokers_wakeup(rk, RD_KAFKA_BROKER_STATE_INIT,
                                    "SASL credentials updated");

        return NULL;
}
