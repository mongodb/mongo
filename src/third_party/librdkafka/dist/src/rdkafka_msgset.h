/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2017 Magnus Edenhill
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

#ifndef _RDKAFKA_MSGSET_H_
#define _RDKAFKA_MSGSET_H_



/**
 * @struct rd_kafka_aborted_txns_t
 *
 * @brief A collection of aborted transactions.
 */
typedef struct rd_kafka_aborted_txns_s {
        rd_avl_t avl;
        /* Note: A list of nodes is maintained alongside
         * the AVL tree to facilitate traversal.
         */
        rd_list_t list;
        int32_t cnt;
} rd_kafka_aborted_txns_t;


rd_kafka_aborted_txns_t *rd_kafka_aborted_txns_new(int32_t txn_cnt);

void rd_kafka_aborted_txns_destroy(rd_kafka_aborted_txns_t *aborted_txns);

void rd_kafka_aborted_txns_sort(rd_kafka_aborted_txns_t *aborted_txns);

void rd_kafka_aborted_txns_add(rd_kafka_aborted_txns_t *aborted_txns,
                               int64_t pid,
                               int64_t first_offset);


/**
 * @name MessageSet writers
 */
rd_kafka_buf_t *rd_kafka_msgset_create_ProduceRequest(rd_kafka_broker_t *rkb,
                                                      rd_kafka_toppar_t *rktp,
                                                      rd_kafka_msgq_t *rkmq,
                                                      const rd_kafka_pid_t pid,
                                                      uint64_t epoch_base_msgid,
                                                      size_t *MessageSetSizep);

/**
 * @name MessageSet readers
 */
rd_kafka_resp_err_t
rd_kafka_msgset_parse(rd_kafka_buf_t *rkbuf,
                      rd_kafka_buf_t *request,
                      rd_kafka_toppar_t *rktp,
                      rd_kafka_aborted_txns_t *aborted_txns,
                      const struct rd_kafka_toppar_ver *tver);

int unittest_aborted_txns(void);

#endif /* _RDKAFKA_MSGSET_H_ */
