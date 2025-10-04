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
 * This fuzz target performs a zstd round-trip test (compress & decompress),
 * compares the result with the original, and calls abort() on corruption.
 */

#define ZSTD_STATIC_LINKING_ONLY

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

static size_t getDecompressionMargin(void const* compressed, size_t cSize, size_t srcSize, int hasSmallBlocks)
{
    size_t margin = ZSTD_decompressionMargin(compressed, cSize);
    if (!hasSmallBlocks) {
        /* The macro should be correct in this case, but it may be smaller
         * because of e.g. block splitting, so take the smaller of the two.
         */
        ZSTD_frameHeader zfh;
        size_t marginM;
        FUZZ_ZASSERT(ZSTD_getFrameHeader(&zfh, compressed, cSize));
        marginM = ZSTD_DECOMPRESSION_MARGIN(srcSize, zfh.blockSizeMax);
        if (marginM < margin)
            margin = marginM;
    }
    return margin;
}

static size_t roundTripTest(void *result, size_t resultCapacity,
                            void *compressed, size_t compressedCapacity,
                            const void *src, size_t srcSize,
                            FUZZ_dataProducer_t *producer)
{
    size_t cSize;
    size_t dSize;
    int targetCBlockSize = 0;
    if (FUZZ_dataProducer_uint32Range(producer, 0, 1)) {
        size_t const remainingBytes = FUZZ_dataProducer_remainingBytes(producer);
        FUZZ_setRandomParameters(cctx, srcSize, producer);
        cSize = ZSTD_compress2(cctx, compressed, compressedCapacity, src, srcSize);
        FUZZ_ZASSERT(cSize);
        FUZZ_ZASSERT(ZSTD_CCtx_getParameter(cctx, ZSTD_c_targetCBlockSize, &targetCBlockSize));
        // Compress a second time and check for determinism
        {
            size_t const cSize0 = cSize;
            XXH64_hash_t const hash0 = XXH64(compressed, cSize, 0);
            FUZZ_dataProducer_rollBack(producer, remainingBytes);
            FUZZ_setRandomParameters(cctx, srcSize, producer);
            cSize = ZSTD_compress2(cctx, compressed, compressedCapacity, src, srcSize);
            FUZZ_ASSERT(cSize == cSize0);
            FUZZ_ASSERT(XXH64(compressed, cSize, 0) == hash0);
        }
    } else {
        int const cLevel = FUZZ_dataProducer_int32Range(producer, kMinClevel, kMaxClevel);
        cSize = ZSTD_compressCCtx(
            cctx, compressed, compressedCapacity, src, srcSize, cLevel);
        FUZZ_ZASSERT(cSize);
        // Compress a second time and check for determinism
        {
            size_t const cSize0 = cSize;
            XXH64_hash_t const hash0 = XXH64(compressed, cSize, 0);
            cSize = ZSTD_compressCCtx(
                cctx, compressed, compressedCapacity, src, srcSize, cLevel);
            FUZZ_ASSERT(cSize == cSize0);
            FUZZ_ASSERT(XXH64(compressed, cSize, 0) == hash0);
        }
    }
    dSize = ZSTD_decompressDCtx(dctx, result, resultCapacity, compressed, cSize);
    FUZZ_ZASSERT(dSize);
    FUZZ_ASSERT_MSG(dSize == srcSize, "Incorrect regenerated size");
    FUZZ_ASSERT_MSG(!FUZZ_memcmp(src, result, dSize), "Corruption!");

    {
        size_t margin = getDecompressionMargin(compressed, cSize, srcSize, targetCBlockSize);
        size_t const outputSize = srcSize + margin;
        char* const output = (char*)FUZZ_malloc(outputSize);
        char* const input = output + outputSize - cSize;
        FUZZ_ASSERT(outputSize >= cSize);
        memcpy(input, compressed, cSize);

        dSize = ZSTD_decompressDCtx(dctx, output, outputSize, input, cSize);
        FUZZ_ZASSERT(dSize);
        FUZZ_ASSERT_MSG(dSize == srcSize, "Incorrect regenerated size");
        FUZZ_ASSERT_MSG(!FUZZ_memcmp(src, output, srcSize), "Corruption!");

        free(output);
    }

    /* When superblock is enabled make sure we don't expand the block more than expected.
     * NOTE: This test is currently disabled because superblock mode can arbitrarily
     * expand the block in the worst case. Once superblock mode has been improved we can
     * re-enable this test.
     */
    if (0 && targetCBlockSize != 0) {
        size_t normalCSize;
        FUZZ_ZASSERT(ZSTD_CCtx_setParameter(cctx, ZSTD_c_targetCBlockSize, 0));
        normalCSize = ZSTD_compress2(cctx, compressed, compressedCapacity, src, srcSize);
        FUZZ_ZASSERT(normalCSize);
        {
            size_t const bytesPerBlock = 3 /* block header */
                + 5 /* Literal header */
                + 6 /* Huffman jump table */
                + 3 /* number of sequences */
                + 1 /* symbol compression modes */;
            size_t const expectedExpansion = bytesPerBlock * (1 + (normalCSize / MAX(1, targetCBlockSize)));
            size_t const allowedExpansion = (srcSize >> 3) + 5 * expectedExpansion + 10;
            FUZZ_ASSERT(cSize <= normalCSize + allowedExpansion);
        }
    }
    return dSize;
}

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    FUZZ_SEQ_PROD_SETUP();

    size_t const rBufSize = size;
    void* rBuf = FUZZ_malloc(rBufSize);
    size_t cBufSize = ZSTD_compressBound(size);
    void* cBuf;

    /* Give a random portion of src data to the producer, to use for
    parameter generation. The rest will be used for (de)compression */
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(src, size);
    size = FUZZ_dataProducer_reserveDataPrefix(producer);

    /* Half of the time fuzz with a 1 byte smaller output size.
     * This will still succeed because we don't use a dictionary, so the dictID
     * field is empty, giving us 4 bytes of overhead.
     */
    cBufSize -= FUZZ_dataProducer_uint32Range(producer, 0, 1);

    cBuf = FUZZ_malloc(cBufSize);

    if (!cctx) {
        cctx = ZSTD_createCCtx();
        FUZZ_ASSERT(cctx);
    }
    if (!dctx) {
        dctx = ZSTD_createDCtx();
        FUZZ_ASSERT(dctx);
    }

    roundTripTest(rBuf, rBufSize, cBuf, cBufSize, src, size, producer);
    free(rBuf);
    free(cBuf);
    FUZZ_dataProducer_free(producer);
#ifndef STATEFUL_FUZZING
    ZSTD_freeCCtx(cctx); cctx = NULL;
    ZSTD_freeDCtx(dctx); dctx = NULL;
#endif
    FUZZ_SEQ_PROD_TEARDOWN();
    return 0;
}
