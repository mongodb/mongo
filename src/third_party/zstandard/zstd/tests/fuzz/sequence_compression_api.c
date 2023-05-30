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
 * This fuzz target performs a zstd round-trip test by generating an arbitrary
 * array of sequences, generating the associated source buffer, calling
 * ZSTD_compressSequences(), and then decompresses and compares the result with
 * the original generated source buffer.
 */

#define ZSTD_STATIC_LINKING_ONLY

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "fuzz_helpers.h"
#include "zstd_helpers.h"
#include "fuzz_data_producer.h"
#include "fuzz_third_party_seq_prod.h"

static ZSTD_CCtx* cctx = NULL;
static ZSTD_DCtx* dctx = NULL;
static void* literalsBuffer = NULL;
static void* generatedSrc = NULL;
static ZSTD_Sequence* generatedSequences = NULL;

static void* dictBuffer = NULL;
static ZSTD_CDict* cdict = NULL;
static ZSTD_DDict* ddict = NULL;

#define ZSTD_FUZZ_GENERATED_SRC_MAXSIZE (1 << 20) /* Allow up to 1MB generated data */
#define ZSTD_FUZZ_GENERATED_LITERALS_SIZE (1 << 20) /* Fixed size 1MB literals buffer */
#define ZSTD_FUZZ_MATCHLENGTH_MAXSIZE (1 << 18) /* Allow up to 256KB matches */
#define ZSTD_FUZZ_GENERATED_DICT_MAXSIZE (1 << ZSTD_WINDOWLOG_MAX_32) /* Allow up to 1 << ZSTD_WINDOWLOG_MAX_32 dictionary */
#define ZSTD_FUZZ_MAX_NBSEQ (1 << 17) /* Maximum of 128K sequences */

/* Deterministic random number generator */
#define FUZZ_RDG_rotl32(x,r) ((x << r) | (x >> (32 - r)))
static uint32_t FUZZ_RDG_rand(uint32_t* src)
{
    static const uint32_t prime1 = 2654435761U;
    static const uint32_t prime2 = 2246822519U;
    uint32_t rand32 = *src;
    rand32 *= prime1;
    rand32 ^= prime2;
    rand32  = FUZZ_RDG_rotl32(rand32, 13);
    *src = rand32;
    return rand32 >> 5;
}

/* Make a pseudorandom string - this simple function exists to avoid
 * taking a dependency on datagen.h to have RDG_genBuffer().
 */
static char* generatePseudoRandomString(char* str, size_t size, FUZZ_dataProducer_t* producer) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJK1234567890!@#$^&*()_";
    uint32_t seed = FUZZ_dataProducer_uint32(producer);
    if (size) {
        for (size_t n = 0; n < size; n++) {
            int key = FUZZ_RDG_rand(&seed) % (int) (sizeof charset - 1);
            str[n] = charset[key];
        }
    }
    return str;
}

/* Returns size of source buffer */
static size_t decodeSequences(void* dst, size_t nbSequences,
                              size_t literalsSize,
                              const void* dict, size_t dictSize,
                              ZSTD_sequenceFormat_e mode)
{
    const uint8_t* litPtr = literalsBuffer;
    const uint8_t* const litBegin = literalsBuffer;
    const uint8_t* const litEnd = litBegin + literalsSize;
    const uint8_t* dictPtr = dict;
    uint8_t* op = dst;
    const uint8_t* const oend = (uint8_t*)dst + ZSTD_FUZZ_GENERATED_SRC_MAXSIZE;
    size_t generatedSrcBufferSize = 0;
    size_t bytesWritten = 0;

    for (size_t i = 0; i < nbSequences; ++i) {
        /* block boundary */
        if (generatedSequences[i].offset == 0)
            FUZZ_ASSERT(generatedSequences[i].matchLength == 0);

        if (litPtr + generatedSequences[i].litLength > litEnd) {
            litPtr = litBegin;
        }
        memcpy(op, litPtr, generatedSequences[i].litLength);
        bytesWritten += generatedSequences[i].litLength;
        op += generatedSequences[i].litLength;
        litPtr += generatedSequences[i].litLength;

        /* Copy over the match */
        {   size_t matchLength = generatedSequences[i].matchLength;
            size_t j = 0;
            size_t k = 0;
            if (dictSize != 0) {
                if (generatedSequences[i].offset > bytesWritten) { /* Offset goes into the dictionary */
                    size_t dictOffset = generatedSequences[i].offset - bytesWritten;
                    size_t matchInDict = MIN(matchLength, dictOffset);
                    for (; k < matchInDict; ++k) {
                        op[k] = dictPtr[dictSize - dictOffset + k];
                    }
                    matchLength -= matchInDict;
                    op += matchInDict;
                }
            }
            for (; j < matchLength; ++j) {
                op[j] = op[j - generatedSequences[i].offset];
            }
            op += j;
            FUZZ_ASSERT(generatedSequences[i].matchLength == j + k);
            bytesWritten += generatedSequences[i].matchLength;
        }
    }
    generatedSrcBufferSize = bytesWritten;
    FUZZ_ASSERT(litPtr <= litEnd);
    if (mode == ZSTD_sf_noBlockDelimiters) {
        const uint32_t lastLLSize = (uint32_t)(litEnd - litPtr);
        if (lastLLSize <= oend - op) {
            memcpy(op, litPtr, lastLLSize);
            generatedSrcBufferSize += lastLLSize;
    }   }
    return generatedSrcBufferSize;
}

/* Returns nb sequences generated
 * Note : random sequences are always valid in ZSTD_sf_noBlockDelimiters mode.
 * However, it can fail with ZSTD_sf_explicitBlockDelimiters,
 * due to potential lack of space in
 */
static size_t generateRandomSequences(FUZZ_dataProducer_t* producer,
                                      size_t literalsSizeLimit, size_t dictSize,
                                      size_t windowLog, ZSTD_sequenceFormat_e mode)
{
    const uint32_t repCode = 0;  /* not used by sequence ingestion api */
    size_t windowSize = 1ULL << windowLog;
    size_t blockSizeMax = MIN(ZSTD_BLOCKSIZE_MAX, windowSize);
    uint32_t matchLengthMax = ZSTD_FUZZ_MATCHLENGTH_MAXSIZE;
    uint32_t bytesGenerated = 0;
    uint32_t nbSeqGenerated = 0;
    uint32_t isFirstSequence = 1;
    uint32_t blockSize = 0;

    if (mode == ZSTD_sf_explicitBlockDelimiters) {
        /* ensure that no sequence can be larger than one block */
        literalsSizeLimit = MIN(literalsSizeLimit, blockSizeMax/2);
        matchLengthMax = MIN(matchLengthMax, blockSizeMax/2);
    }

    while ( nbSeqGenerated < ZSTD_FUZZ_MAX_NBSEQ - 3 /* extra room for explicit delimiters */
         && bytesGenerated < ZSTD_FUZZ_GENERATED_SRC_MAXSIZE
         && !FUZZ_dataProducer_empty(producer)) {
        uint32_t matchLength;
        uint32_t matchBound = matchLengthMax;
        uint32_t offset;
        uint32_t offsetBound;
        const uint32_t minLitLength = (isFirstSequence && (dictSize == 0));
        const uint32_t litLength = FUZZ_dataProducer_uint32Range(producer, minLitLength, (uint32_t)literalsSizeLimit);
        bytesGenerated += litLength;
        if (bytesGenerated > ZSTD_FUZZ_GENERATED_SRC_MAXSIZE) {
            break;
        }
        offsetBound = (bytesGenerated > windowSize) ? windowSize : bytesGenerated + (uint32_t)dictSize;
        offset = FUZZ_dataProducer_uint32Range(producer, 1, offsetBound);
        if (dictSize > 0 && bytesGenerated <= windowSize) {
            /* Prevent match length from being such that it would be associated with an offset too large
             * from the decoder's perspective. If not possible (match would be too small),
             * then reduce the offset if necessary.
             */
            const size_t bytesToReachWindowSize = windowSize - bytesGenerated;
            if (bytesToReachWindowSize < ZSTD_MINMATCH_MIN) {
                const uint32_t newOffsetBound = offsetBound > windowSize ? windowSize : offsetBound;
                offset = FUZZ_dataProducer_uint32Range(producer, 1, newOffsetBound);
            } else {
                matchBound = MIN(matchLengthMax, (uint32_t)bytesToReachWindowSize);
            }
        }
        matchLength = FUZZ_dataProducer_uint32Range(producer, ZSTD_MINMATCH_MIN, matchBound);
        bytesGenerated += matchLength;
        if (bytesGenerated > ZSTD_FUZZ_GENERATED_SRC_MAXSIZE) {
            break;
        }
        {   ZSTD_Sequence seq = {offset, litLength, matchLength, repCode};
            const uint32_t lastLits = FUZZ_dataProducer_uint32Range(producer, 0, litLength);
            #define SPLITPROB 6000
            #define SPLITMARK 5234
            const int split = (FUZZ_dataProducer_uint32Range(producer, 0, SPLITPROB) == SPLITMARK);
            if (mode == ZSTD_sf_explicitBlockDelimiters) {
                const size_t seqSize = seq.litLength + seq.matchLength;
                if (blockSize + seqSize > blockSizeMax) {  /* reaching limit : must end block now */
                    const ZSTD_Sequence endBlock = {0, 0, 0, 0};
                    generatedSequences[nbSeqGenerated++] = endBlock;
                    blockSize = seqSize;
                }
                if (split) {
                    const ZSTD_Sequence endBlock = {0, lastLits, 0, 0};
                    generatedSequences[nbSeqGenerated++] = endBlock;
                    assert(lastLits <= seq.litLength);
                    seq.litLength -= lastLits;
                    blockSize = seqSize - lastLits;
                } else {
                    blockSize += seqSize;
                }
            }
            generatedSequences[nbSeqGenerated++] = seq;
            isFirstSequence = 0;
        }
    }

    if (mode == ZSTD_sf_explicitBlockDelimiters) {
        /* always end sequences with a block delimiter */
        const ZSTD_Sequence endBlock = {0, 0, 0, 0};
        assert(nbSeqGenerated < ZSTD_FUZZ_MAX_NBSEQ);
        generatedSequences[nbSeqGenerated++] = endBlock;
    }
    return nbSeqGenerated;
}

static size_t roundTripTest(void* result, size_t resultCapacity,
                            void* compressed, size_t compressedCapacity,
                            const void* src, size_t srcSize,
                            const ZSTD_Sequence* seqs, size_t seqSize,
                            unsigned hasDict,
                            ZSTD_sequenceFormat_e mode)
{
    size_t cSize;
    size_t dSize;

    if (hasDict) {
        FUZZ_ZASSERT(ZSTD_CCtx_refCDict(cctx, cdict));
        FUZZ_ZASSERT(ZSTD_DCtx_refDDict(dctx, ddict));
    }

    cSize = ZSTD_compressSequences(cctx, compressed, compressedCapacity,
                                   seqs, seqSize,
                                   src, srcSize);
    if ( (ZSTD_getErrorCode(cSize) == ZSTD_error_dstSize_tooSmall)
      && (mode == ZSTD_sf_explicitBlockDelimiters) ) {
        /* Valid scenario : in explicit delimiter mode,
         * it might be possible for the compressed size to outgrow dstCapacity.
         * In which case, it's still a valid fuzzer scenario,
         * but no roundtrip shall be possible */
        return 0;
    }
    /* round-trip */
    FUZZ_ZASSERT(cSize);
    dSize = ZSTD_decompressDCtx(dctx, result, resultCapacity, compressed, cSize);
    FUZZ_ZASSERT(dSize);
    FUZZ_ASSERT_MSG(dSize == srcSize, "Incorrect regenerated size");
    FUZZ_ASSERT_MSG(!FUZZ_memcmp(src, result, srcSize), "Corruption!");
    return dSize;
}

int LLVMFuzzerTestOneInput(const uint8_t* src, size_t size)
{
    FUZZ_SEQ_PROD_SETUP();

    void* rBuf;
    size_t rBufSize;
    void* cBuf;
    size_t cBufSize;
    size_t generatedSrcSize;
    size_t nbSequences;
    size_t dictSize = 0;
    unsigned hasDict;
    unsigned wLog;
    int cLevel;
    ZSTD_sequenceFormat_e mode;

    FUZZ_dataProducer_t* const producer = FUZZ_dataProducer_create(src, size);
    FUZZ_ASSERT(producer);

    if (!cctx) {
        cctx = ZSTD_createCCtx();
        FUZZ_ASSERT(cctx);
    }
    if (!dctx) {
        dctx = ZSTD_createDCtx();
        FUZZ_ASSERT(dctx);
    }

    /* Generate window log first so we don't generate offsets too large */
    wLog = FUZZ_dataProducer_uint32Range(producer, ZSTD_WINDOWLOG_MIN, ZSTD_WINDOWLOG_MAX);
    cLevel = FUZZ_dataProducer_int32Range(producer, -3, 22);
    mode = (ZSTD_sequenceFormat_e)FUZZ_dataProducer_int32Range(producer, 0, 1);

    ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 0);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, cLevel);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, wLog);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_minMatch, ZSTD_MINMATCH_MIN);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_validateSequences, 1);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_blockDelimiters, mode);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_forceAttachDict, ZSTD_dictForceAttach);

    if (!literalsBuffer) {
        literalsBuffer = FUZZ_malloc(ZSTD_FUZZ_GENERATED_LITERALS_SIZE);
        FUZZ_ASSERT(literalsBuffer);
        literalsBuffer = generatePseudoRandomString(literalsBuffer, ZSTD_FUZZ_GENERATED_LITERALS_SIZE, producer);
    }

    if (!dictBuffer) { /* Generate global dictionary buffer */
        ZSTD_compressionParameters cParams;

        /* Generate a large dictionary buffer */
        dictBuffer = calloc(ZSTD_FUZZ_GENERATED_DICT_MAXSIZE, 1);
        FUZZ_ASSERT(dictBuffer);

        /* Create global cdict and ddict */
        cParams = ZSTD_getCParams(1, ZSTD_FUZZ_GENERATED_SRC_MAXSIZE, ZSTD_FUZZ_GENERATED_DICT_MAXSIZE);
        cParams.minMatch = ZSTD_MINMATCH_MIN;
        cParams.hashLog = ZSTD_HASHLOG_MIN;
        cParams.chainLog = ZSTD_CHAINLOG_MIN;

        cdict = ZSTD_createCDict_advanced(dictBuffer, ZSTD_FUZZ_GENERATED_DICT_MAXSIZE, ZSTD_dlm_byRef, ZSTD_dct_rawContent, cParams, ZSTD_defaultCMem);
        ddict = ZSTD_createDDict_advanced(dictBuffer, ZSTD_FUZZ_GENERATED_DICT_MAXSIZE, ZSTD_dlm_byRef, ZSTD_dct_rawContent, ZSTD_defaultCMem);
        FUZZ_ASSERT(cdict);
        FUZZ_ASSERT(ddict);
    }

    FUZZ_ASSERT(cdict);
    FUZZ_ASSERT(ddict);

    hasDict = FUZZ_dataProducer_uint32Range(producer, 0, 1);
    if (hasDict) {
        dictSize = ZSTD_FUZZ_GENERATED_DICT_MAXSIZE;
    }

    if (!generatedSequences) {
        generatedSequences = FUZZ_malloc(sizeof(ZSTD_Sequence)*ZSTD_FUZZ_MAX_NBSEQ);
    }
    if (!generatedSrc) {
        generatedSrc = FUZZ_malloc(ZSTD_FUZZ_GENERATED_SRC_MAXSIZE);
    }

    nbSequences = generateRandomSequences(producer, ZSTD_FUZZ_GENERATED_LITERALS_SIZE, dictSize, wLog, mode);
    generatedSrcSize = decodeSequences(generatedSrc, nbSequences, ZSTD_FUZZ_GENERATED_LITERALS_SIZE, dictBuffer, dictSize, mode);

    /* Note : in explicit block delimiters mode,
     * the fuzzer might generate a lot of small blocks.
     * In which case, the final compressed size might be > ZSTD_compressBound().
     * This is still a valid scenario fuzzer though, which makes it possible to check under-sized dstCapacity.
     * The test just doesn't roundtrip. */
    cBufSize = ZSTD_compressBound(generatedSrcSize);
    cBuf = FUZZ_malloc(cBufSize);

    rBufSize = generatedSrcSize;
    rBuf = FUZZ_malloc(rBufSize);

    {   const size_t result = roundTripTest(rBuf, rBufSize,
                                        cBuf, cBufSize,
                                        generatedSrc, generatedSrcSize,
                                        generatedSequences, nbSequences,
                                        hasDict, mode);
        FUZZ_ASSERT(result <= generatedSrcSize);  /* can be 0 when no round-trip */
    }

    free(rBuf);
    free(cBuf);
    FUZZ_dataProducer_free(producer);
#ifndef STATEFUL_FUZZING
    ZSTD_freeCCtx(cctx); cctx = NULL;
    ZSTD_freeDCtx(dctx); dctx = NULL;
    free(generatedSequences); generatedSequences = NULL;
    free(generatedSrc); generatedSrc = NULL;
    free(literalsBuffer); literalsBuffer = NULL;
#endif
    FUZZ_SEQ_PROD_TEARDOWN();
    return 0;
}
