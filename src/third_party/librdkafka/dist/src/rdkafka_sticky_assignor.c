/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2020-2022, Magnus Edenhill
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
#include "rdkafka_request.h"
#include "rdmap.h"
#include "rdunittest.h"

#include <stdarg.h>
#include <math.h> /* abs() */

/**
 * @name KIP-54 and KIP-341 Sticky assignor.
 *
 * Closely mimicking the official Apache Kafka AbstractStickyAssignor
 * implementation.
 */

/** FIXME
 * Remaining:
 *  isSticky() -- used by tests
 */


/** @brief Assignor state from last rebalance */
typedef struct rd_kafka_sticky_assignor_state_s {
        rd_kafka_topic_partition_list_t *prev_assignment;
        int32_t generation_id;
} rd_kafka_sticky_assignor_state_t;



/**
 * Auxilliary glue types
 */

/**
 * @struct ConsumerPair_t represents a pair of consumer member ids involved in
 *         a partition reassignment, indicating a source consumer a partition
 *         is moving from and a destination partition the same partition is
 *         moving to.
 *
 * @sa PartitionMovements_t
 */
typedef struct ConsumerPair_s {
        const char *src; /**< Source member id */
        const char *dst; /**< Destination member id */
} ConsumerPair_t;


static ConsumerPair_t *ConsumerPair_new(const char *src, const char *dst) {
        ConsumerPair_t *cpair;

        cpair      = rd_malloc(sizeof(*cpair));
        cpair->src = src ? rd_strdup(src) : NULL;
        cpair->dst = dst ? rd_strdup(dst) : NULL;

        return cpair;
}


static void ConsumerPair_free(void *p) {
        ConsumerPair_t *cpair = p;
        if (cpair->src)
                rd_free((void *)cpair->src);
        if (cpair->dst)
                rd_free((void *)cpair->dst);
        rd_free(cpair);
}

static int ConsumerPair_cmp(const void *_a, const void *_b) {
        const ConsumerPair_t *a = _a, *b = _b;
        int r = strcmp(a->src ? a->src : "", b->src ? b->src : "");
        if (r)
                return r;
        return strcmp(a->dst ? a->dst : "", b->dst ? b->dst : "");
}


static unsigned int ConsumerPair_hash(const void *_a) {
        const ConsumerPair_t *a = _a;
        return 31 * (a->src ? rd_map_str_hash(a->src) : 1) +
               (a->dst ? rd_map_str_hash(a->dst) : 1);
}



typedef struct ConsumerGenerationPair_s {
        const char *consumer; /**< Memory owned by caller */
        int generation;
} ConsumerGenerationPair_t;

static void ConsumerGenerationPair_destroy(void *ptr) {
        ConsumerGenerationPair_t *cgpair = ptr;
        rd_free(cgpair);
}

/**
 * @param consumer This memory will be referenced, not copied, and thus must
 *                 outlive the ConsumerGenerationPair_t object.
 */
static ConsumerGenerationPair_t *
ConsumerGenerationPair_new(const char *consumer, int generation) {
        ConsumerGenerationPair_t *cgpair = rd_malloc(sizeof(*cgpair));
        cgpair->consumer                 = consumer;
        cgpair->generation               = generation;
        return cgpair;
}

static int ConsumerGenerationPair_cmp_generation(const void *_a,
                                                 const void *_b) {
        const ConsumerGenerationPair_t *a = _a, *b = _b;
        return a->generation - b->generation;
}



/**
 * Hash map types.
 *
 * Naming convention is:
 * map_<keytype>_<valuetype>_t
 *
 * Where the keytype and valuetype are spoken names of the types and
 * not the specific C types (since that'd be too long).
 */
typedef RD_MAP_TYPE(const char *,
                    rd_kafka_topic_partition_list_t *) map_str_toppar_list_t;

typedef RD_MAP_TYPE(const rd_kafka_topic_partition_t *,
                    const char *) map_toppar_str_t;

typedef RD_MAP_TYPE(const rd_kafka_topic_partition_t *,
                    rd_list_t *) map_toppar_list_t;

typedef RD_MAP_TYPE(const rd_kafka_topic_partition_t *,
                    rd_kafka_metadata_partition_internal_t *) map_toppar_mdpi_t;

typedef RD_MAP_TYPE(const rd_kafka_topic_partition_t *,
                    ConsumerGenerationPair_t *) map_toppar_cgpair_t;

typedef RD_MAP_TYPE(const rd_kafka_topic_partition_t *,
                    ConsumerPair_t *) map_toppar_cpair_t;

typedef RD_MAP_TYPE(const ConsumerPair_t *,
                    rd_kafka_topic_partition_list_t *) map_cpair_toppar_list_t;

/* map<string, map<ConsumerPair*, topic_partition_list_t*>> */
typedef RD_MAP_TYPE(const char *,
                    map_cpair_toppar_list_t *) map_str_map_cpair_toppar_list_t;

typedef RD_MAP_TYPE(const char *, const char *) map_str_str_t;


/** Glue type helpers */

static map_cpair_toppar_list_t *map_cpair_toppar_list_t_new(void) {
        map_cpair_toppar_list_t *map = rd_calloc(1, sizeof(*map));

        RD_MAP_INIT(map, 0, ConsumerPair_cmp, ConsumerPair_hash, NULL,
                    rd_kafka_topic_partition_list_destroy_free);

        return map;
}

static void map_cpair_toppar_list_t_free(void *ptr) {
        map_cpair_toppar_list_t *map = ptr;
        RD_MAP_DESTROY(map);
        rd_free(map);
}


/** @struct Convenience struct for storing consumer/rack and toppar/rack
 * mappings. */
typedef struct {
        /** A map of member_id -> rack_id pairs. */
        map_str_str_t member_id_to_rack_id;
        /* A map of topic partition to rd_kafka_metadata_partition_internal_t */
        map_toppar_mdpi_t toppar_to_mdpi;
} rd_kafka_rack_info_t;

/**
 * @brief Initialize a rd_kafka_rack_info_t.
 *
 * @param topics
 * @param topic_cnt
 * @param mdi
 *
 * This struct is for convenience/easy grouping, and as a consequence, we avoid
 * copying values. Thus, it is intended to be used within the lifetime of this
 * function's arguments.
 *
 * @return rd_kafka_rack_info_t*
 */
static rd_kafka_rack_info_t *
rd_kafka_rack_info_new(rd_kafka_assignor_topic_t **topics,
                       size_t topic_cnt,
                       const rd_kafka_metadata_internal_t *mdi) {
        int i;
        size_t t;
        rd_kafka_group_member_t *rkgm;
        rd_kafka_rack_info_t *rkri = rd_calloc(1, sizeof(rd_kafka_rack_info_t));

        if (!rd_kafka_use_rack_aware_assignment(topics, topic_cnt, mdi)) {
                /* Free everything immediately, we aren't using rack aware
                assignment, this struct is not applicable. */
                rd_free(rkri);
                return NULL;
        }

        rkri->member_id_to_rack_id = (map_str_str_t)RD_MAP_INITIALIZER(
            0, rd_map_str_cmp, rd_map_str_hash,
            NULL /* refs members.rkgm_member_id */,
            NULL /* refs members.rkgm_rack_id */);
        rkri->toppar_to_mdpi = (map_toppar_mdpi_t)RD_MAP_INITIALIZER(
            0, rd_kafka_topic_partition_cmp, rd_kafka_topic_partition_hash,
            rd_kafka_topic_partition_destroy_free, NULL);

        for (t = 0; t < topic_cnt; t++) {
                RD_LIST_FOREACH(rkgm, &topics[t]->members, i) {
                        RD_MAP_SET(&rkri->member_id_to_rack_id,
                                   rkgm->rkgm_member_id->str,
                                   rkgm->rkgm_rack_id->str);
                }

                for (i = 0; i < topics[t]->metadata->partition_cnt; i++) {
                        rd_kafka_topic_partition_t *rkpart =
                            rd_kafka_topic_partition_new(
                                topics[t]->metadata->topic, i);
                        RD_MAP_SET(
                            &rkri->toppar_to_mdpi, rkpart,
                            &topics[t]->metadata_internal->partitions[i]);
                }
        }

        return rkri;
}

/* Destroy a rd_kafka_rack_info_t. */
static void rd_kafka_rack_info_destroy(rd_kafka_rack_info_t *rkri) {
        if (!rkri)
                return;

        RD_MAP_DESTROY(&rkri->member_id_to_rack_id);
        RD_MAP_DESTROY(&rkri->toppar_to_mdpi);

        rd_free(rkri);
}


/* Convenience function to bsearch inside the racks of a
 * rd_kafka_metadata_partition_internal_t. */
static char *rd_kafka_partition_internal_find_rack(
    rd_kafka_metadata_partition_internal_t *mdpi,
    const char *rack) {
        char **partition_racks = mdpi->racks;
        size_t cnt             = mdpi->racks_cnt;

        void *res =
            bsearch(&rack, partition_racks, cnt, sizeof(char *), rd_strcmp3);

        if (res)
                return *(char **)res;
        return NULL;
}


/* Computes whether there is a rack mismatch between the rack of the consumer
 * and the topic partition/any of its replicas. */
static rd_bool_t
rd_kafka_racks_mismatch(rd_kafka_rack_info_t *rkri,
                        const char *consumer,
                        const rd_kafka_topic_partition_t *topic_partition) {
        const char *consumer_rack;
        rd_kafka_metadata_partition_internal_t *mdpi;

        if (rkri == NULL) /* Not using rack aware assignment */
                return rd_false;

        consumer_rack = RD_MAP_GET(&rkri->member_id_to_rack_id, consumer);

        mdpi = RD_MAP_GET(&rkri->toppar_to_mdpi, topic_partition);

        return consumer_rack != NULL &&
               (mdpi == NULL ||
                !rd_kafka_partition_internal_find_rack(mdpi, consumer_rack));
}

/**
 * @struct Provides current state of partition movements between consumers
 *         for each topic, and possible movements for each partition.
 */
typedef struct PartitionMovements_s {
        map_toppar_cpair_t partitionMovements;
        map_str_map_cpair_toppar_list_t partitionMovementsByTopic;
} PartitionMovements_t;


static void PartitionMovements_init(PartitionMovements_t *pmov,
                                    size_t topic_cnt) {
        RD_MAP_INIT(&pmov->partitionMovements, topic_cnt * 3,
                    rd_kafka_topic_partition_cmp, rd_kafka_topic_partition_hash,
                    NULL, ConsumerPair_free);

        RD_MAP_INIT(&pmov->partitionMovementsByTopic, topic_cnt, rd_map_str_cmp,
                    rd_map_str_hash, NULL, map_cpair_toppar_list_t_free);
}

static void PartitionMovements_destroy(PartitionMovements_t *pmov) {
        RD_MAP_DESTROY(&pmov->partitionMovementsByTopic);
        RD_MAP_DESTROY(&pmov->partitionMovements);
}


static ConsumerPair_t *PartitionMovements_removeMovementRecordOfPartition(
    PartitionMovements_t *pmov,
    const rd_kafka_topic_partition_t *toppar) {

        ConsumerPair_t *cpair;
        map_cpair_toppar_list_t *partitionMovementsForThisTopic;
        rd_kafka_topic_partition_list_t *plist;

        cpair = RD_MAP_GET(&pmov->partitionMovements, toppar);
        rd_assert(cpair);

        partitionMovementsForThisTopic =
            RD_MAP_GET(&pmov->partitionMovementsByTopic, toppar->topic);

        plist = RD_MAP_GET(partitionMovementsForThisTopic, cpair);
        rd_assert(plist);

        rd_kafka_topic_partition_list_del(plist, toppar->topic,
                                          toppar->partition);
        if (plist->cnt == 0)
                RD_MAP_DELETE(partitionMovementsForThisTopic, cpair);
        if (RD_MAP_IS_EMPTY(partitionMovementsForThisTopic))
                RD_MAP_DELETE(&pmov->partitionMovementsByTopic, toppar->topic);

        return cpair;
}

static void PartitionMovements_addPartitionMovementRecord(
    PartitionMovements_t *pmov,
    const rd_kafka_topic_partition_t *toppar,
    ConsumerPair_t *cpair) {
        map_cpair_toppar_list_t *partitionMovementsForThisTopic;
        rd_kafka_topic_partition_list_t *plist;

        RD_MAP_SET(&pmov->partitionMovements, toppar, cpair);

        partitionMovementsForThisTopic =
            RD_MAP_GET_OR_SET(&pmov->partitionMovementsByTopic, toppar->topic,
                              map_cpair_toppar_list_t_new());

        plist = RD_MAP_GET_OR_SET(partitionMovementsForThisTopic, cpair,
                                  rd_kafka_topic_partition_list_new(16));

        rd_kafka_topic_partition_list_add(plist, toppar->topic,
                                          toppar->partition);
}

static void
PartitionMovements_movePartition(PartitionMovements_t *pmov,
                                 const rd_kafka_topic_partition_t *toppar,
                                 const char *old_consumer,
                                 const char *new_consumer) {

        if (RD_MAP_GET(&pmov->partitionMovements, toppar)) {
                /* This partition has previously moved */
                ConsumerPair_t *existing_cpair;

                existing_cpair =
                    PartitionMovements_removeMovementRecordOfPartition(pmov,
                                                                       toppar);

                rd_assert(!rd_strcmp(existing_cpair->dst, old_consumer));

                if (rd_strcmp(existing_cpair->src, new_consumer)) {
                        /* Partition is not moving back to its
                         * previous consumer */
                        PartitionMovements_addPartitionMovementRecord(
                            pmov, toppar,
                            ConsumerPair_new(existing_cpair->src,
                                             new_consumer));
                }
        } else {
                PartitionMovements_addPartitionMovementRecord(
                    pmov, toppar, ConsumerPair_new(old_consumer, new_consumer));
        }
}

static const rd_kafka_topic_partition_t *
PartitionMovements_getTheActualPartitionToBeMoved(
    PartitionMovements_t *pmov,
    const rd_kafka_topic_partition_t *toppar,
    const char *oldConsumer,
    const char *newConsumer) {

        ConsumerPair_t *cpair;
        ConsumerPair_t reverse_cpair = {.src = newConsumer, .dst = oldConsumer};
        map_cpair_toppar_list_t *partitionMovementsForThisTopic;
        rd_kafka_topic_partition_list_t *plist;

        if (!RD_MAP_GET(&pmov->partitionMovementsByTopic, toppar->topic))
                return toppar;

        cpair = RD_MAP_GET(&pmov->partitionMovements, toppar);
        if (cpair) {
                /* This partition has previously moved */
                rd_assert(!rd_strcmp(oldConsumer, cpair->dst));

                oldConsumer = cpair->src;
        }

        partitionMovementsForThisTopic =
            RD_MAP_GET(&pmov->partitionMovementsByTopic, toppar->topic);

        plist = RD_MAP_GET(partitionMovementsForThisTopic, &reverse_cpair);
        if (!plist)
                return toppar;

        return &plist->elems[0];
}

#if FIXME

static rd_bool_t hasCycles(map_cpair_toppar_list_t *pairs) {
        return rd_true;  // FIXME
}

/**
 * @remark This method is only used by the AbstractStickyAssignorTest
 *         in the Java client.
 */
static rd_bool_t PartitionMovements_isSticky(rd_kafka_t *rk,
                                             PartitionMovements_t *pmov) {
        const char *topic;
        map_cpair_toppar_list_t *topicMovementPairs;

        RD_MAP_FOREACH(topic, topicMovementPairs,
                       &pmov->partitionMovementsByTopic) {
                if (hasCycles(topicMovementPairs)) {
                        const ConsumerPair_t *cpair;
                        const rd_kafka_topic_partition_list_t *partitions;

                        rd_kafka_log(
                            rk, LOG_ERR, "STICKY",
                            "Sticky assignor: Stickiness is violated for "
                            "topic %s: partition movements for this topic "
                            "occurred among the following consumers: ",
                            topic);
                        RD_MAP_FOREACH(cpair, partitions, topicMovementPairs) {
                                rd_kafka_log(rk, LOG_ERR, "STICKY", " %s -> %s",
                                             cpair->src, cpair->dst);
                        }

                        if (partitions)
                                ; /* Avoid unused warning */

                        return rd_false;
                }
        }

        return rd_true;
}
#endif


/**
 * @brief Comparator to sort ascendingly by rd_map_elem_t object value as
 *        topic partition list count, or by member id if the list count is
 *        identical.
 *        Used to sort sortedCurrentSubscriptions list.
 *
 * elem.key is the consumer member id string,
 * elem.value is the partition list.
 */
static int sort_by_map_elem_val_toppar_list_cnt(const void *_a,
                                                const void *_b) {
        const rd_map_elem_t *a = _a, *b = _b;
        const rd_kafka_topic_partition_list_t *al = a->value, *bl = b->value;
        int r = al->cnt - bl->cnt;
        if (r)
                return r;
        return strcmp((const char *)a->key, (const char *)b->key);
}


/**
 * @brief Assign partition to the most eligible consumer.
 *
 * The assignment should improve the overall balance of the partition
 * assignments to consumers.
 * @returns true if partition was assigned, false otherwise.
 */
static rd_bool_t
maybeAssignPartition(const rd_kafka_topic_partition_t *partition,
                     rd_list_t *sortedCurrentSubscriptions /*rd_map_elem_t*/,
                     map_str_toppar_list_t *currentAssignment,
                     map_str_toppar_list_t *consumer2AllPotentialPartitions,
                     map_toppar_str_t *currentPartitionConsumer,
                     rd_kafka_rack_info_t *rkri) {
        const rd_map_elem_t *elem;
        int i;

        RD_LIST_FOREACH(elem, sortedCurrentSubscriptions, i) {
                const char *consumer = (const char *)elem->key;
                const rd_kafka_topic_partition_list_t *partitions;

                partitions =
                    RD_MAP_GET(consumer2AllPotentialPartitions, consumer);
                if (!rd_kafka_topic_partition_list_find(
                        partitions, partition->topic, partition->partition))
                        continue;
                if (rkri != NULL &&
                    rd_kafka_racks_mismatch(rkri, consumer, partition))
                        continue;

                rd_kafka_topic_partition_list_add(
                    RD_MAP_GET(currentAssignment, consumer), partition->topic,
                    partition->partition);

                RD_MAP_SET(currentPartitionConsumer,
                           rd_kafka_topic_partition_copy(partition), consumer);

                /* Re-sort sortedCurrentSubscriptions since this consumer's
                 * assignment count has increased.
                 * This is an O(N) operation since it is a single shuffle. */
                rd_list_sort(sortedCurrentSubscriptions,
                             sort_by_map_elem_val_toppar_list_cnt);
                return rd_true;
        }
        return rd_false;
}

/**
 * @returns true if the partition has two or more potential consumers.
 */
static RD_INLINE rd_bool_t partitionCanParticipateInReassignment(
    const rd_kafka_topic_partition_t *partition,
    map_toppar_list_t *partition2AllPotentialConsumers) {
        rd_list_t *consumers;

        if (!(consumers =
                  RD_MAP_GET(partition2AllPotentialConsumers, partition)))
                return rd_false;

        return rd_list_cnt(consumers) >= 2;
}


/**
 * @returns true if consumer can participate in reassignment based on
 *          its current assignment.
 */
static RD_INLINE rd_bool_t consumerCanParticipateInReassignment(
    rd_kafka_t *rk,
    const char *consumer,
    map_str_toppar_list_t *currentAssignment,
    map_str_toppar_list_t *consumer2AllPotentialPartitions,
    map_toppar_list_t *partition2AllPotentialConsumers) {
        const rd_kafka_topic_partition_list_t *currentPartitions =
            RD_MAP_GET(currentAssignment, consumer);
        int currentAssignmentSize = currentPartitions->cnt;
        int maxAssignmentSize =
            RD_MAP_GET(consumer2AllPotentialPartitions, consumer)->cnt;
        int i;

        /* FIXME: And then what? Is this a local error? If so, assert. */
        if (currentAssignmentSize > maxAssignmentSize)
                rd_kafka_log(rk, LOG_ERR, "STICKY",
                             "Sticky assignor error: "
                             "Consumer %s is assigned more partitions (%d) "
                             "than the maximum possible (%d)",
                             consumer, currentAssignmentSize,
                             maxAssignmentSize);

        /* If a consumer is not assigned all its potential partitions it is
         * subject to reassignment. */
        if (currentAssignmentSize < maxAssignmentSize)
                return rd_true;

        /* If any of the partitions assigned to a consumer is subject to
         * reassignment the consumer itself is subject to reassignment. */
        for (i = 0; i < currentPartitions->cnt; i++) {
                const rd_kafka_topic_partition_t *partition =
                    &currentPartitions->elems[i];

                if (partitionCanParticipateInReassignment(
                        partition, partition2AllPotentialConsumers))
                        return rd_true;
        }

        return rd_false;
}


/**
 * @brief Process moving partition from old consumer to new consumer.
 */
static void processPartitionMovement(
    rd_kafka_t *rk,
    PartitionMovements_t *partitionMovements,
    const rd_kafka_topic_partition_t *partition,
    const char *newConsumer,
    map_str_toppar_list_t *currentAssignment,
    rd_list_t *sortedCurrentSubscriptions /*rd_map_elem_t*/,
    map_toppar_str_t *currentPartitionConsumer) {

        const char *oldConsumer =
            RD_MAP_GET(currentPartitionConsumer, partition);

        PartitionMovements_movePartition(partitionMovements, partition,
                                         oldConsumer, newConsumer);

        rd_kafka_topic_partition_list_add(
            RD_MAP_GET(currentAssignment, newConsumer), partition->topic,
            partition->partition);

        rd_kafka_topic_partition_list_del(
            RD_MAP_GET(currentAssignment, oldConsumer), partition->topic,
            partition->partition);

        RD_MAP_SET(currentPartitionConsumer,
                   rd_kafka_topic_partition_copy(partition), newConsumer);

        /* Re-sort after assignment count has changed. */
        rd_list_sort(sortedCurrentSubscriptions,
                     sort_by_map_elem_val_toppar_list_cnt);

        rd_kafka_dbg(rk, ASSIGNOR, "STICKY",
                     "%s [%" PRId32 "] %sassigned to %s (from %s)",
                     partition->topic, partition->partition,
                     oldConsumer ? "re" : "", newConsumer,
                     oldConsumer ? oldConsumer : "(none)");
}


/**
 * @brief Reassign \p partition to \p newConsumer
 */
static void reassignPartitionToConsumer(
    rd_kafka_t *rk,
    PartitionMovements_t *partitionMovements,
    const rd_kafka_topic_partition_t *partition,
    map_str_toppar_list_t *currentAssignment,
    rd_list_t *sortedCurrentSubscriptions /*rd_map_elem_t*/,
    map_toppar_str_t *currentPartitionConsumer,
    const char *newConsumer) {

        const char *consumer = RD_MAP_GET(currentPartitionConsumer, partition);
        const rd_kafka_topic_partition_t *partitionToBeMoved;

        /* Find the correct partition movement considering
         * the stickiness requirement. */
        partitionToBeMoved = PartitionMovements_getTheActualPartitionToBeMoved(
            partitionMovements, partition, consumer, newConsumer);

        processPartitionMovement(rk, partitionMovements, partitionToBeMoved,
                                 newConsumer, currentAssignment,
                                 sortedCurrentSubscriptions,
                                 currentPartitionConsumer);
}

/**
 * @brief Reassign \p partition to an eligible new consumer.
 */
static void
reassignPartition(rd_kafka_t *rk,
                  PartitionMovements_t *partitionMovements,
                  const rd_kafka_topic_partition_t *partition,
                  map_str_toppar_list_t *currentAssignment,
                  rd_list_t *sortedCurrentSubscriptions /*rd_map_elem_t*/,
                  map_toppar_str_t *currentPartitionConsumer,
                  map_str_toppar_list_t *consumer2AllPotentialPartitions) {

        const rd_map_elem_t *elem;
        int i;

        /* Find the new consumer */
        RD_LIST_FOREACH(elem, sortedCurrentSubscriptions, i) {
                const char *newConsumer = (const char *)elem->key;

                if (rd_kafka_topic_partition_list_find(
                        RD_MAP_GET(consumer2AllPotentialPartitions,
                                   newConsumer),
                        partition->topic, partition->partition)) {
                        reassignPartitionToConsumer(
                            rk, partitionMovements, partition,
                            currentAssignment, sortedCurrentSubscriptions,
                            currentPartitionConsumer, newConsumer);

                        return;
                }
        }

        rd_assert(!*"reassignPartition(): no new consumer found");
}



/**
 * @brief Determine if the current assignment is balanced.
 *
 * @param currentAssignment the assignment whose balance needs to be checked
 * @param sortedCurrentSubscriptions an ascending sorted set of consumers based
 *                                   on how many topic partitions are already
 *                                   assigned to them
 * @param consumer2AllPotentialPartitions a mapping of all consumers to all
 *                                        potential topic partitions that can be
 *                                        assigned to them.
 *                                        This parameter is called
 *                                        allSubscriptions in the Java
 *                                        implementation, but we choose this
 *                                        name to be more consistent with its
 *                                        use elsewhere in the code.
 * @param partition2AllPotentialConsumers a mapping of all partitions to
 *                                        all potential consumers.
 *
 * @returns true if the given assignment is balanced; false otherwise
 */
static rd_bool_t
isBalanced(rd_kafka_t *rk,
           map_str_toppar_list_t *currentAssignment,
           const rd_list_t *sortedCurrentSubscriptions /*rd_map_elem_t*/,
           map_str_toppar_list_t *consumer2AllPotentialPartitions,
           map_toppar_list_t *partition2AllPotentialConsumers) {

        int minimum = ((const rd_kafka_topic_partition_list_t
                            *)((const rd_map_elem_t *)rd_list_first(
                                   sortedCurrentSubscriptions))
                           ->value)
                          ->cnt;
        int maximum = ((const rd_kafka_topic_partition_list_t
                            *)((const rd_map_elem_t *)rd_list_last(
                                   sortedCurrentSubscriptions))
                           ->value)
                          ->cnt;

        /* Iterators */
        const rd_kafka_topic_partition_list_t *partitions;
        const char *consumer;
        const rd_map_elem_t *elem;
        int i;

        /* The assignment is balanced if minimum and maximum numbers of
         * partitions assigned to consumers differ by at most one. */
        if (minimum >= maximum - 1) {
                rd_kafka_dbg(rk, ASSIGNOR, "STICKY",
                             "Assignment is balanced: "
                             "minimum %d and maximum %d partitions assigned "
                             "to each consumer",
                             minimum, maximum);
                return rd_true;
        }

        /* Mapping from partitions to the consumer assigned to them */
        map_toppar_str_t allPartitions = RD_MAP_INITIALIZER(
            RD_MAP_CNT(partition2AllPotentialConsumers),
            rd_kafka_topic_partition_cmp, rd_kafka_topic_partition_hash,
            NULL /* references currentAssignment */,
            NULL /* references currentAssignment */);

        /* Create a mapping from partitions to the consumer assigned to them */
        RD_MAP_FOREACH(consumer, partitions, currentAssignment) {

                for (i = 0; i < partitions->cnt; i++) {
                        const rd_kafka_topic_partition_t *partition =
                            &partitions->elems[i];
                        const char *existing;
                        if ((existing = RD_MAP_GET(&allPartitions, partition)))
                                rd_kafka_log(rk, LOG_ERR, "STICKY",
                                             "Sticky assignor: %s [%" PRId32
                                             "] "
                                             "is assigned to more than one "
                                             "consumer (%s and %s)",
                                             partition->topic,
                                             partition->partition, existing,
                                             consumer);

                        RD_MAP_SET(&allPartitions, partition, consumer);
                }
        }


        /* For each consumer that does not have all the topic partitions it
         * can get make sure none of the topic partitions it could but did
         * not get cannot be moved to it, because that would break the balance.
         *
         * Note: Since sortedCurrentSubscriptions elements are pointers to
         *       currentAssignment's element we get both the consumer
         *       and partition list in elem here. */
        RD_LIST_FOREACH(elem, sortedCurrentSubscriptions, i) {
                int j;
                const char *consumer = (const char *)elem->key;
                const rd_kafka_topic_partition_list_t *potentialTopicPartitions;
                const rd_kafka_topic_partition_list_t *consumerPartitions;

                consumerPartitions =
                    (const rd_kafka_topic_partition_list_t *)elem->value;

                potentialTopicPartitions =
                    RD_MAP_GET(consumer2AllPotentialPartitions, consumer);

                /* Skip if this consumer already has all the topic partitions
                 * it can get. */
                if (consumerPartitions->cnt == potentialTopicPartitions->cnt)
                        continue;

                /* Otherwise make sure it can't get any more partitions */

                for (j = 0; j < potentialTopicPartitions->cnt; j++) {
                        const rd_kafka_topic_partition_t *partition =
                            &potentialTopicPartitions->elems[j];
                        const char *otherConsumer;
                        int otherConsumerPartitionCount;

                        if (rd_kafka_topic_partition_list_find(
                                consumerPartitions, partition->topic,
                                partition->partition))
                                continue;

                        otherConsumer = RD_MAP_GET(&allPartitions, partition);
                        otherConsumerPartitionCount =
                            RD_MAP_GET(currentAssignment, otherConsumer)->cnt;

                        if (consumerPartitions->cnt <
                            otherConsumerPartitionCount) {
                                rd_kafka_dbg(
                                    rk, ASSIGNOR, "STICKY",
                                    "%s [%" PRId32
                                    "] can be moved from "
                                    "consumer %s (%d partition(s)) to "
                                    "consumer %s (%d partition(s)) "
                                    "for a more balanced assignment",
                                    partition->topic, partition->partition,
                                    otherConsumer, otherConsumerPartitionCount,
                                    consumer, consumerPartitions->cnt);
                                RD_MAP_DESTROY(&allPartitions);
                                return rd_false;
                        }
                }
        }

        RD_MAP_DESTROY(&allPartitions);
        return rd_true;
}


/**
 * @brief Perform reassignment.
 *
 * @returns true if reassignment was performed.
 */
static rd_bool_t
performReassignments(rd_kafka_t *rk,
                     PartitionMovements_t *partitionMovements,
                     rd_kafka_topic_partition_list_t *reassignablePartitions,
                     map_str_toppar_list_t *currentAssignment,
                     map_toppar_cgpair_t *prevAssignment,
                     rd_list_t *sortedCurrentSubscriptions /*rd_map_elem_t*/,
                     map_str_toppar_list_t *consumer2AllPotentialPartitions,
                     map_toppar_list_t *partition2AllPotentialConsumers,
                     map_toppar_str_t *currentPartitionConsumer,
                     rd_kafka_rack_info_t *rkri) {
        rd_bool_t reassignmentPerformed = rd_false;
        rd_bool_t modified, saveIsBalanced = rd_false;
        int iterations = 0;

        /* Repeat reassignment until no partition can be moved to
         * improve the balance. */
        do {
                int i;

                iterations++;

                modified = rd_false;

                /* Reassign all reassignable partitions (starting from the
                 * partition with least potential consumers and if needed)
                 * until the full list is processed or a balance is achieved. */

                for (i = 0; i < reassignablePartitions->cnt &&
                            !isBalanced(rk, currentAssignment,
                                        sortedCurrentSubscriptions,
                                        consumer2AllPotentialPartitions,
                                        partition2AllPotentialConsumers);
                     i++) {
                        const rd_kafka_topic_partition_t *partition =
                            &reassignablePartitions->elems[i];
                        const rd_list_t *consumers = RD_MAP_GET(
                            partition2AllPotentialConsumers, partition);
                        const char *consumer, *otherConsumer;
                        const ConsumerGenerationPair_t *prevcgp;
                        const rd_kafka_topic_partition_list_t *currAssignment;
                        int j;
                        rd_bool_t found_rack;
                        const char *consumer_rack                    = NULL;
                        rd_kafka_metadata_partition_internal_t *mdpi = NULL;

                        /* FIXME: Is this a local error/bug? If so, assert */
                        if (rd_list_cnt(consumers) <= 1)
                                rd_kafka_log(
                                    rk, LOG_ERR, "STICKY",
                                    "Sticky assignor: expected more than "
                                    "one potential consumer for partition "
                                    "%s [%" PRId32 "]",
                                    partition->topic, partition->partition);

                        /* The partition must have a current consumer */
                        consumer =
                            RD_MAP_GET(currentPartitionConsumer, partition);
                        rd_assert(consumer);

                        currAssignment =
                            RD_MAP_GET(currentAssignment, consumer);
                        prevcgp = RD_MAP_GET(prevAssignment, partition);

                        if (prevcgp &&
                            currAssignment->cnt >
                                RD_MAP_GET(currentAssignment, prevcgp->consumer)
                                        ->cnt +
                                    1) {
                                reassignPartitionToConsumer(
                                    rk, partitionMovements, partition,
                                    currentAssignment,
                                    sortedCurrentSubscriptions,
                                    currentPartitionConsumer,
                                    prevcgp->consumer);
                                reassignmentPerformed = rd_true;
                                modified              = rd_true;
                                continue;
                        }

                        /* Check if a better-suited consumer exists for the
                         * partition; if so, reassign it. Use consumer within
                         * rack if possible. */
                        if (rkri) {
                                consumer_rack = RD_MAP_GET(
                                    &rkri->member_id_to_rack_id, consumer);
                                mdpi = RD_MAP_GET(&rkri->toppar_to_mdpi,
                                                  partition);
                        }
                        found_rack = rd_false;

                        if (consumer_rack != NULL && mdpi != NULL &&
                            mdpi->racks_cnt > 0 &&
                            rd_kafka_partition_internal_find_rack(
                                mdpi, consumer_rack)) {
                                RD_LIST_FOREACH(otherConsumer, consumers, j) {
                                        /* No need for rkri == NULL check, that
                                         * is guaranteed if we're inside this if
                                         * block. */
                                        const char *other_consumer_rack =
                                            RD_MAP_GET(
                                                &rkri->member_id_to_rack_id,
                                                otherConsumer);

                                        if (other_consumer_rack == NULL ||
                                            !rd_kafka_partition_internal_find_rack(
                                                mdpi, other_consumer_rack))
                                                continue;

                                        if (currAssignment->cnt <=
                                            RD_MAP_GET(currentAssignment,
                                                       otherConsumer)
                                                    ->cnt +
                                                1)
                                                continue;

                                        reassignPartition(
                                            rk, partitionMovements, partition,
                                            currentAssignment,
                                            sortedCurrentSubscriptions,
                                            currentPartitionConsumer,
                                            consumer2AllPotentialPartitions);

                                        reassignmentPerformed = rd_true;
                                        modified              = rd_true;
                                        found_rack            = rd_true;
                                        break;
                                }
                        }

                        if (found_rack) {
                                continue;
                        }

                        RD_LIST_FOREACH(otherConsumer, consumers, j) {
                                if (consumer == otherConsumer)
                                        continue;

                                if (currAssignment->cnt <=
                                    RD_MAP_GET(currentAssignment, otherConsumer)
                                            ->cnt +
                                        1)
                                        continue;

                                reassignPartition(
                                    rk, partitionMovements, partition,
                                    currentAssignment,
                                    sortedCurrentSubscriptions,
                                    currentPartitionConsumer,
                                    consumer2AllPotentialPartitions);

                                reassignmentPerformed = rd_true;
                                modified              = rd_true;
                                break;
                        }
                }

                if (i < reassignablePartitions->cnt)
                        saveIsBalanced = rd_true;

        } while (modified);

        rd_kafka_dbg(rk, ASSIGNOR, "STICKY",
                     "Reassignment %sperformed after %d iteration(s) of %d "
                     "reassignable partition(s)%s",
                     reassignmentPerformed ? "" : "not ", iterations,
                     reassignablePartitions->cnt,
                     saveIsBalanced ? ": assignment is balanced" : "");

        return reassignmentPerformed;
}


/**
 * @returns the balance score of the given assignment, as the sum of assigned
 *           partitions size difference of all consumer pairs.
 *
 * A perfectly balanced assignment (with all consumers getting the same number
 * of partitions) has a balance score of 0.
 *
 * Lower balance score indicates a more balanced assignment.
 * FIXME: should be called imbalance score then?
 */
static int getBalanceScore(map_str_toppar_list_t *assignment) {
        const char *consumer;
        const rd_kafka_topic_partition_list_t *partitions;
        int *sizes;
        int cnt   = 0;
        int score = 0;
        int i, next;

        /* If there is just a single consumer the assignment will be balanced */
        if (RD_MAP_CNT(assignment) < 2)
                return 0;

        sizes = rd_malloc(sizeof(*sizes) * RD_MAP_CNT(assignment));

        RD_MAP_FOREACH(consumer, partitions, assignment)
        sizes[cnt++] = partitions->cnt;

        for (next = 0; next < cnt; next++)
                for (i = next + 1; i < cnt; i++)
                        score += abs(sizes[next] - sizes[i]);

        rd_free(sizes);

        if (consumer)
                ; /* Avoid unused warning */

        return score;
}

static void maybeAssign(rd_kafka_topic_partition_list_t *unassignedPartitions,
                        map_toppar_list_t *partition2AllPotentialConsumers,
                        rd_list_t *sortedCurrentSubscriptions /*rd_map_elem_t*/,
                        map_str_toppar_list_t *currentAssignment,
                        map_str_toppar_list_t *consumer2AllPotentialPartitions,
                        map_toppar_str_t *currentPartitionConsumer,
                        rd_bool_t removeAssigned,
                        rd_kafka_rack_info_t *rkri) {
        int i;
        const rd_kafka_topic_partition_t *partition;

        for (i = 0; i < unassignedPartitions->cnt; i++) {
                partition = &unassignedPartitions->elems[i];
                rd_bool_t assigned;

                /* Skip if there is no potential consumer for the partition.
                 * FIXME: How could this be? */
                if (rd_list_empty(RD_MAP_GET(partition2AllPotentialConsumers,
                                             partition))) {
                        rd_dassert(!*"sticky assignor bug");
                        continue;
                }

                assigned = maybeAssignPartition(
                    partition, sortedCurrentSubscriptions, currentAssignment,
                    consumer2AllPotentialPartitions, currentPartitionConsumer,
                    rkri);
                if (assigned && removeAssigned) {
                        rd_kafka_topic_partition_list_del_by_idx(
                            unassignedPartitions, i);
                        i--; /* Since the current element was
                              * removed we need the next for
                              * loop iteration to stay at the
                              * same index. */
                }
        }
}

/**
 * @brief Balance the current assignment using the data structures
 *        created in assign_cb(). */
static void balance(rd_kafka_t *rk,
                    PartitionMovements_t *partitionMovements,
                    map_str_toppar_list_t *currentAssignment,
                    map_toppar_cgpair_t *prevAssignment,
                    rd_kafka_topic_partition_list_t *sortedPartitions,
                    rd_kafka_topic_partition_list_t *unassignedPartitions,
                    rd_list_t *sortedCurrentSubscriptions /*rd_map_elem_t*/,
                    map_str_toppar_list_t *consumer2AllPotentialPartitions,
                    map_toppar_list_t *partition2AllPotentialConsumers,
                    map_toppar_str_t *currentPartitionConsumer,
                    rd_bool_t revocationRequired,
                    rd_kafka_rack_info_t *rkri) {

        /* If the consumer with most assignments (thus the last element
         * in the ascendingly ordered sortedCurrentSubscriptions list) has
         * zero partitions assigned it means there is no current assignment
         * for any consumer and the group is thus initializing for the first
         * time. */
        rd_bool_t initializing = ((const rd_kafka_topic_partition_list_t
                                       *)((const rd_map_elem_t *)rd_list_last(
                                              sortedCurrentSubscriptions))
                                      ->value)
                                     ->cnt == 0;
        rd_bool_t reassignmentPerformed = rd_false;

        map_str_toppar_list_t fixedAssignments =
                RD_MAP_INITIALIZER(RD_MAP_CNT(partition2AllPotentialConsumers),
                                   rd_map_str_cmp,
                                   rd_map_str_hash,
                                   NULL,
                                   NULL /* Will transfer ownership of the list
                                         * to currentAssignment at the end of
                                         * this function. */);

        map_str_toppar_list_t preBalanceAssignment = RD_MAP_INITIALIZER(
            RD_MAP_CNT(currentAssignment), rd_map_str_cmp, rd_map_str_hash,
            NULL /* references currentAssignment */,
            rd_kafka_topic_partition_list_destroy_free);
        map_toppar_str_t preBalancePartitionConsumers = RD_MAP_INITIALIZER(
            RD_MAP_CNT(partition2AllPotentialConsumers),
            rd_kafka_topic_partition_cmp, rd_kafka_topic_partition_hash,
            rd_kafka_topic_partition_destroy_free,
            NULL /* refs currentPartitionConsumer */);
        int newScore, oldScore;
        /* Iterator variables */
        const rd_kafka_topic_partition_t *partition;
        const void *ignore;
        const rd_map_elem_t *elem;
        int i;
        rd_kafka_topic_partition_list_t *leftoverUnassignedPartitions;
        rd_bool_t leftoverUnassignedPartitions_allocated = rd_false;

        leftoverUnassignedPartitions =
            unassignedPartitions; /* copy on write. */

        if (rkri != NULL && RD_MAP_CNT(&rkri->member_id_to_rack_id) != 0) {
                leftoverUnassignedPartitions_allocated = rd_true;
                /* Since maybeAssign is called twice, we keep track of those
                 * partitions which the first call has taken care of already,
                 * but we don't want to modify the original
                 * unassignedPartitions. */
                leftoverUnassignedPartitions =
                    rd_kafka_topic_partition_list_copy(unassignedPartitions);
                maybeAssign(leftoverUnassignedPartitions,
                            partition2AllPotentialConsumers,
                            sortedCurrentSubscriptions, currentAssignment,
                            consumer2AllPotentialPartitions,
                            currentPartitionConsumer, rd_true, rkri);
        }
        maybeAssign(leftoverUnassignedPartitions,
                    partition2AllPotentialConsumers, sortedCurrentSubscriptions,
                    currentAssignment, consumer2AllPotentialPartitions,
                    currentPartitionConsumer, rd_false, NULL);

        if (leftoverUnassignedPartitions_allocated)
                rd_kafka_topic_partition_list_destroy(
                    leftoverUnassignedPartitions);


        /* Narrow down the reassignment scope to only those partitions that can
         * actually be reassigned. */
        RD_MAP_FOREACH(partition, ignore, partition2AllPotentialConsumers) {
                if (partitionCanParticipateInReassignment(
                        partition, partition2AllPotentialConsumers))
                        continue;

                rd_kafka_topic_partition_list_del(
                    sortedPartitions, partition->topic, partition->partition);
                rd_kafka_topic_partition_list_del(unassignedPartitions,
                                                  partition->topic,
                                                  partition->partition);
        }

        if (ignore)
                ; /* Avoid unused warning */


        /* Narrow down the reassignment scope to only those consumers that are
         * subject to reassignment. */
        RD_LIST_FOREACH(elem, sortedCurrentSubscriptions, i) {
                const char *consumer = (const char *)elem->key;
                rd_kafka_topic_partition_list_t *partitions;

                if (consumerCanParticipateInReassignment(
                        rk, consumer, currentAssignment,
                        consumer2AllPotentialPartitions,
                        partition2AllPotentialConsumers))
                        continue;

                rd_list_remove_elem(sortedCurrentSubscriptions, i);
                i--; /* Since the current element is removed we need
                      * to rewind the iterator. */

                partitions = rd_kafka_topic_partition_list_copy(
                    RD_MAP_GET(currentAssignment, consumer));
                RD_MAP_DELETE(currentAssignment, consumer);

                RD_MAP_SET(&fixedAssignments, consumer, partitions);
        }


        rd_kafka_dbg(rk, ASSIGNOR, "STICKY",
                     "Prepared balanced reassignment for %d consumers, "
                     "%d available partition(s) where of %d are unassigned "
                     "(initializing=%s, revocationRequired=%s, "
                     "%d fixed assignments)",
                     (int)RD_MAP_CNT(consumer2AllPotentialPartitions),
                     sortedPartitions->cnt, unassignedPartitions->cnt,
                     initializing ? "true" : "false",
                     revocationRequired ? "true" : "false",
                     (int)RD_MAP_CNT(&fixedAssignments));

        /* Create a deep copy of the current assignment so we can revert to it
         * if we do not get a more balanced assignment later. */
        RD_MAP_COPY(&preBalanceAssignment, currentAssignment,
                    NULL /* just reference the key */,
                    (rd_map_copy_t *)rd_kafka_topic_partition_list_copy);
        RD_MAP_COPY(&preBalancePartitionConsumers, currentPartitionConsumer,
                    rd_kafka_topic_partition_copy_void,
                    NULL /* references assign_cb(members) fields */);


        /* If we don't already need to revoke something due to subscription
         * changes, first try to balance by only moving newly added partitions.
         */
        if (!revocationRequired && unassignedPartitions->cnt > 0)
                performReassignments(rk, partitionMovements,
                                     unassignedPartitions, currentAssignment,
                                     prevAssignment, sortedCurrentSubscriptions,
                                     consumer2AllPotentialPartitions,
                                     partition2AllPotentialConsumers,
                                     currentPartitionConsumer, rkri);

        reassignmentPerformed = performReassignments(
            rk, partitionMovements, sortedPartitions, currentAssignment,
            prevAssignment, sortedCurrentSubscriptions,
            consumer2AllPotentialPartitions, partition2AllPotentialConsumers,
            currentPartitionConsumer, rkri);

        /* If we are not preserving existing assignments and we have made
         * changes to the current assignment make sure we are getting a more
         * balanced assignment; otherwise, revert to previous assignment. */

        if (!initializing && reassignmentPerformed &&
            (newScore = getBalanceScore(currentAssignment)) >=
                (oldScore = getBalanceScore(&preBalanceAssignment))) {

                rd_kafka_dbg(rk, ASSIGNOR, "STICKY",
                             "Reassignment performed but keeping previous "
                             "assignment since balance score did not improve: "
                             "new score %d (%d consumers) vs "
                             "old score %d (%d consumers): "
                             "lower score is better",
                             newScore, (int)RD_MAP_CNT(currentAssignment),
                             oldScore, (int)RD_MAP_CNT(&preBalanceAssignment));

                RD_MAP_COPY(
                    currentAssignment, &preBalanceAssignment,
                    NULL /* just reference the key */,
                    (rd_map_copy_t *)rd_kafka_topic_partition_list_copy);

                RD_MAP_CLEAR(currentPartitionConsumer);
                RD_MAP_COPY(currentPartitionConsumer,
                            &preBalancePartitionConsumers,
                            rd_kafka_topic_partition_copy_void,
                            NULL /* references assign_cb(members) fields */);
        }

        RD_MAP_DESTROY(&preBalancePartitionConsumers);
        RD_MAP_DESTROY(&preBalanceAssignment);

        /* Add the fixed assignments (those that could not change) back. */
        if (!RD_MAP_IS_EMPTY(&fixedAssignments)) {
                const rd_map_elem_t *elem;

                RD_MAP_FOREACH_ELEM(elem, &fixedAssignments.rmap) {
                        const char *consumer = elem->key;
                        rd_kafka_topic_partition_list_t *partitions =
                            (rd_kafka_topic_partition_list_t *)elem->value;

                        RD_MAP_SET(currentAssignment, consumer, partitions);

                        rd_list_add(sortedCurrentSubscriptions, (void *)elem);
                }

                /* Re-sort */
                rd_list_sort(sortedCurrentSubscriptions,
                             sort_by_map_elem_val_toppar_list_cnt);
        }

        RD_MAP_DESTROY(&fixedAssignments);
}



/**
 * @brief Populate subscriptions, current and previous assignments based on the
 *        \p members assignments.
 */
static void prepopulateCurrentAssignments(
    rd_kafka_t *rk,
    rd_kafka_group_member_t *members,
    size_t member_cnt,
    map_str_toppar_list_t *subscriptions,
    map_str_toppar_list_t *currentAssignment,
    map_toppar_cgpair_t *prevAssignment,
    map_toppar_str_t *currentPartitionConsumer,
    map_str_toppar_list_t *consumer2AllPotentialPartitions,
    size_t estimated_partition_cnt) {

        /* We need to process subscriptions' user data with each consumer's
         * reported generation in mind.
         * Higher generations overwrite lower generations in case of a conflict.
         * Conflicts will only exist if user data is for different generations.
         */

        /* For each partition we create a sorted list (by generation) of
         * its consumers. */
        RD_MAP_LOCAL_INITIALIZER(
            sortedPartitionConsumersByGeneration, member_cnt * 10 /* FIXME */,
            const rd_kafka_topic_partition_t *,
            /* List of ConsumerGenerationPair_t */
            rd_list_t *, rd_kafka_topic_partition_cmp,
            rd_kafka_topic_partition_hash, NULL, rd_list_destroy_free);
        const rd_kafka_topic_partition_t *partition;
        rd_list_t *consumers;
        int i;

        /* For each partition that is currently assigned to the group members
         * add the member and its generation to
         * sortedPartitionConsumersByGeneration (which is sorted afterwards)
         * indexed by the partition. */
        for (i = 0; i < (int)member_cnt; i++) {
                rd_kafka_group_member_t *consumer = &members[i];
                int j;

                RD_MAP_SET(subscriptions, consumer->rkgm_member_id->str,
                           consumer->rkgm_subscription);

                RD_MAP_SET(currentAssignment, consumer->rkgm_member_id->str,
                           rd_kafka_topic_partition_list_new(10));

                RD_MAP_SET(consumer2AllPotentialPartitions,
                           consumer->rkgm_member_id->str,
                           rd_kafka_topic_partition_list_new(
                               (int)estimated_partition_cnt));

                if (!consumer->rkgm_owned)
                        continue;

                for (j = 0; j < (int)consumer->rkgm_owned->cnt; j++) {
                        partition = &consumer->rkgm_owned->elems[j];

                        consumers = RD_MAP_GET_OR_SET(
                            &sortedPartitionConsumersByGeneration, partition,
                            rd_list_new(10, ConsumerGenerationPair_destroy));

                        rd_list_add(consumers,
                                    ConsumerGenerationPair_new(
                                        consumer->rkgm_member_id->str,
                                        consumer->rkgm_generation));

                        RD_MAP_SET(currentPartitionConsumer,
                                   rd_kafka_topic_partition_copy(partition),
                                   consumer->rkgm_member_id->str);
                }
        }

        /* Populate currentAssignment and prevAssignment.
         * prevAssignment holds the prior ConsumerGenerationPair_t
         * (before current) of each partition. */
        RD_MAP_FOREACH(partition, consumers,
                       &sortedPartitionConsumersByGeneration) {
                /* current and previous are the last two consumers
                 * of each partition, and found is used to check for duplicate
                 * consumers of same generation. */
                ConsumerGenerationPair_t *current, *previous, *found;
                rd_kafka_topic_partition_list_t *partitions;

                /* Sort the per-partition consumers list by generation */
                rd_list_sort(consumers, ConsumerGenerationPair_cmp_generation);

                /* In case a partition is claimed by multiple consumers with the
                 * same generation, invalidate it for all such consumers, and
                 * log an error for this situation. */
                if ((found = rd_list_find_duplicate(
                         consumers, ConsumerGenerationPair_cmp_generation))) {
                        const char *consumer1, *consumer2;
                        int idx = rd_list_index(
                            consumers, found,
                            ConsumerGenerationPair_cmp_generation);
                        consumer1 = ((ConsumerGenerationPair_t *)rd_list_elem(
                                         consumers, idx))
                                        ->consumer;
                        consumer2 = ((ConsumerGenerationPair_t *)rd_list_elem(
                                         consumers, idx + 1))
                                        ->consumer;

                        RD_MAP_DELETE(currentPartitionConsumer, partition);

                        rd_kafka_log(
                            rk, LOG_ERR, "STICKY",
                            "Sticky assignor: Found multiple consumers %s and "
                            "%s claiming the same topic partition %s:%d in the "
                            "same generation %d, this will be invalidated and "
                            "removed from their previous assignment.",
                            consumer1, consumer2, partition->topic,
                            partition->partition, found->generation);
                        continue;
                }

                /* Add current (highest generation) consumer
                 * to currentAssignment. */
                current    = rd_list_last(consumers);
                partitions = RD_MAP_GET(currentAssignment, current->consumer);
                rd_kafka_topic_partition_list_add(partitions, partition->topic,
                                                  partition->partition);

                /* Add previous (next highest generation) consumer, if any,
                 * to prevAssignment. */
                if (rd_list_cnt(consumers) >= 2 &&
                    (previous =
                         rd_list_elem(consumers, rd_list_cnt(consumers) - 2)))
                        RD_MAP_SET(
                            prevAssignment,
                            rd_kafka_topic_partition_copy(partition),
                            ConsumerGenerationPair_new(previous->consumer,
                                                       previous->generation));
        }

        RD_MAP_DESTROY(&sortedPartitionConsumersByGeneration);
}


/**
 * @brief Populate maps for potential partitions per consumer and vice-versa.
 */
static void
populatePotentialMaps(const rd_kafka_assignor_topic_t *atopic,
                      map_toppar_list_t *partition2AllPotentialConsumers,
                      map_str_toppar_list_t *consumer2AllPotentialPartitions,
                      size_t estimated_partition_cnt) {
        int i;
        const rd_kafka_group_member_t *rkgm;

        /* for each eligible (subscribed and available) topic (\p atopic):
         *   for each member subscribing to that topic:
         *     and for each partition of that topic:
         *        add consumer and partition to:
         *          partition2AllPotentialConsumers
         *          consumer2AllPotentialPartitions
         */

        RD_LIST_FOREACH(rkgm, &atopic->members, i) {
                const char *consumer = rkgm->rkgm_member_id->str;
                rd_kafka_topic_partition_list_t *partitions =
                    RD_MAP_GET(consumer2AllPotentialPartitions, consumer);
                int j;

                rd_assert(partitions != NULL);

                for (j = 0; j < atopic->metadata->partition_cnt; j++) {
                        rd_kafka_topic_partition_t *partition;
                        rd_list_t *consumers;

                        /* consumer2AllPotentialPartitions[consumer] += part */
                        partition = rd_kafka_topic_partition_list_add(
                            partitions, atopic->metadata->topic,
                            atopic->metadata->partitions[j].id);

                        /* partition2AllPotentialConsumers[part] += consumer */
                        if (!(consumers =
                                  RD_MAP_GET(partition2AllPotentialConsumers,
                                             partition))) {
                                consumers = rd_list_new(
                                    RD_MAX(2, (int)estimated_partition_cnt / 2),
                                    NULL);
                                RD_MAP_SET(
                                    partition2AllPotentialConsumers,
                                    rd_kafka_topic_partition_copy(partition),
                                    consumers);
                        }
                        rd_list_add(consumers, (void *)consumer);
                }
        }
}


/**
 * @returns true if all consumers have identical subscriptions based on
 *          the currently available topics and partitions.
 *
 * @remark The Java code checks both partition2AllPotentialConsumers and
 *         and consumer2AllPotentialPartitions but since these maps
 *         are symmetrical we only check one of them.
 *         ^ FIXME, but we do.
 */
static rd_bool_t areSubscriptionsIdentical(
    map_toppar_list_t *partition2AllPotentialConsumers,
    map_str_toppar_list_t *consumer2AllPotentialPartitions) {
        const void *ignore;
        const rd_list_t *lcurr, *lprev                       = NULL;
        const rd_kafka_topic_partition_list_t *pcurr, *pprev = NULL;

        RD_MAP_FOREACH(ignore, lcurr, partition2AllPotentialConsumers) {
                if (lprev && rd_list_cmp(lcurr, lprev, rd_map_str_cmp))
                        return rd_false;
                lprev = lcurr;
        }

        RD_MAP_FOREACH(ignore, pcurr, consumer2AllPotentialPartitions) {
                if (pprev && rd_kafka_topic_partition_list_cmp(
                                 pcurr, pprev, rd_kafka_topic_partition_cmp))
                        return rd_false;
                pprev = pcurr;
        }

        if (ignore) /* Avoid unused warning */
                ;

        return rd_true;
}


/**
 * @brief Comparator to sort an rd_kafka_topic_partition_list_t in ascending
 *        order by the number of list elements in the .opaque field, or
 *        secondarily by the topic name.
 *        Used by sortPartitions().
 */
static int
toppar_sort_by_list_cnt(const void *_a, const void *_b, void *opaque) {
        const rd_kafka_topic_partition_t *a = _a, *b = _b;
        const rd_list_t *al = a->opaque, *bl = b->opaque;
        int r = rd_list_cnt(al) - rd_list_cnt(bl); /* ascending order */
        if (r)
                return r;
        return rd_kafka_topic_partition_cmp(a, b);
}


/**
 * @brief Sort valid partitions so they are processed in the potential
 *        reassignment phase in the proper order that causes minimal partition
 *        movement among consumers (hence honouring maximal stickiness).
 *
 * @returns The result of the partitions sort.
 */
static rd_kafka_topic_partition_list_t *
sortPartitions(rd_kafka_t *rk,
               map_str_toppar_list_t *currentAssignment,
               map_toppar_cgpair_t *prevAssignment,
               rd_bool_t isFreshAssignment,
               map_toppar_list_t *partition2AllPotentialConsumers,
               map_str_toppar_list_t *consumer2AllPotentialPartitions) {

        rd_kafka_topic_partition_list_t *sortedPartitions;
        map_str_toppar_list_t assignments = RD_MAP_INITIALIZER(
            RD_MAP_CNT(currentAssignment), rd_map_str_cmp, rd_map_str_hash,
            NULL, rd_kafka_topic_partition_list_destroy_free);
        rd_kafka_topic_partition_list_t *partitions;
        const rd_kafka_topic_partition_t *partition;
        const rd_list_t *consumers;
        const char *consumer;
        rd_list_t sortedConsumers; /* element is the (rd_map_elem_t *) from
                                    * assignments. */
        const rd_map_elem_t *elem;
        rd_bool_t wasEmpty;
        int i;

        sortedPartitions = rd_kafka_topic_partition_list_new(
            (int)RD_MAP_CNT(partition2AllPotentialConsumers));
        ;

        rd_kafka_dbg(rk, ASSIGNOR, "STICKY",
                     "Sort %d partitions in %s assignment",
                     (int)RD_MAP_CNT(partition2AllPotentialConsumers),
                     isFreshAssignment ? "fresh" : "existing");

        if (isFreshAssignment ||
            !areSubscriptionsIdentical(partition2AllPotentialConsumers,
                                       consumer2AllPotentialPartitions)) {
                /* Create an ascending sorted list of partitions based on
                 * how many consumers can potentially use them. */
                RD_MAP_FOREACH(partition, consumers,
                               partition2AllPotentialConsumers) {
                        rd_kafka_topic_partition_list_add(sortedPartitions,
                                                          partition->topic,
                                                          partition->partition)
                            ->opaque = (void *)consumers;
                }

                rd_kafka_topic_partition_list_sort(
                    sortedPartitions, toppar_sort_by_list_cnt, NULL);

                RD_MAP_DESTROY(&assignments);

                return sortedPartitions;
        }

        /* If this is a reassignment and the subscriptions are identical
         * then we just need to list partitions in a round robin fashion
         * (from consumers with most assigned partitions to those
         * with least assigned partitions). */

        /* Create an ascending sorted list of consumers by valid
         * partition count. The list element is the `rd_map_elem_t *`
         * of the assignments map. This allows us to get a sorted list
         * of consumers without too much data duplication. */
        rd_list_init(&sortedConsumers, (int)RD_MAP_CNT(currentAssignment),
                     NULL);

        RD_MAP_FOREACH(consumer, partitions, currentAssignment) {
                rd_kafka_topic_partition_list_t *partitions2;

                /* Sort assigned partitions for consistency (during tests) */
                rd_kafka_topic_partition_list_sort(partitions, NULL, NULL);

                partitions2 =
                    rd_kafka_topic_partition_list_new(partitions->cnt);

                for (i = 0; i < partitions->cnt; i++) {
                        partition = &partitions->elems[i];

                        /* Only add partitions from the current assignment
                         * that still exist. */
                        if (RD_MAP_GET(partition2AllPotentialConsumers,
                                       partition))
                                rd_kafka_topic_partition_list_add(
                                    partitions2, partition->topic,
                                    partition->partition);
                }

                if (partitions2->cnt > 0) {
                        elem = RD_MAP_SET(&assignments, consumer, partitions2);
                        rd_list_add(&sortedConsumers, (void *)elem);
                } else
                        rd_kafka_topic_partition_list_destroy(partitions2);
        }

        /* Sort consumers */
        rd_list_sort(&sortedConsumers, sort_by_map_elem_val_toppar_list_cnt);

        /* At this point sortedConsumers contains an ascending-sorted list
         * of consumers based on how many valid partitions are currently
         * assigned to them. */

        while (!rd_list_empty(&sortedConsumers)) {
                /* Take consumer with most partitions */
                const rd_map_elem_t *elem = rd_list_last(&sortedConsumers);
                const char *consumer      = (const char *)elem->key;
                /* Currently assigned partitions to this consumer */
                rd_kafka_topic_partition_list_t *remainingPartitions =
                    RD_MAP_GET(&assignments, consumer);
                /* Partitions that were assigned to a different consumer
                 * last time */
                rd_kafka_topic_partition_list_t *prevPartitions =
                    rd_kafka_topic_partition_list_new(
                        (int)RD_MAP_CNT(prevAssignment));
                rd_bool_t reSort = rd_true;

                /* From the partitions that had a different consumer before,
                 * keep only those that are assigned to this consumer now. */
                for (i = 0; i < remainingPartitions->cnt; i++) {
                        partition = &remainingPartitions->elems[i];
                        if (RD_MAP_GET(prevAssignment, partition))
                                rd_kafka_topic_partition_list_add(
                                    prevPartitions, partition->topic,
                                    partition->partition);
                }

                if (prevPartitions->cnt > 0) {
                        /* If there is a partition of this consumer that was
                         * assigned to another consumer before, then mark
                         * it as a good option for reassignment. */
                        partition = &prevPartitions->elems[0];

                        rd_kafka_topic_partition_list_del(remainingPartitions,
                                                          partition->topic,
                                                          partition->partition);

                        rd_kafka_topic_partition_list_add(sortedPartitions,
                                                          partition->topic,
                                                          partition->partition);

                        rd_kafka_topic_partition_list_del_by_idx(prevPartitions,
                                                                 0);

                } else if (remainingPartitions->cnt > 0) {
                        /* Otherwise mark any other one of the current
                         * partitions as a reassignment candidate. */
                        partition = &remainingPartitions->elems[0];

                        rd_kafka_topic_partition_list_add(sortedPartitions,
                                                          partition->topic,
                                                          partition->partition);

                        rd_kafka_topic_partition_list_del_by_idx(
                            remainingPartitions, 0);
                } else {
                        rd_list_remove_elem(&sortedConsumers,
                                            rd_list_cnt(&sortedConsumers) - 1);
                        /* No need to re-sort the list (below) */
                        reSort = rd_false;
                }

                rd_kafka_topic_partition_list_destroy(prevPartitions);

                if (reSort) {
                        /* Re-sort the list to keep the consumer with the most
                         * partitions at the end of the list.
                         * This should be an O(N) operation given it is at most
                         * a single shuffle. */
                        rd_list_sort(&sortedConsumers,
                                     sort_by_map_elem_val_toppar_list_cnt);
                }
        }


        wasEmpty = !sortedPartitions->cnt;

        RD_MAP_FOREACH(partition, consumers, partition2AllPotentialConsumers)
        rd_kafka_topic_partition_list_upsert(sortedPartitions, partition->topic,
                                             partition->partition);

        /* If all partitions were added in the foreach loop just above
         * it means there is no order to retain from the sorderConsumer loop
         * below and we sort the partitions according to their topic+partition
         * to get consistent results (mainly in tests). */
        if (wasEmpty)
                rd_kafka_topic_partition_list_sort(sortedPartitions, NULL,
                                                   NULL);

        rd_list_destroy(&sortedConsumers);
        RD_MAP_DESTROY(&assignments);

        return sortedPartitions;
}


/**
 * @brief Transfer currentAssignment to members array.
 */
static void assignToMembers(map_str_toppar_list_t *currentAssignment,
                            rd_kafka_group_member_t *members,
                            size_t member_cnt) {
        size_t i;

        for (i = 0; i < member_cnt; i++) {
                rd_kafka_group_member_t *rkgm = &members[i];
                const rd_kafka_topic_partition_list_t *partitions =
                    RD_MAP_GET(currentAssignment, rkgm->rkgm_member_id->str);
                if (rkgm->rkgm_assignment)
                        rd_kafka_topic_partition_list_destroy(
                            rkgm->rkgm_assignment);
                rkgm->rkgm_assignment =
                    rd_kafka_topic_partition_list_copy(partitions);
        }
}


/**
 * @brief KIP-54 and KIP-341/FIXME sticky assignor.
 *
 * This code is closely mimicking the AK Java AbstractStickyAssignor.assign().
 */
rd_kafka_resp_err_t
rd_kafka_sticky_assignor_assign_cb(rd_kafka_t *rk,
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
        /* FIXME: Let the cgrp pass the actual eligible partition count */
        size_t partition_cnt = member_cnt * 10; /* FIXME */
        const rd_kafka_metadata_internal_t *mdi =
            rd_kafka_metadata_get_internal(metadata);

        rd_kafka_rack_info_t *rkri =
            rd_kafka_rack_info_new(eligible_topics, eligible_topic_cnt, mdi);

        /* Map of subscriptions. This is \p member turned into a map. */
        map_str_toppar_list_t subscriptions =
            RD_MAP_INITIALIZER(member_cnt, rd_map_str_cmp, rd_map_str_hash,
                               NULL /* refs members.rkgm_member_id */,
                               NULL /* refs members.rkgm_subscription */);

        /* Map member to current assignment */
        map_str_toppar_list_t currentAssignment =
            RD_MAP_INITIALIZER(member_cnt, rd_map_str_cmp, rd_map_str_hash,
                               NULL /* refs members.rkgm_member_id */,
                               rd_kafka_topic_partition_list_destroy_free);

        /* Map partition to ConsumerGenerationPair */
        map_toppar_cgpair_t prevAssignment =
            RD_MAP_INITIALIZER(partition_cnt, rd_kafka_topic_partition_cmp,
                               rd_kafka_topic_partition_hash,
                               rd_kafka_topic_partition_destroy_free,
                               ConsumerGenerationPair_destroy);

        /* Partition assignment movements between consumers */
        PartitionMovements_t partitionMovements;

        rd_bool_t isFreshAssignment;

        /* Mapping of all topic partitions to all consumers that can be
         * assigned to them.
         * Value is an rd_list_t* with elements referencing the \p members
         * \c rkgm_member_id->str. */
        map_toppar_list_t partition2AllPotentialConsumers = RD_MAP_INITIALIZER(
            partition_cnt, rd_kafka_topic_partition_cmp,
            rd_kafka_topic_partition_hash,
            rd_kafka_topic_partition_destroy_free, rd_list_destroy_free);

        /* Mapping of all consumers to all potential topic partitions that
         * can be assigned to them. */
        map_str_toppar_list_t consumer2AllPotentialPartitions =
            RD_MAP_INITIALIZER(member_cnt, rd_map_str_cmp, rd_map_str_hash,
                               NULL,
                               rd_kafka_topic_partition_list_destroy_free);

        /* Mapping of partition to current consumer. */
        map_toppar_str_t currentPartitionConsumer =
            RD_MAP_INITIALIZER(partition_cnt, rd_kafka_topic_partition_cmp,
                               rd_kafka_topic_partition_hash,
                               rd_kafka_topic_partition_destroy_free,
                               NULL /* refs members.rkgm_member_id->str */);

        rd_kafka_topic_partition_list_t *sortedPartitions;
        rd_kafka_topic_partition_list_t *unassignedPartitions;
        rd_list_t sortedCurrentSubscriptions;

        rd_bool_t revocationRequired = rd_false;

        /* Iteration variables */
        const char *consumer;
        rd_kafka_topic_partition_list_t *partitions;
        const rd_map_elem_t *elem;
        int i;

        /* Initialize PartitionMovements */
        PartitionMovements_init(&partitionMovements, eligible_topic_cnt);

        /* Prepopulate current and previous assignments */
        prepopulateCurrentAssignments(
            rk, members, member_cnt, &subscriptions, &currentAssignment,
            &prevAssignment, &currentPartitionConsumer,
            &consumer2AllPotentialPartitions, partition_cnt);

        isFreshAssignment = RD_MAP_IS_EMPTY(&currentAssignment);

        /* Populate partition2AllPotentialConsumers and
         * consumer2AllPotentialPartitions maps by each eligible topic. */
        for (i = 0; i < (int)eligible_topic_cnt; i++)
                populatePotentialMaps(
                    eligible_topics[i], &partition2AllPotentialConsumers,
                    &consumer2AllPotentialPartitions, partition_cnt);


        /* Sort valid partitions to minimize partition movements. */
        sortedPartitions = sortPartitions(
            rk, &currentAssignment, &prevAssignment, isFreshAssignment,
            &partition2AllPotentialConsumers, &consumer2AllPotentialPartitions);


        /* All partitions that need to be assigned (initially set to all
         * partitions but adjusted in the following loop) */
        unassignedPartitions =
            rd_kafka_topic_partition_list_copy(sortedPartitions);

        if (rkri)
                rd_kafka_dbg(rk, CGRP, "STICKY",
                             "Sticky assignor: using rack aware assignment.");

        RD_MAP_FOREACH(consumer, partitions, &currentAssignment) {
                if (!RD_MAP_GET(&subscriptions, consumer)) {
                        /* If a consumer that existed before
                         * (and had some partition assignments) is now removed,
                         * remove it from currentAssignment and its
                         * partitions from currentPartitionConsumer */

                        rd_kafka_dbg(rk, ASSIGNOR, "STICKY",
                                     "Removing now non-existent consumer %s "
                                     "with %d previously assigned partitions",
                                     consumer, partitions->cnt);


                        for (i = 0; i < partitions->cnt; i++) {
                                const rd_kafka_topic_partition_t *partition =
                                    &partitions->elems[i];
                                RD_MAP_DELETE(&currentPartitionConsumer,
                                              partition);
                        }

                        /* FIXME: The delete could be optimized by passing the
                         *        underlying elem_t. */
                        RD_MAP_DELETE(&currentAssignment, consumer);

                } else {
                        /* Otherwise (the consumer still exists) */

                        for (i = 0; i < partitions->cnt; i++) {
                                const rd_kafka_topic_partition_t *partition =
                                    &partitions->elems[i];
                                rd_bool_t remove_part = rd_false;

                                if (!RD_MAP_GET(
                                        &partition2AllPotentialConsumers,
                                        partition)) {
                                        /* If this partition of this consumer
                                         * no longer exists remove it from
                                         * currentAssignment of the consumer */
                                        remove_part = rd_true;
                                        RD_MAP_DELETE(&currentPartitionConsumer,
                                                      partition);

                                } else if (!rd_kafka_topic_partition_list_find(
                                               RD_MAP_GET(&subscriptions,
                                                          consumer),
                                               partition->topic,
                                               RD_KAFKA_PARTITION_UA) ||
                                           rd_kafka_racks_mismatch(
                                               rkri, consumer, partition)) {
                                        /* If this partition cannot remain
                                         * assigned to its current consumer
                                         * because the consumer is no longer
                                         * subscribed to its topic, or racks
                                         * don't match for rack-aware
                                         * assignment, remove it from the
                                         * currentAssignment of the consumer. */
                                        remove_part        = rd_true;
                                        revocationRequired = rd_true;
                                } else {
                                        /* Otherwise, remove the topic partition
                                         * from those that need to be assigned
                                         * only if its current consumer is still
                                         * subscribed to its topic (because it
                                         * is already assigned and we would want
                                         * to preserve that assignment as much
                                         * as possible). */
                                        rd_kafka_topic_partition_list_del(
                                            unassignedPartitions,
                                            partition->topic,
                                            partition->partition);
                                }

                                if (remove_part) {
                                        rd_kafka_topic_partition_list_del_by_idx(
                                            partitions, i);
                                        i--; /* Since the current element was
                                              * removed we need the next for
                                              * loop iteration to stay at the
                                              * same index. */
                                }
                        }
                }
        }


        /* At this point we have preserved all valid topic partition to consumer
         * assignments and removed all invalid topic partitions and invalid
         * consumers.
         * Now we need to assign unassignedPartitions to consumers so that the
         * topic partition assignments are as balanced as possible. */

        /* An ascending sorted list of consumers based on how many topic
         * partitions are already assigned to them. The list element is
         * referencing the rd_map_elem_t* from the currentAssignment map. */
        rd_list_init(&sortedCurrentSubscriptions,
                     (int)RD_MAP_CNT(&currentAssignment), NULL);

        RD_MAP_FOREACH_ELEM(elem, &currentAssignment.rmap)
        rd_list_add(&sortedCurrentSubscriptions, (void *)elem);

        rd_list_sort(&sortedCurrentSubscriptions,
                     sort_by_map_elem_val_toppar_list_cnt);

        /* Balance the available partitions across consumers */
        balance(rk, &partitionMovements, &currentAssignment, &prevAssignment,
                sortedPartitions, unassignedPartitions,
                &sortedCurrentSubscriptions, &consumer2AllPotentialPartitions,
                &partition2AllPotentialConsumers, &currentPartitionConsumer,
                revocationRequired, rkri);

        /* Transfer currentAssignment (now updated) to each member's
         * assignment. */
        assignToMembers(&currentAssignment, members, member_cnt);


        rd_list_destroy(&sortedCurrentSubscriptions);

        PartitionMovements_destroy(&partitionMovements);

        rd_kafka_topic_partition_list_destroy(unassignedPartitions);
        rd_kafka_topic_partition_list_destroy(sortedPartitions);
        rd_kafka_rack_info_destroy(rkri);

        RD_MAP_DESTROY(&currentPartitionConsumer);
        RD_MAP_DESTROY(&consumer2AllPotentialPartitions);
        RD_MAP_DESTROY(&partition2AllPotentialConsumers);
        RD_MAP_DESTROY(&prevAssignment);
        RD_MAP_DESTROY(&currentAssignment);
        RD_MAP_DESTROY(&subscriptions);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



/** @brief FIXME docstring */
static void rd_kafka_sticky_assignor_on_assignment_cb(
    const rd_kafka_assignor_t *rkas,
    void **assignor_state,
    const rd_kafka_topic_partition_list_t *partitions,
    const rd_kafkap_bytes_t *assignment_userdata,
    const rd_kafka_consumer_group_metadata_t *rkcgm) {
        rd_kafka_sticky_assignor_state_t *state =
            (rd_kafka_sticky_assignor_state_t *)*assignor_state;

        if (!state)
                state = rd_calloc(1, sizeof(*state));
        else
                rd_kafka_topic_partition_list_destroy(state->prev_assignment);

        state->prev_assignment = rd_kafka_topic_partition_list_copy(partitions);
        state->generation_id   = rkcgm->generation_id;

        *assignor_state = state;
}

/** @brief FIXME docstring */
static rd_kafkap_bytes_t *rd_kafka_sticky_assignor_get_metadata(
    const rd_kafka_assignor_t *rkas,
    void *assignor_state,
    const rd_list_t *topics,
    const rd_kafka_topic_partition_list_t *owned_partitions,
    const rd_kafkap_str_t *rack_id) {
        rd_kafka_sticky_assignor_state_t *state;
        rd_kafka_buf_t *rkbuf;
        rd_kafkap_bytes_t *metadata;
        rd_kafkap_bytes_t *kbytes;
        size_t len;

        /*
         * UserData (Version: 1) => [previous_assignment] generation
         *   previous_assignment => topic [partitions]
         *     topic => STRING
         *     partitions => partition
         *       partition => INT32
         *   generation => INT32
         *
         * If there is no previous assignment, UserData is NULL.
         */


        if (!assignor_state) {
                return rd_kafka_consumer_protocol_member_metadata_new(
                    topics, NULL, 0, owned_partitions, -1 /* generation */,
                    rack_id);
        }

        state = (rd_kafka_sticky_assignor_state_t *)assignor_state;

        rkbuf = rd_kafka_buf_new(1, 100);
        rd_assert(state->prev_assignment != NULL);
        const rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};
        rd_kafka_buf_write_topic_partitions(
            rkbuf, state->prev_assignment, rd_false /*skip invalid offsets*/,
            rd_false /*any offset*/, rd_false /*don't use topic id*/,
            rd_true /*use topic name*/, fields);
        rd_kafka_buf_write_i32(rkbuf, state->generation_id);

        /* Get binary buffer and allocate a new Kafka Bytes with a copy. */
        rd_slice_init_full(&rkbuf->rkbuf_reader, &rkbuf->rkbuf_buf);
        len    = rd_slice_remains(&rkbuf->rkbuf_reader);
        kbytes = rd_kafkap_bytes_new(NULL, (int32_t)len);
        rd_slice_read(&rkbuf->rkbuf_reader, (void *)kbytes->data, len);
        rd_kafka_buf_destroy(rkbuf);

        metadata = rd_kafka_consumer_protocol_member_metadata_new(
            topics, kbytes->data, kbytes->len, owned_partitions,
            state->generation_id, rack_id);

        rd_kafkap_bytes_destroy(kbytes);

        return metadata;
}


/**
 * @brief Destroy assignor state
 */
static void rd_kafka_sticky_assignor_state_destroy(void *assignor_state) {
        rd_kafka_sticky_assignor_state_t *state =
            (rd_kafka_sticky_assignor_state_t *)assignor_state;

        rd_assert(assignor_state);

        rd_kafka_topic_partition_list_destroy(state->prev_assignment);
        rd_free(state);
}



/**
 * @name Sticky assignor unit tests
 *
 *
 * These are based on AbstractStickyAssignorTest.java
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

/* Helper to get consumer rack based on the index of the consumer. */
static rd_kafkap_str_t *
ut_get_consumer_rack(int idx,
                     rd_kafka_assignor_ut_rack_config_t parametrization) {
        const int cycle_size =
            (parametrization == RD_KAFKA_RANGE_ASSIGNOR_UT_NO_BROKER_RACK
                 ? RD_ARRAYSIZE(ALL_RACKS)
                 : 3);
        return (ALL_RACKS[idx % cycle_size]);
}

/* Helper to populate a member's owned partitions (accepted as variadic), and
 * generation. */
static void
ut_populate_member_owned_partitions_generation(rd_kafka_group_member_t *rkgm,
                                               int generation,
                                               size_t partition_cnt,
                                               ...) {
        va_list ap;
        size_t i;

        if (rkgm->rkgm_owned)
                rd_kafka_topic_partition_list_destroy(rkgm->rkgm_owned);
        rkgm->rkgm_owned = rd_kafka_topic_partition_list_new(partition_cnt);

        va_start(ap, partition_cnt);
        for (i = 0; i < partition_cnt; i++) {
                char *topic   = va_arg(ap, char *);
                int partition = va_arg(ap, int);
                rd_kafka_topic_partition_list_add(rkgm->rkgm_owned, topic,
                                                  partition);
        }
        va_end(ap);

        rkgm->rkgm_generation = generation;
}

/* Helper to create topic partition list from a variadic list of topic,
 * partition pairs. */
static rd_kafka_topic_partition_list_t **
ut_create_topic_partition_lists(size_t list_cnt, ...) {
        va_list ap;
        size_t i;
        rd_kafka_topic_partition_list_t **lists =
            rd_calloc(list_cnt, sizeof(rd_kafka_topic_partition_list_t *));

        va_start(ap, list_cnt);
        for (i = 0; i < list_cnt; i++) {
                const char *topic;
                lists[i] = rd_kafka_topic_partition_list_new(0);
                while ((topic = va_arg(ap, const char *))) {
                        int partition = va_arg(ap, int);
                        rd_kafka_topic_partition_list_add(lists[i], topic,
                                                          partition);
                }
        }
        va_end(ap);

        return lists;
}

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

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], NULL);
        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

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
                                       1, "topic1", 0);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], NULL);
        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

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
                                       1, "topic1", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        RD_UT_ASSERT(members[0].rkgm_assignment->cnt == 3,
                     "expected assignment of 3 partitions, got %d partition(s)",
                     members[0].rkgm_assignment->cnt);

        verifyAssignment(&members[0], "topic1", 0, "topic1", 1, "topic1", 2,
                         NULL);
        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

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
                                       2, "topic1", 3, "topic2", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "topic1", 0, "topic1", 1, "topic1", 2,
                         NULL);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

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
                                       2, "topic1", 1, "topic2", 2);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", "topic2", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "topic1", 0, "topic2", 0, "topic2", 1,
                         NULL);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

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
                                       1, "topic1", 1);
        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);
        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "topic1", 0, NULL);
        verifyAssignment(&members[1], NULL);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

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
                                       1, "topic1", 2);
        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);
        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", NULL);


        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "topic1", 0, NULL);
        verifyAssignment(&members[1], "topic1", 1, NULL);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

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
                                       2, "topic1", 3, "topic2", 2);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);
        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", "topic2", NULL);
        ut_initMemberConditionalRack(&members[2], "consumer3",
                                     ut_get_consumer_rack(2, parametrization),
                                     parametrization, "topic1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "topic1", 0, "topic1", 2, NULL);
        verifyAssignment(&members[1], "topic2", 0, "topic2", 1, NULL);
        verifyAssignment(&members[2], "topic1", 1, NULL);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

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
                                       2, "topic1", 3, "topic2", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", "topic2", NULL);
        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", "topic2", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "topic1", 0, "topic1", 2, "topic2", 1,
                         NULL);
        verifyAssignment(&members[1], "topic1", 1, "topic2", 0, "topic2", 2,
                         NULL);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

        rd_kafka_group_member_clear(&members[0]);
        rd_kafka_group_member_clear(&members[1]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int ut_testAddRemoveConsumerOneTopic(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[2];

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "topic1", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);


        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members, 1,
                                    errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "topic1", 0, "topic1", 1, "topic1", 2,
                         NULL);

        verifyValidityAndBalance(members, 1, metadata);
        isFullyBalanced(members, 1);

        /* Add consumer2 */
        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "topic1", 1, "topic1", 2, NULL);
        verifyAssignment(&members[1], "topic1", 0, NULL);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));
        // FIXME: isSticky();


        /* Remove consumer1 */
        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, &members[1], 1,
                                    errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[1], "topic1", 0, "topic1", 1, "topic1", 2,
                         NULL);

        verifyValidityAndBalance(&members[1], 1, metadata);
        isFullyBalanced(&members[1], 1);
        // FIXME: isSticky();

        rd_kafka_group_member_clear(&members[0]);
        rd_kafka_group_member_clear(&members[1]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

/**
 * This unit test performs sticky assignment for a scenario that round robin
 * assignor handles poorly.
 * Topics (partitions per topic):
 *  - topic1 (2), topic2 (1), topic3 (2), topic4 (1), topic5 (2)
 * Subscriptions:
 *  - consumer1: topic1, topic2, topic3, topic4, topic5
 *  - consumer2: topic1, topic3, topic5
 *  - consumer3: topic1, topic3, topic5
 *  - consumer4: topic1, topic2, topic3, topic4, topic5
 * Round Robin Assignment Result:
 *  - consumer1: topic1-0, topic3-0, topic5-0
 *  - consumer2: topic1-1, topic3-1, topic5-1
 *  - consumer3:
 *  - consumer4: topic2-0, topic4-0
 * Sticky Assignment Result:
 *  - consumer1: topic2-0, topic3-0
 *  - consumer2: topic1-0, topic3-1
 *  - consumer3: topic1-1, topic5-0
 *  - consumer4: topic4-0, topic5-1
 */
static int ut_testPoorRoundRobinAssignmentScenario(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[4];

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       5, "topic1", 2, "topic2", 1, "topic3", 2,
                                       "topic4", 1, "topic5", 2);


        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", "topic2",
                                     "topic3", "topic4", "topic5", NULL);
        ut_initMemberConditionalRack(
            &members[1], "consumer2", ut_get_consumer_rack(1, parametrization),
            parametrization, "topic1", "topic3", "topic5", NULL);
        ut_initMemberConditionalRack(
            &members[2], "consumer3", ut_get_consumer_rack(2, parametrization),
            parametrization, "topic1", "topic3", "topic5", NULL);
        ut_initMemberConditionalRack(&members[3], "consumer4",
                                     ut_get_consumer_rack(3, parametrization),
                                     parametrization, "topic1", "topic2",
                                     "topic3", "topic4", "topic5", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "topic2", 0, "topic3", 0, NULL);
        verifyAssignment(&members[1], "topic1", 0, "topic3", 1, NULL);
        verifyAssignment(&members[2], "topic1", 1, "topic5", 0, NULL);
        verifyAssignment(&members[3], "topic4", 0, "topic5", 1, NULL);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

        rd_kafka_group_member_clear(&members[0]);
        rd_kafka_group_member_clear(&members[1]);
        rd_kafka_group_member_clear(&members[2]);
        rd_kafka_group_member_clear(&members[3]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}



static int ut_testAddRemoveTopicTwoConsumers(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[2];

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "topic1", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", "topic2", NULL);
        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", "topic2", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "topic1", 0, "topic1", 2, NULL);
        verifyAssignment(&members[1], "topic1", 1, NULL);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

        /*
         * Add topic2
         */
        RD_UT_SAY("Adding topic2");
        ut_destroy_metadata(metadata);

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       2, "topic1", 3, "topic2", 3);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "topic1", 0, "topic1", 2, "topic2", 1,
                         NULL);
        verifyAssignment(&members[1], "topic1", 1, "topic2", 2, "topic2", 0,
                         NULL);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));
        // FIXME: isSticky();


        /*
         * Remove topic1
         */
        RD_UT_SAY("Removing topic1");
        ut_destroy_metadata(metadata);

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "topic2", 3);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyAssignment(&members[0], "topic2", 1, NULL);
        verifyAssignment(&members[1], "topic2", 0, "topic2", 2, NULL);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));
        // FIXME: isSticky();

        rd_kafka_group_member_clear(&members[0]);
        rd_kafka_group_member_clear(&members[1]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int ut_testReassignmentAfterOneConsumerLeaves(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[19];
        int member_cnt = RD_ARRAYSIZE(members);
        rd_kafka_metadata_topic_t mt[19];
        int topic_cnt = RD_ARRAYSIZE(mt);
        int i;

        for (i = 0; i < topic_cnt; i++) {
                char topic[10];
                rd_snprintf(topic, sizeof(topic), "topic%d", i + 1);
                rd_strdupa(&mt[i].topic, topic);
                mt[i].partition_cnt = i + 1;
        }

        ut_initMetadataConditionalRack0(&metadata, 3, 3, ALL_RACKS,
                                        RD_ARRAYSIZE(ALL_RACKS),
                                        parametrization, mt, topic_cnt);

        for (i = 1; i <= member_cnt; i++) {
                char name[20];
                rd_kafka_topic_partition_list_t *subscription =
                    rd_kafka_topic_partition_list_new(i);
                int j;
                for (j = 1; j <= i; j++) {
                        char topic[16];
                        rd_snprintf(topic, sizeof(topic), "topic%d", j);
                        rd_kafka_topic_partition_list_add(
                            subscription, topic, RD_KAFKA_PARTITION_UA);
                }
                rd_snprintf(name, sizeof(name), "consumer%d", i);

                ut_initMemberConditionalRack(
                    &members[i - 1], name,
                    ut_get_consumer_rack(i, parametrization), parametrization,
                    NULL);

                rd_kafka_topic_partition_list_destroy(
                    members[i - 1].rkgm_subscription);
                members[i - 1].rkgm_subscription = subscription;
        }

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, member_cnt, metadata);


        /*
         * Remove consumer10.
         */
        rd_kafka_group_member_clear(&members[9]);
        memmove(&members[9], &members[10],
                sizeof(*members) * (member_cnt - 10));
        member_cnt--;

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, member_cnt, metadata);
        // FIXME: isSticky();

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int ut_testReassignmentAfterOneConsumerAdded(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[9];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "topic1", 20);

        for (i = 1; i <= member_cnt; i++) {
                char name[20];
                rd_kafka_topic_partition_list_t *subscription =
                    rd_kafka_topic_partition_list_new(1);
                rd_kafka_topic_partition_list_add(subscription, "topic1",
                                                  RD_KAFKA_PARTITION_UA);
                rd_snprintf(name, sizeof(name), "consumer%d", i);
                ut_initMemberConditionalRack(
                    &members[i - 1], name,
                    ut_get_consumer_rack(i, parametrization), parametrization,
                    NULL);
                rd_kafka_topic_partition_list_destroy(
                    members[i - 1].rkgm_subscription);
                members[i - 1].rkgm_subscription = subscription;
        }

        member_cnt--; /* Skip one consumer */
        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, member_cnt, metadata);


        /*
         * Add consumer.
         */
        member_cnt++;

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, member_cnt, metadata);
        // FIXME: isSticky();

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int
ut_testSameSubscriptions(rd_kafka_t *rk,
                         const rd_kafka_assignor_t *rkas,
                         rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[9];
        int member_cnt = RD_ARRAYSIZE(members);
        rd_kafka_metadata_topic_t mt[15];
        int topic_cnt = RD_ARRAYSIZE(mt);
        rd_kafka_topic_partition_list_t *subscription =
            rd_kafka_topic_partition_list_new(topic_cnt);
        int i;

        for (i = 0; i < topic_cnt; i++) {
                char topic[10];
                rd_snprintf(topic, sizeof(topic), "topic%d", i + 1);
                rd_strdupa(&mt[i].topic, topic);
                mt[i].partition_cnt = i + 1;
                rd_kafka_topic_partition_list_add(subscription, topic,
                                                  RD_KAFKA_PARTITION_UA);
        }

        ut_initMetadataConditionalRack0(&metadata, 3, 3, ALL_RACKS,
                                        RD_ARRAYSIZE(ALL_RACKS),
                                        parametrization, mt, topic_cnt);

        for (i = 1; i <= member_cnt; i++) {
                char name[16];
                rd_snprintf(name, sizeof(name), "consumer%d", i);
                ut_initMemberConditionalRack(
                    &members[i - 1], name,
                    ut_get_consumer_rack(i, parametrization), parametrization,
                    NULL);
                rd_kafka_topic_partition_list_destroy(
                    members[i - 1].rkgm_subscription);
                members[i - 1].rkgm_subscription =
                    rd_kafka_topic_partition_list_copy(subscription);
        }

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, member_cnt, metadata);

        /*
         * Remove consumer5
         */
        rd_kafka_group_member_clear(&members[5]);
        memmove(&members[5], &members[6], sizeof(*members) * (member_cnt - 6));
        member_cnt--;

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, member_cnt, metadata);
        // FIXME: isSticky();

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);
        rd_kafka_topic_partition_list_destroy(subscription);

        RD_UT_PASS();
}


static int ut_testLargeAssignmentWithMultipleConsumersLeaving(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        if (rd_unittest_with_valgrind)
                RD_UT_SKIP(
                    "Skipping large assignment test when using Valgrind");

        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[200];
        int member_cnt = RD_ARRAYSIZE(members);
        rd_kafka_metadata_topic_t mt[40];
        int topic_cnt = RD_ARRAYSIZE(mt);
        int i;

        for (i = 0; i < topic_cnt; i++) {
                char topic[10];
                rd_snprintf(topic, sizeof(topic), "topic%d", i + 1);
                rd_strdupa(&mt[i].topic, topic);
                mt[i].partition_cnt = i + 1;
        }

        ut_initMetadataConditionalRack0(&metadata, 3, 3, ALL_RACKS,
                                        RD_ARRAYSIZE(ALL_RACKS),
                                        parametrization, mt, topic_cnt);

        for (i = 0; i < member_cnt; i++) {
                /* Java tests use a random set, this is more deterministic. */
                int sub_cnt = ((i + 1) * 17) % topic_cnt;
                rd_kafka_topic_partition_list_t *subscription =
                    rd_kafka_topic_partition_list_new(sub_cnt);
                char name[16];
                int j;

                /* Subscribe to a subset of topics */
                for (j = 0; j < sub_cnt; j++)
                        rd_kafka_topic_partition_list_add(
                            subscription, metadata->topics[j].topic,
                            RD_KAFKA_PARTITION_UA);

                rd_snprintf(name, sizeof(name), "consumer%d", i + 1);
                ut_initMemberConditionalRack(
                    &members[i], name, ut_get_consumer_rack(i, parametrization),
                    parametrization, NULL);

                rd_kafka_topic_partition_list_destroy(
                    members[i].rkgm_subscription);
                members[i].rkgm_subscription = subscription;
        }

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, member_cnt, metadata);

        /*
         * Remove every 4th consumer (~50)
         */
        for (i = member_cnt - 1; i >= 0; i -= 4) {
                rd_kafka_group_member_clear(&members[i]);
                memmove(&members[i], &members[i + 1],
                        sizeof(*members) * (member_cnt - (i + 1)));
                member_cnt--;
        }

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, member_cnt, metadata);
        // FIXME: isSticky();

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int
ut_testNewSubscription(rd_kafka_t *rk,
                       const rd_kafka_assignor_t *rkas,
                       rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[3];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       5, "topic1", 1, "topic2", 2, "topic3", 3,
                                       "topic4", 4, "topic5", 5);

        for (i = 0; i < member_cnt; i++) {
                char name[16];
                int j;

                rd_snprintf(name, sizeof(name), "consumer%d", i);
                ut_initMemberConditionalRack(
                    &members[i], name, ut_get_consumer_rack(i, parametrization),
                    parametrization, NULL);

                rd_kafka_topic_partition_list_destroy(
                    members[i].rkgm_subscription);
                members[i].rkgm_subscription =
                    rd_kafka_topic_partition_list_new(5);

                for (j = metadata->topic_cnt - (1 + i); j >= 0; j--)
                        rd_kafka_topic_partition_list_add(
                            members[i].rkgm_subscription,
                            metadata->topics[j].topic, RD_KAFKA_PARTITION_UA);
        }

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

        /*
         * Add topic1 to consumer1's subscription
         */
        RD_UT_SAY("Adding topic1 to consumer1");
        rd_kafka_topic_partition_list_add(members[0].rkgm_subscription,
                                          "topic1", RD_KAFKA_PARTITION_UA);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));
        // FIXME: isSticky();

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int ut_testMoveExistingAssignments(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[4];
        int member_cnt                                  = RD_ARRAYSIZE(members);
        rd_kafka_topic_partition_list_t *assignments[4] = RD_ZERO_INIT;
        int i;
        int fails = 0;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "topic1", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);
        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", NULL);
        ut_initMemberConditionalRack(&members[2], "consumer3",
                                     ut_get_consumer_rack(2, parametrization),
                                     parametrization, "topic1", NULL);
        ut_initMemberConditionalRack(&members[3], "consumer4",
                                     ut_get_consumer_rack(3, parametrization),
                                     parametrization, "topic1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, member_cnt, metadata);

        for (i = 0; i < member_cnt; i++) {
                if (members[i].rkgm_assignment->cnt > 1) {
                        RD_UT_WARN("%s assigned %d partitions, expected <= 1",
                                   members[i].rkgm_member_id->str,
                                   members[i].rkgm_assignment->cnt);
                        fails++;
                } else if (members[i].rkgm_assignment->cnt == 1) {
                        assignments[i] = rd_kafka_topic_partition_list_copy(
                            members[i].rkgm_assignment);
                }
        }

        /*
         * Remove potential group leader consumer1
         */
        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, &members[1],
                                    member_cnt - 1, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(&members[1], member_cnt - 1, metadata);
        // FIXME: isSticky()

        for (i = 1; i < member_cnt; i++) {
                if (members[i].rkgm_assignment->cnt != 1) {
                        RD_UT_WARN("%s assigned %d partitions, expected 1",
                                   members[i].rkgm_member_id->str,
                                   members[i].rkgm_assignment->cnt);
                        fails++;
                } else if (assignments[i] &&
                           !rd_kafka_topic_partition_list_find(
                               assignments[i],
                               members[i].rkgm_assignment->elems[0].topic,
                               members[i]
                                   .rkgm_assignment->elems[0]
                                   .partition)) {
                        RD_UT_WARN(
                            "Stickiness was not honored for %s, "
                            "%s [%" PRId32 "] not in previous assignment",
                            members[i].rkgm_member_id->str,
                            members[i].rkgm_assignment->elems[0].topic,
                            members[i].rkgm_assignment->elems[0].partition);
                        fails++;
                }
        }

        RD_UT_ASSERT(!fails, "See previous errors");


        for (i = 0; i < member_cnt; i++) {
                rd_kafka_group_member_clear(&members[i]);
                if (assignments[i])
                        rd_kafka_topic_partition_list_destroy(assignments[i]);
        }
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


/* The original version of this test diverged from the Java implementaion in
 * what it was testing. It's not certain whether it was by mistake, or by
 * design, but the new version matches the Java implementation, and the old one
 * is retained as well, since it provides extra coverage.
 */
static int ut_testMoveExistingAssignments_j(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[3];
        int member_cnt                                  = RD_ARRAYSIZE(members);
        rd_kafka_topic_partition_list_t *assignments[4] = RD_ZERO_INIT;
        int i;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       6, "topic1", 1, "topic2", 1, "topic3", 1,
                                       "topic4", 1, "topic5", 1, "topic6", 1);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", "topic2", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[0], 1 /* generation */, 1, "topic1", 0);

        ut_initMemberConditionalRack(
            &members[1], "consumer2", ut_get_consumer_rack(1, parametrization),
            parametrization, "topic1", "topic2", "topic3", "topic4", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[1], 1 /* generation */, 2, "topic2", 0, "topic3", 0);

        ut_initMemberConditionalRack(&members[2], "consumer3",
                                     ut_get_consumer_rack(2, parametrization),
                                     parametrization, "topic2", "topic3",
                                     "topic4", "topic5", "topic6", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[2], 1 /* generation */, 3, "topic4", 0, "topic5", 0,
            "topic6", 0);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, member_cnt, metadata);

        for (i = 0; i < member_cnt; i++) {
                rd_kafka_group_member_clear(&members[i]);
                if (assignments[i])
                        rd_kafka_topic_partition_list_destroy(assignments[i]);
        }
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int
ut_testStickiness(rd_kafka_t *rk,
                  const rd_kafka_assignor_t *rkas,
                  rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[3];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       6, "topic1", 1, "topic2", 1, "topic3", 1,
                                       "topic4", 1, "topic5", 1, "topic6", 1);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", "topic2", NULL);
        rd_kafka_topic_partition_list_destroy(members[0].rkgm_assignment);
        members[0].rkgm_assignment = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(members[0].rkgm_assignment, "topic1",
                                          0);

        ut_initMemberConditionalRack(
            &members[1], "consumer2", ut_get_consumer_rack(1, parametrization),
            parametrization, "topic1", "topic2", "topic3", "topic4", NULL);
        rd_kafka_topic_partition_list_destroy(members[1].rkgm_assignment);
        members[1].rkgm_assignment = rd_kafka_topic_partition_list_new(2);
        rd_kafka_topic_partition_list_add(members[1].rkgm_assignment, "topic2",
                                          0);
        rd_kafka_topic_partition_list_add(members[1].rkgm_assignment, "topic3",
                                          0);

        ut_initMemberConditionalRack(
            &members[2], "consumer3", ut_get_consumer_rack(1, parametrization),
            parametrization, "topic4", "topic5", "topic6", NULL);
        rd_kafka_topic_partition_list_destroy(members[2].rkgm_assignment);
        members[2].rkgm_assignment = rd_kafka_topic_partition_list_new(3);
        rd_kafka_topic_partition_list_add(members[2].rkgm_assignment, "topic4",
                                          0);
        rd_kafka_topic_partition_list_add(members[2].rkgm_assignment, "topic5",
                                          0);
        rd_kafka_topic_partition_list_add(members[2].rkgm_assignment, "topic6",
                                          0);


        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


/* The original version of this test diverged from the Java implementaion in
 * what it was testing. It's not certain whether it was by mistake, or by
 * design, but the new version matches the Java implementation, and the old one
 * is retained as well, for extra coverage.
 */
static int
ut_testStickiness_j(rd_kafka_t *rk,
                    const rd_kafka_assignor_t *rkas,
                    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[4];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;
        rd_kafka_topic_partition_list_t *assignments[4] = RD_ZERO_INIT;
        int fails                                       = 0;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "topic1", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);
        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", NULL);
        ut_initMemberConditionalRack(&members[2], "consumer3",
                                     ut_get_consumer_rack(2, parametrization),
                                     parametrization, "topic1", NULL);
        ut_initMemberConditionalRack(&members[3], "consumer4",
                                     ut_get_consumer_rack(3, parametrization),
                                     parametrization, "topic1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, member_cnt, metadata);

        for (i = 0; i < member_cnt; i++) {
                if (members[i].rkgm_assignment->cnt > 1) {
                        RD_UT_WARN("%s assigned %d partitions, expected <= 1",
                                   members[i].rkgm_member_id->str,
                                   members[i].rkgm_assignment->cnt);
                        fails++;
                } else if (members[i].rkgm_assignment->cnt == 1) {
                        assignments[i] = rd_kafka_topic_partition_list_copy(
                            members[i].rkgm_assignment);
                }
        }

        /*
         * Remove potential group leader consumer1, by starting members at
         * index 1.
         * Owned partitions of the members are already set to the assignment by
         * verifyValidityAndBalance above to simulate the fact that the assignor
         * has already run once.
         */
        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, &members[1],
                                    member_cnt - 1, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(&members[1], member_cnt - 1, metadata);
        // FIXME: isSticky()

        for (i = 1; i < member_cnt; i++) {
                if (members[i].rkgm_assignment->cnt != 1) {
                        RD_UT_WARN("%s assigned %d partitions, expected 1",
                                   members[i].rkgm_member_id->str,
                                   members[i].rkgm_assignment->cnt);
                        fails++;
                } else if (assignments[i] &&
                           !rd_kafka_topic_partition_list_find(
                               assignments[i],
                               members[i].rkgm_assignment->elems[0].topic,
                               members[i]
                                   .rkgm_assignment->elems[0]
                                   .partition)) {
                        RD_UT_WARN(
                            "Stickiness was not honored for %s, "
                            "%s [%" PRId32 "] not in previous assignment",
                            members[i].rkgm_member_id->str,
                            members[i].rkgm_assignment->elems[0].topic,
                            members[i].rkgm_assignment->elems[0].partition);
                        fails++;
                }
        }

        RD_UT_ASSERT(!fails, "See previous errors");


        for (i = 0; i < member_cnt; i++) {
                rd_kafka_group_member_clear(&members[i]);
                if (assignments[i])
                        rd_kafka_topic_partition_list_destroy(assignments[i]);
        }
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


/**
 * @brief Verify stickiness across three rebalances.
 */
static int
ut_testStickiness2(rd_kafka_t *rk,
                   const rd_kafka_assignor_t *rkas,
                   rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[3];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "topic1", 6);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);
        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", NULL);
        ut_initMemberConditionalRack(&members[2], "consumer3",
                                     ut_get_consumer_rack(2, parametrization),
                                     parametrization, "topic1", NULL);

        /* Just consumer1 */
        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members, 1,
                                    errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, 1, metadata);
        isFullyBalanced(members, 1);
        verifyAssignment(&members[0], "topic1", 0, "topic1", 1, "topic1", 2,
                         "topic1", 3, "topic1", 4, "topic1", 5, NULL);

        /* consumer1 and consumer2 */
        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members, 2,
                                    errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, 2, metadata);
        isFullyBalanced(members, 2);
        verifyAssignment(&members[0], "topic1", 3, "topic1", 4, "topic1", 5,
                         NULL);
        verifyAssignment(&members[1], "topic1", 0, "topic1", 1, "topic1", 2,
                         NULL);

        /* Run it twice, should be stable. */
        for (i = 0; i < 2; i++) {
                /* consumer1, consumer2, and consumer3 */
                err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata,
                                            members, 3, errstr, sizeof(errstr));
                RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

                verifyValidityAndBalance(members, 3, metadata);
                isFullyBalanced(members, 3);
                verifyAssignment(&members[0], "topic1", 4, "topic1", 5, NULL);
                verifyAssignment(&members[1], "topic1", 1, "topic1", 2, NULL);
                verifyAssignment(&members[2], "topic1", 0, "topic1", 3, NULL);
        }

        /* Remove consumer1 */
        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, &members[1], 2,
                                    errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(&members[1], 2, metadata);
        isFullyBalanced(&members[1], 2);
        verifyAssignment(&members[1], "topic1", 1, "topic1", 2, "topic1", 5,
                         NULL);
        verifyAssignment(&members[2], "topic1", 0, "topic1", 3, "topic1", 4,
                         NULL);

        /* Remove consumer2 */
        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, &members[2], 1,
                                    errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(&members[2], 1, metadata);
        isFullyBalanced(&members[2], 1);
        verifyAssignment(&members[2], "topic1", 0, "topic1", 1, "topic1", 2,
                         "topic1", 3, "topic1", 4, "topic1", 5, NULL);

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int ut_testAssignmentUpdatedForDeletedTopic(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[1];

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       2, "topic1", 1, "topic3", 100);

        ut_initMemberConditionalRack(
            &members[0], "consumer1", ut_get_consumer_rack(0, parametrization),
            parametrization, "topic1", "topic2", "topic3", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

        RD_UT_ASSERT(members[0].rkgm_assignment->cnt == 1 + 100,
                     "Expected %d assigned partitions, not %d", 1 + 100,
                     members[0].rkgm_assignment->cnt);

        rd_kafka_group_member_clear(&members[0]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int ut_testNoExceptionThrownWhenOnlySubscribedTopicDeleted(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[1];

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "topic1", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);


        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

        /*
         * Remove topic
         */
        ut_destroy_metadata(metadata);
        metadata = rd_kafka_metadata_new_topic_mock(NULL, 0, -1, 0);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    RD_ARRAYSIZE(members), errstr,
                                    sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));

        rd_kafka_group_member_clear(&members[0]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int ut_testConflictingPreviousAssignments(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[2];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;

        // FIXME: removed from Java test suite, and fails for us, why, why?
        // NOTE: rack-awareness changes aren't made to this test because of
        // the FIXME above.
        RD_UT_PASS();

        metadata = rd_kafka_metadata_new_topic_mockv(1, "topic1", 2);

        /* Both consumer and consumer2 have both partitions assigned */
        ut_init_member(&members[0], "consumer1", "topic1", NULL);
        rd_kafka_topic_partition_list_destroy(members[0].rkgm_assignment);
        members[0].rkgm_assignment = rd_kafka_topic_partition_list_new(2);
        rd_kafka_topic_partition_list_add(members[0].rkgm_assignment, "topic1",
                                          0);
        rd_kafka_topic_partition_list_add(members[0].rkgm_assignment, "topic1",
                                          1);

        ut_init_member(&members[1], "consumer2", "topic1", NULL);
        rd_kafka_topic_partition_list_destroy(members[1].rkgm_assignment);
        members[1].rkgm_assignment = rd_kafka_topic_partition_list_new(2);
        rd_kafka_topic_partition_list_add(members[1].rkgm_assignment, "topic1",
                                          0);
        rd_kafka_topic_partition_list_add(members[1].rkgm_assignment, "topic1",
                                          1);


        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        RD_UT_ASSERT(members[0].rkgm_assignment->cnt == 1 &&
                         members[1].rkgm_assignment->cnt == 1,
                     "Expected consumers to have 1 partition each, "
                     "not %d and %d",
                     members[0].rkgm_assignment->cnt,
                     members[1].rkgm_assignment->cnt);
        RD_UT_ASSERT(members[0].rkgm_assignment->elems[0].partition !=
                         members[1].rkgm_assignment->elems[0].partition,
                     "Expected consumers to have different partitions "
                     "assigned, not same partition %" PRId32,
                     members[0].rkgm_assignment->elems[0].partition);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        isFullyBalanced(members, RD_ARRAYSIZE(members));
        /* FIXME: isSticky() */

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

/* testReassignmentWithRandomSubscriptionsAndChanges is not ported
 * from Java since random tests don't provide meaningful test coverage. */


static int ut_testAllConsumersReachExpectedQuotaAndAreConsideredFilled(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[3];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "topic1", 4);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[0], 1 /* generation */, 2, "topic1", 0, "topic1", 1);

        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[1], 1 /* generation */, 1, "topic1", 2);

        ut_initMemberConditionalRack(&members[2], "consumer3",
                                     ut_get_consumer_rack(2, parametrization),
                                     parametrization, "topic1", NULL);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        verifyAssignment(&members[0], "topic1", 0, "topic1", 1, NULL);
        verifyAssignment(&members[1], "topic1", 2, NULL);
        verifyAssignment(&members[2], "topic1", 3, NULL);

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int ut_testOwnedPartitionsAreInvalidatedForConsumerWithStaleGeneration(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[2];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;
        int current_generation = 10;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       2, "topic1", 3, "topic2", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", "topic2", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[0], current_generation, 3, "topic1", 0, "topic1", 2,
            "topic2", 1);

        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", "topic2", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[1], current_generation - 1, 3, "topic1", 0, "topic1", 2,
            "topic2", 1);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        verifyAssignment(&members[0], "topic1", 0, "topic1", 2, "topic2", 1,
                         NULL);
        verifyAssignment(&members[1], "topic1", 1, "topic2", 0, "topic2", 2,
                         NULL);


        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

static int ut_testOwnedPartitionsAreInvalidatedForConsumerWithNoGeneration(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[2];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;
        int current_generation = 10;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       2, "topic1", 3, "topic2", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", "topic2", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[0], current_generation, 3, "topic1", 0, "topic1", 2,
            "topic2", 1);

        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", "topic2", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[1], -1 /* default generation*/, 3, "topic1", 0, "topic1",
            2, "topic2", 1);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        verifyAssignment(&members[0], "topic1", 0, "topic1", 2, "topic2", 1,
                         NULL);
        verifyAssignment(&members[1], "topic1", 1, "topic2", 0, "topic2", 2,
                         NULL);


        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

static int
ut_testPartitionsTransferringOwnershipIncludeThePartitionClaimedByMultipleConsumersInSameGeneration(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[3];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "topic1", 3);

        // partition topic-0 is owned by multiple consumers
        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[0], 1 /* generation */, 2, "topic1", 0, "topic1", 1);

        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[1], 1 /* generation */, 2, "topic1", 0, "topic1", 2);

        ut_initMemberConditionalRack(&members[2], "consumer3",
                                     ut_get_consumer_rack(2, parametrization),
                                     parametrization, "topic1", NULL);


        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        verifyAssignment(&members[0], "topic1", 1, NULL);
        verifyAssignment(&members[1], "topic1", 2, NULL);
        verifyAssignment(&members[2], "topic1", 0, NULL);

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


/* In Java, there is a way to check what partition transferred ownership.
 * We don't have anything like that for our UTs, so in lieue of that, this
 * test is added along with the previous test to make sure that we move the
 * right partition. Our solution in case of two consumers owning the same
 * partitions with the same generation id was differing from the Java
 * implementation earlier. (Check #4252.) */
static int
ut_testPartitionsTransferringOwnershipIncludeThePartitionClaimedByMultipleConsumersInSameGeneration2(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[3];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       1, "topic1", 3);

        // partition topic-0 is owned by multiple consumers
        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[0], 1 /* generation */, 2, "topic1", 0, "topic1", 1);

        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[1], 1 /* generation */, 2, "topic1", 1, "topic1", 2);

        ut_initMemberConditionalRack(&members[2], "consumer3",
                                     ut_get_consumer_rack(2, parametrization),
                                     parametrization, "topic1", NULL);


        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);
        verifyAssignment(&members[0], "topic1", 0, NULL);
        verifyAssignment(&members[1], "topic1", 2, NULL);
        verifyAssignment(&members[2], "topic1", 1, NULL);

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int ut_testEnsurePartitionsAssignedToHighestGeneration(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[3];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;
        int currentGeneration = 10;

        ut_initMetadataConditionalRack(
            &metadata, 3, 3, ALL_RACKS, RD_ARRAYSIZE(ALL_RACKS),
            parametrization, 3, "topic1", 3, "topic2", 3, "topic3", 3);

        ut_initMemberConditionalRack(
            &members[0], "consumer1", ut_get_consumer_rack(0, parametrization),
            parametrization, "topic1", "topic2", "topic3", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[0], currentGeneration, 3, "topic1", 0, "topic2", 0,
            "topic3", 0);


        ut_initMemberConditionalRack(
            &members[1], "consumer2", ut_get_consumer_rack(1, parametrization),
            parametrization, "topic1", "topic2", "topic3", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[1], currentGeneration - 1, 3, "topic1", 1, "topic2", 1,
            "topic3", 1);


        ut_initMemberConditionalRack(
            &members[2], "consumer3", ut_get_consumer_rack(2, parametrization),
            parametrization, "topic1", "topic2", "topic3", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[2], currentGeneration - 2, 3, "topic2", 1, "topic3", 0,
            "topic3", 2);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);
        verifyAssignment(&members[0], "topic1", 0, "topic2", 0, "topic3", 0,
                         NULL);
        verifyAssignment(&members[1], "topic1", 1, "topic2", 1, "topic3", 1,
                         NULL);
        verifyAssignment(&members[2], "topic1", 2, "topic2", 2, "topic3", 2,
                         NULL);

        verifyValidityAndBalance(members, RD_ARRAYSIZE(members), metadata);

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int ut_testNoReassignmentOnCurrentMembers(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[4];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;
        int currentGeneration = 10;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       4, "topic0", 3, "topic1", 3, "topic2", 3,
                                       "topic3", 3);

        ut_initMemberConditionalRack(
            &members[0], "consumer1", ut_get_consumer_rack(0, parametrization),
            parametrization, "topic0", "topic1", "topic2", "topic3", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[0], -1 /* default generation */, 0);

        ut_initMemberConditionalRack(
            &members[1], "consumer2", ut_get_consumer_rack(1, parametrization),
            parametrization, "topic0", "topic1", "topic2", "topic3", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[1], currentGeneration - 1, 3, "topic0", 0, "topic2", 0,
            "topic1", 0);

        ut_initMemberConditionalRack(
            &members[2], "consumer3", ut_get_consumer_rack(2, parametrization),
            parametrization, "topic0", "topic1", "topic2", "topic3", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[2], currentGeneration - 2, 3, "topic3", 2, "topic2", 2,
            "topic1", 1);

        ut_initMemberConditionalRack(
            &members[3], "consumer4", ut_get_consumer_rack(3, parametrization),
            parametrization, "topic0", "topic1", "topic2", "topic3", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[3], currentGeneration - 3, 3, "topic3", 1, "topic0", 1,
            "topic0", 2);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, member_cnt, metadata);
        verifyAssignment(&members[0], "topic1", 2, "topic2", 1, "topic3", 0,
                         NULL);

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}


static int
ut_testOwnedPartitionsAreInvalidatedForConsumerWithMultipleGeneration(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata;
        rd_kafka_group_member_t members[2];
        int member_cnt = RD_ARRAYSIZE(members);
        int i;
        int currentGeneration = 10;

        ut_initMetadataConditionalRack(&metadata, 3, 3, ALL_RACKS,
                                       RD_ARRAYSIZE(ALL_RACKS), parametrization,
                                       2, "topic1", 3, "topic2", 3);

        ut_initMemberConditionalRack(&members[0], "consumer1",
                                     ut_get_consumer_rack(0, parametrization),
                                     parametrization, "topic1", "topic2", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[0], currentGeneration, 3, "topic1", 0, "topic2", 1,
            "topic1", 1);

        ut_initMemberConditionalRack(&members[1], "consumer2",
                                     ut_get_consumer_rack(1, parametrization),
                                     parametrization, "topic1", "topic2", NULL);
        ut_populate_member_owned_partitions_generation(
            &members[1], currentGeneration - 2, 3, "topic1", 0, "topic2", 1,
            "topic2", 2);

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        verifyValidityAndBalance(members, member_cnt, metadata);
        verifyAssignment(&members[0], "topic1", 0, "topic2", 1, "topic1", 1,
                         NULL);
        verifyAssignment(&members[1], "topic1", 2, "topic2", 2, "topic2", 0,
                         NULL);

        for (i = 0; i < member_cnt; i++)
                rd_kafka_group_member_clear(&members[i]);
        ut_destroy_metadata(metadata);

        RD_UT_PASS();
}

/* Helper for setting up metadata and members, and running the assignor, and
 * verifying validity and balance of the assignment. Does not check the results
 * of the assignment on a per member basis..
 */
static int
setupRackAwareAssignment0(rd_kafka_t *rk,
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
                          rd_kafka_topic_partition_list_t **owned_tp_list,
                          rd_bool_t initialize_members,
                          rd_kafka_metadata_t **metadata) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_metadata_t *metadata_local = NULL;

        size_t i              = 0;
        const int num_brokers = num_broker_racks > 0
                                    ? replication_factor * num_broker_racks
                                    : replication_factor;
        if (!metadata)
                metadata = &metadata_local;

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

        for (i = 0; initialize_members && i < member_cnt; i++) {
                char member_id[10];
                snprintf(member_id, 10, "consumer%d", (int)(i + 1));
                ut_init_member_with_rack(
                    &members[i], member_id, ALL_RACKS[consumer_racks[i]],
                    subscriptions[i], subscriptions_count[i]);

                if (!owned_tp_list || !owned_tp_list[i])
                        continue;

                if (members[i].rkgm_owned)
                        rd_kafka_topic_partition_list_destroy(
                            members[i].rkgm_owned);

                members[i].rkgm_owned =
                    rd_kafka_topic_partition_list_copy(owned_tp_list[i]);
        }

        err = rd_kafka_assignor_run(rk->rk_cgrp, rkas, *metadata, members,
                                    member_cnt, errstr, sizeof(errstr));
        RD_UT_ASSERT(!err, "assignor run failed: %s", errstr);

        /* Note that verifyValidityAndBalance also sets rkgm_owned for each
         * member to rkgm_assignment, so if the members are used without
         * clearing, in another assignor_run, the result should be stable. */
        verifyValidityAndBalance(members, member_cnt, *metadata);

        if (metadata_local)
                ut_destroy_metadata(metadata_local);
        return 0;
}

static int
setupRackAwareAssignment(rd_kafka_t *rk,
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
                         rd_kafka_topic_partition_list_t **owned_tp_list,
                         rd_bool_t initialize_members) {
        return setupRackAwareAssignment0(
            rk, rkas, members, member_cnt, replication_factor, num_broker_racks,
            topic_cnt, topics, partitions, subscriptions_count, subscriptions,
            consumer_racks, owned_tp_list, initialize_members, NULL);
}

/* Helper for testing cases where rack-aware assignment should not be triggered,
 * and assignment should be the same as the pre-rack-aware assignor. Each case
 * is run twice, once with owned partitions set to empty, and in the second
 * case, with owned partitions set to the result of the previous run, to check
 * that the assignment is stable. */
#define verifyNonRackAwareAssignment(rk, rkas, members, member_cnt, topic_cnt, \
                                     topics, partitions, subscriptions_count,  \
                                     subscriptions, ...)                       \
        do {                                                                   \
                size_t idx       = 0;                                          \
                int init_members = 1;                                          \
                rd_kafka_metadata_t *metadata;                                 \
                                                                               \
                /* num_broker_racks = 0, implies that brokers have no          \
                 * configured racks. */                                        \
                for (init_members = 1; init_members >= 0; init_members--) {    \
                        setupRackAwareAssignment(                              \
                            rk, rkas, members, member_cnt, 3, 0, topic_cnt,    \
                            topics, partitions, subscriptions_count,           \
                            subscriptions, RACKS_INITIAL, NULL, init_members); \
                        verifyMultipleAssignment(members, member_cnt,          \
                                                 __VA_ARGS__);                 \
                }                                                              \
                for (idx = 0; idx < member_cnt; idx++)                         \
                        rd_kafka_group_member_clear(&members[idx]);            \
                /* consumer_racks = RACKS_NULL implies that consumers have no  \
                 * racks. */                                                   \
                for (init_members = 1; init_members >= 0; init_members--) {    \
                        setupRackAwareAssignment(                              \
                            rk, rkas, members, member_cnt, 3, 3, topic_cnt,    \
                            topics, partitions, subscriptions_count,           \
                            subscriptions, RACKS_NULL, NULL, init_members);    \
                        verifyMultipleAssignment(members, member_cnt,          \
                                                 __VA_ARGS__);                 \
                }                                                              \
                for (idx = 0; idx < member_cnt; idx++)                         \
                        rd_kafka_group_member_clear(&members[idx]);            \
                /* replication_factor = 3 and num_broker_racks = 3 means that  \
                 * all partitions are replicated on all racks.*/               \
                for (init_members = 1; init_members >= 0; init_members--) {    \
                        setupRackAwareAssignment0(                             \
                            rk, rkas, members, member_cnt, 3, 3, topic_cnt,    \
                            topics, partitions, subscriptions_count,           \
                            subscriptions, RACKS_INITIAL, NULL, init_members,  \
                            &metadata);                                        \
                        verifyMultipleAssignment(members, member_cnt,          \
                                                 __VA_ARGS__);                 \
                        verifyNumPartitionsWithRackMismatch(                   \
                            metadata, members, RD_ARRAYSIZE(members), 0);      \
                        ut_destroy_metadata(metadata);                         \
                }                                                              \
                for (idx = 0; idx < member_cnt; idx++)                         \
                        rd_kafka_group_member_clear(&members[idx]);            \
                /* replication_factor = 4 and num_broker_racks = 4 means that  \
                 * all partitions are replicated on all racks. */              \
                for (init_members = 1; init_members >= 0; init_members--) {    \
                        setupRackAwareAssignment0(                             \
                            rk, rkas, members, member_cnt, 4, 4, topic_cnt,    \
                            topics, partitions, subscriptions_count,           \
                            subscriptions, RACKS_INITIAL, NULL, init_members,  \
                            &metadata);                                        \
                        verifyMultipleAssignment(members, member_cnt,          \
                                                 __VA_ARGS__);                 \
                        verifyNumPartitionsWithRackMismatch(                   \
                            metadata, members, RD_ARRAYSIZE(members), 0);      \
                        ut_destroy_metadata(metadata);                         \
                }                                                              \
                for (idx = 0; idx < member_cnt; idx++)                         \
                        rd_kafka_group_member_clear(&members[idx]);            \
                /* There's no overap between broker racks and consumer racks,  \
                 * since num_broker_racks = 3, they'll be picked from a,b,c    \
                 * and consumer racks are d,e,f. */                            \
                for (init_members = 1; init_members >= 0; init_members--) {    \
                        setupRackAwareAssignment(                              \
                            rk, rkas, members, member_cnt, 3, 3, topic_cnt,    \
                            topics, partitions, subscriptions_count,           \
                            subscriptions, RACKS_FINAL, NULL, init_members);   \
                        verifyMultipleAssignment(members, member_cnt,          \
                                                 __VA_ARGS__);                 \
                }                                                              \
                for (idx = 0; idx < member_cnt; idx++)                         \
                        rd_kafka_group_member_clear(&members[idx]);            \
                /* There's no overap between broker racks and consumer racks,  \
                 * since num_broker_racks = 3, they'll be picked from a,b,c    \
                 * and consumer racks are d,e,NULL. */                         \
                for (init_members = 1; init_members >= 0; init_members--) {    \
                        setupRackAwareAssignment(                              \
                            rk, rkas, members, member_cnt, 3, 3, topic_cnt,    \
                            topics, partitions, subscriptions_count,           \
                            subscriptions, RACKS_ONE_NULL, NULL,               \
                            init_members);                                     \
                        verifyMultipleAssignment(members, member_cnt,          \
                                                 __VA_ARGS__);                 \
                }                                                              \
                for (idx = 0; idx < member_cnt; idx++)                         \
                        rd_kafka_group_member_clear(&members[idx]);            \
        } while (0)


static int ut_testRackAwareAssignmentWithUniformSubscription(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        char *topics[]   = {"t1", "t2", "t3"};
        int partitions[] = {6, 7, 2};
        rd_kafka_group_member_t members[3];
        size_t member_cnt         = RD_ARRAYSIZE(members);
        size_t i                  = 0;
        int subscriptions_count[] = {3, 3, 3};
        char **subscriptions[]    = {topics, topics, topics};
        int init_members          = 0;
        rd_kafka_topic_partition_list_t **owned;
        rd_kafka_metadata_t *metadata;

        if (parametrization !=
            RD_KAFKA_RANGE_ASSIGNOR_UT_BROKER_AND_CONSUMER_RACK) {
                RD_UT_PASS();
        }

        verifyNonRackAwareAssignment(
            rk, rkas, members, RD_ARRAYSIZE(members), RD_ARRAYSIZE(topics),
            topics, partitions, subscriptions_count, subscriptions,
            /* consumer1 */
            "t1", 0, "t1", 3, "t2", 0, "t2", 3, "t2", 6, NULL,
            /* consumer2 */
            "t1", 1, "t1", 4, "t2", 1, "t2", 4, "t3", 0, NULL,
            /* consumer3 */
            "t1", 2, "t1", 5, "t2", 2, "t2", 5, "t3", 1, NULL);

        /* Verify assignment is rack-aligned for lower replication factor where
         * brokers have a subset of partitions */
        for (init_members = 1; init_members >= 0; init_members--) {
                setupRackAwareAssignment0(
                    rk, rkas, members, RD_ARRAYSIZE(members), 1, 3,
                    RD_ARRAYSIZE(topics), topics, partitions,
                    subscriptions_count, subscriptions, RACKS_INITIAL, NULL,
                    init_members, &metadata);
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
                ut_destroy_metadata(metadata);
        }
        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);


        for (init_members = 1; init_members >= 0; init_members--) {
                setupRackAwareAssignment0(
                    rk, rkas, members, RD_ARRAYSIZE(members), 2, 3,
                    RD_ARRAYSIZE(topics), topics, partitions,
                    subscriptions_count, subscriptions, RACKS_INITIAL, NULL,
                    init_members, &metadata);
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
                ut_destroy_metadata(metadata);
        }
        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);

        /* One consumer on a rack with no partitions. We allocate with
         * misaligned rack to this consumer to maintain balance. */
        for (init_members = 1; init_members >= 0; init_members--) {
                setupRackAwareAssignment0(
                    rk, rkas, members, RD_ARRAYSIZE(members), 3, 2,
                    RD_ARRAYSIZE(topics), topics, partitions,
                    subscriptions_count, subscriptions, RACKS_INITIAL, NULL,
                    init_members, &metadata);
                verifyMultipleAssignment(
                    members, RD_ARRAYSIZE(members),
                    /* consumer1 */
                    "t1", 0, "t1", 3, "t2", 0, "t2", 3, "t2", 6, NULL,
                    /* consumer2 */
                    "t1", 1, "t1", 4, "t2", 1, "t2", 4, "t3", 0, NULL,
                    /* consumer3 */
                    "t1", 2, "t1", 5, "t2", 2, "t2", 5, "t3", 1, NULL);
                verifyNumPartitionsWithRackMismatch(metadata, members,
                                                    RD_ARRAYSIZE(members), 5);
                ut_destroy_metadata(metadata);
        }
        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);

        /* Verify that rack-awareness is improved if already owned partitions
         * are misaligned */
        owned = ut_create_topic_partition_lists(
            3,
            /* consumer1 */
            "t1", 0, "t1", 1, "t1", 2, "t1", 3, "t1", 4, NULL,
            /* consumer2 */
            "t1", 5, "t2", 0, "t2", 1, "t2", 2, "t2", 3, NULL,
            /* consumer3 */
            "t2", 4, "t2", 5, "t2", 6, "t3", 0, "t3", 1, NULL);

        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 1,
                                  3, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions,
                                  RACKS_INITIAL, owned, rd_true, &metadata);
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
        ut_destroy_metadata(metadata);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        for (i = 0; i < member_cnt; i++)
                rd_kafka_topic_partition_list_destroy(owned[i]);
        rd_free(owned);


        /* Verify that stickiness is retained when racks match */
        owned = ut_create_topic_partition_lists(
            3,
            /* consumer1 */
            "t1", 0, "t1", 3, "t2", 0, "t2", 3, "t2", 6, NULL,
            /* consumer2 */
            "t1", 1, "t1", 4, "t2", 1, "t2", 4, "t3", 0, NULL,
            /* consumer3 */
            "t1", 2, "t1", 5, "t2", 2, "t2", 5, "t3", 1, NULL);

        /* This test deviates slightly from Java, in that we test with two
         * additional replication factors, 1 and 2, which are not tested in
         * Java. This is because in Java, there is a way to turn rack aware
         * logic on or off for tests. We don't have that, and to test with rack
         * aware logic, we need to change something, in this case, the
         * replication factor. */
        for (i = 1; i <= 3; i++) {
                setupRackAwareAssignment0(
                    rk, rkas, members, RD_ARRAYSIZE(members),
                    i /* replication factor */, 3, RD_ARRAYSIZE(topics), topics,
                    partitions, subscriptions_count, subscriptions,
                    RACKS_INITIAL, owned, rd_true, &metadata);
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
        }

        for (i = 0; i < member_cnt; i++)
                rd_kafka_topic_partition_list_destroy(owned[i]);
        rd_free(owned);

        RD_UT_PASS();
}


static int ut_testRackAwareAssignmentWithNonEqualSubscription(
    rd_kafka_t *rk,
    const rd_kafka_assignor_t *rkas,
    rd_kafka_assignor_ut_rack_config_t parametrization) {
        char *topics[]   = {"t1", "t2", "t3"};
        char *topics0[]  = {"t1", "t3"};
        int partitions[] = {6, 7, 2};
        rd_kafka_group_member_t members[3];
        size_t member_cnt         = RD_ARRAYSIZE(members);
        size_t i                  = 0;
        int subscriptions_count[] = {3, 3, 2};
        char **subscriptions[]    = {topics, topics, topics0};
        int with_owned            = 0;
        rd_kafka_topic_partition_list_t **owned;
        rd_kafka_metadata_t *metadata;

        if (parametrization !=
            RD_KAFKA_RANGE_ASSIGNOR_UT_BROKER_AND_CONSUMER_RACK) {
                RD_UT_PASS();
        }

        verifyNonRackAwareAssignment(
            rk, rkas, members, RD_ARRAYSIZE(members), RD_ARRAYSIZE(topics),
            topics, partitions, subscriptions_count, subscriptions, "t1", 5,
            "t2", 0, "t2", 2, "t2", 4, "t2", 6, NULL,
            /* consumer2 */
            "t1", 3, "t2", 1, "t2", 3, "t2", 5, "t3", 0, NULL,
            /* consumer3 */
            "t1", 0, "t1", 1, "t1", 2, "t1", 4, "t3", 1, NULL);

        // Verify assignment is rack-aligned for lower replication factor where
        // brokers have a subset of partitions
        for (with_owned = 0; with_owned <= 1; with_owned++) {
                setupRackAwareAssignment0(
                    rk, rkas, members, RD_ARRAYSIZE(members), 1, 3,
                    RD_ARRAYSIZE(topics), topics, partitions,
                    subscriptions_count, subscriptions, RACKS_INITIAL, NULL,
                    !with_owned, &metadata);
                verifyMultipleAssignment(
                    members, RD_ARRAYSIZE(members),
                    /* consumer1 */
                    "t1", 3, "t2", 0, "t2", 2, "t2", 3, "t2", 6, NULL,
                    /* consumer2 */
                    "t1", 4, "t2", 1, "t2", 4, "t2", 5, "t3", 0, NULL,
                    /* consumer3 */
                    "t1", 0, "t1", 1, "t1", 2, "t1", 5, "t3", 1, NULL);
                verifyNumPartitionsWithRackMismatch(metadata, members,
                                                    RD_ARRAYSIZE(members), 4);
                ut_destroy_metadata(metadata);
        }
        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);



        for (with_owned = 0; with_owned <= 1; with_owned++) {
                setupRackAwareAssignment0(
                    rk, rkas, members, RD_ARRAYSIZE(members), 2, 3,
                    RD_ARRAYSIZE(topics), topics, partitions,
                    subscriptions_count, subscriptions, RACKS_INITIAL, NULL,
                    !with_owned, &metadata);
                verifyMultipleAssignment(
                    members, RD_ARRAYSIZE(members),
                    /* consumer1 */
                    "t1", 3, "t2", 0, "t2", 2, "t2", 5, "t2", 6, NULL,
                    /* consumer2 */
                    "t1", 0, "t2", 1, "t2", 3, "t2", 4, "t3", 0, NULL,
                    /* consumer3 */
                    "t1", 1, "t1", 2, "t1", 4, "t1", 5, "t3", 1, NULL);
                verifyNumPartitionsWithRackMismatch(metadata, members,
                                                    RD_ARRAYSIZE(members), 0);
                ut_destroy_metadata(metadata);
        }
        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);

        /* One consumer on a rack with no partitions. We allocate with
         * misaligned rack to this consumer to maintain balance. */
        for (with_owned = 0; with_owned <= 1; with_owned++) {
                setupRackAwareAssignment0(
                    rk, rkas, members, RD_ARRAYSIZE(members), 3, 2,
                    RD_ARRAYSIZE(topics), topics, partitions,
                    subscriptions_count, subscriptions, RACKS_INITIAL, NULL,
                    !with_owned, &metadata);
                verifyMultipleAssignment(
                    members, RD_ARRAYSIZE(members),
                    /* consumer1 */
                    "t1", 5, "t2", 0, "t2", 2, "t2", 4, "t2", 6, NULL,
                    /* consumer2 */
                    "t1", 3, "t2", 1, "t2", 3, "t2", 5, "t3", 0, NULL,
                    /* consumer3 */
                    "t1", 0, "t1", 1, "t1", 2, "t1", 4, "t3", 1, NULL);
                verifyNumPartitionsWithRackMismatch(metadata, members,
                                                    RD_ARRAYSIZE(members), 5);
                ut_destroy_metadata(metadata);
        }

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);

        /* Verify that rack-awareness is improved if already owned partitions
         * are misaligned. */
        owned = ut_create_topic_partition_lists(
            3,
            /* consumer1 */
            "t1", 0, "t1", 1, "t1", 2, "t1", 3, "t1", 4, NULL,
            /* consumer2 */
            "t1", 5, "t2", 0, "t2", 1, "t2", 2, "t2", 3, NULL,
            /* consumer3 */
            "t2", 4, "t2", 5, "t2", 6, "t3", 0, "t3", 1, NULL);

        setupRackAwareAssignment0(rk, rkas, members, RD_ARRAYSIZE(members), 1,
                                  3, RD_ARRAYSIZE(topics), topics, partitions,
                                  subscriptions_count, subscriptions,
                                  RACKS_INITIAL, owned, rd_true, &metadata);
        verifyMultipleAssignment(
            members, RD_ARRAYSIZE(members),
            /* consumer1 */
            "t1", 3, "t2", 0, "t2", 2, "t2", 3, "t2", 6, NULL,
            /* consumer2 */
            "t1", 4, "t2", 1, "t2", 4, "t2", 5, "t3", 0, NULL,
            /* consumer3 */
            "t1", 0, "t1", 1, "t1", 2, "t1", 5, "t3", 1, NULL);
        verifyNumPartitionsWithRackMismatch(metadata, members,
                                            RD_ARRAYSIZE(members), 4);
        ut_destroy_metadata(metadata);

        for (i = 0; i < RD_ARRAYSIZE(members); i++)
                rd_kafka_group_member_clear(&members[i]);
        for (i = 0; i < member_cnt; i++)
                rd_kafka_topic_partition_list_destroy(owned[i]);
        rd_free(owned);

        /* One of the Java tests is skipped here, which tests if the rack-aware
         * logic assigns the same partitions as non-rack aware logic. This is
         * because we don't have a way to force rack-aware logic like the Java
         * assignor. */
        RD_UT_PASS();
}

static int rd_kafka_sticky_assignor_unittest(void) {
        rd_kafka_conf_t *conf;
        rd_kafka_t *rk;
        int fails = 0;
        char errstr[256];
        rd_kafka_assignor_t *rkas;
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
            ut_testAddRemoveConsumerOneTopic,
            ut_testPoorRoundRobinAssignmentScenario,
            ut_testAddRemoveTopicTwoConsumers,
            ut_testReassignmentAfterOneConsumerLeaves,
            ut_testReassignmentAfterOneConsumerAdded,
            ut_testSameSubscriptions,
            ut_testLargeAssignmentWithMultipleConsumersLeaving,
            ut_testNewSubscription,
            ut_testMoveExistingAssignments,
            ut_testMoveExistingAssignments_j,
            ut_testStickiness,
            ut_testStickiness_j,
            ut_testStickiness2,
            ut_testAssignmentUpdatedForDeletedTopic,
            ut_testNoExceptionThrownWhenOnlySubscribedTopicDeleted,
            ut_testConflictingPreviousAssignments,
            ut_testAllConsumersReachExpectedQuotaAndAreConsideredFilled,
            ut_testOwnedPartitionsAreInvalidatedForConsumerWithStaleGeneration,
            ut_testOwnedPartitionsAreInvalidatedForConsumerWithNoGeneration,
            ut_testPartitionsTransferringOwnershipIncludeThePartitionClaimedByMultipleConsumersInSameGeneration,
            ut_testPartitionsTransferringOwnershipIncludeThePartitionClaimedByMultipleConsumersInSameGeneration2,
            ut_testEnsurePartitionsAssignedToHighestGeneration,
            ut_testNoReassignmentOnCurrentMembers,
            ut_testOwnedPartitionsAreInvalidatedForConsumerWithMultipleGeneration,
            ut_testRackAwareAssignmentWithUniformSubscription,
            ut_testRackAwareAssignmentWithNonEqualSubscription,
            NULL,
        };
        size_t i;


        conf = rd_kafka_conf_new();
        if (rd_kafka_conf_set(conf, "group.id", "test", errstr,
                              sizeof(errstr)) ||
            rd_kafka_conf_set(conf, "partition.assignment.strategy",
                              "cooperative-sticky", errstr, sizeof(errstr)))
                RD_UT_FAIL("sticky assignor conf failed: %s", errstr);

        rd_kafka_conf_set(conf, "debug", rd_getenv("TEST_DEBUG", NULL), NULL,
                          0);

        rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
        RD_UT_ASSERT(rk, "sticky assignor client instantiation failed: %s",
                     errstr);

        rkas = rd_kafka_assignor_find(rk, "cooperative-sticky");
        RD_UT_ASSERT(rkas, "sticky assignor not found");

        for (i = 0; i < RD_ARRAY_SIZE(ALL_RACKS) - 1; i++) {
                char c       = 'a' + i;
                ALL_RACKS[i] = rd_kafkap_str_new(&c, 1);
        }
        ALL_RACKS[i] = NULL;

        for (i = 0; tests[i]; i++) {
                rd_ts_t ts = rd_clock();
                int r      = 0;
                rd_kafka_assignor_ut_rack_config_t j;

                RD_UT_SAY("[ Test #%" PRIusz " ]", i);
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
 * @brief Initialzie and add sticky assignor.
 */
rd_kafka_resp_err_t rd_kafka_sticky_assignor_init(rd_kafka_t *rk) {
        return rd_kafka_assignor_add(rk, "consumer", "cooperative-sticky",
                                     RD_KAFKA_REBALANCE_PROTOCOL_COOPERATIVE,
                                     rd_kafka_sticky_assignor_assign_cb,
                                     rd_kafka_sticky_assignor_get_metadata,
                                     rd_kafka_sticky_assignor_on_assignment_cb,
                                     rd_kafka_sticky_assignor_state_destroy,
                                     rd_kafka_sticky_assignor_unittest, NULL);
}
