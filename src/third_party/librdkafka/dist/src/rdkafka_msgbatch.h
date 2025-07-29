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
 * PRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
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

#ifndef _RDKAFKA_MSGBATCH_H_
#define _RDKAFKA_MSGBATCH_H_

typedef struct rd_kafka_msgbatch_s {
        rd_kafka_toppar_t *rktp; /**< Reference to partition */

        rd_kafka_msgq_t msgq; /**< Messages in batch */

        /* Following fields are for Idempotent Producer use */
        rd_kafka_pid_t pid;        /**< Producer Id and Epoch */
        int32_t first_seq;         /**< Base sequence */
        int64_t first_msgid;       /**< Base msgid */
        uint64_t epoch_base_msgid; /**< The partition epoch's
                                    *   base msgid. */
        uint64_t last_msgid;       /**< Last message to add to batch.
                                    *   This is used when reconstructing
                                    *   batches for resends with
                                    *   the idempotent producer which
                                    *   require retries to have the
                                    *   exact same messages in them. */

} rd_kafka_msgbatch_t;



/* defined in rdkafka_msg.c */
void rd_kafka_msgbatch_destroy(rd_kafka_msgbatch_t *rkmb);
void rd_kafka_msgbatch_init(rd_kafka_msgbatch_t *rkmb,
                            rd_kafka_toppar_t *rktp,
                            rd_kafka_pid_t pid,
                            uint64_t epoch_base_msgid);
void rd_kafka_msgbatch_set_first_msg(rd_kafka_msgbatch_t *rkmb,
                                     rd_kafka_msg_t *rkm);
void rd_kafka_msgbatch_ready_produce(rd_kafka_msgbatch_t *rkmb);

#endif /* _RDKAFKA_MSGBATCH_H_ */
