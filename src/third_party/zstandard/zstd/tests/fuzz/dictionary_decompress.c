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
 * This fuzz target attempts to decompress the fuzzed data with the dictionary
 * decompression function to ensure the decompressor never crashes. It does not
 * fuzz the dictionary.
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include "fuzz_helpers.h"
#include "zstd_helpers.h"
#include "fuzz_data_producer.h"

static ZSTD_DCtx *dctx = NULL;

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    /* Give a random portion of src data to the producer, to use for
    parameter generation. The rest will be used for (de)compression */
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(src, size);
    size = FUZZ_dataProducer_reserveDataPrefix(producer);

    FUZZ_dict_t dict;
    ZSTD_DDict* ddict = NULL;

    if (!dctx) {
        dctx = ZSTD_createDCtx();
        FUZZ_ASSERT(dctx);
    }
    dict = FUZZ_train(src, size, producer);
    if (FUZZ_dataProducer_uint32Range(producer, 0, 1) == 0) {
        ddict = ZSTD_createDDict(dict.buff, dict.size);
        FUZZ_ASSERT(ddict);
    } else {
        if (FUZZ_dataProducer_uint32Range(producer, 0, 1) == 0)
            FUZZ_ZASSERT(ZSTD_DCtx_loadDictionary_advanced(
                dctx, dict.buff, dict.size,
                (ZSTD_dictLoadMethod_e)FUZZ_dataProducer_uint32Range(producer, 0, 1),
                (ZSTD_dictContentType_e)FUZZ_dataProducer_uint32Range(producer, 0, 2)));
        else
            FUZZ_ZASSERT(ZSTD_DCtx_refPrefix_advanced(
                dctx, dict.buff, dict.size,
                (ZSTD_dictContentType_e)FUZZ_dataProducer_uint32Range(producer, 0, 2)));
    }

    {
        size_t const bufSize = FUZZ_dataProducer_uint32Range(producer, 0, 10 * size);
        void* rBuf = FUZZ_malloc(bufSize);
        if (ddict) {
            ZSTD_decompress_usingDDict(dctx, rBuf, bufSize, src, size, ddict);
        } else {
            ZSTD_decompressDCtx(dctx, rBuf, bufSize, src, size);
        }
        free(rBuf);
    }
    free(dict.buff);
    FUZZ_dataProducer_free(producer);
    ZSTD_freeDDict(ddict);
#ifndef STATEFUL_FUZZING
    ZSTD_freeDCtx(dctx); dctx = NULL;
#endif
    return 0;
}
