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
#include "common/huf.h"
#include "fuzz_helpers.h"
#include "fuzz_data_producer.h"

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(src, size);
    /* Select random parameters: #streams, X1 or X2 decoding, bmi2 */
    int const streams = FUZZ_dataProducer_int32Range(producer, 0, 1);
    int const symbols = FUZZ_dataProducer_int32Range(producer, 0, 1);
    int const bmi2 = ZSTD_cpuid_bmi2(ZSTD_cpuid()) && FUZZ_dataProducer_int32Range(producer, 0, 1);
    /* Select a random cBufSize - it may be too small */
    size_t const dBufSize = FUZZ_dataProducer_uint32Range(producer, 0, 8 * size + 500);
    size_t const maxTableLog = FUZZ_dataProducer_uint32Range(producer, 1, HUF_TABLELOG_MAX);
    HUF_DTable* dt = (HUF_DTable*)FUZZ_malloc(HUF_DTABLE_SIZE(maxTableLog) * sizeof(HUF_DTable));
    size_t const wkspSize = HUF_WORKSPACE_SIZE;
    void* wksp = FUZZ_malloc(wkspSize);
    void* dBuf = FUZZ_malloc(dBufSize);
    dt[0] = maxTableLog * 0x01000001;
    size = FUZZ_dataProducer_remainingBytes(producer);

    if (symbols == 0) {
        size_t const err = HUF_readDTableX1_wksp_bmi2(dt, src, size, wksp, wkspSize, bmi2);
        if (ZSTD_isError(err))
            goto _out;
    } else {
        size_t const err = HUF_readDTableX2_wksp_bmi2(dt, src, size, wksp, wkspSize, bmi2);
        if (ZSTD_isError(err))
            goto _out;
    }
    if (streams == 0)
        HUF_decompress1X_usingDTable_bmi2(dBuf, dBufSize, src, size, dt, bmi2);
    else
        HUF_decompress4X_usingDTable_bmi2(dBuf, dBufSize, src, size, dt, bmi2);

_out:
    free(dt);
    free(wksp);
    free(dBuf);
    FUZZ_dataProducer_free(producer);
    return 0;
}
