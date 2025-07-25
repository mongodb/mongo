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

/**
 * @name MessageSet reader interface
 *
 * Parses FetchResponse for Messages
 *
 *
 * @remark
 * The broker may send partial messages, when this happens we bail out
 * silently and keep the messages that we successfully parsed.
 *
 * "A Guide To The Kafka Protocol" states:
 *   "As an optimization the server is allowed to
 *    return a partial message at the end of the
 *    message set.
 *    Clients should handle this case."
 *
 * We're handling it by not passing the error upstream.
 * This is why most err_parse: goto labels (that are called from buf parsing
 * macros) suppress the error message and why log_decode_errors is off
 * unless PROTOCOL debugging is enabled.
 *
 * When a FetchResponse contains multiple partitions, each partition's
 * MessageSet may be partial, regardless of the other partitions.
 * To make sure the next partition can be parsed, each partition parse
 * uses its own sub-slice of only that partition's MessageSetSize length.
 */

#include "rd.h"
#include "rdunittest.h"
#include "rdavl.h"
#include "rdlist.h"
#include "rdkafka_int.h"
#include "rdkafka_msg.h"
#include "rdkafka_msgset.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_header.h"
#include "rdkafka_lz4.h"

#include "rdvarint.h"
#include "crc32c.h"

#if WITH_ZLIB
#include "rdgz.h"
#endif
#if WITH_SNAPPY
#include "snappy.h"
#endif
#if WITH_ZSTD
#include "rdkafka_zstd.h"
#endif


static RD_INLINE int64_t
rd_kafka_aborted_txns_pop_offset(rd_kafka_aborted_txns_t *aborted_txns,
                                 int64_t pid,
                                 int64_t max_offset);
static RD_INLINE int64_t
rd_kafka_aborted_txns_get_offset(const rd_kafka_aborted_txns_t *aborted_txns,
                                 int64_t pid);


struct msgset_v2_hdr {
        int64_t BaseOffset;
        int32_t Length;
        int32_t PartitionLeaderEpoch;
        int8_t MagicByte;
        int32_t Crc;
        int16_t Attributes;
        int32_t LastOffsetDelta;
        int64_t BaseTimestamp;
        int64_t MaxTimestamp;
        int64_t PID;
        int16_t ProducerEpoch;
        int32_t BaseSequence;
        int32_t RecordCount;
};


/**
 * @struct rd_kafka_aborted_txn_start_offsets_t
 *
 * @brief A sorted list of aborted transaction start offsets
 * (ascending) for a PID, and an offset into that list.
 */
typedef struct rd_kafka_aborted_txn_start_offsets_s {
        rd_avl_node_t avl_node;
        int64_t pid;
        int offsets_idx;
        rd_list_t offsets;
} rd_kafka_aborted_txn_start_offsets_t;


typedef struct rd_kafka_msgset_reader_s {
        rd_kafka_buf_t *msetr_rkbuf; /**< Response read buffer */

        int msetr_relative_offsets; /**< Bool: using relative offsets */

        /**< Outer/wrapper Message fields. */
        struct {
                int64_t offset; /**< Relative_offsets: outer message's
                                 *   Offset (last offset) */
                rd_kafka_timestamp_type_t tstype; /**< Compressed
                                                   *   MessageSet's
                                                   *   timestamp type. */
                int64_t timestamp;                /**< ... timestamp*/
        } msetr_outer;

        struct msgset_v2_hdr *msetr_v2_hdr; /**< MessageSet v2 header */

        /*
         * Aborted Transaction Start Offsets. These are arranged in a map
         * (ABORTED_TXN_OFFSETS), with PID as the key and value as follows:
         *  - OFFSETS:  sorted list of aborted transaction start offsets
         * (ascending)
         *  - IDX:      an index into OFFSETS list, initialized to 0.
         *
         * The logic for processing fetched data is as follows (note: this is
         * different from the Java client):
         *
         * 1. If the message is a transaction control message and the status is
         * ABORT then increment ABORTED_TXN_OFFSETS(PID).IDX. note: sanity check
         * that OFFSETS[ABORTED_TXN_OFFSETS(PID).IDX] is less than the current
         * offset before incrementing. If the status is COMMIT, do nothing.
         *
         * 2. If the message is a normal message, find the corresponding OFFSETS
         * list in ABORTED_TXN_OFFSETS. If it doesn't exist, then keep the
         * message. If the PID does exist, compare ABORTED_TXN_OFFSETS(PID).IDX
         * with len(OFFSETS). If it's >= then the message should be kept. If
         * not, compare the message offset with
         * OFFSETS[ABORTED_TXN_OFFSETS(PID).IDX]. If it's greater than or equal
         * to this value, then the message should be ignored. If it's less than,
         * then the message should be kept.
         *
         * Note: A MessageSet comprises messages from at most one transaction,
         * so the logic in step 2 is done at the message set level.
         */
        rd_kafka_aborted_txns_t *msetr_aborted_txns;

        const struct rd_kafka_toppar_ver *msetr_tver; /**< Toppar op version of
                                                       *   request. */

        int32_t msetr_broker_id;       /**< Broker id (of msetr_rkb) */
        rd_kafka_broker_t *msetr_rkb;  /* @warning Not a refcounted
                                        *          reference! */
        rd_kafka_toppar_t *msetr_rktp; /* @warning Not a refcounted
                                        *          reference! */

        int msetr_msgcnt;            /**< Number of messages in rkq */
        int64_t msetr_msg_bytes;     /**< Number of bytes in rkq */
        rd_kafka_q_t msetr_rkq;      /**< Temp Message and error queue */
        rd_kafka_q_t *msetr_par_rkq; /**< Parent message and error queue,
                                      *   the temp msetr_rkq will be moved
                                      *   to this queue when parsing
                                      *   is done.
                                      *   Refcount is not increased. */

        int64_t msetr_next_offset; /**< Next offset to fetch after
                                    *   this reader run is done.
                                    *   Optional: only used for special
                                    *   cases where the per-message offset
                                    *   can't be relied on for next
                                    *   fetch offset, such as with
                                    *   compacted topics. */

        int msetr_ctrl_cnt; /**< Number of control messages
                             *   or MessageSets received. */

        int msetr_aborted_cnt; /**< Number of aborted MessageSets
                                *   encountered. */

        const char *msetr_srcname; /**< Optional message source string,
                                    *   used in debug logging to
                                    *   indicate messages were
                                    *   from an inner compressed
                                    *   message set.
                                    *   Not freed (use const memory).
                                    *   Add trailing space. */

        rd_kafka_compression_t msetr_compression; /**< Compression codec */
} rd_kafka_msgset_reader_t;



/* Forward declarations */
static rd_kafka_resp_err_t
rd_kafka_msgset_reader_run(rd_kafka_msgset_reader_t *msetr);
static rd_kafka_resp_err_t
rd_kafka_msgset_reader_msgs_v2(rd_kafka_msgset_reader_t *msetr);


/**
 * @brief Set up a MessageSet reader but don't start reading messages.
 */
static void rd_kafka_msgset_reader_init(rd_kafka_msgset_reader_t *msetr,
                                        rd_kafka_buf_t *rkbuf,
                                        rd_kafka_toppar_t *rktp,
                                        const struct rd_kafka_toppar_ver *tver,
                                        rd_kafka_aborted_txns_t *aborted_txns,
                                        rd_kafka_q_t *par_rkq) {

        memset(msetr, 0, sizeof(*msetr));

        msetr->msetr_rkb          = rkbuf->rkbuf_rkb;
        msetr->msetr_broker_id    = rd_kafka_broker_id(msetr->msetr_rkb);
        msetr->msetr_rktp         = rktp;
        msetr->msetr_aborted_txns = aborted_txns;
        msetr->msetr_tver         = tver;
        msetr->msetr_rkbuf        = rkbuf;
        msetr->msetr_srcname      = "";

        rkbuf->rkbuf_uflow_mitigation = "truncated response from broker (ok)";

        /* All parsed messages are put on this temporary op
         * queue first and then moved in one go to the real op queue. */
        rd_kafka_q_init(&msetr->msetr_rkq, msetr->msetr_rkb->rkb_rk);

        /* Make sure enqueued ops get the correct serve/opaque reflecting the
         * original queue. */
        msetr->msetr_rkq.rkq_serve  = par_rkq->rkq_serve;
        msetr->msetr_rkq.rkq_opaque = par_rkq->rkq_opaque;

        /* Keep (non-refcounted) reference to parent queue for
         * moving the messages and events in msetr_rkq to when
         * parsing is done. */
        msetr->msetr_par_rkq = par_rkq;
}



/**
 * @brief Decompress MessageSet, pass the uncompressed MessageSet to
 *        the MessageSet reader.
 */
static rd_kafka_resp_err_t
rd_kafka_msgset_reader_decompress(rd_kafka_msgset_reader_t *msetr,
                                  int MsgVersion,
                                  int Attributes,
                                  int64_t Timestamp,
                                  int64_t Offset,
                                  const void *compressed,
                                  size_t compressed_size) {
        struct iovec iov        = {.iov_base = NULL, .iov_len = 0};
        rd_kafka_toppar_t *rktp = msetr->msetr_rktp;
        int codec = Attributes & RD_KAFKA_MSG_ATTR_COMPRESSION_MASK;
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        rd_kafka_buf_t *rkbufz;

        msetr->msetr_compression = codec;

        switch (codec) {
#if WITH_ZLIB
        case RD_KAFKA_COMPRESSION_GZIP: {
                uint64_t outlenx = 0;

                /* Decompress Message payload */
                iov.iov_base = rd_gz_decompress(compressed,
                                                (int)compressed_size, &outlenx);
                if (unlikely(!iov.iov_base)) {
                        rd_rkb_dbg(msetr->msetr_rkb, MSG, "GZIP",
                                   "Failed to decompress Gzip "
                                   "message at offset %" PRId64 " of %" PRIusz
                                   " bytes: "
                                   "ignoring message",
                                   Offset, compressed_size);
                        err = RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
                        goto err;
                }

                iov.iov_len = (size_t)outlenx;
        } break;
#endif

#if WITH_SNAPPY
        case RD_KAFKA_COMPRESSION_SNAPPY: {
                const char *inbuf = compressed;
                size_t inlen      = compressed_size;
                int r;
                static const unsigned char snappy_java_magic[] = {
                    0x82, 'S', 'N', 'A', 'P', 'P', 'Y', 0};
                static const size_t snappy_java_hdrlen = 8 + 4 + 4;

                /* snappy-java adds its own header (SnappyCodec)
                 * which is not compatible with the official Snappy
                 * implementation.
                 *   8: magic, 4: version, 4: compatible
                 * followed by any number of chunks:
                 *   4: length
                 * ...: snappy-compressed data. */
                if (likely(inlen > snappy_java_hdrlen + 4 &&
                           !memcmp(inbuf, snappy_java_magic, 8))) {
                        /* snappy-java framing */
                        char errstr[128];

                        inbuf = inbuf + snappy_java_hdrlen;
                        inlen -= snappy_java_hdrlen;
                        iov.iov_base = rd_kafka_snappy_java_uncompress(
                            inbuf, inlen, &iov.iov_len, errstr, sizeof(errstr));

                        if (unlikely(!iov.iov_base)) {
                                rd_rkb_dbg(msetr->msetr_rkb, MSG, "SNAPPY",
                                           "%s [%" PRId32
                                           "]: "
                                           "Snappy decompression for message "
                                           "at offset %" PRId64
                                           " failed: %s: "
                                           "ignoring message",
                                           rktp->rktp_rkt->rkt_topic->str,
                                           rktp->rktp_partition, Offset,
                                           errstr);
                                err = RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
                                goto err;
                        }


                } else {
                        /* No framing */

                        /* Acquire uncompressed length */
                        if (unlikely(!rd_kafka_snappy_uncompressed_length(
                                inbuf, inlen, &iov.iov_len))) {
                                rd_rkb_dbg(msetr->msetr_rkb, MSG, "SNAPPY",
                                           "Failed to get length of Snappy "
                                           "compressed payload "
                                           "for message at offset %" PRId64
                                           " (%" PRIusz
                                           " bytes): "
                                           "ignoring message",
                                           Offset, inlen);
                                err = RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
                                goto err;
                        }

                        /* Allocate output buffer for uncompressed data */
                        iov.iov_base = rd_malloc(iov.iov_len);
                        if (unlikely(!iov.iov_base)) {
                                rd_rkb_dbg(msetr->msetr_rkb, MSG, "SNAPPY",
                                           "Failed to allocate Snappy "
                                           "decompress buffer of size %" PRIusz
                                           "for message at offset %" PRId64
                                           " (%" PRIusz
                                           " bytes): %s: "
                                           "ignoring message",
                                           iov.iov_len, Offset, inlen,
                                           rd_strerror(errno));
                                err = RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE;
                                goto err;
                        }

                        /* Uncompress to outbuf */
                        if (unlikely((r = rd_kafka_snappy_uncompress(
                                          inbuf, inlen, iov.iov_base)))) {
                                rd_rkb_dbg(msetr->msetr_rkb, MSG, "SNAPPY",
                                           "Failed to decompress Snappy "
                                           "payload for message at offset "
                                           "%" PRId64 " (%" PRIusz
                                           " bytes): %s: "
                                           "ignoring message",
                                           Offset, inlen,
                                           rd_strerror(-r /*negative errno*/));
                                rd_free(iov.iov_base);
                                err = RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
                                goto err;
                        }
                }

        } break;
#endif

        case RD_KAFKA_COMPRESSION_KLZ4: {
                err =
                    rd_kafka_lz4_decompress(msetr->msetr_rkb,
                                            /* Proper HC? */
                                            MsgVersion >= 1 ? 1 : 0, Offset,
                                            /* @warning Will modify compressed
                                             *          if no proper HC */
                                            (char *)compressed, compressed_size,
                                            &iov.iov_base, &iov.iov_len);
                if (err)
                        goto err;
        } break;

#if WITH_ZSTD
        case RD_KAFKA_COMPRESSION_ZSTD: {
                err = rd_kafka_zstd_decompress(
                    msetr->msetr_rkb, (char *)compressed, compressed_size,
                    &iov.iov_base, &iov.iov_len);
                if (err)
                        goto err;
        } break;
#endif

        default:
                rd_rkb_dbg(msetr->msetr_rkb, MSG, "CODEC",
                           "%s [%" PRId32 "]: Message at offset %" PRId64
                           " with unsupported "
                           "compression codec 0x%x: message ignored",
                           rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                           Offset, (int)codec);

                err = RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED;
                goto err;
        }


        rd_assert(iov.iov_base);

        /*
         * Decompression successful
         */

        /* Create a new buffer pointing to the uncompressed
         * allocated buffer (outbuf) and let messages keep a reference to
         * this new buffer. */
        rkbufz = rd_kafka_buf_new_shadow(iov.iov_base, iov.iov_len, rd_free);
        rkbufz->rkbuf_rkb = msetr->msetr_rkbuf->rkbuf_rkb;
        rd_kafka_broker_keep(rkbufz->rkbuf_rkb);


        /* In MsgVersion v0..1 the decompressed data contains
         * an inner MessageSet, pass it to a new MessageSet reader.
         *
         * For MsgVersion v2 the decompressed data are the list of messages.
         */

        if (MsgVersion <= 1) {
                /* Pass decompressed data (inner Messageset)
                 * to new instance of the MessageSet parser. */
                rd_kafka_msgset_reader_t inner_msetr;
                rd_kafka_msgset_reader_init(
                    &inner_msetr, rkbufz, msetr->msetr_rktp, msetr->msetr_tver,
                    /* there is no aborted transaction
                     * support for MsgVersion < 2 */
                    NULL, &msetr->msetr_rkq);

                inner_msetr.msetr_srcname = "compressed ";

                if (MsgVersion == 1) {
                        /* postproc() will convert relative to
                         * absolute offsets */
                        inner_msetr.msetr_relative_offsets = 1;
                        inner_msetr.msetr_outer.offset     = Offset;

                        /* Apply single LogAppendTime timestamp for
                         * all messages. */
                        if (Attributes & RD_KAFKA_MSG_ATTR_LOG_APPEND_TIME) {
                                inner_msetr.msetr_outer.tstype =
                                    RD_KAFKA_TIMESTAMP_LOG_APPEND_TIME;
                                inner_msetr.msetr_outer.timestamp = Timestamp;
                        }
                }

                /* Parse the inner MessageSet */
                err = rd_kafka_msgset_reader_run(&inner_msetr);

                /* Transfer message count from inner to outer */
                msetr->msetr_msgcnt += inner_msetr.msetr_msgcnt;
                msetr->msetr_msg_bytes += inner_msetr.msetr_msg_bytes;


        } else {
                /* MsgVersion 2 */
                rd_kafka_buf_t *orig_rkbuf = msetr->msetr_rkbuf;

                rkbufz->rkbuf_uflow_mitigation =
                    "truncated response from broker (ok)";

                /* Temporarily replace read buffer with uncompressed buffer */
                msetr->msetr_rkbuf = rkbufz;

                /* Read messages */
                err = rd_kafka_msgset_reader_msgs_v2(msetr);

                /* Restore original buffer */
                msetr->msetr_rkbuf = orig_rkbuf;
        }

        /* Loose our refcnt of the uncompressed rkbuf.
         * Individual messages/rko's will have their own reference. */
        rd_kafka_buf_destroy(rkbufz);

        return err;

err:
        /* Enqueue error messsage:
         * Create op and push on temporary queue. */
        rd_kafka_consumer_err(
            &msetr->msetr_rkq, msetr->msetr_broker_id, err,
            msetr->msetr_tver->version, NULL, rktp, Offset,
            "Decompression (codec 0x%x) of message at %" PRIu64 " of %" PRIusz
            " bytes failed: %s",
            codec, Offset, compressed_size, rd_kafka_err2str(err));

        return err;
}



/**
 * @brief Message parser for MsgVersion v0..1
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or on single-message errors,
 *          or any other error code when the MessageSet parser should stop
 *          parsing (such as for partial Messages).
 */
static rd_kafka_resp_err_t
rd_kafka_msgset_reader_msg_v0_1(rd_kafka_msgset_reader_t *msetr) {
        rd_kafka_buf_t *rkbuf   = msetr->msetr_rkbuf;
        rd_kafka_toppar_t *rktp = msetr->msetr_rktp;
        rd_kafka_broker_t *rkb  = msetr->msetr_rkb;
        struct {
                int64_t Offset;      /* MessageSet header */
                int32_t MessageSize; /* MessageSet header */
                int32_t Crc;
                int8_t MagicByte; /* MsgVersion */
                int8_t Attributes;
                int64_t Timestamp; /* v1 */
        } hdr;                     /* Message header */
        rd_kafkap_bytes_t Key;
        rd_kafkap_bytes_t Value;
        int32_t Value_len;
        rd_kafka_op_t *rko;
        size_t hdrsize = 6; /* Header size following MessageSize */
        rd_slice_t crc_slice;
        rd_kafka_msg_t *rkm;
        int relative_offsets   = 0;
        const char *reloff_str = "";
        /* Only log decoding errors if protocol debugging enabled. */
        int log_decode_errors =
            (rkbuf->rkbuf_rkb->rkb_rk->rk_conf.debug & RD_KAFKA_DBG_PROTOCOL)
                ? LOG_DEBUG
                : 0;
        size_t message_end;

        rd_kafka_buf_read_i64(rkbuf, &hdr.Offset);
        rd_kafka_buf_read_i32(rkbuf, &hdr.MessageSize);
        message_end = rd_slice_offset(&rkbuf->rkbuf_reader) + hdr.MessageSize;

        rd_kafka_buf_read_i32(rkbuf, &hdr.Crc);
        if (!rd_slice_narrow_copy_relative(&rkbuf->rkbuf_reader, &crc_slice,
                                           hdr.MessageSize - 4))
                rd_kafka_buf_check_len(rkbuf, hdr.MessageSize - 4);

        rd_kafka_buf_read_i8(rkbuf, &hdr.MagicByte);
        rd_kafka_buf_read_i8(rkbuf, &hdr.Attributes);

        if (hdr.MagicByte == 1) { /* MsgVersion */
                rd_kafka_buf_read_i64(rkbuf, &hdr.Timestamp);
                hdrsize += 8;
                /* MsgVersion 1 has relative offsets for compressed
                 * MessageSets*/
                if (!(hdr.Attributes & RD_KAFKA_MSG_ATTR_COMPRESSION_MASK) &&
                    msetr->msetr_relative_offsets) {
                        relative_offsets = 1;
                        reloff_str       = "relative ";
                }
        } else
                hdr.Timestamp = 0;

        /* Verify MessageSize */
        if (unlikely(hdr.MessageSize < (ssize_t)hdrsize))
                rd_kafka_buf_parse_fail(
                    rkbuf,
                    "Message at %soffset %" PRId64 " MessageSize %" PRId32
                    " < hdrsize %" PRIusz,
                    reloff_str, hdr.Offset, hdr.MessageSize, hdrsize);

        /* Early check for partial messages */
        rd_kafka_buf_check_len(rkbuf, hdr.MessageSize - hdrsize);

        if (rkb->rkb_rk->rk_conf.check_crcs) {
                /* Verify CRC32 if desired. */
                uint32_t calc_crc;

                calc_crc = rd_slice_crc32(&crc_slice);
                rd_dassert(rd_slice_remains(&crc_slice) == 0);

                if (unlikely(hdr.Crc != (int32_t)calc_crc)) {
                        /* Propagate CRC error to application and
                         * continue with next message. */
                        rd_kafka_consumer_err(
                            &msetr->msetr_rkq, msetr->msetr_broker_id,
                            RD_KAFKA_RESP_ERR__BAD_MSG,
                            msetr->msetr_tver->version, NULL, rktp, hdr.Offset,
                            "Message at %soffset %" PRId64 " (%" PRId32
                            " bytes) "
                            "failed CRC32 check "
                            "(original 0x%" PRIx32
                            " != "
                            "calculated 0x%" PRIx32 ")",
                            reloff_str, hdr.Offset, hdr.MessageSize, hdr.Crc,
                            calc_crc);
                        rd_kafka_buf_skip_to(rkbuf, message_end);
                        rd_atomic64_add(&rkb->rkb_c.rx_err, 1);
                        /* Continue with next message */
                        return RD_KAFKA_RESP_ERR_NO_ERROR;
                }
        }


        /* Extract key */
        rd_kafka_buf_read_bytes(rkbuf, &Key);

        /* Extract Value */
        rd_kafka_buf_read_bytes(rkbuf, &Value);
        Value_len = RD_KAFKAP_BYTES_LEN(&Value);

        /* MessageSets may contain offsets earlier than we
         * requested (compressed MessageSets in particular),
         * drop the earlier messages.
         * Note: the inner offset may only be trusted for
         *       absolute offsets. KIP-31 introduced
         *       ApiVersion 2 that maintains relative offsets
         *       of compressed messages and the base offset
         *       in the outer message is the offset of
         *       the *LAST* message in the MessageSet.
         *       This requires us to assign offsets
         *       after all messages have been read from
         *       the messageset, and it also means
         *       we cant perform this offset check here
         *       in that case. */
        if (!relative_offsets && hdr.Offset < rktp->rktp_offsets.fetch_offset)
                return RD_KAFKA_RESP_ERR_NO_ERROR; /* Continue with next msg */

        /* Handle compressed MessageSet */
        if (unlikely(hdr.Attributes & RD_KAFKA_MSG_ATTR_COMPRESSION_MASK))
                return rd_kafka_msgset_reader_decompress(
                    msetr, hdr.MagicByte, hdr.Attributes, hdr.Timestamp,
                    hdr.Offset, Value.data, Value_len);


        /* Pure uncompressed message, this is the innermost
         * handler after all compression and cascaded
         * MessageSets have been peeled off. */

        /* Create op/message container for message. */
        rko = rd_kafka_op_new_fetch_msg(
            &rkm, rktp, msetr->msetr_tver->version, rkbuf, hdr.Offset,
            (size_t)RD_KAFKAP_BYTES_LEN(&Key),
            RD_KAFKAP_BYTES_IS_NULL(&Key) ? NULL : Key.data,
            (size_t)RD_KAFKAP_BYTES_LEN(&Value),
            RD_KAFKAP_BYTES_IS_NULL(&Value) ? NULL : Value.data);

        rkm->rkm_broker_id = msetr->msetr_broker_id;

        /* Assign message timestamp.
         * If message was in a compressed MessageSet and the outer/wrapper
         * Message.Attribute had a LOG_APPEND_TIME set, use the
         * outer timestamp */
        if (msetr->msetr_outer.tstype == RD_KAFKA_TIMESTAMP_LOG_APPEND_TIME) {
                rkm->rkm_timestamp = msetr->msetr_outer.timestamp;
                rkm->rkm_tstype    = msetr->msetr_outer.tstype;

        } else if (hdr.MagicByte >= 1 && hdr.Timestamp) {
                rkm->rkm_timestamp = hdr.Timestamp;
                if (hdr.Attributes & RD_KAFKA_MSG_ATTR_LOG_APPEND_TIME)
                        rkm->rkm_tstype = RD_KAFKA_TIMESTAMP_LOG_APPEND_TIME;
                else
                        rkm->rkm_tstype = RD_KAFKA_TIMESTAMP_CREATE_TIME;
        }

        /* Enqueue message on temporary queue */
        rd_kafka_q_enq(&msetr->msetr_rkq, rko);
        msetr->msetr_msgcnt++;
        msetr->msetr_msg_bytes += rkm->rkm_key_len + rkm->rkm_len;

        return RD_KAFKA_RESP_ERR_NO_ERROR; /* Continue */

err_parse:
        /* Count all parse errors as partial message errors. */
        rd_atomic64_add(&msetr->msetr_rkb->rkb_c.rx_partial, 1);
        return rkbuf->rkbuf_err;
}



/**
 * @brief Message parser for MsgVersion v2
 */
static rd_kafka_resp_err_t
rd_kafka_msgset_reader_msg_v2(rd_kafka_msgset_reader_t *msetr) {
        rd_kafka_buf_t *rkbuf   = msetr->msetr_rkbuf;
        rd_kafka_toppar_t *rktp = msetr->msetr_rktp;
        struct {
                int64_t Length;
                int8_t MsgAttributes;
                int64_t TimestampDelta;
                int64_t OffsetDelta;
                int64_t Offset; /* Absolute offset */
                rd_kafkap_bytes_t Key;
                rd_kafkap_bytes_t Value;
                rd_kafkap_bytes_t Headers;
        } hdr;
        rd_kafka_op_t *rko;
        rd_kafka_msg_t *rkm;
        /* Only log decoding errors if protocol debugging enabled. */
        int log_decode_errors =
            (rkbuf->rkbuf_rkb->rkb_rk->rk_conf.debug & RD_KAFKA_DBG_PROTOCOL)
                ? LOG_DEBUG
                : 0;
        size_t message_end;

        rd_kafka_buf_read_varint(rkbuf, &hdr.Length);
        message_end =
            rd_slice_offset(&rkbuf->rkbuf_reader) + (size_t)hdr.Length;
        rd_kafka_buf_read_i8(rkbuf, &hdr.MsgAttributes);

        rd_kafka_buf_read_varint(rkbuf, &hdr.TimestampDelta);
        rd_kafka_buf_read_varint(rkbuf, &hdr.OffsetDelta);
        hdr.Offset = msetr->msetr_v2_hdr->BaseOffset + hdr.OffsetDelta;

        /* Skip message if outdated */
        if (hdr.Offset < rktp->rktp_offsets.fetch_offset) {
                rd_rkb_dbg(msetr->msetr_rkb, MSG, "MSG",
                           "%s [%" PRId32
                           "]: "
                           "Skip offset %" PRId64 " < fetch_offset %" PRId64,
                           rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                           hdr.Offset, rktp->rktp_offsets.fetch_offset);
                rd_kafka_buf_skip_to(rkbuf, message_end);
                return RD_KAFKA_RESP_ERR_NO_ERROR; /* Continue with next msg */
        }

        /* Handle control messages */
        if (msetr->msetr_v2_hdr->Attributes & RD_KAFKA_MSGSET_V2_ATTR_CONTROL) {
                struct {
                        int64_t KeySize;
                        int16_t Version;
                        int16_t Type;
                } ctrl_data;
                int64_t aborted_txn_start_offset;

                rd_kafka_buf_read_varint(rkbuf, &ctrl_data.KeySize);

                if (unlikely(ctrl_data.KeySize < 2))
                        rd_kafka_buf_parse_fail(
                            rkbuf,
                            "%s [%" PRId32
                            "]: "
                            "Ctrl message at offset %" PRId64
                            " has invalid key size %" PRId64,
                            rktp->rktp_rkt->rkt_topic->str,
                            rktp->rktp_partition, hdr.Offset,
                            ctrl_data.KeySize);

                rd_kafka_buf_read_i16(rkbuf, &ctrl_data.Version);

                if (ctrl_data.Version != 0) {
                        rd_rkb_dbg(msetr->msetr_rkb, MSG, "MSG",
                                   "%s [%" PRId32
                                   "]: "
                                   "Skipping ctrl msg with "
                                   "unsupported version %" PRId16
                                   " at offset %" PRId64,
                                   rktp->rktp_rkt->rkt_topic->str,
                                   rktp->rktp_partition, ctrl_data.Version,
                                   hdr.Offset);
                        rd_kafka_buf_skip_to(rkbuf, message_end);
                        return RD_KAFKA_RESP_ERR_NO_ERROR; /* Continue with next
                                                              msg */
                }

                if (unlikely(ctrl_data.KeySize != 4))
                        rd_kafka_buf_parse_fail(
                            rkbuf,
                            "%s [%" PRId32
                            "]: "
                            "Ctrl message at offset %" PRId64
                            " has invalid key size %" PRId64,
                            rktp->rktp_rkt->rkt_topic->str,
                            rktp->rktp_partition, hdr.Offset,
                            ctrl_data.KeySize);

                rd_kafka_buf_read_i16(rkbuf, &ctrl_data.Type);

                /* Client is uninterested in value of commit marker */
                rd_kafka_buf_skip(
                    rkbuf, (int32_t)(message_end -
                                     rd_slice_offset(&rkbuf->rkbuf_reader)));

                switch (ctrl_data.Type) {
                case RD_KAFKA_CTRL_MSG_COMMIT:
                        /* always ignore. */
                        break;

                case RD_KAFKA_CTRL_MSG_ABORT:
                        if (msetr->msetr_rkb->rkb_rk->rk_conf.isolation_level !=
                            RD_KAFKA_READ_COMMITTED)
                                break;

                        if (unlikely(!msetr->msetr_aborted_txns)) {
                                rd_rkb_dbg(msetr->msetr_rkb,
                                           MSG | RD_KAFKA_DBG_EOS, "TXN",
                                           "%s [%" PRId32
                                           "] received abort txn "
                                           "ctrl msg at offset %" PRId64
                                           " for "
                                           "PID %" PRId64
                                           ", but there are no "
                                           "known aborted transactions: "
                                           "ignoring",
                                           rktp->rktp_rkt->rkt_topic->str,
                                           rktp->rktp_partition, hdr.Offset,
                                           msetr->msetr_v2_hdr->PID);
                                break;
                        }

                        /* This marks the end of this (aborted) transaction,
                         * advance to next aborted transaction in list */
                        aborted_txn_start_offset =
                            rd_kafka_aborted_txns_pop_offset(
                                msetr->msetr_aborted_txns,
                                msetr->msetr_v2_hdr->PID, hdr.Offset);

                        if (unlikely(aborted_txn_start_offset == -1)) {
                                rd_rkb_dbg(msetr->msetr_rkb,
                                           MSG | RD_KAFKA_DBG_EOS, "TXN",
                                           "%s [%" PRId32
                                           "] received abort txn "
                                           "ctrl msg at offset %" PRId64
                                           " for "
                                           "PID %" PRId64
                                           ", but this offset is "
                                           "not listed as an aborted "
                                           "transaction: aborted transaction "
                                           "was possibly empty: ignoring",
                                           rktp->rktp_rkt->rkt_topic->str,
                                           rktp->rktp_partition, hdr.Offset,
                                           msetr->msetr_v2_hdr->PID);
                                break;
                        }
                        break;


                default:
                        rd_rkb_dbg(msetr->msetr_rkb, MSG,
                                   "TXN"
                                   "%s [%" PRId32
                                   "]: "
                                   "Unsupported ctrl message "
                                   "type %" PRId16
                                   " at offset"
                                   " %" PRId64 ": ignoring",
                                   rktp->rktp_rkt->rkt_topic->str,
                                   rktp->rktp_partition, ctrl_data.Type,
                                   hdr.Offset);
                        break;
                }

                rko = rd_kafka_op_new_ctrl_msg(rktp, msetr->msetr_tver->version,
                                               rkbuf, hdr.Offset);
                rd_kafka_q_enq(&msetr->msetr_rkq, rko);
                msetr->msetr_msgcnt++;

                return RD_KAFKA_RESP_ERR_NO_ERROR;
        }

        /* Regular message */

        /* Note: messages in aborted transactions are skipped at the MessageSet
         * level */

        rd_kafka_buf_read_bytes_varint(rkbuf, &hdr.Key);
        rd_kafka_buf_read_bytes_varint(rkbuf, &hdr.Value);

        /* We parse the Headers later, just store the size (possibly truncated)
         * and pointer to the headers. */
        hdr.Headers.len =
            (int32_t)(message_end - rd_slice_offset(&rkbuf->rkbuf_reader));
        rd_kafka_buf_read_ptr(rkbuf, &hdr.Headers.data, hdr.Headers.len);

        /* Create op/message container for message. */
        rko = rd_kafka_op_new_fetch_msg(
            &rkm, rktp, msetr->msetr_tver->version, rkbuf, hdr.Offset,
            (size_t)RD_KAFKAP_BYTES_LEN(&hdr.Key),
            RD_KAFKAP_BYTES_IS_NULL(&hdr.Key) ? NULL : hdr.Key.data,
            (size_t)RD_KAFKAP_BYTES_LEN(&hdr.Value),
            RD_KAFKAP_BYTES_IS_NULL(&hdr.Value) ? NULL : hdr.Value.data);

        rkm->rkm_broker_id = msetr->msetr_broker_id;

        /* Store pointer to unparsed message headers, they will
         * be parsed on the first access.
         * This pointer points to the rkbuf payload.
         * Note: can't perform struct copy here due to const fields (MSVC) */
        rkm->rkm_u.consumer.binhdrs.len  = hdr.Headers.len;
        rkm->rkm_u.consumer.binhdrs.data = hdr.Headers.data;

        /* Set timestamp.
         *
         * When broker assigns the timestamps (LOG_APPEND_TIME) it will
         * assign the same timestamp for all messages in a MessageSet
         * using MaxTimestamp.
         */
        if ((msetr->msetr_v2_hdr->Attributes &
             RD_KAFKA_MSG_ATTR_LOG_APPEND_TIME) ||
            (hdr.MsgAttributes & RD_KAFKA_MSG_ATTR_LOG_APPEND_TIME)) {
                rkm->rkm_tstype    = RD_KAFKA_TIMESTAMP_LOG_APPEND_TIME;
                rkm->rkm_timestamp = msetr->msetr_v2_hdr->MaxTimestamp;
        } else {
                rkm->rkm_tstype = RD_KAFKA_TIMESTAMP_CREATE_TIME;
                rkm->rkm_timestamp =
                    msetr->msetr_v2_hdr->BaseTimestamp + hdr.TimestampDelta;
        }


        /* Enqueue message on temporary queue */
        rd_kafka_q_enq(&msetr->msetr_rkq, rko);
        msetr->msetr_msgcnt++;
        msetr->msetr_msg_bytes += rkm->rkm_key_len + rkm->rkm_len;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        /* Count all parse errors as partial message errors. */
        rd_atomic64_add(&msetr->msetr_rkb->rkb_c.rx_partial, 1);
        return rkbuf->rkbuf_err;
}


/**
 * @brief Read v2 messages from current buffer position.
 */
static rd_kafka_resp_err_t
rd_kafka_msgset_reader_msgs_v2(rd_kafka_msgset_reader_t *msetr) {
        rd_kafka_buf_t *rkbuf   = msetr->msetr_rkbuf;
        rd_kafka_toppar_t *rktp = msetr->msetr_rktp;
        /* Only log decoding errors if protocol debugging enabled. */
        int log_decode_errors =
            (rkbuf->rkbuf_rkb->rkb_rk->rk_conf.debug & RD_KAFKA_DBG_PROTOCOL)
                ? LOG_DEBUG
                : 0;

        if (msetr->msetr_aborted_txns != NULL &&
            (msetr->msetr_v2_hdr->Attributes &
             (RD_KAFKA_MSGSET_V2_ATTR_TRANSACTIONAL |
              RD_KAFKA_MSGSET_V2_ATTR_CONTROL)) ==
                RD_KAFKA_MSGSET_V2_ATTR_TRANSACTIONAL) {
                /* Transactional non-control MessageSet:
                 * check if it is part of an aborted transaction. */
                int64_t txn_start_offset = rd_kafka_aborted_txns_get_offset(
                    msetr->msetr_aborted_txns, msetr->msetr_v2_hdr->PID);

                if (txn_start_offset != -1 &&
                    msetr->msetr_v2_hdr->BaseOffset >= txn_start_offset) {
                        /* MessageSet is part of aborted transaction */
                        rd_rkb_dbg(msetr->msetr_rkb, MSG, "MSG",
                                   "%s [%" PRId32
                                   "]: "
                                   "Skipping %" PRId32
                                   " message(s) "
                                   "in aborted transaction "
                                   "at offset %" PRId64 " for PID %" PRId64,
                                   rktp->rktp_rkt->rkt_topic->str,
                                   rktp->rktp_partition,
                                   msetr->msetr_v2_hdr->RecordCount,
                                   txn_start_offset, msetr->msetr_v2_hdr->PID);
                        rd_kafka_buf_skip(
                            msetr->msetr_rkbuf,
                            rd_slice_remains(
                                &msetr->msetr_rkbuf->rkbuf_reader));
                        msetr->msetr_aborted_cnt++;
                        return RD_KAFKA_RESP_ERR_NO_ERROR;
                }
        }

        while (rd_kafka_buf_read_remain(msetr->msetr_rkbuf)) {
                rd_kafka_resp_err_t err;
                err = rd_kafka_msgset_reader_msg_v2(msetr);
                if (unlikely(err))
                        return err;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        /* Count all parse errors as partial message errors. */
        rd_atomic64_add(&msetr->msetr_rkb->rkb_c.rx_partial, 1);
        msetr->msetr_v2_hdr = NULL;
        return rkbuf->rkbuf_err;
}



/**
 * @brief MessageSet reader for MsgVersion v2 (FetchRequest v4)
 */
static rd_kafka_resp_err_t
rd_kafka_msgset_reader_v2(rd_kafka_msgset_reader_t *msetr) {
        rd_kafka_buf_t *rkbuf   = msetr->msetr_rkbuf;
        rd_kafka_toppar_t *rktp = msetr->msetr_rktp;
        struct msgset_v2_hdr hdr;
        rd_slice_t save_slice;
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        size_t len_start;
        size_t payload_size;
        int64_t LastOffset; /* Last absolute Offset in MessageSet header */
        /* Only log decoding errors if protocol debugging enabled. */
        int log_decode_errors =
            (rkbuf->rkbuf_rkb->rkb_rk->rk_conf.debug & RD_KAFKA_DBG_PROTOCOL)
                ? LOG_DEBUG
                : 0;

        rd_kafka_buf_read_i64(rkbuf, &hdr.BaseOffset);
        rd_kafka_buf_read_i32(rkbuf, &hdr.Length);
        len_start = rd_slice_offset(&rkbuf->rkbuf_reader);

        if (unlikely(hdr.Length < RD_KAFKAP_MSGSET_V2_SIZE - 8 - 4))
                rd_kafka_buf_parse_fail(rkbuf,
                                        "%s [%" PRId32
                                        "] "
                                        "MessageSet at offset %" PRId64
                                        " length %" PRId32 " < header size %d",
                                        rktp->rktp_rkt->rkt_topic->str,
                                        rktp->rktp_partition, hdr.BaseOffset,
                                        hdr.Length,
                                        RD_KAFKAP_MSGSET_V2_SIZE - 8 - 4);

        rd_kafka_buf_read_i32(rkbuf, &hdr.PartitionLeaderEpoch);
        rd_kafka_buf_read_i8(rkbuf, &hdr.MagicByte);
        rd_kafka_buf_read_i32(rkbuf, &hdr.Crc);

        if (msetr->msetr_rkb->rkb_rk->rk_conf.check_crcs) {
                /* Verify CRC32C if desired. */
                uint32_t calc_crc;
                rd_slice_t crc_slice;
                size_t crc_len = hdr.Length - 4 - 1 - 4;

                if (!rd_slice_narrow_copy_relative(&rkbuf->rkbuf_reader,
                                                   &crc_slice, crc_len))
                        rd_kafka_buf_check_len(rkbuf, crc_len);

                calc_crc = rd_slice_crc32c(&crc_slice);

                if (unlikely((uint32_t)hdr.Crc != calc_crc)) {
                        /* Propagate CRC error to application and
                         * continue with next message. */
                        rd_kafka_consumer_err(
                            &msetr->msetr_rkq, msetr->msetr_broker_id,
                            RD_KAFKA_RESP_ERR__BAD_MSG,
                            msetr->msetr_tver->version, NULL, rktp,
                            hdr.BaseOffset,
                            "MessageSet at offset %" PRId64 " (%" PRId32
                            " bytes) "
                            "failed CRC32C check "
                            "(original 0x%" PRIx32
                            " != "
                            "calculated 0x%" PRIx32 ")",
                            hdr.BaseOffset, hdr.Length, hdr.Crc, calc_crc);
                        rd_kafka_buf_skip_to(rkbuf, crc_len);
                        rd_atomic64_add(&msetr->msetr_rkb->rkb_c.rx_err, 1);
                        return RD_KAFKA_RESP_ERR_NO_ERROR;
                }
        }

        rd_kafka_buf_read_i16(rkbuf, &hdr.Attributes);
        rd_kafka_buf_read_i32(rkbuf, &hdr.LastOffsetDelta);
        LastOffset = hdr.BaseOffset + hdr.LastOffsetDelta;
        rd_kafka_buf_read_i64(rkbuf, &hdr.BaseTimestamp);
        rd_kafka_buf_read_i64(rkbuf, &hdr.MaxTimestamp);
        rd_kafka_buf_read_i64(rkbuf, &hdr.PID);
        rd_kafka_buf_read_i16(rkbuf, &hdr.ProducerEpoch);
        rd_kafka_buf_read_i32(rkbuf, &hdr.BaseSequence);
        rd_kafka_buf_read_i32(rkbuf, &hdr.RecordCount);

        /* Payload size is hdr.Length - MessageSet headers */
        payload_size =
            hdr.Length - (rd_slice_offset(&rkbuf->rkbuf_reader) - len_start);

        if (unlikely(payload_size > rd_kafka_buf_read_remain(rkbuf)))
                rd_kafka_buf_underflow_fail(
                    rkbuf, payload_size,
                    "%s [%" PRId32
                    "] "
                    "MessageSet at offset %" PRId64 " payload size %" PRIusz,
                    rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                    hdr.BaseOffset, payload_size);

        /* If entire MessageSet contains old outdated offsets, skip it. */
        if (LastOffset < rktp->rktp_offsets.fetch_offset) {
                rd_kafka_buf_skip(rkbuf, payload_size);
                goto done;
        }

        if (hdr.Attributes & RD_KAFKA_MSGSET_V2_ATTR_CONTROL)
                msetr->msetr_ctrl_cnt++;

        msetr->msetr_v2_hdr = &hdr;

        /* Handle compressed MessageSet */
        if (hdr.Attributes & RD_KAFKA_MSG_ATTR_COMPRESSION_MASK) {
                const void *compressed;

                compressed =
                    rd_slice_ensure_contig(&rkbuf->rkbuf_reader, payload_size);
                rd_assert(compressed);

                err = rd_kafka_msgset_reader_decompress(
                    msetr, 2 /*MsgVersion v2*/, hdr.Attributes,
                    hdr.BaseTimestamp, hdr.BaseOffset, compressed,
                    payload_size);
                if (err)
                        goto err;

        } else {
                /* Read uncompressed messages */

                /* Save original slice, reduce size of the current one to
                 * be limited by the MessageSet.Length, and then start reading
                 * messages until the lesser slice is exhausted. */
                if (!rd_slice_narrow_relative(&rkbuf->rkbuf_reader, &save_slice,
                                              payload_size))
                        rd_kafka_buf_check_len(rkbuf, payload_size);

                /* Read messages */
                err = rd_kafka_msgset_reader_msgs_v2(msetr);

                /* Restore wider slice */
                rd_slice_widen(&rkbuf->rkbuf_reader, &save_slice);

                if (unlikely(err))
                        goto err;
        }


done:
        /* Set the next fetch offset to the MessageSet header's last offset + 1
         * to avoid getting stuck on compacted MessageSets where the last
         * Message in the MessageSet has an Offset < MessageSet header's
         * last offset.  See KAFKA-5443 */
        msetr->msetr_next_offset = LastOffset + 1;

        msetr->msetr_v2_hdr = NULL;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        /* Count all parse errors as partial message errors. */
        rd_atomic64_add(&msetr->msetr_rkb->rkb_c.rx_partial, 1);
        err = rkbuf->rkbuf_err;
        /* FALLTHRU */
err:
        msetr->msetr_v2_hdr = NULL;
        return err;
}


/**
 * @brief Peek into the next MessageSet to find the MsgVersion.
 *
 * @param MagicBytep the MsgVersion is returned here on success.
 *
 * @returns an error on read underflow or if the MsgVersion is
 *          unsupported.
 */
static rd_kafka_resp_err_t
rd_kafka_msgset_reader_peek_msg_version(rd_kafka_msgset_reader_t *msetr,
                                        int8_t *MagicBytep) {
        rd_kafka_buf_t *rkbuf   = msetr->msetr_rkbuf;
        rd_kafka_toppar_t *rktp = msetr->msetr_rktp;
        /* Only log decoding errors if protocol debugging enabled. */
        int log_decode_errors =
            (rkbuf->rkbuf_rkb->rkb_rk->rk_conf.debug & RD_KAFKA_DBG_PROTOCOL)
                ? LOG_DEBUG
                : 0;
        size_t read_offset = rd_slice_offset(&rkbuf->rkbuf_reader);

        rd_kafka_buf_peek_i8(rkbuf, read_offset + 8 + 4 + 4, MagicBytep);

        if (unlikely(*MagicBytep < 0 || *MagicBytep > 2)) {
                int64_t Offset; /* For error logging */
                int32_t Length;

                rd_kafka_buf_read_i64(rkbuf, &Offset);

                rd_rkb_dbg(msetr->msetr_rkb,
                           MSG | RD_KAFKA_DBG_PROTOCOL | RD_KAFKA_DBG_FETCH,
                           "MAGICBYTE",
                           "%s [%" PRId32
                           "]: "
                           "Unsupported Message(Set) MagicByte %d at "
                           "offset %" PRId64
                           " "
                           "(buffer position %" PRIusz "/%" PRIusz
                           "): skipping",
                           rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                           (int)*MagicBytep, Offset, read_offset,
                           rd_slice_size(&rkbuf->rkbuf_reader));

                if (Offset >= msetr->msetr_rktp->rktp_offsets.fetch_offset) {
                        rd_kafka_consumer_err(
                            &msetr->msetr_rkq, msetr->msetr_broker_id,
                            RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED,
                            msetr->msetr_tver->version, NULL, rktp, Offset,
                            "Unsupported Message(Set) MagicByte %d "
                            "at offset %" PRId64,
                            (int)*MagicBytep, Offset);
                        /* Skip message(set) */
                        msetr->msetr_rktp->rktp_offsets.fetch_offset =
                            Offset + 1;
                }

                /* Skip this Message(Set).
                 * If the message is malformed, the skip may trigger err_parse
                 * and return ERR__BAD_MSG. */
                rd_kafka_buf_read_i32(rkbuf, &Length);
                rd_kafka_buf_skip(rkbuf, Length);

                return RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        return RD_KAFKA_RESP_ERR__BAD_MSG;
}


/**
 * @brief Parse and read messages from msgset reader buffer.
 */
static rd_kafka_resp_err_t
rd_kafka_msgset_reader(rd_kafka_msgset_reader_t *msetr) {
        rd_kafka_buf_t *rkbuf = msetr->msetr_rkbuf;
        rd_kafka_resp_err_t (*reader[])(rd_kafka_msgset_reader_t *) = {
            /* Indexed by MsgVersion/MagicByte, pointing to
             * a Msg(Set)Version reader */
            [0] = rd_kafka_msgset_reader_msg_v0_1,
            [1] = rd_kafka_msgset_reader_msg_v0_1,
            [2] = rd_kafka_msgset_reader_v2};
        rd_kafka_resp_err_t err;

        /* Parse MessageSets until the slice is exhausted or an
         * error occurs (typically a partial message). */
        do {
                int8_t MagicByte;

                /* We dont know the MsgVersion at this point, peek where the
                 * MagicByte resides both in MsgVersion v0..1 and v2 to
                 * know which MessageSet reader to use. */
                err =
                    rd_kafka_msgset_reader_peek_msg_version(msetr, &MagicByte);
                if (unlikely(err)) {
                        if (err == RD_KAFKA_RESP_ERR__BAD_MSG)
                                /* Read underflow, not an error.
                                 * Broker may return a partial Fetch response
                                 * due to its use of sendfile(2). */
                                return RD_KAFKA_RESP_ERR_NO_ERROR;

                        /* Continue on unsupported MsgVersions, the
                         * MessageSet will be skipped. */
                        continue;
                }

                /* Use MsgVersion-specific reader */
                err = reader[(int)MagicByte](msetr);

        } while (!err && rd_slice_remains(&rkbuf->rkbuf_reader) > 0);

        return err;
}



/**
 * @brief MessageSet post-processing.
 *
 * @param last_offsetp will be set to the offset of the last message in the set,
 *                     or -1 if not applicable.
 */
static void rd_kafka_msgset_reader_postproc(rd_kafka_msgset_reader_t *msetr,
                                            int64_t *last_offsetp) {
        rd_kafka_op_t *rko;

        rko = rd_kafka_q_last(&msetr->msetr_rkq, RD_KAFKA_OP_FETCH,
                              0 /* no error ops */);
        if (rko) {
                *last_offsetp = rko->rko_u.fetch.rkm.rkm_offset;

                if (*last_offsetp != -1 && msetr->msetr_relative_offsets) {
                        /* Update messages to absolute offsets
                         * and purge any messages older than the current
                         * fetch offset. */
                        rd_kafka_q_fix_offsets(
                            &msetr->msetr_rkq,
                            msetr->msetr_rktp->rktp_offsets.fetch_offset,
                            msetr->msetr_outer.offset - *last_offsetp);
                }
        }
}



/**
 * @brief Run the MessageSet reader, read messages until buffer is
 *        exhausted (or error encountered), enqueue parsed messages on
 *        partition queue.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if MessageSet was successfully
 *          or partially parsed. When other error codes are returned it
 *          indicates a semi-permanent error (such as unsupported MsgVersion)
 *          and the fetcher should back off this partition to avoid
 *          busy-looping.
 */
static rd_kafka_resp_err_t
rd_kafka_msgset_reader_run(rd_kafka_msgset_reader_t *msetr) {
        rd_kafka_toppar_t *rktp = msetr->msetr_rktp;
        rd_kafka_resp_err_t err;
        int64_t last_offset = -1;

        /* Parse MessageSets and messages */
        err = rd_kafka_msgset_reader(msetr);

        if (unlikely(rd_kafka_q_len(&msetr->msetr_rkq) == 0)) {
                /* The message set didn't contain at least one full message
                 * or no error was posted on the response queue.
                 * This means the size limit perhaps was too tight,
                 * increase it automatically.
                 * If there was at least one control message there
                 * is probably not a size limit and nothing is done.
                 * If there were aborted messagesets and no underflow then
                 * there is no error either (#2993).
                 *
                 * Also; avoid propagating underflow errors, which cause
                 * backoffs, since we'll want to continue fetching the
                 * remaining truncated messages as soon as possible.
                 */
                if (msetr->msetr_ctrl_cnt > 0) {
                        /* Noop */
                        if (err == RD_KAFKA_RESP_ERR__UNDERFLOW)
                                err = RD_KAFKA_RESP_ERR_NO_ERROR;

                } else if (rktp->rktp_fetch_msg_max_bytes < (1 << 30)) {
                        rktp->rktp_fetch_msg_max_bytes *= 2;
                        rd_rkb_dbg(msetr->msetr_rkb, FETCH, "CONSUME",
                                   "Topic %s [%" PRId32
                                   "]: Increasing "
                                   "max fetch bytes to %" PRId32,
                                   rktp->rktp_rkt->rkt_topic->str,
                                   rktp->rktp_partition,
                                   rktp->rktp_fetch_msg_max_bytes);

                        if (err == RD_KAFKA_RESP_ERR__UNDERFLOW)
                                err = RD_KAFKA_RESP_ERR_NO_ERROR;

                } else if (!err && msetr->msetr_aborted_cnt == 0) {
                        rd_kafka_consumer_err(
                            &msetr->msetr_rkq, msetr->msetr_broker_id,
                            RD_KAFKA_RESP_ERR_MSG_SIZE_TOO_LARGE,
                            msetr->msetr_tver->version, NULL, rktp,
                            rktp->rktp_offsets.fetch_offset,
                            "Message at offset %" PRId64
                            " might be too large to fetch, try increasing "
                            "receive.message.max.bytes",
                            rktp->rktp_offsets.fetch_offset);

                } else if (msetr->msetr_aborted_cnt > 0) {
                        /* Noop */
                        if (err == RD_KAFKA_RESP_ERR__UNDERFLOW)
                                err = RD_KAFKA_RESP_ERR_NO_ERROR;
                }

        } else {
                /* MessageSet post-processing. */
                rd_kafka_msgset_reader_postproc(msetr, &last_offset);

                /* Ignore parse errors if there was at least one
                 * good message since it probably indicates a
                 * partial response rather than an erroneous one. */
                if (err == RD_KAFKA_RESP_ERR__UNDERFLOW &&
                    msetr->msetr_msgcnt > 0)
                        err = RD_KAFKA_RESP_ERR_NO_ERROR;
        }

        rd_rkb_dbg(msetr->msetr_rkb, MSG | RD_KAFKA_DBG_FETCH, "CONSUME",
                   "Enqueue %i %smessage(s) (%" PRId64
                   " bytes, %d ops) on %s [%" PRId32
                   "] fetch queue (qlen %d, v%d, last_offset %" PRId64
                   ", %d ctrl msgs, %d aborted msgsets, %s)",
                   msetr->msetr_msgcnt, msetr->msetr_srcname,
                   msetr->msetr_msg_bytes, rd_kafka_q_len(&msetr->msetr_rkq),
                   rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                   rd_kafka_q_len(msetr->msetr_par_rkq),
                   msetr->msetr_tver->version, last_offset,
                   msetr->msetr_ctrl_cnt, msetr->msetr_aborted_cnt,
                   msetr->msetr_compression
                       ? rd_kafka_compression2str(msetr->msetr_compression)
                       : "uncompressed");

        /* Concat all messages&errors onto the parent's queue
         * (the partition's fetch queue) */
        if (rd_kafka_q_concat(msetr->msetr_par_rkq, &msetr->msetr_rkq) != -1) {
                /* Update partition's fetch offset based on
                 * last message's offest. */
                if (likely(last_offset != -1))
                        rktp->rktp_offsets.fetch_offset = last_offset + 1;
        }

        /* Adjust next fetch offset if outlier code has indicated
         * an even later next offset. */
        if (msetr->msetr_next_offset > rktp->rktp_offsets.fetch_offset)
                rktp->rktp_offsets.fetch_offset = msetr->msetr_next_offset;

        rd_kafka_q_destroy_owner(&msetr->msetr_rkq);

        /* Skip remaining part of slice so caller can continue
         * with next partition. */
        rd_slice_read(&msetr->msetr_rkbuf->rkbuf_reader, NULL,
                      rd_slice_remains(&msetr->msetr_rkbuf->rkbuf_reader));
        return err;
}



/**
 * @brief Parse one MessageSet at the current buffer read position,
 *        enqueueing messages, propagating errors, etc.
 * @remark The current rkbuf_reader slice must be limited to the MessageSet size
 *
 * @returns see rd_kafka_msgset_reader_run()
 */
rd_kafka_resp_err_t
rd_kafka_msgset_parse(rd_kafka_buf_t *rkbuf,
                      rd_kafka_buf_t *request,
                      rd_kafka_toppar_t *rktp,
                      rd_kafka_aborted_txns_t *aborted_txns,
                      const struct rd_kafka_toppar_ver *tver) {
        rd_kafka_msgset_reader_t msetr;
        rd_kafka_resp_err_t err;

        rd_kafka_msgset_reader_init(&msetr, rkbuf, rktp, tver, aborted_txns,
                                    rktp->rktp_fetchq);

        /* Parse and handle the message set */
        err = rd_kafka_msgset_reader_run(&msetr);

        rd_atomic64_add(&rktp->rktp_c.rx_msgs, msetr.msetr_msgcnt);
        rd_atomic64_add(&rktp->rktp_c.rx_msg_bytes, msetr.msetr_msg_bytes);

        rd_avg_add(&rktp->rktp_rkt->rkt_avg_batchcnt,
                   (int64_t)msetr.msetr_msgcnt);
        rd_avg_add(&rktp->rktp_rkt->rkt_avg_batchsize,
                   (int64_t)msetr.msetr_msg_bytes);

        return err;
}


/**
 * @brief Offset comparator
 */
static int rd_kafka_offset_cmp(const void *_a, const void *_b) {
        const int64_t *a = _a, *b = _b;
        return (*a > *b) - (*a < *b);
}


/**
 * @brief Pid comparator for rd_kafka_aborted_txn_start_offsets_t
 */
static int rd_kafka_aborted_txn_cmp_by_pid(const void *_a, const void *_b) {
        const rd_kafka_aborted_txn_start_offsets_t *a = _a, *b = _b;
        return (a->pid > b->pid) - (a->pid < b->pid);
}


/**
 * @brief Free resources associated with an AVL tree node.
 */
static void rd_kafka_aborted_txn_node_destroy(void *_node_ptr) {
        rd_kafka_aborted_txn_start_offsets_t *node_ptr = _node_ptr;
        rd_list_destroy(&node_ptr->offsets);
        rd_free(node_ptr);
}


/**
 * @brief Allocate memory for, and initialize a new
 * rd_kafka_aborted_txns_t struct.
 */
rd_kafka_aborted_txns_t *rd_kafka_aborted_txns_new(int32_t txn_cnt) {
        rd_kafka_aborted_txns_t *aborted_txns;
        aborted_txns = rd_malloc(sizeof(*aborted_txns));
        rd_avl_init(&aborted_txns->avl, rd_kafka_aborted_txn_cmp_by_pid, 0);
        rd_list_init(&aborted_txns->list, txn_cnt,
                     rd_kafka_aborted_txn_node_destroy);
        aborted_txns->cnt = txn_cnt;
        return aborted_txns;
}


/**
 * @brief Free all resources associated with a
 * rd_kafka_aborted_txns_t struct.
 */
void rd_kafka_aborted_txns_destroy(rd_kafka_aborted_txns_t *aborted_txns) {
        rd_list_destroy(&aborted_txns->list);
        rd_avl_destroy(&aborted_txns->avl);
        rd_free(aborted_txns);
}


/**
 * @brief Get the abort txn start offsets corresponding to
 * the specified pid.
 */
static RD_INLINE rd_kafka_aborted_txn_start_offsets_t *
rd_kafka_aborted_txns_offsets_for_pid(rd_kafka_aborted_txns_t *aborted_txns,
                                      int64_t pid) {
        rd_kafka_aborted_txn_start_offsets_t node;
        node.pid = pid;
        return RD_AVL_FIND(&aborted_txns->avl, &node);
}


/**
 * @brief Get the next aborted transaction start
 * offset for the specified pid.
 *
 * @param increment_idx if true, the offset index will be incremented.
 * @param max_offset If the next aborted offset is greater than \p max_offset
 *                   then the index is not incremented (regardless of
 *                   \p increment_idx) and the function returns -1.
 *                   This may be the case for empty aborted transactions
 *                   that have an ABORT marker but are not listed in the
 *                   AbortedTxns list.
 *
 *
 * @returns the start offset or -1 if there is none.
 */
static int64_t
rd_kafka_aborted_txns_next_offset(rd_kafka_aborted_txns_t *aborted_txns,
                                  int64_t pid,
                                  rd_bool_t increment_idx,
                                  int64_t max_offset) {
        int64_t abort_start_offset;
        rd_kafka_aborted_txn_start_offsets_t *node_ptr =
            rd_kafka_aborted_txns_offsets_for_pid(aborted_txns, pid);

        if (node_ptr == NULL)
                return -1;

        if (unlikely(node_ptr->offsets_idx >= rd_list_cnt(&node_ptr->offsets)))
                return -1;

        abort_start_offset = *(
            (int64_t *)rd_list_elem(&node_ptr->offsets, node_ptr->offsets_idx));

        if (unlikely(abort_start_offset > max_offset))
                return -1;

        if (increment_idx)
                node_ptr->offsets_idx++;

        return abort_start_offset;
}


/**
 * @brief Get the next aborted transaction start
 * offset for the specified pid and progress the
 * current index to the next one.
 *
 * @param max_offset If the next aborted offset is greater than \p max_offset
 *                   then no offset is popped and the function returns -1.
 *                   This may be the case for empty aborted transactions
 *                   that have an ABORT marker but are not listed in the
 *                   AbortedTxns list.
 *
 * @returns the start offset or -1 if there is none.
 */
static RD_INLINE int64_t
rd_kafka_aborted_txns_pop_offset(rd_kafka_aborted_txns_t *aborted_txns,
                                 int64_t pid,
                                 int64_t max_offset) {
        return rd_kafka_aborted_txns_next_offset(aborted_txns, pid, rd_true,
                                                 max_offset);
}


/**
 * @brief Get the next aborted transaction start
 * offset for the specified pid.
 *
 * @returns the start offset or -1 if there is none.
 */
static RD_INLINE int64_t
rd_kafka_aborted_txns_get_offset(const rd_kafka_aborted_txns_t *aborted_txns,
                                 int64_t pid) {
        return rd_kafka_aborted_txns_next_offset(
            (rd_kafka_aborted_txns_t *)aborted_txns, pid, rd_false, INT64_MAX);
}


/**
 * @brief Add a transaction start offset corresponding
 * to the specified pid to the aborted_txns collection.
 */
void rd_kafka_aborted_txns_add(rd_kafka_aborted_txns_t *aborted_txns,
                               int64_t pid,
                               int64_t first_offset) {
        int64_t *v;
        rd_kafka_aborted_txn_start_offsets_t *node_ptr =
            rd_kafka_aborted_txns_offsets_for_pid(aborted_txns, pid);

        if (!node_ptr) {
                node_ptr              = rd_malloc(sizeof(*node_ptr));
                node_ptr->pid         = pid;
                node_ptr->offsets_idx = 0;
                rd_list_init(&node_ptr->offsets, 0, NULL);
                /* Each PID list has no more than AbortedTxnCnt elements */
                rd_list_prealloc_elems(&node_ptr->offsets, sizeof(int64_t),
                                       aborted_txns->cnt, 0);
                RD_AVL_INSERT(&aborted_txns->avl, node_ptr, avl_node);
                rd_list_add(&aborted_txns->list, node_ptr);
        }

        v  = rd_list_add(&node_ptr->offsets, NULL);
        *v = first_offset;
}


/**
 * @brief Sort each of the abort transaction start
 * offset lists for each pid.
 */
void rd_kafka_aborted_txns_sort(rd_kafka_aborted_txns_t *aborted_txns) {
        int k;
        for (k = 0; k < rd_list_cnt(&aborted_txns->list); k++) {
                rd_kafka_aborted_txn_start_offsets_t *el =
                    rd_list_elem(&aborted_txns->list, k);
                rd_list_sort(&el->offsets, rd_kafka_offset_cmp);
        }
}


/**
 * @brief Unit tests for all functions that operate on
 * rd_kafka_aborted_txns_t
 */
int unittest_aborted_txns(void) {
        rd_kafka_aborted_txns_t *aborted_txns = NULL;
        int64_t start_offset;

        aborted_txns = rd_kafka_aborted_txns_new(7);
        rd_kafka_aborted_txns_add(aborted_txns, 1, 42);
        rd_kafka_aborted_txns_add(aborted_txns, 1, 44);
        rd_kafka_aborted_txns_add(aborted_txns, 1, 10);
        rd_kafka_aborted_txns_add(aborted_txns, 1, 100);
        rd_kafka_aborted_txns_add(aborted_txns, 2, 11);
        rd_kafka_aborted_txns_add(aborted_txns, 2, 7);
        rd_kafka_aborted_txns_add(aborted_txns, 1, 3);
        rd_kafka_aborted_txns_sort(aborted_txns);

        start_offset = rd_kafka_aborted_txns_get_offset(aborted_txns, 1);
        RD_UT_ASSERT(3 == start_offset,
                     "queried start offset was %" PRId64
                     ", "
                     "expected 3",
                     start_offset);

        start_offset = rd_kafka_aborted_txns_get_offset(aborted_txns, 1);
        RD_UT_ASSERT(3 == start_offset,
                     "queried start offset was %" PRId64
                     ", "
                     "expected 3",
                     start_offset);

        start_offset =
            rd_kafka_aborted_txns_pop_offset(aborted_txns, 1, INT64_MAX);
        RD_UT_ASSERT(3 == start_offset,
                     "queried start offset was %" PRId64
                     ", "
                     "expected 3",
                     start_offset);

        start_offset = rd_kafka_aborted_txns_get_offset(aborted_txns, 1);
        RD_UT_ASSERT(10 == start_offset,
                     "queried start offset was %" PRId64
                     ", "
                     "expected 10",
                     start_offset);

        start_offset = rd_kafka_aborted_txns_get_offset(aborted_txns, 2);
        RD_UT_ASSERT(7 == start_offset,
                     "queried start offset was %" PRId64
                     ", "
                     "expected 7",
                     start_offset);

        rd_kafka_aborted_txns_pop_offset(aborted_txns, 1, INT64_MAX);

        start_offset = rd_kafka_aborted_txns_get_offset(aborted_txns, 1);
        RD_UT_ASSERT(42 == start_offset,
                     "queried start offset was %" PRId64
                     ", "
                     "expected 42",
                     start_offset);

        rd_kafka_aborted_txns_pop_offset(aborted_txns, 1, INT64_MAX);

        start_offset = rd_kafka_aborted_txns_get_offset(aborted_txns, 1);
        RD_UT_ASSERT(44 == start_offset,
                     "queried start offset was %" PRId64
                     ", "
                     "expected 44",
                     start_offset);

        start_offset = rd_kafka_aborted_txns_get_offset(aborted_txns, 2);
        RD_UT_ASSERT(7 == start_offset,
                     "queried start offset was %" PRId64
                     ", "
                     "expected 7",
                     start_offset);

        rd_kafka_aborted_txns_pop_offset(aborted_txns, 2, INT64_MAX);

        start_offset = rd_kafka_aborted_txns_get_offset(aborted_txns, 2);
        RD_UT_ASSERT(11 == start_offset,
                     "queried start offset was %" PRId64
                     ", "
                     "expected 11",
                     start_offset);

        /* error cases */
        start_offset = rd_kafka_aborted_txns_get_offset(aborted_txns, 3);
        RD_UT_ASSERT(-1 == start_offset,
                     "queried start offset was %" PRId64
                     ", "
                     "expected -1",
                     start_offset);

        rd_kafka_aborted_txns_pop_offset(aborted_txns, 1, INT64_MAX);
        rd_kafka_aborted_txns_pop_offset(aborted_txns, 1, INT64_MAX);
        rd_kafka_aborted_txns_pop_offset(aborted_txns, 2, INT64_MAX);

        start_offset = rd_kafka_aborted_txns_get_offset(aborted_txns, 1);
        RD_UT_ASSERT(-1 == start_offset,
                     "queried start offset was %" PRId64
                     ", "
                     "expected -1",
                     start_offset);

        start_offset = rd_kafka_aborted_txns_get_offset(aborted_txns, 2);
        RD_UT_ASSERT(-1 == start_offset,
                     "queried start offset was %" PRId64
                     ", "
                     "expected -1",
                     start_offset);

        rd_kafka_aborted_txns_destroy(aborted_txns);

        RD_UT_PASS();
}
