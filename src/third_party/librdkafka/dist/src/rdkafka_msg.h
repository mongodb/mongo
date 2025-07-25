/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012,2013 Magnus Edenhill
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

#ifndef _RDKAFKA_MSG_H_
#define _RDKAFKA_MSG_H_

#include "rdsysqueue.h"

#include "rdkafka_proto.h"
#include "rdkafka_header.h"


/**
 * @brief Internal RD_KAFKA_MSG_F_.. flags
 */
#define RD_KAFKA_MSG_F_RKT_RDLOCKED 0x100000 /* rkt is rdlock():ed */


/**
 * @brief Message.MsgAttributes for MsgVersion v0..v1,
 *        also used for MessageSet.Attributes for MsgVersion v2.
 */
#define RD_KAFKA_MSG_ATTR_GZIP             (1 << 0)
#define RD_KAFKA_MSG_ATTR_SNAPPY           (1 << 1)
#define RD_KAFKA_MSG_ATTR_KLZ4              (3)
#define RD_KAFKA_MSG_ATTR_ZSTD             (4)
#define RD_KAFKA_MSG_ATTR_COMPRESSION_MASK 0x7
#define RD_KAFKA_MSG_ATTR_CREATE_TIME      (0 << 3)
#define RD_KAFKA_MSG_ATTR_LOG_APPEND_TIME  (1 << 3)

/**
 * @brief MessageSet.Attributes for MsgVersion v2
 *
 * Attributes:
 *  -------------------------------------------------------------------------------------------------
 *  | Unused (6-15) | Control (5) | Transactional (4) | Timestamp Type (3) |
 * Compression Type (0-2) |
 *  -------------------------------------------------------------------------------------------------
 */
/* Compression types same as MsgVersion 0 above */
/* Timestamp type same as MsgVersion 0 above */
#define RD_KAFKA_MSGSET_V2_ATTR_TRANSACTIONAL (1 << 4)
#define RD_KAFKA_MSGSET_V2_ATTR_CONTROL       (1 << 5)


typedef struct rd_kafka_msg_s {
        rd_kafka_message_t rkm_rkmessage; /* MUST be first field */
#define rkm_len       rkm_rkmessage.len
#define rkm_payload   rkm_rkmessage.payload
#define rkm_opaque    rkm_rkmessage._private
#define rkm_partition rkm_rkmessage.partition
#define rkm_offset    rkm_rkmessage.offset
#define rkm_key       rkm_rkmessage.key
#define rkm_key_len   rkm_rkmessage.key_len
#define rkm_err       rkm_rkmessage.err

        TAILQ_ENTRY(rd_kafka_msg_s) rkm_link;

        int rkm_flags;
        /* @remark These additional flags must not collide with
         *         the RD_KAFKA_MSG_F_* flags in rdkafka.h */
#define RD_KAFKA_MSG_F_FREE_RKM 0x10000 /* msg_t is allocated */
#define RD_KAFKA_MSG_F_ACCOUNT  0x20000 /* accounted for in curr_msgs */
#define RD_KAFKA_MSG_F_PRODUCER 0x40000 /* Producer message */
#define RD_KAFKA_MSG_F_CONTROL  0x80000 /* Control message */

        rd_kafka_timestamp_type_t rkm_tstype; /* rkm_timestamp type */
        int64_t rkm_timestamp;                /* Message format V1.
                                               * Meaning of timestamp depends on
                                               * message Attribute LogAppendtime (broker)
                                               * or CreateTime (producer).
                                               * Unit is milliseconds since epoch (UTC).*/


        rd_kafka_headers_t *rkm_headers; /**< Parsed headers list, if any. */

        rd_kafka_msg_status_t rkm_status; /**< Persistence status. Updated in
                                           *   the ProduceResponse handler:
                                           *   this value is always up to date.
                                           */
        int32_t rkm_broker_id;            /**< Broker message was produced to
                                           *   or fetched from. */

        union {
                struct {
                        rd_ts_t ts_timeout;  /* Message timeout */
                        rd_ts_t ts_enq;      /* Enqueue/Produce time */
                        rd_ts_t ts_backoff;  /* Backoff next Produce until
                                              * this time. */
                        uint64_t msgid;      /**< Message sequencial id,
                                              *   used to maintain ordering.
                                              *   Starts at 1. */
                        uint64_t last_msgid; /**< On retry this is set
                                              *   on the first message
                                              *   in a batch to point
                                              *   out the last message
                                              *   of the batch so that
                                              *   the batch can be
                                              *   identically reconstructed.
                                              */
                        int retries;         /* Number of retries so far */
                } producer;
#define rkm_ts_timeout rkm_u.producer.ts_timeout
#define rkm_ts_enq     rkm_u.producer.ts_enq
#define rkm_msgid      rkm_u.producer.msgid

                struct {
                        rd_kafkap_bytes_t binhdrs; /**< Unparsed
                                                    *   binary headers in
                                                    *   protocol msg */
                } consumer;
        } rkm_u;
} rd_kafka_msg_t;

TAILQ_HEAD(rd_kafka_msg_head_s, rd_kafka_msg_s);


/** @returns the absolute time a message was enqueued (producer) */
#define rd_kafka_msg_enq_time(rkm) ((rkm)->rkm_ts_enq)

/**
 * @returns the message's total maximum on-wire size.
 * @remark Depending on message version (MagicByte) the actual size
 *         may be smaller.
 */
static RD_INLINE RD_UNUSED size_t
rd_kafka_msg_wire_size(const rd_kafka_msg_t *rkm, int MsgVersion) {
        static const size_t overheads[] = {
            [0] = RD_KAFKAP_MESSAGE_V0_OVERHEAD,
            [1] = RD_KAFKAP_MESSAGE_V1_OVERHEAD,
            [2] = RD_KAFKAP_MESSAGE_V2_MAX_OVERHEAD};
        size_t size;
        rd_dassert(MsgVersion >= 0 && MsgVersion <= 2);

        size = overheads[MsgVersion] + rkm->rkm_len + rkm->rkm_key_len;
        if (MsgVersion == 2 && rkm->rkm_headers)
                size += rd_kafka_headers_serialized_size(rkm->rkm_headers);

        return size;
}


/**
 * @returns the maximum total on-wire message size regardless of MsgVersion.
 *
 * @remark This does not account for the ProduceRequest, et.al, just the
 *         per-message overhead.
 */
static RD_INLINE RD_UNUSED size_t rd_kafka_msg_max_wire_size(size_t keylen,
                                                             size_t valuelen,
                                                             size_t hdrslen) {
        return RD_KAFKAP_MESSAGE_V2_MAX_OVERHEAD + keylen + valuelen + hdrslen;
}

/**
 * @returns the enveloping rd_kafka_msg_t pointer for a rd_kafka_msg_t
 *          wrapped rd_kafka_message_t.
 */
static RD_INLINE RD_UNUSED rd_kafka_msg_t *
rd_kafka_message2msg(rd_kafka_message_t *rkmessage) {
        return (rd_kafka_msg_t *)rkmessage;
}



/**
 * @brief Message queue with message and byte counters.
 */
TAILQ_HEAD(rd_kafka_msgs_head_s, rd_kafka_msg_s);
typedef struct rd_kafka_msgq_s {
        struct rd_kafka_msgs_head_s rkmq_msgs; /* TAILQ_HEAD */
        int32_t rkmq_msg_cnt;
        int64_t rkmq_msg_bytes;
        struct {
                rd_ts_t abstime; /**< Allow wake-ups after this point in time.*/
                int32_t msg_cnt; /**< Signal wake-up when this message count
                                  *   is reached. */
                int64_t msg_bytes;   /**< .. or when this byte count is
                                      *   reached. */
                rd_bool_t on_first;  /**< Wake-up on first message enqueued
                                      *   regardless of .abstime. */
                rd_bool_t signalled; /**< Wake-up (already) signalled. */
        } rkmq_wakeup;
} rd_kafka_msgq_t;

#define RD_KAFKA_MSGQ_INITIALIZER(rkmq)                                        \
        { .rkmq_msgs = TAILQ_HEAD_INITIALIZER((rkmq).rkmq_msgs) }

#define RD_KAFKA_MSGQ_FOREACH(elm, head)                                       \
        TAILQ_FOREACH(elm, &(head)->rkmq_msgs, rkm_link)

/* @brief Check if queue is empty. Proper locks must be held. */
#define RD_KAFKA_MSGQ_EMPTY(rkmq) TAILQ_EMPTY(&(rkmq)->rkmq_msgs)

/**
 * Returns the number of messages in the specified queue.
 */
static RD_INLINE RD_UNUSED int rd_kafka_msgq_len(const rd_kafka_msgq_t *rkmq) {
        return (int)rkmq->rkmq_msg_cnt;
}

/**
 * Returns the total number of bytes in the specified queue.
 */
static RD_INLINE RD_UNUSED size_t
rd_kafka_msgq_size(const rd_kafka_msgq_t *rkmq) {
        return (size_t)rkmq->rkmq_msg_bytes;
}


void rd_kafka_msg_destroy(rd_kafka_t *rk, rd_kafka_msg_t *rkm);

int rd_kafka_msg_new(rd_kafka_topic_t *rkt,
                     int32_t force_partition,
                     int msgflags,
                     char *payload,
                     size_t len,
                     const void *keydata,
                     size_t keylen,
                     void *msg_opaque);

static RD_INLINE RD_UNUSED void rd_kafka_msgq_init(rd_kafka_msgq_t *rkmq) {
        TAILQ_INIT(&rkmq->rkmq_msgs);
        rkmq->rkmq_msg_cnt   = 0;
        rkmq->rkmq_msg_bytes = 0;
}

#if ENABLE_DEVEL
#define rd_kafka_msgq_verify_order(rktp, rkmq, exp_first_msgid, gapless)       \
        rd_kafka_msgq_verify_order0(__FUNCTION__, __LINE__, rktp, rkmq,        \
                                    exp_first_msgid, gapless)
#else
#define rd_kafka_msgq_verify_order(rktp, rkmq, exp_first_msgid, gapless)       \
        do {                                                                   \
        } while (0)
#endif

void rd_kafka_msgq_verify_order0(const char *function,
                                 int line,
                                 const struct rd_kafka_toppar_s *rktp,
                                 const rd_kafka_msgq_t *rkmq,
                                 uint64_t exp_first_msgid,
                                 rd_bool_t gapless);


/**
 * Concat all elements of 'src' onto tail of 'dst'.
 * 'src' will be cleared.
 * Proper locks for 'src' and 'dst' must be held.
 */
static RD_INLINE RD_UNUSED void rd_kafka_msgq_concat(rd_kafka_msgq_t *dst,
                                                     rd_kafka_msgq_t *src) {
        TAILQ_CONCAT(&dst->rkmq_msgs, &src->rkmq_msgs, rkm_link);
        dst->rkmq_msg_cnt += src->rkmq_msg_cnt;
        dst->rkmq_msg_bytes += src->rkmq_msg_bytes;
        rd_kafka_msgq_init(src);
        rd_kafka_msgq_verify_order(NULL, dst, 0, rd_false);
}

/**
 * Move queue 'src' to 'dst' (overwrites dst)
 * Source will be cleared.
 */
static RD_INLINE RD_UNUSED void rd_kafka_msgq_move(rd_kafka_msgq_t *dst,
                                                   rd_kafka_msgq_t *src) {
        TAILQ_MOVE(&dst->rkmq_msgs, &src->rkmq_msgs, rkm_link);
        dst->rkmq_msg_cnt   = src->rkmq_msg_cnt;
        dst->rkmq_msg_bytes = src->rkmq_msg_bytes;
        rd_kafka_msgq_init(src);
        rd_kafka_msgq_verify_order(NULL, dst, 0, rd_false);
}


/**
 * @brief Prepend all elements of \ src onto head of \p dst.
 *        \p src will be cleared/re-initialized.
 *
 * @locks proper locks for \p src and \p dst MUST be held.
 */
static RD_INLINE RD_UNUSED void rd_kafka_msgq_prepend(rd_kafka_msgq_t *dst,
                                                      rd_kafka_msgq_t *src) {
        rd_kafka_msgq_concat(src, dst);
        rd_kafka_msgq_move(dst, src);
        rd_kafka_msgq_verify_order(NULL, dst, 0, rd_false);
}


/**
 * rd_free all msgs in msgq and reinitialize the msgq.
 */
static RD_INLINE RD_UNUSED void rd_kafka_msgq_purge(rd_kafka_t *rk,
                                                    rd_kafka_msgq_t *rkmq) {
        rd_kafka_msg_t *rkm, *next;

        next = TAILQ_FIRST(&rkmq->rkmq_msgs);
        while (next) {
                rkm  = next;
                next = TAILQ_NEXT(next, rkm_link);

                rd_kafka_msg_destroy(rk, rkm);
        }

        rd_kafka_msgq_init(rkmq);
}


/**
 * Remove message from message queue
 */
static RD_INLINE RD_UNUSED rd_kafka_msg_t *
rd_kafka_msgq_deq(rd_kafka_msgq_t *rkmq, rd_kafka_msg_t *rkm, int do_count) {
        if (likely(do_count)) {
                rd_kafka_assert(NULL, rkmq->rkmq_msg_cnt > 0);
                rd_kafka_assert(NULL,
                                rkmq->rkmq_msg_bytes >=
                                    (int64_t)(rkm->rkm_len + rkm->rkm_key_len));
                rkmq->rkmq_msg_cnt--;
                rkmq->rkmq_msg_bytes -= rkm->rkm_len + rkm->rkm_key_len;
        }

        TAILQ_REMOVE(&rkmq->rkmq_msgs, rkm, rkm_link);

        return rkm;
}

static RD_INLINE RD_UNUSED rd_kafka_msg_t *
rd_kafka_msgq_pop(rd_kafka_msgq_t *rkmq) {
        rd_kafka_msg_t *rkm;

        if (((rkm = TAILQ_FIRST(&rkmq->rkmq_msgs))))
                rd_kafka_msgq_deq(rkmq, rkm, 1);

        return rkm;
}


/**
 * @returns the first message in the queue, or NULL if empty.
 *
 * @locks caller's responsibility
 */
static RD_INLINE RD_UNUSED rd_kafka_msg_t *
rd_kafka_msgq_first(const rd_kafka_msgq_t *rkmq) {
        return TAILQ_FIRST(&rkmq->rkmq_msgs);
}

/**
 * @returns the last message in the queue, or NULL if empty.
 *
 * @locks caller's responsibility
 */
static RD_INLINE RD_UNUSED rd_kafka_msg_t *
rd_kafka_msgq_last(const rd_kafka_msgq_t *rkmq) {
        return TAILQ_LAST(&rkmq->rkmq_msgs, rd_kafka_msgs_head_s);
}


/**
 * @returns the MsgId of the first message in the queue, or 0 if empty.
 *
 * @locks caller's responsibility
 */
static RD_INLINE RD_UNUSED uint64_t
rd_kafka_msgq_first_msgid(const rd_kafka_msgq_t *rkmq) {
        const rd_kafka_msg_t *rkm = TAILQ_FIRST(&rkmq->rkmq_msgs);
        if (rkm)
                return rkm->rkm_u.producer.msgid;
        else
                return 0;
}



rd_bool_t rd_kafka_msgq_allow_wakeup_at(rd_kafka_msgq_t *rkmq,
                                        const rd_kafka_msgq_t *dest_rkmq,
                                        rd_ts_t *next_wakeup,
                                        rd_ts_t now,
                                        rd_ts_t linger_us,
                                        int32_t batch_msg_cnt,
                                        int64_t batch_msg_bytes);

/**
 * @returns true if msgq may be awoken.
 */

static RD_INLINE RD_UNUSED rd_bool_t
rd_kafka_msgq_may_wakeup(const rd_kafka_msgq_t *rkmq, rd_ts_t now) {
        /* No: Wakeup already signalled */
        if (rkmq->rkmq_wakeup.signalled)
                return rd_false;

        /* Yes: Wakeup linger time has expired */
        if (now >= rkmq->rkmq_wakeup.abstime)
                return rd_true;

        /* Yes: First message enqueued may trigger wakeup */
        if (rkmq->rkmq_msg_cnt == 1 && rkmq->rkmq_wakeup.on_first)
                return rd_true;

        /* Yes: batch.size or batch.num.messages exceeded */
        if (rkmq->rkmq_msg_cnt >= rkmq->rkmq_wakeup.msg_cnt ||
            rkmq->rkmq_msg_bytes > rkmq->rkmq_wakeup.msg_bytes)
                return rd_true;

        /* No */
        return rd_false;
}


/**
 * @brief Message ordering comparator using the message id
 *        number to order messages in ascending order (FIFO).
 */
static RD_INLINE int rd_kafka_msg_cmp_msgid(const void *_a, const void *_b) {
        const rd_kafka_msg_t *a = _a, *b = _b;

        rd_dassert(a->rkm_u.producer.msgid);

        return RD_CMP(a->rkm_u.producer.msgid, b->rkm_u.producer.msgid);
}

/**
 * @brief Message ordering comparator using the message id
 *        number to order messages in descending order (LIFO).
 */
static RD_INLINE int rd_kafka_msg_cmp_msgid_lifo(const void *_a,
                                                 const void *_b) {
        const rd_kafka_msg_t *a = _a, *b = _b;

        rd_dassert(a->rkm_u.producer.msgid);

        return RD_CMP(b->rkm_u.producer.msgid, a->rkm_u.producer.msgid);
}


/**
 * @brief Insert message at its sorted position using the msgid.
 * @remark This is an O(n) operation.
 * @warning The message must have a msgid set.
 * @returns the message count of the queue after enqueuing the message.
 */
int rd_kafka_msgq_enq_sorted0(rd_kafka_msgq_t *rkmq,
                              rd_kafka_msg_t *rkm,
                              int (*order_cmp)(const void *, const void *));

/**
 * @brief Insert message at its sorted position using the msgid.
 * @remark This is an O(n) operation.
 * @warning The message must have a msgid set.
 * @returns the message count of the queue after enqueuing the message.
 */
int rd_kafka_msgq_enq_sorted(const rd_kafka_topic_t *rkt,
                             rd_kafka_msgq_t *rkmq,
                             rd_kafka_msg_t *rkm);

/**
 * Insert message at head of message queue.
 */
static RD_INLINE RD_UNUSED void rd_kafka_msgq_insert(rd_kafka_msgq_t *rkmq,
                                                     rd_kafka_msg_t *rkm) {
        TAILQ_INSERT_HEAD(&rkmq->rkmq_msgs, rkm, rkm_link);
        rkmq->rkmq_msg_cnt++;
        rkmq->rkmq_msg_bytes += rkm->rkm_len + rkm->rkm_key_len;
}

/**
 * Append message to tail of message queue.
 */
static RD_INLINE RD_UNUSED int rd_kafka_msgq_enq(rd_kafka_msgq_t *rkmq,
                                                 rd_kafka_msg_t *rkm) {
        TAILQ_INSERT_TAIL(&rkmq->rkmq_msgs, rkm, rkm_link);
        rkmq->rkmq_msg_bytes += rkm->rkm_len + rkm->rkm_key_len;
        return (int)++rkmq->rkmq_msg_cnt;
}


/**
 * @returns true if the MsgId extents (first, last) in the two queues overlap.
 */
static RD_INLINE RD_UNUSED rd_bool_t
rd_kafka_msgq_overlap(const rd_kafka_msgq_t *a, const rd_kafka_msgq_t *b) {
        const rd_kafka_msg_t *fa, *la, *fb, *lb;

        if (RD_KAFKA_MSGQ_EMPTY(a) || RD_KAFKA_MSGQ_EMPTY(b))
                return rd_false;

        fa = rd_kafka_msgq_first(a);
        fb = rd_kafka_msgq_first(b);
        la = rd_kafka_msgq_last(a);
        lb = rd_kafka_msgq_last(b);

        return (rd_bool_t)(
            fa->rkm_u.producer.msgid <= lb->rkm_u.producer.msgid &&
            fb->rkm_u.producer.msgid <= la->rkm_u.producer.msgid);
}

/**
 * Scans a message queue for timed out messages and removes them from
 * 'rkmq' and adds them to 'timedout', returning the number of timed out
 * messages.
 * 'timedout' must be initialized.
 */
int rd_kafka_msgq_age_scan(struct rd_kafka_toppar_s *rktp,
                           rd_kafka_msgq_t *rkmq,
                           rd_kafka_msgq_t *timedout,
                           rd_ts_t now,
                           rd_ts_t *abs_next_timeout);

void rd_kafka_msgq_split(rd_kafka_msgq_t *leftq,
                         rd_kafka_msgq_t *rightq,
                         rd_kafka_msg_t *first_right,
                         int cnt,
                         int64_t bytes);

rd_kafka_msg_t *rd_kafka_msgq_find_pos(const rd_kafka_msgq_t *rkmq,
                                       const rd_kafka_msg_t *start_pos,
                                       const rd_kafka_msg_t *rkm,
                                       int (*cmp)(const void *, const void *),
                                       int *cntp,
                                       int64_t *bytesp);

void rd_kafka_msgq_set_metadata(rd_kafka_msgq_t *rkmq,
                                int32_t broker_id,
                                int64_t base_offset,
                                int64_t timestamp,
                                rd_kafka_msg_status_t status);

void rd_kafka_msgq_move_acked(rd_kafka_msgq_t *dest,
                              rd_kafka_msgq_t *src,
                              uint64_t last_msgid,
                              rd_kafka_msg_status_t status);

int rd_kafka_msg_partitioner(rd_kafka_topic_t *rkt,
                             rd_kafka_msg_t *rkm,
                             rd_dolock_t do_lock);


rd_kafka_message_t *rd_kafka_message_get(struct rd_kafka_op_s *rko);
rd_kafka_message_t *rd_kafka_message_get_from_rkm(struct rd_kafka_op_s *rko,
                                                  rd_kafka_msg_t *rkm);
rd_kafka_message_t *rd_kafka_message_new(void);


/**
 * @returns a (possibly) wrapped Kafka protocol message sequence counter
 *          for the non-overflowing \p seq.
 */
static RD_INLINE RD_UNUSED int32_t rd_kafka_seq_wrap(int64_t seq) {
        return (int32_t)(seq & (int64_t)INT32_MAX);
}

void rd_kafka_msgq_dump(FILE *fp, const char *what, rd_kafka_msgq_t *rkmq);

rd_kafka_msg_t *ut_rd_kafka_msg_new(size_t msgsize);
void ut_rd_kafka_msgq_purge(rd_kafka_msgq_t *rkmq);
int unittest_msg(void);

#endif /* _RDKAFKA_MSG_H_ */
