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
 * This fuzz target attempts to decompress a valid compressed frame into
 * an output buffer that is too small to ensure we always get
 * ZSTD_error_dstSize_tooSmall.
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include "fuzz_helpers.h"
#include "zstd.h"
#include "zstd_errors.h"
#include "zstd_helpers.h"
#include "fuzz_data_producer.h"
#include "fuzz_third_party_seq_prod.h"

static ZSTD_CCtx *cctx = NULL;
static ZSTD_DCtx *dctx = NULL;

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    FUZZ_SEQ_PROD_SETUP();

    /* Give a random portion of src data to the producer, to use for
    parameter generation. The rest will be used for (de)compression */
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(src, size);
    size_t rBufSize = FUZZ_dataProducer_uint32Range(producer, 0, size);
    size = FUZZ_dataProducer_remainingBytes(producer);
    /* Ensure the round-trip buffer is too small. */
    if (rBufSize >= size) {
        rBufSize = size > 0 ? size - 1 : 0;
    }
    size_t const cBufSize = ZSTD_compressBound(size);

    if (!cctx) {
        cctx = ZSTD_createCCtx();
        FUZZ_ASSERT(cctx);
    }
    if (!dctx) {
        dctx = ZSTD_createDCtx();
        FUZZ_ASSERT(dctx);
    }

    void *cBuf = FUZZ_malloc(cBufSize);
    void *rBuf = FUZZ_malloc(rBufSize);
    size_t const cSize = ZSTD_compressCCtx(cctx, cBuf, cBufSize, src, size, 1);
    FUZZ_ZASSERT(cSize);
    size_t const rSize = ZSTD_decompressDCtx(dctx, rBuf, rBufSize, cBuf, cSize);
    if (size == 0) {
        FUZZ_ASSERT(rSize == 0);
    } else {
        FUZZ_ASSERT(ZSTD_isError(rSize));
        FUZZ_ASSERT(ZSTD_getErrorCode(rSize) == ZSTD_error_dstSize_tooSmall);
    }
    free(cBuf);
    free(rBuf);
    FUZZ_dataProducer_free(producer);
#ifndef STATEFUL_FUZZING
    ZSTD_freeCCtx(cctx); cctx = NULL;
    ZSTD_freeDCtx(dctx); dctx = NULL;
#endif
    FUZZ_SEQ_PROD_TEARDOWN();
    return 0;
}
