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
#include "rdkafka_lz4.h"

#if WITH_KLZ4_EXT
#include <lz4frame.h>
#else
#include "lz4frame.h"
#endif
#include "rdxxhash.h"

#include "rdbuf.h"

/**
 * Fix-up bad KLZ4 framing caused by buggy Kafka client / broker.
 * The KLZ4F framing format is described in detail here:
 * https://github.com/lz4/lz4/blob/master/doc/lz4_Frame_format.md
 *
 * NOTE: This modifies 'inbuf'.
 *
 * Returns an error on failure to fix (nothing modified), else NO_ERROR.
 */
static rd_kafka_resp_err_t
rd_kafka_lz4_decompress_fixup_bad_framing(rd_kafka_broker_t *rkb,
                                          char *inbuf,
                                          size_t inlen) {
        static const char magic[4] = {0x04, 0x22, 0x4d, 0x18};
        uint8_t FLG, HC, correct_HC;
        size_t of = 4;

        /* Format is:
         *    int32_t magic;
         *    int8_t_ FLG;
         *    int8_t  BD;
         *  [ int64_t contentSize; ]
         *    int8_t  HC;
         */
        if (inlen < 4 + 3 || memcmp(inbuf, magic, 4)) {
                rd_rkb_dbg(rkb, BROKER, "KLZ4FIXUP",
                           "Unable to fix-up legacy KLZ4 framing "
                           "(%" PRIusz " bytes): invalid length or magic value",
                           inlen);
                return RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
        }

        of  = 4; /* past magic */
        FLG = inbuf[of++];
        of++; /* BD */

        if ((FLG >> 3) & 1) /* contentSize */
                of += 8;

        if (of >= inlen) {
                rd_rkb_dbg(rkb, BROKER, "KLZ4FIXUP",
                           "Unable to fix-up legacy KLZ4 framing "
                           "(%" PRIusz " bytes): requires %" PRIusz " bytes",
                           inlen, of);
                return RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
        }

        /* Header hash code */
        HC = inbuf[of];

        /* Calculate correct header hash code */
        correct_HC = (KXXH32(inbuf + 4, of - 4, 0) >> 8) & 0xff;

        if (HC != correct_HC)
                inbuf[of] = correct_HC;

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * Reverse of fix-up: break KLZ4 framing caused to be compatbile with with
 * buggy Kafka client / broker.
 *
 * NOTE: This modifies 'outbuf'.
 *
 * Returns an error on failure to recognize format (nothing modified),
 * else NO_ERROR.
 */
static rd_kafka_resp_err_t
rd_kafka_lz4_compress_break_framing(rd_kafka_broker_t *rkb,
                                    char *outbuf,
                                    size_t outlen) {
        static const char magic[4] = {0x04, 0x22, 0x4d, 0x18};
        uint8_t FLG, HC, bad_HC;
        size_t of = 4;

        /* Format is:
         *    int32_t magic;
         *    int8_t_ FLG;
         *    int8_t  BD;
         *  [ int64_t contentSize; ]
         *    int8_t  HC;
         */
        if (outlen < 4 + 3 || memcmp(outbuf, magic, 4)) {
                rd_rkb_dbg(rkb, BROKER, "KLZ4FIXDOWN",
                           "Unable to break legacy KLZ4 framing "
                           "(%" PRIusz " bytes): invalid length or magic value",
                           outlen);
                return RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
        }

        of  = 4; /* past magic */
        FLG = outbuf[of++];
        of++; /* BD */

        if ((FLG >> 3) & 1) /* contentSize */
                of += 8;

        if (of >= outlen) {
                rd_rkb_dbg(rkb, BROKER, "KLZ4FIXUP",
                           "Unable to break legacy KLZ4 framing "
                           "(%" PRIusz " bytes): requires %" PRIusz " bytes",
                           outlen, of);
                return RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
        }

        /* Header hash code */
        HC = outbuf[of];

        /* Calculate bad header hash code (include magic) */
        bad_HC = (KXXH32(outbuf, of, 0) >> 8) & 0xff;

        if (HC != bad_HC)
                outbuf[of] = bad_HC;

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



/**
 * @brief Decompress KLZ4F (framed) data.
 *        Kafka broker versions <0.10.0.0 (MsgVersion 0) breaks KLZ4 framing
 *        checksum, if \p proper_hc we assume the checksum is okay
 *        (broker version >=0.10.0, MsgVersion >= 1) else we fix it up.
 *
 * @remark May modify \p inbuf (if not \p proper_hc)
 */
rd_kafka_resp_err_t rd_kafka_lz4_decompress(rd_kafka_broker_t *rkb,
                                            int proper_hc,
                                            int64_t Offset,
                                            char *inbuf,
                                            size_t inlen,
                                            void **outbuf,
                                            size_t *outlenp) {
        KLZ4F_errorCode_t code;
        KLZ4F_decompressionContext_t dctx;
        KLZ4F_frameInfo_t fi;
        size_t in_sz, out_sz;
        size_t in_of, out_of;
        size_t r;
        size_t estimated_uncompressed_size;
        size_t outlen;
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        char *out               = NULL;

        *outbuf = NULL;

        code = KLZ4F_createDecompressionContext(&dctx, KLZ4F_VERSION);
        if (KLZ4F_isError(code)) {
                rd_rkb_dbg(rkb, BROKER, "KLZ4DECOMPR",
                           "Unable to create KLZ4 decompression context: %s",
                           KLZ4F_getErrorName(code));
                return RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE;
        }

        if (!proper_hc) {
                /* The original/legacy KLZ4 framing in Kafka was buggy and
                 * calculated the KLZ4 framing header hash code (HC) incorrectly.
                 * We do a fix-up of it here. */
                if ((err = rd_kafka_lz4_decompress_fixup_bad_framing(rkb, inbuf,
                                                                     inlen)))
                        goto done;
        }

        in_sz = inlen;
        r     = KLZ4F_getFrameInfo(dctx, &fi, (const void *)inbuf, &in_sz);
        if (KLZ4F_isError(r)) {
                rd_rkb_dbg(rkb, BROKER, "KLZ4DECOMPR",
                           "Failed to gather KLZ4 frame info: %s",
                           KLZ4F_getErrorName(r));
                err = RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
                goto done;
        }

        /* If uncompressed size is unknown or out of bounds, use a sane
         * default (4x compression) and reallocate if needed
         * More info on max size: http://stackoverflow.com/a/25751871/1821055
         * More info on lz4 compression ratios seen for different data sets:
         * http://dev.ti.com/tirex/content/simplelink_msp432p4_sdk_1_50_00_12/docs/lz4/users_guide/docguide.llQpgm/benchmarking.html
         */
        if (fi.contentSize == 0 || fi.contentSize > inlen * 255) {
                estimated_uncompressed_size = RD_MIN(
                    inlen * 4, (size_t)(rkb->rkb_rk->rk_conf.max_msg_size));
        } else {
                estimated_uncompressed_size = (size_t)fi.contentSize;
        }

        /* Allocate output buffer, we increase this later if needed,
         * but hopefully not. */
        out = rd_malloc(estimated_uncompressed_size);
        if (!out) {
                rd_rkb_log(rkb, LOG_WARNING, "KLZ4DEC",
                           "Unable to allocate decompression "
                           "buffer of %" PRIusz " bytes: %s",
                           estimated_uncompressed_size, rd_strerror(errno));
                err = RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE;
                goto done;
        }


        /* Decompress input buffer to output buffer until input is exhausted. */
        outlen = estimated_uncompressed_size;
        in_of  = in_sz;
        out_of = 0;
        while (in_of < inlen) {
                out_sz = outlen - out_of;
                in_sz  = inlen - in_of;
                r = KLZ4F_decompress(dctx, out + out_of, &out_sz, inbuf + in_of,
                                    &in_sz, NULL);
                if (unlikely(KLZ4F_isError(r))) {
                        rd_rkb_dbg(rkb, MSG, "KLZ4DEC",
                                   "Failed to KLZ4 (%s HC) decompress message "
                                   "(offset %" PRId64
                                   ") at "
                                   "payload offset %" PRIusz "/%" PRIusz ": %s",
                                   proper_hc ? "proper" : "legacy", Offset,
                                   in_of, inlen, KLZ4F_getErrorName(r));
                        err = RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
                        goto done;
                }

                rd_kafka_assert(NULL, out_of + out_sz <= outlen &&
                                          in_of + in_sz <= inlen);
                out_of += out_sz;
                in_of += in_sz;
                if (r == 0)
                        break;

                /* Need to grow output buffer, this shouldn't happen if
                 * contentSize was properly set. */
                if (unlikely(out_of == outlen)) {
                        char *tmp;
                        /* Grow exponentially with some factor > 1 (using 1.75)
                         * for amortized O(1) copying */
                        size_t extra = RD_MAX(outlen * 3 / 4, 1024);

                        rd_atomic64_add(&rkb->rkb_c.zbuf_grow, 1);

                        if (!(tmp = rd_realloc(out, outlen + extra))) {
                                rd_rkb_log(rkb, LOG_WARNING, "KLZ4DEC",
                                           "Unable to grow decompression "
                                           "buffer to %" PRIusz "+%" PRIusz
                                           " bytes: %s",
                                           outlen, extra, rd_strerror(errno));
                                err = RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE;
                                goto done;
                        }
                        out = tmp;
                        outlen += extra;
                }
        }


        if (in_of < inlen) {
                rd_rkb_dbg(rkb, MSG, "KLZ4DEC",
                           "Failed to KLZ4 (%s HC) decompress message "
                           "(offset %" PRId64
                           "): "
                           "%" PRIusz " (out of %" PRIusz ") bytes remaining",
                           proper_hc ? "proper" : "legacy", Offset,
                           inlen - in_of, inlen);
                err = RD_KAFKA_RESP_ERR__BAD_MSG;
                goto done;
        }

        *outbuf  = out;
        *outlenp = out_of;

done:
        code = KLZ4F_freeDecompressionContext(dctx);
        if (KLZ4F_isError(code)) {
                rd_rkb_dbg(rkb, BROKER, "KLZ4DECOMPR",
                           "Failed to close KLZ4 compression context: %s",
                           KLZ4F_getErrorName(code));
                err = RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
        }

        if (err && out)
                rd_free(out);

        return err;
}


/**
 * Allocate space for \p *outbuf and compress all \p iovlen buffers in \p iov.
 * @param proper_hc generate a proper HC (checksum) (kafka >=0.10.0.0,
 * MsgVersion >= 1)
 * @param MessageSetSize indicates (at least) full uncompressed data size,
 *                       possibly including MessageSet fields that will not
 *                       be compressed.
 *
 * @returns allocated buffer in \p *outbuf, length in \p *outlenp.
 */
rd_kafka_resp_err_t rd_kafka_lz4_compress(rd_kafka_broker_t *rkb,
                                          int proper_hc,
                                          int comp_level,
                                          rd_slice_t *slice,
                                          void **outbuf,
                                          size_t *outlenp) {
        KLZ4F_compressionContext_t cctx;
        KLZ4F_errorCode_t r;
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        size_t len              = rd_slice_remains(slice);
        size_t out_sz;
        size_t out_of = 0;
        char *out;
        const void *p;
        size_t rlen;

        /* Required by Kafka */
        const KLZ4F_preferences_t prefs = {
            .frameInfo        = {.blockMode = KLZ4F_blockIndependent},
            .compressionLevel = comp_level};

        *outbuf = NULL;

        out_sz = KLZ4F_compressBound(len, NULL) + 1000;
        if (KLZ4F_isError(out_sz)) {
                rd_rkb_dbg(rkb, MSG, "KLZ4COMPR",
                           "Unable to query KLZ4 compressed size "
                           "(for %" PRIusz " uncompressed bytes): %s",
                           len, KLZ4F_getErrorName(out_sz));
                return RD_KAFKA_RESP_ERR__BAD_MSG;
        }

        out = rd_malloc(out_sz);
        if (!out) {
                rd_rkb_dbg(rkb, MSG, "KLZ4COMPR",
                           "Unable to allocate output buffer "
                           "(%" PRIusz " bytes): %s",
                           out_sz, rd_strerror(errno));
                return RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE;
        }

        r = KLZ4F_createCompressionContext(&cctx, KLZ4F_VERSION);
        if (KLZ4F_isError(r)) {
                rd_rkb_dbg(rkb, MSG, "KLZ4COMPR",
                           "Unable to create KLZ4 compression context: %s",
                           KLZ4F_getErrorName(r));
                rd_free(out);
                return RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE;
        }

        r = KLZ4F_compressBegin(cctx, out, out_sz, &prefs);
        if (KLZ4F_isError(r)) {
                rd_rkb_dbg(rkb, MSG, "KLZ4COMPR",
                           "Unable to begin KLZ4 compression "
                           "(out buffer is %" PRIusz " bytes): %s",
                           out_sz, KLZ4F_getErrorName(r));
                err = RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
                goto done;
        }

        out_of += r;

        while ((rlen = rd_slice_reader(slice, &p))) {
                rd_assert(out_of < out_sz);
                r = KLZ4F_compressUpdate(cctx, out + out_of, out_sz - out_of, p,
                                        rlen, NULL);
                if (unlikely(KLZ4F_isError(r))) {
                        rd_rkb_dbg(rkb, MSG, "KLZ4COMPR",
                                   "KLZ4 compression failed "
                                   "(at of %" PRIusz
                                   " bytes, with "
                                   "%" PRIusz
                                   " bytes remaining in out buffer): "
                                   "%s",
                                   rlen, out_sz - out_of, KLZ4F_getErrorName(r));
                        err = RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
                        goto done;
                }

                out_of += r;
        }

        rd_assert(rd_slice_remains(slice) == 0);

        r = KLZ4F_compressEnd(cctx, out + out_of, out_sz - out_of, NULL);
        if (unlikely(KLZ4F_isError(r))) {
                rd_rkb_dbg(rkb, MSG, "KLZ4COMPR",
                           "Failed to finalize KLZ4 compression "
                           "of %" PRIusz " bytes: %s",
                           len, KLZ4F_getErrorName(r));
                err = RD_KAFKA_RESP_ERR__BAD_COMPRESSION;
                goto done;
        }

        out_of += r;

        /* For the broken legacy framing we need to mess up the header checksum
         * so that the Kafka client / broker code accepts it. */
        if (!proper_hc)
                if ((err =
                         rd_kafka_lz4_compress_break_framing(rkb, out, out_of)))
                        goto done;


        *outbuf  = out;
        *outlenp = out_of;

done:
        KLZ4F_freeCompressionContext(cctx);

        if (err)
                rd_free(out);

        return err;
}
