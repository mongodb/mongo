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
 * This fuzz target performs a zstd round-trip test (compress & decompress) with
 * a raw content dictionary, compares the result with the original, and calls
 * abort() on corruption.
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "fuzz_helpers.h"
#include "zstd_helpers.h"
#include "fuzz_data_producer.h"
#include "fuzz_third_party_seq_prod.h"

static ZSTD_CCtx *cctx = NULL;
static ZSTD_DCtx *dctx = NULL;

static size_t roundTripTest(void *result, size_t resultCapacity,
                            void *compressed, size_t compressedCapacity,
                            const void *src, size_t srcSize,
                            const void *dict, size_t dictSize,
                            FUZZ_dataProducer_t *producer)
{
    ZSTD_dictContentType_e const dictContentType = ZSTD_dct_rawContent;
    int const refPrefix = FUZZ_dataProducer_uint32Range(producer, 0, 1) != 0;
    size_t cSize;

    FUZZ_setRandomParameters(cctx, srcSize, producer);
    /* Disable checksum so we can use sizes smaller than compress bound. */
    FUZZ_ZASSERT(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0));
    if (refPrefix)
        FUZZ_ZASSERT(ZSTD_CCtx_refPrefix_advanced(
            cctx, dict, dictSize,
            ZSTD_dct_rawContent));
    else
        FUZZ_ZASSERT(ZSTD_CCtx_loadDictionary_advanced(
            cctx, dict, dictSize,
            (ZSTD_dictLoadMethod_e)FUZZ_dataProducer_uint32Range(producer, 0, 1),
            ZSTD_dct_rawContent));
    cSize = ZSTD_compress2(cctx, compressed, compressedCapacity, src, srcSize);
    FUZZ_ZASSERT(cSize);

    if (refPrefix)
        FUZZ_ZASSERT(ZSTD_DCtx_refPrefix_advanced(
            dctx, dict, dictSize,
            dictContentType));
    else
        FUZZ_ZASSERT(ZSTD_DCtx_loadDictionary_advanced(
            dctx, dict, dictSize,
            (ZSTD_dictLoadMethod_e)FUZZ_dataProducer_uint32Range(producer, 0, 1),
            dictContentType));
    {
        size_t const ret = ZSTD_decompressDCtx(
                dctx, result, resultCapacity, compressed, cSize);
        return ret;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    FUZZ_SEQ_PROD_SETUP();

    /* Give a random portion of src data to the producer, to use for
    parameter generation. The rest will be used for (de)compression */
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(src, size);
    size = FUZZ_dataProducer_reserveDataPrefix(producer);

    uint8_t const* const srcBuf = src;
    size_t const srcSize = FUZZ_dataProducer_uint32Range(producer, 0, size);
    uint8_t const* const dictBuf = srcBuf + srcSize;
    size_t const dictSize = size - srcSize;
    size_t const decompSize = srcSize;
    void* const decompBuf = FUZZ_malloc(decompSize);
    size_t compSize = ZSTD_compressBound(srcSize);
    void* compBuf;
    /* Half of the time fuzz with a 1 byte smaller output size.
     * This will still succeed because we force the checksum to be disabled,
     * giving us 4 bytes of overhead.
     */
    compSize -= FUZZ_dataProducer_uint32Range(producer, 0, 1);
    compBuf = FUZZ_malloc(compSize);

    if (!cctx) {
        cctx = ZSTD_createCCtx();
        FUZZ_ASSERT(cctx);
    }
    if (!dctx) {
        dctx = ZSTD_createDCtx();
        FUZZ_ASSERT(dctx);
    }

    {
        size_t const result =
            roundTripTest(decompBuf, decompSize, compBuf, compSize, srcBuf, srcSize, dictBuf, dictSize, producer);
        FUZZ_ZASSERT(result);
        FUZZ_ASSERT_MSG(result == srcSize, "Incorrect regenerated size");
        FUZZ_ASSERT_MSG(!FUZZ_memcmp(src, decompBuf, srcSize), "Corruption!");
    }
    free(decompBuf);
    free(compBuf);
    FUZZ_dataProducer_free(producer);
#ifndef STATEFUL_FUZZING
    ZSTD_freeCCtx(cctx); cctx = NULL;
    ZSTD_freeDCtx(dctx); dctx = NULL;
#endif
    FUZZ_SEQ_PROD_TEARDOWN();
    return 0;
}
