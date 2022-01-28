/*
 * Copyright (c) Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/**
 * This fuzz target performs a zstd round-trip test (compress & decompress),
 * compares the result with the original, and calls abort() on corruption.
 */

#define HUF_STATIC_LINKING_ONLY

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "common/cpu.h"
#include "compress/hist.h"
#include "common/huf.h"
#include "fuzz_helpers.h"
#include "fuzz_data_producer.h"

static size_t adjustTableLog(size_t tableLog, size_t maxSymbol)
{
    size_t const alphabetSize = maxSymbol + 1;
    size_t minTableLog = BIT_highbit32(alphabetSize) + 1;
    if ((alphabetSize & (alphabetSize - 1)) != 0) {
        ++minTableLog;
    }
    assert(minTableLog <= 9);
    if (tableLog < minTableLog)
        return minTableLog;
    else
        return tableLog;
}

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(src, size);
    /* Select random parameters: #streams, X1 or X2 decoding, bmi2 */
    int const streams = FUZZ_dataProducer_int32Range(producer, 0, 1);
    int const symbols = FUZZ_dataProducer_int32Range(producer, 0, 1);
    int const bmi2 = ZSTD_cpuid_bmi2(ZSTD_cpuid()) && FUZZ_dataProducer_int32Range(producer, 0, 1);
    /* Select a random cBufSize - it may be too small */
    size_t const cBufSize = FUZZ_dataProducer_uint32Range(producer, 0, 4 * size);
    /* Select a random tableLog - we'll adjust it up later */
    size_t tableLog = FUZZ_dataProducer_uint32Range(producer, 1, 12);
    size_t const kMaxSize = 256 * 1024;
    size = FUZZ_dataProducer_remainingBytes(producer);
    if (size > kMaxSize)
        size = kMaxSize;

    if (size <= 1) {
        FUZZ_dataProducer_free(producer);
        return 0;
    }

    uint32_t maxSymbol = 255;

    U32 count[256];
    size_t const mostFrequent = HIST_count(count, &maxSymbol, src, size);
    FUZZ_ZASSERT(mostFrequent);
    if (mostFrequent == size) {
        /* RLE */
        FUZZ_dataProducer_free(producer);
        return 0;

    }
    FUZZ_ASSERT(maxSymbol <= 255);
    tableLog = adjustTableLog(tableLog, maxSymbol);

    size_t const wkspSize = HUF_WORKSPACE_SIZE;
    void* wksp = FUZZ_malloc(wkspSize);
    void* rBuf = FUZZ_malloc(size);
    void* cBuf = FUZZ_malloc(cBufSize);
    HUF_CElt* ct = (HUF_CElt*)FUZZ_malloc(HUF_CTABLE_SIZE(maxSymbol));
    HUF_DTable* dt = (HUF_DTable*)FUZZ_malloc(HUF_DTABLE_SIZE(tableLog) * sizeof(HUF_DTable));
    dt[0] = tableLog * 0x01000001;

    tableLog = HUF_optimalTableLog(tableLog, size, maxSymbol);
    FUZZ_ASSERT(tableLog <= 12);
    tableLog = HUF_buildCTable_wksp(ct, count, maxSymbol, tableLog, wksp, wkspSize);
    FUZZ_ZASSERT(tableLog);
    size_t const tableSize = HUF_writeCTable_wksp(cBuf, cBufSize, ct, maxSymbol, tableLog, wksp, wkspSize);
    if (ERR_isError(tableSize)) {
        /* Errors on uncompressible data or cBufSize too small */
        goto _out;
    }
    FUZZ_ZASSERT(tableSize);
    if (symbols == 0) {
        FUZZ_ZASSERT(HUF_readDTableX1_wksp_bmi2(dt, cBuf, tableSize, wksp, wkspSize, bmi2));
    } else {
        size_t const ret = HUF_readDTableX2_wksp(dt, cBuf, tableSize, wksp, wkspSize);
        if (ERR_getErrorCode(ret) == ZSTD_error_tableLog_tooLarge) {
            FUZZ_ZASSERT(HUF_readDTableX1_wksp_bmi2(dt, cBuf, tableSize, wksp, wkspSize, bmi2));
        } else {
            FUZZ_ZASSERT(ret);
        }
    }

    size_t cSize;
    size_t rSize;
    if (streams == 0) {
        cSize = HUF_compress1X_usingCTable_bmi2(cBuf, cBufSize, src, size, ct, bmi2);
        FUZZ_ZASSERT(cSize);
        if (cSize != 0)
            rSize = HUF_decompress1X_usingDTable_bmi2(rBuf, size, cBuf, cSize, dt, bmi2);
    } else {
        cSize = HUF_compress4X_usingCTable_bmi2(cBuf, cBufSize, src, size, ct, bmi2);
        FUZZ_ZASSERT(cSize);
        if (cSize != 0)
            rSize = HUF_decompress4X_usingDTable_bmi2(rBuf, size, cBuf, cSize, dt, bmi2);
    }
    if (cSize != 0) {
        FUZZ_ZASSERT(rSize);
        FUZZ_ASSERT_MSG(rSize == size, "Incorrect regenerated size");
        FUZZ_ASSERT_MSG(!FUZZ_memcmp(src, rBuf, size), "Corruption!");
    }
_out:
    free(rBuf);
    free(cBuf);
    free(ct);
    free(dt);
    free(wksp);
    FUZZ_dataProducer_free(producer);
    return 0;
}
