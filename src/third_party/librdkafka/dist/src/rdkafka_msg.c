/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill,
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

#include "rd.h"
#include "rdkafka_int.h"
#include "rdkafka_msg.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_interceptor.h"
#include "rdkafka_header.h"
#include "rdkafka_idempotence.h"
#include "rdkafka_txnmgr.h"
#include "rdkafka_error.h"
#include "rdcrc32.h"
#include "rdfnv1a.h"
#include "rdmurmur2.h"
#include "rdrand.h"
#include "rdtime.h"
#include "rdsysqueue.h"
#include "rdunittest.h"

#include <stdarg.h>


const char *rd_kafka_message_errstr(const rd_kafka_message_t *rkmessage) {
        if (!rkmessage->err)
                return NULL;

        if (rkmessage->payload)
                return (const char *)rkmessage->payload;

        return rd_kafka_err2str(rkmessage->err);
}

const char *
rd_kafka_message_produce_errstr(const rd_kafka_message_t *rkmessage) {
        if (!rkmessage->err)
                return NULL;
        rd_kafka_msg_t *rkm = (rd_kafka_msg_t *)rkmessage;
        return rkm->rkm_u.producer.errstr;
}



/**
 * @brief Check if producing is allowed.
 *
 * @param errorp If non-NULL and an producing is prohibited a new error_t
 *               object will be allocated and returned in this pointer.
 *
 * @returns an error if not allowed, else 0.
 *
 * @remarks Also sets the corresponding errno.
 */
static RD_INLINE rd_kafka_resp_err_t
rd_kafka_check_produce(rd_kafka_t *rk, rd_kafka_error_t **errorp) {
        rd_kafka_resp_err_t err;

        if (unlikely((err = rd_kafka_fatal_error_code(rk)))) {
                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__FATAL, ECANCELED);
                if (errorp) {
                        rd_kafka_rdlock(rk);
                        *errorp = rd_kafka_error_new_fatal(
                            err,
                            "Producing not allowed since a previous fatal "
                            "error was raised: %s",
                            rk->rk_fatal.errstr);
                        rd_kafka_rdunlock(rk);
                }
                return RD_KAFKA_RESP_ERR__FATAL;
        }

        if (likely(rd_kafka_txn_may_enq_msg(rk)))
                return RD_KAFKA_RESP_ERR_NO_ERROR;

        /* Transactional state forbids producing */
        rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__STATE, ENOEXEC);

        if (errorp) {
                rd_kafka_rdlock(rk);
                *errorp = rd_kafka_error_new(
                    RD_KAFKA_RESP_ERR__STATE,
                    "Producing not allowed in transactional state %s",
                    rd_kafka_txn_state2str(rk->rk_eos.txn_state));
                rd_kafka_rdunlock(rk);
        }

        return RD_KAFKA_RESP_ERR__STATE;
}


void rd_kafka_msg_destroy(rd_kafka_t *rk, rd_kafka_msg_t *rkm) {
        // FIXME
        if (rkm->rkm_flags & RD_KAFKA_MSG_F_ACCOUNT) {
                rd_dassert(rk || rkm->rkm_rkmessage.rkt);
                rd_kafka_curr_msgs_sub(rk ? rk : rkm->rkm_rkmessage.rkt->rkt_rk,
                                       1, rkm->rkm_len);
        }

        if (rkm->rkm_headers)
                rd_kafka_headers_destroy(rkm->rkm_headers);

        if (likely(rkm->rkm_rkmessage.rkt != NULL))
                rd_kafka_topic_destroy0(rkm->rkm_rkmessage.rkt);

        if (rkm->rkm_flags & RD_KAFKA_MSG_F_FREE && rkm->rkm_payload)
                rd_free(rkm->rkm_payload);

        if (rkm->rkm_flags & RD_KAFKA_MSG_F_FREE_RKM)
                rd_free(rkm);
}



/**
 * @brief Create a new Producer message, copying the payload as
 *        indicated by msgflags.
 *
 * @returns the new message
 */
static rd_kafka_msg_t *rd_kafka_msg_new00(rd_kafka_topic_t *rkt,
                                          int32_t partition,
                                          int msgflags,
                                          char *payload,
                                          size_t len,
                                          const void *key,
                                          size_t keylen,
                                          void *msg_opaque) {
        rd_kafka_msg_t *rkm;
        size_t mlen = sizeof(*rkm);
        char *p;

        /* If we are to make a copy of the payload, allocate space for it too */
        if (msgflags & RD_KAFKA_MSG_F_COPY) {
                msgflags &= ~RD_KAFKA_MSG_F_FREE;
                mlen += len;
        }

        mlen += keylen;

        /* Note: using rd_malloc here, not rd_calloc, so make sure all fields
         *       are properly set up. */
        rkm          = rd_malloc(mlen);
        rkm->rkm_err = 0;
        rkm->rkm_flags =
            (RD_KAFKA_MSG_F_PRODUCER | RD_KAFKA_MSG_F_FREE_RKM | msgflags);
        rkm->rkm_len           = len;
        rkm->rkm_opaque        = msg_opaque;
        rkm->rkm_rkmessage.rkt = rd_kafka_topic_keep(rkt);

        rkm->rkm_broker_id = -1;
        rkm->rkm_partition = partition;
        rkm->rkm_offset    = RD_KAFKA_OFFSET_INVALID;
        rkm->rkm_timestamp = 0;
        rkm->rkm_tstype    = RD_KAFKA_TIMESTAMP_NOT_AVAILABLE;
        rkm->rkm_status    = RD_KAFKA_MSG_STATUS_NOT_PERSISTED;
        rkm->rkm_headers   = NULL;

        p = (char *)(rkm + 1);

        if (payload && msgflags & RD_KAFKA_MSG_F_COPY) {
                /* Copy payload to space following the ..msg_t */
                rkm->rkm_payload = p;
                memcpy(rkm->rkm_payload, payload, len);
                p += len;

        } else {
                /* Just point to the provided payload. */
                rkm->rkm_payload = payload;
        }

        if (key) {
                rkm->rkm_key     = p;
                rkm->rkm_key_len = keylen;
                memcpy(rkm->rkm_key, key, keylen);
        } else {
                rkm->rkm_key     = NULL;
                rkm->rkm_key_len = 0;
        }

        return rkm;
}



/**
 * @brief Create a new Producer message.
 *
 * @remark Must only be used by producer code.
 *
 * Returns 0 on success or -1 on error.
 * Both errno and 'errp' are set appropriately.
 */
static rd_kafka_msg_t *rd_kafka_msg_new0(rd_kafka_topic_t *rkt,
                                         int32_t force_partition,
                                         int msgflags,
                                         char *payload,
                                         size_t len,
                                         const void *key,
                                         size_t keylen,
                                         void *msg_opaque,
                                         rd_kafka_resp_err_t *errp,
                                         int *errnop,
                                         rd_kafka_headers_t *hdrs,
                                         int64_t timestamp,
                                         rd_ts_t now) {
        rd_kafka_msg_t *rkm;
        size_t hdrs_size = 0;

        if (unlikely(!payload))
                len = 0;
        if (!key)
                keylen = 0;
        if (hdrs)
                hdrs_size = rd_kafka_headers_serialized_size(hdrs);

        if (unlikely(len > INT32_MAX || keylen > INT32_MAX ||
                     rd_kafka_msg_max_wire_size(keylen, len, hdrs_size) >
                         (size_t)rkt->rkt_rk->rk_conf.max_msg_size)) {
                *errp = RD_KAFKA_RESP_ERR_MSG_SIZE_TOO_LARGE;
                if (errnop)
                        *errnop = EMSGSIZE;
                return NULL;
        }

        if (msgflags & RD_KAFKA_MSG_F_BLOCK)
                *errp = rd_kafka_curr_msgs_add(
                    rkt->rkt_rk, 1, len, 1 /*block*/,
                    (msgflags & RD_KAFKA_MSG_F_RKT_RDLOCKED) ? &rkt->rkt_lock
                                                             : NULL);
        else
                *errp = rd_kafka_curr_msgs_add(rkt->rkt_rk, 1, len, 0, NULL);

        if (unlikely(*errp)) {
                if (errnop)
                        *errnop = ENOBUFS;
                return NULL;
        }


        rkm = rd_kafka_msg_new00(
            rkt, force_partition,
            msgflags | RD_KAFKA_MSG_F_ACCOUNT /* curr_msgs_add() */, payload,
            len, key, keylen, msg_opaque);

        memset(&rkm->rkm_u.producer, 0, sizeof(rkm->rkm_u.producer));

        if (timestamp)
                rkm->rkm_timestamp = timestamp;
        else
                rkm->rkm_timestamp = rd_uclock() / 1000;
        rkm->rkm_tstype = RD_KAFKA_TIMESTAMP_CREATE_TIME;

        if (hdrs) {
                rd_dassert(!rkm->rkm_headers);
                rkm->rkm_headers = hdrs;
        }

        rkm->rkm_ts_enq = now;

        if (rkt->rkt_conf.message_timeout_ms == 0) {
                rkm->rkm_ts_timeout = INT64_MAX;
        } else {
                rkm->rkm_ts_timeout =
                    now + (int64_t)rkt->rkt_conf.message_timeout_ms * 1000;
        }

        /* Call interceptor chain for on_send */
        rd_kafka_interceptors_on_send(rkt->rkt_rk, &rkm->rkm_rkmessage);

        return rkm;
}


/**
 * @brief Produce: creates a new message, runs the partitioner and enqueues
 *        into on the selected partition.
 *
 * @returns 0 on success or -1 on error.
 *
 * If the function returns -1 and RD_KAFKA_MSG_F_FREE was specified, then
 * the memory associated with the payload is still the caller's
 * responsibility.
 *
 * @locks none
 */
int rd_kafka_msg_new(rd_kafka_topic_t *rkt,
                     int32_t force_partition,
                     int msgflags,
                     char *payload,
                     size_t len,
                     const void *key,
                     size_t keylen,
                     void *msg_opaque) {
        rd_kafka_msg_t *rkm;
        rd_kafka_resp_err_t err;
        int errnox;

        if (unlikely((err = rd_kafka_check_produce(rkt->rkt_rk, NULL))))
                return -1;

        /* Create message */
        rkm = rd_kafka_msg_new0(rkt, force_partition, msgflags, payload, len,
                                key, keylen, msg_opaque, &err, &errnox, NULL, 0,
                                rd_clock());
        if (unlikely(!rkm)) {
                /* errno is already set by msg_new() */
                rd_kafka_set_last_error(err, errnox);
                return -1;
        }


        /* Partition the message */
        err = rd_kafka_msg_partitioner(rkt, rkm, 1);
        if (likely(!err)) {
                rd_kafka_set_last_error(0, 0);
                return 0;
        }

        /* Interceptor: unroll failing messages by triggering on_ack.. */
        rkm->rkm_err = err;
        rd_kafka_interceptors_on_acknowledgement(rkt->rkt_rk,
                                                 &rkm->rkm_rkmessage);

        /* Handle partitioner failures: it only fails when the application
         * attempts to force a destination partition that does not exist
         * in the cluster.  Note we must clear the RD_KAFKA_MSG_F_FREE
         * flag since our contract says we don't free the payload on
         * failure. */

        rkm->rkm_flags &= ~RD_KAFKA_MSG_F_FREE;
        rd_kafka_msg_destroy(rkt->rkt_rk, rkm);

        /* Translate error codes to errnos. */
        if (err == RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION)
                rd_kafka_set_last_error(err, ESRCH);
        else if (err == RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC)
                rd_kafka_set_last_error(err, ENOENT);
        else
                rd_kafka_set_last_error(err, EINVAL); /* NOTREACHED */

        return -1;
}


/** @remark Keep rd_kafka_produceva() and rd_kafka_producev() in synch */
rd_kafka_error_t *
rd_kafka_produceva(rd_kafka_t *rk, const rd_kafka_vu_t *vus, size_t cnt) {
        rd_kafka_msg_t s_rkm = {
            /* Message defaults */
            .rkm_partition = RD_KAFKA_PARTITION_UA,
            .rkm_timestamp = 0, /* current time */
        };
        rd_kafka_msg_t *rkm          = &s_rkm;
        rd_kafka_topic_t *rkt        = NULL;
        rd_kafka_resp_err_t err      = RD_KAFKA_RESP_ERR_NO_ERROR;
        rd_kafka_error_t *error      = NULL;
        rd_kafka_headers_t *hdrs     = NULL;
        rd_kafka_headers_t *app_hdrs = NULL; /* App-provided headers list */
        size_t i;

        if (unlikely(rd_kafka_check_produce(rk, &error)))
                return error;

        for (i = 0; i < cnt; i++) {
                const rd_kafka_vu_t *vu = &vus[i];
                switch (vu->vtype) {
                case RD_KAFKA_VTYPE_TOPIC:
                        rkt =
                            rd_kafka_topic_new0(rk, vu->u.cstr, NULL, NULL, 1);
                        break;

                case RD_KAFKA_VTYPE_RKT:
                        rkt = rd_kafka_topic_proper(vu->u.rkt);
                        rd_kafka_topic_keep(rkt);
                        break;

                case RD_KAFKA_VTYPE_PARTITION:
                        rkm->rkm_partition = vu->u.i32;
                        break;

                case RD_KAFKA_VTYPE_VALUE:
                        rkm->rkm_payload = vu->u.mem.ptr;
                        rkm->rkm_len     = vu->u.mem.size;
                        break;

                case RD_KAFKA_VTYPE_KEY:
                        rkm->rkm_key     = vu->u.mem.ptr;
                        rkm->rkm_key_len = vu->u.mem.size;
                        break;

                case RD_KAFKA_VTYPE_OPAQUE:
                        rkm->rkm_opaque = vu->u.ptr;
                        break;

                case RD_KAFKA_VTYPE_MSGFLAGS:
                        rkm->rkm_flags = vu->u.i;
                        break;

                case RD_KAFKA_VTYPE_TIMESTAMP:
                        rkm->rkm_timestamp = vu->u.i64;
                        break;

                case RD_KAFKA_VTYPE_HEADER:
                        if (unlikely(app_hdrs != NULL)) {
                                error = rd_kafka_error_new(
                                    RD_KAFKA_RESP_ERR__CONFLICT,
                                    "VTYPE_HEADER and VTYPE_HEADERS "
                                    "are mutually exclusive");
                                goto err;
                        }

                        if (unlikely(!hdrs))
                                hdrs = rd_kafka_headers_new(8);

                        err = rd_kafka_header_add(hdrs, vu->u.header.name, -1,
                                                  vu->u.header.val,
                                                  vu->u.header.size);
                        if (unlikely(err)) {
                                error = rd_kafka_error_new(
                                    err, "Failed to add header: %s",
                                    rd_kafka_err2str(err));
                                goto err;
                        }
                        break;

                case RD_KAFKA_VTYPE_HEADERS:
                        if (unlikely(hdrs != NULL)) {
                                error = rd_kafka_error_new(
                                    RD_KAFKA_RESP_ERR__CONFLICT,
                                    "VTYPE_HEADERS and VTYPE_HEADER "
                                    "are mutually exclusive");
                                goto err;
                        }
                        app_hdrs = vu->u.headers;
                        break;

                default:
                        error = rd_kafka_error_new(
                            RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Unsupported VTYPE %d", (int)vu->vtype);
                        goto err;
                }
        }

        rd_assert(!error);

        if (unlikely(!rkt)) {
                error = rd_kafka_error_new(RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "Topic name or object required");
                goto err;
        }

        rkm = rd_kafka_msg_new0(
            rkt, rkm->rkm_partition, rkm->rkm_flags, rkm->rkm_payload,
            rkm->rkm_len, rkm->rkm_key, rkm->rkm_key_len, rkm->rkm_opaque, &err,
            NULL, app_hdrs ? app_hdrs : hdrs, rkm->rkm_timestamp, rd_clock());

        if (unlikely(err)) {
                error = rd_kafka_error_new(err, "Failed to produce message: %s",
                                           rd_kafka_err2str(err));
                goto err;
        }

        /* Partition the message */
        err = rd_kafka_msg_partitioner(rkt, rkm, 1);
        if (unlikely(err)) {
                /* Handle partitioner failures: it only fails when
                 * the application attempts to force a destination
                 * partition that does not exist in the cluster. */

                /* Interceptors: Unroll on_send by on_ack.. */
                rkm->rkm_err = err;
                rd_kafka_interceptors_on_acknowledgement(rk,
                                                         &rkm->rkm_rkmessage);

                /* Note we must clear the RD_KAFKA_MSG_F_FREE
                 * flag since our contract says we don't free the payload on
                 * failure. */
                rkm->rkm_flags &= ~RD_KAFKA_MSG_F_FREE;

                /* Deassociate application owned headers from message
                 * since headers remain in application ownership
                 * when producev() fails */
                if (app_hdrs && app_hdrs == rkm->rkm_headers)
                        rkm->rkm_headers = NULL;

                rd_kafka_msg_destroy(rk, rkm);

                error = rd_kafka_error_new(err, "Failed to enqueue message: %s",
                                           rd_kafka_err2str(err));
                goto err;
        }

        rd_kafka_topic_destroy0(rkt);

        return NULL;

err:
        if (rkt)
                rd_kafka_topic_destroy0(rkt);

        if (hdrs)
                rd_kafka_headers_destroy(hdrs);

        rd_assert(error != NULL);
        return error;
}



/** @remark Keep rd_kafka_produceva() and rd_kafka_producev() in synch */
rd_kafka_resp_err_t rd_kafka_producev(rd_kafka_t *rk, ...) {
        va_list ap;
        rd_kafka_msg_t s_rkm = {
            /* Message defaults */
            .rkm_partition = RD_KAFKA_PARTITION_UA,
            .rkm_timestamp = 0, /* current time */
        };
        rd_kafka_msg_t *rkm = &s_rkm;
        rd_kafka_vtype_t vtype;
        rd_kafka_topic_t *rkt = NULL;
        rd_kafka_resp_err_t err;
        rd_kafka_headers_t *hdrs     = NULL;
        rd_kafka_headers_t *app_hdrs = NULL; /* App-provided headers list */

        if (unlikely((err = rd_kafka_check_produce(rk, NULL))))
                return err;

        va_start(ap, rk);
        while (!err &&
               (vtype = va_arg(ap, rd_kafka_vtype_t)) != RD_KAFKA_VTYPE_END) {
                switch (vtype) {
                case RD_KAFKA_VTYPE_TOPIC:
                        rkt = rd_kafka_topic_new0(rk, va_arg(ap, const char *),
                                                  NULL, NULL, 1);
                        break;

                case RD_KAFKA_VTYPE_RKT:
                        rkt = rd_kafka_topic_proper(
                            va_arg(ap, rd_kafka_topic_t *));
                        rd_kafka_topic_keep(rkt);
                        break;

                case RD_KAFKA_VTYPE_PARTITION:
                        rkm->rkm_partition = va_arg(ap, int32_t);
                        break;

                case RD_KAFKA_VTYPE_VALUE:
                        rkm->rkm_payload = va_arg(ap, void *);
                        rkm->rkm_len     = va_arg(ap, size_t);
                        break;

                case RD_KAFKA_VTYPE_KEY:
                        rkm->rkm_key     = va_arg(ap, void *);
                        rkm->rkm_key_len = va_arg(ap, size_t);
                        break;

                case RD_KAFKA_VTYPE_OPAQUE:
                        rkm->rkm_opaque = va_arg(ap, void *);
                        break;

                case RD_KAFKA_VTYPE_MSGFLAGS:
                        rkm->rkm_flags = va_arg(ap, int);
                        break;

                case RD_KAFKA_VTYPE_TIMESTAMP:
                        rkm->rkm_timestamp = va_arg(ap, int64_t);
                        break;

                case RD_KAFKA_VTYPE_HEADER: {
                        const char *name;
                        const void *value;
                        ssize_t size;

                        if (unlikely(app_hdrs != NULL)) {
                                err = RD_KAFKA_RESP_ERR__CONFLICT;
                                break;
                        }

                        if (unlikely(!hdrs))
                                hdrs = rd_kafka_headers_new(8);

                        name  = va_arg(ap, const char *);
                        value = va_arg(ap, const void *);
                        size  = va_arg(ap, ssize_t);

                        err = rd_kafka_header_add(hdrs, name, -1, value, size);
                } break;

                case RD_KAFKA_VTYPE_HEADERS:
                        if (unlikely(hdrs != NULL)) {
                                err = RD_KAFKA_RESP_ERR__CONFLICT;
                                break;
                        }
                        app_hdrs = va_arg(ap, rd_kafka_headers_t *);
                        break;

                default:
                        err = RD_KAFKA_RESP_ERR__INVALID_ARG;
                        break;
                }
        }

        va_end(ap);

        if (unlikely(!rkt))
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        if (likely(!err))
                rkm = rd_kafka_msg_new0(
                    rkt, rkm->rkm_partition, rkm->rkm_flags, rkm->rkm_payload,
                    rkm->rkm_len, rkm->rkm_key, rkm->rkm_key_len,
                    rkm->rkm_opaque, &err, NULL, app_hdrs ? app_hdrs : hdrs,
                    rkm->rkm_timestamp, rd_clock());

        if (unlikely(err)) {
                rd_kafka_topic_destroy0(rkt);
                if (hdrs)
                        rd_kafka_headers_destroy(hdrs);
                return err;
        }

        /* Partition the message */
        err = rd_kafka_msg_partitioner(rkt, rkm, 1);
        if (unlikely(err)) {
                /* Handle partitioner failures: it only fails when
                 * the application attempts to force a destination
                 * partition that does not exist in the cluster. */

                /* Interceptors: Unroll on_send by on_ack.. */
                rkm->rkm_err = err;
                rd_kafka_interceptors_on_acknowledgement(rk,
                                                         &rkm->rkm_rkmessage);

                /* Note we must clear the RD_KAFKA_MSG_F_FREE
                 * flag since our contract says we don't free the payload on
                 * failure. */
                rkm->rkm_flags &= ~RD_KAFKA_MSG_F_FREE;

                /* Deassociate application owned headers from message
                 * since headers remain in application ownership
                 * when producev() fails */
                if (app_hdrs && app_hdrs == rkm->rkm_headers)
                        rkm->rkm_headers = NULL;

                rd_kafka_msg_destroy(rk, rkm);
        }

        rd_kafka_topic_destroy0(rkt);

        return err;
}



/**
 * @brief Produce a single message.
 * @locality any application thread
 * @locks none
 */
int rd_kafka_produce(rd_kafka_topic_t *rkt,
                     int32_t partition,
                     int msgflags,
                     void *payload,
                     size_t len,
                     const void *key,
                     size_t keylen,
                     void *msg_opaque) {
        return rd_kafka_msg_new(rkt, partition, msgflags, payload, len, key,
                                keylen, msg_opaque);
}



/**
 * Produce a batch of messages.
 * Returns the number of messages succesfully queued for producing.
 * Each message's .err will be set accordingly.
 */
int rd_kafka_produce_batch(rd_kafka_topic_t *app_rkt,
                           int32_t partition,
                           int msgflags,
                           rd_kafka_message_t *rkmessages,
                           int message_cnt) {
        rd_kafka_msgq_t tmpq = RD_KAFKA_MSGQ_INITIALIZER(tmpq);
        int i;
        int64_t utc_now         = rd_uclock() / 1000;
        rd_ts_t now             = rd_clock();
        int good                = 0;
        int multiple_partitions = (partition == RD_KAFKA_PARTITION_UA ||
                                   (msgflags & RD_KAFKA_MSG_F_PARTITION));
        rd_kafka_resp_err_t all_err;
        rd_kafka_topic_t *rkt   = rd_kafka_topic_proper(app_rkt);
        rd_kafka_toppar_t *rktp = NULL;

        /* Propagated per-message below */
        all_err = rd_kafka_check_produce(rkt->rkt_rk, NULL);

        rd_kafka_topic_rdlock(rkt);
        if (!multiple_partitions) {
                /* Single partition: look up the rktp once. */
                rktp = rd_kafka_toppar_get_avail(rkt, partition,
                                                 1 /*ua on miss*/, &all_err);

        } else {
                /* Indicate to lower-level msg_new..() that rkt is locked
                 * so that they may unlock it momentarily if blocking. */
                msgflags |= RD_KAFKA_MSG_F_RKT_RDLOCKED;
        }

        for (i = 0; i < message_cnt; i++) {
                rd_kafka_msg_t *rkm;

                /* Propagate error for all messages. */
                if (unlikely(all_err)) {
                        rkmessages[i].err = all_err;
                        continue;
                }

                /* Create message */
                rkm = rd_kafka_msg_new0(
                    rkt,
                    (msgflags & RD_KAFKA_MSG_F_PARTITION)
                        ? rkmessages[i].partition
                        : partition,
                    msgflags, rkmessages[i].payload, rkmessages[i].len,
                    rkmessages[i].key, rkmessages[i].key_len,
                    rkmessages[i]._private, &rkmessages[i].err, NULL, NULL,
                    utc_now, now);
                if (unlikely(!rkm)) {
                        if (rkmessages[i].err == RD_KAFKA_RESP_ERR__QUEUE_FULL)
                                all_err = rkmessages[i].err;
                        continue;
                }

                /* Three cases here:
                 *  partition==UA:            run the partitioner (slow)
                 *  RD_KAFKA_MSG_F_PARTITION: produce message to specified
                 *                            partition
                 *  fixed partition:          simply concatenate the queue
                 *                            to partit */
                if (multiple_partitions) {
                        if (rkm->rkm_partition == RD_KAFKA_PARTITION_UA) {
                                /* Partition the message */
                                rkmessages[i].err = rd_kafka_msg_partitioner(
                                    rkt, rkm, 0 /*already locked*/);
                        } else {
                                if (rktp == NULL || rkm->rkm_partition !=
                                                        rktp->rktp_partition) {
                                        rd_kafka_resp_err_t err;
                                        if (rktp != NULL)
                                                rd_kafka_toppar_destroy(rktp);
                                        rktp = rd_kafka_toppar_get_avail(
                                            rkt, rkm->rkm_partition,
                                            1 /*ua on miss*/, &err);

                                        if (unlikely(!rktp)) {
                                                rkmessages[i].err = err;
                                                continue;
                                        }
                                }
                                rd_kafka_toppar_enq_msg(rktp, rkm, now);

                                if (rd_kafka_is_transactional(rkt->rkt_rk)) {
                                        /* Add partition to transaction */
                                        rd_kafka_txn_add_partition(rktp);
                                }
                        }

                        if (unlikely(rkmessages[i].err)) {
                                /* Interceptors: Unroll on_send by on_ack.. */
                                rd_kafka_interceptors_on_acknowledgement(
                                    rkt->rkt_rk, &rkmessages[i]);

                                rd_kafka_msg_destroy(rkt->rkt_rk, rkm);
                                continue;
                        }


                } else {
                        /* Single destination partition. */
                        rd_kafka_toppar_enq_msg(rktp, rkm, now);
                }

                rkmessages[i].err = RD_KAFKA_RESP_ERR_NO_ERROR;
                good++;
        }

        rd_kafka_topic_rdunlock(rkt);

        if (!multiple_partitions && good > 0 &&
            rd_kafka_is_transactional(rkt->rkt_rk) &&
            rktp->rktp_partition != RD_KAFKA_PARTITION_UA) {
                /* Add single destination partition to transaction */
                rd_kafka_txn_add_partition(rktp);
        }

        if (rktp != NULL)
                rd_kafka_toppar_destroy(rktp);

        return good;
}

/**
 * @brief Scan \p rkmq for messages that have timed out and remove them from
 *        \p rkmq and add to \p timedout queue.
 *
 * @param abs_next_timeout will be set to the next message timeout, or 0
 *                         if no timeout. Optional, may be NULL.
 *
 * @returns the number of messages timed out.
 *
 * @locality any
 * @locks toppar_lock MUST be held
 */
int rd_kafka_msgq_age_scan(rd_kafka_toppar_t *rktp,
                           rd_kafka_msgq_t *rkmq,
                           rd_kafka_msgq_t *timedout,
                           rd_ts_t now,
                           rd_ts_t *abs_next_timeout) {
        rd_kafka_msg_t *rkm, *tmp, *first = NULL;
        int cnt = timedout->rkmq_msg_cnt;

        if (abs_next_timeout)
                *abs_next_timeout = 0;

        /* Assume messages are added in time sequencial order */
        TAILQ_FOREACH_SAFE(rkm, &rkmq->rkmq_msgs, rkm_link, tmp) {
                /* NOTE: this is not true for the deprecated (and soon removed)
                 *       LIFO queuing strategy. */
                if (likely(rkm->rkm_ts_timeout > now)) {
                        if (abs_next_timeout)
                                *abs_next_timeout = rkm->rkm_ts_timeout;
                        break;
                }

                if (!first)
                        first = rkm;

                rd_kafka_msgq_deq(rkmq, rkm, 1);
                rd_kafka_msgq_enq(timedout, rkm);
        }

        return timedout->rkmq_msg_cnt - cnt;
}


int rd_kafka_msgq_enq_sorted0(rd_kafka_msgq_t *rkmq,
                              rd_kafka_msg_t *rkm,
                              int (*order_cmp)(const void *, const void *)) {
        TAILQ_INSERT_SORTED(&rkmq->rkmq_msgs, rkm, rd_kafka_msg_t *, rkm_link,
                            order_cmp);
        rkmq->rkmq_msg_bytes += rkm->rkm_len + rkm->rkm_key_len;
        return ++rkmq->rkmq_msg_cnt;
}

int rd_kafka_msgq_enq_sorted(const rd_kafka_topic_t *rkt,
                             rd_kafka_msgq_t *rkmq,
                             rd_kafka_msg_t *rkm) {
        rd_dassert(rkm->rkm_u.producer.msgid != 0);
        return rd_kafka_msgq_enq_sorted0(rkmq, rkm,
                                         rkt->rkt_conf.msg_order_cmp);
}

/**
 * @brief Find the insert before position (i.e., the msg which comes
 *        after \p rkm sequencially) for message \p rkm.
 *
 * @param rkmq insert queue.
 * @param start_pos the element in \p rkmq to start scanning at, or NULL
 *                  to start with the first element.
 * @param rkm message to insert.
 * @param cmp message comparator.
 * @param cntp the accumulated number of messages up to, but not including,
 *             the returned insert position. Optional (NULL).
 *             Do not use when start_pos is set.
 * @param bytesp the accumulated number of bytes up to, but not inclduing,
 *               the returned insert position. Optional (NULL).
 *               Do not use when start_pos is set.
 *
 * @remark cntp and bytesp will NOT be accurate when \p start_pos is non-NULL.
 *
 * @returns the insert position element, or NULL if \p rkm should be
 *          added at tail of queue.
 */
rd_kafka_msg_t *rd_kafka_msgq_find_pos(const rd_kafka_msgq_t *rkmq,
                                       const rd_kafka_msg_t *start_pos,
                                       const rd_kafka_msg_t *rkm,
                                       int (*cmp)(const void *, const void *),
                                       int *cntp,
                                       int64_t *bytesp) {
        const rd_kafka_msg_t *curr;
        int cnt       = 0;
        int64_t bytes = 0;

        for (curr = start_pos ? start_pos : rd_kafka_msgq_first(rkmq); curr;
             curr = TAILQ_NEXT(curr, rkm_link)) {
                if (cmp(rkm, curr) < 0) {
                        if (cntp) {
                                *cntp   = cnt;
                                *bytesp = bytes;
                        }
                        return (rd_kafka_msg_t *)curr;
                }
                if (cntp) {
                        cnt++;
                        bytes += rkm->rkm_len + rkm->rkm_key_len;
                }
        }

        return NULL;
}


/**
 * @brief Split the original \p leftq into a left and right part,
 *        with element \p first_right being the first element in the
 *        right part (\p rightq).
 *
 * @param cnt is the number of messages up to, but not including \p first_right
 *            in \p leftq, namely the number of messages to remain in
 *            \p leftq after the split.
 * @param bytes is the bytes counterpart to \p cnt.
 */
void rd_kafka_msgq_split(rd_kafka_msgq_t *leftq,
                         rd_kafka_msgq_t *rightq,
                         rd_kafka_msg_t *first_right,
                         int cnt,
                         int64_t bytes) {
        rd_kafka_msg_t *llast;

        rd_assert(first_right != TAILQ_FIRST(&leftq->rkmq_msgs));

        llast = TAILQ_PREV(first_right, rd_kafka_msg_head_s, rkm_link);

        rd_kafka_msgq_init(rightq);

        rightq->rkmq_msgs.tqh_first = first_right;
        rightq->rkmq_msgs.tqh_last  = leftq->rkmq_msgs.tqh_last;

        first_right->rkm_link.tqe_prev = &rightq->rkmq_msgs.tqh_first;

        leftq->rkmq_msgs.tqh_last = &llast->rkm_link.tqe_next;
        llast->rkm_link.tqe_next  = NULL;

        rightq->rkmq_msg_cnt   = leftq->rkmq_msg_cnt - cnt;
        rightq->rkmq_msg_bytes = leftq->rkmq_msg_bytes - bytes;
        leftq->rkmq_msg_cnt    = cnt;
        leftq->rkmq_msg_bytes  = bytes;

        rd_kafka_msgq_verify_order(NULL, leftq, 0, rd_false);
        rd_kafka_msgq_verify_order(NULL, rightq, 0, rd_false);
}


/**
 * @brief Set per-message metadata for all messages in \p rkmq
 */
void rd_kafka_msgq_set_metadata(rd_kafka_msgq_t *rkmq,
                                int32_t broker_id,
                                int64_t base_offset,
                                int64_t timestamp,
                                rd_kafka_msg_status_t status) {
        rd_kafka_msg_t *rkm;

        TAILQ_FOREACH(rkm, &rkmq->rkmq_msgs, rkm_link) {
                rkm->rkm_broker_id = broker_id;
                rkm->rkm_offset    = base_offset++;
                if (timestamp != -1) {
                        rkm->rkm_timestamp = timestamp;
                        rkm->rkm_tstype    = RD_KAFKA_TIMESTAMP_LOG_APPEND_TIME;
                }

                /* Don't downgrade a message from any form of PERSISTED
                 * to NOT_PERSISTED, since the original cause of indicating
                 * PERSISTED can't be changed.
                 * E.g., a previous ack or in-flight timeout. */
                if (unlikely(status == RD_KAFKA_MSG_STATUS_NOT_PERSISTED &&
                             rkm->rkm_status !=
                                 RD_KAFKA_MSG_STATUS_NOT_PERSISTED))
                        continue;

                rkm->rkm_status = status;
        }
}


/**
 * @brief Move all messages in \p src to \p dst whose msgid <= last_msgid.
 *
 * @remark src must be ordered
 */
void rd_kafka_msgq_move_acked(rd_kafka_msgq_t *dest,
                              rd_kafka_msgq_t *src,
                              uint64_t last_msgid,
                              rd_kafka_msg_status_t status) {
        rd_kafka_msg_t *rkm;

        while ((rkm = rd_kafka_msgq_first(src)) &&
               rkm->rkm_u.producer.msgid <= last_msgid) {
                rd_kafka_msgq_deq(src, rkm, 1);
                rd_kafka_msgq_enq(dest, rkm);

                rkm->rkm_status = status;
        }

        rd_kafka_msgq_verify_order(NULL, dest, 0, rd_false);
        rd_kafka_msgq_verify_order(NULL, src, 0, rd_false);
}



int32_t rd_kafka_msg_partitioner_random(const rd_kafka_topic_t *rkt,
                                        const void *key,
                                        size_t keylen,
                                        int32_t partition_cnt,
                                        void *rkt_opaque,
                                        void *msg_opaque) {
        int32_t p = rd_jitter(0, partition_cnt - 1);
        if (unlikely(!rd_kafka_topic_partition_available(rkt, p)))
                return rd_jitter(0, partition_cnt - 1);
        else
                return p;
}

int32_t rd_kafka_msg_partitioner_consistent(const rd_kafka_topic_t *rkt,
                                            const void *key,
                                            size_t keylen,
                                            int32_t partition_cnt,
                                            void *rkt_opaque,
                                            void *msg_opaque) {
        return rd_crc32(key, keylen) % partition_cnt;
}

int32_t rd_kafka_msg_partitioner_consistent_random(const rd_kafka_topic_t *rkt,
                                                   const void *key,
                                                   size_t keylen,
                                                   int32_t partition_cnt,
                                                   void *rkt_opaque,
                                                   void *msg_opaque) {
        if (keylen == 0)
                return rd_kafka_msg_partitioner_random(
                    rkt, key, keylen, partition_cnt, rkt_opaque, msg_opaque);
        else
                return rd_kafka_msg_partitioner_consistent(
                    rkt, key, keylen, partition_cnt, rkt_opaque, msg_opaque);
}

int32_t rd_kafka_msg_partitioner_murmur2(const rd_kafka_topic_t *rkt,
                                         const void *key,
                                         size_t keylen,
                                         int32_t partition_cnt,
                                         void *rkt_opaque,
                                         void *msg_opaque) {
        return (rd_murmur2(key, keylen) & 0x7fffffff) % partition_cnt;
}

int32_t rd_kafka_msg_partitioner_murmur2_random(const rd_kafka_topic_t *rkt,
                                                const void *key,
                                                size_t keylen,
                                                int32_t partition_cnt,
                                                void *rkt_opaque,
                                                void *msg_opaque) {
        if (!key)
                return rd_kafka_msg_partitioner_random(
                    rkt, key, keylen, partition_cnt, rkt_opaque, msg_opaque);
        else
                return (rd_murmur2(key, keylen) & 0x7fffffff) % partition_cnt;
}

int32_t rd_kafka_msg_partitioner_fnv1a(const rd_kafka_topic_t *rkt,
                                       const void *key,
                                       size_t keylen,
                                       int32_t partition_cnt,
                                       void *rkt_opaque,
                                       void *msg_opaque) {
        return rd_fnv1a(key, keylen) % partition_cnt;
}

int32_t rd_kafka_msg_partitioner_fnv1a_random(const rd_kafka_topic_t *rkt,
                                              const void *key,
                                              size_t keylen,
                                              int32_t partition_cnt,
                                              void *rkt_opaque,
                                              void *msg_opaque) {
        if (!key)
                return rd_kafka_msg_partitioner_random(
                    rkt, key, keylen, partition_cnt, rkt_opaque, msg_opaque);
        else
                return rd_fnv1a(key, keylen) % partition_cnt;
}

int32_t rd_kafka_msg_sticky_partition(rd_kafka_topic_t *rkt,
                                      const void *key,
                                      size_t keylen,
                                      int32_t partition_cnt,
                                      void *rkt_opaque,
                                      void *msg_opaque) {

        if (!rd_kafka_topic_partition_available(rkt, rkt->rkt_sticky_partition))
                rd_interval_expedite(&rkt->rkt_sticky_intvl, 0);

        if (rd_interval(&rkt->rkt_sticky_intvl,
                        rkt->rkt_rk->rk_conf.sticky_partition_linger_ms * 1000,
                        0) > 0) {
                rkt->rkt_sticky_partition = rd_kafka_msg_partitioner_random(
                    rkt, key, keylen, partition_cnt, rkt_opaque, msg_opaque);
                rd_kafka_dbg(rkt->rkt_rk, TOPIC, "PARTITIONER",
                             "%s [%" PRId32 "] is the new sticky partition",
                             rkt->rkt_topic->str, rkt->rkt_sticky_partition);
        }

        return rkt->rkt_sticky_partition;
}

/**
 * @brief Assigns a message to a topic partition using a partitioner.
 *
 * @param do_lock if RD_DO_LOCK then acquire topic lock.
 *
 * @returns RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION or .._UNKNOWN_TOPIC if
 *          partitioning failed, or 0 on success.
 *
 * @locality any
 * @locks rd_kafka_
 */
int rd_kafka_msg_partitioner(rd_kafka_topic_t *rkt,
                             rd_kafka_msg_t *rkm,
                             rd_dolock_t do_lock) {
        int32_t partition;
        rd_kafka_toppar_t *rktp_new;
        rd_kafka_resp_err_t err;

        if (do_lock)
                rd_kafka_topic_rdlock(rkt);

        switch (rkt->rkt_state) {
        case RD_KAFKA_TOPIC_S_UNKNOWN:
                /* No metadata received from cluster yet.
                 * Put message in UA partition and re-run partitioner when
                 * cluster comes up. */
                partition = RD_KAFKA_PARTITION_UA;
                break;

        case RD_KAFKA_TOPIC_S_NOTEXISTS:
                /* Topic not found in cluster.
                 * Fail message immediately. */
                err = RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC;
                if (do_lock)
                        rd_kafka_topic_rdunlock(rkt);
                return err;

        case RD_KAFKA_TOPIC_S_ERROR:
                /* Topic has permanent error.
                 * Fail message immediately. */
                err = rkt->rkt_err;
                if (do_lock)
                        rd_kafka_topic_rdunlock(rkt);
                return err;

        case RD_KAFKA_TOPIC_S_EXISTS:
                /* Topic exists in cluster. */

                /* Topic exists but has no partitions.
                 * This is usually an transient state following the
                 * auto-creation of a topic. */
                if (unlikely(rkt->rkt_partition_cnt == 0)) {
                        partition = RD_KAFKA_PARTITION_UA;
                        break;
                }

                /* Partition not assigned, run partitioner. */
                if (rkm->rkm_partition == RD_KAFKA_PARTITION_UA) {

                        if (!rkt->rkt_conf.random_partitioner &&
                            (!rkm->rkm_key ||
                             (rkm->rkm_key_len == 0 &&
                              rkt->rkt_conf.partitioner ==
                                  rd_kafka_msg_partitioner_consistent_random))) {
                                partition = rd_kafka_msg_sticky_partition(
                                    rkt, rkm->rkm_key, rkm->rkm_key_len,
                                    rkt->rkt_partition_cnt,
                                    rkt->rkt_conf.opaque, rkm->rkm_opaque);
                        } else {
                                partition = rkt->rkt_conf.partitioner(
                                    rkt, rkm->rkm_key, rkm->rkm_key_len,
                                    rkt->rkt_partition_cnt,
                                    rkt->rkt_conf.opaque, rkm->rkm_opaque);
                        }
                } else
                        partition = rkm->rkm_partition;

                /* Check that partition exists. */
                if (partition >= rkt->rkt_partition_cnt) {
                        err = RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION;
                        if (do_lock)
                                rd_kafka_topic_rdunlock(rkt);
                        return err;
                }
                break;

        default:
                rd_kafka_assert(rkt->rkt_rk, !*"NOTREACHED");
                break;
        }

        /* Get new partition */
        rktp_new = rd_kafka_toppar_get(rkt, partition, 0);

        if (unlikely(!rktp_new)) {
                /* Unknown topic or partition */
                if (rkt->rkt_state == RD_KAFKA_TOPIC_S_NOTEXISTS)
                        err = RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC;
                else
                        err = RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION;

                if (do_lock)
                        rd_kafka_topic_rdunlock(rkt);

                return err;
        }

        rd_atomic64_add(&rktp_new->rktp_c.producer_enq_msgs, 1);

        /* Update message partition */
        if (rkm->rkm_partition == RD_KAFKA_PARTITION_UA)
                rkm->rkm_partition = partition;

        /* Partition is available: enqueue msg on partition's queue */
        rd_kafka_toppar_enq_msg(rktp_new, rkm, rd_clock());
        if (do_lock)
                rd_kafka_topic_rdunlock(rkt);

        if (rktp_new->rktp_partition != RD_KAFKA_PARTITION_UA &&
            rd_kafka_is_transactional(rkt->rkt_rk)) {
                /* Add partition to transaction */
                rd_kafka_txn_add_partition(rktp_new);
        }

        rd_kafka_toppar_destroy(rktp_new); /* from _get() */
        return 0;
}



/**
 * @name Public message type (rd_kafka_message_t)
 */
void rd_kafka_message_destroy(rd_kafka_message_t *rkmessage) {
        rd_kafka_op_t *rko;

        if (likely((rko = (rd_kafka_op_t *)rkmessage->_private) != NULL))
                rd_kafka_op_destroy(rko);
        else {
                rd_kafka_msg_t *rkm = rd_kafka_message2msg(rkmessage);
                rd_kafka_msg_destroy(NULL, rkm);
        }
}


rd_kafka_message_t *rd_kafka_message_new(void) {
        rd_kafka_msg_t *rkm = rd_calloc(1, sizeof(*rkm));
        rkm->rkm_flags      = RD_KAFKA_MSG_F_FREE_RKM;
        rkm->rkm_broker_id  = -1;
        return (rd_kafka_message_t *)rkm;
}


/**
 * @brief Set up a rkmessage from an rko for passing to the application.
 * @remark Will trigger on_consume() interceptors if any.
 */
static rd_kafka_message_t *
rd_kafka_message_setup(rd_kafka_op_t *rko, rd_kafka_message_t *rkmessage) {
        rd_kafka_topic_t *rkt;
        rd_kafka_toppar_t *rktp = NULL;

        if (rko->rko_type == RD_KAFKA_OP_DR) {
                rkt = rko->rko_u.dr.rkt;
        } else {
                if (rko->rko_rktp) {
                        rktp = rko->rko_rktp;
                        rkt  = rktp->rktp_rkt;
                } else
                        rkt = NULL;

                rkmessage->_private = rko;
        }


        if (!rkmessage->rkt && rkt)
                rkmessage->rkt = rd_kafka_topic_keep(rkt);

        if (rktp)
                rkmessage->partition = rktp->rktp_partition;

        if (!rkmessage->err)
                rkmessage->err = rko->rko_err;

        /* Call on_consume interceptors */
        switch (rko->rko_type) {
        case RD_KAFKA_OP_FETCH:
                if (!rkmessage->err && rkt)
                        rd_kafka_interceptors_on_consume(rkt->rkt_rk,
                                                         rkmessage);
                break;

        default:
                break;
        }

        return rkmessage;
}



/**
 * @brief Get rkmessage from rkm (for EVENT_DR)
 * @remark Must only be called just prior to passing a dr to the application.
 */
rd_kafka_message_t *rd_kafka_message_get_from_rkm(rd_kafka_op_t *rko,
                                                  rd_kafka_msg_t *rkm) {
        return rd_kafka_message_setup(rko, &rkm->rkm_rkmessage);
}

/**
 * @brief Convert rko to rkmessage
 * @remark Must only be called just prior to passing a consumed message
 *         or event to the application.
 * @remark Will trigger on_consume() interceptors, if any.
 * @returns a rkmessage (bound to the rko).
 */
rd_kafka_message_t *rd_kafka_message_get(rd_kafka_op_t *rko) {
        rd_kafka_message_t *rkmessage;

        if (!rko)
                return rd_kafka_message_new(); /* empty */

        switch (rko->rko_type) {
        case RD_KAFKA_OP_FETCH:
                /* Use embedded rkmessage */
                rkmessage = &rko->rko_u.fetch.rkm.rkm_rkmessage;
                break;

        case RD_KAFKA_OP_ERR:
        case RD_KAFKA_OP_CONSUMER_ERR:
                rkmessage          = &rko->rko_u.err.rkm.rkm_rkmessage;
                rkmessage->payload = rko->rko_u.err.errstr;
                rkmessage->len =
                    rkmessage->payload ? strlen(rkmessage->payload) : 0;
                rkmessage->offset = rko->rko_u.err.offset;
                break;

        default:
                rd_kafka_assert(NULL, !*"unhandled optype");
                RD_NOTREACHED();
                return NULL;
        }

        return rd_kafka_message_setup(rko, rkmessage);
}


int64_t rd_kafka_message_timestamp(const rd_kafka_message_t *rkmessage,
                                   rd_kafka_timestamp_type_t *tstype) {
        rd_kafka_msg_t *rkm;

        if (rkmessage->err) {
                if (tstype)
                        *tstype = RD_KAFKA_TIMESTAMP_NOT_AVAILABLE;
                return -1;
        }

        rkm = rd_kafka_message2msg((rd_kafka_message_t *)rkmessage);

        if (tstype)
                *tstype = rkm->rkm_tstype;

        return rkm->rkm_timestamp;
}


int64_t rd_kafka_message_latency(const rd_kafka_message_t *rkmessage) {
        rd_kafka_msg_t *rkm;

        rkm = rd_kafka_message2msg((rd_kafka_message_t *)rkmessage);

        if (unlikely(!rkm->rkm_ts_enq))
                return -1;

        return rd_clock() - rkm->rkm_ts_enq;
}


int32_t rd_kafka_message_broker_id(const rd_kafka_message_t *rkmessage) {
        rd_kafka_msg_t *rkm;

        rkm = rd_kafka_message2msg((rd_kafka_message_t *)rkmessage);

        return rkm->rkm_broker_id;
}



/**
 * @brief Parse serialized message headers and populate
 *        rkm->rkm_headers (which must be NULL).
 */
static rd_kafka_resp_err_t rd_kafka_msg_headers_parse(rd_kafka_msg_t *rkm) {
        rd_kafka_buf_t *rkbuf;
        int64_t HeaderCount;
        const int log_decode_errors = 0;
        rd_kafka_resp_err_t err     = RD_KAFKA_RESP_ERR__BAD_MSG;
        int i;
        rd_kafka_headers_t *hdrs = NULL;

        rd_dassert(!rkm->rkm_headers);

        if (RD_KAFKAP_BYTES_LEN(&rkm->rkm_u.consumer.binhdrs) == 0)
                return RD_KAFKA_RESP_ERR__NOENT;

        rkbuf = rd_kafka_buf_new_shadow(
            rkm->rkm_u.consumer.binhdrs.data,
            RD_KAFKAP_BYTES_LEN(&rkm->rkm_u.consumer.binhdrs), NULL);

        rd_kafka_buf_read_varint(rkbuf, &HeaderCount);

        if (HeaderCount <= 0) {
                rd_kafka_buf_destroy(rkbuf);
                return RD_KAFKA_RESP_ERR__NOENT;
        } else if (unlikely(HeaderCount > 100000)) {
                rd_kafka_buf_destroy(rkbuf);
                return RD_KAFKA_RESP_ERR__BAD_MSG;
        }

        hdrs = rd_kafka_headers_new((size_t)HeaderCount);

        for (i = 0; (int64_t)i < HeaderCount; i++) {
                int64_t KeyLen, ValueLen;
                const char *Key, *Value;

                rd_kafka_buf_read_varint(rkbuf, &KeyLen);
                rd_kafka_buf_read_ptr(rkbuf, &Key, (size_t)KeyLen);

                rd_kafka_buf_read_varint(rkbuf, &ValueLen);
                if (unlikely(ValueLen == -1))
                        Value = NULL;
                else
                        rd_kafka_buf_read_ptr(rkbuf, &Value, (size_t)ValueLen);

                rd_kafka_header_add(hdrs, Key, (ssize_t)KeyLen, Value,
                                    (ssize_t)ValueLen);
        }

        rkm->rkm_headers = hdrs;

        rd_kafka_buf_destroy(rkbuf);
        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        err = rkbuf->rkbuf_err;
        rd_kafka_buf_destroy(rkbuf);
        if (hdrs)
                rd_kafka_headers_destroy(hdrs);
        return err;
}



rd_kafka_resp_err_t
rd_kafka_message_headers(const rd_kafka_message_t *rkmessage,
                         rd_kafka_headers_t **hdrsp) {
        rd_kafka_msg_t *rkm;
        rd_kafka_resp_err_t err;

        rkm = rd_kafka_message2msg((rd_kafka_message_t *)rkmessage);

        if (rkm->rkm_headers) {
                *hdrsp = rkm->rkm_headers;
                return RD_KAFKA_RESP_ERR_NO_ERROR;
        }

        /* Producer (rkm_headers will be set if there were any headers) */
        if (rkm->rkm_flags & RD_KAFKA_MSG_F_PRODUCER)
                return RD_KAFKA_RESP_ERR__NOENT;

        /* Consumer */

        /* No previously parsed headers, check if the underlying
         * protocol message had headers and if so, parse them. */
        if (unlikely(!RD_KAFKAP_BYTES_LEN(&rkm->rkm_u.consumer.binhdrs)))
                return RD_KAFKA_RESP_ERR__NOENT;

        err = rd_kafka_msg_headers_parse(rkm);
        if (unlikely(err))
                return err;

        *hdrsp = rkm->rkm_headers;
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


rd_kafka_resp_err_t
rd_kafka_message_detach_headers(rd_kafka_message_t *rkmessage,
                                rd_kafka_headers_t **hdrsp) {
        rd_kafka_msg_t *rkm;
        rd_kafka_resp_err_t err;

        err = rd_kafka_message_headers(rkmessage, hdrsp);
        if (err)
                return err;

        rkm = rd_kafka_message2msg((rd_kafka_message_t *)rkmessage);
        rkm->rkm_headers = NULL;

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


void rd_kafka_message_set_headers(rd_kafka_message_t *rkmessage,
                                  rd_kafka_headers_t *hdrs) {
        rd_kafka_msg_t *rkm;

        rkm = rd_kafka_message2msg((rd_kafka_message_t *)rkmessage);

        if (rkm->rkm_headers) {
                assert(rkm->rkm_headers != hdrs);
                rd_kafka_headers_destroy(rkm->rkm_headers);
        }

        rkm->rkm_headers = hdrs;
}



rd_kafka_msg_status_t
rd_kafka_message_status(const rd_kafka_message_t *rkmessage) {
        rd_kafka_msg_t *rkm;

        rkm = rd_kafka_message2msg((rd_kafka_message_t *)rkmessage);

        return rkm->rkm_status;
}


int32_t rd_kafka_message_leader_epoch(const rd_kafka_message_t *rkmessage) {
        rd_kafka_msg_t *rkm;
        if (unlikely(!rkmessage->rkt || rd_kafka_rkt_is_lw(rkmessage->rkt) ||
                     !rkmessage->rkt->rkt_rk ||
                     rkmessage->rkt->rkt_rk->rk_type != RD_KAFKA_CONSUMER))
                return -1;

        rkm = rd_kafka_message2msg((rd_kafka_message_t *)rkmessage);

        return rkm->rkm_u.consumer.leader_epoch;
}


void rd_kafka_msgq_dump(FILE *fp, const char *what, rd_kafka_msgq_t *rkmq) {
        rd_kafka_msg_t *rkm;
        int cnt = 0;

        fprintf(fp, "%s msgq_dump (%d messages, %" PRIusz " bytes):\n", what,
                rd_kafka_msgq_len(rkmq), rd_kafka_msgq_size(rkmq));
        TAILQ_FOREACH(rkm, &rkmq->rkmq_msgs, rkm_link) {
                fprintf(fp,
                        " [%" PRId32 "]@%" PRId64 ": rkm msgid %" PRIu64
                        ": \"%.*s\"\n",
                        rkm->rkm_partition, rkm->rkm_offset,
                        rkm->rkm_u.producer.msgid, (int)rkm->rkm_len,
                        (const char *)rkm->rkm_payload);
                rd_assert(cnt++ < rkmq->rkmq_msg_cnt);
        }
}



/**
 * @brief Destroy resources associated with msgbatch
 */
void rd_kafka_msgbatch_destroy(rd_kafka_msgbatch_t *rkmb) {
        if (rkmb->rktp) {
                rd_kafka_toppar_destroy(rkmb->rktp);
                rkmb->rktp = NULL;
        }

        rd_assert(RD_KAFKA_MSGQ_EMPTY(&rkmb->msgq));
}


/**
 * @brief Initialize a message batch for the Idempotent Producer.
 */
void rd_kafka_msgbatch_init(rd_kafka_msgbatch_t *rkmb,
                            rd_kafka_toppar_t *rktp,
                            rd_kafka_pid_t pid,
                            uint64_t epoch_base_msgid) {
        memset(rkmb, 0, sizeof(*rkmb));

        rkmb->rktp = rd_kafka_toppar_keep(rktp);

        rd_kafka_msgq_init(&rkmb->msgq);

        rkmb->pid              = pid;
        rkmb->first_seq        = -1;
        rkmb->epoch_base_msgid = epoch_base_msgid;
}


/**
 * @brief Set the first message in the batch. which is used to set
 *        the BaseSequence and keep track of batch reconstruction range.
 *
 * @param rkm is the first message in the batch.
 */
void rd_kafka_msgbatch_set_first_msg(rd_kafka_msgbatch_t *rkmb,
                                     rd_kafka_msg_t *rkm) {
        rd_assert(rkmb->first_msgid == 0);

        if (!rd_kafka_pid_valid(rkmb->pid))
                return;

        rkmb->first_msgid = rkm->rkm_u.producer.msgid;

        /* Our msgid counter is 64-bits, but the
         * Kafka protocol's sequence is only 31 (signed), so we'll
         * need to handle wrapping. */
        rkmb->first_seq = rd_kafka_seq_wrap(rkm->rkm_u.producer.msgid -
                                            rkmb->epoch_base_msgid);

        /* Check if there is a stored last message
         * on the first msg, which means an entire
         * batch of messages are being retried and
         * we need to maintain the exact messages
         * of the original batch.
         * Simply tracking the last message, on
         * the first message, is sufficient for now.
         * Will be 0 if not applicable. */
        rkmb->last_msgid = rkm->rkm_u.producer.last_msgid;
}



/**
 * @brief Message batch is ready to be transmitted.
 *
 * @remark This function assumes the batch will be transmitted and increases
 *         the toppar's in-flight count.
 */
void rd_kafka_msgbatch_ready_produce(rd_kafka_msgbatch_t *rkmb) {
        rd_kafka_toppar_t *rktp = rkmb->rktp;
        rd_kafka_t *rk          = rktp->rktp_rkt->rkt_rk;

        /* Keep track of number of requests in-flight per partition,
         * and the number of partitions with in-flight requests when
         * idempotent producer - this is used to drain partitions
         * before resetting the PID. */
        if (rd_atomic32_add(&rktp->rktp_msgs_inflight,
                            rd_kafka_msgq_len(&rkmb->msgq)) ==
                rd_kafka_msgq_len(&rkmb->msgq) &&
            rd_kafka_is_idempotent(rk))
                rd_kafka_idemp_inflight_toppar_add(rk, rktp);
}



/**
 * @brief Allow queue wakeups after \p abstime, or when the
 *        given \p batch_msg_cnt or \p batch_msg_bytes have been reached.
 *
 * @param rkmq Queue to monitor and set wakeup parameters on.
 * @param dest_rkmq Destination queue used to meter current queue depths
 *                  and oldest message. May be the same as \p rkmq but is
 *                  typically the rktp_xmit_msgq.
 * @param next_wakeup If non-NULL: update the caller's next scheduler wakeup
 *                    according to the wakeup time calculated by this function.
 * @param now The current time.
 * @param linger_us The configured queue linger / batching time.
 * @param batch_msg_cnt Queue threshold before signalling.
 * @param batch_msg_bytes Queue threshold before signalling.
 *
 * @returns true if the wakeup conditions are already met and messages are ready
 *          to be sent, else false.
 *
 * @locks_required rd_kafka_toppar_lock()
 *
 *
 * Producer queue and broker thread wake-up behaviour.
 *
 * There are contradicting requirements at play here:
 *  - Latency: queued messages must be batched and sent according to
 *             batch size and linger.ms configuration.
 *  - Wakeups: keep the number of thread wake-ups to a minimum to avoid
 *             high CPU utilization and context switching.
 *
 * The message queue (rd_kafka_msgq_t) has functionality for the writer (app)
 * to wake up the reader (broker thread) when there's a new message added.
 * This wakeup is done thru a combination of cndvar signalling and IO writes
 * to make sure a thread wakeup is triggered regardless if the broker thread
 * is blocking on cnd_timedwait() or on IO poll.
 * When the broker thread is woken up it will scan all the partitions it is
 * the leader for to check if there are messages to be sent - all according
 * to the configured batch size and linger.ms - and then decide its next
 * wait time depending on the lowest remaining linger.ms setting of any
 * partition with messages enqueued.
 *
 * This wait time must also be set as a threshold on the message queue, telling
 * the writer (app) that it must not trigger a wakeup until the wait time
 * has expired, or the batch sizes have been exceeded.
 *
 * The message queue wakeup time is per partition, while the broker thread
 * wakeup time is the lowest of all its partitions' wakeup times.
 *
 * The per-partition wakeup constraints are calculated and set by
 * rd_kafka_msgq_allow_wakeup_at() which is called from the broker thread's
 * per-partition handler.
 * This function is called each time there are changes to the broker-local
 * partition transmit queue (rktp_xmit_msgq), such as:
 *  - messages are moved from the partition queue (rktp_msgq) to rktp_xmit_msgq
 *  - messages are moved to a ProduceRequest
 *  - messages are timed out from the rktp_xmit_msgq
 *  - the flushing state changed (rd_kafka_flush() is called or returned).
 *
 * If none of these things happen, the broker thread will simply read the
 * last stored wakeup time for each partition and use that for calculating its
 * minimum wait time.
 *
 *
 * On the writer side, namely the application calling rd_kafka_produce(), the
 * followings checks are performed to see if it may trigger a wakeup when
 * it adds a new message to the partition queue:
 *  - the current time has reached the wakeup time (e.g., remaining linger.ms
 *    has expired), or
 *  - with the new message(s) being added, either the batch.size or
 *    batch.num.messages thresholds have been exceeded, or
 *  - the application is calling rd_kafka_flush(),
 *  - and no wakeup has been signalled yet. This is critical since it may take
 *    some time for the broker thread to do its work we'll want to avoid
 *    flooding it with wakeups. So a wakeup is only sent once per
 *    wakeup period.
 */
rd_bool_t rd_kafka_msgq_allow_wakeup_at(rd_kafka_msgq_t *rkmq,
                                        const rd_kafka_msgq_t *dest_rkmq,
                                        rd_ts_t *next_wakeup,
                                        rd_ts_t now,
                                        rd_ts_t linger_us,
                                        int32_t batch_msg_cnt,
                                        int64_t batch_msg_bytes) {
        int32_t msg_cnt   = rd_kafka_msgq_len(dest_rkmq);
        int64_t msg_bytes = rd_kafka_msgq_size(dest_rkmq);

        if (RD_KAFKA_MSGQ_EMPTY(dest_rkmq)) {
                rkmq->rkmq_wakeup.on_first = rd_true;
                rkmq->rkmq_wakeup.abstime  = now + linger_us;
                /* Leave next_wakeup untouched since the queue is empty */
                msg_cnt   = 0;
                msg_bytes = 0;
        } else {
                const rd_kafka_msg_t *rkm = rd_kafka_msgq_first(dest_rkmq);

                rkmq->rkmq_wakeup.on_first = rd_false;

                if (unlikely(rkm->rkm_u.producer.ts_backoff > now)) {
                        /* Honour retry.backoff.ms:
                         * wait for backoff to expire */
                        rkmq->rkmq_wakeup.abstime =
                            rkm->rkm_u.producer.ts_backoff;
                } else {
                        /* Use message's produce() time + linger.ms */
                        rkmq->rkmq_wakeup.abstime =
                            rd_kafka_msg_enq_time(rkm) + linger_us;
                        if (rkmq->rkmq_wakeup.abstime <= now)
                                rkmq->rkmq_wakeup.abstime = now;
                }

                /* Update the caller's scheduler wakeup time */
                if (next_wakeup && rkmq->rkmq_wakeup.abstime < *next_wakeup)
                        *next_wakeup = rkmq->rkmq_wakeup.abstime;

                msg_cnt   = rd_kafka_msgq_len(dest_rkmq);
                msg_bytes = rd_kafka_msgq_size(dest_rkmq);
        }

        /*
         * If there are more messages or bytes in queue than the batch limits,
         * or the linger time has been exceeded,
         * then there is no need for wakeup since the broker thread will
         * produce those messages as quickly as it can.
         */
        if (msg_cnt >= batch_msg_cnt || msg_bytes >= batch_msg_bytes ||
            (msg_cnt > 0 && now >= rkmq->rkmq_wakeup.abstime)) {
                /* Prevent further signalling */
                rkmq->rkmq_wakeup.signalled = rd_true;

                /* Batch is ready */
                return rd_true;
        }

        /* If the current msg or byte count is less than the batch limit
         * then set the rkmq count to the remaining count or size to
         * reach the batch limits.
         * This is for the case where the producer is waiting for more
         * messages to accumulate into a batch. The wakeup should only
         * occur once a threshold is reached or the abstime has expired.
         */
        rkmq->rkmq_wakeup.signalled = rd_false;
        rkmq->rkmq_wakeup.msg_cnt   = batch_msg_cnt - msg_cnt;
        rkmq->rkmq_wakeup.msg_bytes = batch_msg_bytes - msg_bytes;

        return rd_false;
}



/**
 * @brief Verify order (by msgid) in message queue.
 *        For development use only.
 */
void rd_kafka_msgq_verify_order0(const char *function,
                                 int line,
                                 const rd_kafka_toppar_t *rktp,
                                 const rd_kafka_msgq_t *rkmq,
                                 uint64_t exp_first_msgid,
                                 rd_bool_t gapless) {
        const rd_kafka_msg_t *rkm;
        uint64_t exp;
        int errcnt        = 0;
        int cnt           = 0;
        const char *topic = rktp ? rktp->rktp_rkt->rkt_topic->str : "n/a";
        int32_t partition = rktp ? rktp->rktp_partition : -1;

        if (rd_kafka_msgq_len(rkmq) == 0)
                return;

        if (exp_first_msgid)
                exp = exp_first_msgid;
        else {
                exp = rd_kafka_msgq_first(rkmq)->rkm_u.producer.msgid;
                if (exp == 0) /* message without msgid (e.g., UA partition) */
                        return;
        }

        TAILQ_FOREACH(rkm, &rkmq->rkmq_msgs, rkm_link) {
#if 0
                printf("%s:%d: %s [%"PRId32"]: rkm #%d (%p) "
                       "msgid %"PRIu64"\n",
                       function, line,
                       topic, partition,
                       cnt, rkm, rkm->rkm_u.producer.msgid);
#endif
                if (gapless && rkm->rkm_u.producer.msgid != exp) {
                        printf("%s:%d: %s [%" PRId32
                               "]: rkm #%d (%p) "
                               "msgid %" PRIu64
                               ": "
                               "expected msgid %" PRIu64 "\n",
                               function, line, topic, partition, cnt, rkm,
                               rkm->rkm_u.producer.msgid, exp);
                        errcnt++;
                } else if (!gapless && rkm->rkm_u.producer.msgid < exp) {
                        printf("%s:%d: %s [%" PRId32
                               "]: rkm #%d (%p) "
                               "msgid %" PRIu64
                               ": "
                               "expected increased msgid >= %" PRIu64 "\n",
                               function, line, topic, partition, cnt, rkm,
                               rkm->rkm_u.producer.msgid, exp);
                        errcnt++;
                } else
                        exp++;

                if (cnt >= rkmq->rkmq_msg_cnt) {
                        printf("%s:%d: %s [%" PRId32
                               "]: rkm #%d (%p) "
                               "msgid %" PRIu64 ": loop in queue?\n",
                               function, line, topic, partition, cnt, rkm,
                               rkm->rkm_u.producer.msgid);
                        errcnt++;
                        break;
                }

                cnt++;
        }

        rd_assert(!errcnt);
}

rd_kafka_Produce_result_t *rd_kafka_Produce_result_new(int64_t offset,
                                                       int64_t timestamp) {
        rd_kafka_Produce_result_t *ret = rd_calloc(1, sizeof(*ret));
        ret->offset                    = offset;
        ret->timestamp                 = timestamp;
        return ret;
}

void rd_kafka_Produce_result_destroy(rd_kafka_Produce_result_t *result) {
        if (result->record_errors) {
                int32_t i;
                for (i = 0; i < result->record_errors_cnt; i++) {
                        RD_IF_FREE(result->record_errors[i].errstr, rd_free);
                }
                rd_free(result->record_errors);
        }
        RD_IF_FREE(result->errstr, rd_free);
        rd_free(result);
}

rd_kafka_Produce_result_t *
rd_kafka_Produce_result_copy(const rd_kafka_Produce_result_t *result) {
        rd_kafka_Produce_result_t *ret = rd_calloc(1, sizeof(*ret));
        *ret                           = *result;
        if (result->errstr)
                ret->errstr = rd_strdup(result->errstr);
        if (result->record_errors) {
                ret->record_errors = rd_calloc(result->record_errors_cnt,
                                               sizeof(*result->record_errors));
                int32_t i;
                for (i = 0; i < result->record_errors_cnt; i++) {
                        ret->record_errors[i] = result->record_errors[i];
                        if (result->record_errors[i].errstr)
                                ret->record_errors[i].errstr =
                                    rd_strdup(result->record_errors[i].errstr);
                }
        }
        return ret;
}

/**
 * @name Unit tests
 */

/**
 * @brief Unittest: message allocator
 */
rd_kafka_msg_t *ut_rd_kafka_msg_new(size_t msgsize) {
        rd_kafka_msg_t *rkm;

        rkm             = rd_calloc(1, sizeof(*rkm));
        rkm->rkm_flags  = RD_KAFKA_MSG_F_FREE_RKM;
        rkm->rkm_offset = RD_KAFKA_OFFSET_INVALID;
        rkm->rkm_tstype = RD_KAFKA_TIMESTAMP_NOT_AVAILABLE;

        if (msgsize) {
                rd_assert(msgsize <= sizeof(*rkm));
                rkm->rkm_payload = rkm;
                rkm->rkm_len     = msgsize;
        }

        return rkm;
}



/**
 * @brief Unittest: destroy all messages in queue
 */
void ut_rd_kafka_msgq_purge(rd_kafka_msgq_t *rkmq) {
        rd_kafka_msg_t *rkm, *tmp;

        TAILQ_FOREACH_SAFE(rkm, &rkmq->rkmq_msgs, rkm_link, tmp)
        rd_kafka_msg_destroy(NULL, rkm);


        rd_kafka_msgq_init(rkmq);
}



static int ut_verify_msgq_order(const char *what,
                                const rd_kafka_msgq_t *rkmq,
                                uint64_t first,
                                uint64_t last,
                                rd_bool_t req_consecutive) {
        const rd_kafka_msg_t *rkm;
        uint64_t expected = first;
        int incr          = first < last ? +1 : -1;
        int fails         = 0;
        int cnt           = 0;

        TAILQ_FOREACH(rkm, &rkmq->rkmq_msgs, rkm_link) {
                if ((req_consecutive &&
                     rkm->rkm_u.producer.msgid != expected) ||
                    (!req_consecutive &&
                     rkm->rkm_u.producer.msgid < expected)) {
                        if (fails++ < 100)
                                RD_UT_SAY("%s: expected msgid %s %" PRIu64
                                          " not %" PRIu64 " at index #%d",
                                          what, req_consecutive ? "==" : ">=",
                                          expected, rkm->rkm_u.producer.msgid,
                                          cnt);
                }

                cnt++;
                expected += incr;

                if (cnt > rkmq->rkmq_msg_cnt) {
                        RD_UT_SAY("%s: loop in queue?", what);
                        fails++;
                        break;
                }
        }

        RD_UT_ASSERT(!fails, "See %d previous failure(s)", fails);
        return fails;
}

/**
 * @brief Verify ordering comparator for message queues.
 */
static int unittest_msgq_order(const char *what,
                               int fifo,
                               int (*cmp)(const void *, const void *)) {
        rd_kafka_msgq_t rkmq = RD_KAFKA_MSGQ_INITIALIZER(rkmq);
        rd_kafka_msg_t *rkm;
        rd_kafka_msgq_t sendq, sendq2;
        const size_t msgsize = 100;
        int i;

        RD_UT_SAY("%s: testing in %s mode", what, fifo ? "FIFO" : "LIFO");

        for (i = 1; i <= 6; i++) {
                rkm                       = ut_rd_kafka_msg_new(msgsize);
                rkm->rkm_u.producer.msgid = i;
                rd_kafka_msgq_enq_sorted0(&rkmq, rkm, cmp);
        }

        if (fifo) {
                if (ut_verify_msgq_order("added", &rkmq, 1, 6, rd_true))
                        return 1;
        } else {
                if (ut_verify_msgq_order("added", &rkmq, 6, 1, rd_true))
                        return 1;
        }

        /* Move 3 messages to "send" queue which we then re-insert
         * in the original queue (i.e., "retry"). */
        rd_kafka_msgq_init(&sendq);
        while (rd_kafka_msgq_len(&sendq) < 3)
                rd_kafka_msgq_enq(&sendq, rd_kafka_msgq_pop(&rkmq));

        if (fifo) {
                if (ut_verify_msgq_order("send removed", &rkmq, 4, 6, rd_true))
                        return 1;

                if (ut_verify_msgq_order("sendq", &sendq, 1, 3, rd_true))
                        return 1;
        } else {
                if (ut_verify_msgq_order("send removed", &rkmq, 3, 1, rd_true))
                        return 1;

                if (ut_verify_msgq_order("sendq", &sendq, 6, 4, rd_true))
                        return 1;
        }

        /* Retry the messages, which moves them back to sendq
         * maintaining the original order with exponential backoff
         * set to false */
        rd_kafka_retry_msgq(&rkmq, &sendq, 1, 1, 0,
                            RD_KAFKA_MSG_STATUS_NOT_PERSISTED, cmp, rd_false, 0,
                            0);

        RD_UT_ASSERT(rd_kafka_msgq_len(&sendq) == 0,
                     "sendq FIFO should be empty, not contain %d messages",
                     rd_kafka_msgq_len(&sendq));

        if (fifo) {
                if (ut_verify_msgq_order("readded", &rkmq, 1, 6, rd_true))
                        return 1;
        } else {
                if (ut_verify_msgq_order("readded", &rkmq, 6, 1, rd_true))
                        return 1;
        }

        /* Move 4 first messages to to "send" queue, then
         * retry them with max_retries=1 which should now fail for
         * the 3 first messages that were already retried. */
        rd_kafka_msgq_init(&sendq);
        while (rd_kafka_msgq_len(&sendq) < 4)
                rd_kafka_msgq_enq(&sendq, rd_kafka_msgq_pop(&rkmq));

        if (fifo) {
                if (ut_verify_msgq_order("send removed #2", &rkmq, 5, 6,
                                         rd_true))
                        return 1;

                if (ut_verify_msgq_order("sendq #2", &sendq, 1, 4, rd_true))
                        return 1;
        } else {
                if (ut_verify_msgq_order("send removed #2", &rkmq, 2, 1,
                                         rd_true))
                        return 1;

                if (ut_verify_msgq_order("sendq #2", &sendq, 6, 3, rd_true))
                        return 1;
        }

        /* Retry the messages, which should now keep the 3 first messages
         * on sendq (no more retries) and just number 4 moved back.
         * No exponential backoff applied. */
        rd_kafka_retry_msgq(&rkmq, &sendq, 1, 1, 0,
                            RD_KAFKA_MSG_STATUS_NOT_PERSISTED, cmp, rd_false, 0,
                            0);

        if (fifo) {
                if (ut_verify_msgq_order("readded #2", &rkmq, 4, 6, rd_true))
                        return 1;

                if (ut_verify_msgq_order("no more retries", &sendq, 1, 3,
                                         rd_true))
                        return 1;

        } else {
                if (ut_verify_msgq_order("readded #2", &rkmq, 3, 1, rd_true))
                        return 1;

                if (ut_verify_msgq_order("no more retries", &sendq, 6, 4,
                                         rd_true))
                        return 1;
        }

        /* Move all messages back on rkmq without any exponential backoff. */
        rd_kafka_retry_msgq(&rkmq, &sendq, 0, 1000, 0,
                            RD_KAFKA_MSG_STATUS_NOT_PERSISTED, cmp, rd_false, 0,
                            0);


        /* Move first half of messages to sendq (1,2,3).
         * Move second half o messages to sendq2 (4,5,6).
         * Add new message to rkmq (7).
         * Move first half of messages back on rkmq (1,2,3,7).
         * Move second half back on the rkmq (1,2,3,4,5,6,7). */
        rd_kafka_msgq_init(&sendq);
        rd_kafka_msgq_init(&sendq2);

        while (rd_kafka_msgq_len(&sendq) < 3)
                rd_kafka_msgq_enq(&sendq, rd_kafka_msgq_pop(&rkmq));

        while (rd_kafka_msgq_len(&sendq2) < 3)
                rd_kafka_msgq_enq(&sendq2, rd_kafka_msgq_pop(&rkmq));

        rkm                       = ut_rd_kafka_msg_new(msgsize);
        rkm->rkm_u.producer.msgid = i;
        rd_kafka_msgq_enq_sorted0(&rkmq, rkm, cmp);
        /* No exponential backoff applied. */
        rd_kafka_retry_msgq(&rkmq, &sendq, 0, 1000, 0,
                            RD_KAFKA_MSG_STATUS_NOT_PERSISTED, cmp, rd_false, 0,
                            0);
        /* No exponential backoff applied. */
        rd_kafka_retry_msgq(&rkmq, &sendq2, 0, 1000, 0,
                            RD_KAFKA_MSG_STATUS_NOT_PERSISTED, cmp, rd_false, 0,
                            0);

        RD_UT_ASSERT(rd_kafka_msgq_len(&sendq) == 0,
                     "sendq FIFO should be empty, not contain %d messages",
                     rd_kafka_msgq_len(&sendq));
        RD_UT_ASSERT(rd_kafka_msgq_len(&sendq2) == 0,
                     "sendq2 FIFO should be empty, not contain %d messages",
                     rd_kafka_msgq_len(&sendq2));

        if (fifo) {
                if (ut_verify_msgq_order("inject", &rkmq, 1, 7, rd_true))
                        return 1;
        } else {
                if (ut_verify_msgq_order("readded #2", &rkmq, 7, 1, rd_true))
                        return 1;
        }

        RD_UT_ASSERT(rd_kafka_msgq_size(&rkmq) ==
                         rd_kafka_msgq_len(&rkmq) * msgsize,
                     "expected msgq size %" PRIusz ", not %" PRIusz,
                     (size_t)rd_kafka_msgq_len(&rkmq) * msgsize,
                     rd_kafka_msgq_size(&rkmq));


        ut_rd_kafka_msgq_purge(&sendq);
        ut_rd_kafka_msgq_purge(&sendq2);
        ut_rd_kafka_msgq_purge(&rkmq);

        return 0;
}

/**
 * @brief Verify that rd_kafka_seq_wrap() works.
 */
static int unittest_msg_seq_wrap(void) {
        static const struct exp {
                int64_t in;
                int32_t out;
        } exp[] = {
            {0, 0},
            {1, 1},
            {(int64_t)INT32_MAX + 2, 1},
            {(int64_t)INT32_MAX + 1, 0},
            {INT32_MAX, INT32_MAX},
            {INT32_MAX - 1, INT32_MAX - 1},
            {INT32_MAX - 2, INT32_MAX - 2},
            {((int64_t)1 << 33) - 2, INT32_MAX - 1},
            {((int64_t)1 << 33) - 1, INT32_MAX},
            {((int64_t)1 << 34), 0},
            {((int64_t)1 << 35) + 3, 3},
            {1710 + 1229, 2939},
            {-1, -1},
        };
        int i;

        for (i = 0; exp[i].in != -1; i++) {
                int32_t wseq = rd_kafka_seq_wrap(exp[i].in);
                RD_UT_ASSERT(wseq == exp[i].out,
                             "Expected seq_wrap(%" PRId64 ") -> %" PRId32
                             ", not %" PRId32,
                             exp[i].in, exp[i].out, wseq);
        }

        RD_UT_PASS();
}


/**
 * @brief Populate message queue with message ids from lo..hi (inclusive)
 */
static void ut_msgq_populate(rd_kafka_msgq_t *rkmq,
                             uint64_t lo,
                             uint64_t hi,
                             size_t msgsize) {
        uint64_t i;

        for (i = lo; i <= hi; i++) {
                rd_kafka_msg_t *rkm       = ut_rd_kafka_msg_new(msgsize);
                rkm->rkm_u.producer.msgid = i;
                rd_kafka_msgq_enq(rkmq, rkm);
        }
}


struct ut_msg_range {
        uint64_t lo;
        uint64_t hi;
};

/**
 * @brief Verify that msgq insert sorts are optimized. Issue #2508.
 *        All source ranges are combined into a single queue before insert.
 */
static int
unittest_msgq_insert_all_sort(const char *what,
                              double max_us_per_msg,
                              double *ret_us_per_msg,
                              const struct ut_msg_range *src_ranges,
                              const struct ut_msg_range *dest_ranges) {
        rd_kafka_msgq_t destq, srcq;
        int i;
        uint64_t lo = UINT64_MAX, hi = 0;
        uint64_t cnt         = 0;
        const size_t msgsize = 100;
        size_t totsize       = 0;
        rd_ts_t ts;
        double us_per_msg;

        RD_UT_SAY("Testing msgq insert (all) efficiency: %s", what);

        rd_kafka_msgq_init(&destq);
        rd_kafka_msgq_init(&srcq);

        for (i = 0; src_ranges[i].hi > 0; i++) {
                uint64_t this_cnt;

                ut_msgq_populate(&srcq, src_ranges[i].lo, src_ranges[i].hi,
                                 msgsize);
                if (src_ranges[i].lo < lo)
                        lo = src_ranges[i].lo;
                if (src_ranges[i].hi > hi)
                        hi = src_ranges[i].hi;
                this_cnt = (src_ranges[i].hi - src_ranges[i].lo) + 1;
                cnt += this_cnt;
                totsize += msgsize * (size_t)this_cnt;
        }

        for (i = 0; dest_ranges[i].hi > 0; i++) {
                uint64_t this_cnt;

                ut_msgq_populate(&destq, dest_ranges[i].lo, dest_ranges[i].hi,
                                 msgsize);
                if (dest_ranges[i].lo < lo)
                        lo = dest_ranges[i].lo;
                if (dest_ranges[i].hi > hi)
                        hi = dest_ranges[i].hi;
                this_cnt = (dest_ranges[i].hi - dest_ranges[i].lo) + 1;
                cnt += this_cnt;
                totsize += msgsize * (size_t)this_cnt;
        }

        RD_UT_SAY("Begin insert of %d messages into destq with %d messages",
                  rd_kafka_msgq_len(&srcq), rd_kafka_msgq_len(&destq));

        ts = rd_clock();
        rd_kafka_msgq_insert_msgq(&destq, &srcq, rd_kafka_msg_cmp_msgid);
        ts         = rd_clock() - ts;
        us_per_msg = (double)ts / (double)cnt;

        RD_UT_SAY("Done: took %" PRId64 "us, %.4fus/msg", ts, us_per_msg);

        RD_UT_ASSERT(rd_kafka_msgq_len(&srcq) == 0,
                     "srcq should be empty, but contains %d messages",
                     rd_kafka_msgq_len(&srcq));
        RD_UT_ASSERT(rd_kafka_msgq_len(&destq) == (int)cnt,
                     "destq should contain %d messages, not %d", (int)cnt,
                     rd_kafka_msgq_len(&destq));

        if (ut_verify_msgq_order("after", &destq, lo, hi, rd_false))
                return 1;

        RD_UT_ASSERT(rd_kafka_msgq_size(&destq) == totsize,
                     "expected destq size to be %" PRIusz
                     " bytes, not %" PRIusz,
                     totsize, rd_kafka_msgq_size(&destq));

        ut_rd_kafka_msgq_purge(&srcq);
        ut_rd_kafka_msgq_purge(&destq);

        if (!rd_unittest_slow)
                RD_UT_ASSERT(!(us_per_msg > max_us_per_msg + 0.0001),
                             "maximum us/msg exceeded: %.4f > %.4f us/msg",
                             us_per_msg, max_us_per_msg);
        else if (us_per_msg > max_us_per_msg + 0.0001)
                RD_UT_WARN("maximum us/msg exceeded: %.4f > %.4f us/msg",
                           us_per_msg, max_us_per_msg);

        if (ret_us_per_msg)
                *ret_us_per_msg = us_per_msg;

        RD_UT_PASS();
}


/**
 * @brief Verify that msgq insert sorts are optimized. Issue #2508.
 *        Inserts each source range individually.
 */
static int
unittest_msgq_insert_each_sort(const char *what,
                               double max_us_per_msg,
                               double *ret_us_per_msg,
                               const struct ut_msg_range *src_ranges,
                               const struct ut_msg_range *dest_ranges) {
        rd_kafka_msgq_t destq;
        int i;
        uint64_t lo = UINT64_MAX, hi = 0;
        uint64_t cnt         = 0;
        uint64_t scnt        = 0;
        const size_t msgsize = 100;
        size_t totsize       = 0;
        double us_per_msg;
        rd_ts_t accum_ts = 0;

        RD_UT_SAY("Testing msgq insert (each) efficiency: %s", what);

        rd_kafka_msgq_init(&destq);

        for (i = 0; dest_ranges[i].hi > 0; i++) {
                uint64_t this_cnt;

                ut_msgq_populate(&destq, dest_ranges[i].lo, dest_ranges[i].hi,
                                 msgsize);
                if (dest_ranges[i].lo < lo)
                        lo = dest_ranges[i].lo;
                if (dest_ranges[i].hi > hi)
                        hi = dest_ranges[i].hi;
                this_cnt = (dest_ranges[i].hi - dest_ranges[i].lo) + 1;
                cnt += this_cnt;
                totsize += msgsize * (size_t)this_cnt;
        }


        for (i = 0; src_ranges[i].hi > 0; i++) {
                rd_kafka_msgq_t srcq;
                uint64_t this_cnt;
                rd_ts_t ts;

                rd_kafka_msgq_init(&srcq);

                ut_msgq_populate(&srcq, src_ranges[i].lo, src_ranges[i].hi,
                                 msgsize);
                if (src_ranges[i].lo < lo)
                        lo = src_ranges[i].lo;
                if (src_ranges[i].hi > hi)
                        hi = src_ranges[i].hi;
                this_cnt = (src_ranges[i].hi - src_ranges[i].lo) + 1;
                cnt += this_cnt;
                scnt += this_cnt;
                totsize += msgsize * (size_t)this_cnt;

                RD_UT_SAY(
                    "Begin insert of %d messages into destq with "
                    "%d messages",
                    rd_kafka_msgq_len(&srcq), rd_kafka_msgq_len(&destq));

                ts = rd_clock();
                rd_kafka_msgq_insert_msgq(&destq, &srcq,
                                          rd_kafka_msg_cmp_msgid);
                ts = rd_clock() - ts;
                accum_ts += ts;

                RD_UT_SAY("Done: took %" PRId64 "us, %.4fus/msg", ts,
                          (double)ts / (double)this_cnt);

                RD_UT_ASSERT(rd_kafka_msgq_len(&srcq) == 0,
                             "srcq should be empty, but contains %d messages",
                             rd_kafka_msgq_len(&srcq));
                RD_UT_ASSERT(rd_kafka_msgq_len(&destq) == (int)cnt,
                             "destq should contain %d messages, not %d",
                             (int)cnt, rd_kafka_msgq_len(&destq));

                if (ut_verify_msgq_order("after", &destq, lo, hi, rd_false))
                        return 1;

                RD_UT_ASSERT(rd_kafka_msgq_size(&destq) == totsize,
                             "expected destq size to be %" PRIusz
                             " bytes, not %" PRIusz,
                             totsize, rd_kafka_msgq_size(&destq));

                ut_rd_kafka_msgq_purge(&srcq);
        }

        ut_rd_kafka_msgq_purge(&destq);

        us_per_msg = (double)accum_ts / (double)scnt;

        RD_UT_SAY("Total: %.4fus/msg over %" PRId64 " messages in %" PRId64
                  "us",
                  us_per_msg, scnt, accum_ts);

        if (!rd_unittest_slow)
                RD_UT_ASSERT(!(us_per_msg > max_us_per_msg + 0.0001),
                             "maximum us/msg exceeded: %.4f > %.4f us/msg",
                             us_per_msg, max_us_per_msg);
        else if (us_per_msg > max_us_per_msg + 0.0001)
                RD_UT_WARN("maximum us/msg exceeded: %.4f > %.4f us/msg",
                           us_per_msg, max_us_per_msg);


        if (ret_us_per_msg)
                *ret_us_per_msg = us_per_msg;

        RD_UT_PASS();
}



/**
 * @brief Calls both insert_all and insert_each
 */
static int unittest_msgq_insert_sort(const char *what,
                                     double max_us_per_msg,
                                     double *ret_us_per_msg,
                                     const struct ut_msg_range *src_ranges,
                                     const struct ut_msg_range *dest_ranges) {
        double ret_all = 0.0, ret_each = 0.0;
        int r;

        r = unittest_msgq_insert_all_sort(what, max_us_per_msg, &ret_all,
                                          src_ranges, dest_ranges);
        if (r)
                return r;

        r = unittest_msgq_insert_each_sort(what, max_us_per_msg, &ret_each,
                                           src_ranges, dest_ranges);
        if (r)
                return r;

        if (ret_us_per_msg)
                *ret_us_per_msg = RD_MAX(ret_all, ret_each);

        return 0;
}


int unittest_msg(void) {
        int fails              = 0;
        double insert_baseline = 0.0;

        fails += unittest_msgq_order("FIFO", 1, rd_kafka_msg_cmp_msgid);
        fails += unittest_msg_seq_wrap();

        fails += unittest_msgq_insert_sort(
            "get baseline insert time", 100000.0, &insert_baseline,
            (const struct ut_msg_range[]) {{1, 1}, {3, 3}, {0, 0}},
            (const struct ut_msg_range[]) {{2, 2}, {4, 4}, {0, 0}});

        /* Allow some wiggle room in baseline time. */
        if (insert_baseline < 0.1)
                insert_baseline = 0.2;
        insert_baseline *= 3;

        fails += unittest_msgq_insert_sort(
            "single-message ranges", insert_baseline, NULL,
            (const struct ut_msg_range[]) {
                {2, 2}, {4, 4}, {9, 9}, {33692864, 33692864}, {0, 0}},
            (const struct ut_msg_range[]) {{1, 1},
                                           {3, 3},
                                           {5, 5},
                                           {10, 10},
                                           {33692865, 33692865},
                                           {0, 0}});
        fails += unittest_msgq_insert_sort(
            "many messages", insert_baseline, NULL,
            (const struct ut_msg_range[]) {{100000, 200000},
                                           {400000, 450000},
                                           {900000, 920000},
                                           {33692864, 33751992},
                                           {33906868, 33993690},
                                           {40000000, 44000000},
                                           {0, 0}},
            (const struct ut_msg_range[]) {{1, 199},
                                           {350000, 360000},
                                           {500000, 500010},
                                           {1000000, 1000200},
                                           {33751993, 33906867},
                                           {50000001, 50000001},
                                           {0, 0}});
        fails += unittest_msgq_insert_sort(
            "issue #2508", insert_baseline, NULL,
            (const struct ut_msg_range[]) {
                {33692864, 33751992}, {33906868, 33993690}, {0, 0}},
            (const struct ut_msg_range[]) {{33751993, 33906867}, {0, 0}});

        /* The standard case where all of the srcq
         * goes after the destq.
         * Create a big destq and a number of small srcqs.
         * Should not result in O(n) scans to find the insert position. */
        fails += unittest_msgq_insert_sort(
            "issue #2450 (v1.2.1 regression)", insert_baseline, NULL,
            (const struct ut_msg_range[]) {{200000, 200001},
                                           {200002, 200006},
                                           {200009, 200012},
                                           {200015, 200016},
                                           {200020, 200022},
                                           {200030, 200090},
                                           {200091, 200092},
                                           {200093, 200094},
                                           {200095, 200096},
                                           {200097, 200099},
                                           {0, 0}},
            (const struct ut_msg_range[]) {{1, 199999}, {0, 0}});

        return fails;
}
