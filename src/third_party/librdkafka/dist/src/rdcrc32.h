/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018-2022, Magnus Edenhill
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
 * \file rdcrc32.h
 * Functions and types for CRC checks.
 *
 * Generated on Tue May  8 17:36:59 2012,
 * by pycrc v0.7.10, http://www.tty1.net/pycrc/
 *
 * NOTE: Contains librd modifications:
 *       - rd_crc32() helper.
 *       - __RDCRC32___H__ define (was missing the '32' part).
 *
 * using the configuration:
 *    Width        = 32
 *    Poly         = 0x04c11db7
 *    XorIn        = 0xffffffff
 *    ReflectIn    = True
 *    XorOut       = 0xffffffff
 *    ReflectOut   = True
 *    Algorithm    = table-driven
 *****************************************************************************/
#ifndef __RDCRC32___H__
#define __RDCRC32___H__

#include "rd.h"

#include <stdlib.h>
#include <stdint.h>

#if WITH_ZLIB
#include <zlib.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif


/**
 * The definition of the used algorithm.
 *****************************************************************************/
#define CRC_ALGO_TABLE_DRIVEN 1


/**
 * The type of the CRC values.
 *
 * This type must be big enough to contain at least 32 bits.
 *****************************************************************************/
typedef uint32_t rd_crc32_t;

#if !WITH_ZLIB
extern const rd_crc32_t crc_table[256];
#endif


/**
 * Reflect all bits of a \a data word of \a data_len bytes.
 *
 * \param data         The data word to be reflected.
 * \param data_len     The width of \a data expressed in number of bits.
 * \return             The reflected data.
 *****************************************************************************/
rd_crc32_t rd_crc32_reflect(rd_crc32_t data, size_t data_len);


/**
 * Calculate the initial crc value.
 *
 * \return     The initial crc value.
 *****************************************************************************/
static RD_INLINE rd_crc32_t rd_crc32_init(void) {
#if WITH_ZLIB
        return crc32(0, NULL, 0);
#else
        return 0xffffffff;
#endif
}


/**
 * Update the crc value with new data.
 *
 * \param crc      The current crc value.
 * \param data     Pointer to a buffer of \a data_len bytes.
 * \param data_len Number of bytes in the \a data buffer.
 * \return         The updated crc value.
 *****************************************************************************/
/**
 * Update the crc value with new data.
 *
 * \param crc      The current crc value.
 * \param data     Pointer to a buffer of \a data_len bytes.
 * \param data_len Number of bytes in the \a data buffer.
 * \return         The updated crc value.
 *****************************************************************************/
static RD_INLINE RD_UNUSED rd_crc32_t rd_crc32_update(rd_crc32_t crc,
                                                      const unsigned char *data,
                                                      size_t data_len) {
#if WITH_ZLIB
        rd_assert(data_len <= UINT_MAX);
        return crc32(crc, data, (uInt)data_len);
#else
        unsigned int tbl_idx;

        while (data_len--) {
                tbl_idx = (crc ^ *data) & 0xff;
                crc     = (crc_table[tbl_idx] ^ (crc >> 8)) & 0xffffffff;

                data++;
        }
        return crc & 0xffffffff;
#endif
}


/**
 * Calculate the final crc value.
 *
 * \param crc  The current crc value.
 * \return     The final crc value.
 *****************************************************************************/
static RD_INLINE rd_crc32_t rd_crc32_finalize(rd_crc32_t crc) {
#if WITH_ZLIB
        return crc;
#else
        return crc ^ 0xffffffff;
#endif
}


/**
 * Wrapper for performing CRC32 on the provided buffer.
 */
static RD_INLINE rd_crc32_t rd_crc32(const char *data, size_t data_len) {
        return rd_crc32_finalize(rd_crc32_update(
            rd_crc32_init(), (const unsigned char *)data, data_len));
}

#ifdef __cplusplus
} /* closing brace for extern "C" */
#endif

#endif /* __RDCRC32___H__ */
