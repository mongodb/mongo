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

static ZSTD_CCtx *cctx = NULL;
static ZSTD_DCtx *dctx = NULL;
static void* literalsBuffer = NULL;
static void* generatedSrc = NULL;
static ZSTD_Sequence* generatedSequences = NULL;

#define ZSTD_FUZZ_GENERATED_SRC_MAXSIZE (1 << 20) /* Allow up to 1MB generated data */
#define ZSTD_FUZZ_MATCHLENGTH_MAXSIZE (1 << 18) /* Allow up to 256KB matches */
#define ZSTD_FUZZ_GENERATED_DICT_MAXSIZE (1 << 18) /* Allow up to a 256KB dict */
#define ZSTD_FUZZ_GENERATED_LITERALS_SIZE (1 << 18) /* Fixed size 256KB literals buffer */
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
static char *generatePseudoRandomString(char *str, size_t size) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJK1234567890!@#$^&*()_";
    uint32_t seed = 0;
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
                              size_t literalsSize, const void* dict, size_t dictSize) {
    const uint8_t* litPtr = literalsBuffer;
    const uint8_t* const litBegin = literalsBuffer;
    const uint8_t* const litEnd = literalsBuffer + literalsSize;
    const uint8_t* dictPtr = dict;
    uint8_t* op = dst;
    const uint8_t* const oend = dst + ZSTD_FUZZ_GENERATED_SRC_MAXSIZE;
    size_t generatedSrcBufferSize = 0;
    size_t bytesWritten = 0;
    uint32_t lastLLSize;

    for (size_t i = 0; i < nbSequences; ++i) {
        FUZZ_ASSERT(generatedSequences[i].matchLength != 0);
        FUZZ_ASSERT(generatedSequences[i].offset != 0);

        if (litPtr + generatedSequences[i].litLength > litEnd) {
            litPtr = litBegin;
        }
        ZSTD_memcpy(op, litPtr, generatedSequences[i].litLength);
        bytesWritten += generatedSequences[i].litLength;
        op += generatedSequences[i].litLength;
        litPtr += generatedSequences[i].litLength;

        FUZZ_ASSERT(generatedSequences[i].offset != 0);
        /* Copy over the match */
        {   size_t matchLength = generatedSequences[i].matchLength;
            size_t j = 0;
            size_t k = 0;
            if (dictSize != 0) {
                if (generatedSequences[i].offset > bytesWritten) {
                    /* Offset goes into the dictionary */
                    size_t offsetFromEndOfDict = generatedSequences[i].offset - bytesWritten;
                    for (; k < offsetFromEndOfDict && k < matchLength; ++k) {
                        op[k] = dictPtr[dictSize - offsetFromEndOfDict + k];
                    }
                    matchLength -= k;
                    op += k;
                }
            }
            for (; j < matchLength; ++j) {
                op[j] = op[j-(int)generatedSequences[i].offset];
            }
            op += j;
            FUZZ_ASSERT(generatedSequences[i].matchLength == j + k);
            bytesWritten += generatedSequences[i].matchLength;
        }
    }
    generatedSrcBufferSize = bytesWritten;
    FUZZ_ASSERT(litPtr <= litEnd);
    lastLLSize = (uint32_t)(litEnd - litPtr);
    if (lastLLSize <= oend - op) {
        ZSTD_memcpy(op, litPtr, lastLLSize);
        generatedSrcBufferSize += lastLLSize;
    }
    return generatedSrcBufferSize;
}

/* Returns nb sequences generated
 * TODO: Add repcode fuzzing once we support repcode match splits
 */
static size_t generateRandomSequences(FUZZ_dataProducer_t* producer,
                                      size_t literalsSizeLimit, size_t dictSize,
                                      size_t windowLog) {
    uint32_t bytesGenerated = 0;
    uint32_t nbSeqGenerated = 0;
    uint32_t litLength;
    uint32_t matchLength;
    uint32_t matchBound;
    uint32_t offset;
    uint32_t offsetBound;
    uint32_t repCode = 0;
    uint32_t isFirstSequence = 1;
    uint32_t windowSize = 1 << windowLog;

    while (nbSeqGenerated < ZSTD_FUZZ_MAX_NBSEQ
         && bytesGenerated < ZSTD_FUZZ_GENERATED_SRC_MAXSIZE
         && !FUZZ_dataProducer_empty(producer)) {
        matchBound = ZSTD_FUZZ_MATCHLENGTH_MAXSIZE;
        litLength = isFirstSequence && dictSize == 0 ? FUZZ_dataProducer_uint32Range(producer, 1, literalsSizeLimit)
                                                     : FUZZ_dataProducer_uint32Range(producer, 0, literalsSizeLimit);
        bytesGenerated += litLength;
        if (bytesGenerated > ZSTD_FUZZ_GENERATED_SRC_MAXSIZE) {
            break;
        }
        offsetBound = bytesGenerated > windowSize ? windowSize : bytesGenerated + dictSize;
        offset = FUZZ_dataProducer_uint32Range(producer, 1, offsetBound);
        if (dictSize > 0 && bytesGenerated <= windowSize) {
            /* Prevent match length from being such that it would be associated with an offset too large
             * from the decoder's perspective. If not possible (match would be too small),
             * then reduce the offset if necessary.
             */
            size_t bytesToReachWindowSize = windowSize - bytesGenerated;
            if (bytesToReachWindowSize < ZSTD_MINMATCH_MIN) {
                uint32_t newOffsetBound = offsetBound > windowSize ? windowSize : offsetBound;
                offset = FUZZ_dataProducer_uint32Range(producer, 1, newOffsetBound);
            } else {
                matchBound = bytesToReachWindowSize > ZSTD_FUZZ_MATCHLENGTH_MAXSIZE ?
                             ZSTD_FUZZ_MATCHLENGTH_MAXSIZE : bytesToReachWindowSize;
            }
        }
        matchLength = FUZZ_dataProducer_uint32Range(producer, ZSTD_MINMATCH_MIN, matchBound);
        bytesGenerated += matchLength;
        if (bytesGenerated > ZSTD_FUZZ_GENERATED_SRC_MAXSIZE) {
            break;
        }
        ZSTD_Sequence seq = {offset, litLength, matchLength, repCode};
        generatedSequences[nbSeqGenerated++] = seq;
        isFirstSequence = 0;
    }

    return nbSeqGenerated;
}

static size_t roundTripTest(void *result, size_t resultCapacity,
                            void *compressed, size_t compressedCapacity,
                            size_t srcSize,
                            const void *dict, size_t dictSize,
                            size_t generatedSequencesSize,
                            size_t wLog, unsigned cLevel, unsigned hasDict)
{
    size_t cSize;
    size_t dSize;
    ZSTD_CDict* cdict = NULL;
    ZSTD_DDict* ddict = NULL;

    ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 0);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, cLevel);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, wLog);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_minMatch, ZSTD_MINMATCH_MIN);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_validateSequences, 1);
    /* TODO: Add block delim mode fuzzing */
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_blockDelimiters, ZSTD_sf_noBlockDelimiters);
    if (hasDict) {
        FUZZ_ZASSERT(ZSTD_CCtx_loadDictionary(cctx, dict, dictSize));
        FUZZ_ZASSERT(ZSTD_DCtx_loadDictionary(dctx, dict, dictSize));
    }

    cSize = ZSTD_compressSequences(cctx, compressed, compressedCapacity,
                                   generatedSequences, generatedSequencesSize,
                                   generatedSrc, srcSize);
    FUZZ_ZASSERT(cSize);
    dSize = ZSTD_decompressDCtx(dctx, result, resultCapacity, compressed, cSize);
    FUZZ_ZASSERT(dSize);

    if (cdict) {
        ZSTD_freeCDict(cdict);
    }
    if (ddict) {
        ZSTD_freeDDict(ddict);
    }
    return dSize;
}

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    void* rBuf;
    size_t rBufSize;
    void* cBuf;
    size_t cBufSize;
    size_t generatedSrcSize;
    size_t nbSequences;
    void* dictBuffer;
    size_t dictSize = 0;
    unsigned hasDict;
    unsigned wLog;
    int cLevel;

    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(src, size);
    if (literalsBuffer == NULL) {
        literalsBuffer = FUZZ_malloc(ZSTD_FUZZ_GENERATED_LITERALS_SIZE);
        literalsBuffer = generatePseudoRandomString(literalsBuffer, ZSTD_FUZZ_GENERATED_LITERALS_SIZE);
    }

    hasDict = FUZZ_dataProducer_uint32Range(producer, 0, 1);
    if (hasDict) {
        dictSize = FUZZ_dataProducer_uint32Range(producer, 1, ZSTD_FUZZ_GENERATED_DICT_MAXSIZE);
        dictBuffer = FUZZ_malloc(dictSize);
        dictBuffer = generatePseudoRandomString(dictBuffer, dictSize);
    }
    /* Generate window log first so we dont generate offsets too large */
    wLog = FUZZ_dataProducer_uint32Range(producer, ZSTD_WINDOWLOG_MIN, ZSTD_WINDOWLOG_MAX_32);
    cLevel = FUZZ_dataProducer_int32Range(producer, -3, 22);

    if (!generatedSequences) {
        generatedSequences = FUZZ_malloc(sizeof(ZSTD_Sequence)*ZSTD_FUZZ_MAX_NBSEQ);
    }
    if (!generatedSrc) {
        generatedSrc = FUZZ_malloc(ZSTD_FUZZ_GENERATED_SRC_MAXSIZE);
    }
    nbSequences = generateRandomSequences(producer, ZSTD_FUZZ_GENERATED_LITERALS_SIZE, dictSize, wLog);
    generatedSrcSize = decodeSequences(generatedSrc, nbSequences, ZSTD_FUZZ_GENERATED_LITERALS_SIZE, dictBuffer, dictSize);
    cBufSize = ZSTD_compressBound(generatedSrcSize);
    cBuf = FUZZ_malloc(cBufSize);

    rBufSize = generatedSrcSize;
    rBuf = FUZZ_malloc(rBufSize);

    if (!cctx) {
        cctx = ZSTD_createCCtx();
        FUZZ_ASSERT(cctx);
    }
    if (!dctx) {
        dctx = ZSTD_createDCtx();
        FUZZ_ASSERT(dctx);
    }

    size_t const result = roundTripTest(rBuf, rBufSize,
                                        cBuf, cBufSize,
                                        generatedSrcSize,
                                        dictBuffer, dictSize,
                                        nbSequences,
                                        wLog, cLevel, hasDict);
    FUZZ_ZASSERT(result);
    FUZZ_ASSERT_MSG(result == generatedSrcSize, "Incorrect regenerated size");
    FUZZ_ASSERT_MSG(!FUZZ_memcmp(generatedSrc, rBuf, generatedSrcSize), "Corruption!");

    free(rBuf);
    free(cBuf);
    FUZZ_dataProducer_free(producer);
    if (hasDict) {
        free(dictBuffer);
    }
#ifndef STATEFUL_FUZZING
    ZSTD_freeCCtx(cctx); cctx = NULL;
    ZSTD_freeDCtx(dctx); dctx = NULL;
    free(generatedSequences); generatedSequences = NULL;
    free(generatedSrc); generatedSrc = NULL;
    free(literalsBuffer); literalsBuffer = NULL;
#endif
    return 0;
}
