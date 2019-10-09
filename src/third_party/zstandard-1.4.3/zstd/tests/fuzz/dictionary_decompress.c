/*
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
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

static ZSTD_DCtx *dctx = NULL;

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    uint32_t seed = FUZZ_seed(&src, &size);
    FUZZ_dict_t dict;
    ZSTD_DDict* ddict = NULL;
    int i;

    if (!dctx) {
        dctx = ZSTD_createDCtx();
        FUZZ_ASSERT(dctx);
    }
    dict = FUZZ_train(src, size, &seed);
    if (FUZZ_rand32(&seed, 0, 1) == 0) {
        ddict = ZSTD_createDDict(dict.buff, dict.size);
        FUZZ_ASSERT(ddict);
    } else {
        FUZZ_ZASSERT(ZSTD_DCtx_loadDictionary_advanced(
                dctx, dict.buff, dict.size,
                (ZSTD_dictLoadMethod_e)FUZZ_rand32(&seed, 0, 1),
                (ZSTD_dictContentType_e)FUZZ_rand32(&seed, 0, 2)));
    }
    /* Run it 10 times over 10 output sizes. Reuse the context and dict. */
    for (i = 0; i < 10; ++i) {
        size_t const bufSize = FUZZ_rand32(&seed, 0, 2 * size);
        void* rBuf = malloc(bufSize);
        FUZZ_ASSERT(rBuf);
        if (ddict) {
            ZSTD_decompress_usingDDict(dctx, rBuf, bufSize, src, size, ddict);
        } else {
            ZSTD_decompressDCtx(dctx, rBuf, bufSize, src, size);
        }
        free(rBuf);
    }
    free(dict.buff);
    ZSTD_freeDDict(ddict);
#ifndef STATEFUL_FUZZING
    ZSTD_freeDCtx(dctx); dctx = NULL;
#endif
    return 0;
}
