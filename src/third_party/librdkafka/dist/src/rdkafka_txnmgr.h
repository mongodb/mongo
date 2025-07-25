/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2019 Magnus Edenhill
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

#ifndef _RDKAFKA_TXNMGR_H_
#define _RDKAFKA_TXNMGR_H_

/**
 * @returns true if transaction state allows enqueuing new messages
 *          (i.e., produce()), else false.
 *
 * @locality application thread
 * @locks none
 */
static RD_INLINE RD_UNUSED rd_bool_t rd_kafka_txn_may_enq_msg(rd_kafka_t *rk) {
        return !rd_kafka_is_transactional(rk) ||
               rd_atomic32_get(&rk->rk_eos.txn_may_enq);
}


/**
 * @returns true if transaction state allows sending messages to broker,
 *          else false.
 *
 * @locality broker thread
 * @locks none
 */
static RD_INLINE RD_UNUSED rd_bool_t rd_kafka_txn_may_send_msg(rd_kafka_t *rk) {
        rd_bool_t ret;

        rd_kafka_rdlock(rk);
        ret = (rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_IN_TRANSACTION ||
               rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_BEGIN_COMMIT);
        rd_kafka_rdunlock(rk);

        return ret;
}


/**
 * @returns true if transaction and partition state allows sending queued
 *          messages to broker, else false.
 *
 * @locality any
 * @locks toppar_lock MUST be held
 */
static RD_INLINE RD_UNUSED rd_bool_t
rd_kafka_txn_toppar_may_send_msg(rd_kafka_toppar_t *rktp) {
        if (likely(rktp->rktp_flags & RD_KAFKA_TOPPAR_F_IN_TXN))
                return rd_true;

        return rd_false;
}



void rd_kafka_txn_schedule_register_partitions(rd_kafka_t *rk, int backoff_ms);


/**
 * @brief Add partition to transaction (unless already added).
 *
 * The partition will first be added to the pending list (txn_pending_rktps)
 * awaiting registration on the coordinator with AddPartitionsToTxnRequest.
 * On successful registration the partition is flagged as IN_TXN and removed
 * from the pending list.
 *
 * @locality application thread
 * @locks none
 */
static RD_INLINE RD_UNUSED void
rd_kafka_txn_add_partition(rd_kafka_toppar_t *rktp) {
        rd_kafka_t *rk;
        rd_bool_t schedule = rd_false;

        rd_kafka_toppar_lock(rktp);

        /* Already added or registered */
        if (likely(rktp->rktp_flags &
                   (RD_KAFKA_TOPPAR_F_PEND_TXN | RD_KAFKA_TOPPAR_F_IN_TXN))) {
                rd_kafka_toppar_unlock(rktp);
                return;
        }

        rktp->rktp_flags |= RD_KAFKA_TOPPAR_F_PEND_TXN;

        rd_kafka_toppar_unlock(rktp);

        rk = rktp->rktp_rkt->rkt_rk;

        mtx_lock(&rk->rk_eos.txn_pending_lock);
        schedule = TAILQ_EMPTY(&rk->rk_eos.txn_pending_rktps);

        /* List is sorted by topic name since AddPartitionsToTxnRequest()
         * requires it. */
        TAILQ_INSERT_SORTED(&rk->rk_eos.txn_pending_rktps, rktp,
                            rd_kafka_toppar_t *, rktp_txnlink,
                            rd_kafka_toppar_topic_cmp);
        rd_kafka_toppar_keep(rktp);
        mtx_unlock(&rk->rk_eos.txn_pending_lock);

        rd_kafka_dbg(rk, EOS, "ADDPARTS",
                     "Marked %.*s [%" PRId32
                     "] as part of transaction: "
                     "%sscheduling registration",
                     RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                     rktp->rktp_partition, schedule ? "" : "not ");


        /* Schedule registration of partitions by the rdkafka main thread */
        if (unlikely(schedule))
                rd_kafka_txn_schedule_register_partitions(rk, 1 /*immediate*/);
}



void rd_kafka_txn_idemp_state_change(rd_kafka_t *rk,
                                     rd_kafka_idemp_state_t state);

void rd_kafka_txn_set_abortable_error0(rd_kafka_t *rk,
                                       rd_kafka_resp_err_t err,
                                       rd_bool_t requires_epoch_bump,
                                       const char *fmt,
                                       ...) RD_FORMAT(printf, 4, 5);
#define rd_kafka_txn_set_abortable_error(rk, err, ...)                         \
        rd_kafka_txn_set_abortable_error0(rk, err, rd_false, __VA_ARGS__)

#define rd_kafka_txn_set_abortable_error_with_bump(rk, err, ...)               \
        rd_kafka_txn_set_abortable_error0(rk, err, rd_true, __VA_ARGS__)

void rd_kafka_txn_set_fatal_error(rd_kafka_t *rk,
                                  rd_dolock_t do_lock,
                                  rd_kafka_resp_err_t err,
                                  const char *fmt,
                                  ...) RD_FORMAT(printf, 4, 5);

rd_bool_t rd_kafka_txn_coord_query(rd_kafka_t *rk, const char *reason);

rd_bool_t rd_kafka_txn_coord_set(rd_kafka_t *rk,
                                 rd_kafka_broker_t *rkb,
                                 const char *fmt,
                                 ...) RD_FORMAT(printf, 3, 4);

void rd_kafka_txns_term(rd_kafka_t *rk);
void rd_kafka_txns_init(rd_kafka_t *rk);

#endif /* _RDKAFKA_TXNMGR_H_ */
