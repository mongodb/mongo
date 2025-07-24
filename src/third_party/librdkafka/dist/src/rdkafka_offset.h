/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
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

#ifndef _RDKAFKA_OFFSET_H_
#define _RDKAFKA_OFFSET_H_

#include "rdkafka_partition.h"


const char *rd_kafka_offset2str(int64_t offset);


/**
 * @brief Stores the offset for the toppar 'rktp'.
 *        The actual commit of the offset to backing store is usually
 *        performed at a later time (time or threshold based).
 *
 * For the high-level consumer (assign()), this function will reject absolute
 * offsets if the partition is not currently assigned, unless \p force is set.
 * This check was added to avoid a race condition where an application
 * would call offsets_store() after the partitions had been revoked, forcing
 * a future auto-committer on the next assignment to commit this old offset and
 * overwriting whatever newer offset was committed by another consumer.
 *
 * The \p force flag is useful for internal calls to offset_store0() which
 * do not need the protection described above.
 *
 *
 * There is one situation where the \p force flag is troublesome:
 * If the application is using any of the consumer batching APIs,
 * e.g., consume_batch() or the event-based consumption, then it's possible
 * that while the batch is being accumulated or the application is picking off
 * messages from the event a rebalance occurs (in the background) which revokes
 * the current assignment. This revokal will remove all queued messages, but
 * not the ones the application already has accumulated in the event object.
 * Enforcing assignment for store in this state is tricky with a bunch of
 * corner cases, so instead we let those places forcibly store the offset, but
 * then in assign() we reset the stored offset to .._INVALID, just like we do
 * on revoke.
 * Illustrated (with fix):
 *   1. ev = rd_kafka_queue_poll();
 *   2. background rebalance revoke unassigns the partition and sets the
 *      stored offset to _INVALID.
 *   3. application calls message_next(ev) which forcibly sets the
 *      stored offset.
 *   4. background rebalance assigns the partition again, but forcibly sets
 *      the stored offset to .._INVALID to provide a clean state.
 *
 * @param pos Offset and leader epoch to set, may be an absolute offset
 *            or .._INVALID.
 * @param metadata Metadata to be set (optional).
 * @param metadata_size Size of the metadata to be set.
 * @param force Forcibly set \p offset regardless of assignment state.
 * @param do_lock Whether to lock the \p rktp or not (already locked by caller).
 *
 * See head of rdkafka_offset.c for more information.
 *
 * @returns RD_KAFKA_RESP_ERR__STATE if the partition is not currently assigned,
 *          unless \p force is set.
 */
static RD_INLINE RD_UNUSED rd_kafka_resp_err_t
rd_kafka_offset_store0(rd_kafka_toppar_t *rktp,
                       const rd_kafka_fetch_pos_t pos,
                       void *metadata,
                       size_t metadata_size,
                       rd_bool_t force,
                       rd_dolock_t do_lock) {
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;

        if (do_lock)
                rd_kafka_toppar_lock(rktp);

        if (unlikely(!force && !RD_KAFKA_OFFSET_IS_LOGICAL(pos.offset) &&
                     !(rktp->rktp_flags & RD_KAFKA_TOPPAR_F_ASSIGNED) &&
                     !rd_kafka_is_simple_consumer(rktp->rktp_rkt->rkt_rk))) {
                err = RD_KAFKA_RESP_ERR__STATE;
        } else {
                if (rktp->rktp_stored_metadata) {
                        rd_free(rktp->rktp_stored_metadata);
                        rktp->rktp_stored_metadata = NULL;
                }
                rktp->rktp_stored_pos           = pos;
                rktp->rktp_stored_metadata_size = metadata_size;
                if (metadata) {
                        rktp->rktp_stored_metadata = rd_malloc(metadata_size);
                        memcpy(rktp->rktp_stored_metadata, metadata,
                               rktp->rktp_stored_metadata_size);
                }
        }

        if (do_lock)
                rd_kafka_toppar_unlock(rktp);

        return err;
}

rd_kafka_resp_err_t
rd_kafka_offset_store(rd_kafka_topic_t *rkt, int32_t partition, int64_t offset);

rd_kafka_resp_err_t rd_kafka_offset_sync(rd_kafka_toppar_t *rktp);

void rd_kafka_offset_store_term(rd_kafka_toppar_t *rktp,
                                rd_kafka_resp_err_t err);
rd_kafka_resp_err_t rd_kafka_offset_store_stop(rd_kafka_toppar_t *rktp);
void rd_kafka_offset_store_init(rd_kafka_toppar_t *rktp);

void rd_kafka_offset_reset(rd_kafka_toppar_t *rktp,
                           int32_t broker_id,
                           rd_kafka_fetch_pos_t err_pos,
                           rd_kafka_resp_err_t err,
                           const char *fmt,
                           ...) RD_FORMAT(printf, 5, 6);

void rd_kafka_offset_validate(rd_kafka_toppar_t *rktp, const char *fmt, ...)
    RD_FORMAT(printf, 2, 3);

void rd_kafka_offset_query_tmr_cb(rd_kafka_timers_t *rkts, void *arg);

void rd_kafka_update_app_pos(rd_kafka_t *rk,
                             rd_kafka_toppar_t *rktp,
                             rd_kafka_fetch_pos_t pos,
                             rd_dolock_t do_lock);

#endif /* _RDKAFKA_OFFSET_H_ */
