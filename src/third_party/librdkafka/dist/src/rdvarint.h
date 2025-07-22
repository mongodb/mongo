/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2016-2022, Magnus Edenhill
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


#ifndef _RDVARINT_H
#define _RDVARINT_H

#include "rd.h"
#include "rdbuf.h"

/**
 * @name signed varint zig-zag encoder/decoder
 * @{
 *
 */

/**
 * @brief unsigned-varint encodes unsigned integer \p num into buffer
 *        at \p dst of size \p dstsize.
 * @returns the number of bytes written to \p dst, or 0 if not enough space.
 */

static RD_INLINE RD_UNUSED size_t rd_uvarint_enc_u64(char *dst,
                                                     size_t dstsize,
                                                     uint64_t num) {
        size_t of = 0;

        do {
                if (unlikely(of >= dstsize))
                        return 0; /* Not enough space */

                dst[of++] = (num & 0x7f) | (num > 0x7f ? 0x80 : 0);
                num >>= 7;
        } while (num);

        return of;
}

/**
 * @brief encodes a signed integer using zig-zag encoding.
 * @sa rd_uvarint_enc_u64
 */
static RD_INLINE RD_UNUSED size_t rd_uvarint_enc_i64(char *dst,
                                                     size_t dstsize,
                                                     int64_t num) {
        return rd_uvarint_enc_u64(dst, dstsize, (num << 1) ^ (num >> 63));
}


static RD_INLINE RD_UNUSED size_t rd_uvarint_enc_i32(char *dst,
                                                     size_t dstsize,
                                                     int32_t num) {
        return rd_uvarint_enc_i64(dst, dstsize, num);
}



/**
 * @brief Use on return value from rd_uvarint_dec() to check if
 *        decoded varint fit the size_t.
 *
 * @returns 1 on overflow, else 0.
 */
#define RD_UVARINT_OVERFLOW(DEC_RETVAL) (DEC_RETVAL > SIZE_MAX)

/**
 * @returns 1 if there were not enough bytes to decode the varint, else 0.
 */
#define RD_UVARINT_UNDERFLOW(DEC_RETVAL) (DEC_RETVAL == 0)


/**
 * @param DEC_RETVAL the return value from \c rd_uvarint_dec()
 * @returns 1 if varint decoding failed, else 0.
 * @warning \p DEC_RETVAL will be evaluated twice.
 */
#define RD_UVARINT_DEC_FAILED(DEC_RETVAL)                                      \
        (RD_UVARINT_UNDERFLOW(DEC_RETVAL) || RD_UVARINT_OVERFLOW(DEC_RETVAL))


/**
 * @brief Decodes the unsigned-varint in buffer \p src of size \p srcsize
 *        and stores the decoded unsigned integer in \p nump.
 *
 * @remark Use RD_UVARINT_OVERFLOW(returnvalue) to check if the varint
 *         could not fit \p nump, and RD_UVARINT_UNDERFLOW(returnvalue) to
 *         check if there were not enough bytes available in \p src to
 *         decode the full varint.
 *
 * @returns the number of bytes read from \p src.
 */
static RD_INLINE RD_UNUSED size_t rd_uvarint_dec(const char *src,
                                                 size_t srcsize,
                                                 uint64_t *nump) {
        size_t of    = 0;
        uint64_t num = 0;
        int shift    = 0;

        do {
                if (unlikely(srcsize-- == 0))
                        return 0; /* Underflow */
                num |= (uint64_t)(src[(int)of] & 0x7f) << shift;
                shift += 7;
        } while (src[(int)of++] & 0x80);

        *nump = num;
        return of;
}

static RD_INLINE RD_UNUSED size_t rd_varint_dec_i64(const char *src,
                                                    size_t srcsize,
                                                    int64_t *nump) {
        uint64_t n;
        size_t r;

        r = rd_uvarint_dec(src, srcsize, &n);
        if (likely(!RD_UVARINT_DEC_FAILED(r)))
                *nump = (int64_t)(n >> 1) ^ -(int64_t)(n & 1);

        return r;
}


/**
 * @returns the maximum encoded size for a type
 */
#define RD_UVARINT_ENC_SIZEOF(TYPE) (sizeof(TYPE) + 1 + (sizeof(TYPE) / 7))

/**
 * @returns the encoding size of the value 0
 */
#define RD_UVARINT_ENC_SIZE_0() ((size_t)1)


int unittest_rdvarint(void);

/**@}*/


#endif /* _RDVARINT_H */
