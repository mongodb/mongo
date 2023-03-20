/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2020, Magnus Edenhill
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
#include "rdfnv1a.h"


/* FNV-1a by Glenn Fowler, Landon Curt Noll, and Kiem-Phong Vo
 *
 * Based on http://www.isthe.com/chongo/src/fnv/hash_32a.c
 * with librdkafka modifications to match the Sarama default Producer
 * implementation, as seen here:
 * https://github.com/Shopify/sarama/blob/master/partitioner.go#L203 Note that
 * this implementation is only compatible with Sarama's default
 * NewHashPartitioner and not NewReferenceHashPartitioner.
 */
uint32_t rd_fnv1a(const void *key, size_t len) {
        const uint32_t prime  = 0x01000193;  // 16777619
        const uint32_t offset = 0x811C9DC5;  // 2166136261
        size_t i;
        int32_t h = offset;

        const unsigned char *data = (const unsigned char *)key;

        for (i = 0; i < len; i++) {
                h ^= data[i];
                h *= prime;
        }

        /* Take absolute value to match the Sarama NewHashPartitioner
         * implementation */
        if (h < 0) {
                h = -h;
        }

        return (uint32_t)h;
}


/**
 * @brief Unittest for rd_fnv1a()
 */
int unittest_fnv1a(void) {
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

        // Acquired via https://play.golang.org/p/vWIhw3zJINA
        const int32_t golang_hashfnv_results[] = {
            0xd33c4e1,   // kafka
            0x77a58295,  // giberish123456789
            0x23bdd03,   // short_unaligned
            0x2dea3cd2,  // short_unaligned+1
            0x740fa83e,  // short_unaligned+2
            0x310ca263,  // short_unaligned+3
            0x65cbd69c,  // unaligned
            0x6e49c79a,  // unaligned+1
            0x69eed356,  // unaligned+2
            0x6abcc023,  // unaligned+3
            0x7ee3623b,  // ""
            0x7ee3623b,  // NULL
        };

        size_t i;
        for (i = 0; i < RD_ARRAYSIZE(keysToTest); i++) {
                uint32_t h = rd_fnv1a(
                    keysToTest[i], keysToTest[i] ? strlen(keysToTest[i]) : 0);
                RD_UT_ASSERT((int32_t)h == golang_hashfnv_results[i],
                             "Calculated FNV-1a hash 0x%x for \"%s\", "
                             "expected 0x%x",
                             h, keysToTest[i], golang_hashfnv_results[i]);
        }
        RD_UT_PASS();
}
