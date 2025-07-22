/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2019-2022, Magnus Edenhill
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

/**
 * Mocks
 *
 */

#include "rdkafka_int.h"
#include "rdbuf.h"
#include "rdrand.h"
#include "rdkafka_interceptor.h"
#include "rdkafka_mock_int.h"
#include "rdkafka_transport_int.h"
#include "rdkafka_mock.h"
#include "rdunittest.h"

#include <stdarg.h>

typedef struct rd_kafka_mock_request_s rd_kafka_mock_request_t;

static void rd_kafka_mock_cluster_destroy0(rd_kafka_mock_cluster_t *mcluster);
static rd_kafka_mock_request_t *
rd_kafka_mock_request_new(int32_t id, int16_t api_key, int64_t timestamp_us);
static void rd_kafka_mock_request_free(void *element);
static void rd_kafka_mock_coord_remove(rd_kafka_mock_cluster_t *mcluster,
                                       int32_t broker_id);

static rd_kafka_mock_broker_t *
rd_kafka_mock_broker_find(const rd_kafka_mock_cluster_t *mcluster,
                          int32_t broker_id) {
        const rd_kafka_mock_broker_t *mrkb;

        TAILQ_FOREACH(mrkb, &mcluster->brokers, link)
        if (mrkb->id == broker_id)
                return (rd_kafka_mock_broker_t *)mrkb;

        return NULL;
}



/**
 * @brief Unlink and free message set.
 */
static void rd_kafka_mock_msgset_destroy(rd_kafka_mock_partition_t *mpart,
                                         rd_kafka_mock_msgset_t *mset) {
        const rd_kafka_mock_msgset_t *next = TAILQ_NEXT(mset, link);

        /* Removing last messageset */
        if (!next)
                mpart->start_offset = mpart->end_offset;
        else if (mset == TAILQ_FIRST(&mpart->msgsets))
                /* Removing first messageset */
                mpart->start_offset = next->first_offset;

        if (mpart->update_follower_start_offset)
                mpart->follower_start_offset = mpart->start_offset;

        rd_assert(mpart->cnt > 0);
        mpart->cnt--;
        mpart->size -= RD_KAFKAP_BYTES_LEN(&mset->bytes);
        TAILQ_REMOVE(&mpart->msgsets, mset, link);
        rd_free(mset);
}


/**
 * @brief Create a new msgset object with a copy of \p bytes
 *        and appends it to the partition log.
 */
static rd_kafka_mock_msgset_t *
rd_kafka_mock_msgset_new(rd_kafka_mock_partition_t *mpart,
                         const rd_kafkap_bytes_t *bytes,
                         size_t msgcnt) {
        rd_kafka_mock_msgset_t *mset;
        size_t totsize = sizeof(*mset) + RD_KAFKAP_BYTES_LEN(bytes);
        int64_t BaseOffset;
        int32_t PartitionLeaderEpoch;
        int64_t orig_start_offset = mpart->start_offset;

        rd_assert(!RD_KAFKAP_BYTES_IS_NULL(bytes));

        mset = rd_malloc(totsize);
        rd_assert(mset != NULL);

        mset->first_offset = mpart->end_offset;
        mset->last_offset  = mset->first_offset + msgcnt - 1;
        mpart->end_offset  = mset->last_offset + 1;
        if (mpart->update_follower_end_offset)
                mpart->follower_end_offset = mpart->end_offset;
        mpart->cnt++;

        mset->bytes.len    = bytes->len;
        mset->leader_epoch = mpart->leader_epoch;


        mset->bytes.data = (void *)(mset + 1);
        memcpy((void *)mset->bytes.data, bytes->data, mset->bytes.len);
        mpart->size += mset->bytes.len;

        /* Update the base Offset in the MessageSet with the
         * actual absolute log offset. */
        BaseOffset = htobe64(mset->first_offset);
        memcpy((void *)mset->bytes.data, &BaseOffset, sizeof(BaseOffset));
        /* Update the base PartitionLeaderEpoch in the MessageSet with the
         * actual partition leader epoch. */
        PartitionLeaderEpoch = htobe32(mset->leader_epoch);
        memcpy(((char *)mset->bytes.data) + 12, &PartitionLeaderEpoch,
               sizeof(PartitionLeaderEpoch));

        /* Remove old msgsets until within limits */
        while (mpart->cnt > 1 &&
               (mpart->cnt > mpart->max_cnt || mpart->size > mpart->max_size))
                rd_kafka_mock_msgset_destroy(mpart,
                                             TAILQ_FIRST(&mpart->msgsets));

        TAILQ_INSERT_TAIL(&mpart->msgsets, mset, link);

        rd_kafka_dbg(mpart->topic->cluster->rk, MOCK, "MOCK",
                     "Broker %" PRId32 ": Log append %s [%" PRId32
                     "] "
                     "%" PRIusz " messages, %" PRId32
                     " bytes at offset %" PRId64 " (log now %" PRId64
                     "..%" PRId64
                     ", "
                     "original start %" PRId64 ")",
                     mpart->leader->id, mpart->topic->name, mpart->id, msgcnt,
                     RD_KAFKAP_BYTES_LEN(&mset->bytes), mset->first_offset,
                     mpart->start_offset, mpart->end_offset, orig_start_offset);

        return mset;
}

/**
 * @brief Find message set containing \p offset
 */
const rd_kafka_mock_msgset_t *
rd_kafka_mock_msgset_find(const rd_kafka_mock_partition_t *mpart,
                          int64_t offset,
                          rd_bool_t on_follower) {
        const rd_kafka_mock_msgset_t *mset;

        if (!on_follower &&
            (offset < mpart->start_offset || offset > mpart->end_offset))
                return NULL;

        if (on_follower && (offset < mpart->follower_start_offset ||
                            offset > mpart->follower_end_offset))
                return NULL;

        /* FIXME: Maintain an index */

        TAILQ_FOREACH(mset, &mpart->msgsets, link) {
                if (mset->first_offset <= offset && offset <= mset->last_offset)
                        return mset;
        }

        return NULL;
}


/**
 * @brief Looks up or creates a new pidstate for the given partition and PID.
 *
 * The pidstate is used to verify per-partition per-producer BaseSequences
 * for the idempotent/txn producer.
 */
static rd_kafka_mock_pid_t *
rd_kafka_mock_partition_pidstate_get(rd_kafka_mock_partition_t *mpart,
                                     const rd_kafka_mock_pid_t *mpid) {
        rd_kafka_mock_pid_t *pidstate;
        size_t tidlen;

        pidstate = rd_list_find(&mpart->pidstates, mpid, rd_kafka_mock_pid_cmp);
        if (pidstate)
                return pidstate;

        tidlen        = strlen(mpid->TransactionalId);
        pidstate      = rd_malloc(sizeof(*pidstate) + tidlen);
        pidstate->pid = mpid->pid;
        memcpy(pidstate->TransactionalId, mpid->TransactionalId, tidlen);
        pidstate->TransactionalId[tidlen] = '\0';

        pidstate->lo = pidstate->hi = pidstate->window = 0;
        memset(pidstate->seq, 0, sizeof(pidstate->seq));

        rd_list_add(&mpart->pidstates, pidstate);

        return pidstate;
}


/**
 * @brief Validate ProduceRequest records in \p rkbuf.
 *
 * @warning The \p rkbuf must not be read, just peek()ed.
 *
 * This is a very selective validation, currently only:
 * - verify idempotency TransactionalId,PID,Epoch,Seq
 */
static rd_kafka_resp_err_t
rd_kafka_mock_validate_records(rd_kafka_mock_partition_t *mpart,
                               rd_kafka_buf_t *rkbuf,
                               size_t RecordCount,
                               const rd_kafkap_str_t *TransactionalId,
                               rd_bool_t *is_dupd) {
        const int log_decode_errors       = LOG_ERR;
        rd_kafka_mock_cluster_t *mcluster = mpart->topic->cluster;
        rd_kafka_mock_pid_t *mpid;
        rd_kafka_mock_pid_t *mpidstate = NULL;
        rd_kafka_pid_t pid;
        int32_t expected_BaseSequence = -1, BaseSequence = -1;
        rd_kafka_resp_err_t err;

        *is_dupd = rd_false;

        if (!TransactionalId || RD_KAFKAP_STR_LEN(TransactionalId) < 1)
                return RD_KAFKA_RESP_ERR_NO_ERROR;

        rd_kafka_buf_peek_i64(rkbuf, RD_KAFKAP_MSGSET_V2_OF_ProducerId,
                              &pid.id);
        rd_kafka_buf_peek_i16(rkbuf, RD_KAFKAP_MSGSET_V2_OF_ProducerEpoch,
                              &pid.epoch);
        rd_kafka_buf_peek_i32(rkbuf, RD_KAFKAP_MSGSET_V2_OF_BaseSequence,
                              &BaseSequence);

        mtx_lock(&mcluster->lock);
        err = rd_kafka_mock_pid_find(mcluster, TransactionalId, pid, &mpid);
        mtx_unlock(&mcluster->lock);

        if (likely(!err)) {

                if (mpid->pid.epoch != pid.epoch)
                        err = RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH;

                /* Each partition tracks the 5 last Produce requests per PID.*/
                mpidstate = rd_kafka_mock_partition_pidstate_get(mpart, mpid);

                expected_BaseSequence = mpidstate->seq[mpidstate->hi];

                /* A BaseSequence within the range of the last 5 requests is
                 * considered a legal duplicate and will be successfully acked
                 * but not written to the log. */
                if (BaseSequence < mpidstate->seq[mpidstate->lo])
                        err = RD_KAFKA_RESP_ERR_DUPLICATE_SEQUENCE_NUMBER;
                else if (BaseSequence > mpidstate->seq[mpidstate->hi])
                        err = RD_KAFKA_RESP_ERR_OUT_OF_ORDER_SEQUENCE_NUMBER;
                else if (BaseSequence != expected_BaseSequence)
                        *is_dupd = rd_true;
        }

        if (unlikely(err)) {
                rd_kafka_dbg(mcluster->rk, MOCK, "MOCK",
                             "Broker %" PRId32 ": Log append %s [%" PRId32
                             "] failed: PID mismatch: TransactionalId=%.*s "
                             "expected %s BaseSeq %" PRId32
                             ", not %s BaseSeq %" PRId32 ": %s",
                             mpart->leader->id, mpart->topic->name, mpart->id,
                             RD_KAFKAP_STR_PR(TransactionalId),
                             mpid ? rd_kafka_pid2str(mpid->pid) : "n/a",
                             expected_BaseSequence, rd_kafka_pid2str(pid),
                             BaseSequence, rd_kafka_err2name(err));
                return err;
        }

        /* Update BaseSequence window */
        if (unlikely(mpidstate->window < 5))
                mpidstate->window++;
        else
                mpidstate->lo = (mpidstate->lo + 1) % mpidstate->window;
        mpidstate->hi                 = (mpidstate->hi + 1) % mpidstate->window;
        mpidstate->seq[mpidstate->hi] = (int32_t)(BaseSequence + RecordCount);

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        return rkbuf->rkbuf_err;
}

/**
 * @brief Append the MessageSets in \p bytes to the \p mpart partition log.
 *
 * @param BaseOffset will contain the first assigned offset of the message set.
 */
rd_kafka_resp_err_t
rd_kafka_mock_partition_log_append(rd_kafka_mock_partition_t *mpart,
                                   const rd_kafkap_bytes_t *records,
                                   const rd_kafkap_str_t *TransactionalId,
                                   int64_t *BaseOffset) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_buf_t *rkbuf;
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        int8_t MagicByte;
        int32_t RecordCount;
        int16_t Attributes;
        rd_kafka_mock_msgset_t *mset;
        rd_bool_t is_dup = rd_false;

        /* Partially parse the MessageSet in \p bytes to get
         * the message count. */
        rkbuf = rd_kafka_buf_new_shadow(records->data,
                                        RD_KAFKAP_BYTES_LEN(records), NULL);

        rd_kafka_buf_peek_i8(rkbuf, RD_KAFKAP_MSGSET_V2_OF_MagicByte,
                             &MagicByte);
        if (MagicByte != 2) {
                /* We only support MsgVersion 2 for now */
                err = RD_KAFKA_RESP_ERR_UNSUPPORTED_VERSION;
                goto err;
        }

        rd_kafka_buf_peek_i32(rkbuf, RD_KAFKAP_MSGSET_V2_OF_RecordCount,
                              &RecordCount);
        rd_kafka_buf_peek_i16(rkbuf, RD_KAFKAP_MSGSET_V2_OF_Attributes,
                              &Attributes);

        if (RecordCount < 1 ||
            (!(Attributes & RD_KAFKA_MSG_ATTR_COMPRESSION_MASK) &&
             (size_t)RecordCount > RD_KAFKAP_BYTES_LEN(records) /
                                       RD_KAFKAP_MESSAGE_V2_MIN_OVERHEAD)) {
                err = RD_KAFKA_RESP_ERR_INVALID_MSG_SIZE;
                goto err;
        }

        if ((err = rd_kafka_mock_validate_records(
                 mpart, rkbuf, (size_t)RecordCount, TransactionalId, &is_dup)))
                goto err;

        /* If this is a legit duplicate, don't write it to the log. */
        if (is_dup)
                goto err;

        rd_kafka_buf_destroy(rkbuf);

        mset = rd_kafka_mock_msgset_new(mpart, records, (size_t)RecordCount);

        *BaseOffset = mset->first_offset;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        err = rkbuf->rkbuf_err;
err:
        rd_kafka_buf_destroy(rkbuf);
        return err;
}


/**
 * @brief Set the partition leader, or NULL for leader-less.
 */
static void
rd_kafka_mock_partition_set_leader0(rd_kafka_mock_partition_t *mpart,
                                    rd_kafka_mock_broker_t *mrkb) {
        mpart->leader = mrkb;
        mpart->leader_epoch++;
}


/**
 * @brief Verifies that the client-provided leader_epoch matches that of the
 *        partition, else returns the appropriate error.
 */
rd_kafka_resp_err_t rd_kafka_mock_partition_leader_epoch_check(
    const rd_kafka_mock_partition_t *mpart,
    int32_t leader_epoch) {
        if (likely(leader_epoch == -1 || mpart->leader_epoch == leader_epoch))
                return RD_KAFKA_RESP_ERR_NO_ERROR;
        else if (mpart->leader_epoch < leader_epoch)
                return RD_KAFKA_RESP_ERR_UNKNOWN_LEADER_EPOCH;
        else if (mpart->leader_epoch > leader_epoch)
                return RD_KAFKA_RESP_ERR_FENCED_LEADER_EPOCH;

        /* NOTREACHED, but avoids warning */
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Returns the end offset (last offset + 1)
 *        for the passed leader epoch in the mock partition.
 *
 * @param mpart The mock partition
 * @param leader_epoch The leader epoch
 *
 * @return The end offset for the passed \p leader_epoch in \p mpart
 */
int64_t rd_kafka_mock_partition_offset_for_leader_epoch(
    const rd_kafka_mock_partition_t *mpart,
    int32_t leader_epoch) {
        const rd_kafka_mock_msgset_t *mset = NULL;

        if (leader_epoch < 0)
                return -1;

        TAILQ_FOREACH_REVERSE(mset, &mpart->msgsets,
                              rd_kafka_mock_msgset_tailq_s, link) {
                if (mset->leader_epoch == leader_epoch)
                        return mset->last_offset + 1;
        }

        return -1;
}


/**
 * @brief Automatically assign replicas for partition
 */
static void
rd_kafka_mock_partition_assign_replicas(rd_kafka_mock_partition_t *mpart,
                                        int replication_factor) {
        rd_kafka_mock_cluster_t *mcluster = mpart->topic->cluster;
        int replica_cnt = RD_MIN(replication_factor, mcluster->broker_cnt);
        rd_kafka_mock_broker_t *mrkb;
        int i = 0;
        int first_replica;
        int skipped = 0;

        if (mpart->replicas)
                rd_free(mpart->replicas);

        mpart->replicas    = replica_cnt
                                 ? rd_calloc(replica_cnt, sizeof(*mpart->replicas))
                                 : NULL;
        mpart->replica_cnt = replica_cnt;
        if (replica_cnt == 0) {
                rd_kafka_mock_partition_set_leader0(mpart, NULL);
                return;
        }

        first_replica = (mpart->id * replication_factor) % mcluster->broker_cnt;

        /* Use a predictable, determininistic order on a per-topic basis.
         *
         * Two loops are needed for wraparound. */
        TAILQ_FOREACH(mrkb, &mcluster->brokers, link) {
                if (skipped < first_replica) {
                        skipped++;
                        continue;
                }
                if (i == mpart->replica_cnt)
                        break;
                mpart->replicas[i++] = mrkb;
        }
        TAILQ_FOREACH(mrkb, &mcluster->brokers, link) {
                if (i == mpart->replica_cnt)
                        break;
                mpart->replicas[i++] = mrkb;
        }

        /* Select a random leader */
        rd_kafka_mock_partition_set_leader0(
            mpart, mpart->replicas[rd_jitter(0, replica_cnt - 1)]);
}

/**
 * @brief Push a partition leader response to passed \p mpart .
 */
static void
rd_kafka_mock_partition_push_leader_response0(rd_kafka_mock_partition_t *mpart,
                                              int32_t leader_id,
                                              int32_t leader_epoch) {
        rd_kafka_mock_partition_leader_t *leader_response;

        leader_response               = rd_calloc(1, sizeof(*leader_response));
        leader_response->leader_id    = leader_id;
        leader_response->leader_epoch = leader_epoch;
        TAILQ_INSERT_TAIL(&mpart->leader_responses, leader_response, link);
}

/**
 * @brief Return the first mocked partition leader response in \p mpart ,
 *        if available.
 */
rd_kafka_mock_partition_leader_t *
rd_kafka_mock_partition_next_leader_response(rd_kafka_mock_partition_t *mpart) {
        return TAILQ_FIRST(&mpart->leader_responses);
}

/**
 * @brief Unlink and destroy a partition leader response
 */
void rd_kafka_mock_partition_leader_destroy(
    rd_kafka_mock_partition_t *mpart,
    rd_kafka_mock_partition_leader_t *mpart_leader) {
        TAILQ_REMOVE(&mpart->leader_responses, mpart_leader, link);
        rd_free(mpart_leader);
}

/**
 * @brief Unlink and destroy committed offset
 */
static void
rd_kafka_mock_committed_offset_destroy(rd_kafka_mock_partition_t *mpart,
                                       rd_kafka_mock_committed_offset_t *coff) {
        rd_kafkap_str_destroy(coff->metadata);
        TAILQ_REMOVE(&mpart->committed_offsets, coff, link);
        rd_free(coff);
}


/**
 * @brief Find previously committed offset for group.
 */
rd_kafka_mock_committed_offset_t *
rd_kafka_mock_committed_offset_find(const rd_kafka_mock_partition_t *mpart,
                                    const rd_kafkap_str_t *group) {
        const rd_kafka_mock_committed_offset_t *coff;

        TAILQ_FOREACH(coff, &mpart->committed_offsets, link) {
                if (!rd_kafkap_str_cmp_str(group, coff->group))
                        return (rd_kafka_mock_committed_offset_t *)coff;
        }

        return NULL;
}


/**
 * @brief Commit offset for group
 */
rd_kafka_mock_committed_offset_t *
rd_kafka_mock_commit_offset(rd_kafka_mock_partition_t *mpart,
                            const rd_kafkap_str_t *group,
                            int64_t offset,
                            const rd_kafkap_str_t *metadata) {
        rd_kafka_mock_committed_offset_t *coff;

        if (!(coff = rd_kafka_mock_committed_offset_find(mpart, group))) {
                size_t slen = (size_t)RD_KAFKAP_STR_LEN(group);

                coff = rd_malloc(sizeof(*coff) + slen + 1);

                coff->group = (char *)(coff + 1);
                memcpy(coff->group, group->str, slen);
                coff->group[slen] = '\0';

                coff->metadata = NULL;

                TAILQ_INSERT_HEAD(&mpart->committed_offsets, coff, link);
        }

        if (coff->metadata)
                rd_kafkap_str_destroy(coff->metadata);

        coff->metadata = rd_kafkap_str_copy(metadata);

        coff->offset = offset;

        rd_kafka_dbg(mpart->topic->cluster->rk, MOCK, "MOCK",
                     "Topic %s [%" PRId32 "] committing offset %" PRId64
                     " for group %.*s",
                     mpart->topic->name, mpart->id, offset,
                     RD_KAFKAP_STR_PR(group));

        return coff;
}

/**
 * @brief Destroy resources for partition, but the \p mpart itself is not freed.
 */
static void rd_kafka_mock_partition_destroy(rd_kafka_mock_partition_t *mpart) {
        rd_kafka_mock_msgset_t *mset, *tmp;
        rd_kafka_mock_committed_offset_t *coff, *tmpcoff;
        rd_kafka_mock_partition_leader_t *mpart_leader, *tmp_mpart_leader;

        TAILQ_FOREACH_SAFE(mset, &mpart->msgsets, link, tmp)
        rd_kafka_mock_msgset_destroy(mpart, mset);

        TAILQ_FOREACH_SAFE(coff, &mpart->committed_offsets, link, tmpcoff)
        rd_kafka_mock_committed_offset_destroy(mpart, coff);

        TAILQ_FOREACH_SAFE(mpart_leader, &mpart->leader_responses, link,
                           tmp_mpart_leader)
        rd_kafka_mock_partition_leader_destroy(mpart, mpart_leader);

        rd_list_destroy(&mpart->pidstates);

        rd_free(mpart->replicas);
}


static void rd_kafka_mock_partition_init(rd_kafka_mock_topic_t *mtopic,
                                         rd_kafka_mock_partition_t *mpart,
                                         int id,
                                         int replication_factor) {
        mpart->topic = mtopic;
        mpart->id    = id;

        mpart->follower_id  = -1;
        mpart->leader_epoch = -1; /* Start at -1 since assign_replicas() will
                                   * bump it right away to 0. */

        TAILQ_INIT(&mpart->msgsets);

        mpart->max_size = 1024 * 1024 * 5;
        mpart->max_cnt  = 100000;

        mpart->update_follower_start_offset = rd_true;
        mpart->update_follower_end_offset   = rd_true;

        TAILQ_INIT(&mpart->committed_offsets);
        TAILQ_INIT(&mpart->leader_responses);

        rd_list_init(&mpart->pidstates, 0, rd_free);

        rd_kafka_mock_partition_assign_replicas(mpart, replication_factor);
}

rd_kafka_mock_partition_t *
rd_kafka_mock_partition_find(const rd_kafka_mock_topic_t *mtopic,
                             int32_t partition) {
        if (!mtopic || partition < 0 || partition >= mtopic->partition_cnt)
                return NULL;

        return (rd_kafka_mock_partition_t *)&mtopic->partitions[partition];
}


static void rd_kafka_mock_topic_destroy(rd_kafka_mock_topic_t *mtopic) {
        int i;

        for (i = 0; i < mtopic->partition_cnt; i++)
                rd_kafka_mock_partition_destroy(&mtopic->partitions[i]);

        TAILQ_REMOVE(&mtopic->cluster->topics, mtopic, link);
        mtopic->cluster->topic_cnt--;

        rd_free(mtopic->partitions);
        rd_free(mtopic->name);
        rd_free(mtopic);
}


static rd_kafka_mock_topic_t *
rd_kafka_mock_topic_new(rd_kafka_mock_cluster_t *mcluster,
                        const char *topic,
                        int partition_cnt,
                        int replication_factor) {
        rd_kafka_mock_topic_t *mtopic;
        int i;

        mtopic = rd_calloc(1, sizeof(*mtopic));
        /* Assign random topic id */
        mtopic->id      = rd_kafka_Uuid_random();
        mtopic->name    = rd_strdup(topic);
        mtopic->cluster = mcluster;

        mtopic->partition_cnt = partition_cnt;
        mtopic->partitions =
            rd_calloc(partition_cnt, sizeof(*mtopic->partitions));

        for (i = 0; i < partition_cnt; i++)
                rd_kafka_mock_partition_init(mtopic, &mtopic->partitions[i], i,
                                             replication_factor);

        TAILQ_INSERT_TAIL(&mcluster->topics, mtopic, link);
        mcluster->topic_cnt++;

        rd_kafka_dbg(mcluster->rk, MOCK, "MOCK",
                     "Created topic \"%s\" with %d partition(s) and "
                     "replication-factor %d",
                     mtopic->name, mtopic->partition_cnt, replication_factor);

        return mtopic;
}


rd_kafka_mock_topic_t *
rd_kafka_mock_topic_find(const rd_kafka_mock_cluster_t *mcluster,
                         const char *name) {
        const rd_kafka_mock_topic_t *mtopic;

        TAILQ_FOREACH(mtopic, &mcluster->topics, link) {
                if (!strcmp(mtopic->name, name))
                        return (rd_kafka_mock_topic_t *)mtopic;
        }

        return NULL;
}


rd_kafka_mock_topic_t *
rd_kafka_mock_topic_find_by_kstr(const rd_kafka_mock_cluster_t *mcluster,
                                 const rd_kafkap_str_t *kname) {
        const rd_kafka_mock_topic_t *mtopic;

        TAILQ_FOREACH(mtopic, &mcluster->topics, link) {
                if (!strncmp(mtopic->name, kname->str,
                             RD_KAFKAP_STR_LEN(kname)) &&
                    mtopic->name[RD_KAFKAP_STR_LEN(kname)] == '\0')
                        return (rd_kafka_mock_topic_t *)mtopic;
        }

        return NULL;
}

/**
 * @brief Find a mock topic by id.
 *
 * @param mcluster Cluster to search in.
 * @param id Topic id to find.
 * @return Found topic or NULL.
 *
 * @locks mcluster->lock MUST be held.
 */
rd_kafka_mock_topic_t *
rd_kafka_mock_topic_find_by_id(const rd_kafka_mock_cluster_t *mcluster,
                               rd_kafka_Uuid_t id) {
        const rd_kafka_mock_topic_t *mtopic;

        TAILQ_FOREACH(mtopic, &mcluster->topics, link) {
                if (!rd_kafka_Uuid_cmp(mtopic->id, id))
                        return (rd_kafka_mock_topic_t *)mtopic;
        }

        return NULL;
}


/**
 * @brief Create a topic using default settings.
 *        The topic must not already exist.
 *
 * @param errp will be set to an error code that is consistent with
 *             new topics on real clusters.
 */
rd_kafka_mock_topic_t *
rd_kafka_mock_topic_auto_create(rd_kafka_mock_cluster_t *mcluster,
                                const char *topic,
                                int partition_cnt,
                                rd_kafka_resp_err_t *errp) {
        rd_assert(!rd_kafka_mock_topic_find(mcluster, topic));
        *errp = 0;  // FIXME? RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE;
        return rd_kafka_mock_topic_new(mcluster, topic,
                                       partition_cnt == -1
                                           ? mcluster->defaults.partition_cnt
                                           : partition_cnt,
                                       mcluster->defaults.replication_factor);
}


/**
 * @brief Find or create topic.
 *
 * @param partition_cnt If not -1 and the topic does not exist, the automatic
 *                      topic creation will create this number of topics.
 *                      Otherwise use the default.
 */
rd_kafka_mock_topic_t *
rd_kafka_mock_topic_get(rd_kafka_mock_cluster_t *mcluster,
                        const char *topic,
                        int partition_cnt) {
        rd_kafka_mock_topic_t *mtopic;
        rd_kafka_resp_err_t err;

        if ((mtopic = rd_kafka_mock_topic_find(mcluster, topic)))
                return mtopic;

        return rd_kafka_mock_topic_auto_create(mcluster, topic, partition_cnt,
                                               &err);
}

/**
 * @brief Find or create a partition.
 *
 * @returns NULL if topic already exists and partition is out of range.
 */
static rd_kafka_mock_partition_t *
rd_kafka_mock_partition_get(rd_kafka_mock_cluster_t *mcluster,
                            const char *topic,
                            int32_t partition) {
        rd_kafka_mock_topic_t *mtopic;
        rd_kafka_resp_err_t err;

        if (!(mtopic = rd_kafka_mock_topic_find(mcluster, topic)))
                mtopic = rd_kafka_mock_topic_auto_create(mcluster, topic,
                                                         partition + 1, &err);

        if (partition >= mtopic->partition_cnt)
                return NULL;

        return &mtopic->partitions[partition];
}


/**
 * @brief Set IO events for fd
 */
static void
rd_kafka_mock_cluster_io_set_events(rd_kafka_mock_cluster_t *mcluster,
                                    rd_socket_t fd,
                                    int events) {
        int i;

        for (i = 0; i < mcluster->fd_cnt; i++) {
                if (mcluster->fds[i].fd == fd) {
                        mcluster->fds[i].events |= events;
                        return;
                }
        }

        rd_assert(!*"mock_cluster_io_set_events: fd not found");
}

/**
 * @brief Set or clear single IO events for fd
 */
static void
rd_kafka_mock_cluster_io_set_event(rd_kafka_mock_cluster_t *mcluster,
                                   rd_socket_t fd,
                                   rd_bool_t set,
                                   int event) {
        int i;

        for (i = 0; i < mcluster->fd_cnt; i++) {
                if (mcluster->fds[i].fd == fd) {
                        if (set)
                                mcluster->fds[i].events |= event;
                        else
                                mcluster->fds[i].events &= ~event;
                        return;
                }
        }

        rd_assert(!*"mock_cluster_io_set_event: fd not found");
}


/**
 * @brief Clear IO events for fd
 */
static void
rd_kafka_mock_cluster_io_clear_events(rd_kafka_mock_cluster_t *mcluster,
                                      rd_socket_t fd,
                                      int events) {
        int i;

        for (i = 0; i < mcluster->fd_cnt; i++) {
                if (mcluster->fds[i].fd == fd) {
                        mcluster->fds[i].events &= ~events;
                        return;
                }
        }

        rd_assert(!*"mock_cluster_io_set_events: fd not found");
}


static void rd_kafka_mock_cluster_io_del(rd_kafka_mock_cluster_t *mcluster,
                                         rd_socket_t fd) {
        int i;

        for (i = 0; i < mcluster->fd_cnt; i++) {
                if (mcluster->fds[i].fd == fd) {
                        if (i + 1 < mcluster->fd_cnt) {
                                memmove(&mcluster->fds[i],
                                        &mcluster->fds[i + 1],
                                        sizeof(*mcluster->fds) *
                                            (mcluster->fd_cnt - i));
                                memmove(&mcluster->handlers[i],
                                        &mcluster->handlers[i + 1],
                                        sizeof(*mcluster->handlers) *
                                            (mcluster->fd_cnt - i));
                        }

                        mcluster->fd_cnt--;
                        return;
                }
        }

        rd_assert(!*"mock_cluster_io_del: fd not found");
}


/**
 * @brief Add \p fd to IO poll with initial desired events (POLLIN, et.al).
 */
static void rd_kafka_mock_cluster_io_add(rd_kafka_mock_cluster_t *mcluster,
                                         rd_socket_t fd,
                                         int events,
                                         rd_kafka_mock_io_handler_t handler,
                                         void *opaque) {

        if (mcluster->fd_cnt + 1 >= mcluster->fd_size) {
                mcluster->fd_size += 8;

                mcluster->fds = rd_realloc(
                    mcluster->fds, sizeof(*mcluster->fds) * mcluster->fd_size);
                mcluster->handlers =
                    rd_realloc(mcluster->handlers,
                               sizeof(*mcluster->handlers) * mcluster->fd_size);
        }

        memset(&mcluster->fds[mcluster->fd_cnt], 0,
               sizeof(mcluster->fds[mcluster->fd_cnt]));
        mcluster->fds[mcluster->fd_cnt].fd          = fd;
        mcluster->fds[mcluster->fd_cnt].events      = events;
        mcluster->fds[mcluster->fd_cnt].revents     = 0;
        mcluster->handlers[mcluster->fd_cnt].cb     = handler;
        mcluster->handlers[mcluster->fd_cnt].opaque = opaque;
        mcluster->fd_cnt++;
}

/**
 * @brief Reassign partition replicas to broker, after deleting or
 *        adding a new one.
 */
static void
rd_kafka_mock_cluster_reassign_partitions(rd_kafka_mock_cluster_t *mcluster) {
        rd_kafka_mock_topic_t *mtopic;
        TAILQ_FOREACH(mtopic, &mcluster->topics, link) {
                int i;
                for (i = 0; i < mtopic->partition_cnt; i++) {
                        rd_kafka_mock_partition_t *mpart =
                            &mtopic->partitions[i];
                        rd_kafka_mock_partition_assign_replicas(
                            mpart, mpart->replica_cnt);
                }
        }
}

static void rd_kafka_mock_connection_close(rd_kafka_mock_connection_t *mconn,
                                           const char *reason) {
        rd_kafka_buf_t *rkbuf;

        rd_kafka_dbg(mconn->broker->cluster->rk, MOCK, "MOCK",
                     "Broker %" PRId32 ": Connection from %s closed: %s",
                     mconn->broker->id,
                     rd_sockaddr2str(&mconn->peer, RD_SOCKADDR2STR_F_PORT),
                     reason);

        rd_kafka_mock_cgrps_connection_closed(mconn->broker->cluster, mconn);

        rd_kafka_timer_stop(&mconn->broker->cluster->timers, &mconn->write_tmr,
                            rd_true);

        while ((rkbuf = TAILQ_FIRST(&mconn->outbufs.rkbq_bufs))) {
                rd_kafka_bufq_deq(&mconn->outbufs, rkbuf);
                rd_kafka_buf_destroy(rkbuf);
        }

        if (mconn->rxbuf)
                rd_kafka_buf_destroy(mconn->rxbuf);

        rd_kafka_mock_cluster_io_del(mconn->broker->cluster,
                                     mconn->transport->rktrans_s);
        TAILQ_REMOVE(&mconn->broker->connections, mconn, link);
        rd_kafka_transport_close(mconn->transport);
        rd_free(mconn);
}

void rd_kafka_mock_connection_send_response0(rd_kafka_mock_connection_t *mconn,
                                             rd_kafka_buf_t *resp,
                                             rd_bool_t tags_written) {

        if (!tags_written && (resp->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER)) {
                /* Empty struct tags */
                rd_kafka_buf_write_i8(resp, 0);
        }

        /* rkbuf_ts_sent might be initialized with a RTT delay, else 0. */
        resp->rkbuf_ts_sent += rd_clock();

        resp->rkbuf_reshdr.Size =
            (int32_t)(rd_buf_write_pos(&resp->rkbuf_buf) - 4);

        rd_kafka_buf_update_i32(resp, 0, resp->rkbuf_reshdr.Size);

        rd_kafka_dbg(mconn->broker->cluster->rk, MOCK, "MOCK",
                     "Broker %" PRId32 ": Sending %sResponseV%hd to %s",
                     mconn->broker->id,
                     rd_kafka_ApiKey2str(resp->rkbuf_reqhdr.ApiKey),
                     resp->rkbuf_reqhdr.ApiVersion,
                     rd_sockaddr2str(&mconn->peer, RD_SOCKADDR2STR_F_PORT));

        /* Set up a buffer reader for sending the buffer. */
        rd_slice_init_full(&resp->rkbuf_reader, &resp->rkbuf_buf);

        rd_kafka_bufq_enq(&mconn->outbufs, resp);

        rd_kafka_mock_cluster_io_set_events(
            mconn->broker->cluster, mconn->transport->rktrans_s, POLLOUT);
}


/**
 * @returns 1 if a complete request is available in which case \p slicep
 *          is set to a new slice containing the data,
 *          0 if a complete request is not yet available,
 *          -1 on error.
 */
static int
rd_kafka_mock_connection_read_request(rd_kafka_mock_connection_t *mconn,
                                      rd_kafka_buf_t **rkbufp) {
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        rd_kafka_t *rk                    = mcluster->rk;
        const rd_bool_t log_decode_errors = rd_true;
        rd_kafka_buf_t *rkbuf;
        char errstr[128];
        ssize_t r;

        if (!(rkbuf = mconn->rxbuf)) {
                /* Initial read for a protocol request.
                 * Allocate enough room for the protocol header
                 * (where the total size is located). */
                rkbuf = mconn->rxbuf =
                    rd_kafka_buf_new(2, RD_KAFKAP_REQHDR_SIZE);

                /* Protocol parsing code needs the rkb for logging */
                rkbuf->rkbuf_rkb = mconn->broker->cluster->dummy_rkb;
                rd_kafka_broker_keep(rkbuf->rkbuf_rkb);

                /* Make room for request header */
                rd_buf_write_ensure(&rkbuf->rkbuf_buf, RD_KAFKAP_REQHDR_SIZE,
                                    RD_KAFKAP_REQHDR_SIZE);
        }

        /* Read as much data as possible from the socket into the
         * connection receive buffer. */
        r = rd_kafka_transport_recv(mconn->transport, &rkbuf->rkbuf_buf, errstr,
                                    sizeof(errstr));
        if (r == -1) {
                rd_kafka_dbg(
                    rk, MOCK, "MOCK",
                    "Broker %" PRId32
                    ": Connection %s: "
                    "receive failed: %s",
                    mconn->broker->id,
                    rd_sockaddr2str(&mconn->peer, RD_SOCKADDR2STR_F_PORT),
                    errstr);
                return -1;
        } else if (r == 0) {
                return 0; /* Need more data */
        }

        if (rd_buf_write_pos(&rkbuf->rkbuf_buf) == RD_KAFKAP_REQHDR_SIZE) {
                /* Received the full header, now check full request
                 * size and allocate the buffer accordingly. */

                /* Initialize reader */
                rd_slice_init(&rkbuf->rkbuf_reader, &rkbuf->rkbuf_buf, 0,
                              RD_KAFKAP_REQHDR_SIZE);

                rd_kafka_buf_read_i32(rkbuf, &rkbuf->rkbuf_reqhdr.Size);
                rd_kafka_buf_read_i16(rkbuf, &rkbuf->rkbuf_reqhdr.ApiKey);
                rd_kafka_buf_read_i16(rkbuf, &rkbuf->rkbuf_reqhdr.ApiVersion);

                if (rkbuf->rkbuf_reqhdr.ApiKey < 0 ||
                    rkbuf->rkbuf_reqhdr.ApiKey >= RD_KAFKAP__NUM) {
                        rd_kafka_buf_parse_fail(
                            rkbuf, "Invalid ApiKey %hd from %s",
                            rkbuf->rkbuf_reqhdr.ApiKey,
                            rd_sockaddr2str(&mconn->peer,
                                            RD_SOCKADDR2STR_F_PORT));
                        RD_NOTREACHED();
                }

                /* Check if request version has flexible fields (KIP-482) */
                if (mcluster->api_handlers[rkbuf->rkbuf_reqhdr.ApiKey]
                            .FlexVersion != -1 &&
                    rkbuf->rkbuf_reqhdr.ApiVersion >=
                        mcluster->api_handlers[rkbuf->rkbuf_reqhdr.ApiKey]
                            .FlexVersion)
                        rkbuf->rkbuf_flags |= RD_KAFKA_OP_F_FLEXVER;


                rd_kafka_buf_read_i32(rkbuf, &rkbuf->rkbuf_reqhdr.CorrId);

                rkbuf->rkbuf_totlen = rkbuf->rkbuf_reqhdr.Size + 4;

                if (rkbuf->rkbuf_totlen < RD_KAFKAP_REQHDR_SIZE + 2 ||
                    rkbuf->rkbuf_totlen >
                        (size_t)rk->rk_conf.recv_max_msg_size) {
                        rd_kafka_buf_parse_fail(
                            rkbuf, "Invalid request size %" PRId32 " from %s",
                            rkbuf->rkbuf_reqhdr.Size,
                            rd_sockaddr2str(&mconn->peer,
                                            RD_SOCKADDR2STR_F_PORT));
                        RD_NOTREACHED();
                }

                /* Now adjust totlen to skip the header */
                rkbuf->rkbuf_totlen -= RD_KAFKAP_REQHDR_SIZE;

                if (!rkbuf->rkbuf_totlen) {
                        /* Empty request (valid) */
                        *rkbufp      = rkbuf;
                        mconn->rxbuf = NULL;
                        return 1;
                }

                /* Allocate space for the request payload */
                rd_buf_write_ensure(&rkbuf->rkbuf_buf, rkbuf->rkbuf_totlen,
                                    rkbuf->rkbuf_totlen);

        } else if (rd_buf_write_pos(&rkbuf->rkbuf_buf) -
                       RD_KAFKAP_REQHDR_SIZE ==
                   rkbuf->rkbuf_totlen) {
                /* The full request is now read into the buffer. */

                /* Set up response reader slice starting past the
                 * request header */
                rd_slice_init(&rkbuf->rkbuf_reader, &rkbuf->rkbuf_buf,
                              RD_KAFKAP_REQHDR_SIZE,
                              rd_buf_len(&rkbuf->rkbuf_buf) -
                                  RD_KAFKAP_REQHDR_SIZE);

                /* For convenience, shave off the ClientId */
                rd_kafka_buf_skip_str_no_flexver(rkbuf);

                /* And the flexible versions header tags, if any */
                rd_kafka_buf_skip_tags(rkbuf);

                /* Return the buffer to the caller */
                *rkbufp      = rkbuf;
                mconn->rxbuf = NULL;
                return 1;
        }

        return 0;


err_parse:
        return -1;
}

rd_kafka_buf_t *rd_kafka_mock_buf_new_response(const rd_kafka_buf_t *request) {
        rd_kafka_buf_t *rkbuf = rd_kafka_buf_new(1, 100);

        /* Copy request header so the ApiVersion remains known */
        rkbuf->rkbuf_reqhdr = request->rkbuf_reqhdr;

        /* Size, updated later */
        rd_kafka_buf_write_i32(rkbuf, 0);

        /* CorrId */
        rd_kafka_buf_write_i32(rkbuf, request->rkbuf_reqhdr.CorrId);

        if (request->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER) {
                rkbuf->rkbuf_flags |= RD_KAFKA_OP_F_FLEXVER;
                /* Write empty response header tags, unless this is the
                 * ApiVersionResponse which needs to be backwards compatible. */
                if (request->rkbuf_reqhdr.ApiKey != RD_KAFKAP_ApiVersion)
                        rd_kafka_buf_write_i8(rkbuf, 0);
        }

        return rkbuf;
}



/**
 * @brief Parse protocol request.
 *
 * @returns 0 on success, -1 on parse error.
 */
static int
rd_kafka_mock_connection_parse_request(rd_kafka_mock_connection_t *mconn,
                                       rd_kafka_buf_t *rkbuf) {
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        rd_kafka_t *rk                    = mcluster->rk;

        if (rkbuf->rkbuf_reqhdr.ApiKey < 0 ||
            rkbuf->rkbuf_reqhdr.ApiKey >= RD_KAFKAP__NUM ||
            !mcluster->api_handlers[rkbuf->rkbuf_reqhdr.ApiKey].cb) {
                rd_kafka_log(
                    rk, LOG_ERR, "MOCK",
                    "Broker %" PRId32
                    ": unsupported %sRequestV%hd "
                    "from %s",
                    mconn->broker->id,
                    rd_kafka_ApiKey2str(rkbuf->rkbuf_reqhdr.ApiKey),
                    rkbuf->rkbuf_reqhdr.ApiVersion,
                    rd_sockaddr2str(&mconn->peer, RD_SOCKADDR2STR_F_PORT));
                return -1;
        }

        /* ApiVersionRequest handles future versions, for everything else
         * make sure the ApiVersion is supported. */
        if (rkbuf->rkbuf_reqhdr.ApiKey != RD_KAFKAP_ApiVersion &&
            !rd_kafka_mock_cluster_ApiVersion_check(
                mcluster, rkbuf->rkbuf_reqhdr.ApiKey,
                rkbuf->rkbuf_reqhdr.ApiVersion)) {
                rd_kafka_log(
                    rk, LOG_ERR, "MOCK",
                    "Broker %" PRId32
                    ": unsupported %sRequest "
                    "version %hd from %s",
                    mconn->broker->id,
                    rd_kafka_ApiKey2str(rkbuf->rkbuf_reqhdr.ApiKey),
                    rkbuf->rkbuf_reqhdr.ApiVersion,
                    rd_sockaddr2str(&mconn->peer, RD_SOCKADDR2STR_F_PORT));
                return -1;
        }

        mtx_lock(&mcluster->lock);
        if (mcluster->track_requests) {
                rd_list_add(&mcluster->request_list,
                            rd_kafka_mock_request_new(
                                mconn->broker->id, rkbuf->rkbuf_reqhdr.ApiKey,
                                rd_clock()));
        }
        mtx_unlock(&mcluster->lock);

        rd_kafka_dbg(rk, MOCK, "MOCK",
                     "Broker %" PRId32 ": Received %sRequestV%hd from %s",
                     mconn->broker->id,
                     rd_kafka_ApiKey2str(rkbuf->rkbuf_reqhdr.ApiKey),
                     rkbuf->rkbuf_reqhdr.ApiVersion,
                     rd_sockaddr2str(&mconn->peer, RD_SOCKADDR2STR_F_PORT));

        return mcluster->api_handlers[rkbuf->rkbuf_reqhdr.ApiKey].cb(mconn,
                                                                     rkbuf);
}


/**
 * @brief Timer callback to set the POLLOUT flag for a connection after
 *        the delay has expired.
 */
static void rd_kafka_mock_connection_write_out_tmr_cb(rd_kafka_timers_t *rkts,
                                                      void *arg) {
        rd_kafka_mock_connection_t *mconn = arg;

        rd_kafka_mock_cluster_io_set_events(
            mconn->broker->cluster, mconn->transport->rktrans_s, POLLOUT);
}


/**
 * @brief Send as many bytes as possible from the output buffer.
 *
 * @returns 1 if all buffers were sent, 0 if more buffers need to be sent, or
 *          -1 on error.
 */
static ssize_t
rd_kafka_mock_connection_write_out(rd_kafka_mock_connection_t *mconn) {
        rd_kafka_buf_t *rkbuf;
        rd_ts_t now = rd_clock();
        rd_ts_t rtt = mconn->broker->rtt;

        while ((rkbuf = TAILQ_FIRST(&mconn->outbufs.rkbq_bufs))) {
                ssize_t r;
                char errstr[128];
                rd_ts_t ts_delay = 0;

                /* Connection delay/rtt is set. */
                if (rkbuf->rkbuf_ts_sent + rtt > now)
                        ts_delay = rkbuf->rkbuf_ts_sent + rtt;

                /* Response is being delayed */
                if (rkbuf->rkbuf_ts_retry && rkbuf->rkbuf_ts_retry > now)
                        ts_delay = rkbuf->rkbuf_ts_retry + rtt;

                if (ts_delay) {
                        /* Delay response */
                        rd_kafka_timer_start_oneshot(
                            &mconn->broker->cluster->timers, &mconn->write_tmr,
                            rd_false, ts_delay - now,
                            rd_kafka_mock_connection_write_out_tmr_cb, mconn);
                        break;
                }

                if ((r = rd_kafka_transport_send(mconn->transport,
                                                 &rkbuf->rkbuf_reader, errstr,
                                                 sizeof(errstr))) == -1)
                        return -1;

                if (rd_slice_remains(&rkbuf->rkbuf_reader) > 0)
                        return 0; /* Partial send, continue next time */

                /* Entire buffer sent, unlink and free */
                rd_kafka_bufq_deq(&mconn->outbufs, rkbuf);

                rd_kafka_buf_destroy(rkbuf);
        }

        rd_kafka_mock_cluster_io_clear_events(
            mconn->broker->cluster, mconn->transport->rktrans_s, POLLOUT);

        return 1;
}


/**
 * @brief Call connection_write_out() for all the broker's connections.
 *
 * Use to check if any responses should be sent when RTT has changed.
 */
static void
rd_kafka_mock_broker_connections_write_out(rd_kafka_mock_broker_t *mrkb) {
        rd_kafka_mock_connection_t *mconn, *tmp;

        /* Need a safe loop since connections may be removed on send error */
        TAILQ_FOREACH_SAFE(mconn, &mrkb->connections, link, tmp) {
                rd_kafka_mock_connection_write_out(mconn);
        }
}


/**
 * @brief Per-Connection IO handler
 */
static void rd_kafka_mock_connection_io(rd_kafka_mock_cluster_t *mcluster,
                                        rd_socket_t fd,
                                        int events,
                                        void *opaque) {
        rd_kafka_mock_connection_t *mconn = opaque;

        if (events & POLLIN) {
                rd_kafka_buf_t *rkbuf;
                int r;

                while (1) {
                        /* Read full request */
                        r = rd_kafka_mock_connection_read_request(mconn,
                                                                  &rkbuf);
                        if (r == 0)
                                break; /* Need more data */
                        else if (r == -1) {
                                rd_kafka_mock_connection_close(mconn,
                                                               "Read error");
                                return;
                        }

                        /* Parse and handle request */
                        r = rd_kafka_mock_connection_parse_request(mconn,
                                                                   rkbuf);
                        rd_kafka_buf_destroy(rkbuf);
                        if (r == -1) {
                                rd_kafka_mock_connection_close(mconn,
                                                               "Parse error");
                                return;
                        }
                }
        }

        if (events & (POLLERR | POLLHUP)) {
                rd_kafka_mock_connection_close(mconn, "Disconnected");
                return;
        }

        if (events & POLLOUT) {
                if (rd_kafka_mock_connection_write_out(mconn) == -1) {
                        rd_kafka_mock_connection_close(mconn, "Write error");
                        return;
                }
        }
}


/**
 * @brief Set connection as blocking, POLLIN will not be served.
 */
void rd_kafka_mock_connection_set_blocking(rd_kafka_mock_connection_t *mconn,
                                           rd_bool_t blocking) {
        rd_kafka_mock_cluster_io_set_event(mconn->broker->cluster,
                                           mconn->transport->rktrans_s,
                                           !blocking, POLLIN);
}


static rd_kafka_mock_connection_t *
rd_kafka_mock_connection_new(rd_kafka_mock_broker_t *mrkb,
                             rd_socket_t fd,
                             const struct sockaddr_in *peer) {
        rd_kafka_mock_connection_t *mconn;
        rd_kafka_transport_t *rktrans;
        char errstr[128];

        if (!mrkb->up) {
                rd_socket_close(fd);
                return NULL;
        }

        rktrans = rd_kafka_transport_new(mrkb->cluster->dummy_rkb, fd, errstr,
                                         sizeof(errstr));
        if (!rktrans) {
                rd_kafka_log(mrkb->cluster->rk, LOG_ERR, "MOCK",
                             "Failed to create transport for new "
                             "mock connection: %s",
                             errstr);
                rd_socket_close(fd);
                return NULL;
        }

        rd_kafka_transport_post_connect_setup(rktrans);

        mconn            = rd_calloc(1, sizeof(*mconn));
        mconn->broker    = mrkb;
        mconn->transport = rktrans;
        mconn->peer      = *peer;
        rd_kafka_bufq_init(&mconn->outbufs);

        TAILQ_INSERT_TAIL(&mrkb->connections, mconn, link);

        rd_kafka_mock_cluster_io_add(mrkb->cluster, mconn->transport->rktrans_s,
                                     POLLIN, rd_kafka_mock_connection_io,
                                     mconn);

        rd_kafka_dbg(mrkb->cluster->rk, MOCK, "MOCK",
                     "Broker %" PRId32 ": New connection from %s", mrkb->id,
                     rd_sockaddr2str(&mconn->peer, RD_SOCKADDR2STR_F_PORT));

        return mconn;
}



static void rd_kafka_mock_cluster_op_io(rd_kafka_mock_cluster_t *mcluster,
                                        rd_socket_t fd,
                                        int events,
                                        void *opaque) {
        /* Read wake-up fd data and throw away, just used for wake-ups*/
        char buf[1024];
        while (rd_socket_read(fd, buf, sizeof(buf)) > 0)
                ; /* Read all buffered signalling bytes */
}


static int rd_kafka_mock_cluster_io_poll(rd_kafka_mock_cluster_t *mcluster,
                                         int timeout_ms) {
        int r;
        int i;

        r = rd_socket_poll(mcluster->fds, mcluster->fd_cnt, timeout_ms);
        if (r == RD_SOCKET_ERROR) {
                rd_kafka_log(mcluster->rk, LOG_CRIT, "MOCK",
                             "Mock cluster failed to poll %d fds: %d: %s",
                             mcluster->fd_cnt, r,
                             rd_socket_strerror(rd_socket_errno));
                return -1;
        }

        /* Serve ops, if any */
        rd_kafka_q_serve(mcluster->ops, RD_POLL_NOWAIT, 0,
                         RD_KAFKA_Q_CB_CALLBACK, NULL, NULL);

        /* Handle IO events, if any, and if not terminating */
        for (i = 0; mcluster->run && r > 0 && i < mcluster->fd_cnt; i++) {
                if (!mcluster->fds[i].revents)
                        continue;

                /* Call IO handler */
                mcluster->handlers[i].cb(mcluster, mcluster->fds[i].fd,
                                         mcluster->fds[i].revents,
                                         mcluster->handlers[i].opaque);
                r--;
        }

        return 0;
}


static int rd_kafka_mock_cluster_thread_main(void *arg) {
        rd_kafka_mock_cluster_t *mcluster = arg;

        rd_kafka_set_thread_name("mock");
        rd_kafka_set_thread_sysname("rdk:mock");
        rd_kafka_interceptors_on_thread_start(mcluster->rk,
                                              RD_KAFKA_THREAD_BACKGROUND);
        rd_atomic32_add(&rd_kafka_thread_cnt_curr, 1);

        /* Op wakeup fd */
        rd_kafka_mock_cluster_io_add(mcluster, mcluster->wakeup_fds[0], POLLIN,
                                     rd_kafka_mock_cluster_op_io, NULL);

        mcluster->run = rd_true;

        while (mcluster->run) {
                int sleeptime = (int)((rd_kafka_timers_next(&mcluster->timers,
                                                            1000 * 1000 /*1s*/,
                                                            1 /*lock*/) +
                                       999) /
                                      1000);

                if (rd_kafka_mock_cluster_io_poll(mcluster, sleeptime) == -1)
                        break;

                rd_kafka_timers_run(&mcluster->timers, RD_POLL_NOWAIT);
        }

        rd_kafka_mock_cluster_io_del(mcluster, mcluster->wakeup_fds[0]);


        rd_kafka_interceptors_on_thread_exit(mcluster->rk,
                                             RD_KAFKA_THREAD_BACKGROUND);
        rd_atomic32_sub(&rd_kafka_thread_cnt_curr, 1);

        rd_kafka_mock_cluster_destroy0(mcluster);

        return 0;
}



static void rd_kafka_mock_broker_listen_io(rd_kafka_mock_cluster_t *mcluster,
                                           rd_socket_t fd,
                                           int events,
                                           void *opaque) {
        rd_kafka_mock_broker_t *mrkb = opaque;

        if (events & (POLLERR | POLLHUP))
                rd_assert(!*"Mock broker listen socket error");

        if (events & POLLIN) {
                rd_socket_t new_s;
                struct sockaddr_in peer;
                socklen_t peer_size = sizeof(peer);

                new_s = accept(mrkb->listen_s, (struct sockaddr *)&peer,
                               &peer_size);
                if (new_s == RD_SOCKET_ERROR) {
                        rd_kafka_log(mcluster->rk, LOG_ERR, "MOCK",
                                     "Failed to accept mock broker socket: %s",
                                     rd_socket_strerror(rd_socket_errno));
                        return;
                }

                rd_kafka_mock_connection_new(mrkb, new_s, &peer);
        }
}


/**
 * @brief Close all connections to broker.
 */
static void rd_kafka_mock_broker_close_all(rd_kafka_mock_broker_t *mrkb,
                                           const char *reason) {
        rd_kafka_mock_connection_t *mconn;

        while ((mconn = TAILQ_FIRST(&mrkb->connections)))
                rd_kafka_mock_connection_close(mconn, reason);
}

/**
 * @brief Destroy error stack, must be unlinked.
 */
static void
rd_kafka_mock_error_stack_destroy(rd_kafka_mock_error_stack_t *errstack) {
        if (errstack->errs)
                rd_free(errstack->errs);
        rd_free(errstack);
}


static void rd_kafka_mock_broker_destroy(rd_kafka_mock_broker_t *mrkb) {
        rd_kafka_mock_error_stack_t *errstack;

        rd_kafka_mock_broker_close_all(mrkb, "Destroying broker");

        if (mrkb->listen_s != -1) {
                if (mrkb->up)
                        rd_kafka_mock_cluster_io_del(mrkb->cluster,
                                                     mrkb->listen_s);
                rd_socket_close(mrkb->listen_s);
        }

        while ((errstack = TAILQ_FIRST(&mrkb->errstacks))) {
                TAILQ_REMOVE(&mrkb->errstacks, errstack, link);
                rd_kafka_mock_error_stack_destroy(errstack);
        }

        if (mrkb->rack)
                rd_free(mrkb->rack);

        rd_kafka_mock_coord_remove(mrkb->cluster, mrkb->id);

        TAILQ_REMOVE(&mrkb->cluster->brokers, mrkb, link);
        mrkb->cluster->broker_cnt--;

        rd_free(mrkb);
}


rd_kafka_resp_err_t
rd_kafka_mock_broker_decommission(rd_kafka_mock_cluster_t *mcluster,
                                  int32_t broker_id) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.broker_id = broker_id;
        rko->rko_u.mock.cmd       = RD_KAFKA_MOCK_CMD_BROKER_DECOMMISSION;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}

rd_kafka_resp_err_t rd_kafka_mock_broker_add(rd_kafka_mock_cluster_t *mcluster,
                                             int32_t broker_id) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.broker_id = broker_id;
        rko->rko_u.mock.cmd       = RD_KAFKA_MOCK_CMD_BROKER_ADD;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}


/**
 * @brief Starts listening on the mock broker socket.
 *
 * @returns 0 on success or -1 on error (logged).
 */
static int rd_kafka_mock_broker_start_listener(rd_kafka_mock_broker_t *mrkb) {
        rd_assert(mrkb->listen_s != -1);

        if (listen(mrkb->listen_s, 5) == RD_SOCKET_ERROR) {
                rd_kafka_log(mrkb->cluster->rk, LOG_CRIT, "MOCK",
                             "Failed to listen on mock broker socket: %s",
                             rd_socket_strerror(rd_socket_errno));
                return -1;
        }

        rd_kafka_mock_cluster_io_add(mrkb->cluster, mrkb->listen_s, POLLIN,
                                     rd_kafka_mock_broker_listen_io, mrkb);

        return 0;
}


/**
 * @brief Creates a new listener socket for \p mrkb but does NOT starts
 *        listening.
 *
 * @param sin is the address and port to bind. If the port is zero a random
 *            port will be assigned (by the kernel) and the address and port
 *            will be returned in this pointer.
 *
 * @returns listener socket on success or -1 on error (errors are logged).
 */
static int rd_kafka_mock_broker_new_listener(rd_kafka_mock_cluster_t *mcluster,
                                             struct sockaddr_in *sinp) {
        struct sockaddr_in sin = *sinp;
        socklen_t sin_len      = sizeof(sin);
        int listen_s;
        int on = 1;

        if (!sin.sin_family)
                sin.sin_family = AF_INET;

        /*
         * Create and bind socket to any loopback port
         */
        listen_s =
            rd_kafka_socket_cb_linux(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL);
        if (listen_s == RD_SOCKET_ERROR) {
                rd_kafka_log(mcluster->rk, LOG_CRIT, "MOCK",
                             "Unable to create mock broker listen socket: %s",
                             rd_socket_strerror(rd_socket_errno));
                return -1;
        }

        if (setsockopt(listen_s, SOL_SOCKET, SO_REUSEADDR, (void *)&on,
                       sizeof(on)) == -1) {
                rd_kafka_log(mcluster->rk, LOG_CRIT, "MOCK",
                             "Failed to set SO_REUSEADDR on mock broker "
                             "listen socket: %s",
                             rd_socket_strerror(rd_socket_errno));
                rd_socket_close(listen_s);
                return -1;
        }

        if (bind(listen_s, (struct sockaddr *)&sin, sizeof(sin)) ==
            RD_SOCKET_ERROR) {
                rd_kafka_log(mcluster->rk, LOG_CRIT, "MOCK",
                             "Failed to bind mock broker socket to %s: %s",
                             rd_socket_strerror(rd_socket_errno),
                             rd_sockaddr2str(&sin, RD_SOCKADDR2STR_F_PORT));
                rd_socket_close(listen_s);
                return -1;
        }

        if (getsockname(listen_s, (struct sockaddr *)&sin, &sin_len) ==
            RD_SOCKET_ERROR) {
                rd_kafka_log(mcluster->rk, LOG_CRIT, "MOCK",
                             "Failed to get mock broker socket name: %s",
                             rd_socket_strerror(rd_socket_errno));
                rd_socket_close(listen_s);
                return -1;
        }
        rd_assert(sin.sin_family == AF_INET);
        /* If a filled in sinp was passed make sure nothing changed. */
        rd_assert(!sinp->sin_port || !memcmp(sinp, &sin, sizeof(sin)));

        *sinp = sin;

        return listen_s;
}


static rd_kafka_mock_broker_t *
rd_kafka_mock_broker_new(rd_kafka_mock_cluster_t *mcluster,
                         int32_t broker_id,
                         rd_kafka_resp_err_t *err) {
        rd_kafka_mock_broker_t *mrkb;
        rd_socket_t listen_s;
        struct sockaddr_in sin = {
            .sin_family = AF_INET,
            .sin_addr   = {.s_addr = htonl(INADDR_LOOPBACK)}};

        if (rd_kafka_mock_broker_find(mcluster, broker_id)) {
                if (err)
                        *err = RD_KAFKA_RESP_ERR__INVALID_ARG;
                /* A broker with this id already exists. */
                return NULL;
        }

        listen_s = rd_kafka_mock_broker_new_listener(mcluster, &sin);
        if (listen_s == -1) {
                if (err)
                        *err = RD_KAFKA_RESP_ERR__TRANSPORT;
                return NULL;
        }

        /*
         * Create mock broker object
         */
        mrkb = rd_calloc(1, sizeof(*mrkb));

        mrkb->id       = broker_id;
        mrkb->cluster  = mcluster;
        mrkb->up       = rd_true;
        mrkb->listen_s = listen_s;
        mrkb->sin      = sin;
        mrkb->port     = ntohs(sin.sin_port);
        rd_snprintf(mrkb->advertised_listener,
                    sizeof(mrkb->advertised_listener), "%s",
                    rd_sockaddr2str(&sin, 0));

        TAILQ_INIT(&mrkb->connections);
        TAILQ_INIT(&mrkb->errstacks);

        TAILQ_INSERT_TAIL(&mcluster->brokers, mrkb, link);
        mcluster->broker_cnt++;

        if (rd_kafka_mock_broker_start_listener(mrkb) == -1) {
                rd_kafka_mock_broker_destroy(mrkb);
                if (err)
                        *err = RD_KAFKA_RESP_ERR__TRANSPORT;
                return NULL;
        }

        return mrkb;
}


/**
 * @returns the coordtype_t for a coord type string, or -1 on error.
 */
static rd_kafka_coordtype_t rd_kafka_mock_coord_str2type(const char *str) {
        if (!strcmp(str, "transaction"))
                return RD_KAFKA_COORD_TXN;
        else if (!strcmp(str, "group"))
                return RD_KAFKA_COORD_GROUP;
        else
                return (rd_kafka_coordtype_t)-1;
}


/**
 * @brief Unlink and destroy coordinator.
 */
static void rd_kafka_mock_coord_destroy(rd_kafka_mock_cluster_t *mcluster,
                                        rd_kafka_mock_coord_t *mcoord) {
        TAILQ_REMOVE(&mcluster->coords, mcoord, link);
        rd_free(mcoord->key);
        rd_free(mcoord);
}

/**
 * @brief Find coordinator by type and key.
 */
static rd_kafka_mock_coord_t *
rd_kafka_mock_coord_find(rd_kafka_mock_cluster_t *mcluster,
                         rd_kafka_coordtype_t type,
                         const char *key) {
        rd_kafka_mock_coord_t *mcoord;

        TAILQ_FOREACH(mcoord, &mcluster->coords, link) {
                if (mcoord->type == type && !strcmp(mcoord->key, key))
                        return mcoord;
        }

        return NULL;
}


/**
 * @returns the coordinator for KeyType,Key (e.g., GROUP,mygroup).
 */
rd_kafka_mock_broker_t *
rd_kafka_mock_cluster_get_coord(rd_kafka_mock_cluster_t *mcluster,
                                rd_kafka_coordtype_t KeyType,
                                const rd_kafkap_str_t *Key) {
        rd_kafka_mock_broker_t *mrkb;
        rd_kafka_mock_coord_t *mcoord;
        char *key;
        rd_crc32_t hash;
        int idx;

        /* Try the explicit coord list first */
        RD_KAFKAP_STR_DUPA(&key, Key);
        if ((mcoord = rd_kafka_mock_coord_find(mcluster, KeyType, key)))
                return rd_kafka_mock_broker_find(mcluster, mcoord->broker_id);

        /* Else hash the key to select an available broker. */
        hash = rd_crc32(Key->str, RD_KAFKAP_STR_LEN(Key));
        idx  = (int)(hash % mcluster->broker_cnt);

        /* Use the broker index in the list */
        TAILQ_FOREACH(mrkb, &mcluster->brokers, link)
        if (idx-- == 0)
                return mrkb;

        RD_NOTREACHED();
        return NULL;
}


/**
 * @brief Explicitly set coordinator for \p key_type ("transaction", "group")
 *        and \p key.
 */
static rd_kafka_mock_coord_t *
rd_kafka_mock_coord_set(rd_kafka_mock_cluster_t *mcluster,
                        const char *key_type,
                        const char *key,
                        int32_t broker_id) {
        rd_kafka_mock_coord_t *mcoord;
        rd_kafka_coordtype_t type;

        if ((int)(type = rd_kafka_mock_coord_str2type(key_type)) == -1)
                return NULL;

        if ((mcoord = rd_kafka_mock_coord_find(mcluster, type, key)))
                rd_kafka_mock_coord_destroy(mcluster, mcoord);

        mcoord            = rd_calloc(1, sizeof(*mcoord));
        mcoord->type      = type;
        mcoord->key       = rd_strdup(key);
        mcoord->broker_id = broker_id;

        TAILQ_INSERT_TAIL(&mcluster->coords, mcoord, link);

        return mcoord;
}

/**
 * @brief Remove coordinator by broker id.
 */
void rd_kafka_mock_coord_remove(rd_kafka_mock_cluster_t *mcluster,
                                int32_t broker_id) {
        rd_kafka_mock_coord_t *mcoord, *tmp;

        TAILQ_FOREACH_SAFE(mcoord, &mcluster->coords, link, tmp) {
                if (mcoord->broker_id == broker_id) {
                        rd_kafka_mock_coord_destroy(mcluster, mcoord);
                }
        }
}


/**
 * @brief Remove and return the next error, or RD_KAFKA_RESP_ERR_NO_ERROR
 *        if no error.
 */
static rd_kafka_mock_error_rtt_t
rd_kafka_mock_error_stack_next(rd_kafka_mock_error_stack_t *errstack) {
        rd_kafka_mock_error_rtt_t err_rtt = {RD_KAFKA_RESP_ERR_NO_ERROR, 0};

        if (likely(errstack->cnt == 0))
                return err_rtt;

        err_rtt = errstack->errs[0];
        errstack->cnt--;
        if (errstack->cnt > 0)
                memmove(errstack->errs, &errstack->errs[1],
                        sizeof(*errstack->errs) * errstack->cnt);

        return err_rtt;
}


/**
 * @brief Find an error stack based on \p ApiKey
 */
static rd_kafka_mock_error_stack_t *
rd_kafka_mock_error_stack_find(const rd_kafka_mock_error_stack_head_t *shead,
                               int16_t ApiKey) {
        const rd_kafka_mock_error_stack_t *errstack;

        TAILQ_FOREACH(errstack, shead, link)
        if (errstack->ApiKey == ApiKey)
                return (rd_kafka_mock_error_stack_t *)errstack;

        return NULL;
}



/**
 * @brief Find or create an error stack based on \p ApiKey
 */
static rd_kafka_mock_error_stack_t *
rd_kafka_mock_error_stack_get(rd_kafka_mock_error_stack_head_t *shead,
                              int16_t ApiKey) {
        rd_kafka_mock_error_stack_t *errstack;

        if ((errstack = rd_kafka_mock_error_stack_find(shead, ApiKey)))
                return errstack;

        errstack = rd_calloc(1, sizeof(*errstack));

        errstack->ApiKey = ApiKey;
        TAILQ_INSERT_TAIL(shead, errstack, link);

        return errstack;
}



/**
 * @brief Removes and returns the next request error for response's ApiKey.
 *
 * If the error stack has a corresponding rtt/delay it is set on the
 * provided response \p resp buffer.
 */
rd_kafka_resp_err_t
rd_kafka_mock_next_request_error(rd_kafka_mock_connection_t *mconn,
                                 rd_kafka_buf_t *resp) {
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        rd_kafka_mock_error_stack_t *errstack;
        rd_kafka_mock_error_rtt_t err_rtt;

        mtx_lock(&mcluster->lock);

        errstack = rd_kafka_mock_error_stack_find(&mconn->broker->errstacks,
                                                  resp->rkbuf_reqhdr.ApiKey);
        if (likely(!errstack)) {
                errstack = rd_kafka_mock_error_stack_find(
                    &mcluster->errstacks, resp->rkbuf_reqhdr.ApiKey);
                if (likely(!errstack)) {
                        mtx_unlock(&mcluster->lock);
                        return RD_KAFKA_RESP_ERR_NO_ERROR;
                }
        }

        err_rtt             = rd_kafka_mock_error_stack_next(errstack);
        resp->rkbuf_ts_sent = err_rtt.rtt;

        mtx_unlock(&mcluster->lock);

        /* If the error is ERR__TRANSPORT (a librdkafka-specific error code
         * that will never be returned by a broker), we close the connection.
         * This allows closing the connection as soon as a certain
         * request is seen.
         * The handler code in rdkafka_mock_handlers.c does not need to
         * handle this case specifically and will generate a response and
         * enqueue it, but the connection will be down by the time it will
         * be sent.
         * Note: Delayed disconnects (rtt-based) are not supported. */
        if (err_rtt.err == RD_KAFKA_RESP_ERR__TRANSPORT) {
                rd_kafka_dbg(
                    mcluster->rk, MOCK, "MOCK",
                    "Broker %" PRId32
                    ": Forcing close of connection "
                    "from %s",
                    mconn->broker->id,
                    rd_sockaddr2str(&mconn->peer, RD_SOCKADDR2STR_F_PORT));
                rd_kafka_transport_shutdown(mconn->transport);
        }


        return err_rtt.err;
}


void rd_kafka_mock_clear_request_errors(rd_kafka_mock_cluster_t *mcluster,
                                        int16_t ApiKey) {
        rd_kafka_mock_error_stack_t *errstack;

        mtx_lock(&mcluster->lock);

        errstack = rd_kafka_mock_error_stack_find(&mcluster->errstacks, ApiKey);
        if (errstack)
                errstack->cnt = 0;

        mtx_unlock(&mcluster->lock);
}


void rd_kafka_mock_push_request_errors_array(
    rd_kafka_mock_cluster_t *mcluster,
    int16_t ApiKey,
    size_t cnt,
    const rd_kafka_resp_err_t *errors) {
        rd_kafka_mock_error_stack_t *errstack;
        size_t totcnt;
        size_t i;

        mtx_lock(&mcluster->lock);

        errstack = rd_kafka_mock_error_stack_get(&mcluster->errstacks, ApiKey);

        totcnt = errstack->cnt + cnt;

        if (totcnt > errstack->size) {
                errstack->size = totcnt + 4;
                errstack->errs = rd_realloc(
                    errstack->errs, errstack->size * sizeof(*errstack->errs));
        }

        for (i = 0; i < cnt; i++) {
                errstack->errs[errstack->cnt].err   = errors[i];
                errstack->errs[errstack->cnt++].rtt = 0;
        }

        mtx_unlock(&mcluster->lock);
}

void rd_kafka_mock_push_request_errors(rd_kafka_mock_cluster_t *mcluster,
                                       int16_t ApiKey,
                                       size_t cnt,
                                       ...) {
        va_list ap;
        rd_kafka_resp_err_t *errors = rd_alloca(sizeof(*errors) * cnt);
        size_t i;

        va_start(ap, cnt);
        for (i = 0; i < cnt; i++)
                errors[i] = va_arg(ap, rd_kafka_resp_err_t);
        va_end(ap);

        rd_kafka_mock_push_request_errors_array(mcluster, ApiKey, cnt, errors);
}


rd_kafka_resp_err_t
rd_kafka_mock_broker_push_request_error_rtts(rd_kafka_mock_cluster_t *mcluster,
                                             int32_t broker_id,
                                             int16_t ApiKey,
                                             size_t cnt,
                                             ...) {
        rd_kafka_mock_broker_t *mrkb;
        va_list ap;
        rd_kafka_mock_error_stack_t *errstack;
        size_t totcnt;

        mtx_lock(&mcluster->lock);

        if (!(mrkb = rd_kafka_mock_broker_find(mcluster, broker_id))) {
                mtx_unlock(&mcluster->lock);
                return RD_KAFKA_RESP_ERR__UNKNOWN_BROKER;
        }

        errstack = rd_kafka_mock_error_stack_get(&mrkb->errstacks, ApiKey);

        totcnt = errstack->cnt + cnt;

        if (totcnt > errstack->size) {
                errstack->size = totcnt + 4;
                errstack->errs = rd_realloc(
                    errstack->errs, errstack->size * sizeof(*errstack->errs));
        }

        va_start(ap, cnt);
        while (cnt-- > 0) {
                errstack->errs[errstack->cnt].err =
                    va_arg(ap, rd_kafka_resp_err_t);
                errstack->errs[errstack->cnt++].rtt =
                    ((rd_ts_t)va_arg(ap, int)) * 1000;
        }
        va_end(ap);

        mtx_unlock(&mcluster->lock);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


rd_kafka_resp_err_t
rd_kafka_mock_broker_error_stack_cnt(rd_kafka_mock_cluster_t *mcluster,
                                     int32_t broker_id,
                                     int16_t ApiKey,
                                     size_t *cntp) {
        rd_kafka_mock_broker_t *mrkb;
        rd_kafka_mock_error_stack_t *errstack;

        if (!mcluster || !cntp)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        mtx_lock(&mcluster->lock);

        if (!(mrkb = rd_kafka_mock_broker_find(mcluster, broker_id))) {
                mtx_unlock(&mcluster->lock);
                return RD_KAFKA_RESP_ERR__UNKNOWN_BROKER;
        }

        if ((errstack =
                 rd_kafka_mock_error_stack_find(&mrkb->errstacks, ApiKey)))
                *cntp = errstack->cnt;

        mtx_unlock(&mcluster->lock);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


void rd_kafka_mock_topic_set_error(rd_kafka_mock_cluster_t *mcluster,
                                   const char *topic,
                                   rd_kafka_resp_err_t err) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.name = rd_strdup(topic);
        rko->rko_u.mock.cmd  = RD_KAFKA_MOCK_CMD_TOPIC_SET_ERROR;
        rko->rko_u.mock.err  = err;

        rko = rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE);
        if (rko)
                rd_kafka_op_destroy(rko);
}


rd_kafka_resp_err_t
rd_kafka_mock_topic_create(rd_kafka_mock_cluster_t *mcluster,
                           const char *topic,
                           int partition_cnt,
                           int replication_factor) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.name = rd_strdup(topic);
        rko->rko_u.mock.lo   = partition_cnt;
        rko->rko_u.mock.hi   = replication_factor;
        rko->rko_u.mock.cmd  = RD_KAFKA_MOCK_CMD_TOPIC_CREATE;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}

rd_kafka_resp_err_t
rd_kafka_mock_partition_set_leader(rd_kafka_mock_cluster_t *mcluster,
                                   const char *topic,
                                   int32_t partition,
                                   int32_t broker_id) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.name      = rd_strdup(topic);
        rko->rko_u.mock.cmd       = RD_KAFKA_MOCK_CMD_PART_SET_LEADER;
        rko->rko_u.mock.partition = partition;
        rko->rko_u.mock.broker_id = broker_id;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}

rd_kafka_resp_err_t
rd_kafka_mock_partition_set_follower(rd_kafka_mock_cluster_t *mcluster,
                                     const char *topic,
                                     int32_t partition,
                                     int32_t broker_id) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.name      = rd_strdup(topic);
        rko->rko_u.mock.cmd       = RD_KAFKA_MOCK_CMD_PART_SET_FOLLOWER;
        rko->rko_u.mock.partition = partition;
        rko->rko_u.mock.broker_id = broker_id;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}

rd_kafka_resp_err_t
rd_kafka_mock_partition_set_follower_wmarks(rd_kafka_mock_cluster_t *mcluster,
                                            const char *topic,
                                            int32_t partition,
                                            int64_t lo,
                                            int64_t hi) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.name      = rd_strdup(topic);
        rko->rko_u.mock.cmd       = RD_KAFKA_MOCK_CMD_PART_SET_FOLLOWER_WMARKS;
        rko->rko_u.mock.partition = partition;
        rko->rko_u.mock.lo        = lo;
        rko->rko_u.mock.hi        = hi;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}

rd_kafka_resp_err_t
rd_kafka_mock_partition_push_leader_response(rd_kafka_mock_cluster_t *mcluster,
                                             const char *topic,
                                             int partition,
                                             int32_t leader_id,
                                             int32_t leader_epoch) {
        rd_kafka_op_t *rko        = rd_kafka_op_new(RD_KAFKA_OP_MOCK);
        rko->rko_u.mock.name      = rd_strdup(topic);
        rko->rko_u.mock.cmd       = RD_KAFKA_MOCK_CMD_PART_PUSH_LEADER_RESPONSE;
        rko->rko_u.mock.partition = partition;
        rko->rko_u.mock.leader_id = leader_id;
        rko->rko_u.mock.leader_epoch = leader_epoch;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}

rd_kafka_resp_err_t
rd_kafka_mock_broker_set_down(rd_kafka_mock_cluster_t *mcluster,
                              int32_t broker_id) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.broker_id = broker_id;
        rko->rko_u.mock.lo        = rd_false;
        rko->rko_u.mock.cmd       = RD_KAFKA_MOCK_CMD_BROKER_SET_UPDOWN;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}

rd_kafka_resp_err_t
rd_kafka_mock_broker_set_up(rd_kafka_mock_cluster_t *mcluster,
                            int32_t broker_id) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.broker_id = broker_id;
        rko->rko_u.mock.lo        = rd_true;
        rko->rko_u.mock.cmd       = RD_KAFKA_MOCK_CMD_BROKER_SET_UPDOWN;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}

rd_kafka_resp_err_t
rd_kafka_mock_broker_set_rtt(rd_kafka_mock_cluster_t *mcluster,
                             int32_t broker_id,
                             int rtt_ms) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.broker_id = broker_id;
        rko->rko_u.mock.lo        = rtt_ms;
        rko->rko_u.mock.cmd       = RD_KAFKA_MOCK_CMD_BROKER_SET_RTT;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}

rd_kafka_resp_err_t
rd_kafka_mock_broker_set_rack(rd_kafka_mock_cluster_t *mcluster,
                              int32_t broker_id,
                              const char *rack) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.broker_id = broker_id;
        rko->rko_u.mock.name      = rd_strdup(rack);
        rko->rko_u.mock.cmd       = RD_KAFKA_MOCK_CMD_BROKER_SET_RACK;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}

void rd_kafka_mock_broker_set_host_port(rd_kafka_mock_cluster_t *cluster,
                                        int32_t broker_id,
                                        const char *host,
                                        int port) {
        rd_kafka_mock_broker_t *mrkb;

        mtx_lock(&cluster->lock);
        TAILQ_FOREACH(mrkb, &cluster->brokers, link) {
                if (mrkb->id == broker_id) {
                        rd_kafka_dbg(
                            cluster->rk, MOCK, "MOCK",
                            "Broker %" PRId32
                            ": Setting advertised listener from %s:%d to %s:%d",
                            broker_id, mrkb->advertised_listener, mrkb->port,
                            host, port);
                        rd_snprintf(mrkb->advertised_listener,
                                    sizeof(mrkb->advertised_listener), "%s",
                                    host);
                        mrkb->port = port;
                        break;
                }
        }
        mtx_unlock(&cluster->lock);
}

rd_kafka_resp_err_t
rd_kafka_mock_coordinator_set(rd_kafka_mock_cluster_t *mcluster,
                              const char *key_type,
                              const char *key,
                              int32_t broker_id) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.name      = rd_strdup(key_type);
        rko->rko_u.mock.str       = rd_strdup(key);
        rko->rko_u.mock.broker_id = broker_id;
        rko->rko_u.mock.cmd       = RD_KAFKA_MOCK_CMD_COORD_SET;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}

rd_kafka_resp_err_t
rd_kafka_mock_set_apiversion(rd_kafka_mock_cluster_t *mcluster,
                             int16_t ApiKey,
                             int16_t MinVersion,
                             int16_t MaxVersion) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.partition = ApiKey;
        rko->rko_u.mock.lo        = MinVersion;
        rko->rko_u.mock.hi        = MaxVersion;
        rko->rko_u.mock.cmd       = RD_KAFKA_MOCK_CMD_APIVERSION_SET;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}

rd_kafka_resp_err_t
rd_kafka_mock_telemetry_set_requested_metrics(rd_kafka_mock_cluster_t *mcluster,
                                              char **metrics,
                                              size_t metrics_cnt) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.hi      = metrics_cnt;
        rko->rko_u.mock.metrics = NULL;
        if (metrics_cnt) {
                size_t i;
                rko->rko_u.mock.metrics =
                    rd_calloc(metrics_cnt, sizeof(char *));
                for (i = 0; i < metrics_cnt; i++)
                        rko->rko_u.mock.metrics[i] = rd_strdup(metrics[i]);
        }
        rko->rko_u.mock.cmd = RD_KAFKA_MOCK_CMD_REQUESTED_METRICS_SET;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}

rd_kafka_resp_err_t
rd_kafka_mock_telemetry_set_push_interval(rd_kafka_mock_cluster_t *mcluster,
                                          int64_t push_interval_ms) {
        rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_MOCK);

        rko->rko_u.mock.hi  = push_interval_ms;
        rko->rko_u.mock.cmd = RD_KAFKA_MOCK_CMD_TELEMETRY_PUSH_INTERVAL_SET;

        return rd_kafka_op_err_destroy(
            rd_kafka_op_req(mcluster->ops, rko, RD_POLL_INFINITE));
}


/**
 * @brief Apply command to specific broker.
 *
 * @locality mcluster thread
 */
static rd_kafka_resp_err_t
rd_kafka_mock_broker_cmd(rd_kafka_mock_cluster_t *mcluster,
                         rd_kafka_mock_broker_t *mrkb,
                         rd_kafka_op_t *rko) {
        switch (rko->rko_u.mock.cmd) {
        case RD_KAFKA_MOCK_CMD_BROKER_SET_UPDOWN:
                if ((rd_bool_t)rko->rko_u.mock.lo == mrkb->up)
                        break;

                mrkb->up = (rd_bool_t)rko->rko_u.mock.lo;

                if (!mrkb->up) {
                        rd_kafka_mock_cluster_io_del(mcluster, mrkb->listen_s);
                        rd_socket_close(mrkb->listen_s);
                        /* Re-create the listener right away so we retain the
                         * same port. The listener is not started until
                         * the broker is set up (below). */
                        mrkb->listen_s = rd_kafka_mock_broker_new_listener(
                            mcluster, &mrkb->sin);
                        rd_assert(mrkb->listen_s != -1 ||
                                  !*"Failed to-create mock broker listener");

                        rd_kafka_mock_broker_close_all(mrkb, "Broker down");

                } else {
                        int r;
                        rd_assert(mrkb->listen_s != -1);
                        r = rd_kafka_mock_broker_start_listener(mrkb);
                        rd_assert(r == 0 || !*"broker_start_listener() failed");
                }
                break;

        case RD_KAFKA_MOCK_CMD_BROKER_SET_RTT:
                mrkb->rtt = (rd_ts_t)rko->rko_u.mock.lo * 1000;

                /* Check if there is anything to send now that the RTT
                 * has changed or if a timer is to be started. */
                rd_kafka_mock_broker_connections_write_out(mrkb);
                break;

        case RD_KAFKA_MOCK_CMD_BROKER_SET_RACK:
                if (mrkb->rack)
                        rd_free(mrkb->rack);

                if (rko->rko_u.mock.name)
                        mrkb->rack = rd_strdup(rko->rko_u.mock.name);
                else
                        mrkb->rack = NULL;
                break;

        case RD_KAFKA_MOCK_CMD_BROKER_DECOMMISSION:
                rd_kafka_mock_broker_destroy(mrkb);
                rd_kafka_mock_cluster_reassign_partitions(mcluster);
                break;

        default:
                RD_BUG("Unhandled mock cmd %d", rko->rko_u.mock.cmd);
                break;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Apply command to to one or all brokers, depending on the value of
 *        broker_id, where -1 means all, and != -1 means a specific broker.
 *
 * @locality mcluster thread
 */
static rd_kafka_resp_err_t
rd_kafka_mock_brokers_cmd(rd_kafka_mock_cluster_t *mcluster,
                          rd_kafka_op_t *rko) {
        rd_kafka_mock_broker_t *mrkb;

        if (rko->rko_u.mock.broker_id != -1) {
                /* Specific broker */
                mrkb = rd_kafka_mock_broker_find(mcluster,
                                                 rko->rko_u.mock.broker_id);
                if (!mrkb)
                        return RD_KAFKA_RESP_ERR_BROKER_NOT_AVAILABLE;

                return rd_kafka_mock_broker_cmd(mcluster, mrkb, rko);
        }

        /* All brokers */
        TAILQ_FOREACH(mrkb, &mcluster->brokers, link) {
                rd_kafka_resp_err_t err;

                if ((err = rd_kafka_mock_broker_cmd(mcluster, mrkb, rko)))
                        return err;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Handle command op
 *
 * @locality mcluster thread
 */
static rd_kafka_resp_err_t
rd_kafka_mock_cluster_cmd(rd_kafka_mock_cluster_t *mcluster,
                          rd_kafka_op_t *rko) {
        rd_kafka_mock_topic_t *mtopic;
        rd_kafka_mock_partition_t *mpart;
        rd_kafka_mock_broker_t *mrkb;
        size_t i;
        rd_kafka_resp_err_t err;

        switch (rko->rko_u.mock.cmd) {
        case RD_KAFKA_MOCK_CMD_TOPIC_CREATE:
                if (rd_kafka_mock_topic_find(mcluster, rko->rko_u.mock.name))
                        return RD_KAFKA_RESP_ERR_TOPIC_ALREADY_EXISTS;

                if (!rd_kafka_mock_topic_new(mcluster, rko->rko_u.mock.name,
                                             /* partition_cnt */
                                             (int)rko->rko_u.mock.lo,
                                             /* replication_factor */
                                             (int)rko->rko_u.mock.hi))
                        return RD_KAFKA_RESP_ERR_TOPIC_EXCEPTION;
                break;

        case RD_KAFKA_MOCK_CMD_TOPIC_SET_ERROR:
                mtopic =
                    rd_kafka_mock_topic_get(mcluster, rko->rko_u.mock.name, -1);
                mtopic->err = rko->rko_u.mock.err;
                break;

        case RD_KAFKA_MOCK_CMD_PART_SET_LEADER:
                mpart = rd_kafka_mock_partition_get(
                    mcluster, rko->rko_u.mock.name, rko->rko_u.mock.partition);
                if (!mpart)
                        return RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;

                if (rko->rko_u.mock.broker_id != -1) {
                        mrkb = rd_kafka_mock_broker_find(
                            mcluster, rko->rko_u.mock.broker_id);
                        if (!mrkb)
                                return RD_KAFKA_RESP_ERR_BROKER_NOT_AVAILABLE;
                } else {
                        mrkb = NULL;
                }

                rd_kafka_dbg(mcluster->rk, MOCK, "MOCK",
                             "Set %s [%" PRId32 "] leader to %" PRId32,
                             rko->rko_u.mock.name, rko->rko_u.mock.partition,
                             rko->rko_u.mock.broker_id);

                rd_kafka_mock_partition_set_leader0(mpart, mrkb);
                break;

        case RD_KAFKA_MOCK_CMD_PART_SET_FOLLOWER:
                mpart = rd_kafka_mock_partition_get(
                    mcluster, rko->rko_u.mock.name, rko->rko_u.mock.partition);
                if (!mpart)
                        return RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;

                rd_kafka_dbg(mcluster->rk, MOCK, "MOCK",
                             "Set %s [%" PRId32
                             "] preferred follower "
                             "to %" PRId32,
                             rko->rko_u.mock.name, rko->rko_u.mock.partition,
                             rko->rko_u.mock.broker_id);

                mpart->follower_id = rko->rko_u.mock.broker_id;
                break;

        case RD_KAFKA_MOCK_CMD_PART_SET_FOLLOWER_WMARKS:
                mpart = rd_kafka_mock_partition_get(
                    mcluster, rko->rko_u.mock.name, rko->rko_u.mock.partition);
                if (!mpart)
                        return RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;

                rd_kafka_dbg(mcluster->rk, MOCK, "MOCK",
                             "Set %s [%" PRId32
                             "] follower "
                             "watermark offsets to %" PRId64 "..%" PRId64,
                             rko->rko_u.mock.name, rko->rko_u.mock.partition,
                             rko->rko_u.mock.lo, rko->rko_u.mock.hi);

                if (rko->rko_u.mock.lo == -1) {
                        mpart->follower_start_offset = mpart->start_offset;
                        mpart->update_follower_start_offset = rd_true;
                } else {
                        mpart->follower_start_offset = rko->rko_u.mock.lo;
                        mpart->update_follower_start_offset = rd_false;
                }

                if (rko->rko_u.mock.hi == -1) {
                        mpart->follower_end_offset        = mpart->end_offset;
                        mpart->update_follower_end_offset = rd_true;
                } else {
                        mpart->follower_end_offset        = rko->rko_u.mock.hi;
                        mpart->update_follower_end_offset = rd_false;
                }
                break;
        case RD_KAFKA_MOCK_CMD_PART_PUSH_LEADER_RESPONSE:
                mpart = rd_kafka_mock_partition_get(
                    mcluster, rko->rko_u.mock.name, rko->rko_u.mock.partition);
                if (!mpart)
                        return RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;

                rd_kafka_dbg(mcluster->rk, MOCK, "MOCK",
                             "Push %s [%" PRId32 "] leader response: (%" PRId32
                             ", %" PRId32 ")",
                             rko->rko_u.mock.name, rko->rko_u.mock.partition,
                             rko->rko_u.mock.leader_id,
                             rko->rko_u.mock.leader_epoch);

                rd_kafka_mock_partition_push_leader_response0(
                    mpart, rko->rko_u.mock.leader_id,
                    rko->rko_u.mock.leader_epoch);
                break;

                /* Broker commands */
        case RD_KAFKA_MOCK_CMD_BROKER_SET_UPDOWN:
        case RD_KAFKA_MOCK_CMD_BROKER_SET_RTT:
        case RD_KAFKA_MOCK_CMD_BROKER_SET_RACK:
        case RD_KAFKA_MOCK_CMD_BROKER_DECOMMISSION:
                return rd_kafka_mock_brokers_cmd(mcluster, rko);

        case RD_KAFKA_MOCK_CMD_BROKER_ADD:
                if (!rd_kafka_mock_broker_new(mcluster,
                                              rko->rko_u.mock.broker_id, &err))
                        return err;

                rd_kafka_mock_cluster_reassign_partitions(mcluster);
                break;
        case RD_KAFKA_MOCK_CMD_COORD_SET:
                if (!rd_kafka_mock_coord_set(mcluster, rko->rko_u.mock.name,
                                             rko->rko_u.mock.str,
                                             rko->rko_u.mock.broker_id))
                        return RD_KAFKA_RESP_ERR__INVALID_ARG;
                break;

        case RD_KAFKA_MOCK_CMD_APIVERSION_SET:
                if (rko->rko_u.mock.partition < 0 ||
                    rko->rko_u.mock.partition >= RD_KAFKAP__NUM)
                        return RD_KAFKA_RESP_ERR__INVALID_ARG;

                mcluster->api_handlers[(int)rko->rko_u.mock.partition]
                    .MinVersion = (int16_t)rko->rko_u.mock.lo;
                mcluster->api_handlers[(int)rko->rko_u.mock.partition]
                    .MaxVersion = (int16_t)rko->rko_u.mock.hi;
                break;

        case RD_KAFKA_MOCK_CMD_REQUESTED_METRICS_SET:
                mcluster->metrics_cnt = rko->rko_u.mock.hi;
                if (!mcluster->metrics_cnt)
                        break;

                mcluster->metrics =
                    rd_calloc(mcluster->metrics_cnt, sizeof(char *));
                for (i = 0; i < mcluster->metrics_cnt; i++)
                        mcluster->metrics[i] =
                            rd_strdup(rko->rko_u.mock.metrics[i]);
                break;

        case RD_KAFKA_MOCK_CMD_TELEMETRY_PUSH_INTERVAL_SET:
                mcluster->telemetry_push_interval_ms = rko->rko_u.mock.hi;
                break;

        default:
                rd_assert(!*"unknown mock cmd");
                break;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

void rd_kafka_mock_group_initial_rebalance_delay_ms(
    rd_kafka_mock_cluster_t *mcluster,
    int32_t delay_ms) {
        mtx_lock(&mcluster->lock);
        mcluster->defaults.group_initial_rebalance_delay_ms = delay_ms;
        mtx_unlock(&mcluster->lock);
}


static rd_kafka_op_res_t
rd_kafka_mock_cluster_op_serve(rd_kafka_t *rk,
                               rd_kafka_q_t *rkq,
                               rd_kafka_op_t *rko,
                               rd_kafka_q_cb_type_t cb_type,
                               void *opaque) {
        rd_kafka_mock_cluster_t *mcluster = opaque;
        rd_kafka_resp_err_t err           = RD_KAFKA_RESP_ERR_NO_ERROR;

        switch ((int)rko->rko_type) {
        case RD_KAFKA_OP_TERMINATE:
                mcluster->run = rd_false;
                break;

        case RD_KAFKA_OP_MOCK:
                err = rd_kafka_mock_cluster_cmd(mcluster, rko);
                break;

        default:
                rd_assert(!"*unhandled op");
                break;
        }

        rd_kafka_op_reply(rko, err);

        return RD_KAFKA_OP_RES_HANDLED;
}


/**
 * @brief Destroy cluster (internal)
 */
static void rd_kafka_mock_cluster_destroy0(rd_kafka_mock_cluster_t *mcluster) {
        rd_kafka_mock_topic_t *mtopic;
        rd_kafka_mock_broker_t *mrkb;
        rd_kafka_mock_cgrp_classic_t *mcgrp_classic;
        rd_kafka_mock_cgrp_consumer_t *mcgrp_consumer;
        rd_kafka_mock_coord_t *mcoord;
        rd_kafka_mock_error_stack_t *errstack;
        thrd_t dummy_rkb_thread;
        int ret;
        size_t i;

        while ((mtopic = TAILQ_FIRST(&mcluster->topics)))
                rd_kafka_mock_topic_destroy(mtopic);

        while ((mrkb = TAILQ_FIRST(&mcluster->brokers)))
                rd_kafka_mock_broker_destroy(mrkb);

        while ((mcgrp_classic = TAILQ_FIRST(&mcluster->cgrps_classic)))
                rd_kafka_mock_cgrp_classic_destroy(mcgrp_classic);

        while ((mcgrp_consumer = TAILQ_FIRST(&mcluster->cgrps_consumer)))
                rd_kafka_mock_cgrp_consumer_destroy(mcgrp_consumer);

        while ((mcoord = TAILQ_FIRST(&mcluster->coords)))
                rd_kafka_mock_coord_destroy(mcluster, mcoord);

        rd_list_destroy(&mcluster->pids);

        while ((errstack = TAILQ_FIRST(&mcluster->errstacks))) {
                TAILQ_REMOVE(&mcluster->errstacks, errstack, link);
                rd_kafka_mock_error_stack_destroy(errstack);
        }

        rd_list_destroy(&mcluster->request_list);

        dummy_rkb_thread = mcluster->dummy_rkb->rkb_thread;

        /*
         * Destroy dummy broker.
         * WARNING: This is last time we can read
         * from dummy_rkb in this thread!
         */
        rd_kafka_q_enq(mcluster->dummy_rkb->rkb_ops,
                       rd_kafka_op_new(RD_KAFKA_OP_TERMINATE));

        if (thrd_join(dummy_rkb_thread, &ret) != thrd_success)
                rd_assert(!*"failed to join mock dummy broker thread");


        rd_kafka_q_destroy_owner(mcluster->ops);

        rd_kafka_timers_destroy(&mcluster->timers);

        if (mcluster->fd_size > 0) {
                rd_free(mcluster->fds);
                rd_free(mcluster->handlers);
        }

        mtx_destroy(&mcluster->lock);

        rd_free(mcluster->bootstraps);

        rd_socket_close(mcluster->wakeup_fds[0]);
        rd_socket_close(mcluster->wakeup_fds[1]);

        if (mcluster->metrics) {
                for (i = 0; i < mcluster->metrics_cnt; i++) {
                        rd_free(mcluster->metrics[i]);
                }
                rd_free(mcluster->metrics);
        }
}



void rd_kafka_mock_cluster_destroy(rd_kafka_mock_cluster_t *mcluster) {
        int res;
        rd_kafka_op_t *rko;

        rd_kafka_dbg(mcluster->rk, MOCK, "MOCK", "Destroying cluster");

        rd_assert(rd_atomic32_get(&mcluster->rk->rk_mock.cluster_cnt) > 0);
        rd_atomic32_sub(&mcluster->rk->rk_mock.cluster_cnt, 1);

        rko = rd_kafka_op_req2(mcluster->ops, RD_KAFKA_OP_TERMINATE);

        if (rko)
                rd_kafka_op_destroy(rko);

        if (thrd_join(mcluster->thread, &res) != thrd_success)
                rd_assert(!*"failed to join mock thread");

        rd_free(mcluster);
}



rd_kafka_mock_cluster_t *rd_kafka_mock_cluster_new(rd_kafka_t *rk,
                                                   int broker_cnt) {
        rd_kafka_mock_cluster_t *mcluster;
        rd_kafka_mock_broker_t *mrkb;
        int i, r;
        size_t bootstraps_len = 0;
        size_t of;

        mcluster     = rd_calloc(1, sizeof(*mcluster));
        mcluster->rk = rk;

        mcluster->dummy_rkb =
            rd_kafka_broker_add(rk, RD_KAFKA_INTERNAL, RD_KAFKA_PROTO_PLAINTEXT,
                                "mock", 0, RD_KAFKA_NODEID_UA);
        rd_snprintf(mcluster->id, sizeof(mcluster->id), "mockCluster%lx",
                    (intptr_t)mcluster >> 2);

        TAILQ_INIT(&mcluster->brokers);

        for (i = 1; i <= broker_cnt; i++) {
                if (!(mrkb = rd_kafka_mock_broker_new(mcluster, i, NULL))) {
                        rd_kafka_mock_cluster_destroy(mcluster);
                        return NULL;
                }

                /* advertised listener + ":port" + "," */
                bootstraps_len += strlen(mrkb->advertised_listener) + 6 + 1;
        }

        mtx_init(&mcluster->lock, mtx_plain);

        TAILQ_INIT(&mcluster->topics);
        mcluster->defaults.partition_cnt      = 4;
        mcluster->defaults.replication_factor = RD_MIN(3, broker_cnt);
        mcluster->defaults.group_initial_rebalance_delay_ms     = 3000;
        mcluster->track_requests                                = rd_false;
        mcluster->defaults.group_consumer_session_timeout_ms    = 30000;
        mcluster->defaults.group_consumer_heartbeat_interval_ms = 3000;

        TAILQ_INIT(&mcluster->cgrps_classic);

        TAILQ_INIT(&mcluster->cgrps_consumer);

        TAILQ_INIT(&mcluster->coords);

        rd_list_init(&mcluster->pids, 16, rd_free);

        TAILQ_INIT(&mcluster->errstacks);

        memcpy(mcluster->api_handlers, rd_kafka_mock_api_handlers,
               sizeof(mcluster->api_handlers));

        rd_list_init(&mcluster->request_list, 0, rd_kafka_mock_request_free);

        /* Use an op queue for controlling the cluster in
         * a thread-safe manner without locking. */
        mcluster->ops             = rd_kafka_q_new(rk);
        mcluster->ops->rkq_serve  = rd_kafka_mock_cluster_op_serve;
        mcluster->ops->rkq_opaque = mcluster;

        rd_kafka_timers_init(&mcluster->timers, rk, mcluster->ops);

        if ((r = rd_pipe_nonblocking(mcluster->wakeup_fds)) == -1) {
                rd_kafka_log(rk, LOG_ERR, "MOCK",
                             "Failed to setup mock cluster wake-up fds: %s",
                             rd_socket_strerror(r));
        } else {
                const char onebyte = 1;
                rd_kafka_q_io_event_enable(mcluster->ops,
                                           mcluster->wakeup_fds[1], &onebyte,
                                           sizeof(onebyte));
        }


        if (thrd_create(&mcluster->thread, rd_kafka_mock_cluster_thread_main,
                        mcluster) != thrd_success) {
                rd_kafka_log(rk, LOG_CRIT, "MOCK",
                             "Failed to create mock cluster thread: %s",
                             rd_strerror(errno));
                rd_kafka_mock_cluster_destroy(mcluster);
                return NULL;
        }


        /* Construct bootstrap.servers list */
        mcluster->bootstraps = rd_malloc(bootstraps_len + 1);
        of                   = 0;
        TAILQ_FOREACH(mrkb, &mcluster->brokers, link) {
                r = rd_snprintf(&mcluster->bootstraps[of], bootstraps_len - of,
                                "%s%s:%hu", of > 0 ? "," : "",
                                mrkb->advertised_listener, mrkb->port);
                of += r;
                rd_assert(of < bootstraps_len);
        }
        mcluster->bootstraps[of] = '\0';

        rd_kafka_dbg(rk, MOCK, "MOCK", "Mock cluster %s bootstrap.servers=%s",
                     mcluster->id, mcluster->bootstraps);

        rd_atomic32_add(&rk->rk_mock.cluster_cnt, 1);

        return mcluster;
}


rd_kafka_t *
rd_kafka_mock_cluster_handle(const rd_kafka_mock_cluster_t *mcluster) {
        return (rd_kafka_t *)mcluster->rk;
}

rd_kafka_mock_cluster_t *rd_kafka_handle_mock_cluster(const rd_kafka_t *rk) {
        return (rd_kafka_mock_cluster_t *)rk->rk_mock.cluster;
}


const char *
rd_kafka_mock_cluster_bootstraps(const rd_kafka_mock_cluster_t *mcluster) {
        return mcluster->bootstraps;
}

/**
 * @struct Represents a request to the mock cluster along with a timestamp.
 */
struct rd_kafka_mock_request_s {
        int32_t id;      /**< Broker id */
        int16_t api_key; /**< API Key of request */
        rd_ts_t timestamp /**< Timestamp at which request was received */;
};

/**
 * @brief Allocate and initialize a rd_kafka_mock_request_t *
 */
static rd_kafka_mock_request_t *
rd_kafka_mock_request_new(int32_t id, int16_t api_key, int64_t timestamp_us) {
        rd_kafka_mock_request_t *request;
        request            = rd_malloc(sizeof(*request));
        request->id        = id;
        request->api_key   = api_key;
        request->timestamp = timestamp_us;
        return request;
}

static rd_kafka_mock_request_t *
rd_kafka_mock_request_copy(rd_kafka_mock_request_t *mrequest) {
        rd_kafka_mock_request_t *request;
        request            = rd_malloc(sizeof(*request));
        request->id        = mrequest->id;
        request->api_key   = mrequest->api_key;
        request->timestamp = mrequest->timestamp;
        return request;
}

void rd_kafka_mock_request_destroy(rd_kafka_mock_request_t *mrequest) {
        rd_free(mrequest);
}

void rd_kafka_mock_request_destroy_array(rd_kafka_mock_request_t **mrequests,
                                         size_t mrequest_cnt) {
        size_t i;
        for (i = 0; i < mrequest_cnt; i++)
                rd_kafka_mock_request_destroy(mrequests[i]);
        rd_free(mrequests);
}

static void rd_kafka_mock_request_free(void *element) {
        rd_kafka_mock_request_destroy(element);
}

void rd_kafka_mock_start_request_tracking(rd_kafka_mock_cluster_t *mcluster) {
        mtx_lock(&mcluster->lock);
        mcluster->track_requests = rd_true;
        rd_list_clear(&mcluster->request_list);
        mtx_unlock(&mcluster->lock);
}

void rd_kafka_mock_stop_request_tracking(rd_kafka_mock_cluster_t *mcluster) {
        mtx_lock(&mcluster->lock);
        mcluster->track_requests = rd_false;
        rd_list_clear(&mcluster->request_list);
        mtx_unlock(&mcluster->lock);
}

rd_kafka_mock_request_t **
rd_kafka_mock_get_requests(rd_kafka_mock_cluster_t *mcluster, size_t *cntp) {
        size_t i;
        rd_kafka_mock_request_t **ret = NULL;

        mtx_lock(&mcluster->lock);
        *cntp = rd_list_cnt(&mcluster->request_list);
        if (*cntp > 0) {
                ret = rd_calloc(*cntp, sizeof(rd_kafka_mock_request_t *));
                for (i = 0; i < *cntp; i++) {
                        rd_kafka_mock_request_t *mreq =
                            rd_list_elem(&mcluster->request_list, i);
                        ret[i] = rd_kafka_mock_request_copy(mreq);
                }
        }

        mtx_unlock(&mcluster->lock);
        return ret;
}

void rd_kafka_mock_clear_requests(rd_kafka_mock_cluster_t *mcluster) {
        mtx_lock(&mcluster->lock);
        rd_list_clear(&mcluster->request_list);
        mtx_unlock(&mcluster->lock);
}

int32_t rd_kafka_mock_request_id(rd_kafka_mock_request_t *mreq) {
        return mreq->id;
}

int16_t rd_kafka_mock_request_api_key(rd_kafka_mock_request_t *mreq) {
        return mreq->api_key;
}

rd_ts_t rd_kafka_mock_request_timestamp(rd_kafka_mock_request_t *mreq) {
        return mreq->timestamp;
}

/* Unit tests */

/**
 * @brief Create a topic-partition list with vararg arguments.
 *
 * @param cnt Number of topic-partitions.
 * @param ...vararg is a tuple of:
 *           const char *topic_name
 *           int32_t partition
 *
 * @remark The returned pointer ownership is transferred to the caller.
 */
static rd_kafka_topic_partition_list_t *ut_topic_partitions(int cnt, ...) {
        va_list ap;
        const char *topic_name;
        int i = 0;

        rd_kafka_topic_partition_list_t *rktparlist =
            rd_kafka_topic_partition_list_new(cnt);
        va_start(ap, cnt);
        while (i < cnt) {
                topic_name        = va_arg(ap, const char *);
                int32_t partition = va_arg(ap, int32_t);

                rd_kafka_topic_partition_list_add(rktparlist, topic_name,
                                                  partition);
                i++;
        }
        va_end(ap);

        return rktparlist;
}

/**
 * @brief Assert \p expected partition list is equal to \p actual.
 *
 * @param expected Expected partition list.
 * @param actual Actual partition list.
 * @return Comparation result.
 */
static int ut_assert_topic_partitions(rd_kafka_topic_partition_list_t *expected,
                                      rd_kafka_topic_partition_list_t *actual) {
        rd_bool_t equal;
        char expected_str[256] = "";
        char actual_str[256]   = "";

        if (expected)
                RD_UT_ASSERT(actual, "list should be not-NULL, but it's NULL");
        else
                RD_UT_ASSERT(!actual, "list should be NULL, but it's not-NULL");


        if (!expected)
                return 0;

        equal = !rd_kafka_topic_partition_list_cmp(
            actual, expected, rd_kafka_topic_partition_cmp);

        if (!equal) {
                rd_kafka_topic_partition_list_str(expected, expected_str,
                                                  sizeof(expected_str),
                                                  RD_KAFKA_FMT_F_NO_ERR);
                rd_kafka_topic_partition_list_str(actual, actual_str,
                                                  sizeof(actual_str),
                                                  RD_KAFKA_FMT_F_NO_ERR);
        }

        RD_UT_ASSERT(equal, "list should be equal. Expected: %s, got: %s",
                     expected_str, actual_str);
        return 0;
}

/**
 * @struct Fixture used for testing next assignment calculation.
 */
struct cgrp_consumer_member_next_assignment_fixture {
        /** Current member epoch (after calling next assignment). */
        int32_t current_member_epoch;
        /** Current consumer assignment, if changed. */
        rd_kafka_topic_partition_list_t *current_assignment;
        /** Returned assignment, if expected. */
        rd_kafka_topic_partition_list_t *returned_assignment;
        /** Target assignment, if changed. */
        rd_kafka_topic_partition_list_t *target_assignment;
        /** Should simulate a disconnection and reconnection. */
        rd_bool_t reconnected;
        /** Should simulate a session time out. */
        rd_bool_t session_timed_out;
        /** Comment to log. */
        const char *comment;
};

/**
 * @brief Test next assignment calculation using passed \p fixtures.
 *        using a new cluster with a topic with name \p topic and
 *        \p partitions partitions.
 *
 * @param topic Topic name to create.
 * @param partitions Topic partition.
 * @param fixtures Array of fixtures for this test.
 * @param fixtures_cnt Number of elements in \p fixtures.
 * @return Number of occurred errors.
 */
static int ut_cgrp_consumer_member_next_assignment0(
    const char *topic,
    int partitions,
    struct cgrp_consumer_member_next_assignment_fixture *fixtures,
    size_t fixtures_cnt) {
        int failures                 = 0;
        int32_t current_member_epoch = 0;
        size_t i;
        rd_kafka_t *rk;
        rd_kafka_mock_cluster_t *mcluster;
        static rd_kafka_mock_topic_t *mtopic;
        rd_kafka_mock_cgrp_consumer_t *mcgrp;
        rd_kafka_mock_cgrp_consumer_member_t *member;
        char errstr[512];
        rd_kafkap_str_t GroupId         = {.str = "group", .len = 5};
        rd_kafkap_str_t MemberId        = {.str = "A", .len = 1};
        rd_kafkap_str_t InstanceId      = {.len = -1};
        rd_kafkap_str_t SubscribedTopic = {.str = topic, .len = strlen(topic)};
        rd_kafkap_str_t SubscribedTopicRegex = RD_KAFKAP_STR_INITIALIZER_EMPTY;
        struct rd_kafka_mock_connection_s *conn =
            (struct rd_kafka_mock_connection_s
                 *)1; /* fake connection instance */

        rk = rd_kafka_new(RD_KAFKA_CONSUMER, NULL, errstr, sizeof(errstr));
        mcluster = rd_kafka_mock_cluster_new(rk, 1);
        mcgrp    = rd_kafka_mock_cgrp_consumer_get(mcluster, &GroupId);
        member   = rd_kafka_mock_cgrp_consumer_member_add(
            mcgrp, conn, &MemberId, &InstanceId, &SubscribedTopic, 1,
            &SubscribedTopicRegex);
        mtopic = rd_kafka_mock_topic_new(mcluster, topic, partitions, 1);

        for (i = 0; i < fixtures_cnt; i++) {
                int j;
                rd_kafka_topic_partition_list_t *current_assignment,
                    *member_target_assignment, *next_assignment,
                    *returned_assignment;

                RD_UT_SAY("test fixture %" PRIusz ": %s", i,
                          fixtures[i].comment);

                if (fixtures[i].session_timed_out) {
                        rd_kafka_mock_cgrp_consumer_member_leave(mcgrp, member,
                                                                 rd_false);
                        member = rd_kafka_mock_cgrp_consumer_member_add(
                            mcgrp, conn, &MemberId, &InstanceId,
                            &SubscribedTopic, 1, &SubscribedTopicRegex);
                }

                if (fixtures[i].reconnected) {
                        rd_kafka_mock_cgrps_connection_closed(mcluster, conn);
                        conn++;
                        member = rd_kafka_mock_cgrp_consumer_member_add(
                            mcgrp, conn, &MemberId, &InstanceId,
                            &SubscribedTopic, 1, &SubscribedTopicRegex);
                }

                member_target_assignment = fixtures[i].target_assignment;
                if (member_target_assignment) {
                        rd_kafka_mock_cgrp_consumer_target_assignment_t
                            *target_assignment;

                        target_assignment =
                            rd_kafka_mock_cgrp_consumer_target_assignment_new(
                                (char **)&MemberId.str, 1,
                                &member_target_assignment);

                        rd_kafka_mock_cgrp_consumer_target_assignment(
                            mcluster, GroupId.str, target_assignment);
                        rd_kafka_mock_cgrp_consumer_target_assignment_destroy(
                            target_assignment);
                        rd_kafka_topic_partition_list_destroy(
                            member_target_assignment);
                }

                current_assignment = fixtures[i].current_assignment;
                if (current_assignment) {
                        /* Set topic id */
                        for (j = 0; j < current_assignment->cnt; j++) {
                                rd_kafka_topic_partition_set_topic_id(
                                    &current_assignment->elems[j], mtopic->id);
                        }
                }

                next_assignment =
                    rd_kafka_mock_cgrp_consumer_member_next_assignment(
                        member, current_assignment, &current_member_epoch);
                RD_IF_FREE(current_assignment,
                           rd_kafka_topic_partition_list_destroy);
                RD_UT_ASSERT(
                    current_member_epoch == fixtures[i].current_member_epoch,
                    "current member epoch after call. Expected: %" PRId32
                    ", got: %" PRId32,
                    fixtures[i].current_member_epoch, current_member_epoch);

                returned_assignment = fixtures[i].returned_assignment;
                failures += ut_assert_topic_partitions(returned_assignment,
                                                       next_assignment);

                RD_IF_FREE(next_assignment,
                           rd_kafka_topic_partition_list_destroy);
                RD_IF_FREE(returned_assignment,
                           rd_kafka_topic_partition_list_destroy);
        }

        rd_kafka_mock_cluster_destroy(mcluster);
        rd_kafka_destroy(rk);
        return failures;
}

/**
 * @brief Test case where multiple revocations are acked.
 *        Only when they're acked member epoch is bumped
 *        and a new partition is returned to the member.
 *
 * @return Number of occurred errors.
 */
static int ut_cgrp_consumer_member_next_assignment1(void) {
        RD_UT_SAY("Case 1: multiple revocations acked");

        const char *topic = "topic";
        struct cgrp_consumer_member_next_assignment_fixture fixtures[] = {
            {
                .comment = "Target+Returned assignment 0,1,2. Epoch 0 -> 3",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .target_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
                .returned_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
            },
            {
                .comment              = "Current assignment empty",
                .current_member_epoch = 3,
                .current_assignment   = ut_topic_partitions(0),
                .returned_assignment  = NULL,
            },
            {
                .comment              = "Empty heartbeat",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .returned_assignment  = NULL,
            },
            {
                .comment              = "Current assignment 0",
                .current_member_epoch = 3,
                .current_assignment   = ut_topic_partitions(1, topic, 0),
                .returned_assignment  = NULL,
            },
            {
                .comment              = "Empty heartbeat",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .returned_assignment  = NULL,
            },
            {
                .comment              = "Current assignment 0,1",
                .current_member_epoch = 3,
                .current_assignment =
                    ut_topic_partitions(2, topic, 0, topic, 1),
                .returned_assignment = NULL,
            },
            {
                .comment              = "Empty heartbeat",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .returned_assignment  = NULL,
            },
            {
                .comment              = "Current assignment 0,1,2",
                .current_member_epoch = 3,
                .current_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
                .returned_assignment = NULL,
            },
            {
                .comment = "Target assignment 0,1,3. Returned assignment 0,1",
                .current_member_epoch = 3,
                .target_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 3),
                .current_assignment = NULL,
                .returned_assignment =
                    ut_topic_partitions(2, topic, 0, topic, 1),
            },
            {
                .comment = "Target assignment 0,3. Returned assignment 0",
                .current_member_epoch = 3,
                .target_assignment = ut_topic_partitions(2, topic, 0, topic, 3),
                .current_assignment  = NULL,
                .returned_assignment = ut_topic_partitions(1, topic, 0),
            },
            {
                .comment              = "Empty heartbeat",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .returned_assignment  = NULL,
            },
            {
                .comment              = "Current assignment 0,1",
                .current_member_epoch = 3,
                .current_assignment =
                    ut_topic_partitions(2, topic, 0, topic, 1),
                .returned_assignment = NULL,
            },
            {
                .comment              = "Empty heartbeat",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .returned_assignment  = NULL,
            },
            {
                .comment = "Current assignment 0. Returned assignment 0,3. "
                           "Epoch 3 -> 5",
                .current_member_epoch = 5,
                .current_assignment   = ut_topic_partitions(1, topic, 0),
                .returned_assignment =
                    ut_topic_partitions(2, topic, 0, topic, 3),
            },
            {
                .comment              = "Empty heartbeat",
                .current_member_epoch = 5,
                .current_assignment   = NULL,
                .returned_assignment  = NULL,
            },
            {
                .comment              = "Current assignment 0,3",
                .current_member_epoch = 5,
                .current_assignment =
                    ut_topic_partitions(2, topic, 0, topic, 3),
                .returned_assignment = NULL,
            },
        };
        return ut_cgrp_consumer_member_next_assignment0(
            topic, 4, fixtures, RD_ARRAY_SIZE(fixtures));
}

/**
 * @brief Test case where multiple revocations happen.
 *        Only the first revocation is acked and after that
 *        there's a reassignment and epoch bump.
 *
 * @return Number of occurred errors.
 */
static int ut_cgrp_consumer_member_next_assignment2(void) {
        RD_UT_SAY(
            "Case 2: reassignment of revoked partition, partial revocation "
            "acknowledge");

        const char *topic = "topic";
        struct cgrp_consumer_member_next_assignment_fixture fixtures[] = {
            {
                .comment = "Target+Returned assignment 0,1,2. Epoch 0 -> 3",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .target_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
                .returned_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
            },
            {
                .comment              = "Current assignment 0,1,2",
                .current_member_epoch = 3,
                .current_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
                .returned_assignment = NULL,
            },
            {
                .comment = "Target assignment 0,1,3. Returned assignment 0,1",
                .current_member_epoch = 3,
                .target_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 3),
                .current_assignment = NULL,
                .returned_assignment =
                    ut_topic_partitions(2, topic, 0, topic, 1),
            },
            {
                .comment = "Target assignment 0,3. Returned assignment 0",
                .current_member_epoch = 3,
                .target_assignment = ut_topic_partitions(2, topic, 0, topic, 3),
                .current_assignment  = NULL,
                .returned_assignment = ut_topic_partitions(1, topic, 0),
            },
            {
                .comment              = "Empty heartbeat",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .returned_assignment  = NULL,
            },
            {
                .comment              = "Current assignment 0,1",
                .current_member_epoch = 3,
                .current_assignment =
                    ut_topic_partitions(2, topic, 0, topic, 1),
                .returned_assignment = NULL,
            },
            {
                .comment              = "Empty heartbeat",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .returned_assignment  = NULL,
            },
            {
                .comment = "Target+Returned assignment 0,1,3. Epoch 3 -> 6",
                .current_member_epoch = 6,
                .target_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 3),
                .current_assignment = NULL,
                .returned_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 3),
            },
            {
                .comment              = "Empty heartbeat",
                .current_member_epoch = 6,
                .current_assignment   = NULL,
                .returned_assignment  = NULL,
            },
            {
                .comment              = "Current assignment 0,1,3",
                .current_member_epoch = 6,
                .current_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 3),
                .returned_assignment = NULL,
            },
        };
        return ut_cgrp_consumer_member_next_assignment0(
            topic, 4, fixtures, RD_ARRAY_SIZE(fixtures));
}

/**
 * @brief Test case where multiple revocations happen.
 *        They aren't acked but then a
 *        reassignment of all the revoked partition happens, bumping the epoch.
 *
 * @return Number of occurred errors.
 */
static int ut_cgrp_consumer_member_next_assignment3(void) {
        RD_UT_SAY(
            "Case 3: reassignment of revoked partition and new partition, no "
            "revocation acknowledge");

        const char *topic = "topic";
        struct cgrp_consumer_member_next_assignment_fixture fixtures[] = {
            {
                .comment = "Target+Returned assignment 0,1,2. Epoch 0 -> 3",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .target_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
                .returned_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
            },
            {
                .comment              = "Current assignment 0,1,2",
                .current_member_epoch = 3,
                .current_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
                .returned_assignment = NULL,
            },
            {
                .comment = "Target assignment 0,1,3. Returned assignment 0,1",
                .current_member_epoch = 3,
                .target_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 3),
                .current_assignment = NULL,
                .returned_assignment =
                    ut_topic_partitions(2, topic, 0, topic, 1),
            },
            {
                .comment = "Target assignment 0,3. Returned assignment 0",
                .current_member_epoch = 3,
                .target_assignment = ut_topic_partitions(2, topic, 0, topic, 3),
                .current_assignment  = NULL,
                .returned_assignment = ut_topic_partitions(1, topic, 0),
            },
            {
                .comment              = "Empty heartbeat",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .returned_assignment  = NULL,
            },
            {
                .comment = "Target+Returned assignment 0,1,2,3. Epoch 3 -> 6",
                .current_member_epoch = 6,
                .target_assignment    = ut_topic_partitions(
                    3, topic, 0, topic, 1, topic, 2, topic, 3, NULL),
                .returned_assignment = ut_topic_partitions(
                    3, topic, 0, topic, 1, topic, 2, topic, 3, NULL),
            },
            {
                .comment              = "Empty heartbeat",
                .current_member_epoch = 6,
                .current_assignment   = NULL,
                .returned_assignment  = NULL,
            },
            {
                .comment              = "Current assignment 0,1,2,3",
                .current_member_epoch = 6,
                .current_assignment   = ut_topic_partitions(
                    3, topic, 0, topic, 1, topic, 2, topic, 3, NULL),
                .returned_assignment = NULL,
            },
        };
        return ut_cgrp_consumer_member_next_assignment0(
            topic, 4, fixtures, RD_ARRAY_SIZE(fixtures));
}

/**
 * @brief Test case where a disconnection happens and after that
 *        the client send its assignment again, with same member epoch,
 *        and receives back the returned assignment, even if the same.
 *
 * @return Number of occurred errors.
 */
static int ut_cgrp_consumer_member_next_assignment4(void) {
        RD_UT_SAY("Case 4: reconciliation after disconnection");

        const char *topic = "topic";
        struct cgrp_consumer_member_next_assignment_fixture fixtures[] = {
            {
                .comment = "Target+Returned assignment 0,1,2. Epoch 0 -> 3",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .target_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
                .returned_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
            },
            {
                .comment              = "Current assignment empty",
                .current_member_epoch = 3,
                .current_assignment   = ut_topic_partitions(0),
                .returned_assignment  = NULL,
            },
            {
                .comment = "Disconnected, resends current assignment. Returns "
                           "assignment again",
                .reconnected          = rd_true,
                .current_member_epoch = 3,
                .current_assignment   = ut_topic_partitions(0),
                .returned_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
            },
            {
                .comment              = "Empty heartbeat",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .returned_assignment  = NULL,
            },
            {
                .comment              = "Current assignment 0,1,2",
                .current_member_epoch = 3,
                .current_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
                .returned_assignment = NULL,
            },
        };
        return ut_cgrp_consumer_member_next_assignment0(
            topic, 3, fixtures, RD_ARRAY_SIZE(fixtures));
}

/**
 * @brief Test case where a session timeout happens and then
 *        the client receives a FENCED_MEMBER_EPOCH error,
 *        revokes all of its partitions and rejoins with epoch 0.
 *
 * @return Number of occurred errors.
 */
static int ut_cgrp_consumer_member_next_assignment5(void) {
        RD_UT_SAY("Case 5: fenced consumer");

        const char *topic = "topic";
        struct cgrp_consumer_member_next_assignment_fixture fixtures[] = {
            {
                .comment = "Target+Returned assignment 0,1,2. Epoch 0 -> 3",
                .current_member_epoch = 3,
                .current_assignment   = NULL,
                .target_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
                .returned_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
            },
            {
                .comment = "Session times out, receives FENCED_MEMBER_EPOCH. "
                           "Epoch 3 -> 0",
                .session_timed_out    = rd_true,
                .current_member_epoch = -1,
                .current_assignment   = NULL,
                .returned_assignment  = NULL,
            },
            {
                .comment = "Target+Returned assignment 0,1,2. Epoch 0 -> 6",
                .target_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
                .current_member_epoch = 4,
                .current_assignment   = NULL,
                .returned_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
            },
            {
                .comment              = "Current assignment 0,1,2",
                .current_member_epoch = 4,
                .current_assignment =
                    ut_topic_partitions(3, topic, 0, topic, 1, topic, 2),
                .returned_assignment = NULL,
            },
        };
        return ut_cgrp_consumer_member_next_assignment0(
            topic, 3, fixtures, RD_ARRAY_SIZE(fixtures));
}

/**
 * @brief Test all next assignment calculation cases,
 *        for KIP-848 consumer group type and collect
 *        number of errors.
 *
 * @return Number of occurred errors.
 */
static int ut_cgrp_consumer_member_next_assignment(void) {
        RD_UT_BEGIN();
        int failures = 0;

        failures += ut_cgrp_consumer_member_next_assignment1();
        failures += ut_cgrp_consumer_member_next_assignment2();
        failures += ut_cgrp_consumer_member_next_assignment3();
        failures += ut_cgrp_consumer_member_next_assignment4();
        failures += ut_cgrp_consumer_member_next_assignment5();

        RD_UT_ASSERT(!failures, "some tests failed");
        RD_UT_PASS();
}

/**
 * @brief Mock cluster unit tests
 */
int unittest_mock_cluster(void) {
        int fails = 0;
        fails += ut_cgrp_consumer_member_next_assignment();
        return fails;
}
