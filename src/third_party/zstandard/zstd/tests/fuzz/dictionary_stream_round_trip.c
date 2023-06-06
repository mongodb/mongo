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

ZSTD_CCtx *cctx = NULL;
static ZSTD_DCtx *dctx = NULL;
static uint8_t* cBuf = NULL;
static uint8_t* rBuf = NULL;
static size_t bufSize = 0;

static ZSTD_outBuffer makeOutBuffer(uint8_t *dst, size_t capacity,
                                    FUZZ_dataProducer_t *producer)
{
    ZSTD_outBuffer buffer = { dst, 0, 0 };

    FUZZ_ASSERT(capacity > 0);
    buffer.size = (FUZZ_dataProducer_uint32Range(producer, 1, capacity));
    FUZZ_ASSERT(buffer.size <= capacity);

    return buffer;
}

static ZSTD_inBuffer makeInBuffer(const uint8_t **src, size_t *size,
                                  FUZZ_dataProducer_t *producer)
{
    ZSTD_inBuffer buffer = { *src, 0, 0 };

    FUZZ_ASSERT(*size > 0);
    buffer.size = (FUZZ_dataProducer_uint32Range(producer, 1, *size));
    FUZZ_ASSERT(buffer.size <= *size);
    *src += buffer.size;
    *size -= buffer.size;

    return buffer;
}

static size_t compress(uint8_t *dst, size_t capacity,
                        const uint8_t *src, size_t srcSize,
                        const uint8_t* dict, size_t dictSize,
                        FUZZ_dataProducer_t *producer, int refPrefix,
                        ZSTD_dictContentType_e dictContentType)
{
    size_t dstSize = 0;
    ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
    FUZZ_setRandomParameters(cctx, srcSize, producer);

    /* Disable checksum so we can use sizes smaller than compress bound. */
    FUZZ_ZASSERT(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0));
    if (refPrefix)
        FUZZ_ZASSERT(ZSTD_CCtx_refPrefix_advanced(
            cctx, dict, dictSize,
            dictContentType));
    else
        FUZZ_ZASSERT(ZSTD_CCtx_loadDictionary_advanced(
            cctx, dict, dictSize,
            (ZSTD_dictLoadMethod_e)FUZZ_dataProducer_uint32Range(producer, 0, 1),
            dictContentType));

    while (srcSize > 0) {
        ZSTD_inBuffer in = makeInBuffer(&src, &srcSize, producer);
        /* Mode controls the action. If mode == -1 we pick a new mode */
        int mode = -1;
        while (in.pos < in.size || mode != -1) {
            ZSTD_outBuffer out = makeOutBuffer(dst, capacity, producer);
            /* Previous action finished, pick a new mode. */
            if (mode == -1) mode = FUZZ_dataProducer_uint32Range(producer, 0, 9);
            switch (mode) {
                case 0: /* fall-through */
                case 1: /* fall-through */
                case 2: {
                    size_t const ret =
                        ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_flush);
                    FUZZ_ZASSERT(ret);
                    if (ret == 0)
                        mode = -1;
                    break;
                }
                case 3: {
                    size_t ret =
                        ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end);
                    FUZZ_ZASSERT(ret);
                    /* Reset the compressor when the frame is finished */
                    if (ret == 0) {
                        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
                        if (FUZZ_dataProducer_uint32Range(producer, 0, 7) == 0) {
                            size_t const remaining = in.size - in.pos;
                            FUZZ_setRandomParameters(cctx, remaining, producer);
                        }
                        mode = -1;
                    }
                    break;
                }
                case 4: {
                    ZSTD_inBuffer nullIn = { NULL, 0, 0 };
                    ZSTD_outBuffer nullOut = { NULL, 0, 0 };
                    size_t const ret = ZSTD_compressStream2(cctx, &nullOut, &nullIn, ZSTD_e_continue);
                    FUZZ_ZASSERT(ret);
                }
                /* fall-through */
                default: {
                    size_t const ret =
                        ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_continue);
                    FUZZ_ZASSERT(ret);
                    mode = -1;
                }
            }
            dst += out.pos;
            dstSize += out.pos;
            capacity -= out.pos;
        }
    }
    for (;;) {
        ZSTD_inBuffer in = {NULL, 0, 0};
        ZSTD_outBuffer out = makeOutBuffer(dst, capacity, producer);
        size_t const ret = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end);
        FUZZ_ZASSERT(ret);

        dst += out.pos;
        dstSize += out.pos;
        capacity -= out.pos;
        if (ret == 0)
            break;
    }
    return dstSize;
}

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    FUZZ_SEQ_PROD_SETUP();
    size_t neededBufSize;

    /* Give a random portion of src data to the producer, to use for
    parameter generation. The rest will be used for (de)compression */
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(src, size);
    size = FUZZ_dataProducer_reserveDataPrefix(producer);

    neededBufSize = ZSTD_compressBound(size) * 15;

    /* Allocate all buffers and contexts if not already allocated */
    if (neededBufSize > bufSize) {
        free(cBuf);
        free(rBuf);
        cBuf = (uint8_t*)FUZZ_malloc(neededBufSize);
        rBuf = (uint8_t*)FUZZ_malloc(neededBufSize);
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
        ZSTD_dictContentType_e dictContentType = FUZZ_dataProducer_uint32Range(producer, 0, 2);
        FUZZ_dict_t dict = FUZZ_train(src, size, producer);
        int const refPrefix = FUZZ_dataProducer_uint32Range(producer, 0, 1) != 0;

        size_t const cSize = compress(cBuf, neededBufSize, src, size, dict.buff, dict.size, producer, refPrefix, dictContentType);

        if (refPrefix)
            FUZZ_ZASSERT(ZSTD_DCtx_refPrefix_advanced(
                dctx, dict.buff, dict.size,
                dictContentType));
        else
            FUZZ_ZASSERT(ZSTD_DCtx_loadDictionary_advanced(
                dctx, dict.buff, dict.size,
                (ZSTD_dictLoadMethod_e)FUZZ_dataProducer_uint32Range(producer, 0, 1),
                dictContentType));
        size_t const rSize =
            ZSTD_decompressDCtx(dctx, rBuf, neededBufSize, cBuf, cSize);
        FUZZ_ZASSERT(rSize);
        FUZZ_ASSERT_MSG(rSize == size, "Incorrect regenerated size");
        FUZZ_ASSERT_MSG(!FUZZ_memcmp(src, rBuf, size), "Corruption!");
        free(dict.buff);
    }

    FUZZ_dataProducer_free(producer);
#ifndef STATEFUL_FUZZING
    ZSTD_freeCCtx(cctx); cctx = NULL;
    ZSTD_freeDCtx(dctx); dctx = NULL;
#endif
    FUZZ_SEQ_PROD_TEARDOWN();
    return 0;
}
