/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2015, Magnus Edenhill
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
#include "rdunittest.h"
#include "rdmurmur2.h"
#include "rdendian.h"


/* MurmurHash2, by Austin Appleby
 *
 * With librdkafka modifications combinining aligned/unaligned variants
 * into the same function.
 */

#define MM_MIX(h, k, m)                                                        \
        {                                                                      \
                k *= m;                                                        \
                k ^= k >> r;                                                   \
                k *= m;                                                        \
                h *= m;                                                        \
                h ^= k;                                                        \
        }

/*-----------------------------------------------------------------------------
// Based on MurmurHashNeutral2, by Austin Appleby
//
// Same as MurmurHash2, but endian- and alignment-neutral.
// Half the speed though, alas.
//
*/
uint32_t rd_murmur2(const void *key, size_t len) {
        const uint32_t seed = 0x9747b28c;
        const uint32_t m    = 0x5bd1e995;
        const int r         = 24;
        uint32_t h          = seed ^ (uint32_t)len;
        const unsigned char *tail;

        if (likely(((intptr_t)key & 0x3) == 0)) {
                /* Input is 32-bit word aligned. */
                const uint32_t *data = (const uint32_t *)key;

                while (len >= 4) {
                        uint32_t k = htole32(*(uint32_t *)data);

                        MM_MIX(h, k, m);

                        data++;
                        len -= 4;
                }

                tail = (const unsigned char *)data;

        } else {
                /* Unaligned slower variant */
                const unsigned char *data = (const unsigned char *)key;

                while (len >= 4) {
                        uint32_t k;

                        k = data[0];
                        k |= data[1] << 8;
                        k |= data[2] << 16;
                        k |= data[3] << 24;

                        MM_MIX(h, k, m);

                        data += 4;
                        len -= 4;
                }

                tail = data;
        }

        /* Read remaining sub-word */
        switch (len) {
        case 3:
                h ^= tail[2] << 16;
        case 2:
                h ^= tail[1] << 8;
        case 1:
                h ^= tail[0];
                h *= m;
        };

        h ^= h >> 13;
        h *= m;
        h ^= h >> 15;

        /* Last bit is set to 0 because the java implementation uses int_32
         * and then sets to positive number flipping last bit to 1. */
        return h;
}


/**
 * @brief Unittest for rd_murmur2()
 */
int unittest_murmur2(void) {
        const char *short_unaligned = "1234";
        const char *unaligned       = "PreAmbleWillBeRemoved,ThePrePartThatIs";
        const char *keysToTest[]    = {
            "kafka",
            "giberish123456789",
            short_unaligned,
            short_unaligned + 1,
            short_unaligned + 2,
            short_unaligned + 3,
            unaligned,
            unaligned + 1,
            unaligned + 2,
            unaligned + 3,
            "",
            NULL,
        };

        const int32_t java_murmur2_results[] = {
            0xd067cf64,  // kafka
            0x8f552b0c,  // giberish123456789
            0x9fc97b14,  // short_unaligned
            0xe7c009ca,  // short_unaligned+1
            0x873930da,  // short_unaligned+2
            0x5a4b5ca1,  // short_unaligned+3
            0x78424f1c,  // unaligned
            0x4a62b377,  // unaligned+1
            0xe0e4e09e,  // unaligned+2
            0x62b8b43f,  // unaligned+3
            0x106e08d9,  // ""
            0x106e08d9,  // NULL
        };

        size_t i;
        for (i = 0; i < RD_ARRAYSIZE(keysToTest); i++) {
                uint32_t h = rd_murmur2(
                    keysToTest[i], keysToTest[i] ? strlen(keysToTest[i]) : 0);
                RD_UT_ASSERT((int32_t)h == java_murmur2_results[i],
                             "Calculated murmur2 hash 0x%x for \"%s\", "
                             "expected 0x%x",
                             h, keysToTest[i], java_murmur2_results[i]);
        }
        RD_UT_PASS();
}
