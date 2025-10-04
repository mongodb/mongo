/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2015-2022, Magnus Edenhill
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
#include "rdkafka_int.h"
#include "rdkafka_assignor.h"
#include "rdunittest.h"


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

typedef struct {
        rd_kafkap_str_t *member_id;
        rd_list_t *assigned_partitions; /* Contained Type: int* */
} rd_kafka_member_assigned_partitions_pair_t;

/**
 * @brief Intializes a rd_kafka_member_assigned_partitions_pair_t* with
 * assigned_partitions = [].
 *
 * @param member_id
 *
 * The member_id isn't copied, so the returned value can be used only for the
 * lifetime of this function's arguments.
 * @return rd_kafka_member_assigned_partitions_pair_t*
 */
static rd_kafka_member_assigned_partitions_pair_t *
rd_kafka_member_assigned_partitions_pair_new(rd_kafkap_str_t *member_id) {
        rd_kafka_member_assigned_partitions_pair_t *pair =
            rd_calloc(1, sizeof(rd_kafka_member_assigned_partitions_pair_t));

        pair->member_id           = member_id;
        pair->assigned_partitions = rd_list_new(0, NULL);
        return pair;
}

static void rd_kafka_member_assigned_partitions_pair_destroy(void *_pair) {
        rd_kafka_member_assigned_partitions_pair_t *pair =
            (rd_kafka_member_assigned_partitions_pair_t *)_pair;

        /* Do not destroy the member_id, we don't take ownership. */
        RD_IF_FREE(pair->assigned_partitions, rd_list_destroy);
        RD_IF_FREE(pair, rd_free);
}

static int rd_kafka_member_assigned_partitions_pair_cmp(const void *_a,
                                                        const void *_b) {
        rd_kafka_member_assigned_partitions_pair_t *a =
            (rd_kafka_member_assigned_partitions_pair_t *)_a;
        rd_kafka_member_assigned_partitions_pair_t *b =
            (rd_kafka_member_assigned_partitions_pair_t *)_b;
        return rd_kafkap_str_cmp(a->member_id, b->member_id);
}

static rd_kafka_member_assigned_partitions_pair_t *
rd_kafka_find_member_assigned_partitions_pair_by_member_id(
    rd_kafkap_str_t *member_id,
    rd_list_t *rd_kafka_member_assigned_partitions_pair_list) {
        rd_kafka_member_assigned_partitions_pair_t search_pair = {member_id,
                                                                  NULL};
        return rd_list_find(rd_kafka_member_assigned_partitions_pair_list,
                            &search_pair,
                            rd_kafka_member_assigned_partitions_pair_cmp);
}

typedef struct {
        /* Contains topic and list of members - sorted by group instance id and
         * member id. Also contains partitions, along with partition replicas,
         * which will help us with the racks. The members also contain their
         * rack id and the partitions they have already been assigned.
         */
        rd_kafka_assignor_topic_t *topic;
        /* unassigned_partitions[i] is true if the ith partition of this topic
         * is not assigned. We prefer using an array rather than using an
         * rd_list and removing elements, because that involves a memmove on
         * each remove. */
        rd_bool_t *unassigned_partitions;
        /* Number of partitions still to be assigned.*/
        size_t unassigned_partitions_left;
        /* An array of char** arrays. The ith element of this array is a sorted
         * char** array, denoting the racks for the ith partition of this topic.
         * The size of this array is equal to the partition_cnt. */
        char ***partition_racks;
        /* The ith element of this array is the size of partition_racks[i]. */
        size_t *racks_cnt;
        /* Contains a pair denoting the partitions assigned to every subscribed
         * consumer (member, [rd_list_t* of int*]). Sorted by member_id.
         * Contained Type: rd_kafka_member_assigned_partitions_pair_t* */
        rd_list_t *member_to_assigned_partitions;
        /* Contains the number of partitions that should be ideally assigned to
         * every subscribing consumer. */
        int num_partitions_per_consumer;
        /* Contains the number of consumers with extra partitions in case number
         * of partitions isn't perfectly divisible by number of consumers. */
        int remaining_consumers_with_extra_partition;
        /* True if we need to perform rack aware assignment. */
        rd_bool_t needs_rack_aware_assignment;
} rd_kafka_topic_assignment_state_t;


/**
 * @brief Initialize an rd_kafka_topic_assignment_state_t.
 *
 * @param topic
 * @param broker_rack_pair
 * @param broker_rack_pair_cnt
 *
 * The struct rd_kafka_topic_assignment_state_t is mostly for convenience and
 * easy grouping, so we avoid copying values as much as possible. Hence, the
 * returned rd_kafka_topic_assignment_state_t does not own all its values, and
 * should not be used beyond the lifetime of this function's arguments. This
 * function also computes the value of needsRackAwareAssignment given the other
 * information.
 *
 * @return rd_kafka_topic_assignment_state_t*
 */

static rd_kafka_topic_assignment_state_t *
rd_kafka_topic_assignment_state_new(rd_kafka_assignor_topic_t *topic,
                                    const rd_kafka_metadata_internal_t *mdi) {
        int i;
        rd_kafka_group_member_t *member;
        rd_kafka_topic_assignment_state_t *rktas;
        const int partition_cnt = topic->metadata->partition_cnt;

        rktas        = rd_calloc(1, sizeof(rd_kafka_topic_assignment_state_t));
        rktas->topic = topic; /* don't copy. */

        rktas->unassigned_partitions =
            rd_malloc(sizeof(rd_bool_t) * partition_cnt);
        rktas->unassigned_partitions_left = partition_cnt;
        for (i = 0; i < partition_cnt; i++) {
                rktas->unassigned_partitions[i] = rd_true;
        }

        rktas->num_partitions_per_consumer              = 0;
        rktas->remaining_consumers_with_extra_partition = 0;
        if (rd_list_cnt(&topic->members)) {
                rktas->num_partitions_per_consumer =
                    partition_cnt / rd_list_cnt(&topic->members);
                rktas->remaining_consumers_with_extra_partition =
                    partition_cnt % rd_list_cnt(&topic->members);
        }

        rktas->member_to_assigned_partitions =
            rd_list_new(0, rd_kafka_member_assigned_partitions_pair_destroy);

        RD_LIST_FOREACH(member, &topic->members, i) {
                rd_list_add(rktas->member_to_assigned_partitions,
                            rd_kafka_member_assigned_partitions_pair_new(
                                member->rkgm_member_id));
        }

        rd_list_sort(rktas->member_to_assigned_partitions,
                     rd_kafka_member_assigned_partitions_pair_cmp);

        rktas->partition_racks = rd_calloc(partition_cnt, sizeof(char **));
        rktas->racks_cnt       = rd_calloc(partition_cnt, sizeof(size_t));
        for (i = 0; topic->metadata_internal->partitions && i < partition_cnt;
             i++) {
                rktas->racks_cnt[i] =
                    topic->metadata_internal->partitions[i].racks_cnt;
                rktas->partition_racks[i] =
                    topic->metadata_internal->partitions[i].racks;
        }

        rktas->needs_rack_aware_assignment =
            rd_kafka_use_rack_aware_assignment(&topic, 1, mdi);

        return rktas;
}

/* Destroy a rd_kafka_topic_assignment_state_t. */
static void rd_kafka_topic_assignment_state_destroy(void *_rktas) {
        rd_kafka_topic_assignment_state_t *rktas =
            (rd_kafka_topic_assignment_state_t *)_rktas;

        rd_free(rktas->unassigned_partitions);
        rd_list_destroy(rktas->member_to_assigned_partitions);
        rd_free(rktas->partition_racks);
        rd_free(rktas->racks_cnt);
        rd_free(rktas);
}

/**
 * Compare two topic_assignment_states, first on the sorted list of consumers
 * (each consumer from the list of consumers is matched till the first point of
 * difference), and if that's equal, compare on the number of partitions.
 *
 * A list sorted with this comparator will group the topic_assignment_states
 * having the same consumers and the same number of partitions together - this
 * is the criteria of co-partitioned topics.
 */
static int rd_kafka_topic_assignment_state_cmp(const void *_a, const void *_b) {
        int i;
        rd_kafka_topic_assignment_state_t *a =
            (rd_kafka_topic_assignment_state_t *)_a;
        rd_kafka_topic_assignment_state_t *b =
            (rd_kafka_topic_assignment_state_t *)_b;

        /* This guarantee comes from rd_kafka_range_assignor_assign_cb. */
        rd_assert(a->topic->members.rl_flags & RD_LIST_F_SORTED);
        rd_assert(b->topic->members.rl_flags & RD_LIST_F_SORTED);

        /* Based on consumers */
        for (i = 0; i < rd_list_cnt(&a->topic->members) &&
                    i < rd_list_cnt(&b->topic->members);
             i++) {
                rd_kafka_group_member_t *am =
                    rd_list_elem(&a->topic->members, i);
                rd_kafka_group_member_t *bm =
                    rd_list_elem(&b->topic->members, i);
                int cmp_res =
                    rd_kafkap_str_cmp(am->rkgm_member_id, bm->rkgm_member_id);
                if (cmp_res != 0)
                        return cmp_res;
        }

        if (rd_list_cnt(&a->topic->members) !=
            rd_list_cnt(&b->topic->members)) {
                return RD_CMP(rd_list_cnt(&a->topic->members),
                              rd_list_cnt(&b->topic->members));
        }

        /* Based on number of partitions */
        return RD_CMP(a->topic->metadata->partition_cnt,
                      b->topic->metadata->partition_cnt);
}


/* Helper function to wrap a bsearch on the partition's racks. */
static char *rd_kafka_topic_assignment_state_rack_search(
    rd_kafka_topic_assignment_state_t *rktas,
    int partition,
    const char *rack) {
        char **partition_racks = rktas->partition_racks[partition];
        size_t cnt             = rktas->racks_cnt[partition];
        void *res              = NULL;

        if (!partition_racks)
                return NULL;

        res = bsearch(&rack, partition_racks, cnt, sizeof(char *), rd_strcmp3);
        if (!res)
                return NULL;

        return *(char **)res;
}

/*
 * Assigns a partition to a member, and updates fields in rktas for accounting.
 * It's assumed that the partitions assigned to this member don't exceed the
 * allowed number.
 */
static void rd_kafka_assign_partition(rd_kafka_group_member_t *member,
                                      rd_kafka_topic_assignment_state_t *rktas,
                                      int32_t partition) {
        rd_kafka_member_assigned_partitions_pair_t *member_assignment =
            rd_kafka_find_member_assigned_partitions_pair_by_member_id(
                member->rkgm_member_id, rktas->member_to_assigned_partitions);
        rd_assert(member_assignment);

        /* We can't use &partition, since that's a copy on the stack. */
        rd_list_add(member_assignment->assigned_partitions,
                    (void *)&rktas->topic->metadata->partitions[partition].id);
        rd_kafka_topic_partition_list_add_range(member->rkgm_assignment,
                                                rktas->topic->metadata->topic,
                                                partition, partition);

        rd_assert(rktas->unassigned_partitions[partition]);
        rktas->unassigned_partitions[partition] = rd_false;
        rktas->unassigned_partitions_left--;

        if (rd_list_cnt(member_assignment->assigned_partitions) >
            rktas->num_partitions_per_consumer) {
                rktas->remaining_consumers_with_extra_partition -= 1;
        }
}


/* Implementation of may_assign for rd_kafka_assign_ranges. True if the consumer
 * rack is empty, or if is exists within the partition racks. */
static rd_bool_t rd_kafka_racks_match(rd_kafka_group_member_t *member,
                                      rd_kafka_topic_assignment_state_t *rktas,
                                      int32_t partition) {
        rd_kafkap_str_t *consumer_rack = member->rkgm_rack_id;

        if (!consumer_rack || RD_KAFKAP_STR_LEN(consumer_rack) == 0) {
                return rd_true;
        }

        return rd_kafka_topic_assignment_state_rack_search(
                   rktas, partition, consumer_rack->str) != NULL;
}


/* Implementation of may_assign for rd_kafka_assign_ranges. Always true, used to
 * assign remaining partitions after rack-aware assignment is complete. */
static rd_bool_t rd_kafka_always(rd_kafka_group_member_t *member,
                                 rd_kafka_topic_assignment_state_t *rktas,
                                 int32_t partition) {
        return rd_true;
}

/* Assigns as many partitions as possible for a topic to subscribing members,
 * such that no subscribing member exceeds their limit of allowed partitions,
 * and may_assign(member, rktas, partition) is true for each member and
 * partition.
 */
static void rd_kafka_assign_ranges(
    rd_kafka_topic_assignment_state_t *rktas,
    rd_bool_t (*may_assign)(rd_kafka_group_member_t *member,
                            rd_kafka_topic_assignment_state_t *rktas,
                            int32_t partition)) {
        int i;
        rd_kafka_group_member_t *member;
        int32_t *partitions_to_assign =
            rd_alloca(rktas->unassigned_partitions_left * sizeof(int32_t));

        RD_LIST_FOREACH(member, &rktas->topic->members, i) {
                int j;
                rd_kafka_member_assigned_partitions_pair_t *member_assignment;
                int maximum_assignable_to_consumer;
                int partitions_to_assign_cnt;

                if (rktas->unassigned_partitions_left == 0)
                        break;

                member_assignment =
                    rd_kafka_find_member_assigned_partitions_pair_by_member_id(
                        member->rkgm_member_id,
                        rktas->member_to_assigned_partitions);

                maximum_assignable_to_consumer =
                    rktas->num_partitions_per_consumer +
                    (rktas->remaining_consumers_with_extra_partition > 0) -
                    rd_list_cnt(member_assignment->assigned_partitions);

                if (maximum_assignable_to_consumer <= 0)
                        continue;

                partitions_to_assign_cnt = 0;
                for (j = 0; j < rktas->topic->metadata->partition_cnt; j++) {
                        if (!rktas->unassigned_partitions[j]) {
                                continue;
                        }

                        if (maximum_assignable_to_consumer <= 0)
                                break;

                        if (!may_assign(member, rktas, j))
                                continue;

                        partitions_to_assign[partitions_to_assign_cnt] = j;
                        partitions_to_assign_cnt++;
                        maximum_assignable_to_consumer--;
                }

                for (j = 0; j < partitions_to_assign_cnt; j++)
                        rd_kafka_assign_partition(member, rktas,
                                                  partitions_to_assign[j]);
        }
}

/*
 * Assigns partitions for co-partitioned topics in a rack-aware manner on a best
 * effort basis. All partitions may not be assigned to consumers in case a rack
 * aware assignment does not exist.
 */
static void rd_kafka_assign_co_partitioned(
    rd_list_t *
        rktas_bucket /* Contained Type: rd_kafka_topic_assignment_state_t* */) {
        rd_kafka_topic_assignment_state_t *first_rktas =
            rd_list_elem(rktas_bucket, 0);
        rd_kafka_topic_assignment_state_t *rktas;
        rd_kafka_group_member_t *member;
        int i;

        /* Since a "bucket" is a group of topic_assignment_states with the same
         * consumers and number of partitions, we can just fetch them from the
         * first member of the bucket. */
        const int partition_cnt = first_rktas->topic->metadata->partition_cnt;
        const rd_list_t *consumers = &first_rktas->topic->members;

        for (i = 0; i < partition_cnt; i++) {
                /*
                 * To assign the ith partition of all the co partitioned topics,
                 * we need to find a consumerX that fulfils the criteria:
                 *  for all topic_assignment_states in the bucket:
                 *   1. rack(consumerX) is contained inside racks(partition i)
                 *   2. partitions assigned to consumerX does not exceed limits.
                 */
                int j;
                RD_LIST_FOREACH(member, consumers, j) {
                        int m;
                        RD_LIST_FOREACH(rktas, rktas_bucket, m) {
                                int maximum_assignable;
                                rd_kafka_member_assigned_partitions_pair_t
                                    *member_assignment;

                                /* Check (1.) */
                                if (!member->rkgm_rack_id ||
                                    RD_KAFKAP_STR_LEN(member->rkgm_rack_id) ==
                                        0 ||
                                    rd_kafka_topic_assignment_state_rack_search(
                                        rktas, i, member->rkgm_rack_id->str) ==
                                        NULL) {
                                        break;
                                }

                                /* Check (2.) */
                                member_assignment =
                                    rd_kafka_find_member_assigned_partitions_pair_by_member_id(
                                        member->rkgm_member_id,
                                        rktas->member_to_assigned_partitions);
                                maximum_assignable =
                                    rktas->num_partitions_per_consumer +
                                    (rktas
                                         ->remaining_consumers_with_extra_partition >
                                     0) -
                                    rd_list_cnt(
                                        member_assignment->assigned_partitions);

                                if (maximum_assignable <= 0) {
                                        break;
                                }
                        }
                        if (m == rd_list_cnt(rktas_bucket)) {
                                /* Break early - this consumer can be assigned
                                 * this partition. */
                                break;
                        }
                }
                if (j == rd_list_cnt(&first_rktas->topic->members)) {
                        continue; /* We didn't find a suitable consumer. */
                }

                rd_assert(member);

                RD_LIST_FOREACH(rktas, rktas_bucket, j) {
                        rd_kafka_assign_partition(member, rktas, i);
                }

                /* FIXME: A possible optimization: early break here if no
                 * consumer remains with maximum_assignable_to_consumer > 0
                 * across all topics. */
        }
}


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
        rd_list_t *rktas_list = rd_list_new(
            eligible_topic_cnt, rd_kafka_topic_assignment_state_destroy);
        rd_list_t *rktas_buckets = rd_list_new(0, rd_list_destroy_free);
        rd_list_t
            *rktas_current_bucket; /* Contained Type:
                                      rd_kafka_topic_assignment_state_t* */
        rd_kafka_topic_assignment_state_t *rktas;
        rd_kafka_topic_assignment_state_t *prev_rktas;
        const rd_kafka_metadata_internal_t *mdi =
            rd_kafka_metadata_get_internal(metadata);

        /* The range assignor works on a per-topic basis. */
        for (ti = 0; ti < eligible_topic_cnt; ti++) {
                rd_kafka_assignor_topic_t *eligible_topic = eligible_topics[ti];

                /* For each topic, we sort the consumers in lexicographic order,
                 * and create a topic_assignment_state. */
                rd_list_sort(&eligible_topic->members,
                             rd_kafka_group_member_cmp);
                rd_list_add(rktas_list, rd_kafka_topic_assignment_state_new(
                                            eligible_topic, mdi));
        }

        /* Sort the topic_assignment_states to group the topics which need to be
         * co-partitioned. */
        rd_list_sort(rktas_list, rd_kafka_topic_assignment_state_cmp);

        /* Use the sorted list of topic_assignment_states and separate them into
         * "buckets". Each bucket contains topics which can be co-partitioned,
         * ie with the same consumers and number of partitions. */
        prev_rktas           = NULL;
        rktas_current_bucket = NULL;
        RD_LIST_FOREACH(rktas, rktas_list, i) {
                if (prev_rktas && rd_kafka_topic_assignment_state_cmp(
                                      rktas, prev_rktas) == 0) {
                        rd_list_add(rktas_current_bucket, rktas);
                        continue;
                }

                /* The free function is set to NULL, as we don't copy any of the
                 * topic_assignment_states. */
                rktas_current_bucket = rd_list_new(0, NULL);
                rd_list_add(rktas_buckets, rktas_current_bucket);
                prev_rktas = rktas;
                rd_list_add(rktas_current_bucket, rktas);
        }

        /* Iterate through each bucket. In case there's more than one element in
         * the bucket, we prefer co-partitioning over rack awareness. Otherwise,
         * assign with rack-awareness. */
        rktas                = NULL;
        rktas_current_bucket = NULL;
        RD_LIST_FOREACH(rktas_current_bucket, rktas_buckets, i) {
                rd_assert(rd_list_cnt(rktas_current_bucket) > 0);

                if (rd_list_cnt(rktas_current_bucket) == 1) {
                        rktas = rd_list_elem(rktas_current_bucket, 0);
                        if (!rktas->needs_rack_aware_assignment)
                                continue;


                        rd_kafka_dbg(rk, CGRP, "ASSIGN",
                                     "range: Topic %s with %d partition(s) and "
                                     "%d subscribing member(s), single-topic "
                                     "rack-aware assignment",
                                     rktas->topic->metadata->topic,
                                     rktas->topic->metadata->partition_cnt,
                                     rd_list_cnt(&rktas->topic->members));

                        rd_kafka_assign_ranges(rktas, rd_kafka_racks_match);
                } else {
                        rktas = rd_list_elem(rktas_current_bucket, 0);
                        rd_kafka_dbg(
                            rk, CGRP, "ASSIGN",
                            "range: %d topics with %d partition(s) and "
                            "%d subscribing member(s), co-partitioned "
                            "rack-aware assignment",
                            rd_list_cnt(rktas_current_bucket),
                            rktas->topic->metadata->partition_cnt,
                            rd_list_cnt(&rktas->topic->members));

                        rd_kafka_assign_co_partitioned(rktas_current_bucket);
                }
        }

        /* Iterate through each rktas, doing normal assignment for any
         * partitions that might not have gotten a rack-aware assignment.*/
        RD_LIST_FOREACH(rktas, rktas_list, i) {
                rd_kafka_dbg(rk, CGRP, "ASSIGN",
                             "range: Topic %s with %d partition(s) and "
                             "%d subscribing member(s), single-topic "
                             "non-rack-aware assignment for %" PRIusz
                             " leftover partitions",
                             rktas->topic->metadata->topic,
                             rktas->topic->metadata->partition_cnt,
                             rd_list_cnt(&rktas->topic->members),
                             rktas->unassigned_partitions_left);
                rd_kafka_assign_ranges(rktas, rd_kafka_always);
        }

        rd_list_destroy(rktas_list);
        rd_list_destroy(rktas_buckets);

        return 0;
}


/**
 * @name Sticky assignor unit tests
 *
 *
 * These are based on RangeAssignorTest.java
 *
 *
 *
 */


/* All possible racks used in tests, as well as several common rack configs used
 * by consumers */
static rd_kafkap_str_t
    *ALL_RACKS[7]; /* initialized before starting the unit tests. */
static int RACKS_INITIAL[]  = {0, 1, 2};
static int RACKS_NULL[]     = {6, 6, 6};
static int RACKS_FINAL[]    = {4, 5, 6};
static int RACKS_ONE_NULL[] = {6, 4, 5};

static int
ut_testOneConsumerNoTopic(rd_kafka_t *rk,
                          const rd_kafka_assignor_t *rkas,
                          rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[1];


        if (parametrization == RD_KAFKA_RANGE_ASSIGNOR_UT_NO_BROKER_RACK) {
                RD_UT_PASS();
        }

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       0);

        ut_initMemberConditionalRack(&members[0], "consumer1", ALL_RACKS[0],
                                     parametrization, "t1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], NULL);

        rd_kafka_group_member_clear(&members[0]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

static int ut_testOneConsumerNonexistentTopic(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[1];


        if (parametrization == RD_KAFKA_RANGE_ASSIGNOR_UT_NO_BROKER_RACK) {
                RD_UT_PASS();
        }

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "t1", 0);

        ut_initMemberConditionalRack(&members[0], "consumer1", ALL_RACKS[0],
                                     parametrization, "t1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], NULL);

        rd_kafka_group_member_clear(&members[0]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int
ut_testOneConsumerOneTopic(rd_kafka_t *rk,
                           const rd_kafka_assignor_t *rkas,
                           rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[1];

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "t1", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1", ALL_RACKS[0],
                                     parametrization, "t1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);
        RD_UT_ASSERT(members[0].rkgm_assignment->cnt == 3,
                     "expected assignment of 3 partitions, got %d partition(s)",
                     members[0].rkgm_assignment->cnt);

        verifyAssignment(&members[0], "t1", 0, "t1", 1, "t1", 2, NULL);

        rd_kafka_group_member_clear(&members[0]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int ut_testOnlyAssignsPartitionsFromSubscribedTopics(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[1];

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       2, "t1", 3, "t2", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1", ALL_RACKS[0],
                                     parametrization, "t1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "t1", 0, "t1", 1, "t1", 2, NULL);

        rd_kafka_group_member_clear(&members[0]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

static int ut_testOneConsumerMultipleTopics(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[1];

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       2, "t1", 1, "t2", 2);

        ut_initMemberConditionalRack(&members[0], "consumer1", ALL_RACKS[0],
                                     parametrization, "t1", "t2", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "t1", 0, "t2", 0, "t2", 1, NULL);

        rd_kafka_group_member_clear(&members[0]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

static int ut_testTwoConsumersOneTopicOnePartition(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[2];

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "t1", 1);

        ut_initMemberConditionalRack(&members[0], "consumer1", ALL_RACKS[0],
                                     parametrization, "t1", NULL);
        ut_initMemberConditionalRack(&members[1], "consumer2", ALL_RACKS[1],
                                     parametrization, "t1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "t1", 0, NULL);
        verifyAssignment(&members[1], NULL);

        rd_kafka_group_member_clear(&members[0]);
        rd_kafka_group_member_clear(&members[1]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

static int ut_testTwoConsumersOneTopicTwoPartitions(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[2];

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "t1", 2);

        ut_initMemberConditionalRack(&members[0], "consumer1", ALL_RACKS[0],
                                     parametrization, "t1", NULL);
        ut_initMemberConditionalRack(&members[1], "consumer2", ALL_RACKS[1],
                                     parametrization, "t1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "t1", 0, NULL);
        verifyAssignment(&members[1], "t1", 1, NULL);

        rd_kafka_group_member_clear(&members[0]);
        rd_kafka_group_member_clear(&members[1]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

static int ut_testMultipleConsumersMixedTopicSubscriptions(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[3];

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       2, "t1", 3, "t2", 2);

        ut_initMemberConditionalRack(&members[0], "consumer1", ALL_RACKS[0],
                                     parametrization, "t1", NULL);
        ut_initMemberConditionalRack(&members[1], "consumer2", ALL_RACKS[1],
                                     parametrization, "t1", "t2", NULL);
        ut_initMemberConditionalRack(&members[2], "consumer3", ALL_RACKS[2],
                                     parametrization, "t1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "t1", 0, NULL);
        verifyAssignment(&members[1], "t1", 1, "t2", 0, "t2", 1, NULL);
        verifyAssignment(&members[2], "t1", 2, NULL);

        rd_kafka_group_member_clear(&members[0]);
        rd_kafka_group_member_clear(&members[1]);
        rd_kafka_group_member_clear(&members[2]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

static int ut_testTwoConsumersTwoTopicsSixPartitions(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[2];

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       2, "t1", 3, "t2", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1", ALL_RACKS[0],
                                     parametrization, "t1", "t2", NULL);
        ut_initMemberConditionalRack(&members[1], "consumer2", ALL_RACKS[1],
                                     parametrization, "t1", "t2", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "t1", 0, "t1", 1, "t2", 0, "t2", 1, NULL);
        verifyAssignment(&members[1], "t1", 2, "t2", 2, NULL);

        rd_kafka_group_member_clear(&members[0]);
        rd_kafka_group_member_clear(&members[1]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


/* Helper for setting up metadata and members, and running the assignor. Does
 * not check the results of the assignment. */
static int setupRackAwareAssignment0(rd_kafka_t *rk,
                                     const rd_kafka_assignor_t *rkas,
                                     rd_kafka_group_member_t *members,
                                     size_t member_cnt,
                                     int replication_factor,
                                     int num_broker_racks,
                                     size_t topic_cnt,
                                     char *topics[],
                                     int *partitions,
                                     int *subscriptions_count,
                                     char **subscriptions[],
                                     int *consumer_racks,
                                     rd_kafka_metadata_t **metadata) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata_local = NULL;
        if (!metadata)
                metadata = &metadata_local;

        size_t i              = 0;
        const int num_brokers = num_broker_racks > 0
                                    ? replication_factor * num_broker_racks
                                    : replication_factor;

        /* The member naming for tests is consumerN where N is a single
         * character. */
        rd_assert(member_cnt <= 9);

        *metadata = rd_kafka_metadata_new_topic_with_partition_replicas_mock(
            replication_factor, num_brokers, topics, partitions, topic_cnt);
        ut_populate_internal_broker_metadata(
            rd_kafka_metadata_get_internal(*metadata), num_broker_racks,
            ALL_RACKS, RD_ARRAYSIZE(ALL_RACKS));
        ut_populate_internal_topic_metadata(
            rd_kafka_metadata_get_internal(*metadata));

        for (i = 0; i < member_cnt; i++) {
                char member_id[10];
                snprintf(member_id, 10, "consumer%d", (int)(i + 1));
                ut_init_member_with_rack(
                    &members[i], member_id, ALL_RACKS[consumer_racks[i]],
                    subscriptions[i], subscriptions_count[i]);
        }

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, *metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        if (metadata_local)
                ut_destroy_metadata(metadata_local);
        return 0;
}

static int setupRackAwareAssignment(rd_kafka_t *rk,
                                    const rd_kafka_assignor_t *rkas,
                                    rd_kafka_group_member_t *members,
                                    size_t member_cnt,
                                    int replication_factor,
                                    int num_broker_racks,
                                    size_t topic_cnt,
                                    char *topics[],
                                    int *partitions,
                                    int *subscriptions_count,
                                    char **subscriptions[],
                                    int *consumer_racks) {
        return setupRackAwareAssignment0(
            rk, rkas, members, member_cnt, replication_factor, num_broker_racks,
            topic_cnt, topics, partitions, subscriptions_count, subscriptions,
            consumer_racks, NULL);
}

/* Helper for testing cases where rack-aware assignment should not be triggered,
 * and assignment should be the same as the pre-rack-aware assignor. */
#define verifyNonRackAwareAssignment(rk, rkas, members, member_cnt, topic_cnt, \
                                     topics, partitions, subscriptions_count,  \
                                     subscriptions, ...)                       \
        do {                                                                   \
                size_t idx                    = 0;                             \
                rd_kafka_metadata_t *metadata = NULL;                          \
                                                                               \
                /* num_broker_racks = 0, implies that brokers have no          \
                 * configured racks. */                                        \
                setupRackAwareAssignment(rk, rkas, members, member_cnt, 3, 0,  \
                                         topic_cnt, topics, partitions,        \
                                         subscriptions_count, subscriptions,   \
                                         RACKS_INITIAL);                       \
                verifyMultipleAssignment(members, member_cnt, __VA_ARGS__);    \
                for (idx = 0; idx < member_cnt; idx++)                         \
                        rd_kafka_group_member_clear(&members[idx]);            \
                /* consumer_racks = RACKS_NULL implies that consumers have no  \
                 * racks. */                                                   \
                setupRackAwareAssignment(rk, rkas, members, member_cnt, 3, 3,  \
                                         topic_cnt, topics, partitions,        \
                                         subscriptions_count, subscriptions,   \
                                         RACKS_NULL);                          \
                verifyMultipleAssignment(members, member_cnt, __VA_ARGS__);    \
                for (idx = 0; idx < member_cnt; idx++)                         \
                        rd_kafka_group_member_clear(&members[idx]);            \
                /* replication_factor = 3 and num_broker_racks = 3 means that  \
                 * all partitions are replicated on all racks.*/               \
                setupRackAwareAssignment0(rk, rkas, members, member_cnt, 3, 3, \
                                          topic_cnt, topics, partitions,       \
                                          subscriptions_count, subscriptions,  \
                                          RACKS_INITIAL, &metadata);           \
                verifyMultipleAssignment(members, member_cnt, __VA_ARGS__);    \
                verifyNumPartitionsWithRackMismatch(metadata, members,         \
                                                    RD_ARRAYSIZE(members), 0); \
                                                                               \
                for (idx = 0; idx < member_cnt; idx++)                         \
                        rd_kafka_group_member_clear(&members[idx]);            \
                ut_destroy_metadata(metadata);                                 \
                /* replication_factor = 4 and num_broker_racks = 4 means that  \
                 * all partitions are replicated on all racks. */              \
                setupRackAwareAssignment0(rk, rkas, members, member_cnt, 4, 4, \
                                          topic_cnt, topics, partitions,       \
                                          subscriptions_count, subscriptions,  \
                                          RACKS_INITIAL, &metadata);           \
                verifyMultipleAssignment(members, member_cnt, __VA_ARGS__);    \
                verifyNumPartitionsWithRackMismatch(metadata, members,         \
                                                    RD_ARRAYSIZE(members), 0); \
                                                                               \
                for (idx = 0; idx < member_cnt; idx++)                         \
                        rd_kafka_group_member_clear(&members[idx]);            \
                ut_destroy_metadata(metadata);                                 \
                /* There's no overap between broker racks and consumer racks,  \
                 * since num_broker_racks = 3, they'll be picked from a,b,c    \
                 * and consumer racks are d,e,f. */                            \
                setupRackAwareAssignment(rk, rkas, members, member_cnt, 3, 3,  \
                                         topic_cnt, topics, partitions,        \
                                         subscriptions_count, subscriptions,   \
                                         RACKS_FINAL);                         \
                verifyMultipleAssignment(members, member_cnt, __VA_ARGS__);    \
                for (idx = 0; idx < member_cnt; idx++)                         \
                        rd_kafka_group_member_clear(&members[idx]);            \
                /* There's no overap between broker racks and consumer racks,  \
                 * since num_broker_racks = 3, they'll be picked from a,b,c    \
                 * and consumer racks are d,e,NULL. */                         \
                setupRackAwareAssignment(rk, rkas, members, member_cnt, 3, 3,  \
                                         topic_cnt, topics, partitions,        \
                                         subscriptions_count, subscriptions,   \
                                         RACKS_ONE_NULL);                      \
                verifyMultipleAssignment(members, member_cnt, __VA_ARGS__);    \
                for (idx = 0; idx < member_cnt; idx++)                         \
                        rd_kafka_group_member_clear(&members[idx]);            \
        } while (0)

static int ut_testRackAwareAssignmentWithUniformSubscription(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        char *topics[]   = {"t1", "t2", "t3"};
        int partitions[] = {6, 7, 2};
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[3];
        size_t i                  = 0;
        int subscriptions_count[] = {3, 3, 3};
        char **subscriptions[]    = {topics, topics, topics};

        if (parametrization !=
            RD_KAFKA_RANGE_ASSIGNOR_UT_BROKER_AND_CONSUMER_RACK) {
                RD_UT_PASS();
        }

        verifyNonRackAwareAssignment(
            rk, rkas, members, RD_ARRAYSIZE(members), RD_ARRAYSIZE(topics),
            topics, partitions, subscriptions_count, subscriptions,
            /* consumer1*/
            "t1", 0, "t1", 1, "t2", 0, "t2", 1, "t2", 2, "t3", 0, NULL,
            /* consumer2 */
            "t1", 2, "t1", 3, "t2", 3, "t2", 4, "t3", 1, NULL,
            /* consumer3 */
            "t1", 4, "t1", 5, "t2", 5, "t2", 6, NULL);

        /* Verify best-effort rack-aware assignment for lower replication factor
         * where racks have a subset of partitions.*/
        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 1,
                                  3, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions,
                                  RACKS_INITIAL, &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 3, "t2", 0, "t2", 3, "t2", 6, NULL,
            /* consumer2 */
            "t1", 1, "t1", 4, "t2", 1, "t2", 4, "t3", 0, NULL,
            /* consumer3 */
            "t1", 2, "t1", 5, "t2", 2, "t2", 5, "t3", 1, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 0);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 2,
                                  3, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions,
                                  RACKS_INITIAL, &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /*consumer1*/
            "t1", 0, "t1", 2, "t2", 0, "t2", 2, "t2", 3, "t3", 1, NULL,
            /* consumer2 */
            "t1", 1, "t1", 3, "t2", 1, "t2", 4, "t3", 0, NULL,
            /* consumer 3*/
            "t1", 4, "t1", 5, "t2", 5, "t2", 6, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 1);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);


        /* One consumer on a rack with no partitions. */
        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 3,
                                  2, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions,
                                  RACKS_INITIAL, &metadata);
        verifyMultipleAssignment(members, RD_ARRAYSIZE(members),
                                 /* consumer1 */ "t1", 0, "t1", 1, "t2", 0,
                                 "t2", 1, "t2", 2, "t3", 0, NULL,
                                 /* consumer2 */
                                 "t1", 2, "t1", 3, "t2", 3, "t2", 4, "t3", 1,
                                 NULL,
                                 /* consumer3 */
                                 "t1", 4, "t1", 5, "t2", 5, "t2", 6, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 4);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

static int ut_testRackAwareAssignmentWithNonEqualSubscription(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_metadata_t *metadata;
        char *topics[]   = {"t1", "t2", "t3"};
        int partitions[] = {6, 7, 2};
        rd_kafka_group_member_t members[3];
        size_t i                  = 0;
        int subscriptions_count[] = {3, 3, 2};
        char *subscription13[]    = {"t1", "t3"};
        char **subscriptions[]    = {topics, topics, subscription13};

        if (parametrization !=
            RD_KAFKA_RANGE_ASSIGNOR_UT_BROKER_AND_CONSUMER_RACK) {
                RD_UT_PASS();
        }

        verifyNonRackAwareAssignment(
            rk, rkas, members, RD_ARRAYSIZE(members), RD_ARRAYSIZE(topics),
            topics, partitions, subscriptions_count, subscriptions,
            /* consumer1*/
            "t1", 0, "t1", 1, "t2", 0, "t2", 1, "t2", 2, "t2", 3, "t3", 0, NULL,
            /* consumer2 */
            "t1", 2, "t1", 3, "t2", 4, "t2", 5, "t2", 6, "t3", 1, NULL,
            /* consumer3 */
            "t1", 4, "t1", 5, NULL);

        /* Verify best-effort rack-aware assignment for lower replication factor
         * where racks have a subset of partitions. */
        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 1,
                                  3, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions,
                                  RACKS_INITIAL, &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 3, "t2", 0, "t2", 2, "t2", 3, "t2", 6, NULL,
            /* consumer2 */
            "t1", 1, "t1", 4, "t2", 1, "t2", 4, "t2", 5, "t3", 0, NULL,
            /* consumer3 */
            "t1", 2, "t1", 5, "t3", 1, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 2);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 2,
                                  3, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions,
                                  RACKS_INITIAL, &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 2, "t2", 0, "t2", 2, "t2", 3, "t2", 5, "t3", 1, NULL,
            /* consumer2 */
            "t1", 1, "t1", 3, "t2", 1, "t2", 4, "t2", 6, "t3", 0, NULL,
            /* consumer3 */
            "t1", 4, "t1", 5, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 0);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        /* One consumer on a rack with no partitions */
        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 3,
                                  2, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions,
                                  RACKS_INITIAL, &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 1, "t2", 0, "t2", 1, "t2", 2, "t2", 3, "t3", 0, NULL,
            /* consumer2 */
            "t1", 2, "t1", 3, "t2", 4, "t2", 5, "t2", 6, "t3", 1, NULL,
            /* consumer3 */
            "t1", 4, "t1", 5, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 2);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

static int ut_testRackAwareAssignmentWithUniformPartitions(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        char *topics[]            = {"t1", "t2", "t3"};
        int partitions[]          = {5, 5, 5};
        int partitions_mismatch[] = {10, 5, 3};
        rd_kafka_group_member_t members[3];
        size_t i                  = 0;
        int replication_factor    = 0;
        int subscriptions_count[] = {3, 3, 3};
        char **subscriptions[]    = {topics, topics, topics};

        if (parametrization !=
            RD_KAFKA_RANGE_ASSIGNOR_UT_BROKER_AND_CONSUMER_RACK) {
                RD_UT_PASS();
        }

        /* Verify combinations where rack-aware logic is not used. */
        verifyNonRackAwareAssignment(
            rk, rkas, members, RD_ARRAYSIZE(members), RD_ARRAYSIZE(topics),
            topics, partitions, subscriptions_count, subscriptions,
            /* consumer1*/
            "t1", 0, "t1", 1, "t2", 0, "t2", 1, "t3", 0, "t3", 1, NULL,
            /* consumer2 */
            "t1", 2, "t1", 3, "t2", 2, "t2", 3, "t3", 2, "t3", 3, NULL,
            /* consumer3 */
            "t1", 4, "t2", 4, "t3", 4, NULL);

        /* Verify that co-partitioning is prioritized over rack-alignment for
         * topics with equal subscriptions */
        for (replication_factor = 1; replication_factor <= 3;
             replication_factor++) {
                rd_kafka_metadata_t *metadata = NULL;
                setupRackAwareAssignment0(
                    rk, rkas, members, RD_ARRAYSIZE(members),
                    replication_factor, replication_factor < 3 ? 3 : 2,
                    RD_ARRAYSIZE(topics), topics, partitions,
                    subscriptions_count, subscriptions, RACKS_INITIAL,
                    &metadata);
                verifyMultipleAssignment(
                    members, RD_ARRAYSIZE(members),
                    /* consumer1*/
                    "t1", 0, "t1", 1, "t2", 0, "t2", 1, "t3", 0, "t3", 1, NULL,
                    /* consumer2 */
                    "t1", 2, "t1", 3, "t2", 2, "t2", 3, "t3", 2, "t3", 3, NULL,
                    /* consumer3 */
                    "t1", 4, "t2", 4, "t3", 4, NULL);
                verifyNumPartitionsWithRackMismatch(
                    metadata, members, RD_ARRAYSIZE(members),
                    partitions_mismatch[replication_factor - 1]);

                for (i = 0; i < RD_ARRAYSIZE(members); i++)
                        rd_kafka_group_member_clear(&members[i]);
                ut_destroy_metadata(metadata);
        }

        RD_UT_PASS();
}

static int ut_testRackAwareAssignmentWithUniformPartitionsNonEqualSubscription(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_metadata_t *metadata = NULL;
        char *topics[]                = {"t1", "t2", "t3"};
        int partitions[]              = {5, 5, 5};
        rd_kafka_group_member_t members[3];
        size_t i                  = 0;
        int subscriptions_count[] = {3, 3, 2};
        char *subscription13[]    = {"t1", "t3"};
        char **subscriptions[]    = {topics, topics, subscription13};

        if (parametrization !=
            RD_KAFKA_RANGE_ASSIGNOR_UT_BROKER_AND_CONSUMER_RACK) {
                RD_UT_PASS();
        }

        /* Verify combinations where rack-aware logic is not used. */
        verifyNonRackAwareAssignment(
            rk, rkas, members, RD_ARRAYSIZE(members), RD_ARRAYSIZE(topics),
            topics, partitions, subscriptions_count, subscriptions,
            /* consumer1*/
            "t1", 0, "t1", 1, "t2", 0, "t2", 1, "t2", 2, "t3", 0, "t3", 1, NULL,
            /* consumer2 */
            "t1", 2, "t1", 3, "t2", 3, "t2", 4, "t3", 2, "t3", 3, NULL,
            /* consumer3 */
            "t1", 4, "t3", 4, NULL);

        /* Verify that co-partitioning is prioritized over rack-alignment for
         * topics with equal subscriptions */
        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 1,
                                  3, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions,
                                  RACKS_INITIAL, &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 1, "t2", 0, "t2", 1, "t2", 4, "t3", 0, "t3", 1, NULL,
            /* consumer2 */
            "t1", 2, "t1", 3, "t2", 2, "t2", 3, "t3", 2, "t3", 3, NULL,
            /* consumer3 */
            "t1", 4, "t3", 4, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 9);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);


        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 2,
                                  3, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions,
                                  RACKS_INITIAL, &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 2, "t2", 0, "t2", 1, "t2", 3, "t3", 2, NULL,
            /* consumer2 */
            "t1", 0, "t1", 3, "t2", 2, "t2", 4, "t3", 0, "t3", 3, NULL,
            /* consumer3 */
            "t1", 1, "t1", 4, "t3", 1, "t3", 4, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 0);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        /* One consumer on a rack with no partitions */
        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 3,
                                  2, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions,
                                  RACKS_INITIAL, &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 1, "t2", 0, "t2", 1, "t2", 2, "t3", 0, "t3", 1, NULL,
            /* consumer2 */
            "t1", 2, "t1", 3, "t2", 3, "t2", 4, "t3", 2, "t3", 3, NULL,
            /* consumer3 */
            "t1", 4, "t3", 4, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 2);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

static int ut_testRackAwareAssignmentWithCoPartitioning0(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_metadata_t *metadata = NULL;
        char *topics[]                = {"t1", "t2", "t3", "t4"};
        int partitions[]              = {6, 6, 2, 2};
        rd_kafka_group_member_t members[4];
        size_t i                  = 0;
        int subscriptions_count[] = {2, 2, 2, 2};
        char *subscription12[]    = {"t1", "t2"};
        char *subscription34[]    = {"t3", "t4"};
        char **subscriptions[]    = {subscription12, subscription12,
                                  subscription34, subscription34};
        int racks[]               = {0, 1, 1, 0};

        if (parametrization !=
            RD_KAFKA_RANGE_ASSIGNOR_UT_BROKER_AND_CONSUMER_RACK) {
                RD_UT_PASS();
        }

        setupRackAwareAssignment(rk, rkas, members, RD_ARRAYSIZE(members), 3, 2,
                                 RD_ARRAYSIZE(topics), topics, partitions,
                                 subscriptions_count, subscriptions, racks);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 1, "t1", 2, "t2", 0, "t2", 1, "t2", 2, NULL,
            /* consumer2 */
            "t1", 3, "t1", 4, "t1", 5, "t2", 3, "t2", 4, "t2", 5, NULL,
            /* consumer3 */
            "t3", 0, "t4", 0, NULL,
            /* consumer4 */
            "t3", 1, "t4", 1, NULL);
        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);

        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 2,
                                  2, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions, racks,
                                  &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 1, "t1", 2, "t2", 0, "t2", 1, "t2", 2, NULL,
            /* consumer2 */
            "t1", 3, "t1", 4, "t1", 5, "t2", 3, "t2", 4, "t2", 5, NULL,
            /* consumer3 */
            "t3", 0, "t4", 0, NULL,
            /* consumer4 */
            "t3", 1, "t4", 1, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 0);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 1,
                                  2, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions, racks,
                                  &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 2, "t1", 4, "t2", 0, "t2", 2, "t2", 4, NULL,
            /* consumer2 */
            "t1", 1, "t1", 3, "t1", 5, "t2", 1, "t2", 3, "t2", 5, NULL,
            /* consumer3 */
            "t3", 1, "t4", 1, NULL,
            /* consumer4 */
            "t3", 0, "t4", 0, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 0);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

static int ut_testRackAwareAssignmentWithCoPartitioning1(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_metadata_t *metadata = NULL;
        char *topics[]                = {"t1", "t2", "t3", "t4"};
        int partitions[]              = {6, 6, 2, 2};
        rd_kafka_group_member_t members[4];
        size_t i                  = 0;
        int subscriptions_count[] = {4, 4, 4, 4};
        char **subscriptions[]    = {topics, topics, topics, topics};
        int racks[]               = {0, 1, 1, 0};

        if (parametrization !=
            RD_KAFKA_RANGE_ASSIGNOR_UT_BROKER_AND_CONSUMER_RACK) {
                RD_UT_PASS();
        }

        setupRackAwareAssignment(rk, rkas, members, RD_ARRAYSIZE(members), 3, 2,
                                 RD_ARRAYSIZE(topics), topics, partitions,
                                 subscriptions_count, subscriptions, racks);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 1, "t2", 0, "t2", 1, "t3", 0, "t4", 0, NULL,
            /* consumer2 */
            "t1", 2, "t1", 3, "t2", 2, "t2", 3, "t3", 1, "t4", 1, NULL,
            /* consumer3 */
            "t1", 4, "t2", 4, NULL,
            /* consumer4 */
            "t1", 5, "t2", 5, NULL);
        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);

        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 2,
                                  2, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions, racks,
                                  &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 1, "t2", 0, "t2", 1, "t3", 0, "t4", 0, NULL,
            /* consumer2 */
            "t1", 2, "t1", 3, "t2", 2, "t2", 3, "t3", 1, "t4", 1, NULL,
            /* consumer3 */
            "t1", 4, "t2", 4, NULL,
            /* consumer4 */
            "t1", 5, "t2", 5, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 0);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);


        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 1,
                                  2, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions, racks,
                                  &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 2, "t2", 0, "t2", 2, "t3", 0, "t4", 0, NULL,
            /* consumer2 */
            "t1", 1, "t1", 3, "t2", 1, "t2", 3, "t3", 1, "t4", 1, NULL,
            /* consumer3 */
            "t1", 5, "t2", 5, NULL,
            /* consumer4 */
            "t1", 4, "t2", 4, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 0);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);


        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 1,
                                  3, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions, racks,
                                  &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 3, "t2", 0, "t2", 3, "t3", 0, "t4", 0, NULL,
            /* consumer2 */
            "t1", 1, "t1", 4, "t2", 1, "t2", 4, "t3", 1, "t4", 1, NULL,
            /* consumer3 */
            "t1", 2, "t2", 2, NULL,
            /* consumer4 */
            "t1", 5, "t2", 5, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 6);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

static int ut_testCoPartitionedAssignmentWithSameSubscription(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_metadata_t *metadata = NULL;
        char *topics[]                = {"t1", "t2", "t3", "t4", "t5", "t6"};
        int partitions[]              = {6, 6, 2, 2, 4, 4};
        rd_kafka_group_member_t members[3];
        size_t i                  = 0;
        int subscriptions_count[] = {6, 6, 6};
        char **subscriptions[]    = {topics, topics, topics};

        if (parametrization !=
            RD_KAFKA_RANGE_ASSIGNOR_UT_BROKER_AND_CONSUMER_RACK) {
                RD_UT_PASS();
        }

        setupRackAwareAssignment(rk, rkas, members, RD_ARRAYSIZE(members), 3, 0,
                                 RD_ARRAYSIZE(topics), topics, partitions,
                                 subscriptions_count, subscriptions,
                                 RACKS_INITIAL);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 1, "t2", 0, "t2", 1, "t3", 0, "t4", 0, "t5", 0, "t5",
            1, "t6", 0, "t6", 1, NULL,
            /* consumer2 */
            "t1", 2, "t1", 3, "t2", 2, "t2", 3, "t3", 1, "t4", 1, "t5", 2, "t6",
            2, NULL,
            /* consumer3 */
            "t1", 4, "t1", 5, "t2", 4, "t2", 5, "t5", 3, "t6", 3, NULL);
        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);

        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 3,
                                  3, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions,
                                  RACKS_INITIAL, &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 0, "t1", 1, "t2", 0, "t2", 1, "t3", 0, "t4", 0, "t5", 0, "t5",
            1, "t6", 0, "t6", 1, NULL,
            /* consumer2 */
            "t1", 2, "t1", 3, "t2", 2, "t2", 3, "t3", 1, "t4", 1, "t5", 2, "t6",
            2, NULL,
            /* consumer3 */
            "t1", 4, "t1", 5, "t2", 4, "t2", 5, "t5", 3, "t6", 3, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 0);
        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int rd_kafka_range_assignor_unittest(void) {
        rd_kafka_conf_t *conf;
        rd_kafka_t *rk;
        int fails = 0;
        char errstr[256];
        rd_kafka_assignor_t *rkas;
        size_t i;

        conf = rd_kafka_conf_new();
        if (rd_kafka_conf_set(conf, "group.id", "test", errstr,
                              sizeof(errstr)) ||
            rd_kafka_conf_set(conf, "partition.assignment.strategy", "range",
                              errstr, sizeof(errstr)))
                RD_UT_FAIL("range assignor conf failed: %s", errstr);

        rd_kafka_conf_set(conf, "debug", rd_getenv("TEST_DEBUG", NULL), NULL,
                          0);

        rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
        RD_UT_ASSERT(rk, "range assignor client instantiation failed: %s",
                     errstr);
        rkas = rd_kafka_assignor_find(rk, "range");
        RD_UT_ASSERT(rkas, "range assignor not found");

        for (i = 0; i < RD_ARRAY_SIZE(ALL_RACKS) - 1; i++) {
                char c       = 'a' + i;
                ALL_RACKS[i] = rd_kafkap_str_new(&c, 1);
        }
        ALL_RACKS[i] = NULL;

        static int (*tests[])(
            rd_kafka_t *, const rd_kafka_assignor_t *,
            rd_kafka_assignor_ut_rack_config_t parametrization) = {
            ut_testOneConsumerNoTopic,
            ut_testOneConsumerNonexistentTopic,
            ut_testOneConsumerOneTopic,
            ut_testOnlyAssignsPartitionsFromSubscribedTopics,
            ut_testOneConsumerMultipleTopics,
            ut_testTwoConsumersOneTopicOnePartition,
            ut_testTwoConsumersOneTopicTwoPartitions,
            ut_testMultipleConsumersMixedTopicSubscriptions,
            ut_testTwoConsumersTwoTopicsSixPartitions,
            ut_testRackAwareAssignmentWithUniformSubscription,
            ut_testRackAwareAssignmentWithNonEqualSubscription,
            ut_testRackAwareAssignmentWithUniformPartitions,
            ut_testRackAwareAssignmentWithUniformPartitionsNonEqualSubscription,
            ut_testRackAwareAssignmentWithCoPartitioning0,
            ut_testRackAwareAssignmentWithCoPartitioning1,
            ut_testCoPartitionedAssignmentWithSameSubscription,
            NULL,
        };

        for (i = 0; tests[i]; i++) {
                rd_ts_t ts = rd_clock();
                int r      = 0;
                rd_kafka_assignor_ut_rack_config_t j;

                for (j = RD_KAFKA_RANGE_ASSIGNOR_UT_NO_BROKER_RACK;
                     j != RD_KAFKA_RANGE_ASSIGNOR_UT_CONFIG_CNT; j++) {
                        RD_UT_SAY("[ Test #%" PRIusz ", RackConfig = %d ]", i,
                                  j);
                        r += tests[i](rk, rkas, j);
                }
                RD_UT_SAY("[ Test #%" PRIusz " ran for %.3fms ]", i,
                          (double)(rd_clock() - ts) / 1000.0);

                RD_UT_ASSERT(!r, "^ failed");

                fails += r;
        }

        for (i = 0; i < RD_ARRAY_SIZE(ALL_RACKS) - 1; i++) {
                rd_kafkap_str_destroy(ALL_RACKS[i]);
        }

        rd_kafka_destroy(rk);

        return fails;
}



/**
 * @brief Initialzie and add range assignor.
 */
rd_kafka_resp_err_t rd_kafka_range_assignor_init(rd_kafka_t *rk) {
        return rd_kafka_assignor_add(
            rk, "consumer", "range", RD_KAFKA_REBALANCE_PROTOCOL_EAGER,
            rd_kafka_range_assignor_assign_cb,
            rd_kafka_assignor_get_metadata_with_empty_userdata,
            NULL /* on_assignment_cb */, NULL /* destroy_state_cb */,
            rd_kafka_range_assignor_unittest, NULL);
}
