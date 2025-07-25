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

#include "rd.h"
#include "rdkafka_int.h"
#include "rdkafka_msg.h"
#include "rdkafka_msgset.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_header.h"
#include "rdkafka_lz4.h"

#if WITH_ZSTD
#include "rdkafka_zstd.h"
#endif

#include "snappy.h"
#include "rdvarint.h"
#include "crc32c.h"


/** @brief The maxium ProduceRequestion ApiVersion supported by librdkafka */
static const int16_t rd_kafka_ProduceRequest_max_version = 7;


typedef struct rd_kafka_msgset_writer_s {
        rd_kafka_buf_t *msetw_rkbuf; /* Backing store buffer (refcounted)*/

        int16_t msetw_ApiVersion; /* ProduceRequest ApiVersion */
        int msetw_MsgVersion;     /* MsgVersion to construct */
        int msetw_features;       /* Protocol features to use */
        rd_kafka_compression_t msetw_compression; /**< Compression type */
        int msetw_msgcntmax;         /* Max number of messages to send
                                      * in a batch. */
        size_t msetw_messages_len;   /* Total size of Messages, with Message
                                      * framing but without
                                      * MessageSet header */
        size_t msetw_messages_kvlen; /* Total size of Message keys
                                      * and values */

        size_t msetw_MessageSetSize;    /* Current MessageSetSize value */
        size_t msetw_of_MessageSetSize; /* offset of MessageSetSize */
        size_t msetw_of_start;          /* offset of MessageSet */

        int msetw_relative_offsets; /* Bool: use relative offsets */

        /* For MessageSet v2 */
        int msetw_Attributes;       /* MessageSet Attributes */
        int64_t msetw_MaxTimestamp; /* Maximum timestamp in batch */
        size_t msetw_of_CRC;        /* offset of MessageSet.CRC */

        rd_kafka_msgbatch_t *msetw_batch; /**< Convenience pointer to
                                           *   rkbuf_u.Produce.batch */

        /* First message information */
        struct {
                size_t of; /* rkbuf's first message position */
                int64_t timestamp;
        } msetw_firstmsg;

        rd_kafka_pid_t msetw_pid;      /**< Idempotent producer's
                                        *   current Producer Id */
        rd_kafka_broker_t *msetw_rkb;  /* @warning Not a refcounted
                                        *          reference! */
        rd_kafka_toppar_t *msetw_rktp; /* @warning Not a refcounted
                                        *          reference! */
        rd_kafka_msgq_t *msetw_msgq;   /**< Input message queue */
} rd_kafka_msgset_writer_t;



/**
 * @brief Select ApiVersion and MsgVersion to use based on broker's
 *        feature compatibility.
 *
 * @returns -1 if a MsgVersion (or ApiVersion) could not be selected, else 0.
 * @locality broker thread
 */
static RD_INLINE int
rd_kafka_msgset_writer_select_MsgVersion(rd_kafka_msgset_writer_t *msetw) {
        rd_kafka_broker_t *rkb       = msetw->msetw_rkb;
        rd_kafka_toppar_t *rktp      = msetw->msetw_rktp;
        const int16_t max_ApiVersion = rd_kafka_ProduceRequest_max_version;
        int16_t min_ApiVersion       = 0;
        int feature;
        /* Map compression types to required feature and ApiVersion */
        static const struct {
                int feature;
                int16_t ApiVersion;
        } compr_req[RD_KAFKA_COMPRESSION_NUM] = {
                [RD_KAFKA_COMPRESSION_KLZ4] = {RD_KAFKA_FEATURE_KLZ4, 0},
#if WITH_ZSTD
                [RD_KAFKA_COMPRESSION_ZSTD] = {RD_KAFKA_FEATURE_ZSTD, 7},
#endif
        };

        if ((feature = rkb->rkb_features & RD_KAFKA_FEATURE_MSGVER2)) {
                min_ApiVersion          = 3;
                msetw->msetw_MsgVersion = 2;
                msetw->msetw_features |= feature;
        } else if ((feature = rkb->rkb_features & RD_KAFKA_FEATURE_MSGVER1)) {
                min_ApiVersion          = 2;
                msetw->msetw_MsgVersion = 1;
                msetw->msetw_features |= feature;
        } else {
                if ((feature =
                         rkb->rkb_features & RD_KAFKA_FEATURE_THROTTLETIME)) {
                        min_ApiVersion = 1;
                        msetw->msetw_features |= feature;
                } else
                        min_ApiVersion = 0;
                msetw->msetw_MsgVersion = 0;
        }

        msetw->msetw_compression = rktp->rktp_rkt->rkt_conf.compression_codec;

        /*
         * Check that the configured compression type is supported
         * by both client and broker, else disable compression.
         */
        if (msetw->msetw_compression &&
            (rd_kafka_broker_ApiVersion_supported(
                 rkb, RD_KAFKAP_Produce, 0,
                 compr_req[msetw->msetw_compression].ApiVersion, NULL) == -1 ||
             (compr_req[msetw->msetw_compression].feature &&
              !(msetw->msetw_rkb->rkb_features &
                compr_req[msetw->msetw_compression].feature)))) {
                if (unlikely(
                        rd_interval(&rkb->rkb_suppress.unsupported_compression,
                                    /* at most once per day */
                                    (rd_ts_t)86400 * 1000 * 1000, 0) > 0))
                        rd_rkb_log(
                            rkb, LOG_NOTICE, "COMPRESSION",
                            "%.*s [%" PRId32
                            "]: "
                            "Broker does not support compression "
                            "type %s: not compressing batch",
                            RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                            rktp->rktp_partition,
                            rd_kafka_compression2str(msetw->msetw_compression));
                else
                        rd_rkb_dbg(
                            rkb, MSG, "PRODUCE",
                            "%.*s [%" PRId32
                            "]: "
                            "Broker does not support compression "
                            "type %s: not compressing batch",
                            RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                            rktp->rktp_partition,
                            rd_kafka_compression2str(msetw->msetw_compression));

                msetw->msetw_compression = RD_KAFKA_COMPRESSION_NONE;
        } else {
                /* Broker supports this compression type. */
                msetw->msetw_features |=
                    compr_req[msetw->msetw_compression].feature;

                if (min_ApiVersion <
                    compr_req[msetw->msetw_compression].ApiVersion)
                        min_ApiVersion =
                            compr_req[msetw->msetw_compression].ApiVersion;
        }

        /* MsgVersion specific setup. */
        switch (msetw->msetw_MsgVersion) {
        case 2:
                msetw->msetw_relative_offsets = 1; /* OffsetDelta */
                break;
        case 1:
                if (msetw->msetw_compression != RD_KAFKA_COMPRESSION_NONE)
                        msetw->msetw_relative_offsets = 1;
                break;
        }

        /* Set the highest ApiVersion supported by us and broker */
        msetw->msetw_ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_Produce, min_ApiVersion, max_ApiVersion, NULL);

        if (msetw->msetw_ApiVersion == -1) {
                rd_kafka_msg_t *rkm;
                /* This will only happen if the broker reports none, or
                 * no matching ProduceRequest versions, which should never
                 * happen. */
                rd_rkb_log(rkb, LOG_ERR, "PRODUCE",
                           "%.*s [%" PRId32
                           "]: "
                           "No viable ProduceRequest ApiVersions (v%d..%d) "
                           "supported by broker: unable to produce",
                           RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                           rktp->rktp_partition, min_ApiVersion,
                           max_ApiVersion);

                /* Back off and retry in 5s */
                rkm = rd_kafka_msgq_first(msetw->msetw_msgq);
                rd_assert(rkm);
                rkm->rkm_u.producer.ts_backoff = rd_clock() + (5 * 1000 * 1000);
                return -1;
        }

        /* It should not be possible to get a lower version than requested,
         * otherwise the logic in this function is buggy. */
        rd_assert(msetw->msetw_ApiVersion >= min_ApiVersion);

        return 0;
}


/**
 * @brief Allocate buffer for messageset writer based on a previously set
 *        up \p msetw.
 *
 * Allocate iovecs to hold all headers and messages,
 * and allocate enough space to allow copies of small messages.
 * The allocated size is the minimum of message.max.bytes
 * or queued_bytes + msgcntmax * msg_overhead
 */
static void rd_kafka_msgset_writer_alloc_buf(rd_kafka_msgset_writer_t *msetw) {
        rd_kafka_t *rk      = msetw->msetw_rkb->rkb_rk;
        size_t msg_overhead = 0;
        size_t hdrsize      = 0;
        size_t msgsetsize   = 0;
        size_t bufsize;

        rd_kafka_assert(NULL, !msetw->msetw_rkbuf);

        /* Calculate worst-case buffer size, produce header size,
         * message size, etc, this isn't critical but avoids unnecesary
         * extra allocations. The buffer will grow as needed if we get
         * this wrong.
         *
         * ProduceRequest headers go in one iovec:
         *  ProduceRequest v0..2:
         *    RequiredAcks + Timeout +
         *    [Topic + [Partition + MessageSetSize]]
         *
         *  ProduceRequest v3:
         *    TransactionalId + RequiredAcks + Timeout +
         *    [Topic + [Partition + MessageSetSize + MessageSet]]
         */

        /*
         * ProduceRequest header sizes
         */
        switch (msetw->msetw_ApiVersion) {
        case 7:
        case 6:
        case 5:
        case 4:
        case 3:
                /* Add TransactionalId */
                hdrsize += RD_KAFKAP_STR_SIZE(rk->rk_eos.transactional_id);
                /* FALLTHRU */
        case 0:
        case 1:
        case 2:
                hdrsize +=
                    /* RequiredAcks + Timeout + TopicCnt */
                    2 + 4 + 4 +
                    /* Topic */
                    RD_KAFKAP_STR_SIZE(msetw->msetw_rktp->rktp_rkt->rkt_topic) +
                    /* PartitionCnt + Partition + MessageSetSize */
                    4 + 4 + 4;
                msgsetsize += 4; /* MessageSetSize */
                break;

        default:
                RD_NOTREACHED();
        }

        /*
         * MsgVersion specific sizes:
         * - (Worst-case) Message overhead: message fields
         * - MessageSet header size
         */
        switch (msetw->msetw_MsgVersion) {
        case 0:
                /* MsgVer0 */
                msg_overhead = RD_KAFKAP_MESSAGE_V0_OVERHEAD;
                break;
        case 1:
                /* MsgVer1 */
                msg_overhead = RD_KAFKAP_MESSAGE_V1_OVERHEAD;
                break;

        case 2:
                /* MsgVer2 uses varints, we calculate for the worst-case. */
                msg_overhead += RD_KAFKAP_MESSAGE_V2_MAX_OVERHEAD;

                /* MessageSet header fields */
                msgsetsize += 8 /* BaseOffset */ + 4 /* Length */ +
                              4 /* PartitionLeaderEpoch */ +
                              1 /* Magic (MsgVersion) */ +
                              4 /* CRC (CRC32C) */ + 2 /* Attributes */ +
                              4 /* LastOffsetDelta */ + 8 /* BaseTimestamp */ +
                              8 /* MaxTimestamp */ + 8 /* ProducerId */ +
                              2 /* ProducerEpoch */ + 4 /* BaseSequence */ +
                              4 /* RecordCount */;
                break;

        default:
                RD_NOTREACHED();
        }

        /*
         * Calculate total buffer size to allocate
         */
        bufsize = hdrsize + msgsetsize;

        /* If copying for small payloads is enabled, allocate enough
         * space for each message to be copied based on this limit.
         */
        if (rk->rk_conf.msg_copy_max_size > 0) {
                size_t queued_bytes = rd_kafka_msgq_size(msetw->msetw_msgq);
                bufsize +=
                    RD_MIN(queued_bytes, (size_t)rk->rk_conf.msg_copy_max_size *
                                             msetw->msetw_msgcntmax);
        }

        /* Add estimed per-message overhead */
        bufsize += msg_overhead * msetw->msetw_msgcntmax;

        /* Cap allocation at message.max.bytes */
        if (bufsize > (size_t)rk->rk_conf.max_msg_size)
                bufsize = (size_t)rk->rk_conf.max_msg_size;

        /*
         * Allocate iovecs to hold all headers and messages,
         * and allocate auxilliery space for message headers, etc.
         */
        msetw->msetw_rkbuf =
            rd_kafka_buf_new_request(msetw->msetw_rkb, RD_KAFKAP_Produce,
                                     msetw->msetw_msgcntmax / 2 + 10, bufsize);

        rd_kafka_buf_ApiVersion_set(msetw->msetw_rkbuf, msetw->msetw_ApiVersion,
                                    msetw->msetw_features);
}


/**
 * @brief Write the MessageSet header.
 * @remark Must only be called for MsgVersion 2
 */
static void rd_kafka_msgset_writer_write_MessageSet_v2_header(
    rd_kafka_msgset_writer_t *msetw) {
        rd_kafka_buf_t *rkbuf = msetw->msetw_rkbuf;

        rd_kafka_assert(NULL, msetw->msetw_ApiVersion >= 3);
        rd_kafka_assert(NULL, msetw->msetw_MsgVersion == 2);

        /* BaseOffset (also store the offset to the start of
         * the messageset header fields) */
        msetw->msetw_of_start = rd_kafka_buf_write_i64(rkbuf, 0);

        /* Length: updated later */
        rd_kafka_buf_write_i32(rkbuf, 0);

        /* PartitionLeaderEpoch (KIP-101) */
        rd_kafka_buf_write_i32(rkbuf, 0);

        /* Magic (MsgVersion) */
        rd_kafka_buf_write_i8(rkbuf, msetw->msetw_MsgVersion);

        /* CRC (CRC32C): updated later.
         * CRC needs to be done after the entire messageset+messages has
         * been constructed and the following header fields updated. :(
         * Save the offset for this position. so it can be udpated later. */
        msetw->msetw_of_CRC = rd_kafka_buf_write_i32(rkbuf, 0);

        /* Attributes: updated later */
        rd_kafka_buf_write_i16(rkbuf, 0);

        /* LastOffsetDelta: updated later */
        rd_kafka_buf_write_i32(rkbuf, 0);

        /* BaseTimestamp: updated later */
        rd_kafka_buf_write_i64(rkbuf, 0);

        /* MaxTimestamp: updated later */
        rd_kafka_buf_write_i64(rkbuf, 0);

        /* ProducerId */
        rd_kafka_buf_write_i64(rkbuf, msetw->msetw_pid.id);

        /* ProducerEpoch */
        rd_kafka_buf_write_i16(rkbuf, msetw->msetw_pid.epoch);

        /* BaseSequence: updated later in case of Idempotent Producer */
        rd_kafka_buf_write_i32(rkbuf, -1);

        /* RecordCount: udpated later */
        rd_kafka_buf_write_i32(rkbuf, 0);
}


/**
 * @brief Write ProduceRequest headers.
 *        When this function returns the msgset is ready for
 *        writing individual messages.
 *        msetw_MessageSetSize will have been set to the messageset header.
 */
static void
rd_kafka_msgset_writer_write_Produce_header(rd_kafka_msgset_writer_t *msetw) {

        rd_kafka_buf_t *rkbuf = msetw->msetw_rkbuf;
        rd_kafka_t *rk        = msetw->msetw_rkb->rkb_rk;
        rd_kafka_topic_t *rkt = msetw->msetw_rktp->rktp_rkt;

        /* V3: TransactionalId */
        if (msetw->msetw_ApiVersion >= 3)
                rd_kafka_buf_write_kstr(rkbuf, rk->rk_eos.transactional_id);

        /* RequiredAcks */
        rd_kafka_buf_write_i16(rkbuf, rkt->rkt_conf.required_acks);

        /* Timeout */
        rd_kafka_buf_write_i32(rkbuf, rkt->rkt_conf.request_timeout_ms);

        /* TopicArrayCnt */
        rd_kafka_buf_write_i32(rkbuf, 1);

        /* Insert topic */
        rd_kafka_buf_write_kstr(rkbuf, rkt->rkt_topic);

        /* PartitionArrayCnt */
        rd_kafka_buf_write_i32(rkbuf, 1);

        /* Partition */
        rd_kafka_buf_write_i32(rkbuf, msetw->msetw_rktp->rktp_partition);

        /* MessageSetSize: Will be finalized later*/
        msetw->msetw_of_MessageSetSize = rd_kafka_buf_write_i32(rkbuf, 0);

        if (msetw->msetw_MsgVersion == 2) {
                /* MessageSet v2 header */
                rd_kafka_msgset_writer_write_MessageSet_v2_header(msetw);
                msetw->msetw_MessageSetSize = RD_KAFKAP_MSGSET_V2_SIZE;
        } else {
                /* Older MessageSet */
                msetw->msetw_MessageSetSize = RD_KAFKAP_MSGSET_V0_SIZE;
        }
}


/**
 * @brief Initialize a ProduceRequest MessageSet writer for
 *        the given broker and partition.
 *
 *        A new buffer will be allocated to fit the pending messages in queue.
 *
 * @returns the number of messages to enqueue
 *
 * @remark This currently constructs the entire ProduceRequest, containing
 *         a single outer MessageSet for a single partition.
 *
 * @locality broker thread
 */
static int rd_kafka_msgset_writer_init(rd_kafka_msgset_writer_t *msetw,
                                       rd_kafka_broker_t *rkb,
                                       rd_kafka_toppar_t *rktp,
                                       rd_kafka_msgq_t *rkmq,
                                       rd_kafka_pid_t pid,
                                       uint64_t epoch_base_msgid) {
        int msgcnt = rd_kafka_msgq_len(rkmq);

        if (msgcnt == 0)
                return 0;

        memset(msetw, 0, sizeof(*msetw));

        msetw->msetw_rktp = rktp;
        msetw->msetw_rkb  = rkb;
        msetw->msetw_msgq = rkmq;
        msetw->msetw_pid  = pid;

        /* Max number of messages to send in a batch,
         * limited by current queue size or configured batch size,
         * whichever is lower. */
        msetw->msetw_msgcntmax =
            RD_MIN(msgcnt, rkb->rkb_rk->rk_conf.batch_num_messages);
        rd_dassert(msetw->msetw_msgcntmax > 0);

        /* Select MsgVersion to use */
        if (rd_kafka_msgset_writer_select_MsgVersion(msetw) == -1)
                return -1;

        /* Allocate backing buffer */
        rd_kafka_msgset_writer_alloc_buf(msetw);

        /* Construct first part of Produce header + MessageSet header */
        rd_kafka_msgset_writer_write_Produce_header(msetw);

        /* The current buffer position is now where the first message
         * is located.
         * Record the current buffer position so it can be rewound later
         * in case of compression. */
        msetw->msetw_firstmsg.of =
            rd_buf_write_pos(&msetw->msetw_rkbuf->rkbuf_buf);

        rd_kafka_msgbatch_init(&msetw->msetw_rkbuf->rkbuf_u.Produce.batch, rktp,
                               pid, epoch_base_msgid);
        msetw->msetw_batch = &msetw->msetw_rkbuf->rkbuf_u.Produce.batch;

        return msetw->msetw_msgcntmax;
}



/**
 * @brief Copy or link message payload to buffer.
 */
static RD_INLINE void
rd_kafka_msgset_writer_write_msg_payload(rd_kafka_msgset_writer_t *msetw,
                                         const rd_kafka_msg_t *rkm,
                                         void (*free_cb)(void *)) {
        const rd_kafka_t *rk  = msetw->msetw_rkb->rkb_rk;
        rd_kafka_buf_t *rkbuf = msetw->msetw_rkbuf;

        /* If payload is below the copy limit and there is still
         * room in the buffer we'll copy the payload to the buffer,
         * otherwise we push a reference to the memory. */
        if (rkm->rkm_len <= (size_t)rk->rk_conf.msg_copy_max_size &&
            rd_buf_write_remains(&rkbuf->rkbuf_buf) > rkm->rkm_len) {
                rd_kafka_buf_write(rkbuf, rkm->rkm_payload, rkm->rkm_len);
                if (free_cb)
                        free_cb(rkm->rkm_payload);
        } else
                rd_kafka_buf_push(rkbuf, rkm->rkm_payload, rkm->rkm_len,
                                  free_cb);
}


/**
 * @brief Write message headers to buffer.
 *
 * @remark The enveloping HeaderCount varint must already have been written.
 * @returns the number of bytes written to msetw->msetw_rkbuf
 */
static size_t
rd_kafka_msgset_writer_write_msg_headers(rd_kafka_msgset_writer_t *msetw,
                                         const rd_kafka_headers_t *hdrs) {
        rd_kafka_buf_t *rkbuf = msetw->msetw_rkbuf;
        const rd_kafka_header_t *hdr;
        int i;
        size_t start_pos = rd_buf_write_pos(&rkbuf->rkbuf_buf);
        size_t written;

        RD_LIST_FOREACH(hdr, &hdrs->rkhdrs_list, i) {
                rd_kafka_buf_write_varint(rkbuf, hdr->rkhdr_name_size);
                rd_kafka_buf_write(rkbuf, hdr->rkhdr_name,
                                   hdr->rkhdr_name_size);
                rd_kafka_buf_write_varint(
                    rkbuf,
                    hdr->rkhdr_value ? (int64_t)hdr->rkhdr_value_size : -1);
                rd_kafka_buf_write(rkbuf, hdr->rkhdr_value,
                                   hdr->rkhdr_value_size);
        }

        written = rd_buf_write_pos(&rkbuf->rkbuf_buf) - start_pos;
        rd_dassert(written == hdrs->rkhdrs_ser_size);

        return written;
}



/**
 * @brief Write message to messageset buffer with MsgVersion 0 or 1.
 * @returns the number of bytes written.
 */
static size_t
rd_kafka_msgset_writer_write_msg_v0_1(rd_kafka_msgset_writer_t *msetw,
                                      rd_kafka_msg_t *rkm,
                                      int64_t Offset,
                                      int8_t MsgAttributes,
                                      void (*free_cb)(void *)) {
        rd_kafka_buf_t *rkbuf = msetw->msetw_rkbuf;
        size_t MessageSize;
        size_t of_Crc;

        /*
         * MessageSet's (v0 and v1) per-Message header.
         */

        /* Offset (only relevant for compressed messages on MsgVersion v1) */
        rd_kafka_buf_write_i64(rkbuf, Offset);

        /* MessageSize */
        MessageSize = 4 + 1 + 1 + /* Crc+MagicByte+Attributes */
                      4 /* KeyLength */ + rkm->rkm_key_len +
                      4 /* ValueLength */ + rkm->rkm_len;

        if (msetw->msetw_MsgVersion == 1)
                MessageSize += 8; /* Timestamp i64 */

        rd_kafka_buf_write_i32(rkbuf, (int32_t)MessageSize);

        /*
         * Message
         */
        /* Crc: will be updated later */
        of_Crc = rd_kafka_buf_write_i32(rkbuf, 0);

        /* Start Crc calculation of all buf writes. */
        rd_kafka_buf_crc_init(rkbuf);

        /* MagicByte */
        rd_kafka_buf_write_i8(rkbuf, msetw->msetw_MsgVersion);

        /* Attributes */
        rd_kafka_buf_write_i8(rkbuf, MsgAttributes);

        /* V1: Timestamp */
        if (msetw->msetw_MsgVersion == 1)
                rd_kafka_buf_write_i64(rkbuf, rkm->rkm_timestamp);

        /* Message Key */
        rd_kafka_buf_write_bytes(rkbuf, rkm->rkm_key, rkm->rkm_key_len);

        /* Write or copy Value/payload */
        if (rkm->rkm_payload) {
                rd_kafka_buf_write_i32(rkbuf, (int32_t)rkm->rkm_len);
                rd_kafka_msgset_writer_write_msg_payload(msetw, rkm, free_cb);
        } else
                rd_kafka_buf_write_i32(rkbuf, RD_KAFKAP_BYTES_LEN_NULL);

        /* Finalize Crc */
        rd_kafka_buf_update_u32(rkbuf, of_Crc,
                                rd_kafka_buf_crc_finalize(rkbuf));


        /* Return written message size */
        return 8 /*Offset*/ + 4 /*MessageSize*/ + MessageSize;
}

/**
 * @brief Write message to messageset buffer with MsgVersion 2.
 * @returns the number of bytes written.
 */
static size_t
rd_kafka_msgset_writer_write_msg_v2(rd_kafka_msgset_writer_t *msetw,
                                    rd_kafka_msg_t *rkm,
                                    int64_t Offset,
                                    int8_t MsgAttributes,
                                    void (*free_cb)(void *)) {
        rd_kafka_buf_t *rkbuf = msetw->msetw_rkbuf;
        size_t MessageSize    = 0;
        char varint_Length[RD_UVARINT_ENC_SIZEOF(int32_t)];
        char varint_TimestampDelta[RD_UVARINT_ENC_SIZEOF(int64_t)];
        char varint_OffsetDelta[RD_UVARINT_ENC_SIZEOF(int64_t)];
        char varint_KeyLen[RD_UVARINT_ENC_SIZEOF(int32_t)];
        char varint_ValueLen[RD_UVARINT_ENC_SIZEOF(int32_t)];
        char varint_HeaderCount[RD_UVARINT_ENC_SIZEOF(int32_t)];
        size_t sz_Length;
        size_t sz_TimestampDelta;
        size_t sz_OffsetDelta;
        size_t sz_KeyLen;
        size_t sz_ValueLen;
        size_t sz_HeaderCount;
        int HeaderCount   = 0;
        size_t HeaderSize = 0;

        if (rkm->rkm_headers) {
                HeaderCount = rkm->rkm_headers->rkhdrs_list.rl_cnt;
                HeaderSize  = rkm->rkm_headers->rkhdrs_ser_size;
        }

        /* All varints, except for Length, needs to be pre-built
         * so that the Length field can be set correctly and thus have
         * correct varint encoded width. */

        sz_TimestampDelta = rd_uvarint_enc_i64(
            varint_TimestampDelta, sizeof(varint_TimestampDelta),
            rkm->rkm_timestamp - msetw->msetw_firstmsg.timestamp);
        sz_OffsetDelta = rd_uvarint_enc_i64(varint_OffsetDelta,
                                            sizeof(varint_OffsetDelta), Offset);
        sz_KeyLen   = rd_uvarint_enc_i32(varint_KeyLen, sizeof(varint_KeyLen),
                                       rkm->rkm_key
                                           ? (int32_t)rkm->rkm_key_len
                                           : (int32_t)RD_KAFKAP_BYTES_LEN_NULL);
        sz_ValueLen = rd_uvarint_enc_i32(
            varint_ValueLen, sizeof(varint_ValueLen),
            rkm->rkm_payload ? (int32_t)rkm->rkm_len
                             : (int32_t)RD_KAFKAP_BYTES_LEN_NULL);
        sz_HeaderCount =
            rd_uvarint_enc_i32(varint_HeaderCount, sizeof(varint_HeaderCount),
                               (int32_t)HeaderCount);

        /* Calculate MessageSize without length of Length (added later)
         * to store it in Length. */
        MessageSize = 1 /* MsgAttributes */ + sz_TimestampDelta +
                      sz_OffsetDelta + sz_KeyLen + rkm->rkm_key_len +
                      sz_ValueLen + rkm->rkm_len + sz_HeaderCount + HeaderSize;

        /* Length */
        sz_Length = rd_uvarint_enc_i64(varint_Length, sizeof(varint_Length),
                                       MessageSize);
        rd_kafka_buf_write(rkbuf, varint_Length, sz_Length);
        MessageSize += sz_Length;

        /* Attributes: The MsgAttributes argument is losely based on MsgVer0
         *             which don't apply for MsgVer2 */
        rd_kafka_buf_write_i8(rkbuf, 0);

        /* TimestampDelta */
        rd_kafka_buf_write(rkbuf, varint_TimestampDelta, sz_TimestampDelta);

        /* OffsetDelta */
        rd_kafka_buf_write(rkbuf, varint_OffsetDelta, sz_OffsetDelta);

        /* KeyLen */
        rd_kafka_buf_write(rkbuf, varint_KeyLen, sz_KeyLen);

        /* Key (if any) */
        if (rkm->rkm_key)
                rd_kafka_buf_write(rkbuf, rkm->rkm_key, rkm->rkm_key_len);

        /* ValueLen */
        rd_kafka_buf_write(rkbuf, varint_ValueLen, sz_ValueLen);

        /* Write or copy Value/payload */
        if (rkm->rkm_payload)
                rd_kafka_msgset_writer_write_msg_payload(msetw, rkm, free_cb);

        /* HeaderCount */
        rd_kafka_buf_write(rkbuf, varint_HeaderCount, sz_HeaderCount);

        /* Headers array */
        if (rkm->rkm_headers)
                rd_kafka_msgset_writer_write_msg_headers(msetw,
                                                         rkm->rkm_headers);

        /* Return written message size */
        return MessageSize;
}


/**
 * @brief Write message to messageset buffer.
 * @returns the number of bytes written.
 */
static size_t rd_kafka_msgset_writer_write_msg(rd_kafka_msgset_writer_t *msetw,
                                               rd_kafka_msg_t *rkm,
                                               int64_t Offset,
                                               int8_t MsgAttributes,
                                               void (*free_cb)(void *)) {
        size_t outlen;
        size_t (*writer[])(rd_kafka_msgset_writer_t *, rd_kafka_msg_t *,
                           int64_t, int8_t, void (*)(void *)) = {
            [0] = rd_kafka_msgset_writer_write_msg_v0_1,
            [1] = rd_kafka_msgset_writer_write_msg_v0_1,
            [2] = rd_kafka_msgset_writer_write_msg_v2};
        size_t actual_written;
        size_t pre_pos;

        if (likely(rkm->rkm_timestamp))
                MsgAttributes |= RD_KAFKA_MSG_ATTR_CREATE_TIME;

        pre_pos = rd_buf_write_pos(&msetw->msetw_rkbuf->rkbuf_buf);

        outlen = writer[msetw->msetw_MsgVersion](msetw, rkm, Offset,
                                                 MsgAttributes, free_cb);

        actual_written =
            rd_buf_write_pos(&msetw->msetw_rkbuf->rkbuf_buf) - pre_pos;
        rd_assert(outlen <=
                  rd_kafka_msg_wire_size(rkm, msetw->msetw_MsgVersion));
        rd_assert(outlen == actual_written);

        return outlen;
}

/**
 * @brief Write as many messages from the given message queue to
 *        the messageset.
 *
 *        May not write any messages.
 *
 * @returns 1 on success or 0 on error.
 */
static int rd_kafka_msgset_writer_write_msgq(rd_kafka_msgset_writer_t *msetw,
                                             rd_kafka_msgq_t *rkmq) {
        rd_kafka_toppar_t *rktp = msetw->msetw_rktp;
        rd_kafka_broker_t *rkb  = msetw->msetw_rkb;
        size_t len              = rd_buf_len(&msetw->msetw_rkbuf->rkbuf_buf);
        size_t max_msg_size =
            RD_MIN((size_t)msetw->msetw_rkb->rkb_rk->rk_conf.max_msg_size,
                   (size_t)msetw->msetw_rkb->rkb_rk->rk_conf.batch_size);
        rd_ts_t int_latency_base;
        rd_ts_t MaxTimestamp = 0;
        rd_kafka_msg_t *rkm;
        int msgcnt        = 0;
        const rd_ts_t now = rd_clock();

        /* Internal latency calculation base.
         * Uses rkm_ts_timeout which is enqueue time + timeout */
        int_latency_base =
            now + ((rd_ts_t)rktp->rktp_rkt->rkt_conf.message_timeout_ms * 1000);

        /* Acquire BaseTimestamp from first message. */
        rkm = TAILQ_FIRST(&rkmq->rkmq_msgs);
        rd_kafka_assert(NULL, rkm);
        msetw->msetw_firstmsg.timestamp = rkm->rkm_timestamp;

        rd_kafka_msgbatch_set_first_msg(msetw->msetw_batch, rkm);

        /*
         * Write as many messages as possible until buffer is full
         * or limit reached.
         */
        do {
                if (unlikely(msetw->msetw_batch->last_msgid &&
                             msetw->msetw_batch->last_msgid <
                                 rkm->rkm_u.producer.msgid)) {
                        rd_rkb_dbg(rkb, MSG, "PRODUCE",
                                   "%.*s [%" PRId32
                                   "]: "
                                   "Reconstructed MessageSet "
                                   "(%d message(s), %" PRIusz
                                   " bytes, "
                                   "MsgIds %" PRIu64 "..%" PRIu64 ")",
                                   RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                                   rktp->rktp_partition, msgcnt, len,
                                   msetw->msetw_batch->first_msgid,
                                   msetw->msetw_batch->last_msgid);
                        break;
                }

                /* Check if there is enough space in the current messageset
                 * to add this message.
                 * Since calculating the total size of a request at produce()
                 * time is tricky (we don't know the protocol version or
                 * MsgVersion that will be used), we allow a messageset to
                 * overshoot the message.max.bytes limit by one message to
                 * avoid getting stuck here.
                 * The actual messageset size is enforced by the broker. */
                if (unlikely(
                        msgcnt == msetw->msetw_msgcntmax ||
                        (msgcnt > 0 && len + rd_kafka_msg_wire_size(
                                                 rkm, msetw->msetw_MsgVersion) >
                                           max_msg_size))) {
                        rd_rkb_dbg(rkb, MSG, "PRODUCE",
                                   "%.*s [%" PRId32
                                   "]: "
                                   "No more space in current MessageSet "
                                   "(%i message(s), %" PRIusz " bytes)",
                                   RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                                   rktp->rktp_partition, msgcnt, len);
                        break;
                }

                if (unlikely(rkm->rkm_u.producer.ts_backoff > now)) {
                        /* Stop accumulation when we've reached
                         * a message with a retry backoff in the future */
                        break;
                }

                /* Move message to buffer's queue */
                rd_kafka_msgq_deq(rkmq, rkm, 1);
                rd_kafka_msgq_enq(&msetw->msetw_batch->msgq, rkm);

                msetw->msetw_messages_kvlen += rkm->rkm_len + rkm->rkm_key_len;

                /* Add internal latency metrics */
                rd_avg_add(&rkb->rkb_avg_int_latency,
                           int_latency_base - rkm->rkm_ts_timeout);

                /* MessageSet v2's .MaxTimestamp field */
                if (unlikely(MaxTimestamp < rkm->rkm_timestamp))
                        MaxTimestamp = rkm->rkm_timestamp;

                /* Write message to buffer */
                len += rd_kafka_msgset_writer_write_msg(msetw, rkm, msgcnt, 0,
                                                        NULL);

                msgcnt++;

        } while ((rkm = TAILQ_FIRST(&rkmq->rkmq_msgs)));

        msetw->msetw_MaxTimestamp = MaxTimestamp;

        /* Idempotent Producer:
         * When reconstructing a batch to retry make sure
         * the original message sequence span matches identically
         * or we can't guarantee exactly-once delivery.
         * If this check fails we raise a fatal error since
         * it is unrecoverable and most likely caused by a bug
         * in the client implementation.
         * This should not be considered an abortable error for
         * the transactional producer. */
        if (msgcnt > 0 && msetw->msetw_batch->last_msgid) {
                rd_kafka_msg_t *lastmsg;

                lastmsg = rd_kafka_msgq_last(&msetw->msetw_batch->msgq);
                rd_assert(lastmsg);

                if (unlikely(lastmsg->rkm_u.producer.msgid !=
                             msetw->msetw_batch->last_msgid)) {
                        rd_kafka_set_fatal_error(
                            rkb->rkb_rk, RD_KAFKA_RESP_ERR__INCONSISTENT,
                            "Unable to reconstruct MessageSet "
                            "(currently with %d message(s)) "
                            "with msgid range %" PRIu64 "..%" PRIu64
                            ": "
                            "last message added has msgid %" PRIu64
                            ": "
                            "unable to guarantee consistency",
                            msgcnt, msetw->msetw_batch->first_msgid,
                            msetw->msetw_batch->last_msgid,
                            lastmsg->rkm_u.producer.msgid);
                        return 0;
                }
        }
        return 1;
}


#if WITH_ZLIB
/**
 * @brief Compress messageset using gzip/zlib
 */
static int rd_kafka_msgset_writer_compress_gzip(rd_kafka_msgset_writer_t *msetw,
                                                rd_slice_t *slice,
                                                struct iovec *ciov) {

        rd_kafka_broker_t *rkb  = msetw->msetw_rkb;
        rd_kafka_toppar_t *rktp = msetw->msetw_rktp;
        z_stream strm;
        size_t len = rd_slice_remains(slice);
        const void *p;
        size_t rlen;
        int r;
        int comp_level =
            msetw->msetw_rktp->rktp_rkt->rkt_conf.compression_level;

        memset(&strm, 0, sizeof(strm));
        r = deflateInit2(&strm, comp_level, Z_DEFLATED, 15 + 16, 8,
                         Z_DEFAULT_STRATEGY);
        if (r != Z_OK) {
                rd_rkb_log(rkb, LOG_ERR, "GZIP",
                           "Failed to initialize gzip for "
                           "compressing %" PRIusz
                           " bytes in "
                           "topic %.*s [%" PRId32
                           "]: %s (%i): "
                           "sending uncompressed",
                           len, RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                           rktp->rktp_partition, strm.msg ? strm.msg : "", r);
                return -1;
        }

        /* Calculate maximum compressed size and
         * allocate an output buffer accordingly, being
         * prefixed with the Message header. */
        ciov->iov_len  = deflateBound(&strm, (uLong)rd_slice_remains(slice));
        ciov->iov_base = rd_malloc(ciov->iov_len);

        strm.next_out  = (void *)ciov->iov_base;
        strm.avail_out = (uInt)ciov->iov_len;

        /* Iterate through each segment and compress it. */
        while ((rlen = rd_slice_reader(slice, &p))) {

                strm.next_in  = (void *)p;
                strm.avail_in = (uInt)rlen;

                /* Compress message */
                if ((r = deflate(&strm, Z_NO_FLUSH)) != Z_OK) {
                        rd_rkb_log(rkb, LOG_ERR, "GZIP",
                                   "Failed to gzip-compress "
                                   "%" PRIusz " bytes (%" PRIusz
                                   " total) for "
                                   "topic %.*s [%" PRId32
                                   "]: "
                                   "%s (%i): "
                                   "sending uncompressed",
                                   rlen, len,
                                   RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                                   rktp->rktp_partition,
                                   strm.msg ? strm.msg : "", r);
                        deflateEnd(&strm);
                        rd_free(ciov->iov_base);
                        return -1;
                }

                rd_kafka_assert(rkb->rkb_rk, strm.avail_in == 0);
        }

        /* Finish the compression */
        if ((r = deflate(&strm, Z_FINISH)) != Z_STREAM_END) {
                rd_rkb_log(rkb, LOG_ERR, "GZIP",
                           "Failed to finish gzip compression "
                           " of %" PRIusz
                           " bytes for "
                           "topic %.*s [%" PRId32
                           "]: "
                           "%s (%i): "
                           "sending uncompressed",
                           len, RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                           rktp->rktp_partition, strm.msg ? strm.msg : "", r);
                deflateEnd(&strm);
                rd_free(ciov->iov_base);
                return -1;
        }

        ciov->iov_len = strm.total_out;

        /* Deinitialize compression */
        deflateEnd(&strm);

        return 0;
}
#endif


#if WITH_SNAPPY
/**
 * @brief Compress messageset using Snappy
 */
static int
rd_kafka_msgset_writer_compress_snappy(rd_kafka_msgset_writer_t *msetw,
                                       rd_slice_t *slice,
                                       struct iovec *ciov) {
        rd_kafka_broker_t *rkb  = msetw->msetw_rkb;
        rd_kafka_toppar_t *rktp = msetw->msetw_rktp;
        struct iovec *iov;
        size_t iov_max, iov_cnt;
        struct snappy_env senv;
        size_t len = rd_slice_remains(slice);
        int r;

        /* Initialize snappy compression environment */
        rd_kafka_snappy_init_env_sg(&senv, 1 /*iov enable*/);

        /* Calculate maximum compressed size and
         * allocate an output buffer accordingly. */
        ciov->iov_len  = rd_kafka_snappy_max_compressed_length(len);
        ciov->iov_base = rd_malloc(ciov->iov_len);

        iov_max = slice->buf->rbuf_segment_cnt;
        iov     = rd_alloca(sizeof(*iov) * iov_max);

        rd_slice_get_iov(slice, iov, &iov_cnt, iov_max, len);

        /* Compress each message */
        if ((r = rd_kafka_snappy_compress_iov(&senv, iov, iov_cnt, len,
                                              ciov)) != 0) {
                rd_rkb_log(rkb, LOG_ERR, "SNAPPY",
                           "Failed to snappy-compress "
                           "%" PRIusz
                           " bytes for "
                           "topic %.*s [%" PRId32
                           "]: %s: "
                           "sending uncompressed",
                           len, RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                           rktp->rktp_partition, rd_strerror(-r));
                rd_free(ciov->iov_base);
                return -1;
        }

        /* rd_free snappy environment */
        rd_kafka_snappy_free_env(&senv);

        return 0;
}
#endif

/**
 * @brief Compress messageset using KLZ4F
 */
static int rd_kafka_msgset_writer_compress_lz4(rd_kafka_msgset_writer_t *msetw,
                                               rd_slice_t *slice,
                                               struct iovec *ciov) {
        rd_kafka_resp_err_t err;
        int comp_level =
            msetw->msetw_rktp->rktp_rkt->rkt_conf.compression_level;
        err = rd_kafka_lz4_compress(msetw->msetw_rkb,
                                    /* Correct or incorrect HC */
                                    msetw->msetw_MsgVersion >= 1 ? 1 : 0,
                                    comp_level, slice, &ciov->iov_base,
                                    &ciov->iov_len);
        return (err ? -1 : 0);
}

#if WITH_ZSTD
/**
 * @brief Compress messageset using ZSTD
 */
static int rd_kafka_msgset_writer_compress_zstd(rd_kafka_msgset_writer_t *msetw,
                                                rd_slice_t *slice,
                                                struct iovec *ciov) {
        rd_kafka_resp_err_t err;
        int comp_level =
            msetw->msetw_rktp->rktp_rkt->rkt_conf.compression_level;
        err = rd_kafka_zstd_compress(msetw->msetw_rkb, comp_level, slice,
                                     &ciov->iov_base, &ciov->iov_len);
        return (err ? -1 : 0);
}
#endif

/**
 * @brief Compress the message set.
 * @param outlenp in: total uncompressed messages size,
 *                out (on success): returns the compressed buffer size.
 * @returns 0 on success or if -1 if compression failed.
 * @remark Compression failures are not critical, we'll just send the
 *         the messageset uncompressed.
 */
static int rd_kafka_msgset_writer_compress(rd_kafka_msgset_writer_t *msetw,
                                           size_t *outlenp) {
        rd_buf_t *rbuf = &msetw->msetw_rkbuf->rkbuf_buf;
        rd_slice_t slice;
        size_t len        = *outlenp;
        struct iovec ciov = RD_ZERO_INIT; /* Compressed output buffer */
        int r             = -1;
        size_t outlen;

        rd_assert(rd_buf_len(rbuf) >= msetw->msetw_firstmsg.of + len);

        /* Create buffer slice from firstmsg and onwards */
        r = rd_slice_init(&slice, rbuf, msetw->msetw_firstmsg.of, len);
        rd_assert(r == 0 || !*"invalid firstmsg position");

        switch (msetw->msetw_compression) {
#if WITH_ZLIB
        case RD_KAFKA_COMPRESSION_GZIP:
                r = rd_kafka_msgset_writer_compress_gzip(msetw, &slice, &ciov);
                break;
#endif

#if WITH_SNAPPY
        case RD_KAFKA_COMPRESSION_SNAPPY:
                r = rd_kafka_msgset_writer_compress_snappy(msetw, &slice,
                                                           &ciov);
                break;
#endif

        case RD_KAFKA_COMPRESSION_KLZ4:
                r = rd_kafka_msgset_writer_compress_lz4(msetw, &slice, &ciov);
                break;

#if WITH_ZSTD
        case RD_KAFKA_COMPRESSION_ZSTD:
                r = rd_kafka_msgset_writer_compress_zstd(msetw, &slice, &ciov);
                break;
#endif

        default:
                rd_kafka_assert(NULL,
                                !*"notreached: unsupported compression.codec");
                break;
        }

        if (r == -1) /* Compression failed, send uncompressed */
                return -1;


        if (unlikely(ciov.iov_len > len)) {
                /* If the compressed data is larger than the uncompressed size
                 * then throw it away and send as uncompressed. */
                rd_free(ciov.iov_base);
                return -1;
        }

        /* Set compression codec in MessageSet.Attributes */
        msetw->msetw_Attributes |= msetw->msetw_compression;

        /* Rewind rkbuf to the pre-message checkpoint (firstmsg)
         * and replace the original message(s) with the compressed payload,
         * possibly with version dependent enveloping. */
        rd_buf_write_seek(rbuf, msetw->msetw_firstmsg.of);

        rd_kafka_assert(msetw->msetw_rkb->rkb_rk, ciov.iov_len < INT32_MAX);

        if (msetw->msetw_MsgVersion == 2) {
                /* MsgVersion 2 has no inner MessageSet header or wrapping
                 * for compressed messages, just the messages back-to-back,
                 * so we can push the compressed memory directly to the
                 * buffer without wrapping it. */
                rd_buf_push(rbuf, ciov.iov_base, ciov.iov_len, rd_free);
                outlen = ciov.iov_len;

        } else {
                /* Older MessageSets envelope/wrap the compressed MessageSet
                 * in an outer Message. */
                rd_kafka_msg_t rkm = {.rkm_len     = ciov.iov_len,
                                      .rkm_payload = ciov.iov_base,
                                      .rkm_timestamp =
                                          msetw->msetw_firstmsg.timestamp};
                outlen             = rd_kafka_msgset_writer_write_msg(
                    msetw, &rkm, 0, msetw->msetw_compression,
                    rd_free /*free for ciov.iov_base*/);
        }

        *outlenp = outlen;

        return 0;
}



/**
 * @brief Calculate MessageSet v2 CRC (CRC32C) when messageset is complete.
 */
static void
rd_kafka_msgset_writer_calc_crc_v2(rd_kafka_msgset_writer_t *msetw) {
        int32_t crc;
        rd_slice_t slice;
        int r;

        r = rd_slice_init(&slice, &msetw->msetw_rkbuf->rkbuf_buf,
                          msetw->msetw_of_CRC + 4,
                          rd_buf_write_pos(&msetw->msetw_rkbuf->rkbuf_buf) -
                              msetw->msetw_of_CRC - 4);
        rd_assert(!r && *"slice_init failed");

        /* CRC32C calculation */
        crc = rd_slice_crc32c(&slice);

        /* Update CRC at MessageSet v2 CRC offset */
        rd_kafka_buf_update_i32(msetw->msetw_rkbuf, msetw->msetw_of_CRC, crc);
}

/**
 * @brief Finalize MessageSet v2 header fields.
 */
static void rd_kafka_msgset_writer_finalize_MessageSet_v2_header(
    rd_kafka_msgset_writer_t *msetw) {
        rd_kafka_buf_t *rkbuf = msetw->msetw_rkbuf;
        int msgcnt            = rd_kafka_msgq_len(&rkbuf->rkbuf_batch.msgq);

        rd_kafka_assert(NULL, msgcnt > 0);
        rd_kafka_assert(NULL, msetw->msetw_ApiVersion >= 3);

        msetw->msetw_MessageSetSize =
            RD_KAFKAP_MSGSET_V2_SIZE + msetw->msetw_messages_len;

        /* MessageSet.Length is the same as
         * MessageSetSize minus field widths for FirstOffset+Length */
        rd_kafka_buf_update_i32(
            rkbuf, msetw->msetw_of_start + RD_KAFKAP_MSGSET_V2_OF_Length,
            (int32_t)msetw->msetw_MessageSetSize - (8 + 4));

        msetw->msetw_Attributes |= RD_KAFKA_MSG_ATTR_CREATE_TIME;

        if (rd_kafka_is_transactional(msetw->msetw_rkb->rkb_rk))
                msetw->msetw_Attributes |=
                    RD_KAFKA_MSGSET_V2_ATTR_TRANSACTIONAL;

        rd_kafka_buf_update_i16(
            rkbuf, msetw->msetw_of_start + RD_KAFKAP_MSGSET_V2_OF_Attributes,
            msetw->msetw_Attributes);

        rd_kafka_buf_update_i32(rkbuf,
                                msetw->msetw_of_start +
                                    RD_KAFKAP_MSGSET_V2_OF_LastOffsetDelta,
                                msgcnt - 1);

        rd_kafka_buf_update_i64(
            rkbuf, msetw->msetw_of_start + RD_KAFKAP_MSGSET_V2_OF_BaseTimestamp,
            msetw->msetw_firstmsg.timestamp);

        rd_kafka_buf_update_i64(
            rkbuf, msetw->msetw_of_start + RD_KAFKAP_MSGSET_V2_OF_MaxTimestamp,
            msetw->msetw_MaxTimestamp);

        rd_kafka_buf_update_i32(
            rkbuf, msetw->msetw_of_start + RD_KAFKAP_MSGSET_V2_OF_BaseSequence,
            msetw->msetw_batch->first_seq);

        rd_kafka_buf_update_i32(
            rkbuf, msetw->msetw_of_start + RD_KAFKAP_MSGSET_V2_OF_RecordCount,
            msgcnt);

        rd_kafka_msgset_writer_calc_crc_v2(msetw);
}



/**
 * @brief Finalize the MessageSet header, if applicable.
 */
static void
rd_kafka_msgset_writer_finalize_MessageSet(rd_kafka_msgset_writer_t *msetw) {
        rd_dassert(msetw->msetw_messages_len > 0);

        if (msetw->msetw_MsgVersion == 2)
                rd_kafka_msgset_writer_finalize_MessageSet_v2_header(msetw);
        else
                msetw->msetw_MessageSetSize =
                    RD_KAFKAP_MSGSET_V0_SIZE + msetw->msetw_messages_len;

        /* Update MessageSetSize */
        rd_kafka_buf_update_i32(msetw->msetw_rkbuf,
                                msetw->msetw_of_MessageSetSize,
                                (int32_t)msetw->msetw_MessageSetSize);
}


/**
 * @brief Finalize the messageset - call when no more messages are to be
 *        added to the messageset.
 *
 *        Will compress, update final values, CRCs, etc.
 *
 *        The messageset writer is destroyed and the buffer is returned
 *        and ready to be transmitted.
 *
 * @param MessagetSetSizep will be set to the finalized MessageSetSize
 *
 * @returns the buffer to transmit or NULL if there were no messages
 *          in messageset.
 */
static rd_kafka_buf_t *
rd_kafka_msgset_writer_finalize(rd_kafka_msgset_writer_t *msetw,
                                size_t *MessageSetSizep) {
        rd_kafka_buf_t *rkbuf   = msetw->msetw_rkbuf;
        rd_kafka_toppar_t *rktp = msetw->msetw_rktp;
        size_t len;
        int cnt;

        /* No messages added, bail out early. */
        if (unlikely((cnt = rd_kafka_msgq_len(&rkbuf->rkbuf_batch.msgq)) ==
                     0)) {
                rd_kafka_buf_destroy(rkbuf);
                return NULL;
        }

        /* Total size of messages */
        len = rd_buf_write_pos(&msetw->msetw_rkbuf->rkbuf_buf) -
              msetw->msetw_firstmsg.of;
        rd_assert(len > 0);
        rd_assert(len <= (size_t)rktp->rktp_rkt->rkt_rk->rk_conf.max_msg_size);

        rd_atomic64_add(&rktp->rktp_c.tx_msgs, cnt);
        rd_atomic64_add(&rktp->rktp_c.tx_msg_bytes,
                        msetw->msetw_messages_kvlen);

        /* Idempotent Producer:
         * Store request's PID for matching on response
         * if the instance PID has changed and thus made
         * the request obsolete. */
        msetw->msetw_rkbuf->rkbuf_u.Produce.batch.pid = msetw->msetw_pid;

        /* Compress the message set */
        if (msetw->msetw_compression) {
                if (rd_kafka_msgset_writer_compress(msetw, &len) == -1)
                        msetw->msetw_compression = 0;
        }

        msetw->msetw_messages_len = len;

        /* Finalize MessageSet header fields */
        rd_kafka_msgset_writer_finalize_MessageSet(msetw);

        /* Return final MessageSetSize */
        *MessageSetSizep = msetw->msetw_MessageSetSize;

        rd_rkb_dbg(msetw->msetw_rkb, MSG, "PRODUCE",
                   "%s [%" PRId32
                   "]: "
                   "Produce MessageSet with %i message(s) (%" PRIusz
                   " bytes, "
                   "ApiVersion %d, MsgVersion %d, MsgId %" PRIu64
                   ", "
                   "BaseSeq %" PRId32 ", %s, %s)",
                   rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition, cnt,
                   msetw->msetw_MessageSetSize, msetw->msetw_ApiVersion,
                   msetw->msetw_MsgVersion, msetw->msetw_batch->first_msgid,
                   msetw->msetw_batch->first_seq,
                   rd_kafka_pid2str(msetw->msetw_pid),
                   msetw->msetw_compression
                       ? rd_kafka_compression2str(msetw->msetw_compression)
                       : "uncompressed");

        rd_kafka_msgq_verify_order(rktp, &msetw->msetw_batch->msgq,
                                   msetw->msetw_batch->first_msgid, rd_false);

        rd_kafka_msgbatch_ready_produce(msetw->msetw_batch);

        return rkbuf;
}


/**
 * @brief Create ProduceRequest containing as many messages from
 *        the toppar's transmit queue as possible, limited by configuration,
 *        size, etc.
 *
 * @param rkb broker to create buffer for
 * @param rktp toppar to transmit messages for
 * @param MessagetSetSizep will be set to the final MessageSetSize
 *
 * @returns the buffer to transmit or NULL if there were no messages
 *          in messageset.
 *
 * @locality broker thread
 */
rd_kafka_buf_t *rd_kafka_msgset_create_ProduceRequest(rd_kafka_broker_t *rkb,
                                                      rd_kafka_toppar_t *rktp,
                                                      rd_kafka_msgq_t *rkmq,
                                                      const rd_kafka_pid_t pid,
                                                      uint64_t epoch_base_msgid,
                                                      size_t *MessageSetSizep) {

        rd_kafka_msgset_writer_t msetw;

        if (rd_kafka_msgset_writer_init(&msetw, rkb, rktp, rkmq, pid,
                                        epoch_base_msgid) <= 0)
                return NULL;

        if (!rd_kafka_msgset_writer_write_msgq(&msetw, msetw.msetw_msgq)) {
                /* Error while writing messages to MessageSet,
                 * move all messages back on the xmit queue. */
                rd_kafka_msgq_insert_msgq(
                    rkmq, &msetw.msetw_batch->msgq,
                    rktp->rktp_rkt->rkt_conf.msg_order_cmp);
        }

        return rd_kafka_msgset_writer_finalize(&msetw, MessageSetSizep);
}
