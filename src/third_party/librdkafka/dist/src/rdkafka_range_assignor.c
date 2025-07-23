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
 * https://github.com/apache/kafka/blob/trunk/clients/src/main/java/org/apache/kafka/clients/consumer/RangeAssignor.java
 *
 * The range assignor works on a per-topic basis. For each topic, we lay out the
 * available partitions in numeric order and the consumers in lexicographic
 * order. We then divide the number of partitions by the total number of
 * consumers to determine the number of partitions to assign to each consumer.
 * If it does not evenly divide, then the first few consumers will have one
 * extra partition.
 *
 * For example, suppose there are two consumers C0 and C1, two topics t0 and t1,
 * and each topic has 3 partitions, resulting in partitions t0p0, t0p1, t0p2,
 * t1p0, t1p1, and t1p2.
 *
 * The assignment will be:
 * C0: [t0p0, t0p1, t1p0, t1p1]
 * C1: [t0p2, t1p2]
 */

rd_kafka_resp_err_t
rd_kafka_range_assignor_assign_cb(rd_kafka_t *rk,
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
        int i;

        /* The range assignor works on a per-topic basis. */
        for (ti = 0; ti < eligible_topic_cnt; ti++) {
                rd_kafka_assignor_topic_t *eligible_topic = eligible_topics[ti];
                int numPartitionsPerConsumer;
                int consumersWithExtraPartition;

                /* For each topic, we lay out the available partitions in
                 * numeric order and the consumers in lexicographic order. */
                rd_list_sort(&eligible_topic->members,
                             rd_kafka_group_member_cmp);

                /* We then divide the number of partitions by the total number
                 * of consumers to determine the number of partitions to assign
                 * to each consumer. */
                numPartitionsPerConsumer =
                    eligible_topic->metadata->partition_cnt /
                    rd_list_cnt(&eligible_topic->members);

                /* If it does not evenly divide, then the first few consumers
                 * will have one extra partition. */
                consumersWithExtraPartition =
                    eligible_topic->metadata->partition_cnt %
                    rd_list_cnt(&eligible_topic->members);

                rd_kafka_dbg(rk, CGRP, "ASSIGN",
                             "range: Topic %s with %d partition(s) and "
                             "%d subscribing member(s)",
                             eligible_topic->metadata->topic,
                             eligible_topic->metadata->partition_cnt,
                             rd_list_cnt(&eligible_topic->members));

                for (i = 0; i < rd_list_cnt(&eligible_topic->members); i++) {
                        rd_kafka_group_member_t *rkgm =
                            rd_list_elem(&eligible_topic->members, i);
                        int start = numPartitionsPerConsumer * i +
                                    RD_MIN(i, consumersWithExtraPartition);
                        int length =
                            numPartitionsPerConsumer +
                            (i + 1 > consumersWithExtraPartition ? 0 : 1);

                        if (length == 0)
                                continue;

                        rd_kafka_dbg(rk, CGRP, "ASSIGN",
                                     "range: Member \"%s\": "
                                     "assigned topic %s partitions %d..%d",
                                     rkgm->rkgm_member_id->str,
                                     eligible_topic->metadata->topic, start,
                                     start + length - 1);
                        rd_kafka_topic_partition_list_add_range(
                            rkgm->rkgm_assignment,
                            eligible_topic->metadata->topic, start,
                            start + length - 1);
                }
        }

        return 0;
}



/**
 * @brief Initialzie and add range assignor.
 */
rd_kafka_resp_err_t rd_kafka_range_assignor_init(rd_kafka_t *rk) {
        return rd_kafka_assignor_add(
            rk, "consumer", "range", RD_KAFKA_REBALANCE_PROTOCOL_EAGER,
            rd_kafka_range_assignor_assign_cb,
            rd_kafka_assignor_get_metadata_with_empty_userdata, NULL, NULL,
            NULL, NULL);
}
