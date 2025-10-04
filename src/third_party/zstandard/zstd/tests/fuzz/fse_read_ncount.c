/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/**
 * This fuzz target round trips the FSE normalized count with FSE_writeNCount()
 * and FSE_readNcount() to ensure that it can always round trip correctly.
 */

#define FSE_STATIC_LINKING_ONLY
#define ZSTD_STATIC_LINKING_ONLY

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "fuzz_helpers.h"
#include "zstd_helpers.h"
#include "fuzz_data_producer.h"
#include "fse.h"

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(src, size);

    /* Pick a random tableLog and maxSymbolValue */
    unsigned const tableLog = FUZZ_dataProducer_uint32Range(producer, FSE_MIN_TABLELOG, FSE_MAX_TABLELOG);
    unsigned const maxSymbolValue = FUZZ_dataProducer_uint32Range(producer, 0, 255);

    unsigned remainingWeight = (1u << tableLog) - 1;
    size_t dataSize;
    BYTE data[512];
    short ncount[256];

    /* Randomly fill the normalized count */
    memset(ncount, 0, sizeof(ncount));
    {
        unsigned s;
        for (s = 0; s < maxSymbolValue && remainingWeight > 0; ++s) {
            short n = (short)FUZZ_dataProducer_int32Range(producer, -1, remainingWeight);
            ncount[s] = n;
            if (n < 0) {
                remainingWeight -= 1;
            } else {
                assert((unsigned)n <= remainingWeight);
                remainingWeight -= n;
            }
        }
        /* Ensure ncount[maxSymbolValue] != 0 and the sum is (1<<tableLog) */
        ncount[maxSymbolValue] = remainingWeight + 1;
        if (ncount[maxSymbolValue] == 1 && FUZZ_dataProducer_uint32Range(producer, 0, 1) == 1) {
            ncount[maxSymbolValue] = -1;
        }
    }
    /* Write the normalized count */
    {
        FUZZ_ASSERT(sizeof(data) >= FSE_NCountWriteBound(maxSymbolValue, tableLog));
        dataSize = FSE_writeNCount(data, sizeof(data), ncount, maxSymbolValue, tableLog);
        FUZZ_ZASSERT(dataSize);
    }
    /* Read & validate the normalized count */
    {
        short rtNcount[256];
        unsigned rtMaxSymbolValue = 255;
        unsigned rtTableLog;
        /* Copy into a buffer with a random amount of random data at the end */
        size_t const buffSize = (size_t)FUZZ_dataProducer_uint32Range(producer, dataSize, sizeof(data));
        BYTE* const buff = FUZZ_malloc(buffSize);
        size_t rtDataSize;
        memcpy(buff, data, dataSize); 
        {
            size_t b;
            for (b = dataSize; b < buffSize; ++b) {
                buff[b] = (BYTE)FUZZ_dataProducer_uint32Range(producer, 0, 255);
            }
        }

        rtDataSize = FSE_readNCount(rtNcount, &rtMaxSymbolValue, &rtTableLog, buff, buffSize);
        FUZZ_ZASSERT(rtDataSize);
        FUZZ_ASSERT(rtDataSize == dataSize);
        FUZZ_ASSERT(rtMaxSymbolValue == maxSymbolValue);
        FUZZ_ASSERT(rtTableLog == tableLog);
        {
            unsigned s;
            for (s = 0; s <= maxSymbolValue; ++s) {
                FUZZ_ASSERT(ncount[s] == rtNcount[s]);
            }
        }
        free(buff);
    }

    FUZZ_dataProducer_free(producer);
    return 0;
}
