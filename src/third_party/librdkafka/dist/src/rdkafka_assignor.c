/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2015-2022, Magnus Edenhill
 *               2023 Confluent Inc.
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
#include "rdunittest.h"

#include <ctype.h>

/**
 * Clear out and free any memory used by the member, but not the rkgm itself.
 */
void rd_kafka_group_member_clear(rd_kafka_group_member_t *rkgm) {
        if (rkgm->rkgm_owned)
                rd_kafka_topic_partition_list_destroy(rkgm->rkgm_owned);

        if (rkgm->rkgm_subscription)
                rd_kafka_topic_partition_list_destroy(rkgm->rkgm_subscription);

        if (rkgm->rkgm_assignment)
                rd_kafka_topic_partition_list_destroy(rkgm->rkgm_assignment);

        rd_list_destroy(&rkgm->rkgm_eligible);

        if (rkgm->rkgm_member_id)
                rd_kafkap_str_destroy(rkgm->rkgm_member_id);

        if (rkgm->rkgm_group_instance_id)
                rd_kafkap_str_destroy(rkgm->rkgm_group_instance_id);

        if (rkgm->rkgm_userdata)
                rd_kafkap_bytes_destroy(rkgm->rkgm_userdata);

        if (rkgm->rkgm_member_metadata)
                rd_kafkap_bytes_destroy(rkgm->rkgm_member_metadata);

        if (rkgm->rkgm_rack_id)
                rd_kafkap_str_destroy(rkgm->rkgm_rack_id);

        memset(rkgm, 0, sizeof(*rkgm));
}


/**
 * @brief Group member comparator (takes rd_kafka_group_member_t *)
 */
int rd_kafka_group_member_cmp(const void *_a, const void *_b) {
        const rd_kafka_group_member_t *a = (const rd_kafka_group_member_t *)_a;
        const rd_kafka_group_member_t *b = (const rd_kafka_group_member_t *)_b;

        /* Use the group instance id to compare static group members */
        if (!RD_KAFKAP_STR_IS_NULL(a->rkgm_group_instance_id) &&
            !RD_KAFKAP_STR_IS_NULL(b->rkgm_group_instance_id))
                return rd_kafkap_str_cmp(a->rkgm_group_instance_id,
                                         b->rkgm_group_instance_id);

        return rd_kafkap_str_cmp(a->rkgm_member_id, b->rkgm_member_id);
}


/**
 * Returns true if member subscribes to topic, else false.
 */
int rd_kafka_group_member_find_subscription(rd_kafka_t *rk,
                                            const rd_kafka_group_member_t *rkgm,
                                            const char *topic) {
        int i;

        /* Match against member's subscription. */
        for (i = 0; i < rkgm->rkgm_subscription->cnt; i++) {
                const rd_kafka_topic_partition_t *rktpar =
                    &rkgm->rkgm_subscription->elems[i];

                if (rd_kafka_topic_partition_match(rk, rkgm, rktpar, topic,
                                                   NULL))
                        return 1;
        }

        return 0;
}


rd_kafkap_bytes_t *rd_kafka_consumer_protocol_member_metadata_new(
    const rd_list_t *topics,
    const void *userdata,
    size_t userdata_size,
    const rd_kafka_topic_partition_list_t *owned_partitions,
    int generation,
    const rd_kafkap_str_t *rack_id) {

        rd_kafka_buf_t *rkbuf;
        rd_kafkap_bytes_t *kbytes;
        int i;
        int topic_cnt = rd_list_cnt(topics);
        const rd_kafka_topic_info_t *tinfo;
        size_t len;

        /*
         * MemberMetadata => Version Subscription AssignmentStrategies
         *   Version => int16
         *   Subscription => Topics UserData
         *     Topics => [String]
         *     UserData => Bytes
         *   OwnedPartitions => [Topic Partitions] // added in v1
         *     Topic => string
         *     Partitions => [int32]
         *   GenerationId => int32 // added in v2
         *   RackId => string // added in v3
         */

        rkbuf = rd_kafka_buf_new(1, 100 + (topic_cnt * 100) + userdata_size);

        /* Version */
        rd_kafka_buf_write_i16(rkbuf, 3);
        rd_kafka_buf_write_i32(rkbuf, topic_cnt);
        RD_LIST_FOREACH(tinfo, topics, i)
        rd_kafka_buf_write_str(rkbuf, tinfo->topic, -1);
        if (userdata)
                rd_kafka_buf_write_bytes(rkbuf, userdata, userdata_size);
        else /* Kafka 0.9.0.0 can't parse NULL bytes, so we provide empty,
              * which is compatible with all of the built-in Java client
              * assignors at the present time (up to and including v2.5) */
                rd_kafka_buf_write_bytes(rkbuf, "", 0);
        /* Following data is ignored by v0 consumers */
        if (!owned_partitions)
                /* If there are no owned partitions, this is specified as an
                 * empty array, not NULL. */
                rd_kafka_buf_write_i32(rkbuf, 0); /* Topic count */
        else {
                const rd_kafka_topic_partition_field_t fields[] = {
                    RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
                    RD_KAFKA_TOPIC_PARTITION_FIELD_END};
                rd_kafka_buf_write_topic_partitions(
                    rkbuf, owned_partitions,
                    rd_false /*don't skip invalid offsets*/,
                    rd_false /*any offset*/, rd_false /*don't use topic id*/,
                    rd_true /*use topic name*/, fields);
        }

        /* Following data is ignored by consumer version < 2 */
        rd_kafka_buf_write_i32(rkbuf, generation);

        /* Following data is ignored by consumer version < 3 */
        rd_kafka_buf_write_kstr(rkbuf, rack_id);

        /* Get binary buffer and allocate a new Kafka Bytes with a copy. */
        rd_slice_init_full(&rkbuf->rkbuf_reader, &rkbuf->rkbuf_buf);
        len    = rd_slice_remains(&rkbuf->rkbuf_reader);
        kbytes = rd_kafkap_bytes_new(NULL, (int32_t)len);
        rd_slice_read(&rkbuf->rkbuf_reader, (void *)kbytes->data, len);
        rd_kafka_buf_destroy(rkbuf);

        return kbytes;
}



rd_kafkap_bytes_t *rd_kafka_assignor_get_metadata_with_empty_userdata(
    const rd_kafka_assignor_t *rkas,
    void *assignor_state,
    const rd_list_t *topics,
    const rd_kafka_topic_partition_list_t *owned_partitions,
    const rd_kafkap_str_t *rack_id) {
        /* Generation was earlier populated inside userData, and older versions
         * of clients still expect that. So, in case the userData is empty, we
         * set the explicit generation field to the default value, -1 */
        return rd_kafka_consumer_protocol_member_metadata_new(
            topics, NULL, 0, owned_partitions, -1 /* generation */, rack_id);
}



/**
 * Returns 1 if all subscriptions are satifised for this member, else 0.
 */
static int rd_kafka_member_subscription_match(
    rd_kafka_cgrp_t *rkcg,
    rd_kafka_group_member_t *rkgm,
    const rd_kafka_metadata_topic_t *topic_metadata,
    rd_kafka_assignor_topic_t *eligible_topic) {
        int i;
        int has_regex = 0;
        int matched   = 0;

        /* Match against member's subscription. */
        for (i = 0; i < rkgm->rkgm_subscription->cnt; i++) {
                const rd_kafka_topic_partition_t *rktpar =
                    &rkgm->rkgm_subscription->elems[i];
                int matched_by_regex = 0;

                if (rd_kafka_topic_partition_match(rkcg->rkcg_rk, rkgm, rktpar,
                                                   topic_metadata->topic,
                                                   &matched_by_regex)) {
                        rd_list_add(&rkgm->rkgm_eligible,
                                    (void *)topic_metadata);
                        matched++;
                        has_regex += matched_by_regex;
                }
        }

        if (matched)
                rd_list_add(&eligible_topic->members, rkgm);

        if (!has_regex &&
            rd_list_cnt(&rkgm->rkgm_eligible) == rkgm->rkgm_subscription->cnt)
                return 1; /* All subscriptions matched */
        else
                return 0;
}


static void rd_kafka_assignor_topic_destroy(rd_kafka_assignor_topic_t *at) {
        rd_list_destroy(&at->members);
        rd_free(at);
}

int rd_kafka_assignor_topic_cmp(const void *_a, const void *_b) {
        const rd_kafka_assignor_topic_t *a =
            *(const rd_kafka_assignor_topic_t *const *)_a;
        const rd_kafka_assignor_topic_t *b =
            *(const rd_kafka_assignor_topic_t *const *)_b;

        return strcmp(a->metadata->topic, b->metadata->topic);
}

/**
 * Determine the complete set of topics that match at least one of
 * the group member subscriptions. Associate with each of these the
 * complete set of members that are subscribed to it. The result is
 * returned in `eligible_topics`.
 */
static void
rd_kafka_member_subscriptions_map(rd_kafka_cgrp_t *rkcg,
                                  rd_list_t *eligible_topics,
                                  const rd_kafka_metadata_t *metadata,
                                  rd_kafka_group_member_t *members,
                                  int member_cnt) {
        int ti;
        rd_kafka_assignor_topic_t *eligible_topic = NULL;
        rd_kafka_metadata_internal_t *mdi =
            rd_kafka_metadata_get_internal(metadata);

        rd_list_init(eligible_topics, RD_MIN(metadata->topic_cnt, 10),
                     (void *)rd_kafka_assignor_topic_destroy);

        /* For each topic in the cluster, scan through the member list
         * to find matching subscriptions. */
        for (ti = 0; ti < metadata->topic_cnt; ti++) {
                int i;

                /* Ignore topics in blacklist */
                if (rkcg->rkcg_rk->rk_conf.topic_blacklist &&
                    rd_kafka_pattern_match(
                        rkcg->rkcg_rk->rk_conf.topic_blacklist,
                        metadata->topics[ti].topic)) {
                        rd_kafka_dbg(rkcg->rkcg_rk,
                                     TOPIC | RD_KAFKA_DBG_ASSIGNOR, "BLACKLIST",
                                     "Assignor ignoring blacklisted "
                                     "topic \"%s\"",
                                     metadata->topics[ti].topic);
                        continue;
                }

                if (!eligible_topic)
                        eligible_topic = rd_calloc(1, sizeof(*eligible_topic));

                rd_list_init(&eligible_topic->members, member_cnt, NULL);

                /* For each member: scan through its topic subscription */
                for (i = 0; i < member_cnt; i++) {
                        /* Match topic against existing metadata,
                           incl regex matching. */
                        rd_kafka_member_subscription_match(
                            rkcg, &members[i], &metadata->topics[ti],
                            eligible_topic);
                }

                if (rd_list_empty(&eligible_topic->members)) {
                        rd_list_destroy(&eligible_topic->members);
                        continue;
                }

                eligible_topic->metadata          = &metadata->topics[ti];
                eligible_topic->metadata_internal = &mdi->topics[ti];
                rd_list_add(eligible_topics, eligible_topic);
                eligible_topic = NULL;
        }

        if (eligible_topic)
                rd_free(eligible_topic);
}


rd_kafka_resp_err_t rd_kafka_assignor_run(rd_kafka_cgrp_t *rkcg,
                                          const rd_kafka_assignor_t *rkas,
                                          rd_kafka_metadata_t *metadata,
                                          rd_kafka_group_member_t *members,
                                          int member_cnt,
                                          char *errstr,
                                          size_t errstr_size) {
        rd_kafka_resp_err_t err;
        rd_ts_t ts_start = rd_clock();
        int i;
        rd_list_t eligible_topics;
        int j;

        /* Construct eligible_topics, a map of:
         *    topic -> set of members that are subscribed to it. */
        rd_kafka_member_subscriptions_map(rkcg, &eligible_topics, metadata,
                                          members, member_cnt);


        if (rkcg->rkcg_rk->rk_conf.debug &
            (RD_KAFKA_DBG_CGRP | RD_KAFKA_DBG_ASSIGNOR)) {
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_ASSIGNOR, "ASSIGN",
                    "Group \"%s\" running %s assignor for "
                    "%d member(s) and "
                    "%d eligible subscribed topic(s):",
                    rkcg->rkcg_group_id->str, rkas->rkas_protocol_name->str,
                    member_cnt, eligible_topics.rl_cnt);

                for (i = 0; i < member_cnt; i++) {
                        const rd_kafka_group_member_t *member = &members[i];

                        rd_kafka_dbg(
                            rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_ASSIGNOR,
                            "ASSIGN",
                            " Member \"%.*s\"%s with "
                            "%d owned partition(s) and "
                            "%d subscribed topic(s):",
                            RD_KAFKAP_STR_PR(member->rkgm_member_id),
                            !rd_kafkap_str_cmp(member->rkgm_member_id,
                                               rkcg->rkcg_member_id)
                                ? " (me)"
                                : "",
                            member->rkgm_owned ? member->rkgm_owned->cnt : 0,
                            member->rkgm_subscription->cnt);
                        for (j = 0; j < member->rkgm_subscription->cnt; j++) {
                                const rd_kafka_topic_partition_t *p =
                                    &member->rkgm_subscription->elems[j];
                                rd_kafka_dbg(rkcg->rkcg_rk,
                                             CGRP | RD_KAFKA_DBG_ASSIGNOR,
                                             "ASSIGN", "  %s [%" PRId32 "]",
                                             p->topic, p->partition);
                        }
                }
        }

        /* Call assignors assign callback */
        err = rkas->rkas_assign_cb(
            rkcg->rkcg_rk, rkas, rkcg->rkcg_member_id->str, metadata, members,
            member_cnt, (rd_kafka_assignor_topic_t **)eligible_topics.rl_elems,
            eligible_topics.rl_cnt, errstr, errstr_size, rkas->rkas_opaque);

        if (err) {
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_ASSIGNOR, "ASSIGN",
                    "Group \"%s\" %s assignment failed "
                    "for %d member(s): %s",
                    rkcg->rkcg_group_id->str, rkas->rkas_protocol_name->str,
                    (int)member_cnt, errstr);
        } else if (rkcg->rkcg_rk->rk_conf.debug &
                   (RD_KAFKA_DBG_CGRP | RD_KAFKA_DBG_ASSIGNOR)) {
                rd_kafka_dbg(
                    rkcg->rkcg_rk, CGRP | RD_KAFKA_DBG_ASSIGNOR, "ASSIGN",
                    "Group \"%s\" %s assignment for %d member(s) "
                    "finished in %.3fms:",
                    rkcg->rkcg_group_id->str, rkas->rkas_protocol_name->str,
                    (int)member_cnt, (float)(rd_clock() - ts_start) / 1000.0f);
                for (i = 0; i < member_cnt; i++) {
                        const rd_kafka_group_member_t *member = &members[i];

                        rd_kafka_dbg(rkcg->rkcg_rk,
                                     CGRP | RD_KAFKA_DBG_ASSIGNOR, "ASSIGN",
                                     " Member \"%.*s\"%s assigned "
                                     "%d partition(s):",
                                     RD_KAFKAP_STR_PR(member->rkgm_member_id),
                                     !rd_kafkap_str_cmp(member->rkgm_member_id,
                                                        rkcg->rkcg_member_id)
                                         ? " (me)"
                                         : "",
                                     member->rkgm_assignment->cnt);
                        for (j = 0; j < member->rkgm_assignment->cnt; j++) {
                                const rd_kafka_topic_partition_t *p =
                                    &member->rkgm_assignment->elems[j];
                                rd_kafka_dbg(rkcg->rkcg_rk,
                                             CGRP | RD_KAFKA_DBG_ASSIGNOR,
                                             "ASSIGN", "  %s [%" PRId32 "]",
                                             p->topic, p->partition);
                        }
                }
        }

        rd_list_destroy(&eligible_topics);

        return err;
}


/**
 * Assignor protocol string comparator
 */
static int rd_kafka_assignor_cmp_str(const void *_a, const void *_b) {
        const char *a                = _a;
        const rd_kafka_assignor_t *b = _b;

        return rd_kafkap_str_cmp_str2(a, b->rkas_protocol_name);
}

/**
 * Find assignor by protocol name.
 *
 * Locality: any
 * Locks: none
 */
rd_kafka_assignor_t *rd_kafka_assignor_find(rd_kafka_t *rk,
                                            const char *protocol) {
        return (rd_kafka_assignor_t *)rd_list_find(
            &rk->rk_conf.partition_assignors, protocol,
            rd_kafka_assignor_cmp_str);
}


/**
 * Destroys an assignor (but does not unlink).
 */
static void rd_kafka_assignor_destroy(rd_kafka_assignor_t *rkas) {
        rd_kafkap_str_destroy(rkas->rkas_protocol_type);
        rd_kafkap_str_destroy(rkas->rkas_protocol_name);
        rd_free(rkas);
}


/**
 * @brief Check that the rebalance protocol of all enabled assignors is
 *        the same.
 */
rd_kafka_resp_err_t
rd_kafka_assignor_rebalance_protocol_check(const rd_kafka_conf_t *conf) {
        int i;
        rd_kafka_assignor_t *rkas;
        rd_kafka_rebalance_protocol_t rebalance_protocol =
            RD_KAFKA_REBALANCE_PROTOCOL_NONE;

        RD_LIST_FOREACH(rkas, &conf->partition_assignors, i) {
                if (!rkas->rkas_enabled)
                        continue;

                if (rebalance_protocol == RD_KAFKA_REBALANCE_PROTOCOL_NONE)
                        rebalance_protocol = rkas->rkas_protocol;
                else if (rebalance_protocol != rkas->rkas_protocol)
                        return RD_KAFKA_RESP_ERR__CONFLICT;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Add an assignor.
 */
rd_kafka_resp_err_t rd_kafka_assignor_add(
    rd_kafka_t *rk,
    const char *protocol_type,
    const char *protocol_name,
    rd_kafka_rebalance_protocol_t rebalance_protocol,
    rd_kafka_resp_err_t (*assign_cb)(
        rd_kafka_t *rk,
        const struct rd_kafka_assignor_s *rkas,
        const char *member_id,
        const rd_kafka_metadata_t *metadata,
        rd_kafka_group_member_t *members,
        size_t member_cnt,
        rd_kafka_assignor_topic_t **eligible_topics,
        size_t eligible_topic_cnt,
        char *errstr,
        size_t errstr_size,
        void *opaque),
    rd_kafkap_bytes_t *(*get_metadata_cb)(
        const struct rd_kafka_assignor_s *rkas,
        void *assignor_state,
        const rd_list_t *topics,
        const rd_kafka_topic_partition_list_t *owned_partitions,
        const rd_kafkap_str_t *rack_id),
    void (*on_assignment_cb)(const struct rd_kafka_assignor_s *rkas,
                             void **assignor_state,
                             const rd_kafka_topic_partition_list_t *assignment,
                             const rd_kafkap_bytes_t *userdata,
                             const rd_kafka_consumer_group_metadata_t *rkcgm),
    void (*destroy_state_cb)(void *assignor_state),
    int (*unittest_cb)(void),
    void *opaque) {
        rd_kafka_assignor_t *rkas;

        if (rd_kafkap_str_cmp_str(rk->rk_conf.group_protocol_type,
                                  protocol_type))
                return RD_KAFKA_RESP_ERR__UNKNOWN_PROTOCOL;

        if (rebalance_protocol != RD_KAFKA_REBALANCE_PROTOCOL_COOPERATIVE &&
            rebalance_protocol != RD_KAFKA_REBALANCE_PROTOCOL_EAGER)
                return RD_KAFKA_RESP_ERR__UNKNOWN_PROTOCOL;

        /* Dont overwrite application assignors */
        if ((rkas = rd_kafka_assignor_find(rk, protocol_name)))
                return RD_KAFKA_RESP_ERR__CONFLICT;

        rkas = rd_calloc(1, sizeof(*rkas));

        rkas->rkas_protocol_name    = rd_kafkap_str_new(protocol_name, -1);
        rkas->rkas_protocol_type    = rd_kafkap_str_new(protocol_type, -1);
        rkas->rkas_protocol         = rebalance_protocol;
        rkas->rkas_assign_cb        = assign_cb;
        rkas->rkas_get_metadata_cb  = get_metadata_cb;
        rkas->rkas_on_assignment_cb = on_assignment_cb;
        rkas->rkas_destroy_state_cb = destroy_state_cb;
        rkas->rkas_unittest         = unittest_cb;
        rkas->rkas_opaque           = opaque;
        rkas->rkas_index            = INT_MAX;

        rd_list_add(&rk->rk_conf.partition_assignors, rkas);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/* Right trim string of whitespaces */
static void rtrim(char *s) {
        char *e = s + strlen(s);

        if (e == s)
                return;

        while (e >= s && isspace(*e))
                e--;

        *e = '\0';
}


static int rd_kafka_assignor_cmp_idx(const void *ptr1, const void *ptr2) {
        const rd_kafka_assignor_t *rkas1 = (const rd_kafka_assignor_t *)ptr1;
        const rd_kafka_assignor_t *rkas2 = (const rd_kafka_assignor_t *)ptr2;
        return rkas1->rkas_index - rkas2->rkas_index;
}


/**
 * Initialize assignor list based on configuration.
 */
int rd_kafka_assignors_init(rd_kafka_t *rk, char *errstr, size_t errstr_size) {
        char *wanted;
        char *s;
        int idx = 0;

        rd_list_init(&rk->rk_conf.partition_assignors, 3,
                     (void *)rd_kafka_assignor_destroy);

        /* Initialize builtin assignors (ignore errors) */
        rd_kafka_range_assignor_init(rk);
        rd_kafka_roundrobin_assignor_init(rk);
        rd_kafka_sticky_assignor_init(rk);

        rd_strdupa(&wanted, rk->rk_conf.partition_assignment_strategy);

        s = wanted;
        while (*s) {
                rd_kafka_assignor_t *rkas = NULL;
                char *t;

                /* Left trim */
                while (*s == ' ' || *s == ',')
                        s++;

                if ((t = strchr(s, ','))) {
                        *t = '\0';
                        t++;
                } else {
                        t = s + strlen(s);
                }

                /* Right trim */
                rtrim(s);

                rkas = rd_kafka_assignor_find(rk, s);
                if (!rkas) {
                        rd_snprintf(errstr, errstr_size,
                                    "Unsupported partition.assignment.strategy:"
                                    " %s",
                                    s);
                        return -1;
                }

                if (!rkas->rkas_enabled) {
                        rkas->rkas_enabled = 1;
                        rk->rk_conf.enabled_assignor_cnt++;
                        rkas->rkas_index = idx;
                        idx++;
                }

                s = t;
        }

        /* Sort the assignors according to the input strategy order
         * since assignors will be scaned from the list sequentially
         * and the strategies earlier in the list have higher priority. */
        rd_list_sort(&rk->rk_conf.partition_assignors,
                     rd_kafka_assignor_cmp_idx);

        /* Clear the SORTED flag because the list is sorted according to the
         * rkas_index, but will do the search using rkas_protocol_name. */
        rk->rk_conf.partition_assignors.rl_flags &= ~RD_LIST_F_SORTED;

        if (rd_kafka_assignor_rebalance_protocol_check(&rk->rk_conf)) {
                rd_snprintf(errstr, errstr_size,
                            "All partition.assignment.strategy (%s) assignors "
                            "must have the same protocol type, "
                            "online migration between assignors with "
                            "different protocol types is not supported",
                            rk->rk_conf.partition_assignment_strategy);
                return -1;
        }

        return 0;
}



/**
 * Free assignors
 */
void rd_kafka_assignors_term(rd_kafka_t *rk) {
        rd_list_destroy(&rk->rk_conf.partition_assignors);
}

/**
 * @brief Computes whether rack-aware assignment needs to be used, or not.
 */
rd_bool_t
rd_kafka_use_rack_aware_assignment(rd_kafka_assignor_topic_t **topics,
                                   size_t topic_cnt,
                                   const rd_kafka_metadata_internal_t *mdi) {
        /* Computing needs_rack_aware_assignment requires the evaluation of
           three criteria:

           1. At least one of the member has a non-null rack.
           2. At least one common rack exists between members and partitions.
           3. There is a partition which doesn't have replicas on all possible
           racks, or in other words, all partitions don't have replicas on all
           racks. Note that 'all racks' here means racks across all replicas of
           all partitions, not including consumer racks. Also note that 'all
           racks' are computed per-topic for range assignor, and across topics
           for sticky assignor.
        */

        int i;
        size_t t;
        rd_kafka_group_member_t *member;
        rd_list_t *all_consumer_racks  = NULL; /* Contained Type: char* */
        rd_list_t *all_partition_racks = NULL; /* Contained Type: char* */
        char *rack_id                  = NULL;
        rd_bool_t needs_rack_aware_assignment = rd_true; /* assume true */

        /* Criteria 1 */
        /* We don't copy racks, so the free function is NULL. */
        all_consumer_racks = rd_list_new(0, NULL);

        for (t = 0; t < topic_cnt; t++) {
                RD_LIST_FOREACH(member, &topics[t]->members, i) {
                        if (member->rkgm_rack_id &&
                            RD_KAFKAP_STR_LEN(member->rkgm_rack_id)) {
                                /* Repetitions are fine, we will dedup it later.
                                 */
                                rd_list_add(
                                    all_consumer_racks,
                                    /* The const qualifier has to be discarded
                                       because of how rd_list_t and
                                       rd_kafkap_str_t are, but we never modify
                                       items in all_consumer_racks. */
                                    (char *)member->rkgm_rack_id->str);
                        }
                }
        }
        if (rd_list_cnt(all_consumer_racks) == 0) {
                needs_rack_aware_assignment = rd_false;
                goto done;
        }


        /* Critera 2 */
        /* We don't copy racks, so the free function is NULL. */
        all_partition_racks = rd_list_new(0, NULL);

        for (t = 0; t < topic_cnt; t++) {
                const int partition_cnt = topics[t]->metadata->partition_cnt;
                for (i = 0; i < partition_cnt; i++) {
                        size_t j;
                        for (j = 0; j < topics[t]
                                            ->metadata_internal->partitions[i]
                                            .racks_cnt;
                             j++) {
                                char *rack =
                                    topics[t]
                                        ->metadata_internal->partitions[i]
                                        .racks[j];
                                rd_list_add(all_partition_racks, rack);
                        }
                }
        }

        /* If there are no partition racks, Criteria 2 cannot possibly be met.
         */
        if (rd_list_cnt(all_partition_racks) == 0) {
                needs_rack_aware_assignment = rd_false;
                goto done;
        }

        /* Sort and dedup the racks. */
        rd_list_deduplicate(&all_consumer_racks, rd_strcmp2);
        rd_list_deduplicate(&all_partition_racks, rd_strcmp2);


        /* Iterate through each list in order, and see if there's anything in
         * common */
        RD_LIST_FOREACH(rack_id, all_consumer_racks, i) {
                /* Break if there's even a single match. */
                if (rd_list_find(all_partition_racks, rack_id, rd_strcmp2)) {
                        break;
                }
        }
        if (i == rd_list_cnt(all_consumer_racks)) {
                needs_rack_aware_assignment = rd_false;
                goto done;
        }

        /* Criteria 3 */
        for (t = 0; t < topic_cnt; t++) {
                const int partition_cnt = topics[t]->metadata->partition_cnt;
                for (i = 0; i < partition_cnt; i++) {
                        /* Since partition_racks[i] is a subset of
                         * all_partition_racks, and both of them are deduped,
                         * the same size indicates that they're equal. */
                        if ((size_t)(rd_list_cnt(all_partition_racks)) !=
                            topics[t]
                                ->metadata_internal->partitions[i]
                                .racks_cnt) {
                                break;
                        }
                }
                if (i < partition_cnt) {
                        /* Break outer loop if inner loop was broken. */
                        break;
                }
        }

        /* Implies that all partitions have replicas on all racks. */
        if (t == topic_cnt)
                needs_rack_aware_assignment = rd_false;

done:
        RD_IF_FREE(all_consumer_racks, rd_list_destroy);
        RD_IF_FREE(all_partition_racks, rd_list_destroy);

        return needs_rack_aware_assignment;
}


/* Helper to populate the racks for brokers in the metadata for unit tests.
 * Passing num_broker_racks = 0 will return NULL racks. */
void ut_populate_internal_broker_metadata(rd_kafka_metadata_internal_t *mdi,
                                          int num_broker_racks,
                                          rd_kafkap_str_t *all_racks[],
                                          size_t all_racks_cnt) {
        int i;

        rd_assert(num_broker_racks < (int)all_racks_cnt);

        for (i = 0; i < mdi->metadata.broker_cnt; i++) {
                mdi->brokers[i].id = i;
                /* Cast from const to non-const. We don't intend to modify it,
                 * but unfortunately neither implementation of rd_kafkap_str_t
                 * or rd_kafka_metadata_broker_internal_t can be changed. So,
                 * this cast is used - in unit tests only. */
                mdi->brokers[i].rack_id =
                    (char *)(num_broker_racks
                                 ? all_racks[i % num_broker_racks]->str
                                 : NULL);
        }
}

/* Helper to populate the deduplicated racks inside each partition. It's assumed
 * that `mdi->brokers` is set, maybe using
 * `ut_populate_internal_broker_metadata`. */
void ut_populate_internal_topic_metadata(rd_kafka_metadata_internal_t *mdi) {
        int ti;
        rd_kafka_metadata_broker_internal_t *brokers_internal;
        size_t broker_cnt;

        rd_assert(mdi->brokers);

        brokers_internal = mdi->brokers;
        broker_cnt       = mdi->metadata.broker_cnt;

        for (ti = 0; ti < mdi->metadata.topic_cnt; ti++) {
                int i;
                rd_kafka_metadata_topic_t *mdt = &mdi->metadata.topics[ti];
                rd_kafka_metadata_topic_internal_t *mdti = &mdi->topics[ti];

                for (i = 0; i < mdt->partition_cnt; i++) {
                        int j;
                        rd_kafka_metadata_partition_t *partition =
                            &mdt->partitions[i];
                        rd_kafka_metadata_partition_internal_t
                            *partition_internal = &mdti->partitions[i];

                        rd_list_t *curr_list;
                        char *rack;

                        if (partition->replica_cnt == 0)
                                continue;

                        curr_list = rd_list_new(
                            0, NULL); /* use a list for de-duplication */
                        for (j = 0; j < partition->replica_cnt; j++) {
                                rd_kafka_metadata_broker_internal_t key = {
                                    .id = partition->replicas[j]};
                                rd_kafka_metadata_broker_internal_t *broker =
                                    bsearch(
                                        &key, brokers_internal, broker_cnt,
                                        sizeof(
                                            rd_kafka_metadata_broker_internal_t),
                                        rd_kafka_metadata_broker_internal_cmp);
                                if (!broker || !broker->rack_id)
                                        continue;
                                rd_list_add(curr_list, broker->rack_id);
                        }
                        rd_list_deduplicate(&curr_list, rd_strcmp2);

                        partition_internal->racks_cnt = rd_list_cnt(curr_list);
                        partition_internal->racks     = rd_malloc(
                            sizeof(char *) * partition_internal->racks_cnt);
                        RD_LIST_FOREACH(rack, curr_list, j) {
                                partition_internal->racks[j] =
                                    rack; /* no duplication */
                        }
                        rd_list_destroy(curr_list);
                }
        }
}

/* Helper to destroy test metadata. Destroying the metadata has some additional
 * steps in case of tests. */
void ut_destroy_metadata(rd_kafka_metadata_t *md) {
        int ti;
        rd_kafka_metadata_internal_t *mdi = rd_kafka_metadata_get_internal(md);

        for (ti = 0; ti < md->topic_cnt; ti++) {
                int i;
                rd_kafka_metadata_topic_t *mdt           = &md->topics[ti];
                rd_kafka_metadata_topic_internal_t *mdti = &mdi->topics[ti];

                for (i = 0; mdti && i < mdt->partition_cnt; i++) {
                        rd_free(mdti->partitions[i].racks);
                }
        }

        rd_kafka_metadata_destroy(md);
}


/**
 * @brief Set a member's owned partitions based on its assignment.
 *
 * For use between assignor_run(). This is mimicing a consumer receiving
 * its new assignment and including it in the next rebalance as its
 * owned-partitions.
 */
void ut_set_owned(rd_kafka_group_member_t *rkgm) {
        if (rkgm->rkgm_owned)
                rd_kafka_topic_partition_list_destroy(rkgm->rkgm_owned);

        rkgm->rkgm_owned =
            rd_kafka_topic_partition_list_copy(rkgm->rkgm_assignment);
}


void ut_print_toppar_list(const rd_kafka_topic_partition_list_t *partitions) {
        int i;

        for (i = 0; i < partitions->cnt; i++)
                RD_UT_SAY(" %s [%" PRId32 "]", partitions->elems[i].topic,
                          partitions->elems[i].partition);
}


/* Implementation for ut_init_member and ut_init_member_with_rackv. */
static void ut_init_member_internal(rd_kafka_group_member_t *rkgm,
                                    const char *member_id,
                                    const rd_kafkap_str_t *rack_id,
                                    va_list ap) {
        const char *topic;

        memset(rkgm, 0, sizeof(*rkgm));

        rkgm->rkgm_member_id         = rd_kafkap_str_new(member_id, -1);
        rkgm->rkgm_group_instance_id = rd_kafkap_str_new(member_id, -1);
        rkgm->rkgm_rack_id = rack_id ? rd_kafkap_str_copy(rack_id) : NULL;

        rd_list_init(&rkgm->rkgm_eligible, 0, NULL);

        rkgm->rkgm_subscription = rd_kafka_topic_partition_list_new(4);

        while ((topic = va_arg(ap, const char *)))
                rd_kafka_topic_partition_list_add(rkgm->rkgm_subscription,
                                                  topic, RD_KAFKA_PARTITION_UA);

        rkgm->rkgm_assignment =
            rd_kafka_topic_partition_list_new(rkgm->rkgm_subscription->size);

        rkgm->rkgm_generation = 1;
}

/**
 * @brief Initialize group member struct for testing.
 *
 * va-args is a NULL-terminated list of (const char *) topics.
 *
 * Use rd_kafka_group_member_clear() to free fields.
 */
void ut_init_member(rd_kafka_group_member_t *rkgm, const char *member_id, ...) {
        va_list ap;
        va_start(ap, member_id);
        ut_init_member_internal(rkgm, member_id, NULL, ap);
        va_end(ap);
}

/**
 * @brief Initialize group member struct for testing with a rackid.
 *
 * va-args is a NULL-terminated list of (const char *) topics.
 *
 * Use rd_kafka_group_member_clear() to free fields.
 */
void ut_init_member_with_rackv(rd_kafka_group_member_t *rkgm,
                               const char *member_id,
                               const rd_kafkap_str_t *rack_id,
                               ...) {
        va_list ap;
        va_start(ap, rack_id);
        ut_init_member_internal(rkgm, member_id, rack_id, ap);
        va_end(ap);
}

/**
 * @brief Initialize group member struct for testing with a rackid.
 *
 * Topics that the member is subscribed to are specified in an array with the
 * size specified separately.
 *
 * Use rd_kafka_group_member_clear() to free fields.
 */
void ut_init_member_with_rack(rd_kafka_group_member_t *rkgm,
                              const char *member_id,
                              const rd_kafkap_str_t *rack_id,
                              char *topics[],
                              size_t topic_cnt) {
        size_t i;

        memset(rkgm, 0, sizeof(*rkgm));

        rkgm->rkgm_member_id         = rd_kafkap_str_new(member_id, -1);
        rkgm->rkgm_group_instance_id = rd_kafkap_str_new(member_id, -1);
        rkgm->rkgm_rack_id = rack_id ? rd_kafkap_str_copy(rack_id) : NULL;
        rd_list_init(&rkgm->rkgm_eligible, 0, NULL);

        rkgm->rkgm_subscription = rd_kafka_topic_partition_list_new(4);

        for (i = 0; i < topic_cnt; i++) {
                rd_kafka_topic_partition_list_add(
                    rkgm->rkgm_subscription, topics[i], RD_KAFKA_PARTITION_UA);
        }
        rkgm->rkgm_assignment =
            rd_kafka_topic_partition_list_new(rkgm->rkgm_subscription->size);
}

/**
 * @brief Verify that member's assignment matches the expected partitions.
 *
 * The va-list is a NULL-terminated list of (const char *topic, int partition)
 * tuples.
 *
 * @returns 0 on success, else raises a unittest error and returns 1.
 */
int verifyAssignment0(const char *function,
                      int line,
                      rd_kafka_group_member_t *rkgm,
                      ...) {
        va_list ap;
        int cnt = 0;
        const char *topic;
        int fails = 0;

        va_start(ap, rkgm);
        while ((topic = va_arg(ap, const char *))) {
                int partition = va_arg(ap, int);
                cnt++;

                if (!rd_kafka_topic_partition_list_find(rkgm->rkgm_assignment,
                                                        topic, partition)) {
                        RD_UT_WARN(
                            "%s:%d: Expected %s [%d] not found in %s's "
                            "assignment (%d partition(s))",
                            function, line, topic, partition,
                            rkgm->rkgm_member_id->str,
                            rkgm->rkgm_assignment->cnt);
                        fails++;
                }
        }
        va_end(ap);

        if (cnt != rkgm->rkgm_assignment->cnt) {
                RD_UT_WARN(
                    "%s:%d: "
                    "Expected %d assigned partition(s) for %s, not %d",
                    function, line, cnt, rkgm->rkgm_member_id->str,
                    rkgm->rkgm_assignment->cnt);
                fails++;
        }

        if (fails)
                ut_print_toppar_list(rkgm->rkgm_assignment);

        RD_UT_ASSERT(!fails, "%s:%d: See previous errors", function, line);

        return 0;
}

/**
 * @brief Verify that all members' assignment matches the expected partitions.
 *
 * The va-list is a list of (const char *topic, int partition)
 * tuples, and NULL to demarcate different members' assignment.
 *
 * @returns 0 on success, else raises a unittest error and returns 1.
 */
int verifyMultipleAssignment0(const char *function,
                              int line,
                              rd_kafka_group_member_t *rkgms,
                              size_t member_cnt,
                              ...) {
        va_list ap;
        const char *topic;
        int fails = 0;
        size_t i  = 0;

        if (member_cnt == 0) {
                return 0;
        }

        va_start(ap, member_cnt);
        for (i = 0; i < member_cnt; i++) {
                rd_kafka_group_member_t *rkgm = &rkgms[i];
                int cnt                       = 0;
                int local_fails               = 0;

                while ((topic = va_arg(ap, const char *))) {
                        int partition = va_arg(ap, int);
                        cnt++;

                        if (!rd_kafka_topic_partition_list_find(
                                rkgm->rkgm_assignment, topic, partition)) {
                                RD_UT_WARN(
                                    "%s:%d: Expected %s [%d] not found in %s's "
                                    "assignment (%d partition(s))",
                                    function, line, topic, partition,
                                    rkgm->rkgm_member_id->str,
                                    rkgm->rkgm_assignment->cnt);
                                local_fails++;
                        }
                }

                if (cnt != rkgm->rkgm_assignment->cnt) {
                        RD_UT_WARN(
                            "%s:%d: "
                            "Expected %d assigned partition(s) for %s, not %d",
                            function, line, cnt, rkgm->rkgm_member_id->str,
                            rkgm->rkgm_assignment->cnt);
                        fails++;
                }

                if (local_fails)
                        ut_print_toppar_list(rkgm->rkgm_assignment);
                fails += local_fails;
        }
        va_end(ap);

        RD_UT_ASSERT(!fails, "%s:%d: See previous errors", function, line);

        return 0;
}


#define verifyNumPartitionsWithRackMismatchPartition(rktpar, metadata,         \
                                                     increase)                 \
        do {                                                                   \
                if (!rktpar)                                                   \
                        break;                                                 \
                int i;                                                         \
                rd_bool_t noneMatch = rd_true;                                 \
                rd_kafka_metadata_internal_t *metadata_internal =              \
                    rd_kafka_metadata_get_internal(metadata);                  \
                                                                               \
                for (i = 0; i < metadata->topics[j].partitions[k].replica_cnt; \
                     i++) {                                                    \
                        int32_t replica_id =                                   \
                            metadata->topics[j].partitions[k].replicas[i];     \
                        rd_kafka_metadata_broker_internal_t *broker;           \
                        rd_kafka_metadata_broker_internal_find(                \
                            metadata_internal, replica_id, broker);            \
                                                                               \
                        if (broker && !strcmp(rack_id, broker->rack_id)) {     \
                                noneMatch = rd_false;                          \
                                break;                                         \
                        }                                                      \
                }                                                              \
                                                                               \
                if (noneMatch)                                                 \
                        increase++;                                            \
        } while (0);

/**
 * @brief Verify number of partitions with rack mismatch.
 */
int verifyNumPartitionsWithRackMismatch0(const char *function,
                                         int line,
                                         rd_kafka_metadata_t *metadata,
                                         rd_kafka_group_member_t *rkgms,
                                         size_t member_cnt,
                                         int expectedNumMismatch) {
        size_t i;
        int j, k;

        int numMismatched = 0;
        for (i = 0; i < member_cnt; i++) {
                rd_kafka_group_member_t *rkgm = &rkgms[i];
                const char *rack_id           = rkgm->rkgm_rack_id->str;
                if (rack_id) {
                        for (j = 0; j < metadata->topic_cnt; j++) {
                                for (k = 0;
                                     k < metadata->topics[j].partition_cnt;
                                     k++) {
                                        rd_kafka_topic_partition_t *rktpar =
                                            rd_kafka_topic_partition_list_find(
                                                rkgm->rkgm_assignment,
                                                metadata->topics[j].topic, k);
                                        verifyNumPartitionsWithRackMismatchPartition(
                                            rktpar, metadata, numMismatched);
                                }
                        }
                }
        }

        RD_UT_ASSERT(expectedNumMismatch == numMismatched,
                     "%s:%d: Expected %d mismatches, got %d", function, line,
                     expectedNumMismatch, numMismatched);

        return 0;
}


int verifyValidityAndBalance0(const char *func,
                              int line,
                              rd_kafka_group_member_t *members,
                              size_t member_cnt,
                              const rd_kafka_metadata_t *metadata) {
        int fails = 0;
        int i;
        rd_bool_t verbose = rd_false; /* Enable for troubleshooting */

        RD_UT_SAY("%s:%d: verifying assignment for %d member(s):", func, line,
                  (int)member_cnt);

        for (i = 0; i < (int)member_cnt; i++) {
                const char *consumer = members[i].rkgm_member_id->str;
                const rd_kafka_topic_partition_list_t *partitions =
                    members[i].rkgm_assignment;
                int p, j;

                if (verbose)
                        RD_UT_SAY(
                            "%s:%d:   "
                            "consumer \"%s\", %d subscribed topic(s), "
                            "%d assigned partition(s):",
                            func, line, consumer,
                            members[i].rkgm_subscription->cnt, partitions->cnt);

                for (p = 0; p < partitions->cnt; p++) {
                        const rd_kafka_topic_partition_t *partition =
                            &partitions->elems[p];

                        if (verbose)
                                RD_UT_SAY("%s:%d:     %s [%" PRId32 "]", func,
                                          line, partition->topic,
                                          partition->partition);

                        if (!rd_kafka_topic_partition_list_find(
                                members[i].rkgm_subscription, partition->topic,
                                RD_KAFKA_PARTITION_UA)) {
                                RD_UT_WARN("%s [%" PRId32
                                           "] is assigned to "
                                           "%s but it is not subscribed to "
                                           "that topic",
                                           partition->topic,
                                           partition->partition, consumer);
                                fails++;
                        }
                }

                /* Update the member's owned partitions to match
                 * the assignment. */
                ut_set_owned(&members[i]);

                if (i == (int)member_cnt - 1)
                        continue;

                for (j = i + 1; j < (int)member_cnt; j++) {
                        const char *otherConsumer =
                            members[j].rkgm_member_id->str;
                        const rd_kafka_topic_partition_list_t *otherPartitions =
                            members[j].rkgm_assignment;
                        rd_bool_t balanced =
                            abs(partitions->cnt - otherPartitions->cnt) <= 1;

                        for (p = 0; p < partitions->cnt; p++) {
                                const rd_kafka_topic_partition_t *partition =
                                    &partitions->elems[p];

                                if (rd_kafka_topic_partition_list_find(
                                        otherPartitions, partition->topic,
                                        partition->partition)) {
                                        RD_UT_WARN(
                                            "Consumer %s and %s are both "
                                            "assigned %s [%" PRId32 "]",
                                            consumer, otherConsumer,
                                            partition->topic,
                                            partition->partition);
                                        fails++;
                                }


                                /* If assignment is imbalanced and this topic
                                 * is also subscribed by the other consumer
                                 * it means the assignment strategy failed to
                                 * properly balance the partitions. */
                                if (!balanced &&
                                    rd_kafka_topic_partition_list_find_topic_by_name(
                                        otherPartitions, partition->topic)) {
                                        RD_UT_WARN(
                                            "Some %s partition(s) can be "
                                            "moved from "
                                            "%s (%d partition(s)) to "
                                            "%s (%d partition(s)) to "
                                            "achieve a better balance",
                                            partition->topic, consumer,
                                            partitions->cnt, otherConsumer,
                                            otherPartitions->cnt);
                                        fails++;
                                }
                        }
                }
        }

        RD_UT_ASSERT(!fails, "%s:%d: See %d previous errors", func, line,
                     fails);

        return 0;
}

/**
 * @brief Checks that all assigned partitions are fully balanced.
 *
 * Only works for symmetrical subscriptions.
 */
int isFullyBalanced0(const char *function,
                     int line,
                     const rd_kafka_group_member_t *members,
                     size_t member_cnt) {
        int min_assignment = INT_MAX;
        int max_assignment = -1;
        size_t i;

        for (i = 0; i < member_cnt; i++) {
                int size = members[i].rkgm_assignment->cnt;
                if (size < min_assignment)
                        min_assignment = size;
                if (size > max_assignment)
                        max_assignment = size;
        }

        RD_UT_ASSERT(max_assignment - min_assignment <= 1,
                     "%s:%d: Assignment not balanced: min %d, max %d", function,
                     line, min_assignment, max_assignment);

        return 0;
}


/**
 * @brief Unittest for assignors
 */
static int ut_assignors(void) {
        const struct {
                const char *name;
                int topic_cnt;
                struct {
                        const char *name;
                        int partition_cnt;
                } topics[12];
                int member_cnt;
                struct {
                        const char *name;
                        int topic_cnt;
                        const char *topics[12];
                } members[3];
                int expect_cnt;
                struct {
                        const char *protocol_name;
                        struct {
                                int partition_cnt;
                                const char *partitions[12]; /* "topic:part" */
                        } members[3];
                } expect[2];
        } tests[] = {
            /*
             * Test cases
             */
            {
                .name      = "Symmetrical subscription",
                .topic_cnt = 4,
                .topics =
                    {
                        {"a", 3}, /* a:0 a:1 a:2 */
                        {
                            "b",
                            4,
                        },        /* b:0 b:1 b:2 b:3 */
                        {"c", 2}, /* c:0 c:1 */
                        {"d", 1}, /* d:0 */
                    },
                .member_cnt = 2,
                .members =
                    {
                        {.name      = "consumer1",
                         .topic_cnt = 4,
                         .topics    = {"d", "b", "a", "c"}},
                        {.name      = "consumer2",
                         .topic_cnt = 4,
                         .topics    = {"a", "b", "c", "d"}},
                    },
                .expect_cnt = 2,
                .expect =
                    {
                        {
                            .protocol_name = "range",
                            .members =
                                {
                                    /* Consumer1 */
                                    {6,
                                     {"a:0", "a:1", "b:0", "b:1", "c:0",
                                      "d:0"}},
                                    /* Consumer2 */
                                    {4, {"a:2", "b:2", "b:3", "c:1"}},
                                },
                        },
                        {
                            .protocol_name = "roundrobin",
                            .members =
                                {
                                    /* Consumer1 */
                                    {5, {"a:0", "a:2", "b:1", "b:3", "c:1"}},
                                    /* Consumer2 */
                                    {5, {"a:1", "b:0", "b:2", "c:0", "d:0"}},
                                },
                        },
                    },
            },
            {
                .name      = "1*3 partitions (asymmetrical)",
                .topic_cnt = 1,
                .topics =
                    {
                        {"a", 3},
                    },
                .member_cnt = 2,
                .members =
                    {
                        {.name      = "consumer1",
                         .topic_cnt = 3,
                         .topics    = {"a", "b", "c"}},
                        {.name = "consumer2", .topic_cnt = 1, .topics = {"a"}},
                    },
                .expect_cnt = 2,
                .expect =
                    {
                        {
                            .protocol_name = "range",
                            .members =
                                {
                                    /* Consumer1.
                                     * range assignor applies
                                     * per topic. */
                                    {2, {"a:0", "a:1"}},
                                    /* Consumer2 */
                                    {1, {"a:2"}},
                                },
                        },
                        {
                            .protocol_name = "roundrobin",
                            .members =
                                {
                                    /* Consumer1 */
                                    {2, {"a:0", "a:2"}},
                                    /* Consumer2 */
                                    {1, {"a:1"}},
                                },
                        },
                    },
            },
            {
                .name      = "#2121 (asymmetrical)",
                .topic_cnt = 12,
                .topics =
                    {
                        {"a", 1},
                        {"b", 1},
                        {"c", 1},
                        {"d", 1},
                        {"e", 1},
                        {"f", 1},
                        {"g", 1},
                        {"h", 1},
                        {"i", 1},
                        {"j", 1},
                        {"k", 1},
                        {"l", 1},
                    },
                .member_cnt = 2,
                .members =
                    {
                        {
                            .name      = "consumer1",
                            .topic_cnt = 12,
                            .topics =
                                {
                                    "a",
                                    "b",
                                    "c",
                                    "d",
                                    "e",
                                    "f",
                                    "g",
                                    "h",
                                    "i",
                                    "j",
                                    "k",
                                    "l",
                                },
                        },
                        {
                            .name      = "consumer2", /* must be second */
                            .topic_cnt = 5,
                            .topics =
                                {
                                    "b",
                                    "d",
                                    "f",
                                    "h",
                                    "l",
                                },
                        },
                    },
                .expect_cnt = 2,
                .expect =
                    {
                        {
                            .protocol_name = "range",
                            .members =
                                {
                                    /* Consumer1.
                                     * All partitions. */
                                    {12,
                                     {
                                         "a:0",
                                         "b:0",
                                         "c:0",
                                         "d:0",
                                         "e:0",
                                         "f:0",
                                         "g:0",
                                         "h:0",
                                         "i:0",
                                         "j:0",
                                         "k:0",
                                         "l:0",
                                     }},
                                    /* Consumer2 */
                                    {0},
                                },
                        },
                        {
                            .protocol_name = "roundrobin",
                            .members =
                                {
                                    /* Consumer1 */
                                    {
                                        7,
                                        {
                                            "a:0",
                                            "c:0",
                                            "e:0",
                                            "g:0",
                                            "i:0",
                                            "j:0",
                                            "k:0",
                                        },
                                    },
                                    /* Consumer2 */
                                    {5, {"b:0", "d:0", "f:0", "h:0", "l:0"}},
                                },
                        },
                    },
            },
            {NULL},
        };
        rd_kafka_conf_t *conf;
        rd_kafka_t *rk;
        const rd_kafka_assignor_t *rkas;
        int fails = 0;
        int i;

        conf = rd_kafka_conf_new();
        rd_kafka_conf_set(conf, "group.id", "group", NULL, 0);
        rd_kafka_conf_set(conf, "debug", rd_getenv("TEST_DEBUG", NULL), NULL,
                          0);
        rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, NULL, 0);
        RD_UT_ASSERT(rk != NULL, "Failed to create consumer");

        /* Run through test cases */
        for (i = 0; tests[i].name; i++) {
                int ie, it, im;
                rd_kafka_metadata_internal_t metadata_internal;
                rd_kafka_metadata_t metadata;
                rd_kafka_group_member_t *members;

                /* Create topic metadata */
                metadata.topic_cnt = tests[i].topic_cnt;
                metadata.topics =
                    rd_alloca(sizeof(*metadata.topics) * metadata.topic_cnt);
                metadata_internal.topics = rd_alloca(
                    sizeof(*metadata_internal.topics) * metadata.topic_cnt);

                memset(metadata.topics, 0,
                       sizeof(*metadata.topics) * metadata.topic_cnt);
                memset(metadata_internal.topics, 0,
                       sizeof(*metadata_internal.topics) * metadata.topic_cnt);

                for (it = 0; it < metadata.topic_cnt; it++) {
                        int pt;
                        metadata.topics[it].topic =
                            (char *)tests[i].topics[it].name;
                        metadata.topics[it].partition_cnt =
                            tests[i].topics[it].partition_cnt;
                        metadata.topics[it].partitions =
                            rd_alloca(metadata.topics[it].partition_cnt *
                                      sizeof(rd_kafka_metadata_partition_t));
                        metadata_internal.topics[it].partitions = rd_alloca(
                            metadata.topics[it].partition_cnt *
                            sizeof(rd_kafka_metadata_partition_internal_t));
                        for (pt = 0; pt < metadata.topics[it].partition_cnt;
                             pt++) {
                                metadata.topics[it].partitions[pt].id = pt;
                                metadata.topics[it].partitions[pt].replica_cnt =
                                    0;
                                metadata_internal.topics[it]
                                    .partitions[pt]
                                    .racks_cnt = 0;
                                metadata_internal.topics[it]
                                    .partitions[pt]
                                    .racks = NULL;
                        }
                }

                /* Create members */
                members = rd_alloca(sizeof(*members) * tests[i].member_cnt);
                memset(members, 0, sizeof(*members) * tests[i].member_cnt);

                for (im = 0; im < tests[i].member_cnt; im++) {
                        rd_kafka_group_member_t *rkgm = &members[im];
                        rkgm->rkgm_member_id =
                            rd_kafkap_str_new(tests[i].members[im].name, -1);
                        rkgm->rkgm_group_instance_id =
                            rd_kafkap_str_new(tests[i].members[im].name, -1);
                        rd_list_init(&rkgm->rkgm_eligible,
                                     tests[i].members[im].topic_cnt, NULL);

                        rkgm->rkgm_subscription =
                            rd_kafka_topic_partition_list_new(
                                tests[i].members[im].topic_cnt);
                        for (it = 0; it < tests[i].members[im].topic_cnt; it++)
                                rd_kafka_topic_partition_list_add(
                                    rkgm->rkgm_subscription,
                                    tests[i].members[im].topics[it],
                                    RD_KAFKA_PARTITION_UA);

                        rkgm->rkgm_userdata = NULL;

                        rkgm->rkgm_assignment =
                            rd_kafka_topic_partition_list_new(
                                rkgm->rkgm_subscription->size);
                }

                /* For each assignor verify that the assignment
                 * matches the expection set out in the test case. */
                for (ie = 0; ie < tests[i].expect_cnt; ie++) {
                        rd_kafka_resp_err_t err;
                        char errstr[256];

                        RD_UT_SAY("Test case %s: %s assignor", tests[i].name,
                                  tests[i].expect[ie].protocol_name);

                        if (!(rkas = rd_kafka_assignor_find(
                                  rk, tests[i].expect[ie].protocol_name))) {
                                RD_UT_FAIL(
                                    "Assignor test case %s for %s failed: "
                                    "assignor not found",
                                    tests[i].name,
                                    tests[i].expect[ie].protocol_name);
                        }

                        /* Run assignor */
                        metadata_internal.metadata = metadata;
                        err                        = rd_kafka_assignor_run(
                            rk->rk_cgrp, rkas,
                            (rd_kafka_metadata_t *)(&metadata_internal),
                            members, tests[i].member_cnt, errstr,
                            sizeof(errstr));

                        RD_UT_ASSERT(!err, "Assignor case %s for %s failed: %s",
                                     tests[i].name,
                                     tests[i].expect[ie].protocol_name, errstr);

                        /* Verify assignments */
                        for (im = 0; im < tests[i].member_cnt; im++) {
                                rd_kafka_group_member_t *rkgm = &members[im];
                                int ia;

                                if (rkgm->rkgm_assignment->cnt !=
                                    tests[i]
                                        .expect[ie]
                                        .members[im]
                                        .partition_cnt) {
                                        RD_UT_WARN(
                                            " Member %.*s assignment count "
                                            "mismatch: %d != %d",
                                            RD_KAFKAP_STR_PR(
                                                rkgm->rkgm_member_id),
                                            rkgm->rkgm_assignment->cnt,
                                            tests[i]
                                                .expect[ie]
                                                .members[im]
                                                .partition_cnt);
                                        fails++;
                                }

                                if (rkgm->rkgm_assignment->cnt > 0)
                                        rd_kafka_topic_partition_list_sort_by_topic(
                                            rkgm->rkgm_assignment);

                                for (ia = 0; ia < rkgm->rkgm_assignment->cnt;
                                     ia++) {
                                        rd_kafka_topic_partition_t *p =
                                            &rkgm->rkgm_assignment->elems[ia];
                                        char part[64];
                                        const char *exp =
                                            ia < tests[i]
                                                        .expect[ie]
                                                        .members[im]
                                                        .partition_cnt
                                                ? tests[i]
                                                      .expect[ie]
                                                      .members[im]
                                                      .partitions[ia]
                                                : "(none)";

                                        rd_snprintf(part, sizeof(part), "%s:%d",
                                                    p->topic,
                                                    (int)p->partition);

#if 0 /* Enable to print actual assignment */
                                        RD_UT_SAY(" Member %.*s assignment "
                                                  "%d/%d %s =? %s",
                                                  RD_KAFKAP_STR_PR(
                                                          rkgm->rkgm_member_id),
                                                  ia,
                                                  rkgm->rkgm_assignment->cnt-1,
                                                  part, exp);
#endif

                                        if (strcmp(part, exp)) {
                                                RD_UT_WARN(
                                                    " Member %.*s "
                                                    "assignment %d/%d "
                                                    "mismatch: %s != %s",
                                                    RD_KAFKAP_STR_PR(
                                                        rkgm->rkgm_member_id),
                                                    ia,
                                                    rkgm->rkgm_assignment->cnt -
                                                        1,
                                                    part, exp);
                                                fails++;
                                        }
                                }

                                /* Reset assignment for next loop */
                                rd_kafka_topic_partition_list_destroy(
                                    rkgm->rkgm_assignment);
                                rkgm->rkgm_assignment =
                                    rd_kafka_topic_partition_list_new(
                                        rkgm->rkgm_subscription->size);
                        }
                }

                for (im = 0; im < tests[i].member_cnt; im++) {
                        rd_kafka_group_member_t *rkgm = &members[im];
                        rd_kafka_group_member_clear(rkgm);
                }
        }


        /* Run assignor-specific unittests */
        RD_LIST_FOREACH(rkas, &rk->rk_conf.partition_assignors, i) {
                if (rkas->rkas_unittest)
                        fails += rkas->rkas_unittest();
        }

        rd_kafka_destroy(rk);

        if (fails)
                return 1;

        RD_UT_PASS();
}


/**
 * @brief Unit tests for assignors
 */
int unittest_assignors(void) {
        return ut_assignors();
}
