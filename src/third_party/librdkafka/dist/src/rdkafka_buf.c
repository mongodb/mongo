/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2015, Magnus Edenhill
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
#include "rdkafka_buf.h"
#include "rdkafka_broker.h"
#include "rdkafka_interceptor.h"

void rd_kafka_buf_destroy_final(rd_kafka_buf_t *rkbuf) {

        switch (rkbuf->rkbuf_reqhdr.ApiKey) {
        case RD_KAFKAP_Metadata:
                if (rkbuf->rkbuf_u.Metadata.topics)
                        rd_list_destroy(rkbuf->rkbuf_u.Metadata.topics);
                if (rkbuf->rkbuf_u.Metadata.reason)
                        rd_free(rkbuf->rkbuf_u.Metadata.reason);
                if (rkbuf->rkbuf_u.Metadata.rko)
                        rd_kafka_op_reply(rkbuf->rkbuf_u.Metadata.rko,
                                          RD_KAFKA_RESP_ERR__DESTROY);
                if (rkbuf->rkbuf_u.Metadata.decr) {
                        /* Decrease metadata cache's full_.._sent state. */
                        mtx_lock(rkbuf->rkbuf_u.Metadata.decr_lock);
                        rd_kafka_assert(NULL,
                                        (*rkbuf->rkbuf_u.Metadata.decr) > 0);
                        (*rkbuf->rkbuf_u.Metadata.decr)--;
                        mtx_unlock(rkbuf->rkbuf_u.Metadata.decr_lock);
                }
                break;

        case RD_KAFKAP_Produce:
                rd_kafka_msgbatch_destroy(&rkbuf->rkbuf_batch);
                break;
        }

        if (rkbuf->rkbuf_response)
                rd_kafka_buf_destroy(rkbuf->rkbuf_response);

        if (rkbuf->rkbuf_make_opaque && rkbuf->rkbuf_free_make_opaque_cb)
                rkbuf->rkbuf_free_make_opaque_cb(rkbuf->rkbuf_make_opaque);

        rd_kafka_replyq_destroy(&rkbuf->rkbuf_replyq);
        rd_kafka_replyq_destroy(&rkbuf->rkbuf_orig_replyq);

        rd_buf_destroy(&rkbuf->rkbuf_buf);

        if (rkbuf->rkbuf_rktp_vers)
                rd_list_destroy(rkbuf->rkbuf_rktp_vers);

        if (rkbuf->rkbuf_rkb)
                rd_kafka_broker_destroy(rkbuf->rkbuf_rkb);

        rd_refcnt_destroy(&rkbuf->rkbuf_refcnt);

        rd_free(rkbuf);
}



/**
 * @brief Pushes \p buf of size \p len as a new segment on the buffer.
 *
 * \p buf will NOT be freed by the buffer.
 */
void rd_kafka_buf_push0(rd_kafka_buf_t *rkbuf,
                        const void *buf,
                        size_t len,
                        int allow_crc_calc,
                        void (*free_cb)(void *)) {
        rd_buf_push(&rkbuf->rkbuf_buf, buf, len, free_cb);

        if (allow_crc_calc && (rkbuf->rkbuf_flags & RD_KAFKA_OP_F_CRC))
                rkbuf->rkbuf_crc = rd_crc32_update(rkbuf->rkbuf_crc, buf, len);
}



/**
 * @brief Create a new buffer with \p segcmt initial segments and \p size bytes
 *        of initial backing memory.
 *        The underlying buffer will grow as needed.
 *
 * If \p rk is non-NULL (typical case):
 * Additional space for the Kafka protocol headers is inserted automatically.
 */
rd_kafka_buf_t *rd_kafka_buf_new0(int segcnt, size_t size, int flags) {
        rd_kafka_buf_t *rkbuf;

        rkbuf = rd_calloc(1, sizeof(*rkbuf));

        rkbuf->rkbuf_flags = flags;

        rd_buf_init(&rkbuf->rkbuf_buf, segcnt, size);
        rd_refcnt_init(&rkbuf->rkbuf_refcnt, 1);

        return rkbuf;
}


/**
 * @brief Create new request buffer with the request-header written (will
 *        need to be updated with Length, etc, later)
 */
rd_kafka_buf_t *rd_kafka_buf_new_request0(rd_kafka_broker_t *rkb,
                                          int16_t ApiKey,
                                          int segcnt,
                                          size_t size,
                                          rd_bool_t is_flexver) {
        rd_kafka_buf_t *rkbuf;

        /* Make room for common protocol request headers */
        size += RD_KAFKAP_REQHDR_SIZE +
                RD_KAFKAP_STR_SIZE(rkb->rkb_rk->rk_client_id) +
                /* Flexible version adds a tag list to the headers
                 * and to the end of the payload, both of which we send
                 * as empty (1 byte each). */
                (is_flexver ? 1 + 1 : 0);
        segcnt += 1; /* headers */

        rkbuf = rd_kafka_buf_new0(segcnt, size, 0);

        rkbuf->rkbuf_rkb = rkb;
        rd_kafka_broker_keep(rkb);

        rkbuf->rkbuf_rel_timeout = rkb->rkb_rk->rk_conf.socket_timeout_ms;
        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_DEFAULT_RETRIES;

        rkbuf->rkbuf_reqhdr.ApiKey = ApiKey;

        /* Write request header, will be updated later. */
        /* Length: updated later */
        rd_kafka_buf_write_i32(rkbuf, 0);
        /* ApiKey */
        rd_kafka_buf_write_i16(rkbuf, rkbuf->rkbuf_reqhdr.ApiKey);
        /* ApiVersion: updated later */
        rd_kafka_buf_write_i16(rkbuf, 0);
        /* CorrId: updated later */
        rd_kafka_buf_write_i32(rkbuf, 0);

        /* ClientId */
        rd_kafka_buf_write_kstr(rkbuf, rkb->rkb_rk->rk_client_id);

        if (is_flexver) {
                /* Must set flexver after writing the client id since
                 * it is still a standard non-compact string. */
                rkbuf->rkbuf_flags |= RD_KAFKA_OP_F_FLEXVER;

                /* Empty request header tags */
                rd_kafka_buf_write_i8(rkbuf, 0);
        }

        return rkbuf;
}



/**
 * @brief Create new read-only rkbuf shadowing a memory region.
 *
 * @remark \p free_cb (possibly NULL) will be used to free \p ptr when
 *         buffer refcount reaches 0.
 * @remark the buffer may only be read from, not written to.
 *
 * @warning If the caller has log_decode_errors > 0 then it must set up
 *          \c rkbuf->rkbuf_rkb to a refcnt-increased broker object.
 */
rd_kafka_buf_t *
rd_kafka_buf_new_shadow(const void *ptr, size_t size, void (*free_cb)(void *)) {
        rd_kafka_buf_t *rkbuf;

        rkbuf = rd_calloc(1, sizeof(*rkbuf));

        rkbuf->rkbuf_reqhdr.ApiKey = RD_KAFKAP_None;

        rd_buf_init(&rkbuf->rkbuf_buf, 1, 0);
        rd_buf_push(&rkbuf->rkbuf_buf, ptr, size, free_cb);

        rkbuf->rkbuf_totlen = size;

        /* Initialize reader slice */
        rd_slice_init_full(&rkbuf->rkbuf_reader, &rkbuf->rkbuf_buf);

        rd_refcnt_init(&rkbuf->rkbuf_refcnt, 1);

        return rkbuf;
}



void rd_kafka_bufq_enq(rd_kafka_bufq_t *rkbufq, rd_kafka_buf_t *rkbuf) {
        TAILQ_INSERT_TAIL(&rkbufq->rkbq_bufs, rkbuf, rkbuf_link);
        rd_atomic32_add(&rkbufq->rkbq_cnt, 1);
        if (rkbuf->rkbuf_reqhdr.ApiKey == RD_KAFKAP_Produce)
                rd_atomic32_add(&rkbufq->rkbq_msg_cnt,
                                rd_kafka_msgq_len(&rkbuf->rkbuf_batch.msgq));
}

void rd_kafka_bufq_deq(rd_kafka_bufq_t *rkbufq, rd_kafka_buf_t *rkbuf) {
        TAILQ_REMOVE(&rkbufq->rkbq_bufs, rkbuf, rkbuf_link);
        rd_kafka_assert(NULL, rd_atomic32_get(&rkbufq->rkbq_cnt) > 0);
        rd_atomic32_sub(&rkbufq->rkbq_cnt, 1);
        if (rkbuf->rkbuf_reqhdr.ApiKey == RD_KAFKAP_Produce)
                rd_atomic32_sub(&rkbufq->rkbq_msg_cnt,
                                rd_kafka_msgq_len(&rkbuf->rkbuf_batch.msgq));
}

void rd_kafka_bufq_init(rd_kafka_bufq_t *rkbufq) {
        TAILQ_INIT(&rkbufq->rkbq_bufs);
        rd_atomic32_init(&rkbufq->rkbq_cnt, 0);
        rd_atomic32_init(&rkbufq->rkbq_msg_cnt, 0);
}

/**
 * Concat all buffers from 'src' to tail of 'dst'
 */
void rd_kafka_bufq_concat(rd_kafka_bufq_t *dst, rd_kafka_bufq_t *src) {
        TAILQ_CONCAT(&dst->rkbq_bufs, &src->rkbq_bufs, rkbuf_link);
        (void)rd_atomic32_add(&dst->rkbq_cnt, rd_atomic32_get(&src->rkbq_cnt));
        (void)rd_atomic32_add(&dst->rkbq_msg_cnt,
                              rd_atomic32_get(&src->rkbq_msg_cnt));
        rd_kafka_bufq_init(src);
}

/**
 * Purge the wait-response queue.
 * NOTE: 'rkbufq' must be a temporary queue and not one of rkb_waitresps
 *       or rkb_outbufs since buffers may be re-enqueued on those queues.
 *       'rkbufq' needs to be bufq_init():ed before reuse after this call.
 */
void rd_kafka_bufq_purge(rd_kafka_broker_t *rkb,
                         rd_kafka_bufq_t *rkbufq,
                         rd_kafka_resp_err_t err) {
        rd_kafka_buf_t *rkbuf, *tmp;

        rd_kafka_assert(rkb->rkb_rk, thrd_is_current(rkb->rkb_thread));

        rd_rkb_dbg(rkb, QUEUE, "BUFQ", "Purging bufq with %i buffers",
                   rd_atomic32_get(&rkbufq->rkbq_cnt));

        TAILQ_FOREACH_SAFE(rkbuf, &rkbufq->rkbq_bufs, rkbuf_link, tmp) {
                rd_kafka_buf_callback(rkb->rkb_rk, rkb, err, NULL, rkbuf);
        }
}


/**
 * @brief Update bufq for connection reset:
 *
 * - Purge connection-setup API requests from the queue.
 * - Reset any partially sent buffer's offset. (issue #756)
 *
 * Request types purged:
 *   ApiVersion
 *   SaslHandshake
 */
void rd_kafka_bufq_connection_reset(rd_kafka_broker_t *rkb,
                                    rd_kafka_bufq_t *rkbufq) {
        rd_kafka_buf_t *rkbuf, *tmp;
        rd_ts_t now = rd_clock();

        rd_kafka_assert(rkb->rkb_rk, thrd_is_current(rkb->rkb_thread));

        rd_rkb_dbg(rkb, QUEUE, "BUFQ",
                   "Updating %d buffers on connection reset",
                   rd_atomic32_get(&rkbufq->rkbq_cnt));

        TAILQ_FOREACH_SAFE(rkbuf, &rkbufq->rkbq_bufs, rkbuf_link, tmp) {
                switch (rkbuf->rkbuf_reqhdr.ApiKey) {
                case RD_KAFKAP_ApiVersion:
                case RD_KAFKAP_SaslHandshake:
                        rd_kafka_bufq_deq(rkbufq, rkbuf);
                        rd_kafka_buf_callback(rkb->rkb_rk, rkb,
                                              RD_KAFKA_RESP_ERR__DESTROY, NULL,
                                              rkbuf);
                        break;
                default:
                        /* Reset buffer send position and corrid */
                        rd_slice_seek(&rkbuf->rkbuf_reader, 0);
                        rkbuf->rkbuf_corrid = 0;
                        /* Reset timeout */
                        rd_kafka_buf_calc_timeout(rkb->rkb_rk, rkbuf, now);
                        break;
                }
        }
}


void rd_kafka_bufq_dump(rd_kafka_broker_t *rkb,
                        const char *fac,
                        rd_kafka_bufq_t *rkbq) {
        rd_kafka_buf_t *rkbuf;
        int cnt = rd_kafka_bufq_cnt(rkbq);
        rd_ts_t now;

        if (!cnt)
                return;

        now = rd_clock();

        rd_rkb_dbg(rkb, BROKER, fac, "bufq with %d buffer(s):", cnt);

        TAILQ_FOREACH(rkbuf, &rkbq->rkbq_bufs, rkbuf_link) {
                rd_rkb_dbg(rkb, BROKER, fac,
                           " Buffer %s (%" PRIusz " bytes, corrid %" PRId32
                           ", "
                           "connid %d, prio %d, retry %d in %lldms, "
                           "timeout in %lldms)",
                           rd_kafka_ApiKey2str(rkbuf->rkbuf_reqhdr.ApiKey),
                           rkbuf->rkbuf_totlen, rkbuf->rkbuf_corrid,
                           rkbuf->rkbuf_connid, rkbuf->rkbuf_prio,
                           rkbuf->rkbuf_retries,
                           rkbuf->rkbuf_ts_retry
                               ? (rkbuf->rkbuf_ts_retry - now) / 1000LL
                               : 0,
                           rkbuf->rkbuf_ts_timeout
                               ? (rkbuf->rkbuf_ts_timeout - now) / 1000LL
                               : 0);
        }
}



/**
 * @brief Calculate the effective timeout for a request attempt
 */
void rd_kafka_buf_calc_timeout(const rd_kafka_t *rk,
                               rd_kafka_buf_t *rkbuf,
                               rd_ts_t now) {
        if (likely(rkbuf->rkbuf_rel_timeout)) {
                /* Default:
                 * Relative timeout, set request timeout to
                 * to now + rel timeout. */
                rkbuf->rkbuf_ts_timeout = now + rkbuf->rkbuf_rel_timeout * 1000;
        } else if (!rkbuf->rkbuf_force_timeout) {
                /* Use absolute timeout, limited by socket.timeout.ms */
                rd_ts_t sock_timeout =
                    now + rk->rk_conf.socket_timeout_ms * 1000;

                rkbuf->rkbuf_ts_timeout =
                    RD_MIN(sock_timeout, rkbuf->rkbuf_abs_timeout);
        } else {
                /* Use absolue timeout without limit. */
                rkbuf->rkbuf_ts_timeout = rkbuf->rkbuf_abs_timeout;
        }
}

/**
 * Retry failed request, if permitted.
 * @remark \p rkb may be NULL
 * @remark the retry count is only increased for actually transmitted buffers,
 *         if there is a failure while the buffers lingers in the output queue
 *         (rkb_outbufs) then the retry counter is not increased.
 * Returns 1 if the request was scheduled for retry, else 0.
 */
int rd_kafka_buf_retry(rd_kafka_broker_t *rkb, rd_kafka_buf_t *rkbuf) {
        int incr_retry = rd_kafka_buf_was_sent(rkbuf) ? 1 : 0;

        /* Don't allow retries of dummy/empty buffers */
        rd_assert(rd_buf_len(&rkbuf->rkbuf_buf) > 0);

        if (unlikely(!rkb || rkb->rkb_source == RD_KAFKA_INTERNAL ||
                     rd_kafka_terminating(rkb->rkb_rk) ||
                     rkbuf->rkbuf_retries + incr_retry >
                         rkbuf->rkbuf_max_retries))
                return 0;

        /* Absolute timeout, check for expiry. */
        if (rkbuf->rkbuf_abs_timeout && rkbuf->rkbuf_abs_timeout < rd_clock())
                return 0; /* Expired */

        /* Try again */
        rkbuf->rkbuf_ts_sent    = 0;
        rkbuf->rkbuf_ts_timeout = 0; /* Will be updated in calc_timeout() */
        rkbuf->rkbuf_retries += incr_retry;
        rd_kafka_buf_keep(rkbuf);
        rd_kafka_broker_buf_retry(rkb, rkbuf);
        return 1;
}


/**
 * @brief Handle RD_KAFKA_OP_RECV_BUF.
 */
void rd_kafka_buf_handle_op(rd_kafka_op_t *rko, rd_kafka_resp_err_t err) {
        rd_kafka_buf_t *request, *response;
        rd_kafka_t *rk;

        request               = rko->rko_u.xbuf.rkbuf;
        rko->rko_u.xbuf.rkbuf = NULL;

        /* NULL on op_destroy() */
        if (request->rkbuf_replyq.q) {
                int32_t version = request->rkbuf_replyq.version;
                /* Current queue usage is done, but retain original replyq for
                 * future retries, stealing
                 * the current reference. */
                request->rkbuf_orig_replyq = request->rkbuf_replyq;
                rd_kafka_replyq_clear(&request->rkbuf_replyq);
                /* Callback might need to version check so we retain the
                 * version across the clear() call which clears it. */
                request->rkbuf_replyq.version = version;
        }

        if (!request->rkbuf_cb) {
                rd_kafka_buf_destroy(request);
                return;
        }

        /* Let buf_callback() do destroy()s */
        response                = request->rkbuf_response; /* May be NULL */
        request->rkbuf_response = NULL;

        if (!(rk = rko->rko_rk)) {
                rd_assert(request->rkbuf_rkb != NULL);
                rk = request->rkbuf_rkb->rkb_rk;
        }

        rd_kafka_buf_callback(rk, request->rkbuf_rkb, err, response, request);
}



/**
 * Call request.rkbuf_cb(), but:
 *  - if the rkbuf has a rkbuf_replyq the buffer is enqueued on that queue
 *    with op type RD_KAFKA_OP_RECV_BUF.
 *  - else call rkbuf_cb().
 *
 * \p response may be NULL.
 *
 * Will decrease refcount for both response and request, eventually.
 *
 * The decision to retry, and the call to buf_retry(), is delegated
 * to the buffer's response callback.
 */
void rd_kafka_buf_callback(rd_kafka_t *rk,
                           rd_kafka_broker_t *rkb,
                           rd_kafka_resp_err_t err,
                           rd_kafka_buf_t *response,
                           rd_kafka_buf_t *request) {

        rd_kafka_interceptors_on_response_received(
            rk, -1, rkb ? rd_kafka_broker_name(rkb) : "",
            rkb ? rd_kafka_broker_id(rkb) : -1, request->rkbuf_reqhdr.ApiKey,
            request->rkbuf_reqhdr.ApiVersion, request->rkbuf_reshdr.CorrId,
            response ? response->rkbuf_totlen : 0,
            response ? response->rkbuf_ts_sent : -1, err);

        if (err != RD_KAFKA_RESP_ERR__DESTROY && request->rkbuf_replyq.q) {
                rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_RECV_BUF);

                rd_kafka_assert(NULL, !request->rkbuf_response);
                request->rkbuf_response = response;

                /* Increment refcnt since rko_rkbuf will be decref:ed
                 * if replyq_enq() fails and we dont want the rkbuf gone in that
                 * case. */
                rd_kafka_buf_keep(request);
                rko->rko_u.xbuf.rkbuf = request;

                rko->rko_err = err;

                /* Copy original replyq for future retries, with its own
                 * queue reference. */
                rd_kafka_replyq_copy(&request->rkbuf_orig_replyq,
                                     &request->rkbuf_replyq);

                rd_kafka_replyq_enq(&request->rkbuf_replyq, rko, 0);

                rd_kafka_buf_destroy(request); /* from keep above */
                return;
        }

        if (request->rkbuf_cb)
                request->rkbuf_cb(rk, rkb, err, response, request,
                                  request->rkbuf_opaque);

        rd_kafka_buf_destroy(request);
        if (response)
                rd_kafka_buf_destroy(response);
}



/**
 * @brief Set the maker callback, which will be called just prior to sending
 *        to construct the buffer contents.
 *
 * Use this when the usable ApiVersion must be known but the broker may
 * currently be down.
 *
 * See rd_kafka_make_req_cb_t documentation for more info.
 */
void rd_kafka_buf_set_maker(rd_kafka_buf_t *rkbuf,
                            rd_kafka_make_req_cb_t *make_cb,
                            void *make_opaque,
                            void (*free_make_opaque_cb)(void *make_opaque)) {
        rd_assert(!rkbuf->rkbuf_make_req_cb &&
                  !(rkbuf->rkbuf_flags & RD_KAFKA_OP_F_NEED_MAKE));

        rkbuf->rkbuf_make_req_cb         = make_cb;
        rkbuf->rkbuf_make_opaque         = make_opaque;
        rkbuf->rkbuf_free_make_opaque_cb = free_make_opaque_cb;

        rkbuf->rkbuf_flags |= RD_KAFKA_OP_F_NEED_MAKE;
}
