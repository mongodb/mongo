/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

#define ZSTD_STATIC_LINKING_ONLY

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "fuzz_helpers.h"
#include "zstd.h"
#include "zstd_helpers.h"
#include "fuzz_data_producer.h"
#include "fuzz_third_party_seq_prod.h"

static ZSTD_CCtx *cctx = NULL;
static ZSTD_DCtx *dctx = NULL;
static void* cBuf = NULL;
static void* rBuf = NULL;
static size_t bufSize = 0;

static size_t roundTripTest(void *result, size_t resultCapacity,
                            void *compressed, size_t compressedCapacity,
                            const void *src, size_t srcSize,
                            int cLevel)
{
    ZSTD_parameters const params = ZSTD_getParams(cLevel, srcSize, 0);
    size_t ret = ZSTD_compressBegin_advanced(cctx, NULL, 0, params, srcSize);
    FUZZ_ZASSERT(ret);

    ret = ZSTD_compressBlock(cctx, compressed, compressedCapacity, src, srcSize);
    FUZZ_ZASSERT(ret);
    if (ret == 0) {
        FUZZ_ASSERT(resultCapacity >= srcSize);
        if (srcSize > 0) {
            memcpy(result, src, srcSize);
        }
        return srcSize;
    }
    ZSTD_decompressBegin(dctx);
    return ZSTD_decompressBlock(dctx, result, resultCapacity, compressed, ret);
}

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    FUZZ_SEQ_PROD_SETUP();

    /* Give a random portion of src data to the producer, to use for
    parameter generation. The rest will be used for (de)compression */
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(src, size);
    size = FUZZ_dataProducer_reserveDataPrefix(producer);

    int const cLevel = FUZZ_dataProducer_int32Range(producer, kMinClevel, kMaxClevel);

    size_t neededBufSize = size;
    if (size > ZSTD_BLOCKSIZE_MAX)
        size = ZSTD_BLOCKSIZE_MAX;

    /* Allocate all buffers and contexts if not already allocated */
    if (neededBufSize > bufSize || !cBuf || !rBuf) {
        free(cBuf);
        free(rBuf);
        cBuf = FUZZ_malloc(neededBufSize);
        rBuf = FUZZ_malloc(neededBufSize);
        bufSize = neededBufSize;
    }
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
            roundTripTest(rBuf, neededBufSize, cBuf, neededBufSize, src, size,
              cLevel);
        FUZZ_ZASSERT(result);
        FUZZ_ASSERT_MSG(result == size, "Incorrect regenerated size");
        FUZZ_ASSERT_MSG(!FUZZ_memcmp(src, rBuf, size), "Corruption!");
    }
    FUZZ_dataProducer_free(producer);
#ifndef STATEFUL_FUZZING
    ZSTD_freeCCtx(cctx); cctx = NULL;
    ZSTD_freeDCtx(dctx); dctx = NULL;
#endif
    FUZZ_SEQ_PROD_TEARDOWN();
    return 0;
}
