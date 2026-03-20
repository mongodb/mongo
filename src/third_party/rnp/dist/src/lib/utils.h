/*
 * Copyright (c) 2017-2021 [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef RNP_UTILS_H_
#define RNP_UTILS_H_

#include <stdio.h>
#include <limits.h>
#include "logging.h"

/* number of elements in an array */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

/*
 * @params
 * array:       array of the structures to lookup
 * id_field     name of the field to compare against
 * ret_field    filed to return
 * lookup_value lookup value
 * ret          return value
 */
#define ARRAY_LOOKUP_BY_ID(array, id_field, ret_field, lookup_value, ret) \
    do {                                                                  \
        for (size_t i__ = 0; i__ < ARRAY_SIZE(array); i__++) {            \
            if ((array)[i__].id_field == (lookup_value)) {                \
                (ret) = (array)[i__].ret_field;                           \
                break;                                                    \
            }                                                             \
        }                                                                 \
    } while (0)

/* Portable way to convert bits to bytes */

#define BITS_TO_BYTES(b) (((b) + (CHAR_BIT - 1)) / CHAR_BIT)

/* Read big-endian 16-bit value from buf */
inline uint16_t
read_uint16(const uint8_t *buf)
{
    return ((uint16_t) buf[0] << 8) | buf[1];
}

/* Read big-endian 32-bit value from buf */
inline uint32_t
read_uint32(const uint8_t *buf)
{
    return ((uint32_t) buf[0] << 24) | ((uint32_t) buf[1] << 16) | ((uint32_t) buf[2] << 8) |
           (uint32_t) buf[3];
}

/* Store big-endian 16-bit value val in buf */
inline void
write_uint16(uint8_t *buf, uint16_t val)
{
    buf[0] = val >> 8;
    buf[1] = val & 0xff;
}

/* Store big-endian 32-bit value val in buf */
inline void
write_uint32(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val >> 24) & 0xff;
    buf[1] = (uint8_t)(val >> 16) & 0xff;
    buf[2] = (uint8_t)(val >> 8) & 0xff;
    buf[3] = (uint8_t)(val >> 0) & 0xff;
}

inline void
write_uint64(uint8_t *buf, uint64_t val)
{
    buf[0] = (uint8_t)(val >> 56) & 0xff;
    buf[1] = (uint8_t)(val >> 48) & 0xff;
    buf[2] = (uint8_t)(val >> 40) & 0xff;
    buf[3] = (uint8_t)(val >> 32) & 0xff;
    buf[4] = (uint8_t)(val >> 24) & 0xff;
    buf[5] = (uint8_t)(val >> 16) & 0xff;
    buf[6] = (uint8_t)(val >> 8) & 0xff;
    buf[7] = (uint8_t)(val >> 0) & 0xff;
}

inline char *
getenv_logname(void)
{
    char *name = getenv("LOGNAME");
    if (!name) {
        name = getenv("USER");
    }
    return name;
}

#endif
