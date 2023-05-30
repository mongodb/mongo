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
 * This fuzz target makes sure that whenever a compression dictionary can be
 * loaded, the data can be round tripped.
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "fuzz_helpers.h"
#include "zstd_helpers.h"
#include "fuzz_data_producer.h"
#include "fuzz_third_party_seq_prod.h"

/**
 * Compresses the data and returns the compressed size or an error.
 */
static size_t compress(void* compressed, size_t compressedCapacity,
                       void const* source, size_t sourceSize,
                       void const* dict, size_t dictSize,
                       ZSTD_dictLoadMethod_e dictLoadMethod,
                       ZSTD_dictContentType_e dictContentType,
                       int const refPrefix)
{
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    if (refPrefix)
        FUZZ_ZASSERT(ZSTD_CCtx_refPrefix_advanced(
            cctx, dict, dictSize, dictContentType));
    else
        FUZZ_ZASSERT(ZSTD_CCtx_loadDictionary_advanced(
            cctx, dict, dictSize, dictLoadMethod, dictContentType));
    size_t const compressedSize = ZSTD_compress2(
            cctx, compressed, compressedCapacity, source, sourceSize);
    ZSTD_freeCCtx(cctx);
    return compressedSize;
}

static size_t decompress(void* result, size_t resultCapacity,
                         void const* compressed, size_t compressedSize,
                         void const* dict, size_t dictSize,
                       ZSTD_dictLoadMethod_e dictLoadMethod,
                         ZSTD_dictContentType_e dictContentType,
                         int const refPrefix)
{
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (refPrefix)
        FUZZ_ZASSERT(ZSTD_DCtx_refPrefix_advanced(
            dctx, dict, dictSize, dictContentType));
    else
        FUZZ_ZASSERT(ZSTD_DCtx_loadDictionary_advanced(
            dctx, dict, dictSize, dictLoadMethod, dictContentType));
    size_t const resultSize = ZSTD_decompressDCtx(
            dctx, result, resultCapacity, compressed, compressedSize);
    FUZZ_ZASSERT(resultSize);
    ZSTD_freeDCtx(dctx);
    return resultSize;
}

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    FUZZ_SEQ_PROD_SETUP();
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(src, size);
    int const refPrefix = FUZZ_dataProducer_uint32Range(producer, 0, 1) != 0;
    ZSTD_dictLoadMethod_e const dlm =
    size = FUZZ_dataProducer_uint32Range(producer, 0, 1);
    ZSTD_dictContentType_e const dct =
            FUZZ_dataProducer_uint32Range(producer, 0, 2);
    size = FUZZ_dataProducer_remainingBytes(producer);

    DEBUGLOG(2, "Dict load method %d", dlm);
    DEBUGLOG(2, "Dict content type %d", dct);
    DEBUGLOG(2, "Dict size %u", (unsigned)size);

    void* const rBuf = FUZZ_malloc(size);
    size_t const cBufSize = ZSTD_compressBound(size);
    void* const cBuf = FUZZ_malloc(cBufSize);

    size_t const cSize =
            compress(cBuf, cBufSize, src, size, src, size, dlm, dct, refPrefix);
    /* compression failing is okay */
    if (ZSTD_isError(cSize)) {
      FUZZ_ASSERT_MSG(dct != ZSTD_dct_rawContent, "Raw must always succeed!");
      goto out;
    }
    size_t const rSize =
            decompress(rBuf, size, cBuf, cSize, src, size, dlm, dct, refPrefix);
    FUZZ_ASSERT_MSG(rSize == size, "Incorrect regenerated size");
    FUZZ_ASSERT_MSG(!FUZZ_memcmp(src, rBuf, size), "Corruption!");

out:
    free(cBuf);
    free(rBuf);
    FUZZ_dataProducer_free(producer);
    FUZZ_SEQ_PROD_TEARDOWN();
    return 0;
}
