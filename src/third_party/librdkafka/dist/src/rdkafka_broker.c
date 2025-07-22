/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
 *               2023 Confluent Inc.
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

#if defined(__MINGW32__)
#include <ws2tcpip.h>
#endif

#ifndef _WIN32
#define _GNU_SOURCE
/*
 * AIX defines this and the value needs to be set correctly. For Solaris,
 * src/rd.h defines _POSIX_SOURCE to be 200809L, which corresponds to XPG7,
 * which itself is not compatible with _XOPEN_SOURCE on that platform.
 */
#if !defined(_AIX) && !defined(__sun)
#define _XOPEN_SOURCE
#endif
#include <signal.h>
#endif

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#include "rd.h"
#include "rdaddr.h"
#include "rdkafka_int.h"
#include "rdkafka_msg.h"
#include "rdkafka_msgset.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_broker.h"
#include "rdkafka_offset.h"
#include "rdkafka_telemetry.h"
#include "rdkafka_transport.h"
#include "rdkafka_proto.h"
#include "rdkafka_buf.h"
#include "rdkafka_request.h"
#include "rdkafka_sasl.h"
#include "rdkafka_interceptor.h"
#include "rdkafka_idempotence.h"
#include "rdkafka_txnmgr.h"
#include "rdkafka_fetcher.h"
#include "rdtime.h"
#include "rdcrc32.h"
#include "rdrand.h"
#include "rdkafka_lz4.h"
#if WITH_SSL
#include <openssl/err.h>
#endif
#include "rdendian.h"
#include "rdunittest.h"


static const int rd_kafka_max_block_ms = 1000;

const char *rd_kafka_broker_state_names[] = {
    "INIT",           "DOWN",        "TRY_CONNECT", "CONNECT",
    "SSL_HANDSHAKE",  "AUTH_LEGACY", "UP",          "APIVERSION_QUERY",
    "AUTH_HANDSHAKE", "AUTH_REQ",    "REAUTH"};

const char *rd_kafka_secproto_names[] = {
    [RD_KAFKA_PROTO_PLAINTEXT]      = "plaintext",
    [RD_KAFKA_PROTO_SSL]            = "ssl",
    [RD_KAFKA_PROTO_SASL_PLAINTEXT] = "sasl_plaintext",
    [RD_KAFKA_PROTO_SASL_SSL]       = "sasl_ssl",
    NULL};


/**
 * @returns true for logical brokers (e.g., coordinators) without an address set
 *
 * @locks_required rkb_lock
 */
#define rd_kafka_broker_is_addrless(rkb) (*(rkb)->rkb_nodename == '\0')

/**
 * @returns true if the broker needs a persistent connection
 * @locaility broker thread
 */
static RD_INLINE rd_bool_t
rd_kafka_broker_needs_persistent_connection(rd_kafka_broker_t *rkb) {
        return rkb->rkb_persistconn.internal ||
               rd_atomic32_get(&rkb->rkb_persistconn.coord);
}

/**
 * @returns > 0 if a connection to this broker is needed, else 0.
 * @locality broker thread
 * @locks none
 */
static RD_INLINE int rd_kafka_broker_needs_connection(rd_kafka_broker_t *rkb) {
        return rkb->rkb_state == RD_KAFKA_BROKER_STATE_INIT &&
               !rd_kafka_broker_or_instance_terminating(rkb) &&
               !rd_kafka_fatal_error_code(rkb->rkb_rk) &&
               (!rkb->rkb_rk->rk_conf.sparse_connections ||
                rd_kafka_broker_needs_persistent_connection(rkb));
}


static void rd_kafka_broker_handle_purge_queues(rd_kafka_broker_t *rkb,
                                                rd_kafka_op_t *rko);
static void rd_kafka_broker_trigger_monitors(rd_kafka_broker_t *rkb);


#define rd_kafka_broker_terminating(rkb)                                       \
        (rd_refcnt_get(&(rkb)->rkb_refcnt) <= 1)


/**
 * Construct broker nodename.
 */
static void rd_kafka_mk_nodename(char *dest,
                                 size_t dsize,
                                 const char *name,
                                 uint16_t port) {
        rd_snprintf(dest, dsize, "%s:%hu", name, port);
}

/**
 * Construct descriptive broker name
 */
static void rd_kafka_mk_brokername(char *dest,
                                   size_t dsize,
                                   rd_kafka_secproto_t proto,
                                   const char *nodename,
                                   int32_t nodeid,
                                   rd_kafka_confsource_t source) {

        /* Prepend protocol name to brokername, unless it is a
         * standard plaintext or logical broker in which case we
         * omit the protocol part. */
        if (proto != RD_KAFKA_PROTO_PLAINTEXT && source != RD_KAFKA_LOGICAL) {
                int r = rd_snprintf(dest, dsize, "%s://",
                                    rd_kafka_secproto_names[proto]);
                if (r >= (int)dsize) /* Skip proto name if it wont fit.. */
                        r = 0;

                dest += r;
                dsize -= r;
        }

        if (nodeid == RD_KAFKA_NODEID_UA)
                rd_snprintf(dest, dsize, "%s%s", nodename,
                            source == RD_KAFKA_LOGICAL
                                ? ""
                                : (source == RD_KAFKA_INTERNAL ? "/internal"
                                                               : "/bootstrap"));
        else
                rd_snprintf(dest, dsize, "%s/%" PRId32, nodename, nodeid);
}


/**
 * @brief Enable protocol feature(s) for the current broker.
 *
 * @locks broker_lock MUST be held
 * @locality broker thread
 */
static void rd_kafka_broker_feature_enable(rd_kafka_broker_t *rkb,
                                           int features) {
        if (features & rkb->rkb_features)
                return;

        rkb->rkb_features |= features;
        rd_rkb_dbg(rkb, BROKER | RD_KAFKA_DBG_PROTOCOL | RD_KAFKA_DBG_FEATURE,
                   "FEATURE", "Updated enabled protocol features +%s to %s",
                   rd_kafka_features2str(features),
                   rd_kafka_features2str(rkb->rkb_features));
}


/**
 * @brief Disable protocol feature(s) for the current broker.
 *
 * @locks broker_lock MUST be held
 * @locality broker thread
 */
static void rd_kafka_broker_feature_disable(rd_kafka_broker_t *rkb,
                                            int features) {
        if (!(features & rkb->rkb_features))
                return;

        rkb->rkb_features &= ~features;
        rd_rkb_dbg(rkb, BROKER | RD_KAFKA_DBG_PROTOCOL | RD_KAFKA_DBG_FEATURE,
                   "FEATURE", "Updated enabled protocol features -%s to %s",
                   rd_kafka_features2str(features),
                   rd_kafka_features2str(rkb->rkb_features));
}


/**
 * @brief Set protocol feature(s) for the current broker.
 *
 * @remark This replaces the previous feature set.
 *
 * @locality broker thread
 * @locks rd_kafka_broker_lock()
 */
static void rd_kafka_broker_features_set(rd_kafka_broker_t *rkb, int features) {
        if (rkb->rkb_features == features)
                return;

        rkb->rkb_features = features;
        rd_rkb_dbg(rkb, BROKER, "FEATURE",
                   "Updated enabled protocol features to %s",
                   rd_kafka_features2str(rkb->rkb_features));
}

/**
 * @brief Check and return supported ApiVersion for \p ApiKey.
 *
 * @returns the highest supported ApiVersion in the specified range (inclusive)
 *          or -1 if the ApiKey is not supported or no matching ApiVersion.
 *          The current feature set is also returned in \p featuresp
 *
 * @remark Same as rd_kafka_broker_ApiVersion_supported except for locking.
 *
 * @locks rd_kafka_broker_lock() if do_lock is rd_false
 * @locks_acquired rd_kafka_broker_lock() if do_lock is rd_true
 * @locality any
 */
int16_t rd_kafka_broker_ApiVersion_supported0(rd_kafka_broker_t *rkb,
                                              int16_t ApiKey,
                                              int16_t minver,
                                              int16_t maxver,
                                              int *featuresp,
                                              rd_bool_t do_lock) {
        struct rd_kafka_ApiVersion skel = {.ApiKey = ApiKey};
        struct rd_kafka_ApiVersion ret  = RD_ZERO_INIT, *retp;

        if (do_lock)
                rd_kafka_broker_lock(rkb);
        if (featuresp)
                *featuresp = rkb->rkb_features;

        if (rkb->rkb_features & RD_KAFKA_FEATURE_UNITTEST) {
                /* For unit tests let the broker support everything. */
                if (do_lock)
                        rd_kafka_broker_unlock(rkb);
                return maxver;
        }

        retp =
            bsearch(&skel, rkb->rkb_ApiVersions, rkb->rkb_ApiVersions_cnt,
                    sizeof(*rkb->rkb_ApiVersions), rd_kafka_ApiVersion_key_cmp);
        if (retp)
                ret = *retp;

        if (do_lock)
                rd_kafka_broker_unlock(rkb);

        if (!retp)
                return -1;

        if (ret.MaxVer < maxver) {
                if (ret.MaxVer < minver)
                        return -1;
                else
                        return ret.MaxVer;
        } else if (ret.MinVer > maxver)
                return -1;
        else
                return maxver;
}

/**
 * @brief Check and return supported ApiVersion for \p ApiKey.
 *
 * @returns the highest supported ApiVersion in the specified range (inclusive)
 *          or -1 if the ApiKey is not supported or no matching ApiVersion.
 *          The current feature set is also returned in \p featuresp
 * @locks none
 * @locks_acquired rd_kafka_broker_lock()
 * @locality any
 */
int16_t rd_kafka_broker_ApiVersion_supported(rd_kafka_broker_t *rkb,
                                             int16_t ApiKey,
                                             int16_t minver,
                                             int16_t maxver,
                                             int *featuresp) {
        return rd_kafka_broker_ApiVersion_supported0(
            rkb, ApiKey, minver, maxver, featuresp, rd_true /* do_lock */);
}

/**
 * @brief Set broker state.
 *
 *        \c rkb->rkb_state is the previous state, while
 *        \p state is the new state.
 *
 * @locks rd_kafka_broker_lock() MUST be held.
 * @locality broker thread
 */
void rd_kafka_broker_set_state(rd_kafka_broker_t *rkb, int state) {
        rd_bool_t trigger_monitors = rd_false;

        if ((int)rkb->rkb_state == state)
                return;

        rd_kafka_dbg(rkb->rkb_rk, BROKER, "STATE",
                     "%s: Broker changed state %s -> %s", rkb->rkb_name,
                     rd_kafka_broker_state_names[rkb->rkb_state],
                     rd_kafka_broker_state_names[state]);

        if (rkb->rkb_source == RD_KAFKA_INTERNAL ||
            RD_KAFKA_BROKER_IS_LOGICAL(rkb)) {
                /* no-op */
        } else if (rd_kafka_broker_state_is_down(state) &&
                   !rkb->rkb_down_reported) {
                /* Propagate ALL_BROKERS_DOWN event if all brokers are
                 * now down, unless we're terminating.
                 * Only trigger for brokers that has an address set,
                 * e.g., not logical brokers that lost their address. */
                if (rd_atomic32_add(&rkb->rkb_rk->rk_broker_down_cnt, 1) ==
                        rd_atomic32_get(&rkb->rkb_rk->rk_broker_cnt) -
                            rd_atomic32_get(
                                &rkb->rkb_rk->rk_logical_broker_cnt) &&
                    !rd_kafka_terminating(rkb->rkb_rk)) {
                        rd_kafka_rebootstrap(rkb->rkb_rk);
                        rd_kafka_op_err(
                            rkb->rkb_rk, RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN,
                            "%i/%i brokers are down",
                            rd_atomic32_get(&rkb->rkb_rk->rk_broker_down_cnt),
                            rd_atomic32_get(&rkb->rkb_rk->rk_broker_cnt) -
                                rd_atomic32_get(
                                    &rkb->rkb_rk->rk_logical_broker_cnt));
                }
                rkb->rkb_down_reported = 1;

        } else if (rd_kafka_broker_state_is_up(state) &&
                   rkb->rkb_down_reported) {
                rd_atomic32_sub(&rkb->rkb_rk->rk_broker_down_cnt, 1);
                rkb->rkb_down_reported = 0;
        }

        if (rkb->rkb_source != RD_KAFKA_INTERNAL) {
                if (rd_kafka_broker_state_is_up(state) &&
                    !rd_kafka_broker_state_is_up(rkb->rkb_state)) {
                        /* Up -> Down */
                        if (!RD_KAFKA_BROKER_IS_LOGICAL(rkb))
                                rd_atomic32_add(&rkb->rkb_rk->rk_broker_up_cnt,
                                                1);

                        trigger_monitors = rd_true;

                } else if (rd_kafka_broker_state_is_up(rkb->rkb_state) &&
                           !rd_kafka_broker_state_is_up(state)) {
                        /* ~Down(!Up) -> Up */
                        if (!RD_KAFKA_BROKER_IS_LOGICAL(rkb))
                                rd_atomic32_sub(&rkb->rkb_rk->rk_broker_up_cnt,
                                                1);

                        trigger_monitors = rd_true;
                }

                /* If the connection or connection attempt failed and there
                 * are coord_reqs or cgrp awaiting this coordinator to come up
                 * then trigger the monitors so that rd_kafka_coord_req_fsm()
                 * is triggered, which in turn may trigger a new coordinator
                 * query. */
                if (state == RD_KAFKA_BROKER_STATE_DOWN &&
                    rd_atomic32_get(&rkb->rkb_persistconn.coord) > 0)
                        trigger_monitors = rd_true;
        }

        rkb->rkb_state    = state;
        rkb->rkb_ts_state = rd_clock();

        if (trigger_monitors)
                rd_kafka_broker_trigger_monitors(rkb);

        /* Call on_broker_state_change interceptors */
        rd_kafka_interceptors_on_broker_state_change(
            rkb->rkb_rk, rkb->rkb_nodeid,
            rd_kafka_secproto_names[rkb->rkb_proto], rkb->rkb_origname,
            rkb->rkb_port, rd_kafka_broker_state_names[rkb->rkb_state]);

        rd_kafka_brokers_broadcast_state_change(rkb->rkb_rk);
}


/**
 * @brief Set, log and propagate broker fail error.
 *
 * @param rkb Broker connection that failed.
 * @param level Syslog level. LOG_DEBUG will not be logged unless debugging
 *              is enabled.
 * @param err The type of error that occurred.
 * @param fmt Format string.
 * @param ap Format string arguments.
 *
 * @locks none
 * @locality broker thread
 */
static void rd_kafka_broker_set_error(rd_kafka_broker_t *rkb,
                                      int level,
                                      rd_kafka_resp_err_t err,
                                      const char *fmt,
                                      va_list ap) {
        char errstr[512];
        char extra[128];
        size_t of = 0, ofe;
        rd_bool_t identical, suppress;
        int state_duration_ms = (int)((rd_clock() - rkb->rkb_ts_state) / 1000);


        /* If this is a logical broker we include its current nodename/address
         * in the log message. */
        rd_kafka_broker_lock(rkb);
        if (rkb->rkb_source == RD_KAFKA_LOGICAL &&
            !rd_kafka_broker_is_addrless(rkb)) {
                of = (size_t)rd_snprintf(errstr, sizeof(errstr),
                                         "%s: ", rkb->rkb_nodename);
                if (of > sizeof(errstr))
                        of = 0; /* If nodename overflows the entire buffer we
                                 * skip it completely since the error message
                                 * itself is more important. */
        }
        rd_kafka_broker_unlock(rkb);

        ofe = (size_t)rd_vsnprintf(errstr + of, sizeof(errstr) - of, fmt, ap);
        if (ofe > sizeof(errstr) - of)
                ofe = sizeof(errstr) - of;
        of += ofe;

        /* Provide more meaningful error messages in certain cases */
        if (err == RD_KAFKA_RESP_ERR__TRANSPORT &&
            !strcmp(errstr, "Disconnected")) {
                if (rkb->rkb_state == RD_KAFKA_BROKER_STATE_APIVERSION_QUERY) {
                        /* A disconnect while requesting ApiVersion typically
                         * means we're connecting to a SSL-listener as
                         * PLAINTEXT, but may also be caused by connecting to
                         * a broker that does not support ApiVersion (<0.10). */

                        if (rkb->rkb_proto != RD_KAFKA_PROTO_SSL &&
                            rkb->rkb_proto != RD_KAFKA_PROTO_SASL_SSL)
                                rd_kafka_broker_set_error(
                                    rkb, level, err,
                                    "Disconnected while requesting "
                                    "ApiVersion: "
                                    "might be caused by incorrect "
                                    "security.protocol configuration "
                                    "(connecting to a SSL listener?) or "
                                    "broker version is < 0.10 "
                                    "(see api.version.request)",
                                    ap /*ignored*/);
                        else
                                rd_kafka_broker_set_error(
                                    rkb, level, err,
                                    "Disconnected while requesting "
                                    "ApiVersion: "
                                    "might be caused by broker version "
                                    "< 0.10 (see api.version.request)",
                                    ap /*ignored*/);
                        return;

                } else if (rkb->rkb_state == RD_KAFKA_BROKER_STATE_UP &&
                           state_duration_ms < 2000 /*2s*/ &&
                           rkb->rkb_rk->rk_conf.security_protocol !=
                               RD_KAFKA_PROTO_SASL_SSL &&
                           rkb->rkb_rk->rk_conf.security_protocol !=
                               RD_KAFKA_PROTO_SASL_PLAINTEXT) {
                        /* If disconnected shortly after transitioning to UP
                         * state it typically means the broker listener is
                         * configured for SASL authentication but the client
                         * is not. */
                        rd_kafka_broker_set_error(
                            rkb, level, err,
                            "Disconnected: verify that security.protocol "
                            "is correctly configured, broker might "
                            "require SASL authentication",
                            ap /*ignored*/);
                        return;
                }
        }

        /* Check if error is identical to last error (prior to appending
         * the variable suffix "after Xms in state Y"), if so we should
         * suppress it. */
        identical = err == rkb->rkb_last_err.err &&
                    !strcmp(rkb->rkb_last_err.errstr, errstr);
        suppress = identical && rd_interval(&rkb->rkb_suppress.fail_error,
                                            30 * 1000 * 1000 /*30s*/, 0) <= 0;

        /* Copy last error prior to adding extras */
        rkb->rkb_last_err.err = err;
        rd_strlcpy(rkb->rkb_last_err.errstr, errstr,
                   sizeof(rkb->rkb_last_err.errstr));

        /* Time since last state change to help debug connection issues */
        ofe = rd_snprintf(extra, sizeof(extra), "after %dms in state %s",
                          state_duration_ms,
                          rd_kafka_broker_state_names[rkb->rkb_state]);

        /* Number of suppressed identical logs */
        if (identical && !suppress && rkb->rkb_last_err.cnt >= 1 &&
            ofe + 30 < sizeof(extra)) {
                size_t r =
                    (size_t)rd_snprintf(extra + ofe, sizeof(extra) - ofe,
                                        ", %d identical error(s) suppressed",
                                        rkb->rkb_last_err.cnt);
                if (r < sizeof(extra) - ofe)
                        ofe += r;
                else
                        ofe = sizeof(extra);
        }

        /* Append the extra info if there is enough room */
        if (ofe > 0 && of + ofe + 4 < sizeof(errstr))
                rd_snprintf(errstr + of, sizeof(errstr) - of, " (%s)", extra);

        /* Don't log interrupt-wakeups when terminating */
        if (err == RD_KAFKA_RESP_ERR__INTR && rd_kafka_terminating(rkb->rkb_rk))
                suppress = rd_true;

        if (!suppress)
                rkb->rkb_last_err.cnt = 1;
        else
                rkb->rkb_last_err.cnt++;

        rd_rkb_dbg(rkb, BROKER, "FAIL", "%s (%s)%s%s", errstr,
                   rd_kafka_err2name(err),
                   identical ? ": identical to last error" : "",
                   suppress ? ": error log suppressed" : "");

        if (level != LOG_DEBUG && (level <= LOG_CRIT || !suppress)) {
                rd_kafka_log(rkb->rkb_rk, level, "FAIL", "%s: %s",
                             rkb->rkb_name, errstr);

                /* Send ERR op to application for processing. */
                rd_kafka_q_op_err(rkb->rkb_rk->rk_rep, err, "%s: %s",
                                  rkb->rkb_name, errstr);
        }
}


/**
 * @brief Failure propagation to application.
 *
 * Will tear down connection to broker and trigger a reconnect.
 *
 * \p level is the log level, <=LOG_INFO will be logged while =LOG_DEBUG will
 * be debug-logged.
 *
 * @locality broker thread
 */
void rd_kafka_broker_fail(rd_kafka_broker_t *rkb,
                          int level,
                          rd_kafka_resp_err_t err,
                          const char *fmt,
                          ...) {
        va_list ap;
        rd_kafka_bufq_t tmpq_waitresp, tmpq;
        int old_state;
        rd_kafka_toppar_t *rktp;

        rd_kafka_assert(rkb->rkb_rk, thrd_is_current(rkb->rkb_thread));

        if (rkb->rkb_transport) {
                rd_kafka_transport_close(rkb->rkb_transport);
                rkb->rkb_transport = NULL;

                if (rkb->rkb_state >= RD_KAFKA_BROKER_STATE_UP)
                        rd_atomic32_add(&rkb->rkb_c.disconnects, 1);
        }

        rkb->rkb_req_timeouts = 0;

        if (rkb->rkb_recv_buf) {
                rd_kafka_buf_destroy(rkb->rkb_recv_buf);
                rkb->rkb_recv_buf = NULL;
        }

        rkb->rkb_reauth_in_progress = rd_false;

        va_start(ap, fmt);
        rd_kafka_broker_set_error(rkb, level, err, fmt, ap);
        va_end(ap);

        rd_kafka_broker_lock(rkb);

        /* If we're currently asking for ApiVersion and the connection
         * went down it probably means the broker does not support that request
         * and tore down the connection. In this case we disable that feature
         * flag. */
        if (rkb->rkb_state == RD_KAFKA_BROKER_STATE_APIVERSION_QUERY)
                rd_kafka_broker_feature_disable(rkb,
                                                RD_KAFKA_FEATURE_APIVERSION);

        /* Set broker state */
        old_state = rkb->rkb_state;
        rd_kafka_broker_set_state(rkb, RD_KAFKA_BROKER_STATE_DOWN);

        /* Stop any pending reauth timer, since a teardown/reconnect will
         * require a new timer. */
        rd_kafka_timer_stop(&rkb->rkb_rk->rk_timers, &rkb->rkb_sasl_reauth_tmr,
                            1 /*lock*/);

        /* Unlock broker since a requeue will try to lock it. */
        rd_kafka_broker_unlock(rkb);

        rd_atomic64_set(&rkb->rkb_c.ts_send, 0);
        rd_atomic64_set(&rkb->rkb_c.ts_recv, 0);

        /*
         * Purge all buffers
         * (put bufs on a temporary queue since bufs may be requeued,
         *  make sure outstanding requests are re-enqueued before
         *  bufs on outbufs queue.)
         */
        rd_kafka_bufq_init(&tmpq_waitresp);
        rd_kafka_bufq_init(&tmpq);
        rd_kafka_bufq_concat(&tmpq_waitresp, &rkb->rkb_waitresps);
        rd_kafka_bufq_concat(&tmpq, &rkb->rkb_outbufs);
        rd_atomic32_init(&rkb->rkb_blocking_request_cnt, 0);

        /* Purge the in-flight buffers (might get re-enqueued in case
         * of retries). */
        rd_kafka_bufq_purge(rkb, &tmpq_waitresp, err);

        /* Purge the waiting-in-output-queue buffers,
         * might also get re-enqueued. */
        rd_kafka_bufq_purge(rkb, &tmpq,
                            /* If failure was caused by a timeout,
                             * adjust the error code for in-queue requests. */
                            err == RD_KAFKA_RESP_ERR__TIMED_OUT
                                ? RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE
                                : err);

        /* Update bufq for connection reset:
         *  - Purge connection-setup requests from outbufs since they will be
         *    reissued on the next connect.
         *  - Reset any partially sent buffer's offset.
         */
        rd_kafka_bufq_connection_reset(rkb, &rkb->rkb_outbufs);

        /* Extra debugging for tracking termination-hang issues:
         * show what is keeping this broker from decommissioning. */
        if (rd_kafka_terminating(rkb->rkb_rk) &&
            !rd_kafka_broker_terminating(rkb)) {
                rd_rkb_dbg(rkb, BROKER | RD_KAFKA_DBG_PROTOCOL, "BRKTERM",
                           "terminating: broker still has %d refcnt(s), "
                           "%" PRId32 " buffer(s), %d partition(s)",
                           rd_refcnt_get(&rkb->rkb_refcnt),
                           rd_kafka_bufq_cnt(&rkb->rkb_outbufs),
                           rkb->rkb_toppar_cnt);
                rd_kafka_bufq_dump(rkb, "BRKOUTBUFS", &rkb->rkb_outbufs);
        }

        /* If this broker acts as the preferred (follower) replica for any
         * partition, delegate the partition back to the leader. */
        TAILQ_FOREACH(rktp, &rkb->rkb_toppars, rktp_rkblink) {
                rd_kafka_toppar_lock(rktp);
                if (unlikely(rktp->rktp_broker != rkb)) {
                        /* Currently migrating away from this
                         * broker, skip. */
                        rd_kafka_toppar_unlock(rktp);
                        continue;
                }
                rd_kafka_toppar_unlock(rktp);

                if (rktp->rktp_leader_id != rktp->rktp_broker_id) {
                        rd_kafka_toppar_delegate_to_leader(rktp);
                } else if (rd_kafka_broker_termination_in_progress(rkb)) {
                        /* Remove `rktp_broker` and `rktp_leader`
                         * references in `rktp`, even if this broker
                         * is still the leader, to allow to
                         * decommission it. */
                        rd_kafka_toppar_undelegate(rktp);
                        rd_kafka_toppar_forget_leader(rktp);
                }
        }

        /* If the broker is the preferred telemetry broker, remove it. */
        /* TODO(milind): check if this right. */
        mtx_lock(&rkb->rkb_rk->rk_telemetry.lock);
        if (rkb->rkb_rk->rk_telemetry.preferred_broker == rkb) {
                rd_kafka_dbg(rkb->rkb_rk, TELEMETRY, "TELBRKLOST",
                             "Lost telemetry broker %s due to state change",
                             rkb->rkb_name);
                rd_kafka_broker_destroy(
                    rkb->rkb_rk->rk_telemetry.preferred_broker);
                rkb->rkb_rk->rk_telemetry.preferred_broker = NULL;
        }
        mtx_unlock(&rkb->rkb_rk->rk_telemetry.lock);

        /* Query for topic leaders to quickly pick up on failover. */
        if (err != RD_KAFKA_RESP_ERR__DESTROY &&
            err != RD_KAFKA_RESP_ERR__DESTROY_BROKER &&
            old_state >= RD_KAFKA_BROKER_STATE_UP)
                rd_kafka_metadata_refresh_known_topics(
                    rkb->rkb_rk, NULL, rd_true /*force*/, "broker down");
}



/**
 * @brief Handle broker connection close.
 *
 * @locality broker thread
 */
void rd_kafka_broker_conn_closed(rd_kafka_broker_t *rkb,
                                 rd_kafka_resp_err_t err,
                                 const char *errstr) {
        int log_level = LOG_ERR;

        if (!rkb->rkb_rk->rk_conf.log_connection_close) {
                /* Silence all connection closes */
                log_level = LOG_DEBUG;

        } else {
                /* Silence close logs for connections that are idle,
                 * it is most likely the broker's idle connection
                 * reaper kicking in.
                 *
                 * Indications there might be an error and not an
                 * idle disconnect:
                 *  - If the connection age is low a disconnect
                 *    typically indicates a failure, such as protocol mismatch.
                 *  - If the connection hasn't been idle long enough.
                 *  - There are outstanding requests, or requests enqueued.
                 *
                 * For non-idle connections, adjust log level:
                 *  - requests in-flight: LOG_WARNING
                 *  - else: LOG_INFO
                 */
                rd_ts_t now = rd_clock();
                rd_ts_t minidle =
                    RD_MAX(60 * 1000 /*60s*/,
                           rkb->rkb_rk->rk_conf.socket_timeout_ms) *
                    1000;
                int inflight = rd_kafka_bufq_cnt(&rkb->rkb_waitresps);
                int inqueue  = rd_kafka_bufq_cnt(&rkb->rkb_outbufs);

                if (rkb->rkb_ts_state + minidle < now &&
                    rd_atomic64_get(&rkb->rkb_c.ts_send) + minidle < now &&
                    inflight + inqueue == 0)
                        log_level = LOG_DEBUG;
                else if (inflight > 1)
                        log_level = LOG_WARNING;
                else
                        log_level = LOG_INFO;
        }

        rd_kafka_broker_fail(rkb, log_level, err, "%s", errstr);
}


/**
 * @brief Purge requests in \p rkbq matching request \p ApiKey
 *        and partition \p rktp.
 *
 * @warning ApiKey must be RD_KAFKAP_Produce
 *
 * @returns the number of purged buffers.
 *
 * @locality broker thread
 */
static int rd_kafka_broker_bufq_purge_by_toppar(rd_kafka_broker_t *rkb,
                                                rd_kafka_bufq_t *rkbq,
                                                int64_t ApiKey,
                                                rd_kafka_toppar_t *rktp,
                                                rd_kafka_resp_err_t err) {
        rd_kafka_buf_t *rkbuf, *tmp;
        int cnt = 0;

        rd_assert(ApiKey == RD_KAFKAP_Produce);

        TAILQ_FOREACH_SAFE(rkbuf, &rkbq->rkbq_bufs, rkbuf_link, tmp) {

                if (rkbuf->rkbuf_reqhdr.ApiKey != ApiKey ||
                    rkbuf->rkbuf_u.Produce.batch.rktp != rktp ||
                    /* Skip partially sent buffers and let them transmit.
                     * The alternative would be to kill the connection here,
                     * which is more drastic and costly. */
                    rd_slice_offset(&rkbuf->rkbuf_reader) > 0)
                        continue;

                rd_kafka_bufq_deq(rkbq, rkbuf);

                rd_kafka_buf_callback(rkb->rkb_rk, rkb, err, NULL, rkbuf);
                cnt++;
        }

        return cnt;
}


/**
 * Scan bufq for buffer timeouts, trigger buffer callback on timeout.
 *
 * If \p partial_cntp is non-NULL any partially sent buffers will increase
 * the provided counter by 1.
 *
 * @param ApiKey Only match requests with this ApiKey, or -1 for all.
 * @param now If 0, all buffers will time out, else the current clock.
 * @param description "N requests timed out <description>", e.g., "in flight".
 *                    Only used if log_first_n > 0.
 * @param log_first_n Log the first N request timeouts.
 *
 * @returns the number of timed out buffers.
 *
 * @locality broker thread
 */
static int rd_kafka_broker_bufq_timeout_scan(rd_kafka_broker_t *rkb,
                                             int is_waitresp_q,
                                             rd_kafka_bufq_t *rkbq,
                                             int *partial_cntp,
                                             int16_t ApiKey,
                                             rd_kafka_resp_err_t err,
                                             rd_ts_t now,
                                             const char *description,
                                             int log_first_n) {
        rd_kafka_buf_t *rkbuf, *tmp;
        int cnt = 0;
        int idx = -1;
        const rd_kafka_buf_t *holb;

restart:
        holb = TAILQ_FIRST(&rkbq->rkbq_bufs);

        TAILQ_FOREACH_SAFE(rkbuf, &rkbq->rkbq_bufs, rkbuf_link, tmp) {
                rd_kafka_broker_state_t pre_state, post_state;

                idx++;

                if (likely(now && rkbuf->rkbuf_ts_timeout > now))
                        continue;

                if (ApiKey != -1 && rkbuf->rkbuf_reqhdr.ApiKey != ApiKey)
                        continue;

                if (partial_cntp && rd_slice_offset(&rkbuf->rkbuf_reader) > 0)
                        (*partial_cntp)++;

                /* Convert rkbuf_ts_sent to elapsed time since request */
                if (rkbuf->rkbuf_ts_sent)
                        rkbuf->rkbuf_ts_sent = now - rkbuf->rkbuf_ts_sent;
                else
                        rkbuf->rkbuf_ts_sent = now - rkbuf->rkbuf_ts_enq;

                rd_kafka_bufq_deq(rkbq, rkbuf);

                if (now && cnt < log_first_n) {
                        char holbstr[256];
                        /* Head of line blocking:
                         * If this is not the first request in queue, but the
                         * initial first request did not time out,
                         * it typically means the first request is a
                         * long-running blocking one, holding up the
                         * sub-sequent requests.
                         * In this case log what is likely holding up the
                         * requests and what caused this request to time out. */
                        if (holb && holb == TAILQ_FIRST(&rkbq->rkbq_bufs)) {
                                rd_snprintf(
                                    holbstr, sizeof(holbstr),
                                    ": possibly held back by "
                                    "preceeding%s %sRequest with "
                                    "timeout in %dms",
                                    (holb->rkbuf_flags & RD_KAFKA_OP_F_BLOCKING)
                                        ? " blocking"
                                        : "",
                                    rd_kafka_ApiKey2str(
                                        holb->rkbuf_reqhdr.ApiKey),
                                    (int)((holb->rkbuf_ts_timeout - now) /
                                          1000));
                                /* Only log the HOLB once */
                                holb = NULL;
                        } else {
                                *holbstr = '\0';
                        }

                        rd_rkb_log(
                            rkb, LOG_NOTICE, "REQTMOUT",
                            "Timed out %sRequest %s "
                            "(after %" PRId64 "ms, timeout #%d)%s",
                            rd_kafka_ApiKey2str(rkbuf->rkbuf_reqhdr.ApiKey),
                            description, rkbuf->rkbuf_ts_sent / 1000, cnt,
                            holbstr);
                }

                if (is_waitresp_q &&
                    rkbuf->rkbuf_flags & RD_KAFKA_OP_F_BLOCKING &&
                    rd_atomic32_sub(&rkb->rkb_blocking_request_cnt, 1) == 0)
                        rd_kafka_brokers_broadcast_state_change(rkb->rkb_rk);

                pre_state = rd_kafka_broker_get_state(rkb);

                rd_kafka_buf_callback(rkb->rkb_rk, rkb, err, NULL, rkbuf);
                cnt++;

                /* If the buf_callback() triggered a broker state change
                 * (typically through broker_fail()) we can't trust the
                 * queue we are scanning to not have been touched, so we
                 * either restart the scan or bail out (if broker is now down),
                 * depending on the new state. #2326 */
                post_state = rd_kafka_broker_get_state(rkb);
                if (pre_state != post_state) {
                        /* If the new state is DOWN it means broker_fail()
                         * was called which may have modified the queues,
                         * to keep things safe we stop scanning this queue. */
                        if (post_state == RD_KAFKA_BROKER_STATE_DOWN)
                                break;
                        /* Else start scanning the queue from the beginning. */
                        goto restart;
                }
        }

        return cnt;
}


/**
 * Scan the wait-response and outbuf queues for message timeouts.
 *
 * Locality: Broker thread
 */
static void rd_kafka_broker_timeout_scan(rd_kafka_broker_t *rkb, rd_ts_t now) {
        int inflight_cnt, retry_cnt, outq_cnt;
        int partial_cnt = 0;

        rd_kafka_assert(rkb->rkb_rk, thrd_is_current(rkb->rkb_thread));

        /* In-flight requests waiting for response */
        inflight_cnt = rd_kafka_broker_bufq_timeout_scan(
            rkb, 1, &rkb->rkb_waitresps, NULL, -1, RD_KAFKA_RESP_ERR__TIMED_OUT,
            now, "in flight", 5);
        /* Requests in retry queue */
        retry_cnt = rd_kafka_broker_bufq_timeout_scan(
            rkb, 0, &rkb->rkb_retrybufs, NULL, -1,
            RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE, now, "in retry queue", 0);
        /* Requests in local queue not sent yet.
         * partial_cnt is included in outq_cnt and denotes a request
         * that has been partially transmitted. */
        outq_cnt = rd_kafka_broker_bufq_timeout_scan(
            rkb, 0, &rkb->rkb_outbufs, &partial_cnt, -1,
            RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE, now, "in output queue", 0);

        if (inflight_cnt + retry_cnt + outq_cnt + partial_cnt > 0) {
                rd_rkb_log(rkb, LOG_WARNING, "REQTMOUT",
                           "Timed out %i in-flight, %i retry-queued, "
                           "%i out-queue, %i partially-sent requests",
                           inflight_cnt, retry_cnt, outq_cnt, partial_cnt);

                rkb->rkb_req_timeouts += inflight_cnt + outq_cnt;
                rd_atomic64_add(&rkb->rkb_c.req_timeouts,
                                inflight_cnt + outq_cnt);

                /* If this was a partially sent request that timed out, or the
                 * number of timed out requests have reached the
                 * socket.max.fails threshold, we need to take down the
                 * connection. */
                if (partial_cnt > 0 ||
                    (rkb->rkb_rk->rk_conf.socket_max_fails &&
                     rkb->rkb_req_timeouts >=
                         rkb->rkb_rk->rk_conf.socket_max_fails &&
                     rkb->rkb_state >= RD_KAFKA_BROKER_STATE_UP)) {
                        char rttinfo[32];
                        /* Print average RTT (if avail) to help diagnose. */
                        rd_avg_calc(&rkb->rkb_avg_rtt, now);
                        rd_avg_calc(
                            &rkb->rkb_telemetry.rd_avg_current.rkb_avg_rtt,
                            now);
                        if (rkb->rkb_avg_rtt.ra_v.avg)
                                rd_snprintf(rttinfo, sizeof(rttinfo),
                                            " (average rtt %.3fms)",
                                            (float)(rkb->rkb_avg_rtt.ra_v.avg /
                                                    1000.0f));
                        else if (rkb->rkb_telemetry.rd_avg_current.rkb_avg_rtt
                                     .ra_v.avg)
                                rd_snprintf(
                                    rttinfo, sizeof(rttinfo),
                                    " (average rtt %.3fms)",
                                    (float)(rkb->rkb_telemetry.rd_avg_current
                                                .rkb_avg_rtt.ra_v.avg /
                                            1000.0f));
                        else
                                rttinfo[0] = 0;
                        rd_kafka_broker_fail(rkb, LOG_ERR,
                                             RD_KAFKA_RESP_ERR__TIMED_OUT,
                                             "%i request(s) timed out: "
                                             "disconnect%s",
                                             rkb->rkb_req_timeouts, rttinfo);
                }
        }
}



static ssize_t rd_kafka_broker_send(rd_kafka_broker_t *rkb, rd_slice_t *slice) {
        ssize_t r;
        char errstr[128];

        rd_kafka_assert(rkb->rkb_rk,
                        rkb->rkb_state >= RD_KAFKA_BROKER_STATE_UP);
        rd_kafka_assert(rkb->rkb_rk, rkb->rkb_transport);

        r = rd_kafka_transport_send(rkb->rkb_transport, slice, errstr,
                                    sizeof(errstr));

        if (r == -1) {
                rd_kafka_broker_fail(rkb, LOG_ERR, RD_KAFKA_RESP_ERR__TRANSPORT,
                                     "Send failed: %s", errstr);
                rd_atomic64_add(&rkb->rkb_c.tx_err, 1);
                return -1;
        }

        rd_atomic64_add(&rkb->rkb_c.tx_bytes, r);
        rd_atomic64_add(&rkb->rkb_c.tx, 1);
        return r;
}



static int rd_kafka_broker_resolve(rd_kafka_broker_t *rkb,
                                   const char *nodename,
                                   rd_bool_t reset_cached_addr) {
        const char *errstr;
        int save_idx = 0;

        if (!*nodename && rkb->rkb_source == RD_KAFKA_LOGICAL) {
                rd_kafka_broker_fail(rkb, LOG_DEBUG, RD_KAFKA_RESP_ERR__RESOLVE,
                                     "Logical broker has no address yet");
                return -1;
        }

        if (rkb->rkb_rsal &&
            (reset_cached_addr ||
             rkb->rkb_ts_rsal_last +
                     (rkb->rkb_rk->rk_conf.broker_addr_ttl * 1000) <
                 rd_clock())) {
                /* Address list has expired. */

                /* Save the address index to make sure we still round-robin
                 * if we get the same address list back */
                save_idx = rkb->rkb_rsal->rsal_curr;

                rd_sockaddr_list_destroy(rkb->rkb_rsal);
                rkb->rkb_rsal = NULL;
        }

        if (!rkb->rkb_rsal) {
                /* Resolve */
                rkb->rkb_rsal = rd_getaddrinfo(
                    nodename, RD_KAFKA_PORT_STR, AI_ADDRCONFIG,
                    rkb->rkb_rk->rk_conf.broker_addr_family, SOCK_STREAM,
                    IPPROTO_TCP, rkb->rkb_rk->rk_conf.resolve_cb,
                    rkb->rkb_rk->rk_conf.opaque, &errstr);

                if (!rkb->rkb_rsal) {
                        rd_kafka_broker_fail(
                            rkb, LOG_ERR, RD_KAFKA_RESP_ERR__RESOLVE,
                            "Failed to resolve '%s': %s", nodename, errstr);
                        return -1;
                } else {
                        rkb->rkb_ts_rsal_last = rd_clock();
                        /* Continue at previous round-robin position */
                        if (rkb->rkb_rsal->rsal_cnt > save_idx)
                                rkb->rkb_rsal->rsal_curr = save_idx;
                }
        }

        return 0;
}


static void rd_kafka_broker_buf_enq0(rd_kafka_broker_t *rkb,
                                     rd_kafka_buf_t *rkbuf) {
        rd_ts_t now;

        rd_kafka_assert(rkb->rkb_rk, thrd_is_current(rkb->rkb_thread));

        if (rkb->rkb_rk->rk_conf.sparse_connections &&
            rkb->rkb_state == RD_KAFKA_BROKER_STATE_INIT) {
                /* Sparse connections:
                 * Trigger connection when a new request is enqueued. */
                rkb->rkb_persistconn.internal++;
                rd_kafka_broker_lock(rkb);
                rd_kafka_broker_set_state(rkb,
                                          RD_KAFKA_BROKER_STATE_TRY_CONNECT);
                rd_kafka_broker_unlock(rkb);
        }

        now                 = rd_clock();
        rkbuf->rkbuf_ts_enq = now;
        rkbuf->rkbuf_flags &= ~RD_KAFKA_OP_F_SENT;

        /* Calculate request attempt timeout */
        rd_kafka_buf_calc_timeout(rkb->rkb_rk, rkbuf, now);

        if (likely(rkbuf->rkbuf_prio == RD_KAFKA_PRIO_NORMAL)) {
                /* Insert request at tail of queue */
                TAILQ_INSERT_TAIL(&rkb->rkb_outbufs.rkbq_bufs, rkbuf,
                                  rkbuf_link);

        } else {
                /* Insert request after any requests with a higher or
                 * equal priority.
                 * Also make sure the request is after added any partially
                 * sent request (of any prio).
                 * We need to check if buf corrid is set rather than
                 * rkbuf_of since SSL_write may return 0 and expect the
                 * exact same arguments the next call. */
                rd_kafka_buf_t *prev, *after = NULL;

                TAILQ_FOREACH(prev, &rkb->rkb_outbufs.rkbq_bufs, rkbuf_link) {
                        if (prev->rkbuf_prio < rkbuf->rkbuf_prio &&
                            prev->rkbuf_corrid == 0)
                                break;
                        after = prev;
                }

                if (after)
                        TAILQ_INSERT_AFTER(&rkb->rkb_outbufs.rkbq_bufs, after,
                                           rkbuf, rkbuf_link);
                else
                        TAILQ_INSERT_HEAD(&rkb->rkb_outbufs.rkbq_bufs, rkbuf,
                                          rkbuf_link);
        }

        rd_atomic32_add(&rkb->rkb_outbufs.rkbq_cnt, 1);
        if (rkbuf->rkbuf_reqhdr.ApiKey == RD_KAFKAP_Produce)
                rd_atomic32_add(&rkb->rkb_outbufs.rkbq_msg_cnt,
                                rd_kafka_msgq_len(&rkbuf->rkbuf_batch.msgq));
}


/**
 * Finalize a stuffed rkbuf for sending to broker.
 */
static void rd_kafka_buf_finalize(rd_kafka_t *rk, rd_kafka_buf_t *rkbuf) {
        size_t totsize;

        rd_assert(!(rkbuf->rkbuf_flags & RD_KAFKA_OP_F_NEED_MAKE));

        if (rkbuf->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER) {
                /* Empty struct tags */
                rd_kafka_buf_write_i8(rkbuf, 0);
        }

        /* Calculate total request buffer length. */
        totsize = rd_buf_len(&rkbuf->rkbuf_buf) - 4;

        /* Set up a buffer reader for sending the buffer. */
        rd_slice_init_full(&rkbuf->rkbuf_reader, &rkbuf->rkbuf_buf);

        /**
         * Update request header fields
         */
        /* Total request length */
        rd_kafka_buf_update_i32(rkbuf, 0, (int32_t)totsize);

        /* ApiVersion */
        rd_kafka_buf_update_i16(rkbuf, 4 + 2, rkbuf->rkbuf_reqhdr.ApiVersion);
}


void rd_kafka_broker_buf_enq1(rd_kafka_broker_t *rkb,
                              rd_kafka_buf_t *rkbuf,
                              rd_kafka_resp_cb_t *resp_cb,
                              void *opaque) {


        rkbuf->rkbuf_cb     = resp_cb;
        rkbuf->rkbuf_opaque = opaque;

        rd_kafka_buf_finalize(rkb->rkb_rk, rkbuf);

        rd_kafka_broker_buf_enq0(rkb, rkbuf);
}


/**
 * Enqueue buffer on broker's xmit queue, but fail buffer immediately
 * if broker is not up.
 *
 * Locality: broker thread
 */
static int rd_kafka_broker_buf_enq2(rd_kafka_broker_t *rkb,
                                    rd_kafka_buf_t *rkbuf) {
        if (unlikely(rkb->rkb_source == RD_KAFKA_INTERNAL)) {
                /* Fail request immediately if this is the internal broker. */
                rd_kafka_buf_callback(rkb->rkb_rk, rkb,
                                      RD_KAFKA_RESP_ERR__TRANSPORT, NULL,
                                      rkbuf);
                return -1;
        }

        rd_kafka_broker_buf_enq0(rkb, rkbuf);

        return 0;
}



/**
 * Enqueue buffer for tranmission.
 * Responses are enqueued on 'replyq' (RD_KAFKA_OP_RECV_BUF)
 *
 * Locality: any thread
 */
void rd_kafka_broker_buf_enq_replyq(rd_kafka_broker_t *rkb,
                                    rd_kafka_buf_t *rkbuf,
                                    rd_kafka_replyq_t replyq,
                                    rd_kafka_resp_cb_t *resp_cb,
                                    void *opaque) {

        assert(rkbuf->rkbuf_rkb == rkb);
        if (resp_cb) {
                rkbuf->rkbuf_replyq = replyq;
                rkbuf->rkbuf_cb     = resp_cb;
                rkbuf->rkbuf_opaque = opaque;
        } else {
                rd_dassert(!replyq.q);
        }

        /* Unmaked buffers will be finalized after the make callback. */
        if (!(rkbuf->rkbuf_flags & RD_KAFKA_OP_F_NEED_MAKE))
                rd_kafka_buf_finalize(rkb->rkb_rk, rkbuf);

        if (thrd_is_current(rkb->rkb_thread)) {
                rd_kafka_broker_buf_enq2(rkb, rkbuf);

        } else {
                rd_kafka_op_t *rko    = rd_kafka_op_new(RD_KAFKA_OP_XMIT_BUF);
                rko->rko_u.xbuf.rkbuf = rkbuf;
                rd_kafka_q_enq(rkb->rkb_ops, rko);
        }
}



/**
 * @returns the current broker state change version.
 *          Pass this value to future rd_kafka_brokers_wait_state_change() calls
 *          to avoid the race condition where a state-change happens between
 *          an initial call to some API that fails and the sub-sequent
 *          .._wait_state_change() call.
 */
int rd_kafka_brokers_get_state_version(rd_kafka_t *rk) {
        int version;
        mtx_lock(&rk->rk_broker_state_change_lock);
        version = rk->rk_broker_state_change_version;
        mtx_unlock(&rk->rk_broker_state_change_lock);
        return version;
}

/**
 * @brief Wait at most \p timeout_ms for any state change for any broker.
 *        \p stored_version is the value previously returned by
 *        rd_kafka_brokers_get_state_version() prior to another API call
 *        that failed due to invalid state.
 *
 * Triggers:
 *   - broker state changes
 *   - broker transitioning from blocking to non-blocking
 *   - partition leader changes
 *   - group state changes
 *
 * @remark There is no guarantee that a state change actually took place.
 *
 * @returns 1 if a state change was signaled (maybe), else 0 (timeout)
 *
 * @locality any thread
 */
int rd_kafka_brokers_wait_state_change(rd_kafka_t *rk,
                                       int stored_version,
                                       int timeout_ms) {
        int r;
        mtx_lock(&rk->rk_broker_state_change_lock);
        if (stored_version != rk->rk_broker_state_change_version)
                r = 1;
        else
                r = cnd_timedwait_ms(&rk->rk_broker_state_change_cnd,
                                     &rk->rk_broker_state_change_lock,
                                     timeout_ms) == thrd_success;
        mtx_unlock(&rk->rk_broker_state_change_lock);
        return r;
}


/**
 * @brief Same as rd_kafka_brokers_wait_state_change() but will trigger
 *        the wakeup asynchronously through the provided \p eonce.
 *
 *        If the eonce was added to the wait list its reference count
 *        will have been updated, this reference is later removed by
 *        rd_kafka_broker_state_change_trigger_eonce() by calling trigger().
 *
 * @returns 1 if the \p eonce was added to the wait-broker-state-changes list,
 *          or 0 if the \p stored_version is outdated in which case the
 *          caller should redo the broker lookup.
 */
int rd_kafka_brokers_wait_state_change_async(rd_kafka_t *rk,
                                             int stored_version,
                                             rd_kafka_enq_once_t *eonce) {
        int r = 1;
        mtx_lock(&rk->rk_broker_state_change_lock);

        if (stored_version != rk->rk_broker_state_change_version)
                r = 0;
        else {
                rd_kafka_enq_once_add_source(eonce, "wait broker state change");
                rd_list_add(&rk->rk_broker_state_change_waiters, eonce);
        }

        mtx_unlock(&rk->rk_broker_state_change_lock);
        return r;
}


/**
 * @brief eonce trigger callback for rd_list_apply() call in
 *        rd_kafka_brokers_broadcast_state_change()
 */
static int rd_kafka_broker_state_change_trigger_eonce(void *elem,
                                                      void *opaque) {
        rd_kafka_enq_once_t *eonce = elem;
        rd_kafka_enq_once_trigger(eonce, RD_KAFKA_RESP_ERR_NO_ERROR,
                                  "broker state change");
        return 0; /* remove eonce from list */
}


/**
 * @brief Broadcast broker state change to listeners, if any.
 *
 * @locality any thread
 */
void rd_kafka_brokers_broadcast_state_change(rd_kafka_t *rk) {

        rd_kafka_dbg(rk, GENERIC, "BROADCAST", "Broadcasting state change");

        mtx_lock(&rk->rk_broker_state_change_lock);

        /* Bump version */
        rk->rk_broker_state_change_version++;

        /* Trigger waiters */
        rd_list_apply(&rk->rk_broker_state_change_waiters,
                      rd_kafka_broker_state_change_trigger_eonce, NULL);

        /* Broadcast to listeners */
        cnd_broadcast(&rk->rk_broker_state_change_cnd);

        mtx_unlock(&rk->rk_broker_state_change_lock);
}


/**
 * @returns a random broker (with refcnt increased) with matching \p state
 *          and where the \p filter function returns 0.
 *
 * Uses reservoir sampling.
 *
 * @param is_up Any broker that is up (UP or UPDATE state), \p state is ignored.
 * @param filtered_cnt Optional pointer to integer which will be set to the
 *               number of brokers that matches the \p state or \p is_up but
 *               were filtered out by \p filter.
 * @param filter is an optional callback used to filter out undesired brokers.
 *               The filter function should return 1 to filter out a broker,
 *               or 0 to keep it in the list of eligible brokers to return.
 *               rd_kafka_broker_lock() is held during the filter callback.
 *
 *
 * @locks rd_kafka_*lock() MUST be held
 * @locality any
 */
rd_kafka_broker_t *rd_kafka_broker_random0(const char *func,
                                           int line,
                                           rd_kafka_t *rk,
                                           rd_bool_t is_up,
                                           int state,
                                           int *filtered_cnt,
                                           int (*filter)(rd_kafka_broker_t *rk,
                                                         void *opaque),
                                           void *opaque) {
        rd_kafka_broker_t *rkb, *good = NULL;
        int cnt  = 0;
        int fcnt = 0;

        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                if (rd_kafka_broker_or_instance_terminating(rkb) ||
                    RD_KAFKA_BROKER_IS_LOGICAL(rkb))
                        continue;

                rd_kafka_broker_lock(rkb);
                if ((is_up && rd_kafka_broker_state_is_up(rkb->rkb_state)) ||
                    (!is_up && (state == -1 || (int)rkb->rkb_state == state))) {
                        if (filter && filter(rkb, opaque)) {
                                /* Filtered out */
                                fcnt++;
                        } else {
                                if (cnt < 1 || rd_jitter(0, cnt) < 1) {
                                        if (good)
                                                rd_kafka_broker_destroy(good);
                                        rd_kafka_broker_keep_fl(func, line,
                                                                rkb);
                                        good = rkb;
                                }
                                cnt += 1;
                        }
                }
                rd_kafka_broker_unlock(rkb);
        }

        if (filtered_cnt)
                *filtered_cnt = fcnt;

        return good;
}

/**
 * @returns the broker (with refcnt increased) with the highest weight based
 *          based on the provided weighing function.
 *
 * If multiple brokers share the same weight reservoir sampling will be used
 * to randomly select one.
 *
 * @param weight_cb Weighing function that should return the sort weight
 *                  for the given broker.
 *                  Higher weight is better.
 *                  A weight of <= 0 will filter out the broker.
 *                  The passed broker object is locked.
 * @param features (optional) Required broker features.
 *
 * @locks_required rk(read)
 * @locality any
 */
static rd_kafka_broker_t *
rd_kafka_broker_weighted(rd_kafka_t *rk,
                         int (*weight_cb)(rd_kafka_broker_t *rkb),
                         int features) {
        rd_kafka_broker_t *rkb, *good = NULL;
        int highest = 0;
        int cnt     = 0;

        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                int weight;
                if (rd_kafka_broker_or_instance_terminating(rkb))
                        continue;

                rd_kafka_broker_lock(rkb);
                if (features && (rkb->rkb_features & features) != features)
                        weight = 0;
                else
                        weight = weight_cb(rkb);
                rd_kafka_broker_unlock(rkb);

                if (weight <= 0 || weight < highest)
                        continue;

                if (weight > highest) {
                        highest = weight;
                        cnt     = 0;
                }

                /* If same weight (cnt > 0), use reservoir sampling */
                if (cnt < 1 || rd_jitter(0, cnt) < 1) {
                        if (good)
                                rd_kafka_broker_destroy(good);
                        rd_kafka_broker_keep(rkb);
                        good = rkb;
                }
                cnt++;
        }

        return good;
}

/**
 * @brief Weighing function to select a usable broker connections,
 *        promoting connections according to the scoring below.
 *
 * Priority order:
 *  - is not a bootstrap broker
 *  - least idle last 10 minutes (unless blocking)
 *  - least idle hours (if above 10 minutes idle)
 *  - is not a logical broker (these connections have dedicated use and should
 *                             preferably not be used for other purposes)
 *  - is not blocking
 *
 * Will prefer the most recently used broker connection for two reasons:
 * - this connection is most likely to function properly.
 * - allows truly idle connections to be killed by the broker's/LB's
 *   idle connection reaper.
 *
 * Connection must be up.
 *
 * @locks_required rkb
 */
static int rd_kafka_broker_weight_usable(rd_kafka_broker_t *rkb) {
        int weight = 0;

        if (!rd_kafka_broker_state_is_up(rkb->rkb_state) ||
            RD_KAFKA_BROKER_IS_LOGICAL(rkb))
                return 0;

        weight += 2000;

        if (likely(!rd_atomic32_get(&rkb->rkb_blocking_request_cnt))) {
                rd_ts_t tx_last = rd_atomic64_get(&rkb->rkb_c.ts_send);
                int idle        = (int)((rd_clock() -
                                  (tx_last > 0 ? tx_last : rkb->rkb_ts_state)) /
                                 1000000);

                weight += 1; /* is not blocking */
                if (rkb->rkb_source == RD_KAFKA_LEARNED)
                        /* Prefer learned brokers */
                        weight += 1000;

                /* Prefer least idle broker (based on last 10 minutes use) */
                if (idle < 0)
                        ; /*clock going backwards? do nothing */
                else if (idle < 600 /*10 minutes*/)
                        weight += 1000 + (600 - idle);
                else /* Else least idle hours (capped to 100h) */
                        weight += 100 + (100 - RD_MIN((idle / 3600), 100));
        }

        return weight;
}


/**
 * @brief Returns a random broker (with refcnt increased) in state \p state.
 *
 * Uses Reservoir sampling.
 *
 * @param filter is optional, see rd_kafka_broker_random().
 *
 * @sa rd_kafka_broker_random
 *
 * @locks rd_kafka_*lock(rk) MUST be held.
 * @locality any thread
 */
rd_kafka_broker_t *rd_kafka_broker_any(rd_kafka_t *rk,
                                       int state,
                                       int (*filter)(rd_kafka_broker_t *rkb,
                                                     void *opaque),
                                       void *opaque,
                                       const char *reason) {
        rd_kafka_broker_t *rkb;

        rkb = rd_kafka_broker_random(rk, state, filter, opaque);

        if (!rkb && rk->rk_conf.sparse_connections) {
                /* Sparse connections:
                 * If no eligible broker was found, schedule
                 * a random broker for connecting. */
                rd_kafka_connect_any(rk, reason);
        }

        return rkb;
}


/**
 * @brief Returns a random broker (with refcnt increased) which is up.
 *
 * @param filtered_cnt optional, see rd_kafka_broker_random0().
 * @param filter is optional, see rd_kafka_broker_random0().
 *
 * @sa rd_kafka_broker_random
 *
 * @locks rd_kafka_*lock(rk) MUST be held.
 * @locality any thread
 */
rd_kafka_broker_t *rd_kafka_broker_any_up(rd_kafka_t *rk,
                                          int *filtered_cnt,
                                          int (*filter)(rd_kafka_broker_t *rkb,
                                                        void *opaque),
                                          void *opaque,
                                          const char *reason) {
        rd_kafka_broker_t *rkb;

        rkb = rd_kafka_broker_random0(__FUNCTION__, __LINE__, rk,
                                      rd_true /*is_up*/, -1, filtered_cnt,
                                      filter, opaque);

        if (!rkb && rk->rk_conf.sparse_connections) {
                /* Sparse connections:
                 * If no eligible broker was found, schedule
                 * a random broker for connecting. */
                rd_kafka_connect_any(rk, reason);
        }

        return rkb;
}


/**
 * @brief Spend at most \p timeout_ms to acquire a usable (Up) broker.
 *
 * Prefers the most recently used broker, see rd_kafka_broker_weight_usable().
 *
 * @param features (optional) Required broker features.
 *
 * @returns A probably usable broker with increased refcount, or NULL on timeout
 * @locks rd_kafka_*lock() if !do_lock
 * @locality any
 *
 * @sa rd_kafka_broker_any_up()
 */
rd_kafka_broker_t *rd_kafka_broker_any_usable(rd_kafka_t *rk,
                                              int timeout_ms,
                                              rd_dolock_t do_lock,
                                              int features,
                                              const char *reason) {
        const rd_ts_t ts_end = rd_timeout_init(timeout_ms);

        while (1) {
                rd_kafka_broker_t *rkb;
                int remains;
                int version = rd_kafka_brokers_get_state_version(rk);

                if (do_lock)
                        rd_kafka_rdlock(rk);

                rkb = rd_kafka_broker_weighted(
                    rk, rd_kafka_broker_weight_usable, features);

                if (!rkb && rk->rk_conf.sparse_connections) {
                        /* Sparse connections:
                         * If no eligible broker was found, schedule
                         * a random broker for connecting. */
                        rd_kafka_connect_any(rk, reason);
                }

                if (do_lock)
                        rd_kafka_rdunlock(rk);

                if (rkb)
                        return rkb;

                remains = rd_timeout_remains(ts_end);
                if (rd_timeout_expired(remains))
                        return NULL;

                rd_kafka_brokers_wait_state_change(rk, version, remains);
        }

        return NULL;
}



/**
 * @returns the broker handle for \p broker_id using cached metadata
 *          information (if available) in state == \p state,
 *          with refcount increaesd.
 *
 *          Otherwise enqueues the \p eonce on the wait-state-change queue
 *          which will be triggered on broker state changes.
 *          It may also be triggered erroneously, so the caller
 *          should call rd_kafka_broker_get_async() again when
 *          the eonce is triggered.
 *
 * @locks none
 * @locality any thread
 */
rd_kafka_broker_t *rd_kafka_broker_get_async(rd_kafka_t *rk,
                                             int32_t broker_id,
                                             int state,
                                             rd_kafka_enq_once_t *eonce) {
        int version;
        do {
                rd_kafka_broker_t *rkb;

                version = rd_kafka_brokers_get_state_version(rk);

                rd_kafka_rdlock(rk);
                rkb = rd_kafka_broker_find_by_nodeid0(rk, broker_id, state,
                                                      rd_true);
                rd_kafka_rdunlock(rk);

                if (rkb)
                        return rkb;

        } while (!rd_kafka_brokers_wait_state_change_async(rk, version, eonce));

        return NULL; /* eonce added to wait list */
}


/**
 * @brief Asynchronously look up current list of broker ids until available.
 *        Bootstrap and logical brokers are excluded from the list.
 *
 *        To be called repeatedly with an valid eonce until a non-NULL
 *        list is returned.
 *
 * @param rk Client instance.
 * @param eonce For triggering asynchronously on state change
 *              in case broker list isn't yet available.
 * @return List of int32_t with broker nodeids when ready, NULL when the eonce
 *         was added to the wait list.
 */
rd_list_t *rd_kafka_brokers_get_nodeids_async(rd_kafka_t *rk,
                                              rd_kafka_enq_once_t *eonce) {
        rd_list_t *nodeids = NULL;
        int version, i, broker_cnt;

        do {
                rd_kafka_broker_t *rkb;
                version = rd_kafka_brokers_get_state_version(rk);

                rd_kafka_rdlock(rk);
                broker_cnt = rd_atomic32_get(&rk->rk_broker_cnt);
                if (nodeids) {
                        if (broker_cnt > rd_list_cnt(nodeids)) {
                                rd_list_destroy(nodeids);
                                /* Will be recreated just after */
                                nodeids = NULL;
                        } else {
                                rd_list_set_cnt(nodeids, 0);
                        }
                }
                if (!nodeids) {
                        nodeids = rd_list_new(0, NULL);
                        rd_list_init_int32(nodeids, broker_cnt);
                }
                i = 0;
                TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                        if (rd_kafka_broker_or_instance_terminating(rkb))
                                continue;

                        rd_kafka_broker_lock(rkb);
                        if (rkb->rkb_nodeid != -1 &&
                            !RD_KAFKA_BROKER_IS_LOGICAL(rkb)) {
                                rd_list_set_int32(nodeids, i++,
                                                  rkb->rkb_nodeid);
                        }
                        rd_kafka_broker_unlock(rkb);
                }
                rd_kafka_rdunlock(rk);

                if (!rd_list_empty(nodeids))
                        return nodeids;
        } while (!rd_kafka_brokers_wait_state_change_async(rk, version, eonce));

        if (nodeids) {
                rd_list_destroy(nodeids);
        }
        return NULL; /* eonce added to wait list */
}


/**
 * @returns the current controller using cached metadata information,
 *          and only if the broker's state == \p state.
 *          The reference count is increased for the returned broker.
 *
 * @locks none
 * @locality any thread
 */

static rd_kafka_broker_t *rd_kafka_broker_controller_nowait(rd_kafka_t *rk,
                                                            int state) {
        rd_kafka_broker_t *rkb;

        rd_kafka_rdlock(rk);

        if (rk->rk_controllerid == -1) {
                rd_kafka_rdunlock(rk);
                rd_kafka_metadata_refresh_brokers(rk, NULL,
                                                  "lookup controller");
                return NULL;
        }

        rkb = rd_kafka_broker_find_by_nodeid0(rk, rk->rk_controllerid, state,
                                              rd_true);

        rd_kafka_rdunlock(rk);

        return rkb;
}


/**
 * @returns the current controller using cached metadata information if
 *          available in state == \p state, with refcount increaesd.
 *
 *          Otherwise enqueues the \p eonce on the wait-controller queue
 *          which will be triggered on controller updates or broker state
 *          changes. It may also be triggered erroneously, so the caller
 *          should call rd_kafka_broker_controller_async() again when
 *          the eonce is triggered.
 *
 * @locks none
 * @locality any thread
 */
rd_kafka_broker_t *
rd_kafka_broker_controller_async(rd_kafka_t *rk,
                                 int state,
                                 rd_kafka_enq_once_t *eonce) {
        int version;
        do {
                rd_kafka_broker_t *rkb;

                version = rd_kafka_brokers_get_state_version(rk);

                rkb = rd_kafka_broker_controller_nowait(rk, state);
                if (rkb)
                        return rkb;

        } while (!rd_kafka_brokers_wait_state_change_async(rk, version, eonce));

        return NULL; /* eonce added to wait list */
}


/**
 * @returns the current controller using cached metadata information,
 *          blocking up to \p abs_timeout for the controller to be known
 *          and to reach state == \p state. The reference count is increased
 *          for the returned broker.
 *
 * @locks none
 * @locality any thread
 */
rd_kafka_broker_t *
rd_kafka_broker_controller(rd_kafka_t *rk, int state, rd_ts_t abs_timeout) {

        while (1) {
                int version = rd_kafka_brokers_get_state_version(rk);
                rd_kafka_broker_t *rkb;
                int remains_ms;

                rkb = rd_kafka_broker_controller_nowait(rk, state);
                if (rkb)
                        return rkb;

                remains_ms = rd_timeout_remains(abs_timeout);
                if (rd_timeout_expired(remains_ms))
                        return NULL;

                rd_kafka_brokers_wait_state_change(rk, version, remains_ms);
        }
}



/**
 * Find a waitresp (rkbuf awaiting response) by the correlation id.
 */
static rd_kafka_buf_t *rd_kafka_waitresp_find(rd_kafka_broker_t *rkb,
                                              int32_t corrid) {
        rd_kafka_buf_t *rkbuf;
        rd_ts_t now = rd_clock();

        rd_kafka_assert(rkb->rkb_rk, thrd_is_current(rkb->rkb_thread));

        TAILQ_FOREACH(rkbuf, &rkb->rkb_waitresps.rkbq_bufs, rkbuf_link)
        if (rkbuf->rkbuf_corrid == corrid) {
                /* Convert ts_sent to RTT */
                rkbuf->rkbuf_ts_sent = now - rkbuf->rkbuf_ts_sent;
                rd_avg_add(&rkb->rkb_avg_rtt, rkbuf->rkbuf_ts_sent);
                rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_rtt,
                           rkbuf->rkbuf_ts_sent);

                switch (rkbuf->rkbuf_reqhdr.ApiKey) {
                case RD_KAFKAP_Fetch:
                        if (rkb->rkb_rk->rk_type == RD_KAFKA_CONSUMER)
                                rd_avg_add(&rkb->rkb_telemetry.rd_avg_current
                                                .rkb_avg_fetch_latency,
                                           rkbuf->rkbuf_ts_sent);
                        break;
                case RD_KAFKAP_OffsetCommit:
                        if (rkb->rkb_rk->rk_type == RD_KAFKA_CONSUMER)
                                rd_avg_add(
                                    &rkb->rkb_rk->rk_telemetry.rd_avg_current
                                         .rk_avg_commit_latency,
                                    rkbuf->rkbuf_ts_sent);
                        break;
                case RD_KAFKAP_Produce:
                        if (rkb->rkb_rk->rk_type == RD_KAFKA_PRODUCER)
                                rd_avg_add(&rkb->rkb_telemetry.rd_avg_current
                                                .rkb_avg_produce_latency,
                                           rkbuf->rkbuf_ts_sent);
                        break;
                default:
                        break;
                }

                if (rkbuf->rkbuf_flags & RD_KAFKA_OP_F_BLOCKING &&
                    rd_atomic32_sub(&rkb->rkb_blocking_request_cnt, 1) == 1)
                        rd_kafka_brokers_broadcast_state_change(rkb->rkb_rk);

                rd_kafka_bufq_deq(&rkb->rkb_waitresps, rkbuf);
                return rkbuf;
        }
        return NULL;
}



/**
 * Map a response message to a request.
 */
static int rd_kafka_req_response(rd_kafka_broker_t *rkb,
                                 rd_kafka_buf_t *rkbuf) {
        rd_kafka_buf_t *req   = NULL;
        int log_decode_errors = LOG_ERR;

        rd_kafka_assert(rkb->rkb_rk, thrd_is_current(rkb->rkb_thread));


        /* Find corresponding request message by correlation id */
        if (unlikely(!(req = rd_kafka_waitresp_find(
                           rkb, rkbuf->rkbuf_reshdr.CorrId)))) {
                /* unknown response. probably due to request timeout */
                rd_atomic64_add(&rkb->rkb_c.rx_corrid_err, 1);
                rd_rkb_dbg(rkb, BROKER, "RESPONSE",
                           "Response for unknown CorrId %" PRId32
                           " (timed out?)",
                           rkbuf->rkbuf_reshdr.CorrId);
                rd_kafka_interceptors_on_response_received(
                    rkb->rkb_rk, -1, rd_kafka_broker_name(rkb), rkb->rkb_nodeid,
                    -1, -1, rkbuf->rkbuf_reshdr.CorrId, rkbuf->rkbuf_totlen, -1,
                    RD_KAFKA_RESP_ERR__NOENT);
                rd_kafka_buf_destroy(rkbuf);
                return -1;
        }

        rd_rkb_dbg(rkb, PROTOCOL, "RECV",
                   "Received %sResponse (v%hd, %" PRIusz
                   " bytes, CorrId %" PRId32 ", rtt %.2fms)",
                   rd_kafka_ApiKey2str(req->rkbuf_reqhdr.ApiKey),
                   req->rkbuf_reqhdr.ApiVersion, rkbuf->rkbuf_totlen,
                   rkbuf->rkbuf_reshdr.CorrId,
                   (float)req->rkbuf_ts_sent / 1000.0f);

        /* Copy request's header and certain flags to response object's
         * reqhdr for convenience. */
        rkbuf->rkbuf_reqhdr = req->rkbuf_reqhdr;
        rkbuf->rkbuf_flags |=
            (req->rkbuf_flags & RD_KAFKA_BUF_FLAGS_RESP_COPY_MASK);
        rkbuf->rkbuf_ts_sent = req->rkbuf_ts_sent; /* copy rtt */

        /* Set up response reader slice starting past the response header */
        rd_slice_init(&rkbuf->rkbuf_reader, &rkbuf->rkbuf_buf,
                      RD_KAFKAP_RESHDR_SIZE,
                      rd_buf_len(&rkbuf->rkbuf_buf) - RD_KAFKAP_RESHDR_SIZE);

        /* In case of flexibleVersion, skip the response header tags.
         * The ApiVersion request/response is different since it needs
         * be backwards compatible and thus has no header tags. */
        if (req->rkbuf_reqhdr.ApiKey != RD_KAFKAP_ApiVersion)
                rd_kafka_buf_skip_tags(rkbuf);

        if (!rkbuf->rkbuf_rkb) {
                rkbuf->rkbuf_rkb = rkb;
                rd_kafka_broker_keep(rkbuf->rkbuf_rkb);
        } else
                rd_assert(rkbuf->rkbuf_rkb == rkb);

        /* Call callback. */
        rd_kafka_buf_callback(rkb->rkb_rk, rkb, 0, rkbuf, req);

        return 0;

err_parse:
        rd_atomic64_add(&rkb->rkb_c.rx_err, 1);
        rd_kafka_buf_callback(rkb->rkb_rk, rkb, rkbuf->rkbuf_err, NULL, req);
        rd_kafka_buf_destroy(rkbuf);
        return -1;
}



int rd_kafka_recv(rd_kafka_broker_t *rkb) {
        rd_kafka_buf_t *rkbuf;
        ssize_t r;
        /* errstr is not set by buf_read errors, so default it here. */
        char errstr[512]            = "Protocol parse failure";
        rd_kafka_resp_err_t err     = RD_KAFKA_RESP_ERR_NO_ERROR;
        const int log_decode_errors = LOG_ERR;


        /* It is impossible to estimate the correct size of the response
         * so we split the read up in two parts: first we read the protocol
         * length and correlation id (i.e., the Response header), and then
         * when we know the full length of the response we allocate a new
         * buffer and call receive again.
         * All this in an async fashion (e.g., partial reads).
         */
        if (!(rkbuf = rkb->rkb_recv_buf)) {
                /* No receive in progress: create new buffer */

                rkbuf = rd_kafka_buf_new(2, RD_KAFKAP_RESHDR_SIZE);

                rkb->rkb_recv_buf = rkbuf;

                /* Set up buffer reader for the response header. */
                rd_buf_write_ensure(&rkbuf->rkbuf_buf, RD_KAFKAP_RESHDR_SIZE,
                                    RD_KAFKAP_RESHDR_SIZE);
        }

        rd_dassert(rd_buf_write_remains(&rkbuf->rkbuf_buf) > 0);

        r = rd_kafka_transport_recv(rkb->rkb_transport, &rkbuf->rkbuf_buf,
                                    errstr, sizeof(errstr));
        if (unlikely(r <= 0)) {
                if (r == 0)
                        return 0; /* EAGAIN */
                err = RD_KAFKA_RESP_ERR__TRANSPORT;
                rd_atomic64_add(&rkb->rkb_c.rx_err, 1);
                goto err;
        }

        rd_atomic64_set(&rkb->rkb_c.ts_recv, rd_clock());

        if (rkbuf->rkbuf_totlen == 0) {
                /* Packet length not known yet. */

                if (unlikely(rd_buf_write_pos(&rkbuf->rkbuf_buf) <
                             RD_KAFKAP_RESHDR_SIZE)) {
                        /* Need response header for packet length and corrid.
                         * Wait for more data. */
                        return 0;
                }

                rd_assert(!rkbuf->rkbuf_rkb);
                rkbuf->rkbuf_rkb = rkb; /* Protocol parsing code needs
                                         * the rkb for logging, but we dont
                                         * want to keep a reference to the
                                         * broker this early since that extra
                                         * refcount will mess with the broker's
                                         * refcount-based termination code. */

                /* Initialize reader */
                rd_slice_init(&rkbuf->rkbuf_reader, &rkbuf->rkbuf_buf, 0,
                              RD_KAFKAP_RESHDR_SIZE);

                /* Read protocol header */
                rd_kafka_buf_read_i32(rkbuf, &rkbuf->rkbuf_reshdr.Size);
                rd_kafka_buf_read_i32(rkbuf, &rkbuf->rkbuf_reshdr.CorrId);

                rkbuf->rkbuf_rkb = NULL; /* Reset */

                rkbuf->rkbuf_totlen = rkbuf->rkbuf_reshdr.Size;

                /* Make sure message size is within tolerable limits. */
                if (rkbuf->rkbuf_totlen < 4 /*CorrId*/ ||
                    rkbuf->rkbuf_totlen >
                        (size_t)rkb->rkb_rk->rk_conf.recv_max_msg_size) {
                        rd_snprintf(errstr, sizeof(errstr),
                                    "Invalid response size %" PRId32
                                    " (0..%i): "
                                    "increase receive.message.max.bytes",
                                    rkbuf->rkbuf_reshdr.Size,
                                    rkb->rkb_rk->rk_conf.recv_max_msg_size);
                        err = RD_KAFKA_RESP_ERR__BAD_MSG;
                        rd_atomic64_add(&rkb->rkb_c.rx_err, 1);
                        goto err;
                }

                rkbuf->rkbuf_totlen -= 4; /*CorrId*/

                if (rkbuf->rkbuf_totlen > 0) {
                        /* Allocate another buffer that fits all data (short of
                         * the common response header). We want all
                         * data to be in contigious memory. */

                        rd_buf_write_ensure_contig(&rkbuf->rkbuf_buf,
                                                   rkbuf->rkbuf_totlen);
                }
        }

        if (rd_buf_write_pos(&rkbuf->rkbuf_buf) - RD_KAFKAP_RESHDR_SIZE ==
            rkbuf->rkbuf_totlen) {
                /* Message is complete, pass it on to the original requester. */
                rkb->rkb_recv_buf = NULL;
                rd_atomic64_add(&rkb->rkb_c.rx, 1);
                rd_atomic64_add(&rkb->rkb_c.rx_bytes,
                                rd_buf_write_pos(&rkbuf->rkbuf_buf));
                rd_kafka_req_response(rkb, rkbuf);
        }

        return 1;

err_parse:
        err = rkbuf->rkbuf_err;
err:
        if (!strcmp(errstr, "Disconnected"))
                rd_kafka_broker_conn_closed(rkb, err, errstr);
        else
                rd_kafka_broker_fail(rkb, LOG_ERR, err, "Receive failed: %s",
                                     errstr);
        return -1;
}


/**
 * Linux version of socket_cb providing racefree CLOEXEC.
 */
int rd_kafka_socket_cb_linux(int domain, int type, int protocol, void *opaque) {
#ifdef SOCK_CLOEXEC
        return socket(domain, type | SOCK_CLOEXEC, protocol);
#else
        return rd_kafka_socket_cb_generic(domain, type, protocol, opaque);
#endif
}

/**
 * Fallback version of socket_cb NOT providing racefree CLOEXEC,
 * but setting CLOEXEC after socket creation (if FD_CLOEXEC is defined).
 */
int rd_kafka_socket_cb_generic(int domain,
                               int type,
                               int protocol,
                               void *opaque) {
        int s;
        int on = 1;
        s      = (int)socket(domain, type, protocol);
        if (s == -1)
                return -1;
#ifdef FD_CLOEXEC
        if (fcntl(s, F_SETFD, FD_CLOEXEC, &on) == -1)
                fprintf(stderr,
                        "WARNING: librdkafka: %s: "
                        "fcntl(FD_CLOEXEC) failed: %s: ignoring\n",
                        __FUNCTION__, rd_strerror(errno));
#endif
        return s;
}



/**
 * @brief Update the reconnect backoff.
 *        Should be called when a connection is made, or all addresses
 *        a broker resolves to has been exhausted without successful connect.
 *
 * @locality broker thread
 * @locks none
 */
static void
rd_kafka_broker_update_reconnect_backoff(rd_kafka_broker_t *rkb,
                                         const rd_kafka_conf_t *conf,
                                         rd_ts_t now) {
        int backoff;

        /* If last connection attempt was more than reconnect.backoff.max.ms
         * ago, reset the reconnect backoff to the initial
         * reconnect.backoff.ms value. */
        if (rkb->rkb_ts_reconnect + (conf->reconnect_backoff_max_ms * 1000) <
            now)
                rkb->rkb_reconnect_backoff_ms = conf->reconnect_backoff_ms;

        /* Apply -25%...+50% jitter to next backoff. */
        backoff = rd_jitter((int)((float)rkb->rkb_reconnect_backoff_ms * 0.75),
                            (int)((float)rkb->rkb_reconnect_backoff_ms * 1.5));

        /* Cap to reconnect.backoff.max.ms. */
        backoff = RD_MIN(backoff, conf->reconnect_backoff_max_ms);

        /* Set time of next reconnect */
        rkb->rkb_ts_reconnect         = now + (backoff * 1000);
        rkb->rkb_reconnect_backoff_ms = RD_MIN(
            rkb->rkb_reconnect_backoff_ms * 2, conf->reconnect_backoff_max_ms);
}


/**
 * @brief Calculate time until next reconnect attempt.
 *
 * @returns the number of milliseconds to the next connection attempt, or 0
 *          if immediate.
 * @locality broker thread
 * @locks none
 */

static RD_INLINE int
rd_kafka_broker_reconnect_backoff(const rd_kafka_broker_t *rkb, rd_ts_t now) {
        rd_ts_t remains;

        if (unlikely(rkb->rkb_ts_reconnect == 0))
                return 0; /* immediate */

        remains = rkb->rkb_ts_reconnect - now;
        if (remains <= 0)
                return 0; /* immediate */

        return (int)(remains / 1000);
}


/**
 * @brief Unittest for reconnect.backoff.ms
 */
static int rd_ut_reconnect_backoff(void) {
        rd_kafka_broker_t rkb = RD_ZERO_INIT;
        rd_kafka_conf_t conf  = {.reconnect_backoff_ms     = 10,
                                 .reconnect_backoff_max_ms = 90};
        rd_ts_t now           = 1000000;
        int backoff;

        rkb.rkb_reconnect_backoff_ms = conf.reconnect_backoff_ms;

        /* broker's backoff is the initial reconnect.backoff.ms=10 */
        rd_kafka_broker_update_reconnect_backoff(&rkb, &conf, now);
        backoff = rd_kafka_broker_reconnect_backoff(&rkb, now);
        RD_UT_ASSERT_RANGE(backoff, 7, 15, "%d");

        /* .. 20 */
        rd_kafka_broker_update_reconnect_backoff(&rkb, &conf, now);
        backoff = rd_kafka_broker_reconnect_backoff(&rkb, now);
        RD_UT_ASSERT_RANGE(backoff, 15, 30, "%d");

        /* .. 40 */
        rd_kafka_broker_update_reconnect_backoff(&rkb, &conf, now);
        backoff = rd_kafka_broker_reconnect_backoff(&rkb, now);
        RD_UT_ASSERT_RANGE(backoff, 30, 60, "%d");

        /* .. 80, the jitter is capped at reconnect.backoff.max.ms=90  */
        rd_kafka_broker_update_reconnect_backoff(&rkb, &conf, now);
        backoff = rd_kafka_broker_reconnect_backoff(&rkb, now);
        RD_UT_ASSERT_RANGE(backoff, 60, conf.reconnect_backoff_max_ms, "%d");

        /* .. 90, capped by reconnect.backoff.max.ms */
        rd_kafka_broker_update_reconnect_backoff(&rkb, &conf, now);
        backoff = rd_kafka_broker_reconnect_backoff(&rkb, now);
        RD_UT_ASSERT_RANGE(backoff, 67, conf.reconnect_backoff_max_ms, "%d");

        /* .. 90, should remain at capped value. */
        rd_kafka_broker_update_reconnect_backoff(&rkb, &conf, now);
        backoff = rd_kafka_broker_reconnect_backoff(&rkb, now);
        RD_UT_ASSERT_RANGE(backoff, 67, conf.reconnect_backoff_max_ms, "%d");

        RD_UT_PASS();
}


/**
 * @brief Initiate asynchronous connection attempt to the next address
 *        in the broker's address list.
 *        While the connect is asynchronous and its IO served in the
 *        CONNECT state, the initial name resolve is blocking.
 *
 * @returns -1 on error, 0 if broker does not have a hostname, or 1
 *          if the connection is now in progress.
 */
static int rd_kafka_broker_connect(rd_kafka_broker_t *rkb) {
        const rd_sockaddr_inx_t *sinx;
        char errstr[512];
        char nodename[RD_KAFKA_NODENAME_SIZE];
        rd_bool_t reset_cached_addr = rd_false;

        rd_rkb_dbg(rkb, BROKER, "CONNECT", "broker in state %s connecting",
                   rd_kafka_broker_state_names[rkb->rkb_state]);

        rd_atomic32_add(&rkb->rkb_c.connects, 1);

        rd_kafka_broker_lock(rkb);
        rd_strlcpy(nodename, rkb->rkb_nodename, sizeof(nodename));

        /* If the nodename was changed since the last connect,
         * reset the address cache. */
        reset_cached_addr = (rkb->rkb_connect_epoch != rkb->rkb_nodename_epoch);
        rkb->rkb_connect_epoch = rkb->rkb_nodename_epoch;
        /* Logical brokers might not have a hostname set, in which case
         * we should not try to connect. */
        if (*nodename)
                rd_kafka_broker_set_state(rkb, RD_KAFKA_BROKER_STATE_CONNECT);
        rd_kafka_broker_unlock(rkb);

        if (!*nodename) {
                rd_rkb_dbg(rkb, BROKER, "CONNECT",
                           "broker has no address yet: postponing connect");
                return 0;
        }

        rd_kafka_broker_update_reconnect_backoff(rkb, &rkb->rkb_rk->rk_conf,
                                                 rd_clock());

        if (rd_kafka_broker_resolve(rkb, nodename, reset_cached_addr) == -1)
                return -1;

        sinx = rd_sockaddr_list_next(rkb->rkb_rsal);

        rd_kafka_assert(rkb->rkb_rk, !rkb->rkb_transport);

        if (!(rkb->rkb_transport = rd_kafka_transport_connect(
                  rkb, sinx, errstr, sizeof(errstr)))) {
                rd_kafka_broker_fail(rkb, LOG_ERR, RD_KAFKA_RESP_ERR__TRANSPORT,
                                     "%s", errstr);
                return -1;
        }

        rkb->rkb_ts_connect = rd_clock();

        return 1;
}


/**
 * @brief Call when connection is ready to transition to fully functional
 *        UP state.
 *
 * @locality Broker thread
 */
void rd_kafka_broker_connect_up(rd_kafka_broker_t *rkb) {
        int features;

        rkb->rkb_max_inflight       = rkb->rkb_rk->rk_conf.max_inflight;
        rkb->rkb_reauth_in_progress = rd_false;

        rd_kafka_broker_lock(rkb);
        rd_kafka_broker_set_state(rkb, RD_KAFKA_BROKER_STATE_UP);
        rd_kafka_broker_unlock(rkb);

        /* Request metadata (async):
         * try locally known topics first and if there are none try
         * getting just the broker list. */
        if (rd_kafka_metadata_refresh_known_topics(
                NULL, rkb, rd_false /*dont force*/, "connected") ==
            RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC)
                rd_kafka_metadata_refresh_brokers(NULL, rkb, "connected");

        if (rd_kafka_broker_ApiVersion_supported(
                rkb, RD_KAFKAP_GetTelemetrySubscriptions, 0, 0, &features) !=
                -1 &&
            rkb->rkb_source == RD_KAFKA_LEARNED &&
            rkb->rkb_rk->rk_conf.enable_metrics_push) {
                rd_kafka_t *rk = rkb->rkb_rk;
                rd_kafka_op_t *rko =
                    rd_kafka_op_new(RD_KAFKA_OP_SET_TELEMETRY_BROKER);
                rd_kafka_broker_keep(rkb);
                rko->rko_u.telemetry_broker.rkb = rkb;
                rd_kafka_q_enq(rk->rk_ops, rko);
        }
}



static void rd_kafka_broker_connect_auth(rd_kafka_broker_t *rkb);


/**
 * @brief Parses and handles SaslMechanism response, transitions
 *        the broker state.
 *
 */
static void rd_kafka_broker_handle_SaslHandshake(rd_kafka_t *rk,
                                                 rd_kafka_broker_t *rkb,
                                                 rd_kafka_resp_err_t err,
                                                 rd_kafka_buf_t *rkbuf,
                                                 rd_kafka_buf_t *request,
                                                 void *opaque) {
        const int log_decode_errors = LOG_ERR;
        int32_t MechCnt;
        int16_t ErrorCode;
        int i       = 0;
        char *mechs = "(n/a)";
        size_t msz, mof = 0;

        if (rd_kafka_broker_is_any_err_destroy(err))
                return;

        if (err)
                goto err;

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);
        rd_kafka_buf_read_i32(rkbuf, &MechCnt);

        if (MechCnt < 0 || MechCnt > 100)
                rd_kafka_buf_parse_fail(
                    rkbuf, "Invalid MechanismCount %" PRId32, MechCnt);

        /* Build a CSV string of supported mechanisms. */
        msz    = RD_MIN(511, 1 + (MechCnt * 32));
        mechs  = rd_alloca(msz);
        *mechs = '\0';

        for (i = 0; i < MechCnt; i++) {
                rd_kafkap_str_t mech;
                rd_kafka_buf_read_str(rkbuf, &mech);

                mof += rd_snprintf(mechs + mof, msz - mof, "%s%.*s",
                                   i ? "," : "", RD_KAFKAP_STR_PR(&mech));

                if (mof >= msz)
                        break;
        }

        rd_rkb_dbg(rkb, PROTOCOL | RD_KAFKA_DBG_SECURITY | RD_KAFKA_DBG_BROKER,
                   "SASLMECHS", "Broker supported SASL mechanisms: %s", mechs);

        if (ErrorCode) {
                err = ErrorCode;
                goto err;
        }

        /* Circle back to connect_auth() to start proper AUTH state. */
        rd_kafka_broker_connect_auth(rkb);
        return;

err_parse:
        err = rkbuf->rkbuf_err;
err:
        rd_kafka_broker_fail(rkb, LOG_ERR, RD_KAFKA_RESP_ERR__AUTHENTICATION,
                             "SASL %s mechanism handshake failed: %s: "
                             "broker's supported mechanisms: %s",
                             rkb->rkb_rk->rk_conf.sasl.mechanisms,
                             rd_kafka_err2str(err), mechs);
}


/**
 * @brief Transition state to:
 *        - AUTH_HANDSHAKE (if SASL is configured and handshakes supported)
 *        - AUTH (if SASL is configured but no handshake is required or
 *                not supported, or has already taken place.)
 *        - UP (if SASL is not configured)
 *
 * @locks_acquired rkb
 */
static void rd_kafka_broker_connect_auth(rd_kafka_broker_t *rkb) {

        if ((rkb->rkb_proto == RD_KAFKA_PROTO_SASL_PLAINTEXT ||
             rkb->rkb_proto == RD_KAFKA_PROTO_SASL_SSL)) {

                rd_rkb_dbg(rkb, SECURITY | RD_KAFKA_DBG_BROKER, "AUTH",
                           "Auth in state %s (handshake %ssupported)",
                           rd_kafka_broker_state_names[rkb->rkb_state],
                           (rkb->rkb_features & RD_KAFKA_FEATURE_SASL_HANDSHAKE)
                               ? ""
                               : "not ");

                /* Broker >= 0.10.0: send request to select mechanism */
                if (rkb->rkb_state != RD_KAFKA_BROKER_STATE_AUTH_HANDSHAKE &&
                    (rkb->rkb_features & RD_KAFKA_FEATURE_SASL_HANDSHAKE)) {

                        rd_kafka_broker_lock(rkb);
                        rd_kafka_broker_set_state(
                            rkb, RD_KAFKA_BROKER_STATE_AUTH_HANDSHAKE);
                        rd_kafka_broker_unlock(rkb);

                        rd_kafka_SaslHandshakeRequest(
                            rkb, rkb->rkb_rk->rk_conf.sasl.mechanisms,
                            RD_KAFKA_NO_REPLYQ,
                            rd_kafka_broker_handle_SaslHandshake, NULL);
                } else {
                        /* Either Handshake succeeded (protocol selected)
                         * or Handshakes were not supported.
                         * In both cases continue with authentication. */
                        char sasl_errstr[512];

                        rd_kafka_broker_lock(rkb);
                        rd_kafka_broker_set_state(
                            rkb,
                            (rkb->rkb_features & RD_KAFKA_FEATURE_SASL_AUTH_REQ)
                                ? RD_KAFKA_BROKER_STATE_AUTH_REQ
                                : RD_KAFKA_BROKER_STATE_AUTH_LEGACY);
                        rd_kafka_broker_unlock(rkb);

                        if (rd_kafka_sasl_client_new(
                                rkb->rkb_transport, sasl_errstr,
                                sizeof(sasl_errstr)) == -1) {
                                rd_kafka_broker_fail(
                                    rkb, LOG_ERR,
                                    RD_KAFKA_RESP_ERR__AUTHENTICATION,
                                    "Failed to initialize "
                                    "SASL authentication: %s",
                                    sasl_errstr);
                                return;
                        }
                }

                return;
        }

        /* No authentication required. */
        rd_kafka_broker_connect_up(rkb);
}


/**
 * @brief Specify API versions to use for this connection.
 *
 * @param apis is an allocated list of supported partitions.
 *        If NULL the default set will be used based on the
 *        \p broker.version.fallback property.
 * @param api_cnt number of elements in \p apis
 *
 * @remark \p rkb takes ownership of \p apis.
 *
 * @locality Broker thread
 * @locks_required rkb
 */
static void rd_kafka_broker_set_api_versions(rd_kafka_broker_t *rkb,
                                             struct rd_kafka_ApiVersion *apis,
                                             size_t api_cnt) {

        if (rkb->rkb_ApiVersions)
                rd_free(rkb->rkb_ApiVersions);


        if (!apis) {
                rd_rkb_dbg(
                    rkb, PROTOCOL | RD_KAFKA_DBG_BROKER, "APIVERSION",
                    "Using (configuration fallback) %s protocol features",
                    rkb->rkb_rk->rk_conf.broker_version_fallback);


                rd_kafka_get_legacy_ApiVersions(
                    rkb->rkb_rk->rk_conf.broker_version_fallback, &apis,
                    &api_cnt, rkb->rkb_rk->rk_conf.broker_version_fallback);

                /* Make a copy to store on broker. */
                rd_kafka_ApiVersions_copy(apis, api_cnt, &apis, &api_cnt);
        }

        rkb->rkb_ApiVersions     = apis;
        rkb->rkb_ApiVersions_cnt = api_cnt;

        /* Update feature set based on supported broker APIs. */
        rd_kafka_broker_features_set(
            rkb, rd_kafka_features_check(rkb, apis, api_cnt));
}


/**
 * Handler for ApiVersion response.
 */
static void rd_kafka_broker_handle_ApiVersion(rd_kafka_t *rk,
                                              rd_kafka_broker_t *rkb,
                                              rd_kafka_resp_err_t err,
                                              rd_kafka_buf_t *rkbuf,
                                              rd_kafka_buf_t *request,
                                              void *opaque) {
        struct rd_kafka_ApiVersion *apis = NULL;
        size_t api_cnt                   = 0;
        int16_t retry_ApiVersion         = -1;

        if (rd_kafka_broker_is_any_err_destroy(err))
                return;

        err = rd_kafka_handle_ApiVersion(rk, rkb, err, rkbuf, request, &apis,
                                         &api_cnt);

        /* Broker does not support our ApiVersionRequest version,
         * see if we can downgrade to an older version. */
        if (err == RD_KAFKA_RESP_ERR_UNSUPPORTED_VERSION) {
                size_t i;

                /* Find the broker's highest supported version for
                 * ApiVersionRequest and use that to retry. */
                for (i = 0; i < api_cnt; i++) {
                        if (apis[i].ApiKey == RD_KAFKAP_ApiVersion) {
                                retry_ApiVersion =
                                    RD_MIN(request->rkbuf_reqhdr.ApiVersion - 1,
                                           apis[i].MaxVer);
                                break;
                        }
                }

                /* Before v3 the broker would not return its supported
                 * ApiVersionRequests, so we go straight for version 0. */
                if (i == api_cnt && request->rkbuf_reqhdr.ApiVersion > 0)
                        retry_ApiVersion = 0;

        } else if (err == RD_KAFKA_RESP_ERR_INVALID_REQUEST) {
                rd_rkb_log(rkb, LOG_ERR, "APIVERSION",
                           "ApiVersionRequest v%hd failed due to "
                           "invalid request: "
                           "check client.software.name (\"%s\") and "
                           "client.software.version (\"%s\") "
                           "for invalid characters: "
                           "falling back to older request version",
                           request->rkbuf_reqhdr.ApiVersion,
                           rk->rk_conf.sw_name, rk->rk_conf.sw_version);
                retry_ApiVersion = 0;
        }

        if (err && apis)
                rd_free(apis);

        if (retry_ApiVersion != -1) {
                /* Retry request with a lower version */
                rd_rkb_dbg(
                    rkb, BROKER | RD_KAFKA_DBG_FEATURE | RD_KAFKA_DBG_PROTOCOL,
                    "APIVERSION",
                    "ApiVersionRequest v%hd failed due to %s: "
                    "retrying with v%hd",
                    request->rkbuf_reqhdr.ApiVersion, rd_kafka_err2name(err),
                    retry_ApiVersion);
                rd_kafka_ApiVersionRequest(
                    rkb, retry_ApiVersion, RD_KAFKA_NO_REPLYQ,
                    rd_kafka_broker_handle_ApiVersion, NULL);
                return;
        }


        if (err) {
                if (rkb->rkb_transport)
                        rd_kafka_broker_fail(
                            rkb, LOG_WARNING, RD_KAFKA_RESP_ERR__TRANSPORT,
                            "ApiVersionRequest failed: %s: "
                            "probably due to broker version < 0.10 "
                            "(see api.version.request configuration)",
                            rd_kafka_err2str(err));
                return;
        }

        rd_kafka_broker_lock(rkb);
        rd_kafka_broker_set_api_versions(rkb, apis, api_cnt);
        rd_kafka_broker_unlock(rkb);

        rd_kafka_broker_connect_auth(rkb);
}


/**
 * Call when asynchronous connection attempt completes, either succesfully
 * (if errstr is NULL) or fails.
 *
 * @locks_acquired rkb
 * @locality broker thread
 */
void rd_kafka_broker_connect_done(rd_kafka_broker_t *rkb, const char *errstr) {

        if (errstr) {
                /* Connect failed */
                rd_kafka_broker_fail(rkb, LOG_ERR, RD_KAFKA_RESP_ERR__TRANSPORT,
                                     "%s", errstr);
                return;
        }

        /* Connect succeeded */
        rkb->rkb_connid++;
        rd_rkb_dbg(rkb, BROKER | RD_KAFKA_DBG_PROTOCOL, "CONNECTED",
                   "Connected (#%d)", rkb->rkb_connid);
        rkb->rkb_max_inflight = 1; /* Hold back other requests until
                                    * ApiVersion, SaslHandshake, etc
                                    * are done. */

        rd_kafka_transport_poll_set(rkb->rkb_transport, POLLIN);

        rd_kafka_broker_lock(rkb);

        if (rkb->rkb_rk->rk_conf.api_version_request &&
            rd_interval_immediate(&rkb->rkb_ApiVersion_fail_intvl, 0, 0) > 0) {
                /* Use ApiVersion to query broker for supported API versions. */
                rd_kafka_broker_feature_enable(rkb,
                                               RD_KAFKA_FEATURE_APIVERSION);
        }

        if (!(rkb->rkb_features & RD_KAFKA_FEATURE_APIVERSION)) {
                /* Use configured broker.version.fallback to
                 * figure out API versions.
                 * In case broker.version.fallback indicates a version
                 * that supports ApiVersionRequest it will update
                 * rkb_features to have FEATURE_APIVERSION set which will
                 * trigger an ApiVersionRequest below. */
                rd_kafka_broker_set_api_versions(rkb, NULL, 0);
        }

        if (rkb->rkb_features & RD_KAFKA_FEATURE_APIVERSION) {
                /* Query broker for supported API versions.
                 * This may fail with a disconnect on non-supporting brokers
                 * so hold off any other requests until we get a response,
                 * and if the connection is torn down we disable this feature.
                 */
                rd_kafka_broker_set_state(
                    rkb, RD_KAFKA_BROKER_STATE_APIVERSION_QUERY);
                rd_kafka_broker_unlock(rkb);

                rd_kafka_ApiVersionRequest(
                    rkb, -1 /* Use highest version we support */,
                    RD_KAFKA_NO_REPLYQ, rd_kafka_broker_handle_ApiVersion,
                    NULL);
        } else {
                rd_kafka_broker_unlock(rkb);

                /* Authenticate if necessary */
                rd_kafka_broker_connect_auth(rkb);
        }
}



/**
 * @brief Checks if the given API request+version is supported by the broker.
 * @returns 1 if supported, else 0.
 * @locality broker thread
 * @locks none
 */
static RD_INLINE int rd_kafka_broker_request_supported(rd_kafka_broker_t *rkb,
                                                       rd_kafka_buf_t *rkbuf) {
        struct rd_kafka_ApiVersion skel = {.ApiKey =
                                               rkbuf->rkbuf_reqhdr.ApiKey};
        struct rd_kafka_ApiVersion *ret;

        if (unlikely(rkbuf->rkbuf_reqhdr.ApiKey == RD_KAFKAP_ApiVersion))
                return 1; /* ApiVersion requests are used to detect
                           * the supported API versions, so should always
                           * be allowed through. */

        /* First try feature flags, if any, which may cover a larger
         * set of APIs. */
        if (rkbuf->rkbuf_features)
                return (rkb->rkb_features & rkbuf->rkbuf_features) ==
                       rkbuf->rkbuf_features;

        /* Then try the ApiVersion map. */
        ret =
            bsearch(&skel, rkb->rkb_ApiVersions, rkb->rkb_ApiVersions_cnt,
                    sizeof(*rkb->rkb_ApiVersions), rd_kafka_ApiVersion_key_cmp);
        if (!ret)
                return 0;

        return ret->MinVer <= rkbuf->rkbuf_reqhdr.ApiVersion &&
               rkbuf->rkbuf_reqhdr.ApiVersion <= ret->MaxVer;
}


/**
 * Send queued messages to broker
 *
 * Locality: io thread
 */
int rd_kafka_send(rd_kafka_broker_t *rkb) {
        rd_kafka_buf_t *rkbuf;
        unsigned int cnt = 0;

        rd_kafka_assert(rkb->rkb_rk, thrd_is_current(rkb->rkb_thread));

        while (rkb->rkb_state >= RD_KAFKA_BROKER_STATE_UP &&
               rd_kafka_bufq_cnt(&rkb->rkb_waitresps) < rkb->rkb_max_inflight &&
               (rkbuf = TAILQ_FIRST(&rkb->rkb_outbufs.rkbq_bufs))) {
                ssize_t r;
                size_t pre_of = rd_slice_offset(&rkbuf->rkbuf_reader);
                rd_ts_t now;

                if (unlikely(rkbuf->rkbuf_flags & RD_KAFKA_OP_F_NEED_MAKE)) {
                        /* Request has not been created/baked yet,
                         * call its make callback. */
                        rd_kafka_resp_err_t err;

                        err = rkbuf->rkbuf_make_req_cb(
                            rkb, rkbuf, rkbuf->rkbuf_make_opaque);

                        rkbuf->rkbuf_flags &= ~RD_KAFKA_OP_F_NEED_MAKE;

                        /* Free the make_opaque */
                        if (rkbuf->rkbuf_free_make_opaque_cb &&
                            rkbuf->rkbuf_make_opaque) {
                                rkbuf->rkbuf_free_make_opaque_cb(
                                    rkbuf->rkbuf_make_opaque);
                                rkbuf->rkbuf_make_opaque = NULL;
                        }

                        if (unlikely(err)) {
                                rd_kafka_bufq_deq(&rkb->rkb_outbufs, rkbuf);
                                rd_rkb_dbg(rkb, BROKER | RD_KAFKA_DBG_PROTOCOL,
                                           "MAKEREQ",
                                           "Failed to make %sRequest: %s",
                                           rd_kafka_ApiKey2str(
                                               rkbuf->rkbuf_reqhdr.ApiKey),
                                           rd_kafka_err2str(err));
                                rd_kafka_buf_callback(rkb->rkb_rk, rkb, err,
                                                      NULL, rkbuf);
                                continue;
                        }

                        rd_kafka_buf_finalize(rkb->rkb_rk, rkbuf);
                }

                /* Check for broker support */
                if (unlikely(!rd_kafka_broker_request_supported(rkb, rkbuf))) {
                        rd_kafka_bufq_deq(&rkb->rkb_outbufs, rkbuf);
                        rd_rkb_dbg(
                            rkb, BROKER | RD_KAFKA_DBG_PROTOCOL, "UNSUPPORTED",
                            "Failing %sResponse "
                            "(v%hd, %" PRIusz " bytes, CorrId %" PRId32
                            "): "
                            "request not supported by broker "
                            "(missing api.version.request=false or "
                            "incorrect broker.version.fallback config?)",
                            rd_kafka_ApiKey2str(rkbuf->rkbuf_reqhdr.ApiKey),
                            rkbuf->rkbuf_reqhdr.ApiVersion, rkbuf->rkbuf_totlen,
                            rkbuf->rkbuf_reshdr.CorrId);
                        rd_kafka_buf_callback(
                            rkb->rkb_rk, rkb,
                            RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE, NULL,
                            rkbuf);
                        continue;
                }

                /* Set CorrId header field, unless this is the latter part
                 * of a partial send in which case the corrid has already
                 * been set.
                 * Due to how SSL_write() will accept a buffer but still
                 * return 0 in some cases we can't rely on the buffer offset
                 * but need to use corrid to check this. SSL_write() expects
                 * us to send the same buffer again when 0 is returned.
                 */
                if (rkbuf->rkbuf_corrid == 0 ||
                    rkbuf->rkbuf_connid != rkb->rkb_connid) {
                        rd_assert(rd_slice_offset(&rkbuf->rkbuf_reader) == 0);
                        rkbuf->rkbuf_corrid = ++rkb->rkb_corrid;
                        rd_kafka_buf_update_i32(rkbuf, 4 + 2 + 2,
                                                rkbuf->rkbuf_corrid);
                        rkbuf->rkbuf_connid = rkb->rkb_connid;
                } else if (pre_of > RD_KAFKAP_REQHDR_SIZE) {
                        rd_kafka_assert(NULL,
                                        rkbuf->rkbuf_connid == rkb->rkb_connid);
                }

                if (0) {
                        rd_rkb_dbg(
                            rkb, PROTOCOL, "SEND",
                            "Send %s corrid %" PRId32
                            " at "
                            "offset %" PRIusz "/%" PRIusz,
                            rd_kafka_ApiKey2str(rkbuf->rkbuf_reqhdr.ApiKey),
                            rkbuf->rkbuf_corrid, pre_of,
                            rd_slice_size(&rkbuf->rkbuf_reader));
                }

                if ((r = rd_kafka_broker_send(rkb, &rkbuf->rkbuf_reader)) == -1)
                        return -1;

                now = rd_clock();
                rd_atomic64_set(&rkb->rkb_c.ts_send, now);

                /* Partial send? Continue next time. */
                if (rd_slice_remains(&rkbuf->rkbuf_reader) > 0) {
                        rd_rkb_dbg(
                            rkb, PROTOCOL, "SEND",
                            "Sent partial %sRequest "
                            "(v%hd, "
                            "%" PRIdsz "+%" PRIdsz "/%" PRIusz
                            " bytes, "
                            "CorrId %" PRId32 ")",
                            rd_kafka_ApiKey2str(rkbuf->rkbuf_reqhdr.ApiKey),
                            rkbuf->rkbuf_reqhdr.ApiVersion, (ssize_t)pre_of, r,
                            rd_slice_size(&rkbuf->rkbuf_reader),
                            rkbuf->rkbuf_corrid);
                        return 0;
                }

                rd_rkb_dbg(rkb, PROTOCOL, "SEND",
                           "Sent %sRequest (v%hd, %" PRIusz " bytes @ %" PRIusz
                           ", "
                           "CorrId %" PRId32 ")",
                           rd_kafka_ApiKey2str(rkbuf->rkbuf_reqhdr.ApiKey),
                           rkbuf->rkbuf_reqhdr.ApiVersion,
                           rd_slice_size(&rkbuf->rkbuf_reader), pre_of,
                           rkbuf->rkbuf_corrid);

                rd_atomic64_add(&rkb->rkb_c.reqtype[rkbuf->rkbuf_reqhdr.ApiKey],
                                1);

                /* Notify transport layer of full request sent */
                if (likely(rkb->rkb_transport != NULL))
                        rd_kafka_transport_request_sent(rkb, rkbuf);

                /* Entire buffer sent, unlink from outbuf */
                rd_kafka_bufq_deq(&rkb->rkb_outbufs, rkbuf);
                rkbuf->rkbuf_flags |= RD_KAFKA_OP_F_SENT;

                /* Store time for RTT calculation */
                rkbuf->rkbuf_ts_sent = now;

                /* Add to outbuf_latency averager */
                rd_avg_add(&rkb->rkb_avg_outbuf_latency,
                           rkbuf->rkbuf_ts_sent - rkbuf->rkbuf_ts_enq);
                rd_avg_add(
                    &rkb->rkb_telemetry.rd_avg_current.rkb_avg_outbuf_latency,
                    rkbuf->rkbuf_ts_sent - rkbuf->rkbuf_ts_enq);


                if (rkbuf->rkbuf_flags & RD_KAFKA_OP_F_BLOCKING &&
                    rd_atomic32_add(&rkb->rkb_blocking_request_cnt, 1) == 1)
                        rd_kafka_brokers_broadcast_state_change(rkb->rkb_rk);

                /* Put buffer on response wait list unless we are not
                 * expecting a response (required_acks=0). */
                if (!(rkbuf->rkbuf_flags & RD_KAFKA_OP_F_NO_RESPONSE))
                        rd_kafka_bufq_enq(&rkb->rkb_waitresps, rkbuf);
                else { /* Call buffer callback for delivery report. */
                        rd_kafka_buf_callback(rkb->rkb_rk, rkb, 0, NULL, rkbuf);
                }

                cnt++;
        }

        return cnt;
}


/**
 * Add 'rkbuf' to broker 'rkb's retry queue.
 */
void rd_kafka_broker_buf_retry(rd_kafka_broker_t *rkb, rd_kafka_buf_t *rkbuf) {

        int64_t backoff = 0;
        /* Restore original replyq since replyq.q will have been NULLed
         * by buf_callback()/replyq_enq(). */
        if (!rkbuf->rkbuf_replyq.q && rkbuf->rkbuf_orig_replyq.q) {
                rkbuf->rkbuf_replyq = rkbuf->rkbuf_orig_replyq;
                rd_kafka_replyq_clear(&rkbuf->rkbuf_orig_replyq);
        }

        /* If called from another thread than rkb's broker thread
         * enqueue the buffer on the broker's op queue. */
        if (!thrd_is_current(rkb->rkb_thread)) {
                rd_kafka_op_t *rko    = rd_kafka_op_new(RD_KAFKA_OP_XMIT_RETRY);
                rko->rko_u.xbuf.rkbuf = rkbuf;
                rd_kafka_q_enq(rkb->rkb_ops, rko);
                return;
        }

        rd_rkb_dbg(rkb, PROTOCOL, "RETRY",
                   "Retrying %sRequest (v%hd, %" PRIusz
                   " bytes, retry %d/%d, "
                   "prev CorrId %" PRId32 ") in %dms",
                   rd_kafka_ApiKey2str(rkbuf->rkbuf_reqhdr.ApiKey),
                   rkbuf->rkbuf_reqhdr.ApiVersion,
                   rd_slice_size(&rkbuf->rkbuf_reader), rkbuf->rkbuf_retries,
                   rkbuf->rkbuf_max_retries, rkbuf->rkbuf_corrid,
                   rkb->rkb_rk->rk_conf.retry_backoff_ms);

        rd_atomic64_add(&rkb->rkb_c.tx_retries, 1);
        /* In some cases, failed Produce requests do not increment the retry
         * count, see rd_kafka_handle_Produce_error. */
        if (rkbuf->rkbuf_retries > 0)
                backoff = (1 << (rkbuf->rkbuf_retries - 1)) *
                          (rkb->rkb_rk->rk_conf.retry_backoff_ms);
        else
                backoff = rkb->rkb_rk->rk_conf.retry_backoff_ms;

        /* We are multiplying by 10 as (backoff_ms * percent * 1000)/100 ->
         * backoff_ms * jitter * 10 */
        backoff = rd_jitter(100 - RD_KAFKA_RETRY_JITTER_PERCENT,
                            100 + RD_KAFKA_RETRY_JITTER_PERCENT) *
                  backoff * 10;

        if (backoff > rkb->rkb_rk->rk_conf.retry_backoff_max_ms * 1000)
                backoff = rkb->rkb_rk->rk_conf.retry_backoff_max_ms * 1000;

        rkbuf->rkbuf_ts_retry = rd_clock() + backoff;
        /* Precaution: time out the request if it hasn't moved from the
         * retry queue within the retry interval (such as when the broker is
         * down). */
        // FIXME: implememt this properly.
        rkbuf->rkbuf_ts_timeout = rkbuf->rkbuf_ts_retry + (5 * 1000 * 1000);

        /* Reset send offset */
        rd_slice_seek(&rkbuf->rkbuf_reader, 0);
        rkbuf->rkbuf_corrid = 0;

        rd_kafka_bufq_enq(&rkb->rkb_retrybufs, rkbuf);
}


/**
 * Move buffers that have expired their retry backoff time from the
 * retry queue to the outbuf.
 */
static void rd_kafka_broker_retry_bufs_move(rd_kafka_broker_t *rkb,
                                            rd_ts_t *next_wakeup) {
        rd_ts_t now = rd_clock();
        rd_kafka_buf_t *rkbuf;
        int cnt = 0;

        while ((rkbuf = TAILQ_FIRST(&rkb->rkb_retrybufs.rkbq_bufs))) {
                if (rkbuf->rkbuf_ts_retry > now) {
                        if (rkbuf->rkbuf_ts_retry < *next_wakeup)
                                *next_wakeup = rkbuf->rkbuf_ts_retry;
                        break;
                }

                rd_kafka_bufq_deq(&rkb->rkb_retrybufs, rkbuf);

                rd_kafka_broker_buf_enq0(rkb, rkbuf);
                cnt++;
        }

        if (cnt > 0)
                rd_rkb_dbg(rkb, BROKER, "RETRY",
                           "Moved %d retry buffer(s) to output queue", cnt);
}


/**
 * @brief Propagate delivery report for entire message queue.
 *
 * @param err The error which will be set on each message.
 * @param status The status which will be set on each message.
 *
 * To avoid extra iterations, the \p err and \p status are set on
 * the message as they are popped off the OP_DR msgq in rd_kafka_poll() et.al
 */
void rd_kafka_dr_msgq0(rd_kafka_topic_t *rkt,
                       rd_kafka_msgq_t *rkmq,
                       rd_kafka_resp_err_t err,
                       const rd_kafka_Produce_result_t *presult) {
        rd_kafka_t *rk = rkt->rkt_rk;

        if (unlikely(rd_kafka_msgq_len(rkmq) == 0))
                return;

        if (err && rd_kafka_is_transactional(rk))
                rd_atomic64_add(&rk->rk_eos.txn_dr_fails,
                                rd_kafka_msgq_len(rkmq));

        /* Call on_acknowledgement() interceptors */
        rd_kafka_interceptors_on_acknowledgement_queue(
            rk, rkmq,
            (presult && presult->record_errors_cnt > 1)
                ? RD_KAFKA_RESP_ERR_NO_ERROR
                : err);

        if (rk->rk_drmode != RD_KAFKA_DR_MODE_NONE &&
            (!rk->rk_conf.dr_err_only || err)) {
                /* Pass all messages to application thread in one op. */
                rd_kafka_op_t *rko;

                rko               = rd_kafka_op_new(RD_KAFKA_OP_DR);
                rko->rko_err      = err;
                rko->rko_u.dr.rkt = rd_kafka_topic_keep(rkt);
                if (presult)
                        rko->rko_u.dr.presult =
                            rd_kafka_Produce_result_copy(presult);
                rd_kafka_msgq_init(&rko->rko_u.dr.msgq);

                /* Move all messages to op's msgq */
                rd_kafka_msgq_move(&rko->rko_u.dr.msgq, rkmq);

                rd_kafka_q_enq(rk->rk_rep, rko);

        } else {
                /* No delivery report callback. */

                /* Destroy the messages right away. */
                rd_kafka_msgq_purge(rk, rkmq);
        }
}


/**
 * @brief Trigger delivery reports for implicitly acked messages.
 *
 * @locks none
 * @locality broker thread - either last or current leader
 */
void rd_kafka_dr_implicit_ack(rd_kafka_broker_t *rkb,
                              rd_kafka_toppar_t *rktp,
                              uint64_t last_msgid) {
        rd_kafka_msgq_t acked        = RD_KAFKA_MSGQ_INITIALIZER(acked);
        rd_kafka_msgq_t acked2       = RD_KAFKA_MSGQ_INITIALIZER(acked2);
        rd_kafka_msg_status_t status = RD_KAFKA_MSG_STATUS_POSSIBLY_PERSISTED;

        if (rktp->rktp_rkt->rkt_conf.required_acks != 0)
                status = RD_KAFKA_MSG_STATUS_PERSISTED;

        rd_kafka_msgq_move_acked(&acked, &rktp->rktp_xmit_msgq, last_msgid,
                                 status);
        rd_kafka_msgq_move_acked(&acked2, &rktp->rktp_msgq, last_msgid, status);

        /* Insert acked2 into acked in correct order */
        rd_kafka_msgq_insert_msgq(&acked, &acked2,
                                  rktp->rktp_rkt->rkt_conf.msg_order_cmp);

        if (!rd_kafka_msgq_len(&acked))
                return;

        rd_rkb_dbg(rkb, MSG | RD_KAFKA_DBG_EOS, "IMPLICITACK",
                   "%.*s [%" PRId32
                   "] %d message(s) implicitly acked "
                   "by subsequent batch success "
                   "(msgids %" PRIu64 "..%" PRIu64
                   ", "
                   "last acked %" PRIu64 ")",
                   RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                   rktp->rktp_partition, rd_kafka_msgq_len(&acked),
                   rd_kafka_msgq_first(&acked)->rkm_u.producer.msgid,
                   rd_kafka_msgq_last(&acked)->rkm_u.producer.msgid,
                   last_msgid);

        /* Trigger delivery reports */
        rd_kafka_dr_msgq(rktp->rktp_rkt, &acked, RD_KAFKA_RESP_ERR_NO_ERROR);
}

/**
 * @brief Broker id comparator
 */
static int rd_kafka_broker_cmp_by_id(const void *_a, const void *_b) {
        const rd_kafka_broker_t *a = _a, *b = _b;
        return RD_CMP(a->rkb_nodeid, b->rkb_nodeid);
}


/**
 * @brief Set the broker logname (used in logs) to a copy of \p logname.
 *
 * @locality any
 * @locks none
 */
static void rd_kafka_broker_set_logname(rd_kafka_broker_t *rkb,
                                        const char *logname) {
        mtx_lock(&rkb->rkb_logname_lock);
        if (rkb->rkb_logname)
                rd_free(rkb->rkb_logname);
        rkb->rkb_logname = rd_strdup(logname);
        mtx_unlock(&rkb->rkb_logname_lock);
}



/**
 * @brief Prepare destruction of the broker object.
 *
 * Since rd_kafka_broker_terminating() relies on the refcnt of the
 * broker to reach 1, we need to loose any self-references
 * to avoid a hang (waiting for refcnt decrease) on destruction.
 *
 * @locality broker thread
 * @locks none
 */
static void rd_kafka_broker_prepare_destroy(rd_kafka_broker_t *rkb) {
        rd_kafka_broker_monitor_del(&rkb->rkb_coord_monitor);
}

static rd_kafka_resp_err_t rd_kafka_broker_destroy_error(rd_kafka_t *rk) {
        return rd_kafka_terminating(rk) ? RD_KAFKA_RESP_ERR__DESTROY
                                        : RD_KAFKA_RESP_ERR__DESTROY_BROKER;
}

/**
 * @brief Serve a broker op (an op posted by another thread to be handled by
 *        this broker's thread).
 *
 * @returns true if calling op loop should break out, else false to continue.
 * @locality broker thread
 * @locks none
 */
static RD_WARN_UNUSED_RESULT rd_bool_t
rd_kafka_broker_op_serve(rd_kafka_broker_t *rkb, rd_kafka_op_t *rko) {
        rd_kafka_toppar_t *rktp;
        rd_kafka_resp_err_t topic_err;
        rd_bool_t wakeup = rd_false;

        rd_kafka_assert(rkb->rkb_rk, thrd_is_current(rkb->rkb_thread));

        switch (rko->rko_type) {
        case RD_KAFKA_OP_NODE_UPDATE: {
                rd_bool_t updated = rd_false;
                char brokername[RD_KAFKA_NODENAME_SIZE];

                /* Need kafka_wrlock for updating rk_broker_by_id */
                rd_kafka_wrlock(rkb->rkb_rk);
                rd_kafka_broker_lock(rkb);

                if (strcmp(rkb->rkb_nodename, rko->rko_u.node.nodename)) {
                        rd_rkb_dbg(rkb, BROKER, "UPDATE",
                                   "Nodename changed from %s to %s",
                                   rkb->rkb_nodename, rko->rko_u.node.nodename);
                        rd_strlcpy(rkb->rkb_nodename, rko->rko_u.node.nodename,
                                   sizeof(rkb->rkb_nodename));
                        rkb->rkb_nodename_epoch++;
                        updated = rd_true;
                }

                rd_kafka_mk_brokername(brokername, sizeof(brokername),
                                       rkb->rkb_proto, rkb->rkb_nodename,
                                       rkb->rkb_nodeid, RD_KAFKA_LEARNED);
                if (strcmp(rkb->rkb_name, brokername)) {
                        /* Udate the name copy used for logging. */
                        rd_kafka_broker_set_logname(rkb, brokername);

                        rd_rkb_dbg(rkb, BROKER, "UPDATE",
                                   "Name changed from %s to %s", rkb->rkb_name,
                                   brokername);
                        rd_strlcpy(rkb->rkb_name, brokername,
                                   sizeof(rkb->rkb_name));
                }
                rd_kafka_broker_unlock(rkb);
                rd_kafka_wrunlock(rkb->rkb_rk);

                if (updated) {
                        rd_kafka_broker_fail(rkb, LOG_DEBUG,
                                             RD_KAFKA_RESP_ERR__TRANSPORT,
                                             "Broker hostname updated");
                }

                rd_kafka_brokers_broadcast_state_change(rkb->rkb_rk);
                break;
        }

        case RD_KAFKA_OP_XMIT_BUF:
                rd_kafka_broker_buf_enq2(rkb, rko->rko_u.xbuf.rkbuf);
                rko->rko_u.xbuf.rkbuf = NULL; /* buffer now owned by broker */
                if (rko->rko_replyq.q) {
                        /* Op will be reused for forwarding response. */
                        rko = NULL;
                }
                break;

        case RD_KAFKA_OP_XMIT_RETRY:
                rd_kafka_broker_buf_retry(rkb, rko->rko_u.xbuf.rkbuf);
                rko->rko_u.xbuf.rkbuf = NULL;
                break;

        case RD_KAFKA_OP_PARTITION_JOIN:
                /*
                 * Add partition to broker toppars
                 */
                rktp = rko->rko_rktp;
                rd_kafka_toppar_lock(rktp);

                /* Abort join if instance is terminating */
                if (rd_kafka_broker_or_instance_terminating(rkb) ||
                    (rktp->rktp_flags & RD_KAFKA_TOPPAR_F_REMOVE)) {
                        rd_rkb_dbg(
                            rkb, BROKER | RD_KAFKA_DBG_TOPIC, "TOPBRK",
                            "Topic %s [%" PRId32
                            "]: not joining broker: "
                            "%s",
                            rktp->rktp_rkt->rkt_topic->str,
                            rktp->rktp_partition,
                            rd_kafka_terminating(rkb->rkb_rk)
                                ? "instance is terminating"
                            : rd_kafka_broker_termination_in_progress(rkb)
                                ? "broker is terminating"
                                : "partition removed");

                        rd_kafka_broker_destroy(rktp->rktp_next_broker);
                        rktp->rktp_next_broker = NULL;
                        rd_kafka_toppar_unlock(rktp);
                        break;
                }

                /* See if we are still the next broker */
                if (rktp->rktp_next_broker != rkb) {
                        rd_rkb_dbg(
                            rkb, BROKER | RD_KAFKA_DBG_TOPIC, "TOPBRK",
                            "Topic %s [%" PRId32
                            "]: not joining broker "
                            "(next broker %s)",
                            rktp->rktp_rkt->rkt_topic->str,
                            rktp->rktp_partition,
                            rktp->rktp_next_broker
                                ? rd_kafka_broker_name(rktp->rktp_next_broker)
                                : "(none)");

                        /* Need temporary refcount so we can safely unlock
                         * after q_enq(). */
                        rd_kafka_toppar_keep(rktp);

                        /* No, forward this op to the new next broker. */
                        rd_kafka_q_enq(rktp->rktp_next_broker->rkb_ops, rko);
                        rko = NULL;

                        rd_kafka_toppar_unlock(rktp);
                        rd_kafka_toppar_destroy(rktp);

                        break;
                }

                rd_rkb_dbg(rkb, BROKER | RD_KAFKA_DBG_TOPIC, "TOPBRK",
                           "Topic %s [%" PRId32
                           "]: joining broker "
                           "(rktp %p, %d message(s) queued)",
                           rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                           rktp, rd_kafka_msgq_len(&rktp->rktp_msgq));

                rd_kafka_assert(NULL,
                                !(rktp->rktp_flags & RD_KAFKA_TOPPAR_F_ON_RKB));
                rktp->rktp_flags |= RD_KAFKA_TOPPAR_F_ON_RKB;
                rd_kafka_toppar_keep(rktp);
                rd_kafka_broker_lock(rkb);
                TAILQ_INSERT_TAIL(&rkb->rkb_toppars, rktp, rktp_rkblink);
                rkb->rkb_toppar_cnt++;
                rd_kafka_broker_unlock(rkb);
                rktp->rktp_broker = rkb;
                rd_assert(!rktp->rktp_msgq_wakeup_q);
                rktp->rktp_msgq_wakeup_q = rd_kafka_q_keep(rkb->rkb_ops);
                rd_kafka_broker_keep(rkb);

                if (rkb->rkb_rk->rk_type == RD_KAFKA_PRODUCER) {
                        rd_kafka_broker_active_toppar_add(rkb, rktp, "joining");

                        if (rd_kafka_is_idempotent(rkb->rkb_rk)) {
                                /* Wait for all outstanding requests from
                                 * the previous leader to finish before
                                 * producing anything to this new leader. */
                                rd_kafka_idemp_drain_toppar(
                                    rktp,
                                    "wait for outstanding requests to "
                                    "finish before producing to "
                                    "new leader");
                        }
                } else if (rkb->rkb_rk->rk_type == RD_KAFKA_CONSUMER) {
                        rktp->rktp_ts_fetch_backoff = 0;
                }

                rd_kafka_broker_destroy(rktp->rktp_next_broker);
                rktp->rktp_next_broker = NULL;

                rd_kafka_toppar_unlock(rktp);

                rd_kafka_brokers_broadcast_state_change(rkb->rkb_rk);
                break;

        case RD_KAFKA_OP_PARTITION_LEAVE:
                /*
                 * Remove partition from broker toppars
                 */
                rktp = rko->rko_rktp;

                /* If there is a topic-wide error, use it as error code
                 * when failing messages below. */
                topic_err = rd_kafka_topic_get_error(rktp->rktp_rkt);

                rd_kafka_toppar_lock(rktp);

                /* Multiple PARTITION_LEAVEs are possible during partition
                 * migration, make sure we're supposed to handle this one. */
                if (unlikely(rktp->rktp_broker != rkb)) {
                        rd_rkb_dbg(rkb, BROKER | RD_KAFKA_DBG_TOPIC, "TOPBRK",
                                   "Topic %s [%" PRId32
                                   "]: "
                                   "ignoring PARTITION_LEAVE: "
                                   "not delegated to broker (%s)",
                                   rktp->rktp_rkt->rkt_topic->str,
                                   rktp->rktp_partition,
                                   rktp->rktp_broker
                                       ? rd_kafka_broker_name(rktp->rktp_broker)
                                       : "none");
                        rd_kafka_toppar_unlock(rktp);
                        break;
                }
                rd_kafka_toppar_unlock(rktp);

                /* Remove from fetcher list */
                rd_kafka_toppar_fetch_decide(rktp, rkb, 1 /*force remove*/);

                if (rkb->rkb_rk->rk_type == RD_KAFKA_PRODUCER) {
                        /* Purge any ProduceRequests for this toppar
                         * in the output queue. */
                        rd_kafka_broker_bufq_purge_by_toppar(
                            rkb, &rkb->rkb_outbufs, RD_KAFKAP_Produce, rktp,
                            RD_KAFKA_RESP_ERR__RETRY);
                }


                rd_kafka_toppar_lock(rktp);

                rd_rkb_dbg(rkb, BROKER | RD_KAFKA_DBG_TOPIC, "TOPBRK",
                           "Topic %s [%" PRId32
                           "]: leaving broker "
                           "(%d messages in xmitq, next broker %s, rktp %p)",
                           rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                           rd_kafka_msgq_len(&rktp->rktp_xmit_msgq),
                           rktp->rktp_next_broker
                               ? rd_kafka_broker_name(rktp->rktp_next_broker)
                               : "(none)",
                           rktp);

                /* Insert xmitq(broker-local) messages to the msgq(global)
                 * at their sorted position to maintain ordering. */
                rd_kafka_msgq_insert_msgq(
                    &rktp->rktp_msgq, &rktp->rktp_xmit_msgq,
                    rktp->rktp_rkt->rkt_conf.msg_order_cmp);

                if (rkb->rkb_rk->rk_type == RD_KAFKA_PRODUCER)
                        rd_kafka_broker_active_toppar_del(rkb, rktp, "leaving");

                rd_kafka_broker_lock(rkb);
                TAILQ_REMOVE(&rkb->rkb_toppars, rktp, rktp_rkblink);
                rkb->rkb_toppar_cnt--;
                rd_kafka_broker_unlock(rkb);
                rd_kafka_broker_destroy(rktp->rktp_broker);
                if (rktp->rktp_msgq_wakeup_q) {
                        rd_kafka_q_destroy(rktp->rktp_msgq_wakeup_q);
                        rktp->rktp_msgq_wakeup_q = NULL;
                }
                rktp->rktp_broker = NULL;

                rd_assert(rktp->rktp_flags & RD_KAFKA_TOPPAR_F_ON_RKB);
                rktp->rktp_flags &= ~RD_KAFKA_TOPPAR_F_ON_RKB;

                if (rktp->rktp_next_broker) {
                        /* There is a next broker we need to migrate to. */
                        rko->rko_type = RD_KAFKA_OP_PARTITION_JOIN;
                        rd_kafka_q_enq(rktp->rktp_next_broker->rkb_ops, rko);
                        rko = NULL;
                } else {
                        rd_rkb_dbg(rkb, BROKER | RD_KAFKA_DBG_TOPIC, "TOPBRK",
                                   "Topic %s [%" PRId32
                                   "]: no next broker, "
                                   "failing %d message(s) in partition queue",
                                   rktp->rktp_rkt->rkt_topic->str,
                                   rktp->rktp_partition,
                                   rd_kafka_msgq_len(&rktp->rktp_msgq));
                        rd_kafka_assert(NULL, rd_kafka_msgq_len(
                                                  &rktp->rktp_xmit_msgq) == 0);
                        rd_kafka_dr_msgq(
                            rktp->rktp_rkt, &rktp->rktp_msgq,
                            rd_kafka_terminating(rkb->rkb_rk)
                                ? RD_KAFKA_RESP_ERR__DESTROY
                                : (topic_err
                                       ? topic_err
                                       : RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION));

                        if (rkb->rkb_rk->rk_type == RD_KAFKA_CONSUMER) {
                                rd_kafka_toppar_purge_internal_fetch_queue_maybe(
                                    rktp);
                        }
                }

                rd_kafka_toppar_unlock(rktp);
                rd_kafka_toppar_destroy(rktp); /* from JOIN */

                rd_kafka_brokers_broadcast_state_change(rkb->rkb_rk);
                break;

        case RD_KAFKA_OP_TERMINATE:
                /* nop: just a wake-up. */
                rd_rkb_dbg(rkb, BROKER, "TERM",
                           "Received TERMINATE op in state %s: "
                           "%d refcnts, %d toppar(s), %d active toppar(s), "
                           "%d outbufs, %d waitresps, %d retrybufs",
                           rd_kafka_broker_state_names[rkb->rkb_state],
                           rd_refcnt_get(&rkb->rkb_refcnt), rkb->rkb_toppar_cnt,
                           rkb->rkb_active_toppar_cnt,
                           (int)rd_kafka_bufq_cnt(&rkb->rkb_outbufs),
                           (int)rd_kafka_bufq_cnt(&rkb->rkb_waitresps),
                           (int)rd_kafka_bufq_cnt(&rkb->rkb_retrybufs));
                /* Expedite termination by bringing down the broker
                 * and trigger a state change.
                 * This makes sure any eonce dependent on state changes
                 * are triggered. */
                rd_kafka_broker_fail(rkb, LOG_DEBUG,
                                     rd_kafka_broker_destroy_error(rkb->rkb_rk),
                                     "Decommissioning this broker");

                rd_kafka_broker_prepare_destroy(rkb);
                /* Release main thread reference here */
                rd_kafka_broker_destroy(rkb);
                wakeup = rd_true;
                break;

        case RD_KAFKA_OP_WAKEUP:
                wakeup = rd_true;
                break;

        case RD_KAFKA_OP_PURGE:
                rd_kafka_broker_handle_purge_queues(rkb, rko);
                rko = NULL; /* the rko is reused for the reply */
                break;

        case RD_KAFKA_OP_CONNECT:
                /* Sparse connections: connection requested, transition
                 * to TRY_CONNECT state to trigger new connection. */
                if (rkb->rkb_state == RD_KAFKA_BROKER_STATE_INIT) {
                        rd_rkb_dbg(rkb, BROKER, "CONNECT",
                                   "Received CONNECT op");
                        rkb->rkb_persistconn.internal++;
                        rd_kafka_broker_lock(rkb);
                        rd_kafka_broker_set_state(
                            rkb, RD_KAFKA_BROKER_STATE_TRY_CONNECT);
                        rd_kafka_broker_unlock(rkb);

                } else if (rkb->rkb_state >=
                           RD_KAFKA_BROKER_STATE_TRY_CONNECT) {
                        rd_bool_t do_disconnect = rd_false;

                        /* If the nodename was changed since the last connect,
                         * close the current connection. */

                        rd_kafka_broker_lock(rkb);
                        do_disconnect =
                            (rkb->rkb_connect_epoch != rkb->rkb_nodename_epoch);
                        rd_kafka_broker_unlock(rkb);

                        if (do_disconnect)
                                rd_kafka_broker_fail(
                                    rkb, LOG_DEBUG,
                                    RD_KAFKA_RESP_ERR__TRANSPORT,
                                    "Closing connection due to "
                                    "nodename change");
                }

                /* Expedite next reconnect */
                rkb->rkb_ts_reconnect = 0;

                wakeup = rd_true;
                break;

        case RD_KAFKA_OP_SASL_REAUTH:
                rd_rkb_dbg(rkb, BROKER, "REAUTH", "Received REAUTH op");

                /* We don't need a lock for rkb_max_inflight. It's changed only
                 * on the broker thread. */
                rkb->rkb_max_inflight = 1;

                rd_kafka_broker_lock(rkb);
                rd_kafka_broker_set_state(rkb, RD_KAFKA_BROKER_STATE_REAUTH);
                rd_kafka_broker_unlock(rkb);

                wakeup = rd_true;
                break;

        default:
                rd_kafka_assert(rkb->rkb_rk, !*"unhandled op type");
                break;
        }

        if (rko)
                rd_kafka_op_reply(rko, RD_KAFKA_RESP_ERR_NO_ERROR);

        return wakeup;
}



/**
 * @brief Serve broker ops.
 * @returns the number of ops served
 */
static RD_WARN_UNUSED_RESULT int
rd_kafka_broker_ops_serve(rd_kafka_broker_t *rkb, rd_ts_t timeout_us) {
        rd_kafka_op_t *rko;
        int cnt = 0;

        while ((rko = rd_kafka_q_pop(rkb->rkb_ops, timeout_us, 0)) &&
               (cnt++, !rd_kafka_broker_op_serve(rkb, rko)))
                timeout_us = RD_POLL_NOWAIT;

        return cnt;
}

/**
 * @brief Serve broker ops and IOs.
 *
 * If a connection exists, poll IO first based on timeout.
 * Use remaining timeout for ops queue poll.
 *
 * If no connection, poll ops queue using timeout.
 *
 * Sparse connections: if there's need for a connection, set
 *                     timeout to NOWAIT.
 *
 * @param abs_timeout Maximum block time (absolute time).
 *
 * @returns true on wakeup (broker state machine needs to be served),
 *          else false.
 *
 * @locality broker thread
 * @locks none
 */
static RD_WARN_UNUSED_RESULT rd_bool_t
rd_kafka_broker_ops_io_serve(rd_kafka_broker_t *rkb, rd_ts_t abs_timeout) {
        rd_ts_t now;
        rd_bool_t wakeup;

        if (unlikely(rd_kafka_broker_or_instance_terminating(rkb)))
                abs_timeout = rd_clock() + 1000;
        else if (unlikely(rd_kafka_broker_needs_connection(rkb)))
                abs_timeout = RD_POLL_NOWAIT;
        else if (unlikely(abs_timeout == RD_POLL_INFINITE))
                abs_timeout =
                    rd_clock() + ((rd_ts_t)rd_kafka_max_block_ms * 1000);


        if (likely(rkb->rkb_transport != NULL)) {
                /* Poll and serve IO events and also poll the ops queue.
                 *
                 * The return value indicates if ops_serve() below should
                 * use a timeout or not.
                 *
                 * If there are ops enqueued cut the timeout short so
                 * that they're processed as soon as possible.
                 */
                if (abs_timeout > 0 && rd_kafka_q_len(rkb->rkb_ops) > 0)
                        abs_timeout = RD_POLL_NOWAIT;

                if (rd_kafka_transport_io_serve(
                        rkb->rkb_transport, rkb->rkb_ops,
                        rd_timeout_remains(abs_timeout)))
                        abs_timeout = RD_POLL_NOWAIT;
        }


        /* Serve broker ops */
        wakeup =
            rd_kafka_broker_ops_serve(rkb, rd_timeout_remains_us(abs_timeout));

        rd_atomic64_add(&rkb->rkb_c.wakeups, 1);

        /* An op might have triggered the need for a connection, if so
         * transition to TRY_CONNECT state. */
        if (unlikely(rd_kafka_broker_needs_connection(rkb) &&
                     rkb->rkb_state == RD_KAFKA_BROKER_STATE_INIT)) {
                rd_kafka_broker_lock(rkb);
                rd_kafka_broker_set_state(rkb,
                                          RD_KAFKA_BROKER_STATE_TRY_CONNECT);
                rd_kafka_broker_unlock(rkb);
                wakeup = rd_true;
        }

        /* Scan queues for timeouts. */
        now = rd_clock();
        if (rd_interval(&rkb->rkb_timeout_scan_intvl, 1000000, now) > 0)
                rd_kafka_broker_timeout_scan(rkb, now);

        return wakeup;
}


/**
 * @brief Consumer: Serve the toppars assigned to this broker.
 *
 * @returns the minimum Fetch backoff time (abs timestamp) for the
 *          partitions to fetch.
 *
 * @locality broker thread
 */
static rd_ts_t rd_kafka_broker_consumer_toppars_serve(rd_kafka_broker_t *rkb) {
        rd_kafka_toppar_t *rktp, *rktp_tmp;
        rd_ts_t min_backoff = RD_TS_MAX;

        TAILQ_FOREACH_SAFE(rktp, &rkb->rkb_toppars, rktp_rkblink, rktp_tmp) {
                rd_ts_t backoff;

                /* Serve toppar to update desired rktp state */
                backoff = rd_kafka_broker_consumer_toppar_serve(rkb, rktp);
                if (backoff < min_backoff)
                        min_backoff = backoff;
        }

        return min_backoff;
}


/**
 * @brief Scan toppar's xmit and producer queue for message timeouts and
 *        enqueue delivery reports for timed out messages.
 *
 * @param abs_next_timeout will be set to the next message timeout, or 0
 *                         if no timeout.
 *
 * @returns the number of messages timed out.
 *
 * @locality toppar's broker handler thread
 * @locks toppar_lock MUST be held
 */
static int rd_kafka_broker_toppar_msgq_scan(rd_kafka_broker_t *rkb,
                                            rd_kafka_toppar_t *rktp,
                                            rd_ts_t now,
                                            rd_ts_t *abs_next_timeout) {
        rd_kafka_msgq_t xtimedout = RD_KAFKA_MSGQ_INITIALIZER(xtimedout);
        rd_kafka_msgq_t qtimedout = RD_KAFKA_MSGQ_INITIALIZER(qtimedout);
        int xcnt, qcnt, cnt;
        uint64_t first, last;
        rd_ts_t next;

        *abs_next_timeout = 0;

        xcnt = rd_kafka_msgq_age_scan(rktp, &rktp->rktp_xmit_msgq, &xtimedout,
                                      now, &next);
        if (next && next < *abs_next_timeout)
                *abs_next_timeout = next;

        qcnt = rd_kafka_msgq_age_scan(rktp, &rktp->rktp_msgq, &qtimedout, now,
                                      &next);
        if (next && (!*abs_next_timeout || next < *abs_next_timeout))
                *abs_next_timeout = next;

        cnt = xcnt + qcnt;
        if (likely(cnt == 0))
                return 0;

        /* Insert queue-timedout into xmitqueue-timedout in a sorted fashion */
        rd_kafka_msgq_insert_msgq(&xtimedout, &qtimedout,
                                  rktp->rktp_rkt->rkt_conf.msg_order_cmp);

        first = rd_kafka_msgq_first(&xtimedout)->rkm_u.producer.msgid;
        last  = rd_kafka_msgq_last(&xtimedout)->rkm_u.producer.msgid;

        rd_rkb_dbg(rkb, MSG, "TIMEOUT",
                   "%s [%" PRId32
                   "]: timed out %d+%d message(s) "
                   "(MsgId %" PRIu64 "..%" PRIu64
                   "): message.timeout.ms exceeded",
                   rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition, xcnt,
                   qcnt, first, last);

        /* Trigger delivery report for timed out messages */
        rd_kafka_dr_msgq(rktp->rktp_rkt, &xtimedout,
                         RD_KAFKA_RESP_ERR__MSG_TIMED_OUT);

        return cnt;
}


/**
 * @brief Producer: Check this broker's toppars for message timeouts.
 *
 * This is only used by the internal broker to enforce message timeouts.
 *
 * @returns the next absolute scan time.
 *
 * @locality internal broker thread.
 */
static rd_ts_t rd_kafka_broker_toppars_timeout_scan(rd_kafka_broker_t *rkb,
                                                    rd_ts_t now) {
        rd_kafka_toppar_t *rktp;
        rd_ts_t next = now + (1000 * 1000);

        TAILQ_FOREACH(rktp, &rkb->rkb_toppars, rktp_rkblink) {
                rd_ts_t this_next;

                rd_kafka_toppar_lock(rktp);

                if (unlikely(rktp->rktp_broker != rkb)) {
                        /* Currently migrating away from this
                         * broker. */
                        rd_kafka_toppar_unlock(rktp);
                        continue;
                }

                /* Scan queues for msg timeouts */
                rd_kafka_broker_toppar_msgq_scan(rkb, rktp, now, &this_next);

                rd_kafka_toppar_unlock(rktp);

                if (this_next && this_next < next)
                        next = this_next;
        }

        return next;
}


/**
 * @brief Idle function for the internal broker handle.
 */
static void rd_kafka_broker_internal_serve(rd_kafka_broker_t *rkb,
                                           rd_ts_t abs_timeout) {
        int initial_state = rkb->rkb_state;
        rd_bool_t wakeup;

        if (rkb->rkb_rk->rk_type == RD_KAFKA_CONSUMER) {
                /* Consumer */
                do {
                        rd_kafka_broker_consumer_toppars_serve(rkb);

                        wakeup = rd_kafka_broker_ops_io_serve(rkb, abs_timeout);

                } while (!rd_kafka_broker_terminating(rkb) &&
                         (int)rkb->rkb_state == initial_state && !wakeup &&
                         !rd_timeout_expired(rd_timeout_remains(abs_timeout)));
        } else {
                /* Producer */
                rd_ts_t next_timeout_scan = 0;

                do {
                        rd_ts_t now = rd_clock();

                        if (now >= next_timeout_scan)
                                next_timeout_scan =
                                    rd_kafka_broker_toppars_timeout_scan(rkb,
                                                                         now);

                        wakeup = rd_kafka_broker_ops_io_serve(
                            rkb, RD_MIN(abs_timeout, next_timeout_scan));

                } while (!rd_kafka_broker_terminating(rkb) &&
                         (int)rkb->rkb_state == initial_state && !wakeup &&
                         !rd_timeout_expired(rd_timeout_remains(abs_timeout)));
        }
}


/**
 * @returns the number of requests that may be enqueued before
 *          queue.backpressure.threshold is reached.
 */

static RD_INLINE unsigned int
rd_kafka_broker_outbufs_space(rd_kafka_broker_t *rkb) {
        int r = rkb->rkb_rk->rk_conf.queue_backpressure_thres -
                rd_atomic32_get(&rkb->rkb_outbufs.rkbq_cnt);
        return r < 0 ? 0 : (unsigned int)r;
}



/**
 * @brief Update \p *next_wakeup_ptr to \p maybe_next_wakeup if it is sooner.
 *
 * Both parameters are absolute timestamps.
 * \p maybe_next_wakeup must not be 0.
 */
#define rd_kafka_set_next_wakeup(next_wakeup_ptr, maybe_next_wakeup)           \
        do {                                                                   \
                rd_ts_t *__n = (next_wakeup_ptr);                              \
                rd_ts_t __m  = (maybe_next_wakeup);                            \
                rd_dassert(__m != 0);                                          \
                if (__m < *__n)                                                \
                        *__n = __m;                                            \
        } while (0)


/**
 * @brief Serve a toppar for producing.
 *
 * @param next_wakeup will be updated to when the next wake-up/attempt is
 *                    desired. Does not take the current value into
 *                    consideration, even if it is lower.
 * @param do_timeout_scan perform msg timeout scan
 * @param may_send if set to false there is something on the global level
 *                 that prohibits sending messages, such as a transactional
 *                 state.
 * @param flushing App is calling flush(): override linger.ms as immediate.
 *
 * @returns the number of messages produced.
 *
 * @locks none
 * @locality broker thread
 */
static int rd_kafka_toppar_producer_serve(rd_kafka_broker_t *rkb,
                                          rd_kafka_toppar_t *rktp,
                                          const rd_kafka_pid_t pid,
                                          rd_ts_t now,
                                          rd_ts_t *next_wakeup,
                                          rd_bool_t do_timeout_scan,
                                          rd_bool_t may_send,
                                          rd_bool_t flushing) {
        int cnt = 0;
        int r;
        rd_kafka_msg_t *rkm;
        int move_cnt = 0;
        int max_requests;
        int reqcnt;
        int inflight              = 0;
        uint64_t epoch_base_msgid = 0;
        rd_bool_t batch_ready     = rd_false;

        /* By limiting the number of not-yet-sent buffers (rkb_outbufs) we
         * provide a backpressure mechanism to the producer loop
         * which allows larger message batches to accumulate and thus
         * increase throughput.
         * This comes at no latency cost since there are already
         * buffers enqueued waiting for transmission. */
        max_requests = rd_kafka_broker_outbufs_space(rkb);

        rd_kafka_toppar_lock(rktp);

        if (unlikely(rktp->rktp_broker != rkb)) {
                /* Currently migrating away from this
                 * broker. */
                rd_kafka_toppar_unlock(rktp);
                return 0;
        }

        if (unlikely(do_timeout_scan)) {
                int timeoutcnt;
                rd_ts_t next;

                /* Scan queues for msg timeouts */
                timeoutcnt =
                    rd_kafka_broker_toppar_msgq_scan(rkb, rktp, now, &next);

                if (next)
                        rd_kafka_set_next_wakeup(next_wakeup, next);

                if (rd_kafka_is_idempotent(rkb->rkb_rk)) {
                        if (!rd_kafka_pid_valid(pid)) {
                                /* If we don't have a PID, we can't transmit
                                 * any messages. */
                                rd_kafka_toppar_unlock(rktp);
                                return 0;

                        } else if (timeoutcnt > 0) {
                                /* Message timeouts will lead to gaps the in
                                 * the message sequence and thus trigger
                                 * OutOfOrderSequence errors from the broker.
                                 * Bump the epoch to reset the base msgid after
                                 * draining all partitions. */

                                /* Must not hold toppar lock */
                                rd_kafka_toppar_unlock(rktp);

                                rd_kafka_idemp_drain_epoch_bump(
                                    rkb->rkb_rk, RD_KAFKA_RESP_ERR__TIMED_OUT,
                                    "%d message(s) timed out "
                                    "on %s [%" PRId32 "]",
                                    timeoutcnt, rktp->rktp_rkt->rkt_topic->str,
                                    rktp->rktp_partition);
                                return 0;
                        }
                }
        }

        if (unlikely(!may_send)) {
                /* Sends prohibited on the broker or instance level */
                max_requests = 0;
        } else if (unlikely(rd_kafka_fatal_error_code(rkb->rkb_rk))) {
                /* Fatal error has been raised, don't produce. */
                max_requests = 0;
        } else if (unlikely(RD_KAFKA_TOPPAR_IS_PAUSED(rktp))) {
                /* Partition is paused */
                max_requests = 0;
        } else if (unlikely(rd_kafka_is_transactional(rkb->rkb_rk) &&
                            !rd_kafka_txn_toppar_may_send_msg(rktp))) {
                /* Partition not registered in transaction yet */
                max_requests = 0;
        } else if (max_requests > 0) {
                /* Move messages from locked partition produce queue
                 * to broker-local xmit queue. */
                if ((move_cnt = rktp->rktp_msgq.rkmq_msg_cnt) > 0) {

                        rd_kafka_msgq_insert_msgq(
                            &rktp->rktp_xmit_msgq, &rktp->rktp_msgq,
                            rktp->rktp_rkt->rkt_conf.msg_order_cmp);
                }

                /* Calculate maximum wait-time to honour
                 * queue.buffering.max.ms contract.
                 * Unless flushing in which case immediate
                 * wakeups are allowed. */
                batch_ready = rd_kafka_msgq_allow_wakeup_at(
                    &rktp->rktp_msgq, &rktp->rktp_xmit_msgq,
                    /* Only update the broker thread wakeup time
                     * if connection is up and messages can actually be
                     * sent, otherwise the wakeup can't do much. */
                    rkb->rkb_state == RD_KAFKA_BROKER_STATE_UP ? next_wakeup
                                                               : NULL,
                    now, flushing ? 1 : rkb->rkb_rk->rk_conf.buffering_max_us,
                    /* Batch message count threshold */
                    rkb->rkb_rk->rk_conf.batch_num_messages,
                    /* Batch total size threshold */
                    rkb->rkb_rk->rk_conf.batch_size);
        }

        rd_kafka_toppar_unlock(rktp);


        if (rd_kafka_is_idempotent(rkb->rkb_rk)) {
                /* Update the partition's cached PID, and reset the
                 * base msg sequence if necessary */
                rd_bool_t did_purge = rd_false;

                if (unlikely(!rd_kafka_pid_eq(pid, rktp->rktp_eos.pid))) {
                        /* Flush any ProduceRequests for this partition in the
                         * output buffer queue to speed up recovery. */
                        rd_kafka_broker_bufq_purge_by_toppar(
                            rkb, &rkb->rkb_outbufs, RD_KAFKAP_Produce, rktp,
                            RD_KAFKA_RESP_ERR__RETRY);
                        did_purge = rd_true;

                        if (rd_kafka_pid_valid(rktp->rktp_eos.pid))
                                rd_rkb_dbg(
                                    rkb, QUEUE, "TOPPAR",
                                    "%.*s [%" PRId32
                                    "] PID has changed: "
                                    "must drain requests for all "
                                    "partitions before resuming reset "
                                    "of PID",
                                    RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                                    rktp->rktp_partition);
                }

                inflight = rd_atomic32_get(&rktp->rktp_msgs_inflight);

                if (unlikely(rktp->rktp_eos.wait_drain)) {
                        if (inflight) {
                                /* Waiting for in-flight requests to
                                 * drain/finish before producing anything more.
                                 * This is used to recover to a consistent
                                 * state when the partition leader
                                 * has changed, or timed out messages
                                 * have been removed from the queue. */

                                rd_rkb_dbg(
                                    rkb, QUEUE, "TOPPAR",
                                    "%.*s [%" PRId32
                                    "] waiting for "
                                    "%d in-flight request(s) to drain "
                                    "from queue before continuing "
                                    "to produce",
                                    RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                                    rktp->rktp_partition, inflight);

                                /* Flush any ProduceRequests for this
                                 * partition in the output buffer queue to
                                 * speed up draining. */
                                if (!did_purge)
                                        rd_kafka_broker_bufq_purge_by_toppar(
                                            rkb, &rkb->rkb_outbufs,
                                            RD_KAFKAP_Produce, rktp,
                                            RD_KAFKA_RESP_ERR__RETRY);

                                return 0;
                        }

                        rd_rkb_dbg(rkb, QUEUE, "TOPPAR",
                                   "%.*s [%" PRId32
                                   "] all in-flight requests "
                                   "drained from queue",
                                   RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                                   rktp->rktp_partition);

                        rktp->rktp_eos.wait_drain = rd_false;
                }

                /* Limit the number of in-flight requests (per partition)
                 * to the broker's sequence de-duplication window. */
                max_requests = RD_MIN(max_requests,
                                      RD_KAFKA_IDEMP_MAX_INFLIGHT - inflight);
        }


        /* Check if allowed to create and enqueue a ProduceRequest */
        if (max_requests <= 0)
                return 0;

        r = rktp->rktp_xmit_msgq.rkmq_msg_cnt;
        if (r == 0)
                return 0;

        rd_kafka_msgq_verify_order(rktp, &rktp->rktp_xmit_msgq, 0, rd_false);

        rd_rkb_dbg(rkb, QUEUE, "TOPPAR",
                   "%.*s [%" PRId32
                   "] %d message(s) in "
                   "xmit queue (%d added from partition queue)",
                   RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                   rktp->rktp_partition, r, move_cnt);

        rkm = TAILQ_FIRST(&rktp->rktp_xmit_msgq.rkmq_msgs);
        rd_dassert(rkm != NULL);

        if (rd_kafka_is_idempotent(rkb->rkb_rk)) {
                /* Update the partition's cached PID, and reset the
                 * base msg sequence if necessary */
                if (unlikely(!rd_kafka_pid_eq(pid, rktp->rktp_eos.pid))) {
                        /* Attempt to change the pid, it will fail if there
                         * are outstanding messages in-flight, in which case
                         * we eventually come back here to retry. */
                        if (!rd_kafka_toppar_pid_change(
                                rktp, pid, rkm->rkm_u.producer.msgid))
                                return 0;
                }

                rd_kafka_toppar_lock(rktp);
                /* Idempotent producer epoch base msgid, this is passed to the
                 * ProduceRequest and msgset writer to adjust the protocol-level
                 * per-message sequence number. */
                epoch_base_msgid = rktp->rktp_eos.epoch_base_msgid;
                rd_kafka_toppar_unlock(rktp);
        }

        if (unlikely(rkb->rkb_state != RD_KAFKA_BROKER_STATE_UP)) {
                /* There are messages to send but connection is not up. */
                rd_rkb_dbg(rkb, BROKER, "TOPPAR",
                           "%.*s [%" PRId32
                           "] "
                           "%d message(s) queued but broker not up",
                           RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                           rktp->rktp_partition, r);
                rkb->rkb_persistconn.internal++;
                return 0;
        }

        /* Attempt to fill the batch size, but limit our waiting
         * to queue.buffering.max.ms, batch.num.messages, and batch.size. */
        if (!batch_ready) {
                /* Wait for more messages or queue.buffering.max.ms
                 * to expire. */
                return 0;
        }

        /* Send Produce requests for this toppar, honouring the
         * queue backpressure threshold. */
        for (reqcnt = 0; reqcnt < max_requests; reqcnt++) {
                r = rd_kafka_ProduceRequest(rkb, rktp, pid, epoch_base_msgid);
                if (likely(r > 0))
                        cnt += r;
                else
                        break;
        }

        /* Update the allowed wake-up time based on remaining messages
         * in the queue. */
        if (cnt > 0) {
                rd_kafka_toppar_lock(rktp);
                batch_ready = rd_kafka_msgq_allow_wakeup_at(
                    &rktp->rktp_msgq, &rktp->rktp_xmit_msgq, next_wakeup, now,
                    flushing ? 1 : rkb->rkb_rk->rk_conf.buffering_max_us,
                    /* Batch message count threshold */
                    rkb->rkb_rk->rk_conf.batch_num_messages,
                    /* Batch total size threshold */
                    rkb->rkb_rk->rk_conf.batch_size);
                rd_kafka_toppar_unlock(rktp);
        }

        return cnt;
}



/**
 * @brief Produce from all toppars assigned to this broker.
 *
 * @param next_wakeup is updated if the next IO/ops timeout should be
 *                    less than the input value (i.e., sooner).
 *
 * @returns the total number of messages produced.
 */
static int rd_kafka_broker_produce_toppars(rd_kafka_broker_t *rkb,
                                           rd_ts_t now,
                                           rd_ts_t *next_wakeup,
                                           rd_bool_t do_timeout_scan) {
        rd_kafka_toppar_t *rktp;
        int cnt                 = 0;
        rd_ts_t ret_next_wakeup = *next_wakeup;
        rd_kafka_pid_t pid      = RD_KAFKA_PID_INITIALIZER;
        rd_bool_t may_send      = rd_true;
        rd_bool_t flushing      = rd_false;

        /* Round-robin serve each toppar. */
        rktp = rkb->rkb_active_toppar_next;
        if (unlikely(!rktp))
                return 0;

        if (rd_kafka_is_idempotent(rkb->rkb_rk)) {
                /* Idempotent producer: get a copy of the current pid. */
                pid = rd_kafka_idemp_get_pid(rkb->rkb_rk);

                /* If we don't have a valid pid, or the transaction state
                 * prohibits sending messages, return immedatiely,
                 * unless the per-partition timeout scan needs to run.
                 * The broker threads are woken up when a PID is acquired
                 * or the transaction state changes. */
                if (!rd_kafka_pid_valid(pid))
                        may_send = rd_false;
                else if (rd_kafka_is_transactional(rkb->rkb_rk) &&
                         !rd_kafka_txn_may_send_msg(rkb->rkb_rk))
                        may_send = rd_false;

                if (!may_send && !do_timeout_scan)
                        return 0;
        }

        flushing = may_send && rd_atomic32_get(&rkb->rkb_rk->rk_flushing) > 0;

        do {
                rd_ts_t this_next_wakeup = ret_next_wakeup;

                /* Try producing toppar */
                cnt += rd_kafka_toppar_producer_serve(
                    rkb, rktp, pid, now, &this_next_wakeup, do_timeout_scan,
                    may_send, flushing);

                rd_kafka_set_next_wakeup(&ret_next_wakeup, this_next_wakeup);

        } while ((rktp = CIRCLEQ_LOOP_NEXT(&rkb->rkb_active_toppars, rktp,
                                           rktp_activelink)) !=
                 rkb->rkb_active_toppar_next);

        /* Update next starting toppar to produce in round-robin list. */
        rd_kafka_broker_active_toppar_next(
            rkb,
            CIRCLEQ_LOOP_NEXT(&rkb->rkb_active_toppars, rktp, rktp_activelink));

        *next_wakeup = ret_next_wakeup;

        return cnt;
}

/**
 * @brief Producer serving
 */
static void rd_kafka_broker_producer_serve(rd_kafka_broker_t *rkb,
                                           rd_ts_t abs_timeout) {
        rd_interval_t timeout_scan;
        unsigned int initial_state = rkb->rkb_state;
        rd_ts_t now;
        int cnt = 0;

        rd_interval_init(&timeout_scan);

        rd_kafka_assert(rkb->rkb_rk, thrd_is_current(rkb->rkb_thread));

        rd_kafka_broker_lock(rkb);

        while (!rd_kafka_broker_terminating(rkb) &&
               rkb->rkb_state == initial_state &&
               (abs_timeout > (now = rd_clock()))) {
                rd_bool_t do_timeout_scan;
                rd_ts_t next_wakeup = abs_timeout;
                rd_bool_t overshot;

                rd_kafka_broker_unlock(rkb);

                /* Perform timeout scan on first iteration, thus
                 * on each state change, to make sure messages in
                 * partition rktp_xmit_msgq are timed out before
                 * being attempted to re-transmit. */
                overshot = rd_interval(&timeout_scan, 1000 * 1000, now) >= 0;
                do_timeout_scan = cnt++ == 0 || overshot;

                rd_kafka_broker_produce_toppars(rkb, now, &next_wakeup,
                                                do_timeout_scan);

                /* Check and move retry buffers */
                if (unlikely(rd_atomic32_get(&rkb->rkb_retrybufs.rkbq_cnt) > 0))
                        rd_kafka_broker_retry_bufs_move(rkb, &next_wakeup);

                if (rd_kafka_broker_ops_io_serve(rkb, next_wakeup))
                        return; /* Wakeup */

                rd_kafka_broker_lock(rkb);
        }

        rd_kafka_broker_unlock(rkb);
}



/**
 * Consumer serving
 */
static void rd_kafka_broker_consumer_serve(rd_kafka_broker_t *rkb,
                                           rd_ts_t abs_timeout) {
        unsigned int initial_state = rkb->rkb_state;
        rd_ts_t now;

        rd_kafka_assert(rkb->rkb_rk, thrd_is_current(rkb->rkb_thread));

        rd_kafka_broker_lock(rkb);

        while (!rd_kafka_broker_terminating(rkb) &&
               rkb->rkb_state == initial_state &&
               abs_timeout > (now = rd_clock())) {
                rd_ts_t min_backoff;

                rd_kafka_broker_unlock(rkb);

                /* Serve toppars */
                min_backoff = rd_kafka_broker_consumer_toppars_serve(rkb);
                if (rkb->rkb_ts_fetch_backoff > now &&
                    rkb->rkb_ts_fetch_backoff < min_backoff)
                        min_backoff = rkb->rkb_ts_fetch_backoff;

                if (min_backoff < RD_TS_MAX &&
                    rkb->rkb_state != RD_KAFKA_BROKER_STATE_UP) {
                        /* There are partitions to fetch but the
                         * connection is not up. */
                        rkb->rkb_persistconn.internal++;
                }

                /* Send Fetch request message for all underflowed toppars
                 * if the connection is up and there are no outstanding
                 * fetch requests for this connection. */
                if (!rkb->rkb_fetching &&
                    rkb->rkb_state == RD_KAFKA_BROKER_STATE_UP) {
                        if (min_backoff < now) {
                                rd_kafka_broker_fetch_toppars(rkb, now);
                                min_backoff = abs_timeout;
                        } else if (min_backoff < RD_TS_MAX)
                                rd_rkb_dbg(rkb, FETCH, "FETCH",
                                           "Fetch backoff for %" PRId64 "ms",
                                           (min_backoff - now) / 1000);
                } else {
                        /* Nothing needs to be done, next wakeup
                         * is from ops, state change, IO, or this timeout */
                        min_backoff = abs_timeout;
                }

                /* Check and move retry buffers */
                if (unlikely(rd_atomic32_get(&rkb->rkb_retrybufs.rkbq_cnt) > 0))
                        rd_kafka_broker_retry_bufs_move(rkb, &min_backoff);

                if (min_backoff > abs_timeout)
                        min_backoff = abs_timeout;

                if (rd_kafka_broker_ops_io_serve(rkb, min_backoff))
                        return; /* Wakeup */

                rd_kafka_broker_lock(rkb);
        }

        rd_kafka_broker_unlock(rkb);
}



/**
 * @brief Check if connections.max.idle.ms has been exceeded and if so
 *        close the connection.
 *
 * @remark Must only be called if connections.max.idle.ms > 0 and
 *         the current broker state is UP (or UPDATE).
 *
 * @locality broker thread
 */
static RD_INLINE void rd_kafka_broker_idle_check(rd_kafka_broker_t *rkb) {
        rd_ts_t ts_send          = rd_atomic64_get(&rkb->rkb_c.ts_send);
        rd_ts_t ts_recv          = rd_atomic64_get(&rkb->rkb_c.ts_recv);
        rd_ts_t ts_last_activity = RD_MAX(ts_send, ts_recv);
        int idle_ms;

        /* If nothing has been sent yet, use the connection time as
         * last activity. */
        if (unlikely(!ts_last_activity))
                ts_last_activity = rkb->rkb_ts_state;

        idle_ms = (int)((rd_clock() - ts_last_activity) / 1000);

        if (likely(idle_ms < rkb->rkb_rk->rk_conf.connections_max_idle_ms))
                return;

        rd_kafka_broker_fail(rkb, LOG_DEBUG, RD_KAFKA_RESP_ERR__TRANSPORT,
                             "Connection max idle time exceeded "
                             "(%dms since last activity)",
                             idle_ms);
}


/**
 * @brief Serve broker thread according to client type.
 *        May be called in any broker state.
 *
 * This function is to be called from the state machine in
 * rd_kafka_broker_thread_main, and will return when
 * there was a state change, or the handle is terminating.
 *
 * Broker threads are triggered by three things:
 *  - Ops from other parts of librdkafka / app.
 *    This is the rkb_ops queue which is served from
 *    rd_kafka_broker_ops_io_serve().
 *  - IO from broker socket.
 *    The ops queue is also IO-triggered to provide
 *    quick wakeup when thread is blocking on IO.
 *    Also serverd from rd_kafka_broker_ops_io_serve().
 *    When there is no broker socket only the ops
 *    queue is served.
 *  - Ops/IO timeout when there were no ops or
 *    IO events within a variable timeout.
 *
 * For each iteration of the loops in producer_serve(), consumer_serve(),
 * etc, the Ops and IO are polled, and the client type specific
 * logic is executed. For the consumer this logic checks which partitions
 * to fetch or backoff, and sends Fetch requests.
 * The producer checks for messages to batch and transmit.
 * All types check for request timeouts, etc.
 *
 * Wakeups
 * =======
 * The logic returns a next wakeup time which controls how long the
 * next Ops/IO poll may block before the logic wants to run again;
 * this is typically controlled by `linger.ms` on the Producer
 * and fetch backoffs on the consumer.
 *
 * Remote threads may also want to wake up the Ops/IO poll so that
 * the logic is run more quickly. For example when a new message
 * is enqueued by produce() it is important that it is batched
 * and transmitted within the configured `linger.ms`.
 *
 * Any op enqueued on the broker ops queue (rkb_ops) will automatically
 * trigger a wakeup of the broker thread (either by wakeup_fd IO event
 * or by the conditional variable of rkb_ops being triggered - or both).
 *
 * Produced messages are not enqueued on the rkb_ops queue but on
 * the partition's rktp_msgq message queue. To provide quick wakeups
 * the partition has a reference to the partition's current leader broker
 * thread's rkb_ops queue, rktp_msgq_wakeup_q.
 * When enqueuing a message on the partition queue and the queue was
 * previously empty, the rktp_msgq_wakeup_q (which is rkb_ops) is woken up
 * by rd_kafka_q_yield(), which sets a YIELD flag and triggers the cond var
 * to wake up the broker thread (without allocating and enqueuing an rko).
 * This also triggers the wakeup_fd of rkb_ops, if necessary.
 *
 * When sparse connections is enabled the broker will linger in the
 * INIT state until there's a need for a connection, in which case
 * it will set its state to DOWN to trigger the connection.
 * This is controlled both by the shared rkb_persistconn atomic counters
 * that may be updated from other parts of the code, as well as the
 * temporary per broker_serve() rkb_persistconn.internal counter which
 * is used by the broker handler code to detect if a connection is needed,
 * such as when a partition is being produced to.
 *
 *
 * @param timeout_ms The maximum timeout for blocking Ops/IO.
 *
 * @locality broker thread
 * @locks none
 */
static void rd_kafka_broker_serve(rd_kafka_broker_t *rkb, int timeout_ms) {
        rd_ts_t abs_timeout;

        if (unlikely(rd_kafka_broker_or_instance_terminating(rkb) ||
                     timeout_ms == RD_POLL_NOWAIT))
                timeout_ms = 1;
        else if (timeout_ms == RD_POLL_INFINITE)
                timeout_ms = rd_kafka_max_block_ms;

        abs_timeout = rd_timeout_init(timeout_ms);
        /* Must be a valid absolute time from here on. */
        rd_assert(abs_timeout > 0);

        /* rkb_persistconn.internal is the per broker_serve()
         * automatic counter that keeps track of anything
         * in the producer/consumer logic needs this broker connection
         * to be up.
         * The value is reset here on each serve(). If there are queued
         * requests we know right away that a connection is needed. */
        rkb->rkb_persistconn.internal =
            rd_atomic32_get(&rkb->rkb_outbufs.rkbq_cnt) > 0;

        if (rkb->rkb_source == RD_KAFKA_INTERNAL) {
                rd_kafka_broker_internal_serve(rkb, abs_timeout);
                return;
        }

        if (rkb->rkb_rk->rk_type == RD_KAFKA_PRODUCER)
                rd_kafka_broker_producer_serve(rkb, abs_timeout);
        else if (rkb->rkb_rk->rk_type == RD_KAFKA_CONSUMER)
                rd_kafka_broker_consumer_serve(rkb, abs_timeout);

        if (rkb->rkb_rk->rk_conf.connections_max_idle_ms &&
            rkb->rkb_state == RD_KAFKA_BROKER_STATE_UP)
                rd_kafka_broker_idle_check(rkb);
}


/**
 * @returns true if all broker addresses have been tried.
 *
 * @locality broker thread
 * @locks_required none
 * @locks_acquired none
 */
static rd_bool_t
rd_kafka_broker_addresses_exhausted(const rd_kafka_broker_t *rkb) {
        return !rkb->rkb_rsal || rkb->rkb_rsal->rsal_cnt == 0 ||
               rkb->rkb_rsal->rsal_curr + 1 == rkb->rkb_rsal->rsal_cnt;
}


static int rd_kafka_broker_thread_main(void *arg) {
        rd_kafka_broker_t *rkb = arg;
        rd_kafka_t *rk         = rkb->rkb_rk;
        rd_kafka_op_t *terminate_op;

        rd_kafka_set_thread_name("%s", rkb->rkb_name);
        rd_kafka_set_thread_sysname("rdk:broker%" PRId32, rkb->rkb_nodeid);

        rd_kafka_interceptors_on_thread_start(rk, RD_KAFKA_THREAD_BROKER);

        (void)rd_atomic32_add(&rd_kafka_thread_cnt_curr, 1);

        /* Our own refcount was increased just prior to thread creation,
         * when refcount drops to 1 it is just us left and the broker
         * thread should terminate. */

        /* Acquire lock (which was held by thread creator during creation)
         * to synchronise state. */
        rd_kafka_broker_lock(rkb);
        rd_kafka_broker_unlock(rkb);

        rd_rkb_dbg(rkb, BROKER, "BRKMAIN", "Enter main broker thread");

        while (!rd_kafka_broker_terminating(rkb)) {
                int backoff;
                int r;
                rd_kafka_broker_state_t orig_state;

        redo:
                orig_state = rkb->rkb_state;

                switch (rkb->rkb_state) {
                case RD_KAFKA_BROKER_STATE_INIT:
                        /* Check if there is demand for a connection
                         * to this broker, if so jump to TRY_CONNECT state. */
                        if (!rd_kafka_broker_needs_connection(rkb)) {
                                rd_kafka_broker_serve(rkb,
                                                      rd_kafka_max_block_ms);
                                break;
                        }

                        /* The INIT state also exists so that an initial
                         * connection failure triggers a state transition
                         * which might trigger a ALL_BROKERS_DOWN error. */
                        rd_kafka_broker_lock(rkb);
                        rd_kafka_broker_set_state(
                            rkb, RD_KAFKA_BROKER_STATE_TRY_CONNECT);
                        rd_kafka_broker_unlock(rkb);
                        goto redo; /* effectively a fallthru to TRY_CONNECT */

                case RD_KAFKA_BROKER_STATE_DOWN:
                        rd_kafka_broker_lock(rkb);
                        if (rkb->rkb_rk->rk_conf.sparse_connections)
                                rd_kafka_broker_set_state(
                                    rkb, RD_KAFKA_BROKER_STATE_INIT);
                        else
                                rd_kafka_broker_set_state(
                                    rkb, RD_KAFKA_BROKER_STATE_TRY_CONNECT);
                        rd_kafka_broker_unlock(rkb);
                        goto redo; /* effectively a fallthru to TRY_CONNECT */

                case RD_KAFKA_BROKER_STATE_TRY_CONNECT:
                        if (rkb->rkb_source == RD_KAFKA_INTERNAL) {
                                rd_kafka_broker_lock(rkb);
                                rd_kafka_broker_set_state(
                                    rkb, RD_KAFKA_BROKER_STATE_UP);
                                rd_kafka_broker_unlock(rkb);
                                break;
                        }

                        if (unlikely(
                                rd_kafka_broker_or_instance_terminating(rkb))) {
                                rd_kafka_broker_serve(rkb, 1000);
                                break;
                        }

                        if (!rd_kafka_sasl_ready(rkb->rkb_rk)) {
                                /* SASL provider not yet ready. */
                                rd_kafka_broker_serve(rkb,
                                                      rd_kafka_max_block_ms);
                                /* Continue while loop to try again (as long as
                                 * we are not terminating). */
                                continue;
                        }

                        /* Throttle & jitter reconnects to avoid
                         * thundering horde of reconnecting clients after
                         * a broker / network outage. Issue #403 */
                        backoff =
                            rd_kafka_broker_reconnect_backoff(rkb, rd_clock());
                        if (backoff > 0) {
                                rd_rkb_dbg(rkb, BROKER, "RECONNECT",
                                           "Delaying next reconnect by %dms",
                                           backoff);
                                rd_kafka_broker_serve(rkb, (int)backoff);
                                continue;
                        }

                        /* Initiate asynchronous connection attempt.
                         * Only the host lookup is blocking here. */
                        r = rd_kafka_broker_connect(rkb);
                        if (r == -1) {
                                /* Immediate failure, most likely host
                                 * resolving failed.
                                 * Try the next resolve result until we've
                                 * tried them all, in which case we sleep a
                                 * short while to avoid busy looping. */
                                if (rd_kafka_broker_addresses_exhausted(rkb))
                                        rd_kafka_broker_serve(
                                            rkb, rd_kafka_max_block_ms);
                        } else if (r == 0) {
                                /* Broker has no hostname yet, wait
                                 * for hostname to be set and connection
                                 * triggered by received OP_CONNECT. */
                                rd_kafka_broker_serve(rkb,
                                                      rd_kafka_max_block_ms);
                        } else {
                                /* Connection in progress, state will
                                 * have changed to STATE_CONNECT. */
                        }

                        break;

                case RD_KAFKA_BROKER_STATE_CONNECT:
                case RD_KAFKA_BROKER_STATE_SSL_HANDSHAKE:
                case RD_KAFKA_BROKER_STATE_AUTH_LEGACY:
                case RD_KAFKA_BROKER_STATE_AUTH_REQ:
                case RD_KAFKA_BROKER_STATE_AUTH_HANDSHAKE:
                case RD_KAFKA_BROKER_STATE_APIVERSION_QUERY:
                        /* Asynchronous connect in progress. */
                        rd_kafka_broker_serve(rkb, rd_kafka_max_block_ms);

                        /* Connect failure.
                         * Try the next resolve result until we've
                         * tried them all, in which case we back off the next
                         * connection attempt to avoid busy looping. */
                        if (rkb->rkb_state == RD_KAFKA_BROKER_STATE_DOWN &&
                            rd_kafka_broker_addresses_exhausted(rkb))
                                rd_kafka_broker_update_reconnect_backoff(
                                    rkb, &rkb->rkb_rk->rk_conf, rd_clock());
                        /* If we haven't made progress from the last state, and
                         * if we have exceeded
                         * socket_connection_setup_timeout_ms, then error out.
                         * Don't error out in case this is a reauth, for which
                         * socket_connection_setup_timeout_ms is not
                         * applicable. */
                        else if (
                            rkb->rkb_state == orig_state &&
                            !rkb->rkb_reauth_in_progress &&
                            rd_clock() >=
                                (rkb->rkb_ts_connect +
                                 (rd_ts_t)rk->rk_conf
                                         .socket_connection_setup_timeout_ms *
                                     1000))
                                rd_kafka_broker_fail(
                                    rkb, LOG_WARNING,
                                    RD_KAFKA_RESP_ERR__TRANSPORT,
                                    "Connection setup timed out in state %s",
                                    rd_kafka_broker_state_names
                                        [rkb->rkb_state]);

                        break;

                case RD_KAFKA_BROKER_STATE_REAUTH:
                        /* Since we've already authenticated once, the provider
                         * should be ready. */
                        rd_assert(rd_kafka_sasl_ready(rkb->rkb_rk));

                        /* Since we aren't disconnecting, the transport isn't
                         * destroyed, and as a consequence, some of the SASL
                         * state leaks unless we destroy it before the reauth.
                         */
                        rd_kafka_sasl_close(rkb->rkb_transport);

                        rkb->rkb_reauth_in_progress = rd_true;

                        rd_kafka_broker_connect_auth(rkb);
                        break;

                case RD_KAFKA_BROKER_STATE_UP:
                        rd_kafka_broker_serve(rkb, rd_kafka_max_block_ms);
                        break;
                }

                if (rd_kafka_broker_or_instance_terminating(rkb)) {
                        /* Handle is terminating: fail the send+retry queue
                         * to speed up termination, otherwise we'll
                         * need to wait for request timeouts. */
                        r = rd_kafka_broker_bufq_timeout_scan(
                            rkb, 0, &rkb->rkb_outbufs, NULL, -1,
                            rd_kafka_broker_destroy_error(rk), 0, NULL, 0);
                        r += rd_kafka_broker_bufq_timeout_scan(
                            rkb, 0, &rkb->rkb_retrybufs, NULL, -1,
                            rd_kafka_broker_destroy_error(rk), 0, NULL, 0);
                        rd_rkb_dbg(
                            rkb, BROKER, "TERMINATE",
                            "Handle is terminating in state %s: "
                            "%d refcnts (%p), %d toppar(s), "
                            "%d active toppar(s), "
                            "%d outbufs, %d waitresps, %d retrybufs: "
                            "failed %d request(s) in retry+outbuf",
                            rd_kafka_broker_state_names[rkb->rkb_state],
                            rd_refcnt_get(&rkb->rkb_refcnt), &rkb->rkb_refcnt,
                            rkb->rkb_toppar_cnt, rkb->rkb_active_toppar_cnt,
                            (int)rd_kafka_bufq_cnt(&rkb->rkb_outbufs),
                            (int)rd_kafka_bufq_cnt(&rkb->rkb_waitresps),
                            (int)rd_kafka_bufq_cnt(&rkb->rkb_retrybufs), r);
                }
        }

        /* Disable and drain ops queue.
         * Simply purging the ops queue risks leaving dangling references
         * for ops such as PARTITION_JOIN/PARTITION_LEAVE where the broker
         * reference is not maintained in the rko (but in rktp_next_leader).
         * #1596.
         * Do this before failing the broker to make sure no buffers
         * are enqueued after that. */
        rd_kafka_q_disable(rkb->rkb_ops);
        while (rd_kafka_broker_ops_serve(rkb, RD_POLL_NOWAIT))
                ;

        rd_kafka_broker_fail(rkb, LOG_DEBUG, rd_kafka_broker_destroy_error(rk),
                             "Broker handle is terminating");

        rd_rkb_dbg(rkb, BROKER, "TERMINATE",
                   "Handle terminates in state %s: "
                   "%d refcnts (%p), %d toppar(s), "
                   "%d active toppar(s), "
                   "%d outbufs, %d waitresps, %d retrybufs",
                   rd_kafka_broker_state_names[rkb->rkb_state],
                   rd_refcnt_get(&rkb->rkb_refcnt), &rkb->rkb_refcnt,
                   rkb->rkb_toppar_cnt, rkb->rkb_active_toppar_cnt,
                   (int)rd_kafka_bufq_cnt(&rkb->rkb_outbufs),
                   (int)rd_kafka_bufq_cnt(&rkb->rkb_waitresps),
                   (int)rd_kafka_bufq_cnt(&rkb->rkb_retrybufs));

        rd_dassert(rkb->rkb_state == RD_KAFKA_BROKER_STATE_DOWN);
        if (rkb->rkb_source != RD_KAFKA_INTERNAL) {
                rd_kafka_wrlock(rkb->rkb_rk);
                TAILQ_REMOVE(&rkb->rkb_rk->rk_brokers, rkb, rkb_link);

                if (RD_KAFKA_BROKER_IS_LOGICAL(rkb)) {
                        rd_atomic32_sub(&rkb->rkb_rk->rk_logical_broker_cnt, 1);
                } else if (rkb->rkb_down_reported) {
                        rd_atomic32_sub(&rkb->rkb_rk->rk_broker_down_cnt, 1);
                }
                rd_atomic32_sub(&rkb->rkb_rk->rk_broker_cnt, 1);
                rd_kafka_wrunlock(rkb->rkb_rk);
        }

#if WITH_SSL
        /* Remove OpenSSL per-thread error state to avoid memory leaks */
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
        /*(OpenSSL libraries handle thread init and deinit)
         * https://github.com/openssl/openssl/pull/1048 */
#elif OPENSSL_VERSION_NUMBER >= 0x10000000L
        ERR_remove_thread_state(NULL);
#endif
#endif

        rd_kafka_interceptors_on_thread_exit(rk, RD_KAFKA_THREAD_BROKER);

        rd_atomic32_sub(&rd_kafka_thread_cnt_curr, 1);

        terminate_op = rd_kafka_op_new(RD_KAFKA_OP_TERMINATE);
        terminate_op->rko_u.terminated.rkb = rkb;
        terminate_op->rko_u.terminated.cb =
            rd_kafka_decommissioned_broker_thread_join;
        rd_kafka_q_enq(rk->rk_ops, terminate_op);

        /* Release broker thread reference here and call destroy final. */
        rd_kafka_broker_destroy(rkb);

        return 0;
}


/**
 * Final destructor. Refcnt must be 0.
 */
void rd_kafka_broker_destroy_final(rd_kafka_broker_t *rkb) {

        rd_assert(thrd_is_current(rkb->rkb_thread));
        rd_assert(TAILQ_EMPTY(&rkb->rkb_monitors));
        rd_assert(TAILQ_EMPTY(&rkb->rkb_outbufs.rkbq_bufs));
        rd_assert(TAILQ_EMPTY(&rkb->rkb_waitresps.rkbq_bufs));
        rd_assert(TAILQ_EMPTY(&rkb->rkb_retrybufs.rkbq_bufs));
        rd_assert(TAILQ_EMPTY(&rkb->rkb_toppars));

        if (rkb->rkb_source != RD_KAFKA_INTERNAL &&
            (rkb->rkb_rk->rk_conf.security_protocol ==
                 RD_KAFKA_PROTO_SASL_PLAINTEXT ||
             rkb->rkb_rk->rk_conf.security_protocol == RD_KAFKA_PROTO_SASL_SSL))
                rd_kafka_sasl_broker_term(rkb);

        if (rkb->rkb_wakeup_fd[0] != -1)
                rd_socket_close(rkb->rkb_wakeup_fd[0]);
        if (rkb->rkb_wakeup_fd[1] != -1)
                rd_socket_close(rkb->rkb_wakeup_fd[1]);

        if (rkb->rkb_recv_buf)
                rd_kafka_buf_destroy(rkb->rkb_recv_buf);

        if (rkb->rkb_rsal)
                rd_sockaddr_list_destroy(rkb->rkb_rsal);

        if (rkb->rkb_ApiVersions)
                rd_free(rkb->rkb_ApiVersions);
        rd_free(rkb->rkb_origname);

        rd_kafka_q_purge(rkb->rkb_ops);
        rd_kafka_q_destroy_owner(rkb->rkb_ops);

        rd_avg_destroy(&rkb->rkb_avg_int_latency);
        rd_avg_destroy(&rkb->rkb_avg_outbuf_latency);
        rd_avg_destroy(&rkb->rkb_avg_rtt);
        rd_avg_destroy(&rkb->rkb_avg_throttle);
        rd_avg_destroy(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_rtt);
        rd_avg_destroy(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_rtt);
        rd_avg_destroy(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_throttle);
        rd_avg_destroy(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_throttle);
        rd_avg_destroy(
            &rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_outbuf_latency);
        rd_avg_destroy(
            &rkb->rkb_telemetry.rd_avg_current.rkb_avg_outbuf_latency);

        if (rkb->rkb_rk->rk_type == RD_KAFKA_CONSUMER) {
                rd_avg_destroy(
                    &rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_fetch_latency);
                rd_avg_destroy(
                    &rkb->rkb_telemetry.rd_avg_current.rkb_avg_fetch_latency);
        } else if (rkb->rkb_rk->rk_type == RD_KAFKA_PRODUCER) {
                rd_avg_destroy(
                    &rkb->rkb_telemetry.rd_avg_current.rkb_avg_produce_latency);
                rd_avg_destroy(&rkb->rkb_telemetry.rd_avg_rollover
                                    .rkb_avg_produce_latency);
        }


        mtx_lock(&rkb->rkb_logname_lock);
        rd_free(rkb->rkb_logname);
        rkb->rkb_logname = NULL;
        mtx_unlock(&rkb->rkb_logname_lock);
        mtx_destroy(&rkb->rkb_logname_lock);

        rd_kafka_timer_stop(&rkb->rkb_rk->rk_timers, &rkb->rkb_sasl_reauth_tmr,
                            1 /*lock*/);

        mtx_destroy(&rkb->rkb_lock);

        rd_refcnt_destroy(&rkb->rkb_refcnt);

        rd_free(rkb);
}


/**
 * Returns the internal broker with refcnt increased.
 */
rd_kafka_broker_t *rd_kafka_broker_internal(rd_kafka_t *rk) {
        rd_kafka_broker_t *rkb;

        mtx_lock(&rk->rk_internal_rkb_lock);
        rkb = rk->rk_internal_rkb;
        if (rkb)
                rd_kafka_broker_keep(rkb);
        mtx_unlock(&rk->rk_internal_rkb_lock);

        return rkb;
}


/**
 * Adds a broker with refcount set to 1.
 * If 'source' is RD_KAFKA_INTERNAL an internal broker is added
 * that does not actually represent or connect to a real broker, it is used
 * for serving unassigned toppar's op queues.
 *
 * Locks: rd_kafka_wrlock(rk) must be held
 */
rd_kafka_broker_t *rd_kafka_broker_add(rd_kafka_t *rk,
                                       rd_kafka_confsource_t source,
                                       rd_kafka_secproto_t proto,
                                       const char *name,
                                       uint16_t port,
                                       int32_t nodeid) {
        rd_kafka_broker_t *rkb;
#ifndef _WIN32
        int r;
        sigset_t newset, oldset;
#endif

        rkb = rd_calloc(1, sizeof(*rkb));

        if (source != RD_KAFKA_LOGICAL) {
                rd_kafka_mk_nodename(rkb->rkb_nodename,
                                     sizeof(rkb->rkb_nodename), name, port);
                rd_kafka_mk_brokername(rkb->rkb_name, sizeof(rkb->rkb_name),
                                       proto, rkb->rkb_nodename, nodeid,
                                       source);
        } else {
                /* Logical broker does not have a nodename (address) or port
                 * at initialization. */
                rd_snprintf(rkb->rkb_name, sizeof(rkb->rkb_name), "%s", name);
        }

        rkb->rkb_source   = source;
        rkb->rkb_rk       = rk;
        rkb->rkb_ts_state = rd_clock();
        rkb->rkb_nodeid   = nodeid;
        rkb->rkb_proto    = proto;
        rkb->rkb_port     = port;
        rkb->rkb_origname = rd_strdup(name);

        mtx_init(&rkb->rkb_lock, mtx_plain);
        mtx_init(&rkb->rkb_logname_lock, mtx_plain);
        rkb->rkb_logname = rd_strdup(rkb->rkb_name);
        TAILQ_INIT(&rkb->rkb_toppars);
        CIRCLEQ_INIT(&rkb->rkb_active_toppars);
        TAILQ_INIT(&rkb->rkb_monitors);
        rd_kafka_bufq_init(&rkb->rkb_outbufs);
        rd_kafka_bufq_init(&rkb->rkb_waitresps);
        rd_kafka_bufq_init(&rkb->rkb_retrybufs);
        rkb->rkb_ops = rd_kafka_q_new(rk);
        rd_avg_init(&rkb->rkb_avg_int_latency, RD_AVG_GAUGE, 0, 100 * 1000, 2,
                    rk->rk_conf.stats_interval_ms);
        rd_avg_init(&rkb->rkb_avg_outbuf_latency, RD_AVG_GAUGE, 0, 100 * 1000,
                    2, rk->rk_conf.stats_interval_ms);
        rd_avg_init(&rkb->rkb_avg_rtt, RD_AVG_GAUGE, 0, 500 * 1000, 2,
                    rk->rk_conf.stats_interval_ms);
        rd_avg_init(&rkb->rkb_avg_throttle, RD_AVG_GAUGE, 0, 5000 * 1000, 2,
                    rk->rk_conf.stats_interval_ms);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_rtt,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2,
                    rk->rk_conf.enable_metrics_push);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_rtt,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2,
                    rk->rk_conf.enable_metrics_push);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_throttle,
                    RD_AVG_GAUGE, 0, 5000 * 1000, 2,
                    rk->rk_conf.enable_metrics_push);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_throttle,
                    RD_AVG_GAUGE, 0, 5000 * 1000, 2,
                    rk->rk_conf.enable_metrics_push);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_outbuf_latency,
                    RD_AVG_GAUGE, 0, 100 * 1000, 2,
                    rk->rk_conf.enable_metrics_push);
        rd_avg_init(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_outbuf_latency,
                    RD_AVG_GAUGE, 0, 100 * 1000, 2,
                    rk->rk_conf.enable_metrics_push);

        if (rk->rk_type == RD_KAFKA_CONSUMER) {
                rd_avg_init(
                    &rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_fetch_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2,
                    rk->rk_conf.enable_metrics_push);
                rd_avg_init(
                    &rkb->rkb_telemetry.rd_avg_current.rkb_avg_fetch_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2,
                    rk->rk_conf.enable_metrics_push);
        } else if (rk->rk_type == RD_KAFKA_PRODUCER) {
                rd_avg_init(
                    &rkb->rkb_telemetry.rd_avg_current.rkb_avg_produce_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
                rd_avg_init(
                    &rkb->rkb_telemetry.rd_avg_rollover.rkb_avg_produce_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2, rd_true);
        }

        rd_refcnt_init(&rkb->rkb_refcnt, 0);
        rd_kafka_broker_keep(rkb); /* rk_broker's refcount */

        rkb->rkb_reconnect_backoff_ms = rk->rk_conf.reconnect_backoff_ms;
        rd_atomic32_init(&rkb->rkb_persistconn.coord, 0);
        rd_atomic32_init(&rkb->termination_in_progress, 0);

        rd_atomic64_init(&rkb->rkb_c.ts_send, 0);
        rd_atomic64_init(&rkb->rkb_c.ts_recv, 0);

        /* ApiVersion fallback interval */
        if (rkb->rkb_rk->rk_conf.api_version_request) {
                rd_interval_init(&rkb->rkb_ApiVersion_fail_intvl);
                rd_interval_fixed(
                    &rkb->rkb_ApiVersion_fail_intvl,
                    (rd_ts_t)rkb->rkb_rk->rk_conf.api_version_fallback_ms *
                        1000);
        }

        rd_interval_init(&rkb->rkb_suppress.unsupported_compression);
        rd_interval_init(&rkb->rkb_suppress.unsupported_kip62);
        rd_interval_init(&rkb->rkb_suppress.fail_error);

#ifndef _WIN32
        /* Block all signals in newly created thread.
         * To avoid race condition we block all signals in the calling
         * thread, which the new thread will inherit its sigmask from,
         * and then restore the original sigmask of the calling thread when
         * we're done creating the thread.
         * NOTE: term_sig remains unblocked since we use it on termination
         *       to quickly interrupt system calls. */
        sigemptyset(&oldset);
        sigfillset(&newset);
        if (rkb->rkb_rk->rk_conf.term_sig)
                sigdelset(&newset, rkb->rkb_rk->rk_conf.term_sig);
        pthread_sigmask(SIG_SETMASK, &newset, &oldset);
#endif

        /*
         * Fd-based queue wake-ups using a non-blocking pipe.
         * Writes are best effort, if the socket queue is full
         * the write fails (silently) but this has no effect on latency
         * since the POLLIN flag will already have been raised for fd.
         */
        rkb->rkb_wakeup_fd[0] = -1;
        rkb->rkb_wakeup_fd[1] = -1;

#ifndef _WIN32
        if ((r = rd_pipe_nonblocking(rkb->rkb_wakeup_fd)) == -1) {
                rd_rkb_log(rkb, LOG_ERR, "WAKEUPFD",
                           "Failed to setup broker queue wake-up fds: "
                           "%s: disabling low-latency mode",
                           rd_strerror(r));

        } else if (source == RD_KAFKA_INTERNAL) {
                /* nop: internal broker has no IO transport. */

        } else {
                char onebyte = 1;

                rd_rkb_dbg(rkb, QUEUE, "WAKEUPFD",
                           "Enabled low-latency ops queue wake-ups");
                rd_kafka_q_io_event_enable(rkb->rkb_ops, rkb->rkb_wakeup_fd[1],
                                           &onebyte, sizeof(onebyte));
        }
#endif

        /* Lock broker's lock here to synchronise state, i.e., hold off
         * the broker thread until we've finalized the rkb. */
        rd_kafka_broker_lock(rkb);
        rd_kafka_broker_keep(rkb); /* broker thread's refcnt */
        if (thrd_create(&rkb->rkb_thread, rd_kafka_broker_thread_main, rkb) !=
            thrd_success) {
                rd_kafka_broker_unlock(rkb);

                rd_kafka_log(rk, LOG_CRIT, "THREAD",
                             "Unable to create broker thread");

                /* Send ERR op back to application for processing. */
                rd_kafka_op_err(rk, RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE,
                                "Unable to create broker thread");

                rd_free(rkb);

#ifndef _WIN32
                /* Restore sigmask of caller */
                pthread_sigmask(SIG_SETMASK, &oldset, NULL);
#endif

                return NULL;
        }

        if (rkb->rkb_source != RD_KAFKA_INTERNAL) {
                if (rk->rk_conf.security_protocol ==
                        RD_KAFKA_PROTO_SASL_PLAINTEXT ||
                    rk->rk_conf.security_protocol == RD_KAFKA_PROTO_SASL_SSL)
                        rd_kafka_sasl_broker_init(rkb);

                /* Insert broker at head of list, idea is that
                 * newer brokers are more relevant than old ones,
                 * and in particular LEARNED brokers are more relevant
                 * than CONFIGURED (bootstrap) and LOGICAL brokers. */
                TAILQ_INSERT_HEAD(&rkb->rkb_rk->rk_brokers, rkb, rkb_link);
                (void)rd_atomic32_add(&rkb->rkb_rk->rk_broker_cnt, 1);

                if (rkb->rkb_nodeid != -1 && !RD_KAFKA_BROKER_IS_LOGICAL(rkb)) {
                        rd_list_add(&rkb->rkb_rk->rk_broker_by_id, rkb);
                        rd_list_sort(&rkb->rkb_rk->rk_broker_by_id,
                                     rd_kafka_broker_cmp_by_id);
                }

                rd_rkb_dbg(rkb, BROKER, "BROKER",
                           "Added new broker with NodeId %" PRId32,
                           rkb->rkb_nodeid);
        }

        /* Call on_broker_state_change interceptors */
        rd_kafka_interceptors_on_broker_state_change(
            rk, rkb->rkb_nodeid, rd_kafka_secproto_names[rkb->rkb_proto],
            rkb->rkb_origname, rkb->rkb_port,
            rd_kafka_broker_state_names[rkb->rkb_state]);

        rd_kafka_broker_unlock(rkb);

        /* Add broker state monitor for the coordinator request to use.
         * This is needed by the transactions implementation and DeleteGroups.
         */
        rd_kafka_broker_monitor_add(&rkb->rkb_coord_monitor, rkb, rk->rk_ops,
                                    rd_kafka_coord_rkb_monitor_cb);


#ifndef _WIN32
        /* Restore sigmask of caller */
        pthread_sigmask(SIG_SETMASK, &oldset, NULL);
#endif

        return rkb;
}


/**
 * @brief Adds a logical broker.
 *
 *        Logical brokers act just like any broker handle, but will not have
 *        an initial address set. The address (or nodename is it is called
 *        internally) can be set from another broker handle
 *        by calling rd_kafka_broker_set_nodename().
 *
 *        This allows maintaining a logical group coordinator broker
 *        handle that can ambulate between real broker addresses.
 *
 *        Logical broker constraints:
 *         - will not have a broker-id set (-1).
 *         - will not have a port set (0).
 *         - the address for the broker may change.
 *         - the name of broker will not correspond to the address,
 *           but the \p name given here.
 *
 * @returns a new broker, holding a refcount for the caller.
 *
 * @locality any rdkafka thread
 * @locks none
 */
rd_kafka_broker_t *rd_kafka_broker_add_logical(rd_kafka_t *rk,
                                               const char *name) {
        rd_kafka_broker_t *rkb;

        rd_kafka_wrlock(rk);
        rkb = rd_kafka_broker_add(rk, RD_KAFKA_LOGICAL,
                                  rk->rk_conf.security_protocol, name,
                                  0 /*port*/, -1 /*brokerid*/);
        rd_assert(rkb && *"failed to create broker thread");
        rd_kafka_wrunlock(rk);

        rd_atomic32_add(&rk->rk_logical_broker_cnt, 1);

        rd_dassert(RD_KAFKA_BROKER_IS_LOGICAL(rkb));
        rd_kafka_broker_keep(rkb);
        return rkb;
}


/**
 * @brief Update the nodename (address) of broker \p rkb
 *        with the nodename from broker \p from_rkb (may be NULL).
 *
 *        If \p rkb is connected, the connection will be torn down.
 *        A new connection may be attempted to the new address
 *        if a persistent connection is needed (standard connection rules).
 *
 *        The broker's logname is also updated to include \p from_rkb's
 *        broker id.
 *
 * @param from_rkb Use the nodename from this broker. If NULL, clear
 *                 the \p rkb nodename.
 *
 * @remark Must only be called for logical brokers.
 *
 * @locks none
 */
void rd_kafka_broker_set_nodename(rd_kafka_broker_t *rkb,
                                  rd_kafka_broker_t *from_rkb) {
        char nodename[RD_KAFKA_NODENAME_SIZE];
        char brokername[RD_KAFKA_NODENAME_SIZE];
        int32_t nodeid;
        rd_bool_t changed = rd_false;

        rd_assert(RD_KAFKA_BROKER_IS_LOGICAL(rkb));

        rd_assert(rkb != from_rkb);

        /* Get nodename from from_rkb */
        if (from_rkb) {
                rd_kafka_broker_lock(from_rkb);
                rd_strlcpy(nodename, from_rkb->rkb_nodename, sizeof(nodename));
                nodeid = from_rkb->rkb_nodeid;
                rd_kafka_broker_unlock(from_rkb);
        } else {
                *nodename = '\0';
                nodeid    = -1;
        }

        /* Set nodename on rkb */
        rd_kafka_broker_lock(rkb);
        if (strcmp(rkb->rkb_nodename, nodename)) {
                rd_rkb_dbg(rkb, BROKER, "NODENAME",
                           "Broker nodename changed from \"%s\" to \"%s\"",
                           rkb->rkb_nodename, nodename);
                rd_strlcpy(rkb->rkb_nodename, nodename,
                           sizeof(rkb->rkb_nodename));
                rkb->rkb_nodename_epoch++;
                changed = rd_true;
        }
        rd_kafka_broker_unlock(rkb);

        /* Update the log name to include (or exclude) the nodeid.
         * The nodeid is appended as "..logname../nodeid" */
        rd_kafka_mk_brokername(brokername, sizeof(brokername), rkb->rkb_proto,
                               rkb->rkb_name, nodeid, rkb->rkb_source);

        rd_kafka_broker_set_logname(rkb, brokername);

        if (!changed)
                return;

        /* Trigger a disconnect & reconnect */
        rd_kafka_broker_schedule_connection(rkb);
}


/**
 * @brief Find broker by nodeid (not -1) and
 *        possibly filtered by state (unless -1).
 *
 * @param do_connect If sparse connections are enabled and the broker is found
 *                   but not up, a connection will be triggered.
 *
 * @locks: rd_kafka_*lock() MUST be held
 * @remark caller must release rkb reference by rd_kafka_broker_destroy()
 */
rd_kafka_broker_t *rd_kafka_broker_find_by_nodeid0_fl(const char *func,
                                                      int line,
                                                      rd_kafka_t *rk,
                                                      int32_t nodeid,
                                                      int state,
                                                      rd_bool_t do_connect) {
        rd_kafka_broker_t *rkb;
        rd_kafka_broker_t skel = {.rkb_nodeid = nodeid};

        if (rd_kafka_terminating(rk))
                return NULL;

        rkb = rd_list_find(&rk->rk_broker_by_id, &skel,
                           rd_kafka_broker_cmp_by_id);

        if (!rkb)
                return NULL;

        if (state != -1) {
                int broker_state;
                rd_kafka_broker_lock(rkb);
                broker_state = (int)rkb->rkb_state;
                rd_kafka_broker_unlock(rkb);

                if (broker_state != state) {
                        if (do_connect &&
                            broker_state == RD_KAFKA_BROKER_STATE_INIT &&
                            rk->rk_conf.sparse_connections)
                                rd_kafka_broker_schedule_connection(rkb);
                        return NULL;
                }
        }

        rd_kafka_broker_keep_fl(func, line, rkb);
        return rkb;
}

/**
 * Locks: rd_kafka_rdlock(rk) must be held
 * NOTE: caller must release rkb reference by rd_kafka_broker_destroy()
 */
static rd_kafka_broker_t *rd_kafka_broker_find(rd_kafka_t *rk,
                                               rd_kafka_secproto_t proto,
                                               const char *name,
                                               uint16_t port) {
        rd_kafka_broker_t *rkb;
        char nodename[RD_KAFKA_NODENAME_SIZE];

        rd_kafka_mk_nodename(nodename, sizeof(nodename), name, port);

        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                if (rd_kafka_broker_or_instance_terminating(rkb) ||
                    RD_KAFKA_BROKER_IS_LOGICAL(rkb))
                        continue;

                rd_kafka_broker_lock(rkb);
                if (rkb->rkb_proto == proto &&
                    !strcmp(rkb->rkb_nodename, nodename)) {
                        rd_kafka_broker_keep(rkb);
                        rd_kafka_broker_unlock(rkb);
                        return rkb;
                }
                rd_kafka_broker_unlock(rkb);
        }

        return NULL;
}


/**
 * Parse a broker host name.
 * The string 'name' is modified and null-terminated portions of it
 * are returned in 'proto', 'host', and 'port'.
 *
 * Returns 0 on success or -1 on parse error.
 */
static int rd_kafka_broker_name_parse(rd_kafka_t *rk,
                                      char **name,
                                      rd_kafka_secproto_t *proto,
                                      const char **host,
                                      uint16_t *port) {
        char *s = *name;
        char *orig;
        char *n, *t, *t2;

        /* Save a temporary copy of the original name for logging purposes */
        rd_strdupa(&orig, *name);

        /* Find end of this name (either by delimiter or end of string */
        if ((n = strchr(s, ',')))
                *n = '\0';
        else
                n = s + strlen(s) - 1;


        /* Check if this looks like an url. */
        if ((t = strstr(s, "://"))) {
                int i;
                /* "proto://host[:port]" */

                if (t == s) {
                        rd_kafka_log(rk, LOG_WARNING, "BROKER",
                                     "Broker name \"%s\" parse error: "
                                     "empty protocol name",
                                     orig);
                        return -1;
                }

                /* Make protocol uppercase */
                for (t2 = s; t2 < t; t2++)
                        *t2 = toupper(*t2);

                *t = '\0';

                /* Find matching protocol by name. */
                for (i = 0; i < RD_KAFKA_PROTO_NUM; i++)
                        if (!rd_strcasecmp(s, rd_kafka_secproto_names[i]))
                                break;

                /* Unsupported protocol */
                if (i == RD_KAFKA_PROTO_NUM) {
                        rd_kafka_log(rk, LOG_WARNING, "BROKER",
                                     "Broker name \"%s\" parse error: "
                                     "unsupported protocol \"%s\"",
                                     orig, s);

                        return -1;
                }

                *proto = i;

                /* Enforce protocol */
                if (rk->rk_conf.security_protocol != *proto) {
                        rd_kafka_log(
                            rk, LOG_WARNING, "BROKER",
                            "Broker name \"%s\" parse error: "
                            "protocol \"%s\" does not match "
                            "security.protocol setting \"%s\"",
                            orig, s,
                            rd_kafka_secproto_names[rk->rk_conf
                                                        .security_protocol]);
                        return -1;
                }

                /* Hostname starts here */
                s = t + 3;

                /* Ignore anything that looks like the path part of an URL */
                if ((t = strchr(s, '/')))
                        *t = '\0';

        } else
                *proto = rk->rk_conf.security_protocol; /* Default protocol */


        *port = RD_KAFKA_PORT;
        /* Check if port has been specified, but try to identify IPv6
         * addresses first:
         *  t = last ':' in string
         *  t2 = first ':' in string
         *  If t and t2 are equal then only one ":" exists in name
         *  and thus an IPv4 address with port specified.
         *  Else if not equal and t is prefixed with "]" then it's an
         *  IPv6 address with port specified.
         *  Else no port specified. */
        if ((t = strrchr(s, ':')) &&
            ((t2 = strchr(s, ':')) == t || *(t - 1) == ']')) {
                *t    = '\0';
                *port = atoi(t + 1);
        }

        /* Empty host name -> localhost */
        if (!*s)
                s = "localhost";

        *host = s;
        *name = n + 1; /* past this name. e.g., next name/delimiter to parse */

        return 0;
}

/**
 * @brief Add a broker from a string of type "[proto://]host[:port]" to the list
 * of brokers. *cnt is increased by one if a broker was added, else not.
 */
static void rd_kafka_find_or_add_broker(rd_kafka_t *rk,
                                        rd_kafka_secproto_t proto,
                                        const char *host,
                                        uint16_t port,
                                        int *cnt) {
        rd_kafka_broker_t *rkb = NULL;

        if ((rkb = rd_kafka_broker_find(rk, proto, host, port)) &&
            rkb->rkb_source == RD_KAFKA_CONFIGURED) {
                (*cnt)++;
        } else if (rd_kafka_broker_add(rk, RD_KAFKA_CONFIGURED, proto, host,
                                       port, RD_KAFKA_NODEID_UA) != NULL)
                (*cnt)++;

        /* If rd_kafka_broker_find returned a broker its
         * reference needs to be released
         * See issue #193 */
        if (rkb)
                rd_kafka_broker_destroy(rkb);
}

/**
 * @brief Adds a (csv list of) broker(s).
 * Returns the number of brokers succesfully added.
 *
 * @locality any thread
 * @locks none
 */
int rd_kafka_brokers_add0(rd_kafka_t *rk,
                          const char *brokerlist,
                          rd_bool_t is_bootstrap_server_list) {
        char *s_copy = rd_strdup(brokerlist);
        char *s      = s_copy;
        int cnt      = 0;
        int pre_cnt  = rd_atomic32_get(&rk->rk_broker_cnt);
        rd_sockaddr_inx_t *sinx;
        rd_sockaddr_list_t *sockaddr_list;

        /* Parse comma-separated list of brokers. */
        while (*s) {
                uint16_t port;
                const char *host;
                const char *err_str;
                const char *resolved_FQDN;
                rd_kafka_secproto_t proto;

                if (*s == ',' || *s == ' ') {
                        s++;
                        continue;
                }

                if (rd_kafka_broker_name_parse(rk, &s, &proto, &host, &port) ==
                    -1)
                        break;

                rd_kafka_wrlock(rk);
                if (is_bootstrap_server_list &&
                    rk->rk_conf.client_dns_lookup ==
                        RD_KAFKA_RESOLVE_CANONICAL_BOOTSTRAP_SERVERS_ONLY) {
                        rd_kafka_dbg(rk, ALL, "INIT",
                                     "Canonicalizing bootstrap broker %s:%d",
                                     host, port);
                        sockaddr_list = rd_getaddrinfo(
                            host, RD_KAFKA_PORT_STR, AI_ADDRCONFIG,
                            rk->rk_conf.broker_addr_family, SOCK_STREAM,
                            IPPROTO_TCP, rk->rk_conf.resolve_cb,
                            rk->rk_conf.opaque, &err_str);

                        if (!sockaddr_list) {
                                rd_kafka_log(rk, LOG_WARNING, "BROKER",
                                             "Failed to resolve '%s': %s", host,
                                             err_str);
                                rd_kafka_wrunlock(rk);
                                continue;
                        }

                        RD_SOCKADDR_LIST_FOREACH(sinx, sockaddr_list) {
                                resolved_FQDN = rd_sockaddr2str(
                                    sinx, RD_SOCKADDR2STR_F_RESOLVE);
                                rd_kafka_dbg(
                                    rk, ALL, "INIT",
                                    "Adding broker with resolved hostname %s",
                                    resolved_FQDN);

                                rd_kafka_find_or_add_broker(
                                    rk, proto, resolved_FQDN, port, &cnt);
                        };

                        rd_sockaddr_list_destroy(sockaddr_list);
                } else {
                        rd_kafka_find_or_add_broker(rk, proto, host, port,
                                                    &cnt);
                }

                rd_kafka_wrunlock(rk);
        }

        rd_free(s_copy);

        if (rk->rk_conf.sparse_connections && cnt > 0 && pre_cnt == 0) {
                /* Sparse connections:
                 * If this was the first set of brokers added,
                 * select a random one to trigger the initial cluster
                 * connection. */
                rd_kafka_rdlock(rk);
                rd_kafka_connect_any(rk, "bootstrap servers added");
                rd_kafka_rdunlock(rk);
        }

        return cnt;
}


int rd_kafka_brokers_add(rd_kafka_t *rk, const char *brokerlist) {
        rd_kafka_wrlock(rk);
        rd_list_add(&rk->additional_brokerlists, rd_strdup(brokerlist));
        rd_kafka_wrunlock(rk);
        return rd_kafka_brokers_add0(rk, brokerlist, rd_false);
}


/**
 * @brief Adds a new broker or updates an existing one.
 *
 * @param rkbp if non-NULL, will be set to the broker object with
 *             refcount increased, or NULL on error.
 *
 * @locks none
 * @locality any
 */
void rd_kafka_broker_update(rd_kafka_t *rk,
                            rd_kafka_secproto_t proto,
                            const struct rd_kafka_metadata_broker *mdb,
                            rd_kafka_broker_t **rkbp) {
        rd_kafka_broker_t *rkb;
        char nodename[RD_KAFKA_NODENAME_SIZE];
        int needs_update = 0;

        rd_kafka_mk_nodename(nodename, sizeof(nodename), mdb->host, mdb->port);

        rd_kafka_wrlock(rk);
        if (unlikely(rd_kafka_terminating(rk))) {
                /* Dont update metadata while terminating, do this
                 * after acquiring lock for proper synchronisation */
                rd_kafka_wrunlock(rk);
                if (rkbp)
                        *rkbp = NULL;
                return;
        }

        if ((rkb = rd_kafka_broker_find_by_nodeid(rk, mdb->id))) {
                /* Broker matched by nodeid, see if we need to update
                 * the hostname. */
                if (strcmp(rkb->rkb_nodename, nodename))
                        needs_update = 1;
        } else if ((rkb = rd_kafka_broker_add(rk, RD_KAFKA_LEARNED, proto,
                                              mdb->host, mdb->port, mdb->id))) {
                rd_kafka_broker_keep(rkb);
        }

        rd_kafka_wrunlock(rk);

        if (rkb) {
                /* Existing broker */
                if (needs_update) {
                        rd_kafka_op_t *rko;
                        rko = rd_kafka_op_new(RD_KAFKA_OP_NODE_UPDATE);
                        rd_strlcpy(rko->rko_u.node.nodename, nodename,
                                   sizeof(rko->rko_u.node.nodename));
                        /* Perform a blocking op request so that all
                         * broker-related state, such as the rk broker list,
                         * is up to date by the time this call returns.
                         * Ignore&destroy the response. */
                        rd_kafka_op_err_destroy(
                            rd_kafka_op_req(rkb->rkb_ops, rko, -1));
                }
        }

        if (rkbp)
                *rkbp = rkb;
        else if (rkb)
                rd_kafka_broker_destroy(rkb);
}


/**
 * @returns the broker id, or RD_KAFKA_NODEID_UA if \p rkb is NULL.
 *
 * @locality any
 */
int32_t rd_kafka_broker_id(rd_kafka_broker_t *rkb) {
        if (unlikely(!rkb))
                return RD_KAFKA_NODEID_UA;

        return rkb->rkb_nodeid;
}


/**
 * Returns a thread-safe temporary copy of the broker name.
 * Must not be called more than 4 times from the same expression.
 *
 * Locks: none
 * Locality: any thread
 */
const char *rd_kafka_broker_name(rd_kafka_broker_t *rkb) {
        static RD_TLS char ret[4][RD_KAFKA_NODENAME_SIZE];
        static RD_TLS int reti = 0;

        reti = (reti + 1) % 4;
        mtx_lock(&rkb->rkb_logname_lock);
        rd_snprintf(ret[reti], sizeof(ret[reti]), "%s", rkb->rkb_logname);
        mtx_unlock(&rkb->rkb_logname_lock);

        return ret[reti];
}



/**
 * @brief Send dummy OP to broker thread to wake it up from IO sleep.
 *
 * @locality any
 * @locks any
 */
void rd_kafka_broker_wakeup(rd_kafka_broker_t *rkb, const char *reason) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_WAKEUP);
        rd_kafka_op_set_prio(rko, RD_KAFKA_PRIO_FLASH);
        rd_kafka_q_enq(rkb->rkb_ops, rko);
        rd_rkb_dbg(rkb, QUEUE, "WAKEUP", "Wake-up: %s", reason);
}

/**
 * @brief Wake up all broker threads that are in at least state \p min_state
 *
 * @locality any
 * @locks none: rd_kafka_*lock() MUST NOT be held
 *
 * @returns the number of broker threads woken up
 */
int rd_kafka_all_brokers_wakeup(rd_kafka_t *rk,
                                int min_state,
                                const char *reason) {
        int cnt = 0;
        rd_kafka_broker_t *rkb;

        rd_kafka_rdlock(rk);
        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                int do_wakeup;

                rd_kafka_broker_lock(rkb);
                do_wakeup = (int)rkb->rkb_state >= min_state;
                rd_kafka_broker_unlock(rkb);

                if (do_wakeup) {
                        rd_kafka_broker_wakeup(rkb, reason);
                        cnt += 1;
                }
        }
        rd_kafka_rdunlock(rk);

        if (cnt > 0)
                rd_kafka_dbg(rk, BROKER | RD_KAFKA_DBG_QUEUE, "WAKEUP",
                             "Wake-up sent to %d broker thread%s in "
                             "state >= %s: %s",
                             cnt, cnt > 1 ? "s" : "",
                             rd_kafka_broker_state_names[min_state], reason);

        return cnt;
}

/**
 * @brief Filter out brokers that have at least one connection attempt.
 */
static int rd_kafka_broker_filter_never_connected(rd_kafka_broker_t *rkb,
                                                  void *opaque) {
        return rd_atomic32_get(&rkb->rkb_c.connects);
}

/**
 * @brief Filter out brokers that aren't learned ones.
 */
static int rd_kafka_broker_filter_learned(rd_kafka_broker_t *rkb,
                                          void *opaque) {
        return rkb->rkb_source != RD_KAFKA_LEARNED;
}

/**
 * @brief Filter out brokers that aren't learned ones or
 *        that have at least one connection attempt.
 */
static int
rd_kafka_broker_filter_learned_never_connected(rd_kafka_broker_t *rkb,
                                               void *opaque) {
        return rd_atomic32_get(&rkb->rkb_c.connects) ||
               rkb->rkb_source != RD_KAFKA_LEARNED;
}

static void rd_kafka_connect_any_timer_cb(rd_kafka_timers_t *rkts, void *arg) {
        const char *reason = (const char *)arg;
        rd_kafka_t *rk     = rkts->rkts_rk;
        if (rd_kafka_terminating(rk))
                return;

        /* Acquire the read lock for `rd_kafka_connect_any` */
        rd_kafka_rdlock(rk);
        rd_kafka_connect_any(rk, reason);
        rd_kafka_rdunlock(rk);
}

/**
 * @brief Sparse connections:
 *        Select a random broker to connect to if no brokers are up.
 *
 *        This is a non-blocking call, the connection is
 *        performed by the selected broker thread.
 *
 * @locality any
 * @locks rd_kafka_rdlock() MUST be held
 */
void rd_kafka_connect_any(rd_kafka_t *rk, const char *reason) {
        rd_kafka_broker_t *rkb;
        rd_ts_t suppr;

        /* Don't count connections to logical brokers since they serve
         * a specific purpose (group coordinator) and their connections
         * should not be reused for other purposes.
         * rd_kafka_broker_random() will not return LOGICAL brokers. */
        if (rd_atomic32_get(&rk->rk_broker_up_cnt) > 0 ||
            rd_atomic32_get(&rk->rk_broker_cnt) -
                    rd_atomic32_get(&rk->rk_logical_broker_cnt) ==
                0)
                return;

        mtx_lock(&rk->rk_suppress.sparse_connect_lock);

        suppr = rd_interval(&rk->rk_suppress.sparse_connect_random,
                            rk->rk_conf.sparse_connect_intvl * 1000, 0);
        mtx_unlock(&rk->rk_suppress.sparse_connect_lock);

        if (suppr <= 0) {
                rd_kafka_dbg(rk, BROKER | RD_KAFKA_DBG_GENERIC, "CONNECT",
                             "Not selecting any broker for cluster connection: "
                             "still suppressed for %" PRId64 "ms: %s",
                             -suppr / 1000, reason);
                /* Retry after interval + 1ms has passed */
                rd_kafka_timer_start_oneshot(
                    &rk->rk_timers, &rk->rk_suppress.sparse_connect_random_tmr,
                    rd_false /* don't restart */, 1000LL - suppr,
                    rd_kafka_connect_any_timer_cb, (void *)reason);
                return;
        }

        /* In case there no learned brokers never connected to,
         * 90% of times select a learned broker in init state.
         *
         * This avoids problems after re-bootstrapping that cause
         * the bootstrap brokers to be always preferred
         * given there are learned brokers that already connected and
         * caused ALL_BROKERS_DOWN.
         *
         * If that happens those learned brokers
         * that already connected are never selected unless
         * they disappear and re-appear again as new brokers with 0 connects,
         * so we have to assing a higher probability to it.
         *
         * Additionally we cannot always prefer the learned
         * brokers as their address could have changed and we need to
         * connect to the bootstrap brokers to know that.
         * KIP-1102 `metadata.recovery.rebootstrap.trigger.ms` would
         * be triggered in this case after 5 mins
         * but it's a long time to wait.
         */

        /* First pass:  only match learned brokers never connected to
         *              in state INIT, to try to exhaust
         *              the available brokers so that an
         *              ERR_ALL_BROKERS_DOWN error can be raised. */
        rkb = rd_kafka_broker_random(
            rk, RD_KAFKA_BROKER_STATE_INIT,
            rd_kafka_broker_filter_learned_never_connected, NULL);

#if ENABLE_DEVEL == 1
        if (rkb)
                rd_dassert(rkb->rkb_source == RD_KAFKA_LEARNED);
#endif

        if (!rkb && rd_jitter(0, 9) > 0) { /* 0.9 probability */
                /* Second pass:  only match learned brokers
                 *               in state INIT. */
                rkb = rd_kafka_broker_random(rk, RD_KAFKA_BROKER_STATE_INIT,
                                             rd_kafka_broker_filter_learned,
                                             NULL);

#if ENABLE_DEVEL == 1
                if (rkb)
                        rd_dassert(rkb->rkb_source == RD_KAFKA_LEARNED);
#endif
        }

        /* Third pass:  only match brokers never connected to,
         *              to try to exhaust the available brokers
         *              so that an ERR_ALL_BROKERS_DOWN error
         *              can be raised. */
        if (!rkb)
                rkb = rd_kafka_broker_random(
                    rk, RD_KAFKA_BROKER_STATE_INIT,
                    rd_kafka_broker_filter_never_connected, NULL);

        /* Fourth pass: match any non-connected/non-connecting broker. */
        if (!rkb)
                rkb = rd_kafka_broker_random(rk, RD_KAFKA_BROKER_STATE_INIT,
                                             NULL, NULL);

        if (!rkb) {
                /* No brokers matched:
                 * this happens if all brokers are in > INIT state,
                 * in which case they're already connecting. */

                rd_kafka_dbg(rk, BROKER | RD_KAFKA_DBG_GENERIC, "CONNECT",
                             "Cluster connection already in progress: %s",
                             reason);
                return;
        }

        rd_rkb_dbg(rkb, BROKER | RD_KAFKA_DBG_GENERIC, "CONNECT",
                   "Selected for cluster connection: "
                   "%s (broker has %d connection attempt(s))",
                   reason, rd_atomic32_get(&rkb->rkb_c.connects));

        rd_kafka_broker_schedule_connection(rkb);

        rd_kafka_broker_destroy(rkb); /* refcnt from ..broker_random() */
}



/**
 * @brief Send PURGE queue request to broker.
 *
 * @locality any
 * @locks none
 */
void rd_kafka_broker_purge_queues(rd_kafka_broker_t *rkb,
                                  int purge_flags,
                                  rd_kafka_replyq_t replyq) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_PURGE);
        rd_kafka_op_set_prio(rko, RD_KAFKA_PRIO_FLASH);
        rko->rko_replyq        = replyq;
        rko->rko_u.purge.flags = purge_flags;
        rd_kafka_q_enq(rkb->rkb_ops, rko);
}


/**
 * @brief Handle purge queues request
 *
 * @locality broker thread
 * @locks none
 */
static void rd_kafka_broker_handle_purge_queues(rd_kafka_broker_t *rkb,
                                                rd_kafka_op_t *rko) {
        int purge_flags  = rko->rko_u.purge.flags;
        int inflight_cnt = 0, retry_cnt = 0, outq_cnt = 0, partial_cnt = 0;

        rd_rkb_dbg(rkb, QUEUE | RD_KAFKA_DBG_TOPIC, "PURGE",
                   "Purging queues with flags %s",
                   rd_kafka_purge_flags2str(purge_flags));


        /**
         * First purge any Produce requests to move the
         * messages from the request's message queue to delivery reports.
         */

        /* Purge in-flight ProduceRequests */
        if (purge_flags & RD_KAFKA_PURGE_F_INFLIGHT)
                inflight_cnt = rd_kafka_broker_bufq_timeout_scan(
                    rkb, 1, &rkb->rkb_waitresps, NULL, RD_KAFKAP_Produce,
                    RD_KAFKA_RESP_ERR__PURGE_INFLIGHT, 0, NULL, 0);

        if (purge_flags & RD_KAFKA_PURGE_F_QUEUE) {
                /* Requests in retry queue */
                retry_cnt = rd_kafka_broker_bufq_timeout_scan(
                    rkb, 0, &rkb->rkb_retrybufs, NULL, RD_KAFKAP_Produce,
                    RD_KAFKA_RESP_ERR__PURGE_QUEUE, 0, NULL, 0);

                /* Requests in transmit queue not completely sent yet.
                 * partial_cnt is included in outq_cnt and denotes a request
                 * that has been partially transmitted. */
                outq_cnt = rd_kafka_broker_bufq_timeout_scan(
                    rkb, 0, &rkb->rkb_outbufs, &partial_cnt, RD_KAFKAP_Produce,
                    RD_KAFKA_RESP_ERR__PURGE_QUEUE, 0, NULL, 0);

                /* Purging a partially transmitted request will mess up
                 * the protocol stream, so we need to disconnect from the broker
                 * to get a clean protocol socket. */
                if (partial_cnt)
                        rd_kafka_broker_fail(
                            rkb, LOG_DEBUG, RD_KAFKA_RESP_ERR__PURGE_QUEUE,
                            "Purged %d partially sent request: "
                            "forcing disconnect",
                            partial_cnt);
        }

        rd_rkb_dbg(rkb, QUEUE | RD_KAFKA_DBG_TOPIC, "PURGEQ",
                   "Purged %i in-flight, %i retry-queued, "
                   "%i out-queue, %i partially-sent requests",
                   inflight_cnt, retry_cnt, outq_cnt, partial_cnt);

        /* Purge partition queues */
        if (purge_flags & RD_KAFKA_PURGE_F_QUEUE) {
                rd_kafka_toppar_t *rktp;
                int msg_cnt  = 0;
                int part_cnt = 0;

                TAILQ_FOREACH(rktp, &rkb->rkb_toppars, rktp_rkblink) {
                        int r;

                        r = rd_kafka_toppar_purge_queues(
                            rktp, purge_flags, rd_true /*include xmit msgq*/);
                        if (r > 0) {
                                msg_cnt += r;
                                part_cnt++;
                        }
                }

                rd_rkb_dbg(rkb, QUEUE | RD_KAFKA_DBG_TOPIC, "PURGEQ",
                           "Purged %i message(s) from %d partition(s)", msg_cnt,
                           part_cnt);
        }

        rd_kafka_op_reply(rko, RD_KAFKA_RESP_ERR_NO_ERROR);
}


/**
 * @brief Add toppar to broker's active list list.
 *
 * For consumer this means the fetch list.
 * For producers this is all partitions assigned to this broker.
 *
 * @locality broker thread
 * @locks rktp_lock MUST be held
 */
void rd_kafka_broker_active_toppar_add(rd_kafka_broker_t *rkb,
                                       rd_kafka_toppar_t *rktp,
                                       const char *reason) {
        int is_consumer = rkb->rkb_rk->rk_type == RD_KAFKA_CONSUMER;

        if (is_consumer && rktp->rktp_fetch)
                return; /* Already added */

        CIRCLEQ_INSERT_TAIL(&rkb->rkb_active_toppars, rktp, rktp_activelink);
        rkb->rkb_active_toppar_cnt++;

        if (is_consumer)
                rktp->rktp_fetch = 1;

        if (unlikely(rkb->rkb_active_toppar_cnt == 1))
                rd_kafka_broker_active_toppar_next(rkb, rktp);

        rd_rkb_dbg(rkb, TOPIC, "FETCHADD",
                   "Added %.*s [%" PRId32
                   "] to %s list (%d entries, opv %d, "
                   "%d messages queued): %s",
                   RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                   rktp->rktp_partition, is_consumer ? "fetch" : "active",
                   rkb->rkb_active_toppar_cnt, rktp->rktp_fetch_version,
                   rd_kafka_msgq_len(&rktp->rktp_msgq), reason);
}


/**
 * @brief Remove toppar from active list.
 *
 * Locality: broker thread
 * Locks: none
 */
void rd_kafka_broker_active_toppar_del(rd_kafka_broker_t *rkb,
                                       rd_kafka_toppar_t *rktp,
                                       const char *reason) {
        int is_consumer = rkb->rkb_rk->rk_type == RD_KAFKA_CONSUMER;

        if (is_consumer && !rktp->rktp_fetch)
                return; /* Not added */

        CIRCLEQ_REMOVE(&rkb->rkb_active_toppars, rktp, rktp_activelink);
        rd_kafka_assert(NULL, rkb->rkb_active_toppar_cnt > 0);
        rkb->rkb_active_toppar_cnt--;

        if (is_consumer)
                rktp->rktp_fetch = 0;

        if (rkb->rkb_active_toppar_next == rktp) {
                /* Update next pointer */
                rd_kafka_broker_active_toppar_next(
                    rkb, CIRCLEQ_LOOP_NEXT(&rkb->rkb_active_toppars, rktp,
                                           rktp_activelink));
        }

        rd_rkb_dbg(rkb, TOPIC, "FETCHADD",
                   "Removed %.*s [%" PRId32
                   "] from %s list "
                   "(%d entries, opv %d): %s",
                   RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                   rktp->rktp_partition, is_consumer ? "fetch" : "active",
                   rkb->rkb_active_toppar_cnt, rktp->rktp_fetch_version,
                   reason);
}


/**
 * @brief Schedule connection for \p rkb.
 *        Will trigger disconnection for logical brokers whose nodename
 *        was changed.
 *
 * @locality any
 * @locks none
 */
void rd_kafka_broker_schedule_connection(rd_kafka_broker_t *rkb) {
        rd_kafka_op_t *rko;
        rko = rd_kafka_op_new(RD_KAFKA_OP_CONNECT);
        rd_kafka_op_set_prio(rko, RD_KAFKA_PRIO_FLASH);
        rd_kafka_q_enq(rkb->rkb_ops, rko);
}


/**
 * @brief Add need for persistent connection to \p rkb
 *        with rkb_persistconn atomic counter \p acntp
 *
 * @locality any
 * @locks none
 */
void rd_kafka_broker_persistent_connection_add(rd_kafka_broker_t *rkb,
                                               rd_atomic32_t *acntp) {

        if (rd_atomic32_add(acntp, 1) == 1) {
                /* First one, trigger event. */
                rd_kafka_broker_schedule_connection(rkb);
        }
}


/**
 * @brief Remove need for persistent connection to \p rkb
 *        with rkb_persistconn atomic counter \p acntp
 *
 * @locality any
 * @locks none
 */
void rd_kafka_broker_persistent_connection_del(rd_kafka_broker_t *rkb,
                                               rd_atomic32_t *acntp) {
        int32_t r = rd_atomic32_sub(acntp, 1);
        rd_assert(r >= 0);
}



/**
 * @brief OP_BROKER_MONITOR callback trampoline which
 *        calls the rkbmon's callback.
 *
 * @locality monitoree's op handler thread
 * @locks none
 */
static rd_kafka_op_res_t rd_kafka_broker_monitor_op_cb(rd_kafka_t *rk,
                                                       rd_kafka_q_t *rkq,
                                                       rd_kafka_op_t *rko) {
        if (rko->rko_err != RD_KAFKA_RESP_ERR__DESTROY)
                rko->rko_u.broker_monitor.cb(rko->rko_u.broker_monitor.rkb);
        return RD_KAFKA_OP_RES_HANDLED;
}

/**
 * @brief Trigger ops for registered monitors when the broker
 *        state goes from or to UP.
 *
 * @locality broker thread
 * @locks rkb_lock MUST be held
 */
static void rd_kafka_broker_trigger_monitors(rd_kafka_broker_t *rkb) {
        rd_kafka_broker_monitor_t *rkbmon;

        TAILQ_FOREACH(rkbmon, &rkb->rkb_monitors, rkbmon_link) {
                rd_kafka_op_t *rko =
                    rd_kafka_op_new_cb(rkb->rkb_rk, RD_KAFKA_OP_BROKER_MONITOR,
                                       rd_kafka_broker_monitor_op_cb);
                rd_kafka_broker_keep(rkb);
                rko->rko_u.broker_monitor.rkb = rkb;
                rko->rko_u.broker_monitor.cb  = rkbmon->rkbmon_cb;
                rd_kafka_q_enq(rkbmon->rkbmon_q, rko);
        }
}


/**
 * @brief Adds a monitor for when the broker goes up or down.
 *
 * The callback will be triggered on the caller's op queue handler thread.
 *
 * Use rd_kafka_broker_is_up() in your callback to get the current
 * state of the broker, since it might have changed since the event
 * was enqueued.
 *
 * @param rkbmon monitoree's monitor.
 * @param rkb broker to monitor.
 * @param rkq queue for event op.
 * @param callback callback to be triggered from \p rkq's op handler.
 * @opaque opaque passed to callback.
 *
 * @locks none
 * @locality any
 */
void rd_kafka_broker_monitor_add(rd_kafka_broker_monitor_t *rkbmon,
                                 rd_kafka_broker_t *rkb,
                                 rd_kafka_q_t *rkq,
                                 void (*callback)(rd_kafka_broker_t *rkb)) {
        rd_assert(!rkbmon->rkbmon_rkb);
        rkbmon->rkbmon_rkb = rkb;
        rkbmon->rkbmon_q   = rkq;
        rd_kafka_q_keep(rkbmon->rkbmon_q);
        rkbmon->rkbmon_cb = callback;

        rd_kafka_broker_keep(rkb);

        rd_kafka_broker_lock(rkb);
        TAILQ_INSERT_TAIL(&rkb->rkb_monitors, rkbmon, rkbmon_link);
        rd_kafka_broker_unlock(rkb);
}


/**
 * @brief Removes a monitor previously added with
 *        rd_kafka_broker_monitor_add().
 *
 * @warning The rkbmon's callback may still be called after
 *          _del() has been called due to the buffering nature
 *          of op queues.
 *
 * @locks none
 * @locality any
 */
void rd_kafka_broker_monitor_del(rd_kafka_broker_monitor_t *rkbmon) {
        rd_kafka_broker_t *rkb = rkbmon->rkbmon_rkb;

        if (!rkb)
                return;

        rd_kafka_broker_lock(rkb);
        rkbmon->rkbmon_rkb = NULL;
        rd_kafka_q_destroy(rkbmon->rkbmon_q);
        TAILQ_REMOVE(&rkb->rkb_monitors, rkbmon, rkbmon_link);
        rd_kafka_broker_unlock(rkb);

        rd_kafka_broker_destroy(rkb);
}

/**
 * @brief Starts the reauth timer for this broker.
 *        If connections_max_reauth_ms=0, then no timer is set.
 *
 * @locks none
 * @locality broker thread
 */
void rd_kafka_broker_start_reauth_timer(rd_kafka_broker_t *rkb,
                                        int64_t connections_max_reauth_ms) {
        /* Timer should not already be started. It indicates that we're about to
         * schedule an extra reauth, but this shouldn't be a cause for failure
         * in production use cases, so, clear the timer. */
        if (rd_kafka_timer_is_started(&rkb->rkb_rk->rk_timers,
                                      &rkb->rkb_sasl_reauth_tmr))
                rd_kafka_timer_stop(&rkb->rkb_rk->rk_timers,
                                    &rkb->rkb_sasl_reauth_tmr, 1 /*lock*/);

        if (connections_max_reauth_ms == 0)
                return;

        rd_kafka_timer_start_oneshot(
            &rkb->rkb_rk->rk_timers, &rkb->rkb_sasl_reauth_tmr, rd_false,
            connections_max_reauth_ms * 900 /* 90% * microsecond*/,
            rd_kafka_broker_start_reauth_cb, (void *)rkb);
}

/**
 * @brief Starts the reauth process for the broker rkb.
 *
 * @locks none
 * @locality main thread
 */
void rd_kafka_broker_start_reauth_cb(rd_kafka_timers_t *rkts, void *_rkb) {
        rd_kafka_op_t *rko     = NULL;
        rd_kafka_broker_t *rkb = (rd_kafka_broker_t *)_rkb;
        rd_dassert(rkb);
        rko = rd_kafka_op_new(RD_KAFKA_OP_SASL_REAUTH);
        rd_kafka_q_enq(rkb->rkb_ops, rko);
}

int32_t *rd_kafka_brokers_learned_ids(rd_kafka_t *rk, size_t *cntp) {
        rd_kafka_broker_t *rkb;
        int32_t *ids, *p;
        int32_t i;

        *cntp = 0;
        rd_kafka_rdlock(rk);
        ids = malloc(sizeof(*ids) * rd_list_cnt(&rk->rk_broker_by_id));
        p   = ids;
        RD_LIST_FOREACH(rkb, &rk->rk_broker_by_id, i) {
                *p++ = rkb->rkb_nodeid;
                (*cntp)++;
        }
        rd_kafka_rdunlock(rk);

        return ids;
}

/**
 * @brief Decommission a broker.
 *
 * @param rk Client instance.
 * @param rkb Broker to decommission.
 * @param wait_thrds Add the broker's thread to this list if not NULL.
 *
 * @locks rd_kafka_wrlock() is dropped and reacquired.
 *
 * Broker threads hold a refcount and detect when it reaches 1 and then
 * decommissions itself. Callers can wait for this to happen by calling
 * thrd_join() on elements of \p wait_thrds. Callers are responsible for
 * managing the creation and destruction of \p wait_thrds which can be NULL.
 */
void rd_kafka_broker_decommission(rd_kafka_t *rk,
                                  rd_kafka_broker_t *rkb,
                                  rd_list_t *wait_thrds) {

        if (rd_atomic32_get(&rkb->termination_in_progress) > 0)
                return;

        rd_atomic32_add(&rkb->termination_in_progress, 1);

        /* Add broker's thread to wait_thrds list for later joining */
        if (wait_thrds) {
                thrd_t *thrd = rd_malloc(sizeof(*thrd));
                *thrd        = rkb->rkb_thread;

                rd_list_add(wait_thrds, thrd);
        }

        rd_list_remove(&rk->rk_broker_by_id, rkb);
        rd_kafka_wrunlock(rk);

        rd_kafka_dbg(rk, BROKER, "DESTROY", "Sending TERMINATE to %s",
                     rd_kafka_broker_name(rkb));

#ifndef _WIN32
        /* Interrupt IO threads to speed up termination. */
        if (rk->rk_conf.term_sig)
                pthread_kill(rkb->rkb_thread, rk->rk_conf.term_sig);
#endif

        if (rk->rk_cgrp && rk->rk_cgrp->rkcg_curr_coord &&
            rk->rk_cgrp->rkcg_curr_coord == rkb)
                /* If we're decommissioning current coordinator handle,
                 * mark it as dead and decrease its reference count. */
                rd_kafka_cgrp_coord_dead(rk->rk_cgrp,
                                         RD_KAFKA_RESP_ERR__DESTROY_BROKER,
                                         "Group coordinator decommissioned");
        /* Send op to trigger queue/io wake-up.
         * Broker thread will destroy this thread reference.
         * WARNING: This is last time we can read from rkb in this thread! */
        rd_kafka_q_enq(rkb->rkb_ops, rd_kafka_op_new(RD_KAFKA_OP_TERMINATE));

        rd_kafka_wrlock(rk);
}

/**
 * @name Unit tests
 * @{
 *
 */
int unittest_broker(void) {
        int fails = 0;

        fails += rd_ut_reconnect_backoff();

        return fails;
}

/**@}*/
