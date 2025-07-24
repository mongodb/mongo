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

#ifndef _RDKAFKA_COORD_H_
#define _RDKAFKA_COORD_H_


typedef TAILQ_HEAD(rd_kafka_coord_cache_head_s,
                   rd_kafka_coord_cache_entry_s) rd_kafka_coord_cache_head_t;

/**
 * @brief Coordinator cache entry
 */
typedef struct rd_kafka_coord_cache_entry_s {
        TAILQ_ENTRY(rd_kafka_coord_cache_entry_s) cce_link;
        rd_kafka_coordtype_t cce_coordtype; /**< Coordinator type */
        char *cce_coordkey;                 /**< Coordinator type key,
                                             *   e.g the group id */
        rd_ts_t cce_ts_used;                /**< Last used timestamp */
        rd_kafka_broker_t *cce_rkb;         /**< The cached coordinator */

} rd_kafka_coord_cache_entry_t;

/**
 * @brief Coordinator cache
 */
typedef struct rd_kafka_coord_cache_s {
        rd_kafka_coord_cache_head_t cc_entries; /**< Cache entries */
        int cc_cnt;                             /**< Number of entries */
        rd_ts_t cc_expire_thres;                /**< Entries not used in
                                                 *   this long will be
                                                 *   expired */
} rd_kafka_coord_cache_t;


void rd_kafka_coord_cache_expire(rd_kafka_coord_cache_t *cc);
void rd_kafka_coord_cache_evict(rd_kafka_coord_cache_t *cc,
                                rd_kafka_broker_t *rkb);
void rd_kafka_coord_cache_destroy(rd_kafka_coord_cache_t *cc);
void rd_kafka_coord_cache_init(rd_kafka_coord_cache_t *cc, int expire_thres_ms);



/**
 * @name Coordinator requests
 */

/**
 * @brief Request to be sent to coordinator.
 *        Includes looking up, caching, and connecting to, the coordinator.
 */
typedef struct rd_kafka_coord_req_s {
        TAILQ_ENTRY(rd_kafka_coord_req_s) creq_link; /**< rk_coord_reqs */
        rd_kafka_coordtype_t creq_coordtype;         /**< Coordinator type */
        char *creq_coordkey;                         /**< Coordinator key */

        rd_kafka_op_t *creq_rko;        /**< Requester's rko that is
                                         *   provided to creq_send_req_cb
                                         *   (optional). */
        rd_kafka_timer_t creq_tmr;      /**< Delay timer. */
        rd_ts_t creq_ts_timeout;        /**< Absolute timeout.
                                         *   Will fail with an error
                                         *   code pertaining to the
                                         *   current state */
        rd_interval_t creq_query_intvl; /**< Coord query interval (1s) */

        rd_kafka_send_req_cb_t *creq_send_req_cb; /**< Sender callback */

        rd_kafka_replyq_t creq_replyq;    /**< Reply queue */
        rd_kafka_resp_cb_t *creq_resp_cb; /**< Reply queue response
                                           *   parsing callback for the
                                           *   request sent by
                                           *   send_req_cb */
        void *creq_reply_opaque;          /**< Opaque passed to
                                           *   creq_send_req_cb and
                                           *   creq_resp_cb. */

        int creq_refcnt;     /**< Internal reply queue for
                              *   FindCoordinator requests
                              *   which is forwarded to the
                              *   rk_ops queue, but allows
                              *   destroying the creq even
                              *   with outstanding
                              *   FindCoordinator requests. */
        rd_bool_t creq_done; /**< True if request was sent */

        rd_kafka_broker_t *creq_rkb; /**< creq is waiting for this broker to
                                      *   come up. */
} rd_kafka_coord_req_t;


void rd_kafka_coord_req(rd_kafka_t *rk,
                        rd_kafka_coordtype_t coordtype,
                        const char *coordkey,
                        rd_kafka_send_req_cb_t *send_req_cb,
                        rd_kafka_op_t *rko,
                        int delay_ms,
                        int timeout_ms,
                        rd_kafka_replyq_t replyq,
                        rd_kafka_resp_cb_t *resp_cb,
                        void *reply_opaque);

void rd_kafka_coord_rkb_monitor_cb(rd_kafka_broker_t *rkb);

void rd_kafka_coord_reqs_term(rd_kafka_t *rk);
void rd_kafka_coord_reqs_init(rd_kafka_t *rk);
#endif /* _RDKAFKA_COORD_H_ */
