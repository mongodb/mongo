/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2020-2022, Magnus Edenhill
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


#ifndef _RDKAFKA_ASSIGNMENT_H_
#define _RDKAFKA_ASSIGNMENT_H_

typedef struct rd_kafka_assignment_s {
        /** All currently assigned partitions. */
        rd_kafka_topic_partition_list_t *all;
        /** Partitions in need of action (subset of .all) */
        rd_kafka_topic_partition_list_t *pending;
        /** Partitions that are being queried for committed
         * offsets (subset of .all) */
        rd_kafka_topic_partition_list_t *queried;
        /** Partitions that have been removed from the assignment
         * but not yet decommissioned. (not included in .all) */
        rd_kafka_topic_partition_list_t *removed;
        /** Number of started partitions */
        int started_cnt;
        /** Number of partitions being stopped. */
        int wait_stop_cnt;
        /** Assignment version: any change to the assignment will bump this
         *  version by one. This is used to know if a protocol response is
         *  outdated or not.
         *  @locks_required none
         *  @locality rdkafka main thread */
        int64_t version;
} rd_kafka_assignment_t;


int rd_kafka_assignment_clear(rd_kafka_t *rk);
rd_kafka_error_t *
rd_kafka_assignment_add(rd_kafka_t *rk,
                        rd_kafka_topic_partition_list_t *partitions);
rd_kafka_error_t *
rd_kafka_assignment_subtract(rd_kafka_t *rk,
                             rd_kafka_topic_partition_list_t *partitions);
void rd_kafka_assignment_partition_stopped(rd_kafka_t *rk,
                                           rd_kafka_toppar_t *rktp);
void rd_kafka_assignment_pause(rd_kafka_t *rk, const char *reason);
void rd_kafka_assignment_resume(rd_kafka_t *rk, const char *reason);
void rd_kafka_assignment_serve(rd_kafka_t *rk);
rd_bool_t rd_kafka_assignment_in_progress(rd_kafka_t *rk);
void rd_kafka_assignment_destroy(rd_kafka_t *rk);
void rd_kafka_assignment_init(rd_kafka_t *rk);

#endif /* _RDKAFKA_ASSIGNMENT_H_ */
