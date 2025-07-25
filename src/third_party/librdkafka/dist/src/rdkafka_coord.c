/*
 * librdkafka - Apache Kafka C library
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


#include "rdkafka_int.h"
#include "rdkafka_request.h"
#include "rdkafka_coord.h"


/**
 * @name Coordinator cache
 * @{
 *
 */
void rd_kafka_coord_cache_entry_destroy(rd_kafka_coord_cache_t *cc,
                                        rd_kafka_coord_cache_entry_t *cce) {
        rd_assert(cc->cc_cnt > 0);
        rd_free(cce->cce_coordkey);
        rd_kafka_broker_destroy(cce->cce_rkb);
        TAILQ_REMOVE(&cc->cc_entries, cce, cce_link);
        cc->cc_cnt--;
        rd_free(cce);
}


/**
 * @brief Delete any expired cache entries
 *
 * @locality rdkafka main thread
 */
void rd_kafka_coord_cache_expire(rd_kafka_coord_cache_t *cc) {
        rd_kafka_coord_cache_entry_t *cce, *next;
        rd_ts_t expire = rd_clock() - cc->cc_expire_thres;

        next = TAILQ_LAST(&cc->cc_entries, rd_kafka_coord_cache_head_s);
        while (next) {
                cce = next;

                if (cce->cce_ts_used > expire)
                        break;

                next = TAILQ_PREV(cce, rd_kafka_coord_cache_head_s, cce_link);
                rd_kafka_coord_cache_entry_destroy(cc, cce);
        }
}


static rd_kafka_coord_cache_entry_t *
rd_kafka_coord_cache_find(rd_kafka_coord_cache_t *cc,
                          rd_kafka_coordtype_t coordtype,
                          const char *coordkey) {
        rd_kafka_coord_cache_entry_t *cce;

        TAILQ_FOREACH(cce, &cc->cc_entries, cce_link) {
                if (cce->cce_coordtype == coordtype &&
                    !strcmp(cce->cce_coordkey, coordkey)) {
                        /* Match */
                        cce->cce_ts_used = rd_clock();
                        if (TAILQ_FIRST(&cc->cc_entries) != cce) {
                                /* Move to head of list */
                                TAILQ_REMOVE(&cc->cc_entries, cce, cce_link);
                                TAILQ_INSERT_HEAD(&cc->cc_entries, cce,
                                                  cce_link);
                        }
                        return cce;
                }
        }

        return NULL;
}


rd_kafka_broker_t *rd_kafka_coord_cache_get(rd_kafka_coord_cache_t *cc,
                                            rd_kafka_coordtype_t coordtype,
                                            const char *coordkey) {
        rd_kafka_coord_cache_entry_t *cce;

        cce = rd_kafka_coord_cache_find(cc, coordtype, coordkey);
        if (!cce)
                return NULL;

        rd_kafka_broker_keep(cce->cce_rkb);
        return cce->cce_rkb;
}



static void rd_kafka_coord_cache_add(rd_kafka_coord_cache_t *cc,
                                     rd_kafka_coordtype_t coordtype,
                                     const char *coordkey,
                                     rd_kafka_broker_t *rkb) {
        rd_kafka_coord_cache_entry_t *cce;

        if (!(cce = rd_kafka_coord_cache_find(cc, coordtype, coordkey))) {
                if (cc->cc_cnt > 10) {
                        /* Not enough room in cache, remove least used entry */
                        rd_kafka_coord_cache_entry_t *rem = TAILQ_LAST(
                            &cc->cc_entries, rd_kafka_coord_cache_head_s);
                        rd_kafka_coord_cache_entry_destroy(cc, rem);
                }

                cce                = rd_calloc(1, sizeof(*cce));
                cce->cce_coordtype = coordtype;
                cce->cce_coordkey  = rd_strdup(coordkey);
                cce->cce_ts_used   = rd_clock();

                TAILQ_INSERT_HEAD(&cc->cc_entries, cce, cce_link);
                cc->cc_cnt++;
        }

        if (cce->cce_rkb != rkb) {
                if (cce->cce_rkb)
                        rd_kafka_broker_destroy(cce->cce_rkb);
                cce->cce_rkb = rkb;
                rd_kafka_broker_keep(rkb);
        }
}


/**
 * @brief Evict any cache entries for broker \p rkb.
 *
 * Use this when a request returns ERR_NOT_COORDINATOR_FOR...
 *
 * @locality rdkafka main thread
 * @locks none
 */
void rd_kafka_coord_cache_evict(rd_kafka_coord_cache_t *cc,
                                rd_kafka_broker_t *rkb) {
        rd_kafka_coord_cache_entry_t *cce, *tmp;

        TAILQ_FOREACH_SAFE(cce, &cc->cc_entries, cce_link, tmp) {
                if (cce->cce_rkb == rkb)
                        rd_kafka_coord_cache_entry_destroy(cc, cce);
        }
}

/**
 * @brief Destroy all coord cache entries.
 */
void rd_kafka_coord_cache_destroy(rd_kafka_coord_cache_t *cc) {
        rd_kafka_coord_cache_entry_t *cce;

        while ((cce = TAILQ_FIRST(&cc->cc_entries)))
                rd_kafka_coord_cache_entry_destroy(cc, cce);
}


/**
 * @brief Initialize the coord cache.
 *
 * Locking of the coord-cache is up to the owner.
 */
void rd_kafka_coord_cache_init(rd_kafka_coord_cache_t *cc,
                               int expire_thres_ms) {
        TAILQ_INIT(&cc->cc_entries);
        cc->cc_cnt          = 0;
        cc->cc_expire_thres = expire_thres_ms * 1000;
}

/**@}*/


/**
 * @name Asynchronous coordinator requests
 * @{
 *
 */



static void rd_kafka_coord_req_fsm(rd_kafka_t *rk, rd_kafka_coord_req_t *creq);

/**
 * @brief Timer callback for delayed coord requests.
 */
static void rd_kafka_coord_req_tmr_cb(rd_kafka_timers_t *rkts, void *arg) {
        rd_kafka_coord_req_t *creq = arg;

        rd_kafka_coord_req_fsm(rkts->rkts_rk, creq);
}


/**
 * @brief Look up coordinator for \p coordtype and \p coordkey
 *        (either from cache or by FindCoordinator), make sure there is
 *        a connection to the coordinator, and then call \p send_req_cb,
 *        passing the coordinator broker instance and \p rko
 *        to send the request.
 *        These steps may be performed by this function, or asynchronously
 *        at a later time.
 *
 * @param delay_ms If non-zero, delay scheduling of the coord request
 *                 for this long. The passed \p timeout_ms is automatically
 *                 adjusted to + \p delay_ms.
 *
 * Response, or error, is sent on \p replyq with callback \p rkbuf_cb.
 *
 * @locality rdkafka main thread
 * @locks none
 */
void rd_kafka_coord_req(rd_kafka_t *rk,
                        rd_kafka_coordtype_t coordtype,
                        const char *coordkey,
                        rd_kafka_send_req_cb_t *send_req_cb,
                        rd_kafka_op_t *rko,
                        int delay_ms,
                        int timeout_ms,
                        rd_kafka_replyq_t replyq,
                        rd_kafka_resp_cb_t *resp_cb,
                        void *reply_opaque) {
        rd_kafka_coord_req_t *creq;

        creq                    = rd_calloc(1, sizeof(*creq));
        creq->creq_coordtype    = coordtype;
        creq->creq_coordkey     = rd_strdup(coordkey);
        creq->creq_ts_timeout   = rd_timeout_init(delay_ms + timeout_ms);
        creq->creq_send_req_cb  = send_req_cb;
        creq->creq_rko          = rko;
        creq->creq_replyq       = replyq;
        creq->creq_resp_cb      = resp_cb;
        creq->creq_reply_opaque = reply_opaque;
        creq->creq_refcnt       = 1;
        creq->creq_done         = rd_false;
        rd_interval_init(&creq->creq_query_intvl);

        TAILQ_INSERT_TAIL(&rk->rk_coord_reqs, creq, creq_link);

        if (delay_ms)
                rd_kafka_timer_start_oneshot(&rk->rk_timers, &creq->creq_tmr,
                                             rd_true, (rd_ts_t)delay_ms * 1000,
                                             rd_kafka_coord_req_tmr_cb, creq);
        else
                rd_kafka_coord_req_fsm(rk, creq);
}


/**
 * @brief Decrease refcount of creq and free it if no more references.
 *
 * @param done Mark creq as done, having performed its duties. There may still
 *             be lingering references.
 *
 * @returns true if creq was destroyed, else false.
 */
static rd_bool_t rd_kafka_coord_req_destroy(rd_kafka_t *rk,
                                            rd_kafka_coord_req_t *creq,
                                            rd_bool_t done) {

        rd_assert(creq->creq_refcnt > 0);

        if (done) {
                /* Request has been performed, remove from rk_coord_reqs
                 * list so creq won't be triggered again by state broadcasts,
                 * etc. */
                rd_dassert(!creq->creq_done);
                TAILQ_REMOVE(&rk->rk_coord_reqs, creq, creq_link);
                creq->creq_done = rd_true;

                rd_kafka_timer_stop(&rk->rk_timers, &creq->creq_tmr,
                                    RD_DO_LOCK);
        }

        if (--creq->creq_refcnt > 0)
                return rd_false;

        rd_dassert(creq->creq_done);

        /* Clear out coordinator we were waiting for. */
        if (creq->creq_rkb) {
                rd_kafka_broker_persistent_connection_del(
                    creq->creq_rkb, &creq->creq_rkb->rkb_persistconn.coord);
                rd_kafka_broker_destroy(creq->creq_rkb);
                creq->creq_rkb = NULL;
        }

        rd_kafka_replyq_destroy(&creq->creq_replyq);
        rd_free(creq->creq_coordkey);
        rd_free(creq);

        return rd_true;
}

static void rd_kafka_coord_req_keep(rd_kafka_coord_req_t *creq) {
        creq->creq_refcnt++;
}

static void rd_kafka_coord_req_fail(rd_kafka_t *rk,
                                    rd_kafka_coord_req_t *creq,
                                    rd_kafka_resp_err_t err) {
        rd_kafka_op_t *reply;
        rd_kafka_buf_t *rkbuf;

        reply         = rd_kafka_op_new(RD_KAFKA_OP_RECV_BUF);
        reply->rko_rk = rk; /* Set rk since the rkbuf will not have a rkb
                             * to reach it. */
        reply->rko_err = err;

        /* Need a dummy rkbuf to pass state to the buf resp_cb */
        rkbuf                   = rd_kafka_buf_new(0, 0);
        rkbuf->rkbuf_cb         = creq->creq_resp_cb;
        rkbuf->rkbuf_opaque     = creq->creq_reply_opaque;
        reply->rko_u.xbuf.rkbuf = rkbuf;

        rd_kafka_replyq_enq(&creq->creq_replyq, reply, 0);

        rd_kafka_coord_req_destroy(rk, creq, rd_true /*done*/);
}


static void rd_kafka_coord_req_handle_FindCoordinator(rd_kafka_t *rk,
                                                      rd_kafka_broker_t *rkb,
                                                      rd_kafka_resp_err_t err,
                                                      rd_kafka_buf_t *rkbuf,
                                                      rd_kafka_buf_t *request,
                                                      void *opaque) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_coord_req_t *creq  = opaque;
        int16_t ErrorCode;
        rd_kafkap_str_t Host;
        int32_t NodeId, Port;
        char errstr[256] = "";
        int actions;
        rd_kafka_broker_t *coord;
        rd_kafka_metadata_broker_t mdb = RD_ZERO_INIT;

        /* If creq has finished (possibly because of an earlier FindCoordinator
         * response or a broker state broadcast we simply ignore the
         * response. */
        if (creq->creq_done)
                err = RD_KAFKA_RESP_ERR__DESTROY;

        if (err)
                goto err;

        if (request->rkbuf_reqhdr.ApiVersion >= 1)
                rd_kafka_buf_read_throttle_time(rkbuf);

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

        if (request->rkbuf_reqhdr.ApiVersion >= 1) {
                rd_kafkap_str_t ErrorMsg;
                rd_kafka_buf_read_str(rkbuf, &ErrorMsg);
                if (ErrorCode)
                        rd_snprintf(errstr, sizeof(errstr), "%.*s",
                                    RD_KAFKAP_STR_PR(&ErrorMsg));
        }

        if ((err = ErrorCode))
                goto err;

        rd_kafka_buf_read_i32(rkbuf, &NodeId);
        rd_kafka_buf_read_str(rkbuf, &Host);
        rd_kafka_buf_read_i32(rkbuf, &Port);

        mdb.id = NodeId;
        RD_KAFKAP_STR_DUPA(&mdb.host, &Host);
        mdb.port = Port;

        /* Find, update or add broker */
        rd_kafka_broker_update(rk, rkb->rkb_proto, &mdb, &coord);

        if (!coord) {
                err = RD_KAFKA_RESP_ERR__FAIL;
                rd_snprintf(errstr, sizeof(errstr),
                            "Failed to add broker: "
                            "instance is probably terminating");
                goto err;
        }


        rd_kafka_coord_cache_add(&rk->rk_coord_cache, creq->creq_coordtype,
                                 creq->creq_coordkey, coord);
        rd_kafka_broker_destroy(coord); /* refcnt from broker_update() */

        rd_kafka_coord_req_fsm(rk, creq);

        /* Drop refcount from req_fsm() */
        rd_kafka_coord_req_destroy(rk, creq, rd_false /*!done*/);

        return;

err_parse:
        err = rkbuf->rkbuf_err;
err:
        actions = rd_kafka_err_action(
            rkb, err, request,

            RD_KAFKA_ERR_ACTION_SPECIAL, RD_KAFKA_RESP_ERR__DESTROY,

            RD_KAFKA_ERR_ACTION_PERMANENT,
            RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED,

            RD_KAFKA_ERR_ACTION_PERMANENT,
            RD_KAFKA_RESP_ERR_CLUSTER_AUTHORIZATION_FAILED,

            RD_KAFKA_ERR_ACTION_REFRESH, RD_KAFKA_RESP_ERR__TRANSPORT,

            RD_KAFKA_ERR_ACTION_RETRY,
            RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE,

            RD_KAFKA_ERR_ACTION_END);

        if (actions & RD_KAFKA_ERR_ACTION_PERMANENT) {
                rd_kafka_coord_req_fail(rk, creq, err);
                return;

        } else if (actions & RD_KAFKA_ERR_ACTION_RETRY) {
                rd_kafka_buf_retry(rkb, request);
                return; /* Keep refcnt from req_fsm() and retry */
        }

        /* Rely on state broadcast to trigger retry */

        /* Drop refcount from req_fsm() */
        rd_kafka_coord_req_destroy(rk, creq, rd_false /*!done*/);
}



/**
 * @brief State machine for async coordinator requests.
 *
 * @remark May destroy the \p creq.
 *
 * @locality any
 * @locks none
 */
static void rd_kafka_coord_req_fsm(rd_kafka_t *rk, rd_kafka_coord_req_t *creq) {
        rd_kafka_broker_t *rkb;
        rd_kafka_resp_err_t err;

        if (creq->creq_done)
                /* crqeq has already performed its actions, this is a
                 * lingering reference, e.g., a late FindCoordinator response.
                 * Just ignore. */
                return;

        if (unlikely(rd_kafka_terminating(rk))) {
                rd_kafka_coord_req_fail(rk, creq, RD_KAFKA_RESP_ERR__DESTROY);
                return;
        }

        /* Do nothing if creq is delayed and the delay time hasn't expired yet.
         * We will be called again by the timer once it expires.*/
        if (rd_kafka_timer_next(&rk->rk_timers, &creq->creq_tmr, RD_DO_LOCK) >
            0)
                return;

        /* Check cache first */
        rkb = rd_kafka_coord_cache_get(
            &rk->rk_coord_cache, creq->creq_coordtype, creq->creq_coordkey);

        if (rkb) {
                if (rd_kafka_broker_is_up(rkb)) {
                        /* Cached coordinator is up, send request */
                        rd_kafka_replyq_t replyq;

                        /* Clear out previous coordinator we waited for. */
                        if (creq->creq_rkb) {
                                rd_kafka_broker_persistent_connection_del(
                                    creq->creq_rkb,
                                    &creq->creq_rkb->rkb_persistconn.coord);
                                rd_kafka_broker_destroy(creq->creq_rkb);
                                creq->creq_rkb = NULL;
                        }

                        rd_kafka_replyq_copy(&replyq, &creq->creq_replyq);
                        err = creq->creq_send_req_cb(rkb, creq->creq_rko,
                                                     replyq, creq->creq_resp_cb,
                                                     creq->creq_reply_opaque);

                        if (err) {
                                /* Permanent error, e.g., request not
                                 *  supported by broker. */
                                rd_kafka_replyq_destroy(&replyq);
                                rd_kafka_coord_req_fail(rk, creq, err);
                        } else {
                                rd_kafka_coord_req_destroy(rk, creq,
                                                           rd_true /*done*/);
                        }

                } else if (creq->creq_rkb == rkb) {
                        /* No change in coordinator, but it is still not up.
                         * Query for coordinator if at least a second has
                         * passed since this coord_req was created or the
                         * last time we queried. */
                        if (rd_interval(&creq->creq_query_intvl,
                                        1000 * 1000 /* 1s */, 0) > 0) {
                                rd_rkb_dbg(rkb, BROKER, "COORD",
                                           "Coordinator connection is "
                                           "still down: "
                                           "querying for new coordinator");
                                rd_kafka_broker_destroy(rkb);
                                goto query_coord;
                        }

                } else {
                        /* No connection yet.
                         * Let broker thread know we need a connection.
                         * We'll be re-triggered on broker state broadcast. */

                        if (creq->creq_rkb) {
                                /* Clear previous */
                                rd_kafka_broker_persistent_connection_del(
                                    creq->creq_rkb,
                                    &creq->creq_rkb->rkb_persistconn.coord);
                                rd_kafka_broker_destroy(creq->creq_rkb);
                        }

                        rd_kafka_broker_keep(rkb);
                        creq->creq_rkb = rkb;
                        rd_kafka_broker_persistent_connection_add(
                            rkb, &rkb->rkb_persistconn.coord);
                }

                rd_kafka_broker_destroy(rkb);
                return;

        } else if (creq->creq_rkb) {
                /* No coordinator information, clear out the previous
                 * coordinator we waited for. */
                rd_kafka_broker_persistent_connection_del(
                    creq->creq_rkb, &creq->creq_rkb->rkb_persistconn.coord);
                rd_kafka_broker_destroy(creq->creq_rkb);
                creq->creq_rkb = NULL;
        }

query_coord:
        /* Get any usable broker to look up the coordinator */
        rkb = rd_kafka_broker_any_usable(rk, RD_POLL_NOWAIT, RD_DO_LOCK,
                                         RD_KAFKA_FEATURE_BROKER_GROUP_COORD,
                                         "broker to look up coordinator");

        if (!rkb) {
                /* No available brokers yet, we'll be re-triggered on
                 * broker state broadcast. */
                return;
        }


        /* Send FindCoordinator request, the handler will continue
         * the state machine. */
        rd_kafka_coord_req_keep(creq);
        err = rd_kafka_FindCoordinatorRequest(
            rkb, creq->creq_coordtype, creq->creq_coordkey,
            RD_KAFKA_REPLYQ(rk->rk_ops, 0),
            rd_kafka_coord_req_handle_FindCoordinator, creq);

        rd_kafka_broker_destroy(rkb);

        if (err) {
                rd_kafka_coord_req_fail(rk, creq, err);
                /* from keep() above */
                rd_kafka_coord_req_destroy(rk, creq, rd_false /*!done*/);
        }
}



/**
 * @brief Callback called from rdkafka main thread on each
 *        broker state change from or to UP.
 *
 * @locality rdkafka main thread
 * @locks none
 */
void rd_kafka_coord_rkb_monitor_cb(rd_kafka_broker_t *rkb) {
        rd_kafka_t *rk = rkb->rkb_rk;
        rd_kafka_coord_req_t *creq, *tmp;

        /* Run through all coord_req fsms */
        TAILQ_FOREACH_SAFE(creq, &rk->rk_coord_reqs, creq_link, tmp) {
                rd_kafka_coord_req_fsm(rk, creq);
        }
}



/**
 * @brief Instance is terminating: destroy all coord reqs
 */
void rd_kafka_coord_reqs_term(rd_kafka_t *rk) {
        rd_kafka_coord_req_t *creq;

        while ((creq = TAILQ_FIRST(&rk->rk_coord_reqs)))
                rd_kafka_coord_req_fail(rk, creq, RD_KAFKA_RESP_ERR__DESTROY);
}


/**
 * @brief Initialize coord reqs list.
 */
void rd_kafka_coord_reqs_init(rd_kafka_t *rk) {
        TAILQ_INIT(&rk->rk_coord_reqs);
}

/**@}*/
