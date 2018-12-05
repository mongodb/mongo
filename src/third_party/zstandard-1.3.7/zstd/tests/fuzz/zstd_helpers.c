/*
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 */

#define ZSTD_STATIC_LINKING_ONLY

#include "zstd_helpers.h"
#include "fuzz_helpers.h"
#include "zstd.h"

static void set(ZSTD_CCtx *cctx, ZSTD_cParameter param, unsigned value)
{
    FUZZ_ZASSERT(ZSTD_CCtx_setParameter(cctx, param, value));
}

static void setRand(ZSTD_CCtx *cctx, ZSTD_cParameter param, unsigned min,
                    unsigned max, uint32_t *state) {
    unsigned const value = FUZZ_rand32(state, min, max);
    set(cctx, param, value);
}

ZSTD_compressionParameters FUZZ_randomCParams(size_t srcSize, uint32_t *state)
{
    /* Select compression parameters */
    ZSTD_compressionParameters cParams;
    cParams.windowLog = FUZZ_rand32(state, ZSTD_WINDOWLOG_MIN, 15);
    cParams.hashLog = FUZZ_rand32(state, ZSTD_HASHLOG_MIN, 15);
    cParams.chainLog = FUZZ_rand32(state, ZSTD_CHAINLOG_MIN, 16);
    cParams.searchLog = FUZZ_rand32(state, ZSTD_SEARCHLOG_MIN, 9);
    cParams.searchLength = FUZZ_rand32(state, ZSTD_SEARCHLENGTH_MIN,
                                              ZSTD_SEARCHLENGTH_MAX);
    cParams.targetLength = FUZZ_rand32(state, 0, 512);
    cParams.strategy = FUZZ_rand32(state, ZSTD_fast, ZSTD_btultra);
    return ZSTD_adjustCParams(cParams, srcSize, 0);
}

ZSTD_frameParameters FUZZ_randomFParams(uint32_t *state)
{
    /* Select frame parameters */
    ZSTD_frameParameters fParams;
    fParams.contentSizeFlag = FUZZ_rand32(state, 0, 1);
    fParams.checksumFlag = FUZZ_rand32(state, 0, 1);
    fParams.noDictIDFlag = FUZZ_rand32(state, 0, 1);
    return fParams;
}

ZSTD_parameters FUZZ_randomParams(size_t srcSize, uint32_t *state)
{
    ZSTD_parameters params;
    params.cParams = FUZZ_randomCParams(srcSize, state);
    params.fParams = FUZZ_randomFParams(state);
    return params;
}

void FUZZ_setRandomParameters(ZSTD_CCtx *cctx, size_t srcSize, uint32_t *state)
{
    ZSTD_compressionParameters cParams = FUZZ_randomCParams(srcSize, state);
    set(cctx, ZSTD_p_windowLog, cParams.windowLog);
    set(cctx, ZSTD_p_hashLog, cParams.hashLog);
    set(cctx, ZSTD_p_chainLog, cParams.chainLog);
    set(cctx, ZSTD_p_searchLog, cParams.searchLog);
    set(cctx, ZSTD_p_minMatch, cParams.searchLength);
    set(cctx, ZSTD_p_targetLength, cParams.targetLength);
    set(cctx, ZSTD_p_compressionStrategy, cParams.strategy);
    /* Select frame parameters */
    setRand(cctx, ZSTD_p_contentSizeFlag, 0, 1, state);
    setRand(cctx, ZSTD_p_checksumFlag, 0, 1, state);
    setRand(cctx, ZSTD_p_dictIDFlag, 0, 1, state);
    setRand(cctx, ZSTD_p_forceAttachDict, -2, 2, state);
    /* Select long distance matchig parameters */
    setRand(cctx, ZSTD_p_enableLongDistanceMatching, 0, 1, state);
    setRand(cctx, ZSTD_p_ldmHashLog, ZSTD_HASHLOG_MIN, 16, state);
    setRand(cctx, ZSTD_p_ldmMinMatch, ZSTD_LDM_MINMATCH_MIN,
            ZSTD_LDM_MINMATCH_MAX, state);
    setRand(cctx, ZSTD_p_ldmBucketSizeLog, 0, ZSTD_LDM_BUCKETSIZELOG_MAX,
            state);
    setRand(cctx, ZSTD_p_ldmHashEveryLog, 0,
            ZSTD_WINDOWLOG_MAX - ZSTD_HASHLOG_MIN, state);
}
