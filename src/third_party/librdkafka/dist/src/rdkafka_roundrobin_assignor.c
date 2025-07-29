/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2015 Magnus Edenhill
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
#include "rdkafka_assignor.h"


/**
 * Source:
 * https://github.com/apache/kafka/blob/trunk/clients/src/main/java/org/apache/kafka/clients/consumer/RoundRobinAssignor.java
 *
 * The roundrobin assignor lays out all the available partitions and all the
 * available consumers. It then proceeds to do a roundrobin assignment from
 * partition to consumer. If the subscriptions of all consumer instances are
 * identical, then the partitions will be uniformly distributed. (i.e., the
 * partition ownership counts will be within a delta of exactly one across all
 * consumers.)
 *
 * For example, suppose there are two consumers C0 and C1, two topics t0 and
 * t1, and each topic has 3 partitions, resulting in partitions t0p0, t0p1,
 * t0p2, t1p0, t1p1, and t1p2.
 *
 * The assignment will be:
 * C0: [t0p0, t0p2, t1p1]
 * C1: [t0p1, t1p0, t1p2]
 */

rd_kafka_resp_err_t rd_kafka_roundrobin_assignor_assign_cb(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    const char *member_id,
    const rd_kafka_metadata_t *metadata,
    rd_kafka_group_member_t *members,
    size_t member_cnt,
    rd_kafka_assignor_topic_t **eligible_topics,
    size_t eligible_topic_cnt,
    char *errstr,
    size_t errstr_size,
    void *opaque) {
        unsigned int ti;
        int next = -1; /* Next member id */

        /* Sort topics by name */
        qsort(eligible_topics, eligible_topic_cnt, sizeof(*eligible_topics),
              rd_kafka_assignor_topic_cmp);

        /* Sort members by name */
        qsort(members, member_cnt, sizeof(*members), rd_kafka_group_member_cmp);

        for (ti = 0; ti < eligible_topic_cnt; ti++) {
                rd_kafka_assignor_topic_t *eligible_topic = eligible_topics[ti];
                int partition;

                /* For each topic+partition, assign one member (in a cyclic
                 * iteration) per partition until the partitions are exhausted*/
                for (partition = 0;
                     partition < eligible_topic->metadata->partition_cnt;
                     partition++) {
                        rd_kafka_group_member_t *rkgm;

                        /* Scan through members until we find one with a
                         * subscription to this topic. */
                        do {
                                next = (next + 1) % member_cnt;
                        } while (!rd_kafka_group_member_find_subscription(
                            rk, &members[next],
                            eligible_topic->metadata->topic));

                        rkgm = &members[next];

                        rd_kafka_dbg(rk, CGRP, "ASSIGN",
                                     "roundrobin: Member \"%s\": "
                                     "assigned topic %s partition %d",
                                     rkgm->rkgm_member_id->str,
                                     eligible_topic->metadata->topic,
                                     partition);

                        rd_kafka_topic_partition_list_add(
                            rkgm->rkgm_assignment,
                            eligible_topic->metadata->topic, partition);
                }
        }


        return 0;
}



/**
 * @brief Initialzie and add roundrobin assignor.
 */
rd_kafka_resp_err_t rd_kafka_roundrobin_assignor_init(rd_kafka_t *rk) {
        return rd_kafka_assignor_add(
            rk, "consumer", "roundrobin", RD_KAFKA_REBALANCE_PROTOCOL_EAGER,
            rd_kafka_roundrobin_assignor_assign_cb,
            rd_kafka_assignor_get_metadata_with_empty_userdata, NULL, NULL,
            NULL, NULL);
}
