/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
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
#ifndef _RDKAFKA_BUF_H_
#define _RDKAFKA_BUF_H_

#include "rdkafka_int.h"
#include "rdcrc32.h"
#include "rdlist.h"
#include "rdbuf.h"
#include "rdkafka_msgbatch.h"

typedef struct rd_kafka_broker_s rd_kafka_broker_t;

#define RD_KAFKA_HEADERS_IOV_CNT 2


/**
 * Temporary buffer with memory aligned writes to accommodate
 * effective and platform safe struct writes.
 */
typedef struct rd_tmpabuf_s {
        size_t size;
        size_t of;
        char *buf;
        int failed;
        rd_bool_t assert_on_fail;
} rd_tmpabuf_t;

/**
 * @brief Initialize new tmpabuf of non-final \p size bytes.
 */
static RD_UNUSED void
rd_tmpabuf_new(rd_tmpabuf_t *tab, size_t size, rd_bool_t assert_on_fail) {
        tab->buf            = NULL;
        tab->size           = RD_ROUNDUP(size, 8);
        tab->of             = 0;
        tab->failed         = 0;
        tab->assert_on_fail = assert_on_fail;
}

/**
 * @brief Add a new allocation of \p _size bytes,
 *        rounded up to maximum word size,
 *        for \p _times times.
 */
#define rd_tmpabuf_add_alloc_times(_tab, _size, _times)                        \
        (_tab)->size += RD_ROUNDUP(_size, 8) * _times

#define rd_tmpabuf_add_alloc(_tab, _size)                                      \
        rd_tmpabuf_add_alloc_times(_tab, _size, 1)
/**
 * @brief Finalize tmpabuf pre-allocating tab->size bytes.
 */
#define rd_tmpabuf_finalize(_tab) (_tab)->buf = rd_malloc((_tab)->size)

/**
 * @brief Free memory allocated by tmpabuf
 */
static RD_UNUSED void rd_tmpabuf_destroy(rd_tmpabuf_t *tab) {
        rd_free(tab->buf);
}

/**
 * @returns 1 if a previous operation failed.
 */
static RD_UNUSED RD_INLINE int rd_tmpabuf_failed(rd_tmpabuf_t *tab) {
        return tab->failed;
}

/**
 * @brief Allocate \p size bytes for writing, returning an aligned pointer
 *        to the memory.
 * @returns the allocated pointer (within the tmpabuf) on success or
 *          NULL if the requested number of bytes + alignment is not available
 *          in the tmpabuf.
 */
static RD_UNUSED void *
rd_tmpabuf_alloc0(const char *func, int line, rd_tmpabuf_t *tab, size_t size) {
        void *ptr;

        if (unlikely(tab->failed))
                return NULL;

        if (unlikely(tab->of + size > tab->size)) {
                if (tab->assert_on_fail) {
                        fprintf(stderr,
                                "%s: %s:%d: requested size %" PRIusz
                                " + %" PRIusz " > %" PRIusz "\n",
                                __FUNCTION__, func, line, tab->of, size,
                                tab->size);
                        assert(!*"rd_tmpabuf_alloc: not enough size in buffer");
                }
                return NULL;
        }

        ptr = (void *)(tab->buf + tab->of);
        tab->of += RD_ROUNDUP(size, 8);

        return ptr;
}

#define rd_tmpabuf_alloc(tab, size)                                            \
        rd_tmpabuf_alloc0(__FUNCTION__, __LINE__, tab, size)

/**
 * @brief Write \p buf of \p size bytes to tmpabuf memory in an aligned fashion.
 *
 * @returns the allocated and written-to pointer (within the tmpabuf) on success
 *          or NULL if the requested number of bytes + alignment is not
 * available in the tmpabuf.
 */
static RD_UNUSED void *rd_tmpabuf_write0(const char *func,
                                         int line,
                                         rd_tmpabuf_t *tab,
                                         const void *buf,
                                         size_t size) {
        void *ptr = rd_tmpabuf_alloc0(func, line, tab, size);

        if (likely(ptr && size))
                memcpy(ptr, buf, size);

        return ptr;
}
#define rd_tmpabuf_write(tab, buf, size)                                       \
        rd_tmpabuf_write0(__FUNCTION__, __LINE__, tab, buf, size)


/**
 * @brief Wrapper for rd_tmpabuf_write() that takes a nul-terminated string.
 */
static RD_UNUSED char *rd_tmpabuf_write_str0(const char *func,
                                             int line,
                                             rd_tmpabuf_t *tab,
                                             const char *str) {
        return rd_tmpabuf_write0(func, line, tab, str, strlen(str) + 1);
}
#define rd_tmpabuf_write_str(tab, str)                                         \
        rd_tmpabuf_write_str0(__FUNCTION__, __LINE__, tab, str)



/**
 * Response handling callback.
 *
 * NOTE: Callbacks must check for 'err == RD_KAFKA_RESP_ERR__DESTROY'
 *       which indicates that some entity is terminating (rd_kafka_t, broker,
 *       toppar, queue, etc) and the callback may not be called in the
 *       correct thread. In this case the callback must perform just
 *       the most minimal cleanup and dont trigger any other operations.
 *
 * NOTE: rkb, reply and request may be NULL, depending on error situation.
 */
typedef void(rd_kafka_resp_cb_t)(rd_kafka_t *rk,
                                 rd_kafka_broker_t *rkb,
                                 rd_kafka_resp_err_t err,
                                 rd_kafka_buf_t *reply,
                                 rd_kafka_buf_t *request,
                                 void *opaque);


/**
 * @brief Sender callback. This callback is used to construct and send (enq)
 *        a rkbuf on a particular broker.
 */
typedef rd_kafka_resp_err_t(rd_kafka_send_req_cb_t)(rd_kafka_broker_t *rkb,
                                                    rd_kafka_op_t *rko,
                                                    rd_kafka_replyq_t replyq,
                                                    rd_kafka_resp_cb_t *resp_cb,
                                                    void *reply_opaque);


/**
 * @brief Request maker. A callback that constructs the actual contents
 *        of a request.
 *
 * When constructing a request the ApiVersion typically needs to be selected
 * which requires the broker's supported ApiVersions to be known, which in
 * turn requires the broker connection to be UP.
 *
 * As a buffer constructor you have two choices:
 *   a. acquire the broker handle, wait for it to come up, and then construct
 *      the request buffer, or
 *   b. acquire the broker handle, enqueue an uncrafted/unmaked
 *      request on the broker request queue, and when the broker is up
 *      the make_req_cb will be called for you to construct the request.
 *
 * From a code complexity standpoint, the latter option is usually the least
 * complex and voids the caller to care about any of the broker state.
 * Any information that is required to construct the request is passed through
 * the make_opaque, which can be automatically freed by the buffer code
 * when it has been used, or handled by the caller (in which case it must
 * outlive the lifetime of the buffer).
 *
 * Usage:
 *
 *  1. Construct an rkbuf with the appropriate ApiKey.
 *  2. Make a copy or reference of any data that is needed to construct the
 *     request, e.g., through rd_kafka_topic_partition_list_copy(). This
 *     data is passed by the make_opaque.
 *  3. Set the make callback by calling rd_kafka_buf_set_maker() and pass
 *     the make_opaque data and a free function, if needed.
 *  4. The callback will eventually be called from the broker thread.
 *  5. In the make callback construct the request on the passed rkbuf.
 *  6. The request is sent to the broker and the make_opaque is freed.
 *
 * See rd_kafka_ListOffsetsRequest() in rdkafka_request.c for an example.
 *
 */
typedef rd_kafka_resp_err_t(rd_kafka_make_req_cb_t)(rd_kafka_broker_t *rkb,
                                                    rd_kafka_buf_t *rkbuf,
                                                    void *make_opaque);

/**
 * @struct Request and response buffer
 *
 */
struct rd_kafka_buf_s { /* rd_kafka_buf_t */
        TAILQ_ENTRY(rd_kafka_buf_s) rkbuf_link;

        int32_t rkbuf_corrid;

        rd_ts_t rkbuf_ts_retry; /* Absolute send retry time */

        int rkbuf_flags; /* RD_KAFKA_OP_F */

        /** What convenience flags to copy from request to response along
         *  with the reqhdr. */
#define RD_KAFKA_BUF_FLAGS_RESP_COPY_MASK (RD_KAFKA_OP_F_FLEXVER)

        rd_kafka_prio_t rkbuf_prio; /**< Request priority */

        rd_buf_t rkbuf_buf;      /**< Send/Recv byte buffer */
        rd_slice_t rkbuf_reader; /**< Buffer slice reader for rkbuf_buf */

        int rkbuf_connid;    /* broker connection id (used when buffer
                              * was partially sent). */
        size_t rkbuf_totlen; /* recv: total expected length,
                              * send: not used */

        rd_crc32_t rkbuf_crc; /* Current CRC calculation */

        struct rd_kafkap_reqhdr rkbuf_reqhdr; /* Request header.
                                               * These fields are encoded
                                               * and written to output buffer
                                               * on buffer finalization.
                                               * Note:
                                               * The request's
                                               * reqhdr is copied to the
                                               * response's reqhdr as a
                                               * convenience. */
        struct rd_kafkap_reshdr rkbuf_reshdr; /* Response header.
                                               * Decoded fields are copied
                                               * here from the buffer
                                               * to provide an ease-of-use
                                               * interface to the header */

        int32_t rkbuf_expected_size; /* expected size of message */

        rd_kafka_replyq_t rkbuf_replyq;        /* Enqueue response on replyq */
        rd_kafka_replyq_t rkbuf_orig_replyq;   /* Original replyq to be used
                                                * for retries from inside
                                                * the rkbuf_cb() callback
                                                * since rkbuf_replyq will
                                                * have been reset. */
        rd_kafka_resp_cb_t *rkbuf_cb;          /* Response callback */
        struct rd_kafka_buf_s *rkbuf_response; /* Response buffer */

        rd_kafka_make_req_cb_t *rkbuf_make_req_cb; /**< Callback to construct
                                                    *   the request itself.
                                                    *   Will be used if
                                                    *   RD_KAFKA_OP_F_NEED_MAKE
                                                    *   is set. */
        void *rkbuf_make_opaque; /**< Opaque passed to rkbuf_make_req_cb.
                                  *   Will be freed automatically after use
                                  *   by the rkbuf code. */
        void (*rkbuf_free_make_opaque_cb)(void *); /**< Free function for
                                                    *   rkbuf_make_opaque. */

        struct rd_kafka_broker_s *rkbuf_rkb; /**< Optional broker object
                                              *   with refcnt increased used
                                              *   for logging decode errors
                                              *   if log_decode_errors is > 0 */

        rd_refcnt_t rkbuf_refcnt;
        void *rkbuf_opaque;

        int rkbuf_max_retries; /**< Maximum retries to attempt. */
        int rkbuf_retries;     /**< Retries so far. */


        int rkbuf_features; /* Required feature(s) that must be
                             * supported by broker. */

        rd_ts_t rkbuf_ts_enq;
        rd_ts_t rkbuf_ts_sent; /* Initially: Absolute time of transmission,
                                * after response: RTT. */

        /* Request timeouts:
         *  rkbuf_ts_timeout is the effective absolute request timeout used
         *  by the timeout scanner to see if a request has timed out.
         *  It is set when a request is enqueued on the broker transmit
         *  queue based on the relative or absolute timeout:
         *
         *  rkbuf_rel_timeout is the per-request-transmit relative timeout,
         *  this value is reused for each sub-sequent retry of a request.
         *
         *  rkbuf_abs_timeout is the absolute request timeout, spanning
         *  all retries.
         *  This value is effectively limited by socket.timeout.ms for
         *  each transmission, but the absolute timeout for a request's
         *  lifetime is the absolute value.
         *
         *  Use rd_kafka_buf_set_timeout() to set a relative timeout
         *  that will be reused on retry,
         *  or rd_kafka_buf_set_abs_timeout() to set a fixed absolute timeout
         *  for the case where the caller knows the request will be
         *  semantically outdated when that absolute time expires, such as for
         *  session.timeout.ms-based requests.
         *
         * The decision to retry a request is delegated to the rkbuf_cb
         * response callback, which should use rd_kafka_err_action()
         * and check the return actions for RD_KAFKA_ERR_ACTION_RETRY to be set
         * and then call rd_kafka_buf_retry().
         * rd_kafka_buf_retry() will enqueue the request on the rkb_retrybufs
         * queue with a backoff time of retry.backoff.ms.
         * The rkb_retrybufs queue is served by the broker thread's timeout
         * scanner.
         * @warning rkb_retrybufs is NOT purged on broker down.
         */
        rd_ts_t rkbuf_ts_timeout; /* Request timeout (absolute time). */
        rd_ts_t
            rkbuf_abs_timeout; /* Absolute timeout for request, including
                                * retries.
                                * Mutually exclusive with rkbuf_rel_timeout*/
        int rkbuf_rel_timeout; /* Relative timeout (ms), used for retries.
                                * Defaults to socket.timeout.ms.
                                * Mutually exclusive with rkbuf_abs_timeout*/
        rd_bool_t rkbuf_force_timeout; /**< Force request timeout to be
                                        *   remaining abs_timeout regardless
                                        *   of socket.timeout.ms. */


        int64_t rkbuf_offset; /* Used by OffsetCommit */

        rd_list_t *rkbuf_rktp_vers; /* Toppar + Op Version map.
                                     * Used by FetchRequest. */

        rd_kafka_resp_err_t rkbuf_err; /* Buffer parsing error code */

        union {
                struct {
                        rd_list_t *topics; /* Requested topics (char *) */
                        rd_list_t *
                            topic_ids; /* Requested topic ids rd_kafka_Uuid_t */
                        char *reason;  /* Textual reason */
                        rd_kafka_op_t *rko;    /* Originating rko with replyq
                                                * (if any) */
                        rd_bool_t all_topics;  /**< Full/All topics requested */
                        rd_bool_t cgrp_update; /**< Update cgrp with topic
                                                *   status from response. */
                        rd_bool_t force_racks; /**< Force the returned metadata
                                                *   to contain partition to
                                                *   rack mapping. */

                        int *decr; /* Decrement this integer by one
                                    * when request is complete:
                                    * typically points to metadata
                                    * cache's full_.._sent.
                                    * Will be performed with
                                    * decr_lock held. */
                        mtx_t *decr_lock;

                } Metadata;
                struct {
                        rd_kafka_msgbatch_t batch; /**< MessageSet/batch */
                } Produce;
                struct {
                        rd_bool_t commit; /**< true = txn commit,
                                           *   false = txn abort */
                } EndTxn;
        } rkbuf_u;

#define rkbuf_batch rkbuf_u.Produce.batch

        const char *rkbuf_uflow_mitigation; /**< Buffer read underflow
                                             *   human readable mitigation
                                             *   string (const memory).
                                             *   This is used to hint the
                                             *   user why the underflow
                                             *   might have occurred, which
                                             *   depends on request type. */
};



/**
 * @name Read buffer interface
 *
 * Memory reading helper macros to be used when parsing network responses.
 *
 * Assumptions:
 *   - an 'err_parse:' goto-label must be available for error bailouts,
 *                     the error code will be set in rkbuf->rkbuf_err
 *   - local `int log_decode_errors` variable set to the logging level
 *     to log parse errors (or 0 to turn off logging).
 */

#define rd_kafka_buf_parse_fail(rkbuf, ...)                                    \
        do {                                                                   \
                if (log_decode_errors > 0 && rkbuf->rkbuf_rkb) {               \
                        rd_rkb_log(                                            \
                            rkbuf->rkbuf_rkb, log_decode_errors, "PROTOERR",   \
                            "Protocol parse failure for %s v%hd%s "            \
                            "at %" PRIusz "/%" PRIusz                          \
                            " (%s:%i) "                                        \
                            "(incorrect broker.version.fallback?)",            \
                            rd_kafka_ApiKey2str(rkbuf->rkbuf_reqhdr.ApiKey),   \
                            rkbuf->rkbuf_reqhdr.ApiVersion,                    \
                            (rkbuf->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER        \
                                 ? "(flex)"                                    \
                                 : ""),                                        \
                            rd_slice_offset(&rkbuf->rkbuf_reader),             \
                            rd_slice_size(&rkbuf->rkbuf_reader), __FUNCTION__, \
                            __LINE__);                                         \
                        rd_rkb_log(rkbuf->rkbuf_rkb, log_decode_errors,        \
                                   "PROTOERR", __VA_ARGS__);                   \
                }                                                              \
                (rkbuf)->rkbuf_err = RD_KAFKA_RESP_ERR__BAD_MSG;               \
                goto err_parse;                                                \
        } while (0)

/**
 * @name Fail buffer reading due to buffer underflow.
 */
#define rd_kafka_buf_underflow_fail(rkbuf, wantedlen, ...)                     \
        do {                                                                   \
                if (log_decode_errors > 0 && rkbuf->rkbuf_rkb) {               \
                        char __tmpstr[256];                                    \
                        rd_snprintf(__tmpstr, sizeof(__tmpstr),                \
                                    ": " __VA_ARGS__);                         \
                        if (strlen(__tmpstr) == 2)                             \
                                __tmpstr[0] = '\0';                            \
                        rd_rkb_log(                                            \
                            rkbuf->rkbuf_rkb, log_decode_errors, "PROTOUFLOW", \
                            "Protocol read buffer underflow "                  \
                            "for %s v%hd "                                     \
                            "at %" PRIusz "/%" PRIusz                          \
                            " (%s:%i): "                                       \
                            "expected %" PRIusz                                \
                            " bytes > "                                        \
                            "%" PRIusz " remaining bytes (%s)%s",              \
                            rd_kafka_ApiKey2str(rkbuf->rkbuf_reqhdr.ApiKey),   \
                            rkbuf->rkbuf_reqhdr.ApiVersion,                    \
                            rd_slice_offset(&rkbuf->rkbuf_reader),             \
                            rd_slice_size(&rkbuf->rkbuf_reader), __FUNCTION__, \
                            __LINE__, wantedlen,                               \
                            rd_slice_remains(&rkbuf->rkbuf_reader),            \
                            rkbuf->rkbuf_uflow_mitigation                      \
                                ? rkbuf->rkbuf_uflow_mitigation                \
                                : "incorrect broker.version.fallback?",        \
                            __tmpstr);                                         \
                }                                                              \
                (rkbuf)->rkbuf_err = RD_KAFKA_RESP_ERR__UNDERFLOW;             \
                goto err_parse;                                                \
        } while (0)


/**
 * Returns the number of remaining bytes available to read.
 */
#define rd_kafka_buf_read_remain(rkbuf) rd_slice_remains(&(rkbuf)->rkbuf_reader)

/**
 * Checks that at least 'len' bytes remain to be read in buffer, else fails.
 */
#define rd_kafka_buf_check_len(rkbuf, len)                                     \
        do {                                                                   \
                size_t __len0 = (size_t)(len);                                 \
                if (unlikely(__len0 > rd_kafka_buf_read_remain(rkbuf))) {      \
                        rd_kafka_buf_underflow_fail(rkbuf, __len0);            \
                }                                                              \
        } while (0)

/**
 * Skip (as in read and ignore) the next 'len' bytes.
 */
#define rd_kafka_buf_skip(rkbuf, len)                                          \
        do {                                                                   \
                size_t __len1 = (size_t)(len);                                 \
                if (__len1 &&                                                  \
                    !rd_slice_read(&(rkbuf)->rkbuf_reader, NULL, __len1))      \
                        rd_kafka_buf_check_len(rkbuf, __len1);                 \
        } while (0)

/**
 * Skip (as in read and ignore) up to fixed position \p pos.
 */
#define rd_kafka_buf_skip_to(rkbuf, pos)                                       \
        do {                                                                   \
                size_t __len1 =                                                \
                    (size_t)(pos)-rd_slice_offset(&(rkbuf)->rkbuf_reader);     \
                if (__len1 &&                                                  \
                    !rd_slice_read(&(rkbuf)->rkbuf_reader, NULL, __len1))      \
                        rd_kafka_buf_check_len(rkbuf, __len1);                 \
        } while (0)



/**
 * Read 'len' bytes and copy to 'dstptr'
 */
#define rd_kafka_buf_read(rkbuf, dstptr, len)                                  \
        do {                                                                   \
                size_t __len2 = (size_t)(len);                                 \
                if (!rd_slice_read(&(rkbuf)->rkbuf_reader, dstptr, __len2))    \
                        rd_kafka_buf_check_len(rkbuf, __len2);                 \
        } while (0)


/**
 * @brief Read \p len bytes at slice offset \p offset and copy to \p dstptr
 *        without affecting the current reader position.
 */
#define rd_kafka_buf_peek(rkbuf, offset, dstptr, len)                          \
        do {                                                                   \
                size_t __len2 = (size_t)(len);                                 \
                if (!rd_slice_peek(&(rkbuf)->rkbuf_reader, offset, dstptr,     \
                                   __len2))                                    \
                        rd_kafka_buf_check_len(rkbuf, (offset) + (__len2));    \
        } while (0)


/**
 * Read a 16,32,64-bit integer and store it in 'dstptr'
 */
#define rd_kafka_buf_read_i64(rkbuf, dstptr)                                   \
        do {                                                                   \
                int64_t _v;                                                    \
                int64_t *_vp = dstptr;                                         \
                rd_kafka_buf_read(rkbuf, &_v, sizeof(_v));                     \
                *_vp = be64toh(_v);                                            \
        } while (0)

#define rd_kafka_buf_peek_i64(rkbuf, of, dstptr)                               \
        do {                                                                   \
                int64_t _v;                                                    \
                int64_t *_vp = dstptr;                                         \
                rd_kafka_buf_peek(rkbuf, of, &_v, sizeof(_v));                 \
                *_vp = be64toh(_v);                                            \
        } while (0)

#define rd_kafka_buf_read_i32(rkbuf, dstptr)                                   \
        do {                                                                   \
                int32_t _v;                                                    \
                int32_t *_vp = dstptr;                                         \
                rd_kafka_buf_read(rkbuf, &_v, sizeof(_v));                     \
                *_vp = be32toh(_v);                                            \
        } while (0)

#define rd_kafka_buf_peek_i32(rkbuf, of, dstptr)                               \
        do {                                                                   \
                int32_t _v;                                                    \
                int32_t *_vp = dstptr;                                         \
                rd_kafka_buf_peek(rkbuf, of, &_v, sizeof(_v));                 \
                *_vp = be32toh(_v);                                            \
        } while (0)


/* Same as .._read_i32 but does a direct assignment.
 * dst is assumed to be a scalar, not pointer. */
#define rd_kafka_buf_read_i32a(rkbuf, dst)                                     \
        do {                                                                   \
                int32_t _v;                                                    \
                rd_kafka_buf_read(rkbuf, &_v, 4);                              \
                dst = (int32_t)be32toh(_v);                                    \
        } while (0)

#define rd_kafka_buf_read_i16(rkbuf, dstptr)                                   \
        do {                                                                   \
                int16_t _v;                                                    \
                int16_t *_vp = dstptr;                                         \
                rd_kafka_buf_read(rkbuf, &_v, sizeof(_v));                     \
                *_vp = (int16_t)be16toh(_v);                                   \
        } while (0)

#define rd_kafka_buf_peek_i16(rkbuf, of, dstptr)                               \
        do {                                                                   \
                int16_t _v;                                                    \
                int16_t *_vp = dstptr;                                         \
                rd_kafka_buf_peek(rkbuf, of, &_v, sizeof(_v));                 \
                *_vp = be16toh(_v);                                            \
        } while (0)

#define rd_kafka_buf_read_i16a(rkbuf, dst)                                     \
        do {                                                                   \
                int16_t _v;                                                    \
                rd_kafka_buf_read(rkbuf, &_v, 2);                              \
                dst = (int16_t)be16toh(_v);                                    \
        } while (0)

#define rd_kafka_buf_read_i8(rkbuf, dst) rd_kafka_buf_read(rkbuf, dst, 1)

#define rd_kafka_buf_peek_i8(rkbuf, of, dst)                                   \
        rd_kafka_buf_peek(rkbuf, of, dst, 1)

#define rd_kafka_buf_read_bool(rkbuf, dstptr)                                  \
        do {                                                                   \
                int8_t _v;                                                     \
                rd_bool_t *_dst = dstptr;                                      \
                rd_kafka_buf_read(rkbuf, &_v, 1);                              \
                *_dst = (rd_bool_t)_v;                                         \
        } while (0)


/**
 * @brief Read varint and store in int64_t \p dst
 */
#define rd_kafka_buf_read_varint(rkbuf, dstptr)                                \
        do {                                                                   \
                int64_t _v;                                                    \
                int64_t *_vp = dstptr;                                         \
                size_t _r = rd_slice_read_varint(&(rkbuf)->rkbuf_reader, &_v); \
                if (unlikely(RD_UVARINT_UNDERFLOW(_r)))                        \
                        rd_kafka_buf_underflow_fail(rkbuf, (size_t)0,          \
                                                    "varint parsing failed");  \
                *_vp = _v;                                                     \
        } while (0)


/**
 * @brief Read unsigned varint and store in uint64_t \p dst
 */
#define rd_kafka_buf_read_uvarint(rkbuf, dstptr)                               \
        do {                                                                   \
                uint64_t _v;                                                   \
                uint64_t *_vp = dstptr;                                        \
                size_t _r =                                                    \
                    rd_slice_read_uvarint(&(rkbuf)->rkbuf_reader, &_v);        \
                if (unlikely(RD_UVARINT_UNDERFLOW(_r)))                        \
                        rd_kafka_buf_underflow_fail(rkbuf, (size_t)0,          \
                                                    "uvarint parsing failed"); \
                *_vp = _v;                                                     \
        } while (0)


/**
 * @brief Read Kafka COMPACT_STRING (VARINT+N) or
 *        standard String representation (2+N).
 *
 * The kstr data will be updated to point to the rkbuf. */
#define rd_kafka_buf_read_str(rkbuf, kstr)                                     \
        do {                                                                   \
                int _klen;                                                     \
                if ((rkbuf)->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER) {            \
                        uint64_t _uva;                                         \
                        rd_kafka_buf_read_uvarint(rkbuf, &_uva);               \
                        (kstr)->len = ((int32_t)_uva) - 1;                     \
                        _klen       = (kstr)->len;                             \
                } else {                                                       \
                        rd_kafka_buf_read_i16a(rkbuf, (kstr)->len);            \
                        _klen = RD_KAFKAP_STR_LEN(kstr);                       \
                }                                                              \
                if (RD_KAFKAP_STR_IS_NULL(kstr))                               \
                        (kstr)->str = NULL;                                    \
                else if (RD_KAFKAP_STR_LEN(kstr) == 0)                         \
                        (kstr)->str = "";                                      \
                else if (!((kstr)->str = rd_slice_ensure_contig(               \
                               &rkbuf->rkbuf_reader, _klen)))                  \
                        rd_kafka_buf_check_len(rkbuf, _klen);                  \
        } while (0)

/* Read Kafka String representation (2+N) and write it to the \p tmpabuf
 * with a trailing nul byte. */
#define rd_kafka_buf_read_str_tmpabuf(rkbuf, tmpabuf, dst)                     \
        do {                                                                   \
                rd_kafkap_str_t _kstr;                                         \
                size_t _slen;                                                  \
                char *_dst;                                                    \
                rd_kafka_buf_read_str(rkbuf, &_kstr);                          \
                if (RD_KAFKAP_STR_IS_NULL(&_kstr)) {                           \
                        dst = NULL;                                            \
                        break;                                                 \
                }                                                              \
                _slen = RD_KAFKAP_STR_LEN(&_kstr);                             \
                if (!(_dst = rd_tmpabuf_write(tmpabuf, _kstr.str, _slen + 1))) \
                        rd_kafka_buf_parse_fail(                               \
                            rkbuf,                                             \
                            "Not enough room in tmpabuf: "                     \
                            "%" PRIusz "+%" PRIusz " > %" PRIusz,              \
                            (tmpabuf)->of, _slen + 1, (tmpabuf)->size);        \
                _dst[_slen] = '\0';                                            \
                dst         = (void *)_dst;                                    \
        } while (0)

/**
 * Skip a string without flexver.
 */
#define rd_kafka_buf_skip_str_no_flexver(rkbuf)                                \
        do {                                                                   \
                int16_t _slen;                                                 \
                rd_kafka_buf_read_i16(rkbuf, &_slen);                          \
                rd_kafka_buf_skip(rkbuf, RD_KAFKAP_STR_LEN0(_slen));           \
        } while (0)

/**
 * Skip a string (generic).
 */
#define rd_kafka_buf_skip_str(rkbuf)                                           \
        do {                                                                   \
                if ((rkbuf)->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER) {            \
                        uint64_t _uva;                                         \
                        rd_kafka_buf_read_uvarint(rkbuf, &_uva);               \
                        rd_kafka_buf_skip(                                     \
                            rkbuf, RD_KAFKAP_STR_LEN0(((int64_t)_uva) - 1));   \
                } else {                                                       \
                        rd_kafka_buf_skip_str_no_flexver(rkbuf);               \
                }                                                              \
        } while (0)
/**
 * Read Kafka COMPACT_BYTES representation (VARINT+N) or
 * standard BYTES representation(4+N).
 * The 'kbytes' will be updated to point to rkbuf data.
 */
#define rd_kafka_buf_read_kbytes(rkbuf, kbytes)                                \
        do {                                                                   \
                int32_t _klen;                                                 \
                if (!(rkbuf->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER)) {           \
                        rd_kafka_buf_read_i32a(rkbuf, _klen);                  \
                } else {                                                       \
                        uint64_t _uva;                                         \
                        rd_kafka_buf_read_uvarint(rkbuf, &_uva);               \
                        _klen = ((int32_t)_uva) - 1;                           \
                }                                                              \
                (kbytes)->len = _klen;                                         \
                if (RD_KAFKAP_BYTES_IS_NULL(kbytes)) {                         \
                        (kbytes)->data = NULL;                                 \
                        (kbytes)->len  = 0;                                    \
                } else if (RD_KAFKAP_BYTES_LEN(kbytes) == 0)                   \
                        (kbytes)->data = "";                                   \
                else if (!((kbytes)->data = rd_slice_ensure_contig(            \
                               &(rkbuf)->rkbuf_reader, _klen)))                \
                        rd_kafka_buf_check_len(rkbuf, _klen);                  \
        } while (0)

/**
 * @brief Read \p size bytes from buffer, setting \p *ptr to the start
 *        of the memory region.
 */
#define rd_kafka_buf_read_ptr(rkbuf, ptr, size)                                \
        do {                                                                   \
                size_t _klen = size;                                           \
                if (!(*(ptr) = (void *)rd_slice_ensure_contig(                 \
                          &(rkbuf)->rkbuf_reader, _klen)))                     \
                        rd_kafka_buf_check_len(rkbuf, _klen);                  \
        } while (0)


/**
 * @brief Read varint-lengted Kafka Bytes representation
 */
#define rd_kafka_buf_read_kbytes_varint(rkbuf, kbytes)                         \
        do {                                                                   \
                int64_t _len2;                                                 \
                size_t _r =                                                    \
                    rd_slice_read_varint(&(rkbuf)->rkbuf_reader, &_len2);      \
                if (unlikely(RD_UVARINT_UNDERFLOW(_r)))                        \
                        rd_kafka_buf_underflow_fail(rkbuf, (size_t)0,          \
                                                    "varint parsing failed");  \
                (kbytes)->len = (int32_t)_len2;                                \
                if (RD_KAFKAP_BYTES_IS_NULL(kbytes)) {                         \
                        (kbytes)->data = NULL;                                 \
                        (kbytes)->len  = 0;                                    \
                } else if (RD_KAFKAP_BYTES_LEN(kbytes) == 0)                   \
                        (kbytes)->data = "";                                   \
                else if (!((kbytes)->data = rd_slice_ensure_contig(            \
                               &(rkbuf)->rkbuf_reader, (size_t)_len2)))        \
                        rd_kafka_buf_check_len(rkbuf, _len2);                  \
        } while (0)


/**
 * @brief Read throttle_time_ms (i32) from response and pass the value
 *        to the throttle handling code.
 */
#define rd_kafka_buf_read_throttle_time(rkbuf)                                 \
        do {                                                                   \
                int32_t _throttle_time_ms;                                     \
                rd_kafka_buf_read_i32(rkbuf, &_throttle_time_ms);              \
                rd_kafka_op_throttle_time((rkbuf)->rkbuf_rkb,                  \
                                          (rkbuf)->rkbuf_rkb->rkb_rk->rk_rep,  \
                                          _throttle_time_ms);                  \
        } while (0)


/**
 * @brief Discard all KIP-482 Tags at the current position in the buffer.
 */
#define rd_kafka_buf_skip_tags(rkbuf)                                          \
        do {                                                                   \
                uint64_t _tagcnt;                                              \
                if (!((rkbuf)->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER))           \
                        break;                                                 \
                rd_kafka_buf_read_uvarint(rkbuf, &_tagcnt);                    \
                while (_tagcnt-- > 0) {                                        \
                        uint64_t _tagtype, _taglen;                            \
                        rd_kafka_buf_read_uvarint(rkbuf, &_tagtype);           \
                        rd_kafka_buf_read_uvarint(rkbuf, &_taglen);            \
                        if (_taglen > 0)                                       \
                                rd_kafka_buf_skip(rkbuf, (size_t)(_taglen));   \
                }                                                              \
        } while (0)

/**
 * @brief Read KIP-482 Tags at current position in the buffer using
 *        the `read_tag` function receiving the `opaque' pointer.
 */
#define rd_kafka_buf_read_tags(rkbuf, read_tag, ...)                           \
        do {                                                                   \
                uint64_t _tagcnt;                                              \
                if (!((rkbuf)->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER))           \
                        break;                                                 \
                rd_kafka_buf_read_uvarint(rkbuf, &_tagcnt);                    \
                while (_tagcnt-- > 0) {                                        \
                        uint64_t _tagtype, _taglen;                            \
                        rd_kafka_buf_read_uvarint(rkbuf, &_tagtype);           \
                        rd_kafka_buf_read_uvarint(rkbuf, &_taglen);            \
                        int _read_tag_resp =                                   \
                            read_tag(rkbuf, _tagtype, _taglen, __VA_ARGS__);   \
                        if (_read_tag_resp == -1)                              \
                                goto err_parse;                                \
                        if (!_read_tag_resp && _taglen > 0)                    \
                                rd_kafka_buf_skip(rkbuf, (size_t)(_taglen));   \
                }                                                              \
        } while (0)

/**
 * @brief Write \p tagcnt tags at the current position in the buffer.
 * Calling \p write_tag to write each one with \p rkbuf , tagtype
 * argument and the remaining arguments.
 */
#define rd_kafka_buf_write_tags(rkbuf, write_tag, tags, tagcnt, ...)           \
        do {                                                                   \
                uint64_t i;                                                    \
                if (!((rkbuf)->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER))           \
                        break;                                                 \
                rd_kafka_buf_write_uvarint(rkbuf, tagcnt);                     \
                for (i = 0; i < tagcnt; i++) {                                 \
                        size_t of_taglen, prev_buf_len;                        \
                        rd_kafka_buf_write_uvarint(rkbuf, tags[i]);            \
                        of_taglen    = rd_kafka_buf_write_arraycnt_pos(rkbuf); \
                        prev_buf_len = (rkbuf)->rkbuf_buf.rbuf_len;            \
                        write_tag(rkbuf, tags[i], __VA_ARGS__);                \
                        rd_kafka_buf_finalize_arraycnt(                        \
                            rkbuf, of_taglen,                                  \
                            (rkbuf)->rkbuf_buf.rbuf_len - prev_buf_len - 1);   \
                }                                                              \
        } while (0)


/**
 * @brief Write empty tags at the current position in the buffer.
 */
#define rd_kafka_buf_write_tags_empty(rkbuf)                                   \
        do {                                                                   \
                if (!((rkbuf)->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER))           \
                        break;                                                 \
                rd_kafka_buf_write_i8(rkbuf, 0);                               \
        } while (0)


/**
 * @brief Reads an ARRAY or COMPACT_ARRAY count depending on buffer type.
 */
#define rd_kafka_buf_read_arraycnt(rkbuf, arrcnt, maxval)                      \
        do {                                                                   \
                if ((rkbuf)->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER) {            \
                        uint64_t _uva;                                         \
                        rd_kafka_buf_read_uvarint(rkbuf, &_uva);               \
                        *(arrcnt) = (int32_t)_uva - 1;                         \
                } else {                                                       \
                        rd_kafka_buf_read_i32(rkbuf, arrcnt);                  \
                }                                                              \
                if (*(arrcnt) < -1 ||                                          \
                    ((maxval) != -1 && *(arrcnt) > (maxval)))                  \
                        rd_kafka_buf_parse_fail(                               \
                            rkbuf, "ApiArrayCnt %" PRId32 " out of range",     \
                            *(arrcnt));                                        \
        } while (0)



/**
 * @returns true if buffer has been sent on wire, else 0.
 */
#define rd_kafka_buf_was_sent(rkbuf) ((rkbuf)->rkbuf_flags & RD_KAFKA_OP_F_SENT)

typedef struct rd_kafka_bufq_s {
        TAILQ_HEAD(, rd_kafka_buf_s) rkbq_bufs;
        rd_atomic32_t rkbq_cnt;
        rd_atomic32_t rkbq_msg_cnt;
} rd_kafka_bufq_t;

#define rd_kafka_bufq_cnt(rkbq) rd_atomic32_get(&(rkbq)->rkbq_cnt)

/**
 * @brief Set buffer's request timeout to relative \p timeout_ms measured
 *        from the time the buffer is sent on the underlying socket.
 *
 * @param now Reuse current time from existing rd_clock() var, else 0.
 *
 * The relative timeout value is reused upon request retry.
 */
static RD_INLINE void
rd_kafka_buf_set_timeout(rd_kafka_buf_t *rkbuf, int timeout_ms, rd_ts_t now) {
        if (!now)
                now = rd_clock();
        rkbuf->rkbuf_rel_timeout = timeout_ms;
        rkbuf->rkbuf_abs_timeout = 0;
}


/**
 * @brief Calculate the effective timeout for a request attempt
 */
void rd_kafka_buf_calc_timeout(const rd_kafka_t *rk,
                               rd_kafka_buf_t *rkbuf,
                               rd_ts_t now);


/**
 * @brief Set buffer's request timeout to relative \p timeout_ms measured
 *        from \p now.
 *
 * @param now Reuse current time from existing rd_clock() var, else 0.
 * @param force If true: force request timeout to be same as remaining
 *                       abs timeout, regardless of socket.timeout.ms.
 *              If false: cap each request timeout to socket.timeout.ms.
 *
 * The remaining time is used as timeout for request retries.
 */
static RD_INLINE void rd_kafka_buf_set_abs_timeout0(rd_kafka_buf_t *rkbuf,
                                                    int timeout_ms,
                                                    rd_ts_t now,
                                                    rd_bool_t force) {
        if (!now)
                now = rd_clock();
        rkbuf->rkbuf_rel_timeout   = 0;
        rkbuf->rkbuf_abs_timeout   = now + ((rd_ts_t)timeout_ms * 1000);
        rkbuf->rkbuf_force_timeout = force;
}

#define rd_kafka_buf_set_abs_timeout(rkbuf, timeout_ms, now)                   \
        rd_kafka_buf_set_abs_timeout0(rkbuf, timeout_ms, now, rd_false)


#define rd_kafka_buf_set_abs_timeout_force(rkbuf, timeout_ms, now)             \
        rd_kafka_buf_set_abs_timeout0(rkbuf, timeout_ms, now, rd_true)


#define rd_kafka_buf_keep(rkbuf) rd_refcnt_add(&(rkbuf)->rkbuf_refcnt)
#define rd_kafka_buf_destroy(rkbuf)                                            \
        rd_refcnt_destroywrapper(&(rkbuf)->rkbuf_refcnt,                       \
                                 rd_kafka_buf_destroy_final(rkbuf))

void rd_kafka_buf_destroy_final(rd_kafka_buf_t *rkbuf);
void rd_kafka_buf_push0(rd_kafka_buf_t *rkbuf,
                        const void *buf,
                        size_t len,
                        int allow_crc_calc,
                        void (*free_cb)(void *));
#define rd_kafka_buf_push(rkbuf, buf, len, free_cb)                            \
        rd_kafka_buf_push0(rkbuf, buf, len, 1 /*allow_crc*/, free_cb)
rd_kafka_buf_t *rd_kafka_buf_new0(int segcnt, size_t size, int flags);
#define rd_kafka_buf_new(segcnt, size) rd_kafka_buf_new0(segcnt, size, 0)
rd_kafka_buf_t *rd_kafka_buf_new_request0(rd_kafka_broker_t *rkb,
                                          int16_t ApiKey,
                                          int segcnt,
                                          size_t size,
                                          rd_bool_t is_flexver);
#define rd_kafka_buf_new_request(rkb, ApiKey, segcnt, size)                    \
        rd_kafka_buf_new_request0(rkb, ApiKey, segcnt, size, rd_false)

#define rd_kafka_buf_new_flexver_request(rkb, ApiKey, segcnt, size,            \
                                         is_flexver)                           \
        rd_kafka_buf_new_request0(rkb, ApiKey, segcnt, size, is_flexver)
void rd_kafka_buf_upgrade_flexver_request(rd_kafka_buf_t *rkbuf);

rd_kafka_buf_t *
rd_kafka_buf_new_shadow(const void *ptr, size_t size, void (*free_cb)(void *));
void rd_kafka_bufq_enq(rd_kafka_bufq_t *rkbufq, rd_kafka_buf_t *rkbuf);
void rd_kafka_bufq_deq(rd_kafka_bufq_t *rkbufq, rd_kafka_buf_t *rkbuf);
void rd_kafka_bufq_init(rd_kafka_bufq_t *rkbufq);
void rd_kafka_bufq_concat(rd_kafka_bufq_t *dst, rd_kafka_bufq_t *src);
void rd_kafka_bufq_purge(rd_kafka_broker_t *rkb,
                         rd_kafka_bufq_t *rkbufq,
                         rd_kafka_resp_err_t err);
void rd_kafka_bufq_connection_reset(rd_kafka_broker_t *rkb,
                                    rd_kafka_bufq_t *rkbufq);
void rd_kafka_bufq_dump(rd_kafka_broker_t *rkb,
                        const char *fac,
                        rd_kafka_bufq_t *rkbq);

int rd_kafka_buf_retry(rd_kafka_broker_t *rkb, rd_kafka_buf_t *rkbuf);

void rd_kafka_buf_handle_op(rd_kafka_op_t *rko, rd_kafka_resp_err_t err);
void rd_kafka_buf_callback(rd_kafka_t *rk,
                           rd_kafka_broker_t *rkb,
                           rd_kafka_resp_err_t err,
                           rd_kafka_buf_t *response,
                           rd_kafka_buf_t *request);



/**
 *
 * Write buffer interface
 *
 */

/**
 * Set request API type version
 */
static RD_UNUSED RD_INLINE void
rd_kafka_buf_ApiVersion_set(rd_kafka_buf_t *rkbuf,
                            int16_t version,
                            int features) {
        rkbuf->rkbuf_reqhdr.ApiVersion = version;
        rkbuf->rkbuf_features          = features;
}


/**
 * @returns the ApiVersion for a request
 */
#define rd_kafka_buf_ApiVersion(rkbuf) ((rkbuf)->rkbuf_reqhdr.ApiVersion)



/**
 * Write (copy) data to buffer at current write-buffer position.
 * There must be enough space allocated in the rkbuf.
 * Returns offset to written destination buffer.
 */
static RD_INLINE size_t rd_kafka_buf_write(rd_kafka_buf_t *rkbuf,
                                           const void *data,
                                           size_t len) {
        size_t r;

        r = rd_buf_write(&rkbuf->rkbuf_buf, data, len);

        if (rkbuf->rkbuf_flags & RD_KAFKA_OP_F_CRC)
                rkbuf->rkbuf_crc = rd_crc32_update(rkbuf->rkbuf_crc, data, len);

        return r;
}



/**
 * Write (copy) 'data' to buffer at 'ptr'.
 * There must be enough space to fit 'len'.
 * This will overwrite the buffer at given location and length.
 *
 * NOTE: rd_kafka_buf_update() MUST NOT be called when a CRC calculation
 *       is in progress (between rd_kafka_buf_crc_init() & .._crc_finalize())
 */
static RD_INLINE void rd_kafka_buf_update(rd_kafka_buf_t *rkbuf,
                                          size_t of,
                                          const void *data,
                                          size_t len) {
        rd_kafka_assert(NULL, !(rkbuf->rkbuf_flags & RD_KAFKA_OP_F_CRC));
        rd_buf_write_update(&rkbuf->rkbuf_buf, of, data, len);
}

/**
 * Write int8_t to buffer.
 */
static RD_INLINE size_t rd_kafka_buf_write_i8(rd_kafka_buf_t *rkbuf, int8_t v) {
        return rd_kafka_buf_write(rkbuf, &v, sizeof(v));
}

/**
 * Update int8_t in buffer at offset 'of'.
 * 'of' should have been previously returned by `.._buf_write_i8()`.
 */
static RD_INLINE void
rd_kafka_buf_update_i8(rd_kafka_buf_t *rkbuf, size_t of, int8_t v) {
        rd_kafka_buf_update(rkbuf, of, &v, sizeof(v));
}

/**
 * Write int16_t to buffer.
 * The value will be endian-swapped before write.
 */
static RD_INLINE size_t rd_kafka_buf_write_i16(rd_kafka_buf_t *rkbuf,
                                               int16_t v) {
        v = htobe16(v);
        return rd_kafka_buf_write(rkbuf, &v, sizeof(v));
}

/**
 * Update int16_t in buffer at offset 'of'.
 * 'of' should have been previously returned by `.._buf_write_i16()`.
 */
static RD_INLINE void
rd_kafka_buf_update_i16(rd_kafka_buf_t *rkbuf, size_t of, int16_t v) {
        v = htobe16(v);
        rd_kafka_buf_update(rkbuf, of, &v, sizeof(v));
}

/**
 * Write int32_t to buffer.
 * The value will be endian-swapped before write.
 */
static RD_INLINE size_t rd_kafka_buf_write_i32(rd_kafka_buf_t *rkbuf,
                                               int32_t v) {
        v = (int32_t)htobe32(v);
        return rd_kafka_buf_write(rkbuf, &v, sizeof(v));
}

/**
 * Update int32_t in buffer at offset 'of'.
 * 'of' should have been previously returned by `.._buf_write_i32()`.
 */
static RD_INLINE void
rd_kafka_buf_update_i32(rd_kafka_buf_t *rkbuf, size_t of, int32_t v) {
        v = htobe32(v);
        rd_kafka_buf_update(rkbuf, of, &v, sizeof(v));
}

/**
 * Update int32_t in buffer at offset 'of'.
 * 'of' should have been previously returned by `.._buf_write_i32()`.
 */
static RD_INLINE void
rd_kafka_buf_update_u32(rd_kafka_buf_t *rkbuf, size_t of, uint32_t v) {
        v = htobe32(v);
        rd_kafka_buf_update(rkbuf, of, &v, sizeof(v));
}


/**
 * @brief Write varint-encoded signed value to buffer.
 */
static RD_INLINE size_t rd_kafka_buf_write_varint(rd_kafka_buf_t *rkbuf,
                                                  int64_t v) {
        char varint[RD_UVARINT_ENC_SIZEOF(v)];
        size_t sz;

        sz = rd_uvarint_enc_i64(varint, sizeof(varint), v);

        return rd_kafka_buf_write(rkbuf, varint, sz);
}

/**
 * @brief Write varint-encoded unsigned value to buffer.
 */
static RD_INLINE size_t rd_kafka_buf_write_uvarint(rd_kafka_buf_t *rkbuf,
                                                   uint64_t v) {
        char varint[RD_UVARINT_ENC_SIZEOF(v)];
        size_t sz;

        sz = rd_uvarint_enc_u64(varint, sizeof(varint), v);

        return rd_kafka_buf_write(rkbuf, varint, sz);
}



/**
 * @brief Write standard or flexver arround count field to buffer.
 *        Use this when the array count is known beforehand, else use
 *        rd_kafka_buf_write_arraycnt_pos().
 */
static RD_INLINE RD_UNUSED size_t
rd_kafka_buf_write_arraycnt(rd_kafka_buf_t *rkbuf, size_t cnt) {

        /* Count must fit in 31-bits minus the per-byte carry-bit */
        rd_assert(cnt + 1 < (size_t)(INT_MAX >> 4));

        if (!(rkbuf->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER))
                return rd_kafka_buf_write_i32(rkbuf, (int32_t)cnt);

        /* CompactArray has a base of 1, 0 is for Null arrays */
        cnt += 1;
        return rd_kafka_buf_write_uvarint(rkbuf, (uint64_t)cnt);
}


/**
 * @brief Write array count field to buffer (i32) for later update with
 *        rd_kafka_buf_finalize_arraycnt().
 */
#define rd_kafka_buf_write_arraycnt_pos(rkbuf) rd_kafka_buf_write_i32(rkbuf, 0)


/**
 * @brief Write the final array count to the position returned from
 *        rd_kafka_buf_write_arraycnt_pos().
 *
 * Update int32_t in buffer at offset 'of' but serialize it as
 * compact uvarint (that must not exceed 4 bytes storage)
 * if the \p rkbuf is marked as FLEXVER, else just update it as
 * as a standard update_i32().
 *
 * @remark For flexibleVersions this will shrink the buffer and move data
 *         and may thus be costly.
 */
static RD_INLINE void
rd_kafka_buf_finalize_arraycnt(rd_kafka_buf_t *rkbuf, size_t of, size_t cnt) {
        char buf[sizeof(int32_t)];
        size_t sz, r;

        rd_assert(cnt < (size_t)INT_MAX);

        if (!(rkbuf->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER)) {
                rd_kafka_buf_update_i32(rkbuf, of, (int32_t)cnt);
                return;
        }

        /* CompactArray has a base of 1, 0 is for Null arrays */
        cnt += 1;

        sz = rd_uvarint_enc_u64(buf, sizeof(buf), (uint64_t)cnt);
        rd_assert(!RD_UVARINT_OVERFLOW(sz));
        if (cnt < 127)
                rd_assert(sz == 1);
        rd_buf_write_update(&rkbuf->rkbuf_buf, of, buf, sz);

        if (sz < sizeof(int32_t)) {
                /* Varint occupies less space than the allotted 4 bytes, erase
                 * the remaining bytes. */
                r = rd_buf_erase(&rkbuf->rkbuf_buf, of + sz,
                                 sizeof(int32_t) - sz);
                rd_assert(r == sizeof(int32_t) - sz);
        }
}


/**
 * Write int64_t to buffer.
 * The value will be endian-swapped before write.
 */
static RD_INLINE size_t rd_kafka_buf_write_i64(rd_kafka_buf_t *rkbuf,
                                               int64_t v) {
        v = htobe64(v);
        return rd_kafka_buf_write(rkbuf, &v, sizeof(v));
}

/**
 * Update int64_t in buffer at address 'ptr'.
 * 'of' should have been previously returned by `.._buf_write_i64()`.
 */
static RD_INLINE void
rd_kafka_buf_update_i64(rd_kafka_buf_t *rkbuf, size_t of, int64_t v) {
        v = htobe64(v);
        rd_kafka_buf_update(rkbuf, of, &v, sizeof(v));
}

/**
 * @brief Write standard (2-byte header) or KIP-482 COMPACT_STRING to buffer.
 *
 * @remark Copies the string.
 *
 * @returns the offset in \p rkbuf where the string was written.
 */
static RD_INLINE size_t rd_kafka_buf_write_kstr(rd_kafka_buf_t *rkbuf,
                                                const rd_kafkap_str_t *kstr) {
        size_t len, r;

        if (!(rkbuf->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER)) {
                /* Standard string */
                if (!kstr || RD_KAFKAP_STR_IS_NULL(kstr))
                        return rd_kafka_buf_write_i16(rkbuf, -1);

                if (RD_KAFKAP_STR_IS_SERIALIZED(kstr))
                        return rd_kafka_buf_write(rkbuf,
                                                  RD_KAFKAP_STR_SER(kstr),
                                                  RD_KAFKAP_STR_SIZE(kstr));

                len = RD_KAFKAP_STR_LEN(kstr);
                r   = rd_kafka_buf_write_i16(rkbuf, (int16_t)len);
                rd_kafka_buf_write(rkbuf, kstr->str, len);

                return r;
        }

        /* COMPACT_STRING lengths are:
         *  0   = NULL,
         *  1   = empty
         *  N.. = length + 1
         */
        if (!kstr || RD_KAFKAP_STR_IS_NULL(kstr))
                len = 0;
        else
                len = RD_KAFKAP_STR_LEN(kstr) + 1;

        r = rd_kafka_buf_write_uvarint(rkbuf, (uint64_t)len);
        if (len > 1)
                rd_kafka_buf_write(rkbuf, kstr->str, len - 1);
        return r;
}



/**
 * @brief Write standard (2-byte header) or KIP-482 COMPACT_STRING to buffer.
 *
 * @remark Copies the string.
 */
static RD_INLINE size_t rd_kafka_buf_write_str(rd_kafka_buf_t *rkbuf,
                                               const char *str,
                                               size_t len) {
        size_t r;

        if (!(rkbuf->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER)) {
                /* Standard string */
                if (!str)
                        len = RD_KAFKAP_STR_LEN_NULL;
                else if (len == (size_t)-1)
                        len = strlen(str);
                r = rd_kafka_buf_write_i16(rkbuf, (int16_t)len);
                if (str)
                        rd_kafka_buf_write(rkbuf, str, len);
                return r;
        }

        /* COMPACT_STRING lengths are:
         *  0   = NULL,
         *  1   = empty
         *  N.. = length + 1
         */
        if (!str)
                len = 0;
        else if (len == (size_t)-1)
                len = strlen(str) + 1;
        else
                len++;

        r = rd_kafka_buf_write_uvarint(rkbuf, (uint64_t)len);
        if (len > 1)
                rd_kafka_buf_write(rkbuf, str, len - 1);
        return r;
}



/**
 * Push (i.e., no copy) Kafka string to buffer iovec
 */
static RD_INLINE void rd_kafka_buf_push_kstr(rd_kafka_buf_t *rkbuf,
                                             const rd_kafkap_str_t *kstr) {
        rd_kafka_buf_push(rkbuf, RD_KAFKAP_STR_SER(kstr),
                          RD_KAFKAP_STR_SIZE(kstr), NULL);
}



/**
 * Write (copy) Kafka bytes to buffer.
 */
static RD_INLINE size_t
rd_kafka_buf_write_kbytes(rd_kafka_buf_t *rkbuf,
                          const rd_kafkap_bytes_t *kbytes) {
        size_t len, r;

        if (!(rkbuf->rkbuf_flags & RD_KAFKA_OP_F_FLEXVER)) {
                if (!kbytes || RD_KAFKAP_BYTES_IS_NULL(kbytes))
                        return rd_kafka_buf_write_i32(rkbuf, -1);

                if (RD_KAFKAP_BYTES_IS_SERIALIZED(kbytes))
                        return rd_kafka_buf_write(rkbuf,
                                                  RD_KAFKAP_BYTES_SER(kbytes),
                                                  RD_KAFKAP_BYTES_SIZE(kbytes));

                len = RD_KAFKAP_BYTES_LEN(kbytes);
                rd_kafka_buf_write_i32(rkbuf, (int32_t)len);
                rd_kafka_buf_write(rkbuf, kbytes->data, len);

                return 4 + len;
        }

        /* COMPACT_BYTES lengths are:
         *  0   = NULL,
         *  1   = empty
         *  N.. = length + 1
         */
        if (!kbytes)
                len = 0;
        else
                len = kbytes->len + 1;

        r = rd_kafka_buf_write_uvarint(rkbuf, (uint64_t)len);
        if (len > 1) {
                rd_kafka_buf_write(rkbuf, kbytes->data, len - 1);
                r += len - 1;
        }
        return r;
}

/**
 * Write (copy) binary bytes to buffer as Kafka bytes encapsulate data.
 */
static RD_INLINE size_t rd_kafka_buf_write_bytes(rd_kafka_buf_t *rkbuf,
                                                 const void *payload,
                                                 size_t size) {
        size_t r;
        if (!payload)
                size = RD_KAFKAP_BYTES_LEN_NULL;
        r = rd_kafka_buf_write_i32(rkbuf, (int32_t)size);
        if (payload)
                rd_kafka_buf_write(rkbuf, payload, size);
        return r;
}


/**
 * @brief Write bool to buffer.
 */
static RD_INLINE size_t rd_kafka_buf_write_bool(rd_kafka_buf_t *rkbuf,
                                                rd_bool_t v) {
        return rd_kafka_buf_write_i8(rkbuf, (int8_t)v);
}


/**
 * Write Kafka Message to buffer
 * The number of bytes written is returned in '*outlenp'.
 *
 * Returns the buffer offset of the first byte.
 */
size_t rd_kafka_buf_write_Message(rd_kafka_broker_t *rkb,
                                  rd_kafka_buf_t *rkbuf,
                                  int64_t Offset,
                                  int8_t MagicByte,
                                  int8_t Attributes,
                                  int64_t Timestamp,
                                  const void *key,
                                  int32_t key_len,
                                  const void *payload,
                                  int32_t len,
                                  int *outlenp);

/**
 * Start calculating CRC from now and track it in '*crcp'.
 */
static RD_INLINE RD_UNUSED void rd_kafka_buf_crc_init(rd_kafka_buf_t *rkbuf) {
        rd_kafka_assert(NULL, !(rkbuf->rkbuf_flags & RD_KAFKA_OP_F_CRC));
        rkbuf->rkbuf_flags |= RD_KAFKA_OP_F_CRC;
        rkbuf->rkbuf_crc = rd_crc32_init();
}

/**
 * Finalizes CRC calculation and returns the calculated checksum.
 */
static RD_INLINE RD_UNUSED rd_crc32_t
rd_kafka_buf_crc_finalize(rd_kafka_buf_t *rkbuf) {
        rkbuf->rkbuf_flags &= ~RD_KAFKA_OP_F_CRC;
        return rd_crc32_finalize(rkbuf->rkbuf_crc);
}



/**
 * @brief Check if buffer's replyq.version is outdated.
 * @param rkbuf: may be NULL, for convenience.
 *
 * @returns 1 if this is an outdated buffer, else 0.
 */
static RD_UNUSED RD_INLINE int
rd_kafka_buf_version_outdated(const rd_kafka_buf_t *rkbuf, int version) {
        return rkbuf && rkbuf->rkbuf_replyq.version &&
               rkbuf->rkbuf_replyq.version < version;
}


void rd_kafka_buf_set_maker(rd_kafka_buf_t *rkbuf,
                            rd_kafka_make_req_cb_t *make_cb,
                            void *make_opaque,
                            void (*free_make_opaque_cb)(void *make_opaque));


#define rd_kafka_buf_read_uuid(rkbuf, uuid)                                    \
        do {                                                                   \
                rd_kafka_buf_read_i64(rkbuf,                                   \
                                      &((uuid)->most_significant_bits));       \
                rd_kafka_buf_read_i64(rkbuf,                                   \
                                      &((uuid)->least_significant_bits));      \
                (uuid)->base64str[0] = '\0';                                   \
        } while (0)

static RD_UNUSED void rd_kafka_buf_write_uuid(rd_kafka_buf_t *rkbuf,
                                              rd_kafka_Uuid_t *uuid) {
        rd_kafka_buf_write_i64(rkbuf, uuid->most_significant_bits);
        rd_kafka_buf_write_i64(rkbuf, uuid->least_significant_bits);
}

#endif /* _RDKAFKA_BUF_H_ */
